/*++

Module Name:

    handle_ioctl.c

Abstract:

    IOCTL handlers for KswordARK handle-table inspection operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE) - sizeof(KSWORD_ARK_HANDLE_ENTRY))

static VOID
KswordARKHandleIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format one handle IOCTL diagnostic line. 中文说明：日志失败不影响用户请求，
    这里只保留足够上下文用于排查 capability 和缓冲区问题。

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string.
    FormatText - printf-style ANSI template.
    ... - Template arguments.

Return Value:

    None.

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (VOID)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

NTSTATUS
KswordARKHandleIoctlEnumProcessHandles(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES. 中文说明：请求包必须提供 PID，
    输出包可截断，R3 依据 totalCount/returnedCount 判断是否需要更大缓冲。

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length.
    OutputBufferLength - Caller output length.
    BytesReturned - Receives response bytes written by the feature.

Return Value:

    NTSTATUS from validation or handle-table enumeration.

--*/
{
    KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST* enumRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 enum-handle ioctl: input buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    enumRequest = (KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST*)inputBuffer;
    if (enumRequest->processId == 0UL) {
        KswordARKHandleIoctlLog(Device, "Warn", "R0 enum-handle ioctl: missing pid.");
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 enum-handle ioctl: output buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateProcessHandles(outputBuffer, actualOutputLength, enumRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(
            Device,
            "Error",
            "R0 enum-handle failed: pid=%lu, status=0x%08X, outBytes=%Iu.",
            (unsigned long)enumRequest->processId,
            (unsigned int)status,
            *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE* responseHeader = (KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE*)outputBuffer;
        KswordARKHandleIoctlLog(
            Device,
            "Info",
            "R0 enum-handle success: pid=%lu, total=%lu, returned=%lu, status=%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->processId,
            (unsigned long)responseHeader->totalCount,
            (unsigned long)responseHeader->returnedCount,
            (unsigned long)responseHeader->overallStatus,
            *BytesReturned);
    }

    return status;
}

NTSTATUS
KswordARKHandleIoctlQueryHandleObject(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT. 中文说明：该路径只接收 PID 和
    handle value，不接收 object address，避免把展示地址变成操作凭据。

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length.
    OutputBufferLength - Caller output length.
    BytesReturned - Receives fixed response length.

Return Value:

    NTSTATUS from validation or object query helper.

--*/
{
    KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 query-handle-object ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST*)inputBuffer;
    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 query-handle-object ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryHandleObject(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 query-handle-object failed: pid=%lu, handle=0x%I64X, status=0x%08X.", (unsigned long)queryRequest->processId, queryRequest->handleValue, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE)) {
        KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* response = (KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE*)outputBuffer;
        KswordARKHandleIoctlLog(
            Device,
            "Info",
            "R0 query-handle-object success: pid=%lu, handle=0x%I64X, queryStatus=%lu, proxyStatus=%lu.",
            (unsigned long)response->processId,
            response->handleValue,
            (unsigned long)response->queryStatus,
            (unsigned long)response->proxyStatus);
    }

    return STATUS_SUCCESS;
}
