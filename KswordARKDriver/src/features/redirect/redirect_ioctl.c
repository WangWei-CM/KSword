/*++

Module Name:

    redirect_ioctl.c

Abstract:

    IOCTL handlers for KswordARK file and registry redirection rules.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKRedirectIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    输出重定向 IOCTL 日志。中文说明：只记录规则数量和状态码，不记录完整路径，
    避免敏感路径进入日志通道。

Arguments:

    Device - WDF 设备对象。
    LevelText - 日志级别。
    FormatText - printf 风格格式串。
    ... - 格式化参数。

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
KswordARKRedirectIoctlSetRules(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_REDIRECT_SET_RULES。中文说明：规则修改需要写权限，后端
    完成完整校验与快照替换。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 WDF 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from validation or backend.

--*/
{
    KSWORD_ARK_REDIRECT_SET_RULES_REQUEST* setRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKRedirectIoctlLog(Device, "Warn", "R0 redirect set-rules denied, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST),
        (PVOID*)&setRequest,
        NULL);
    if (!NT_SUCCESS(status)) {
        KswordARKRedirectIoctlLog(Device, "Error", "R0 redirect set-rules input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKRedirectIoctlLog(Device, "Error", "R0 redirect set-rules output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRedirectSetRules(
        setRequest,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE)) {
        KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE* response =
            (KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE*)outputBuffer;
        KswordARKRedirectIoctlLog(
            Device,
            response->status == KSWORD_ARK_REDIRECT_STATUS_APPLIED ? "Info" : "Warn",
            "R0 redirect set-rules status=%lu file=%lu registry=%lu last=0x%08X.",
            (unsigned long)response->status,
            (unsigned long)response->fileRuleCount,
            (unsigned long)response->registryRuleCount,
            (unsigned int)response->lastStatus);
    }

    return status;
}

NTSTATUS
KswordARKRedirectIoctlQueryStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_REDIRECT_QUERY_STATUS。中文说明：返回当前规则快照、命中
    计数和 registry callback 注册状态。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 WDF 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from output retrieval or backend.

--*/
{
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_REDIRECT_STATUS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKRedirectIoctlLog(Device, "Error", "R0 redirect status output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRedirectQueryStatus(
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_REDIRECT_STATUS_RESPONSE)) {
        KSWORD_ARK_REDIRECT_STATUS_RESPONSE* response =
            (KSWORD_ARK_REDIRECT_STATUS_RESPONSE*)outputBuffer;
        KswordARKRedirectIoctlLog(
            Device,
            "Info",
            "R0 redirect status flags=0x%08X file=%lu registry=%lu.",
            (unsigned int)response->runtimeFlags,
            (unsigned long)response->fileRuleCount,
            (unsigned long)response->registryRuleCount);
    }

    return status;
}
