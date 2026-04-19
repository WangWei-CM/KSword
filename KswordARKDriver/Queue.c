/*++

Module Name:

    queue.c

Abstract:

    This file contains the queue entry points and callbacks.

Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "queue.tmh"

#include <ntstrsafe.h>

#include "../shared/driver/KswordArkProcessIoctl.h"

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE (0x0001)
#endif

#ifndef PROCESS_SUSPEND_RESUME
#define PROCESS_SUSPEND_RESUME (0x0800)
#endif

#ifndef PROCESS_SET_INFORMATION
#define PROCESS_SET_INFORMATION (0x0200)
#endif

#define KSWORD_ARK_PROCESS_INFO_CLASS_PROTECTION 61UL

typedef NTSTATUS(NTAPI* KSWORD_PS_SUSPEND_PROCESS_FN)(
    _In_ PEPROCESS Process
    );

typedef UCHAR(NTAPI* KSWORD_PS_GET_PROCESS_PROTECTION_FN)(
    _In_ PEPROCESS Process
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)(
    _In_ HANDLE ProcessHandle
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_SET_INFORMATION_PROCESS_FN)(
    _In_ HANDLE ProcessHandle,
    _In_ ULONG ProcessInformationClass,
    _In_reads_bytes_(ProcessInformationLength) PVOID ProcessInformation,
    _In_ ULONG ProcessInformationLength
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, KswordARKDriverQueueInitialize)
#endif

// KswordARKDriverResolvePsSuspendProcess:
// - Resolve PsSuspendProcess first; this export is available on more systems.
static KSWORD_PS_SUSPEND_PROCESS_FN
KswordARKDriverResolvePsSuspendProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsSuspendProcess");
    return (KSWORD_PS_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

// KswordARKDriverResolveZwOrNtSuspendProcess:
// - Fallback resolver for Zw/Nt suspend APIs that use process handle input.
static KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN
KswordARKDriverResolveZwOrNtSuspendProcess(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwSuspendProcess");
    {
        KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN routineAddress =
            (KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
        if (routineAddress != NULL) {
            return routineAddress;
        }
    }

    RtlInitUnicodeString(&routineName, L"NtSuspendProcess");
    return (KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

// KswordARKDriverResolvePsGetProcessProtection:
// - Resolve PsGetProcessProtection for runtime offset discovery.
static KSWORD_PS_GET_PROCESS_PROTECTION_FN
KswordARKDriverResolvePsGetProcessProtection(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsGetProcessProtection");
    return (KSWORD_PS_GET_PROCESS_PROTECTION_FN)MmGetSystemRoutineAddress(&routineName);
}

// KswordARKDriverResolveProcessProtectionOffset:
// - Parse PsGetProcessProtection machine code to find EPROCESS protection-byte offset.
static LONG
KswordARKDriverResolveProcessProtectionOffset(
    VOID
    )
{
    KSWORD_PS_GET_PROCESS_PROTECTION_FN psGetProcessProtection = NULL;
    const UCHAR* routineCode = NULL;
    ULONG scanIndex = 0U;

    psGetProcessProtection = KswordARKDriverResolvePsGetProcessProtection();
    if (psGetProcessProtection == NULL) {
        return -1;
    }

    routineCode = (const UCHAR*)psGetProcessProtection;
    for (scanIndex = 0U; scanIndex + 8U < 64U; ++scanIndex) {
        LONG offsetValue = -1;

        // Pattern A: 0F B6 81 xx xx xx xx  => movzx eax, byte ptr [rcx+imm32]
        if (routineCode[scanIndex] == 0x0F &&
            routineCode[scanIndex + 1U] == 0xB6 &&
            routineCode[scanIndex + 2U] == 0x81) {
            RtlCopyMemory(&offsetValue, routineCode + scanIndex + 3U, sizeof(offsetValue));
            if (offsetValue > 0 && offsetValue < 0x4000) {
                return offsetValue;
            }
        }

        // Pattern B: 8A 81 xx xx xx xx => mov al, byte ptr [rcx+imm32]
        if (routineCode[scanIndex] == 0x8A &&
            routineCode[scanIndex + 1U] == 0x81) {
            RtlCopyMemory(&offsetValue, routineCode + scanIndex + 2U, sizeof(offsetValue));
            if (offsetValue > 0 && offsetValue < 0x4000) {
                return offsetValue;
            }
        }

        // Pattern C: 0F B6 41 xx => movzx eax, byte ptr [rcx+imm8]
        if (routineCode[scanIndex] == 0x0F &&
            routineCode[scanIndex + 1U] == 0xB6 &&
            routineCode[scanIndex + 2U] == 0x41) {
            const CHAR offset8 = (CHAR)routineCode[scanIndex + 3U];
            if (offset8 > 0) {
                return (LONG)offset8;
            }
        }

        // Pattern D: 8A 41 xx => mov al, byte ptr [rcx+imm8]
        if (routineCode[scanIndex] == 0x8A &&
            routineCode[scanIndex + 1U] == 0x41) {
            const CHAR offset8 = (CHAR)routineCode[scanIndex + 2U];
            if (offset8 > 0) {
                return (LONG)offset8;
            }
        }
    }

    return -1;
}

// KswordARKDriverResolveZwSetInformationProcess:
// - Resolve ZwSetInformationProcess dynamically for broad WDK compatibility.
static KSWORD_ZW_SET_INFORMATION_PROCESS_FN
KswordARKDriverResolveZwSetInformationProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"ZwSetInformationProcess");
    return (KSWORD_ZW_SET_INFORMATION_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

// KswordARKDriverPatchProcessProtectionLevelByPid:
// - Fallback path: patch EPROCESS protection byte directly.
static NTSTATUS
KswordARKDriverPatchProcessProtectionLevelByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    )
{
    const LONG protectionOffset = KswordARKDriverResolveProcessProtectionOffset();
    KSWORD_PS_GET_PROCESS_PROTECTION_FN psGetProcessProtection = NULL;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (protectionOffset <= 0) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    psGetProcessProtection = KswordARKDriverResolvePsGetProcessProtection();

    __try {
        PUCHAR processBase = (PUCHAR)processObject;
        volatile UCHAR* protectionByte = (volatile UCHAR*)(processBase + (ULONG)protectionOffset);
        *protectionByte = protectionLevel;

        if (psGetProcessProtection != NULL) {
            const UCHAR appliedLevel = psGetProcessProtection(processObject);
            status = (appliedLevel == protectionLevel) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        }
        else {
            status = STATUS_SUCCESS;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ObDereferenceObject(processObject);
    return status;
}

// KswordARKDriverTerminateProcessByPid:
// - Open the target process by PID and terminate it via ZwTerminateProcess.
static NTSTATUS
KswordARKDriverTerminateProcessByPid(
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    )
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        PROCESS_TERMINATE,
        &objectAttributes,
        &clientId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwTerminateProcess(processHandle, exitStatus);
    ZwClose(processHandle);
    return status;
}

// KswordARKDriverSuspendProcessByPid:
// - Suspend target process by PID (PsSuspendProcess preferred, Zw/Nt fallback).
static NTSTATUS
KswordARKDriverSuspendProcessByPid(
    _In_ ULONG processId
    )
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    KSWORD_PS_SUSPEND_PROCESS_FN psSuspendProcess = NULL;
    KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN zwOrNtSuspendProcess = NULL;

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    // Prefer PsSuspendProcess with PEPROCESS input for wider compatibility.
    psSuspendProcess = KswordARKDriverResolvePsSuspendProcess();
    if (psSuspendProcess != NULL) {
        status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        status = psSuspendProcess(processObject);
        ObDereferenceObject(processObject);
        return status;
    }

    // Fallback to Zw/NtSuspendProcess with process-handle input.
    zwOrNtSuspendProcess = KswordARKDriverResolveZwOrNtSuspendProcess();
    if (zwOrNtSuspendProcess == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        PROCESS_SUSPEND_RESUME,
        &objectAttributes,
        &clientId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = zwOrNtSuspendProcess(processHandle);
    ZwClose(processHandle);
    return status;
}

// KswordARKDriverSetProcessPplLevelByPid:
// - Set target process protection level by PID (ZwSetInformation preferred, patch fallback).
static NTSTATUS
KswordARKDriverSetProcessPplLevelByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    )
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR protectionLevelByte = protectionLevel;
    KSWORD_ZW_SET_INFORMATION_PROCESS_FN zwSetInformationProcess = NULL;
    const UCHAR protectionType = (UCHAR)(protectionLevel & 0x07U);

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    // Base validation:
    // - 0x00 disables protection;
    // - non-zero requires Type==1 (PPL).
    if (protectionLevel != 0U && protectionType != 0x01U) {
        return STATUS_INVALID_PARAMETER;
    }

    // Preferred path: ZwSetInformationProcess(ProcessProtectionInformation).
    zwSetInformationProcess = KswordARKDriverResolveZwSetInformationProcess();
    if (zwSetInformationProcess != NULL) {
        InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        clientId.UniqueProcess = ULongToHandle(processId);
        clientId.UniqueThread = NULL;
        status = ZwOpenProcess(
            &processHandle,
            PROCESS_SET_INFORMATION,
            &objectAttributes,
            &clientId);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        status = zwSetInformationProcess(
            processHandle,
            KSWORD_ARK_PROCESS_INFO_CLASS_PROTECTION,
            &protectionLevelByte,
            (ULONG)sizeof(protectionLevelByte));
        ZwClose(processHandle);

        // Succeeded on the preferred path.
        if (NT_SUCCESS(status)) {
            return status;
        }

        // If this info class is rejected by OS, continue to fallback patch path.
        if (status != STATUS_INVALID_PARAMETER &&
            status != STATUS_INVALID_INFO_CLASS &&
            status != STATUS_NOT_IMPLEMENTED &&
            status != STATUS_NOT_SUPPORTED) {
            return status;
        }
    }

    // Fallback path: patch EPROCESS protection byte directly.
    return KswordARKDriverPatchProcessProtectionLevelByPid(processId, protectionLevelByte);
}

NTSTATUS
KswordARKDriverQueueInitialize(
    _In_ WDFDEVICE Device
    )
/*++

Routine Description:

     Configure the default queue and callbacks.

Arguments:

    Device - Handle to a framework device object.

Return Value:

    NTSTATUS

--*/
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;

    PAGED_CODE();

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchParallel
        );

    queueConfig.EvtIoDeviceControl = KswordARKDriverEvtIoDeviceControl;
    queueConfig.EvtIoRead = KswordARKDriverEvtIoRead;
    queueConfig.EvtIoStop = KswordARKDriverEvtIoStop;

    status = WdfIoQueueCreate(
        Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
        );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "WdfIoQueueCreate failed %!STATUS!", status);
        return status;
    }

    return STATUS_SUCCESS;
}

VOID
KswordARKDriverEvtIoRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length
    )
/*++

Routine Description:

    This event is invoked when the framework receives IRP_MJ_READ request.

Arguments:

    Queue - Handle to the queue object.
    Request - Handle to a framework request object.
    Length - Requested output length in bytes.

Return Value:

    VOID

--*/
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PVOID outputBuffer = NULL;
    size_t outputBufferLength = 0;
    size_t bytesWritten = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Length);

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        1,
        &outputBuffer,
        &outputBufferLength);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "WdfRequestRetrieveOutputBuffer failed %!STATUS!", status);
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    status = KswordARKDriverReadNextLogLine(
        device,
        outputBuffer,
        outputBufferLength,
        &bytesWritten);
    WdfRequestCompleteWithInformation(Request, status, bytesWritten);
}

VOID
KswordARKDriverEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
/*++

Routine Description:

    This event is invoked when the framework receives IRP_MJ_DEVICE_CONTROL request.

Arguments:

    Queue - Handle to the queue object.
    Request - Handle to a framework request object.
    OutputBufferLength - Size of output buffer in bytes.
    InputBufferLength - Size of input buffer in bytes.
    IoControlCode - I/O control code.

Return Value:

    VOID

--*/
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PVOID inputBuffer = NULL;
    size_t inputBufferLength = 0;
    NTSTATUS status = STATUS_SUCCESS;
    size_t completeBytes = 0;
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };

    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_QUEUE,
        "%!FUNC! Queue 0x%p, Request 0x%p OutputBufferLength %d InputBufferLength %d IoControlCode %d",
        Queue,
        Request,
        (int)OutputBufferLength,
        (int)InputBufferLength,
        IoControlCode);

    switch (IoControlCode) {
    case IOCTL_KSWORD_ARK_TERMINATE_PROCESS:
    {
        KSWORD_ARK_TERMINATE_PROCESS_REQUEST* terminateRequest = NULL;

        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(KSWORD_ARK_TERMINATE_PROCESS_REQUEST),
            &inputBuffer,
            &inputBufferLength);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 terminate ioctl: input buffer invalid, status=0x%08X",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        terminateRequest = (KSWORD_ARK_TERMINATE_PROCESS_REQUEST*)inputBuffer;
        if (terminateRequest->processId == 0U || terminateRequest->processId <= 4U) {
            status = STATUS_INVALID_PARAMETER;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 terminate ioctl: pid=%lu rejected.",
                (unsigned long)terminateRequest->processId);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
            break;
        }

        (void)RtlStringCbPrintfA(
            logMessage,
            sizeof(logMessage),
            "R0 terminate ioctl: pid=%lu, exit=0x%08X.",
            (unsigned long)terminateRequest->processId,
            (unsigned int)terminateRequest->exitStatus);
        (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);

        status = KswordARKDriverTerminateProcessByPid(
            (ULONG)terminateRequest->processId,
            (NTSTATUS)terminateRequest->exitStatus);
        if (NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 terminate success: pid=%lu.",
                (unsigned long)terminateRequest->processId);
            (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);
            completeBytes = sizeof(KSWORD_ARK_TERMINATE_PROCESS_REQUEST);
        }
        else {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 terminate failed: pid=%lu, status=0x%08X.",
                (unsigned long)terminateRequest->processId,
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
        }
        break;
    }
    case IOCTL_KSWORD_ARK_SUSPEND_PROCESS:
    {
        KSWORD_ARK_SUSPEND_PROCESS_REQUEST* suspendRequest = NULL;

        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(KSWORD_ARK_SUSPEND_PROCESS_REQUEST),
            &inputBuffer,
            &inputBufferLength);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 suspend ioctl: input buffer invalid, status=0x%08X",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        suspendRequest = (KSWORD_ARK_SUSPEND_PROCESS_REQUEST*)inputBuffer;
        if (suspendRequest->processId == 0U || suspendRequest->processId <= 4U) {
            status = STATUS_INVALID_PARAMETER;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 suspend ioctl: pid=%lu rejected.",
                (unsigned long)suspendRequest->processId);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
            break;
        }

        (void)RtlStringCbPrintfA(
            logMessage,
            sizeof(logMessage),
            "R0 suspend ioctl: pid=%lu.",
            (unsigned long)suspendRequest->processId);
        (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);

        status = KswordARKDriverSuspendProcessByPid((ULONG)suspendRequest->processId);
        if (NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 suspend success: pid=%lu.",
                (unsigned long)suspendRequest->processId);
            (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);
            completeBytes = sizeof(KSWORD_ARK_SUSPEND_PROCESS_REQUEST);
        }
        else {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 suspend failed: pid=%lu, status=0x%08X.",
                (unsigned long)suspendRequest->processId,
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
        }
        break;
    }
    case IOCTL_KSWORD_ARK_SET_PPL_LEVEL:
    {
        KSWORD_ARK_SET_PPL_LEVEL_REQUEST* setPplRequest = NULL;

        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(KSWORD_ARK_SET_PPL_LEVEL_REQUEST),
            &inputBuffer,
            &inputBufferLength);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 set PPL ioctl: input buffer invalid, status=0x%08X",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        setPplRequest = (KSWORD_ARK_SET_PPL_LEVEL_REQUEST*)inputBuffer;
        if (setPplRequest->processId == 0U || setPplRequest->processId <= 4U) {
            status = STATUS_INVALID_PARAMETER;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 set PPL ioctl: pid=%lu rejected.",
                (unsigned long)setPplRequest->processId);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
            break;
        }

        (void)RtlStringCbPrintfA(
            logMessage,
            sizeof(logMessage),
            "R0 set PPL ioctl: pid=%lu, level=0x%02X.",
            (unsigned long)setPplRequest->processId,
            (unsigned int)setPplRequest->protectionLevel);
        (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);

        status = KswordARKDriverSetProcessPplLevelByPid(
            (ULONG)setPplRequest->processId,
            (UCHAR)setPplRequest->protectionLevel);
        if (NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 set PPL success: pid=%lu, level=0x%02X.",
                (unsigned long)setPplRequest->processId,
                (unsigned int)setPplRequest->protectionLevel);
            (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);
            completeBytes = sizeof(KSWORD_ARK_SET_PPL_LEVEL_REQUEST);
        }
        else {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 set PPL failed: pid=%lu, level=0x%02X, status=0x%08X.",
                (unsigned long)setPplRequest->processId,
                (unsigned int)setPplRequest->protectionLevel,
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
        }
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        (void)RtlStringCbPrintfA(
            logMessage,
            sizeof(logMessage),
            "Unsupported ioctl=0x%08X.",
            (unsigned int)IoControlCode);
        (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, completeBytes);
}

VOID
KswordARKDriverEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
/*++

Routine Description:

    This event is invoked for a power-managed queue before the device leaves D0.

Arguments:

    Queue - Handle to the queue object.
    Request - Handle to a framework request object.
    ActionFlags - Bitwise OR of one or more WDF_REQUEST_STOP_ACTION_FLAGS values.

Return Value:

    VOID

--*/
{
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_QUEUE,
        "%!FUNC! Queue 0x%p, Request 0x%p ActionFlags %d",
        Queue,
        Request,
        ActionFlags);

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ActionFlags);
}
