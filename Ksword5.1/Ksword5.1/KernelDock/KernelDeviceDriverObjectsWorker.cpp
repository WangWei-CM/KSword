
#include "KernelDeviceDriverObjectsWorker.h"

// ============================================================
// KernelDeviceDriverObjectsWorker.cpp
// 作用说明：
// 1) 仅使用 R3 对象管理器 API 枚举 \Device / \Driver / \FileSystem /
//    \FileSystem\Filters 四个目录；
// 2) 对符号链接对象尝试解析目标路径，用于理解设备路径与驱动入口；
// 3) 把目录对象、驱动对象、文件系统对象与符号链接对象统一整理为只读行。
// ============================================================

#include "../Framework.h"

#include <QStringList>

#include <algorithm> // std::max/std::sort：排序与扩容。
#include <array>     // std::array：FormatMessage 固定缓冲。
#include <cstdint>   // std::uint8_t：原始缓冲字节。
#include <limits>    // std::numeric_limits：UNION_STRING 长度约束。
#include <utility>   // std::move：移动枚举结果条目。
#include <vector>    // std::vector：枚举结果与临时缓冲。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// ============================================================
// 兼容性补齐：
// - 某些 SDK 组合没有导出对象目录/符号链接访问掩码；
// - 这里按对象管理器常量补齐，避免头文件差异导致的编译风险。
// ============================================================
#ifndef DIRECTORY_QUERY
#define DIRECTORY_QUERY 0x0001
#endif
#ifndef DIRECTORY_TRAVERSE
#define DIRECTORY_TRAVERSE 0x0002
#endif
#ifndef SYMBOLIC_LINK_QUERY
#define SYMBOLIC_LINK_QUERY 0x0001
#endif

namespace
{
    // ============================================================
    // 任务常量：
    // - 统一控制枚举缓冲区和单目录最大返回数量；
    // - 这里保持保守上限，避免 UI 被异常大量对象拖慢。
    // ============================================================
    constexpr NTSTATUS kStatusSuccess = static_cast<NTSTATUS>(0x00000000L);
    constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001L);
    constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
    constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);
    constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
    constexpr NTSTATUS kStatusNoMoreEntries = static_cast<NTSTATUS>(0x8000001AL);

    constexpr std::size_t kMaxEntriesPerDirectory = 4096; // 单目录最大枚举数。
    constexpr ULONG kInitialDirectoryQueryBuffer = 16 * 1024U; // 目录初始缓冲大小。
    constexpr ULONG kInitialSymbolicLinkChars = 1024U; // 符号链接初始缓冲字符数。

    constexpr ACCESS_MASK kDirectoryQueryAccess = DIRECTORY_QUERY | DIRECTORY_TRAVERSE;
    constexpr ACCESS_MASK kSymbolicLinkQueryAccess = SYMBOLIC_LINK_QUERY;

    // ============================================================
    // Nt API 函数指针：
    // - 采用动态装载，避免对链接阶段的额外依赖；
    // - 仅需要目录枚举和符号链接解析两个能力。
    // ============================================================
    using NtOpenDirectoryObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtQueryDirectoryObjectFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
    using NtOpenSymbolicLinkObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtQuerySymbolicLinkObjectFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);

    // ============================================================
    // NtKernelObjectApi
    // - 缓存 ntdll 模块句柄与所需入口点；
    // - 仅承载本 worker 使用的 R3 API。
    // ============================================================
    struct NtKernelObjectApi
    {
        HMODULE ntdllModule = nullptr;                           // ntdllModule：ntdll.dll 句柄。
        NtOpenDirectoryObjectFn openDirectoryObject = nullptr;   // openDirectoryObject：NtOpenDirectoryObject。
        NtQueryDirectoryObjectFn queryDirectoryObject = nullptr; // queryDirectoryObject：NtQueryDirectoryObject。
        NtOpenSymbolicLinkObjectFn openSymbolicLinkObject = nullptr;   // openSymbolicLinkObject：NtOpenSymbolicLinkObject。
        NtQuerySymbolicLinkObjectFn querySymbolicLinkObject = nullptr;  // querySymbolicLinkObject：NtQuerySymbolicLinkObject。
    };

    // ============================================================
    // ScopedNtHandle
    // - 作用：自动关闭 Nt 打开的对象句柄；
    // - 返回：析构时无返回值，只负责释放资源。
    // ============================================================
    struct ScopedNtHandle
    {
        HANDLE handle = nullptr; // handle：托管句柄。

        ~ScopedNtHandle()
        {
            if (handle != nullptr)
            {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        }

        ScopedNtHandle() = default;
        ScopedNtHandle(const ScopedNtHandle&) = delete;
        ScopedNtHandle& operator=(const ScopedNtHandle&) = delete;
    };

    // ============================================================
    // KsObjectDirectoryInformation
    // - 作用：匹配 NtQueryDirectoryObject 返回布局；
    // - 说明：只需 Name 与 TypeName 两个字段即可满足当前任务。
    // ============================================================
    struct KsObjectDirectoryInformation
    {
        UNICODE_STRING Name;
        UNICODE_STRING TypeName;
    };

    // ============================================================
    // RootSpec
    // - 作用：描述一次根目录枚举任务；
    // - 仅包含输入路径、展示标题和语义说明，不涉及任何写操作。
    // ============================================================
    struct RootSpec
    {
        QString directoryPathText;  // directoryPathText：实际枚举的对象目录路径。
        QString displayNameText;    // displayNameText：UI 中的目录标题。
        QString descriptionText;    // descriptionText：目录用途说明。
    };

    // normalizePathText：
    // - 输入：原始对象路径；
    // - 处理：去空白、统一斜杠并保证以反斜杠开头；
    // - 返回：标准化后的对象路径文本。
    QString normalizePathText(const QString& rawPathText)
    {
        QString normalizedPathText = rawPathText.trimmed();
        normalizedPathText.replace('/', '\\');

        while (normalizedPathText.contains(QStringLiteral("\\\\")))
        {
            normalizedPathText.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }

        if (normalizedPathText.isEmpty())
        {
            return QStringLiteral("\\");
        }

        if (!normalizedPathText.startsWith('\\'))
        {
            normalizedPathText.prepend('\\');
        }
        return normalizedPathText;
    }

    // joinObjectPath：
    // - 输入：目录路径与对象名；
    // - 处理：按对象管理器路径规则拼接；
    // - 返回：完整对象路径。
    QString joinObjectPath(const QString& directoryPathText, const QString& objectNameText)
    {
        const QString normalizedDirectoryPathText = normalizePathText(directoryPathText);
        if (normalizedDirectoryPathText == QStringLiteral("\\"))
        {
            return QStringLiteral("\\%1").arg(objectNameText);
        }
        if (normalizedDirectoryPathText.endsWith('\\'))
        {
            return normalizedDirectoryPathText + objectNameText;
        }
        return normalizedDirectoryPathText + QStringLiteral("\\") + objectNameText;
    }

    // unicodeStringToQString：
    // - 输入：NT API 返回的 UNICODE_STRING；
    // - 处理：按字符长度转换为 Qt 字符串；
    // - 返回：去掉空值后的 QString。
    QString unicodeStringToQString(const UNICODE_STRING& unicodeText)
    {
        if (unicodeText.Buffer == nullptr || unicodeText.Length == 0)
        {
            return QString();
        }

        return QString::fromWCharArray(
            unicodeText.Buffer,
            unicodeText.Length / static_cast<USHORT>(sizeof(wchar_t)));
    }

    // initializeUnicodeString：
    // - 输入：std::wstring 源文本；
    // - 处理：把原始 wide 字符串填入 UNICODE_STRING；
    // - 返回：无返回值，结果写入 unicodeTextOut。
    void initializeUnicodeString(UNICODE_STRING& unicodeTextOut, const std::wstring& sourceText)
    {
        unicodeTextOut.Buffer = const_cast<PWSTR>(sourceText.c_str());
        unicodeTextOut.Length = static_cast<USHORT>(
            std::min<std::size_t>(
                sourceText.size() * sizeof(wchar_t),
                static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));
        unicodeTextOut.MaximumLength = static_cast<USHORT>(unicodeTextOut.Length + sizeof(wchar_t));
    }

    // isNeedGrowBufferStatus：
    // - 输入：NTSTATUS；
    // - 处理：判断是否属于“缓冲不足，需要扩容重试”；
    // - 返回：true 表示应扩大缓冲区。
    bool isNeedGrowBufferStatus(const NTSTATUS statusCode)
    {
        return statusCode == kStatusInfoLengthMismatch
            || statusCode == kStatusBufferOverflow
            || statusCode == kStatusBufferTooSmall;
    }

    // ntStatusToText：
    // - 输入：ntdll 模块句柄和 NTSTATUS；
    // - 处理：组合十六进制码与系统消息，便于诊断；
    // - 返回：可读状态文本。
    QString ntStatusToText(const HMODULE ntdllModule, const NTSTATUS statusCode)
    {
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

    // loadNtKernelObjectApi：
    // - 输入：无；
    // - 处理：装载 ntdll.dll 并解析对象目录/符号链接相关入口；
    // - 返回：true 表示可继续枚举；false 表示致命错误，errorTextOut 会写入原因。
    bool loadNtKernelObjectApi(NtKernelObjectApi& apiOut, QString& errorTextOut)
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

        apiOut.openDirectoryObject = reinterpret_cast<NtOpenDirectoryObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtOpenDirectoryObject"));
        apiOut.queryDirectoryObject = reinterpret_cast<NtQueryDirectoryObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtQueryDirectoryObject"));
        apiOut.openSymbolicLinkObject = reinterpret_cast<NtOpenSymbolicLinkObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtOpenSymbolicLinkObject"));
        apiOut.querySymbolicLinkObject = reinterpret_cast<NtQuerySymbolicLinkObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtQuerySymbolicLinkObject"));

        if (apiOut.openDirectoryObject == nullptr
            || apiOut.queryDirectoryObject == nullptr
            || apiOut.openSymbolicLinkObject == nullptr
            || apiOut.querySymbolicLinkObject == nullptr)
        {
            errorTextOut = QStringLiteral("解析 NtOpen/NtQuery Directory 或 SymbolicLink API 失败。");
            return false;
        }
        return true;
    }

    // openDirectoryHandle：
    // - 输入：对象目录路径；
    // - 处理：使用 NtOpenDirectoryObject 申请只读查询权限；
    // - 返回：NTSTATUS，成功时 directoryHandleOut 持有有效句柄。
    NTSTATUS openDirectoryHandle(
        const NtKernelObjectApi& api,
        const QString& directoryPathText,
        HANDLE& directoryHandleOut)
    {
        directoryHandleOut = nullptr;

        const QString normalizedPathText = normalizePathText(directoryPathText);
        const std::wstring pathWideText = normalizedPathText.toStdWString();

        UNICODE_STRING objectPath{};
        initializeUnicodeString(objectPath, pathWideText);

        OBJECT_ATTRIBUTES objectAttributes{};
        InitializeObjectAttributes(&objectAttributes, &objectPath, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

        return api.openDirectoryObject(&directoryHandleOut, kDirectoryQueryAccess, &objectAttributes);
    }

    // openSymbolicLinkHandle：
    // - 输入：符号链接完整路径；
    // - 处理：按对象路径打开符号链接句柄；
    // - 返回：NTSTATUS，成功时 symbolicLinkHandleOut 持有有效句柄。
    NTSTATUS openSymbolicLinkHandle(
        const NtKernelObjectApi& api,
        const QString& symbolicLinkPathText,
        HANDLE& symbolicLinkHandleOut)
    {
        symbolicLinkHandleOut = nullptr;

        const QString normalizedPathText = normalizePathText(symbolicLinkPathText);
        const std::wstring pathWideText = normalizedPathText.toStdWString();

        UNICODE_STRING objectPath{};
        initializeUnicodeString(objectPath, pathWideText);

        OBJECT_ATTRIBUTES objectAttributes{};
        InitializeObjectAttributes(&objectAttributes, &objectPath, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

        return api.openSymbolicLinkObject(&symbolicLinkHandleOut, kSymbolicLinkQueryAccess, &objectAttributes);
    }

    // querySymbolicLinkTargetInternal：
    // - 输入：已加载的 API、符号链接路径；
    // - 处理：打开符号链接并读取目标路径，失败时保留状态文本；
    // - 返回：true 表示目标解析成功，false 表示失败。
    bool querySymbolicLinkTargetInternal(
        const NtKernelObjectApi& api,
        const QString& symbolicLinkPathText,
        QString& targetTextOut,
        QString& statusTextOut)
    {
        targetTextOut.clear();
        statusTextOut.clear();

        ScopedNtHandle symbolicLinkHandle;
        const NTSTATUS openStatus = openSymbolicLinkHandle(api, symbolicLinkPathText, symbolicLinkHandle.handle);
        if (!NT_SUCCESS(openStatus))
        {
            statusTextOut = ntStatusToText(api.ntdllModule, openStatus);
            return false;
        }

        ULONG targetChars = kInitialSymbolicLinkChars;
        for (int retryIndex = 0; retryIndex < 6; ++retryIndex)
        {
            std::vector<wchar_t> targetBuffer(targetChars, L'\0');
            UNICODE_STRING targetUnicode{};
            targetUnicode.Buffer = targetBuffer.data();
            targetUnicode.Length = 0;
            targetUnicode.MaximumLength = static_cast<USHORT>(
                std::min<std::size_t>(
                    targetChars * sizeof(wchar_t),
                    static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));

            ULONG returnLength = 0;
            const NTSTATUS queryStatus = api.querySymbolicLinkObject(
                symbolicLinkHandle.handle,
                &targetUnicode,
                &returnLength);

            if (NT_SUCCESS(queryStatus))
            {
                targetTextOut = QString::fromWCharArray(
                    targetUnicode.Buffer,
                    targetUnicode.Length / static_cast<USHORT>(sizeof(wchar_t)));
                statusTextOut = ntStatusToText(api.ntdllModule, queryStatus);
                return true;
            }

            if (!isNeedGrowBufferStatus(queryStatus))
            {
                statusTextOut = ntStatusToText(api.ntdllModule, queryStatus);
                return false;
            }

            targetChars = std::max<ULONG>(
                targetChars * 2U,
                static_cast<ULONG>(returnLength / sizeof(wchar_t) + 8U));
        }

        statusTextOut = QStringLiteral("符号链接目标解析重试达到上限。");
        return false;
    }

    // enumerateDirectoryEntries：
    // - 输入：API 句柄与目录路径；
    // - 处理：循环调用 NtQueryDirectoryObject，逐条提取对象名和类型；
    // - 返回：true 表示目录枚举完成，false 表示中途遇到致命错误。
    bool enumerateDirectoryEntries(
        const NtKernelObjectApi& api,
        const QString& directoryPathText,
        std::vector<KernelDeviceDriverObjectEntry>& rowsOut,
        NTSTATUS& statusCodeOut,
        bool& truncatedOut)
    {
        rowsOut.clear();
        statusCodeOut = kStatusUnsuccessful;
        truncatedOut = false;

        ScopedNtHandle directoryHandle;
        statusCodeOut = openDirectoryHandle(api, directoryPathText, directoryHandle.handle);
        if (!NT_SUCCESS(statusCodeOut))
        {
            return false;
        }

        ULONG queryContext = 0;
        BOOLEAN restartScan = TRUE;
        ULONG queryBufferSize = kInitialDirectoryQueryBuffer;

        for (std::size_t entryIndex = 0; entryIndex < kMaxEntriesPerDirectory; ++entryIndex)
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

            if (queryBuffer.size() < sizeof(KsObjectDirectoryInformation))
            {
                statusCodeOut = kStatusBufferTooSmall;
                return false;
            }

            const auto* recordInfo = reinterpret_cast<const KsObjectDirectoryInformation*>(queryBuffer.data());
            const QString objectNameText = unicodeStringToQString(recordInfo->Name).trimmed();
            const QString objectTypeText = unicodeStringToQString(recordInfo->TypeName).trimmed();
            if (objectNameText.isEmpty())
            {
                continue;
            }

            KernelDeviceDriverObjectEntry entry;
            entry.directoryPathText = normalizePathText(directoryPathText);
            entry.objectNameText = objectNameText;
            entry.objectTypeText = objectTypeText.isEmpty() ? QStringLiteral("<未知>") : objectTypeText;
            entry.fullPathText = joinObjectPath(entry.directoryPathText, entry.objectNameText);
            entry.querySucceeded = true;
            entry.statusCode = queryStatus;
            entry.isDirectory = entry.objectTypeText.compare(QStringLiteral("Directory"), Qt::CaseInsensitive) == 0;
            entry.isSymbolicLink = entry.objectTypeText.compare(QStringLiteral("SymbolicLink"), Qt::CaseInsensitive) == 0;
            entry.statusText = QStringLiteral("已枚举");
            entry.capabilityHintText = QStringLiteral("叶子对象，建议查属性");

            if (entry.isDirectory)
            {
                entry.capabilityHintText = QStringLiteral("可用目录递归继续展开（本 tab 不做深递归）");
            }
            else if (entry.isSymbolicLink)
            {
                QString targetText;
                QString targetStatusText;
                if (querySymbolicLinkTargetInternal(api, entry.fullPathText, targetText, targetStatusText))
                {
                    entry.targetPathText = targetText;
                    entry.statusText = QStringLiteral("已枚举，符号链接目标已解析");
                    entry.capabilityHintText = QStringLiteral("可解析符号链接，用于理解设备路径");
                }
                else
                {
                    entry.targetPathText = QString();
                    entry.statusText = QStringLiteral("已枚举，符号链接目标解析失败：%1").arg(targetStatusText);
                    entry.capabilityHintText = QStringLiteral("可解析符号链接，用于理解设备路径");
                }
            }
            else if (entry.objectTypeText.compare(QStringLiteral("Device"), Qt::CaseInsensitive) == 0)
            {
                entry.capabilityHintText = QStringLiteral("叶子对象，建议查属性");
            }
            else if (entry.objectTypeText.compare(QStringLiteral("Driver"), Qt::CaseInsensitive) == 0)
            {
                entry.capabilityHintText = QStringLiteral("叶子对象，建议结合服务名和加载顺序分析");
            }
            else if (entry.objectTypeText.compare(QStringLiteral("FileSystem"), Qt::CaseInsensitive) == 0)
            {
                entry.capabilityHintText = QStringLiteral("叶子对象，建议结合挂载卷与过滤器链路分析");
            }

            entry.detailText = QStringLiteral(
                "目录路径: %1\n"
                "对象名称: %2\n"
                "对象类型: %3\n"
                "完整路径: %4\n"
                "符号链接目标: %5\n"
                "状态: %6\n"
                "能力提示: %7")
                .arg(
                    entry.directoryPathText,
                    entry.objectNameText,
                    entry.objectTypeText,
                    entry.fullPathText,
                    entry.targetPathText.isEmpty() ? QStringLiteral("<无>") : entry.targetPathText,
                    entry.statusText,
                    entry.capabilityHintText);
            rowsOut.push_back(std::move(entry));
        }

        truncatedOut = true;
        statusCodeOut = kStatusSuccess;
        return true;
    }

    // buildScopeEntry：
    // - 输入：目录规格、枚举状态和是否成功；
    // - 处理：生成一条只读“目录范围”说明行，便于 UI 了解当前分组；
    // - 返回：可直接加入结果集的条目。
    KernelDeviceDriverObjectEntry buildScopeEntry(
        const RootSpec& rootSpec,
        const NTSTATUS statusCode,
        const bool querySucceeded)
    {
        KernelDeviceDriverObjectEntry entry;
        entry.directoryPathText = normalizePathText(rootSpec.directoryPathText);
        entry.objectNameText = entry.directoryPathText.section('\\', -1, -1).trimmed();
        if (entry.objectNameText.isEmpty())
        {
            entry.objectNameText = entry.directoryPathText;
        }
        entry.objectTypeText = QStringLiteral("Directory");
        entry.fullPathText = entry.directoryPathText;
        entry.targetPathText = QString();
        entry.statusCode = statusCode;
        entry.querySucceeded = querySucceeded;
        entry.isDirectory = true;
        entry.isSymbolicLink = false;
        entry.isScopeEntry = true;
        entry.statusText = querySucceeded ? QStringLiteral("目录已打开，可继续枚举") : ntStatusToText(::GetModuleHandleW(L"ntdll.dll"), statusCode);
        entry.capabilityHintText = QStringLiteral("可用目录递归继续展开（本 tab 不做深递归）");
        entry.detailText = QStringLiteral(
            "目录路径: %1\n"
            "目录标题: %2\n"
            "目录说明: %3\n"
            "状态: %4\n"
            "能力提示: %5")
            .arg(
                entry.directoryPathText,
                rootSpec.displayNameText,
                rootSpec.descriptionText,
                entry.statusText,
                entry.capabilityHintText);
        return entry;
    }

    // buildRootSpecList：
    // - 输入：无；
    // - 处理：返回本专项视图要枚举的四个对象目录；
    // - 返回：目录规格列表。
    std::vector<RootSpec> buildRootSpecList()
    {
        return {
            {
                QStringLiteral("\\Device"),
                QStringLiteral("Device"),
                QStringLiteral("设备对象与设备符号链接"),
            },
            {
                QStringLiteral("\\Driver"),
                QStringLiteral("Driver"),
                QStringLiteral("已加载驱动对象"),
            },
            {
                QStringLiteral("\\FileSystem"),
                QStringLiteral("FileSystem"),
                QStringLiteral("文件系统驱动对象"),
            },
            {
                QStringLiteral("\\FileSystem\\Filters"),
                QStringLiteral("FileSystem\\Filters"),
                QStringLiteral("文件系统过滤器对象"),
            },
        };
    }
}

// runKernelDeviceDriverObjectsSnapshotTask：
// - 输入：rowsOut 接收全部只读枚举结果；errorTextOut 接收致命错误原因；
// - 处理：枚举四个对象目录并对符号链接做目标解析，所有信息只用于诊断展示；
// - 返回：true 表示成功完成枚举；false 表示 ntdll 装载等不可恢复错误。
bool runKernelDeviceDriverObjectsSnapshotTask(
    std::vector<KernelDeviceDriverObjectEntry>& rowsOut,
    QString& errorTextOut)
{
    rowsOut.clear();
    errorTextOut.clear();

    kLogEvent taskEvent;
    info << taskEvent << "[KernelDeviceDriverObjectsWorker] 开始枚举设备与驱动对象。" << eol;

    NtKernelObjectApi api;
    if (!loadNtKernelObjectApi(api, errorTextOut))
    {
        err << taskEvent
            << "[KernelDeviceDriverObjectsWorker] Nt API 装载失败: "
            << errorTextOut.toStdString()
            << eol;
        return false;
    }

    std::vector<KernelDeviceDriverObjectEntry> resultRows;
    const std::vector<RootSpec> rootSpecList = buildRootSpecList();

    for (const RootSpec& rootSpec : rootSpecList)
    {
        std::vector<KernelDeviceDriverObjectEntry> directoryRows;
        NTSTATUS directoryStatus = kStatusUnsuccessful;
        bool truncated = false;
        const bool enumerateOk = enumerateDirectoryEntries(
            api,
            rootSpec.directoryPathText,
            directoryRows,
            directoryStatus,
            truncated);

        resultRows.push_back(buildScopeEntry(rootSpec, directoryStatus, enumerateOk));

        if (!enumerateOk)
        {
            continue;
        }

        std::sort(directoryRows.begin(), directoryRows.end(), [](const KernelDeviceDriverObjectEntry& left, const KernelDeviceDriverObjectEntry& right) {
            if (left.isScopeEntry != right.isScopeEntry)
            {
                return left.isScopeEntry && !right.isScopeEntry;
            }

            const int typeCompare = QString::compare(left.objectTypeText, right.objectTypeText, Qt::CaseInsensitive);
            if (typeCompare != 0)
            {
                return typeCompare < 0;
            }

            return QString::compare(left.objectNameText, right.objectNameText, Qt::CaseInsensitive) < 0;
        });

        for (KernelDeviceDriverObjectEntry& entry : directoryRows)
        {
            entry.detailText = QStringLiteral(
                "目录路径: %1\n"
                "对象名称: %2\n"
                "对象类型: %3\n"
                "完整路径: %4\n"
                "符号链接目标: %5\n"
                "状态: %6\n"
                "能力提示: %7")
                .arg(
                    entry.directoryPathText,
                    entry.objectNameText,
                    entry.objectTypeText,
                    entry.fullPathText,
                    entry.targetPathText.isEmpty() ? QStringLiteral("<无>") : entry.targetPathText,
                    entry.statusText,
                    entry.capabilityHintText);
            resultRows.push_back(std::move(entry));
        }

        if (truncated)
        {
            KernelDeviceDriverObjectEntry truncatedEntry;
            truncatedEntry.directoryPathText = normalizePathText(rootSpec.directoryPathText);
            truncatedEntry.objectNameText = QStringLiteral("<已截断>");
            truncatedEntry.objectTypeText = QStringLiteral("<Info>");
            truncatedEntry.fullPathText = rootSpec.directoryPathText;
            truncatedEntry.targetPathText = QString();
            truncatedEntry.statusCode = kStatusSuccess;
            truncatedEntry.querySucceeded = true;
            truncatedEntry.isDirectory = false;
            truncatedEntry.isSymbolicLink = false;
            truncatedEntry.isScopeEntry = false;
            truncatedEntry.statusText = QStringLiteral("达到单目录上限 %1 项").arg(kMaxEntriesPerDirectory);
            truncatedEntry.capabilityHintText = QStringLiteral("可继续筛选当前结果，避免一次性加载更大范围");
            truncatedEntry.detailText = QStringLiteral(
                "目录路径: %1\n"
                "提示: 为避免 UI 卡顿，单目录结果已截断到 %2 项。")
                .arg(
                    truncatedEntry.directoryPathText,
                    QString::number(kMaxEntriesPerDirectory));
            resultRows.push_back(std::move(truncatedEntry));
        }
    }

    std::sort(resultRows.begin(), resultRows.end(), [](const KernelDeviceDriverObjectEntry& left, const KernelDeviceDriverObjectEntry& right) {
        const int directoryCompare = QString::compare(left.directoryPathText, right.directoryPathText, Qt::CaseInsensitive);
        if (directoryCompare != 0)
        {
            return directoryCompare < 0;
        }

        if (left.isScopeEntry != right.isScopeEntry)
        {
            return left.isScopeEntry && !right.isScopeEntry;
        }

        const int typeCompare = QString::compare(left.objectTypeText, right.objectTypeText, Qt::CaseInsensitive);
        if (typeCompare != 0)
        {
            return typeCompare < 0;
        }

        return QString::compare(left.objectNameText, right.objectNameText, Qt::CaseInsensitive) < 0;
    });

    rowsOut = std::move(resultRows);

    std::size_t failedCount = 0;
    for (const KernelDeviceDriverObjectEntry& row : rowsOut)
    {
        if (!row.querySucceeded)
        {
            ++failedCount;
        }
    }

    info << taskEvent
        << "[KernelDeviceDriverObjectsWorker] 枚举完成, rowCount="
        << rowsOut.size()
        << ", failedCount="
        << failedCount
        << eol;
    return true;
}
