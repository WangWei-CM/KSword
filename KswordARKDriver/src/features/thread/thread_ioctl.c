/*++

Module Name:

    thread_ioctl.c

Abstract:

    IOCTL handlers for KswordARK thread inspection operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSWORD_ARK_THREAD_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_THREAD_RESPONSE) - sizeof(KSWORD_ARK_THREAD_ENTRY))

static VOID
KswordARKThreadIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one thread-handler diagnostic message. 这里集中处理
    日志格式化，避免枚举 handler 内部夹杂重复的 RtlStringCbVPrintfA 调用。

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string.
    FormatText - printf-style ANSI message template.
    ... - Template arguments.

Return Value:

    None. 日志失败不影响 IOCTL 主路径。

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
KswordARKThreadIoctlEnumThread(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ENUM_THREAD. 输入包是可选的；缺省时枚举全部线程
    并请求全部 Phase-3 扩展字段。输出包允许部分填充，R3 通过 totalCount 判断
    是否需要更大缓冲区。

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Supplied input bytes; shorter input selects defaults.
    OutputBufferLength - Supplied output bytes; checked by WDF output retrieval.
    BytesReturned - Receives the feature-written response byte count.

Return Value:

    NTSTATUS from buffer retrieval or KswordARKDriverEnumerateThreads.

--*/
{
    KSWORD_ARK_ENUM_THREAD_REQUEST* enumRequest = NULL;
    KSWORD_ARK_ENUM_THREAD_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_ENUM_THREAD_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKThreadIoctlLog(Device, "Error", "R0 enum-thread ioctl: input buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (hasInput) {
        enumRequest = (KSWORD_ARK_ENUM_THREAD_REQUEST*)inputBuffer;
    }
    else {
        enumRequest = &defaultRequest;
        enumRequest->flags = KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_ALL;
        enumRequest->processId = 0UL;
        enumRequest->reserved0 = 0UL;
        enumRequest->reserved1 = 0UL;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_THREAD_ENUM_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKThreadIoctlLog(Device, "Error", "R0 enum-thread ioctl: output buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateThreads(outputBuffer, actualOutputLength, enumRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKThreadIoctlLog(Device, "Error", "R0 enum-thread failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_THREAD_ENUM_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_THREAD_RESPONSE* responseHeader = (KSWORD_ARK_ENUM_THREAD_RESPONSE*)outputBuffer;
        KswordARKThreadIoctlLog(
            Device,
            "Info",
            "R0 enum-thread success: total=%lu, returned=%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->totalCount,
            (unsigned long)responseHeader->returnedCount,
            *BytesReturned);
    }
    else {
        KswordARKThreadIoctlLog(Device, "Warn", "R0 enum-thread success: outBytes=%Iu (header partial).", *BytesReturned);
    }

    return status;
}
