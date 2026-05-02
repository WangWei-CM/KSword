/*++

Module Name:

    registry_ioctl.c

Abstract:

    IOCTL handlers for KswordARK registry read operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKRegistryIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    格式化并写入注册表 IOCTL 日志。中文说明：日志失败不影响主 IOCTL，
    因为读取结果已经通过结构化响应返回给 R3。

Arguments:

    Device - WDF 设备对象。
    LevelText - 日志级别。
    FormatText - printf 风格 ANSI 模板。
    ... - 模板参数。

Return Value:

    None. 本函数没有返回值。

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(
        logMessage,
        sizeof(logMessage),
        FormatText,
        arguments))) {
        (void)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

NTSTATUS
KswordARKRegistryIoctlReadValue(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE。中文说明：handler 只负责
    WDF 缓冲校验，注册表读取由 registry_query.c 实现。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；必须包含固定请求。
    OutputBufferLength - 输出长度；必须容纳固定响应。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from validation or feature backend.

--*/
{
    KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST* readRequest = NULL;
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
        sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKRegistryIoctlLog(Device, "Error", "R0 registry read ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKRegistryIoctlLog(Device, "Error", "R0 registry read ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    readRequest = (KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST*)inputBuffer;
    status = KswordARKDriverReadRegistryValue(
        outputBuffer,
        actualOutputLength,
        readRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKRegistryIoctlLog(Device, "Error", "R0 registry read failed before response: status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE)) {
        KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE* response =
            (KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*)outputBuffer;
        KswordARKRegistryIoctlLog(
            Device,
            response->status == KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS ? "Info" : "Warn",
            "R0 registry read response: status=%lu, type=%lu, data=%lu/%lu, last=0x%08X.",
            (unsigned long)response->status,
            (unsigned long)response->valueType,
            (unsigned long)response->dataBytes,
            (unsigned long)response->requiredBytes,
            (unsigned int)response->lastStatus);
    }

    return STATUS_SUCCESS;
}
