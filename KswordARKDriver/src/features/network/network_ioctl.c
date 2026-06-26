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

static NTSTATUS
KswordARKNetworkIoctlRetrieveAuditBuffers(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t RequiredOutputLength,
    _Outptr_result_maybenull_ KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST** QueryRequestOut,
    _Outptr_result_bytebuffer_(*ActualOutputLengthOut) PVOID* OutputBufferOut,
    _Out_ size_t* ActualOutputLengthOut
    )
/*++

Routine Description:

    提取网络审计 IOCTL 的可选请求与必需输出缓冲。中文说明：四个只读审计 handler
    共用相同 buffer 规则，避免在每个 handler 中复制 WDF 检索分支。

Arguments:

    Request - 当前 WDF 请求。
    InputBufferLength - dispatch 提供的输入长度。
    RequiredOutputLength - 响应头最小长度。
    QueryRequestOut - 接收可选请求；未提供时返回 NULL。
    OutputBufferOut - 接收输出缓冲。
    ActualOutputLengthOut - 接收输出缓冲实际长度。

Return Value:

    NTSTATUS from shared validation helpers.

--*/
{
    PVOID inputBuffer = NULL;
    size_t actualInputLength = 0U;
    BOOLEAN inputPresent = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (QueryRequestOut == NULL || OutputBufferOut == NULL || ActualOutputLengthOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *QueryRequestOut = NULL;
    *OutputBufferOut = NULL;
    *ActualOutputLengthOut = 0U;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &inputPresent);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (inputPresent) {
        UNREFERENCED_PARAMETER(actualInputLength);
        *QueryRequestOut = (KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST*)inputBuffer;
    }

    return KswordARKRetrieveRequiredOutputBuffer(
        Request,
        RequiredOutputLength,
        OutputBufferOut,
        ActualOutputLengthOut);
}

NTSTATUS
KswordARKNetworkIoctlQueryTcpEndpoints(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_NETWORK_QUERY_TCP_ENDPOINTS。中文说明：这是只读审计入口，
    handler 只负责 buffer 检索，TCP 表遍历由 network_audit.c 后端负责。

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
    KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKNetworkIoctlRetrieveAuditBuffers(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW),
        &queryRequest,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkIoctlLog(Device, "Error", "R0 network TCP audit buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKNetworkQueryTcpEndpoints(
        queryRequest,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW)) {
        KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE* response =
            (KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE*)outputBuffer;
        KswordARKNetworkIoctlLog(Device, "Info", "R0 network TCP audit status=%lu rows=%lu/%lu.", response->status, response->returnedRowCount, response->totalRowCount);
    }

    return status;
}

NTSTATUS
KswordARKNetworkIoctlQueryUdpEndpoints(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_NETWORK_QUERY_UDP_ENDPOINTS。中文说明：该入口只读返回 UDP
    endpoint 审计响应，不删除连接也不改变端口隐藏策略。

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
    KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKNetworkIoctlRetrieveAuditBuffers(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW),
        &queryRequest,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkIoctlLog(Device, "Error", "R0 network UDP audit buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKNetworkQueryUdpEndpoints(
        queryRequest,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW)) {
        KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE* response =
            (KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE*)outputBuffer;
        KswordARKNetworkIoctlLog(Device, "Info", "R0 network UDP audit status=%lu rows=%lu/%lu.", response->status, response->returnedRowCount, response->totalRowCount);
    }

    return status;
}

NTSTATUS
KswordARKNetworkIoctlQueryWfpInventory(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_NETWORK_QUERY_WFP_INVENTORY。中文说明：该入口只读返回
    WFP provider/sublayer/filter/callout inventory 骨架，不禁用或删除 WFP 对象。

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
    KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKNetworkIoctlRetrieveAuditBuffers(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW),
        &queryRequest,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkIoctlLog(Device, "Error", "R0 network WFP audit buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKNetworkQueryWfpInventory(
        queryRequest,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW)) {
        KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE* response =
            (KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE*)outputBuffer;
        KswordARKNetworkIoctlLog(Device, "Info", "R0 network WFP audit status=%lu rows=%lu/%lu.", response->status, response->returnedRowCount, response->totalRowCount);
    }

    return status;
}

NTSTATUS
KswordARKNetworkIoctlQueryNdisChain(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_NETWORK_QUERY_NDIS_CHAIN。中文说明：该入口只读返回
    NDIS chain 骨架，不 detach、不 pause、不重排任何 NDIS 组件。

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
    KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKNetworkIoctlRetrieveAuditBuffers(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW),
        &queryRequest,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkIoctlLog(Device, "Error", "R0 network NDIS audit buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKNetworkQueryNdisChain(
        queryRequest,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW)) {
        KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE* response =
            (KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE*)outputBuffer;
        KswordARKNetworkIoctlLog(Device, "Info", "R0 network NDIS audit status=%lu rows=%lu/%lu.", response->status, response->returnedRowCount, response->totalRowCount);
    }

    return status;
}
