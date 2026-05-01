/*++

Module Name:

    wsl_silo_ioctl.c

Abstract:

    IOCTL handler for Phase-13 WSL/Pico and Silo diagnostics.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKWslSiloIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    输出 WSL/Silo 查询日志。中文说明：日志只记录 PID/TID 和状态，不记录内部
    指针或 Linux 进程细节，避免泄露过多内核诊断信息。

Arguments:

    Device - WDF 设备对象。
    LevelText - 日志等级。
    FormatText - printf 风格格式串。
    ... - 格式参数。

Return Value:

    None.

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
KswordARKWslSiloIoctlQuery(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_WSL_SILO。中文说明：请求包必须提供，flags 为
    0 时查询全部基础信息；真正字段读取由 wsl_silo_query.c 完成。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 接收写入长度。

Return Value:

    NTSTATUS from validation or backend.

--*/
{
    KSWORD_ARK_QUERY_WSL_SILO_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    const ULONG allowedFlags = KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_ALL;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_WSL_SILO_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKWslSiloIoctlLog(Device, "Error", "R0 query-wsl-silo: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_WSL_SILO_REQUEST*)inputBuffer;
    if ((queryRequest->flags & ~allowedFlags) != 0UL) {
        KswordARKWslSiloIoctlLog(Device, "Warn", "R0 query-wsl-silo: flags rejected, flags=0x%08X.", (unsigned int)queryRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_WSL_SILO_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKWslSiloIoctlLog(Device, "Error", "R0 query-wsl-silo: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryWslSilo(
        outputBuffer,
        actualOutputLength,
        queryRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKWslSiloIoctlLog(Device, "Error", "R0 query-wsl-silo failed: pid=%lu, tid=%lu, status=0x%08X.", (unsigned long)queryRequest->processId, (unsigned long)queryRequest->threadId, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_WSL_SILO_RESPONSE)) {
        KSWORD_ARK_QUERY_WSL_SILO_RESPONSE* response = (KSWORD_ARK_QUERY_WSL_SILO_RESPONSE*)outputBuffer;
        KswordARKWslSiloIoctlLog(
            Device,
            "Info",
            "R0 query-wsl-silo success: pid=%lu, tid=%lu, status=%lu, fields=0x%08X.",
            (unsigned long)response->processId,
            (unsigned long)response->threadId,
            (unsigned long)response->queryStatus,
            (unsigned int)response->fieldFlags);
    }

    return STATUS_SUCCESS;
}
