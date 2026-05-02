/*++

Module Name:

    network_ioctl.c

Abstract:

    IOCTL handlers for KswordARK network filter and port-hide rules.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKNetworkIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    输出网络 IOCTL 日志。中文说明：规则变更属于安全敏感操作，记录状态与规则数
    便于 R3 日志面板审计。

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
KswordARKNetworkIoctlSetRules(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_NETWORK_SET_RULES。中文说明：该 IOCTL 需要写权限，规则
    后端负责完整校验与一次性快照替换。

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
    KSWORD_ARK_NETWORK_SET_RULES_REQUEST* setRequest = NULL;
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
        KswordARKNetworkIoctlLog(Device, "Warn", "R0 network set-rules denied, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_NETWORK_SET_RULES_REQUEST),
        (PVOID*)&setRequest,
        NULL);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkIoctlLog(Device, "Error", "R0 network set-rules input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_NETWORK_SET_RULES_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkIoctlLog(Device, "Error", "R0 network set-rules output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKNetworkSetRules(
        setRequest,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_NETWORK_SET_RULES_RESPONSE)) {
        KSWORD_ARK_NETWORK_SET_RULES_RESPONSE* response =
            (KSWORD_ARK_NETWORK_SET_RULES_RESPONSE*)outputBuffer;
        KswordARKNetworkIoctlLog(
            Device,
            response->status == KSWORD_ARK_NETWORK_STATUS_APPLIED ? "Info" : "Warn",
            "R0 network set-rules status=%lu rules=%lu block=%lu hide=%lu last=0x%08X.",
            (unsigned long)response->status,
            (unsigned long)response->appliedCount,
            (unsigned long)response->blockedRuleCount,
            (unsigned long)response->hiddenPortRuleCount,
            (unsigned int)response->lastStatus);
    }

    return status;
}

NTSTATUS
KswordARKNetworkIoctlQueryStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS。中文说明：输出固定状态响应，包含
    WFP 注册状态、规则快照和 classify 计数。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 WDF 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from buffer retrieval or backend.

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
        sizeof(KSWORD_ARK_NETWORK_STATUS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkIoctlLog(Device, "Error", "R0 network status output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKNetworkQueryStatus(
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_NETWORK_STATUS_RESPONSE)) {
        KSWORD_ARK_NETWORK_STATUS_RESPONSE* response =
            (KSWORD_ARK_NETWORK_STATUS_RESPONSE*)outputBuffer;
        KswordARKNetworkIoctlLog(
            Device,
            "Info",
            "R0 network status flags=0x%08X rules=%lu blockedHits=%I64u.",
            (unsigned int)response->runtimeFlags,
            (unsigned long)response->ruleCount,
            (unsigned long long)response->blockedCount);
    }

    return status;
}
