/*++

Module Name:

    driver_unload.c

Abstract:

    Force DriverObject unload by name.

Environment:

    Kernel-mode Driver Framework

--*/

#define KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL 1
#include "../callback/callback_external_core.h"

#include "ark/ark_driver.h"
#include "driver_integrity.h"

#include <ntstrsafe.h>

/* 中文说明：默认等待 DriverUnload 系统线程 3 秒，避免 UI 无限阻塞。 */
#define KSW_DRIVER_UNLOAD_DEFAULT_TIMEOUT_MS 3000UL
/* 中文说明：最大等待 30 秒，防止恶意/异常 DriverUnload 挂死调用者。 */
#define KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS 30000UL
/* 中文说明：卸载线程上下文使用独立 tag，便于 pool 泄漏排查。 */
#define KSW_DRIVER_UNLOAD_TAG 'uDsK'
/* 中文说明：对象目录枚举缓冲 tag，用于服务名和 DriverObject 名不一致时兜底。 */
#define KSW_DRIVER_UNLOAD_DIRECTORY_TAG 'dDsK'
/* 中文说明：DeviceObject 清理最多遍历 128 个节点，避免损坏链表造成无限循环。 */
#define KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT 128UL
/* 中文说明：对象目录单条查询缓冲 16KB，足够容纳异常长对象名。 */
#define KSW_DRIVER_UNLOAD_DIRECTORY_QUERY_BYTES (16UL * 1024UL)
/* 中文说明：每个目录最多扫描 4096 项，避免异常对象目录导致长时间占用 IOCTL。 */
#define KSW_DRIVER_UNLOAD_DIRECTORY_MAX_ENTRIES 4096UL
/* 中文说明：按模块基址强力清理最多尝试 256 个回调，防止异常枚举导致 IOCTL 长时间占用。 */
#define KSW_DRIVER_UNLOAD_MAX_CALLBACK_CLEANUP_COUNT 256UL

/* 中文说明：SystemModuleInformation 用于把回调地址映射到内核模块基址。 */
#define KSW_DRIVER_UNLOAD_SYSTEM_MODULE_CLASS 11UL
/* 中文说明：强卸载前最多检查 128 个 DeviceObject，和删除上限保持一致。 */
#define KSW_DRIVER_UNLOAD_PREFLIGHT_DEVICE_LIMIT KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT
/* 中文说明：释放最后一个 DriverObject 引用后短暂重试，等待对象管理器/loader 完成同步清理。 */
#define KSW_DRIVER_UNLOAD_POST_VERIFY_RETRIES 5UL
/* 中文说明：每次闭环验证之间等待 20ms，避免 IOCTL 长时间占用。 */
#define KSW_DRIVER_UNLOAD_POST_VERIFY_DELAY_MS 20UL

#ifndef STATUS_REQUEST_NOT_ACCEPTED
/* 中文说明：旧 WDK 头缺失时补齐策略拒绝状态码，用于 preflight 拒绝。 */
#define STATUS_REQUEST_NOT_ACCEPTED ((NTSTATUS)0xC00000D0L)
#endif

#ifndef STATUS_DRIVER_BLOCKED_CRITICAL
/* 中文说明：旧 WDK 头缺失时补齐核心驱动拒绝状态码。 */
#define STATUS_DRIVER_BLOCKED_CRITICAL ((NTSTATUS)0xC000036BL)
#endif

#ifndef THREAD_ALL_ACCESS
/* 中文说明：旧 WDK 头缺失时补齐线程全访问掩码，供 PsCreateSystemThread 使用。 */
#define THREAD_ALL_ACCESS 0x001FFFFFUL
#endif

#ifndef DIRECTORY_QUERY
/* 中文说明：部分 WDK 头不暴露目录对象访问位，按 NT 定义补齐 DIRECTORY_QUERY。 */
#define DIRECTORY_QUERY 0x0001
#endif

#ifndef STATUS_NO_MORE_ENTRIES
/* 中文说明：ZwQueryDirectoryObject 扫描结束时常返回该 warning status。 */
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001AL)
#endif

#ifndef STATUS_IMAGE_ALREADY_LOADED
/* 中文说明：旧 WDK 头缺失时补齐“镜像仍在 loader list 中”的失败状态。 */
#define STATUS_IMAGE_ALREADY_LOADED ((NTSTATUS)0xC000010EL)
#endif

/* 中文说明：ZwQueryDirectoryObject 返回的单条对象目录信息布局。 */
typedef struct _KSW_OBJECT_DIRECTORY_INFORMATION
{
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} KSW_OBJECT_DIRECTORY_INFORMATION, *PKSW_OBJECT_DIRECTORY_INFORMATION;

/* 中文说明：命名 DriverObject 引用入口，和 driver_object_query.c 保持同一策略。 */
NTSYSAPI
NTSTATUS
NTAPI
ObReferenceObjectByName(
    _In_ PUNICODE_STRING ObjectName,
    _In_ ULONG Attributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Inout_opt_ PVOID ParseContext,
    _Out_ PVOID* Object
    );

/* 中文说明：IoDriverObjectType 用于 ObReferenceObjectByName 的类型约束。 */
extern POBJECT_TYPE* IoDriverObjectType;

/* 中文说明：ObMakeTemporaryObject 让命名对象在引用计数归零后可被对象管理器回收。 */
NTSYSAPI
VOID
NTAPI
ObMakeTemporaryObject(
    _In_ PVOID Object
    );

/* 中文说明：优先使用 Windows 自己的 I/O 管理器卸载路径，而不是只手工调用 DriverUnload。 */
NTSYSAPI
NTSTATUS
NTAPI
ZwUnloadDriver(
    _In_ PUNICODE_STRING DriverServiceName
    );

/* 中文说明：打开 NT 对象目录，用于按 ServiceKeyName 兜底查找 DriverObject。 */
NTSYSAPI
NTSTATUS
NTAPI
ZwOpenDirectoryObject(
    _Out_ PHANDLE DirectoryHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_ATTRIBUTES ObjectAttributes
    );

/* 中文说明：枚举 NT 对象目录项，参考 SKT64 目录扫描思路但使用公开 Zw 接口。 */
NTSYSAPI
NTSTATUS
NTAPI
ZwQueryDirectoryObject(
    _In_ HANDLE DirectoryHandle,
    _Out_writes_bytes_opt_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN ReturnSingleEntry,
    _In_ BOOLEAN RestartScan,
    _Inout_ PULONG Context,
    _Out_opt_ PULONG ReturnLength
    );

/* 中文说明：查询系统模块表，用于验证回调函数地址属于目标模块。 */
NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

/* 中文说明：卸载线程上下文在非分页池中分配，线程退出前释放。 */
typedef struct _KSW_DRIVER_UNLOAD_CONTEXT
{
    PDRIVER_OBJECT DriverObject;
    ULONG Flags;
    NTSTATUS UnloadStatus;
    NTSTATUS CleanupStatus;
    PDRIVER_UNLOAD DriverUnload;
    ULONG DeletedDeviceCount;
    ULONG CleanupFlagsApplied;
    BOOLEAN AttemptDirectUnload;
    WCHAR ServiceRegistryPath[KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS];
} KSW_DRIVER_UNLOAD_CONTEXT, *PKSW_DRIVER_UNLOAD_CONTEXT;

/* 中文说明：ZwUnloadDriver 线程上下文不保存 DriverObject，避免额外对象引用阻塞系统卸载。 */
typedef struct _KSW_DRIVER_UNLOAD_ZW_CONTEXT
{
    NTSTATUS UnloadStatus;
    WCHAR ServiceRegistryPath[KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS];
} KSW_DRIVER_UNLOAD_ZW_CONTEXT, *PKSW_DRIVER_UNLOAD_ZW_CONTEXT;

/* 中文说明：ZwQuerySystemInformation(SystemModuleInformation) 的单项布局。 */
typedef struct _KSW_DRIVER_UNLOAD_SYSTEM_MODULE_ENTRY
{
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} KSW_DRIVER_UNLOAD_SYSTEM_MODULE_ENTRY, *PKSW_DRIVER_UNLOAD_SYSTEM_MODULE_ENTRY;

/* 中文说明：系统模块表头，Modules 为变长数组首元素。 */
typedef struct _KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION
{
    ULONG NumberOfModules;
    KSW_DRIVER_UNLOAD_SYSTEM_MODULE_ENTRY Modules[1];
} KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION, *PKSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION;

/* 中文说明：强力清理回调的聚合计数，最终回填到 R3 响应。 */
typedef struct _KSW_DRIVER_UNLOAD_CALLBACK_CLEANUP_RESULT
{
    ULONG Candidates;
    ULONG Removed;
    ULONG Failures;
    NTSTATUS LastStatus;
} KSW_DRIVER_UNLOAD_CALLBACK_CLEANUP_RESULT, *PKSW_DRIVER_UNLOAD_CALLBACK_CLEANUP_RESULT;

/* 中文说明：卸载前预检结果，所有字段只用于决定是否允许执行破坏性步骤。 */
typedef struct _KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT
{
    BOOLEAN AllowZwUnload;
    BOOLEAN AllowDirectUnload;
    BOOLEAN AllowDestructiveCleanup;
    BOOLEAN HasServiceRegistryPath;
    BOOLEAN HasDriverUnload;
    BOOLEAN HasValidDynData;
    BOOLEAN HasPdbBackedDynData;
    BOOLEAN HasValidDriverObjectOffsets;
    BOOLEAN HasValidLoaderEvidence;
    BOOLEAN HasDeviceChain;
    BOOLEAN HasCrossDriverAttach;
    BOOLEAN HasDeviceLoop;
    BOOLEAN HasAttachedDevice;
    BOOLEAN HasBusyDeviceReference;
    BOOLEAN IsCoreKernelModule;
    BOOLEAN IsSelfModule;
    ULONGLONG DriverStart;
    ULONGLONG DriverEnd;
    ULONGLONG LoaderEntryAddress;
    ULONGLONG LoaderDllBase;
    ULONG LoaderSizeOfImage;
    NTSTATUS Status;
    WCHAR ServiceRegistryPath[KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS];
} KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT, *PKSW_DRIVER_UNLOAD_PREFLIGHT_RESULT;

/* 中文说明：把内部 preflight 结果压缩成 handler 可打印的诊断快照。 */
static VOID
KswordARKDriverUnloadCapturePreflightDiagnostics(
    _Out_ KSW_DRIVER_UNLOAD_DIAGNOSTICS* Diagnostics,
    _In_ const KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Preflight
    )
{
    if (Diagnostics == NULL || Preflight == NULL) {
        return;
    }

    Diagnostics->preflightStatus = Preflight->Status;
    Diagnostics->allowZwUnload = Preflight->AllowZwUnload;
    Diagnostics->allowDirectUnload = Preflight->AllowDirectUnload;
    Diagnostics->allowDestructiveCleanup = Preflight->AllowDestructiveCleanup;
    Diagnostics->hasServiceRegistryPath = Preflight->HasServiceRegistryPath;
    Diagnostics->hasDriverUnload = Preflight->HasDriverUnload;
    Diagnostics->hasValidDynData = Preflight->HasValidDynData;
    Diagnostics->hasPdbBackedDynData = Preflight->HasPdbBackedDynData;
    Diagnostics->hasValidDriverObjectOffsets = Preflight->HasValidDriverObjectOffsets;
    Diagnostics->hasValidLoaderEvidence = Preflight->HasValidLoaderEvidence;
    Diagnostics->hasDeviceChain = Preflight->HasDeviceChain;
    Diagnostics->hasCrossDriverAttach = Preflight->HasCrossDriverAttach;
    Diagnostics->hasDeviceLoop = Preflight->HasDeviceLoop;
    Diagnostics->hasAttachedDevice = Preflight->HasAttachedDevice;
    Diagnostics->hasBusyDeviceReference = Preflight->HasBusyDeviceReference;
    Diagnostics->isCoreKernelModule = Preflight->IsCoreKernelModule;
    Diagnostics->isSelfModule = Preflight->IsSelfModule;
    Diagnostics->driverStart = Preflight->DriverStart;
    Diagnostics->loaderEntryAddress = Preflight->LoaderEntryAddress;
    Diagnostics->loaderDllBase = Preflight->LoaderDllBase;
    Diagnostics->loaderSizeOfImage = Preflight->LoaderSizeOfImage;
}

/* 中文说明：进程 Ex notify 函数签名，供批量移除时调用公开 Ps* 移除 API。 */
typedef VOID
(*KSW_DRIVER_UNLOAD_PROCESS_NOTIFY_EX)(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    );

/* 中文说明：线程 notify 函数签名，供 PsRemoveCreateThreadNotifyRoutine 使用。 */
typedef VOID
(*KSW_DRIVER_UNLOAD_THREAD_NOTIFY)(
    _In_ HANDLE ProcessId,
    _In_ HANDLE ThreadId,
    _In_ BOOLEAN Create
    );

/* 中文说明：镜像加载 notify 函数签名，供 PsRemoveLoadImageNotifyRoutine 使用。 */
typedef VOID
(*KSW_DRIVER_UNLOAD_IMAGE_NOTIFY)(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ HANDLE ProcessId,
    _In_ PIMAGE_INFO ImageInfo
    );

/* 中文说明：ASCII 范围宽字符大写化，只用于 NT 对象目录名和服务名比较。 */
static WCHAR
KswordARKDriverUnloadUpcaseAscii(
    _In_ WCHAR Character
    )
{
    if (Character >= L'a' && Character <= L'z') {
        return (WCHAR)(Character - L'a' + L'A');
    }
    return Character;
}

/* 中文说明：比较两个 NUL 结尾宽字符串前缀，忽略 ASCII 大小写。 */
static BOOLEAN
KswordARKDriverUnloadStartsWithInsensitive(
    _In_z_ const WCHAR* Text,
    _In_z_ const WCHAR* Prefix
    )
{
    ULONG index = 0UL;

    // 输入：两个 NUL 结尾字符串；处理：逐字符 ASCII 大小写归一化。
    // 返回：Text 带 Prefix 前缀时为 TRUE；任一参数为空或字符不匹配时为 FALSE。
    if (Text == NULL || Prefix == NULL) {
        return FALSE;
    }

    while (Prefix[index] != L'\0') {
        if (KswordARKDriverUnloadUpcaseAscii(Text[index]) !=
            KswordARKDriverUnloadUpcaseAscii(Prefix[index])) {
            return FALSE;
        }
        ++index;
    }

    return TRUE;
}

/* 中文说明：比较有限长 ANSI 文件名是否以指定后缀结尾，大小写不敏感。 */
static BOOLEAN
KswordARKDriverUnloadAnsiEndsWithInsensitive(
    _In_reads_bytes_(TextBytes) const UCHAR* Text,
    _In_ ULONG TextBytes,
    _In_z_ PCSTR Suffix
    )
{
    ULONG textChars = 0UL;
    ULONG suffixChars = 0UL;
    ULONG index = 0UL;

    // 输入：SystemModuleInformation 中的有限长 ANSI 文件名和 NUL 结尾后缀。
    // 处理：先计算实际长度，再从尾部逐字符 ASCII 小写比较。
    // 返回：Text 以后缀结尾时 TRUE；输入无效、长度不足或不匹配时 FALSE。
    if (Text == NULL || TextBytes == 0UL || Suffix == NULL) {
        return FALSE;
    }
    while (textChars < TextBytes && Text[textChars] != '\0') {
        ++textChars;
    }
    while (Suffix[suffixChars] != '\0') {
        ++suffixChars;
    }
    if (suffixChars == 0UL || textChars < suffixChars) {
        return FALSE;
    }
    for (index = 0UL; index < suffixChars; ++index) {
        CHAR left = (CHAR)Text[textChars - suffixChars + index];
        CHAR right = Suffix[index];
        if (left >= 'A' && left <= 'Z') {
            left = (CHAR)(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = (CHAR)(right + ('a' - 'A'));
        }
        if (left != right) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 中文说明：判断 NT 对象路径是否带指定前缀，比较过程大小写不敏感。 */
static BOOLEAN
KswordARKDriverUnloadNameHasPrefix(
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* ObjectName,
    _In_z_ const WCHAR* Prefix
    )
{
    ULONG index = 0UL;

    if (ObjectName == NULL || Prefix == NULL) {
        return FALSE;
    }

    while (Prefix[index] != L'\0') {
        WCHAR left = KswordARKDriverUnloadUpcaseAscii(ObjectName[index]);
        WCHAR right = KswordARKDriverUnloadUpcaseAscii(Prefix[index]);

        if (left != right) {
            return FALSE;
        }
        ++index;
    }

    return TRUE;
}

/* 中文说明：从 \Driver\X 或 \FileSystem\Filters\X 中提取最后一级对象名。 */
static NTSTATUS
KswordARKDriverUnloadExtractLeafName(
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* ObjectName,
    _Out_writes_(LeafChars) PWCHAR LeafName,
    _In_ ULONG LeafChars
    )
{
    ULONG index = 0UL;
    ULONG leafStart = 0UL;

    if (ObjectName == NULL || LeafName == NULL || LeafChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    LeafName[0] = L'\0';
    while (index < KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS &&
        ObjectName[index] != L'\0') {
        if (ObjectName[index] == L'\\') {
            leafStart = index + 1UL;
        }
        ++index;
    }
    if (index == 0UL || leafStart >= index) {
        return STATUS_INVALID_PARAMETER;
    }

    return RtlStringCchCopyW(LeafName, LeafChars, ObjectName + leafStart);
}

/* 中文说明：计算固定 NUL 结尾 WCHAR 字符串长度，最多检查协议允许长度。 */
static ULONG
KswordARKDriverUnloadCountFixedStringChars(
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* Text
    )
{
    ULONG chars = 0UL;

    if (Text == NULL) {
        return 0UL;
    }

    while (chars < KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS &&
        Text[chars] != L'\0') {
        ++chars;
    }
    return chars;
}

/* 中文说明：把 UNICODE_STRING 安全复制为 NUL 结尾固定缓冲。 */
static NTSTATUS
KswordARKDriverUnloadCopyUnicodeToFixed(
    _In_opt_ PCUNICODE_STRING SourceName,
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars
    )
{
    ULONG charsToCopy = 0UL;

    // 输入：内核 UNICODE_STRING 和固定输出缓冲。
    // 处理：按 Length 限定复制，不要求 SourceName 自身 NUL 结尾。
    // 返回：成功复制返回 STATUS_SUCCESS；无来源或缓冲无效返回参数错误。
    if (SourceName == NULL ||
        SourceName->Buffer == NULL ||
        SourceName->Length == 0 ||
        Destination == NULL ||
        DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    charsToCopy = (ULONG)(SourceName->Length / sizeof(WCHAR));
    if (charsToCopy == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (charsToCopy >= DestinationChars) {
        charsToCopy = DestinationChars - 1UL;
    }

    RtlCopyMemory(Destination, SourceName->Buffer, (SIZE_T)charsToCopy * sizeof(WCHAR));
    Destination[charsToCopy] = L'\0';
    return STATUS_SUCCESS;
}

/* 中文说明：前置声明，供注册表路径构造逻辑复用最后一级对象名提取。 */
static NTSTATUS
KswordARKDriverUnloadExtractLeafName(
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* ObjectName,
    _Out_writes_(LeafChars) PWCHAR LeafName,
    _In_ ULONG LeafChars
    );

/* 中文说明：由 DriverObject/ServiceKeyName 推导 ZwUnloadDriver 需要的服务注册表路径。 */
static NTSTATUS
KswordARKDriverUnloadBuildServiceRegistryPath(
    _In_opt_ PDRIVER_OBJECT DriverObject,
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* NormalizedDriverName,
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars
    )
{
    WCHAR serviceName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
    NTSTATUS status = STATUS_SUCCESS;

    // 输入：已引用 DriverObject 和规范化对象名。
    // 处理：优先使用 DriverExtension->ServiceKeyName；如果已经是注册表绝对路径则原样使用；
    //      否则按服务名拼到 HKLM\SYSTEM\CurrentControlSet\Services。
    // 返回：输出完整 NT 注册表路径，失败时返回对应 NTSTATUS。
    if (Destination == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    Destination[0] = L'\0';

    __try {
        if (DriverObject != NULL && DriverObject->DriverExtension != NULL) {
            status = KswordARKDriverUnloadCopyUnicodeToFixed(
                &DriverObject->DriverExtension->ServiceKeyName,
                serviceName,
                RTL_NUMBER_OF(serviceName));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    if (!NT_SUCCESS(status) || serviceName[0] == L'\0') {
        if (NormalizedDriverName != NULL &&
            NT_SUCCESS(KswordARKDriverUnloadExtractLeafName(
                NormalizedDriverName,
                serviceName,
                RTL_NUMBER_OF(serviceName)))) {
            status = STATUS_SUCCESS;
        }
        else {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    if (KswordARKDriverUnloadStartsWithInsensitive(
        serviceName,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\")) {
        return RtlStringCchCopyW(Destination, DestinationChars, serviceName);
    }
    if (KswordARKDriverUnloadStartsWithInsensitive(serviceName, L"System\\CurrentControlSet\\Services\\")) {
        return RtlStringCchPrintfW(
            Destination,
            DestinationChars,
            L"\\Registry\\Machine\\%ws",
            serviceName);
    }
    if (KswordARKDriverUnloadStartsWithInsensitive(serviceName, L"\\System\\CurrentControlSet\\Services\\")) {
        return RtlStringCchPrintfW(
            Destination,
            DestinationChars,
            L"\\Registry\\Machine%ws",
            serviceName);
    }

    return RtlStringCchPrintfW(
        Destination,
        DestinationChars,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\%ws",
        serviceName);
}

/* 中文说明：比较 UNICODE_STRING 的最后一级名称和固定字符串，忽略 ASCII 大小写。 */
static BOOLEAN
KswordARKDriverUnloadUnicodeLeafEqualsFixed(
    _In_opt_ PCUNICODE_STRING SourceName,
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* ExpectedLeaf
    )
{
    ULONG sourceChars = 0UL;
    ULONG expectedChars = 0UL;
    ULONG leafStart = 0UL;
    ULONG leafChars = 0UL;
    ULONG index = 0UL;

    if (SourceName == NULL ||
        SourceName->Buffer == NULL ||
        SourceName->Length == 0 ||
        ExpectedLeaf == NULL) {
        return FALSE;
    }

    sourceChars = (ULONG)(SourceName->Length / sizeof(WCHAR));
    expectedChars = KswordARKDriverUnloadCountFixedStringChars(ExpectedLeaf);
    if (sourceChars == 0UL || expectedChars == 0UL ||
        expectedChars >= KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) {
        return FALSE;
    }

    for (index = 0UL; index < sourceChars; ++index) {
        if (SourceName->Buffer[index] == L'\\') {
            leafStart = index + 1UL;
        }
    }
    if (leafStart >= sourceChars) {
        return FALSE;
    }

    leafChars = sourceChars - leafStart;
    if (leafChars != expectedChars) {
        return FALSE;
    }

    for (index = 0UL; index < expectedChars; ++index) {
        WCHAR left = KswordARKDriverUnloadUpcaseAscii(SourceName->Buffer[leafStart + index]);
        WCHAR right = KswordARKDriverUnloadUpcaseAscii(ExpectedLeaf[index]);

        if (left != right) {
            return FALSE;
        }
    }

    return TRUE;
}

/* 中文说明：判断一个已引用 DriverObject 是否属于指定服务名。 */
static BOOLEAN
KswordARKDriverUnloadDriverObjectMatchesServiceLeaf(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* ServiceLeaf
    )
{
    BOOLEAN matches = FALSE;

    if (DriverObject == NULL || ServiceLeaf == NULL) {
        return FALSE;
    }

    __try {
        if (DriverObject->DriverExtension != NULL &&
            KswordARKDriverUnloadUnicodeLeafEqualsFixed(
                &DriverObject->DriverExtension->ServiceKeyName,
                ServiceLeaf)) {
            matches = TRUE;
        }
        else if (KswordARKDriverUnloadUnicodeLeafEqualsFixed(
            &DriverObject->DriverName,
            ServiceLeaf)) {
            matches = TRUE;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        matches = FALSE;
    }

    return matches;
}

/* 中文说明：把共享协议中的名称复制为完整 DriverObject 对象路径。 */
static NTSTATUS
KswordARKDriverUnloadBuildObjectName(
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* SourceName,
    _Out_writes_(DestinationChars) PWCHAR DestinationName,
    _In_ ULONG DestinationChars
    )
{
    ULONG inputChars = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (SourceName == NULL || DestinationName == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    DestinationName[0] = L'\0';

    while (inputChars < KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS &&
        SourceName[inputChars] != L'\0') {
        ++inputChars;
    }
    if (inputChars == 0UL || inputChars >= KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (SourceName[0] == L'\\') {
        /*
         * 中文说明：R3 手工输入完整对象路径时保持原样，支持 \Driver\X、
         * \FileSystem\X 以及 \FileSystem\Filters\X 等真实 DriverObject 名称。
         */
        status = RtlStringCchCopyNW(
            DestinationName,
            DestinationChars,
            SourceName,
            inputChars);
    }
    else {
        status = RtlStringCchPrintfW(
            DestinationName,
            DestinationChars,
            L"\\Driver\\%ws",
            SourceName);
    }

    return status;
}

/* 中文说明：打开一个对象目录，失败时返回底层 NTSTATUS。 */
static NTSTATUS
KswordARKDriverUnloadOpenDirectory(
    _In_z_ const WCHAR* DirectoryName,
    _Out_ HANDLE* DirectoryHandleOut
    )
{
    UNICODE_STRING directoryName;
    OBJECT_ATTRIBUTES objectAttributes;

    if (DirectoryName == NULL || DirectoryHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DirectoryHandleOut = NULL;
    RtlInitUnicodeString(&directoryName, DirectoryName);
    InitializeObjectAttributes(
        &objectAttributes,
        &directoryName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    return ZwOpenDirectoryObject(
        DirectoryHandleOut,
        DIRECTORY_QUERY,
        &objectAttributes);
}

/* 中文说明：尝试引用一个完整 DriverObject 对象路径，成功时返回已引用对象。 */
static NTSTATUS
KswordARKDriverUnloadReferenceCandidateName(
    _In_z_ const WCHAR* CandidateName,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut,
    _Out_writes_(NameChars) PWCHAR NormalizedNameOut,
    _In_ ULONG NameChars
    )
{
    UNICODE_STRING objectName;
    NTSTATUS status = STATUS_SUCCESS;

    if (CandidateName == NULL ||
        DriverObjectOut == NULL ||
        NormalizedNameOut == NULL ||
        NameChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DriverObjectOut = NULL;
    status = RtlStringCchCopyW(NormalizedNameOut, NameChars, CandidateName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&objectName, NormalizedNameOut);
    /* 中文说明：ObReferenceObjectByName 返回对象引用而不是句柄，不能带 OBJ_KERNEL_HANDLE。 */
    status = ObReferenceObjectByName(
        &objectName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        0,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)DriverObjectOut);
    if (!NT_SUCCESS(status)) {
        *DriverObjectOut = NULL;
    }
    return status;
}

/* 中文说明：拼接“目录路径 + 子对象名”为完整候选 DriverObject 路径。 */
static NTSTATUS
KswordARKDriverUnloadBuildDirectoryCandidateName(
    _In_z_ const WCHAR* DirectoryName,
    _In_ PCUNICODE_STRING EntryName,
    _Out_writes_(CandidateChars) PWCHAR CandidateName,
    _In_ ULONG CandidateChars
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (DirectoryName == NULL ||
        EntryName == NULL ||
        EntryName->Buffer == NULL ||
        EntryName->Length == 0 ||
        CandidateName == NULL ||
        CandidateChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    CandidateName[0] = L'\0';
    status = RtlStringCchCopyW(CandidateName, CandidateChars, DirectoryName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (CandidateName[KswordARKDriverUnloadCountFixedStringChars(CandidateName) - 1UL] != L'\\') {
        status = RtlStringCchCatW(CandidateName, CandidateChars, L"\\");
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    return RtlStringCchCatNW(
        CandidateName,
        CandidateChars,
        EntryName->Buffer,
        (size_t)(EntryName->Length / sizeof(WCHAR)));
}

/* 中文说明：在一个对象目录中按 ServiceKeyName/DriverName 兜底查找 DriverObject。 */
static NTSTATUS
KswordARKDriverUnloadReferenceByServiceLeafInDirectory(
    _In_z_ const WCHAR* DirectoryName,
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* ServiceLeaf,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut,
    _Out_writes_(NameChars) PWCHAR NormalizedNameOut,
    _In_ ULONG NameChars
    )
{
    HANDLE directoryHandle = NULL;
    PKSW_OBJECT_DIRECTORY_INFORMATION entry = NULL;
    ULONG queryContext = 0UL;
    ULONG returnLength = 0UL;
    ULONG scannedEntries = 0UL;
    BOOLEAN restartScan = TRUE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS finalStatus = STATUS_OBJECT_NAME_NOT_FOUND;

    if (DirectoryName == NULL ||
        ServiceLeaf == NULL ||
        DriverObjectOut == NULL ||
        NormalizedNameOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DriverObjectOut = NULL;
    status = KswordARKDriverUnloadOpenDirectory(DirectoryName, &directoryHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    entry = (PKSW_OBJECT_DIRECTORY_INFORMATION)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        KSW_DRIVER_UNLOAD_DIRECTORY_QUERY_BYTES,
        KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
#pragma warning(pop)
    if (entry == NULL) {
        ZwClose(directoryHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (scannedEntries < KSW_DRIVER_UNLOAD_DIRECTORY_MAX_ENTRIES) {
        WCHAR candidateName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
        PDRIVER_OBJECT candidateObject = NULL;
        NTSTATUS candidateStatus = STATUS_SUCCESS;

        RtlZeroMemory(entry, KSW_DRIVER_UNLOAD_DIRECTORY_QUERY_BYTES);
        status = ZwQueryDirectoryObject(
            directoryHandle,
            entry,
            KSW_DRIVER_UNLOAD_DIRECTORY_QUERY_BYTES,
            TRUE,
            restartScan,
            &queryContext,
            &returnLength);
        restartScan = FALSE;
        if (status == STATUS_NO_MORE_ENTRIES) {
            finalStatus = STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        }
        if (!NT_SUCCESS(status)) {
            finalStatus = status;
            break;
        }

        ++scannedEntries;
        if (entry->Name.Buffer == NULL || entry->Name.Length == 0) {
            continue;
        }

        candidateStatus = KswordARKDriverUnloadBuildDirectoryCandidateName(
            DirectoryName,
            &entry->Name,
            candidateName,
            RTL_NUMBER_OF(candidateName));
        if (!NT_SUCCESS(candidateStatus)) {
            finalStatus = candidateStatus;
            continue;
        }

        candidateStatus = KswordARKDriverUnloadReferenceCandidateName(
            candidateName,
            &candidateObject,
            NormalizedNameOut,
            NameChars);
        if (!NT_SUCCESS(candidateStatus)) {
            if (candidateStatus != STATUS_OBJECT_TYPE_MISMATCH) {
                finalStatus = candidateStatus;
            }
            continue;
        }

        if (KswordARKDriverUnloadDriverObjectMatchesServiceLeaf(candidateObject, ServiceLeaf)) {
            *DriverObjectOut = candidateObject;
            ExFreePoolWithTag(entry, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
            ZwClose(directoryHandle);
            return STATUS_SUCCESS;
        }

        ObDereferenceObject(candidateObject);
    }

    ExFreePoolWithTag(entry, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
    ZwClose(directoryHandle);
    return finalStatus;
}

/* 中文说明：参考 SKT64 遍历对象目录的思路，用 ServiceKeyName 处理名称不一致的驱动。 */
static NTSTATUS
KswordARKDriverUnloadReferenceByServiceLeaf(
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* ServiceLeaf,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut,
    _Out_writes_(NameChars) PWCHAR NormalizedNameOut,
    _In_ ULONG NameChars
    )
{
    static const WCHAR* const directoriesToScan[] = {
        L"\\Driver",
        L"\\FileSystem",
        L"\\FileSystem\\Filters"
    };
    ULONG directoryIndex = 0UL;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;

    if (ServiceLeaf == NULL || DriverObjectOut == NULL || NormalizedNameOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DriverObjectOut = NULL;
    for (directoryIndex = 0UL;
        directoryIndex < RTL_NUMBER_OF(directoriesToScan);
        ++directoryIndex) {
        NTSTATUS scanStatus = KswordARKDriverUnloadReferenceByServiceLeafInDirectory(
            directoriesToScan[directoryIndex],
            ServiceLeaf,
            DriverObjectOut,
            NormalizedNameOut,
            NameChars);

        if (NT_SUCCESS(scanStatus)) {
            return STATUS_SUCCESS;
        }
        if (scanStatus != STATUS_OBJECT_NAME_NOT_FOUND &&
            scanStatus != STATUS_OBJECT_PATH_NOT_FOUND &&
            scanStatus != STATUS_OBJECT_TYPE_MISMATCH) {
            status = scanStatus;
        }
    }

    return status;
}

/* 中文说明：判断 DriverObject 的镜像基址是否等于模块表中的基址。 */
static BOOLEAN
KswordARKDriverUnloadDriverObjectMatchesModuleBase(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONGLONG TargetModuleBase
    )
{
    BOOLEAN matches = FALSE;

    if (DriverObject == NULL || TargetModuleBase == 0ULL) {
        return FALSE;
    }

    __try {
        if ((ULONGLONG)(ULONG_PTR)DriverObject->DriverStart == TargetModuleBase) {
            matches = TRUE;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        matches = FALSE;
    }

    return matches;
}

/* 中文说明：在一个对象目录中按模块基址反查 DriverObject。 */
static NTSTATUS
KswordARKDriverUnloadReferenceByModuleBaseInDirectory(
    _In_z_ const WCHAR* DirectoryName,
    _In_ ULONGLONG TargetModuleBase,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut,
    _Out_writes_(NameChars) PWCHAR NormalizedNameOut,
    _In_ ULONG NameChars
    )
{
    HANDLE directoryHandle = NULL;
    PKSW_OBJECT_DIRECTORY_INFORMATION entry = NULL;
    ULONG queryContext = 0UL;
    ULONG returnLength = 0UL;
    ULONG scannedEntries = 0UL;
    BOOLEAN restartScan = TRUE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS finalStatus = STATUS_OBJECT_NAME_NOT_FOUND;

    if (DirectoryName == NULL ||
        TargetModuleBase == 0ULL ||
        DriverObjectOut == NULL ||
        NormalizedNameOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DriverObjectOut = NULL;
    status = KswordARKDriverUnloadOpenDirectory(DirectoryName, &directoryHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    entry = (PKSW_OBJECT_DIRECTORY_INFORMATION)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        KSW_DRIVER_UNLOAD_DIRECTORY_QUERY_BYTES,
        KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
#pragma warning(pop)
    if (entry == NULL) {
        ZwClose(directoryHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (scannedEntries < KSW_DRIVER_UNLOAD_DIRECTORY_MAX_ENTRIES) {
        WCHAR candidateName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
        PDRIVER_OBJECT candidateObject = NULL;
        NTSTATUS candidateStatus = STATUS_SUCCESS;

        RtlZeroMemory(entry, KSW_DRIVER_UNLOAD_DIRECTORY_QUERY_BYTES);
        status = ZwQueryDirectoryObject(
            directoryHandle,
            entry,
            KSW_DRIVER_UNLOAD_DIRECTORY_QUERY_BYTES,
            TRUE,
            restartScan,
            &queryContext,
            &returnLength);
        restartScan = FALSE;
        if (status == STATUS_NO_MORE_ENTRIES) {
            finalStatus = STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        }
        if (!NT_SUCCESS(status)) {
            finalStatus = status;
            break;
        }

        ++scannedEntries;
        if (entry->Name.Buffer == NULL || entry->Name.Length == 0) {
            continue;
        }

        candidateStatus = KswordARKDriverUnloadBuildDirectoryCandidateName(
            DirectoryName,
            &entry->Name,
            candidateName,
            RTL_NUMBER_OF(candidateName));
        if (!NT_SUCCESS(candidateStatus)) {
            finalStatus = candidateStatus;
            continue;
        }

        candidateStatus = KswordARKDriverUnloadReferenceCandidateName(
            candidateName,
            &candidateObject,
            NormalizedNameOut,
            NameChars);
        if (!NT_SUCCESS(candidateStatus)) {
            if (candidateStatus != STATUS_OBJECT_TYPE_MISMATCH) {
                finalStatus = candidateStatus;
            }
            continue;
        }

        if (KswordARKDriverUnloadDriverObjectMatchesModuleBase(candidateObject, TargetModuleBase)) {
            *DriverObjectOut = candidateObject;
            ExFreePoolWithTag(entry, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
            ZwClose(directoryHandle);
            return STATUS_SUCCESS;
        }

        ObDereferenceObject(candidateObject);
    }

    ExFreePoolWithTag(entry, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
    ZwClose(directoryHandle);
    return finalStatus;
}

/* 中文说明：按模块基址扫描对象目录，处理“服务已停止但模块仍在”的残留场景。 */
static NTSTATUS
KswordARKDriverUnloadReferenceByModuleBase(
    _In_ ULONGLONG TargetModuleBase,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut,
    _Out_writes_(NameChars) PWCHAR NormalizedNameOut,
    _In_ ULONG NameChars
    )
{
    static const WCHAR* const directoriesToScan[] = {
        L"\\Driver",
        L"\\FileSystem",
        L"\\FileSystem\\Filters"
    };
    ULONG directoryIndex = 0UL;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;

    if (TargetModuleBase == 0ULL || DriverObjectOut == NULL || NormalizedNameOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DriverObjectOut = NULL;
    for (directoryIndex = 0UL;
        directoryIndex < RTL_NUMBER_OF(directoriesToScan);
        ++directoryIndex) {
        NTSTATUS scanStatus = KswordARKDriverUnloadReferenceByModuleBaseInDirectory(
            directoriesToScan[directoryIndex],
            TargetModuleBase,
            DriverObjectOut,
            NormalizedNameOut,
            NameChars);

        if (NT_SUCCESS(scanStatus)) {
            return STATUS_SUCCESS;
        }
        if (scanStatus != STATUS_OBJECT_NAME_NOT_FOUND &&
            scanStatus != STATUS_OBJECT_PATH_NOT_FOUND &&
            scanStatus != STATUS_OBJECT_TYPE_MISMATCH) {
            status = scanStatus;
        }
    }

    return status;
}

/* 中文说明：只有对象名/路径未找到时才尝试其它目录，避免掩盖权限或参数错误。 */
static BOOLEAN
KswordARKDriverUnloadShouldTryAlternateName(
    _In_ NTSTATUS Status
    )
{
    return (Status == STATUS_OBJECT_NAME_NOT_FOUND ||
        Status == STATUS_OBJECT_PATH_NOT_FOUND ||
        Status == STATUS_NOT_FOUND) ? TRUE : FALSE;
}

/* 中文说明：按对象名引用 DriverObject，不接受 R3 传入地址。 */
static NTSTATUS
KswordARKDriverUnloadReferenceByName(
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut,
    _Out_writes_(NameChars) PWCHAR NormalizedNameOut,
    _In_ ULONG NameChars
    )
{
    WCHAR firstCandidate[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
    WCHAR leafName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
    WCHAR alternateName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
    NTSTATUS status = STATUS_SUCCESS;

    if (Request == NULL || DriverObjectOut == NULL || NormalizedNameOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *DriverObjectOut = NULL;
    NormalizedNameOut[0] = L'\0';

    if ((Request->flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_TARGET_MODULE_BASE_PRESENT) != 0UL &&
        Request->targetModuleBase != 0ULL) {
        status = KswordARKDriverUnloadReferenceByModuleBase(
            Request->targetModuleBase,
            DriverObjectOut,
            NormalizedNameOut,
            NameChars);
        if (NT_SUCCESS(status) || !KswordARKDriverUnloadShouldTryAlternateName(status)) {
            return status;
        }
    }

    status = KswordARKDriverUnloadBuildObjectName(
        Request->driverName,
        firstCandidate,
        RTL_NUMBER_OF(firstCandidate));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKDriverUnloadReferenceCandidateName(
        firstCandidate,
        DriverObjectOut,
        NormalizedNameOut,
        NameChars);
    if (NT_SUCCESS(status) || !KswordARKDriverUnloadShouldTryAlternateName(status)) {
        return status;
    }

    /*
     * 中文说明：SCM 服务名不总是等于 DriverObject 的 \Driver\ 名称。
     * 文件系统和 mini-filter 常见对象目录是 \FileSystem\ 或
     * \FileSystem\Filters\，因此在“对象未找到”时按最后一级名称兜底。
     */
    if (!NT_SUCCESS(KswordARKDriverUnloadExtractLeafName(
        firstCandidate,
        leafName,
        RTL_NUMBER_OF(leafName)))) {
        return status;
    }

    if (!KswordARKDriverUnloadNameHasPrefix(firstCandidate, L"\\FileSystem\\")) {
        NTSTATUS alternateStatus = RtlStringCchPrintfW(
            alternateName,
            RTL_NUMBER_OF(alternateName),
            L"\\FileSystem\\%ws",
            leafName);
        if (NT_SUCCESS(alternateStatus)) {
            alternateStatus = KswordARKDriverUnloadReferenceCandidateName(
                alternateName,
                DriverObjectOut,
                NormalizedNameOut,
                NameChars);
            if (NT_SUCCESS(alternateStatus) ||
                !KswordARKDriverUnloadShouldTryAlternateName(alternateStatus)) {
                return alternateStatus;
            }
            status = alternateStatus;
        }
    }

    if (!KswordARKDriverUnloadNameHasPrefix(firstCandidate, L"\\FileSystem\\Filters\\")) {
        NTSTATUS alternateStatus = RtlStringCchPrintfW(
            alternateName,
            RTL_NUMBER_OF(alternateName),
            L"\\FileSystem\\Filters\\%ws",
            leafName);
        if (NT_SUCCESS(alternateStatus)) {
            alternateStatus = KswordARKDriverUnloadReferenceCandidateName(
                alternateName,
                DriverObjectOut,
                NormalizedNameOut,
                NameChars);
            if (NT_SUCCESS(alternateStatus) ||
                !KswordARKDriverUnloadShouldTryAlternateName(alternateStatus)) {
                return alternateStatus;
            }
            status = alternateStatus;
        }
    }

    if (!KswordARKDriverUnloadNameHasPrefix(firstCandidate, L"\\Driver\\")) {
        NTSTATUS alternateStatus = RtlStringCchPrintfW(
            alternateName,
            RTL_NUMBER_OF(alternateName),
            L"\\Driver\\%ws",
            leafName);
        if (NT_SUCCESS(alternateStatus)) {
            alternateStatus = KswordARKDriverUnloadReferenceCandidateName(
                alternateName,
                DriverObjectOut,
                NormalizedNameOut,
                NameChars);
            if (NT_SUCCESS(alternateStatus) ||
                !KswordARKDriverUnloadShouldTryAlternateName(alternateStatus)) {
                return alternateStatus;
            }
            status = alternateStatus;
        }
    }

    status = KswordARKDriverUnloadReferenceByServiceLeaf(
        leafName,
        DriverObjectOut,
        NormalizedNameOut,
        NameChars);
    if (NT_SUCCESS(status) || !KswordARKDriverUnloadShouldTryAlternateName(status)) {
        return status;
    }

    return status;
}

/* 中文说明：判断回调枚举行是否可以由公开/受控路径尝试移除。 */
static BOOLEAN
KswordARKDriverUnloadCallbackEntryIsRemovable(
    _In_ const KSWORD_ARK_CALLBACK_ENUM_ENTRY* Entry
    )
{
    if (Entry == NULL) {
        return FALSE;
    }
    if ((Entry->fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE) == 0UL) {
        return FALSE;
    }
    if (Entry->callbackAddress == 0ULL) {
        return FALSE;
    }

    switch (Entry->callbackClass) {
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS:
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD:
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE:
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT:
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY:
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER:
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT:
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER:
        return TRUE;
    default:
        return FALSE;
    }
}

/* 中文说明：把枚举回调类别转换成移除 IOCTL 使用的类别值。 */
static ULONG
KswordARKDriverUnloadCallbackClassToRemoveType(
    _In_ ULONG CallbackClass
    )
{
    switch (CallbackClass) {
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER;
    default:
        return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS;
    }
}

/* 中文说明：构建系统模块快照，用于把回调地址严格归属到目标模块。 */
static NTSTATUS
KswordARKDriverUnloadBuildModuleSnapshot(
    _Outptr_result_bytebuffer_(*BufferBytesOut) KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION** ModuleInfoOut,
    _Out_ ULONG* BufferBytesOut
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;
    KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;

    if (ModuleInfoOut == NULL || BufferBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ModuleInfoOut = NULL;
    *BufferBytesOut = 0UL;

    status = ZwQuerySystemInformation(
        KSW_DRIVER_UNLOAD_SYSTEM_MODULE_CLASS,
        NULL,
        0UL,
        &requiredBytes);
    if (requiredBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    moduleInfo = (KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION*)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        requiredBytes,
        KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
#pragma warning(pop)
    if (moduleInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQuerySystemInformation(
        KSW_DRIVER_UNLOAD_SYSTEM_MODULE_CLASS,
        moduleInfo,
        requiredBytes,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(moduleInfo, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
        return status;
    }

    *ModuleInfoOut = moduleInfo;
    *BufferBytesOut = requiredBytes;
    return STATUS_SUCCESS;
}

/* 中文说明：判断某个地址是否落入指定模块基址对应的镜像范围。 */
static BOOLEAN
KswordARKDriverUnloadAddressBelongsToModuleBase(
    _In_opt_ const KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONGLONG Address,
    _In_ ULONGLONG TargetModuleBase
    )
{
    ULONG moduleIndex = 0UL;

    if (ModuleInfo == NULL || Address == 0ULL || TargetModuleBase == 0ULL) {
        return FALSE;
    }

    for (moduleIndex = 0UL; moduleIndex < ModuleInfo->NumberOfModules; ++moduleIndex) {
        const KSW_DRIVER_UNLOAD_SYSTEM_MODULE_ENTRY* moduleEntry = &ModuleInfo->Modules[moduleIndex];
        const ULONGLONG moduleBase = (ULONGLONG)(ULONG_PTR)moduleEntry->ImageBase;
        const ULONGLONG moduleEnd = moduleBase + (ULONGLONG)moduleEntry->ImageSize;

        if (moduleBase != TargetModuleBase) {
            continue;
        }
        if (Address >= moduleBase && Address < moduleEnd) {
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* 中文说明：判断枚举行是否属于目标模块；非函数地址类必须已有 moduleBase 字段。 */
static BOOLEAN
KswordARKDriverUnloadCallbackEntryMatchesModuleBase(
    _In_ const KSWORD_ARK_CALLBACK_ENUM_ENTRY* Entry,
    _In_opt_ const KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONGLONG TargetModuleBase
    )
{
    if (Entry == NULL || TargetModuleBase == 0ULL) {
        return FALSE;
    }
    if ((Entry->fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_MODULE) != 0UL &&
        Entry->moduleBase == TargetModuleBase) {
        return TRUE;
    }
    if ((Entry->fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER) != 0UL) {
        return FALSE;
    }
    return KswordARKDriverUnloadAddressBelongsToModuleBase(
        ModuleInfo,
        Entry->callbackAddress,
        TargetModuleBase);
}

/* 中文说明：调用单条外部回调移除路径，并把状态聚合到结果计数。 */
static VOID
KswordARKDriverUnloadRemoveOneCallbackEntry(
    _In_ const KSWORD_ARK_CALLBACK_ENUM_ENTRY* Entry,
    _Inout_ KSW_DRIVER_UNLOAD_CALLBACK_CLEANUP_RESULT* CleanupResult
    )
{
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST removeRequest;
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE removeResponse;
    NTSTATUS removeStatus = STATUS_SUCCESS;

    if (Entry == NULL || CleanupResult == NULL) {
        return;
    }

    RtlZeroMemory(&removeRequest, sizeof(removeRequest));
    RtlZeroMemory(&removeResponse, sizeof(removeResponse));
    removeRequest.size = sizeof(removeRequest);
    removeRequest.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
    removeRequest.callbackClass = KswordARKDriverUnloadCallbackClassToRemoveType(Entry->callbackClass);
    removeRequest.flags = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_NONE;
    removeRequest.callbackAddress = Entry->callbackAddress;
    removeResponse.size = sizeof(removeResponse);
    removeResponse.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
    removeResponse.callbackClass = removeRequest.callbackClass;
    removeResponse.callbackAddress = removeRequest.callbackAddress;

    switch (removeRequest.callbackClass) {
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS:
        removeStatus = PsSetCreateProcessNotifyRoutineEx(
            (KSW_DRIVER_UNLOAD_PROCESS_NOTIFY_EX)(ULONG_PTR)removeRequest.callbackAddress,
            TRUE);
        if (removeStatus == STATUS_PROCEDURE_NOT_FOUND || removeStatus == STATUS_INVALID_PARAMETER) {
            removeStatus = PsSetCreateProcessNotifyRoutine(
                (PCREATE_PROCESS_NOTIFY_ROUTINE)(ULONG_PTR)removeRequest.callbackAddress,
                TRUE);
        }
        break;

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD:
        removeStatus = PsRemoveCreateThreadNotifyRoutine(
            (KSW_DRIVER_UNLOAD_THREAD_NOTIFY)(ULONG_PTR)removeRequest.callbackAddress);
        break;

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE:
        removeStatus = PsRemoveLoadImageNotifyRoutine(
            (KSW_DRIVER_UNLOAD_IMAGE_NOTIFY)(ULONG_PTR)removeRequest.callbackAddress);
        break;

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER:
        removeStatus = KswordArkCallbackExternalRemoveByRequest(
            &removeRequest,
            &removeResponse);
        break;

    default:
        removeStatus = STATUS_INVALID_PARAMETER;
        break;
    }

    if (NT_SUCCESS(removeStatus)) {
        CleanupResult->Removed += 1UL;
    }
    else {
        CleanupResult->Failures += 1UL;
        CleanupResult->LastStatus = removeStatus;
    }
}

/* 中文说明：按模块基址枚举并移除可验证回调，避免目标残留模块继续靠回调运行。 */
static NTSTATUS
KswordARKDriverUnloadRemoveCallbacksByModuleBase(
    _In_ ULONGLONG TargetModuleBase,
    _Out_ KSW_DRIVER_UNLOAD_CALLBACK_CLEANUP_RESULT* CleanupResult
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    KSW_DRIVER_UNLOAD_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG responseBytes = 0UL;
    KSWORD_ARK_ENUM_CALLBACKS_RESPONSE* enumResponse = NULL;
    ULONG entryIndex = 0UL;
    ULONG parsedEntries = 0UL;

    if (CleanupResult == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(CleanupResult, sizeof(*CleanupResult));
    CleanupResult->LastStatus = STATUS_SUCCESS;
    if (TargetModuleBase == 0ULL) {
        CleanupResult->LastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKDriverUnloadBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    if (!NT_SUCCESS(status)) {
        CleanupResult->LastStatus = status;
        return status;
    }

    responseBytes = sizeof(KSWORD_ARK_ENUM_CALLBACKS_RESPONSE) +
        ((KSW_DRIVER_UNLOAD_MAX_CALLBACK_CLEANUP_COUNT - 1UL) * sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY));
#pragma warning(push)
#pragma warning(disable:4996)
    enumResponse = (KSWORD_ARK_ENUM_CALLBACKS_RESPONSE*)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        responseBytes,
        KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
#pragma warning(pop)
    if (enumResponse == NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
        CleanupResult->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(enumResponse, responseBytes);
    enumResponse->size = sizeof(KSWORD_ARK_ENUM_CALLBACKS_RESPONSE);
    enumResponse->version = KSWORD_ARK_CALLBACK_ENUM_PROTOCOL_VERSION;
    enumResponse->entrySize = sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY);
    enumResponse->lastStatus = STATUS_SUCCESS;

    {
        KSWORD_ARK_CALLBACK_ENUM_BUILDER builder;

        RtlZeroMemory(&builder, sizeof(builder));
        builder.Response = enumResponse;
        builder.EntryCapacity = KSW_DRIVER_UNLOAD_MAX_CALLBACK_CLEANUP_COUNT;
        builder.LastStatus = STATUS_SUCCESS;
        KswordArkCallbackEnumAddMinifilters(&builder);
        KswordArkCallbackEnumAddPrivateCallbacks(&builder);
        KswordArkCallbackExternalAddCallbacks(&builder);
        enumResponse->totalCount = builder.TotalCount;
        enumResponse->returnedCount = builder.ReturnedCount;
        enumResponse->flags = builder.Flags;
        enumResponse->lastStatus = builder.LastStatus;
    }

    parsedEntries = enumResponse->returnedCount;
    if (parsedEntries > KSW_DRIVER_UNLOAD_MAX_CALLBACK_CLEANUP_COUNT) {
        parsedEntries = KSW_DRIVER_UNLOAD_MAX_CALLBACK_CLEANUP_COUNT;
    }

    for (entryIndex = 0UL; entryIndex < parsedEntries; ++entryIndex) {
        const KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = &enumResponse->entries[entryIndex];

        if (!KswordARKDriverUnloadCallbackEntryIsRemovable(entry)) {
            continue;
        }
        if (!KswordARKDriverUnloadCallbackEntryMatchesModuleBase(
            entry,
            moduleInfo,
            TargetModuleBase)) {
            continue;
        }
        CleanupResult->Candidates += 1UL;
        KswordARKDriverUnloadRemoveOneCallbackEntry(entry, CleanupResult);
    }

    ExFreePoolWithTag(enumResponse, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
    ExFreePoolWithTag(moduleInfo, KSW_DRIVER_UNLOAD_DIRECTORY_TAG);
    return CleanupResult->Failures == 0UL ? STATUS_SUCCESS : CleanupResult->LastStatus;
}

/* 中文说明：强制卸载后用于中和目标 DriverObject 的拒绝 IRP stub。 */
static NTSTATUS
KswordARKDriverUnloadRejectedDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    // 输入：系统分发到已被强制中和 DriverObject 的设备对象和 IRP。
    // 处理：不访问目标驱动私有扩展，只把 IRP 以 STATUS_DELETE_PENDING 完成。
    // 返回：STATUS_DELETE_PENDING，提示调用方设备正在删除/不可用。
    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp != NULL) {
        Irp->IoStatus.Status = STATUS_DELETE_PENDING;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    return STATUS_DELETE_PENDING;
}

/* 中文说明：可选中和 dispatch 表，和 SKT64 的 force unload 语义对齐但由 flag 控制。 */
static VOID
KswordARKDriverUnloadClearDispatchUnsafe(
    _Inout_ PDRIVER_OBJECT DriverObject
    )
{
    if (DriverObject == NULL) {
        return;
    }

    __try {
        ULONG majorIndex = 0UL;
        DriverObject->FastIoDispatch = NULL;
        for (majorIndex = 0UL; majorIndex <= IRP_MJ_MAXIMUM_FUNCTION; ++majorIndex) {
            DriverObject->MajorFunction[majorIndex] = KswordARKDriverUnloadRejectedDispatch;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        (VOID)0;
    }
}

/* 中文说明：可选清理 DriverUnload 指针，避免右键重复触发同一个卸载入口。 */
static VOID
KswordARKDriverUnloadClearUnloadPointerUnsafe(
    _Inout_ PDRIVER_OBJECT DriverObject
    )
{
    if (DriverObject == NULL) {
        return;
    }

    __try {
        DriverObject->DriverUnload = NULL;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        (VOID)0;
    }
}

/* 中文说明：目标没有 DriverUnload 时，按原始 DeviceObject->NextDevice 链删除设备。 */
static NTSTATUS
KswordARKDriverUnloadDeleteDeviceObjectsUnsafe(
    _Inout_ PDRIVER_OBJECT DriverObject,
    _Out_ ULONG* DeletedDeviceCountOut
    )
{
    PDEVICE_OBJECT deviceCursor = NULL;
    PDEVICE_OBJECT deviceList[KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT];
    ULONG deviceCount = 0UL;
    ULONG deletedDeviceCount = 0UL;
    NTSTATUS validationStatus = STATUS_SUCCESS;

    if (DeletedDeviceCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *DeletedDeviceCountOut = 0UL;

    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(deviceList, sizeof(deviceList));

    /*
     * 中文说明：删除 DeviceObject 无法回滚，因此先完整快照并校验链表。
     * 如果链表过长、成环或混入其它 DriverObject，直接失败，不先删一半。
     */
    __try {
        deviceCursor = DriverObject->DeviceObject;
        while (deviceCursor != NULL) {
            ULONG previousIndex = 0UL;
            PDEVICE_OBJECT nextDevice = NULL;

            if (deviceCount >= KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT) {
                validationStatus = STATUS_BUFFER_OVERFLOW;
                break;
            }
            for (previousIndex = 0UL; previousIndex < deviceCount; ++previousIndex) {
                if (deviceList[previousIndex] == deviceCursor) {
                    validationStatus = STATUS_INVALID_DEVICE_REQUEST;
                    break;
                }
            }
            if (!NT_SUCCESS(validationStatus)) {
                break;
            }
            if (deviceCursor->DriverObject != DriverObject) {
                validationStatus = STATUS_OBJECT_TYPE_MISMATCH;
                break;
            }
            if (deviceCursor->AttachedDevice != NULL ||
                deviceCursor->ReferenceCount != 0) {
                validationStatus = STATUS_DEVICE_BUSY;
                break;
            }

            nextDevice = deviceCursor->NextDevice;
            deviceList[deviceCount] = deviceCursor;
            deviceCount += 1UL;
            deviceCursor = nextDevice;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    if (!NT_SUCCESS(validationStatus)) {
        return validationStatus;
    }

    __try {
        ULONG deleteIndex = 0UL;
        for (deleteIndex = 0UL; deleteIndex < deviceCount; ++deleteIndex) {
            IoDeleteDevice(deviceList[deleteIndex]);
            deletedDeviceCount += 1UL;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *DeletedDeviceCountOut = deletedDeviceCount;
        return GetExceptionCode();
    }

    *DeletedDeviceCountOut = deletedDeviceCount;
    return STATUS_SUCCESS;
}

/* Verify one DeviceObject chain entry is not busy before destructive fallback. */
static NTSTATUS
KswordARKDriverUnloadCheckDeviceObjectIdleUnsafe(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ PDEVICE_OBJECT* NextDeviceOut
    )
{
    // Inputs: target DriverObject, one DeviceObject from its NextDevice chain, and an output slot.
    // Processing: read owner, next, attached, and ReferenceCount while guarded by SEH.
    // Return: STATUS_SUCCESS when the device can participate in direct unload; otherwise a blocking NTSTATUS.
    if (DriverObject == NULL || DeviceObject == NULL || NextDeviceOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *NextDeviceOut = NULL;
    __try {
        if (DeviceObject->DriverObject != DriverObject) {
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        if (DeviceObject->AttachedDevice != NULL) {
            return STATUS_DEVICE_BUSY;
        }
        if (DeviceObject->ReferenceCount != 0) {
            return STATUS_DEVICE_BUSY;
        }
        *NextDeviceOut = DeviceObject->NextDevice;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

/* 中文说明：检查 DriverUnload 返回后是否已经清空 DeviceObject 链。 */
static NTSTATUS
KswordARKDriverUnloadRequireNoDeviceObjectsUnsafe(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    // 输入：仍被本线程引用的目标 DriverObject。
    // 处理：只读检查 DeviceObject 链首；不遍历、不删除、不修正。
    // 返回：无设备返回 STATUS_SUCCESS；仍有设备返回 STATUS_DEVICE_BUSY；异常透传异常码。
    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        return DriverObject->DeviceObject == NULL
            ? STATUS_SUCCESS
            : STATUS_DEVICE_BUSY;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

/* 中文说明：从 DriverObject 基址加 DynData 偏移安全读取一个指针字段。 */
static BOOLEAN
KswordARKDriverUnloadReadPointerFieldByOffset(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG FieldOffset,
    _Out_ PVOID* ValueOut
    )
{
    const UCHAR* fieldAddress = NULL;

    // 输入：已引用 DriverObject、PDB/DynData 字段偏移和输出指针。
    // 处理：拒绝缺失偏移，用 MmCopyMemory 包装函数读取目标字段。
    // 返回：读取成功且完整时 TRUE；任何参数、偏移或内存读取失败时 FALSE。
    if (DriverObject == NULL ||
        ValueOut == NULL ||
        !KswordARKDriverIntegrityOffsetPresent(FieldOffset)) {
        return FALSE;
    }

    *ValueOut = NULL;
    fieldAddress = (const UCHAR*)DriverObject + (SIZE_T)FieldOffset;
    return KswordARKHookReadMemorySafe(fieldAddress, ValueOut, sizeof(*ValueOut));
}

/* 中文说明：从 DriverObject 基址加 DynData 偏移安全读取一个 ULONG 字段。 */
static BOOLEAN
KswordARKDriverUnloadReadUlongFieldByOffset(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG FieldOffset,
    _Out_ ULONG* ValueOut
    )
{
    const UCHAR* fieldAddress = NULL;

    // 输入：已引用 DriverObject、PDB/DynData 字段偏移和输出 ULONG。
    // 处理：拒绝缺失偏移，用 MmCopyMemory 包装函数读取目标字段。
    // 返回：读取成功且完整时 TRUE；任何参数、偏移或内存读取失败时 FALSE。
    if (DriverObject == NULL ||
        ValueOut == NULL ||
        !KswordARKDriverIntegrityOffsetPresent(FieldOffset)) {
        return FALSE;
    }

    *ValueOut = 0UL;
    fieldAddress = (const UCHAR*)DriverObject + (SIZE_T)FieldOffset;
    return KswordARKHookReadMemorySafe(fieldAddress, ValueOut, sizeof(*ValueOut));
}

/* 中文说明：验证 PDB/DynData _DRIVER_OBJECT 偏移和当前 WDK 视图一致。 */
static BOOLEAN
KswordARKDriverUnloadValidateDriverObjectOffsets(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ const KSW_DYN_STATE* DynState
    )
{
    PVOID driverStart = NULL;
    ULONG driverSize = 0UL;
    PVOID driverSection = NULL;
    PVOID driverUnload = NULL;
    PVOID majorFunction = NULL;
    PVOID expectedMajorFunction = NULL;

    // 输入：目标 DriverObject 和已经通过身份匹配的 DynData 快照。
    // 处理：只读读取 PDB profile 给出的关键 _DRIVER_OBJECT 字段，并与当前 WDK 结构访问结果交叉比较。
    // 返回：全部关键字段一致时 TRUE；任一字段缺失、读取失败或不一致时 FALSE。
    if (DriverObject == NULL || DynState == NULL) {
        return FALSE;
    }

    if (!KswordARKDriverUnloadReadPointerFieldByOffset(
            DriverObject,
            DynState->Kernel.DoDriverStart,
            &driverStart) ||
        !KswordARKDriverUnloadReadUlongFieldByOffset(
            DriverObject,
            DynState->Kernel.DoDriverSize,
            &driverSize) ||
        !KswordARKDriverUnloadReadPointerFieldByOffset(
            DriverObject,
            DynState->Kernel.DoDriverSection,
            &driverSection) ||
        !KswordARKDriverUnloadReadPointerFieldByOffset(
            DriverObject,
            DynState->Kernel.DoDriverUnload,
            &driverUnload) ||
        !KswordARKDriverUnloadReadPointerFieldByOffset(
            DriverObject,
            DynState->Kernel.DoMajorFunction,
            &majorFunction)) {
        return FALSE;
    }

    expectedMajorFunction = (PVOID)(ULONG_PTR)DriverObject->MajorFunction[0];
    if (driverStart != DriverObject->DriverStart ||
        driverSize != DriverObject->DriverSize ||
        driverSection != DriverObject->DriverSection ||
        driverUnload != (PVOID)(ULONG_PTR)DriverObject->DriverUnload ||
        majorFunction != expectedMajorFunction) {
        return FALSE;
    }

    return TRUE;
}

/* 中文说明：确认卸载强路径依赖的 DynData 字段全部来自当前匹配的 PDB profile。 */
static BOOLEAN
KswordARKDriverUnloadHasPdbBackedDynData(
    _In_ const KSW_DYN_STATE* DynState
    )
{
    // 输入：当前 DynData 快照。
    // 处理：检查强卸载依赖的 _DRIVER_OBJECT、_KLDR_DATA_TABLE_ENTRY 和 PsLoadedModuleList 字段来源。
    // 返回：所有关键字段均来自 PDB profile 时 TRUE；否则 FALSE。
    if (DynState == NULL) {
        return FALSE;
    }

    return DynState->PdbProfileActive &&
        DynState->KernelSources.DoDriverStart == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelSources.DoDriverSize == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelSources.DoDriverSection == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelSources.DoMajorFunction == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelSources.DoDriverUnload == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelSources.KldrInLoadOrderLinks == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelSources.KldrDllBase == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelSources.KldrSizeOfImage == KSW_DYN_FIELD_SOURCE_PDB_PROFILE &&
        DynState->KernelGlobalSources.PsLoadedModuleList == KSW_DYN_FIELD_SOURCE_PDB_PROFILE;
}

/* 中文说明：读取卸载目标的只读前置证据，并决定是否允许系统卸载或强力清理。 */
static NTSTATUS
KswordARKDriverUnloadBuildPreflightResult(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* NormalizedDriverName,
    _In_ ULONGLONG TargetModuleBase,
    _In_ ULONG Flags,
    _Out_ KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Result
    )
{
    KSW_DYN_STATE dynState;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* targetModule = NULL;
    KSW_DRIVER_INTEGRITY_LDR_TARGET ldrTarget;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS evidenceStatus = STATUS_SUCCESS;
    ULONGLONG driverStart = 0ULL;
    ULONGLONG driverEnd = 0ULL;

    if (DriverObject == NULL || Result == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    UNREFERENCED_PARAMETER(moduleInfoBytes);

    RtlZeroMemory(Result, sizeof(*Result));
    RtlZeroMemory(&dynState, sizeof(dynState));
    RtlZeroMemory(&ldrTarget, sizeof(ldrTarget));

    Result->AllowDirectUnload = FALSE;
    Result->AllowZwUnload = FALSE;
    Result->AllowDestructiveCleanup =
        (Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP) != 0UL ? TRUE : FALSE;

    __try {
        driverStart = (ULONGLONG)(ULONG_PTR)DriverObject->DriverStart;
        driverEnd = driverStart + (ULONGLONG)DriverObject->DriverSize;
        Result->HasDriverUnload = (DriverObject->DriverUnload != NULL) ? TRUE : FALSE;
        status = KswordARKDriverUnloadBuildServiceRegistryPath(
            DriverObject,
            NormalizedDriverName,
            Result->ServiceRegistryPath,
            RTL_NUMBER_OF(Result->ServiceRegistryPath));
        Result->HasServiceRegistryPath = NT_SUCCESS(status) ? TRUE : FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    if (driverStart == 0ULL ||
        driverEnd <= driverStart ||
        driverEnd < driverStart) {
        Result->AllowDirectUnload = FALSE;
        Result->AllowZwUnload = FALSE;
        Result->AllowDestructiveCleanup = FALSE;
        Result->Status = STATUS_INVALID_PARAMETER;
        return Result->Status;
    }
    Result->DriverStart = driverStart;
    Result->DriverEnd = driverEnd;

    if (TargetModuleBase != 0ULL &&
        TargetModuleBase != driverStart) {
        Result->AllowDirectUnload = FALSE;
        Result->AllowZwUnload = FALSE;
        Result->AllowDestructiveCleanup = FALSE;
        Result->Status = STATUS_OBJECT_TYPE_MISMATCH;
        return Result->Status;
    }

    KswordARKDynDataSnapshot(&dynState);
    Result->AllowDirectUnload = Result->HasDriverUnload;
    Result->AllowZwUnload = Result->HasServiceRegistryPath;
    Result->HasValidDynData =
        dynState.Initialized &&
        dynState.NtosActive &&
        (dynState.CapabilityMask & KSW_CAP_DRIVER_OBJECT_FIELDS) != 0ULL &&
        (dynState.CapabilityMask & KSW_CAP_KERNEL_MODULE_LIST_FIELDS) != 0ULL &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.DoDriverStart) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.DoDriverSize) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.DoDriverSection) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.DoMajorFunction) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.DoDriverUnload) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.KldrDllBase) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.KldrSizeOfImage) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.Kernel.KldrInLoadOrderLinks) &&
        KswordARKDriverIntegrityOffsetPresent(dynState.KernelGlobals.PsLoadedModuleList);
    Result->HasPdbBackedDynData = KswordARKDriverUnloadHasPdbBackedDynData(&dynState);

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    if (NT_SUCCESS(status) && moduleInfo != NULL) {
        targetModule = KswordARKDriverIntegrityFindModuleForAddress(moduleInfo, driverStart);
        Result->IsCoreKernelModule = KswordARKDriverIntegrityIsCoreKernelModule(targetModule);
        if (targetModule != NULL) {
            const UCHAR* fileName = NULL;
            ULONG fileNameBytes = 0UL;

            KswordARKHookGetModuleFileName(targetModule, &fileName, &fileNameBytes);
            if (KswordARKDriverUnloadAnsiEndsWithInsensitive(fileName, fileNameBytes, "KswordARK.sys")) {
                Result->IsSelfModule = TRUE;
            }
            if (KswordARKDriverUnloadAnsiEndsWithInsensitive(fileName, fileNameBytes, "ntoskrnl.exe") ||
                KswordARKDriverUnloadAnsiEndsWithInsensitive(fileName, fileNameBytes, "ntkrnlmp.exe") ||
                KswordARKDriverUnloadAnsiEndsWithInsensitive(fileName, fileNameBytes, "ntkrnlpa.exe") ||
                KswordARKDriverUnloadAnsiEndsWithInsensitive(fileName, fileNameBytes, "ntkrpamp.exe") ||
                KswordARKDriverUnloadAnsiEndsWithInsensitive(fileName, fileNameBytes, "hal.dll")) {
                Result->IsCoreKernelModule = TRUE;
            }
        }
    }
    else {
        evidenceStatus = status;
        status = STATUS_SUCCESS;
    }

    if (!Result->IsSelfModule &&
        Result->ServiceRegistryPath[0] != L'\0') {
        const WCHAR* leafName = Result->ServiceRegistryPath;
        if (KswordARKDriverUnloadStartsWithInsensitive(
            Result->ServiceRegistryPath,
            L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\")) {
            const ULONG prefixChars = KswordARKDriverUnloadCountFixedStringChars(
                L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
            leafName = Result->ServiceRegistryPath + prefixChars;
        }
        if (KswordARKDriverUnloadStartsWithInsensitive(leafName, L"KswordARK")) {
            Result->IsSelfModule = TRUE;
        }
    }

    if (Result->HasValidDynData) {
        status = KswordARKDriverIntegrityFindLoadedModule(&dynState, driverStart, &ldrTarget);
        if (NT_SUCCESS(status) && ldrTarget.Found) {
            const ULONGLONG driverSize = (ULONGLONG)DriverObject->DriverSize;
            Result->LoaderEntryAddress = ldrTarget.EntryAddress;
            Result->LoaderDllBase = ldrTarget.DllBase;
            Result->LoaderSizeOfImage = ldrTarget.SizeOfImage;
            if (ldrTarget.DllBase == driverStart &&
                ldrTarget.SizeOfImage != 0UL &&
                (ULONGLONG)ldrTarget.SizeOfImage == driverSize &&
                ((ULONGLONG)(ULONG_PTR)DriverObject->DriverSection == 0ULL ||
                    (ULONGLONG)(ULONG_PTR)DriverObject->DriverSection == ldrTarget.EntryAddress)) {
                Result->HasValidLoaderEvidence = TRUE;
            }
            else {
                evidenceStatus = STATUS_OBJECT_TYPE_MISMATCH;
                Result->HasValidLoaderEvidence = FALSE;
            }
        }
        else {
            evidenceStatus = status;
            Result->HasValidLoaderEvidence = FALSE;
        }
        status = STATUS_SUCCESS;
    }

    if (Result->HasValidDynData && Result->HasValidLoaderEvidence) {
        Result->HasValidDriverObjectOffsets =
            KswordARKDriverUnloadValidateDriverObjectOffsets(DriverObject, &dynState);
        if (!Result->HasValidDriverObjectOffsets && evidenceStatus == STATUS_SUCCESS) {
            evidenceStatus = STATUS_OBJECT_TYPE_MISMATCH;
        }
    }

    __try {
        PDEVICE_OBJECT rootDevice = DriverObject->DeviceObject;
        ULONG visitedCount = 0UL;
        PDEVICE_OBJECT visited[KSW_DRIVER_UNLOAD_PREFLIGHT_DEVICE_LIMIT] = { 0 };

        while (rootDevice != NULL) {
            PDEVICE_OBJECT nextDevice = NULL;
            ULONG previousIndex = 0UL;
            NTSTATUS deviceIdleStatus = STATUS_SUCCESS;

            Result->HasDeviceChain = TRUE;
            if (visitedCount >= KSW_DRIVER_UNLOAD_PREFLIGHT_DEVICE_LIMIT) {
                if (evidenceStatus == STATUS_SUCCESS) {
                    evidenceStatus = STATUS_BUFFER_OVERFLOW;
                }
                break;
            }
            for (previousIndex = 0UL; previousIndex < visitedCount; ++previousIndex) {
                if (visited[previousIndex] == rootDevice) {
                    Result->HasDeviceLoop = TRUE;
                    break;
                }
            }
            if (Result->HasDeviceLoop) {
                break;
            }
            visited[visitedCount++] = rootDevice;

            deviceIdleStatus = KswordARKDriverUnloadCheckDeviceObjectIdleUnsafe(
                DriverObject,
                rootDevice,
                &nextDevice);
            if (deviceIdleStatus == STATUS_OBJECT_TYPE_MISMATCH) {
                Result->HasCrossDriverAttach = TRUE;
                break;
            }
            if (deviceIdleStatus == STATUS_DEVICE_BUSY) {
                if (rootDevice->AttachedDevice != NULL) {
                    Result->HasAttachedDevice = TRUE;
                }
                if (rootDevice->ReferenceCount != 0) {
                    Result->HasBusyDeviceReference = TRUE;
                }
                if (evidenceStatus == STATUS_SUCCESS) {
                    evidenceStatus = STATUS_DEVICE_BUSY;
                }
                break;
            }
            if (!NT_SUCCESS(deviceIdleStatus)) {
                if (evidenceStatus == STATUS_SUCCESS) {
                    evidenceStatus = deviceIdleStatus;
                }
                break;
            }

            rootDevice = nextDevice;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        evidenceStatus = status;
        status = STATUS_SUCCESS;
    }

    if (Result->IsSelfModule || Result->IsCoreKernelModule) {
        Result->AllowDirectUnload = FALSE;
        Result->AllowZwUnload = FALSE;
        Result->AllowDestructiveCleanup = FALSE;
        Result->Status = STATUS_DRIVER_BLOCKED_CRITICAL;
        if (moduleInfo != NULL) {
            ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
        }
        return Result->Status;
    }

    if (Result->HasDeviceLoop || Result->HasCrossDriverAttach) {
        if (evidenceStatus == STATUS_SUCCESS) {
            evidenceStatus = STATUS_INVALID_DEVICE_REQUEST;
        }
        Result->AllowDestructiveCleanup = FALSE;
    }
    if (Result->HasAttachedDevice || Result->HasBusyDeviceReference) {
        if (evidenceStatus == STATUS_SUCCESS) {
            evidenceStatus = STATUS_DEVICE_BUSY;
        }
        Result->AllowDestructiveCleanup = FALSE;
        Result->AllowDirectUnload = FALSE;
    }

    if (!Result->HasServiceRegistryPath) {
        Result->AllowZwUnload = FALSE;
    }
    if (!Result->HasValidDynData ||
        !Result->HasPdbBackedDynData ||
        !Result->HasValidLoaderEvidence) {
        if (evidenceStatus == STATUS_SUCCESS) {
            evidenceStatus = STATUS_REQUEST_NOT_ACCEPTED;
        }
        Result->AllowDestructiveCleanup = FALSE;
        Result->AllowDirectUnload = FALSE;
    }
    if (!Result->HasValidDriverObjectOffsets) {
        if (evidenceStatus == STATUS_SUCCESS) {
            evidenceStatus = STATUS_OBJECT_TYPE_MISMATCH;
        }
        Result->AllowDestructiveCleanup = FALSE;
        Result->AllowDirectUnload = FALSE;
    }

    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }
    Result->Status = evidenceStatus;
    return STATUS_SUCCESS;
}

/* 中文说明：执行强制卸载后的附加清理，所有动作都必须由有效 flag 明确启用。 */
static NTSTATUS
KswordARKDriverUnloadApplyCleanupUnsafe(
    _Inout_ PKSW_DRIVER_UNLOAD_CONTEXT UnloadContext,
    _In_ BOOLEAN DriverUnloadWasCalled
    )
{
    NTSTATUS cleanupStatus = STATUS_SUCCESS;
    BOOLEAN allowPersistentCleanup = FALSE;
    BOOLEAN deleteDeviceObjects = FALSE;

    if (UnloadContext == NULL || UnloadContext->DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    allowPersistentCleanup =
        (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP) != 0UL
        ? TRUE
        : FALSE;
    if (!allowPersistentCleanup) {
        return STATUS_SUCCESS;
    }

    deleteDeviceObjects =
        (!DriverUnloadWasCalled &&
            (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD) != 0UL) ||
        ((UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS) != 0UL)
        ? TRUE
        : FALSE;
    if (deleteDeviceObjects) {
        ULONG deletedDeviceCount = 0UL;
        NTSTATUS deleteStatus = KswordARKDriverUnloadDeleteDeviceObjectsUnsafe(
            UnloadContext->DriverObject,
            &deletedDeviceCount);
        UnloadContext->DeletedDeviceCount = deletedDeviceCount;
        UnloadContext->CleanupFlagsApplied |=
            (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS) != 0UL
            ? KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS
            : KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD;
        if (!NT_SUCCESS(deleteStatus)) {
            return deleteStatus;
        }
    }
    if (DriverUnloadWasCalled && !deleteDeviceObjects) {
        NTSTATUS noDeviceStatus = KswordARKDriverUnloadRequireNoDeviceObjectsUnsafe(
            UnloadContext->DriverObject);
        if (!NT_SUCCESS(noDeviceStatus)) {
            return noDeviceStatus;
        }
    }

    if ((UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT) != 0UL) {
        __try {
            ObMakeTemporaryObject(UnloadContext->DriverObject);
            UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }
    if ((UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER) != 0UL) {
        KswordARKDriverUnloadClearUnloadPointerUnsafe(UnloadContext->DriverObject);
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER;
    }
    if (DriverUnloadWasCalled &&
        (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD) != 0UL) {
        KswordARKDriverUnloadClearDispatchUnsafe(UnloadContext->DriverObject);
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD;
    }
    if (!DriverUnloadWasCalled &&
        (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD) != 0UL) {
        KswordARKDriverUnloadClearDispatchUnsafe(UnloadContext->DriverObject);
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD;
    }

    return cleanupStatus;
}

/* 中文说明：将 R3 请求 flags 降级为 R0 实际允许执行的 flags。 */
static ULONG
KswordARKDriverUnloadSanitizeFlags(
    _In_ ULONG RequestedFlags
    )
{
    // 输入：R3 原始强卸载 flags。
    // 处理：保留定位类 flag；若没有 ALLOW_DESTRUCTIVE_CLEANUP，则清除所有持久改写/移除类 flag。
    // 返回：R0 本次实际执行的 flags，用于回填响应并驱动后续逻辑。
    const ULONG mutatingMask =
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_REMOVE_CALLBACKS_BY_MODULE_BASE;

    if ((RequestedFlags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP) == 0UL) {
        return RequestedFlags & ~mutatingMask;
    }
    return RequestedFlags;
}

/* 中文说明：目标没有 DriverUnload 时，判断是否仍需进入后处理分支。 */
static BOOLEAN
KswordARKDriverUnloadShouldCleanupWithoutUnload(
    _In_ ULONG Flags
    )
{
    // 输入：R3 传入的强卸载 flags。
    // 处理：仅当显式允许持久清理且携带具体清理位时才返回 TRUE。
    // 返回：TRUE 表示没有 DriverUnload 也要执行后处理；FALSE 表示直接报告缺少 DriverUnload。
    if ((Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP) == 0UL) {
        return FALSE;
    }
    if ((Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD) != 0UL) {
        return TRUE;
    }
    if ((Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER) != 0UL) {
        return TRUE;
    }
    if ((Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD) != 0UL) {
        return TRUE;
    }
    if ((Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS) != 0UL) {
        return TRUE;
    }
    if ((Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT) != 0UL) {
        return TRUE;
    }
    return FALSE;
}

/* 中文说明：判断本次请求是否显式要求 DriverObject 后处理/中和。 */
static BOOLEAN
KswordARKDriverUnloadHasCleanupRequest(
    _In_ ULONG Flags
    )
{
    // 输入：R3 传入的强制卸载 flags。
    // 处理：只检查会改变 DriverObject/回调状态的清理位，不依赖 FORCE_CLEANUP 组合宏。
    // 返回：TRUE 表示调用 DriverUnload 前后还请求了额外清理动作；FALSE 表示只调用 DriverUnload。
    const ULONG cleanupMask =
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_REMOVE_CALLBACKS_BY_MODULE_BASE;

    return ((Flags & cleanupMask) != 0UL) ? TRUE : FALSE;
}

/* 中文说明：把 preflight 的“成功但不可执行”状态归一成明确拒绝码。 */
static NTSTATUS
KswordARKDriverUnloadPreflightDenyStatus(
    _In_opt_ const KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Preflight
    )
{
    // 输入：卸载前预检结果，可为空。
    // 处理：优先保留 preflight 中已经计算出的具体失败原因；若状态仍为成功，
    //      根据关键布尔证据补一个稳定 NTSTATUS，避免 R3 看到 lastStatus=0。
    // 返回：可直接写入 response->lastStatus / waitStatus 的失败 NTSTATUS。
    if (Preflight == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(Preflight->Status)) {
        return Preflight->Status;
    }
    if (Preflight->IsSelfModule || Preflight->IsCoreKernelModule) {
        return STATUS_DRIVER_BLOCKED_CRITICAL;
    }
    if (Preflight->HasDeviceLoop || Preflight->HasCrossDriverAttach) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (Preflight->HasAttachedDevice || Preflight->HasBusyDeviceReference) {
        return STATUS_DEVICE_BUSY;
    }
    if (!Preflight->HasValidDriverObjectOffsets ||
        (Preflight->DriverStart != 0ULL &&
            Preflight->LoaderDllBase != 0ULL &&
            Preflight->LoaderDllBase != Preflight->DriverStart)) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (!Preflight->HasValidDynData ||
        !Preflight->HasPdbBackedDynData ||
        !Preflight->HasValidLoaderEvidence) {
        return STATUS_REQUEST_NOT_ACCEPTED;
    }
    if (!Preflight->HasServiceRegistryPath && !Preflight->HasDriverUnload) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    return STATUS_REQUEST_NOT_ACCEPTED;
}

/* 中文说明：集中判断是否允许从系统卸载失败降级到 direct DriverUnload/中和。 */
static BOOLEAN
KswordARKDriverUnloadCanUseDestructiveFallback(
    _In_opt_ const KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Preflight,
    _In_ ULONG Flags
    )
{
    // 输入：preflight 证据和已经过 sanitize 的请求 flags。
    // 处理：要求 ALLOW_DESTRUCTIVE_CLEANUP、PDB-backed DynData、loader 对齐、
    //      live _DRIVER_OBJECT 偏移自检全部通过，并且确实有可执行动作。
    // 返回：TRUE 表示可以进入强制 fallback；FALSE 表示必须只返回失败状态。
    if (Preflight == NULL) {
        return FALSE;
    }
    if (!Preflight->AllowDestructiveCleanup ||
        !Preflight->HasValidDynData ||
        !Preflight->HasPdbBackedDynData ||
        !Preflight->HasValidDriverObjectOffsets ||
        !Preflight->HasValidLoaderEvidence) {
        return FALSE;
    }
    if (!KswordARKDriverUnloadHasCleanupRequest(Flags)) {
        return FALSE;
    }
    if (!Preflight->AllowDirectUnload &&
        !KswordARKDriverUnloadShouldCleanupWithoutUnload(Flags)) {
        return FALSE;
    }
    return TRUE;
}

/* 中文说明：仅在 Zw 卸载成功但闭环仍 busy 时，判断是否允许做后置中和。 */
/* 中文说明：判断本次强制 fallback 是否允许在 DriverUnload 前先移除目标模块回调。 */
static BOOLEAN
KswordARKDriverUnloadCanPreCleanupCallbacks(
    _In_opt_ const KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Preflight,
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request
    )
{
    // 输入：preflight 证据和本地请求快照。
    // 处理：回调清理只在模块基址明确、基址与 DriverStart 一致、且强制 fallback
    //      已经满足 PDB-backed 安全门时启用；服务名路径不带基址时拒绝回调清理。
    // 返回：TRUE 表示可以调用按模块基址回调清理；FALSE 表示不能执行该高危步骤。
    if (Preflight == NULL || Request == NULL) {
        return FALSE;
    }
    if ((Request->flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_REMOVE_CALLBACKS_BY_MODULE_BASE) == 0UL ||
        (Request->flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_TARGET_MODULE_BASE_PRESENT) == 0UL ||
        Request->targetModuleBase == 0ULL) {
        return FALSE;
    }
    if (Request->targetModuleBase != Preflight->DriverStart) {
        return FALSE;
    }
    return KswordARKDriverUnloadCanUseDestructiveFallback(Preflight, Request->flags);
}

/* 中文说明：纯系统卸载线程；只携带服务注册表路径，不额外持有 DriverObject 引用。 */
static VOID
KswordARKDriverUnloadZwThreadRoutine(
    _In_opt_ PVOID StartContext
    )
{
    PKSW_DRIVER_UNLOAD_ZW_CONTEXT unloadContext = (PKSW_DRIVER_UNLOAD_ZW_CONTEXT)StartContext;
    UNICODE_STRING serviceRegistryPath;

    if (unloadContext == NULL || unloadContext->ServiceRegistryPath[0] == L'\0') {
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
        return;
    }

    RtlInitUnicodeString(&serviceRegistryPath, unloadContext->ServiceRegistryPath);
    unloadContext->UnloadStatus = ZwUnloadDriver(&serviceRegistryPath);
    PsTerminateSystemThread(unloadContext->UnloadStatus);
}

/* 中文说明：运行不持有 DriverObject 引用的纯 ZwUnloadDriver 路径。 */
static NTSTATUS
KswordARKDriverUnloadRunZwOnly(
    _In_reads_(KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS) const WCHAR* ServiceRegistryPath,
    _In_ ULONG TimeoutMilliseconds,
    _Out_ NTSTATUS* WaitStatusOut,
    _Out_ NTSTATUS* UnloadStatusOut
    )
{
    HANDLE threadHandle = NULL;
    PETHREAD threadObject = NULL;
    LARGE_INTEGER timeoutInterval;
    PKSW_DRIVER_UNLOAD_ZW_CONTEXT zwContext = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    // 输入：完整 Services 注册表路径和等待超时。
    // 处理：在系统线程中调用 ZwUnloadDriver；上下文只保存路径，不保存 DriverObject。
    // 返回：等待失败/超时直接返回对应状态；等待成功时返回 ZwUnloadDriver 的 NTSTATUS。
    if (ServiceRegistryPath == NULL ||
        ServiceRegistryPath[0] == L'\0' ||
        WaitStatusOut == NULL ||
        UnloadStatusOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *WaitStatusOut = STATUS_SUCCESS;
    *UnloadStatusOut = STATUS_PENDING;

#pragma warning(push)
#pragma warning(disable:4996)
    zwContext = (PKSW_DRIVER_UNLOAD_ZW_CONTEXT)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(*zwContext),
        KSW_DRIVER_UNLOAD_TAG);
#pragma warning(pop)
    if (zwContext == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(zwContext, sizeof(*zwContext));
    status = RtlStringCchCopyW(
        zwContext->ServiceRegistryPath,
        RTL_NUMBER_OF(zwContext->ServiceRegistryPath),
        ServiceRegistryPath);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(zwContext, KSW_DRIVER_UNLOAD_TAG);
        return status;
    }
    zwContext->UnloadStatus = STATUS_PENDING;

    if (TimeoutMilliseconds == 0UL) {
        TimeoutMilliseconds = KSW_DRIVER_UNLOAD_DEFAULT_TIMEOUT_MS;
    }
    if (TimeoutMilliseconds > KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS) {
        TimeoutMilliseconds = KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS;
    }

    status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        KswordARKDriverUnloadZwThreadRoutine,
        zwContext);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(zwContext, KSW_DRIVER_UNLOAD_TAG);
        return status;
    }

    status = ObReferenceObjectByHandle(
        threadHandle,
        SYNCHRONIZE,
        NULL,
        KernelMode,
        (PVOID*)&threadObject,
        NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(threadHandle);
        /* 中文说明：线程可能已运行并访问上下文，因此这里不释放 zwContext。 */
        return status;
    }

    timeoutInterval.QuadPart = -((LONGLONG)TimeoutMilliseconds * 10LL * 1000LL);
    *WaitStatusOut = KeWaitForSingleObject(
        threadObject,
        Executive,
        KernelMode,
        FALSE,
        &timeoutInterval);
    *UnloadStatusOut = zwContext->UnloadStatus;
    ObDereferenceObject(threadObject);
    ZwClose(threadHandle);

    if (*WaitStatusOut == STATUS_TIMEOUT) {
        /* 中文说明：超时后 Zw 线程仍可能写状态，宁可泄漏上下文也不释放。 */
        return STATUS_TIMEOUT;
    }
    if (!NT_SUCCESS(*WaitStatusOut)) {
        ExFreePoolWithTag(zwContext, KSW_DRIVER_UNLOAD_TAG);
        return *WaitStatusOut;
    }

    status = *UnloadStatusOut;
    ExFreePoolWithTag(zwContext, KSW_DRIVER_UNLOAD_TAG);
    return status;
}

/* Verify that no named DriverObject can be referenced after the final local dereference. */
static NTSTATUS
KswordARKDriverUnloadVerifyDriverObjectGone(
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request,
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* NormalizedDriverName
    )
{
    KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST verifyRequest;
    PDRIVER_OBJECT referencedObject = NULL;
    WCHAR verifiedName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
    NTSTATUS status = STATUS_SUCCESS;

    // Inputs: original request snapshot and the normalized object path found before unload.
    // Processing: try the same lookup path used by the operation, but fall back to the exact
    // normalized name so module-base requests cannot hide a still-named DriverObject.
    // Return: STATUS_SUCCESS only when the object is no longer referenceable; otherwise a
    // blocking NTSTATUS that is safe to expose as the unload result.
    if (Request == NULL || NormalizedDriverName == NULL || NormalizedDriverName[0] == L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&verifyRequest, sizeof(verifyRequest));
    RtlCopyMemory(&verifyRequest, Request, sizeof(verifyRequest));
    status = KswordARKDriverUnloadReferenceByName(
        &verifyRequest,
        &referencedObject,
        verifiedName,
        RTL_NUMBER_OF(verifiedName));
    if (NT_SUCCESS(status)) {
        ObDereferenceObject(referencedObject);
        return STATUS_DEVICE_BUSY;
    }
    if (!KswordARKDriverUnloadShouldTryAlternateName(status) &&
        status != STATUS_OBJECT_TYPE_MISMATCH) {
        return status;
    }

    RtlZeroMemory(&verifyRequest, sizeof(verifyRequest));
    verifyRequest.version = KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION;
    (VOID)RtlStringCchCopyW(
        verifyRequest.driverName,
        RTL_NUMBER_OF(verifyRequest.driverName),
        NormalizedDriverName);
    status = KswordARKDriverUnloadReferenceByName(
        &verifyRequest,
        &referencedObject,
        verifiedName,
        RTL_NUMBER_OF(verifiedName));
    if (NT_SUCCESS(status)) {
        ObDereferenceObject(referencedObject);
        return STATUS_DEVICE_BUSY;
    }
    if (!KswordARKDriverUnloadShouldTryAlternateName(status) &&
        status != STATUS_OBJECT_TYPE_MISMATCH) {
        return status;
    }

    return STATUS_SUCCESS;
}

/* Verify that the target image left both loader-list and module-list views. */
static NTSTATUS
KswordARKDriverUnloadVerifyLoaderGone(
    _In_ const KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Preflight
    )
{
    KSW_DYN_STATE dynState;
    KSW_DRIVER_INTEGRITY_LDR_TARGET ldrTarget;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    NTSTATUS loaderStatus = STATUS_SUCCESS;
    NTSTATUS moduleStatus = STATUS_SUCCESS;
    BOOLEAN checkedAnyView = FALSE;

    // Inputs: preflight evidence containing the exact DriverStart/module base.
    // Processing: re-walk PsLoadedModuleList with PDB-backed offsets, then compare the
    // public SystemModuleInformation snapshot. Both are read-only checks.
    // Return: STATUS_SUCCESS when no view still owns the target base; failure when the
    // image is still listed or when every verification view is unavailable.
    if (Preflight == NULL || Preflight->DriverStart == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&dynState, sizeof(dynState));
    RtlZeroMemory(&ldrTarget, sizeof(ldrTarget));
    KswordARKDynDataSnapshot(&dynState);
    if (dynState.Initialized &&
        dynState.NtosActive &&
        KswordARKDriverUnloadHasPdbBackedDynData(&dynState)) {
        checkedAnyView = TRUE;
        loaderStatus = KswordARKDriverIntegrityFindLoadedModule(
            &dynState,
            Preflight->DriverStart,
            &ldrTarget);
        if (NT_SUCCESS(loaderStatus) && ldrTarget.Found) {
            return STATUS_IMAGE_ALREADY_LOADED;
        }
        if (!NT_SUCCESS(loaderStatus) &&
            loaderStatus != STATUS_NOT_FOUND) {
            return loaderStatus;
        }
    }

    moduleStatus = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    if (NT_SUCCESS(moduleStatus) && moduleInfo != NULL) {
        checkedAnyView = TRUE;
        if (KswordARKDriverIntegrityFindModuleForAddress(
                moduleInfo,
                Preflight->DriverStart) != NULL) {
            ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
            return STATUS_IMAGE_ALREADY_LOADED;
        }
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }
    else if (!checkedAnyView) {
        return moduleStatus;
    }

    return checkedAnyView ? STATUS_SUCCESS : STATUS_REQUEST_NOT_ACCEPTED;
}

/* Close the ReactOS-style strong-unload loop after local references are released. */
static NTSTATUS
KswordARKDriverUnloadVerifyClosedLoop(
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request,
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* NormalizedDriverName,
    _In_ const KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Preflight
    )
{
    ULONG attemptIndex = 0UL;
    NTSTATUS lastStatus = STATUS_SUCCESS;
    LARGE_INTEGER delayInterval;

    // Inputs: request snapshot, normalized DriverObject name, and preflight loader evidence.
    // Processing: retry a short bounded object/loader verification window after the final
    // ObDereferenceObject, because object-manager and image-unload side effects may complete
    // just after the unload worker exits.
    // Return: STATUS_SUCCESS when both object and image are gone; otherwise the most specific
    // failure from the last verification pass.
    if (Request == NULL || NormalizedDriverName == NULL || Preflight == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    delayInterval.QuadPart = -((LONGLONG)KSW_DRIVER_UNLOAD_POST_VERIFY_DELAY_MS * 10LL * 1000LL);
    for (attemptIndex = 0UL;
        attemptIndex < KSW_DRIVER_UNLOAD_POST_VERIFY_RETRIES;
        ++attemptIndex) {
        NTSTATUS objectStatus = KswordARKDriverUnloadVerifyDriverObjectGone(
            Request,
            NormalizedDriverName);
        NTSTATUS loaderStatus = STATUS_SUCCESS;

        if (NT_SUCCESS(objectStatus)) {
            loaderStatus = KswordARKDriverUnloadVerifyLoaderGone(Preflight);
            if (NT_SUCCESS(loaderStatus)) {
                return STATUS_SUCCESS;
            }
            lastStatus = loaderStatus;
        }
        else {
            lastStatus = objectStatus;
        }

        if (attemptIndex + 1UL < KSW_DRIVER_UNLOAD_POST_VERIFY_RETRIES) {
            (VOID)KeDelayExecutionThread(
                KernelMode,
                FALSE,
                &delayInterval);
        }
    }

    return NT_SUCCESS(lastStatus) ? STATUS_REQUEST_NOT_ACCEPTED : lastStatus;
}

/* 中文说明：系统线程实际调用 DriverUnload，隔离调用栈和等待超时。 */
static VOID
KswordARKDriverUnloadThreadRoutine(
    _In_opt_ PVOID StartContext
    )
{
    PKSW_DRIVER_UNLOAD_CONTEXT unloadContext = (PKSW_DRIVER_UNLOAD_CONTEXT)StartContext;

    if (unloadContext == NULL || unloadContext->DriverObject == NULL) {
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
        return;
    }

    unloadContext->UnloadStatus = STATUS_SUCCESS;
    unloadContext->CleanupStatus = STATUS_SUCCESS;
    unloadContext->DriverUnload = unloadContext->DriverObject->DriverUnload;

    if (unloadContext->AttemptDirectUnload && unloadContext->DriverUnload != NULL) {
        __try {
            unloadContext->DriverUnload(unloadContext->DriverObject);
            unloadContext->UnloadStatus = STATUS_SUCCESS;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            unloadContext->UnloadStatus = GetExceptionCode();
        }
        if (NT_SUCCESS(unloadContext->UnloadStatus)) {
            unloadContext->CleanupStatus = KswordARKDriverUnloadApplyCleanupUnsafe(
                unloadContext,
                TRUE);
        }
    }
    else if (KswordARKDriverUnloadShouldCleanupWithoutUnload(unloadContext->Flags)) {
        unloadContext->CleanupStatus = KswordARKDriverUnloadApplyCleanupUnsafe(
            unloadContext,
            FALSE);
        unloadContext->UnloadStatus = STATUS_PROCEDURE_NOT_FOUND;
    }
    else {
        unloadContext->UnloadStatus = STATUS_PROCEDURE_NOT_FOUND;
    }

    /* 中文说明：线程持有的 DriverObject 引用在这里释放，父线程只释放自己的引用。 */
    ObDereferenceObject(unloadContext->DriverObject);
    PsTerminateSystemThread(unloadContext->UnloadStatus);
}

/* 中文说明：启动系统线程并等待卸载结果。 */
static NTSTATUS
KswordARKDriverUnloadRunThread(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG Flags,
    _In_ ULONG TimeoutMilliseconds,
    _Out_ NTSTATUS* WaitStatusOut,
    _Out_ NTSTATUS* UnloadStatusOut,
    _Out_ NTSTATUS* CleanupStatusOut,
    _Out_ PDRIVER_UNLOAD* DriverUnloadOut,
    _Out_ ULONG* CleanupFlagsAppliedOut,
    _Out_ ULONG* DeletedDeviceCountOut,
    _In_opt_ const KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT* Preflight
    )
{
    HANDLE threadHandle = NULL;
    PETHREAD threadObject = NULL;
    LARGE_INTEGER timeoutInterval;
    PKSW_DRIVER_UNLOAD_CONTEXT unloadContext = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (DriverObject == NULL ||
        WaitStatusOut == NULL ||
        UnloadStatusOut == NULL ||
        CleanupStatusOut == NULL ||
        DriverUnloadOut == NULL ||
        CleanupFlagsAppliedOut == NULL ||
        DeletedDeviceCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *WaitStatusOut = STATUS_SUCCESS;
    *UnloadStatusOut = STATUS_SUCCESS;
    *CleanupStatusOut = STATUS_SUCCESS;
    *DriverUnloadOut = DriverObject->DriverUnload;
    *CleanupFlagsAppliedOut = 0UL;
    *DeletedDeviceCountOut = 0UL;

#pragma warning(push)
#pragma warning(disable:4996)
    unloadContext = (PKSW_DRIVER_UNLOAD_CONTEXT)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(*unloadContext),
        KSW_DRIVER_UNLOAD_TAG);
#pragma warning(pop)
    if (unloadContext == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* 中文说明：卸载线程可能超时后继续运行，因此上下文不能放在父线程栈上。 */
    RtlZeroMemory(unloadContext, sizeof(*unloadContext));
    unloadContext->DriverObject = DriverObject;
    unloadContext->Flags = Flags;
    unloadContext->UnloadStatus = STATUS_PENDING;
    unloadContext->CleanupStatus = STATUS_SUCCESS;
    unloadContext->DriverUnload = DriverObject->DriverUnload;
    unloadContext->AttemptDirectUnload =
        (Preflight != NULL &&
            Preflight->AllowDirectUnload &&
            Preflight->AllowDestructiveCleanup &&
            Preflight->HasValidDynData &&
            Preflight->HasPdbBackedDynData &&
            Preflight->HasValidDriverObjectOffsets &&
            Preflight->HasValidLoaderEvidence) ? TRUE : FALSE;
    if (Preflight != NULL) {
        (VOID)RtlStringCchCopyW(
            unloadContext->ServiceRegistryPath,
            RTL_NUMBER_OF(unloadContext->ServiceRegistryPath),
            Preflight->ServiceRegistryPath);
    }
    ObReferenceObject(DriverObject);

    if (TimeoutMilliseconds == 0UL) {
        TimeoutMilliseconds = KSW_DRIVER_UNLOAD_DEFAULT_TIMEOUT_MS;
    }
    if (TimeoutMilliseconds > KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS) {
        TimeoutMilliseconds = KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS;
    }

    status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        KswordARKDriverUnloadThreadRoutine,
        unloadContext);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(DriverObject);
        ExFreePoolWithTag(unloadContext, KSW_DRIVER_UNLOAD_TAG);
        return status;
    }

    status = ObReferenceObjectByHandle(
        threadHandle,
        SYNCHRONIZE,
        NULL,
        KernelMode,
        (PVOID*)&threadObject,
        NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(threadHandle);
        /* 中文说明：线程可能已经运行，不能释放上下文；由超时泄漏策略兜底。 */
        return status;
    }

    timeoutInterval.QuadPart = -((LONGLONG)TimeoutMilliseconds * 10LL * 1000LL);
    *WaitStatusOut = KeWaitForSingleObject(
        threadObject,
        Executive,
        KernelMode,
        FALSE,
        &timeoutInterval);

    *UnloadStatusOut = unloadContext->UnloadStatus;
    *CleanupStatusOut = unloadContext->CleanupStatus;
    *DriverUnloadOut = unloadContext->DriverUnload;
    *CleanupFlagsAppliedOut = unloadContext->CleanupFlagsApplied;
    *DeletedDeviceCountOut = unloadContext->DeletedDeviceCount;
    ObDereferenceObject(threadObject);
    ZwClose(threadHandle);

    if (*WaitStatusOut == STATUS_TIMEOUT) {
        /* 中文说明：超时后卸载线程仍可能访问上下文，宁可泄漏小块内存也不释放。 */
        return STATUS_TIMEOUT;
    }
    if (!NT_SUCCESS(*WaitStatusOut)) {
        ExFreePoolWithTag(unloadContext, KSW_DRIVER_UNLOAD_TAG);
        return *WaitStatusOut;
    }

    /*
     * 中文说明：目标没有 DriverUnload 时，强卸载路径可能只做 DriverObject
     * 中和后返回。如果 cleanup 已经成功，不能再把 STATUS_PROCEDURE_NOT_FOUND
     * 当成失败透传给 R3，否则 UI 会把 status=7 的成功清理误显示成错误。
     */
    if (*DriverUnloadOut == NULL &&
        unloadContext->CleanupFlagsApplied != 0UL &&
        NT_SUCCESS(*CleanupStatusOut)) {
        *UnloadStatusOut = STATUS_SUCCESS;
        status = STATUS_SUCCESS;
    }
    else {
        status = !NT_SUCCESS(*CleanupStatusOut) ? *CleanupStatusOut : *UnloadStatusOut;
    }

    ExFreePoolWithTag(unloadContext, KSW_DRIVER_UNLOAD_TAG);
    return status;
}

NTSTATUS
KswordARKDriverForceUnloadDriver(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut,
    _Out_opt_ KSW_DRIVER_UNLOAD_DIAGNOSTICS* Diagnostics
    )
/*++

Routine Description:

    Force-unload a DriverObject by name. 中文说明：第一优先路径只调用目标
    DriverObject->DriverUnload；当目标没有 DriverUnload 时，只有显式 flag
    才清 dispatch 表，不主动删除 DeviceObject。

Arguments:

    OutputBuffer - 固定响应缓冲。
    OutputBufferLength - 输出缓冲长度。
    Request - R3 请求，包含 DriverObject 名称和 flags。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；底层卸载结果写入 response->lastStatus。

--*/
{
    KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE* response = NULL;
    KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST requestSnapshot;
    PDRIVER_OBJECT driverObject = NULL;
    WCHAR normalizedName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
    KSW_DRIVER_UNLOAD_PREFLIGHT_RESULT preflightResult;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS waitStatus = STATUS_SUCCESS;
    NTSTATUS unloadStatus = STATUS_SUCCESS;
    NTSTATUS cleanupStatus = STATUS_SUCCESS;
    NTSTATUS callbackCleanupStatus = STATUS_SUCCESS;
    PDRIVER_UNLOAD driverUnload = NULL;
    ULONG cleanupFlagsApplied = 0UL;
    ULONG deletedDeviceCount = 0UL;
    ULONG requestedFlags = 0UL;
    KSW_DRIVER_UNLOAD_CALLBACK_CLEANUP_RESULT callbackCleanupResult;

    if (Diagnostics != NULL) {
        RtlZeroMemory(Diagnostics, sizeof(*Diagnostics));
    }

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(&callbackCleanupResult, sizeof(callbackCleanupResult));
    callbackCleanupResult.LastStatus = STATUS_SUCCESS;
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /*
     * 中文说明：IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER 使用 METHOD_BUFFERED；
     * KMDF 可能让输入请求和输出响应指向同一块 SystemBuffer。
     * 后续会清零输出缓冲，因此必须先把 R3 请求完整复制到本地栈变量。
     */
    RtlCopyMemory(&requestSnapshot, Request, sizeof(requestSnapshot));
    requestedFlags = requestSnapshot.flags;
    if (Diagnostics != NULL) {
        Diagnostics->requestedFlags = requestedFlags;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNKNOWN;
    response->reserved = requestedFlags;
    requestSnapshot.flags = KswordARKDriverUnloadSanitizeFlags(requestSnapshot.flags);
    if (Diagnostics != NULL) {
        Diagnostics->sanitizedFlags = requestSnapshot.flags;
    }
    response->flags = requestSnapshot.flags;
    response->lastStatus = STATUS_SUCCESS;
    response->waitStatus = STATUS_SUCCESS;
    response->callbackLastStatus = STATUS_SUCCESS;

    /*
     * 中文说明：强制卸载保留给恶意驱动处置场景，但默认只调用 DriverUnload。
     * 任何会持久改写 DriverObject、删除 DeviceObject 或移除回调的动作都必须
     * 同时带 ALLOW_DESTRUCTIVE_CLEANUP；旧 R3 的 FORCE_CLEANUP 会被降级，避免
     * 失败后留下半清理状态，导致目标驱动后续真实卸载时 bugcheck。
     */

    status = KswordARKDriverUnloadReferenceByName(
        &requestSnapshot,
        &driverObject,
        normalizedName,
        KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
    if (Diagnostics != NULL) {
        Diagnostics->stages |= KSW_DRIVER_UNLOAD_DIAG_STAGE_REFERENCE;
        Diagnostics->referenceStatus = status;
    }
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_REFERENCE_FAILED;
        response->lastStatus = status;
        if (normalizedName[0] != L'\0') {
            /* 中文说明：失败时也回填规范化对象名，便于 R3 日志确认实际解析目标。 */
            (VOID)RtlStringCchCopyW(
                response->driverName,
                KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS,
                normalizedName);
        }
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    response->driverObjectAddress = (ULONGLONG)(ULONG_PTR)driverObject;
    response->driverUnloadAddress = (ULONGLONG)(ULONG_PTR)driverObject->DriverUnload;
    RtlStringCchCopyW(
        response->driverName,
        KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS,
        normalizedName);

    status = KswordARKDriverUnloadBuildPreflightResult(
        driverObject,
        normalizedName,
        requestSnapshot.targetModuleBase,
        requestSnapshot.flags,
        &preflightResult);
    if (Diagnostics != NULL) {
        Diagnostics->stages |= KSW_DRIVER_UNLOAD_DIAG_STAGE_PREFLIGHT;
        Diagnostics->preflightBuildStatus = status;
    }
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED;
        response->lastStatus = status;
        response->waitStatus = status;
        *BytesWrittenOut = sizeof(*response);
        ObDereferenceObject(driverObject);
        return STATUS_SUCCESS;
    }
    if (Diagnostics != NULL) {
        KswordARKDriverUnloadCapturePreflightDiagnostics(Diagnostics, &preflightResult);
    }

    if (!preflightResult.AllowDirectUnload &&
        !preflightResult.AllowZwUnload &&
        !preflightResult.AllowDestructiveCleanup) {
        const NTSTATUS denyStatus = KswordARKDriverUnloadPreflightDenyStatus(&preflightResult);
        if (Diagnostics != NULL) {
            Diagnostics->preflightDenyStatus = denyStatus;
        }
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED;
        response->lastStatus = denyStatus;
        response->waitStatus = denyStatus;
        *BytesWrittenOut = sizeof(*response);
        ObDereferenceObject(driverObject);
        return STATUS_SUCCESS;
    }

    if (!preflightResult.AllowDestructiveCleanup) {
        const ULONG destructiveMask =
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD |
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD |
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER |
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD |
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS |
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT |
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_REMOVE_CALLBACKS_BY_MODULE_BASE |
            KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP;
        requestSnapshot.flags &= ~destructiveMask;
    }
    if (Diagnostics != NULL) {
        Diagnostics->finalFlags = requestSnapshot.flags;
    }
    response->flags = requestSnapshot.flags;

    /*
     * 中文说明：ZwUnloadDriver 必须在不持有目标 DriverObject 引用的状态下执行。
     * ObReferenceObjectByName 得到的引用只用于 preflight；真正系统卸载前先释放。
     * 如果系统卸载失败且 preflight 允许强制路径，再重新引用对象进入手工卸载。
     */
    ObDereferenceObject(driverObject);
    driverObject = NULL;

    if (preflightResult.AllowZwUnload) {
        status = KswordARKDriverUnloadRunZwOnly(
            preflightResult.ServiceRegistryPath,
            requestSnapshot.timeoutMilliseconds,
            &waitStatus,
            &unloadStatus);
        if (Diagnostics != NULL) {
            Diagnostics->stages |= KSW_DRIVER_UNLOAD_DIAG_STAGE_ZW;
            Diagnostics->zwRunStatus = status;
            Diagnostics->zwWaitStatus = waitStatus;
            Diagnostics->zwUnloadStatus = unloadStatus;
        }
        cleanupStatus = STATUS_SUCCESS;
        if (NT_SUCCESS(status)) {
            /*
             * 中文说明：ZwUnloadDriver 的返回值只说明系统卸载路径已正常返回，
             * 不能单独证明 DriverObject 已经不可引用、镜像也已离开 loader 视图。
             * 因此普通路径也必须通过同一闭环验证；验证失败时，如果调用方显式
             * 允许 destructive fallback，则继续进入 direct fallback，否则把失败
             * 原因回填给 R3，避免 UI 把“未真正卸载”显示成成功。
             */
            NTSTATUS verifyStatus = KswordARKDriverUnloadVerifyClosedLoop(
                &requestSnapshot,
                normalizedName,
                &preflightResult);
            if (Diagnostics != NULL) {
                Diagnostics->stages |= KSW_DRIVER_UNLOAD_DIAG_STAGE_ZW_VERIFY;
                Diagnostics->zwVerifyStatus = verifyStatus;
            }
            if (NT_SUCCESS(verifyStatus)) {
                response->lastStatus = status;
                response->waitStatus = waitStatus;
                response->cleanupFlagsApplied = 0UL;
                response->deletedDeviceCount = 0UL;
                response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED;
                *BytesWrittenOut = sizeof(*response);
                return STATUS_SUCCESS;
            }
            status = verifyStatus;
        }
        if (NT_SUCCESS(status) ||
            !KswordARKDriverUnloadCanUseDestructiveFallback(&preflightResult, requestSnapshot.flags)) {
            const NTSTATUS reportStatus = NT_SUCCESS(status)
                ? KswordARKDriverUnloadPreflightDenyStatus(&preflightResult)
                : status;
            if (Diagnostics != NULL && Diagnostics->preflightDenyStatus == STATUS_SUCCESS) {
                Diagnostics->preflightDenyStatus = NT_SUCCESS(status)
                    ? reportStatus
                    : KswordARKDriverUnloadPreflightDenyStatus(&preflightResult);
            }
            response->lastStatus = reportStatus;
            response->waitStatus = waitStatus;
            response->cleanupFlagsApplied = 0UL;
            response->deletedDeviceCount = 0UL;
            response->status = (reportStatus == STATUS_TIMEOUT || waitStatus == STATUS_TIMEOUT)
                ? KSWORD_ARK_DRIVER_UNLOAD_STATUS_WAIT_TIMEOUT
                : KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED;
            *BytesWrittenOut = sizeof(*response);
            return STATUS_SUCCESS;
        }
    }

    if (!KswordARKDriverUnloadCanUseDestructiveFallback(&preflightResult, requestSnapshot.flags)) {
        const NTSTATUS denyStatus = KswordARKDriverUnloadPreflightDenyStatus(&preflightResult);
        if (Diagnostics != NULL) {
            Diagnostics->preflightDenyStatus = denyStatus;
        }
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED;
        response->lastStatus = denyStatus;
        response->waitStatus = denyStatus;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    /*
     * 中文说明：ReactOS/I/O 管理器卸载模型不是“只调用 DriverUnload”。
     * direct fallback 只有在 DriverUnload 清空设备链后，把 DriverObject 标记为
     * temporary，并释放本驱动持有的最后引用，才有机会进入对象删除与镜像卸载
     * 闭环。因此进入强制 fallback 后 R0 自动补上 MAKE_TEMPORARY_OBJECT。
     */
    requestSnapshot.flags |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT;
    if (Diagnostics != NULL) {
        Diagnostics->finalFlags = requestSnapshot.flags;
    }
    response->flags = requestSnapshot.flags;

    status = KswordARKDriverUnloadReferenceByName(
        &requestSnapshot,
        &driverObject,
        normalizedName,
        KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
    if (Diagnostics != NULL) {
        Diagnostics->referenceStatus = status;
    }
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_REFERENCE_FAILED;
        response->lastStatus = status;
        response->waitStatus = waitStatus;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    response->driverObjectAddress = (ULONGLONG)(ULONG_PTR)driverObject;
    response->driverUnloadAddress = (ULONGLONG)(ULONG_PTR)driverObject->DriverUnload;

    if (KswordARKDriverUnloadCanPreCleanupCallbacks(&preflightResult, &requestSnapshot)) {
        callbackCleanupStatus = KswordARKDriverUnloadRemoveCallbacksByModuleBase(
            requestSnapshot.targetModuleBase,
            &callbackCleanupResult);
        response->callbackCandidates = callbackCleanupResult.Candidates;
        response->callbacksRemoved = callbackCleanupResult.Removed;
        response->callbackFailures = callbackCleanupResult.Failures;
        response->callbackLastStatus = callbackCleanupResult.LastStatus;
        if (!NT_SUCCESS(callbackCleanupStatus) &&
            response->callbackLastStatus == STATUS_SUCCESS) {
            response->callbackLastStatus = callbackCleanupStatus;
        }
    }

    status = KswordARKDriverUnloadRunThread(
        driverObject,
        requestSnapshot.flags,
        requestSnapshot.timeoutMilliseconds,
        &waitStatus,
        &unloadStatus,
        &cleanupStatus,
        &driverUnload,
        &cleanupFlagsApplied,
        &deletedDeviceCount,
        &preflightResult);
    if (Diagnostics != NULL) {
        Diagnostics->stages |= KSW_DRIVER_UNLOAD_DIAG_STAGE_DIRECT;
        Diagnostics->directRunStatus = status;
        Diagnostics->directWaitStatus = waitStatus;
        Diagnostics->directUnloadStatus = unloadStatus;
        Diagnostics->directCleanupStatus = cleanupStatus;
    }

    response->driverUnloadAddress = (ULONGLONG)(ULONG_PTR)driverUnload;
    response->lastStatus = status;
    response->waitStatus = waitStatus;
    response->cleanupFlagsApplied = cleanupFlagsApplied;
    response->deletedDeviceCount = deletedDeviceCount;

    if (driverObject != NULL) {
        ObDereferenceObject(driverObject);
        driverObject = NULL;
    }
    if (NT_SUCCESS(status) &&
        NT_SUCCESS(cleanupStatus) &&
        cleanupFlagsApplied != 0UL &&
        (cleanupFlagsApplied & KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT) != 0UL) {
        NTSTATUS verifyStatus = KswordARKDriverUnloadVerifyClosedLoop(
            &requestSnapshot,
            normalizedName,
            &preflightResult);
        if (Diagnostics != NULL) {
            Diagnostics->stages |= KSW_DRIVER_UNLOAD_DIAG_STAGE_DIRECT_VERIFY;
            Diagnostics->directVerifyStatus = verifyStatus;
        }
        if (!NT_SUCCESS(verifyStatus)) {
            status = verifyStatus;
            cleanupStatus = verifyStatus;
            response->lastStatus = verifyStatus;
        }
    }

    if (status == STATUS_TIMEOUT || waitStatus == STATUS_TIMEOUT) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_WAIT_TIMEOUT;
    }
    else if (!NT_SUCCESS(cleanupStatus)) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_CLEANUP_FAILED;
    }
    else if (driverUnload == NULL && unloadStatus == STATUS_PROCEDURE_NOT_FOUND) {
        response->status = NT_SUCCESS(cleanupStatus) &&
            KswordARKDriverUnloadHasCleanupRequest(requestSnapshot.flags)
            ? KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP
            : KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOAD_ROUTINE_MISSING;
    }
    else if (NT_SUCCESS(status)) {
        response->status = KswordARKDriverUnloadHasCleanupRequest(requestSnapshot.flags)
            ? KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP
            : KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED;
    }
    else {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED;
    }

    if (Diagnostics != NULL && Diagnostics->finalFlags == 0UL) {
        Diagnostics->finalFlags = requestSnapshot.flags;
    }
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
