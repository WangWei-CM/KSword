/*++

Module Name:

    memory_ioctl.c

Abstract:

    IOCTL handlers for physical memory and page-table inspection operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#ifndef STATUS_REQUEST_NOT_ACCEPTED
#define STATUS_REQUEST_NOT_ACCEPTED ((NTSTATUS)0xC00000D0L)
#endif

// 物理读取响应头不包含尾随 data[1]，handler 只要求 R3 至少提供头部空间。
#define KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE) - sizeof(((KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE*)0)->data))

// 物理写入请求头不包含尾随 data[1]，handler 用它计算完整输入长度。
#define KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST) - sizeof(((KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST*)0)->data))

static VOID
KswordARKMemoryToolIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    写入物理内存/PTE 工具 IOCTL 诊断日志。中文说明：日志只记录地址、长度、
    状态和 PID，不记录物理内存内容，避免把敏感字节写入环形日志。

Arguments:

    Device - WDF 设备对象，用于投递日志。
    LevelText - 日志等级文本。
    FormatText - printf 风格 ANSI 格式串。
    ... - 格式参数。

Return Value:

    None. 日志失败不影响 IOCTL 请求完成。

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
KswordARKMemoryIoctlReadPhysicalMemory(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_READ_PHYSICAL_MEMORY。中文说明：handler 负责 WDF
    缓冲获取、flags/长度初筛和读访问校验，实际 MmCopyMemory 读取在 backend。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation or KswordARKDriverReadPhysicalMemory.

--*/
{
    KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST* readRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST),
        (PVOID*)&readRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 read-physical ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (readRequest->flags != 0UL ||
        readRequest->reserved != 0UL ||
        readRequest->reserved2 != 0UL) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 read-physical ioctl: flags/reserved rejected, flags=0x%08X.", (unsigned int)readRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    if (readRequest->bytesToRead > KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 read-physical ioctl: size rejected, pa=0x%I64X, bytes=%lu.", readRequest->physicalAddress, (unsigned long)readRequest->bytesToRead);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 read-physical ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverReadPhysicalMemory(
        outputBuffer,
        actualOutputLength,
        readRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 read-physical failed: pa=0x%I64X, bytes=%lu, status=0x%08X.", readRequest->physicalAddress, (unsigned long)readRequest->bytesToRead, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_PHYSICAL_READ_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE* response =
            (KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE*)outputBuffer;
        KswordARKMemoryToolIoctlLog(
            Device,
            "Info",
            "R0 read-physical response: pa=0x%I64X, status=%lu, requested=%lu, read=%lu.",
            response->requestedPhysicalAddress,
            (unsigned long)response->readStatus,
            (unsigned long)response->requestedBytes,
            (unsigned long)response->bytesRead);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMemoryIoctlWritePhysicalMemory(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_WRITE_PHYSICAL_MEMORY。中文说明：写入物理内存要求
    FILE_WRITE_ACCESS、FORCE 确认和 safety policy 允许，backend 再执行映射写入。

Arguments:

    Device - WDF 设备对象，用于日志和 safety policy。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation, safety policy or KswordARKDriverWritePhysicalMemory.

--*/
{
    KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST* writeRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    size_t requiredInputLength = 0U;
    const ULONG allowedFlags =
        KSWORD_ARK_PHYSICAL_WRITE_FLAG_UI_CONFIRMED |
        KSWORD_ARK_PHYSICAL_WRITE_FLAG_FORCE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 write-physical denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE,
        (PVOID*)&writeRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 write-physical ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if ((writeRequest->flags & ~allowedFlags) != 0UL ||
        writeRequest->reserved != 0UL ||
        writeRequest->reserved2 != 0UL) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 write-physical ioctl: flags/reserved rejected, flags=0x%08X.", (unsigned int)writeRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    if (writeRequest->bytesToWrite == 0UL ||
        writeRequest->bytesToWrite > KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 write-physical ioctl: size rejected, pa=0x%I64X, bytes=%lu.", writeRequest->physicalAddress, (unsigned long)writeRequest->bytesToWrite);
        return STATUS_INVALID_PARAMETER;
    }
    if ((SIZE_T)writeRequest->bytesToWrite >
        (MAXSIZE_T - KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE)) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 write-physical ioctl: size overflow rejected, bytes=%lu.", (unsigned long)writeRequest->bytesToWrite);
        return STATUS_INVALID_PARAMETER;
    }

    requiredInputLength =
        KSWORD_ARK_PHYSICAL_WRITE_REQUEST_HEADER_SIZE +
        (SIZE_T)writeRequest->bytesToWrite;
    if (actualInputLength < requiredInputLength) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 write-physical ioctl: input truncated, actual=%Iu, required=%Iu.", actualInputLength, requiredInputLength);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 write-physical ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if ((writeRequest->flags & KSWORD_ARK_PHYSICAL_WRITE_FLAG_FORCE) == 0UL) {
        KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE* response =
            (KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE*)outputBuffer;
        RtlZeroMemory(outputBuffer, actualOutputLength);
        response->version = KSWORD_ARK_MEMORY_PROTOCOL_VERSION;
        response->size = sizeof(*response);
        response->fieldFlags =
            KSWORD_ARK_MEMORY_FIELD_WRITE_DATA_PRESENT |
            KSWORD_ARK_MEMORY_FIELD_FORCE_WRITE_REQUIRED;
        response->writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_FORCE_REQUIRED;
        response->mapStatus = STATUS_REQUEST_NOT_ACCEPTED;
        response->copyStatus = STATUS_REQUEST_NOT_ACCEPTED;
        response->source = KSWORD_ARK_MEMORY_SOURCE_R0_MM_MAP_PHYSICAL_MEMORY;
        response->requestedBytes = writeRequest->bytesToWrite;
        response->maxBytesPerRequest = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES;
        response->requestedPhysicalAddress = writeRequest->physicalAddress;
        *BytesReturned = sizeof(*response);
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 write-physical requires force confirmation: pa=0x%I64X, bytes=%lu.", writeRequest->physicalAddress, (unsigned long)writeRequest->bytesToWrite);
        return STATUS_SUCCESS;
    }

    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_MEMORY_WRITE;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        safetyContext.TargetProcessId = 0UL;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 write-physical denied by safety policy: pa=0x%I64X, bytes=%lu, status=0x%08X.", writeRequest->physicalAddress, (unsigned long)writeRequest->bytesToWrite, (unsigned int)status);
            return status;
        }
    }

    status = KswordARKDriverWritePhysicalMemory(
        outputBuffer,
        actualOutputLength,
        writeRequest,
        actualInputLength,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 write-physical failed: pa=0x%I64X, bytes=%lu, status=0x%08X.", writeRequest->physicalAddress, (unsigned long)writeRequest->bytesToWrite, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE)) {
        KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE* response =
            (KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE*)outputBuffer;
        KswordARKMemoryToolIoctlLog(
            Device,
            "Info",
            "R0 write-physical response: pa=0x%I64X, status=%lu, requested=%lu, written=%lu.",
            response->requestedPhysicalAddress,
            (unsigned long)response->writeStatus,
            (unsigned long)response->requestedBytes,
            (unsigned long)response->bytesWritten);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMemoryIoctlTranslateVirtualAddress(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS。中文说明：handler 仅做
    缓冲和 flags 校验，实际 CR3/页表只读遍历由 backend 完成。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation or KswordARKDriverTranslateVirtualAddress.

--*/
{
    KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST* translateRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST),
        (PVOID*)&translateRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 translate-va ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (translateRequest->flags != 0UL || translateRequest->reserved != 0UL) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 translate-va ioctl: flags/reserved rejected, flags=0x%08X.", (unsigned int)translateRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 translate-va ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverTranslateVirtualAddress(
        outputBuffer,
        actualOutputLength,
        translateRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 translate-va failed: pid=%lu, va=0x%I64X, status=0x%08X.", (unsigned long)translateRequest->processId, translateRequest->virtualAddress, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE)) {
        KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE* response =
            (KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE*)outputBuffer;
        KswordARKMemoryToolIoctlLog(
            Device,
            "Info",
            "R0 translate-va response: pid=%lu, va=0x%I64X, resolved=%lu, status=%lu, pa=0x%I64X.",
            (unsigned long)response->info.processId,
            response->info.virtualAddress,
            (unsigned long)response->info.resolved,
            (unsigned long)response->info.queryStatus,
            response->info.physicalAddress);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMemoryIoctlQueryPageTableEntry(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY。中文说明：返回 PML4E/PDPTE/
    PDE/PTE 原始值、flags、索引、page size 和大页类型，默认不提供写页表能力。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation or KswordARKDriverQueryPageTableEntry.

--*/
{
    KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST),
        (PVOID*)&queryRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 query-pte ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (queryRequest->flags != 0UL || queryRequest->reserved != 0UL) {
        KswordARKMemoryToolIoctlLog(Device, "Warn", "R0 query-pte ioctl: flags/reserved rejected, flags=0x%08X.", (unsigned int)queryRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 query-pte ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryPageTableEntry(
        outputBuffer,
        actualOutputLength,
        queryRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryToolIoctlLog(Device, "Error", "R0 query-pte failed: pid=%lu, va=0x%I64X, status=0x%08X.", (unsigned long)queryRequest->processId, queryRequest->virtualAddress, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE)) {
        KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE* response =
            (KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE*)outputBuffer;
        KswordARKMemoryToolIoctlLog(
            Device,
            "Info",
            "R0 query-pte response: pid=%lu, va=0x%I64X, resolved=%lu, status=%lu, pageSize=%lu.",
            (unsigned long)response->info.processId,
            response->info.virtualAddress,
            (unsigned long)response->info.resolved,
            (unsigned long)response->info.queryStatus,
            (unsigned long)response->info.pageSize);
    }

    return STATUS_SUCCESS;
}
