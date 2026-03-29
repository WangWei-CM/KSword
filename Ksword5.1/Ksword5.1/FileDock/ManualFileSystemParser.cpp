#include "ManualFileSystemParser.h"

// ============================================================
// ManualFileSystemParser.cpp
// 说明：
// 1) 提供 NTFS/FAT32 的手动目录解析；
// 2) 提供 NTFS 删除项扫描与驻留数据恢复；
// 3) 解析逻辑全部封装在本文件，UI 只消费统一结构。
// ============================================================

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
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

namespace
{
    // NtfsNameLink 作用：
    // - 表示同一条 MFT 记录下的一个“目录名链接”；
    // - 用于保留硬链接/多父目录场景，避免一条记录只能映射到一个目录。
    struct NtfsNameLink
    {
        std::uint64_t parentIndex = 0;         // 父目录记录号。
        QString fileName;                      // 该父目录下显示的文件名。
        int nameScore = -1;                    // 名称优先级，优先 Win32/Win32&DOS。
    };

    // NtfsDataRun 作用：
    // - 保存非 resident 主数据流的一个数据段；
    // - 用于后续估算“簇是否仍未被覆盖”。
    struct NtfsDataRun
    {
        std::uint64_t startLcn = 0;            // 数据段起始 LCN，稀疏段时为 0。
        std::uint64_t clusterCount = 0;        // 当前数据段占用的簇数量。
        bool isSparse = false;                 // 是否稀疏段，true 表示该段逻辑上为 0 填充。
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
        std::uint64_t parentIndex = 0;         // 父目录记录号。
        QString fileName;                      // 文件名（优先 Win32 命名空间）。
        std::uint64_t sizeBytes = 0;           // 文件大小。
        std::uint64_t modifiedTime100ns = 0;   // 修改时间（FILETIME 100ns）。
        bool inUse = false;                    // 是否在用。
        bool isDirectory = false;              // 是否目录。
        bool hasPrimaryDataStream = false;     // 是否存在未命名主数据流。
        bool nonResidentData = false;          // 未命名主数据流是否为非 resident。
        bool residentReady = false;            // 是否成功提取驻留数据。
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

    std::mutex g_ntfsCacheMutex; // NTFS 缓存互斥锁。
    std::unordered_map<std::wstring, std::shared_ptr<NtfsCacheEntry>> g_ntfsCache; // 分卷缓存字典。

    // buildTypeText 前置声明：
    // - 供上方的 WinAPI 补齐辅助函数复用类型文本生成逻辑；
    // - 具体实现保持在后文原位置，避免重复实现。
    QString buildTypeText(const QString& fileName, const bool isDirectory);

    // buildNtfsCacheIndex 作用：
    // - 为缓存记录生成“目录项级索引”和“记录号索引”；
    // - 避免每次切目录都重新遍历全量 MFT 记录。
    void buildNtfsCacheIndex(NtfsCacheEntry& cacheEntry)
    {
        cacheEntry.directoryLinks.clear();
        cacheEntry.recordOffsetByIndex.clear();
        cacheEntry.directoryLinks.reserve(cacheEntry.records.size() * 2);
        cacheEntry.recordOffsetByIndex.reserve(cacheEntry.records.size());

        for (std::size_t i = 0; i < cacheEntry.records.size(); ++i)
        {
            const NtfsRawRecord& recordValue = cacheEntry.records[i];
            cacheEntry.recordOffsetByIndex.emplace(recordValue.recordIndex, i);

            if (!recordValue.nameLinks.empty())
            {
                for (const NtfsNameLink& nameLink : recordValue.nameLinks)
                {
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

        std::sort(
            cacheEntry.directoryLinks.begin(),
            cacheEntry.directoryLinks.end(),
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
            });
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
        std::vector<ks::file::ManualDirectoryEntry>& entriesOut)
    {
        entriesOut.clear();

        const QString normalizedPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
        QDir fallbackDirectory(normalizedPath);
        if (!fallbackDirectory.exists())
        {
            return false;
        }

        const QFileInfoList fallbackEntries = fallbackDirectory.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& fileInfoValue : fallbackEntries)
        {
            ks::file::ManualDirectoryEntry itemValue{};
            itemValue.name = fileInfoValue.fileName();
            itemValue.absolutePath = fileInfoValue.absoluteFilePath();
            itemValue.isDirectory = fileInfoValue.isDir();
            itemValue.sizeBytes = itemValue.isDirectory ? 0 : static_cast<std::uint64_t>(fileInfoValue.size());
            itemValue.modifiedTime = fileInfoValue.lastModified();
            itemValue.typeText = buildTypeText(itemValue.name, itemValue.isDirectory);
            entriesOut.push_back(std::move(itemValue));
        }
        return true;
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

    // readSignedLe64 作用：
    // - 读取长度 1~8 字节的有符号小端整数；
    // - 供 NTFS runlist 解析相对 LCN 偏移使用。
    std::int64_t readSignedLe64(const std::byte* ptr, const std::uint8_t byteCount)
    {
        std::int64_t value = 0;
        if (ptr == nullptr || byteCount == 0 || byteCount > 8)
        {
            return 0;
        }

        for (std::uint8_t i = 0; i < byteCount; ++i)
        {
            value |= (static_cast<std::int64_t>(static_cast<std::uint8_t>(ptr[i])) << (i * 8));
        }

        // 若最高字节符号位为 1，则需要手动做符号扩展。
        const std::int64_t signMask = static_cast<std::int64_t>(1) << (byteCount * 8 - 1);
        if (byteCount < 8 && (value & signMask) != 0)
        {
            value |= (~static_cast<std::int64_t>(0)) << (byteCount * 8);
        }
        return value;
    }

    // parseNtfsRunList 作用：
    // - 解析非 resident 主数据流的 runlist；
    // - 输出每一段实际 LCN 范围或稀疏段信息。
    bool parseNtfsRunList(
        const std::byte* runListPtr,
        const std::byte* runListEnd,
        std::vector<NtfsDataRun>& dataRunsOut)
    {
        dataRunsOut.clear();
        if (runListPtr == nullptr || runListEnd == nullptr || runListPtr >= runListEnd)
        {
            return false;
        }

        std::int64_t currentLcn = 0;
        while (runListPtr < runListEnd)
        {
            const std::uint8_t headerValue = static_cast<std::uint8_t>(*runListPtr);
            runListPtr += 1;
            if (headerValue == 0)
            {
                return !dataRunsOut.empty();
            }

            const std::uint8_t lengthFieldBytes = (headerValue & 0x0F);
            const std::uint8_t offsetFieldBytes = ((headerValue >> 4) & 0x0F);
            if (lengthFieldBytes == 0
                || lengthFieldBytes > 8
                || offsetFieldBytes > 8
                || runListPtr + lengthFieldBytes + offsetFieldBytes > runListEnd)
            {
                dataRunsOut.clear();
                return false;
            }

            std::uint64_t clusterCountValue = 0;
            for (std::uint8_t i = 0; i < lengthFieldBytes; ++i)
            {
                clusterCountValue |=
                    (static_cast<std::uint64_t>(static_cast<std::uint8_t>(runListPtr[i])) << (i * 8));
            }
            if (clusterCountValue == 0)
            {
                dataRunsOut.clear();
                return false;
            }

            NtfsDataRun runValue{};
            runValue.clusterCount = clusterCountValue;
            if (offsetFieldBytes == 0)
            {
                runValue.isSparse = true;
            }
            else
            {
                const std::int64_t lcnDeltaValue =
                    readSignedLe64(runListPtr + lengthFieldBytes, offsetFieldBytes);
                currentLcn += lcnDeltaValue;
                if (currentLcn < 0)
                {
                    dataRunsOut.clear();
                    return false;
                }
                runValue.startLcn = static_cast<std::uint64_t>(currentLcn);
            }

            dataRunsOut.push_back(std::move(runValue));
            runListPtr += lengthFieldBytes + offsetFieldBytes;
        }
        return !dataRunsOut.empty();
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

    // readBytesAtOffset 作用：在指定偏移读取固定长度字节块。
    bool readBytesAtOffset(
        const HANDLE fileHandle,
        const std::uint64_t offsetValue,
        const std::uint32_t sizeValue,
        std::byte* bufferPtr,
        QString& errorTextOut)
    {
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
        const std::uint64_t bitmapBytes = (totalClusters + 7ULL) / 8ULL;
        constexpr std::uint64_t MaxBitmapBytes = 128ULL * 1024ULL * 1024ULL;
        if (bitmapBytes == 0 || bitmapBytes > MaxBitmapBytes)
        {
            errorTextOut = QStringLiteral("卷位图过大或为空, bytes=%1").arg(static_cast<qulonglong>(bitmapBytes));
            ::CloseHandle(volumeHandle);
            return false;
        }

        STARTING_LCN_INPUT_BUFFER inputBuffer{};
        inputBuffer.StartingLcn.QuadPart = 0;
        const std::size_t headerBytes = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
        std::vector<std::uint8_t> outputBuffer(headerBytes + static_cast<std::size_t>(bitmapBytes) + 64ULL);
        returnedBytes = 0;
        if (::DeviceIoControl(
            volumeHandle,
            FSCTL_GET_VOLUME_BITMAP,
            &inputBuffer,
            static_cast<DWORD>(sizeof(inputBuffer)),
            outputBuffer.data(),
            static_cast<DWORD>(outputBuffer.size()),
            &returnedBytes,
            nullptr) == FALSE)
        {
            errorTextOut = QStringLiteral("FSCTL_GET_VOLUME_BITMAP失败, code=%1").arg(::GetLastError());
            ::CloseHandle(volumeHandle);
            return false;
        }
        ::CloseHandle(volumeHandle);

        if (returnedBytes < headerBytes)
        {
            errorTextOut = QStringLiteral("卷位图返回长度不足。");
            return false;
        }

        const VOLUME_BITMAP_BUFFER* bitmapBuffer =
            reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(outputBuffer.data());
        bitmapOut.startingLcn =
            static_cast<std::uint64_t>(bitmapBuffer->StartingLcn.QuadPart);
        bitmapOut.clusterCount =
            static_cast<std::uint64_t>(bitmapBuffer->BitmapSize.QuadPart);

        const std::uint64_t actualBitmapBytes = (bitmapOut.clusterCount + 7ULL) / 8ULL;
        if (headerBytes + actualBitmapBytes > returnedBytes
            || headerBytes + actualBitmapBytes > outputBuffer.size())
        {
            errorTextOut = QStringLiteral("卷位图数据长度异常。");
            return false;
        }

        bitmapOut.bitmapBytes.assign(
            outputBuffer.begin() + static_cast<std::ptrdiff_t>(headerBytes),
            outputBuffer.begin() + static_cast<std::ptrdiff_t>(headerBytes + actualBitmapBytes));
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
        if (relativeStartBit + clusterCount > bitmapValue.clusterCount)
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
            totalClusters += runValue.clusterCount;
            if (runValue.isSparse)
            {
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
            intactClusters += (runValue.clusterCount - allocatedClusters);
        }

        if (totalClusters == 0)
        {
            return (recordValue.sizeBytes == 0) ? 100 : -1;
        }

        return static_cast<int>((intactClusters * 100ULL + totalClusters / 2ULL) / totalClusters);
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
            if (attrType == 0x30 && !nonResident)
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

                        // nameLinks：对同一父目录只保留最佳可显示名称，避免 DOS 名和长名重复污染列表。
                        bool parentLinkUpdated = false;
                        for (NtfsNameLink& nameLink : recordOut.nameLinks)
                        {
                            if (nameLink.parentIndex != parentIndexValue)
                            {
                                continue;
                            }
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
                        if (captureResidentData && dataLength > 0)
                        {
                            recordOut.residentData = QByteArray(
                                reinterpret_cast<const char*>(recordBytes.data() + dataStart),
                                static_cast<int>(dataLength));
                        }
                    }
                }
                else if (attrLength >= 56)
                {
                    recordOut.hasPrimaryDataStream = true;
                    recordOut.nonResidentData = true;
                    const std::uint64_t nonResidentSize = le64(recordBytes.data() + attrOffset + 48);
                    if (nonResidentSize > 0)
                    {
                        recordOut.sizeBytes = nonResidentSize;
                    }

                    const std::uint16_t runListOffset = le16(recordBytes.data() + attrOffset + 32);
                    const std::size_t runListStart = attrOffset + runListOffset;
                    if (runListOffset >= 0x40
                        && runListStart < attrEnd)
                    {
                        std::vector<NtfsDataRun> runValues;
                        if (parseNtfsRunList(recordBytes.data() + runListStart, recordBytes.data() + attrEnd, runValues))
                        {
                            recordOut.dataRuns = std::move(runValues);
                        }
                    }
                }
            }

            attrOffset += attrLength;
        }

        recordOut.fileName = preferredName;
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
        QString& errorTextOut)
    {
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
            if (queryOk == FALSE)
            {
                const DWORD queryErrorCode = ::GetLastError();
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
        const std::function<void(int, const QString&)>& progressCallback,
        std::shared_ptr<const NtfsCacheEntry>* cacheEntryOut)
    {
        const std::wstring cacheKey = toWide(volumeRoot.toUpper());
        const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
        constexpr qint64 NtfsCacheTtlMsec = 60000; // 缓存 60 秒，避免同卷短时间重复全盘扫描。
        constexpr std::uint64_t NtfsHardMaxRecordCount = 1500000ULL; // 全局硬上限：兼顾大卷覆盖率与内存占用。
        const std::uint64_t effectiveMaxRecordCount = (maxRecordCountHint == 0)
            ? NtfsHardMaxRecordCount
            : std::min<std::uint64_t>(maxRecordCountHint, NtfsHardMaxRecordCount);
        if (useCache)
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
                    return true;
                }
            }
        }

        const QString volumeDevicePath = buildVolumeDevicePath(volumeRoot);
        QString openVolumeErrorText;
        HANDLE volumeHandle = openReadHandle(volumeDevicePath, openVolumeErrorText);
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openVolumeErrorText;
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
            ::CloseHandle(volumeHandle);
            return false;
        }
        if (progressCallback)
        {
            progressCallback(8, QStringLiteral("已读取 NTFS 引导区"));
        }

        const QByteArray oemText(reinterpret_cast<const char*>(bootBytes.data() + 3), 8);
        if (!oemText.startsWith("NTFS"))
        {
            ::CloseHandle(volumeHandle);
            errorTextOut = QStringLiteral("不是 NTFS 卷。");
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
            ::CloseHandle(volumeHandle);
            errorTextOut = QStringLiteral("MFT记录大小异常: %1").arg(bytesPerRecord);
            return false;
        }

        const std::uint64_t MaxRecordCount = effectiveMaxRecordCount;
        HANDLE sourceHandle = INVALID_HANDLE_VALUE;        // sourceHandle：本轮实际读取句柄（$MFT 或卷句柄）。
        std::uint64_t sourceBaseOffset = 0;               // sourceBaseOffset：读取起点偏移（$MFT=0，卷回退=MFT偏移）。
        std::uint64_t parseCount = 0;                     // parseCount：计划解析记录数。
        bool usingVolumeFallback = false;                 // usingVolumeFallback：是否进入卷偏移直读回退。
        QString fallbackReasonText;                       // fallbackReasonText：回退原因日志文本。

        // 第一优先级：直接读取 \\.\X:\$MFT。
        const QString mftPath = QStringLiteral("\\\\.\\%1\\$MFT").arg(volumeRoot.left(2).toUpper());
        QString openMftErrorText;
        HANDLE mftHandle = openReadHandle(mftPath, openMftErrorText);
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
        if (usingVolumeFallback)
        {
            if (mftHandle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(mftHandle);
                mftHandle = INVALID_HANDLE_VALUE;
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
                    fsctlErrorText))
            {
                recordsOut = std::move(fsctlRecords);
                ::CloseHandle(volumeHandle);

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

                if (useCache)
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
                    buildNtfsCacheIndex(*cacheEntry);

                    std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
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

            if (!fsctlErrorText.isEmpty())
            {
                fallbackReasonText =
                    fallbackReasonText.isEmpty()
                    ? fsctlErrorText
                    : (fallbackReasonText + QStringLiteral("; FSCTL回退失败: ") + fsctlErrorText);
            }

            // 第二层回退：卷偏移连续读取（作为兜底方案保留）。
            // 先读取 MFT 第 0 条记录（$MFT 自身），尽力拿到真实 MFT 数据长度。
            // 这样可以避免直接按整个卷空间估算，导致 parseCount 被顶到 600000 上限。
            std::uint64_t estimatedMftRecordCount = 0;
            {
                std::vector<std::byte> firstRecordBytes(bytesPerRecord);
                QString firstRecordErrorText;
                if (readBytesAtOffset(
                    volumeHandle,
                    mftStartOffset,
                    bytesPerRecord,
                    firstRecordBytes.data(),
                    firstRecordErrorText))
                {
                    NtfsRawRecord mftRecordValue{};
                    if (parseNtfsRecord(firstRecordBytes, 0, bytesPerSector, false, mftRecordValue)
                        && mftRecordValue.sizeBytes >= bytesPerRecord)
                    {
                        estimatedMftRecordCount =
                            mftRecordValue.sizeBytes / static_cast<std::uint64_t>(bytesPerRecord);
                    }
                }
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
                ::CloseHandle(volumeHandle);
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
                ::CloseHandle(volumeHandle);
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
                << eol;
        }

        // 通用解析流程：从 sourceHandle 按记录序号读取并解析。
        std::vector<std::byte> recordBytes(bytesPerRecord);
        recordsOut.clear();
        recordsOut.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(parseCount, 200000ULL)));
        std::uint32_t consecutiveReadFailCount = 0;    // 连续读取失败计数：防止卷末尾反复失败造成长时间阻塞。
        std::uint32_t consecutiveEmptyCount = 0;       // 连续空记录计数：回退模式用于提前停止无效扫描。
        std::uint32_t consecutiveInvalidCount = 0;     // 连续无效记录计数：回退模式下用于识别“已离开有效 MFT 区间”。
        std::uint64_t validRecordCount = 0;            // 已解析出的有效记录数，用于无效区间提前终止判定。
        int lastReportedPercent = -1;                  // lastReportedPercent：顺序扫描分支的最近一次上报百分比。
        for (std::uint64_t indexValue = 0; indexValue < parseCount; ++indexValue)
        {
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
                        usingVolumeFallback
                        ? QStringLiteral("按卷偏移扫描 MFT 记录")
                        : QStringLiteral("按 $MFT 逻辑文件扫描记录"));
                }
            }

            const std::uint64_t offsetValue = sourceBaseOffset + indexValue * bytesPerRecord;
            QString readErrorText;
            if (!readBytesAtOffset(sourceHandle, offsetValue, bytesPerRecord, recordBytes.data(), readErrorText))
            {
                ++consecutiveReadFailCount;
                if (consecutiveReadFailCount >= 8)
                {
                    break;
                }
                continue;
            }
            consecutiveReadFailCount = 0;

            NtfsRawRecord recordValue{};
            if (!parseNtfsRecord(recordBytes, indexValue, bytesPerSector, captureResidentData, recordValue))
            {
                ++consecutiveInvalidCount;

                // 回退模式下，若出现大段全 0 区域，说明已到有效 MFT 尾部附近，可提前结束。
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
                    if (usingVolumeFallback && indexValue > 4096 && consecutiveEmptyCount > 2048)
                    {
                        break;
                    }
                }
                else
                {
                    consecutiveEmptyCount = 0;
                }

                // 回退模式下，连续大量无效记录通常表示已跳出连续 MFT 数据区，
                // 继续扫描只会显著拖慢解析，故在满足条件后提前终止。
                if (usingVolumeFallback
                    && validRecordCount > 1024
                    && indexValue > 8192
                    && consecutiveInvalidCount > 8192)
                {
                    break;
                }
                continue;
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
            recordsOut.push_back(std::move(recordValue));
        }

        if (mftHandle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(mftHandle);
        }
        ::CloseHandle(volumeHandle);

        if (recordsOut.empty())
        {
            errorTextOut = QStringLiteral("MFT解析结果为空。");
            return false;
        }

        if (useCache)
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
            buildNtfsCacheIndex(*cacheEntry);

            std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
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
            buildNtfsCacheIndex(*cacheEntry);
            *cacheEntryOut = cacheEntry;
            if (!copyRecordsOut)
            {
                recordsOut.clear();
            }
        }
        return true;
    }

    // resolveNtfsDirectoryIndex 作用：按路径段定位目标目录的 MFT 记录号。
    bool resolveNtfsDirectoryIndex(
        const NtfsCacheEntry& cacheEntry,
        const QStringList& pathSegments,
        std::uint64_t& directoryIndexOut)
    {
        std::uint64_t currentIndex = 5;
        for (const QString& segmentText : pathSegments)
        {
            bool found = false;
            const auto childRange = findNtfsDirectoryLinkRange(cacheEntry.directoryLinks, currentIndex);
            for (auto it = childRange.first; it != childRange.second; ++it)
            {
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
        const std::uint64_t mftStartOffset = mftStartCluster * bytesPerCluster;
        const std::int8_t clustersPerRecord = static_cast<std::int8_t>(bootBytes[64]);
        std::uint32_t bytesPerRecord = 1024;
        if (clustersPerRecord < 0)
        {
            bytesPerRecord = (1u << (-clustersPerRecord));
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
            if (readBytesAtOffset(
                mftHandle,
                fileReference * static_cast<std::uint64_t>(bytesPerRecord),
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

        std::vector<std::byte> recordBytes(bytesPerRecord);
        if (!readBytesAtOffset(
            volumeHandle,
            mftStartOffset + fileReference * static_cast<std::uint64_t>(bytesPerRecord),
            bytesPerRecord,
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

        if (!parseNtfsRecord(recordBytes, fileReference, bytesPerSector, true, recordOut))
        {
            errorTextOut = fsctlErrorText.isEmpty()
                ? QStringLiteral("卷偏移记录解析失败。")
                : (fsctlErrorText + QStringLiteral("; 卷偏移记录解析失败。"));
            return false;
        }
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
        if (!readBytesAtOffset(volumeHandle, entryOffset, 4, entryBytes.data(), errorTextOut))
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
        QString& errorTextOut)
    {
        chainOut.clear();
        if (firstCluster < 2)
        {
            return false;
        }

        std::uint32_t currentCluster = firstCluster;
        constexpr std::size_t MaxClusterCount = 262144;
        for (std::size_t i = 0; i < MaxClusterCount; ++i)
        {
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
        return !chainOut.empty();
    }

    // enumerateFatDirectoryByCluster 作用：解析某目录簇链下的目录项。
    bool enumerateFatDirectoryByCluster(
        const HANDLE volumeHandle,
        const Fat32BootInfo& infoValue,
        const std::uint32_t dirCluster,
        std::vector<Fat32Entry>& entriesOut,
        QString& errorTextOut)
    {
        entriesOut.clear();
        std::vector<std::uint32_t> chainList;
        if (!loadClusterChain(volumeHandle, infoValue, dirCluster, chainList, errorTextOut))
        {
            return false;
        }

        std::vector<std::byte> clusterBytes(infoValue.bytesPerCluster);
        std::vector<QString> lfnParts;
        for (std::uint32_t clusterValue : chainList)
        {
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
        return true;
    }

    // resolveFatDirectoryCluster 作用：按路径定位到目标目录簇号。
    bool resolveFatDirectoryCluster(
        const HANDLE volumeHandle,
        const Fat32BootInfo& infoValue,
        const QStringList& pathSegments,
        std::uint32_t& clusterOut,
        QString& errorTextOut)
    {
        std::uint32_t currentCluster = infoValue.rootCluster;
        for (const QString& segmentText : pathSegments)
        {
            std::vector<Fat32Entry> children;
            if (!enumerateFatDirectoryByCluster(volumeHandle, infoValue, currentCluster, children, errorTextOut))
            {
                return false;
            }
            bool found = false;
            for (const Fat32Entry& childItem : children)
            {
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
    return ManualFsType::Unknown;
}

bool ks::file::ManualFileSystemParser::enumerateDirectory(
    const QString& pathText,
    std::vector<ManualDirectoryEntry>& entriesOut,
    ManualFsType& fsTypeOut,
    QString& errorTextOut,
    bool* usedWinApiFallbackOut)
{
    entriesOut.clear();
    errorTextOut.clear();
    if (usedWinApiFallbackOut != nullptr)
    {
        *usedWinApiFallbackOut = false;
    }
    fsTypeOut = detectFileSystemType(pathText);

    if (fsTypeOut == ManualFsType::Ntfs)
    {
        const QString volumeRoot = trimVolumeRoot(pathText);
        std::vector<NtfsRawRecord> recordsValue;
        std::shared_ptr<const NtfsCacheEntry> cacheSnapshot;
        constexpr std::uint64_t DirectoryListMaxRecords = 250000ULL; // 目录浏览优先响应速度，限制单次扫描规模。
        constexpr std::uint64_t DirectoryRetryMaxRecords = 1200000ULL; // 定位目录失败时扩展扫描上限，避免漏掉高编号目录项。
        if (!loadNtfsRecords(volumeRoot, recordsValue, errorTextOut, DirectoryListMaxRecords, true, true, false, false, false, {}, &cacheSnapshot))
        {
            return false;
        }

        const QStringList pathSegments = splitRelativeSegments(pathText);
        std::uint64_t dirIndex = 5;
        bool usedFullRangeScan = false; // usedFullRangeScan：标记本次是否已经执行过 60 万记录全量扫描。
        bool resolveOk = (cacheSnapshot != nullptr)
            && resolveNtfsDirectoryIndex(*cacheSnapshot, pathSegments, dirIndex);
        if (!resolveOk && DirectoryListMaxRecords < DirectoryRetryMaxRecords)
        {
            std::vector<NtfsRawRecord> retryRecords;
            QString retryErrorText;
            std::shared_ptr<const NtfsCacheEntry> retrySnapshot;
            if (loadNtfsRecords(volumeRoot, retryRecords, retryErrorText, DirectoryRetryMaxRecords, true, true, false, false, false, {}, &retrySnapshot))
            {
                recordsValue.swap(retryRecords);
                cacheSnapshot = retrySnapshot;
                usedFullRangeScan = true;
                resolveOk = (cacheSnapshot != nullptr)
                    && resolveNtfsDirectoryIndex(*cacheSnapshot, pathSegments, dirIndex);
            }
            else if (errorTextOut.isEmpty())
            {
                errorTextOut = retryErrorText;
            }
        }
        if (!resolveOk)
        {
            // 兜底策略：目录真实存在但 MFT 链路解析失败时，回退到 WinAPI 枚举，
            // 避免手动模式直接“空白/报错不可访问”。
            if (enumerateDirectoryByWinApi(pathText, entriesOut))
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

            if (errorTextOut.isEmpty())
            {
                errorTextOut = QStringLiteral("NTFS目录不存在或不可访问。");
            }
            return false;
        }

        const QString currentPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
        auto appendEntriesByDirectoryIndex =
            [&entriesOut, &currentPath, &cacheSnapshot](const std::uint64_t targetDirectoryIndex)
            {
                entriesOut.clear();
                if (cacheSnapshot == nullptr)
                {
                    return;
                }

                const auto childRange = findNtfsDirectoryLinkRange(cacheSnapshot->directoryLinks, targetDirectoryIndex);
                for (auto it = childRange.first; it != childRange.second; ++it)
                {
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
            };

        appendEntriesByDirectoryIndex(dirIndex);

        // 对 Windows API 结果做一次低成本补齐校验：
        // 1) 手动结果若少于 WinAPI，优先补齐缺失项，避免用户看到“比资源管理器更少”；
        // 2) 这样即使目录里存在超出当前扫描窗口的新记录，也不会继续强制整卷深扫卡 UI。
        std::vector<ManualDirectoryEntry> winApiEntries;
        if (enumerateDirectoryByWinApi(pathText, winApiEntries))
        {
            QSet<QString> existingNameSet;
            existingNameSet.reserve(static_cast<int>(entriesOut.size() * 2 + 8));
            for (const ManualDirectoryEntry& itemValue : entriesOut)
            {
                existingNameSet.insert(itemValue.name.toCaseFolded());
            }

            std::size_t mergedCount = 0;
            for (const ManualDirectoryEntry& fallbackItem : winApiEntries)
            {
                const QString normalizedName = fallbackItem.name.toCaseFolded();
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
                    << "[FileDock] NTFS手动解析结果少于WinAPI，已补齐缺失目录项, path="
                    << QDir::toNativeSeparators(QDir::cleanPath(pathText)).toStdString()
                    << ", manualRows="
                    << static_cast<qulonglong>(entriesOut.size() - mergedCount)
                    << ", mergedRows="
                    << static_cast<qulonglong>(mergedCount)
                    << ", winApiRows="
                    << static_cast<qulonglong>(winApiEntries.size())
                    << eol;
            }
        }

        // 当目录已定位且手动路径返回空时，才执行一次扩大扫描，尽量兼顾“速度优先”和“极端目录可用性”。
        if (entriesOut.empty() && !usedFullRangeScan && DirectoryListMaxRecords < DirectoryRetryMaxRecords)
        {
            std::vector<NtfsRawRecord> retryRecords;
            QString retryErrorText;
            std::shared_ptr<const NtfsCacheEntry> retrySnapshot;
            if (loadNtfsRecords(volumeRoot, retryRecords, retryErrorText, DirectoryRetryMaxRecords, true, true, false, false, false, {}, &retrySnapshot))
            {
                std::uint64_t retryDirIndex = 5;
                if (retrySnapshot != nullptr && resolveNtfsDirectoryIndex(*retrySnapshot, pathSegments, retryDirIndex))
                {
                    recordsValue.swap(retryRecords);
                    cacheSnapshot = retrySnapshot;
                    dirIndex = retryDirIndex;
                    appendEntriesByDirectoryIndex(dirIndex);
                }
            }
        }
    }
    else if (fsTypeOut == ManualFsType::Fat32)
    {
        const QString volumeRoot = trimVolumeRoot(pathText);
        QString openErrorText;
        HANDLE volumeHandle = openReadHandle(buildVolumeDevicePath(volumeRoot), openErrorText);
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openErrorText;
            return false;
        }

        Fat32BootInfo bootInfo{};
        if (!readFat32BootInfo(volumeHandle, bootInfo, errorTextOut))
        {
            ::CloseHandle(volumeHandle);
            return false;
        }

        std::uint32_t dirCluster = bootInfo.rootCluster;
        const QStringList pathSegments = splitRelativeSegments(pathText);
        if (!resolveFatDirectoryCluster(volumeHandle, bootInfo, pathSegments, dirCluster, errorTextOut))
        {
            ::CloseHandle(volumeHandle);
            return false;
        }

        std::vector<Fat32Entry> fatEntries;
        if (!enumerateFatDirectoryByCluster(volumeHandle, bootInfo, dirCluster, fatEntries, errorTextOut))
        {
            ::CloseHandle(volumeHandle);
            return false;
        }
        ::CloseHandle(volumeHandle);

        const QString currentPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
        for (const Fat32Entry& fatItem : fatEntries)
        {
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
    else
    {
        errorTextOut = QStringLiteral("当前卷不是 NTFS/FAT32，无法手动解析。");
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
                : (fsTypeOut == ManualFsType::Fat32 ? "FAT32" : "Unknown"))
            << eol;
    }

    std::sort(
        entriesOut.begin(),
        entriesOut.end(),
        [](const ManualDirectoryEntry& left, const ManualDirectoryEntry& right) {
            if (left.isDirectory != right.isDirectory)
            {
                return left.isDirectory && !right.isDirectory;
            }
            return QString::compare(left.name, right.name, Qt::CaseInsensitive) < 0;
        });
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
    constexpr std::uint64_t DeletedScanMaxRecords = 1500000ULL; // 删除恢复默认拉满当前硬上限，优先提高命中率。
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
                itemValue.estimatedIntegrityPercent =
                    estimateDeletedRecordIntegrityPercent(recordValue, bitmapSnapshotPtr);
                itemValue.hasOriginalName = hasOriginalName;
                itemValue.residentDataReady = recordValue.residentReady;
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

    std::sort(
        deletedOut.begin(),
        deletedOut.end(),
        [](const NtfsDeletedFileEntry& left, const NtfsDeletedFileEntry& right) {
            // 完整度优先：
            // 1) 已知完整度的项排在未知前；
            // 2) 同为已知时按完整度降序；
            // 3) 再优先原始文件名，再按时间与大小排序。
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
        progressCallback(100, QStringLiteral("删除项排序完成"));
    }
    return true;
}

bool ks::file::ManualFileSystemParser::recoverNtfsResidentFile(
    const QString& volumeRootPath,
    const NtfsDeletedFileEntry& deletedEntry,
    const QString& targetFilePath,
    QString& errorTextOut)
{
    Q_UNUSED(volumeRootPath);
    errorTextOut.clear();
    if (!deletedEntry.residentDataReady)
    {
        errorTextOut = QStringLiteral("该条目不是驻留数据，暂不支持直接恢复。");
        return false;
    }
    if (targetFilePath.trimmed().isEmpty())
    {
        errorTextOut = QStringLiteral("目标文件路径为空。");
        return false;
    }

    // residentDataBytes：优先复用条目里已有数据；若扫描阶段未缓存，则在这里按记录号回读。
    QByteArray residentDataBytes = deletedEntry.residentData;
    if (deletedEntry.sizeBytes > 0
        && residentDataBytes.size() != static_cast<int>(deletedEntry.sizeBytes))
    {
        NtfsRawRecord recordValue{};
        const QString volumeRoot = trimVolumeRoot(volumeRootPath);
        if (volumeRoot.isEmpty())
        {
            errorTextOut = QStringLiteral("卷根路径无效。");
            return false;
        }

        QString readRecordErrorText;
        if (!loadNtfsSingleRecord(volumeRoot, deletedEntry.fileReference, recordValue, readRecordErrorText))
        {
            errorTextOut = QStringLiteral("按需回读 resident 数据失败：%1").arg(readRecordErrorText);
            return false;
        }
        if (recordValue.inUse)
        {
            errorTextOut = QStringLiteral("该 MFT 记录已重新被占用，无法安全恢复。");
            return false;
        }
        if (!recordValue.residentReady)
        {
            errorTextOut = QStringLiteral("该记录当前已不是 resident 主数据流。");
            return false;
        }

        residentDataBytes = recordValue.residentData;
        if (residentDataBytes.size() != static_cast<int>(recordValue.sizeBytes))
        {
            errorTextOut = QStringLiteral("按需回读的数据长度异常。");
            return false;
        }
    }

    QFile targetFile(targetFilePath);
    if (!targetFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        errorTextOut = QStringLiteral("无法写入目标文件：%1").arg(targetFilePath);
        return false;
    }
    const qint64 writeBytes = targetFile.write(residentDataBytes);
    targetFile.close();
    if (writeBytes != residentDataBytes.size())
    {
        errorTextOut = QStringLiteral("写入长度不完整。");
        return false;
    }
    return true;
}
