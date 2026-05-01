/*++

Module Name:

    preflight_ioctl.c

Abstract:

    IOCTL handler for Phase-16 release preflight diagnostics.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKPreflightIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    输出 preflight 查询日志。中文说明：发布前诊断日志只记录汇总状态和数量，
    详细 check 文本由响应包承载。

Arguments:

    Device - WDF 设备对象。
    LevelText - 日志级别。
    FormatText - printf 风格格式串。
    ... - 格式参数。

Return Value:

    None. 本函数没有返回值。

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
KswordARKPreflightIoctlQuery(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_PREFLIGHT。中文说明：输入可选；输出按容量返回
    check entries，R3 可根据 totalCheckCount 扩容后重试。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 写入字节数。

Return Value:

    NTSTATUS from validation or preflight backend.

--*/
{
    KSWORD_ARK_QUERY_PREFLIGHT_REQUEST* preflightRequest = NULL;
    KSWORD_ARK_QUERY_PREFLIGHT_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_QUERY_PREFLIGHT_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKPreflightIoctlLog(Device, "Error", "R0 preflight: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (hasInput) {
        preflightRequest = (KSWORD_ARK_QUERY_PREFLIGHT_REQUEST*)inputBuffer;
        if (preflightRequest->size < sizeof(KSWORD_ARK_QUERY_PREFLIGHT_REQUEST) ||
            preflightRequest->version != KSWORD_ARK_PREFLIGHT_PROTOCOL_VERSION ||
            (preflightRequest->flags & ~KSWORD_ARK_PREFLIGHT_QUERY_FLAG_INCLUDE_ALL) != 0UL) {
            KswordARKPreflightIoctlLog(Device, "Warn", "R0 preflight: request rejected, flags=0x%08X.", (unsigned int)preflightRequest->flags);
            return STATUS_INVALID_PARAMETER;
        }
    }
    else {
        preflightRequest = &defaultRequest;
        preflightRequest->size = sizeof(defaultRequest);
        preflightRequest->version = KSWORD_ARK_PREFLIGHT_PROTOCOL_VERSION;
        preflightRequest->flags = KSWORD_ARK_PREFLIGHT_QUERY_FLAG_INCLUDE_EXTERNAL_GATES;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_PREFLIGHT_RESPONSE) - sizeof(KSWORD_ARK_PREFLIGHT_CHECK_ENTRY),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKPreflightIoctlLog(Device, "Error", "R0 preflight: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKPreflightQuery(
        outputBuffer,
        actualOutputLength,
        preflightRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKPreflightIoctlLog(Device, "Error", "R0 preflight query failed, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_PREFLIGHT_RESPONSE) - sizeof(KSWORD_ARK_PREFLIGHT_CHECK_ENTRY)) {
        KSWORD_ARK_QUERY_PREFLIGHT_RESPONSE* response =
            (KSWORD_ARK_QUERY_PREFLIGHT_RESPONSE*)outputBuffer;
        KswordARKPreflightIoctlLog(
            Device,
            response->overallStatus <= KSWORD_ARK_PREFLIGHT_STATUS_WARN ? "Info" : "Warn",
            "R0 preflight complete: overall=%lu, checks=%lu/%lu.",
            (unsigned long)response->overallStatus,
            (unsigned long)response->returnedCheckCount,
            (unsigned long)response->totalCheckCount);
    }

    return STATUS_SUCCESS;
}
