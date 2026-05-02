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
} KSW_DRIVER_UNLOAD_CONTEXT, *PKSW_DRIVER_UNLOAD_CONTEXT;

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

/* 中文说明：可选清理 dispatch 表，和 SKT64 的 force unload 语义对齐但由 flag 控制。 */
static VOID
KswordARKDriverUnloadClearDispatchUnsafe(
    _Inout_ PDRIVER_OBJECT DriverObject
    )
{
    if (DriverObject == NULL) {
        return;
    }

    __try {
        DriverObject->FastIoDispatch = NULL;
        RtlZeroMemory(DriverObject->MajorFunction, sizeof(DriverObject->MajorFunction));
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
    ULONG deletedDeviceCount = 0UL;

    if (DeletedDeviceCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *DeletedDeviceCountOut = 0UL;

    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        deviceCursor = DriverObject->DeviceObject;
        while (deviceCursor != NULL &&
            deletedDeviceCount < KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT) {
            PDEVICE_OBJECT nextDevice = deviceCursor->NextDevice;

            /* 中文说明：IoDeleteDevice 会修改驱动设备链，因此先保存 NextDevice。 */
            IoDeleteDevice(deviceCursor);
            deletedDeviceCount += 1UL;
            deviceCursor = nextDevice;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *DeletedDeviceCountOut = deletedDeviceCount;
        return GetExceptionCode();
    }

    *DeletedDeviceCountOut = deletedDeviceCount;
    if (deletedDeviceCount >= KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT &&
        deviceCursor != NULL) {
        return STATUS_BUFFER_OVERFLOW;
    }
    return STATUS_SUCCESS;
}

/* 中文说明：执行强制卸载后的附加清理，所有动作都必须由请求 flag 明确启用。 */
static NTSTATUS
KswordARKDriverUnloadApplyCleanupUnsafe(
    _Inout_ PKSW_DRIVER_UNLOAD_CONTEXT UnloadContext,
    _In_ BOOLEAN DriverUnloadWasCalled
    )
{
    NTSTATUS cleanupStatus = STATUS_SUCCESS;

    if (UnloadContext == NULL || UnloadContext->DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
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
    if (!DriverUnloadWasCalled &&
        (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD) != 0UL) {
        ULONG deletedDeviceCount = 0UL;
        NTSTATUS deleteStatus = KswordARKDriverUnloadDeleteDeviceObjectsUnsafe(
            UnloadContext->DriverObject,
            &deletedDeviceCount);
        UnloadContext->DeletedDeviceCount = deletedDeviceCount;
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD;
        if (!NT_SUCCESS(deleteStatus)) {
            cleanupStatus = deleteStatus;
        }
    }
    if ((UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER) != 0UL) {
        KswordARKDriverUnloadClearUnloadPointerUnsafe(UnloadContext->DriverObject);
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER;
    }
    if ((UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT) != 0UL) {
        __try {
            ObMakeTemporaryObject(UnloadContext->DriverObject);
            UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            cleanupStatus = GetExceptionCode();
        }
    }

    return cleanupStatus;
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
    if (unloadContext->DriverUnload != NULL) {
        __try {
            unloadContext->DriverUnload(unloadContext->DriverObject);
            unloadContext->UnloadStatus = STATUS_SUCCESS;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            unloadContext->UnloadStatus = GetExceptionCode();
        }
        unloadContext->CleanupStatus = KswordARKDriverUnloadApplyCleanupUnsafe(
            unloadContext,
            TRUE);
    }
    else if ((unloadContext->Flags & (KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT)) != 0UL) {
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
    _Out_ PDRIVER_UNLOAD* DriverUnloadOut
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
        DriverUnloadOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *WaitStatusOut = STATUS_SUCCESS;
    *UnloadStatusOut = STATUS_SUCCESS;
    *CleanupStatusOut = STATUS_SUCCESS;
    *DriverUnloadOut = DriverObject->DriverUnload;
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
     * 中文说明：目标没有 DriverUnload 时，FORCE_CLEANUP 路径本来就是“清理
     * DriverObject/DeviceObject 后返回”。如果 cleanup 已经成功，不能再把
     * STATUS_PROCEDURE_NOT_FOUND 当成失败透传给 R3，否则 UI 会把 status=7
     * 的成功清理误显示成错误。
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
    _Out_ size_t* BytesWrittenOut
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
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS waitStatus = STATUS_SUCCESS;
    NTSTATUS unloadStatus = STATUS_SUCCESS;
    NTSTATUS cleanupStatus = STATUS_SUCCESS;
    NTSTATUS callbackCleanupStatus = STATUS_SUCCESS;
    PDRIVER_UNLOAD driverUnload = NULL;
    KSW_DRIVER_UNLOAD_CALLBACK_CLEANUP_RESULT callbackCleanupResult;

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

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNKNOWN;
    response->flags = requestSnapshot.flags;
    response->lastStatus = STATUS_SUCCESS;
    response->waitStatus = STATUS_SUCCESS;
    response->callbackLastStatus = STATUS_SUCCESS;

    if ((requestSnapshot.flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_REMOVE_CALLBACKS_BY_MODULE_BASE) != 0UL &&
        (requestSnapshot.flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_TARGET_MODULE_BASE_PRESENT) != 0UL &&
        requestSnapshot.targetModuleBase != 0ULL) {
        callbackCleanupStatus = KswordARKDriverUnloadRemoveCallbacksByModuleBase(
            requestSnapshot.targetModuleBase,
            &callbackCleanupResult);
        response->callbackCandidates = callbackCleanupResult.Candidates;
        response->callbacksRemoved = callbackCleanupResult.Removed;
        response->callbackFailures = callbackCleanupResult.Failures;
        response->callbackLastStatus = callbackCleanupResult.LastStatus;
    }

    status = KswordARKDriverUnloadReferenceByName(
        &requestSnapshot,
        &driverObject,
        normalizedName,
        KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
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

    status = KswordARKDriverUnloadRunThread(
        driverObject,
        requestSnapshot.flags,
        requestSnapshot.timeoutMilliseconds,
        &waitStatus,
        &unloadStatus,
        &cleanupStatus,
        &driverUnload);

    response->driverUnloadAddress = (ULONGLONG)(ULONG_PTR)driverUnload;
    response->lastStatus = status;
    response->waitStatus = waitStatus;
    if (!NT_SUCCESS(callbackCleanupStatus) && response->callbackLastStatus == STATUS_SUCCESS) {
        response->callbackLastStatus = callbackCleanupStatus;
    }
    if (status == STATUS_TIMEOUT || waitStatus == STATUS_TIMEOUT) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_WAIT_TIMEOUT;
    }
    else if (!NT_SUCCESS(cleanupStatus)) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_CLEANUP_FAILED;
    }
    else if (driverUnload == NULL && unloadStatus == STATUS_PROCEDURE_NOT_FOUND) {
        response->status = NT_SUCCESS(cleanupStatus) &&
            (requestSnapshot.flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP) != 0UL
            ? KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP
            : KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOAD_ROUTINE_MISSING;
    }
    else if (NT_SUCCESS(status)) {
        response->status = (requestSnapshot.flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP) != 0UL
            ? KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP
            : KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED;
    }
    else {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED;
    }

    ObDereferenceObject(driverObject);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
