/*++

Module Name:

    debug_output_ioctl.c

Abstract:

    IOCTL adapters for kernel debug-output capture control and draining.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

// 处理捕获启停与状态查询，请求和响应均由 METHOD_BUFFERED 系统缓冲区承载。
NTSTATUS KswordARKDebugOutputIoctlControl(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned)
{
    KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST* input;
    KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST requestSnapshot;
    KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE* output;
    NTSTATUS status;

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    // 先按共享协议的固定头尺寸取缓冲区，防止短请求进入业务层。
    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST),
        (PVOID*)&input,
        NULL);
    if (!NT_SUCCESS(status) || InputBufferLength < sizeof(*input)) {
        return NT_SUCCESS(status) ? STATUS_BUFFER_TOO_SMALL : status;
    }
    // METHOD_BUFFERED 的输入输出共享同一系统缓冲区，写响应前必须保存完整请求。
    requestSnapshot = *input;
    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE),
        (PVOID*)&output,
        NULL);
    if (!NT_SUCCESS(status) || OutputBufferLength < sizeof(*output)) {
        return NT_SUCCESS(status) ? STATUS_BUFFER_TOO_SMALL : status;
    }

    // 业务层会同时验证协议版本，并返回当前注册与丢弃状态。
    status = KswordARKDebugOutputControl(Device, &requestSnapshot, output);
    *BytesReturned = sizeof(*output);
    return status;
}

// 按游标批量读取内核环形缓冲区，实际返回长度由业务层精确计算。
NTSTATUS KswordARKDebugOutputIoctlDrain(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned)
{
    KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST* input;
    KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST requestSnapshot;
    KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE* output;
    const size_t responseHeaderSize = FIELD_OFFSET(KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE, records);
    NTSTATUS status;

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    // 输入至少包含版本、尺寸、游标和本次期望条数。
    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST),
        (PVOID*)&input,
        NULL);
    if (!NT_SUCCESS(status) || InputBufferLength < sizeof(*input)) {
        return NT_SUCCESS(status) ? STATUS_BUFFER_TOO_SMALL : status;
    }
    // Drain 会清零变长输出区，因此必须先复制 afterSequence/maxRecords 等输入字段。
    requestSnapshot = *input;
    status = WdfRequestRetrieveOutputBuffer(
        Request,
        responseHeaderSize,
        (PVOID*)&output,
        NULL);
    if (!NT_SUCCESS(status) || OutputBufferLength < responseHeaderSize) {
        return NT_SUCCESS(status) ? STATUS_BUFFER_TOO_SMALL : status;
    }

    // 高频轮询不在这里输出任何调试日志，避免捕获自身形成反馈循环。
    return KswordARKDebugOutputDrain(
        Device,
        &requestSnapshot,
        output,
        OutputBufferLength,
        BytesReturned);
}
