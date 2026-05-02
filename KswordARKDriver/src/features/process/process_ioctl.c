/*++

Module Name:

    process_ioctl.c

Abstract:

    IOCTL handlers for KswordARK process operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#ifndef STATUS_PROCESS_IS_TERMINATING
#define STATUS_PROCESS_IS_TERMINATING ((NTSTATUS)0xC000010AL)
#endif

#define KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_PROCESS_RESPONSE) - sizeof(KSWORD_ARK_PROCESS_ENTRY))

static VOID
KswordARKProcessIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one process-handler log message.

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string such as Info/Warn/Error.
    FormatText - printf-style ANSI message template.
    ... - Template arguments.

Return Value:

    None. Formatting or enqueue failures are intentionally ignored.

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
KswordARKProcessIoctlTerminate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_TERMINATE_PROCESS. The handler validates the fixed
    request and forwards process termination to the existing process feature.

Arguments:

    Device - WDF device used for logging and feature context.
    Request - Current IOCTL request.
    InputBufferLength - Caller-supplied input length; used by WDF retrieval.
    OutputBufferLength - Caller-supplied output length; unused for this IOCTL.
    BytesReturned - Receives sizeof(request) on success and zero on failure.

Return Value:

    NTSTATUS from validation or KswordARKDriverTerminateProcessByPid.

--*/
{
    KSWORD_ARK_TERMINATE_PROCESS_REQUEST* terminateRequest = NULL;
    PVOID inputBuffer = NULL;
    size_t actualInputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_TERMINATE_PROCESS_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 terminate ioctl: input buffer invalid, status=0x%08X", (unsigned int)status);
        return status;
    }

    terminateRequest = (KSWORD_ARK_TERMINATE_PROCESS_REQUEST*)inputBuffer;
    status = KswordARKValidateUserPid((ULONG)terminateRequest->processId);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 terminate ioctl: pid=%lu rejected.", (unsigned long)terminateRequest->processId);
        return status;
    }
    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_TERMINATE;
        safetyContext.TargetProcessId = (ULONG)terminateRequest->processId;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKProcessIoctlLog(Device, "Warn", "R0 terminate denied by safety policy: pid=%lu, status=0x%08X.", (unsigned long)terminateRequest->processId, (unsigned int)status);
            return status;
        }
    }

    KswordARKProcessIoctlLog(Device, "Info", "R0 terminate ioctl: pid=%lu, exit=0x%08X.", (unsigned long)terminateRequest->processId, (unsigned int)terminateRequest->exitStatus);
    status = KswordARKDriverTerminateProcessByPid(Device, (ULONG)terminateRequest->processId, (NTSTATUS)terminateRequest->exitStatus);
    if (NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Info", "R0 terminate success: pid=%lu.", (unsigned long)terminateRequest->processId);
        *BytesReturned = sizeof(KSWORD_ARK_TERMINATE_PROCESS_REQUEST);
    }
    else if (status == STATUS_PROCESS_IS_TERMINATING) {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 terminate pending: pid=%lu, status=0x%08X (still terminating).", (unsigned long)terminateRequest->processId, (unsigned int)status);
    }
    else {
        KswordARKProcessIoctlLog(Device, "Error", "R0 terminate failed: pid=%lu, status=0x%08X.", (unsigned long)terminateRequest->processId, (unsigned int)status);
    }

    return status;
}

NTSTATUS
KswordARKProcessIoctlSuspend(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_SUSPEND_PROCESS with the same request and completion
    semantics as the previous dispatch switch case.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length; used by WDF retrieval.
    OutputBufferLength - Caller output length; unused for this IOCTL.
    BytesReturned - Receives sizeof(request) on success and zero on failure.

Return Value:

    NTSTATUS from validation or KswordARKDriverSuspendProcessByPid.

--*/
{
    KSWORD_ARK_SUSPEND_PROCESS_REQUEST* suspendRequest = NULL;
    PVOID inputBuffer = NULL;
    size_t actualInputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(KSWORD_ARK_SUSPEND_PROCESS_REQUEST), &inputBuffer, &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 suspend ioctl: input buffer invalid, status=0x%08X", (unsigned int)status);
        return status;
    }

    suspendRequest = (KSWORD_ARK_SUSPEND_PROCESS_REQUEST*)inputBuffer;
    status = KswordARKValidateUserPid((ULONG)suspendRequest->processId);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 suspend ioctl: pid=%lu rejected.", (unsigned long)suspendRequest->processId);
        return status;
    }
    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_SUSPEND;
        safetyContext.TargetProcessId = (ULONG)suspendRequest->processId;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKProcessIoctlLog(Device, "Warn", "R0 suspend denied by safety policy: pid=%lu, status=0x%08X.", (unsigned long)suspendRequest->processId, (unsigned int)status);
            return status;
        }
    }

    KswordARKProcessIoctlLog(Device, "Info", "R0 suspend ioctl: pid=%lu.", (unsigned long)suspendRequest->processId);
    status = KswordARKDriverSuspendProcessByPid((ULONG)suspendRequest->processId);
    if (NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Info", "R0 suspend success: pid=%lu.", (unsigned long)suspendRequest->processId);
        *BytesReturned = sizeof(KSWORD_ARK_SUSPEND_PROCESS_REQUEST);
    }
    else {
        KswordARKProcessIoctlLog(Device, "Error", "R0 suspend failed: pid=%lu, status=0x%08X.", (unsigned long)suspendRequest->processId, (unsigned int)status);
    }

    return status;
}

NTSTATUS
KswordARKProcessIoctlSetPplLevel(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_SET_PPL_LEVEL by forwarding the requested protection
    byte to the process feature after basic PID validation.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length; used by WDF retrieval.
    OutputBufferLength - Caller output length; unused for this IOCTL.
    BytesReturned - Receives sizeof(request) on success and zero on failure.

Return Value:

    NTSTATUS from validation or KswordARKDriverSetProcessPplLevelByPid.

--*/
{
    KSWORD_ARK_SET_PPL_LEVEL_REQUEST* setPplRequest = NULL;
    PVOID inputBuffer = NULL;
    size_t actualInputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(KSWORD_ARK_SET_PPL_LEVEL_REQUEST), &inputBuffer, &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 set PPL ioctl: input buffer invalid, status=0x%08X", (unsigned int)status);
        return status;
    }

    setPplRequest = (KSWORD_ARK_SET_PPL_LEVEL_REQUEST*)inputBuffer;
    status = KswordARKValidateUserPid((ULONG)setPplRequest->processId);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 set PPL ioctl: pid=%lu rejected.", (unsigned long)setPplRequest->processId);
        return status;
    }
    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_SET_PROTECTION;
        safetyContext.TargetProcessId = (ULONG)setPplRequest->processId;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKProcessIoctlLog(Device, "Warn", "R0 set PPL denied by safety policy: pid=%lu, status=0x%08X.", (unsigned long)setPplRequest->processId, (unsigned int)status);
            return status;
        }
    }

    KswordARKProcessIoctlLog(Device, "Info", "R0 set PPL ioctl: pid=%lu, level=0x%02X.", (unsigned long)setPplRequest->processId, (unsigned int)setPplRequest->protectionLevel);
    status = KswordARKDriverSetProcessPplLevelByPid((ULONG)setPplRequest->processId, (UCHAR)setPplRequest->protectionLevel);
    if (NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Info", "R0 set PPL success: pid=%lu, level=0x%02X.", (unsigned long)setPplRequest->processId, (unsigned int)setPplRequest->protectionLevel);
        *BytesReturned = sizeof(KSWORD_ARK_SET_PPL_LEVEL_REQUEST);
    }
    else {
        KswordARKProcessIoctlLog(Device, "Error", "R0 set PPL failed: pid=%lu, level=0x%02X, status=0x%08X.", (unsigned long)setPplRequest->processId, (unsigned int)setPplRequest->protectionLevel, (unsigned int)status);
    }

    return status;
}
NTSTATUS
KswordARKProcessIoctlEnumProcess(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ENUM_PROCESS. Optional input keeps the old default
    scan-CID-table behavior; output parsing and feature call remain unchanged.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Supplied input bytes; shorter input selects defaults.
    OutputBufferLength - Supplied output bytes; checked by WDF output retrieval.
    BytesReturned - Receives the feature-written response byte count.

Return Value:

    NTSTATUS from buffer retrieval or KswordARKDriverEnumerateProcesses.

--*/
{
    KSWORD_ARK_ENUM_PROCESS_REQUEST* enumRequest = NULL;
    KSWORD_ARK_ENUM_PROCESS_REQUEST defaultRequest = { 0 };
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

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_ENUM_PROCESS_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 enum-process ioctl: input buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (hasInput) {
        enumRequest = (KSWORD_ARK_ENUM_PROCESS_REQUEST*)inputBuffer;
    }
    else {
        enumRequest = &defaultRequest;
        enumRequest->flags = KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE;
        enumRequest->startPid = 0UL;
        enumRequest->endPid = 0UL;
        enumRequest->reserved = 0UL;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(Request, KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE, &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 enum-process ioctl: output buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateProcesses(outputBuffer, actualOutputLength, enumRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 enum-process failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_PROCESS_RESPONSE* responseHeader = (KSWORD_ARK_ENUM_PROCESS_RESPONSE*)outputBuffer;
        KswordARKProcessIoctlLog(
            Device,
            "Info",
            "R0 enum-process success: total=%lu, returned=%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->totalCount,
            (unsigned long)responseHeader->returnedCount,
            *BytesReturned);
    }
    else {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 enum-process success: outBytes=%Iu (header partial).", *BytesReturned);
    }

    return status;
}

NTSTATUS
KswordARKProcessIoctlSetVisibility(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY。中文说明：该 IOCTL 只更新
    驱动内可恢复隐藏标记，不执行 DKOM 断链，实际过滤由 R3 进程列表完成。

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
    KSWORD_ARK_SET_PROCESS_VISIBILITY_REQUEST* visibilityRequest = NULL;
    KSWORD_ARK_SET_PROCESS_VISIBILITY_RESPONSE* visibilityResponse = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    ULONG visibilityStatus = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_UNKNOWN;
    ULONG hiddenCount = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 process visibility denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_PROCESS_VISIBILITY_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 process visibility ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_PROCESS_VISIBILITY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 process visibility ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    visibilityRequest = (KSWORD_ARK_SET_PROCESS_VISIBILITY_REQUEST*)inputBuffer;
    visibilityResponse = (KSWORD_ARK_SET_PROCESS_VISIBILITY_RESPONSE*)outputBuffer;
    RtlZeroMemory(visibilityResponse, sizeof(*visibilityResponse));
    visibilityResponse->version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
    visibilityResponse->processId = visibilityRequest->processId;

    status = KswordARKDriverSetProcessVisibility(
        (ULONG)visibilityRequest->processId,
        (ULONG)visibilityRequest->action,
        &visibilityStatus,
        &hiddenCount);

    visibilityResponse->status = visibilityStatus;
    visibilityResponse->hiddenCount = hiddenCount;
    visibilityResponse->lastStatus = status;
    *BytesReturned = sizeof(*visibilityResponse);

    if (NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(
            Device,
            "Info",
            "R0 process visibility updated: pid=%lu, action=%lu, status=%lu, hiddenCount=%lu.",
            (unsigned long)visibilityRequest->processId,
            (unsigned long)visibilityRequest->action,
            (unsigned long)visibilityStatus,
            (unsigned long)hiddenCount);
    }
    else {
        KswordARKProcessIoctlLog(
            Device,
            "Error",
            "R0 process visibility failed: pid=%lu, action=%lu, status=0x%08X.",
            (unsigned long)visibilityRequest->processId,
            (unsigned long)visibilityRequest->action,
            (unsigned int)status);
    }

    return status;
}

NTSTATUS
KswordARKProcessIoctlSetSpecialFlags(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS。中文说明：handler 负责
    写访问、安全策略和固定包校验，具体 BreakOnTermination/APC 写入在
    process_flags.c 中完成。

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
    KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_REQUEST* specialRequest = NULL;
    KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_RESPONSE* specialResponse = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    ULONG operationStatus = KSWORD_ARK_PROCESS_SPECIAL_STATUS_UNKNOWN;
    ULONG appliedFlags = 0UL;
    ULONG touchedThreadCount = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 process-special denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 process-special ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 process-special ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    specialRequest = (KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_REQUEST*)inputBuffer;
    specialResponse = (KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_RESPONSE*)outputBuffer;
    RtlZeroMemory(specialResponse, sizeof(*specialResponse));
    specialResponse->version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
    specialResponse->processId = specialRequest->processId;
    specialResponse->action = specialRequest->action;

    status = KswordARKValidateUserPid((ULONG)specialRequest->processId);
    if (NT_SUCCESS(status)) {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_SET_PROTECTION;
        safetyContext.TargetProcessId = (ULONG)specialRequest->processId;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
    }

    if (NT_SUCCESS(status)) {
        status = KswordARKDriverSetProcessSpecialFlags(
            (ULONG)specialRequest->processId,
            (ULONG)specialRequest->action,
            (ULONG)specialRequest->flags,
            &operationStatus,
            &appliedFlags,
            &touchedThreadCount);
    }
    else {
        operationStatus = KSWORD_ARK_PROCESS_SPECIAL_STATUS_OPERATION_FAILED;
    }

    specialResponse->status = operationStatus;
    specialResponse->appliedFlags = appliedFlags;
    specialResponse->touchedThreadCount = touchedThreadCount;
    specialResponse->lastStatus = status;
    *BytesReturned = sizeof(*specialResponse);

    KswordARKProcessIoctlLog(
        Device,
        NT_SUCCESS(status) ? "Info" : "Error",
        "R0 process-special result: pid=%lu, action=%lu, status=%lu, touched=%lu, nt=0x%08X.",
        (unsigned long)specialRequest->processId,
        (unsigned long)specialRequest->action,
        (unsigned long)operationStatus,
        (unsigned long)touchedThreadCount,
        (unsigned int)status);
    return status;
}

NTSTATUS
KswordARKProcessIoctlDkomProcess(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_DKOM_PROCESS。中文说明：当前只提供从 PspCidTable
    删除目标 PID 对应 EPROCESS 的高危动作，地址均由 R0 自行解析。

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
    KSWORD_ARK_DKOM_PROCESS_REQUEST* dkomRequest = NULL;
    KSWORD_ARK_DKOM_PROCESS_RESPONSE* dkomResponse = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    ULONG operationStatus = KSWORD_ARK_PROCESS_DKOM_STATUS_UNKNOWN;
    ULONG removedEntries = 0UL;
    ULONG64 pspCidTableAddress = 0ULL;
    ULONG64 processObjectAddress = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Warn", "R0 process-dkom denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_DKOM_PROCESS_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 process-dkom ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_DKOM_PROCESS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKProcessIoctlLog(Device, "Error", "R0 process-dkom ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    dkomRequest = (KSWORD_ARK_DKOM_PROCESS_REQUEST*)inputBuffer;
    dkomResponse = (KSWORD_ARK_DKOM_PROCESS_RESPONSE*)outputBuffer;
    RtlZeroMemory(dkomResponse, sizeof(*dkomResponse));
    dkomResponse->version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
    dkomResponse->processId = dkomRequest->processId;
    dkomResponse->action = dkomRequest->action;

    status = KswordARKValidateUserPid((ULONG)dkomRequest->processId);
    if (NT_SUCCESS(status)) {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH;
        safetyContext.TargetProcessId = (ULONG)dkomRequest->processId;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
    }

    if (NT_SUCCESS(status)) {
        status = KswordARKDriverDkomProcess(
            (ULONG)dkomRequest->processId,
            (ULONG)dkomRequest->action,
            (ULONG)dkomRequest->flags,
            &operationStatus,
            &removedEntries,
            &pspCidTableAddress,
            &processObjectAddress);
    }
    else {
        operationStatus = KSWORD_ARK_PROCESS_DKOM_STATUS_OPERATION_FAILED;
    }

    dkomResponse->status = operationStatus;
    dkomResponse->removedEntries = removedEntries;
    dkomResponse->lastStatus = status;
    dkomResponse->pspCidTableAddress = pspCidTableAddress;
    dkomResponse->processObjectAddress = processObjectAddress;
    *BytesReturned = sizeof(*dkomResponse);

    KswordARKProcessIoctlLog(
        Device,
        NT_SUCCESS(status) ? "Info" : "Error",
        "R0 process-dkom result: pid=%lu, action=%lu, status=%lu, removed=%lu, cid=0x%I64X, nt=0x%08X.",
        (unsigned long)dkomRequest->processId,
        (unsigned long)dkomRequest->action,
        (unsigned long)operationStatus,
        (unsigned long)removedEntries,
        pspCidTableAddress,
        (unsigned int)status);
    return status;
}
