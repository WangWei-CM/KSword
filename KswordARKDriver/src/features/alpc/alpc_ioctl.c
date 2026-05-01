/*++

Module Name:

    alpc_ioctl.c

Abstract:

    IOCTL handler for KswordARK ALPC inspection.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKAlpcIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    格式化并写入 ALPC IOCTL 诊断日志。中文说明：日志失败不影响 IOCTL
    主路径，只保留 pid/handle/status 这类排障关键字段。

Arguments:

    Device - WDF 设备对象。
    LevelText - 日志级别文本。
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
KswordARKAlpcIoctlQueryAlpcPort(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_ALPC_PORT。中文说明：handler 只负责 WDF
    缓冲校验和日志，真正的 PID+Handle 对象引用与 ALPC 字段读取在 feature
    query 层完成。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 WDF 请求。
    InputBufferLength - 输入缓冲长度。
    OutputBufferLength - 输出缓冲长度。
    BytesReturned - 接收返回字节数。

Return Value:

    NTSTATUS 表示 WDF 缓冲校验或查询执行结果。

--*/
{
    KSWORD_ARK_QUERY_ALPC_PORT_REQUEST* queryRequest = NULL;
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
        sizeof(KSWORD_ARK_QUERY_ALPC_PORT_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKAlpcIoctlLog(Device, "Error", "R0 query-alpc ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_ALPC_PORT_REQUEST*)inputBuffer;
    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKAlpcIoctlLog(Device, "Error", "R0 query-alpc ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryAlpcPort(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKAlpcIoctlLog(
            Device,
            "Error",
            "R0 query-alpc failed: pid=%lu, handle=0x%I64X, status=0x%08X.",
            (unsigned long)queryRequest->processId,
            queryRequest->handleValue,
            (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE)) {
        KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE* response = (KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE*)outputBuffer;
        KswordARKAlpcIoctlLog(
            Device,
            "Info",
            "R0 query-alpc success: pid=%lu, handle=0x%I64X, queryStatus=%lu, fieldFlags=0x%08lX.",
            (unsigned long)response->processId,
            response->handleValue,
            (unsigned long)response->queryStatus,
            (unsigned long)response->fieldFlags);
    }

    return STATUS_SUCCESS;
}
