/*++

Module Name:

    kernel_ioctl.c

Abstract:

    IOCTL handlers for KswordARK kernel inspection operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY))

#define KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE) - sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY))

#define KSWORD_ARK_INLINE_HOOK_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY))

#define KSWORD_ARK_IAT_EAT_HOOK_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY))

static VOID
KswordARKKernelIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one kernel-handler log message.

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string.
    FormatText - printf-style ANSI message template.
    ... - Template arguments.

Return Value:

    None. Formatting or enqueue failures are ignored.

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (void)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

NTSTATUS
KswordARKKernelIoctlEnumSsdt(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ENUM_SSDT. Optional input preserves the legacy
    default request, and the feature function owns SSDT enumeration details.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Supplied input bytes; shorter input selects defaults.
    OutputBufferLength - Supplied output bytes; checked by WDF output retrieval.
    BytesReturned - Receives the feature-written response byte count.

Return Value:

    NTSTATUS from buffer retrieval or KswordARKDriverEnumerateSsdt.

--*/
{
    KSWORD_ARK_ENUM_SSDT_REQUEST* enumRequest = NULL;
    KSWORD_ARK_ENUM_SSDT_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveOptionalInputBuffer(Request, InputBufferLength, sizeof(KSWORD_ARK_ENUM_SSDT_REQUEST), &inputBuffer, &actualInputLength, &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-ssdt ioctl: input buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (hasInput) {
        enumRequest = (KSWORD_ARK_ENUM_SSDT_REQUEST*)inputBuffer;
    }
    else {
        enumRequest = &defaultRequest;
        enumRequest->flags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED;
        enumRequest->reserved = 0UL;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(Request, KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE, &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-ssdt ioctl: output buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateSsdt(outputBuffer, actualOutputLength, enumRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-ssdt failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_SSDT_RESPONSE* responseHeader = (KSWORD_ARK_ENUM_SSDT_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(Device, "Info", "R0 enum-ssdt success: total=%lu, returned=%lu, outBytes=%Iu.", (unsigned long)responseHeader->totalCount, (unsigned long)responseHeader->returnedCount, *BytesReturned);
    }
    else {
        KswordARKKernelIoctlLog(Device, "Warn", "R0 enum-ssdt success: outBytes=%Iu (header partial).", *BytesReturned);
    }

    return status;
}

NTSTATUS
KswordARKKernelIoctlQueryDriverObject(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT. The UI supplies only a driver
    object namespace name; kernel mode references the DriverObject itself and
    returns diagnostic-only addresses, dispatch table rows and device chains.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Supplied input bytes.
    OutputBufferLength - Supplied output bytes.
    BytesReturned - Receives the feature-written response byte count.

Return Value:

    NTSTATUS from buffer retrieval or KswordARKDriverQueryDriverObject.

--*/
{
    KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
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
        sizeof(KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST),
        (PVOID*)&queryRequest,
        NULL);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 query-driver-object ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 query-driver-object ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryDriverObject(
        outputBuffer,
        actualOutputLength,
        queryRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 query-driver-object failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE* responseHeader =
            (KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(
            Device,
            "Info",
            "R0 query-driver-object success: status=%lu, devices=%lu/%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->queryStatus,
            (unsigned long)responseHeader->returnedDeviceCount,
            (unsigned long)responseHeader->totalDeviceCount,
            *BytesReturned);
    }

    return status;
}

NTSTATUS
KswordARKKernelIoctlEnumShadowSsdt(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT。中文说明：handler 只负责缓冲
    获取与默认请求填充，SSSDT 解析策略由 ssdt_query.c 承担。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；缺省时使用 include-unresolved。
    OutputBufferLength - 输出长度；由 WDF 再确认。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from validation or feature backend.

--*/
{
    KSWORD_ARK_ENUM_SSDT_REQUEST* enumRequest = NULL;
    KSWORD_ARK_ENUM_SSDT_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_ENUM_SSDT_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-shadow-ssdt ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (hasInput) {
        enumRequest = (KSWORD_ARK_ENUM_SSDT_REQUEST*)inputBuffer;
    }
    else {
        enumRequest = &defaultRequest;
        enumRequest->flags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-shadow-ssdt ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateShadowSsdt(
        outputBuffer,
        actualOutputLength,
        enumRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-shadow-ssdt failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_SSDT_RESPONSE* responseHeader =
            (KSWORD_ARK_ENUM_SSDT_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(
            Device,
            "Info",
            "R0 enum-shadow-ssdt success: total=%lu, returned=%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->totalCount,
            (unsigned long)responseHeader->returnedCount,
            *BytesReturned);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKKernelIoctlScanInlineHooks(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS。中文说明：扫描为只读诊断，
    handler 支持可选请求；缺省时只返回可疑外跳行。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；缺省时使用默认扫描参数。
    OutputBufferLength - 输出长度；由 WDF 再确认。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from validation or feature backend.

--*/
{
    KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST* scanRequest = NULL;
    KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 scan-inline-hooks ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    scanRequest = hasInput ?
        (KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST*)inputBuffer :
        &defaultRequest;

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_INLINE_HOOK_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 scan-inline-hooks ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverScanInlineHooks(
        outputBuffer,
        actualOutputLength,
        scanRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 scan-inline-hooks failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_INLINE_HOOK_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE* responseHeader =
            (KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(
            Device,
            "Info",
            "R0 scan-inline-hooks success: total=%lu, returned=%lu, modules=%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->totalCount,
            (unsigned long)responseHeader->returnedCount,
            (unsigned long)responseHeader->moduleCount,
            *BytesReturned);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKKernelIoctlPatchInlineHook(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK。中文说明：普通请求返回
    FORCE_REQUIRED；强制请求必须带写访问并通过 safety policy。

Arguments:

    Device - WDF 设备对象，用于日志和 safety policy。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；必须包含固定修复请求。
    OutputBufferLength - 输出长度；必须容纳固定响应。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from validation, safety policy or feature backend.

--*/
{
    KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST* patchRequest = NULL;
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

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Warn", "R0 patch-inline-hook denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST),
        (PVOID*)&patchRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 patch-inline-hook ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 patch-inline-hook ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if ((patchRequest->flags & KSWORD_ARK_KERNEL_PATCH_FLAG_FORCE) != 0UL) {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;

        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKKernelIoctlLog(Device, "Warn", "R0 patch-inline-hook denied by safety policy: target=0x%I64X, status=0x%08X.", patchRequest->functionAddress, (unsigned int)status);
            return status;
        }
    }

    status = KswordARKDriverPatchInlineHook(
        outputBuffer,
        actualOutputLength,
        patchRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 patch-inline-hook failed: target=0x%I64X, status=0x%08X.", patchRequest->functionAddress, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE)) {
        KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE* response =
            (KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(
            Device,
            response->status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED ? "Info" : "Warn",
            "R0 patch-inline-hook response: target=0x%I64X, status=%lu, bytes=%lu, last=0x%08X.",
            response->functionAddress,
            (unsigned long)response->status,
            (unsigned long)response->bytesPatched,
            (unsigned int)response->lastStatus);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKKernelIoctlEnumIatEatHooks(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS。中文说明：枚举为只读诊断，
    支持模块名过滤、仅导入/仅导出以及包含干净项。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；缺省时扫描 IAT 和 EAT 可疑项。
    OutputBufferLength - 输出长度；由 WDF 再确认。
    BytesReturned - 返回写入字节数。

Return Value:

    NTSTATUS from validation or feature backend.

--*/
{
    KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST* scanRequest = NULL;
    KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    defaultRequest.flags =
        KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS |
        KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-iat-eat ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    scanRequest = hasInput ?
        (KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST*)inputBuffer :
        &defaultRequest;

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_IAT_EAT_HOOK_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-iat-eat ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateIatEatHooks(
        outputBuffer,
        actualOutputLength,
        scanRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-iat-eat failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_IAT_EAT_HOOK_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE* responseHeader =
            (KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(
            Device,
            "Info",
            "R0 enum-iat-eat success: total=%lu, returned=%lu, modules=%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->totalCount,
            (unsigned long)responseHeader->returnedCount,
            (unsigned long)responseHeader->moduleCount,
            *BytesReturned);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKKernelIoctlForceUnloadDriver(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER。中文说明：handler 只负责写访问、
    fixed buffer 校验和 safety policy，真实卸载逻辑在 driver_unload.c。

Arguments:

    Device - WDF 设备对象，用于日志和 safety policy。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；必须包含固定请求。
    OutputBufferLength - 输出长度；必须容纳固定响应。
    BytesReturned - 返回响应字节数。

Return Value:

    NTSTATUS from validation, safety policy or feature backend.

--*/
{
    KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* unloadRequest = NULL;
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

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Warn", "R0 force-unload-driver denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST),
        (PVOID*)&unloadRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 force-unload-driver ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 force-unload-driver ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_DRIVER_UNLOAD;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        safetyContext.TargetText = unloadRequest->driverName;
        safetyContext.TargetTextChars = KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKKernelIoctlLog(Device, "Warn", "R0 force-unload-driver denied by safety policy: status=0x%08X.", (unsigned int)status);
            return status;
        }
    }

    status = KswordARKDriverForceUnloadDriver(
        outputBuffer,
        actualOutputLength,
        unloadRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 force-unload-driver backend failed: status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE)) {
        KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE* response =
            (KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(
            Device,
            response->status == KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED ? "Info" : "Warn",
            "R0 force-unload-driver response: status=%lu, object=0x%I64X, unload=0x%I64X, last=0x%08X, wait=0x%08X.",
            (unsigned long)response->status,
            response->driverObjectAddress,
            response->driverUnloadAddress,
            (unsigned int)response->lastStatus,
            (unsigned int)response->waitStatus);
    }

    return STATUS_SUCCESS;
}
