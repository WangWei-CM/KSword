/*++

Module Name:

    trust_ioctl.c

Abstract:

    IOCTL handler for Phase-14 image trust diagnostics.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKTrustIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    输出 trust 查询日志。中文说明：日志只记录长度、flags、状态码，不输出完整
    文件路径，避免日志中泄露用户隐私路径。

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
KswordARKTrustIoctlQueryImageTrust(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_IMAGE_TRUST。中文说明：handler 只做协议
    长度/flags/path 校验，真实 CI/signing level 查询交给 trust_query.c。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation or feature backend.

--*/
{
    KSWORD_ARK_QUERY_IMAGE_TRUST_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    const ULONG allowedFlags =
        KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_GLOBAL_CI |
        KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_FILE_SIGNING_LEVEL |
        KSWORD_ARK_TRUST_QUERY_FLAG_OPEN_REPARSE_POINT;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_IMAGE_TRUST_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKTrustIoctlLog(Device, "Error", "R0 query-image-trust: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_IMAGE_TRUST_REQUEST*)inputBuffer;
    if ((queryRequest->flags & ~allowedFlags) != 0UL) {
        KswordARKTrustIoctlLog(Device, "Warn", "R0 query-image-trust: flags rejected, flags=0x%08X.", (unsigned int)queryRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    if (queryRequest->pathLengthChars >= KSWORD_ARK_TRUST_PATH_MAX_CHARS) {
        KswordARKTrustIoctlLog(Device, "Warn", "R0 query-image-trust: path length rejected, chars=%u.", (unsigned int)queryRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }
    if (queryRequest->pathLengthChars != 0U &&
        queryRequest->path[queryRequest->pathLengthChars] != L'\0') {
        KswordARKTrustIoctlLog(Device, "Warn", "R0 query-image-trust: path not null-terminated, chars=%u.", (unsigned int)queryRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKTrustIoctlLog(Device, "Error", "R0 query-image-trust: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryImageTrust(
        outputBuffer,
        actualOutputLength,
        queryRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKTrustIoctlLog(Device, "Error", "R0 query-image-trust failed: chars=%u, status=0x%08X.", (unsigned int)queryRequest->pathLengthChars, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE)) {
        KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE* response =
            (KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE*)outputBuffer;
        KswordARKTrustIoctlLog(
            Device,
            "Info",
            "R0 query-image-trust success: status=%lu, source=%lu, fields=0x%08X.",
            (unsigned long)response->queryStatus,
            (unsigned long)response->trustSource,
            (unsigned int)response->fieldFlags);
    }

    return STATUS_SUCCESS;
}
