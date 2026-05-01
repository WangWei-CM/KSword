/*++

Module Name:

    process_memory_ioctl.c

Abstract:

    IOCTL handlers for Phase-11 read-only process memory operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSWORD_ARK_MEMORY_READ_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE) - sizeof(((KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE*)0)->data))

static VOID
KswordARKMemoryIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    写入 Phase-11 内存 IOCTL 诊断日志。中文说明：这里只记录 PID、地址、
    状态和字节数，不记录读取到的数据，避免日志泄露目标进程内容。

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - 日志等级文本。
    FormatText - printf 风格 ANSI 格式串。
    ... - 格式参数。

Return Value:

    None. 日志失败不影响 IOCTL 完成。

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
KswordARKMemoryIoctlQueryVirtualMemory(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY。中文说明：handler 只负责
    WDF 缓冲获取、PID/flags/地址基础校验，真正的 ZwQueryVirtualMemory 查询
    由 process_memory.c 完成。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation or feature backend.

--*/
{
    KSWORD_ARK_QUERY_VIRTUAL_MEMORY_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    const ULONG allowedFlags = KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_MAPPED_FILE_NAME;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_VIRTUAL_MEMORY_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Error", "R0 query-vm ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_VIRTUAL_MEMORY_REQUEST*)inputBuffer;
    if ((queryRequest->flags & ~allowedFlags) != 0UL) {
        KswordARKMemoryIoctlLog(Device, "Warn", "R0 query-vm ioctl: flags rejected, flags=0x%08X.", (unsigned int)queryRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    status = KswordARKValidateUserPid(queryRequest->processId);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Warn", "R0 query-vm ioctl: pid rejected, pid=%lu.", (unsigned long)queryRequest->processId);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_VIRTUAL_MEMORY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Error", "R0 query-vm ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryVirtualMemory(
        outputBuffer,
        actualOutputLength,
        queryRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Error", "R0 query-vm failed: pid=%lu, address=0x%I64X, status=0x%08X.", (unsigned long)queryRequest->processId, queryRequest->baseAddress, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_VIRTUAL_MEMORY_RESPONSE)) {
        KSWORD_ARK_QUERY_VIRTUAL_MEMORY_RESPONSE* response = (KSWORD_ARK_QUERY_VIRTUAL_MEMORY_RESPONSE*)outputBuffer;
        KswordARKMemoryIoctlLog(
            Device,
            "Info",
            "R0 query-vm success: pid=%lu, address=0x%I64X, status=%lu, fields=0x%08X.",
            (unsigned long)response->processId,
            response->requestedBaseAddress,
            (unsigned long)response->queryStatus,
            (unsigned int)response->fieldFlags);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMemoryIoctlReadVirtualMemory(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY。中文说明：第一版只读，不提供写、
    分配或改保护；读取失败以 response->readStatus/copyStatus 呈现给 R3。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation or feature backend.

--*/
{
    KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST* readRequest = NULL;
    PVOID inputBuffer = NULL;
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
        sizeof(KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Error", "R0 read-vm ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    readRequest = (KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST*)inputBuffer;
    if (readRequest->flags != 0UL) {
        KswordARKMemoryIoctlLog(Device, "Warn", "R0 read-vm ioctl: flags rejected, flags=0x%08X.", (unsigned int)readRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    status = KswordARKValidateUserPid(readRequest->processId);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Warn", "R0 read-vm ioctl: pid rejected, pid=%lu.", (unsigned long)readRequest->processId);
        return status;
    }
    if (readRequest->bytesToRead > KSWORD_ARK_MEMORY_READ_MAX_BYTES) {
        KswordARKMemoryIoctlLog(Device, "Warn", "R0 read-vm ioctl: size rejected, pid=%lu, bytes=%lu.", (unsigned long)readRequest->processId, (unsigned long)readRequest->bytesToRead);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_MEMORY_READ_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Error", "R0 read-vm ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverReadVirtualMemory(
        outputBuffer,
        actualOutputLength,
        readRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMemoryIoctlLog(Device, "Error", "R0 read-vm failed: pid=%lu, address=0x%I64X, bytes=%lu, status=0x%08X.", (unsigned long)readRequest->processId, readRequest->baseAddress, (unsigned long)readRequest->bytesToRead, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_MEMORY_READ_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE* response = (KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE*)outputBuffer;
        KswordARKMemoryIoctlLog(
            Device,
            "Info",
            "R0 read-vm success: pid=%lu, address=0x%I64X, status=%lu, requested=%lu, read=%lu.",
            (unsigned long)response->processId,
            response->requestedBaseAddress,
            (unsigned long)response->readStatus,
            (unsigned long)response->requestedBytes,
            (unsigned long)response->bytesRead);
    }

    return STATUS_SUCCESS;
}
