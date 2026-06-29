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

// {D2B28BC6-9E08-4D07-9F7B-2BA821D8AA51}
static const GUID KSWORD_ARK_AUDIT_WFP_SUBLAYER =
{ 0xd2b28bc6, 0x9e08, 0x4d07, { 0x9f, 0x7b, 0x2b, 0xa8, 0x21, 0xd8, 0xaa, 0x51 } };

// {9CEBA6FD-DC43-4E48-A013-FDB83823674B}
static const GUID KSWORD_ARK_AUDIT_WFP_CONNECT_CALLOUT =
{ 0x9ceba6fd, 0xdc43, 0x4e48, { 0xa0, 0x13, 0xfd, 0xb8, 0x38, 0x23, 0x67, 0x4b } };

// {75FCE1D8-0E28-4C58-9957-90181B75B6AA}
static const GUID KSWORD_ARK_AUDIT_WFP_RECV_ACCEPT_CALLOUT =
{ 0x75fce1d8, 0x0e28, 0x4c58, { 0x99, 0x57, 0x90, 0x18, 0x1b, 0x75, 0xb6, 0xaa } };

// {C38D57D1-05A7-4C33-904F-7FBCEEE60E82}
static const GUID KSWORD_ARK_AUDIT_FWPM_LAYER_ALE_AUTH_CONNECT_V4 =
{ 0xc38d57d1, 0x05a7, 0x4c33, { 0x90, 0x4f, 0x7f, 0xbc, 0xee, 0xe6, 0x0e, 0x82 } };

// {E1CD9FE7-F4B5-4273-96C0-592E487B8650}
static const GUID KSWORD_ARK_AUDIT_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4 =
{ 0xe1cd9fe7, 0xf4b5, 0x4273, { 0x96, 0xc0, 0x59, 0x2e, 0x48, 0x7b, 0x86, 0x50 } };

#define KSWORD_ARK_AUDIT_FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4 43UL
#define KSWORD_ARK_AUDIT_FWPS_LAYER_ALE_AUTH_CONNECT_V4 47UL

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

    填充 TCP/UDP endpoint 响应头。中文说明：当前不遍历 tcpip 内部表，因此
    total/returned 都为 0，并通过 AUDIT_UNAVAILABLE 暴露安全降级原因。

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
    Response->status = KSWORD_ARK_NETWORK_STATUS_AUDIT_UNAVAILABLE;
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
        STATUS_NOT_SUPPORTED);
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
        STATUS_NOT_SUPPORTED);
    *BytesWrittenOut = KSWORD_ARK_NETWORK_ENDPOINT_HEADER_SIZE;
    return STATUS_SUCCESS;
}

static ULONG
KswordARKNetworkAuditWfpCapacityFromBuffer(
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    计算 WFP inventory 输出缓冲能容纳的行数。中文说明：响应结构尾部声明为
    entries[1]，实际 METHOD_BUFFERED 缓冲按 header + N * row 解释。

Arguments:

    OutputBufferLength - R3 提供的输出缓冲长度。

Return Value:

    返回当前缓冲最多可写入的 WFP 行数。

--*/
{
    size_t payloadLength = 0U;
    size_t rowCapacity = 0U;

    if (OutputBufferLength <= KSWORD_ARK_NETWORK_WFP_HEADER_SIZE) {
        return 0UL;
    }

    payloadLength = OutputBufferLength - KSWORD_ARK_NETWORK_WFP_HEADER_SIZE;
    rowCapacity = payloadLength / sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW);
    if (rowCapacity > 0xFFFFFFFFULL) {
        return 0xFFFFFFFFUL;
    }

    return (ULONG)rowCapacity;
}

static VOID
KswordARKNetworkAuditCopyGuid(
    _Out_writes_bytes_(16) UCHAR* Destination,
    _In_ const GUID* Source
    )
/*++

Routine Description:

    拷贝 GUID 到 shared 协议的 16 字节原始字段。中文说明：协议层使用 byte[16]
    避免 R3/R0 结构体对 GUID 类型定义产生额外依赖。

Arguments:

    Destination - 目标 16 字节缓冲。
    Source - 源 GUID。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (Destination == NULL || Source == NULL) {
        return;
    }

    RtlCopyMemory(Destination, Source, sizeof(GUID));
}

static VOID
KswordARKNetworkAuditCopyName(
    _Out_writes_(KSWORD_ARK_NETWORK_NAME_CHARS) WCHAR* Destination,
    _In_z_ PCWSTR Source
    )
/*++

Routine Description:

    拷贝固定长度诊断名称。中文说明：所有 owner/name 字段都保证 NUL 结尾，避免
    R3 展示层读取越界。

Arguments:

    Destination - 协议行里的固定宽字符缓冲。
    Source - 只读常量字符串。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (Destination == NULL || Source == NULL) {
        return;
    }

    (VOID)RtlStringCchCopyW(Destination, KSWORD_ARK_NETWORK_NAME_CHARS, Source);
}

static VOID
KswordARKNetworkAuditFillWfpSublayerRow(
    _Out_ KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW* Row,
    _In_ ULONG RowId
    )
/*++

Routine Description:

    填充 KswordARK 自有 WFP sublayer 行。中文说明：该证据来自驱动注册时使用的
    固定 GUID，不读取 BFE 私有结构，也不修改任何 WFP 对象。

Arguments:

    Row - 输出行。
    RowId - 本次响应内的行号。

Return Value:

    None. 本函数没有返回值。

--*/
{
    RtlZeroMemory(Row, sizeof(*Row));
    Row->rowId = RowId;
    Row->objectKind = KSWORD_ARK_NETWORK_WFP_OBJECT_SUBLAYER;
    Row->flags = KSWORD_ARK_NETWORK_AUDIT_ROW_FLAG_FIELD_MISSING;
    Row->fieldMask = 0UL;
    KswordARKNetworkAuditCopyGuid(Row->objectKey, &KSWORD_ARK_AUDIT_WFP_SUBLAYER);
    KswordARKNetworkAuditCopyName(Row->ownerModule, L"KswordARK.sys runtime sublayer");
}

static VOID
KswordARKNetworkAuditFillWfpCalloutRow(
    _Out_ KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW* Row,
    _In_ ULONG RowId,
    _In_ const GUID* CalloutKey,
    _In_ const GUID* LayerKey,
    _In_ ULONG LayerId,
    _In_ ULONG CalloutId,
    _In_z_ PCWSTR NameText
    )
/*++

Routine Description:

    填充 KswordARK 自有 WFP callout 行。中文说明：calloutId 来自运行时注册结果，
    classify 函数地址暂不导出到协议行，避免把未声明 ABI 当成完整证据。

Arguments:

    Row - 输出行。
    RowId - 本次响应内的行号。
    CalloutKey - BFE callout GUID。
    LayerKey - WFP layer GUID。
    LayerId - FWPS layer 数值 ID。
    CalloutId - FwpsCalloutRegister 返回的 callout id。
    NameText - 诊断名称。

Return Value:

    None. 本函数没有返回值。

--*/
{
    RtlZeroMemory(Row, sizeof(*Row));
    Row->rowId = RowId;
    Row->objectKind = KSWORD_ARK_NETWORK_WFP_OBJECT_CALLOUT;
    Row->flags = KSWORD_ARK_NETWORK_AUDIT_ROW_FLAG_FIELD_MISSING;
    Row->layerId = LayerId;
    Row->calloutId = CalloutId;
    Row->fieldMask = 0UL;
    KswordARKNetworkAuditCopyGuid(Row->subLayerKey, LayerKey);
    KswordARKNetworkAuditCopyGuid(Row->objectKey, CalloutKey);
    KswordARKNetworkAuditCopyName(Row->ownerModule, NameText);
}

static VOID
KswordARKNetworkAuditFillWfpFilterRow(
    _Out_ KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW* Row,
    _In_ ULONG RowId,
    _In_ const GUID* CalloutKey,
    _In_ const GUID* LayerKey,
    _In_ ULONG LayerId,
    _In_ ULONG CalloutId,
    _In_ UINT64 FilterId,
    _In_z_ PCWSTR NameText
    )
/*++

Routine Description:

    填充 KswordARK 自有 WFP filter 行。中文说明：filterId 来自 FwpmFilterAdd0 输出，
    可用于 R3 确认本驱动 filter 是否注册成功，但本查询不删除、不禁用 filter。

Arguments:

    Row - 输出行。
    RowId - 本次响应内的行号。
    CalloutKey - filter action 引用的 callout GUID。
    LayerKey - filter 所属 layer GUID。
    LayerId - FWPS layer 数值 ID。
    CalloutId - 对应 callout id。
    FilterId - BFE filter id。
    NameText - 诊断名称。

Return Value:

    None. 本函数没有返回值。

--*/
{
    RtlZeroMemory(Row, sizeof(*Row));
    Row->rowId = RowId;
    Row->objectKind = KSWORD_ARK_NETWORK_WFP_OBJECT_FILTER;
    Row->flags = KSWORD_ARK_NETWORK_AUDIT_ROW_FLAG_FIELD_MISSING;
    Row->layerId = LayerId;
    Row->calloutId = CalloutId;
    Row->filterId = FilterId;
    Row->fieldMask = 0UL;
    KswordARKNetworkAuditCopyGuid(Row->providerKey, CalloutKey);
    KswordARKNetworkAuditCopyGuid(Row->subLayerKey, &KSWORD_ARK_AUDIT_WFP_SUBLAYER);
    KswordARKNetworkAuditCopyGuid(Row->objectKey, LayerKey);
    KswordARKNetworkAuditCopyName(Row->ownerModule, NameText);
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

    构造 WFP provider/sublayer/filter/callout inventory 响应。中文说明：该函数只导出
    KswordARK 运行时已经注册的 WFP 对象 ID/GUID，不遍历 BFE 私有链表。

Arguments:

    Request - 可选查询请求。
    OutputBuffer - R3 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示写入只读响应；参数或缓冲错误直接返回失败。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE* response = NULL;
    ULONG generation = 0UL;
    ULONG runtimeFlags = 0UL;
    ULONG connectCalloutId = 0UL;
    ULONG recvAcceptCalloutId = 0UL;
    UINT64 connectFilterId = 0ULL;
    UINT64 recvAcceptFilterId = 0ULL;
    ULONG budgetRows = 0UL;
    ULONG rowCapacity = 0UL;
    ULONG totalRows = 0UL;
    ULONG rowsToWrite = 0UL;
    ULONG rowIndex = 0UL;
    NTSTATUS registerStatus = STATUS_SUCCESS;
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
    runtimeFlags = runtime->RuntimeFlags;
    connectCalloutId = runtime->ConnectCalloutId;
    recvAcceptCalloutId = runtime->RecvAcceptCalloutId;
    connectFilterId = runtime->ConnectFilterId;
    recvAcceptFilterId = runtime->RecvAcceptFilterId;
    registerStatus = runtime->RegisterStatus;
    ExReleasePushLockShared(&runtime->Lock);

    budgetRows = KswordARKNetworkAuditNormalizeBudget(Request);
    rowCapacity = KswordARKNetworkAuditWfpCapacityFromBuffer(OutputBufferLength);
    if ((runtimeFlags & KSWORD_ARK_NETWORK_RUNTIME_WFP_STARTED) != 0UL) {
        totalRows = 5UL;
    }
    rowsToWrite = totalRows;
    if (rowsToWrite > budgetRows) {
        rowsToWrite = budgetRows;
    }
    if (rowsToWrite > rowCapacity) {
        rowsToWrite = rowCapacity;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_NETWORK_PROTOCOL_VERSION;
    response->size = (ULONG)KSWORD_ARK_NETWORK_WFP_HEADER_SIZE;
    response->status = (totalRows != 0UL) ?
        KSWORD_ARK_NETWORK_STATUS_APPLIED :
        KSWORD_ARK_NETWORK_STATUS_AUDIT_UNAVAILABLE;
    response->flags = KswordARKNetworkAuditNormalizeFlags(Request);
    response->totalRowCount = totalRows;
    response->returnedRowCount = rowsToWrite;
    response->entrySize = sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW);
    response->sourceFlags = KSWORD_ARK_NETWORK_AUDIT_SOURCE_RUNTIME_STATE;
    response->budgetRows = budgetRows;
    response->generation = generation;
    response->lastStatus = (totalRows != 0UL) ? STATUS_SUCCESS : registerStatus;

    if (rowIndex < rowsToWrite) {
        KswordARKNetworkAuditFillWfpSublayerRow(&response->entries[rowIndex], rowIndex);
        rowIndex += 1UL;
    }
    if (rowIndex < rowsToWrite) {
        KswordARKNetworkAuditFillWfpCalloutRow(
            &response->entries[rowIndex],
            rowIndex,
            &KSWORD_ARK_AUDIT_WFP_CONNECT_CALLOUT,
            &KSWORD_ARK_AUDIT_FWPM_LAYER_ALE_AUTH_CONNECT_V4,
            KSWORD_ARK_AUDIT_FWPS_LAYER_ALE_AUTH_CONNECT_V4,
            connectCalloutId,
            L"KswordARK ALE connect callout");
        rowIndex += 1UL;
    }
    if (rowIndex < rowsToWrite) {
        KswordARKNetworkAuditFillWfpCalloutRow(
            &response->entries[rowIndex],
            rowIndex,
            &KSWORD_ARK_AUDIT_WFP_RECV_ACCEPT_CALLOUT,
            &KSWORD_ARK_AUDIT_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
            KSWORD_ARK_AUDIT_FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
            recvAcceptCalloutId,
            L"KswordARK ALE recv-accept callout");
        rowIndex += 1UL;
    }
    if (rowIndex < rowsToWrite) {
        KswordARKNetworkAuditFillWfpFilterRow(
            &response->entries[rowIndex],
            rowIndex,
            &KSWORD_ARK_AUDIT_WFP_CONNECT_CALLOUT,
            &KSWORD_ARK_AUDIT_FWPM_LAYER_ALE_AUTH_CONNECT_V4,
            KSWORD_ARK_AUDIT_FWPS_LAYER_ALE_AUTH_CONNECT_V4,
            connectCalloutId,
            connectFilterId,
            L"KswordARK ALE connect filter");
        rowIndex += 1UL;
    }
    if (rowIndex < rowsToWrite) {
        KswordARKNetworkAuditFillWfpFilterRow(
            &response->entries[rowIndex],
            rowIndex,
            &KSWORD_ARK_AUDIT_WFP_RECV_ACCEPT_CALLOUT,
            &KSWORD_ARK_AUDIT_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
            KSWORD_ARK_AUDIT_FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
            recvAcceptCalloutId,
            recvAcceptFilterId,
            L"KswordARK ALE recv-accept filter");
    }

    *BytesWrittenOut = KSWORD_ARK_NETWORK_WFP_HEADER_SIZE +
        ((size_t)rowsToWrite * sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW));
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
    response->status = KSWORD_ARK_NETWORK_STATUS_AUDIT_UNAVAILABLE;
    response->flags = KswordARKNetworkAuditNormalizeFlags(Request);
    response->totalRowCount = 0UL;
    response->returnedRowCount = 0UL;
    response->entrySize = sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW);
    response->sourceFlags = KSWORD_ARK_NETWORK_AUDIT_SOURCE_NONE;
    response->budgetRows = KswordARKNetworkAuditNormalizeBudget(Request);
    response->generation = generation;
    response->lastStatus = STATUS_NOT_SUPPORTED;
    *BytesWrittenOut = KSWORD_ARK_NETWORK_NDIS_HEADER_SIZE;
    return STATUS_SUCCESS;
}
