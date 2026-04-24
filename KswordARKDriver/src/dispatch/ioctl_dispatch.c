/*++

Module Name:

    ioctl_dispatch.c

Abstract:

    This file contains IOCTL dispatch for the default queue.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "ark/ark_ioctl.h"
#include "ioctl_dispatch.tmh"

#include <ntstrsafe.h>

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
    case IOCTL_KSWORD_ARK_DELETE_PATH:
    {
        KSWORD_ARK_DELETE_PATH_REQUEST* deleteRequest = NULL;
        BOOLEAN isDirectory = FALSE;

        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(KSWORD_ARK_DELETE_PATH_REQUEST),
            &inputBuffer,
            &inputBufferLength);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 delete ioctl: input buffer invalid, status=0x%08X",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        deleteRequest = (KSWORD_ARK_DELETE_PATH_REQUEST*)inputBuffer;
        isDirectory =
            ((deleteRequest->flags & KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY) != 0UL)
            ? TRUE
            : FALSE;

        if ((deleteRequest->flags & (~KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY)) != 0UL) {
            status = STATUS_INVALID_PARAMETER;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 delete ioctl: flags rejected, flags=0x%08X.",
                (unsigned int)deleteRequest->flags);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
            break;
        }

        if (deleteRequest->pathLengthChars == 0U ||
            deleteRequest->pathLengthChars >= KSWORD_ARK_DELETE_PATH_MAX_CHARS) {
            status = STATUS_INVALID_PARAMETER;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 delete ioctl: path length rejected, chars=%u.",
                (unsigned int)deleteRequest->pathLengthChars);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
            break;
        }

        if (deleteRequest->path[deleteRequest->pathLengthChars] != L'\0') {
            status = STATUS_INVALID_PARAMETER;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 delete ioctl: path not null-terminated, chars=%u.",
                (unsigned int)deleteRequest->pathLengthChars);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
            break;
        }

        (void)RtlStringCbPrintfA(
            logMessage,
            sizeof(logMessage),
            "R0 delete ioctl: chars=%u, directory=%u.",
            (unsigned int)deleteRequest->pathLengthChars,
            (unsigned int)isDirectory);
        (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);

        status = KswordARKDriverDeletePath(
            deleteRequest->path,
            deleteRequest->pathLengthChars,
            isDirectory);
        if (NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 delete success: chars=%u, directory=%u.",
                (unsigned int)deleteRequest->pathLengthChars,
                (unsigned int)isDirectory);
            (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);
            completeBytes = sizeof(KSWORD_ARK_DELETE_PATH_REQUEST);
        }
        else {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 delete failed: chars=%u, directory=%u, status=0x%08X.",
                (unsigned int)deleteRequest->pathLengthChars,
                (unsigned int)isDirectory,
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
        }
        break;
    }
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
