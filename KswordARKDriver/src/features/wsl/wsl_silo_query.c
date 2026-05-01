/*++

Module Name:

    wsl_silo_query.c

Abstract:

    Phase-13 WSL/Pico and Silo read-only diagnostics.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntimage.h>

#define KSWORD_ARK_WSL_SYSTEM_MODULE_INFORMATION_CLASS 11UL
#define KSWORD_ARK_WSL_OFFSET_UNAVAILABLE KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE
#define KSWORD_ARK_WSL_PROCESS_SUBSYSTEM_INFORMATION 75UL
#define KSWORD_ARK_WSL_THREAD_SUBSYSTEM_INFORMATION 38UL

#define KSWORD_ARK_WSL_SILO_ROUTINE_GET_IDENTIFIER       0x00000001UL
#define KSWORD_ARK_WSL_SILO_ROUTINE_GET_EFFECTIVE_SERVER 0x00000002UL
#define KSWORD_ARK_WSL_SILO_ROUTINE_IS_HOST              0x00000004UL
#define KSWORD_ARK_WSL_SILO_ROUTINE_GET_SERVICE_SESSION  0x00000008UL
#define KSWORD_ARK_WSL_SILO_ROUTINE_GET_ACTIVE_CONSOLE   0x00000010UL
#define KSWORD_ARK_WSL_SILO_ROUTINE_GET_CONTAINER_ID     0x00000020UL

typedef BOOLEAN(NTAPI* KSWORD_LXP_THREAD_GET_CURRENT_FN)(
    _Outptr_ PVOID* PicoContextOut
    );

typedef ULONG(NTAPI* KSWORD_PS_GET_SILO_IDENTIFIER_FN)(
    _In_opt_ PVOID Silo
    );

typedef PVOID(NTAPI* KSWORD_PS_GET_EFFECTIVE_SERVER_SILO_FN)(
    _In_opt_ PVOID Silo
    );

typedef BOOLEAN(NTAPI* KSWORD_PS_IS_HOST_SILO_FN)(
    _In_opt_ PVOID Silo
    );

typedef ULONG(NTAPI* KSWORD_PS_GET_SERVER_SILO_SESSION_FN)(
    _In_opt_ PVOID Silo
    );

typedef GUID*(NTAPI* KSWORD_PS_GET_SILO_CONTAINER_ID_FN)(
    _In_ PVOID Silo
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN)(
    _In_ HANDLE ProcessHandle,
    _In_ ULONG ProcessInformationClass,
    _Out_writes_bytes_(ProcessInformationLength) PVOID ProcessInformation,
    _In_ ULONG ProcessInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_QUERY_INFORMATION_THREAD_FN)(
    _In_ HANDLE ThreadHandle,
    _In_ ULONG ThreadInformationClass,
    _Out_writes_bytes_(ThreadInformationLength) PVOID ThreadInformation,
    _In_ ULONG ThreadInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

typedef struct _KSWORD_WSL_SYSTEM_MODULE_ENTRY
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
} KSWORD_WSL_SYSTEM_MODULE_ENTRY, *PKSWORD_WSL_SYSTEM_MODULE_ENTRY;

typedef struct _KSWORD_WSL_SYSTEM_MODULE_INFORMATION
{
    ULONG NumberOfModules;
    KSWORD_WSL_SYSTEM_MODULE_ENTRY Modules[1];
} KSWORD_WSL_SYSTEM_MODULE_INFORMATION, *PKSWORD_WSL_SYSTEM_MODULE_INFORMATION;

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTSYSAPI
NTSTATUS
NTAPI
PsLookupThreadByThreadId(
    _In_ HANDLE ThreadId,
    _Outptr_ PETHREAD* Thread
    );

NTKERNELAPI
HANDLE
PsGetThreadId(
    _In_ PETHREAD Thread
    );

NTKERNELAPI
NTSTATUS
ObOpenObjectByPointer(
    _In_ PVOID Object,
    _In_ ULONG HandleAttributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PHANDLE Handle
    );

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_ PCCH RoutineName
    );

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION (0x1000)
#endif

#ifndef THREAD_QUERY_LIMITED_INFORMATION
#define THREAD_QUERY_LIMITED_INFORMATION (0x0800)
#endif

static BOOLEAN
KswordARKWslOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    判断 lxcore DynData offset 是否可用。中文说明：0xffff 和 Ksword 自己的
    unavailable sentinel 都视为不可读。

Arguments:

    Offset - DynData offset.

Return Value:

    TRUE 表示可以用于指针读取。

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static ULONG
KswordARKWslNormalizeOffset(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    将内部 offset sentinel 转换为共享协议 sentinel。中文说明：R3 只需要判断
    KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE，不暴露内部 0xffff 细节。

Arguments:

    Offset - 原始 offset.

Return Value:

    可用 offset 或 unavailable sentinel。

--*/
{
    return KswordARKWslOffsetPresent(Offset) ? Offset : KSWORD_ARK_WSL_OFFSET_UNAVAILABLE;
}

static CHAR
KswordARKWslAsciiLower(
    _In_ CHAR Character
    )
{
    if (Character >= 'A' && Character <= 'Z') {
        return (CHAR)(Character + ('a' - 'A'));
    }
    return Character;
}

static BOOLEAN
KswordARKWslModuleNameEquals(
    _In_reads_bytes_(LeftBytes) const UCHAR* LeftText,
    _In_ ULONG LeftBytes,
    _In_z_ PCSTR RightText
    )
/*++

Routine Description:

    比较 SystemModuleInformation 中的 bounded ANSI 模块名。中文说明：模块名
    缓冲不保证尾零，必须按长度上限逐字节比较。

Arguments:

    LeftText - bounded ANSI text.
    LeftBytes - 可读字节数。
    RightText - 目标模块名。

Return Value:

    TRUE 表示大小写不敏感匹配。

--*/
{
    ULONG index = 0UL;

    if (LeftText == NULL || LeftBytes == 0UL || RightText == NULL) {
        return FALSE;
    }

    for (index = 0UL; index < LeftBytes; ++index) {
        CHAR leftCharacter = (CHAR)LeftText[index];
        CHAR rightCharacter = RightText[index];

        if (rightCharacter == '\0') {
            return (leftCharacter == '\0') ? TRUE : FALSE;
        }
        if (leftCharacter == '\0') {
            return FALSE;
        }
        if (KswordARKWslAsciiLower(leftCharacter) != KswordARKWslAsciiLower(rightCharacter)) {
            return FALSE;
        }
    }

    return (RightText[index] == '\0') ? TRUE : FALSE;
}

static PVOID
KswordARKWslAllocate(
    _In_ SIZE_T Bytes
    )
{
    if (Bytes == 0U) {
        return NULL;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, Bytes, 'wLsK');
#pragma warning(pop)
}

static PVOID
KswordARKWslFindExportedRoutine(
    _In_z_ PCSTR ModuleName,
    _In_z_ PCSTR RoutineName
    )
/*++

Routine Description:

    从已加载模块列表查找导出函数。中文说明：这里仿照 System Informer 的
    KphGetRoutineAddress 思路，但使用 SystemModuleInformation，避免依赖
    PsLoadedModuleList/PsLoadedModuleResource 私有符号。

Arguments:

    ModuleName - 目标模块名，例如 lxcore.sys。
    RoutineName - 导出函数名，例如 LxpThreadGetCurrent。

Return Value:

    导出地址或 NULL。

--*/
{
    PKSWORD_WSL_SYSTEM_MODULE_INFORMATION moduleInfo = NULL;
    ULONG requiredBytes = 0UL;
    ULONG queryBytes = 0UL;
    ULONG index = 0UL;
    PVOID routineAddress = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    status = ZwQuerySystemInformation(
        KSWORD_ARK_WSL_SYSTEM_MODULE_INFORMATION_CLASS,
        NULL,
        0UL,
        &requiredBytes);
    if (requiredBytes == 0UL) {
        return NULL;
    }

    queryBytes = requiredBytes + (64UL * 1024UL);
    moduleInfo = (PKSWORD_WSL_SYSTEM_MODULE_INFORMATION)KswordARKWslAllocate(queryBytes);
    if (moduleInfo == NULL) {
        return NULL;
    }

    status = ZwQuerySystemInformation(
        KSWORD_ARK_WSL_SYSTEM_MODULE_INFORMATION_CLASS,
        moduleInfo,
        queryBytes,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(moduleInfo, 'wLsK');
        return NULL;
    }

    for (index = 0UL; index < moduleInfo->NumberOfModules; ++index) {
        const KSWORD_WSL_SYSTEM_MODULE_ENTRY* entry = &moduleInfo->Modules[index];
        const UCHAR* fileName = entry->FullPathName;
        ULONG fileNameBytes = sizeof(entry->FullPathName);

        if (entry->OffsetToFileName < sizeof(entry->FullPathName)) {
            fileName = entry->FullPathName + entry->OffsetToFileName;
            fileNameBytes = (ULONG)(sizeof(entry->FullPathName) - entry->OffsetToFileName);
        }

        if (!KswordARKWslModuleNameEquals(fileName, fileNameBytes, ModuleName)) {
            continue;
        }

        __try {
            routineAddress = RtlFindExportedRoutineByName(entry->ImageBase, RoutineName);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            routineAddress = NULL;
        }
        break;
    }

    ExFreePoolWithTag(moduleInfo, 'wLsK');
    return routineAddress;
}

static KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN
KswordARKWslResolveZwQueryInformationProcess(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwQueryInformationProcess");
    return (KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_ZW_QUERY_INFORMATION_THREAD_FN
KswordARKWslResolveZwQueryInformationThread(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwQueryInformationThread");
    return (KSWORD_ZW_QUERY_INFORMATION_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static ULONG
KswordARKWslResolveSiloRoutineMask(
    VOID
    )
/*++

Routine Description:

    动态解析公开 Silo 导出函数可用性。中文说明：Phase-13 第一版仅展示可用性，
    不注册 silo monitor，避免引入额外生命周期复杂度。

Arguments:

    None.

Return Value:

    KSWORD_ARK_WSL_SILO_ROUTINE_* 位图。

--*/
{
    ULONG mask = 0UL;
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetSiloIdentifier");
    if (MmGetSystemRoutineAddress(&routineName) != NULL) {
        mask |= KSWORD_ARK_WSL_SILO_ROUTINE_GET_IDENTIFIER;
    }
    RtlInitUnicodeString(&routineName, L"PsGetEffectiveServerSilo");
    if (MmGetSystemRoutineAddress(&routineName) != NULL) {
        mask |= KSWORD_ARK_WSL_SILO_ROUTINE_GET_EFFECTIVE_SERVER;
    }
    RtlInitUnicodeString(&routineName, L"PsIsHostSilo");
    if (MmGetSystemRoutineAddress(&routineName) != NULL) {
        mask |= KSWORD_ARK_WSL_SILO_ROUTINE_IS_HOST;
    }
    RtlInitUnicodeString(&routineName, L"PsGetServerSiloServiceSessionId");
    if (MmGetSystemRoutineAddress(&routineName) != NULL) {
        mask |= KSWORD_ARK_WSL_SILO_ROUTINE_GET_SERVICE_SESSION;
    }
    RtlInitUnicodeString(&routineName, L"PsGetServerSiloActiveConsoleId");
    if (MmGetSystemRoutineAddress(&routineName) != NULL) {
        mask |= KSWORD_ARK_WSL_SILO_ROUTINE_GET_ACTIVE_CONSOLE;
    }
    RtlInitUnicodeString(&routineName, L"PsGetSiloContainerId");
    if (MmGetSystemRoutineAddress(&routineName) != NULL) {
        mask |= KSWORD_ARK_WSL_SILO_ROUTINE_GET_CONTAINER_ID;
    }

    return mask;
}

static NTSTATUS
KswordARKWslQueryProcessSubsystem(
    _In_ PEPROCESS ProcessObject,
    _Out_ ULONG* SubsystemOut
    )
/*++

Routine Description:

    查询目标进程 subsystem 类型。中文说明：Windows 旧版本不支持该 info class
    时按 Win32 处理，与 System Informer 行为一致。

Arguments:

    ProcessObject - 目标 EPROCESS。
    SubsystemOut - 接收 subsystem 类型。

Return Value:

    STATUS_SUCCESS 或底层打开/查询状态。

--*/
{
    KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN zwQueryInformationProcess = NULL;
    HANDLE processHandle = NULL;
    ULONG subsystemType = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || SubsystemOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *SubsystemOut = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;
    zwQueryInformationProcess = KswordARKWslResolveZwQueryInformationProcess();
    if (zwQueryInformationProcess == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = ObOpenObjectByPointer(
        ProcessObject,
        OBJ_KERNEL_HANDLE,
        NULL,
        PROCESS_QUERY_LIMITED_INFORMATION,
        *PsProcessType,
        KernelMode,
        &processHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = zwQueryInformationProcess(
        processHandle,
        KSWORD_ARK_WSL_PROCESS_SUBSYSTEM_INFORMATION,
        &subsystemType,
        sizeof(subsystemType),
        NULL);
    ZwClose(processHandle);

    if (status == STATUS_INVALID_INFO_CLASS) {
        subsystemType = KSWORD_ARK_WSL_SUBSYSTEM_WIN32;
        status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(status)) {
        *SubsystemOut = subsystemType;
    }

    return status;
}

static NTSTATUS
KswordARKWslQueryThreadSubsystem(
    _In_ PETHREAD ThreadObject,
    _Out_ ULONG* SubsystemOut
    )
/*++

Routine Description:

    查询目标线程 subsystem 类型。中文说明：该字段用来区分 WSL/Pico 线程与普通
    Win32 线程，失败时不尝试读取 lxcore 私有结构。

Arguments:

    ThreadObject - 目标 ETHREAD。
    SubsystemOut - 接收 subsystem 类型。

Return Value:

    STATUS_SUCCESS 或底层打开/查询状态。

--*/
{
    KSWORD_ZW_QUERY_INFORMATION_THREAD_FN zwQueryInformationThread = NULL;
    HANDLE threadHandle = NULL;
    ULONG subsystemType = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;
    NTSTATUS status = STATUS_SUCCESS;

    if (ThreadObject == NULL || SubsystemOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *SubsystemOut = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;
    zwQueryInformationThread = KswordARKWslResolveZwQueryInformationThread();
    if (zwQueryInformationThread == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = ObOpenObjectByPointer(
        ThreadObject,
        OBJ_KERNEL_HANDLE,
        NULL,
        THREAD_QUERY_LIMITED_INFORMATION,
        *PsThreadType,
        KernelMode,
        &threadHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = zwQueryInformationThread(
        threadHandle,
        KSWORD_ARK_WSL_THREAD_SUBSYSTEM_INFORMATION,
        &subsystemType,
        sizeof(subsystemType),
        NULL);
    ZwClose(threadHandle);

    if (status == STATUS_INVALID_INFO_CLASS) {
        subsystemType = KSWORD_ARK_WSL_SUBSYSTEM_WIN32;
        status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(status)) {
        *SubsystemOut = subsystemType;
    }

    return status;
}

static NTSTATUS
KswordARKWslReadLinuxIdsFromCurrentThread(
    _In_ const KSW_DYN_LXCORE_OFFSETS* Offsets,
    _Out_ ULONG* LinuxPidOut,
    _Out_ ULONG* LinuxTidOut
    )
/*++

Routine Description:

    从当前线程 lxcore pico context 中读取 Linux PID/TID。中文说明：System
    Informer 对任意线程使用 APC 回到原线程环境；这里第一版只在当前线程环境
    下读取，避免跨线程 APC 生命周期风险。

Arguments:

    Offsets - lxcore DynData offsets。
    LinuxPidOut - 接收 Linux PID。
    LinuxTidOut - 接收 Linux TID。

Return Value:

    STATUS_SUCCESS 或读取失败状态。

--*/
{
    KSWORD_LXP_THREAD_GET_CURRENT_FN lxpThreadGetCurrent = NULL;
    PVOID picoContext = NULL;
    PVOID value = NULL;
    ULONG linuxPid = 0UL;
    ULONG linuxTid = 0UL;

    if (Offsets == NULL || LinuxPidOut == NULL || LinuxTidOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *LinuxPidOut = 0UL;
    *LinuxTidOut = 0UL;
    if (!KswordARKWslOffsetPresent(Offsets->LxPicoProc) ||
        !KswordARKWslOffsetPresent(Offsets->LxPicoProcInfo) ||
        !KswordARKWslOffsetPresent(Offsets->LxPicoProcInfoPID) ||
        !KswordARKWslOffsetPresent(Offsets->LxPicoThrdInfo) ||
        !KswordARKWslOffsetPresent(Offsets->LxPicoThrdInfoTID)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    lxpThreadGetCurrent =
        (KSWORD_LXP_THREAD_GET_CURRENT_FN)KswordARKWslFindExportedRoutine(
            "lxcore.sys",
            "LxpThreadGetCurrent");
    if (lxpThreadGetCurrent == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        if (!lxpThreadGetCurrent(&picoContext) || picoContext == NULL) {
            return STATUS_NOT_FOUND;
        }

        value = *(PVOID*)((PUCHAR)picoContext + Offsets->LxPicoThrdInfo);
        linuxTid = *(ULONG*)((PUCHAR)value + Offsets->LxPicoThrdInfoTID);

        value = *(PVOID*)((PUCHAR)picoContext + Offsets->LxPicoProc);
        value = *(PVOID*)((PUCHAR)value + Offsets->LxPicoProcInfo);
        linuxPid = *(ULONG*)((PUCHAR)value + Offsets->LxPicoProcInfoPID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    *LinuxPidOut = linuxPid;
    *LinuxTidOut = linuxTid;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverQueryWslSilo(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_WSL_SILO_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询 WSL/Pico 和 Silo 基础诊断信息。中文说明：lxcore 加载/匹配状态来自
    DynData；Linux PID/TID 第一版只在请求线程就是当前线程时解析。

Arguments:

    OutputBuffer - 响应缓冲区。
    OutputBufferLength - 响应长度。
    Request - 查询请求。
    BytesWrittenOut - 接收 sizeof(response)。

Return Value:

    STATUS_SUCCESS 表示响应包有效；细节通过 queryStatus 和各 NTSTATUS 返回。

--*/
{
    KSWORD_ARK_QUERY_WSL_SILO_RESPONSE* response = NULL;
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    PETHREAD threadObject = NULL;
    ULONG requestFlags = 0UL;
    ULONG processSubsystem = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;
    ULONG threadSubsystem = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_WSL_SILO_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    RtlZeroMemory(&dynState, sizeof(dynState));

    response = (KSWORD_ARK_QUERY_WSL_SILO_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WSL_SILO_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->queryStatus = KSWORD_ARK_WSL_QUERY_STATUS_UNAVAILABLE;
    response->processLookupStatus = STATUS_NOT_SUPPORTED;
    response->threadLookupStatus = STATUS_NOT_SUPPORTED;
    response->processSubsystemStatus = STATUS_NOT_SUPPORTED;
    response->threadSubsystemStatus = STATUS_NOT_SUPPORTED;
    response->linuxPidStatus = STATUS_NOT_SUPPORTED;
    response->linuxTidStatus = STATUS_NOT_SUPPORTED;
    response->siloStatus = STATUS_NOT_SUPPORTED;
    response->processId = Request->processId;
    response->threadId = Request->threadId;
    response->processSubsystemType = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;
    response->threadSubsystemType = KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN;

    KswordARKDynDataSnapshot(&dynState);
    response->dynDataCapabilityMask = dynState.CapabilityMask;
    response->lxcore = dynState.Lxcore;
    response->lxPicoProcOffset = KswordARKWslNormalizeOffset(dynState.LxcoreOffsets.LxPicoProc);
    response->lxPicoProcInfoOffset = KswordARKWslNormalizeOffset(dynState.LxcoreOffsets.LxPicoProcInfo);
    response->lxPicoProcInfoPidOffset = KswordARKWslNormalizeOffset(dynState.LxcoreOffsets.LxPicoProcInfoPID);
    response->lxPicoThrdInfoOffset = KswordARKWslNormalizeOffset(dynState.LxcoreOffsets.LxPicoThrdInfo);
    response->lxPicoThrdInfoTidOffset = KswordARKWslNormalizeOffset(dynState.LxcoreOffsets.LxPicoThrdInfoTID);

    if (dynState.Lxcore.present != 0UL) {
        response->fieldFlags |= KSWORD_ARK_WSL_FIELD_LXCORE_PRESENT;
    }
    if (dynState.LxcoreActive &&
        (dynState.CapabilityMask & KSW_CAP_WSL_LXCORE_FIELDS) != 0ULL) {
        response->fieldFlags |= KSWORD_ARK_WSL_FIELD_LXCORE_DYNDATA_ACTIVE;
    }

    requestFlags = (Request->flags == 0UL) ? KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_ALL : Request->flags;

    if ((requestFlags & KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_SILO) != 0UL) {
        response->siloRoutinesMask = KswordARKWslResolveSiloRoutineMask();
        response->siloStatus = (response->siloRoutinesMask != 0UL) ? STATUS_SUCCESS : STATUS_PROCEDURE_NOT_FOUND;
        if (response->siloRoutinesMask != 0UL) {
            response->fieldFlags |= KSWORD_ARK_WSL_FIELD_SILO_ROUTINES_PRESENT;
        }
    }

    if ((requestFlags & KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_PROCESS) != 0UL &&
        Request->processId != 0UL) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
        response->processLookupStatus = status;
        if (NT_SUCCESS(status)) {
            status = KswordARKWslQueryProcessSubsystem(processObject, &processSubsystem);
            response->processSubsystemStatus = status;
            if (NT_SUCCESS(status)) {
                response->processSubsystemType = processSubsystem;
                response->fieldFlags |= KSWORD_ARK_WSL_FIELD_PROCESS_SUBSYSTEM_PRESENT;
            }
        }
    }

    if ((requestFlags & KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_THREAD) != 0UL &&
        Request->threadId != 0UL) {
        status = PsLookupThreadByThreadId(ULongToHandle(Request->threadId), &threadObject);
        response->threadLookupStatus = status;
        if (NT_SUCCESS(status)) {
            status = KswordARKWslQueryThreadSubsystem(threadObject, &threadSubsystem);
            response->threadSubsystemStatus = status;
            if (NT_SUCCESS(status)) {
                response->threadSubsystemType = threadSubsystem;
                response->fieldFlags |= KSWORD_ARK_WSL_FIELD_THREAD_SUBSYSTEM_PRESENT;
            }

            if (PsGetThreadId(threadObject) == PsGetCurrentThreadId()) {
                response->fieldFlags |= KSWORD_ARK_WSL_FIELD_CURRENT_THREAD_CONTEXT;
                if ((response->fieldFlags & KSWORD_ARK_WSL_FIELD_LXCORE_DYNDATA_ACTIVE) != 0UL &&
                    threadSubsystem == KSWORD_ARK_WSL_SUBSYSTEM_WSL) {
                    ULONG linuxPid = 0UL;
                    ULONG linuxTid = 0UL;
                    status = KswordARKWslReadLinuxIdsFromCurrentThread(
                        &dynState.LxcoreOffsets,
                        &linuxPid,
                        &linuxTid);
                    response->linuxPidStatus = status;
                    response->linuxTidStatus = status;
                    if (NT_SUCCESS(status)) {
                        response->linuxProcessId = linuxPid;
                        response->linuxThreadId = linuxTid;
                        response->fieldFlags |=
                            KSWORD_ARK_WSL_FIELD_LINUX_PID_PRESENT |
                            KSWORD_ARK_WSL_FIELD_LINUX_TID_PRESENT;
                    }
                }
            }
            else {
                response->linuxPidStatus = STATUS_NOT_SUPPORTED;
                response->linuxTidStatus = STATUS_NOT_SUPPORTED;
            }
        }
    }

    if (threadObject != NULL) {
        ObDereferenceObject(threadObject);
    }
    if (processObject != NULL) {
        ObDereferenceObject(processObject);
    }

    if ((response->fieldFlags & KSWORD_ARK_WSL_FIELD_LXCORE_PRESENT) == 0UL) {
        response->queryStatus = KSWORD_ARK_WSL_QUERY_STATUS_WSL_NOT_LOADED;
    }
    else if ((response->fieldFlags & KSWORD_ARK_WSL_FIELD_LXCORE_DYNDATA_ACTIVE) == 0UL) {
        response->queryStatus = KSWORD_ARK_WSL_QUERY_STATUS_DYNDATA_MISSING;
    }
    else if ((response->fieldFlags & (KSWORD_ARK_WSL_FIELD_LINUX_PID_PRESENT | KSWORD_ARK_WSL_FIELD_LINUX_TID_PRESENT)) != 0UL) {
        response->queryStatus = KSWORD_ARK_WSL_QUERY_STATUS_OK;
    }
    else if ((Request->threadId != 0UL) &&
        (response->fieldFlags & KSWORD_ARK_WSL_FIELD_CURRENT_THREAD_CONTEXT) == 0UL) {
        response->queryStatus = KSWORD_ARK_WSL_QUERY_STATUS_NOT_CURRENT_THREAD;
    }
    else if (processSubsystem != KSWORD_ARK_WSL_SUBSYSTEM_WSL &&
        threadSubsystem != KSWORD_ARK_WSL_SUBSYSTEM_WSL) {
        response->queryStatus = KSWORD_ARK_WSL_QUERY_STATUS_NOT_WSL_SUBSYSTEM;
    }
    else {
        response->queryStatus = KSWORD_ARK_WSL_QUERY_STATUS_PARTIAL;
    }

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
