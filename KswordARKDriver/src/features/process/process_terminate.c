/*++

Module Name:

    process_terminate.c

Abstract:

    Process termination pipeline for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "..\..\platform\process_resolver.h"
#include "process_crossview.h"
#include "process_extended.h"
#include <ntstrsafe.h>
#include <stdarg.h>

// 中文说明：本文件承载进程终止的多阶段后端，避免 process_actions.c 继续膨胀。
// 中文说明：公开入口保持 KswordARKDriverTerminateProcessByPid 不变，R3 协议不变。

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTSYSAPI
PETHREAD
NTAPI
PsGetNextProcessThread(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

NTSYSAPI
NTSTATUS
NTAPI
PsLookupThreadByThreadId(
    _In_ HANDLE ThreadId,
    _Outptr_ PETHREAD* Thread
    );

NTSYSAPI
PEPROCESS
NTAPI
PsGetThreadProcess(
    _In_ PETHREAD Thread
    );

extern NTKERNELAPI PEPROCESS PsInitialSystemProcess;

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

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE (0x0001)
#endif

#ifndef THREAD_TERMINATE
#define THREAD_TERMINATE (0x0001)
#endif

#ifndef STATUS_PROCESS_IS_TERMINATING
#define STATUS_PROCESS_IS_TERMINATING ((NTSTATUS)0xC000010AL)
#endif

#ifndef STATUS_THREAD_IS_TERMINATING
#define STATUS_THREAD_IS_TERMINATING ((NTSTATUS)0xC000004BL)
#endif

#define KSWORD_ARK_ENUM_PID_STEP 4UL
#define KSWORD_ARK_TERMINATE_WAIT_MAX_MS 500UL
#define KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS 800UL
#define KSWORD_ARK_TERMINATE_SCAN_CALL_MAX_BYTES 0x240UL
#define KSWORD_ARK_THREAD_SCAN_MAX_ID 0x00800000UL
#define KSWORD_ARK_TERMINATE_CID_WALK_MAX_NODES 0x00100000UL

#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#endif

#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif

typedef PETHREAD(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

typedef NTSTATUS(NTAPI* KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)(
    _In_ PETHREAD Thread,
    _In_ NTSTATUS ExitStatus,
    _In_ BOOLEAN SelfTerminate,
    _In_opt_ PVOID Reserved
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN)(
    _In_opt_ HANDLE ThreadHandle,
    _In_ NTSTATUS ExitStatus
    );

typedef enum _KSWORD_ARK_TERMINATE_PROCESS_RESOLVE_SOURCE
{
    KswordArkTerminateResolveUnknown = 0,
    KswordArkTerminateResolveDirectCid = 1,
    KswordArkTerminateResolveCidTableCid = 2,
    KswordArkTerminateResolveCidTableUniquePid = 3
} KSWORD_ARK_TERMINATE_PROCESS_RESOLVE_SOURCE;

typedef struct _KSWORD_ARK_TERMINATE_PROCESS_TARGET
{
    PEPROCESS ProcessObject;
    ULONG RequestedProcessId;
    ULONG CidProcessId;
    ULONG UniqueProcessId;
    KSWORD_ARK_TERMINATE_PROCESS_RESOLVE_SOURCE ResolveSource;
} KSWORD_ARK_TERMINATE_PROCESS_TARGET;

typedef struct _KSWORD_ARK_TERMINATE_CID_MATCH_CONTEXT
{
    const KSW_DYN_STATE* DynState;
    ULONG RequestedProcessId;
    PEPROCESS ProcessObject;
    ULONG CidProcessId;
    ULONG UniqueProcessId;
    KSWORD_ARK_TERMINATE_PROCESS_RESOLVE_SOURCE ResolveSource;
    NTSTATUS LastStatus;
} KSWORD_ARK_TERMINATE_CID_MATCH_CONTEXT;

static VOID
KswordARKDriverLogTerminateMessageV(
    _In_opt_ WDFDEVICE device,
    _In_z_ PCSTR levelText,
    _In_z_ PCSTR formatText,
    _In_ va_list arguments
    )
{
    CHAR messageBuffer[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };

    if (device == NULL || levelText == NULL || formatText == NULL) {
        return;
    }

    if (NT_SUCCESS(RtlStringCbVPrintfA(
        messageBuffer,
        sizeof(messageBuffer),
        formatText,
        arguments))) {
        (void)KswordARKDriverEnqueueLogFrame(device, levelText, messageBuffer);
    }
}

VOID
KswordARKDriverLogTerminateMessage(
    _In_opt_ WDFDEVICE device,
    _In_z_ PCSTR levelText,
    _In_z_ PCSTR formatText,
    ...
    )
{
    va_list arguments;

    va_start(arguments, formatText);
    KswordARKDriverLogTerminateMessageV(device, levelText, formatText, arguments);
    va_end(arguments);
}

static BOOLEAN
KswordARKDriverIsResolverMissingStatus(
    _In_ NTSTATUS status
    )
{
    return (status == STATUS_PROCEDURE_NOT_FOUND ||
        status == STATUS_NOT_IMPLEMENTED ||
        status == STATUS_NOT_FOUND) ? TRUE : FALSE;
}

static VOID
KswordARKDriverMergeTerminateFailure(
    _In_ NTSTATUS candidateStatus,
    _Inout_ NTSTATUS* aggregateStatus
    )
{
    if (aggregateStatus == NULL) {
        return;
    }
    if (NT_SUCCESS(candidateStatus)) {
        return;
    }
    if (candidateStatus == STATUS_PROCESS_IS_TERMINATING ||
        candidateStatus == STATUS_THREAD_IS_TERMINATING) {
        return;
    }

    if (NT_SUCCESS(*aggregateStatus) || *aggregateStatus == STATUS_UNSUCCESSFUL) {
        *aggregateStatus = candidateStatus;
        return;
    }

    if (KswordARKDriverIsResolverMissingStatus(*aggregateStatus) &&
        !KswordARKDriverIsResolverMissingStatus(candidateStatus)) {
        *aggregateStatus = candidateStatus;
        return;
    }
}

static KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKDriverResolvePsGetNextProcessThread(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsGetNextProcessThread");
    return (KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static PUCHAR
KswordARKDriverResolveKernelRoutineAddress(
    _In_z_ PCWSTR RoutineName
    )
/*++

Routine Description:

    Resolve one optional ntoskrnl routine by name.

Arguments:

    RoutineName - NUL-terminated routine name.

Return Value:

    Routine address on success; NULL when the routine is not exported.

--*/
{
    UNICODE_STRING routineName;

    if (RoutineName == NULL) {
        return NULL;
    }
    RtlInitUnicodeString(&routineName, RoutineName);
    return (PUCHAR)MmGetSystemRoutineAddress(&routineName);
}

static BOOLEAN
KswordARKDriverCallSiteHasR8TrueHint(
    _In_reads_bytes_(RoutineOffset) const UCHAR* RoutineAddress,
    _In_ ULONG RoutineOffset
    )
/*++

Routine Description:

    Check whether bytes before a CALL look like "SelfTerminate = TRUE" setup.
    中文说明：PsTerminateSystemThread 通常在调用 PspTerminateThreadByPointer
    前设置 r8/r8b 为 1；该弱特征只用于在多个 CALL 中优先选择候选目标。

Arguments:

    RoutineAddress - Base of the routine being scanned.
    RoutineOffset - Offset of the candidate CALL opcode.

Return Value:

    TRUE when a nearby mov r8{b,d},1 pattern is present; otherwise FALSE.

--*/
{
    ULONG windowStart = 0UL;
    ULONG index = 0UL;

    if (RoutineAddress == NULL || RoutineOffset == 0UL) {
        return FALSE;
    }

    windowStart = (RoutineOffset > 48UL) ? (RoutineOffset - 48UL) : 0UL;
    for (index = windowStart; index < RoutineOffset; ++index) {
        if (index + 3UL <= RoutineOffset &&
            RoutineAddress[index] == 0x41U &&
            RoutineAddress[index + 1UL] == 0xB0U &&
            RoutineAddress[index + 2UL] == 0x01U) {
            return TRUE;
        }
        if (index + 6UL <= RoutineOffset &&
            RoutineAddress[index] == 0x41U &&
            RoutineAddress[index + 1UL] == 0xB8U &&
            RoutineAddress[index + 2UL] == 0x01U &&
            RoutineAddress[index + 3UL] == 0x00U &&
            RoutineAddress[index + 4UL] == 0x00U &&
            RoutineAddress[index + 5UL] == 0x00U) {
            return TRUE;
        }
    }
    return FALSE;
}

static KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN
KswordARKDriverScanRoutineForTerminateThreadCall(
    _In_z_ PCWSTR RoutineName,
    _In_ BOOLEAN PreferSelfTerminateHint,
    _In_ BOOLEAN PreferLastCandidate
    )
/*++

Routine Description:

    Scan one exported routine for a near CALL target that is likely
    PspTerminateThreadByPointer. 中文说明：一些系统不导出 Nt/ZwTerminateThread；
    PsTerminateSystemThread 仍通常导出，且内部会调用同一个私有终止例程。

Arguments:

    RoutineName - Exported routine to scan.
    PreferSelfTerminateHint - TRUE to prefer CALL sites preceded by r8=1.
    PreferLastCandidate - TRUE to keep scanning and return the last plausible
        CALL when no stronger hint was found.

Return Value:

    Candidate private routine pointer or NULL.

--*/
{
    PUCHAR routineAddress = KswordARKDriverResolveKernelRoutineAddress(RoutineName);
    KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN selectedRoutine = NULL;
    ULONG scanOffset = 0UL;

    if (routineAddress == NULL) {
        return NULL;
    }

    for (scanOffset = 0UL; scanOffset + 5UL < KSWORD_ARK_TERMINATE_SCAN_CALL_MAX_BYTES; ++scanOffset) {
        LONG relativeOffset = 0;
        PUCHAR targetAddress = NULL;
        ULONG_PTR routineValue = (ULONG_PTR)routineAddress;
        ULONG_PTR targetValue = 0;
        ULONG_PTR delta = 0;

        if (routineAddress[scanOffset] == 0xC3U || routineAddress[scanOffset] == 0xCCU) {
            break;
        }
        if (routineAddress[scanOffset] != 0xE8U) {
            continue;
        }

        RtlCopyMemory(
            &relativeOffset,
            routineAddress + scanOffset + 1UL,
            sizeof(relativeOffset));

        targetAddress = routineAddress + scanOffset + 5UL + relativeOffset;
        targetValue = (ULONG_PTR)targetAddress;
        delta = (targetValue > routineValue)
            ? (targetValue - routineValue)
            : (routineValue - targetValue);
        if (delta > 0x400000UL) {
            continue;
        }
        if (!MmIsAddressValid(targetAddress)) {
            continue;
        }

        if (PreferSelfTerminateHint &&
            KswordARKDriverCallSiteHasR8TrueHint(routineAddress, scanOffset)) {
            return (KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)targetAddress;
        }

        selectedRoutine = (KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)targetAddress;
        if (!PreferLastCandidate) {
            return selectedRoutine;
        }
    }

    return selectedRoutine;
}

static KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN
KswordARKDriverResolvePspTerminateThreadByPointer(
    VOID
    )
{
    static KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN cachedRoutine = NULL;
    static BOOLEAN hasResolved = FALSE;
    UNICODE_STRING routineName;

    if (hasResolved) {
        return cachedRoutine;
    }
    hasResolved = TRUE;

    // Try direct lookup first (some private symbols can surface on test kernels).
    RtlInitUnicodeString(&routineName, L"PspTerminateThreadByPointer");
    cachedRoutine = (KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)MmGetSystemRoutineAddress(&routineName);
    if (cachedRoutine != NULL) {
        return cachedRoutine;
    }

    // Dynamic locate fallback #1: scan Nt/ZwTerminateThread when exported.
    cachedRoutine = KswordARKDriverScanRoutineForTerminateThreadCall(
        L"NtTerminateThread",
        FALSE,
        FALSE);
    if (cachedRoutine != NULL) {
        return cachedRoutine;
    }
    cachedRoutine = KswordARKDriverScanRoutineForTerminateThreadCall(
        L"ZwTerminateThread",
        FALSE,
        FALSE);
    if (cachedRoutine != NULL) {
        return cachedRoutine;
    }

    // Dynamic locate fallback #2: PsTerminateSystemThread is commonly exported.
    cachedRoutine = KswordARKDriverScanRoutineForTerminateThreadCall(
        L"PsTerminateSystemThread",
        TRUE,
        TRUE);

    return cachedRoutine;
}

static KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN
KswordARKDriverResolveZwOrNtTerminateThread(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwTerminateThread");
    {
        KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN routineAddress =
            (KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
        if (routineAddress != NULL) {
            return routineAddress;
        }
    }

    RtlInitUnicodeString(&routineName, L"NtTerminateThread");
    return (KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static PCSTR
KswordARKDriverTerminateResolveSourceText(
    _In_ KSWORD_ARK_TERMINATE_PROCESS_RESOLVE_SOURCE ResolveSource
    )
/*++

Routine Description:

    Convert the internal terminate-target resolver source into a compact log
    string. 中文说明：该文本只用于 R0 日志，IOCTL 协议仍保持 PID 输入语义。

Arguments:

    ResolveSource - Internal resolver source enum.

Return Value:

    Static NUL-terminated string.

--*/
{
    switch (ResolveSource) {
    case KswordArkTerminateResolveDirectCid:
        return "DirectCid";
    case KswordArkTerminateResolveCidTableCid:
        return "CidTableCid";
    case KswordArkTerminateResolveCidTableUniquePid:
        return "CidTableUniquePid";
    default:
        return "Unknown";
    }
}

static NTSTATUS
KswordARKDriverReadTerminateUniqueProcessId(
    _In_opt_ const KSW_DYN_STATE* DynState,
    _In_ PEPROCESS ProcessObject,
    _Out_ ULONG* UniqueProcessIdOut
    )
/*++

Routine Description:

    Read _EPROCESS.UniqueProcessId from a process object for termination target
    matching. 中文说明：隐藏场景中 R3 传入的 PID 可能正是被对方写入的假
    UniqueProcessId；本函数只读字段，不恢复链表或修改对象。

Arguments:

    DynState - Optional DynData snapshot that contains EpUniqueProcessId.
    ProcessObject - Referenced candidate EPROCESS.
    UniqueProcessIdOut - Receives the current UniqueProcessId value.

Return Value:

    STATUS_SUCCESS when the field was read; otherwise a validation/DynData/read
    status.

--*/
{
    PVOID uniqueProcessId = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || UniqueProcessIdOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *UniqueProcessIdOut = 0UL;

    if (DynState == NULL ||
        !DynState->Initialized ||
        !KswordARKCrossViewOffsetPresent(DynState->Kernel.EpUniqueProcessId)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = KswordARKCrossViewReadPointerField(
        ProcessObject,
        DynState->Kernel.EpUniqueProcessId,
        &uniqueProcessId);
    if (NT_SUCCESS(status)) {
        *UniqueProcessIdOut = HandleToULong(uniqueProcessId);
    }
    return status;
}

static VOID
KswordARKDriverTerminateCidMatchCallback(
    _In_ const KSW_CROSSVIEW_CID_ENTRY* Entry,
    _Inout_opt_ PVOID Context
    )
/*++

Routine Description:

    CID-table walker callback used by the terminate resolver. 中文说明：回调只
    接受 walker 已经按 PsProcessType 校验/引用过的进程对象；命中后额外
    增加一次引用，保证 walker 释放临时引用后终止链路仍持有对象。

Arguments:

    Entry - Read-only CID walker payload.
    Context - KSWORD_ARK_TERMINATE_CID_MATCH_CONTEXT.

Return Value:

    None. The walker continues scanning; later callbacks are ignored after the
    first match.

--*/
{
    KSWORD_ARK_TERMINATE_CID_MATCH_CONTEXT* matchContext =
        (KSWORD_ARK_TERMINATE_CID_MATCH_CONTEXT*)Context;
    PEPROCESS processObject = NULL;
    ULONG publicUniqueProcessId = 0UL;
    ULONG privateUniqueProcessId = 0UL;
    NTSTATUS privatePidStatus = STATUS_SUCCESS;
    BOOLEAN matched = FALSE;
    KSWORD_ARK_TERMINATE_PROCESS_RESOLVE_SOURCE resolveSource =
        KswordArkTerminateResolveUnknown;

    if (matchContext == NULL ||
        matchContext->ProcessObject != NULL ||
        Entry == NULL ||
        !Entry->Referenced ||
        Entry->Object == NULL) {
        return;
    }

    processObject = (PEPROCESS)Entry->Object;
    publicUniqueProcessId = HandleToULong(PsGetProcessId(processObject));
    privateUniqueProcessId = publicUniqueProcessId;
    privatePidStatus = KswordARKDriverReadTerminateUniqueProcessId(
        matchContext->DynState,
        processObject,
        &privateUniqueProcessId);
    if (!NT_SUCCESS(privatePidStatus) &&
        privatePidStatus != STATUS_PROCEDURE_NOT_FOUND &&
        NT_SUCCESS(matchContext->LastStatus)) {
        matchContext->LastStatus = privatePidStatus;
    }

    if (Entry->CidValue == matchContext->RequestedProcessId) {
        matched = TRUE;
        resolveSource = KswordArkTerminateResolveCidTableCid;
    }
    else if (publicUniqueProcessId == matchContext->RequestedProcessId ||
        privateUniqueProcessId == matchContext->RequestedProcessId) {
        matched = TRUE;
        resolveSource = KswordArkTerminateResolveCidTableUniquePid;
    }

    if (!matched) {
        return;
    }

    ObReferenceObject(processObject);
    matchContext->ProcessObject = processObject;
    matchContext->CidProcessId = Entry->CidValue;
    matchContext->UniqueProcessId = privateUniqueProcessId;
    matchContext->ResolveSource = resolveSource;
    matchContext->LastStatus = STATUS_SUCCESS;
}

static VOID
KswordARKDriverReleaseTerminateTarget(
    _Inout_ KSWORD_ARK_TERMINATE_PROCESS_TARGET* Target
    )
/*++

Routine Description:

    Release the process reference held by a terminate target descriptor.

Arguments:

    Target - Mutable target descriptor.

Return Value:

    None. The descriptor is zeroed after dereferencing.

--*/
{
    if (Target == NULL) {
        return;
    }
    if (Target->ProcessObject != NULL) {
        ObDereferenceObject(Target->ProcessObject);
    }
    RtlZeroMemory(Target, sizeof(*Target));
}

static NTSTATUS
KswordARKDriverResolveTerminateTargetByCidTable(
    _In_opt_ WDFDEVICE Device,
    _In_ ULONG ProcessId,
    _Out_ KSWORD_ARK_TERMINATE_PROCESS_TARGET* Target
    )
/*++

Routine Description:

    Resolve a termination target through PspCidTable when direct lookup by the
    caller-supplied PID fails. 中文说明：该路径覆盖“CID 表仍保留原 PID，但
    EPROCESS.UniqueProcessId 已被改成假 PID”的隐藏场景；它只读 CID 表并
    引用命中的 EPROCESS，不触碰 ActiveProcessLinks。

Arguments:

    Device - Optional device used for diagnostic logging.
    ProcessId - PID-like identity supplied through the existing IOCTL.
    Target - Receives a referenced process object and resolved identities.

Return Value:

    STATUS_SUCCESS when a process object was found; otherwise lookup/walk status.

--*/
{
    KSW_DYN_STATE dynState;
    KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_TERMINATE_CID_MATCH_CONTEXT matchContext;
    PVOID pspCidTableAddress = NULL;
    ULONG64 missingCapabilityMask = 0ULL;
    ULONG visitedEntries = 0UL;
    BOOLEAN usedDynDataGlobal = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (Target == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&dynState, sizeof(dynState));
    RtlZeroMemory(&fieldOffsets, sizeof(fieldOffsets));
    RtlZeroMemory(&matchContext, sizeof(matchContext));
    KswordARKDynDataSnapshot(&dynState);
    KswordARKCrossViewFillFieldOffsets(&dynState, &fieldOffsets);

    if (PsProcessType == NULL || *PsProcessType == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = KswordARKCrossViewResolvePspCidTableAddress(
        &dynState,
        &fieldOffsets,
        &pspCidTableAddress,
        &missingCapabilityMask,
        &usedDynDataGlobal);
    if (!NT_SUCCESS(status) || pspCidTableAddress == NULL) {
        KswordARKDriverLogTerminateMessage(
            Device,
            "Warn",
            "R0 terminate CID resolver unavailable: requestPid=%lu, status=0x%08X, missingCaps=0x%I64X.",
            (unsigned long)ProcessId,
            (unsigned int)status,
            missingCapabilityMask);
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

    matchContext.DynState = &dynState;
    matchContext.RequestedProcessId = ProcessId;
    matchContext.LastStatus = STATUS_NOT_FOUND;
    status = KswordARKCrossViewWalkCidTable(
        &dynState,
        pspCidTableAddress,
        *PsProcessType,
        KSWORD_ARK_TERMINATE_CID_WALK_MAX_NODES,
        KswordARKDriverTerminateCidMatchCallback,
        &matchContext,
        &visitedEntries);

    KswordARKDriverLogTerminateMessage(
        Device,
        matchContext.ProcessObject != NULL ? "Info" : "Warn",
        "R0 terminate CID resolver scan: requestPid=%lu, pspCid=%p, usedDyn=%u, visited=%lu, status=0x%08X, match=%p, cid=%lu, unique=%lu, source=%s.",
        (unsigned long)ProcessId,
        pspCidTableAddress,
        usedDynDataGlobal ? 1U : 0U,
        (unsigned long)visitedEntries,
        (unsigned int)status,
        matchContext.ProcessObject,
        (unsigned long)matchContext.CidProcessId,
        (unsigned long)matchContext.UniqueProcessId,
        KswordARKDriverTerminateResolveSourceText(matchContext.ResolveSource));

    if (matchContext.ProcessObject == NULL) {
        if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW) {
            return status;
        }
        return matchContext.LastStatus;
    }

    Target->ProcessObject = matchContext.ProcessObject;
    Target->RequestedProcessId = ProcessId;
    Target->CidProcessId = matchContext.CidProcessId;
    Target->UniqueProcessId = matchContext.UniqueProcessId;
    Target->ResolveSource = matchContext.ResolveSource;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDriverResolveTerminateTarget(
    _In_opt_ WDFDEVICE Device,
    _In_ ULONG ProcessId,
    _Out_ KSWORD_ARK_TERMINATE_PROCESS_TARGET* Target
    )
/*++

Routine Description:

    Resolve the process object used by the whole R0 termination pipeline.
    中文说明：优先按 CID/PID 直接 lookup；如果调用方传入的是被写入
    UniqueProcessId 的假 PID，则降级到 CID 表全表 walk 反查对象。

Arguments:

    Device - Optional device used for diagnostic logging.
    ProcessId - PID-like value from R3.
    Target - Receives the referenced process object.

Return Value:

    STATUS_SUCCESS when Target owns a reference; otherwise lookup status.

--*/
{
    PEPROCESS processObject = NULL;
    ULONG uniqueProcessId = 0UL;
    KSW_DYN_STATE dynState;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS cidTableStatus = STATUS_SUCCESS;
    NTSTATUS directLookupStatus = STATUS_SUCCESS;

    if (Target == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Target, sizeof(*Target));

    cidTableStatus = KswordARKDriverResolveTerminateTargetByCidTable(
        Device,
        ProcessId,
        Target);
    if (NT_SUCCESS(cidTableStatus)) {
        return STATUS_SUCCESS;
    }

    /*
     * 中文说明：
     * - 终止动作属于低频高危动作，优先完整走 PspCidTable 证据链；
     * - 只有 CID 表 resolver 缺 DynData/被系统限制/无法命中时，才回退到
     *   PsLookupProcessByProcessId；
     * - 这样 PsLookup 被 inline hook 时不会影响主路径，最多影响最后兜底。
     */
    KswordARKDriverLogTerminateMessage(
        Device,
        "Warn",
        "R0 terminate CID-first resolver failed: requestPid=%lu, status=0x%08X; falling back to PsLookup.",
        (unsigned long)ProcessId,
        (unsigned int)cidTableStatus);

    directLookupStatus = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (NT_SUCCESS(directLookupStatus)) {
        RtlZeroMemory(&dynState, sizeof(dynState));
        KswordARKDynDataSnapshot(&dynState);
        status = KswordARKDriverReadTerminateUniqueProcessId(
            &dynState,
            processObject,
            &uniqueProcessId);
        if (!NT_SUCCESS(status)) {
            uniqueProcessId = HandleToULong(PsGetProcessId(processObject));
        }

        Target->ProcessObject = processObject;
        Target->RequestedProcessId = ProcessId;
        Target->CidProcessId = ProcessId;
        Target->UniqueProcessId = uniqueProcessId;
        Target->ResolveSource = KswordArkTerminateResolveDirectCid;
        return STATUS_SUCCESS;
    }

    KswordARKDriverLogTerminateMessage(
        Device,
        "Warn",
        "R0 terminate direct CID lookup failed: requestPid=%lu, status=0x%08X; falling back to CID-table unique-pid scan.",
        (unsigned long)ProcessId,
        (unsigned int)directLookupStatus);

    status = directLookupStatus;
    return NT_SUCCESS(cidTableStatus) ? status : cidTableStatus;
}

static NTSTATUS
KswordARKDriverOpenProcessHandleForTerminate(
    _In_ PEPROCESS ProcessObject,
    _Out_ HANDLE* ProcessHandleOut
    )
/*++

Routine Description:

    Open a terminate handle from an already resolved EPROCESS object.
    中文说明：不再调用 ZwOpenProcess(CLIENT_ID)，避免 UniqueProcessId 被改写后
    句柄打开路径重新依赖不可信 PID。

Arguments:

    ProcessObject - Referenced target EPROCESS.
    ProcessHandleOut - Receives a kernel process handle.

Return Value:

    STATUS_SUCCESS or ObOpenObjectByPointer status.

--*/
{
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || ProcessHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ProcessHandleOut = NULL;

    status = ObOpenObjectByPointer(
        ProcessObject,
        OBJ_KERNEL_HANDLE,
        NULL,
        PROCESS_TERMINATE | SYNCHRONIZE,
        *PsProcessType,
        KernelMode,
        &processHandle);
    if (NT_SUCCESS(status)) {
        *ProcessHandleOut = processHandle;
    }
    return status;
}

static NTSTATUS
KswordARKDriverWaitProcessExitByObject(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG TimeoutMs
    )
/*++

Routine Description:

    Wait for a process object to become signaled instead of polling by PID.
    中文说明：隐藏进程的 PID 字段可能不可靠，等待对象本身能覆盖三种终止
    stage 的退出判断。

Arguments:

    ProcessObject - Referenced target EPROCESS.
    TimeoutMs - Maximum wait time in milliseconds; zero performs a poll.

Return Value:

    STATUS_SUCCESS when the process is signaled/exited; STATUS_PROCESS_IS_TERMINATING
    when it is still active after the timeout.

--*/
{
    LARGE_INTEGER timeoutInterval;
    NTSTATUS waitStatus = STATUS_SUCCESS;

    if (ProcessObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_PROCESS_IS_TERMINATING;
    }

    if (TimeoutMs == 0UL) {
        timeoutInterval.QuadPart = 0;
    }
    else {
        timeoutInterval.QuadPart = -((LONGLONG)TimeoutMs * 10 * 1000);
    }

    waitStatus = KeWaitForSingleObject(
        ProcessObject,
        Executive,
        KernelMode,
        FALSE,
        &timeoutInterval);
    if (waitStatus == STATUS_SUCCESS) {
        return STATUS_SUCCESS;
    }
    if (waitStatus == STATUS_TIMEOUT) {
        return STATUS_PROCESS_IS_TERMINATING;
    }
    return waitStatus;
}

static BOOLEAN
KswordARKDriverIsProcessTerminatedByObject(
    _In_ PEPROCESS ProcessObject
    )
/*++

Routine Description:

    Poll the target process object signal state.

Arguments:

    ProcessObject - Referenced target EPROCESS.

Return Value:

    TRUE when the process is already signaled; FALSE when still active or when
    the poll cannot be performed safely.

--*/
{
    return NT_SUCCESS(KswordARKDriverWaitProcessExitByObject(ProcessObject, 0UL)) ? TRUE : FALSE;
}

static NTSTATUS
KswordARKDriverReadProcessProtectionForTerminate(
    _In_ PEPROCESS ProcessObject,
    _Out_ UCHAR* ProtectionOut
    )
/*++

Routine Description:

    Read EPROCESS.Protection before termination so PP/PPL targets can be
    downgraded by object. 中文说明：读取失败不阻断终止链路，调用方会继续按
    最强终止方案执行。

Arguments:

    ProcessObject - Referenced target EPROCESS.
    ProtectionOut - Receives the raw protection byte.

Return Value:

    STATUS_SUCCESS or DynData/read status.

--*/
{
    KSW_DYN_STATE dynState;
    LONG runtimeProtectionOffset = -1;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || ProtectionOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ProtectionOut = 0U;

    /*
     * 优先使用当前内核 PsIsProtectedProcess* 反推出的 Protection 偏移。
     * 返回：运行时偏移可读时直接返回；失败时再尝试 DynData profile。
     */
    runtimeProtectionOffset = KswordARKDriverResolveProcessProtectionOffset();
    if (runtimeProtectionOffset > 0 && runtimeProtectionOffset <= 0x3000L) {
        __try {
            RtlCopyMemory(
                ProtectionOut,
                (PUCHAR)ProcessObject + (ULONG)runtimeProtectionOffset,
                sizeof(*ProtectionOut));
            return STATUS_SUCCESS;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
        }
    }

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);
    if ((dynState.CapabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) !=
        KSW_CAP_PROCESS_PROTECTION_PATCH ||
        !KswordARKCrossViewOffsetPresent(dynState.Kernel.EpProtection)) {
        return NT_SUCCESS(status) ? STATUS_PROCEDURE_NOT_FOUND : status;
    }

    __try {
        RtlCopyMemory(ProtectionOut, (PUCHAR)ProcessObject + dynState.Kernel.EpProtection, sizeof(*ProtectionOut));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    return status;
}

static NTSTATUS
KswordARKDriverClearProcessProtectionForTerminate(
    _In_opt_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_TERMINATE_PROCESS_TARGET* Target
    )
/*++

Routine Description:

    Clear PP/PPL protection bytes before running the termination stages.
    中文说明：如果目标带 Protection，则按已解析 EPROCESS 对象清零
    Protection/SignatureLevel/SectionSignatureLevel；失败只记录日志，不让
    缺失 DynData 直接中断后续强制终止。

Arguments:

    Device - Optional device used for diagnostic logging.
    Target - Resolved terminate target.

Return Value:

    STATUS_SUCCESS when no patch was needed or patch succeeded; otherwise the
    patch/read status for logging.

--*/
{
    UCHAR protection = 0U;
    UCHAR protectionAfter = 0U;
    NTSTATUS readStatus = STATUS_SUCCESS;
    NTSTATUS patchStatus = STATUS_SUCCESS;
    NTSTATUS verifyStatus = STATUS_SUCCESS;

    if (Target == NULL || Target->ProcessObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    readStatus = KswordARKDriverReadProcessProtectionForTerminate(
        Target->ProcessObject,
        &protection);
    if (NT_SUCCESS(readStatus) && protection == 0U) {
        KswordARKDriverLogTerminateMessage(
            Device,
            "Info",
            "R0 terminate protection precheck: requestPid=%lu, cid=%lu, unique=%lu, protection=0x00, patch skipped.",
            (unsigned long)Target->RequestedProcessId,
            (unsigned long)Target->CidProcessId,
            (unsigned long)Target->UniqueProcessId);
        return STATUS_SUCCESS;
    }

    patchStatus = KswordARKProcessPatchProtectionByDynDataObject(
        Target->ProcessObject,
        0U,
        0U,
        0U);
    verifyStatus = KswordARKDriverReadProcessProtectionForTerminate(
        Target->ProcessObject,
        &protectionAfter);
    KswordARKDriverLogTerminateMessage(
        Device,
        (NT_SUCCESS(patchStatus) && NT_SUCCESS(verifyStatus) && protectionAfter == 0U) ? "Info" : "Warn",
        "R0 terminate protection clear: requestPid=%lu, cid=%lu, unique=%lu, readStatus=0x%08X, oldProtection=0x%02X, patchStatus=0x%08X, verifyStatus=0x%08X, newProtection=0x%02X.",
        (unsigned long)Target->RequestedProcessId,
        (unsigned long)Target->CidProcessId,
        (unsigned long)Target->UniqueProcessId,
        (unsigned int)readStatus,
        (unsigned int)protection,
        (unsigned int)patchStatus,
        (unsigned int)verifyStatus,
        (unsigned int)protectionAfter);

    return patchStatus;
}

static NTSTATUS
KswordARKDriverTerminateProcessThreadsByPointer(
    _In_opt_ WDFDEVICE device,
    _In_ const KSWORD_ARK_TERMINATE_PROCESS_TARGET* Target,
    _In_ NTSTATUS exitStatus
    )
{
    KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN pspTerminateThreadByPointer = NULL;
    KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN zwOrNtTerminateThread = NULL;
    PEPROCESS processObject = NULL;
    ULONG processId = 0UL;
    PETHREAD threadCursor = NULL;
    ULONG threadVisitedCount = 0UL;
    ULONG threadTerminatedCount = 0UL;
    ULONG threadFailureCount = 0UL;
    BOOLEAN useCidThreadScan = FALSE;
    BOOLEAN hasTerminateRoutine = FALSE;
    NTSTATUS lastFailureStatus = STATUS_UNSUCCESSFUL;

    if (Target == NULL || Target->ProcessObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    processObject = Target->ProcessObject;
    processId = Target->RequestedProcessId;

    psGetNextProcessThread = KswordARKDriverResolvePsGetNextProcessThread();
    pspTerminateThreadByPointer = KswordARKDriverResolvePspTerminateThreadByPointer();
    if (pspTerminateThreadByPointer == NULL) {
        zwOrNtTerminateThread = KswordARKDriverResolveZwOrNtTerminateThread();
    }
    hasTerminateRoutine =
        (pspTerminateThreadByPointer != NULL || zwOrNtTerminateThread != NULL) ? TRUE : FALSE;

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate fallback#2 resolver: pid=%lu, PsGetNextProcessThread=%p, PspTerminateThreadByPointer=%p, ZwOrNtTerminateThread=%p.",
        (unsigned long)processId,
        psGetNextProcessThread,
        pspTerminateThreadByPointer,
        zwOrNtTerminateThread);

    if (!hasTerminateRoutine) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate fallback#2 unavailable: pid=%lu, reason=%s.",
            (unsigned long)processId,
            "thread terminate routine missing");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    useCidThreadScan = (psGetNextProcessThread == NULL) ? TRUE : FALSE;
    if (useCidThreadScan) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate fallback#2 switching to CID thread scan: pid=%lu, maxTid=0x%08X.",
            (unsigned long)processId,
            (unsigned int)KSWORD_ARK_THREAD_SCAN_MAX_ID);
    }

    if (!useCidThreadScan) {
        threadCursor = psGetNextProcessThread(processObject, NULL);
        while (threadCursor != NULL) {
            PETHREAD nextThread = psGetNextProcessThread(processObject, threadCursor);
            NTSTATUS terminateStatus = STATUS_UNSUCCESSFUL;
            threadVisitedCount += 1UL;

            if (pspTerminateThreadByPointer != NULL) {
                terminateStatus = pspTerminateThreadByPointer(
                    threadCursor,
                    exitStatus,
                    FALSE,
                    NULL);
            }
            else {
                HANDLE threadHandle = NULL;
                terminateStatus = ObOpenObjectByPointer(
                    threadCursor,
                    OBJ_KERNEL_HANDLE,
                    NULL,
                    THREAD_TERMINATE,
                    *PsThreadType,
                    KernelMode,
                    &threadHandle);
                if (NT_SUCCESS(terminateStatus)) {
                    if (zwOrNtTerminateThread != NULL) {
                        terminateStatus = zwOrNtTerminateThread(threadHandle, exitStatus);
                    }
                    else {
                        terminateStatus = STATUS_PROCEDURE_NOT_FOUND;
                    }
                    ZwClose(threadHandle);
                }
            }

            if (NT_SUCCESS(terminateStatus) ||
                terminateStatus == STATUS_THREAD_IS_TERMINATING ||
                terminateStatus == STATUS_PROCESS_IS_TERMINATING) {
                threadTerminatedCount += 1UL;
            }
            else {
                threadFailureCount += 1UL;
                lastFailureStatus = terminateStatus;
            }

            ObDereferenceObject(threadCursor);
            threadCursor = nextThread;
        }
    }
    else {
        ULONG scanThreadId = 4UL;
        for (;;) {
            PETHREAD threadObject = NULL;
            NTSTATUS lookupThreadStatus = PsLookupThreadByThreadId(ULongToHandle(scanThreadId), &threadObject);
            if (NT_SUCCESS(lookupThreadStatus)) {
                if (PsGetThreadProcess(threadObject) == processObject) {
                    NTSTATUS terminateStatus = STATUS_UNSUCCESSFUL;
                    threadVisitedCount += 1UL;

                    if (pspTerminateThreadByPointer != NULL) {
                        terminateStatus = pspTerminateThreadByPointer(
                            threadObject,
                            exitStatus,
                            FALSE,
                            NULL);
                    }
                    else {
                        HANDLE threadHandle = NULL;
                        terminateStatus = ObOpenObjectByPointer(
                            threadObject,
                            OBJ_KERNEL_HANDLE,
                            NULL,
                            THREAD_TERMINATE,
                            *PsThreadType,
                            KernelMode,
                            &threadHandle);
                        if (NT_SUCCESS(terminateStatus)) {
                            if (zwOrNtTerminateThread != NULL) {
                                terminateStatus = zwOrNtTerminateThread(threadHandle, exitStatus);
                            }
                            else {
                                terminateStatus = STATUS_PROCEDURE_NOT_FOUND;
                            }
                            ZwClose(threadHandle);
                        }
                    }

                    if (NT_SUCCESS(terminateStatus) ||
                        terminateStatus == STATUS_THREAD_IS_TERMINATING ||
                        terminateStatus == STATUS_PROCESS_IS_TERMINATING) {
                        threadTerminatedCount += 1UL;
                    }
                    else {
                        threadFailureCount += 1UL;
                        lastFailureStatus = terminateStatus;
                    }
                }
                ObDereferenceObject(threadObject);
            }

            if ((KSWORD_ARK_THREAD_SCAN_MAX_ID - scanThreadId) < KSWORD_ARK_ENUM_PID_STEP) {
                break;
            }
            scanThreadId += KSWORD_ARK_ENUM_PID_STEP;
        }
    }

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate fallback#2 result: pid=%lu, visited=%lu, terminated=%lu, failed=%lu, lastFailure=0x%08X.",
        (unsigned long)processId,
        (unsigned long)threadVisitedCount,
        (unsigned long)threadTerminatedCount,
        (unsigned long)threadFailureCount,
        (unsigned int)lastFailureStatus);

    if (threadTerminatedCount > 0UL) {
        return STATUS_SUCCESS;
    }
    if (lastFailureStatus != STATUS_UNSUCCESSFUL) {
        return lastFailureStatus;
    }
    if (threadVisitedCount == 0UL) {
        return STATUS_NOT_FOUND;
    }
    return lastFailureStatus;
}

NTSTATUS
KswordARKDriverZeroProcessUserMemoryByPid(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId
    );

NTSTATUS
KswordARKDriverZeroProcessUserMemoryByObject(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId,
    _In_ PEPROCESS processObject
    );

NTSTATUS
KswordARKDriverTerminateProcessByPid(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    )
/*++

Routine Description:

    Resolve the target process by PID/CID evidence once, then run the full R0
    termination pipeline against that EPROCESS object. 中文说明：R3/IOCTL 仍只传
    PID，但 R0 会在 PID 字段被改写时通过 CID 表反查真实对象，三种结束
    方案都复用同一个对象。

Arguments:

    processId - PID-like target identity supplied by user mode.
    exitStatus - Exit status to report.

Return Value:

    NTSTATUS

--*/
{
    KSWORD_ARK_TERMINATE_PROCESS_TARGET target;
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS waitStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS threadTerminateStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS memoryZeroStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS aggregateFailureStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS finalStatus = STATUS_UNSUCCESSFUL;
    BOOLEAN processStillPresent = FALSE;

    RtlZeroMemory(&target, sizeof(target));

    if (processId == 0U || processId <= 4U) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate rejected: pid=%lu.",
            (unsigned long)processId);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKDriverResolveTerminateTarget(device, processId, &target);
    if (!NT_SUCCESS(status)) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate target resolve failed: requestPid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)status);
        return status;
    }

    if (target.CidProcessId <= 4UL || target.ProcessObject == PsInitialSystemProcess) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate rejected after resolve: requestPid=%lu, cid=%lu, unique=%lu, process=%p.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId,
            (unsigned long)target.UniqueProcessId,
            target.ProcessObject);
        finalStatus = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    if (device != NULL && target.CidProcessId != target.RequestedProcessId) {
        KSWORD_ARK_SAFETY_CONTEXT resolvedSafetyContext;
        RtlZeroMemory(&resolvedSafetyContext, sizeof(resolvedSafetyContext));
        resolvedSafetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_TERMINATE;
        resolvedSafetyContext.TargetProcessId = target.CidProcessId;
        resolvedSafetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(device, &resolvedSafetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKDriverLogTerminateMessage(
                device,
                "Warn",
                "R0 terminate denied after CID resolve: requestPid=%lu, cid=%lu, status=0x%08X.",
                (unsigned long)target.RequestedProcessId,
                (unsigned long)target.CidProcessId,
                (unsigned int)status);
            finalStatus = status;
            goto Exit;
        }
    }

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate pipeline begin: requestPid=%lu, cid=%lu, unique=%lu, source=%s, process=%p, exit=0x%08X.",
        (unsigned long)target.RequestedProcessId,
        (unsigned long)target.CidProcessId,
        (unsigned long)target.UniqueProcessId,
        KswordARKDriverTerminateResolveSourceText(target.ResolveSource),
        target.ProcessObject,
        (unsigned int)exitStatus);

    if (KswordARKDriverIsProcessTerminatedByObject(target.ProcessObject)) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate target already signaled before stage#1: requestPid=%lu, cid=%lu.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId);
        finalStatus = STATUS_SUCCESS;
        goto Exit;
    }

    (void)KswordARKDriverClearProcessProtectionForTerminate(device, &target);

    status = KswordARKDriverOpenProcessHandleForTerminate(target.ProcessObject, &processHandle);
    if (NT_SUCCESS(status)) {
        status = ZwTerminateProcess(processHandle, exitStatus);
        ZwClose(processHandle);
        processHandle = NULL;
    }
    else {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate stage#1 open-by-object failed: requestPid=%lu, cid=%lu, status=0x%08X.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId,
            (unsigned int)status);
    }

    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(status) ? "Info" : "Warn",
        "R0 terminate stage#1 ZwTerminateProcess: requestPid=%lu, cid=%lu, status=0x%08X.",
        (unsigned long)target.RequestedProcessId,
        (unsigned long)target.CidProcessId,
        (unsigned int)status);
    if (NT_SUCCESS(status)) {
        finalStatus = STATUS_SUCCESS;
        goto Exit;
    }
    KswordARKDriverMergeTerminateFailure(status, &aggregateFailureStatus);

    if (status == STATUS_PROCESS_IS_TERMINATING) {
        waitStatus = KswordARKDriverWaitProcessExitByObject(
            target.ProcessObject,
            KSWORD_ARK_TERMINATE_WAIT_MAX_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#1 wait: requestPid=%lu, cid=%lu, status=0x%08X.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus)) {
            finalStatus = STATUS_SUCCESS;
            goto Exit;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = !KswordARKDriverIsProcessTerminatedByObject(target.ProcessObject);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process already gone after stage#1: requestPid=%lu, cid=%lu.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId);
        finalStatus = STATUS_SUCCESS;
        goto Exit;
    }

    // Fallback method #2:
    // dynamic-resolve PspTerminateThreadByPointer and terminate every thread.
    threadTerminateStatus = KswordARKDriverTerminateProcessThreadsByPointer(
        device,
        &target,
        exitStatus);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(threadTerminateStatus) ? "Info" : "Warn",
        "R0 terminate stage#2 thread-sweep: requestPid=%lu, cid=%lu, status=0x%08X.",
        (unsigned long)target.RequestedProcessId,
        (unsigned long)target.CidProcessId,
        (unsigned int)threadTerminateStatus);
    KswordARKDriverMergeTerminateFailure(threadTerminateStatus, &aggregateFailureStatus);

    if (NT_SUCCESS(threadTerminateStatus)) {
        waitStatus = KswordARKDriverWaitProcessExitByObject(
            target.ProcessObject,
            KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#2 wait: requestPid=%lu, cid=%lu, status=0x%08X.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus)) {
            finalStatus = STATUS_SUCCESS;
            goto Exit;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = !KswordARKDriverIsProcessTerminatedByObject(target.ProcessObject);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process gone after stage#2: requestPid=%lu, cid=%lu.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId);
        finalStatus = STATUS_SUCCESS;
        goto Exit;
    }

    // Fallback method #3:
    // zero writable user memory regions to force process unusable.
    memoryZeroStatus = KswordARKDriverZeroProcessUserMemoryByObject(
        device,
        target.RequestedProcessId,
        target.ProcessObject);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(memoryZeroStatus) ? "Info" : "Warn",
        "R0 terminate stage#3 memory-zero: requestPid=%lu, cid=%lu, status=0x%08X.",
        (unsigned long)target.RequestedProcessId,
        (unsigned long)target.CidProcessId,
        (unsigned int)memoryZeroStatus);
    KswordARKDriverMergeTerminateFailure(memoryZeroStatus, &aggregateFailureStatus);

    if (NT_SUCCESS(memoryZeroStatus)) {
        waitStatus = KswordARKDriverWaitProcessExitByObject(
            target.ProcessObject,
            KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#3 wait: requestPid=%lu, cid=%lu, status=0x%08X.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus) ||
            KswordARKDriverIsProcessTerminatedByObject(target.ProcessObject)) {
            finalStatus = STATUS_SUCCESS;
            goto Exit;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = !KswordARKDriverIsProcessTerminatedByObject(target.ProcessObject);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process gone after stage#3: requestPid=%lu, cid=%lu.",
            (unsigned long)target.RequestedProcessId,
            (unsigned long)target.CidProcessId);
        finalStatus = STATUS_SUCCESS;
        goto Exit;
    }

    if (aggregateFailureStatus == STATUS_UNSUCCESSFUL) {
        aggregateFailureStatus = status;
    }
    KswordARKDriverLogTerminateMessage(
        device,
        "Error",
        "R0 terminate pipeline failed: requestPid=%lu, cid=%lu, unique=%lu, source=%s, final=0x%08X, stage1=0x%08X, stage2=0x%08X, stage3=0x%08X, wait=0x%08X.",
        (unsigned long)target.RequestedProcessId,
        (unsigned long)target.CidProcessId,
        (unsigned long)target.UniqueProcessId,
        KswordARKDriverTerminateResolveSourceText(target.ResolveSource),
        (unsigned int)aggregateFailureStatus,
        (unsigned int)status,
        (unsigned int)threadTerminateStatus,
        (unsigned int)memoryZeroStatus,
        (unsigned int)waitStatus);
    finalStatus = aggregateFailureStatus;

Exit:
    if (processHandle != NULL) {
        ZwClose(processHandle);
        processHandle = NULL;
    }
    KswordARKDriverReleaseTerminateTarget(&target);
    return finalStatus;
}

