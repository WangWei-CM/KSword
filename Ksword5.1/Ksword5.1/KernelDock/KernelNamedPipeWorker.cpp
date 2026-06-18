#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "KernelNamedPipeWorker.h"

// ============================================================
// KernelNamedPipeWorker.cpp
// 作用说明：
// 1) 使用 NtOpenFile + NtQueryDirectoryFile 枚举 NPFS Named Pipe 目录；
// 2) 保留路径候选和失败状态，便于 UI 展示兼容性差异；
// 3) 明确不使用系统句柄表枚举，不枚举任何进程句柄。
// ============================================================

#include <QDateTime>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

#include <Windows.h>
#include <Winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef FILE_LIST_DIRECTORY
#define FILE_LIST_DIRECTORY 0x0001
#endif

namespace
{
    constexpr NTSTATUS kStatusNoMoreFiles = static_cast<NTSTATUS>(0x80000006L);
    constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
    constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);
    constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
    constexpr ULONG kFileDirectoryInformationClass = 1U;
    constexpr ULONG kInitialDirectoryBufferBytes = 64U * 1024U;
    constexpr ULONG kMaximumDirectoryBufferBytes = 1024U * 1024U;
    constexpr std::size_t kMaximumPipeRowsPerDirectory = 65536U;

    using NtOpenFileFn = NTSTATUS(NTAPI*)(
        PHANDLE FileHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        PIO_STATUS_BLOCK IoStatusBlock,
        ULONG ShareAccess,
        ULONG OpenOptions);

    using NtQueryDirectoryFileFn = NTSTATUS(NTAPI*)(
        HANDLE FileHandle,
        HANDLE Event,
        PVOID ApcRoutine,
        PVOID ApcContext,
        PIO_STATUS_BLOCK IoStatusBlock,
        PVOID FileInformation,
        ULONG Length,
        ULONG FileInformationClass,
        BOOLEAN ReturnSingleEntry,
        PUNICODE_STRING FileName,
        BOOLEAN RestartScan);

    struct NtFileDirectoryApi
    {
        HMODULE ntdllModule = nullptr;
        NtOpenFileFn openFile = nullptr;
        NtQueryDirectoryFileFn queryDirectoryFile = nullptr;
    };

    struct ScopedHandle
    {
        HANDLE handle = nullptr;

        ~ScopedHandle()
        {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        }

        ScopedHandle() = default;
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
    };

    struct KsFileDirectoryInformation
    {
        ULONG NextEntryOffset;
        ULONG FileIndex;
        LARGE_INTEGER CreationTime;
        LARGE_INTEGER LastAccessTime;
        LARGE_INTEGER LastWriteTime;
        LARGE_INTEGER ChangeTime;
        LARGE_INTEGER EndOfFile;
        LARGE_INTEGER AllocationSize;
        ULONG FileAttributes;
        ULONG FileNameLength;
        WCHAR FileName[1];
    };

    // isNeedGrowBufferStatus：
    // - 输入 status：NtQueryDirectoryFile 返回的 NTSTATUS；
    // - 处理逻辑：识别表示当前目录枚举缓冲区不足或记录被截断的状态；
    // - 返回结果：true 表示调用方应扩大缓冲区后重试当前扫描批次。
    bool isNeedGrowBufferStatus(const NTSTATUS status)
    {
        return status == kStatusInfoLengthMismatch
            || status == kStatusBufferOverflow
            || status == kStatusBufferTooSmall;
    }

    // initializeUnicodeString：
    // - 输入 unicodeTextOut：待填充的 UNICODE_STRING；sourceText：生命周期由调用方保持的宽字符串；
    // - 处理逻辑：按 NT API 需要填充 Buffer/Length/MaximumLength，并防止 USHORT 长度溢出；
    // - 返回结果：无，unicodeTextOut 指向 sourceText 内部缓冲区。
    void initializeUnicodeString(UNICODE_STRING& unicodeTextOut, const std::wstring& sourceText)
    {
        unicodeTextOut.Buffer = const_cast<PWSTR>(sourceText.c_str());
        unicodeTextOut.Length = static_cast<USHORT>(
            std::min<std::size_t>(
                sourceText.size() * sizeof(wchar_t),
                static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));
        unicodeTextOut.MaximumLength = static_cast<USHORT>(
            std::min<std::size_t>(
                unicodeTextOut.Length + sizeof(wchar_t),
                static_cast<std::size_t>(std::numeric_limits<USHORT>::max())));
    }

    // ntStatusToText：
    // - 输入 ntdllModule：用于 FormatMessageW 的 ntdll 模块句柄；status：待格式化的 NTSTATUS；
    // - 处理逻辑：优先输出十六进制状态码，再尝试从 ntdll 消息表附加系统文本；
    // - 返回结果：适合 UI 展示的状态字符串，消息表缺失时只返回十六进制。
    QString ntStatusToText(const HMODULE ntdllModule, const NTSTATUS status)
    {
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<std::uint32_t>(status), 8, 16, QChar('0'))
            .toUpper();

        std::array<wchar_t, 256> messageBuffer{};
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            ntdllModule,
            static_cast<DWORD>(status),
            0,
            messageBuffer.data(),
            static_cast<DWORD>(messageBuffer.size()),
            nullptr);
        if (messageLength == 0)
        {
            return hexText;
        }

        return QStringLiteral("%1 %2")
            .arg(hexText, QString::fromWCharArray(messageBuffer.data()).trimmed());
    }

    // fileTimeToText：
    // - 输入 fileTime：FILE_DIRECTORY_INFORMATION 中的 100ns Windows 时间戳；
    // - 处理逻辑：将有效 FILETIME 转为本地时区字符串，不可用或异常值保留诊断文本；
    // - 返回结果：格式化时间文本，或 <Unavailable>/<Invalid:...>。
    QString fileTimeToText(const LARGE_INTEGER& fileTime)
    {
        if (fileTime.QuadPart <= 0)
        {
            return QStringLiteral("<Unavailable>");
        }

        constexpr std::int64_t windowsToUnix100Ns = 116444736000000000LL;
        const std::int64_t unixMilliseconds = (fileTime.QuadPart - windowsToUnix100Ns) / 10000LL;
        if (unixMilliseconds <= 0)
        {
            return QStringLiteral("<Invalid:%1>").arg(static_cast<qlonglong>(fileTime.QuadPart));
        }

        return QDateTime::fromMSecsSinceEpoch(unixMilliseconds, QTimeZone::UTC)
            .toLocalTime()
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    }

    // attributesToText：
    // - 输入 attributes：FILE_DIRECTORY_INFORMATION.FileAttributes 位图；
    // - 处理逻辑：拆解常见 FILE_ATTRIBUTE_* 标志，并保留原始十六进制值；
    // - 返回结果：例如 0x00000010 (DIRECTORY) 的 UI 展示文本。
    QString attributesToText(const ULONG attributes)
    {
        QStringList parts;
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U) parts << QStringLiteral("READONLY");
        if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0U) parts << QStringLiteral("HIDDEN");
        if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0U) parts << QStringLiteral("SYSTEM");
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) parts << QStringLiteral("DIRECTORY");
        if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0U) parts << QStringLiteral("ARCHIVE");
        if ((attributes & FILE_ATTRIBUTE_NORMAL) != 0U) parts << QStringLiteral("NORMAL");
        if ((attributes & FILE_ATTRIBUTE_TEMPORARY) != 0U) parts << QStringLiteral("TEMPORARY");
        if (parts.isEmpty())
        {
            parts << QStringLiteral("0");
        }

        return QStringLiteral("0x%1 (%2)")
            .arg(static_cast<std::uint32_t>(attributes), 8, 16, QChar('0'))
            .arg(parts.join(QStringLiteral("|")))
            .toUpper();
    }

    // joinPipeNtPath：
    // - 输入 directoryPath：成功枚举的候选目录；pipeName：NPFS 返回的管道名；
    // - 处理逻辑：规范目录分隔符并避免重复尾部反斜杠；
    // - 返回结果：完整 NT 风格路径，例如 \Device\NamedPipe\InitShutdown。
    QString joinPipeNtPath(const QString& directoryPath, const QString& pipeName)
    {
        QString normalizedDirectory = directoryPath.trimmed();
        normalizedDirectory.replace('/', '\\');
        while (normalizedDirectory.endsWith('\\') && normalizedDirectory.size() > 1)
        {
            normalizedDirectory.chop(1);
        }
        return QStringLiteral("%1\\%2").arg(normalizedDirectory, pipeName);
    }

    // buildNamedPipeDirectoryCandidates：
    // - 输入：无；
    // - 处理逻辑：保留多个等价或兼容路径，优先使用原生 \Device\NamedPipe；
    // - 返回结果：按尝试顺序排列的 NPFS 命名管道目录候选。
    std::vector<QString> buildNamedPipeDirectoryCandidates()
    {
        std::vector<QString> candidates;
        candidates.push_back(QStringLiteral("\\Device\\NamedPipe"));
        candidates.push_back(QStringLiteral("\\Device\\NamedPipe\\"));
        candidates.push_back(QStringLiteral("\\??\\PIPE"));
        candidates.push_back(QStringLiteral("\\??\\PIPE\\"));
        return candidates;
    }

    // loadNtFileDirectoryApi：
    // - 输入 apiOut：接收 ntdll 模块和函数指针；errorTextOut：接收失败原因；
    // - 处理逻辑：动态解析 NtOpenFile/NtQueryDirectoryFile，避免新增静态链接依赖；
    // - 返回结果：true 表示两个入口均可用，false 表示 worker 不应继续枚举。
    bool loadNtFileDirectoryApi(NtFileDirectoryApi& apiOut, QString& errorTextOut)
    {
        errorTextOut.clear();
        apiOut.ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (apiOut.ntdllModule == nullptr)
        {
            apiOut.ntdllModule = ::LoadLibraryW(L"ntdll.dll");
        }
        if (apiOut.ntdllModule == nullptr)
        {
            errorTextOut = QStringLiteral("加载 ntdll.dll 失败。");
            return false;
        }

        apiOut.openFile = reinterpret_cast<NtOpenFileFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtOpenFile"));
        apiOut.queryDirectoryFile = reinterpret_cast<NtQueryDirectoryFileFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtQueryDirectoryFile"));
        if (apiOut.openFile == nullptr || apiOut.queryDirectoryFile == nullptr)
        {
            errorTextOut = QStringLiteral("解析 NtOpenFile/NtQueryDirectoryFile 失败。");
            return false;
        }
        return true;
    }

    // openNamedPipeDirectory：
    // - 输入 api：已解析的 NT 文件 API；directoryPath：待打开的 NT 路径；
    // - 处理逻辑：以 FILE_LIST_DIRECTORY | SYNCHRONIZE 打开 NPFS 目录，要求目录对象且使用同步 I/O；
    // - 返回结果：NtOpenFile 的原始 NTSTATUS，同时通过 handleOut/ioStatusOut 返回句柄和 I/O 状态。
    NTSTATUS openNamedPipeDirectory(
        const NtFileDirectoryApi& api,
        const QString& directoryPath,
        HANDLE& handleOut,
        IO_STATUS_BLOCK& ioStatusOut)
    {
        handleOut = nullptr;
        ioStatusOut = IO_STATUS_BLOCK{};

        const std::wstring pathWide = directoryPath.trimmed().toStdWString();
        UNICODE_STRING objectName{};
        initializeUnicodeString(objectName, pathWide);

        OBJECT_ATTRIBUTES objectAttributes{};
        InitializeObjectAttributes(&objectAttributes, &objectName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

        return api.openFile(
            &handleOut,
            FILE_LIST_DIRECTORY | SYNCHRONIZE,
            &objectAttributes,
            &ioStatusOut,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);
    }

    // parseDirectoryBuffer：
    // - 输入 buffer/validBytes：NtQueryDirectoryFile 返回的 FILE_DIRECTORY_INFORMATION 批次；
    // - 处理逻辑：逐项校验 NextEntryOffset/FileNameLength 边界，转换为 KernelNamedPipeEntry；
    // - 返回结果：true 表示批次解析完成，false 表示 errorTextOut 携带边界或格式错误。
    bool parseDirectoryBuffer(
        const NtFileDirectoryApi& api,
        const QString& directoryPath,
        const std::vector<std::uint8_t>& buffer,
        const ULONG validBytes,
        const NTSTATUS batchStatus,
        std::vector<KernelNamedPipeEntry>& rowsOut,
        QString& errorTextOut)
    {
        errorTextOut.clear();
        if (validBytes < sizeof(KsFileDirectoryInformation))
        {
            errorTextOut = QStringLiteral("NtQueryDirectoryFile 返回缓冲区过小。");
            return false;
        }

        std::size_t offset = 0;
        std::size_t guardCount = 0;
        while (offset + sizeof(KsFileDirectoryInformation) <= validBytes)
        {
            if (++guardCount > kMaximumPipeRowsPerDirectory)
            {
                errorTextOut = QStringLiteral("目录项数量超过保护上限。");
                return false;
            }

            const auto* info = reinterpret_cast<const KsFileDirectoryInformation*>(buffer.data() + offset);
            const std::size_t fileNameBytes = static_cast<std::size_t>(info->FileNameLength);
            const std::size_t recordMinimumBytes = offsetof(KsFileDirectoryInformation, FileName) + fileNameBytes;
            if (offset + recordMinimumBytes > validBytes)
            {
                errorTextOut = QStringLiteral("目录项文件名越过返回缓冲区。");
                return false;
            }

            const QString pipeName = QString::fromWCharArray(
                info->FileName,
                static_cast<int>(fileNameBytes / sizeof(wchar_t))).trimmed();
            if (!pipeName.isEmpty() && pipeName != QStringLiteral(".") && pipeName != QStringLiteral(".."))
            {
                KernelNamedPipeEntry row;
                row.pipeName = pipeName;
                row.ntPath = joinPipeNtPath(directoryPath, pipeName);
                row.sourceDirectory = directoryPath;
                row.statusText = ntStatusToText(api.ntdllModule, batchStatus);
                row.querySucceeded = true;
                row.attributes = static_cast<std::uint32_t>(info->FileAttributes);
                row.attributesText = attributesToText(info->FileAttributes);
                row.lastWriteTime = static_cast<std::int64_t>(info->LastWriteTime.QuadPart);
                row.lastWriteTimeText = fileTimeToText(info->LastWriteTime);
                rowsOut.push_back(std::move(row));
            }

            if (info->NextEntryOffset == 0)
            {
                break;
            }
            offset += info->NextEntryOffset;
        }

        return true;
    }

    // enumerateNamedPipeDirectory：
    // - 输入 api：已解析的 NT API；directoryPath：一个候选 NPFS 目录；
    // - 处理逻辑：打开目录后循环 NtQueryDirectoryFile，处理 STATUS_NO_MORE_FILES、缓冲不足和权限失败；
    // - 返回结果：true 表示该目录完整枚举到结束或达到保护上限，rowsOut 追加成功解析的条目。
    bool enumerateNamedPipeDirectory(
        const NtFileDirectoryApi& api,
        const QString& directoryPath,
        KernelNamedPipeDirectoryStatus& directoryStatusOut,
        std::vector<KernelNamedPipeEntry>& rowsOut)
    {
        directoryStatusOut = KernelNamedPipeDirectoryStatus{};
        directoryStatusOut.candidatePath = directoryPath;

        IO_STATUS_BLOCK openIoStatus{};
        ScopedHandle directoryHandle;
        const NTSTATUS openStatus = openNamedPipeDirectory(
            api,
            directoryPath,
            directoryHandle.handle,
            openIoStatus);
        directoryStatusOut.lastStatus = static_cast<std::uint32_t>(openStatus);
        directoryStatusOut.statusText = ntStatusToText(api.ntdllModule, openStatus);
        if (!NT_SUCCESS(openStatus))
        {
            directoryStatusOut.openSucceeded = false;
            return false;
        }
        directoryStatusOut.openSucceeded = true;

        ULONG bufferSize = kInitialDirectoryBufferBytes;
        BOOLEAN restartScan = TRUE;
        std::size_t emittedRows = 0;

        for (;;)
        {
            std::vector<std::uint8_t> buffer(bufferSize, 0);
            IO_STATUS_BLOCK queryIoStatus{};
            const NTSTATUS queryStatus = api.queryDirectoryFile(
                directoryHandle.handle,
                nullptr,
                nullptr,
                nullptr,
                &queryIoStatus,
                buffer.data(),
                bufferSize,
                kFileDirectoryInformationClass,
                FALSE,
                nullptr,
                restartScan);
            restartScan = FALSE;

            directoryStatusOut.lastStatus = static_cast<std::uint32_t>(queryStatus);
            directoryStatusOut.statusText = ntStatusToText(api.ntdllModule, queryStatus);

            if (queryStatus == kStatusNoMoreFiles)
            {
                directoryStatusOut.querySucceeded = true;
                directoryStatusOut.statusText = QStringLiteral("%1 (枚举完成)")
                    .arg(ntStatusToText(api.ntdllModule, queryStatus));
                directoryStatusOut.returnedRows = emittedRows;
                return true;
            }

            if (isNeedGrowBufferStatus(queryStatus))
            {
                if (bufferSize >= kMaximumDirectoryBufferBytes)
                {
                    directoryStatusOut.querySucceeded = false;
                    directoryStatusOut.statusText = QStringLiteral("%1 (缓冲区已达上限)")
                        .arg(ntStatusToText(api.ntdllModule, queryStatus));
                    directoryStatusOut.returnedRows = emittedRows;
                    return false;
                }
                bufferSize = std::min<ULONG>(bufferSize * 2U, kMaximumDirectoryBufferBytes);
                continue;
            }

            if (!NT_SUCCESS(queryStatus))
            {
                directoryStatusOut.querySucceeded = false;
                directoryStatusOut.returnedRows = emittedRows;
                return false;
            }

            const ULONG validBytes = static_cast<ULONG>(
                std::min<ULONG_PTR>(
                    static_cast<ULONG_PTR>(queryIoStatus.Information),
                    static_cast<ULONG_PTR>(buffer.size())));
            QString parseErrorText;
            const std::size_t beforeRows = rowsOut.size();
            if (!parseDirectoryBuffer(api, directoryPath, buffer, validBytes, queryStatus, rowsOut, parseErrorText))
            {
                directoryStatusOut.querySucceeded = false;
                directoryStatusOut.statusText = parseErrorText;
                directoryStatusOut.returnedRows = emittedRows;
                return false;
            }

            emittedRows += rowsOut.size() - beforeRows;
            if (emittedRows >= kMaximumPipeRowsPerDirectory)
            {
                directoryStatusOut.querySucceeded = true;
                directoryStatusOut.statusText = QStringLiteral("达到单目录保护上限，结果已截断。");
                directoryStatusOut.returnedRows = emittedRows;
                return true;
            }
        }
    }

    // deduplicateAndSortRows：
    // - 输入 rows：多个候选目录返回的原始行；
    // - 处理逻辑：按 NT 路径大小写折叠去重，再按 pipeName/ntPath 排序；
    // - 返回结果：无，rows 被替换为去重排序后的结果。
    void deduplicateAndSortRows(std::vector<KernelNamedPipeEntry>& rows)
    {
        std::set<QString> seenPathSet;
        std::vector<KernelNamedPipeEntry> uniqueRows;
        uniqueRows.reserve(rows.size());

        for (const KernelNamedPipeEntry& row : rows)
        {
            const QString key = row.ntPath.toCaseFolded();
            if (seenPathSet.find(key) != seenPathSet.end())
            {
                continue;
            }
            seenPathSet.insert(key);
            uniqueRows.push_back(row);
        }

        std::sort(
            uniqueRows.begin(),
            uniqueRows.end(),
            [](const KernelNamedPipeEntry& left, const KernelNamedPipeEntry& right)
            {
                const int nameCompare = QString::compare(left.pipeName, right.pipeName, Qt::CaseInsensitive);
                if (nameCompare != 0)
                {
                    return nameCompare < 0;
                }
                return QString::compare(left.ntPath, right.ntPath, Qt::CaseInsensitive) < 0;
            });

        rows.swap(uniqueRows);
    }
}

KernelNamedPipeSnapshot runKernelNamedPipeSnapshotTask()
{
    const auto beginTime = std::chrono::steady_clock::now();

    KernelNamedPipeSnapshot snapshot;
    NtFileDirectoryApi api;
    if (!loadNtFileDirectoryApi(api, snapshot.errorText))
    {
        snapshot.taskSucceeded = false;
        snapshot.summaryText = snapshot.errorText;
        snapshot.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
        return snapshot;
    }

    std::vector<KernelNamedPipeEntry> collectedRows;
    const std::vector<QString> candidates = buildNamedPipeDirectoryCandidates();
    for (const QString& candidatePath : candidates)
    {
        KernelNamedPipeDirectoryStatus directoryStatus;
        const bool queryOk = enumerateNamedPipeDirectory(api, candidatePath, directoryStatus, collectedRows);
        snapshot.anyQuerySucceeded = snapshot.anyQuerySucceeded || queryOk;
        snapshot.directories.push_back(directoryStatus);
    }

    deduplicateAndSortRows(collectedRows);
    snapshot.rows = std::move(collectedRows);
    snapshot.taskSucceeded = true;
    snapshot.elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beginTime).count());
    snapshot.summaryText = QStringLiteral("候选路径:%1 | 成功路径:%2 | 管道:%3 | %4 ms")
        .arg(static_cast<qulonglong>(snapshot.directories.size()))
        .arg(static_cast<qulonglong>(std::count_if(
            snapshot.directories.begin(),
            snapshot.directories.end(),
            [](const KernelNamedPipeDirectoryStatus& status) { return status.querySucceeded; })))
        .arg(static_cast<qulonglong>(snapshot.rows.size()))
        .arg(static_cast<qulonglong>(snapshot.elapsedMs));
    if (!snapshot.anyQuerySucceeded)
    {
        snapshot.errorText = QStringLiteral("所有 Named Pipe 候选目录均未成功枚举，请查看详情面板中的 NTSTATUS。");
    }
    return snapshot;
}
