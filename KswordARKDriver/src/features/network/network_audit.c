/*++

Module Name:

    network_audit.c

Abstract:

    Read-only network audit IOCTL backends for TCP, UDP, WFP and NDIS skeleton
    queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "network_internal.h"

#define KSWORD_ARK_NETWORK_ENDPOINT_HEADER_SIZE \
    (sizeof(KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW))

#define KSWORD_ARK_NETWORK_WFP_HEADER_SIZE \
    (sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW))

#define KSWORD_ARK_NETWORK_NDIS_HEADER_SIZE \
    (sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW))

#define KSWORD_ARK_NETWORK_AUDIT_DEFAULT_BUDGET_ROWS 256UL

static ULONG
KswordARKNetworkAuditNormalizeBudget(
    _In_opt_ const KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* Request
    )
/*++

Routine Description:

    计算网络审计返回预算。中文说明：0 表示使用保守默认值，过大的请求被限制到
    协议上限，保证后续 PDB traversal 接入时仍然是 budget-limited。

Arguments:

    Request - 可选审计请求。

Return Value:

    返回本次查询允许考虑的最大 row 数。

--*/
{
    ULONG budgetRows = KSWORD_ARK_NETWORK_AUDIT_DEFAULT_BUDGET_ROWS;

    if (Request != NULL && Request->maxRows != 0UL) {
        budgetRows = Request->maxRows;
    }
    if (budgetRows > KSWORD_ARK_NETWORK_AUDIT_MAX_REQUESTED_ROWS) {
        budgetRows = KSWORD_ARK_NETWORK_AUDIT_MAX_REQUESTED_ROWS;
    }

    return budgetRows;
}

static ULONG
KswordARKNetworkAuditNormalizeFlags(
    _In_opt_ const KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* Request
    )
/*++

Routine Description:

    规范化网络审计 flags。中文说明：调用方未提供地址族时默认 IPv4+IPv6，后续
    endpoint traversal 可以直接按该掩码过滤。

Arguments:

    Request - 可选审计请求。

Return Value:

    返回只包含已知审计查询 flag 的掩码。

--*/
{
    ULONG flags = KSWORD_ARK_NETWORK_AUDIT_QUERY_FLAG_INCLUDE_ALL;

    if (Request != NULL && (Request->flags & KSWORD_ARK_NETWORK_AUDIT_QUERY_FLAG_INCLUDE_ALL) != 0UL) {
        flags = Request->flags & KSWORD_ARK_NETWORK_AUDIT_QUERY_FLAG_INCLUDE_ALL;
    }

    return flags;
}

static NTSTATUS
KswordARKNetworkAuditValidateRequest(
    _In_opt_ const KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* Request
    )
/*++

Routine Description:

    校验网络审计请求头。中文说明：骨架阶段只接受协议版本和已知 flags；未知位
    直接拒绝，避免未来 R3 误以为隐藏开关已生效。

Arguments:

    Request - 可选审计请求。

Return Value:

    STATUS_SUCCESS 表示请求可使用；失败状态表示 handler 应返回参数错误。

--*/
{
    if (Request == NULL) {
        return STATUS_SUCCESS;
    }
    if (Request->size != 0UL && Request->size < sizeof(*Request)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->version != KSWORD_ARK_NETWORK_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }
    if ((Request->flags & ~KSWORD_ARK_NETWORK_AUDIT_QUERY_FLAG_INCLUDE_ALL) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
KswordARKNetworkAuditFillEndpointHeader(
    _Out_ KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE* Response,
    _In_ ULONG Flags,
    _In_ ULONG BudgetRows,
    _In_ ULONG RuntimeGeneration,
    _In_ NTSTATUS LastStatus
    )
/*++

Routine Description:

    填充 TCP/UDP endpoint 响应头。中文说明：当前骨架不遍历 tcpip 内部表，因此
    total/returned 都为 0，并通过 AUDIT_STUB 暴露降级原因。

Arguments:

    Response - endpoint 响应头。
    Flags - 规范化查询 flags。
    BudgetRows - 本次查询预算。
    RuntimeGeneration - 网络运行时 generation。
    LastStatus - 具体降级状态。

Return Value:

    None. 本函数没有返回值。

--*/
{
    Response->version = KSWORD_ARK_NETWORK_PROTOCOL_VERSION;
    Response->size = (ULONG)KSWORD_ARK_NETWORK_ENDPOINT_HEADER_SIZE;
    Response->status = KSWORD_ARK_NETWORK_STATUS_AUDIT_STUB;
    Response->flags = Flags;
    Response->totalRowCount = 0UL;
    Response->returnedRowCount = 0UL;
    Response->entrySize = sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW);
    Response->sourceFlags = KSWORD_ARK_NETWORK_AUDIT_SOURCE_NONE;
    Response->budgetRows = BudgetRows;
    Response->generation = RuntimeGeneration;
    Response->lastStatus = LastStatus;
}

NTSTATUS
KswordARKNetworkQueryTcpEndpoints(
    _In_opt_ const KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    构造 TCP endpoint 只读审计响应。中文说明：该阶段只提供稳定协议骨架，后续
    tcpip PDB traversal 接入时必须维持 count-first 和预算限制。

Arguments:

    Request - 可选查询请求。
    OutputBuffer - R3 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示写入降级响应；参数或缓冲错误直接返回失败。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE* response = NULL;
    ULONG generation = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_NETWORK_ENDPOINT_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = KswordARKNetworkAuditValidateRequest(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ExAcquirePushLockShared(&runtime->Lock);
    generation = runtime->Generation;
    ExReleasePushLockShared(&runtime->Lock);

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE*)OutputBuffer;
    KswordARKNetworkAuditFillEndpointHeader(
        response,
        KswordARKNetworkAuditNormalizeFlags(Request),
        KswordARKNetworkAuditNormalizeBudget(Request),
        generation,
        STATUS_NOT_IMPLEMENTED);
    *BytesWrittenOut = KSWORD_ARK_NETWORK_ENDPOINT_HEADER_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKNetworkQueryUdpEndpoints(
    _In_opt_ const KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    构造 UDP endpoint 只读审计响应。中文说明：当前不枚举 UDP endpoint，避免在
    未完成 PDB/锁纪律确认前读取 tcpip 私有表。

Arguments:

    Request - 可选查询请求。
    OutputBuffer - R3 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示写入降级响应；参数或缓冲错误直接返回失败。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE* response = NULL;
    ULONG generation = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_NETWORK_ENDPOINT_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = KswordARKNetworkAuditValidateRequest(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ExAcquirePushLockShared(&runtime->Lock);
    generation = runtime->Generation;
    ExReleasePushLockShared(&runtime->Lock);

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE*)OutputBuffer;
    KswordARKNetworkAuditFillEndpointHeader(
        response,
        KswordARKNetworkAuditNormalizeFlags(Request),
        KswordARKNetworkAuditNormalizeBudget(Request),
        generation,
        STATUS_NOT_IMPLEMENTED);
    *BytesWrittenOut = KSWORD_ARK_NETWORK_ENDPOINT_HEADER_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKNetworkQueryWfpInventory(
    _In_opt_ const KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    构造 WFP provider/sublayer/filter/callout inventory 骨架响应。中文说明：该函数
    不禁用 WFP、不删除 filter/callout，只为后续只读枚举保留协议面。

Arguments:

    Request - 可选查询请求。
    OutputBuffer - R3 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示写入降级响应；参数或缓冲错误直接返回失败。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE* response = NULL;
    ULONG generation = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_NETWORK_WFP_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = KswordARKNetworkAuditValidateRequest(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ExAcquirePushLockShared(&runtime->Lock);
    generation = runtime->Generation;
    ExReleasePushLockShared(&runtime->Lock);

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_NETWORK_PROTOCOL_VERSION;
    response->size = (ULONG)KSWORD_ARK_NETWORK_WFP_HEADER_SIZE;
    response->status = KSWORD_ARK_NETWORK_STATUS_AUDIT_STUB;
    response->flags = KswordARKNetworkAuditNormalizeFlags(Request);
    response->totalRowCount = 0UL;
    response->returnedRowCount = 0UL;
    response->entrySize = sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW);
    response->sourceFlags = KSWORD_ARK_NETWORK_AUDIT_SOURCE_RUNTIME_STATE;
    response->budgetRows = KswordARKNetworkAuditNormalizeBudget(Request);
    response->generation = generation;
    response->lastStatus = STATUS_NOT_IMPLEMENTED;
    *BytesWrittenOut = KSWORD_ARK_NETWORK_WFP_HEADER_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKNetworkQueryNdisChain(
    _In_opt_ const KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    构造 NDIS miniport/filter/protocol/binding chain 骨架响应。中文说明：该函数
    不 detach、不 pause、不重排 NDIS 组件，只返回只读审计协议头。

Arguments:

    Request - 可选查询请求。
    OutputBuffer - R3 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示写入降级响应；参数或缓冲错误直接返回失败。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE* response = NULL;
    ULONG generation = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_NETWORK_NDIS_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = KswordARKNetworkAuditValidateRequest(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ExAcquirePushLockShared(&runtime->Lock);
    generation = runtime->Generation;
    ExReleasePushLockShared(&runtime->Lock);

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_NETWORK_PROTOCOL_VERSION;
    response->size = (ULONG)KSWORD_ARK_NETWORK_NDIS_HEADER_SIZE;
    response->status = KSWORD_ARK_NETWORK_STATUS_AUDIT_STUB;
    response->flags = KswordARKNetworkAuditNormalizeFlags(Request);
    response->totalRowCount = 0UL;
    response->returnedRowCount = 0UL;
    response->entrySize = sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW);
    response->sourceFlags = KSWORD_ARK_NETWORK_AUDIT_SOURCE_NONE;
    response->budgetRows = KswordARKNetworkAuditNormalizeBudget(Request);
    response->generation = generation;
    response->lastStatus = STATUS_NOT_IMPLEMENTED;
    *BytesWrittenOut = KSWORD_ARK_NETWORK_NDIS_HEADER_SIZE;
    return STATUS_SUCCESS;
}
