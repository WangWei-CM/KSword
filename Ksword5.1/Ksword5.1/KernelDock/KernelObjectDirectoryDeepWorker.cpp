
#include "KernelObjectDirectoryDeepWorker.h"

// ============================================================
// KernelObjectDirectoryDeepWorker.cpp
// 作用说明：
// 1) 动态解析 ntdll 中 Object Manager Directory 查询 API；
// 2) 从指定根目录递归枚举 Directory 对象；
// 3) 对深度、单目录条目数和总条目数做硬限制，避免卡死。
// ============================================================

#include "../Framework.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef DIRECTORY_QUERY
#define DIRECTORY_QUERY 0x0001
#endif
#ifndef DIRECTORY_TRAVERSE
#define DIRECTORY_TRAVERSE 0x0002
#endif

namespace
{
    constexpr NTSTATUS kStatusSuccess = static_cast<NTSTATUS>(0x00000000L);
    constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001L);
    constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
    constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);
    constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
    constexpr NTSTATUS kStatusNoMoreEntries = static_cast<NTSTATUS>(0x8000001AL);

    constexpr ACCESS_MASK kDirectoryQueryAccess = DIRECTORY_QUERY | DIRECTORY_TRAVERSE;
    constexpr ULONG kInitialDirectoryQueryBuffer = 16 * 1024U;
    constexpr int kDefaultMaxDepth = 4;
    constexpr int kHardMaxDepth = 32;
    constexpr std::size_t kDefaultMaxEntriesPerDirectory = 4096;
    constexpr std::size_t kHardMaxEntriesPerDirectory = 65536;
    constexpr std::size_t kDefaultMaxTotalEntries = 50000;
    constexpr std::size_t kHardMaxTotalEntries = 500000;

    using NtOpenDirectoryObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtQueryDirectoryObjectFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);

    // NtDirectoryDeepApi：
    // - 作用：缓存 NtOpenDirectoryObject / NtQueryDirectoryObject 入口。
    struct NtDirectoryDeepApi
    {
        HMODULE ntdllModule = nullptr;
        NtOpenDirectoryObjectFn openDirectoryObject = nullptr;
        NtQueryDirectoryObjectFn queryDirectoryObject = nullptr;
    };

    // ScopedNtHandle：
    // - 作用：托管 NtOpenDirectoryObject 返回的 HANDLE。
    // - 返回行为：析构时自动 CloseHandle，无显式返回值。
    struct ScopedNtHandle
    {
        HANDLE handle = nullptr;

        ScopedNtHandle() = default;
        ScopedNtHandle(const ScopedNtHandle&) = delete;
        ScopedNtHandle& operator=(const ScopedNtHandle&) = delete;

        ~ScopedNtHandle()
        {
            if (handle != nullptr)
            {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        }
    };

    // ObjectDirectoryInformation：
    // - 作用：描述 NtQueryDirectoryObject 返回的一条目录项。
    // - 字段：Name 为对象名，TypeName 为对象类型名。
    struct ObjectDirectoryInformation
    {
        UNICODE_STRING Name;
        UNICODE_STRING TypeName;
    };

    // DirectoryChildRecord：
    // - 作用：保存单目录查询得到的子对象记录，供递归调度使用。
    struct DirectoryChildRecord
    {
        QString objectName;
        QString objectType;
    };

    // PendingDirectory：
    // - 作用：BFS 队列元素，表示等待枚举的 Directory 对象。
    struct PendingDirectory
    {
        QString rootPath;
        QString directoryPath;
        int depth = 0;
    };

    QString normalizeObjectDirectoryPath(const QString& rawPath)
    {
        // 输入：用户输入或递归拼接的对象目录路径。
        // 处理：替换斜杠、压缩重复反斜杠、确保以 "\" 开头。
        // 返回：规范化 Object Manager 路径；空输入返回根目录 "\"。
        QString normalizedPath = rawPath.trimmed();
        normalizedPath.replace('/', '\\');
        while (normalizedPath.contains(QStringLiteral("\\\\")))
        {
            normalizedPath.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }
        if (normalizedPath.isEmpty())
        {
            return QStringLiteral("\\");
        }
        if (!normalizedPath.startsWith('\\'))
        {
            normalizedPath.prepend('\\');
        }
        while (normalizedPath.size() > 1 && normalizedPath.endsWith('\\'))
        {
            normalizedPath.chop(1);
        }
        return normalizedPath;
    }

    QString joinObjectDirectoryPath(const QString& directoryPath, const QString& objectName)
    {
        // 输入：父目录路径和对象名。
        // 处理：按 Object Manager 路径规则拼接，根目录特殊处理。
        // 返回：完整对象路径。
        const QString normalizedDirectory = normalizeObjectDirectoryPath(directoryPath);
        if (normalizedDirectory == QStringLiteral("\\"))
        {
            return QStringLiteral("\\%1").arg(objectName);
        }
        return normalizedDirectory + QStringLiteral("\\") + objectName;
    }

    QString leafNameFromPath(const QString& objectPath)
    {
        // 输入：完整对象路径。
        // 处理：取最后一个反斜杠后的名称，根目录返回 "\"。
        // 返回：可展示的叶子名。
        const QString normalizedPath = normalizeObjectDirectoryPath(objectPath);
        if (normalizedPath == QStringLiteral("\\"))
        {
            return QStringLiteral("\\");
        }
        const int slashIndex = normalizedPath.lastIndexOf('\\');
        return slashIndex >= 0 ? normalizedPath.mid(slashIndex + 1) : normalizedPath;
    }

    QString parentPathFromPath(const QString& objectPath)
    {
        // 输入：完整对象路径。
        // 处理：取父目录路径；根目录或一级对象父路径均返回 "\"。
        // 返回：父目录路径。
        const QString normalizedPath = normalizeObjectDirectoryPath(objectPath);
        if (normalizedPath == QStringLiteral("\\"))
        {
            return QStringLiteral("\\");
        }
        const int slashIndex = normalizedPath.lastIndexOf('\\');
        if (slashIndex <= 0)
        {
            return QStringLiteral("\\");
        }
        return normalizedPath.left(slashIndex);
    }

    QString unicodeStringToQString(const UNICODE_STRING& unicodeText)
    {
        // 输入：NtQueryDirectoryObject 返回的 UNICODE_STRING。
        // 处理：按 Length 字节数转换，不依赖 NUL 结尾。
        // 返回：QString；空 Buffer 或空 Length 返回空字符串。
        if (unicodeText.Buffer == nullptr || unicodeText.Length == 0)
        {
            return QString();
        }
        return QString::fromWCharArray(
            unicodeText.Buffer,
            unicodeText.Length / static_cast<USHORT>(sizeof(wchar_t)));
    }

    void initializeUnicodeString(UNICODE_STRING& unicodeTextOut, const std::wstring& sourceText)
    {
        // 输入：稳定生命周期的 std::wstring。
        // 处理：填充 UNICODE_STRING，长度按 USHORT 上限截断保护。
        // 返回：无；结果写入 unicodeTextOut。
        unicodeTextOut.Buffer = const_cast<PWSTR>(sourceText.c_str());
        unicodeTextOut.Length = static_cast<USHORT>(
            std::min<std::size_t>(
                sourceText.size() * sizeof(wchar_t),
                static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));
        unicodeTextOut.MaximumLength = static_cast<USHORT>(unicodeTextOut.Length + sizeof(wchar_t));
    }

    bool isNeedGrowBufferStatus(const NTSTATUS statusCode)
    {
        // 输入：NtQueryDirectoryObject 返回状态。
        // 返回：true 表示应扩大缓冲区重试。
        return statusCode == kStatusInfoLengthMismatch
            || statusCode == kStatusBufferTooSmall
            || statusCode == kStatusBufferOverflow;
    }

    QString ntStatusToText(const HMODULE ntdllModule, const NTSTATUS statusCode)
    {
        // 输入：NTSTATUS 与 ntdll 模块句柄。
        // 处理：优先用 FormatMessageW 解析 ntdll 消息，失败则仅返回十六进制。
        // 返回：用于 UI 展示的状态文本。
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(statusCode), 8, 16, QChar('0'))
            .toUpper();

        std::array<wchar_t, 256> messageBuffer{};
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            ntdllModule,
            static_cast<DWORD>(statusCode),
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

    bool loadNtDirectoryDeepApi(NtDirectoryDeepApi& apiOut, QString& errorTextOut)
    {
        // 输入：apiOut 为输出结构。
        // 处理：从 ntdll.dll 动态解析目录对象查询 API。
        // 返回：true=解析成功；false=缺少 ntdll 或必要导出。
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

        apiOut.openDirectoryObject = reinterpret_cast<NtOpenDirectoryObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtOpenDirectoryObject"));
        apiOut.queryDirectoryObject = reinterpret_cast<NtQueryDirectoryObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtQueryDirectoryObject"));
        if (apiOut.openDirectoryObject == nullptr || apiOut.queryDirectoryObject == nullptr)
        {
            errorTextOut = QStringLiteral("解析 NtOpenDirectoryObject 或 NtQueryDirectoryObject 失败。");
            return false;
        }
        return true;
    }

    NTSTATUS openDirectoryHandle(
        const NtDirectoryDeepApi& api,
        const QString& directoryPath,
        HANDLE& directoryHandleOut)
    {
        // 输入：api 和 Object Manager 目录路径。
        // 处理：构造 OBJECT_ATTRIBUTES，仅申请 DIRECTORY_QUERY/TRAVERSE。
        // 返回：NtOpenDirectoryObject 的 NTSTATUS；句柄写入 directoryHandleOut。
        directoryHandleOut = nullptr;
        const QString normalizedPath = normalizeObjectDirectoryPath(directoryPath);
        const std::wstring pathWideText = normalizedPath.toStdWString();

        UNICODE_STRING objectPath{};
        initializeUnicodeString(objectPath, pathWideText);

        OBJECT_ATTRIBUTES objectAttributes{};
        InitializeObjectAttributes(&objectAttributes, &objectPath, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        return api.openDirectoryObject(&directoryHandleOut, kDirectoryQueryAccess, &objectAttributes);
    }

    KernelObjectDirectoryDeepOptions sanitizeOptions(const KernelObjectDirectoryDeepOptions& rawOptions)
    {
        // 输入：调用方提供的原始限制参数。
        // 处理：修正空路径、负深度和过大上限。
        // 返回：可安全执行的限制参数。
        KernelObjectDirectoryDeepOptions options = rawOptions;
        options.rootPath = normalizeObjectDirectoryPath(options.rootPath);
        if (options.maxDepth < 0)
        {
            options.maxDepth = kDefaultMaxDepth;
        }
        options.maxDepth = std::min(options.maxDepth, kHardMaxDepth);

        if (options.maxEntriesPerDirectory == 0)
        {
            options.maxEntriesPerDirectory = kDefaultMaxEntriesPerDirectory;
        }
        options.maxEntriesPerDirectory = std::min(
            options.maxEntriesPerDirectory,
            kHardMaxEntriesPerDirectory);

        if (options.maxTotalEntries == 0)
        {
            options.maxTotalEntries = kDefaultMaxTotalEntries;
        }
        options.maxTotalEntries = std::min(options.maxTotalEntries, kHardMaxTotalEntries);
        return options;
    }

    bool appendRowWithTotalLimit(
        KernelObjectDirectoryDeepResult& result,
        const KernelObjectDirectoryDeepOptions& options,
        KernelObjectDirectoryDeepEntry row)
    {
        // 输入：结果对象、安全上限和待写入记录。
        // 处理：总记录数达到上限时拒绝继续写入并标记 totalLimitReached。
        // 返回：true=写入成功；false=触达总上限。
        if (result.rows.size() >= options.maxTotalEntries)
        {
            result.totalLimitReached = true;
            return false;
        }
        result.rows.push_back(std::move(row));
        return true;
    }

    KernelObjectDirectoryDeepEntry makeDirectoryFailureRow(
        const QString& rootPath,
        const QString& directoryPath,
        const int depth,
        const QString& statusText)
    {
        // 输入：失败目录上下文。
        // 处理：构造一条 querySucceeded=false 的 Directory 记录。
        // 返回：可直接写入结果列表的失败记录。
        KernelObjectDirectoryDeepEntry row;
        row.rootPath = rootPath;
        row.directoryPath = parentPathFromPath(directoryPath);
        row.objectName = leafNameFromPath(directoryPath);
        row.objectType = QStringLiteral("Directory");
        row.fullPath = normalizeObjectDirectoryPath(directoryPath);
        row.depth = depth;
        row.statusText = statusText;
        row.querySucceeded = false;
        row.isDirectory = true;
        return row;
    }

    bool enumerateSingleDirectory(
        const NtDirectoryDeepApi& api,
        const QString& directoryPath,
        const std::size_t maxEntriesPerDirectory,
        std::vector<DirectoryChildRecord>& recordsOut,
        NTSTATUS& statusCodeOut,
        bool& perDirectoryLimitReachedOut)
    {
        // 输入：Nt API、目录路径和单目录上限。
        // 处理：逐条调用 NtQueryDirectoryObject，按上限收集 Name/TypeName。
        // 返回：true=目录打开并枚举流程完成；false=打开或查询失败。
        recordsOut.clear();
        statusCodeOut = kStatusUnsuccessful;
        perDirectoryLimitReachedOut = false;

        ScopedNtHandle directoryHandle;
        statusCodeOut = openDirectoryHandle(api, directoryPath, directoryHandle.handle);
        if (!NT_SUCCESS(statusCodeOut))
        {
            return false;
        }

        ULONG queryContext = 0;
        BOOLEAN restartScan = TRUE;
        ULONG queryBufferSize = kInitialDirectoryQueryBuffer;

        for (std::size_t entryIndex = 0; entryIndex < maxEntriesPerDirectory; ++entryIndex)
        {
            std::vector<std::uint8_t> queryBuffer;
            NTSTATUS queryStatus = kStatusUnsuccessful;
            ULONG returnLength = 0;
            bool queryFinished = false;

            for (int retryIndex = 0; retryIndex < 6; ++retryIndex)
            {
                queryBuffer.assign(queryBufferSize, 0);
                queryStatus = api.queryDirectoryObject(
                    directoryHandle.handle,
                    queryBuffer.data(),
                    queryBufferSize,
                    TRUE,
                    restartScan,
                    &queryContext,
                    &returnLength);

                if (queryStatus == kStatusNoMoreEntries)
                {
                    statusCodeOut = kStatusSuccess;
                    return true;
                }
                if (isNeedGrowBufferStatus(queryStatus))
                {
                    queryBufferSize = std::max(queryBufferSize * 2U, returnLength + 512U);
                    continue;
                }

                queryFinished = true;
                break;
            }

            restartScan = FALSE;
            if (!queryFinished)
            {
                statusCodeOut = queryStatus;
                return false;
            }
            if (!NT_SUCCESS(queryStatus))
            {
                statusCodeOut = queryStatus;
                return false;
            }
            if (queryBuffer.size() < sizeof(ObjectDirectoryInformation))
            {
                statusCodeOut = kStatusBufferTooSmall;
                return false;
            }

            const auto* recordInfo = reinterpret_cast<const ObjectDirectoryInformation*>(queryBuffer.data());
            DirectoryChildRecord record;
            record.objectName = unicodeStringToQString(recordInfo->Name).trimmed();
            record.objectType = unicodeStringToQString(recordInfo->TypeName).trimmed();
            if (!record.objectName.isEmpty())
            {
                recordsOut.push_back(std::move(record));
            }
        }

        perDirectoryLimitReachedOut = true;
        statusCodeOut = kStatusSuccess;
        return true;
    }
}

KernelObjectDirectoryDeepResult runKernelObjectDirectoryDeepSnapshotTask(
    const KernelObjectDirectoryDeepOptions& rawOptions)
{
    KernelObjectDirectoryDeepResult result;
    const KernelObjectDirectoryDeepOptions options = sanitizeOptions(rawOptions);
    result.normalizedRootPath = options.rootPath;

    NtDirectoryDeepApi api;
    if (!loadNtDirectoryDeepApi(api, result.errorText))
    {
        result.success = false;
        return result;
    }

    std::queue<PendingDirectory> pendingDirectories;
    pendingDirectories.push(PendingDirectory{ options.rootPath, options.rootPath, 0 });

    while (!pendingDirectories.empty() && !result.totalLimitReached)
    {
        const PendingDirectory currentDirectory = pendingDirectories.front();
        pendingDirectories.pop();

        std::vector<DirectoryChildRecord> childRecords;
        NTSTATUS queryStatus = kStatusUnsuccessful;
        bool perDirectoryLimitReached = false;
        const bool queryOk = enumerateSingleDirectory(
            api,
            currentDirectory.directoryPath,
            options.maxEntriesPerDirectory,
            childRecords,
            queryStatus,
            perDirectoryLimitReached);

        ++result.visitedDirectoryCount;
        if (!queryOk)
        {
            ++result.failedDirectoryCount;
            appendRowWithTotalLimit(
                result,
                options,
                makeDirectoryFailureRow(
                    currentDirectory.rootPath,
                    currentDirectory.directoryPath,
                    currentDirectory.depth,
                    QStringLiteral("目录访问失败：%1").arg(ntStatusToText(api.ntdllModule, queryStatus))));
            continue;
        }

        if (perDirectoryLimitReached)
        {
            result.perDirectoryLimitReached = true;
        }

        for (const DirectoryChildRecord& childRecord : childRecords)
        {
            const bool childIsDirectory =
                childRecord.objectType.compare(QStringLiteral("Directory"), Qt::CaseInsensitive) == 0;
            const QString childFullPath = joinObjectDirectoryPath(
                currentDirectory.directoryPath,
                childRecord.objectName);

            KernelObjectDirectoryDeepEntry row;
            row.rootPath = currentDirectory.rootPath;
            row.directoryPath = currentDirectory.directoryPath;
            row.objectName = childRecord.objectName;
            row.objectType = childRecord.objectType;
            row.fullPath = childFullPath;
            row.depth = currentDirectory.depth;
            row.querySucceeded = true;
            row.isDirectory = childIsDirectory;
            row.statusText = childIsDirectory
                ? QStringLiteral("Directory 已发现")
                : QStringLiteral("叶子对象");

            if (!appendRowWithTotalLimit(result, options, std::move(row)))
            {
                break;
            }

            if (!childIsDirectory)
            {
                continue;
            }
            if (currentDirectory.depth >= options.maxDepth)
            {
                result.depthLimitReached = true;
                continue;
            }
            pendingDirectories.push(PendingDirectory{
                currentDirectory.rootPath,
                childFullPath,
                currentDirectory.depth + 1
                });
        }
    }

    if (result.totalLimitReached)
    {
        KernelObjectDirectoryDeepEntry limitRow;
        limitRow.rootPath = options.rootPath;
        limitRow.directoryPath = options.rootPath;
        limitRow.objectName = QStringLiteral("<TotalLimit>");
        limitRow.objectType = QStringLiteral("Diagnostic");
        limitRow.fullPath = options.rootPath;
        limitRow.depth = 0;
        limitRow.statusText = QStringLiteral("达到总条目上限 %1，递归已停止。").arg(options.maxTotalEntries);
        limitRow.querySucceeded = false;
        limitRow.isDirectory = false;
        if (result.rows.size() < options.maxTotalEntries)
        {
            result.rows.push_back(std::move(limitRow));
        }
    }

    result.success = true;
    return result;
}
