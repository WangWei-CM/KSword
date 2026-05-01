/*++

Module Name:

    safety_ioctl.c

Abstract:

    IOCTL handlers for Phase-15 dangerous-operation policy.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKSafetyIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    输出 safety IOCTL 日志。中文说明：策略变更是安全敏感动作，统一记录状态码
    与 generation，方便 R3 日志面板展示。

Arguments:

    Device - WDF 设备对象。
    LevelText - 日志级别。
    FormatText - printf 风格格式串。
    ... - 格式参数。

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
KswordARKSafetyIoctlQueryPolicy(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_SAFETY_POLICY。中文说明：输入包可省略，输出
    总是固定响应结构，便于状态页轮询。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 写入字节数。

Return Value:

    NTSTATUS from buffer retrieval or policy query.

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
        sizeof(KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKSafetyIoctlLog(Device, "Error", "R0 query-safety-policy: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKSafetyQueryPolicy(outputBuffer, actualOutputLength, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKSafetyIoctlLog(Device, "Error", "R0 query-safety-policy failed, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE)) {
        KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE* response =
            (KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE*)outputBuffer;
        KswordARKSafetyIoctlLog(
            Device,
            "Info",
            "R0 query-safety-policy success: flags=0x%08X, generation=%lu.",
            (unsigned int)response->policyFlags,
            (unsigned long)response->policyGeneration);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKSafetyIoctlSetPolicy(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_SET_SAFETY_POLICY。中文说明：该 IOCTL 需要写访问，
    用于 R3 高级模式开关和确认策略切换。

Arguments:

    Device - WDF 设备对象。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度。
    OutputBufferLength - 输出长度。
    BytesReturned - 写入字节数。

Return Value:

    NTSTATUS from validation or policy update.

--*/
{
    KSWORD_ARK_SET_SAFETY_POLICY_REQUEST* setRequest = NULL;
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
        KswordARKSafetyIoctlLog(Device, "Warn", "R0 set-safety-policy denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_SAFETY_POLICY_REQUEST),
        (PVOID*)&setRequest,
        NULL);
    if (!NT_SUCCESS(status)) {
        KswordARKSafetyIoctlLog(Device, "Error", "R0 set-safety-policy: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKSafetyIoctlLog(Device, "Error", "R0 set-safety-policy: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKSafetySetPolicy(
        setRequest,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKSafetyIoctlLog(Device, "Error", "R0 set-safety-policy failed, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE)) {
        KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE* response =
            (KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE*)outputBuffer;
        KswordARKSafetyIoctlLog(
            Device,
            NT_SUCCESS(response->status) ? "Info" : "Warn",
            "R0 set-safety-policy complete: old=0x%08X, new=0x%08X, status=0x%08X.",
            (unsigned int)response->oldPolicyFlags,
            (unsigned int)response->newPolicyFlags,
            (unsigned int)response->status);
    }

    return STATUS_SUCCESS;
}
