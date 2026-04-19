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

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE (0x0001)
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, KswordARKDriverQueueInitialize)
#endif

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
