/*++

Module Name:

    memory_physical.c

Abstract:

    R0 physical memory read/write helpers for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#ifndef STATUS_PARTIAL_COPY
#define STATUS_PARTIAL_COPY ((NTSTATUS)0x8000000DL)
#endif

#ifndef STATUS_REQUEST_NOT_ACCEPTED
#define STATUS_REQUEST_NOT_ACCEPTED ((NTSTATUS)0xC00000D0L)
#endif

/* 物理读响应头不包含尾随 data[1]，调用方按 header + bytesRead 解释返回长度。 */
#define KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE) - sizeof(((KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE*)0)->data))

/* 物理写请求头不包含尾随 data[1]，用于校验 METHOD_BUFFERED 输入是否完整。 */
#define KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST) - sizeof(((KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST*)0)->data))

// x64 当前页表格式最多使用 52 位物理地址；更高位保守拒绝，避免 LARGE_INTEGER 符号扩展风险。
#define KSWORD_ARK_PHYSICAL_ADDRESS_MAX 0x000FFFFFFFFFFFFFULL

static BOOLEAN
KswordARKPhysicalIsRangeValid(
    _In_ ULONG64 PhysicalAddress,
    _In_ SIZE_T Length
    )
/*++

Routine Description:

    校验物理地址区间是否可被本模块接受。中文说明：函数只做通用算术保护，
    包括零长度、52 位上限和 endAddress 环绕检测，不判断具体机器是否装有该页。

Arguments:

    PhysicalAddress - 起始物理地址。
    Length - 请求长度，允许为零。

Return Value:

    TRUE 表示区间没有溢出且位于保守物理地址上限内；FALSE 表示应拒绝。

--*/
{
    ULONG64 endAddress = 0ULL;

    if (PhysicalAddress > KSWORD_ARK_PHYSICAL_ADDRESS_MAX) {
        return FALSE;
    }
    if (Length == 0U) {
        return TRUE;
    }
    if ((ULONG64)Length > (KSWORD_ARK_PHYSICAL_ADDRESS_MAX - PhysicalAddress + 1ULL)) {
        return FALSE;
    }

    endAddress = PhysicalAddress + (ULONG64)Length - 1ULL;
    if (endAddress < PhysicalAddress) {
        return FALSE;
    }

    return endAddress <= KSWORD_ARK_PHYSICAL_ADDRESS_MAX;
}

static VOID
KswordARKPhysicalInitReadResponse(
    _Out_ KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE* Response,
    _In_ const KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST* Request
    )
/*++

Routine Description:

    初始化物理内存读取响应头。中文说明：所有状态先填成不可用或未支持，后续
    读取路径只更新实际完成的字段，避免失败分支留下未初始化数据。

Arguments:

    Response - 输出响应头。
    Request - 输入读取请求。

Return Value:

    None. 函数只写入调用方提供的响应结构。

--*/
{
    Response->version = KSWORD_ARK_MEMORY_PROTOCOL_VERSION;
    Response->headerSize = KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE;
    Response->fieldFlags = 0UL;
    Response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_UNAVAILABLE;
    Response->copyStatus = STATUS_NOT_SUPPORTED;
    Response->source = KSWORD_ARK_MEMORY_SOURCE_R0_MM_COPY_PHYSICAL_MEMORY;
    Response->requestedBytes = Request->bytesToRead;
    Response->bytesRead = 0UL;
    Response->maxBytesPerRequest = KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES;
    Response->requestedPhysicalAddress = Request->physicalAddress;
}

static VOID
KswordARKPhysicalInitWriteResponse(
    _Out_ KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE* Response,
    _In_ const KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST* Request
    )
/*++

Routine Description:

    初始化物理内存写入响应头。中文说明：写入是高风险路径，默认状态不会假定
    已经映射或已经复制，调用方可通过 mapStatus/copyStatus 看到失败位置。

Arguments:

    Response - 输出响应头。
    Request - 输入写入请求。

Return Value:

    None. 函数不返回状态。

--*/
{
    Response->version = KSWORD_ARK_MEMORY_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    Response->fieldFlags = KSWORD_ARK_MEMORY_FIELD_WRITE_DATA_PRESENT;
    Response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_UNAVAILABLE;
    Response->mapStatus = STATUS_NOT_SUPPORTED;
    Response->copyStatus = STATUS_NOT_SUPPORTED;
    Response->source = KSWORD_ARK_MEMORY_SOURCE_R0_MM_MAP_PHYSICAL_MEMORY;
    Response->requestedBytes = Request->bytesToWrite;
    Response->bytesWritten = 0UL;
    Response->maxBytesPerRequest = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES;
    Response->requestedPhysicalAddress = Request->physicalAddress;
}

NTSTATUS
KswordARKDriverReadPhysicalMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    安全读取一段物理内存。中文说明：函数优先使用 MmCopyMemory 的 physical
    copy 路径，不建立持久映射；读取长度被协议上限限制，并且所有地址计算先做
    溢出检查。需要 PASSIVE_LEVEL，避免在高 IRQL 下触发可能阻塞的内核路径。

Arguments:

    OutputBuffer - 响应缓冲区，头部后紧跟读取数据。
    OutputBufferLength - 响应缓冲区总长度。
    Request - 读取请求，包含物理地址和长度。
    BytesWrittenOut - 接收实际写入响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；读取细节写入 response->readStatus/copyStatus。

--*/
{
    KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE* response = NULL;
    MM_COPY_ADDRESS copyAddress;
    SIZE_T bytesAvailable = 0U;
    SIZE_T bytesCopied = 0U;
    SIZE_T bytesToRead = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->flags != 0UL || Request->reserved != 0UL || Request->reserved2 != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->bytesToRead > KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE*)OutputBuffer;
    KswordARKPhysicalInitReadResponse(response, Request);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_IRQL_REJECTED;
        response->copyStatus = STATUS_INVALID_DEVICE_STATE;
        *BytesWrittenOut = KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    bytesToRead = (SIZE_T)Request->bytesToRead;
    if (!KswordARKPhysicalIsRangeValid(Request->physicalAddress, bytesToRead)) {
        response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_RANGE_REJECTED;
        response->copyStatus = STATUS_INVALID_PARAMETER;
        *BytesWrittenOut = KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    if (bytesToRead == 0U) {
        response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_OK;
        response->copyStatus = STATUS_SUCCESS;
        response->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT;
        *BytesWrittenOut = KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    bytesAvailable = OutputBufferLength - KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE;
    if (bytesToRead > bytesAvailable) {
        response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_BUFFER_TOO_SMALL;
        response->copyStatus = STATUS_BUFFER_TOO_SMALL;
        *BytesWrittenOut = KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&copyAddress, sizeof(copyAddress));
    copyAddress.PhysicalAddress.QuadPart = (LONGLONG)Request->physicalAddress;

    __try {
        status = MmCopyMemory(
            response->data,
            copyAddress,
            bytesToRead,
            MM_COPY_MEMORY_PHYSICAL,
            &bytesCopied);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        bytesCopied = 0U;
    }

    response->copyStatus = status;
    response->bytesRead = (ULONG)bytesCopied;
    if (bytesCopied > 0U) {
        response->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT;
    }

    if (NT_SUCCESS(status) && bytesCopied == bytesToRead) {
        response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_OK;
    }
    else if (bytesCopied > 0U || status == STATUS_PARTIAL_COPY) {
        response->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PARTIAL_COPY;
        response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_PARTIAL;
    }
    else {
        response->readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_COPY_FAILED;
    }

    *BytesWrittenOut = KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE + bytesCopied;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverWritePhysicalMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST* Request,
    _In_ size_t RequestBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    受控写入一段物理内存。中文说明：函数要求 FORCE 标志、限制长度、校验
    METHOD_BUFFERED 输入完整性，并用 MmMapIoSpaceEx/MmUnmapIoSpace 成对管理
    临时映射；不提供 PTE/PDE 修改，也不绕过 PatchGuard。

Arguments:

    OutputBuffer - 固定响应缓冲区。
    OutputBufferLength - 响应缓冲区总长度。
    Request - 写入请求，头部后紧跟待写字节。
    RequestBufferLength - 输入缓冲区实际长度。
    BytesWrittenOut - 接收固定响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；写入细节写入 response->writeStatus。

--*/
{
    KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE* response = NULL;
    PHYSICAL_ADDRESS physicalAddress;
    PVOID mappedAddress = NULL;
    PVOID writeAddress = NULL;
    ULONG64 pageOffset = 0ULL;
    SIZE_T mappedLength = 0U;
    SIZE_T bytesToWrite = 0U;
    SIZE_T requiredInputBytes = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < sizeof(KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (RequestBufferLength < KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->reserved != 0UL || Request->reserved2 != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((Request->flags &
        ~(KSWORD_ARK_PHYSICAL_WRITE_FLAG_UI_CONFIRMED | KSWORD_ARK_PHYSICAL_WRITE_FLAG_FORCE)) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->bytesToWrite == 0UL ||
        Request->bytesToWrite > KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    bytesToWrite = (SIZE_T)Request->bytesToWrite;
    if (bytesToWrite > (MAXSIZE_T - KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE)) {
        return STATUS_INVALID_PARAMETER;
    }
    requiredInputBytes = KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE + bytesToWrite;
    if (RequestBufferLength < requiredInputBytes) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE*)OutputBuffer;
    KswordARKPhysicalInitWriteResponse(response, Request);

    if ((Request->flags & KSWORD_ARK_PHYSICAL_WRITE_FLAG_FORCE) == 0UL) {
        response->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_FORCE_WRITE_REQUIRED;
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_FORCE_REQUIRED;
        response->mapStatus = STATUS_REQUEST_NOT_ACCEPTED;
        response->copyStatus = STATUS_REQUEST_NOT_ACCEPTED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    response->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_FORCE_WRITE_USED;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_IRQL_REJECTED;
        response->mapStatus = STATUS_INVALID_DEVICE_STATE;
        response->copyStatus = STATUS_INVALID_DEVICE_STATE;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    if (!KswordARKPhysicalIsRangeValid(Request->physicalAddress, bytesToWrite)) {
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_RANGE_REJECTED;
        response->mapStatus = STATUS_INVALID_PARAMETER;
        response->copyStatus = STATUS_INVALID_PARAMETER;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    pageOffset = Request->physicalAddress & ((ULONG64)PAGE_SIZE - 1ULL);
    if ((ULONG64)bytesToWrite > ((ULONG64)MAXSIZE_T - pageOffset)) {
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_RANGE_REJECTED;
        response->mapStatus = STATUS_INTEGER_OVERFLOW;
        response->copyStatus = STATUS_INTEGER_OVERFLOW;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    mappedLength = (SIZE_T)(pageOffset + (ULONG64)bytesToWrite);

    RtlZeroMemory(&physicalAddress, sizeof(physicalAddress));
    physicalAddress.QuadPart =
        (LONGLONG)(Request->physicalAddress & ~((ULONG64)PAGE_SIZE - 1ULL));

    mappedAddress = MmMapIoSpaceEx(
        physicalAddress,
        mappedLength,
        PAGE_READWRITE | PAGE_NOCACHE);
    if (mappedAddress == NULL) {
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_MAP_FAILED;
        response->mapStatus = STATUS_INSUFFICIENT_RESOURCES;
        response->copyStatus = STATUS_INSUFFICIENT_RESOURCES;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    response->mapStatus = STATUS_SUCCESS;
    writeAddress = (PVOID)((PUCHAR)mappedAddress + pageOffset);

    __try {
        RtlCopyMemory(writeAddress, Request->data, bytesToWrite);
        response->bytesWritten = Request->bytesToWrite;
        response->copyStatus = STATUS_SUCCESS;
        response->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT;
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        response->bytesWritten = 0UL;
        response->copyStatus = status;
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_COPY_FAILED;
    }

    MmUnmapIoSpace(mappedAddress, mappedLength);
    mappedAddress = NULL;

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
