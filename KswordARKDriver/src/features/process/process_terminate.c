/*++

Module Name:

    process_terminate.c

Abstract:

    Process termination pipeline for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
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
#define KSWORD_ARK_TERMINATE_WAIT_SLICE_MS 10UL
#define KSWORD_ARK_TERMINATE_WAIT_MAX_MS 500UL
#define KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS 800UL
#define KSWORD_ARK_TERMINATE_SCAN_CALL_MAX_BYTES 0x240UL
#define KSWORD_ARK_THREAD_SCAN_MAX_ID 0x00800000UL

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

static KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN
KswordARKDriverResolvePspTerminateThreadByPointer(
    VOID
    )
{
    static KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN cachedRoutine = NULL;
    static BOOLEAN hasResolved = FALSE;
    UNICODE_STRING routineName;
    PUCHAR ntTerminateThreadAddress = NULL;
    ULONG scanOffset = 0UL;

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

    // Dynamic locate fallback: scan NtTerminateThread for near-call target in ntoskrnl text.
    RtlInitUnicodeString(&routineName, L"NtTerminateThread");
    ntTerminateThreadAddress = (PUCHAR)MmGetSystemRoutineAddress(&routineName);
    if (ntTerminateThreadAddress == NULL) {
        return NULL;
    }

    for (scanOffset = 0UL; scanOffset + 5UL < KSWORD_ARK_TERMINATE_SCAN_CALL_MAX_BYTES; ++scanOffset) {
        LONG relativeOffset = 0;
        PUCHAR targetAddress = NULL;
        ULONG_PTR ntAddress = (ULONG_PTR)ntTerminateThreadAddress;
        ULONG_PTR targetValue = 0;

        if (ntTerminateThreadAddress[scanOffset] == 0xC3U) {
            break;
        }
        if (ntTerminateThreadAddress[scanOffset] != 0xE8U) {
            continue;
        }

        RtlCopyMemory(
            &relativeOffset,
            ntTerminateThreadAddress + scanOffset + 1UL,
            sizeof(relativeOffset));

        targetAddress = ntTerminateThreadAddress + scanOffset + 5UL + relativeOffset;
        targetValue = (ULONG_PTR)targetAddress;
        if (targetValue <= ntAddress) {
            continue;
        }
        if ((targetValue - ntAddress) > 0x200000UL) {
            continue;
        }
        if (!MmIsAddressValid(targetAddress)) {
            continue;
        }

        cachedRoutine = (KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)targetAddress;
        break;
    }

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

static NTSTATUS
KswordARKDriverOpenProcessHandleForTerminate(
    _In_ ULONG processId,
    _Out_ HANDLE* processHandleOut
    )
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (processHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *processHandleOut = NULL;

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        PROCESS_TERMINATE,
        &objectAttributes,
        &clientId);
    if (NT_SUCCESS(status)) {
        *processHandleOut = processHandle;
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ObOpenObjectByPointer(
        processObject,
        OBJ_KERNEL_HANDLE,
        NULL,
        PROCESS_TERMINATE,
        *PsProcessType,
        KernelMode,
        &processHandle);
    ObDereferenceObject(processObject);

    if (NT_SUCCESS(status)) {
        *processHandleOut = processHandle;
    }
    return status;
}

static BOOLEAN
KswordARKDriverIsProcessPresentByPid(
    _In_ ULONG processId
    )
{
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    ObDereferenceObject(processObject);
    return TRUE;
}

static NTSTATUS
KswordARKDriverWaitProcessExitByPid(
    _In_ ULONG processId,
    _In_ ULONG timeoutMs
    )
{
    ULONG elapsedMs = 0U;
    LARGE_INTEGER delayInterval;

    if (!KswordARKDriverIsProcessPresentByPid(processId)) {
        return STATUS_SUCCESS;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_PROCESS_IS_TERMINATING;
    }

    delayInterval.QuadPart = -((LONGLONG)KSWORD_ARK_TERMINATE_WAIT_SLICE_MS * 10 * 1000);
    while (elapsedMs < timeoutMs) {
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delayInterval);
        if (!KswordARKDriverIsProcessPresentByPid(processId)) {
            return STATUS_SUCCESS;
        }
        elapsedMs += KSWORD_ARK_TERMINATE_WAIT_SLICE_MS;
    }

    return STATUS_PROCESS_IS_TERMINATING;
}

static NTSTATUS
KswordARKDriverTerminateProcessThreadsByPointer(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    )
{
    KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN pspTerminateThreadByPointer = NULL;
    KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN zwOrNtTerminateThread = NULL;
    PEPROCESS processObject = NULL;
    PETHREAD threadCursor = NULL;
    ULONG threadVisitedCount = 0UL;
    ULONG threadTerminatedCount = 0UL;
    ULONG threadFailureCount = 0UL;
    BOOLEAN useCidThreadScan = FALSE;
    BOOLEAN hasTerminateRoutine = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS lastFailureStatus = STATUS_UNSUCCESSFUL;

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

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate fallback#2 lookup failed: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)status);
        return status;
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

    ObDereferenceObject(processObject);

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
KswordARKDriverTerminateProcessByPid(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    )
/*++

Routine Description:

    Open the target process by PID and terminate it via ZwTerminateProcess.

Arguments:

    processId - Target process ID.
    exitStatus - Exit status to report.

Return Value:

    NTSTATUS

--*/
{
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS waitStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS threadTerminateStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS memoryZeroStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS aggregateFailureStatus = STATUS_UNSUCCESSFUL;
    BOOLEAN processStillPresent = FALSE;

    if (processId == 0U || processId <= 4U) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate rejected: pid=%lu.",
            (unsigned long)processId);
        return STATUS_INVALID_PARAMETER;
    }

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate pipeline begin: pid=%lu, exit=0x%08X.",
        (unsigned long)processId,
        (unsigned int)exitStatus);

    status = KswordARKDriverOpenProcessHandleForTerminate(processId, &processHandle);
    if (!NT_SUCCESS(status)) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate open failed: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)status);
        return status;
    }

    status = ZwTerminateProcess(processHandle, exitStatus);
    ZwClose(processHandle);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(status) ? "Info" : "Warn",
        "R0 terminate stage#1 ZwTerminateProcess: pid=%lu, status=0x%08X.",
        (unsigned long)processId,
        (unsigned int)status);
    if (NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }
    KswordARKDriverMergeTerminateFailure(status, &aggregateFailureStatus);

    if (status == STATUS_PROCESS_IS_TERMINATING) {
        waitStatus = KswordARKDriverWaitProcessExitByPid(
            processId,
            KSWORD_ARK_TERMINATE_WAIT_MAX_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#1 wait: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus)) {
            return STATUS_SUCCESS;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = KswordARKDriverIsProcessPresentByPid(processId);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process already gone after stage#1: pid=%lu.",
            (unsigned long)processId);
        return STATUS_SUCCESS;
    }

    // Fallback method #2:
    // dynamic-resolve PspTerminateThreadByPointer and terminate every thread.
    threadTerminateStatus = KswordARKDriverTerminateProcessThreadsByPointer(
        device,
        processId,
        exitStatus);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(threadTerminateStatus) ? "Info" : "Warn",
        "R0 terminate stage#2 thread-sweep: pid=%lu, status=0x%08X.",
        (unsigned long)processId,
        (unsigned int)threadTerminateStatus);
    KswordARKDriverMergeTerminateFailure(threadTerminateStatus, &aggregateFailureStatus);

    if (NT_SUCCESS(threadTerminateStatus)) {
        waitStatus = KswordARKDriverWaitProcessExitByPid(
            processId,
            KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#2 wait: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus)) {
            return STATUS_SUCCESS;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = KswordARKDriverIsProcessPresentByPid(processId);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process gone after stage#2: pid=%lu.",
            (unsigned long)processId);
        return STATUS_SUCCESS;
    }

    // Fallback method #3:
    // zero writable user memory regions to force process unusable.
    memoryZeroStatus = KswordARKDriverZeroProcessUserMemoryByPid(device, processId);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(memoryZeroStatus) ? "Info" : "Warn",
        "R0 terminate stage#3 memory-zero: pid=%lu, status=0x%08X.",
        (unsigned long)processId,
        (unsigned int)memoryZeroStatus);
    KswordARKDriverMergeTerminateFailure(memoryZeroStatus, &aggregateFailureStatus);

    if (NT_SUCCESS(memoryZeroStatus)) {
        waitStatus = KswordARKDriverWaitProcessExitByPid(
            processId,
            KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#3 wait: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus) || !KswordARKDriverIsProcessPresentByPid(processId)) {
            return STATUS_SUCCESS;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = KswordARKDriverIsProcessPresentByPid(processId);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process gone after stage#3: pid=%lu.",
            (unsigned long)processId);
        return STATUS_SUCCESS;
    }

    if (aggregateFailureStatus == STATUS_UNSUCCESSFUL) {
        aggregateFailureStatus = status;
    }
    KswordARKDriverLogTerminateMessage(
        device,
        "Error",
        "R0 terminate pipeline failed: pid=%lu, final=0x%08X, stage1=0x%08X, stage2=0x%08X, stage3=0x%08X, wait=0x%08X.",
        (unsigned long)processId,
        (unsigned int)aggregateFailureStatus,
        (unsigned int)status,
        (unsigned int)threadTerminateStatus,
        (unsigned int)memoryZeroStatus,
        (unsigned int)waitStatus);
    return aggregateFailureStatus;
}

