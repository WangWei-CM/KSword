/*++

Module Name:

    io_queue.c

Abstract:

    This file contains queue setup plus read/stop callbacks.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "io_queue.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, KswordARKDriverQueueInitialize)
#endif

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
