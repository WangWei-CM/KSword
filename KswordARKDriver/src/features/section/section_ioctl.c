/*++

Module Name:

    section_ioctl.c

Abstract:

    IOCTL handler for process SectionObject / ControlArea inspection.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE) - sizeof(KSWORD_ARK_SECTION_MAPPING_ENTRY))

#define KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE) - sizeof(KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY))

static VOID
KswordARKSectionIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    格式化 Section IOCTL 日志。中文说明：日志只做诊断，不改变 IOCTL
    完成状态。

Arguments:

    Device - WDF 设备对象。
    LevelText - 日志级别。
    FormatText - printf 风格格式字符串。
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
KswordARKSectionIoctlQueryProcessSection(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION。中文说明：handler 只负责
    WDF 缓冲获取和日志，实际 PID 查询与 ControlArea 遍历在 feature 层完成。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 WDF 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 接收返回字节数。

Return Value:

    NTSTATUS 表示缓冲校验或查询执行结果。

--*/
{
    KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKSectionIoctlLog(Device, "Error", "R0 query-section ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST*)inputBuffer;
    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKSectionIoctlLog(Device, "Error", "R0 query-section ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryProcessSection(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKSectionIoctlLog(
            Device,
            "Error",
            "R0 query-section failed: pid=%lu, status=0x%08X.",
            (unsigned long)queryRequest->processId,
            (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE* response = (KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE*)outputBuffer;
        KswordARKSectionIoctlLog(
            Device,
            "Info",
            "R0 query-section success: pid=%lu, status=%lu, total=%lu, returned=%lu.",
            (unsigned long)response->processId,
            (unsigned long)response->queryStatus,
            (unsigned long)response->totalCount,
            (unsigned long)response->returnedCount);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKSectionIoctlQueryFileSectionMappings(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS。中文说明：handler 只做
    请求包和路径长度校验，文件对象打开和 ControlArea 遍历交给 feature 层。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 WDF 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 接收返回字节数。

Return Value:

    NTSTATUS 表示缓冲校验或查询执行结果。

--*/
{
    KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKSectionIoctlLog(Device, "Error", "R0 query-file-section ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST*)inputBuffer;
    if (queryRequest->pathLengthChars == 0U ||
        queryRequest->pathLengthChars >= KSWORD_ARK_FILE_SECTION_PATH_MAX_CHARS ||
        queryRequest->path[queryRequest->pathLengthChars] != L'\0') {
        KswordARKSectionIoctlLog(Device, "Warn", "R0 query-file-section ioctl: path rejected, chars=%u.", (unsigned int)queryRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKSectionIoctlLog(Device, "Error", "R0 query-file-section ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryFileSectionMappings(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKSectionIoctlLog(
            Device,
            "Error",
            "R0 query-file-section failed: chars=%u, status=0x%08X.",
            (unsigned int)queryRequest->pathLengthChars,
            (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE* response = (KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE*)outputBuffer;
        KswordARKSectionIoctlLog(
            Device,
            "Info",
            "R0 query-file-section success: chars=%u, status=%lu, total=%lu, returned=%lu.",
            (unsigned int)queryRequest->pathLengthChars,
            (unsigned long)response->queryStatus,
            (unsigned long)response->totalCount,
            (unsigned long)response->returnedCount);
    }

    return STATUS_SUCCESS;
}
