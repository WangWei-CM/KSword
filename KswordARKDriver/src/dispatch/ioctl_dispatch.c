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

#define KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_PROCESS_RESPONSE) - sizeof(KSWORD_ARK_PROCESS_ENTRY))
#define KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY))

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
    BOOLEAN completeRequest = TRUE;
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
            device,
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
        else if (status == STATUS_PROCESS_IS_TERMINATING) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 terminate pending: pid=%lu, status=0x%08X (still terminating).",
                (unsigned long)terminateRequest->processId,
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
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
    case IOCTL_KSWORD_ARK_ENUM_PROCESS:
    {
        KSWORD_ARK_ENUM_PROCESS_REQUEST* enumRequest = NULL;
        KSWORD_ARK_ENUM_PROCESS_REQUEST defaultRequest = { 0 };
        PVOID outputBuffer = NULL;
        size_t outputBufferLength = 0;

        if (InputBufferLength >= sizeof(KSWORD_ARK_ENUM_PROCESS_REQUEST)) {
            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(KSWORD_ARK_ENUM_PROCESS_REQUEST),
                &inputBuffer,
                &inputBufferLength);
            if (!NT_SUCCESS(status)) {
                (void)RtlStringCbPrintfA(
                    logMessage,
                    sizeof(logMessage),
                    "R0 enum-process ioctl: input buffer invalid, status=0x%08X.",
                    (unsigned int)status);
                (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
                break;
            }
            enumRequest = (KSWORD_ARK_ENUM_PROCESS_REQUEST*)inputBuffer;
        }
        else {
            enumRequest = &defaultRequest;
            enumRequest->flags = KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE;
            enumRequest->startPid = 0U;
            enumRequest->endPid = 0U;
            enumRequest->reserved = 0U;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request,
            KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE,
            &outputBuffer,
            &outputBufferLength);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-process ioctl: output buffer invalid, status=0x%08X.",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        status = KswordARKDriverEnumerateProcesses(
            outputBuffer,
            outputBufferLength,
            enumRequest,
            &completeBytes);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-process failed: status=0x%08X, outBytes=%Iu.",
                (unsigned int)status,
                completeBytes);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        if (completeBytes >= KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE) {
            KSWORD_ARK_ENUM_PROCESS_RESPONSE* responseHeader =
                (KSWORD_ARK_ENUM_PROCESS_RESPONSE*)outputBuffer;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-process success: total=%lu, returned=%lu, outBytes=%Iu.",
                (unsigned long)responseHeader->totalCount,
                (unsigned long)responseHeader->returnedCount,
                completeBytes);
            (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);
        }
        else {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-process success: outBytes=%Iu (header partial).",
                completeBytes);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
        }
        break;
    }
    case IOCTL_KSWORD_ARK_ENUM_SSDT:
    {
        KSWORD_ARK_ENUM_SSDT_REQUEST* enumRequest = NULL;
        KSWORD_ARK_ENUM_SSDT_REQUEST defaultRequest = { 0 };
        PVOID outputBuffer = NULL;
        size_t outputBufferLength = 0;

        if (InputBufferLength >= sizeof(KSWORD_ARK_ENUM_SSDT_REQUEST)) {
            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(KSWORD_ARK_ENUM_SSDT_REQUEST),
                &inputBuffer,
                &inputBufferLength);
            if (!NT_SUCCESS(status)) {
                (void)RtlStringCbPrintfA(
                    logMessage,
                    sizeof(logMessage),
                    "R0 enum-ssdt ioctl: input buffer invalid, status=0x%08X.",
                    (unsigned int)status);
                (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
                break;
            }
            enumRequest = (KSWORD_ARK_ENUM_SSDT_REQUEST*)inputBuffer;
        }
        else {
            enumRequest = &defaultRequest;
            enumRequest->flags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED;
            enumRequest->reserved = 0U;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request,
            KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE,
            &outputBuffer,
            &outputBufferLength);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-ssdt ioctl: output buffer invalid, status=0x%08X.",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        status = KswordARKDriverEnumerateSsdt(
            outputBuffer,
            outputBufferLength,
            enumRequest,
            &completeBytes);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-ssdt failed: status=0x%08X, outBytes=%Iu.",
                (unsigned int)status,
                completeBytes);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
            break;
        }

        if (completeBytes >= KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE) {
            KSWORD_ARK_ENUM_SSDT_RESPONSE* responseHeader =
                (KSWORD_ARK_ENUM_SSDT_RESPONSE*)outputBuffer;
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-ssdt success: total=%lu, returned=%lu, outBytes=%Iu.",
                (unsigned long)responseHeader->totalCount,
                (unsigned long)responseHeader->returnedCount,
                completeBytes);
            (void)KswordARKDriverEnqueueLogFrame(device, "Info", logMessage);
        }
        else {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "R0 enum-ssdt success: outBytes=%Iu (header partial).",
                completeBytes);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
        }
        break;
    }
    case IOCTL_KSWORD_ARK_SET_CALLBACK_RULES:
    {
        status = KswordARKCallbackIoctlSetRules(
            Request,
            InputBufferLength,
            &completeBytes);
        if (NT_SUCCESS(status)) {
            (void)KswordARKDriverEnqueueLogFrame(device, "Info", "Callback rules applied.");
        }
        else {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "Callback rules apply failed, status=0x%08X.",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Error", logMessage);
        }
        break;
    }
    case IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE:
    {
        status = KswordARKCallbackIoctlGetRuntimeState(
            Request,
            OutputBufferLength,
            &completeBytes);
        break;
    }
    case IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT:
    {
        status = KswordARKCallbackIoctlWaitEvent(
            Request,
            OutputBufferLength,
            &completeBytes);
        if (status == STATUS_PENDING) {
            completeRequest = FALSE;
        }
        break;
    }
    case IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT:
    {
        status = KswordARKCallbackIoctlAnswerEvent(
            Request,
            InputBufferLength,
            &completeBytes);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "Callback answer failed, status=0x%08X.",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
        }
        break;
    }
    case IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS:
    {
        status = KswordARKCallbackIoctlCancelAllPending(&completeBytes);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "Cancel-all pending decisions failed, status=0x%08X.",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
        }
        break;
    }
    case IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK:
    {
        status = KswordARKCallbackIoctlRemoveExternalCallback(
            Request,
            InputBufferLength,
            OutputBufferLength,
            &completeBytes);
        if (!NT_SUCCESS(status)) {
            (void)RtlStringCbPrintfA(
                logMessage,
                sizeof(logMessage),
                "Remove external callback failed, status=0x%08X.",
                (unsigned int)status);
            (void)KswordARKDriverEnqueueLogFrame(device, "Warn", logMessage);
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

    if (completeRequest) {
        WdfRequestCompleteWithInformation(Request, status, completeBytes);
    }
}
