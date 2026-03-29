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
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
        bool residentReady = false;            // 是否成功提取驻留数据。
        QByteArray residentData;               // 驻留数据内容。
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
        std::vector<NtfsRawRecord> records;     // 缓存记录集合。
        qint64 loadedMsec = 0;                  // 缓存时间戳（毫秒）。
        std::uint64_t recordLimit = 0;          // 本次缓存覆盖的最大记录数上限。
    };

    std::mutex g_ntfsCacheMutex; // NTFS 缓存互斥锁。
    std::unordered_map<std::wstring, NtfsCacheEntry> g_ntfsCache; // 分卷缓存字典。

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

    // ntfsFixup 作用：应用 NTFS USA 修复，保证扇区尾校验通过。
    bool ntfsFixup(std::vector<std::byte>& recordBytes)
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
        for (std::uint16_t i = 1; i < usaCount; ++i)
        {
            const std::size_t tailOffset = static_cast<std::size_t>(i) * 512ULL - 2ULL;
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
    bool parseNtfsRecord(std::vector<std::byte>& recordBytes, const std::uint64_t recordIndex, NtfsRawRecord& recordOut)
    {
        if (recordBytes.size() < 64 || std::memcmp(recordBytes.data(), "FILE", 4) != 0)
        {
            return false;
        }
        if (!ntfsFixup(recordBytes))
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

            const bool nonResident = (recordBytes[attrOffset + 8] != std::byte{ 0 });
            if (attrType == 0x30 && !nonResident)
            {
                const std::uint32_t contentLength = le32(recordBytes.data() + attrOffset + 16);
                const std::uint16_t contentOffset = le16(recordBytes.data() + attrOffset + 20);
                const std::size_t contentStart = attrOffset + contentOffset;
                if (contentLength >= 66 && contentStart + contentLength <= recordBytes.size())
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
                        if (score >= preferredNameScore)
                        {
                            preferredNameScore = score;
                            preferredName = candidateName;
                            recordOut.parentIndex = (parentRef & 0x0000FFFFFFFFFFFFULL);
                            recordOut.modifiedTime100ns = modified100ns;
                            recordOut.sizeBytes = realSize;
                        }
                    }
                }
            }
            else if (attrType == 0x80)
            {
                if (!nonResident)
                {
                    const std::uint32_t dataLength = le32(recordBytes.data() + attrOffset + 16);
                    const std::uint16_t dataOffset = le16(recordBytes.data() + attrOffset + 20);
                    const std::size_t dataStart = attrOffset + dataOffset;
                    constexpr std::uint32_t MaxResidentSize = 2 * 1024 * 1024;
                    if (dataLength > 0 && dataLength <= MaxResidentSize && dataStart + dataLength <= recordBytes.size())
                    {
                        recordOut.residentData = QByteArray(
                            reinterpret_cast<const char*>(recordBytes.data() + dataStart),
                            static_cast<int>(dataLength));
                        recordOut.residentReady = true;
                        recordOut.sizeBytes = static_cast<std::uint64_t>(dataLength);
                    }
                }
                else if (attrOffset + 56 <= recordBytes.size())
                {
                    const std::uint64_t nonResidentSize = le64(recordBytes.data() + attrOffset + 48);
                    if (nonResidentSize > 0)
                    {
                        recordOut.sizeBytes = nonResidentSize;
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
        const std::uint32_t bytesPerRecordHint,
        const std::uint64_t maxRecordCount,
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
            if (!parseNtfsRecord(recordBytes, actualRecordIndex, recordValue))
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
            if (recordValue.fileName.isEmpty() && recordValue.recordIndex != 5)
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
        const bool allowFsctlFallback)
    {
        const std::wstring cacheKey = toWide(volumeRoot.toUpper());
        const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
        constexpr qint64 NtfsCacheTtlMsec = 60000; // 缓存 60 秒，避免同卷短时间重复全盘扫描。
        constexpr std::uint64_t NtfsHardMaxRecordCount = 1500000ULL; // 全局硬上限：兼顾大卷覆盖率与内存占用。
        const std::uint64_t effectiveMaxRecordCount = (maxRecordCountHint == 0)
            ? NtfsHardMaxRecordCount
            : std::min<std::uint64_t>(maxRecordCountHint, NtfsHardMaxRecordCount);
        {
            std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
            const auto cacheIt = g_ntfsCache.find(cacheKey);
            if (cacheIt != g_ntfsCache.end()
                && (nowMsec - cacheIt->second.loadedMsec) <= NtfsCacheTtlMsec
                && cacheIt->second.recordLimit >= effectiveMaxRecordCount)
            {
                recordsOut = cacheIt->second.records;
                return true;
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

        // 先读取 NTFS 引导扇区，后续常规模式和回退模式都依赖这组参数。
        std::array<std::byte, 512> bootBytes{};
        if (!readBytesAtOffset(volumeHandle, 0, 512, bootBytes.data(), errorTextOut))
        {
            ::CloseHandle(volumeHandle);
            return false;
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
                    bytesPerRecord,
                    MaxRecordCount,
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

                {
                    std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
                    NtfsCacheEntry cacheEntry{};
                    cacheEntry.records = recordsOut;
                    cacheEntry.loadedMsec = nowMsec;
                    cacheEntry.recordLimit = MaxRecordCount;
                    g_ntfsCache[cacheKey] = std::move(cacheEntry);
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
                    if (parseNtfsRecord(firstRecordBytes, 0, mftRecordValue)
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
        for (std::uint64_t indexValue = 0; indexValue < parseCount; ++indexValue)
        {
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
            if (!parseNtfsRecord(recordBytes, indexValue, recordValue))
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
            if (recordValue.fileName.isEmpty() && recordValue.recordIndex != 5)
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

        {
            std::scoped_lock<std::mutex> lock(g_ntfsCacheMutex);
            NtfsCacheEntry cacheEntry{};
            cacheEntry.records = recordsOut;
            cacheEntry.loadedMsec = nowMsec;
            cacheEntry.recordLimit = MaxRecordCount;
            g_ntfsCache[cacheKey] = std::move(cacheEntry);
        }
        return true;
    }

    // resolveNtfsDirectoryIndex 作用：按路径段定位目标目录的 MFT 记录号。
    bool resolveNtfsDirectoryIndex(
        const std::vector<NtfsRawRecord>& recordsValue,
        const QStringList& pathSegments,
        std::uint64_t& directoryIndexOut)
    {
        std::unordered_multimap<std::uint64_t, const NtfsRawRecord*> childMap;
        childMap.reserve(recordsValue.size() * 2);
        for (const NtfsRawRecord& recordValue : recordsValue)
        {
            childMap.emplace(recordValue.parentIndex, &recordValue);
        }

        std::uint64_t currentIndex = 5;
        for (const QString& segmentText : pathSegments)
        {
            bool found = false;
            const auto childRange = childMap.equal_range(currentIndex);
            for (auto it = childRange.first; it != childRange.second; ++it)
            {
                const NtfsRawRecord* childRecord = it->second;
                if (childRecord == nullptr || !childRecord->inUse || !childRecord->isDirectory)
                {
                    continue;
                }
                if (childRecord->fileName.compare(segmentText, Qt::CaseInsensitive) == 0)
                {
                    currentIndex = childRecord->recordIndex;
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

    // buildNtfsPathHint 作用：从父链重建可读路径。
    QString buildNtfsPathHint(
        const QString& volumeRootPath,
        const NtfsRawRecord& targetRecord,
        const std::unordered_map<std::uint64_t, const NtfsRawRecord*>& recordMap)
    {
        QStringList segments;
        segments.push_front(targetRecord.fileName);
        std::uint64_t parentIndex = targetRecord.parentIndex;
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
    QString& errorTextOut)
{
    entriesOut.clear();
    errorTextOut.clear();
    fsTypeOut = detectFileSystemType(pathText);

    if (fsTypeOut == ManualFsType::Ntfs)
    {
        const QString volumeRoot = trimVolumeRoot(pathText);
        std::vector<NtfsRawRecord> recordsValue;
        constexpr std::uint64_t DirectoryListMaxRecords = 250000ULL; // 目录浏览优先响应速度，限制单次扫描规模。
        constexpr std::uint64_t DirectoryRetryMaxRecords = 1200000ULL; // 定位目录失败时扩展扫描上限，避免漏掉高编号目录项。
        if (!loadNtfsRecords(volumeRoot, recordsValue, errorTextOut, DirectoryListMaxRecords, true))
        {
            return false;
        }

        const QStringList pathSegments = splitRelativeSegments(pathText);
        std::uint64_t dirIndex = 5;
        bool usedFullRangeScan = false; // usedFullRangeScan：标记本次是否已经执行过 60 万记录全量扫描。
        bool resolveOk = resolveNtfsDirectoryIndex(recordsValue, pathSegments, dirIndex);
        if (!resolveOk && DirectoryListMaxRecords < DirectoryRetryMaxRecords)
        {
            std::vector<NtfsRawRecord> retryRecords;
            QString retryErrorText;
            if (loadNtfsRecords(volumeRoot, retryRecords, retryErrorText, DirectoryRetryMaxRecords, true))
            {
                recordsValue.swap(retryRecords);
                usedFullRangeScan = true;
                resolveOk = resolveNtfsDirectoryIndex(recordsValue, pathSegments, dirIndex);
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
            const QString normalizedPath = QDir::toNativeSeparators(QDir::cleanPath(pathText));
            QDir fallbackDirectory(normalizedPath);
            if (fallbackDirectory.exists())
            {
                const QFileInfoList fallbackEntries = fallbackDirectory.entryInfoList(
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
                for (const QFileInfo& fileInfoValue : fallbackEntries)
                {
                    ManualDirectoryEntry itemValue{};
                    itemValue.name = fileInfoValue.fileName();
                    itemValue.absolutePath = fileInfoValue.absoluteFilePath();
                    itemValue.isDirectory = fileInfoValue.isDir();
                    itemValue.sizeBytes = itemValue.isDirectory ? 0 : static_cast<std::uint64_t>(fileInfoValue.size());
                    itemValue.modifiedTime = fileInfoValue.lastModified();
                    itemValue.typeText = buildTypeText(itemValue.name, itemValue.isDirectory);
                    entriesOut.push_back(std::move(itemValue));
                }

                kLogEvent event;
                warn << event
                    << "[FileDock] NTFS链路定位失败，回退WinAPI枚举, path="
                    << normalizedPath.toStdString()
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
            [&recordsValue, &entriesOut, &currentPath](const std::uint64_t targetDirectoryIndex)
            {
                entriesOut.clear();
                for (const NtfsRawRecord& recordValue : recordsValue)
                {
                    if (!recordValue.inUse || recordValue.parentIndex != targetDirectoryIndex || recordValue.fileName.isEmpty())
                    {
                        continue;
                    }

                    ManualDirectoryEntry itemValue{};
                    itemValue.name = recordValue.fileName;
                    itemValue.absolutePath = QDir(currentPath).filePath(recordValue.fileName);
                    itemValue.isDirectory = recordValue.isDirectory;
                    itemValue.sizeBytes = recordValue.isDirectory ? 0 : recordValue.sizeBytes;
                    itemValue.modifiedTime = fileTimeToLocal(recordValue.modifiedTime100ns);
                    itemValue.typeText = buildTypeText(recordValue.fileName, recordValue.isDirectory);
                    itemValue.ntfsFileReference = recordValue.recordIndex;
                    entriesOut.push_back(std::move(itemValue));
                }
            };

        appendEntriesByDirectoryIndex(dirIndex);

        // 当目录已定位但结果为空时，执行一次全量重试，避免因扫描上限导致“存在目录却显示空列表”。
        if (entriesOut.empty() && !usedFullRangeScan && DirectoryListMaxRecords < DirectoryRetryMaxRecords)
        {
            std::vector<NtfsRawRecord> retryRecords;
            QString retryErrorText;
            if (loadNtfsRecords(volumeRoot, retryRecords, retryErrorText, DirectoryRetryMaxRecords, true))
            {
                std::uint64_t retryDirIndex = 5;
                if (resolveNtfsDirectoryIndex(retryRecords, pathSegments, retryDirIndex))
                {
                    recordsValue.swap(retryRecords);
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
    QString& errorTextOut)
{
    deletedOut.clear();
    errorTextOut.clear();
    if (detectFileSystemType(volumeRootPath) != ManualFsType::Ntfs)
    {
        errorTextOut = QStringLiteral("仅 NTFS 卷支持误删扫描。");
        return false;
    }

    std::vector<NtfsRawRecord> recordsValue;
    const QString volumeRoot = trimVolumeRoot(volumeRootPath);
    constexpr std::uint64_t DeletedScanMaxRecords = 600000ULL; // 删除恢复需要覆盖更多记录，保持较高上限。
    if (!loadNtfsRecords(volumeRoot, recordsValue, errorTextOut, DeletedScanMaxRecords, false))
    {
        return false;
    }

    std::unordered_map<std::uint64_t, const NtfsRawRecord*> recordMap;
    recordMap.reserve(recordsValue.size());
    for (const NtfsRawRecord& recordValue : recordsValue)
    {
        recordMap.emplace(recordValue.recordIndex, &recordValue);
    }

    constexpr std::size_t MaxDeletedRecords = 20000;
    for (const NtfsRawRecord& recordValue : recordsValue)
    {
        if (recordValue.inUse || recordValue.isDirectory || recordValue.fileName.isEmpty())
        {
            continue;
        }

        NtfsDeletedFileEntry itemValue{};
        itemValue.fileName = recordValue.fileName;
        itemValue.pathHint = buildNtfsPathHint(volumeRoot, recordValue, recordMap);
        itemValue.sizeBytes = recordValue.sizeBytes;
        itemValue.modifiedTime = fileTimeToLocal(recordValue.modifiedTime100ns);
        itemValue.fileReference = recordValue.recordIndex;
        itemValue.residentDataReady = recordValue.residentReady;
        itemValue.residentData = recordValue.residentData;
        deletedOut.push_back(std::move(itemValue));
        if (deletedOut.size() >= MaxDeletedRecords)
        {
            break;
        }
    }

    std::sort(
        deletedOut.begin(),
        deletedOut.end(),
        [](const NtfsDeletedFileEntry& left, const NtfsDeletedFileEntry& right) {
            if (left.modifiedTime.isValid() && right.modifiedTime.isValid())
            {
                return left.modifiedTime > right.modifiedTime;
            }
            if (left.modifiedTime.isValid() != right.modifiedTime.isValid())
            {
                return left.modifiedTime.isValid();
            }
            return QString::compare(left.fileName, right.fileName, Qt::CaseInsensitive) < 0;
        });
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

    QFile targetFile(targetFilePath);
    if (!targetFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        errorTextOut = QStringLiteral("无法写入目标文件：%1").arg(targetFilePath);
        return false;
    }
    const qint64 writeBytes = targetFile.write(deletedEntry.residentData);
    targetFile.close();
    if (writeBytes != deletedEntry.residentData.size())
    {
        errorTextOut = QStringLiteral("写入长度不完整。");
        return false;
    }
    return true;
}
