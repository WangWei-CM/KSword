#include "ManualFileSystemParser.h"

// ============================================================
// ManualFileSystemParser.cpp
// 说明：
// 1) 提供 NTFS/FAT32 的手动目录解析；
// 2) 提供 NTFS 删除项扫描与驻留/非驻留数据安全恢复；
// 3) 解析逻辑全部封装在本文件，UI 只消费统一结构。
// ============================================================

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryFile>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>

#include "NtfsRunListDecode.h"

namespace
{
    // runlist 解码已移入 ks::file（见 NtfsRunListDecode.h）：它是纯字节解析，
    // 剥离出去才能脱离真实卷做边界测试。这里保留别名，调用点不变。
    using ks::file::NtfsDataRun;
    using ks::file::ParseNtfsRunList;

    using ks::file::DirectoryCancellationCallback;

    bool directoryCanceled(const DirectoryCancellationCallback& canceled)
    {
        return canceled && canceled();
    }

    // 保证目录任务在任何取消出口关闭自身句柄；sourceHandle 只是借用，不重复关闭。
    class DirectoryReadHandle final
    {
    public:
        explicit DirectoryReadHandle(const HANDLE handle) : m_handle(handle) {}
        ~DirectoryReadHandle() { close(); }
        DirectoryReadHandle(const DirectoryReadHandle&) = delete;
        DirectoryReadHandle& operator=(const DirectoryReadHandle&) = delete;
        operator HANDLE() const { return m_handle; }
        void close()
        {
            if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr)
            {
                ::CloseHandle(m_handle);
                m_handle = INVALID_HANDLE_VALUE;
            }
        }
    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };

    // 带取消的目录排序不能在 comparator 中切换排序关系。
    // 小块仍用 std::sort；合并逐项检查，未完成载荷由工作线程丢弃。
    template <typename Value, typename Compare>
    bool sortDirectoryValues(
        std::vector<Value>& values,
        const Compare& compare,
        const DirectoryCancellationCallback& canceled)
    {
        if (!canceled)
        {
            std::sort(values.begin(), values.end(), compare);
            return true;
        }
        constexpr std::size_t SortChunkSize = 2048;
        for (std::size_t begin = 0; begin < values.size(); begin += SortChunkSize)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            const std::size_t end = begin + std::min(SortChunkSize, values.size() - begin);
            std::sort(
                values.begin() + static_cast<std::ptrdiff_t>(begin),
                values.begin() + static_cast<std::ptrdiff_t>(end),
                compare);
        }
        std::vector<Value> merged;
        if (values.size() > SortChunkSize)
        {
            merged.reserve(values.size());
        }
        for (std::size_t width = SortChunkSize; width < values.size();)
        {
            merged.clear();
            for (std::size_t begin = 0; begin < values.size();)
            {
                std::size_t left = begin;
                const std::size_t middle = begin + std::min(width, values.size() - begin);
                std::size_t right = middle;
                const std::size_t end = middle + std::min(width, values.size() - middle);
                while (left < middle || right < end)
                {
                    if ((merged.size() % 256) == 0 && directoryCanceled(canceled))
                    {
                        return false;
                    }
                    if (right < end && (left == middle || compare(values[right], values[left])))
                    {
                        merged.push_back(std::move(values[right++]));
                    }
                    else
                    {
                        merged.push_back(std::move(values[left++]));
                    }
                }
                begin = end;
            }
            values.swap(merged);
            if (width > values.size() / 2)
            {
                break;
            }
            width *= 2;
        }
        return !directoryCanceled(canceled);
    }

    // NtfsNameLink 作用：
    // - 表示同一条 MFT 记录下的一个“目录名链接”；
    // - 用于保留硬链接/多父目录场景，避免一条记录只能映射到一个目录。
    struct NtfsNameLink
    {
        std::uint64_t parentIndex = 0;         // 父目录记录号。
        QString fileName;                      // 该父目录下显示的文件名。
        int nameScore = -1;                    // 名称优先级，优先 Win32/Win32&DOS。
    };

    // NtfsVolumeBitmapSnapshot 作用：
    // - 保存卷位图快照；
    // - 用于判断删除文件的数据簇是否仍然未被重新分配。
    struct NtfsVolumeBitmapSnapshot
    {
        std::uint64_t startingLcn = 0;         // 当前位图起始 LCN。
        std::uint64_t clusterCount = 0;        // 位图覆盖的簇数量。
        std::vector<std::uint8_t> bitmapBytes; // 每个 bit 表示一个簇是否已分配。
    };

    // NtfsRawRecord 作用：
    // - 保存单条 MFT 记录中与目录显示/恢复相关的字段；
    // - 同时用于“目录列表”和“误删扫描”两类场景。
    struct NtfsRawRecord
    {
        std::uint64_t recordIndex = 0;         // 记录号。
        std::uint16_t sequenceNumber = 0;      // MFT 记录序列号，用于识别记录复用。
        std::uint64_t parentIndex = 0;         // 父目录记录号。
        QString fileName;                      // 文件名（优先 Win32 命名空间）。
        std::uint64_t sizeBytes = 0;           // 文件大小。
        std::uint64_t initializedSizeBytes = 0;// 非驻留流已初始化长度，尾部未初始化区域应补零。
        std::uint64_t modifiedTime100ns = 0;   // 修改时间（FILETIME 100ns）。
        bool inUse = false;                    // 是否在用。
        bool isDirectory = false;              // 是否目录。
        bool hasPrimaryDataStream = false;     // 是否存在未命名主数据流。
        bool nonResidentData = false;          // 未命名主数据流是否为非 resident。
        bool residentReady = false;            // 是否成功提取驻留数据。
        bool unsupportedDataStream = false;    // 主数据流是否使用当前不支持的布局/压缩/加密。
        bool hasAttributeList = false;         // 是否存在 $ATTRIBUTE_LIST，可能含外部数据段。
        std::uint16_t dataAttributeFlags = 0;  // $DATA 属性标志（压缩/加密/稀疏）。
        QByteArray residentData;               // 驻留数据内容。
        std::vector<NtfsDataRun> dataRuns;     // 非 resident 主数据流的数据段集合。
        std::vector<NtfsNameLink> nameLinks;   // 当前记录关联的全部目录名链接。
    };

    // NtfsDirectoryLink 作用：
    // - 把目录项视角从“记录级”展开为“目录名级”；
    // - 这样同一记录存在多个父目录/硬链接时，目录页也能完整显示。
    struct NtfsDirectoryLink
    {
        std::uint64_t parentIndex = 0;         // 父目录记录号。
        std::uint64_t recordIndex = 0;         // 子记录号。
        QString fileName;                      // 该父目录下的显示名称。
    };

    // Fat32BootInfo 作用：
    // - 保存 FAT32 BPB 中的必要字段；
    // - 用于读取簇链并解析目录项。
    struct Fat32BootInfo
    {
        std::uint16_t bytesPerSector = 512;    // 每扇区字节数。
        std::uint8_t sectorsPerCluster = 8;    // 每簇扇区数。
        std::uint16_t reservedSectors = 0;     // 保留扇区。
        std::uint8_t fatCount = 2;             // FAT 表数量。
        std::uint32_t sectorsPerFat = 0;       // 每 FAT 占用扇区。
        std::uint32_t rootCluster = 2;         // 根目录簇号。
        std::uint64_t fatOffset = 0;           // FAT 表起始偏移（字节）。
        std::uint64_t dataOffset = 0;          // 数据区起始偏移（字节）。
        std::uint32_t bytesPerCluster = 4096;  // 每簇字节数。
    };

    // Fat32Entry 作用：表示 FAT32 目录项原始解析结果。
    struct Fat32Entry
    {
        QString name;                           // 文件名（优先 LFN）。
        std::uint32_t firstCluster = 0;         // 起始簇号。
        std::uint64_t sizeBytes = 0;            // 文件大小。
        bool isDirectory = false;               // 是否目录。
        QDateTime modifiedTime;                 // 修改时间。
    };

    // ExFatBootInfo 作用：
    // - 保存 exFAT Boot Region 中目录枚举需要的字段；
    // - exFAT 的簇堆从 clusterHeapOffset 开始，簇号仍从 2 开始。
    struct ExFatBootInfo
    {
        std::uint64_t fatOffsetBytes = 0;        // FAT 起始字节偏移。
        std::uint64_t clusterHeapOffsetBytes = 0;// 簇堆起始字节偏移。
        std::uint32_t clusterCount = 0;          // 卷内簇数量。
        std::uint32_t rootDirectoryCluster = 2;  // 根目录起始簇。
        std::uint32_t bytesPerSector = 512;      // 每扇区字节数。
        std::uint32_t sectorsPerCluster = 1;     // 每簇扇区数。
        std::uint32_t bytesPerCluster = 512;     // 每簇字节数。
    };

    // ExFatEntry 作用：表示 exFAT 目录项解析结果。
    struct ExFatEntry
    {
        QString name;                            // 文件名。
        std::uint32_t firstCluster = 0;          // 起始簇号。
        std::uint64_t sizeBytes = 0;             // 文件大小。
        bool isDirectory = false;                // 是否目录。
        bool noFatChain = false;                 // true 表示目录/文件内容按连续簇读取。
    };

    // NtfsCacheEntry 作用：
    // - 缓存同一卷最近一次 MFT 解析结果；
    // - 避免手动模式连续切目录时重复扫描造成卡顿。
    struct NtfsCacheEntry
    {
        std::vector<NtfsRawRecord> records;                       // 缓存记录集合。
        std::vector<NtfsDirectoryLink> directoryLinks;            // 目录项级索引集合。
        std::unordered_map<std::uint64_t, std::size_t> recordOffsetByIndex; // 记录号到数组下标映射。
        qint64 loadedMsec = 0;                                    // 缓存时间戳（毫秒）。
        std::uint64_t recordLimit = 0;                            // 本次缓存覆盖的最大记录数上限。
        bool fsctlFallbackAllowed = false;                        // 本次缓存是否允许 FSCTL 回退解析。
    };

    // NtfsRecordKeepPolicy 作用：
    // - 控制扫描过程中哪些记录需要留在结果集里。
    // 为什么需要它：
    // - 误删扫描必须覆盖整个 $MFT：NTFS 记录号从低往高分配，删除后空闲的低号记录
    //   会被优先复用，所以“已删除且未被复用”的记录几乎全部集中在 MFT 尾部；
    //   只扫前若干万条会稳定得到 0 个删除项。
    // - 但整卷 MFT 可达数百万条记录，全量保留会让内存随 MFT 规模线性膨胀，
    //   因此误删扫描只保留删除项与目录（目录用于重建路径提示）。
    enum class NtfsRecordKeepPolicy : int
    {
        All = 0,                  // 保留全部解析成功的记录（目录浏览用）。
        DeletedAndDirectories = 1 // 只保留已删除记录与目录记录（误删扫描用）。
    };

    std::mutex g_ntfsCacheMutex; // NTFS 缓存互斥锁。
    std::unordered_map<std::wstring, std::shared_ptr<NtfsCacheEntry>> g_ntfsCache; // 分卷缓存字典。

    // buildTypeText 前置声明：
    // - 供上方的 WinAPI 补齐辅助函数复用类型文本生成逻辑；
    // - 具体实现保持在后文原位置，避免重复实现。
    QString buildTypeText(const QString& fileName, const bool isDirectory);

    // buildNtfsCacheIndex 作用：
    // - 为缓存记录生成“目录项级索引”和“记录号索引”；
    // - 避免每次切目录都重新遍历全量 MFT 记录。
    bool buildNtfsCacheIndex(
        NtfsCacheEntry& cacheEntry,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        cacheEntry.directoryLinks.clear();
        cacheEntry.recordOffsetByIndex.clear();
        cacheEntry.directoryLinks.reserve(cacheEntry.records.size() * 2);
        cacheEntry.recordOffsetByIndex.reserve(cacheEntry.records.size());

        for (std::size_t i = 0; i < cacheEntry.records.size(); ++i)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            const NtfsRawRecord& recordValue = cacheEntry.records[i];
            cacheEntry.recordOffsetByIndex.emplace(recordValue.recordIndex, i);

            if (!recordValue.nameLinks.empty())
            {
                for (const NtfsNameLink& nameLink : recordValue.nameLinks)
                {
                    if (directoryCanceled(canceled))
                    {
                        return false;
                    }
                    if (nameLink.fileName.isEmpty())
                    {
                        continue;
                    }

                    NtfsDirectoryLink dirLink{};
                    dirLink.parentIndex = nameLink.parentIndex;
                    dirLink.recordIndex = recordValue.recordIndex;
                    dirLink.fileName = nameLink.fileName;
                    cacheEntry.directoryLinks.push_back(std::move(dirLink));
                }
                continue;
            }

            if (!recordValue.fileName.isEmpty())
            {
                NtfsDirectoryLink dirLink{};
                dirLink.parentIndex = recordValue.parentIndex;
                dirLink.recordIndex = recordValue.recordIndex;
                dirLink.fileName = recordValue.fileName;
                cacheEntry.directoryLinks.push_back(std::move(dirLink));
            }
        }

        return sortDirectoryValues(
            cacheEntry.directoryLinks,
            [](const NtfsDirectoryLink& left, const NtfsDirectoryLink& right) {
                if (left.parentIndex != right.parentIndex)
                {
                    return left.parentIndex < right.parentIndex;
                }
                const int compareResult = QString::compare(left.fileName, right.fileName, Qt::CaseInsensitive);
                if (compareResult != 0)
                {
                    return compareResult < 0;
                }
                return left.recordIndex < right.recordIndex;
            },
            canceled);
    }

    // findNtfsDirectoryLinkRange 作用：
    // - 在已按 parentIndex 排序的目录项索引中，定位某个父目录的全部子项范围；
    // - 返回值可直接用于遍历该目录的所有孩子。
    auto findNtfsDirectoryLinkRange(
        const std::vector<NtfsDirectoryLink>& directoryLinks,
        const std::uint64_t parentIndex)
    {
        const auto lowerIt = std::lower_bound(
            directoryLinks.begin(),
            directoryLinks.end(),
            parentIndex,
            [](const NtfsDirectoryLink& linkValue, const std::uint64_t targetParentIndex) {
                return linkValue.parentIndex < targetParentIndex;
            });
        const auto upperIt = std::upper_bound(
            lowerIt,
            directoryLinks.end(),
            parentIndex,
            [](const std::uint64_t targetParentIndex, const NtfsDirectoryLink& linkValue) {
                return targetParentIndex < linkValue.parentIndex;
            });
        return std::make_pair(lowerIt, upperIt);
    }

    // enumerateDirectoryByWinApi 作用：
    // - 用 Windows API/QDir 快速列出目录项；
    // - 用作手动 NTFS 结果无法完整覆盖时的补齐与最终兜底。
    bool enumerateDirectoryByWinApi(
        const QString& pathText,
        std::vector<ks::file::ManualDirectoryEntry>& entriesOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        entriesOut.clear();

        const QString normalizedPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
        QDir fallbackDirectory(normalizedPath);
        if (!fallbackDirectory.exists())
        {
            return false;
        }

        QDirIterator iterator(
            normalizedPath,
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDirIterator::NoIteratorFlags);
        while (iterator.hasNext())
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            iterator.next();
            if (directoryCanceled(canceled))
            {
                return false;
            }
            const QFileInfo fileInfoValue = iterator.fileInfo();
            ks::file::ManualDirectoryEntry itemValue{};
            itemValue.name = fileInfoValue.fileName();
            itemValue.absolutePath = fileInfoValue.absoluteFilePath();
            itemValue.isDirectory = fileInfoValue.isDir();
            itemValue.sizeBytes = itemValue.isDirectory ? 0 : static_cast<std::uint64_t>(fileInfoValue.size());
            itemValue.modifiedTime = fileInfoValue.lastModified();
            itemValue.typeText = buildTypeText(itemValue.name, itemValue.isDirectory);
            entriesOut.push_back(std::move(itemValue));
        }
        return sortDirectoryValues(
            entriesOut,
            [](const ks::file::ManualDirectoryEntry& left, const ks::file::ManualDirectoryEntry& right) {
                if (left.isDirectory != right.isDirectory)
                {
                    return left.isDirectory && !right.isDirectory;
                }
                return QString::compare(left.name, right.name, Qt::CaseInsensitive) < 0;
            },
            canceled);
    }

    // le16/le32/le64 作用：读取小端整数。
    std::uint16_t le16(const std::byte* ptr)
    {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(ptr[0]))
            | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(ptr[1])) << 8);
    }
    std::uint32_t le32(const std::byte* ptr)
    {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(ptr[0]))
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(ptr[1])) << 8)
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(ptr[2])) << 16)
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(ptr[3])) << 24);
    }
    std::uint64_t le64(const std::byte* ptr)
    {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
        {
            value |= (static_cast<std::uint64_t>(static_cast<std::uint8_t>(ptr[i])) << (i * 8));
        }
        return value;
    }


    // trimVolumeRoot 作用：从任意路径提取卷根，如 C:\。
    QString trimVolumeRoot(const QString& pathText)
    {
        const QString cleanText = QDir::toNativeSeparators(QDir::cleanPath(pathText.trimmed()));
        if (cleanText.size() < 2 || cleanText[1] != QChar(':'))
        {
            return QString();
        }
        return cleanText.left(2).toUpper() + QStringLiteral("\\");
    }

    // buildVolumeDevicePath 作用：卷根转设备路径 \\.\C:。
    QString buildVolumeDevicePath(const QString& rootPathText)
    {
        if (rootPathText.size() < 2)
        {
            return QString();
        }
        return QStringLiteral("\\\\.\\%1").arg(rootPathText.left(2).toUpper());
    }

    // toWide 作用：QString 转 UTF-16 宽字符路径。
    std::wstring toWide(const QString& text)
    {
        return std::wstring(reinterpret_cast<const wchar_t*>(text.utf16()));
    }

    // queryExistingPathVolumeIdentity 作用：
    // - 打开已存在的目录并跟随 Junction/符号链接，取得真实卷 GUID；
    // - 网络共享则返回稳定的 UNC 共享根，避免把映射盘误判为本地其它卷；
    // - 非驻留恢复用它阻止经挂载点绕过“不能写回源卷”的安全约束。
    QString queryExistingPathVolumeIdentity(const QString& existingPath)
    {
        const std::wstring nativePath = toWide(
            QDir::toNativeSeparators(QDir::cleanPath(existingPath)));
        HANDLE pathHandle = ::CreateFileW(
            nativePath.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (pathHandle == INVALID_HANDLE_VALUE)
        {
            return QString();
        }

        // queryFinalPath 作用：按指定卷命名格式读取句柄的最终路径。
        const auto queryFinalPath =
            [pathHandle](const DWORD volumeNameFlag) -> QString
            {
                const DWORD queryFlags =
                    FILE_NAME_NORMALIZED | volumeNameFlag;
                const DWORD requiredChars =
                    ::GetFinalPathNameByHandleW(
                        pathHandle,
                        nullptr,
                        0,
                        queryFlags);
                if (requiredChars == 0 ||
                    requiredChars >
                        static_cast<DWORD>(
                            std::numeric_limits<int>::max() - 2))
                {
                    return QString();
                }
                std::vector<wchar_t> pathBuffer(
                    static_cast<std::size_t>(requiredChars) + 2ULL,
                    L'\0');
                const DWORD writtenChars =
                    ::GetFinalPathNameByHandleW(
                        pathHandle,
                        pathBuffer.data(),
                        static_cast<DWORD>(pathBuffer.size()),
                        queryFlags);
                if (writtenChars == 0 ||
                    writtenChars >= static_cast<DWORD>(pathBuffer.size()))
                {
                    return QString();
                }
                return QString::fromWCharArray(
                    pathBuffer.data(),
                    static_cast<int>(writtenChars));
            };

        QString finalPath = queryFinalPath(VOLUME_NAME_GUID);
        if (finalPath.startsWith(
                QStringLiteral("\\\\?\\Volume{"),
                Qt::CaseInsensitive))
        {
            const int volumeEndIndex =
                finalPath.indexOf(QStringLiteral("}\\"));
            ::CloseHandle(pathHandle);
            return volumeEndIndex >= 0
                ? finalPath.left(volumeEndIndex + 2).toUpper()
                : QString();
        }

        // 网络共享不支持 VOLUME_NAME_GUID；退回 DOS/UNC 最终路径并只保留共享根。
        finalPath = queryFinalPath(VOLUME_NAME_DOS);
        ::CloseHandle(pathHandle);
        const QString uncPrefix = QStringLiteral("\\\\?\\UNC\\");
        if (finalPath.startsWith(uncPrefix, Qt::CaseInsensitive))
        {
            const QStringList pathParts =
                finalPath.mid(uncPrefix.size()).split(
                    QChar('\\'),
                    Qt::SkipEmptyParts);
            if (pathParts.size() >= 2)
            {
                return QString::fromLatin1("UNC:%1\\%2")
                    .arg(pathParts.at(0), pathParts.at(1))
                    .toUpper();
            }
            return QString();
        }
        const QString dosPrefix = QStringLiteral("\\\\?\\");
        if (finalPath.startsWith(dosPrefix, Qt::CaseInsensitive) &&
            finalPath.size() >= dosPrefix.size() + 3 &&
            finalPath.at(dosPrefix.size() + 1) == QChar(':'))
        {
            return (
                QString::fromLatin1("DOS:") +
                finalPath.mid(dosPrefix.size(), 3))
                .toUpper();
        }
        return QString();
    }

    // readBytesAtOffset 作用：在指定偏移读取固定长度字节块。
    bool readBytesAtOffset(
        const HANDLE fileHandle,
        const std::uint64_t offsetValue,
        const std::uint32_t sizeValue,
        std::byte* bufferPtr,
        QString& errorTextOut)
    {
        if (bufferPtr == nullptr ||
            offsetValue >
                static_cast<std::uint64_t>(
                    std::numeric_limits<LONGLONG>::max()) ||
            static_cast<std::uint64_t>(sizeValue) >
                static_cast<std::uint64_t>(
                    std::numeric_limits<LONGLONG>::max()) -
                    offsetValue)
        {
            errorTextOut = QStringLiteral(
                "读取偏移或长度超出 Windows 文件指针范围, offset=%1, size=%2")
                .arg(static_cast<qulonglong>(offsetValue))
                .arg(sizeValue);
            return false;
        }

        LARGE_INTEGER targetOffset{};
        targetOffset.QuadPart = static_cast<LONGLONG>(offsetValue);
        if (::SetFilePointerEx(fileHandle, targetOffset, nullptr, FILE_BEGIN) == FALSE)
        {
            errorTextOut = QStringLiteral("SetFilePointerEx失败, code=%1").arg(::GetLastError());
            return false;
        }

        DWORD readSize = 0;
        if (::ReadFile(fileHandle, bufferPtr, sizeValue, &readSize, nullptr) == FALSE)
        {
            errorTextOut = QStringLiteral("ReadFile失败, code=%1").arg(::GetLastError());
            return false;
        }
        if (readSize != sizeValue)
        {
            errorTextOut = QStringLiteral("读取长度不足, expect=%1, actual=%2").arg(sizeValue).arg(readSize);
            return false;
        }
        return true;
    }

    // readBytesAtSectorAlignedOffset 作用：
    // - 输入任意卷内字节偏移和读取长度，内部先扩大为扇区对齐的读取窗口；
    // - 处理 raw volume 句柄在 FAT/FAT32/exFAT FAT 表项读取时拒绝非扇区对齐 seek 的情况；
    // - 成功时仅把调用方请求的原始字节范围复制到 bufferPtr，函数本身无其它返回数据；
    // - 失败时返回 false，并把阶段、簇号、原始偏移、对齐偏移和扇区大小写入 errorTextOut。
    bool readBytesAtSectorAlignedOffset(
        const HANDLE fileHandle,
        const std::uint64_t offsetValue,
        const std::uint32_t sizeValue,
        const std::uint32_t sectorSizeValue,
        const QString& stageText,
        const std::uint32_t clusterValue,
        std::byte* bufferPtr,
        QString& errorTextOut)
    {
        if (bufferPtr == nullptr || sizeValue == 0)
        {
            errorTextOut = QStringLiteral("%1失败：读取参数为空, cluster=%2, entryOffset=0x%3, size=%4, sectorSize=%5")
                .arg(stageText)
                .arg(clusterValue)
                .arg(QString::number(offsetValue, 16).toUpper())
                .arg(sizeValue)
                .arg(sectorSizeValue);
            return false;
        }
        if (sectorSizeValue == 0)
        {
            errorTextOut = QStringLiteral("%1失败：扇区大小为0, cluster=%2, entryOffset=0x%3")
                .arg(stageText)
                .arg(clusterValue)
                .arg(QString::number(offsetValue, 16).toUpper());
            return false;
        }
        if (offsetValue > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(sizeValue))
        {
            errorTextOut = QStringLiteral("%1失败：读取偏移溢出, cluster=%2, entryOffset=0x%3, size=%4, sectorSize=%5")
                .arg(stageText)
                .arg(clusterValue)
                .arg(QString::number(offsetValue, 16).toUpper())
                .arg(sizeValue)
                .arg(sectorSizeValue);
            return false;
        }

        const std::uint64_t sectorSize = static_cast<std::uint64_t>(sectorSizeValue);
        const std::uint64_t alignedOffset = (offsetValue / sectorSize) * sectorSize;
        const std::uint64_t inSectorOffset = offsetValue - alignedOffset;
        const std::uint64_t requestEndOffset = offsetValue + static_cast<std::uint64_t>(sizeValue);
        if (requestEndOffset > std::numeric_limits<std::uint64_t>::max() - (sectorSize - 1ULL)
            || alignedOffset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        {
            errorTextOut = QStringLiteral("%1失败：对齐偏移溢出, cluster=%2, entryOffset=0x%3, alignedOffset=0x%4, size=%5, sectorSize=%6")
                .arg(stageText)
                .arg(clusterValue)
                .arg(QString::number(offsetValue, 16).toUpper())
                .arg(QString::number(alignedOffset, 16).toUpper())
                .arg(sizeValue)
                .arg(sectorSizeValue);
            return false;
        }
        const std::uint64_t alignedEndOffset =
            ((requestEndOffset + sectorSize - 1ULL) / sectorSize) * sectorSize;
        const std::uint64_t alignedSize64 = alignedEndOffset - alignedOffset;
        if (alignedSize64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            errorTextOut = QStringLiteral("%1失败：对齐读取窗口过大, cluster=%2, entryOffset=0x%3, alignedOffset=0x%4, alignedSize=%5, sectorSize=%6")
                .arg(stageText)
                .arg(clusterValue)
                .arg(QString::number(offsetValue, 16).toUpper())
                .arg(QString::number(alignedOffset, 16).toUpper())
                .arg(alignedSize64)
                .arg(sectorSizeValue);
            return false;
        }

        std::vector<std::byte> alignedBytes(static_cast<std::size_t>(alignedSize64));
        QString innerErrorText;
        if (!readBytesAtOffset(
            fileHandle,
            alignedOffset,
            static_cast<std::uint32_t>(alignedSize64),
            alignedBytes.data(),
            innerErrorText))
        {
            errorTextOut = QStringLiteral("%1失败, cluster=%2, entryOffset=0x%3, alignedOffset=0x%4, alignedSize=%5, sectorSize=%6, inner=%7")
                .arg(stageText)
                .arg(clusterValue)
                .arg(QString::number(offsetValue, 16).toUpper())
                .arg(QString::number(alignedOffset, 16).toUpper())
                .arg(alignedSize64)
                .arg(sectorSizeValue)
                .arg(innerErrorText);
            return false;
        }

        std::memcpy(
            bufferPtr,
            alignedBytes.data() + static_cast<std::size_t>(inSectorOffset),
            static_cast<std::size_t>(sizeValue));
        return true;
    }

    // guessDeletedFileExtension 作用：
    // - 当原始文件名已经丢失时，尽量根据 resident 数据头猜测扩展名；
    // - 若无法判断，则统一回落为 bin。
    QString guessDeletedFileExtension(const QByteArray& residentData)
    {
        if (residentData.size() >= 8
            && static_cast<unsigned char>(residentData[0]) == 0x89
            && residentData.mid(1, 3) == "PNG")
        {
            return QStringLiteral("png");
        }
        if (residentData.size() >= 3
            && static_cast<unsigned char>(residentData[0]) == 0xFF
            && static_cast<unsigned char>(residentData[1]) == 0xD8
            && static_cast<unsigned char>(residentData[2]) == 0xFF)
        {
            return QStringLiteral("jpg");
        }
        if (residentData.size() >= 4 && residentData.left(4) == "%PDF")
        {
            return QStringLiteral("pdf");
        }
        if (residentData.size() >= 4 && residentData.left(4) == "PK\x03\x04")
        {
            return QStringLiteral("zip");
        }
        if (residentData.size() >= 6 && (residentData.left(6) == "GIF87a" || residentData.left(6) == "GIF89a"))
        {
            return QStringLiteral("gif");
        }
        if (residentData.size() >= 2 && residentData.left(2) == "BM")
        {
            return QStringLiteral("bmp");
        }
        if (residentData.size() >= 8
            && static_cast<unsigned char>(residentData[0]) == 0x52
            && static_cast<unsigned char>(residentData[1]) == 0x61
            && static_cast<unsigned char>(residentData[2]) == 0x72
            && static_cast<unsigned char>(residentData[3]) == 0x21)
        {
            return QStringLiteral("rar");
        }
        return QStringLiteral("bin");
    }

    // buildSyntheticDeletedFileName 作用：
    // - 为“名称缺失”的删除记录生成占位文件名；
    // - 便于结果列表展示与后续导出落盘。
    QString buildSyntheticDeletedFileName(const NtfsRawRecord& recordValue)
    {
        QString suffixText = QStringLiteral("bin");
        if (!recordValue.residentData.isEmpty())
        {
            suffixText = guessDeletedFileExtension(recordValue.residentData);
        }

        return QStringLiteral("deleted_%1.%2")
            .arg(static_cast<qulonglong>(recordValue.recordIndex))
            .arg(suffixText);
    }

    // fileTimeToLocal 作用：FILETIME(100ns) 转本地时间。
    QDateTime fileTimeToLocal(const std::uint64_t fileTime100ns)
    {
        if (fileTime100ns == 0)
        {
            return QDateTime();
        }
        constexpr qint64 EpochDeltaMsec = 11644473600000LL;
        const qint64 unixMsec = static_cast<qint64>(fileTime100ns / 10000ULL) - EpochDeltaMsec;
        return QDateTime::fromMSecsSinceEpoch(unixMsec, QTimeZone::UTC).toLocalTime();
    }

    // openReadHandle 作用：统一以共享只读打开句柄。
    HANDLE openReadHandle(const QString& nativePathText, QString& errorTextOut)
    {
        const std::wstring pathWide = toWide(nativePathText);

        // enablePrivilegeByName 作用：按需启用当前进程令牌特权（如 SeBackupPrivilege）。
        const auto enablePrivilegeByName = [](const wchar_t* privilegeName) -> bool {
            if (privilegeName == nullptr)
            {
                return false;
            }
            HANDLE tokenHandle = nullptr;
            if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tokenHandle) == FALSE)
            {
                return false;
            }

            LUID luidValue{};
            if (::LookupPrivilegeValueW(nullptr, privilegeName, &luidValue) == FALSE)
            {
                ::CloseHandle(tokenHandle);
                return false;
            }

            TOKEN_PRIVILEGES privileges{};
            privileges.PrivilegeCount = 1;
            privileges.Privileges[0].Luid = luidValue;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            ::SetLastError(ERROR_SUCCESS);
            const BOOL adjustOk = ::AdjustTokenPrivileges(
                tokenHandle,
                FALSE,
                &privileges,
                static_cast<DWORD>(sizeof(privileges)),
                nullptr,
                nullptr);
            const DWORD adjustError = ::GetLastError();
            ::CloseHandle(tokenHandle);
            return adjustOk != FALSE && adjustError == ERROR_SUCCESS;
        };

        // 第一轮：常规只读打开。
        HANDLE handleValue = ::CreateFileW(
            pathWide.c_str(),
            FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handleValue == INVALID_HANDLE_VALUE)
        {
            const DWORD firstError = ::GetLastError();

            // 第二轮：针对 $MFT/系统元文件，启用备份相关特权并加 BackupSemantics 再试。
            if (firstError == ERROR_ACCESS_DENIED)
            {
                enablePrivilegeByName(SE_BACKUP_NAME);
                enablePrivilegeByName(SE_RESTORE_NAME);
                enablePrivilegeByName(SE_MANAGE_VOLUME_NAME);
                handleValue = ::CreateFileW(
                    pathWide.c_str(),
                    FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr);
            }

            if (handleValue == INVALID_HANDLE_VALUE)
            {
                errorTextOut = QStringLiteral("CreateFile失败: %1, code=%2").arg(nativePathText).arg(::GetLastError());
                return INVALID_HANDLE_VALUE;
            }
        }
        return handleValue;
    }

    // loadNtfsVolumeBitmapSnapshot 作用：
    // - 读取整卷簇位图；
    // - 供误删扫描估算“数据簇是否仍未被覆盖”。
    bool loadNtfsVolumeBitmapSnapshot(
        const QString& volumeRoot,
        NtfsVolumeBitmapSnapshot& bitmapOut,
        QString& errorTextOut)
    {
        bitmapOut = NtfsVolumeBitmapSnapshot{};
        errorTextOut.clear();

        QString openErrorText;
        HANDLE volumeHandle = openReadHandle(buildVolumeDevicePath(volumeRoot), openErrorText);
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openErrorText;
            return false;
        }

        NTFS_VOLUME_DATA_BUFFER volumeData{};
        DWORD returnedBytes = 0;
        if (::DeviceIoControl(
            volumeHandle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            nullptr,
            0,
            &volumeData,
            static_cast<DWORD>(sizeof(volumeData)),
            &returnedBytes,
            nullptr) == FALSE)
        {
            errorTextOut = QStringLiteral("FSCTL_GET_NTFS_VOLUME_DATA失败, code=%1").arg(::GetLastError());
            ::CloseHandle(volumeHandle);
            return false;
        }

        const std::uint64_t totalClusters =
            static_cast<std::uint64_t>(volumeData.TotalClusters.QuadPart);
        if (totalClusters == 0)
        {
            errorTextOut = QStringLiteral("卷位图为空。");
            ::CloseHandle(volumeHandle);
            return false;
        }

        // FSCTL_GET_VOLUME_BITMAP 一次只返回输出缓冲区装得下的那一段，
        // 缓冲区不足时以 ERROR_MORE_DATA 返回并填满缓冲区，因此必须循环续读。
        // 早期实现按“整卷位图一次拿全”申请缓冲区：大卷直接被上限挡掉，
        // 位图缺失又会让所有非驻留项退化成“完整度未知 / 禁止导出”。
        constexpr std::size_t BitmapChunkPayloadBytes = 8ULL * 1024ULL * 1024ULL;
        constexpr std::uint64_t MaxBitmapBytes = 256ULL * 1024ULL * 1024ULL;
        const std::size_t headerBytes = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
        std::vector<std::uint8_t> outputBuffer(headerBytes + BitmapChunkPayloadBytes + 64ULL);

        bitmapOut.startingLcn = 0;
        bitmapOut.clusterCount = 0;
        bitmapOut.bitmapBytes.clear();
        bitmapOut.bitmapBytes.reserve(
            static_cast<std::size_t>(
                std::min<std::uint64_t>((totalClusters + 7ULL) / 8ULL, MaxBitmapBytes)));

        std::uint64_t nextLcn = 0;
        while (nextLcn < totalClusters)
        {
            STARTING_LCN_INPUT_BUFFER inputBuffer{};
            inputBuffer.StartingLcn.QuadPart = static_cast<LONGLONG>(nextLcn);
            returnedBytes = 0;
            const BOOL queryOk = ::DeviceIoControl(
                volumeHandle,
                FSCTL_GET_VOLUME_BITMAP,
                &inputBuffer,
                static_cast<DWORD>(sizeof(inputBuffer)),
                outputBuffer.data(),
                static_cast<DWORD>(outputBuffer.size()),
                &returnedBytes,
                nullptr);
            const DWORD queryErrorCode = queryOk != FALSE ? ERROR_SUCCESS : ::GetLastError();
            if (queryOk == FALSE && queryErrorCode != ERROR_MORE_DATA)
            {
                ::CloseHandle(volumeHandle);
                errorTextOut = QStringLiteral("FSCTL_GET_VOLUME_BITMAP失败, startLcn=%1, code=%2")
                    .arg(static_cast<qulonglong>(nextLcn))
                    .arg(queryErrorCode);
                return false;
            }
            if (returnedBytes <= headerBytes)
            {
                ::CloseHandle(volumeHandle);
                errorTextOut = QStringLiteral("卷位图返回长度不足, startLcn=%1")
                    .arg(static_cast<qulonglong>(nextLcn));
                return false;
            }

            const VOLUME_BITMAP_BUFFER* bitmapBuffer =
                reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(outputBuffer.data());
            // 驱动会把 StartingLcn 向下对齐到字节边界；nextLcn 始终保持 8 的倍数，
            // 因此这里对齐后的起点必须与请求一致，否则说明段间出现空洞，直接停止。
            const std::uint64_t chunkStartLcn =
                static_cast<std::uint64_t>(bitmapBuffer->StartingLcn.QuadPart);
            if (chunkStartLcn != nextLcn)
            {
                break;
            }

            // BitmapSize 是“从 StartingLcn 到卷末尾的剩余簇数”，不是本段返回的簇数。
            const std::uint64_t remainingClusters =
                static_cast<std::uint64_t>(bitmapBuffer->BitmapSize.QuadPart);
            const std::uint64_t payloadBytes =
                static_cast<std::uint64_t>(returnedBytes) - headerBytes;
            const std::uint64_t chunkClusters =
                std::min<std::uint64_t>(remainingClusters, payloadBytes * 8ULL);
            if (chunkClusters == 0)
            {
                break;
            }
            const std::uint64_t chunkBytes = (chunkClusters + 7ULL) / 8ULL;
            if (chunkBytes > payloadBytes)
            {
                ::CloseHandle(volumeHandle);
                errorTextOut = QStringLiteral("卷位图数据长度异常, startLcn=%1")
                    .arg(static_cast<qulonglong>(chunkStartLcn));
                return false;
            }

            const std::uint8_t* payloadPtr = outputBuffer.data() + headerBytes;
            bitmapOut.bitmapBytes.insert(
                bitmapOut.bitmapBytes.end(),
                payloadPtr,
                payloadPtr + static_cast<std::size_t>(chunkBytes));
            bitmapOut.clusterCount += chunkClusters;
            nextLcn = chunkStartLcn + chunkClusters;

            // 超大卷只保留前段位图：部分覆盖仍可判定落在该范围内的删除项，
            // 范围之外由 tryCountAllocatedClustersInRange 判为“无法评估”，不会误判为安全。
            if (bitmapOut.bitmapBytes.size() >= static_cast<std::size_t>(MaxBitmapBytes))
            {
                errorTextOut = QStringLiteral("卷位图超过 %1MB 上限，仅加载前 %2 个簇。")
                    .arg(static_cast<qulonglong>(MaxBitmapBytes / (1024ULL * 1024ULL)))
                    .arg(static_cast<qulonglong>(bitmapOut.clusterCount));
                break;
            }
        }
        ::CloseHandle(volumeHandle);

        if (bitmapOut.clusterCount == 0 || bitmapOut.bitmapBytes.empty())
        {
            errorTextOut = QStringLiteral("卷位图未返回任何有效数据。");
            return false;
        }
        return true;
    }

    // tryCountAllocatedClustersInRange 作用：
    // - 统计指定簇范围内当前已分配的簇数量；
    // - 返回 false 表示位图不覆盖该区间，当前无法评估。
    bool tryCountAllocatedClustersInRange(
        const NtfsVolumeBitmapSnapshot& bitmapValue,
        const std::uint64_t startLcn,
        const std::uint64_t clusterCount,
        std::uint64_t& allocatedClustersOut)
    {
        allocatedClustersOut = 0;
        if (clusterCount == 0)
        {
            return true;
        }
        if (startLcn < bitmapValue.startingLcn)
        {
            return false;
        }

        const std::uint64_t relativeStartBit = startLcn - bitmapValue.startingLcn;
        if (clusterCount > bitmapValue.clusterCount ||
            relativeStartBit > bitmapValue.clusterCount - clusterCount)
        {
            return false;
        }

        const std::uint64_t endBitExclusive = relativeStartBit + clusterCount;
        std::uint64_t currentBit = relativeStartBit;
        while (currentBit < endBitExclusive)
        {
            const std::uint64_t byteIndex = currentBit / 8ULL;
            const std::uint8_t bitOffset = static_cast<std::uint8_t>(currentBit % 8ULL);
            const std::uint64_t bitsInThisByte =
                std::min<std::uint64_t>(8ULL - bitOffset, endBitExclusive - currentBit);

            std::uint8_t byteValue = bitmapValue.bitmapBytes[static_cast<std::size_t>(byteIndex)];
            byteValue = static_cast<std::uint8_t>(byteValue >> bitOffset);
            if (bitsInThisByte < 8ULL)
            {
                const std::uint8_t maskValue = static_cast<std::uint8_t>((1u << bitsInThisByte) - 1u);
                byteValue = static_cast<std::uint8_t>(byteValue & maskValue);
            }

            allocatedClustersOut += static_cast<std::uint64_t>(std::popcount(static_cast<unsigned int>(byteValue)));
            currentBit += bitsInThisByte;
        }
        return true;
    }

    // estimateDeletedRecordIntegrityPercent 作用：
    // - 根据 resident 状态或非 resident 数据簇是否仍空闲，估算文件完整度；
    // - 结果用于恢复列表排序与“完整度”展示。
    int estimateDeletedRecordIntegrityPercent(
        const NtfsRawRecord& recordValue,
        const NtfsVolumeBitmapSnapshot* bitmapValue)
    {
        if (recordValue.residentReady && !recordValue.nonResidentData)
        {
            return 100;
        }
        if (!recordValue.nonResidentData || bitmapValue == nullptr || recordValue.dataRuns.empty())
        {
            return -1;
        }

        std::uint64_t totalClusters = 0;
        std::uint64_t intactClusters = 0;
        for (const NtfsDataRun& runValue : recordValue.dataRuns)
        {
            if (totalClusters >
                std::numeric_limits<std::uint64_t>::max() -
                    runValue.clusterCount)
            {
                return -1;
            }
            totalClusters += runValue.clusterCount;
            if (runValue.isSparse)
            {
                if (intactClusters >
                    std::numeric_limits<std::uint64_t>::max() -
                        runValue.clusterCount)
                {
                    return -1;
                }
                intactClusters += runValue.clusterCount;
                continue;
            }

            std::uint64_t allocatedClusters = 0;
            if (!tryCountAllocatedClustersInRange(
                bitmapValue[0],
                runValue.startLcn,
                runValue.clusterCount,
                allocatedClusters))
            {
                return -1;
            }
            if (allocatedClusters > runValue.clusterCount)
            {
                return -1;
            }
            const std::uint64_t freeClusterCount =
                runValue.clusterCount - allocatedClusters;
            if (intactClusters >
                std::numeric_limits<std::uint64_t>::max() - freeClusterCount)
            {
                return -1;
            }
            intactClusters += freeClusterCount;
        }

        if (totalClusters == 0)
        {
            return (recordValue.sizeBytes == 0) ? 100 : -1;
        }

        return static_cast<int>(
            (static_cast<long double>(intactClusters) * 100.0L /
                static_cast<long double>(totalClusters)) +
            0.5L);
    }

    // deletedRecordRecoveryCapability 作用：
    // - 把底层记录布局和卷位图完整度归一为 UI 可消费的恢复能力；
    // - “可恢复”仅表示扫描时满足条件，真正导出前仍会重新校验两次卷位图。
    ks::file::NtfsRecoveryCapability deletedRecordRecoveryCapability(
        const NtfsRawRecord& recordValue,
        const int integrityPercent)
    {
        if (recordValue.residentReady &&
            !recordValue.nonResidentData &&
            !recordValue.unsupportedDataStream)
        {
            return ks::file::NtfsRecoveryCapability::Resident;
        }
        if (!recordValue.nonResidentData)
        {
            return recordValue.unsupportedDataStream
                ? ks::file::NtfsRecoveryCapability::UnsupportedStream
                : ks::file::NtfsRecoveryCapability::MetadataOnly;
        }
        if (recordValue.unsupportedDataStream || recordValue.hasAttributeList)
        {
            return ks::file::NtfsRecoveryCapability::UnsupportedStream;
        }
        if (recordValue.dataRuns.empty())
        {
            return ks::file::NtfsRecoveryCapability::MetadataOnly;
        }
        return integrityPercent == 100
            ? ks::file::NtfsRecoveryCapability::NonResidentIntact
            : ks::file::NtfsRecoveryCapability::NonResidentAtRisk;
    }

    // validateDeletedDataRunsUnallocated 作用：
    // - 要求每个非稀疏数据簇在当前卷位图中仍是“未分配”；
    // - 任一簇已复用、越界或位图缺失都拒绝恢复，避免输出混合的新旧数据。
    bool validateDeletedDataRunsUnallocated(
        const NtfsRawRecord& recordValue,
        const NtfsVolumeBitmapSnapshot& bitmapValue,
        QString& errorTextOut)
    {
        for (const NtfsDataRun& runValue : recordValue.dataRuns)
        {
            if (runValue.isSparse)
            {
                continue;
            }
            std::uint64_t allocatedClusters = 0;
            if (!tryCountAllocatedClustersInRange(
                bitmapValue,
                runValue.startLcn,
                runValue.clusterCount,
                allocatedClusters))
            {
                errorTextOut = QStringLiteral("卷位图未覆盖完整数据段，无法证明恢复安全。");
                return false;
            }
            if (allocatedClusters != 0)
            {
                errorTextOut = QStringLiteral(
                    "检测到 %1 个数据簇已被重新分配，已拒绝输出可能损坏的文件。")
                    .arg(static_cast<qulonglong>(allocatedClusters));
                return false;
            }
        }
        return true;
    }

    // sameDeletedDataRunLayout 作用：
    // - 比较恢复读取前后的主数据 runlist；
    // - MFT 记录在导出过程中一旦变化就不提交临时文件。
    bool sameDeletedDataRunLayout(
        const NtfsRawRecord& firstRecord,
        const NtfsRawRecord& secondRecord)
    {
        if (firstRecord.sequenceNumber != secondRecord.sequenceNumber ||
            firstRecord.sizeBytes != secondRecord.sizeBytes ||
            firstRecord.initializedSizeBytes != secondRecord.initializedSizeBytes ||
            firstRecord.dataAttributeFlags != secondRecord.dataAttributeFlags ||
            firstRecord.hasPrimaryDataStream != secondRecord.hasPrimaryDataStream ||
            firstRecord.nonResidentData != secondRecord.nonResidentData ||
            firstRecord.unsupportedDataStream != secondRecord.unsupportedDataStream ||
            firstRecord.hasAttributeList != secondRecord.hasAttributeList ||
            firstRecord.dataRuns.size() != secondRecord.dataRuns.size())
        {
            return false;
        }
        for (std::size_t runIndex = 0;
             runIndex < firstRecord.dataRuns.size();
             ++runIndex)
        {
            const NtfsDataRun& firstRun = firstRecord.dataRuns[runIndex];
            const NtfsDataRun& secondRun = secondRecord.dataRuns[runIndex];
            if (firstRun.startLcn != secondRun.startLcn ||
                firstRun.clusterCount != secondRun.clusterCount ||
                firstRun.isSparse != secondRun.isSparse)
            {
                return false;
            }
        }
        return true;
    }

    // ntfsFixup 作用：应用 NTFS USA 修复，保证扇区尾校验通过。
    bool ntfsFixup(std::vector<std::byte>& recordBytes, const std::uint16_t bytesPerSectorHint)
    {
        if (recordBytes.size() < 64)
        {
            return false;
        }
        const std::uint16_t usaOffset = le16(recordBytes.data() + 4);
        const std::uint16_t usaCount = le16(recordBytes.data() + 6);
        if (usaOffset < 8 || usaCount < 2)
        {
            return false;
        }
        const std::size_t usaBytes = static_cast<std::size_t>(usaCount) * 2;
        if (usaOffset + usaBytes > recordBytes.size())
        {
            return false;
        }

        const std::byte* usaPtr = recordBytes.data() + usaOffset;
        const std::uint16_t signature = le16(usaPtr);
        // usaSectorCount 用途：记录当前 FILE 记录被切成多少个物理扇区片段。
        const std::size_t usaSectorCount = static_cast<std::size_t>(usaCount - 1);
        if (usaSectorCount == 0)
        {
            return false;
        }

        // sectorStrideBytes 用途：USA 修复时每一段的跨度，优先验证真实扇区大小，不再写死 512。
        std::size_t sectorStrideBytes = 0;
        if (bytesPerSectorHint >= 256
            && bytesPerSectorHint <= recordBytes.size()
            && (recordBytes.size() % bytesPerSectorHint) == 0
            && (recordBytes.size() / bytesPerSectorHint) == usaSectorCount)
        {
            sectorStrideBytes = static_cast<std::size_t>(bytesPerSectorHint);
        }
        else if ((recordBytes.size() % usaSectorCount) == 0)
        {
            sectorStrideBytes = recordBytes.size() / usaSectorCount;
        }
        if (sectorStrideBytes < 256)
        {
            return false;
        }

        for (std::uint16_t i = 1; i < usaCount; ++i)
        {
            const std::size_t tailOffset = static_cast<std::size_t>(i) * sectorStrideBytes - 2ULL;
            if (tailOffset + 2 > recordBytes.size())
            {
                return false;
            }
            if (le16(recordBytes.data() + tailOffset) != signature)
            {
                return false;
            }
            const std::uint16_t fixedValue = le16(usaPtr + i * 2);
            recordBytes[tailOffset] = static_cast<std::byte>(fixedValue & 0xFF);
            recordBytes[tailOffset + 1] = static_cast<std::byte>((fixedValue >> 8) & 0xFF);
        }
        return true;
    }

    // parseNtfsRecord 作用：解析单条 MFT 记录，抽取名称/父目录/大小/驻留数据。
    bool parseNtfsRecord(
        std::vector<std::byte>& recordBytes,
        const std::uint64_t recordIndex,
        const std::uint16_t bytesPerSectorHint,
        const bool captureResidentData,
        NtfsRawRecord& recordOut)
    {
        if (recordBytes.size() < 64 || std::memcmp(recordBytes.data(), "FILE", 4) != 0)
        {
            return false;
        }
        if (!ntfsFixup(recordBytes, bytesPerSectorHint))
        {
            return false;
        }

        recordOut = NtfsRawRecord{};
        recordOut.recordIndex = recordIndex;
        recordOut.sequenceNumber = le16(recordBytes.data() + 16);
        const std::uint16_t flags = le16(recordBytes.data() + 22);
        recordOut.inUse = ((flags & 0x0001) != 0);
        recordOut.isDirectory = ((flags & 0x0002) != 0);

        const std::uint16_t attrOffsetStart = le16(recordBytes.data() + 20);
        if (attrOffsetStart >= recordBytes.size())
        {
            return false;
        }

        QString preferredName;
        int preferredNameScore = -1;
        std::size_t attrOffset = attrOffsetStart;
        while (attrOffset + 24 <= recordBytes.size())
        {
            const std::uint32_t attrType = le32(recordBytes.data() + attrOffset);
            if (attrType == 0xFFFFFFFF)
            {
                break;
            }
            const std::uint32_t attrLength = le32(recordBytes.data() + attrOffset + 4);
            if (attrLength < 24 || attrOffset + attrLength > recordBytes.size())
            {
                break;
            }
            const std::size_t attrEnd = attrOffset + attrLength;

            const bool nonResident = (recordBytes[attrOffset + 8] != std::byte{ 0 });
            const std::uint8_t attrNameLength = static_cast<std::uint8_t>(recordBytes[attrOffset + 9]);
            if (attrType == 0x20)
            {
                // $ATTRIBUTE_LIST 只有在确实列出外部/后续未命名 $DATA extent 时
                // 才阻止恢复；仅列出其它属性不会误伤可完整读取的主数据流。
                if (nonResident)
                {
                    recordOut.hasAttributeList = true;
                }
                else
                {
                    const std::uint32_t listLength =
                        le32(recordBytes.data() + attrOffset + 16);
                    const std::uint16_t listOffset =
                        le16(recordBytes.data() + attrOffset + 20);
                    const std::size_t listStart = attrOffset + listOffset;
                    if (listStart + listLength > attrEnd)
                    {
                        recordOut.hasAttributeList = true;
                    }
                    else
                    {
                        std::size_t listEntryOffset = listStart;
                        while (listEntryOffset + 26 <= listStart + listLength)
                        {
                            const std::uint32_t listedType =
                                le32(recordBytes.data() + listEntryOffset);
                            const std::uint16_t listedLength =
                                le16(recordBytes.data() + listEntryOffset + 4);
                            const std::uint8_t listedNameLength =
                                static_cast<std::uint8_t>(
                                    recordBytes[listEntryOffset + 6]);
                            if (listedLength < 26 ||
                                listEntryOffset + listedLength >
                                    listStart + listLength)
                            {
                                recordOut.hasAttributeList = true;
                                break;
                            }
                            if (listedType == 0x80 && listedNameLength == 0)
                            {
                                const std::uint64_t listedLowestVcn =
                                    le64(recordBytes.data() + listEntryOffset + 8);
                                const std::uint64_t listedRecordIndex =
                                    le64(recordBytes.data() + listEntryOffset + 16) &
                                    0x0000FFFFFFFFFFFFULL;
                                if (listedLowestVcn > 0 ||
                                    listedRecordIndex != recordIndex)
                                {
                                    recordOut.hasAttributeList = true;
                                }
                            }
                            listEntryOffset += listedLength;
                        }
                    }
                }
            }
            else if (attrType == 0x30 && !nonResident)
            {
                const std::uint32_t contentLength = le32(recordBytes.data() + attrOffset + 16);
                const std::uint16_t contentOffset = le16(recordBytes.data() + attrOffset + 20);
                const std::size_t contentStart = attrOffset + contentOffset;
                if (contentLength >= 66 && contentStart + contentLength <= attrEnd)
                {
                    const std::byte* contentPtr = recordBytes.data() + contentStart;
                    const std::uint64_t parentRef = le64(contentPtr);
                    const std::uint64_t modified100ns = le64(contentPtr + 16);
                    const std::uint64_t realSize = le64(contentPtr + 48);
                    const std::uint8_t nameLength = static_cast<std::uint8_t>(contentPtr[64]);
                    const std::uint8_t nameNamespace = static_cast<std::uint8_t>(contentPtr[65]);
                    const std::size_t nameBytes = static_cast<std::size_t>(nameLength) * 2ULL;
                    if (66 + nameBytes <= contentLength)
                    {
                        const QString candidateName = QString::fromUtf16(
                            reinterpret_cast<const char16_t*>(contentPtr + 66),
                            static_cast<qsizetype>(nameLength));
                        const int score = (nameNamespace == 1 || nameNamespace == 3) ? 2 : (nameNamespace == 0 ? 1 : 0);
                        const std::uint64_t parentIndexValue = (parentRef & 0x0000FFFFFFFFFFFFULL);

                        // nameLinks：NTFS 目录项本质上是“文件名链接”，不是“一个记录只能显示一行”。
                        // 这里按“父目录 + 文件名”保留链接，避免同一目录下多个硬链接被压成一个。
                        bool parentLinkUpdated = false;
                        for (NtfsNameLink& nameLink : recordOut.nameLinks)
                        {
                            if (nameLink.parentIndex != parentIndexValue
                                || nameLink.fileName.compare(candidateName, Qt::CaseSensitive) != 0)
                            {
                                continue;
                            }

                            // 同名链接只保留更适合展示的命名空间版本，主要用于消除大小写/命名空间重复。
                            if (score >= nameLink.nameScore)
                            {
                                nameLink.fileName = candidateName;
                                nameLink.nameScore = score;
                            }
                            parentLinkUpdated = true;
                            break;
                        }
                        if (!parentLinkUpdated)
                        {
                            NtfsNameLink nameLink{};
                            nameLink.parentIndex = parentIndexValue;
                            nameLink.fileName = candidateName;
                            nameLink.nameScore = score;
                            recordOut.nameLinks.push_back(std::move(nameLink));
                        }

                        // preferredName：记录级默认名称只用于路径提示，不参与目录行去重。
                        if (score >= preferredNameScore)
                        {
                            preferredNameScore = score;
                            preferredName = candidateName;
                            recordOut.parentIndex = parentIndexValue;
                            recordOut.modifiedTime100ns = modified100ns;
                            recordOut.sizeBytes = realSize;
                        }
                        else
                        {
                            if (recordOut.modifiedTime100ns == 0 && modified100ns != 0)
                            {
                                recordOut.modifiedTime100ns = modified100ns;
                            }
                            if (recordOut.sizeBytes == 0 && realSize != 0)
                            {
                                recordOut.sizeBytes = realSize;
                            }
                        }
                    }
                }
            }
            else if (attrType == 0x80)
            {
                // 只处理未命名主数据流，避免把 ADS 误判为文件主内容。
                if (attrNameLength != 0)
                {
                    attrOffset += attrLength;
                    continue;
                }

                const std::uint16_t attributeFlags =
                    le16(recordBytes.data() + attrOffset + 12);
                constexpr std::uint16_t CompressedAttributeFlag = 0x0001;
                constexpr std::uint16_t EncryptedAttributeFlag = 0x4000;
                const bool unsupportedAttributeEncoding =
                    (attributeFlags & (CompressedAttributeFlag | EncryptedAttributeFlag)) != 0;
                // 同一记录若出现多个未命名 $DATA 属性，通常代表多 extent 布局；
                // 单记录安全恢复只接受一个从 VCN 0 开始的完整主数据属性。
                if (recordOut.hasPrimaryDataStream)
                {
                    recordOut.unsupportedDataStream = true;
                    attrOffset += attrLength;
                    continue;
                }
                recordOut.dataAttributeFlags = attributeFlags;
                recordOut.unsupportedDataStream =
                    recordOut.unsupportedDataStream || unsupportedAttributeEncoding;
                recordOut.hasPrimaryDataStream = true;

                if (!nonResident)
                {
                    recordOut.hasPrimaryDataStream = true;
                    recordOut.nonResidentData = false;
                    const std::uint32_t dataLength = le32(recordBytes.data() + attrOffset + 16);
                    const std::uint16_t dataOffset = le16(recordBytes.data() + attrOffset + 20);
                    const std::size_t dataStart = attrOffset + dataOffset;
                    constexpr std::uint32_t MaxResidentSize = 2 * 1024 * 1024;
                    if (dataLength <= MaxResidentSize && dataStart + dataLength <= attrEnd)
                    {
                        recordOut.residentReady = true;
                        recordOut.sizeBytes = static_cast<std::uint64_t>(dataLength);
                        recordOut.initializedSizeBytes =
                            static_cast<std::uint64_t>(dataLength);
                        if (captureResidentData && dataLength > 0)
                        {
                            recordOut.residentData = QByteArray(
                                reinterpret_cast<const char*>(recordBytes.data() + dataStart),
                                static_cast<int>(dataLength));
                        }
                    }
                }
                else if (attrLength >= 64)
                {
                    recordOut.hasPrimaryDataStream = true;
                    recordOut.nonResidentData = true;
                    const std::uint64_t lowestVcn =
                        le64(recordBytes.data() + attrOffset + 16);
                    const std::uint64_t highestVcn =
                        le64(recordBytes.data() + attrOffset + 24);
                    if (lowestVcn != 0 || highestVcn < lowestVcn)
                    {
                        recordOut.unsupportedDataStream = true;
                    }
                    const std::uint64_t nonResidentSize = le64(recordBytes.data() + attrOffset + 48);
                    const std::uint64_t initializedSize =
                        le64(recordBytes.data() + attrOffset + 56);
                    if (nonResidentSize > 0)
                    {
                        recordOut.sizeBytes = nonResidentSize;
                    }
                    recordOut.initializedSizeBytes =
                        std::min(initializedSize, nonResidentSize);
                    if (initializedSize > nonResidentSize)
                    {
                        recordOut.unsupportedDataStream = true;
                    }

                    const std::uint16_t runListOffset = le16(recordBytes.data() + attrOffset + 32);
                    const std::size_t runListStart = attrOffset + runListOffset;
                    if (runListOffset >= 0x40
                        && runListStart < attrEnd)
                    {
                        std::vector<NtfsDataRun> runValues;
                        if (ParseNtfsRunList(recordBytes.data() + runListStart, recordBytes.data() + attrEnd, runValues))
                        {
                            std::uint64_t parsedClusterCount = 0;
                            for (const NtfsDataRun& runValue : runValues)
                            {
                                if (parsedClusterCount >
                                    std::numeric_limits<std::uint64_t>::max() - runValue.clusterCount)
                                {
                                    recordOut.unsupportedDataStream = true;
                                    parsedClusterCount = 0;
                                    break;
                                }
                                parsedClusterCount += runValue.clusterCount;
                            }
                            if (parsedClusterCount == 0 ||
                                highestVcn < lowestVcn ||
                                parsedClusterCount != (highestVcn - lowestVcn + 1ULL))
                            {
                                recordOut.unsupportedDataStream = true;
                            }
                            recordOut.dataRuns = std::move(runValues);
                        }
                    }
                }
            }

            attrOffset += attrLength;
        }

        // parentHasDisplayNameSet：先记录每个父目录是否已有非 DOS 名称，避免 remove_if 期间读写同一 vector。
        QSet<qulonglong> parentHasDisplayNameSet;
        for (const NtfsNameLink& linkValue : recordOut.nameLinks)
        {
            if (linkValue.nameScore > 0)
            {
                parentHasDisplayNameSet.insert(static_cast<qulonglong>(linkValue.parentIndex));
            }
        }

        // 纯 DOS 短名只是同一目录链接的 8.3 别名；当该父目录已有 Win32/POSIX 名称时不单独展示。
        recordOut.nameLinks.erase(
            std::remove_if(
                recordOut.nameLinks.begin(),
                recordOut.nameLinks.end(),
                [&parentHasDisplayNameSet](const NtfsNameLink& linkValue) {
                    if (linkValue.nameScore != 0)
                    {
                        return false;
                    }
                    return parentHasDisplayNameSet.contains(static_cast<qulonglong>(linkValue.parentIndex));
                }),
            recordOut.nameLinks.end());

        recordOut.fileName = preferredName;
        return true;
    }

    // NtfsMftExtent 作用：
    // - 表示 $MFT 自身的一个连续数据段，同时保存该段的起始 VCN；
    // - 有了起始 VCN 才能把“逻辑记录号”换算回“卷内物理偏移”。
    struct NtfsMftExtent
    {
        std::uint64_t startVcn = 0;            // 该段在 $MFT 内的起始虚拟簇号。
        std::uint64_t startLcn = 0;            // 该段在卷内的起始逻辑簇号。
        std::uint64_t clusterCount = 0;        // 该段的簇数量。
    };

    // NtfsMftLocator 作用：
    // - 保存 $MFT 未命名主数据流的 runlist，把 MFT 记录号换算成卷内真实字节偏移。
    // 为什么必须有它：
    // - $MFT 在使用过一段时间的卷上几乎必然碎片化（MFT zone 用尽后按 extent 扩展）；
    // - 若继续按“MFT 起始偏移 + 记录号 * 记录长度”线性推进，越过第一个 extent 后
    //   读到的就是其它文件的数据：轻则解析失败导致扫描提前中断，
    //   重则把恰好带 FILE 签名的旧数据解析成记录号错乱的假条目。
    struct NtfsMftLocator
    {
        std::vector<NtfsMftExtent> extents;    // $MFT 数据段集合（按 VCN 升序）。
        std::uint64_t bytesPerCluster = 0;     // 每簇字节数。
        std::uint32_t bytesPerRecord = 0;      // 单条 MFT 记录字节数。
        std::uint64_t validRecordCount = 0;    // 按 $MFT 数据长度推算的有效记录数。

        // isUsable 作用：判断当前 locator 是否具备记录号换算能力。
        bool isUsable() const
        {
            return !extents.empty() && bytesPerCluster > 0 && bytesPerRecord > 0;
        }

        // mappedRecordCapacity 作用：返回 runlist 实际覆盖到的记录数上限。
        std::uint64_t mappedRecordCapacity() const
        {
            if (!isUsable())
            {
                return 0;
            }
            const NtfsMftExtent& lastExtent = extents.back();
            if (lastExtent.startVcn >
                std::numeric_limits<std::uint64_t>::max() - lastExtent.clusterCount)
            {
                return 0;
            }
            const std::uint64_t totalClusters = lastExtent.startVcn + lastExtent.clusterCount;
            if (totalClusters > std::numeric_limits<std::uint64_t>::max() / bytesPerCluster)
            {
                return 0;
            }
            return (totalClusters * bytesPerCluster) / bytesPerRecord;
        }

        // tryMapRecordRange 作用：
        // - 把 MFT 记录号换算成卷内字节偏移，并给出该记录所在数据段的剩余连续字节数；
        // - 调用方可据此一次性读取多条相邻记录，避免逐条 1KB 读带来的巨大开销。
        // 出参 volumeOffsetOut：记录首字节在卷内的绝对偏移。
        // 出参 contiguousBytesOut：从该偏移起仍位于同一数据段的连续字节数。
        // 返回值：记录号越界或落在稀疏段时返回 false。
        bool tryMapRecordRange(
            const std::uint64_t recordIndex,
            std::uint64_t& volumeOffsetOut,
            std::uint64_t& contiguousBytesOut) const
        {
            volumeOffsetOut = 0;
            contiguousBytesOut = 0;
            if (!isUsable()
                || recordIndex > std::numeric_limits<std::uint64_t>::max() / bytesPerRecord)
            {
                return false;
            }

            const std::uint64_t logicalOffset = recordIndex * bytesPerRecord;
            const std::uint64_t targetVcn = logicalOffset / bytesPerCluster;
            const std::uint64_t inClusterOffset = logicalOffset % bytesPerCluster;

            // extents 按 startVcn 升序排列，用二分定位目标 VCN 所属数据段。
            const auto extentIt = std::upper_bound(
                extents.begin(),
                extents.end(),
                targetVcn,
                [](const std::uint64_t vcnValue, const NtfsMftExtent& extentValue) {
                    return vcnValue < extentValue.startVcn;
                });
            if (extentIt == extents.begin())
            {
                return false;
            }
            const NtfsMftExtent& extentValue = *(extentIt - 1);
            if (targetVcn >= extentValue.startVcn + extentValue.clusterCount)
            {
                return false;
            }

            const std::uint64_t clusterOffsetInExtent = targetVcn - extentValue.startVcn;
            if (extentValue.startLcn >
                std::numeric_limits<std::uint64_t>::max() - clusterOffsetInExtent)
            {
                return false;
            }
            const std::uint64_t targetLcn = extentValue.startLcn + clusterOffsetInExtent;
            if (targetLcn > std::numeric_limits<std::uint64_t>::max() / bytesPerCluster)
            {
                return false;
            }
            const std::uint64_t clusterOffsetBytes = targetLcn * bytesPerCluster;
            if (clusterOffsetBytes >
                std::numeric_limits<std::uint64_t>::max() - inClusterOffset)
            {
                return false;
            }

            volumeOffsetOut = clusterOffsetBytes + inClusterOffset;
            contiguousBytesOut =
                (extentValue.clusterCount - clusterOffsetInExtent) * bytesPerCluster
                - inClusterOffset;
            return contiguousBytesOut >= bytesPerRecord;
        }
    };

    // loadNtfsMftLocator 作用：
    // - 读取 MFT 第 0 条记录（$MFT 自身），解析其未命名主数据流 runlist，构建记录号定位器。
    // 调用方法：
    // - 卷偏移兜底路径在开始遍历前调用一次。
    // 入参 volumeHandle：已打开的卷句柄。
    // 入参 mftStartOffset：引导扇区给出的 MFT 起始字节偏移（记录 0 一定位于此处）。
    // 出参 locatorOut：构建成功的定位器。
    // 出参 errorTextOut：失败原因文本。
    // 返回值：成功返回 true，失败返回 false（调用方可退化为线性推进）。
    bool loadNtfsMftLocator(
        const HANDLE volumeHandle,
        const std::uint64_t mftStartOffset,
        const std::uint16_t bytesPerSector,
        const std::uint64_t bytesPerCluster,
        const std::uint32_t bytesPerRecord,
        NtfsMftLocator& locatorOut,
        QString& errorTextOut)
    {
        locatorOut = NtfsMftLocator{};
        errorTextOut.clear();

        std::vector<std::byte> firstRecordBytes(bytesPerRecord);
        if (!readBytesAtSectorAlignedOffset(
            volumeHandle,
            mftStartOffset,
            bytesPerRecord,
            bytesPerSector,
            QStringLiteral("读取$MFT记录0"),
            0,
            firstRecordBytes.data(),
            errorTextOut))
        {
            return false;
        }

        NtfsRawRecord mftRecordValue{};
        if (!parseNtfsRecord(firstRecordBytes, 0, bytesPerSector, false, mftRecordValue))
        {
            errorTextOut = QStringLiteral("解析 $MFT 记录 0 失败，无法建立 MFT runlist 映射。");
            return false;
        }
        if (!mftRecordValue.nonResidentData || mftRecordValue.dataRuns.empty())
        {
            errorTextOut = QStringLiteral("$MFT 记录 0 未给出可用的非驻留 runlist。");
            return false;
        }

        std::uint64_t currentVcn = 0;
        locatorOut.extents.reserve(mftRecordValue.dataRuns.size());
        for (const NtfsDataRun& runValue : mftRecordValue.dataRuns)
        {
            // $MFT 自身不应存在稀疏段；一旦出现说明 runlist 解析已经不可信。
            if (runValue.isSparse || runValue.clusterCount == 0)
            {
                errorTextOut = QStringLiteral("$MFT runlist 含稀疏或空数据段，映射不可信。");
                return false;
            }
            if (currentVcn > std::numeric_limits<std::uint64_t>::max() - runValue.clusterCount)
            {
                errorTextOut = QStringLiteral("$MFT runlist 虚拟簇号累加溢出。");
                return false;
            }

            NtfsMftExtent extentValue{};
            extentValue.startVcn = currentVcn;
            extentValue.startLcn = runValue.startLcn;
            extentValue.clusterCount = runValue.clusterCount;
            locatorOut.extents.push_back(extentValue);
            currentVcn += runValue.clusterCount;
        }

        locatorOut.bytesPerCluster = bytesPerCluster;
        locatorOut.bytesPerRecord = bytesPerRecord;
        locatorOut.validRecordCount =
            (mftRecordValue.sizeBytes >= bytesPerRecord)
            ? (mftRecordValue.sizeBytes / bytesPerRecord)
            : 0;

        // hasAttributeList 说明 $MFT 的 runlist 可能被拆到其它记录里，
        // 这里只覆盖到本记录列出的部分，需要让调用方知道映射范围可能不完整。
        if (mftRecordValue.hasAttributeList)
        {
            errorTextOut = QStringLiteral("$MFT 使用 $ATTRIBUTE_LIST，runlist 可能不完整。");
        }
        return locatorOut.isUsable();
    }

    // readNtfsRecordViaLocator 作用：
    // - 借助 MFT runlist 映射读取单条记录，并在内部按大块缓存相邻记录；
    // - 块缓存永远不跨数据段，因此块内偏移与记录号始终一一对应。
    // 入参 chunkBytes/chunkFirstRecord/chunkRecordCount：调用方持有的块缓存状态。
    // 出参 recordBytesOut：目标记录的原始字节。
    // 返回值：成功返回 true；记录号无法映射或读取失败返回 false。
    bool readNtfsRecordViaLocator(
        const HANDLE volumeHandle,
        const NtfsMftLocator& locatorValue,
        const std::uint16_t bytesPerSector,
        const std::uint64_t recordIndex,
        const std::uint64_t parseCount,
        std::vector<std::byte>& chunkBytes,
        std::uint64_t& chunkFirstRecord,
        std::uint64_t& chunkRecordCount,
        std::vector<std::byte>& recordBytesOut,
        QString& errorTextOut)
    {
        const std::uint32_t bytesPerRecord = locatorValue.bytesPerRecord;
        if (bytesPerRecord == 0)
        {
            errorTextOut = QStringLiteral("MFT 记录长度为 0。");
            return false;
        }

        const bool insideChunk =
            (chunkRecordCount > 0)
            && (recordIndex >= chunkFirstRecord)
            && (recordIndex - chunkFirstRecord < chunkRecordCount);
        if (!insideChunk)
        {
            std::uint64_t volumeOffset = 0;
            std::uint64_t contiguousBytes = 0;
            if (!locatorValue.tryMapRecordRange(recordIndex, volumeOffset, contiguousBytes))
            {
                chunkRecordCount = 0;
                errorTextOut = QStringLiteral("记录号 %1 超出 $MFT runlist 覆盖范围。")
                    .arg(static_cast<qulonglong>(recordIndex));
                return false;
            }

            // 单次读取上限 1MB：兼顾吞吐与内存占用，且不跨越当前数据段。
            constexpr std::uint64_t MftChunkTargetBytes = 1ULL * 1024ULL * 1024ULL;
            const std::uint64_t remainingRecords = (parseCount > recordIndex)
                ? (parseCount - recordIndex)
                : 1ULL;
            std::uint64_t chunkRecords = std::min<std::uint64_t>(
                { MftChunkTargetBytes / bytesPerRecord,
                  contiguousBytes / bytesPerRecord,
                  remainingRecords });
            if (chunkRecords == 0)
            {
                chunkRecords = 1;
            }

            const std::uint64_t chunkSizeBytes = chunkRecords * bytesPerRecord;
            chunkBytes.resize(static_cast<std::size_t>(chunkSizeBytes));
            if (!readBytesAtSectorAlignedOffset(
                volumeHandle,
                volumeOffset,
                static_cast<std::uint32_t>(chunkSizeBytes),
                bytesPerSector,
                QStringLiteral("按$MFT runlist读取记录块"),
                0,
                chunkBytes.data(),
                errorTextOut))
            {
                chunkRecordCount = 0;
                return false;
            }
            chunkFirstRecord = recordIndex;
            chunkRecordCount = chunkRecords;
        }

        const std::size_t inChunkOffset =
            static_cast<std::size_t>((recordIndex - chunkFirstRecord) * bytesPerRecord);
        recordBytesOut.resize(bytesPerRecord);
        std::memcpy(recordBytesOut.data(), chunkBytes.data() + inChunkOffset, bytesPerRecord);
        return true;
    }

    // buildTypeText 作用：将文件类型映射为界面展示文本。
    QString buildTypeText(const QString& fileName, const bool isDirectory)
    {
        if (isDirectory)
        {
            return QStringLiteral("目录");
        }
        const QString suffixText = QFileInfo(fileName).suffix().trimmed();
        return suffixText.isEmpty() ? QStringLiteral("文件") : (suffixText.toUpper() + QStringLiteral(" 文件"));
    }

    // splitRelativeSegments 作用：提取卷内路径分段（不含盘符）。
    QStringList splitRelativeSegments(const QString& absolutePath)
    {
        QString cleanPathText = QDir::cleanPath(QDir::fromNativeSeparators(absolutePath));
        if (cleanPathText.size() >= 2 && cleanPathText[1] == QChar(':'))
        {
            cleanPathText = cleanPathText.mid(2);
        }
        if (cleanPathText.startsWith('/'))
        {
            cleanPathText.remove(0, 1);
        }
        return cleanPathText.isEmpty() ? QStringList() : cleanPathText.split('/', Qt::SkipEmptyParts);
    }

    // tryLoadNtfsRecordsByFsctl 作用：
    // - 通过 FSCTL_GET_NTFS_FILE_RECORD 从卷句柄逐条提取 MFT 记录；
    // - 不依赖 $MFT 直接路径，可绕过 \\.\X:\$MFT 的访问限制；
    // - 能够正确处理 MFT 碎片化，不会像“卷偏移连续读取”那样漏记录。
    // 参数 volumeHandle：
    // - 已打开的卷句柄（\\.\X:）。
    // 参数 bytesPerRecordHint：
    // - 从引导扇区推断的 MFT 记录大小（兜底）。
    // 参数 maxRecordCount：
    // - 本轮最大扫描记录数上限。
    // 参数 recordsOut：
    // - 输出解析后的 NTFS 记录集合。
    // 参数 errorTextOut：
    // - 返回失败原因文本。
    // 返回值：
    // - 成功返回 true，失败返回 false。
    bool tryLoadNtfsRecordsByFsctl(
        const HANDLE volumeHandle,
        const std::uint16_t bytesPerSectorHint,
        const std::uint32_t bytesPerRecordHint,
        const std::uint64_t maxRecordCount,
        const bool captureResidentData,
        const bool keepNamelessRecords,
        const std::function<void(int, const QString&)>& progressCallback,
        std::vector<NtfsRawRecord>& recordsOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        NTFS_VOLUME_DATA_BUFFER volumeData{};
        DWORD returnedBytes = 0;
        const BOOL volumeDataOk = ::DeviceIoControl(
            volumeHandle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            nullptr,
            0,
            &volumeData,
            static_cast<DWORD>(sizeof(volumeData)),
            &returnedBytes,
            nullptr);
        if (volumeDataOk == FALSE)
        {
            errorTextOut = QStringLiteral("FSCTL_GET_NTFS_VOLUME_DATA失败, code=%1").arg(::GetLastError());
            return false;
        }

        // bytesPerRecord：FSCTL 回传的记录大小优先，异常时回退到引导扇区估算值。
        std::uint32_t bytesPerRecord = volumeData.BytesPerFileRecordSegment;
        if (bytesPerRecord < 512 || bytesPerRecord > 16384)
        {
            bytesPerRecord = bytesPerRecordHint;
        }
        if (bytesPerRecord < 512 || bytesPerRecord > 16384)
        {
            errorTextOut = QStringLiteral("FSCTL回退失败：MFT记录大小异常, bytesPerRecord=%1").arg(bytesPerRecord);
            return false;
        }

        // mftRecordCountByValidData：根据 MFT 有效数据长度估算可遍历记录数。
        const std::uint64_t mftRecordCountByValidData =
            static_cast<std::uint64_t>(volumeData.MftValidDataLength.QuadPart)
            / static_cast<std::uint64_t>(bytesPerRecord);
        std::uint64_t parseCount = std::min(mftRecordCountByValidData, maxRecordCount);
        if (parseCount == 0)
        {
            // 部分系统可能返回 0，这里提供保守兜底，避免直接失败。
            parseCount = std::min<std::uint64_t>(maxRecordCount, 65536ULL);
        }

        // outputBufferBytes：FSCTL 输出缓冲区大小（结构头 + 一条记录）。
        const std::size_t outputHeaderBytes = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer);
        const std::size_t outputBufferBytes = outputHeaderBytes + static_cast<std::size_t>(bytesPerRecord) + 16ULL;
        std::vector<std::uint8_t> outputBuffer(outputBufferBytes);

        recordsOut.clear();
        recordsOut.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(parseCount, 200000ULL)));

        // FSCTL_GET_NTFS_FILE_RECORD 的行为是：
        // - 返回“编号 <= 输入值”的最近一条在用记录；
        // - 因此必须从高到低枚举，不能从 0 正向扫描。
        std::uint64_t requestRecordIndex = (parseCount > 0) ? (parseCount - 1) : 0;
        std::uint64_t lastReturnedRecordIndex = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t visitedCount = 0;                 // visitedCount：已访问的记录数（用于受 maxRecordCount 限制）。
        std::uint32_t consecutiveQueryFailCount = 0;    // 连续查询失败次数。
        std::uint32_t consecutiveInvalidRecordCount = 0; // 连续无效记录次数。
        int lastReportedPercent = -1;                   // lastReportedPercent：FSCTL 分支的最近一次上报百分比。
        while (visitedCount < parseCount)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            NTFS_FILE_RECORD_INPUT_BUFFER inputBuffer{};
            inputBuffer.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(requestRecordIndex);

            returnedBytes = 0;
            const BOOL queryOk = ::DeviceIoControl(
                volumeHandle,
                FSCTL_GET_NTFS_FILE_RECORD,
                &inputBuffer,
                static_cast<DWORD>(sizeof(inputBuffer)),
                outputBuffer.data(),
                static_cast<DWORD>(outputBuffer.size()),
                &returnedBytes,
                nullptr);
            const DWORD queryErrorCode = queryOk == FALSE ? ::GetLastError() : ERROR_SUCCESS;
            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (queryOk == FALSE)
            {
                ++consecutiveQueryFailCount;
                if (!recordsOut.empty()
                    && consecutiveQueryFailCount > 2048)
                {
                    break;
                }

                if (queryErrorCode == ERROR_HANDLE_EOF || queryErrorCode == ERROR_FILE_NOT_FOUND)
                {
                    break;
                }

                if (requestRecordIndex == 0)
                {
                    break;
                }
                requestRecordIndex -= 1;
                continue;
            }
            consecutiveQueryFailCount = 0;
            visitedCount += 1;
            if (progressCallback)
            {
                const int percentValue = 10
                    + static_cast<int>((visitedCount * 70ULL) / std::max<std::uint64_t>(parseCount, 1ULL));
                if (percentValue != lastReportedPercent
                    && ((visitedCount % 4096ULL) == 0 || visitedCount == parseCount))
                {
                    lastReportedPercent = percentValue;
                    progressCallback(percentValue, QStringLiteral("FSCTL扫描 MFT 记录"));
                }
            }

            if (returnedBytes <= outputHeaderBytes)
            {
                if (requestRecordIndex == 0)
                {
                    break;
                }
                requestRecordIndex -= 1;
                continue;
            }
            NTFS_FILE_RECORD_OUTPUT_BUFFER* outputRecord =
                reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(outputBuffer.data());
            const std::uint32_t fileRecordLength = outputRecord->FileRecordLength;
            const std::uint64_t actualRecordIndex =
                static_cast<std::uint64_t>(outputRecord->FileReferenceNumber.QuadPart & 0x0000FFFFFFFFFFFFULL);

            // 若返回记录号比请求值还大，说明当前返回不符合“向下枚举”预期，直接降级请求号继续。
            if (actualRecordIndex > requestRecordIndex)
            {
                if (requestRecordIndex == 0)
                {
                    break;
                }
                requestRecordIndex -= 1;
                continue;
            }

            // 防止连续返回同一条记录导致死循环。
            if (actualRecordIndex == lastReturnedRecordIndex)
            {
                if (actualRecordIndex == 0)
                {
                    break;
                }
                requestRecordIndex = actualRecordIndex - 1;
                continue;
            }
            lastReturnedRecordIndex = actualRecordIndex;

            if (fileRecordLength < 64
                || fileRecordLength > bytesPerRecord
                || outputHeaderBytes + fileRecordLength > returnedBytes
                || outputHeaderBytes + fileRecordLength > outputBuffer.size())
            {
                if (actualRecordIndex == 0)
                {
                    break;
                }
                requestRecordIndex = actualRecordIndex - 1;
                continue;
            }

            std::vector<std::byte> recordBytes(fileRecordLength);
            std::memcpy(
                recordBytes.data(),
                outputBuffer.data() + outputHeaderBytes,
                fileRecordLength);

            NtfsRawRecord recordValue{};
            if (!parseNtfsRecord(recordBytes, actualRecordIndex, bytesPerSectorHint, captureResidentData, recordValue))
            {
                ++consecutiveInvalidRecordCount;
                if (!recordsOut.empty()
                    && consecutiveInvalidRecordCount > 8192)
                {
                    break;
                }

                if (actualRecordIndex == 0)
                {
                    break;
                }
                requestRecordIndex = actualRecordIndex - 1;
                continue;
            }

            consecutiveInvalidRecordCount = 0;
            if (!keepNamelessRecords
                && recordValue.fileName.isEmpty()
                && recordValue.recordIndex != 5)
            {
                if (actualRecordIndex == 0)
                {
                    break;
                }
                requestRecordIndex = actualRecordIndex - 1;
                continue;
            }
            recordsOut.push_back(std::move(recordValue));

            if (actualRecordIndex == 0)
            {
                break;
            }
            requestRecordIndex = actualRecordIndex - 1;
        }

        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (recordsOut.empty())
        {
            errorTextOut = QStringLiteral("FSCTL回退失败：未解析到任何MFT记录。");
            return false;
        }
        return true;
    }

    // loadNtfsRecords 作用：扫描 $MFT 并解析记录。
    // 说明：
    // 1) 优先按 \\.\X:\$MFT 文件方式读取；
    // 2) 若 $MFT 打开失败（常见 ERROR_ACCESS_DENIED=5），自动回退到“卷偏移直读”；
    // 3) 回退模式根据 NTFS 引导扇区中的 MFT 起始簇定位读取，避免被 $MFT 路径权限拦截。
    bool loadNtfsRecords(
        const QString& volumeRoot,
        std::vector<NtfsRawRecord>& recordsOut,
        QString& errorTextOut,
        const std::uint64_t maxRecordCountHint,
        const bool allowFsctlFallback,
        const bool useCache,
        const bool copyRecordsOut,
        const bool captureResidentData,
        const bool keepNamelessRecords,
        const NtfsRecordKeepPolicy keepPolicy,
        const std::function<void(int, const QString&)>& progressCallback,
        std::shared_ptr<const NtfsCacheEntry>* cacheEntryOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        const std::wstring cacheKey = toWide(volumeRoot.toUpper());
        const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
        constexpr qint64 NtfsCacheTtlMsec = 60000; // 缓存 60 秒，避免同卷短时间重复全盘扫描。
        const bool keepDeletedAndDirectoriesOnly =
            (keepPolicy == NtfsRecordKeepPolicy::DeletedAndDirectories);
        // 硬上限按保留策略区分：
        // - All 模式每条记录都进内存，必须压在 150 万条以内；
        // - DeletedAndDirectories 模式只留删除项和目录，可以覆盖整个 $MFT，
        //   这里给的是防病态的上界，实际扫描量由 $MFT 有效记录数决定。
        const std::uint64_t NtfsHardMaxRecordCount =
            keepDeletedAndDirectoriesOnly ? 64000000ULL : 1500000ULL;
        const std::uint64_t effectiveMaxRecordCount = (maxRecordCountHint == 0)
            ? NtfsHardMaxRecordCount
            : std::min<std::uint64_t>(maxRecordCountHint, NtfsHardMaxRecordCount);
        // 记录集不完整时禁止入缓存，否则会污染依赖全量记录的目录浏览。
        const bool cacheAllowed = useCache && !keepDeletedAndDirectoriesOnly;
        if (cacheAllowed)
        {
            std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
            const auto cacheIt = g_ntfsCache.find(cacheKey);
            if (cacheIt != g_ntfsCache.end()
                && (nowMsec - cacheIt->second->loadedMsec) <= NtfsCacheTtlMsec
                && cacheIt->second->recordLimit >= effectiveMaxRecordCount)
            {
                if (cacheIt->second->fsctlFallbackAllowed == allowFsctlFallback)
                {
                    if (copyRecordsOut)
                    {
                        recordsOut = cacheIt->second->records;
                    }
                    else
                    {
                        recordsOut.clear();
                    }
                    if (cacheEntryOut != nullptr)
                    {
                        *cacheEntryOut = cacheIt->second;
                    }
                    if (progressCallback)
                    {
                        progressCallback(75, QStringLiteral("命中 NTFS 缓存"));
                    }
                    if (directoryCanceled(canceled))
                    {
                        return false;
                    }
                    return true;
                }
            }
        }

        if (directoryCanceled(canceled))
        {
            return false;
        }
        const QString volumeDevicePath = buildVolumeDevicePath(volumeRoot);
        QString openVolumeErrorText;
        DirectoryReadHandle volumeHandle(openReadHandle(volumeDevicePath, openVolumeErrorText));
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openVolumeErrorText;
            return false;
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (progressCallback)
        {
            progressCallback(4, QStringLiteral("已打开卷句柄"));
        }

        // 先读取 NTFS 引导扇区，后续常规模式和回退模式都依赖这组参数。
        std::array<std::byte, 512> bootBytes{};
        if (!readBytesAtOffset(volumeHandle, 0, 512, bootBytes.data(), errorTextOut))
        {
            volumeHandle.close();
            return false;
        }
        if (progressCallback)
        {
            progressCallback(8, QStringLiteral("已读取 NTFS 引导区"));
        }

        const QByteArray oemText(reinterpret_cast<const char*>(bootBytes.data() + 3), 8);
        if (!oemText.startsWith("NTFS"))
        {
            volumeHandle.close();
            errorTextOut = QStringLiteral("不是 NTFS 卷。");
            return false;
        }

        const std::uint16_t bytesPerSector = le16(bootBytes.data() + 11);
        const std::uint8_t sectorsPerCluster = static_cast<std::uint8_t>(bootBytes[13]);
        if (bytesPerSector == 0 || sectorsPerCluster == 0)
        {
            volumeHandle.close();
            errorTextOut = QStringLiteral("NTFS 引导参数异常：扇区或簇大小为 0。");
            return false;
        }

        const std::uint64_t bytesPerCluster =
            static_cast<std::uint64_t>(bytesPerSector) *
            static_cast<std::uint64_t>(sectorsPerCluster);
        const std::uint64_t mftStartCluster = le64(bootBytes.data() + 0x30);
        if (mftStartCluster >
            std::numeric_limits<std::uint64_t>::max() /
                bytesPerCluster)
        {
            volumeHandle.close();
            errorTextOut = QStringLiteral("卷偏移记录解析失败。");
            return false;
        }
        const std::uint64_t mftStartOffset = mftStartCluster * bytesPerCluster;

        const std::int8_t clustersPerRecord = static_cast<std::int8_t>(bootBytes[64]);
        std::uint32_t bytesPerRecord = 1024;
        if (clustersPerRecord < 0)
        {
            const int powerValue = -clustersPerRecord;
            bytesPerRecord = (1u << powerValue);
        }
        else
        {
            bytesPerRecord =
                static_cast<std::uint32_t>(bytesPerSector) *
                static_cast<std::uint32_t>(sectorsPerCluster) *
                static_cast<std::uint32_t>(clustersPerRecord);
        }
        if (bytesPerRecord < 512 || bytesPerRecord > 16384)
        {
            volumeHandle.close();
            errorTextOut = QStringLiteral("MFT记录大小异常: %1").arg(bytesPerRecord);
            return false;
        }

        const std::uint64_t MaxRecordCount = effectiveMaxRecordCount;
        HANDLE sourceHandle = INVALID_HANDLE_VALUE;        // sourceHandle：本轮实际读取句柄（$MFT 或卷句柄）。
        std::uint64_t sourceBaseOffset = 0;               // sourceBaseOffset：读取起点偏移（$MFT=0，卷回退=MFT偏移）。
        std::uint64_t parseCount = 0;                     // parseCount：计划解析记录数。
        bool usingVolumeFallback = false;                 // usingVolumeFallback：是否进入卷偏移直读回退。
        QString fallbackReasonText;                       // fallbackReasonText：回退原因日志文本。
        NtfsMftLocator mftLocator;                        // mftLocator：$MFT 自身 runlist，卷偏移回退时用于精确定位记录。
        bool usingMftLocator = false;                     // usingMftLocator：本轮是否按 runlist 映射记录号而非线性推进。

        if (directoryCanceled(canceled))
        {
            return false;
        }
        // 第一优先级：直接读取 \\.\X:\$MFT。
        const QString mftPath = QStringLiteral("\\\\.\\%1\\$MFT").arg(volumeRoot.left(2).toUpper());
        QString openMftErrorText;
        DirectoryReadHandle mftHandle(openReadHandle(mftPath, openMftErrorText));
        if (mftHandle != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER mftFileSize{};
            if (::GetFileSizeEx(mftHandle, &mftFileSize) == FALSE)
            {
                fallbackReasonText = QStringLiteral("读取$MFT大小失败, code=%1，改用卷偏移回退。")
                    .arg(::GetLastError());
                usingVolumeFallback = true;
            }
            else
            {
                const std::uint64_t recordCount =
                    static_cast<std::uint64_t>(mftFileSize.QuadPart) / bytesPerRecord;
                parseCount = std::min(recordCount, MaxRecordCount);
                sourceHandle = mftHandle;
                sourceBaseOffset = 0;
            }
        }
        else
        {
            // 典型场景：CreateFile \\.\C:\$MFT 返回 code=5。
            fallbackReasonText = openMftErrorText;
            usingVolumeFallback = true;
        }

        // 回退路径：
        // 1) 先尝试 FSCTL_GET_NTFS_FILE_RECORD（能处理 MFT 碎片化）；
        // 2) 若 FSCTL 也失败，再回退到“卷偏移连续读取”。
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (usingVolumeFallback)
        {
            if (mftHandle != INVALID_HANDLE_VALUE)
            {
                mftHandle.close();
            }

            // 第一层回退：FSCTL 按记录号读取 MFT。
            std::vector<NtfsRawRecord> fsctlRecords;
            QString fsctlErrorText;
            if (allowFsctlFallback
                && tryLoadNtfsRecordsByFsctl(
                    volumeHandle,
                    bytesPerSector,
                    bytesPerRecord,
                    MaxRecordCount,
                    captureResidentData,
                    keepNamelessRecords,
                    progressCallback,
                    fsctlRecords,
                    fsctlErrorText,
                    canceled))
            {
                recordsOut = std::move(fsctlRecords);
                volumeHandle.close();

                {
                    kLogEvent fallbackEvent;
                    info << fallbackEvent
                        << "[FileDock] $MFT 打开失败，启用FSCTL回退解析, volume="
                        << volumeRoot.toStdString()
                        << ", reason="
                        << fallbackReasonText.toStdString()
                        << ", rows="
                        << recordsOut.size()
                        << eol;
                }

                if (cacheAllowed)
                {
                    std::shared_ptr<NtfsCacheEntry> cacheEntry = std::make_shared<NtfsCacheEntry>();
                    if (copyRecordsOut)
                    {
                        cacheEntry->records = recordsOut;
                    }
                    else
                    {
                        cacheEntry->records = std::move(recordsOut);
                    }
                    cacheEntry->loadedMsec = nowMsec;
                    cacheEntry->recordLimit = MaxRecordCount;
                    cacheEntry->fsctlFallbackAllowed = allowFsctlFallback;
                    if (!buildNtfsCacheIndex(*cacheEntry, canceled))
                    {
                        return false;
                    }

                    if (directoryCanceled(canceled))
                    {
                        return false;
                    }
                    std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
                    if (directoryCanceled(canceled))
                    {
                        return false;
                    }
                    g_ntfsCache[cacheKey] = cacheEntry;
                    if (cacheEntryOut != nullptr)
                    {
                        *cacheEntryOut = cacheEntry;
                    }
                }
                if (!copyRecordsOut)
                {
                    recordsOut.clear();
                }
                return true;
            }

            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (!fsctlErrorText.isEmpty())
            {
                fallbackReasonText =
                    fallbackReasonText.isEmpty()
                    ? fsctlErrorText
                    : (fallbackReasonText + QStringLiteral("; FSCTL回退失败: ") + fsctlErrorText);
            }

            // 第二层回退：卷偏移直读。
            // 先读取 MFT 第 0 条记录（$MFT 自身），解析出它的 runlist：
            // 1) runlist 给出真实 MFT 数据长度，避免按整卷空间估算把 parseCount 顶到硬上限；
            // 2) 更关键的是能把记录号精确映射到卷内物理偏移，
            //    $MFT 碎片化时不会像“线性推进”那样越过第一个 extent 后读到其它文件的数据。
            std::uint64_t estimatedMftRecordCount = 0;
            {
                QString locatorErrorText;
                if (loadNtfsMftLocator(
                    volumeHandle,
                    mftStartOffset,
                    bytesPerSector,
                    bytesPerCluster,
                    bytesPerRecord,
                    mftLocator,
                    locatorErrorText))
                {
                    usingMftLocator = true;
                    estimatedMftRecordCount = mftLocator.validRecordCount;
                    const std::uint64_t mappedRecordCapacity = mftLocator.mappedRecordCapacity();
                    if (mappedRecordCapacity > 0
                        && (estimatedMftRecordCount == 0
                            || estimatedMftRecordCount > mappedRecordCapacity))
                    {
                        // runlist 覆盖范围才是真正能读到的上限，超出部分只会读到越界数据。
                        estimatedMftRecordCount = mappedRecordCapacity;
                    }
                    if (!locatorErrorText.isEmpty())
                    {
                        fallbackReasonText += QStringLiteral("; MFT映射告警: ") + locatorErrorText;
                    }
                }
                else
                {
                    // 映射构建失败时退化为旧的线性推进，只能覆盖 $MFT 第一个数据段。
                    fallbackReasonText +=
                        QStringLiteral("; 构建$MFT映射失败(退化为线性推进): ") + locatorErrorText;
                }
            }

            if (directoryCanceled(canceled))
            {
                return false;
            }
            // 先取卷长度，计算理论可读记录数。
            std::uint64_t volumeBytes = 0;
            GET_LENGTH_INFORMATION lengthInfo{};
            DWORD returnedBytes = 0;
            if (::DeviceIoControl(
                volumeHandle,
                IOCTL_DISK_GET_LENGTH_INFO,
                nullptr,
                0,
                &lengthInfo,
                static_cast<DWORD>(sizeof(lengthInfo)),
                &returnedBytes,
                nullptr) != FALSE)
            {
                volumeBytes = static_cast<std::uint64_t>(lengthInfo.Length.QuadPart);
            }
            else
            {
                LARGE_INTEGER fallbackLength{};
                if (::GetFileSizeEx(volumeHandle, &fallbackLength) != FALSE)
                {
                    volumeBytes = static_cast<std::uint64_t>(fallbackLength.QuadPart);
                }
            }

            if (volumeBytes == 0 || mftStartOffset >= volumeBytes)
            {
                volumeHandle.close();
                errorTextOut = QStringLiteral(
                    "卷级回退失败：无法计算有效 MFT 区间。mftOffset=%1, volumeBytes=%2, reason=%3")
                    .arg(static_cast<qulonglong>(mftStartOffset))
                    .arg(static_cast<qulonglong>(volumeBytes))
                    .arg(fallbackReasonText);
                return false;
            }

            const std::uint64_t readableBytes = volumeBytes - mftStartOffset;
            const std::uint64_t fallbackRecordCountByVolume = readableBytes / bytesPerRecord;
            if (estimatedMftRecordCount > 0)
            {
                parseCount = std::min(estimatedMftRecordCount, MaxRecordCount);
            }
            else
            {
                parseCount = std::min(fallbackRecordCountByVolume, MaxRecordCount);
            }
            if (parseCount == 0)
            {
                volumeHandle.close();
                errorTextOut = QStringLiteral(
                    "卷级回退失败：MFT 可读记录数为 0。mftOffset=%1, volumeBytes=%2")
                    .arg(static_cast<qulonglong>(mftStartOffset))
                    .arg(static_cast<qulonglong>(volumeBytes));
                return false;
            }

            sourceHandle = volumeHandle;
            sourceBaseOffset = mftStartOffset;

            // 记录“进入卷偏移兜底”日志，便于定位 FSCTL 失败原因。
            kLogEvent fallbackEvent;
            info << fallbackEvent
                << "[FileDock] $MFT/FSCTL 均失败，启用卷偏移兜底解析, volume="
                << volumeRoot.toStdString()
                << ", reason="
                << fallbackReasonText.toStdString()
                << ", mftOffset="
                << static_cast<qulonglong>(mftStartOffset)
                << ", parseCount="
                << static_cast<qulonglong>(parseCount)
                << ", estimatedByMft="
                << static_cast<qulonglong>(estimatedMftRecordCount)
                << ", mftLocator="
                << (usingMftLocator ? "runlist" : "linear")
                << ", mftExtents="
                << mftLocator.extents.size()
                << eol;
        }

        // 通用解析流程：从 sourceHandle 按记录序号读取并解析。
        std::vector<std::byte> recordBytes(bytesPerRecord);
        std::vector<std::byte> locatorChunkBytes;      // locatorChunkBytes：runlist 模式下的记录块缓存。
        std::uint64_t locatorChunkFirstRecord = 0;     // locatorChunkFirstRecord：块缓存首条记录号。
        std::uint64_t locatorChunkRecordCount = 0;     // locatorChunkRecordCount：块缓存覆盖的记录条数。
        recordsOut.clear();
        recordsOut.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(parseCount, 200000ULL)));
        std::uint32_t consecutiveReadFailCount = 0;    // 连续读取失败计数：防止卷末尾反复失败造成长时间阻塞。
        std::uint32_t consecutiveEmptyCount = 0;       // 连续空记录计数：线性推进模式用于提前停止无效扫描。
        std::uint32_t consecutiveInvalidCount = 0;     // 连续无效记录计数：线性推进模式下用于识别“已离开有效 MFT 区间”。
        std::uint64_t validRecordCount = 0;            // 已解析出的有效记录数，用于无效区间提前终止判定。
        std::uint64_t totalReadFailCount = 0;          // 累计读取失败次数（诊断用）。
        std::uint64_t totalInvalidCount = 0;           // 累计记录解析失败次数（诊断用）。
        std::uint64_t stoppedAtIndex = parseCount;     // 实际停止位置（诊断用，等于 parseCount 表示走完全程）。
        QString lastReadFailText;                      // 最后一次读取失败原因（诊断用）。
        int lastReportedPercent = -1;                  // lastReportedPercent：顺序扫描分支的最近一次上报百分比。
        for (std::uint64_t indexValue = 0; indexValue < parseCount; ++indexValue)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            if (progressCallback
                && (((indexValue % 4096ULL) == 0) || (indexValue + 1 == parseCount)))
            {
                const int percentValue = 10
                    + static_cast<int>(((indexValue + 1ULL) * 70ULL) / std::max<std::uint64_t>(parseCount, 1ULL));
                if (percentValue != lastReportedPercent)
                {
                    lastReportedPercent = percentValue;
                    progressCallback(
                        percentValue,
                        usingMftLocator
                        ? QStringLiteral("按 $MFT runlist 扫描记录")
                        : (usingVolumeFallback
                            ? QStringLiteral("按卷偏移扫描 MFT 记录")
                            : QStringLiteral("按 $MFT 逻辑文件扫描记录")));
                }
            }

            QString readErrorText;
            bool readOk = false;
            if (usingMftLocator)
            {
                // runlist 映射模式：记录号 → 卷内物理偏移，$MFT 碎片化也不会串位。
                readOk = readNtfsRecordViaLocator(
                    sourceHandle,
                    mftLocator,
                    bytesPerSector,
                    indexValue,
                    parseCount,
                    locatorChunkBytes,
                    locatorChunkFirstRecord,
                    locatorChunkRecordCount,
                    recordBytes,
                    readErrorText);
            }
            else
            {
                // 线性推进模式：仅在 $MFT 逻辑文件可读、或 runlist 映射构建失败时使用。
                // 卷句柄要求按逻辑扇区对齐访问，4K 扇区上 1KB 记录并不天然对齐，
                // 因此这里统一走对齐读，避免 ReadFile 直接返回 ERROR_INVALID_PARAMETER。
                const std::uint64_t offsetValue = sourceBaseOffset + indexValue * bytesPerRecord;
                recordBytes.resize(bytesPerRecord);
                readOk = readBytesAtSectorAlignedOffset(
                    sourceHandle,
                    offsetValue,
                    bytesPerRecord,
                    bytesPerSector,
                    QStringLiteral("线性扫描 MFT 记录"),
                    0,
                    recordBytes.data(),
                    readErrorText);
            }
            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (!readOk)
            {
                ++consecutiveReadFailCount;
                ++totalReadFailCount;
                lastReadFailText = readErrorText;
                if (consecutiveReadFailCount >= 8)
                {
                    stoppedAtIndex = indexValue;
                    break;
                }
                continue;
            }
            consecutiveReadFailCount = 0;

            NtfsRawRecord recordValue{};
            if (!parseNtfsRecord(recordBytes, indexValue, bytesPerSector, captureResidentData, recordValue))
            {
                ++consecutiveInvalidCount;
                ++totalInvalidCount;

                // 若出现大段全 0 区域，线性推进模式说明已到有效 MFT 尾部附近，可提前结束。
                bool allZeroBytes = true;
                for (const std::byte byteValue : recordBytes)
                {
                    if (byteValue != std::byte{ 0 })
                    {
                        allZeroBytes = false;
                        break;
                    }
                }
                if (allZeroBytes)
                {
                    ++consecutiveEmptyCount;
                    if (usingVolumeFallback
                        && !usingMftLocator
                        && indexValue > 4096
                        && consecutiveEmptyCount > 2048)
                    {
                        stoppedAtIndex = indexValue;
                        break;
                    }
                }
                else
                {
                    consecutiveEmptyCount = 0;
                }

                // 这两处提前终止只对线性推进模式成立：那里连续无效确实代表已跳出 MFT 数据区。
                // runlist 映射模式下每条记录都落在 $MFT 真实数据段内，
                // 中间成片的未用记录属于正常现象，提前终止只会漏掉后半段删除项。
                if (usingVolumeFallback
                    && !usingMftLocator
                    && validRecordCount > 1024
                    && indexValue > 8192
                    && consecutiveInvalidCount > 8192)
                {
                    stoppedAtIndex = indexValue;
                    break;
                }
                continue;
            }

            // runlist 映射模式下核对记录头自带的 MFT record number（offset 0x2C）。
            // 映射一旦出错就会产出记录号错乱的条目，而这些条目会被恢复流程当作真实
            // 目标去读簇，因此这里宁可丢弃也不能放行。
            // NTFS 3.0 及更早的记录没有该字段，值为 0 时不参与判定。
            if (usingMftLocator && indexValue != 0 && recordBytes.size() >= 48)
            {
                const std::uint32_t headerRecordIndex = le32(recordBytes.data() + 44);
                if (headerRecordIndex != 0
                    && static_cast<std::uint64_t>(headerRecordIndex) != indexValue)
                {
                    ++consecutiveInvalidCount;
                    continue;
                }
            }

            consecutiveEmptyCount = 0;
            consecutiveInvalidCount = 0;
            validRecordCount += 1;
            if (!keepNamelessRecords
                && recordValue.fileName.isEmpty()
                && recordValue.recordIndex != 5)
            {
                continue;
            }
            // 误删扫描只需要删除项，以及重建路径提示要用的目录记录（在用与否都要）。
            // 在用的普通文件占 MFT 绝大多数，全部留下会让内存随 MFT 规模线性膨胀。
            if (keepDeletedAndDirectoriesOnly
                && recordValue.inUse
                && !recordValue.isDirectory)
            {
                continue;
            }
            recordsOut.push_back(std::move(recordValue));
        }

        if (mftHandle != INVALID_HANDLE_VALUE)
        {
            mftHandle.close();
        }
        volumeHandle.close();

        // 扫描路径诊断：一次性给出走了哪条路径、计划/实际扫描量、失败分布。
        // 「扫描完成但 0 项」有多种成因（提前中断 / 记录全在用 / 读取失败），
        // 只看结果数分辨不出来，这条日志是定位入口。
        // 全小写下划线 token 不会被 i18n 审计提取，改文案无需同步语言包。
        {
            std::uint64_t inUseCount = 0;
            std::uint64_t directoryCount = 0;
            std::uint64_t deletedFileCount = 0;
            for (const NtfsRawRecord& recordValue : recordsOut)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }

                if (recordValue.inUse)
                {
                    ++inUseCount;
                }
                if (recordValue.isDirectory)
                {
                    ++directoryCount;
                }
                if (!recordValue.inUse && !recordValue.isDirectory)
                {
                    ++deletedFileCount;
                }
            }

            kLogEvent event;
            info << event
                << "ntfs_scan_diag"
                << " volume="
                << volumeRoot.toStdString()
                << " source="
                << (usingMftLocator
                    ? "volume_runlist"
                    : (usingVolumeFallback ? "volume_linear" : "mft_logical_file"))
                << " parsecount="
                << static_cast<qulonglong>(parseCount)
                << " stoppedat="
                << static_cast<qulonglong>(stoppedAtIndex)
                << " records="
                << recordsOut.size()
                << " scanned="
                << static_cast<qulonglong>(validRecordCount)
                << " readfail="
                << static_cast<qulonglong>(totalReadFailCount)
                << " invalid="
                << static_cast<qulonglong>(totalInvalidCount)
                << " inuse="
                << static_cast<qulonglong>(inUseCount)
                << " dir="
                << static_cast<qulonglong>(directoryCount)
                << " deletedfile="
                << static_cast<qulonglong>(deletedFileCount)
                << " bytesperrecord="
                << bytesPerRecord
                << " lastreadfail="
                << lastReadFailText.toStdString()
                << eol;
        }

        if (recordsOut.empty())
        {
            errorTextOut = QStringLiteral("MFT解析结果为空。");
            return false;
        }

        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (cacheAllowed)
        {
            std::shared_ptr<NtfsCacheEntry> cacheEntry = std::make_shared<NtfsCacheEntry>();
            if (copyRecordsOut)
            {
                cacheEntry->records = recordsOut;
            }
            else
            {
                cacheEntry->records = std::move(recordsOut);
            }
            cacheEntry->loadedMsec = nowMsec;
            cacheEntry->recordLimit = MaxRecordCount;
            cacheEntry->fsctlFallbackAllowed = allowFsctlFallback;
            if (!buildNtfsCacheIndex(*cacheEntry, canceled))
            {
                return false;
            }

            if (directoryCanceled(canceled))
            {
                return false;
            }
            std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
            if (directoryCanceled(canceled))
            {
                return false;
            }
            g_ntfsCache[cacheKey] = cacheEntry;
            if (cacheEntryOut != nullptr)
            {
                *cacheEntryOut = cacheEntry;
            }
            if (!copyRecordsOut)
            {
                recordsOut.clear();
            }
        }
        else if (cacheEntryOut != nullptr)
        {
            std::shared_ptr<NtfsCacheEntry> cacheEntry = std::make_shared<NtfsCacheEntry>();
            if (copyRecordsOut)
            {
                cacheEntry->records = recordsOut;
            }
            else
            {
                cacheEntry->records = std::move(recordsOut);
            }
            cacheEntry->loadedMsec = nowMsec;
            cacheEntry->recordLimit = MaxRecordCount;
            cacheEntry->fsctlFallbackAllowed = allowFsctlFallback;
            if (!buildNtfsCacheIndex(*cacheEntry, canceled))
            {
                return false;
            }
            if (directoryCanceled(canceled))
            {
                return false;
            }
            *cacheEntryOut = cacheEntry;
            if (!copyRecordsOut)
            {
                recordsOut.clear();
            }
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        return true;
    }

    // resolveNtfsDirectoryIndex 作用：按路径段定位目标目录的 MFT 记录号。
    bool resolveNtfsDirectoryIndex(
        const NtfsCacheEntry& cacheEntry,
        const QStringList& pathSegments,
        std::uint64_t& directoryIndexOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        std::uint64_t currentIndex = 5;
        for (const QString& segmentText : pathSegments)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            bool found = false;
            const auto childRange = findNtfsDirectoryLinkRange(cacheEntry.directoryLinks, currentIndex);
            for (auto it = childRange.first; it != childRange.second; ++it)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }

                const auto recordIt = cacheEntry.recordOffsetByIndex.find(it->recordIndex);
                if (recordIt == cacheEntry.recordOffsetByIndex.end())
                {
                    continue;
                }

                const NtfsRawRecord& childRecord = cacheEntry.records[recordIt->second];
                if (!childRecord.inUse || !childRecord.isDirectory)
                {
                    continue;
                }
                if (it->fileName.compare(segmentText, Qt::CaseInsensitive) == 0)
                {
                    currentIndex = childRecord.recordIndex;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return false;
            }
        }
        directoryIndexOut = currentIndex;
        return true;
    }

    // buildNtfsPathHintByName 作用：
    // - 根据“显示名 + 父目录记录号”重建尽可能完整的路径提示；
    // - 供误删扫描在多 FILE_NAME 链接场景下生成更准确的候选路径。
    QString buildNtfsPathHintByName(
        const QString& volumeRootPath,
        const QString& targetName,
        const std::uint64_t parentIndexValue,
        const std::unordered_map<std::uint64_t, const NtfsRawRecord*>& recordMap)
    {
        QStringList segments;
        segments.push_front(targetName);
        std::uint64_t parentIndex = parentIndexValue;
        int depthGuard = 0;
        while (parentIndex != 0 && parentIndex != 5 && depthGuard < 64)
        {
            const auto parentIt = recordMap.find(parentIndex);
            if (parentIt == recordMap.end() || parentIt->second == nullptr)
            {
                break;
            }
            const NtfsRawRecord* parentRecord = parentIt->second;
            if (parentRecord->fileName.isEmpty())
            {
                break;
            }
            segments.push_front(parentRecord->fileName);
            parentIndex = parentRecord->parentIndex;
            ++depthGuard;
        }

        QString pathText = QDir::toNativeSeparators(volumeRootPath);
        if (!pathText.endsWith('\\'))
        {
            pathText += '\\';
        }
        pathText += segments.join('\\');
        return pathText;
    }

    // buildNtfsPathHint 作用：从记录默认名称与父链重建可读路径。
    QString buildNtfsPathHint(
        const QString& volumeRootPath,
        const NtfsRawRecord& targetRecord,
        const std::unordered_map<std::uint64_t, const NtfsRawRecord*>& recordMap)
    {
        return buildNtfsPathHintByName(
            volumeRootPath,
            targetRecord.fileName,
            targetRecord.parentIndex,
            recordMap);
    }

    // tryReadNtfsSingleRecordByFsctl 作用：
    // - 按记录号读取单条 NTFS MFT 记录；
    // - 用于导出时按需回读 resident 数据，避免扫描阶段缓存大量文件内容。
    bool tryReadNtfsSingleRecordByFsctl(
        const HANDLE volumeHandle,
        const std::uint64_t fileReference,
        const std::uint16_t bytesPerSectorHint,
        const std::uint32_t bytesPerRecordHint,
        NtfsRawRecord& recordOut,
        QString& errorTextOut)
    {
        NTFS_VOLUME_DATA_BUFFER volumeData{};
        DWORD returnedBytes = 0;
        if (::DeviceIoControl(
            volumeHandle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            nullptr,
            0,
            &volumeData,
            static_cast<DWORD>(sizeof(volumeData)),
            &returnedBytes,
            nullptr) == FALSE)
        {
            errorTextOut = QStringLiteral("FSCTL_GET_NTFS_VOLUME_DATA失败, code=%1").arg(::GetLastError());
            return false;
        }

        std::uint32_t bytesPerRecord = volumeData.BytesPerFileRecordSegment;
        if (bytesPerRecord < 512 || bytesPerRecord > 16384)
        {
            bytesPerRecord = bytesPerRecordHint;
        }
        if (bytesPerRecord < 512 || bytesPerRecord > 16384)
        {
            errorTextOut = QStringLiteral("读取单条记录失败：MFT记录大小异常, bytesPerRecord=%1").arg(bytesPerRecord);
            return false;
        }

        const std::size_t outputHeaderBytes = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer);
        const std::size_t outputBufferBytes = outputHeaderBytes + static_cast<std::size_t>(bytesPerRecord) + 16ULL;
        std::vector<std::uint8_t> outputBuffer(outputBufferBytes);

        NTFS_FILE_RECORD_INPUT_BUFFER inputBuffer{};
        inputBuffer.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(fileReference);
        returnedBytes = 0;
        if (::DeviceIoControl(
            volumeHandle,
            FSCTL_GET_NTFS_FILE_RECORD,
            &inputBuffer,
            static_cast<DWORD>(sizeof(inputBuffer)),
            outputBuffer.data(),
            static_cast<DWORD>(outputBuffer.size()),
            &returnedBytes,
            nullptr) == FALSE)
        {
            errorTextOut = QStringLiteral("FSCTL_GET_NTFS_FILE_RECORD失败, code=%1").arg(::GetLastError());
            return false;
        }
        if (returnedBytes <= outputHeaderBytes)
        {
            errorTextOut = QStringLiteral("FSCTL_GET_NTFS_FILE_RECORD返回长度不足。");
            return false;
        }

        NTFS_FILE_RECORD_OUTPUT_BUFFER* outputRecord =
            reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(outputBuffer.data());
        const std::uint64_t actualRecordIndex =
            static_cast<std::uint64_t>(outputRecord->FileReferenceNumber.QuadPart & 0x0000FFFFFFFFFFFFULL);
        const std::uint32_t fileRecordLength = outputRecord->FileRecordLength;
        if (actualRecordIndex != fileReference)
        {
            errorTextOut = QStringLiteral("目标记录不存在或已被替换, expect=%1, actual=%2")
                .arg(static_cast<qulonglong>(fileReference))
                .arg(static_cast<qulonglong>(actualRecordIndex));
            return false;
        }
        if (fileRecordLength < 64
            || fileRecordLength > bytesPerRecord
            || outputHeaderBytes + fileRecordLength > returnedBytes
            || outputHeaderBytes + fileRecordLength > outputBuffer.size())
        {
            errorTextOut = QStringLiteral("单条记录长度异常, recordLength=%1").arg(fileRecordLength);
            return false;
        }

        std::vector<std::byte> recordBytes(fileRecordLength);
        std::memcpy(recordBytes.data(), outputBuffer.data() + outputHeaderBytes, fileRecordLength);
        if (!parseNtfsRecord(recordBytes, fileReference, bytesPerSectorHint, true, recordOut))
        {
            errorTextOut = QStringLiteral("单条记录解析失败。");
            return false;
        }
        return true;
    }

    // loadNtfsSingleRecord 作用：
    // - 为单文件恢复按需读取指定 MFT 记录；
    // - 优先 FSCTL 精确取回，失败后再回退到 $MFT/卷偏移直读。
    bool loadNtfsSingleRecord(
        const QString& volumeRoot,
        const std::uint64_t fileReference,
        NtfsRawRecord& recordOut,
        QString& errorTextOut)
    {
        errorTextOut.clear();
        const QString volumeDevicePath = buildVolumeDevicePath(volumeRoot);
        QString openVolumeErrorText;
        HANDLE volumeHandle = openReadHandle(volumeDevicePath, openVolumeErrorText);
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openVolumeErrorText;
            return false;
        }

        std::array<std::byte, 512> bootBytes{};
        if (!readBytesAtOffset(volumeHandle, 0, 512, bootBytes.data(), errorTextOut))
        {
            ::CloseHandle(volumeHandle);
            return false;
        }

        const std::uint16_t bytesPerSector = le16(bootBytes.data() + 11);
        const std::uint8_t sectorsPerCluster = static_cast<std::uint8_t>(bootBytes[13]);
        if (bytesPerSector == 0 || sectorsPerCluster == 0)
        {
            ::CloseHandle(volumeHandle);
            errorTextOut = QStringLiteral("NTFS 引导参数异常：扇区或簇大小为 0。");
            return false;
        }

        const std::uint64_t bytesPerCluster =
            static_cast<std::uint64_t>(bytesPerSector) *
            static_cast<std::uint64_t>(sectorsPerCluster);
        const std::uint64_t mftStartCluster = le64(bootBytes.data() + 0x30);
        if (mftStartCluster >
            std::numeric_limits<std::uint64_t>::max() / bytesPerCluster)
        {
            ::CloseHandle(volumeHandle);
            errorTextOut = QStringLiteral("MFT 起始簇换算发生溢出。");
            return false;
        }
        const std::uint64_t mftStartOffset = mftStartCluster * bytesPerCluster;
        const std::int8_t clustersPerRecord = static_cast<std::int8_t>(bootBytes[64]);
        std::uint32_t bytesPerRecord = 1024;
        if (clustersPerRecord < 0)
        {
            const int recordSizeExponent =
                -static_cast<int>(clustersPerRecord);
            bytesPerRecord = recordSizeExponent < 32
                ? (1u << recordSizeExponent)
                : 0U;
        }
        else
        {
            bytesPerRecord =
                static_cast<std::uint32_t>(bytesPerSector) *
                static_cast<std::uint32_t>(sectorsPerCluster) *
                static_cast<std::uint32_t>(clustersPerRecord);
        }
        if (bytesPerRecord < 512 || bytesPerRecord > 16384)
        {
            ::CloseHandle(volumeHandle);
            errorTextOut = QStringLiteral("MFT记录大小异常: %1").arg(bytesPerRecord);
            return false;
        }
        const bool recordByteOffsetValid =
            fileReference <=
                std::numeric_limits<std::uint64_t>::max() /
                    static_cast<std::uint64_t>(bytesPerRecord);
        const std::uint64_t recordByteOffset = recordByteOffsetValid
            ? fileReference * static_cast<std::uint64_t>(bytesPerRecord)
            : 0ULL;

        QString fsctlErrorText;
        if (tryReadNtfsSingleRecordByFsctl(volumeHandle, fileReference, bytesPerSector, bytesPerRecord, recordOut, fsctlErrorText))
        {
            ::CloseHandle(volumeHandle);
            return true;
        }

        const QString mftPath = QStringLiteral("\\\\.\\%1\\$MFT").arg(volumeRoot.left(2).toUpper());
        QString openMftErrorText;
        HANDLE mftHandle = openReadHandle(mftPath, openMftErrorText);
        if (mftHandle != INVALID_HANDLE_VALUE)
        {
            std::vector<std::byte> recordBytes(bytesPerRecord);
            QString readErrorText;
            if (recordByteOffsetValid &&
                readBytesAtOffset(
                mftHandle,
                recordByteOffset,
                bytesPerRecord,
                recordBytes.data(),
                readErrorText)
                && parseNtfsRecord(recordBytes, fileReference, bytesPerSector, true, recordOut))
            {
                ::CloseHandle(mftHandle);
                ::CloseHandle(volumeHandle);
                return true;
            }
            if (!readErrorText.isEmpty())
            {
                fsctlErrorText += QStringLiteral("; $MFT直读失败: ") + readErrorText;
            }
            else
            {
                fsctlErrorText += QStringLiteral("; $MFT直读失败: 记录解析失败");
            }
            ::CloseHandle(mftHandle);
        }
        else if (!openMftErrorText.isEmpty())
        {
            fsctlErrorText += QStringLiteral("; 打开$MFT失败: ") + openMftErrorText;
        }

        if (!recordByteOffsetValid ||
            recordByteOffset >
                std::numeric_limits<std::uint64_t>::max() -
                    mftStartOffset)
        {
            ::CloseHandle(volumeHandle);
            errorTextOut = QStringLiteral("卷偏移记录解析失败。");
            return false;
        }

        // 卷偏移回退优先按 $MFT 自身 runlist 定位：
        // 直接用“MFT 起始偏移 + 记录号 * 记录长度”只在 $MFT 完全连续时才成立，
        // 碎片化时会落到其它文件的数据上，导致扫描列表里明明存在的条目恢复不了。
        std::uint64_t rawRecordOffset = mftStartOffset + recordByteOffset;
        {
            NtfsMftLocator mftLocator;
            QString locatorErrorText;
            std::uint64_t mappedOffset = 0;
            std::uint64_t mappedContiguousBytes = 0;
            if (loadNtfsMftLocator(
                volumeHandle,
                mftStartOffset,
                bytesPerSector,
                bytesPerCluster,
                bytesPerRecord,
                mftLocator,
                locatorErrorText)
                && mftLocator.tryMapRecordRange(
                    fileReference,
                    mappedOffset,
                    mappedContiguousBytes))
            {
                rawRecordOffset = mappedOffset;
            }
            else if (!locatorErrorText.isEmpty())
            {
                fsctlErrorText += QStringLiteral("; $MFT映射不可用: ") + locatorErrorText;
            }
        }

        std::vector<std::byte> recordBytes(bytesPerRecord);
        if (!readBytesAtSectorAlignedOffset(
            volumeHandle,
            rawRecordOffset,
            bytesPerRecord,
            bytesPerSector,
            QStringLiteral("卷偏移读取单条记录"),
            0,
            recordBytes.data(),
            errorTextOut))
        {
            ::CloseHandle(volumeHandle);
            if (!fsctlErrorText.isEmpty())
            {
                errorTextOut = fsctlErrorText + QStringLiteral("; 卷偏移直读失败: ") + errorTextOut;
            }
            return false;
        }
        ::CloseHandle(volumeHandle);

        // 无论走映射还是线性偏移，都必须核对记录头中的 MFT record number，
        // 防止把同一物理偏移上的其它记录当成恢复目标。
        const std::uint32_t actualRawRecordIndex =
            recordBytes.size() >= 48
            ? le32(recordBytes.data() + 44)
            : std::numeric_limits<std::uint32_t>::max();
        if (fileReference >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()) ||
            actualRawRecordIndex !=
                static_cast<std::uint32_t>(fileReference))
        {
            errorTextOut = QStringLiteral(
                "目标记录不存在或已被替换, expect=%1, actual=%2")
                .arg(static_cast<qulonglong>(fileReference))
                .arg(actualRawRecordIndex);
            return false;
        }
        if (!parseNtfsRecord(recordBytes, fileReference, bytesPerSector, true, recordOut))
        {
            errorTextOut = fsctlErrorText.isEmpty()
                ? QStringLiteral("卷偏移记录解析失败。")
                : (fsctlErrorText + QStringLiteral("; 卷偏移记录解析失败。"));
            return false;
        }
        return true;
    }

    // queryNtfsFileReferenceByPath 作用：
    // - 打开目标目录并读取稳定的 NTFS 文件引用号；
    // - 当目录记录号超出快速 MFT 窗口时，避免把“未扫描到”误判成目录不存在。
    bool queryNtfsFileReferenceByPath(
        const QString& directoryPath,
        std::uint64_t& fileReferenceOut,
        QString& errorTextOut)
    {
        fileReferenceOut = 0;
        const std::wstring pathWide = toWide(
            QDir::toNativeSeparators(QDir::cleanPath(directoryPath)));
        HANDLE directoryHandle = ::CreateFileW(
            pathWide.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (directoryHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = QStringLiteral(
                "读取 NTFS 目录引用号失败, code=%1")
                .arg(::GetLastError());
            return false;
        }

        BY_HANDLE_FILE_INFORMATION fileInfo{};
        const BOOL queryOk =
            ::GetFileInformationByHandle(directoryHandle, &fileInfo);
        const DWORD queryError = queryOk != FALSE
            ? ERROR_SUCCESS
            : ::GetLastError();
        ::CloseHandle(directoryHandle);
        if (queryOk == FALSE)
        {
            errorTextOut = QStringLiteral(
                "读取 NTFS 目录引用号失败, code=%1")
                .arg(queryError);
            return false;
        }

        const std::uint64_t rawFileReference =
            (static_cast<std::uint64_t>(fileInfo.nFileIndexHigh) << 32ULL) |
            static_cast<std::uint64_t>(fileInfo.nFileIndexLow);
        fileReferenceOut =
            rawFileReference & 0x0000FFFFFFFFFFFFULL;
        return true;
    }

    // supplementNtfsDirectoryEntriesByMftEnumeration 作用：
    // - 使用 FSCTL_ENUM_USN_DATA 遍历活动 MFT 记录，只收集目标父目录的直接孩子；
    // - 对命中的记录再用 FSCTL_GET_NTFS_FILE_RECORD 读取原始 FILE_NAME/大小/时间；
    // - 该路径不依赖 QDir/FindFirstFile 枚举，专门补齐快速窗口之外的高编号记录。
    bool supplementNtfsDirectoryEntriesByMftEnumeration(
        const QString& volumeRoot,
        const QString& currentPath,
        const std::uint64_t directoryFileReference,
        std::vector<ks::file::ManualDirectoryEntry>& entriesInOut,
        std::size_t& addedCountOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        struct DirectoryCandidate
        {
            std::uint64_t fileReference = 0; // fileReference：候选孩子的 48 位 MFT 记录号。
            QString fileName;                // fileName：USN/MFT 枚举返回的当前链接名称。
            std::uint32_t fileAttributes = 0;// fileAttributes：目录标志的低成本兜底来源。
        };

        addedCountOut = 0;
        errorTextOut.clear();
        QString openErrorText;
        DirectoryReadHandle volumeHandle(
            openReadHandle(buildVolumeDevicePath(volumeRoot), openErrorText));
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openErrorText;
            return false;
        }

        NTFS_VOLUME_DATA_BUFFER volumeData{};
        DWORD returnedBytes = 0;
        if (::DeviceIoControl(
            volumeHandle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            nullptr,
            0,
            &volumeData,
            static_cast<DWORD>(sizeof(volumeData)),
            &returnedBytes,
            nullptr) == FALSE)
        {
            errorTextOut = QStringLiteral(
                "FSCTL_GET_NTFS_VOLUME_DATA失败, code=%1")
                .arg(::GetLastError());
            volumeHandle.close();
            return false;
        }

        // MFT_ENUM_DATA 只返回紧凑的 USN/MFT 元数据，比读取并缓存数百万条完整 MFT 记录更适合目录补全。
        constexpr DWORD EnumerationBufferBytes = 1024UL * 1024UL;
        std::vector<std::uint8_t> enumerationBuffer(
            static_cast<std::size_t>(EnumerationBufferBytes));
        std::vector<DirectoryCandidate> candidateList;
        QSet<QString> candidateKeySet;

        MFT_ENUM_DATA enumerationData{};
        enumerationData.StartFileReferenceNumber = 0;
        enumerationData.LowUsn = 0;
        enumerationData.HighUsn =
            std::numeric_limits<USN>::max();

        bool enumerationFinished = false;
        while (!enumerationFinished)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            returnedBytes = 0;
            const BOOL enumerateOk = ::DeviceIoControl(
                volumeHandle,
                FSCTL_ENUM_USN_DATA,
                &enumerationData,
                static_cast<DWORD>(sizeof(enumerationData)),
                enumerationBuffer.data(),
                EnumerationBufferBytes,
                &returnedBytes,
                nullptr);
            const DWORD enumerateError = enumerateOk == FALSE ? ::GetLastError() : ERROR_SUCCESS;
            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (enumerateOk == FALSE)
            {
                if (enumerateError == ERROR_HANDLE_EOF)
                {
                    break;
                }

                errorTextOut = QStringLiteral(
                    "FSCTL_ENUM_USN_DATA失败, code=%1")
                    .arg(enumerateError);
                volumeHandle.close();
                return false;
            }
            if (returnedBytes <= sizeof(DWORDLONG))
            {
                break;
            }

            DWORDLONG nextFileReference = 0;
            std::memcpy(
                &nextFileReference,
                enumerationBuffer.data(),
                sizeof(nextFileReference));
            std::size_t recordOffset = sizeof(DWORDLONG);
            while (recordOffset + sizeof(USN_RECORD_COMMON_HEADER) <= returnedBytes)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }

                const USN_RECORD_COMMON_HEADER* commonHeader =
                    reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(
                        enumerationBuffer.data() + recordOffset);
                const std::uint32_t recordLength = commonHeader->RecordLength;
                if (recordLength < sizeof(USN_RECORD_COMMON_HEADER) ||
                    recordOffset + recordLength > returnedBytes)
                {
                    errorTextOut = QStringLiteral(
                        "FSCTL_ENUM_USN_DATA返回了损坏的记录。");
                    volumeHandle.close();
                    return false;
                }

                // NTFS 的 FSCTL_ENUM_USN_DATA 当前返回 V2 记录；其它版本跳过而不是按错误布局读取。
                if (commonHeader->MajorVersion == 2 &&
                    recordLength >= offsetof(USN_RECORD_V2, FileName))
                {
                    const USN_RECORD_V2* usnRecord =
                        reinterpret_cast<const USN_RECORD_V2*>(commonHeader);
                    const std::size_t fileNameEnd =
                        static_cast<std::size_t>(usnRecord->FileNameOffset) +
                        static_cast<std::size_t>(usnRecord->FileNameLength);
                    const std::uint64_t parentFileReference =
                        static_cast<std::uint64_t>(
                            usnRecord->ParentFileReferenceNumber) &
                        0x0000FFFFFFFFFFFFULL;
                    if (parentFileReference == directoryFileReference &&
                        usnRecord->FileNameLength > 0 &&
                        fileNameEnd <= recordLength)
                    {
                        DirectoryCandidate candidate{};
                        candidate.fileReference =
                            static_cast<std::uint64_t>(
                                usnRecord->FileReferenceNumber) &
                            0x0000FFFFFFFFFFFFULL;
                        candidate.fileName = QString::fromWCharArray(
                            reinterpret_cast<const wchar_t*>(
                                reinterpret_cast<const std::uint8_t*>(usnRecord) +
                                usnRecord->FileNameOffset),
                            static_cast<qsizetype>(
                                usnRecord->FileNameLength / sizeof(wchar_t)));
                        candidate.fileAttributes = usnRecord->FileAttributes;

                        const QString candidateKey =
                            QStringLiteral("%1|%2")
                            .arg(static_cast<qulonglong>(
                                candidate.fileReference))
                            .arg(candidate.fileName.toCaseFolded());
                        if (!candidate.fileName.isEmpty() &&
                            !candidateKeySet.contains(candidateKey))
                        {
                            candidateKeySet.insert(candidateKey);
                            candidateList.push_back(std::move(candidate));
                        }
                    }
                }
                recordOffset += recordLength;
            }

            const std::uint64_t currentStart =
                static_cast<std::uint64_t>(
                    enumerationData.StartFileReferenceNumber);
            if (nextFileReference <= currentStart)
            {
                enumerationFinished = true;
            }
            else
            {
                enumerationData.StartFileReferenceNumber =
                    nextFileReference;
            }
        }

        QSet<QString> existingNameSet;
        existingNameSet.reserve(
            static_cast<int>(entriesInOut.size() * 2ULL + 16ULL));
        for (const ks::file::ManualDirectoryEntry& entryValue : entriesInOut)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            existingNameSet.insert(entryValue.name.toCaseFolded());
        }

        const std::uint16_t bytesPerSector =
            static_cast<std::uint16_t>(volumeData.BytesPerSector);
        const std::uint32_t bytesPerRecord =
            volumeData.BytesPerFileRecordSegment;
        for (const DirectoryCandidate& candidate : candidateList)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            NtfsRawRecord recordValue{};
            QString recordErrorText;
            const bool recordOk = tryReadNtfsSingleRecordByFsctl(
                volumeHandle,
                candidate.fileReference,
                bytesPerSector,
                bytesPerRecord,
                recordValue,
                recordErrorText);

            if (directoryCanceled(canceled))
            {
                return false;
            }
            // appendEntry 统一按名称去重；同一目录在 NTFS 中不允许存在大小写折叠后相同的两个名字。
            const auto appendEntry =
                [&entriesInOut,
                 &existingNameSet,
                 &currentPath,
                 &candidate](
                    const QString& fileName,
                    const bool isDirectory,
                    const std::uint64_t sizeBytes,
                    const std::uint64_t modifiedTime100ns)
                {
                    const QString normalizedName = fileName;
                    const QString normalizedKey =
                        normalizedName.toCaseFolded();
                    if (normalizedName.isEmpty() ||
                        existingNameSet.contains(normalizedKey))
                    {
                        return false;
                    }

                    ks::file::ManualDirectoryEntry itemValue{};
                    itemValue.name = normalizedName;
                    itemValue.absolutePath =
                        QDir(currentPath).filePath(normalizedName);
                    itemValue.isDirectory = isDirectory;
                    itemValue.sizeBytes =
                        isDirectory ? 0 : sizeBytes;
                    itemValue.modifiedTime =
                        fileTimeToLocal(modifiedTime100ns);
                    itemValue.typeText =
                        buildTypeText(normalizedName, isDirectory);
                    itemValue.ntfsFileReference =
                        candidate.fileReference;
                    existingNameSet.insert(normalizedKey);
                    entriesInOut.push_back(std::move(itemValue));
                    return true;
                };

            bool appendedExactLink = false;
            if (recordOk && recordValue.inUse)
            {
                for (const NtfsNameLink& nameLink : recordValue.nameLinks)
                {
                    if (directoryCanceled(canceled))
                    {
                        return false;
                    }

                    if (nameLink.parentIndex !=
                        directoryFileReference)
                    {
                        continue;
                    }
                    appendedExactLink =
                        appendEntry(
                            nameLink.fileName,
                            recordValue.isDirectory,
                            recordValue.sizeBytes,
                            recordValue.modifiedTime100ns) ||
                        appendedExactLink;
                }
            }

            // 卷在枚举期间可能变化；精确 MFT 回读失败时仍保留 USN 活动记录提供的名称与目录标志。
            if (!appendedExactLink)
            {
                const bool isDirectory =
                    (candidate.fileAttributes &
                        FILE_ATTRIBUTE_DIRECTORY) != 0;
                appendedExactLink = appendEntry(
                    candidate.fileName,
                    isDirectory,
                    0,
                    0);
            }
            if (appendedExactLink)
            {
                addedCountOut += 1;
            }
        }

        if (directoryCanceled(canceled))
        {
            return false;
        }
        volumeHandle.close();
        return true;
    }

    // decodeFatDateTime 作用：把 FAT 日期+时间转换为 QDateTime。
    QDateTime decodeFatDateTime(const std::uint16_t dateValue, const std::uint16_t timeValue)
    {
        const int yearValue = 1980 + ((dateValue >> 9) & 0x7F);
        const int monthValue = (dateValue >> 5) & 0x0F;
        const int dayValue = dateValue & 0x1F;
        const int hourValue = (timeValue >> 11) & 0x1F;
        const int minuteValue = (timeValue >> 5) & 0x3F;
        const int secondValue = (timeValue & 0x1F) * 2;
        if (monthValue <= 0 || monthValue > 12 || dayValue <= 0 || dayValue > 31)
        {
            return QDateTime();
        }
        const QDate dateObj(yearValue, monthValue, dayValue);
        const QTime timeObj(hourValue, minuteValue, secondValue);
        return (dateObj.isValid() && timeObj.isValid())
            ? QDateTime(dateObj, timeObj, QTimeZone::systemTimeZone())
            : QDateTime();
    }

    // decodeFatLongNamePart 作用：解析 LFN 条目的 13 个 UTF-16 字符。
    QString decodeFatLongNamePart(const std::byte* entryPtr)
    {
        const std::array<int, 13> offsets{
            1, 3, 5, 7, 9,
            14, 16, 18, 20, 22, 24,
            28, 30
        };
        QString textOut;
        textOut.reserve(13);
        for (int offsetValue : offsets)
        {
            const char16_t ch = static_cast<char16_t>(le16(entryPtr + offsetValue));
            if (ch == u'\0' || ch == u'\xFFFF')
            {
                break;
            }
            textOut.append(QChar(ch));
        }
        return textOut;
    }

    // decodeFatShortName 作用：把 8.3 名转换为常见字符串。
    QString decodeFatShortName(const std::byte* entryPtr)
    {
        QByteArray nameText(reinterpret_cast<const char*>(entryPtr), 8);
        QByteArray extText(reinterpret_cast<const char*>(entryPtr + 8), 3);
        nameText = nameText.trimmed();
        extText = extText.trimmed();
        const QString baseText = QString::fromLatin1(nameText);
        const QString extPart = QString::fromLatin1(extText);
        return extPart.isEmpty() ? baseText : (baseText + QStringLiteral(".") + extPart);
    }

    // readFat32BootInfo 作用：读取 FAT32 BPB 并计算关键偏移。
    bool readFat32BootInfo(const HANDLE volumeHandle, Fat32BootInfo& infoOut, QString& errorTextOut)
    {
        std::array<std::byte, 512> bootBytes{};
        if (!readBytesAtOffset(volumeHandle, 0, 512, bootBytes.data(), errorTextOut))
        {
            return false;
        }

        const QByteArray fsText(reinterpret_cast<const char*>(bootBytes.data() + 82), 8);
        if (!fsText.startsWith("FAT32"))
        {
            errorTextOut = QStringLiteral("不是 FAT32 卷。");
            return false;
        }

        infoOut.bytesPerSector = le16(bootBytes.data() + 11);
        infoOut.sectorsPerCluster = static_cast<std::uint8_t>(bootBytes[13]);
        infoOut.reservedSectors = le16(bootBytes.data() + 14);
        infoOut.fatCount = static_cast<std::uint8_t>(bootBytes[16]);
        infoOut.sectorsPerFat = le32(bootBytes.data() + 36);
        infoOut.rootCluster = le32(bootBytes.data() + 44);
        if (infoOut.bytesPerSector == 0 || infoOut.sectorsPerCluster == 0 || infoOut.sectorsPerFat == 0)
        {
            errorTextOut = QStringLiteral("FAT32 BPB 参数异常。");
            return false;
        }

        infoOut.bytesPerCluster =
            static_cast<std::uint32_t>(infoOut.bytesPerSector) *
            static_cast<std::uint32_t>(infoOut.sectorsPerCluster);
        infoOut.fatOffset =
            static_cast<std::uint64_t>(infoOut.reservedSectors) *
            static_cast<std::uint64_t>(infoOut.bytesPerSector);
        const std::uint64_t dataStartSector =
            static_cast<std::uint64_t>(infoOut.reservedSectors) +
            static_cast<std::uint64_t>(infoOut.fatCount) * static_cast<std::uint64_t>(infoOut.sectorsPerFat);
        infoOut.dataOffset = dataStartSector * static_cast<std::uint64_t>(infoOut.bytesPerSector);
        return true;
    }

    // clusterOffset 作用：簇号转卷内字节偏移。
    std::uint64_t clusterOffset(const Fat32BootInfo& infoValue, const std::uint32_t clusterValue)
    {
        const std::uint64_t indexValue = (clusterValue <= 2) ? 0 : static_cast<std::uint64_t>(clusterValue - 2);
        return infoValue.dataOffset + indexValue * static_cast<std::uint64_t>(infoValue.bytesPerCluster);
    }

    // readFatNextCluster 作用：读取 FAT 表中的下一簇编号。
    bool readFatNextCluster(
        const HANDLE volumeHandle,
        const Fat32BootInfo& infoValue,
        const std::uint32_t clusterValue,
        std::uint32_t& nextOut,
        QString& errorTextOut)
    {
        const std::uint64_t entryOffset = infoValue.fatOffset + static_cast<std::uint64_t>(clusterValue) * 4ULL;
        std::array<std::byte, 4> entryBytes{};
        if (!readBytesAtSectorAlignedOffset(
            volumeHandle,
            entryOffset,
            4,
            infoValue.bytesPerSector,
            QStringLiteral("FAT32 FAT entry"),
            clusterValue,
            entryBytes.data(),
            errorTextOut))
        {
            return false;
        }
        nextOut = (le32(entryBytes.data()) & 0x0FFFFFFF);
        return true;
    }

    // loadClusterChain 作用：按 FAT 链读取目录簇序列。
    bool loadClusterChain(
        const HANDLE volumeHandle,
        const Fat32BootInfo& infoValue,
        const std::uint32_t firstCluster,
        std::vector<std::uint32_t>& chainOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        chainOut.clear();
        if (firstCluster < 2)
        {
            return false;
        }

        std::uint32_t currentCluster = firstCluster;
        constexpr std::size_t MaxClusterCount = 262144;
        for (std::size_t i = 0; i < MaxClusterCount; ++i)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            chainOut.push_back(currentCluster);
            std::uint32_t nextCluster = 0;
            if (!readFatNextCluster(volumeHandle, infoValue, currentCluster, nextCluster, errorTextOut))
            {
                return false;
            }
            if (nextCluster >= 0x0FFFFFF8 || nextCluster == 0 || nextCluster == currentCluster)
            {
                break;
            }
            currentCluster = nextCluster;
        }
        return !directoryCanceled(canceled) && !chainOut.empty();
    }

    // enumerateFatDirectoryByCluster 作用：解析某目录簇链下的目录项。
    bool enumerateFatDirectoryByCluster(
        const HANDLE volumeHandle,
        const Fat32BootInfo& infoValue,
        const std::uint32_t dirCluster,
        std::vector<Fat32Entry>& entriesOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        entriesOut.clear();
        std::vector<std::uint32_t> chainList;
        if (!loadClusterChain(volumeHandle, infoValue, dirCluster, chainList, errorTextOut, canceled))
        {
            return false;
        }

        std::vector<std::byte> clusterBytes(infoValue.bytesPerCluster);
        std::vector<QString> lfnParts;
        for (std::uint32_t clusterValue : chainList)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (!readBytesAtOffset(
                volumeHandle,
                clusterOffset(infoValue, clusterValue),
                infoValue.bytesPerCluster,
                clusterBytes.data(),
                errorTextOut))
            {
                return false;
            }

            for (std::size_t off = 0; off + 32 <= clusterBytes.size(); off += 32)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }
                const std::byte* entryPtr = clusterBytes.data() + off;
                const std::uint8_t firstByte = static_cast<std::uint8_t>(entryPtr[0]);
                const std::uint8_t attrByte = static_cast<std::uint8_t>(entryPtr[11]);
                if (firstByte == 0x00)
                {
                    return true;
                }
                if (firstByte == 0xE5)
                {
                    lfnParts.clear();
                    continue;
                }
                if (attrByte == 0x0F)
                {
                    lfnParts.push_back(decodeFatLongNamePart(entryPtr));
                    continue;
                }
                if ((attrByte & 0x08) != 0)
                {
                    lfnParts.clear();
                    continue;
                }

                QString entryName;
                if (!lfnParts.empty())
                {
                    for (auto it = lfnParts.rbegin(); it != lfnParts.rend(); ++it)
                    {
                        if (directoryCanceled(canceled))
                        {
                            return false;
                        }
                        entryName += *it;
                    }
                }
                else
                {
                    entryName = decodeFatShortName(entryPtr);
                }
                lfnParts.clear();
                if (entryName == QStringLiteral(".") || entryName == QStringLiteral(".."))
                {
                    continue;
                }

                const std::uint16_t clusterHigh = le16(entryPtr + 20);
                const std::uint16_t clusterLow = le16(entryPtr + 26);
                const std::uint32_t firstClusterValue =
                    (static_cast<std::uint32_t>(clusterHigh) << 16) |
                    static_cast<std::uint32_t>(clusterLow);
                const std::uint32_t fileSize = le32(entryPtr + 28);
                const std::uint16_t modTime = le16(entryPtr + 22);
                const std::uint16_t modDate = le16(entryPtr + 24);

                Fat32Entry itemValue{};
                itemValue.name = entryName;
                itemValue.firstCluster = firstClusterValue;
                itemValue.sizeBytes = fileSize;
                itemValue.isDirectory = ((attrByte & 0x10) != 0);
                itemValue.modifiedTime = decodeFatDateTime(modDate, modTime);
                entriesOut.push_back(std::move(itemValue));
            }
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        return true;
    }

    // resolveFatDirectoryCluster 作用：按路径定位到目标目录簇号。
    bool resolveFatDirectoryCluster(
        const HANDLE volumeHandle,
        const Fat32BootInfo& infoValue,
        const QStringList& pathSegments,
        std::uint32_t& clusterOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        std::uint32_t currentCluster = infoValue.rootCluster;
        for (const QString& segmentText : pathSegments)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            std::vector<Fat32Entry> children;
            if (!enumerateFatDirectoryByCluster(volumeHandle, infoValue, currentCluster, children, errorTextOut, canceled))
            {
                return false;
            }
            bool found = false;
            for (const Fat32Entry& childItem : children)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }
                if (!childItem.isDirectory)
                {
                    continue;
                }
                if (childItem.name.compare(segmentText, Qt::CaseInsensitive) == 0)
                {
                    currentCluster = childItem.firstCluster < 2 ? infoValue.rootCluster : childItem.firstCluster;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                errorTextOut = QStringLiteral("FAT32目录不存在：%1").arg(segmentText);
                return false;
            }
        }
        clusterOut = currentCluster;
        if (directoryCanceled(canceled))
        {
            return false;
        }
        return true;
    }

    // readExFatBootInfo 作用：
    // - 读取 exFAT Boot Sector 并计算 FAT/ClusterHeap 基础偏移；
    // - 返回 false 时 errorTextOut 给出可显示错误。
    bool readExFatBootInfo(const HANDLE volumeHandle, ExFatBootInfo& infoOut, QString& errorTextOut)
    {
        std::array<std::byte, 512> bootBytes{};
        if (!readBytesAtOffset(volumeHandle, 0, 512, bootBytes.data(), errorTextOut))
        {
            return false;
        }

        const QByteArray fsText(reinterpret_cast<const char*>(bootBytes.data() + 3), 8);
        if (fsText != QByteArrayLiteral("EXFAT   "))
        {
            errorTextOut = QStringLiteral("不是 exFAT 卷。");
            return false;
        }

        const std::uint8_t bytesPerSectorShift = static_cast<std::uint8_t>(bootBytes[108]);
        const std::uint8_t sectorsPerClusterShift = static_cast<std::uint8_t>(bootBytes[109]);
        if (bytesPerSectorShift < 9 || bytesPerSectorShift > 12 || sectorsPerClusterShift > 25)
        {
            errorTextOut = QStringLiteral("exFAT Boot Sector 参数异常。");
            return false;
        }

        infoOut.bytesPerSector = 1UL << bytesPerSectorShift;
        infoOut.sectorsPerCluster = 1UL << sectorsPerClusterShift;
        infoOut.bytesPerCluster = infoOut.bytesPerSector * infoOut.sectorsPerCluster;
        infoOut.fatOffsetBytes = le32(bootBytes.data() + 80) * static_cast<std::uint64_t>(infoOut.bytesPerSector);
        infoOut.clusterHeapOffsetBytes = le32(bootBytes.data() + 88) * static_cast<std::uint64_t>(infoOut.bytesPerSector);
        infoOut.clusterCount = le32(bootBytes.data() + 92);
        infoOut.rootDirectoryCluster = le32(bootBytes.data() + 96);
        if (infoOut.clusterCount == 0 ||
            infoOut.rootDirectoryCluster < 2 ||
            infoOut.rootDirectoryCluster >= infoOut.clusterCount + 2ULL ||
            infoOut.bytesPerCluster == 0)
        {
            errorTextOut = QStringLiteral("exFAT 簇参数异常。");
            return false;
        }
        return true;
    }

    // exFatClusterOffset 作用：把 exFAT 簇号转换为卷内字节偏移。
    std::uint64_t exFatClusterOffset(const ExFatBootInfo& infoValue, const std::uint32_t clusterValue)
    {
        const std::uint64_t clusterIndex = (clusterValue <= 2U) ? 0ULL : static_cast<std::uint64_t>(clusterValue - 2U);
        return infoValue.clusterHeapOffsetBytes + clusterIndex * static_cast<std::uint64_t>(infoValue.bytesPerCluster);
    }

    // readExFatNextCluster 作用：读取 exFAT FAT 表中的下一簇编号。
    bool readExFatNextCluster(
        const HANDLE volumeHandle,
        const ExFatBootInfo& infoValue,
        const std::uint32_t clusterValue,
        std::uint32_t& nextOut,
        QString& errorTextOut)
    {
        const std::uint64_t entryOffset = infoValue.fatOffsetBytes + static_cast<std::uint64_t>(clusterValue) * 4ULL;
        std::array<std::byte, 4> entryBytes{};
        if (!readBytesAtSectorAlignedOffset(
            volumeHandle,
            entryOffset,
            4,
            infoValue.bytesPerSector,
            QStringLiteral("exFAT FAT entry"),
            clusterValue,
            entryBytes.data(),
            errorTextOut))
        {
            return false;
        }
        nextOut = le32(entryBytes.data());
        return true;
    }

    // loadExFatClusterChain 作用：
    // - 读取 exFAT 目录簇链；
    // - noFatChain=true 时按连续簇读取，常见于 NoFatChain 标志目录。
    bool loadExFatClusterChain(
        const HANDLE volumeHandle,
        const ExFatBootInfo& infoValue,
        const std::uint32_t firstCluster,
        const std::uint64_t dataLength,
        const bool noFatChain,
        std::vector<std::uint32_t>& chainOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        chainOut.clear();
        if (firstCluster < 2 || firstCluster >= infoValue.clusterCount + 2ULL)
        {
            return false;
        }

        const std::uint64_t requestedClusters = dataLength == 0
            ? 1ULL
            : ((dataLength + infoValue.bytesPerCluster - 1ULL) / infoValue.bytesPerCluster);
        const std::uint64_t maxClusters = std::min<std::uint64_t>(std::max<std::uint64_t>(requestedClusters, 1ULL), 262144ULL);
        std::uint32_t currentCluster = firstCluster;
        for (std::uint64_t index = 0; index < maxClusters; ++index)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (currentCluster < 2 || currentCluster >= infoValue.clusterCount + 2ULL)
            {
                break;
            }
            chainOut.push_back(currentCluster);
            if (noFatChain)
            {
                currentCluster += 1U;
                continue;
            }

            std::uint32_t nextCluster = 0;
            if (!readExFatNextCluster(volumeHandle, infoValue, currentCluster, nextCluster, errorTextOut))
            {
                return false;
            }
            if (nextCluster >= 0xFFFFFFF8UL || nextCluster == 0 || nextCluster == currentCluster)
            {
                break;
            }
            currentCluster = nextCluster;
        }
        return !directoryCanceled(canceled) && !chainOut.empty();
    }

    // decodeExFatNamePart 作用：解析 exFAT 文件名二级目录项中的 UTF-16 名称片段。
    QString decodeExFatNamePart(const std::byte* entryPtr, const std::uint8_t maxChars)
    {
        QString textOut;
        const std::uint8_t charsToRead = std::min<std::uint8_t>(maxChars, 15U);
        for (std::uint8_t i = 0; i < charsToRead; ++i)
        {
            const char16_t ch = static_cast<char16_t>(le16(entryPtr + 2 + (i * 2)));
            if (ch == 0x0000 || ch == 0xFFFF)
            {
                break;
            }
            textOut.append(QChar(ch));
        }
        return textOut;
    }

    // enumerateExFatDirectoryByCluster 作用：解析 exFAT 目录簇链下的目录项。
    bool enumerateExFatDirectoryByCluster(
        const HANDLE volumeHandle,
        const ExFatBootInfo& infoValue,
        const std::uint32_t dirCluster,
        const std::uint64_t dirDataLength,
        const bool noFatChain,
        std::vector<ExFatEntry>& entriesOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        entriesOut.clear();
        std::vector<std::uint32_t> chainList;
        if (!loadExFatClusterChain(volumeHandle, infoValue, dirCluster, dirDataLength, noFatChain, chainList, errorTextOut, canceled))
        {
            return false;
        }

        std::vector<std::byte> clusterBytes(infoValue.bytesPerCluster);
        std::vector<std::byte> directoryBytes;
        directoryBytes.reserve(chainList.size() * static_cast<std::size_t>(infoValue.bytesPerCluster));
        for (std::uint32_t clusterValue : chainList)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (!readBytesAtOffset(
                volumeHandle,
                exFatClusterOffset(infoValue, clusterValue),
                infoValue.bytesPerCluster,
                clusterBytes.data(),
                errorTextOut))
            {
                return false;
            }
            directoryBytes.insert(directoryBytes.end(), clusterBytes.begin(), clusterBytes.end());
            if (dirDataLength > 0 && directoryBytes.size() >= dirDataLength)
            {
                break;
            }
        }

        for (std::size_t off = 0; off + 32 <= directoryBytes.size(); off += 32)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            const std::byte* entryPtr = directoryBytes.data() + off;
            const std::uint8_t entryType = static_cast<std::uint8_t>(entryPtr[0]);
            if (entryType == 0x00)
            {
                break;
            }
            if (entryType != 0x85)
            {
                continue;
            }

            const std::uint8_t secondaryCount = static_cast<std::uint8_t>(entryPtr[1]);
            if (secondaryCount == 0 || off + (static_cast<std::size_t>(secondaryCount) + 1ULL) * 32ULL > directoryBytes.size())
            {
                continue;
            }

            const std::uint16_t attributes = le16(entryPtr + 4);
            bool streamSeen = false;
            std::uint8_t nameLength = 0;
            std::uint32_t firstCluster = 0;
            std::uint64_t dataLength = 0;
            bool childNoFatChain = false;
            QString nameText;
            for (std::uint8_t subIndex = 1; subIndex <= secondaryCount; ++subIndex)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }
                const std::byte* secondaryPtr = directoryBytes.data() + off + static_cast<std::size_t>(subIndex) * 32ULL;
                const std::uint8_t secondaryType = static_cast<std::uint8_t>(secondaryPtr[0]);
                if (secondaryType == 0xC0)
                {
                    streamSeen = true;
                    childNoFatChain = (static_cast<std::uint8_t>(secondaryPtr[1]) & 0x02U) != 0;
                    nameLength = static_cast<std::uint8_t>(secondaryPtr[3]);
                    firstCluster = le32(secondaryPtr + 20);
                    dataLength = le64(secondaryPtr + 24);
                    continue;
                }
                if (secondaryType == 0xC1 && streamSeen)
                {
                    const std::uint8_t remainingChars = static_cast<std::uint8_t>(
                        nameLength > static_cast<std::uint8_t>(nameText.size())
                        ? (nameLength - static_cast<std::uint8_t>(nameText.size()))
                        : 0U);
                    nameText += decodeExFatNamePart(secondaryPtr, remainingChars);
                }
            }
            if (nameText.isEmpty())
            {
                continue;
            }

            ExFatEntry itemValue{};
            itemValue.name = nameText;
            itemValue.firstCluster = firstCluster;
            itemValue.sizeBytes = dataLength;
            itemValue.isDirectory = (attributes & 0x10U) != 0;
            itemValue.noFatChain = childNoFatChain;
            entriesOut.push_back(std::move(itemValue));
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        return true;
    }

    // resolveExFatDirectory 作用：按路径定位 exFAT 目标目录的簇号和目录流长度。
    bool resolveExFatDirectory(
        const HANDLE volumeHandle,
        const ExFatBootInfo& infoValue,
        const QStringList& pathSegments,
        std::uint32_t& clusterOut,
        std::uint64_t& dataLengthOut,
        bool& noFatChainOut,
        QString& errorTextOut,
        const DirectoryCancellationCallback& canceled = {})
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        clusterOut = infoValue.rootDirectoryCluster;
        dataLengthOut = 0;
        noFatChainOut = false;
        for (const QString& segmentText : pathSegments)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }
            std::vector<ExFatEntry> children;
            if (!enumerateExFatDirectoryByCluster(volumeHandle, infoValue, clusterOut, dataLengthOut, noFatChainOut, children, errorTextOut, canceled))
            {
                return false;
            }
            bool found = false;
            for (const ExFatEntry& childItem : children)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }
                if (!childItem.isDirectory)
                {
                    continue;
                }
                if (childItem.name.compare(segmentText, Qt::CaseInsensitive) == 0)
                {
                    clusterOut = childItem.firstCluster;
                    dataLengthOut = childItem.sizeBytes;
                    noFatChainOut = childItem.noFatChain;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                errorTextOut = QStringLiteral("exFAT目录不存在：%1").arg(segmentText);
                return false;
            }
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        return true;
    }
}

ks::file::ManualFsType ks::file::ManualFileSystemParser::detectFileSystemType(const QString& pathText)
{
    const QString volumeRoot = trimVolumeRoot(pathText);
    if (volumeRoot.isEmpty())
    {
        return ManualFsType::Unknown;
    }

    wchar_t fsName[MAX_PATH] = {};
    const std::wstring rootWide = toWide(volumeRoot);
    if (::GetVolumeInformationW(rootWide.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH) == FALSE)
    {
        return ManualFsType::Unknown;
    }

    const QString fsText = QString::fromWCharArray(fsName).trimmed().toUpper();
    if (fsText == QStringLiteral("NTFS"))
    {
        return ManualFsType::Ntfs;
    }
    if (fsText == QStringLiteral("FAT32"))
    {
        return ManualFsType::Fat32;
    }
    if (fsText == QStringLiteral("EXFAT"))
    {
        return ManualFsType::ExFat;
    }
    return ManualFsType::Unknown;
}

bool ks::file::ManualFileSystemParser::enumerateDirectory(
    const QString& pathText,
    std::vector<ManualDirectoryEntry>& entriesOut,
    ManualFsType& fsTypeOut,
    QString& errorTextOut,
    bool* usedWinApiFallbackOut,
    const ManualFsType requestedFsType,
    const bool strictMftOnly,
    const DirectoryCancellationCallback& canceled)
{
    entriesOut.clear();
    errorTextOut.clear();
    if (directoryCanceled(canceled))
    {
        return false;
    }

    if (usedWinApiFallbackOut != nullptr)
    {
        *usedWinApiFallbackOut = false;
    }
    fsTypeOut = requestedFsType == ManualFsType::Unknown
        ? detectFileSystemType(pathText)
        : requestedFsType;

    if (directoryCanceled(canceled))
    {
        return false;
    }
    if (fsTypeOut == ManualFsType::Ntfs)
    {
        const QString volumeRoot = trimVolumeRoot(pathText);
        std::vector<NtfsRawRecord> recordsValue;
        std::shared_ptr<const NtfsCacheEntry> cacheSnapshot;
        constexpr std::uint64_t DirectoryRetryMaxRecords = 1200000ULL; // 定位目录失败时扩展扫描上限，避免漏掉高编号目录项。
        // 严格 MFT 模式同样先用快速窗口起步。
        // 一上来就按 120 万条扫描看似"更完整"，实际代价无法接受：
        // 该模式的缓存键与普通模式不同（allowFsctlFallback=false），永远不会命中
        // 普通模式留下的缓存，于是每次切到这个模式都要全量重扫一遍；
        // 120 万条记录在解析后要占数百 MB 内存，扫描期间下拉框一直是禁用状态，
        // 表现出来就是"一选这个模式界面就卡住"。
        // 快速窗口定位不到目标目录时，下面的重试段会自动扩到 120 万条，
        // 因此完整性并不依赖这里的初始值。
        const std::uint64_t DirectoryListMaxRecords = 250000ULL;
        // allowFsctlFallback 在严格模式下必须关闭：FSCTL_GET_NTFS_FILE_RECORD 由
        // 文件系统驱动应答，会经过整条过滤链，正是本模式要绕开的路径。
        if (!loadNtfsRecords(volumeRoot, recordsValue, errorTextOut, DirectoryListMaxRecords, !strictMftOnly, true, false, false, false, NtfsRecordKeepPolicy::All, {}, &cacheSnapshot, canceled))
        {
            return false;
        }

        const QStringList pathSegments = splitRelativeSegments(pathText);
        std::uint64_t dirIndex = 5;
        bool usedFullRangeScan = false; // usedFullRangeScan：标记本次是否已经执行过 120 万记录扩大扫描。
        bool resolveOk = (cacheSnapshot != nullptr)
            && resolveNtfsDirectoryIndex(*cacheSnapshot, pathSegments, dirIndex, canceled);
        // 快速 MFT 窗口可能不包含目标目录本身；直接从目录句柄取得真实文件引用号后继续纯 NTFS 枚举。
        // 该兜底会打开目录句柄，属于 Windows API 路径，严格 MFT 模式下必须跳过。
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (!resolveOk && !strictMftOnly)
        {
            std::uint64_t pathFileReference = 0;
            QString referenceErrorText;
            if (queryNtfsFileReferenceByPath(
                pathText,
                pathFileReference,
                referenceErrorText))
            {
                dirIndex = pathFileReference;
                resolveOk = true;
                errorTextOut.clear();

                kLogEvent event;
                info << event
                    << "[FileDock] 目标目录超出快速MFT窗口，已通过目录文件引用继续枚举, path="
                    << QDir::toNativeSeparators(
                        QDir::cleanPath(pathText)).toStdString()
                    << ", fileReference="
                    << static_cast<qulonglong>(dirIndex)
                    << eol;
            }
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (!resolveOk && strictMftOnly)
        {
            // 严格模式没有 WinAPI 兜底可用，只剩"扩大扫描窗口"这一条路：
            // 目标目录的 MFT 记录号可能大于快速窗口。这里就地重试一次全量扫描，
            // 而不是把重试留到后面的"结果为空"分支——那个分支要求先定位成功，
            // 严格模式定位失败时根本走不到。
            std::vector<NtfsRawRecord> retryRecords;
            QString retryErrorText;
            std::shared_ptr<const NtfsCacheEntry> retrySnapshot;
            if (loadNtfsRecords(volumeRoot, retryRecords, retryErrorText, DirectoryRetryMaxRecords, false, true, false, false, false, NtfsRecordKeepPolicy::All, {}, &retrySnapshot, canceled))
            {
                std::uint64_t retryDirIndex = 5;
                if (retrySnapshot != nullptr &&
                    resolveNtfsDirectoryIndex(*retrySnapshot, pathSegments, retryDirIndex, canceled))
                {
                    recordsValue.swap(retryRecords);
                    cacheSnapshot = retrySnapshot;
                    dirIndex = retryDirIndex;
                    resolveOk = true;
                    usedFullRangeScan = true;
                    errorTextOut.clear();

                    kLogEvent event;
                    info << event
                        << "[FileDock] 纯MFT解析扩大扫描后定位到目录, path="
                        << QDir::toNativeSeparators(QDir::cleanPath(pathText)).toStdString()
                        << ", fileReference="
                        << static_cast<qulonglong>(dirIndex)
                        << eol;
                }
            }
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (!resolveOk && strictMftOnly)
        {
            // 扩大扫描后仍定位不到：宁可报错也不回退。返回一份掺了 WinAPI 行的
            // 列表会让"MFT 独有条目"的判定彻底失效。
            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (errorTextOut.isEmpty())
            {
                errorTextOut = QStringLiteral(
                    "纯MFT解析未能在 $MFT 中定位该目录（扫描窗口不足或目录记录已损坏）。");
            }
            return false;
        }
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (!resolveOk)
        {
            // 兜底策略：目录真实存在但 MFT 链路解析失败时，回退到 WinAPI 枚举，
            // 避免手动模式直接“空白/报错不可访问”。
            if (enumerateDirectoryByWinApi(pathText, entriesOut, canceled))
            {
                if (usedWinApiFallbackOut != nullptr)
                {
                    *usedWinApiFallbackOut = true;
                }

                kLogEvent event;
                warn << event
                    << "[FileDock] NTFS链路定位失败，回退WinAPI枚举, path="
                    << QDir::toNativeSeparators(QDir::cleanPath(pathText)).toStdString()
                    << ", rows="
                    << entriesOut.size()
                    << eol;
                errorTextOut.clear();
                return true;
            }

            if (directoryCanceled(canceled))
            {
                return false;
            }
            if (errorTextOut.isEmpty())
            {
                errorTextOut = QStringLiteral("NTFS目录不存在或不可访问。");
            }
            return false;
        }

        const QString currentPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
        auto appendEntriesByDirectoryIndex =
            [&entriesOut, &currentPath, &cacheSnapshot, &canceled](const std::uint64_t targetDirectoryIndex)
            {
                entriesOut.clear();
                if (cacheSnapshot == nullptr)
                {
                    return false;
                }

                const auto childRange = findNtfsDirectoryLinkRange(cacheSnapshot->directoryLinks, targetDirectoryIndex);
                for (auto it = childRange.first; it != childRange.second; ++it)
                {
                    if (directoryCanceled(canceled))
                    {
                        return false;
                    }

                    const auto recordIt = cacheSnapshot->recordOffsetByIndex.find(it->recordIndex);
                    if (recordIt == cacheSnapshot->recordOffsetByIndex.end())
                    {
                        continue;
                    }

                    const NtfsRawRecord& recordValue = cacheSnapshot->records[recordIt->second];
                    if (!recordValue.inUse || it->fileName.isEmpty())
                    {
                        continue;
                    }

                    ManualDirectoryEntry itemValue{};
                    itemValue.name = it->fileName;
                    itemValue.absolutePath = QDir(currentPath).filePath(it->fileName);
                    itemValue.isDirectory = recordValue.isDirectory;
                    itemValue.sizeBytes = recordValue.isDirectory ? 0 : recordValue.sizeBytes;
                    itemValue.modifiedTime = fileTimeToLocal(recordValue.modifiedTime100ns);
                    itemValue.typeText = buildTypeText(it->fileName, recordValue.isDirectory);
                    itemValue.ntfsFileReference = recordValue.recordIndex;
                    entriesOut.push_back(std::move(itemValue));
                }
                return !directoryCanceled(canceled);
            };

        if (!appendEntriesByDirectoryIndex(dirIndex))
        {
            return false;
        }

        // WinAPI 结果只作为完整性对照：发现缺项时先用 FSCTL_ENUM_USN_DATA 定向补齐高编号 MFT 记录。
        // 只有 NTFS 定向枚举仍无法取回的残余项目，才允许复制 WinAPI 行并标记真实回退来源。
        // 严格 MFT 模式完全跳过本段：无论是 USN 定向补齐还是 WinAPI 合并，
        // 都会把"文件系统愿意让你看到的条目"混进纯 MFT 视图，隐藏项检测随之失效。
        std::vector<ManualDirectoryEntry> winApiEntries;
        if (!strictMftOnly && enumerateDirectoryByWinApi(pathText, winApiEntries, canceled))
        {
            const auto buildExistingNameSet =
                [&entriesOut, &canceled]()
                {
                    QSet<QString> nameSet;
                    nameSet.reserve(
                        static_cast<int>(
                            entriesOut.size() * 2ULL + 16ULL));
                    for (const ManualDirectoryEntry& itemValue : entriesOut)
                    {
                        if (directoryCanceled(canceled))
                        {
                            return nameSet;
                        }
                        nameSet.insert(itemValue.name.toCaseFolded());
                    }
                    return nameSet;
                };

            QSet<QString> existingNameSet =
                buildExistingNameSet();
            if (directoryCanceled(canceled))
            {
                return false;
            }

            std::size_t missingBeforeMftCount = 0;
            for (const ManualDirectoryEntry& winApiItem : winApiEntries)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }

                if (!existingNameSet.contains(
                    winApiItem.name.toCaseFolded()))
                {
                    missingBeforeMftCount += 1;
                }
            }

            if (missingBeforeMftCount > 0)
            {
                std::size_t mftAddedCount = 0;
                QString mftEnumerationErrorText;
                const bool mftEnumerationOk =
                    supplementNtfsDirectoryEntriesByMftEnumeration(
                        volumeRoot,
                        currentPath,
                        dirIndex,
                        entriesOut,
                        mftAddedCount,
                        mftEnumerationErrorText,
                        canceled);
                if (directoryCanceled(canceled))
                {
                    return false;
                }

                if (mftEnumerationOk)
                {
                    existingNameSet = buildExistingNameSet();
                    if (directoryCanceled(canceled))
                    {
                        return false;
                    }

                    kLogEvent event;
                    info << event
                        << "[FileDock] NTFS定向MFT枚举完成, path="
                        << currentPath.toStdString()
                        << ", missingBefore="
                        << static_cast<qulonglong>(
                            missingBeforeMftCount)
                        << ", mftAdded="
                        << static_cast<qulonglong>(mftAddedCount)
                        << eol;
                }
                else
                {
                    kLogEvent event;
                    warn << event
                        << "[FileDock] NTFS定向MFT枚举失败，保留WinAPI最终兜底, path="
                        << currentPath.toStdString()
                        << ", error="
                        << mftEnumerationErrorText.toStdString()
                        << eol;
                }
            }

            std::size_t mergedCount = 0;
            for (const ManualDirectoryEntry& fallbackItem : winApiEntries)
            {
                if (directoryCanceled(canceled))
                {
                    return false;
                }

                const QString normalizedName =
                    fallbackItem.name.toCaseFolded();
                if (existingNameSet.contains(normalizedName))
                {
                    continue;
                }

                existingNameSet.insert(normalizedName);
                entriesOut.push_back(fallbackItem);
                mergedCount += 1;
            }

            if (mergedCount > 0)
            {
                if (usedWinApiFallbackOut != nullptr)
                {
                    *usedWinApiFallbackOut = true;
                }

                kLogEvent event;
                warn << event
                    << "[FileDock] NTFS定向MFT枚举后仍有缺项，已使用WinAPI兜底, path="
                    << QDir::toNativeSeparators(
                        QDir::cleanPath(pathText)).toStdString()
                    << ", mftRows="
                    << static_cast<qulonglong>(
                        entriesOut.size() - mergedCount)
                    << ", fallbackRows="
                    << static_cast<qulonglong>(mergedCount)
                    << ", winApiRows="
                    << static_cast<qulonglong>(winApiEntries.size())
                    << eol;
            }
        }

        // 当目录已定位且手动路径返回空时，才执行一次扩大扫描，尽量兼顾“速度优先”和“极端目录可用性”。
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (entriesOut.empty() && !usedFullRangeScan && DirectoryListMaxRecords < DirectoryRetryMaxRecords)
        {
            std::vector<NtfsRawRecord> retryRecords;
            QString retryErrorText;
            std::shared_ptr<const NtfsCacheEntry> retrySnapshot;
            if (loadNtfsRecords(volumeRoot, retryRecords, retryErrorText, DirectoryRetryMaxRecords, !strictMftOnly, true, false, false, false, NtfsRecordKeepPolicy::All, {}, &retrySnapshot, canceled))
            {
                std::uint64_t retryDirIndex = 5;
                if (retrySnapshot != nullptr && resolveNtfsDirectoryIndex(*retrySnapshot, pathSegments, retryDirIndex, canceled))
                {
                    recordsValue.swap(retryRecords);
                    cacheSnapshot = retrySnapshot;
                    dirIndex = retryDirIndex;
                    if (!appendEntriesByDirectoryIndex(dirIndex))
                    {
                        return false;
                    }
                }
            }
        }
    }
    else if (fsTypeOut == ManualFsType::Fat32)
    {
        const QString volumeRoot = trimVolumeRoot(pathText);
        QString openErrorText;
        DirectoryReadHandle volumeHandle(openReadHandle(buildVolumeDevicePath(volumeRoot), openErrorText));
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openErrorText;
            return false;
        }

        Fat32BootInfo bootInfo{};
        if (!readFat32BootInfo(volumeHandle, bootInfo, errorTextOut))
        {
            volumeHandle.close();
            return false;
        }

        std::uint32_t dirCluster = bootInfo.rootCluster;
        const QStringList pathSegments = splitRelativeSegments(pathText);
        if (!resolveFatDirectoryCluster(volumeHandle, bootInfo, pathSegments, dirCluster, errorTextOut, canceled))
        {
            volumeHandle.close();
            return false;
        }

        std::vector<Fat32Entry> fatEntries;
        if (!enumerateFatDirectoryByCluster(volumeHandle, bootInfo, dirCluster, fatEntries, errorTextOut, canceled))
        {
            volumeHandle.close();
            return false;
        }
        volumeHandle.close();

        const QString currentPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
        for (const Fat32Entry& fatItem : fatEntries)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            ManualDirectoryEntry itemValue{};
            itemValue.name = fatItem.name;
            itemValue.absolutePath = QDir(currentPath).filePath(fatItem.name);
            itemValue.isDirectory = fatItem.isDirectory;
            itemValue.sizeBytes = fatItem.isDirectory ? 0 : fatItem.sizeBytes;
            itemValue.modifiedTime = fatItem.modifiedTime;
            itemValue.typeText = buildTypeText(fatItem.name, fatItem.isDirectory);
            entriesOut.push_back(std::move(itemValue));
        }
    }
    else if (fsTypeOut == ManualFsType::ExFat)
    {
        const QString volumeRoot = trimVolumeRoot(pathText);
        QString openErrorText;
        DirectoryReadHandle volumeHandle(openReadHandle(buildVolumeDevicePath(volumeRoot), openErrorText));
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openErrorText;
            return false;
        }

        ExFatBootInfo bootInfo{};
        if (!readExFatBootInfo(volumeHandle, bootInfo, errorTextOut))
        {
            volumeHandle.close();
            return false;
        }

        std::uint32_t dirCluster = bootInfo.rootDirectoryCluster;
        std::uint64_t dirDataLength = 0;
        bool dirNoFatChain = false;
        const QStringList pathSegments = splitRelativeSegments(pathText);
        if (!resolveExFatDirectory(volumeHandle, bootInfo, pathSegments, dirCluster, dirDataLength, dirNoFatChain, errorTextOut, canceled))
        {
            volumeHandle.close();
            return false;
        }

        std::vector<ExFatEntry> exFatEntries;
        if (!enumerateExFatDirectoryByCluster(volumeHandle, bootInfo, dirCluster, dirDataLength, dirNoFatChain, exFatEntries, errorTextOut, canceled))
        {
            volumeHandle.close();
            return false;
        }
        volumeHandle.close();

        const QString currentPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
        for (const ExFatEntry& exFatItem : exFatEntries)
        {
            if (directoryCanceled(canceled))
            {
                return false;
            }

            ManualDirectoryEntry itemValue{};
            itemValue.name = exFatItem.name;
            itemValue.absolutePath = QDir(currentPath).filePath(exFatItem.name);
            itemValue.isDirectory = exFatItem.isDirectory;
            itemValue.sizeBytes = exFatItem.isDirectory ? 0 : exFatItem.sizeBytes;
            itemValue.typeText = buildTypeText(exFatItem.name, exFatItem.isDirectory);
            entriesOut.push_back(std::move(itemValue));
        }
    }
    else
    {
        errorTextOut = QStringLiteral("当前卷不是 NTFS/FAT32/exFAT，无法手动解析。");
        return false;
    }

    if (entriesOut.empty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 手动解析结果为空, path="
            << QDir::toNativeSeparators(pathText).toStdString()
            << ", fsType="
            << (fsTypeOut == ManualFsType::Ntfs
                ? "NTFS"
                : (fsTypeOut == ManualFsType::Fat32 ? "FAT32" : (fsTypeOut == ManualFsType::ExFat ? "exFAT" : "Unknown")))
            << eol;
    }

    return sortDirectoryValues(
        entriesOut,
        [](const ManualDirectoryEntry& left, const ManualDirectoryEntry& right) {
            if (left.isDirectory != right.isDirectory)
            {
                return left.isDirectory && !right.isDirectory;
            }
            return QString::compare(left.name, right.name, Qt::CaseInsensitive) < 0;
        },
        canceled);
}

bool ks::file::ManualFileSystemParser::enumerateDirectoryByMft(
    const QString& pathText,
    std::vector<ManualDirectoryEntry>& entriesOut,
    QString& errorTextOut,
    MftScanDiagnostics* diagnosticsOut,
    const DirectoryCancellationCallback& canceled)
{
    entriesOut.clear();
    errorTextOut.clear();
    if (diagnosticsOut != nullptr)
    {
        *diagnosticsOut = MftScanDiagnostics{};
    }
    if (directoryCanceled(canceled))
    {
        return false;
    }

    const ManualFsType detectedType = detectFileSystemType(pathText);
    if (directoryCanceled(canceled))
    {
        return false;
    }
    if (detectedType != ManualFsType::Ntfs)
    {
        errorTextOut = QStringLiteral(
            "纯MFT解析仅适用于 NTFS 卷，当前卷类型为 %1。")
            .arg(detectedType == ManualFsType::Fat32
                ? QStringLiteral("FAT32")
                : (detectedType == ManualFsType::ExFat
                    ? QStringLiteral("exFAT")
                    : QStringLiteral("未知")));
        return false;
    }

    ManualFsType resolvedType = ManualFsType::Ntfs;
    bool usedWinApiFallback = false;
    // strictMftOnly=true：结果必须完全来自卷偏移直读的 $MFT 字节。
    if (!enumerateDirectory(
            pathText,
            entriesOut,
            resolvedType,
            errorTextOut,
            &usedWinApiFallback,
            ManualFsType::Ntfs,
            true,
            canceled))
    {
        return false;
    }

    if (directoryCanceled(canceled))
    {
        return false;
    }
    if (diagnosticsOut == nullptr)
    {
        return true;
    }
    MftScanDiagnostics diagnostics;
    diagnostics.mftEntryCount = static_cast<int>(entriesOut.size());

    /*
     * 对照视图只用于比较，绝不并入结果。差集的两个方向含义不同：
     * - mftOnly：$MFT 里有、目录枚举看不到，是过滤层/目录索引隐藏文件的典型特征；
     * - winApiOnly：目录枚举看得到、$MFT 扫描窗口内没有，通常是扫描上限或
     *   扫描期间目录变化造成的，不能当作异常证据。
     */
    std::vector<ManualDirectoryEntry> winApiEntries;
    if (!enumerateDirectoryByWinApi(pathText, winApiEntries, canceled))
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        *diagnosticsOut = std::move(diagnostics);
        return true;
    }
    if (directoryCanceled(canceled))
    {
        return false;
    }
    diagnostics.comparisonAvailable = true;
    diagnostics.winApiEntryCount = static_cast<int>(winApiEntries.size());

    QSet<QString> mftNameSet;
    mftNameSet.reserve(static_cast<int>(entriesOut.size()) + 16);
    for (const ManualDirectoryEntry& itemValue : entriesOut)
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        mftNameSet.insert(itemValue.name.toCaseFolded());
    }
    QSet<QString> winApiNameSet;
    winApiNameSet.reserve(static_cast<int>(winApiEntries.size()) + 16);
    for (const ManualDirectoryEntry& itemValue : winApiEntries)
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        winApiNameSet.insert(itemValue.name.toCaseFolded());
    }

    for (const ManualDirectoryEntry& itemValue : entriesOut)
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (!winApiNameSet.contains(itemValue.name.toCaseFolded()))
        {
            diagnostics.mftOnlyNames.append(itemValue.name);
        }
    }
    for (const ManualDirectoryEntry& itemValue : winApiEntries)
    {
        if (directoryCanceled(canceled))
        {
            return false;
        }
        if (!mftNameSet.contains(itemValue.name.toCaseFolded()))
        {
            diagnostics.winApiOnlyNames.append(itemValue.name);
        }
    }

    if (!diagnostics.mftOnlyNames.isEmpty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 纯MFT解析发现目录枚举不可见的条目, path="
            << QDir::toNativeSeparators(QDir::cleanPath(pathText)).toStdString()
            << ", mftRows="
            << diagnostics.mftEntryCount
            << ", winApiRows="
            << diagnostics.winApiEntryCount
            << ", mftOnly="
            << diagnostics.mftOnlyNames.size()
            << eol;
    }
    if (directoryCanceled(canceled))
    {
        return false;
    }
    *diagnosticsOut = std::move(diagnostics);
    return true;
}

bool ks::file::ManualFileSystemParser::enumerateNtfsDeletedFiles(
    const QString& volumeRootPath,
    std::vector<NtfsDeletedFileEntry>& deletedOut,
    QString& errorTextOut,
    const std::function<void(int, const QString&)>& progressCallback)
{
    deletedOut.clear();
    errorTextOut.clear();
    if (progressCallback)
    {
        progressCallback(1, QStringLiteral("准备误删扫描"));
    }
    if (detectFileSystemType(volumeRootPath) != ManualFsType::Ntfs)
    {
        errorTextOut = QStringLiteral("仅 NTFS 卷支持误删扫描。");
        return false;
    }

    std::vector<NtfsRawRecord> recordsValue;
    const QString volumeRoot = trimVolumeRoot(volumeRootPath);
    // 0 表示不额外设限：必须覆盖 $MFT 全部有效记录。
    // NTFS 会优先复用低号空闲记录，"已删除且未被复用"的记录几乎全在 MFT 尾部；
    // 之前固定扫前 150 万条，在大卷上扫到的全是在用记录，结果稳定为 0 项。
    constexpr std::uint64_t DeletedScanMaxRecords = 0ULL;
    // 删除恢复优先保留 deleted 记录，因此这里禁用 FSCTL 路径，改走 $MFT/卷偏移扫描。
    if (!loadNtfsRecords(
        volumeRoot,
        recordsValue,
        errorTextOut,
        DeletedScanMaxRecords,
        false,
        false,
        true,
        false,
        true,
        NtfsRecordKeepPolicy::DeletedAndDirectories,
        progressCallback,
        nullptr))
    {
        return false;
    }
    if (progressCallback)
    {
        progressCallback(82, QStringLiteral("MFT 扫描完成，开始过滤删除项"));
    }

    // bitmapSnapshot：卷位图快照仅用于完整度估算，加载失败时不影响扫描主流程。
    NtfsVolumeBitmapSnapshot bitmapSnapshot{};
    const NtfsVolumeBitmapSnapshot* bitmapSnapshotPtr = nullptr;
    QString bitmapErrorText;
    if (loadNtfsVolumeBitmapSnapshot(volumeRoot, bitmapSnapshot, bitmapErrorText))
    {
        bitmapSnapshotPtr = &bitmapSnapshot;
        if (progressCallback)
        {
            progressCallback(86, QStringLiteral("已读取卷位图，开始估算完整度"));
        }
        // 成功但带告警说明位图只覆盖了卷的前一段，超出部分的完整度会保持“未知”。
        if (!bitmapErrorText.isEmpty())
        {
            kLogEvent event;
            warn << event
                << "[FileDock] 误删扫描仅取到部分卷位图, volume="
                << volumeRoot.toStdString()
                << ", detail="
                << bitmapErrorText.toStdString()
                << eol;
        }
    }
    else if (!bitmapErrorText.isEmpty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 误删扫描未能加载卷位图，完整度估算将退化为未知, volume="
            << volumeRoot.toStdString()
            << ", error="
            << bitmapErrorText.toStdString()
            << eol;
    }

    std::unordered_map<std::uint64_t, const NtfsRawRecord*> recordMap;
    recordMap.reserve(recordsValue.size());
    for (const NtfsRawRecord& recordValue : recordsValue)
    {
        recordMap.emplace(recordValue.recordIndex, &recordValue);
    }

    // emittedKeySet：避免同一记录的同一路径提示被重复加入结果。
    QSet<QString> emittedKeySet;

    // 恢复扫描结果上限：
    // 1) 目录级表格仍使用 QTableWidget，过大结果会显著拖慢界面；
    // 2) 这里先将上限提升到 8 万，兼顾覆盖率与 UI 可承受范围。
    constexpr std::size_t MaxDeletedRecords = 80000;
    std::size_t scannedDeletedCandidateCount = 0; // scannedDeletedCandidateCount：已评估的删除候选记录数。
    for (const NtfsRawRecord& recordValue : recordsValue)
    {
        if (recordValue.inUse || recordValue.isDirectory)
        {
            continue;
        }
        scannedDeletedCandidateCount += 1;
        if (progressCallback
            && ((scannedDeletedCandidateCount % 2048U) == 0))
        {
            const int percentValue = 86
                + static_cast<int>((scannedDeletedCandidateCount * 10ULL)
                    / std::max<std::size_t>(recordsValue.size(), static_cast<std::size_t>(1)));
            progressCallback(std::min(percentValue, 96), QStringLiteral("过滤删除项并估算完整度"));
        }

        auto appendDeletedItem =
            [&deletedOut, &emittedKeySet, &recordValue, &recordMap, &volumeRoot, bitmapSnapshotPtr](
                const QString& fileNameValue,
                const std::uint64_t parentIndexValue,
                const bool hasOriginalName)
            {
                const QString normalizedFileName = fileNameValue.trimmed();
                if (normalizedFileName.isEmpty())
                {
                    return;
                }

                const QString dedupeKey = QStringLiteral("%1|%2|%3")
                    .arg(static_cast<qulonglong>(recordValue.recordIndex))
                    .arg(static_cast<qulonglong>(parentIndexValue))
                    .arg(normalizedFileName.toCaseFolded());
                if (emittedKeySet.contains(dedupeKey))
                {
                    return;
                }
                emittedKeySet.insert(dedupeKey);

                NtfsDeletedFileEntry itemValue{};
                itemValue.fileName = normalizedFileName;
                itemValue.pathHint = buildNtfsPathHintByName(volumeRoot, normalizedFileName, parentIndexValue, recordMap);
                itemValue.sizeBytes = recordValue.sizeBytes;
                itemValue.modifiedTime = fileTimeToLocal(recordValue.modifiedTime100ns);
                itemValue.fileReference = recordValue.recordIndex;
                itemValue.sequenceNumber = recordValue.sequenceNumber;
                itemValue.estimatedIntegrityPercent =
                    estimateDeletedRecordIntegrityPercent(recordValue, bitmapSnapshotPtr);
                itemValue.hasOriginalName = hasOriginalName;
                itemValue.residentDataReady = recordValue.residentReady;
                itemValue.recoveryCapability = deletedRecordRecoveryCapability(
                    recordValue,
                    itemValue.estimatedIntegrityPercent);
                deletedOut.push_back(std::move(itemValue));
            };

        if (!recordValue.nameLinks.empty())
        {
            for (const NtfsNameLink& nameLink : recordValue.nameLinks)
            {
                appendDeletedItem(nameLink.fileName, nameLink.parentIndex, true);
                if (deletedOut.size() >= MaxDeletedRecords)
                {
                    break;
                }
            }
        }
        else
        {
            if (!recordValue.fileName.isEmpty())
            {
                appendDeletedItem(recordValue.fileName, recordValue.parentIndex, true);
            }
            else if (recordValue.hasPrimaryDataStream || recordValue.sizeBytes > 0 || recordValue.residentReady)
            {
                appendDeletedItem(buildSyntheticDeletedFileName(recordValue), recordValue.parentIndex, false);
            }
        }

        if (deletedOut.size() >= MaxDeletedRecords)
        {
            break;
        }
    }

    // 触顶必须显式告知：修复 $MFT 映射后覆盖率大幅提高，静默截断会让用户
    // 误以为“这就是全部删除项”，从而漏掉真正想找回的文件。
    const bool truncatedByDisplayLimit = (deletedOut.size() >= MaxDeletedRecords);
    if (truncatedByDisplayLimit)
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 误删扫描结果已达显示上限，结果被截断, volume="
            << volumeRoot.toStdString()
            << ", limit="
            << MaxDeletedRecords
            << eol;
    }

    std::sort(
        deletedOut.begin(),
        deletedOut.end(),
        [](const NtfsDeletedFileEntry& left, const NtfsDeletedFileEntry& right) {
            // 完整度优先：
            // 1) 先排列可安全恢复的驻留/非驻留项；
            // 2) 再按已知完整度降序；
            // 3) 最后优先原始文件名、时间与大小。
            const auto capabilityRank = [](const NtfsRecoveryCapability capability) -> int {
                switch (capability)
                {
                case NtfsRecoveryCapability::Resident:
                    return 4;
                case NtfsRecoveryCapability::NonResidentIntact:
                    return 3;
                case NtfsRecoveryCapability::NonResidentAtRisk:
                    return 2;
                case NtfsRecoveryCapability::UnsupportedStream:
                    return 1;
                case NtfsRecoveryCapability::MetadataOnly:
                default:
                    return 0;
                }
            };
            const int leftCapabilityRank = capabilityRank(left.recoveryCapability);
            const int rightCapabilityRank = capabilityRank(right.recoveryCapability);
            if (leftCapabilityRank != rightCapabilityRank)
            {
                return leftCapabilityRank > rightCapabilityRank;
            }
            const bool leftIntegrityKnown = (left.estimatedIntegrityPercent >= 0);
            const bool rightIntegrityKnown = (right.estimatedIntegrityPercent >= 0);
            if (leftIntegrityKnown != rightIntegrityKnown)
            {
                return leftIntegrityKnown;
            }
            if (leftIntegrityKnown
                && rightIntegrityKnown
                && left.estimatedIntegrityPercent != right.estimatedIntegrityPercent)
            {
                return left.estimatedIntegrityPercent > right.estimatedIntegrityPercent;
            }
            if (left.hasOriginalName != right.hasOriginalName)
            {
                return left.hasOriginalName;
            }
            if (left.residentDataReady != right.residentDataReady)
            {
                return left.residentDataReady;
            }
            if (left.modifiedTime.isValid() && right.modifiedTime.isValid())
            {
                return left.modifiedTime > right.modifiedTime;
            }
            if (left.modifiedTime.isValid() != right.modifiedTime.isValid())
            {
                return left.modifiedTime.isValid();
            }
            if (left.sizeBytes != right.sizeBytes)
            {
                return left.sizeBytes > right.sizeBytes;
            }
            return QString::compare(left.fileName, right.fileName, Qt::CaseInsensitive) < 0;
        });
    if (progressCallback)
    {
        progressCallback(
            100,
            truncatedByDisplayLimit
            ? QStringLiteral("删除项排序完成（已达显示上限，结果被截断）")
            : QStringLiteral("删除项排序完成"));
    }
    return true;
}

bool ks::file::ManualFileSystemParser::recoverNtfsDeletedFile(
    const QString& volumeRootPath,
    const NtfsDeletedFileEntry& deletedEntry,
    const QString& targetFilePath,
    QString& errorTextOut,
    const std::function<void(int, const QString&)>& progressCallback)
{
    errorTextOut.clear();
    const auto reportProgress =
        [&progressCallback](const int percentValue, const QString& stageText)
        {
            if (progressCallback)
            {
                progressCallback(std::clamp(percentValue, 0, 100), stageText);
            }
        };
    reportProgress(1, QStringLiteral("正在校验恢复参数"));

    // 只接受扫描阶段已经证明安全的两类候选。
    // 尤其不能让“曾检测到簇已复用”的条目在占用者随后删除后重新变成可恢复，
    // 因为当前空闲并不能证明簇内容仍属于原文件。
    if (deletedEntry.recoveryCapability !=
            NtfsRecoveryCapability::Resident &&
        deletedEntry.recoveryCapability !=
            NtfsRecoveryCapability::NonResidentIntact)
    {
        errorTextOut = QStringLiteral(
            "选中项均不满足安全恢复条件；请查看“恢复能力”和“完整度”列。");
        return false;
    }

    const QString volumeRoot = trimVolumeRoot(volumeRootPath);
    if (volumeRoot.isEmpty())
    {
        errorTextOut = QStringLiteral("卷根路径无效。");
        return false;
    }
    const QString normalizedTargetPath =
        QDir::cleanPath(targetFilePath.trimmed());
    if (normalizedTargetPath.isEmpty() ||
        !QDir::isAbsolutePath(normalizedTargetPath))
    {
        errorTextOut = QStringLiteral("目标文件路径为空或不是绝对路径。");
        return false;
    }
    if (QFileInfo::exists(normalizedTargetPath))
    {
        errorTextOut = QStringLiteral("目标文件已存在，恢复器不会覆盖现有文件：%1")
            .arg(QDir::toNativeSeparators(normalizedTargetPath));
        return false;
    }
    const QFileInfo targetInfo(normalizedTargetPath);
    const QDir targetDirectory = targetInfo.dir();
    if (!targetDirectory.exists())
    {
        errorTextOut = QStringLiteral("目标目录不存在：%1")
            .arg(QDir::toNativeSeparators(targetDirectory.absolutePath()));
        return false;
    }

    // 恢复前必须按记录号重新读取 MFT，避免依赖数分钟前的扫描快照。
    reportProgress(6, QStringLiteral("正在重新读取 MFT 记录"));
    NtfsRawRecord recordValue{};
    QString readRecordErrorText;
    if (!loadNtfsSingleRecord(
        volumeRoot,
        deletedEntry.fileReference,
        recordValue,
        readRecordErrorText))
    {
        errorTextOut = QStringLiteral("恢复前回读 MFT 记录失败：%1")
            .arg(readRecordErrorText);
        return false;
    }
    if (recordValue.inUse)
    {
        errorTextOut = QStringLiteral("该 MFT 记录已重新被占用，无法安全恢复。");
        return false;
    }
    if (deletedEntry.sequenceNumber != 0 &&
        recordValue.sequenceNumber != deletedEntry.sequenceNumber)
    {
        errorTextOut = QStringLiteral(
            "MFT 序列号已变化（扫描时 %1，当前 %2），记录可能被复用。")
            .arg(deletedEntry.sequenceNumber)
            .arg(recordValue.sequenceNumber);
        return false;
    }
    if (recordValue.sizeBytes != deletedEntry.sizeBytes)
    {
        errorTextOut = QStringLiteral(
            "MFT 数据长度已变化（扫描时 %1，当前 %2），已停止恢复。")
            .arg(static_cast<qulonglong>(deletedEntry.sizeBytes))
            .arg(static_cast<qulonglong>(recordValue.sizeBytes));
        return false;
    }
    if (!recordValue.hasPrimaryDataStream)
    {
        errorTextOut = QStringLiteral("该记录当前没有可恢复的未命名主数据流。");
        return false;
    }
    const bool expectedNonResident =
        deletedEntry.recoveryCapability ==
        NtfsRecoveryCapability::NonResidentIntact;
    if (recordValue.nonResidentData != expectedNonResident)
    {
        errorTextOut = expectedNonResident
            ? QStringLiteral("该记录当前没有可恢复的未命名主数据流。")
            : QStringLiteral("该记录当前已不是 resident 主数据流。");
        return false;
    }

    // 非驻留恢复在创建任何输出临时文件前先做位置和布局预检。
    // 否则同卷输出临时文件自身的簇分配就可能覆盖待恢复数据。
    if (recordValue.nonResidentData)
    {
        const QString sourceVolumeIdentity =
            queryExistingPathVolumeIdentity(volumeRoot);
        const QString targetVolumeIdentity =
            queryExistingPathVolumeIdentity(targetDirectory.absolutePath());
        if (sourceVolumeIdentity.isEmpty() ||
            targetVolumeIdentity.isEmpty())
        {
            errorTextOut = QStringLiteral(
                "无法确认源卷或目标目录的真实卷身份，已停止非驻留恢复。");
            return false;
        }
        if (sourceVolumeIdentity.compare(
                targetVolumeIdentity,
                Qt::CaseInsensitive) == 0)
        {
            errorTextOut = QStringLiteral(
                "非驻留文件不能恢复到源卷 %1，请选择其它卷或网络目录，避免输出文件覆盖待恢复簇。")
                .arg(volumeRoot);
            return false;
        }
        if (recordValue.unsupportedDataStream ||
            recordValue.hasAttributeList ||
            recordValue.dataRuns.empty())
        {
            errorTextOut = QStringLiteral(
                "该非驻留流使用压缩、加密或跨记录属性布局，当前无法安全重建。");
            return false;
        }
    }

    // 临时文件固定创建在目标目录；QTemporaryFile::rename 只执行底层原子
    // rename，不会像 QFile 那样回退为 copy/delete，也不会覆盖已存在目标。
    QTemporaryFile targetFile(
        targetDirectory.filePath(
            QStringLiteral(".ksword-recovery-XXXXXX.tmp")));
    targetFile.setAutoRemove(true);
    if (!targetFile.open())
    {
        errorTextOut = QStringLiteral("无法创建恢复临时文件：%1")
            .arg(QDir::toNativeSeparators(normalizedTargetPath));
        return false;
    }
    const auto cancelTargetWrite = [&targetFile]()
        {
            targetFile.close();
            targetFile.remove();
        };
    const auto writeExact =
        [&targetFile, &errorTextOut](const char* dataPointer, const qint64 byteCount) -> bool
        {
            if (byteCount <= 0)
            {
                return true;
            }
            const qint64 writtenBytes = targetFile.write(dataPointer, byteCount);
            if (writtenBytes != byteCount)
            {
                errorTextOut = QStringLiteral(
                    "恢复临时文件写入不完整（期望 %1，实际 %2）。")
                    .arg(byteCount)
                    .arg(writtenBytes);
                return false;
            }
            return true;
        };
    const auto commitTargetWithoutOverwrite =
        [&targetFile,
         &cancelTargetWrite,
         &normalizedTargetPath,
         &errorTextOut]() -> bool
        {
            if (!targetFile.flush())
            {
                const QString flushError = targetFile.errorString();
                cancelTargetWrite();
                errorTextOut = QStringLiteral("提交恢复文件失败：%1")
                    .arg(flushError);
                return false;
            }

            targetFile.close();
            if (QFileInfo::exists(normalizedTargetPath))
            {
                targetFile.remove();
                errorTextOut = QStringLiteral(
                    "恢复期间目标路径被其它程序创建，已拒绝覆盖：%1")
                    .arg(QDir::toNativeSeparators(normalizedTargetPath));
                return false;
            }

            // QTemporaryFile::rename 不执行 copy/delete 回退；Windows 原子
            // rename 在目标已存在时失败，因此上面的提示检查即使发生竞争，
            // 这里仍是最终的不可覆盖安全门。
            if (!targetFile.rename(normalizedTargetPath))
            {
                const QString renameError = targetFile.errorString();
                const bool targetNowExists =
                    QFileInfo::exists(normalizedTargetPath);
                targetFile.remove();
                errorTextOut = targetNowExists
                    ? QStringLiteral(
                        "恢复期间目标路径被其它程序创建，已拒绝覆盖：%1")
                        .arg(QDir::toNativeSeparators(normalizedTargetPath))
                    : QStringLiteral("提交恢复文件失败：%1")
                        .arg(renameError);
                return false;
            }

            // rename 成功后 QTemporaryFile 的内部路径已是最终目标；必须关闭
            // 自动删除，否则析构会把刚提交的恢复文件删除。
            targetFile.setAutoRemove(false);
            return true;
        };

    if (!recordValue.nonResidentData)
    {
        reportProgress(30, QStringLiteral("正在提取驻留数据"));
        if (recordValue.unsupportedDataStream || !recordValue.residentReady)
        {
            cancelTargetWrite();
            errorTextOut = QStringLiteral("驻留主数据流使用了当前不支持的属性编码。");
            return false;
        }
        if (recordValue.sizeBytes >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
            recordValue.residentData.size() !=
                static_cast<int>(recordValue.sizeBytes))
        {
            cancelTargetWrite();
            errorTextOut = QStringLiteral("驻留数据长度异常，已取消恢复。");
            return false;
        }
        if (!writeExact(
            recordValue.residentData.constData(),
            recordValue.residentData.size()))
        {
            cancelTargetWrite();
            return false;
        }
        reportProgress(90, QStringLiteral("正在原子提交恢复文件"));
        if (!commitTargetWithoutOverwrite())
        {
            return false;
        }
        reportProgress(100, QStringLiteral("驻留文件恢复完成"));
        return true;
    }

    reportProgress(12, QStringLiteral("正在校验非驻留数据簇"));
    NtfsVolumeBitmapSnapshot bitmapBefore{};
    QString bitmapErrorText;
    if (!loadNtfsVolumeBitmapSnapshot(
        volumeRoot,
        bitmapBefore,
        bitmapErrorText) ||
        !validateDeletedDataRunsUnallocated(
            recordValue,
            bitmapBefore,
            errorTextOut))
    {
        cancelTargetWrite();
        if (errorTextOut.isEmpty())
        {
            errorTextOut = QStringLiteral("恢复前卷位图校验失败：%1")
                .arg(bitmapErrorText);
        }
        return false;
    }

    QString openVolumeErrorText;
    HANDLE volumeHandle =
        openReadHandle(buildVolumeDevicePath(volumeRoot), openVolumeErrorText);
    if (volumeHandle == INVALID_HANDLE_VALUE)
    {
        cancelTargetWrite();
        errorTextOut = QStringLiteral("无法打开源卷读取数据簇：%1")
            .arg(openVolumeErrorText);
        return false;
    }

    std::array<std::byte, 512> bootBytes{};
    if (!readBytesAtOffset(
        volumeHandle,
        0,
        static_cast<std::uint32_t>(bootBytes.size()),
        bootBytes.data(),
        errorTextOut))
    {
        ::CloseHandle(volumeHandle);
        cancelTargetWrite();
        return false;
    }
    const QByteArray oemText(
        reinterpret_cast<const char*>(bootBytes.data() + 3),
        8);
    const std::uint16_t bytesPerSector =
        le16(bootBytes.data() + 11);
    const std::uint8_t sectorsPerCluster =
        static_cast<std::uint8_t>(bootBytes[13]);
    if (!oemText.startsWith("NTFS") ||
        bytesPerSector == 0 ||
        sectorsPerCluster == 0)
    {
        ::CloseHandle(volumeHandle);
        cancelTargetWrite();
        errorTextOut = QStringLiteral("源卷 NTFS 引导参数无效。");
        return false;
    }
    const std::uint64_t bytesPerCluster =
        static_cast<std::uint64_t>(bytesPerSector) *
        static_cast<std::uint64_t>(sectorsPerCluster);
    constexpr std::uint64_t MaximumReadChunkBytes =
        4ULL * 1024ULL * 1024ULL;
    const std::uint64_t maximumChunkClusters =
        std::max<std::uint64_t>(
            1ULL,
            MaximumReadChunkBytes / bytesPerCluster);

    std::uint64_t totalRunCapacity = 0;
    for (const NtfsDataRun& runValue : recordValue.dataRuns)
    {
        if (runValue.clusterCount >
            std::numeric_limits<std::uint64_t>::max() / bytesPerCluster ||
            totalRunCapacity >
            std::numeric_limits<std::uint64_t>::max() -
                runValue.clusterCount * bytesPerCluster)
        {
            ::CloseHandle(volumeHandle);
            cancelTargetWrite();
            errorTextOut = QStringLiteral("非驻留数据段容量溢出。");
            return false;
        }
        totalRunCapacity += runValue.clusterCount * bytesPerCluster;
    }
    if (totalRunCapacity < recordValue.sizeBytes)
    {
        ::CloseHandle(volumeHandle);
        cancelTargetWrite();
        errorTextOut = QStringLiteral(
            "非驻留 runlist 仅覆盖 %1 字节，小于文件长度 %2，可能缺少外部数据段。")
            .arg(static_cast<qulonglong>(totalRunCapacity))
            .arg(static_cast<qulonglong>(recordValue.sizeBytes));
        return false;
    }

    reportProgress(18, QStringLiteral("正在读取非驻留数据簇"));
    std::uint64_t logicalBytesWritten = 0;
    std::vector<std::byte> rawReadBuffer;
    QByteArray sparseZeroBuffer;
    for (const NtfsDataRun& runValue : recordValue.dataRuns)
    {
        if (logicalBytesWritten >= recordValue.sizeBytes)
        {
            break;
        }
        const std::uint64_t runCapacityBytes =
            runValue.clusterCount * bytesPerCluster;
        const std::uint64_t logicalBytesInRun =
            std::min<std::uint64_t>(
                runCapacityBytes,
                recordValue.sizeBytes - logicalBytesWritten);
        std::uint64_t runBytesProcessed = 0;
        while (runBytesProcessed < logicalBytesInRun)
        {
            const std::uint64_t remainingRunBytes =
                logicalBytesInRun - runBytesProcessed;
            const std::uint64_t logicalChunkBytes =
                std::min<std::uint64_t>(
                    remainingRunBytes,
                    maximumChunkClusters * bytesPerCluster);
            const std::uint64_t initializedChunkBytes =
                logicalBytesWritten < recordValue.initializedSizeBytes
                ? std::min<std::uint64_t>(
                    logicalChunkBytes,
                    recordValue.initializedSizeBytes - logicalBytesWritten)
                : 0;
            const std::uint64_t chunkClusters =
                (std::max<std::uint64_t>(
                    initializedChunkBytes,
                    1ULL) +
                    bytesPerCluster - 1ULL) /
                bytesPerCluster;
            const std::uint64_t alignedChunkBytes =
                chunkClusters * bytesPerCluster;
            if (alignedChunkBytes >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()))
            {
                ::CloseHandle(volumeHandle);
                cancelTargetWrite();
                errorTextOut = QStringLiteral("单次簇读取长度超过系统限制。");
                return false;
            }

            if (runValue.isSparse || initializedChunkBytes == 0)
            {
                sparseZeroBuffer.fill(
                    '\0',
                    static_cast<qsizetype>(logicalChunkBytes));
                if (!writeExact(
                    sparseZeroBuffer.constData(),
                    sparseZeroBuffer.size()))
                {
                    ::CloseHandle(volumeHandle);
                    cancelTargetWrite();
                    return false;
                }
            }
            else
            {
                const std::uint64_t runClusterOffset =
                    runBytesProcessed / bytesPerCluster;
                if (runValue.startLcn >
                    std::numeric_limits<std::uint64_t>::max() -
                        runClusterOffset ||
                    runValue.startLcn + runClusterOffset >
                    std::numeric_limits<std::uint64_t>::max() /
                        bytesPerCluster)
                {
                    ::CloseHandle(volumeHandle);
                    cancelTargetWrite();
                    errorTextOut = QStringLiteral("非驻留数据簇偏移溢出。");
                    return false;
                }
                const std::uint64_t rawOffset =
                    (runValue.startLcn + runClusterOffset) *
                    bytesPerCluster;
                rawReadBuffer.resize(
                    static_cast<std::size_t>(alignedChunkBytes));
                QString rawReadErrorText;
                if (!readBytesAtOffset(
                    volumeHandle,
                    rawOffset,
                    static_cast<std::uint32_t>(alignedChunkBytes),
                    rawReadBuffer.data(),
                    rawReadErrorText))
                {
                    ::CloseHandle(volumeHandle);
                    cancelTargetWrite();
                    errorTextOut = QStringLiteral(
                        "读取删除数据簇失败（LCN %1）：%2")
                        .arg(static_cast<qulonglong>(
                            runValue.startLcn + runClusterOffset))
                        .arg(rawReadErrorText);
                    return false;
                }
                if (!writeExact(
                    reinterpret_cast<const char*>(rawReadBuffer.data()),
                    static_cast<qint64>(initializedChunkBytes)))
                {
                    ::CloseHandle(volumeHandle);
                    cancelTargetWrite();
                    return false;
                }
                const std::uint64_t uninitializedChunkBytes =
                    logicalChunkBytes - initializedChunkBytes;
                if (uninitializedChunkBytes > 0)
                {
                    sparseZeroBuffer.fill(
                        '\0',
                        static_cast<qsizetype>(uninitializedChunkBytes));
                    if (!writeExact(
                        sparseZeroBuffer.constData(),
                        sparseZeroBuffer.size()))
                    {
                        ::CloseHandle(volumeHandle);
                        cancelTargetWrite();
                        return false;
                    }
                }
            }

            runBytesProcessed += logicalChunkBytes;
            logicalBytesWritten += logicalChunkBytes;
            const int readPercent =
                recordValue.sizeBytes == 0
                ? 80
                : 18 + static_cast<int>(
                    (static_cast<long double>(logicalBytesWritten) * 62.0L) /
                    static_cast<long double>(recordValue.sizeBytes));
            reportProgress(
                std::min(readPercent, 80),
                QStringLiteral("正在读取非驻留数据簇"));
        }
    }
    ::CloseHandle(volumeHandle);

    if (logicalBytesWritten != recordValue.sizeBytes)
    {
        cancelTargetWrite();
        errorTextOut = QStringLiteral(
            "非驻留数据读取不完整（期望 %1，实际 %2）。")
            .arg(static_cast<qulonglong>(recordValue.sizeBytes))
            .arg(static_cast<qulonglong>(logicalBytesWritten));
        return false;
    }

    // 提交前再次读取卷位图与 MFT：恢复期间任何簇分配或记录变化都会使临时文件作废。
    reportProgress(84, QStringLiteral("正在执行提交前二次校验"));
    NtfsVolumeBitmapSnapshot bitmapAfter{};
    bitmapErrorText.clear();
    if (!loadNtfsVolumeBitmapSnapshot(
        volumeRoot,
        bitmapAfter,
        bitmapErrorText) ||
        !validateDeletedDataRunsUnallocated(
            recordValue,
            bitmapAfter,
            errorTextOut))
    {
        cancelTargetWrite();
        if (errorTextOut.isEmpty())
        {
            errorTextOut = QStringLiteral("恢复后卷位图校验失败：%1")
                .arg(bitmapErrorText);
        }
        return false;
    }

    NtfsRawRecord finalRecordValue{};
    readRecordErrorText.clear();
    if (!loadNtfsSingleRecord(
        volumeRoot,
        deletedEntry.fileReference,
        finalRecordValue,
        readRecordErrorText) ||
        finalRecordValue.inUse ||
        !sameDeletedDataRunLayout(recordValue, finalRecordValue))
    {
        cancelTargetWrite();
        errorTextOut = readRecordErrorText.isEmpty()
            ? QStringLiteral("恢复过程中 MFT 记录或 runlist 已变化，临时文件未提交。")
            : QStringLiteral("提交前回读 MFT 失败：%1").arg(readRecordErrorText);
        return false;
    }

    reportProgress(94, QStringLiteral("正在原子提交恢复文件"));
    if (!commitTargetWithoutOverwrite())
    {
        return false;
    }
    reportProgress(100, QStringLiteral("非驻留文件恢复完成"));
    return true;
}

bool ks::file::ManualFileSystemParser::recoverNtfsResidentFile(
    const QString& volumeRootPath,
    const NtfsDeletedFileEntry& deletedEntry,
    const QString& targetFilePath,
    QString& errorTextOut)
{
    // 旧入口没有恢复能力枚举时，仅允许它继续表达原有的 Resident 恢复语义。
    NtfsDeletedFileEntry residentEntry = deletedEntry;
    if (residentEntry.recoveryCapability ==
            NtfsRecoveryCapability::MetadataOnly &&
        residentEntry.residentDataReady)
    {
        residentEntry.recoveryCapability =
            NtfsRecoveryCapability::Resident;
    }
    return recoverNtfsDeletedFile(
        volumeRootPath,
        residentEntry,
        targetFilePath,
        errorTextOut);
}
