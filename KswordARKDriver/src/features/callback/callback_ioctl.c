/*++

Module Name:

    callback_ioctl.c

Abstract:

    IOCTL handlers for KswordARK callback interception operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKCallbackIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one callback-handler log message.

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
KswordARKCallbackIoctlSetRulesHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_SET_CALLBACK_RULES by delegating blob validation and
    activation to the existing callback rule module.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Rule blob length supplied by user mode.
    OutputBufferLength - Caller output length; unused for this IOCTL.
    BytesReturned - Receives callback module completion bytes.

Return Value:

    NTSTATUS from KswordARKCallbackIoctlSetRules.

--*/
{
    NTSTATUS status;
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;
    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_CALLBACK_SET_RULES;
        safetyContext.TargetProcessId = 0UL;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKCallbackIoctlLog(Device, "Warn", "Callback rules denied by safety policy, status=0x%08X.", (unsigned int)status);
            return status;
        }
    }

    status = KswordARKCallbackIoctlSetRules(Request, InputBufferLength, BytesReturned);
    if (NT_SUCCESS(status)) {
        (void)KswordARKDriverEnqueueLogFrame(Device, "Info", "Callback rules applied.");
    }
    else {
        KswordARKCallbackIoctlLog(Device, "Error", "Callback rules apply failed, status=0x%08X.", (unsigned int)status);
    }
    return status;
}

NTSTATUS
KswordARKCallbackIoctlGetRuntimeStateHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE by forwarding the output
    request to the callback runtime module.

Arguments:

    Device - WDF device, currently unused by this pass-through handler.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length; unused for this IOCTL.
    OutputBufferLength - Runtime state output buffer length.
    BytesReturned - Receives sizeof(runtime-state) on success.

Return Value:

    NTSTATUS from KswordARKCallbackIoctlGetRuntimeState.

--*/
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;
    return KswordARKCallbackIoctlGetRuntimeState(Request, OutputBufferLength, BytesReturned);
}

NTSTATUS
KswordARKCallbackIoctlWaitEventHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT. STATUS_PENDING is intentionally
    returned to dispatch so the request is not completed twice.

Arguments:

    Device - WDF device, currently unused by this pass-through handler.
    Request - Current IOCTL request.
    InputBufferLength - Wait request length, validated by callback module.
    OutputBufferLength - Event packet output buffer length.
    BytesReturned - Receives bytes when completed synchronously.

Return Value:

    NTSTATUS from KswordARKCallbackIoctlWaitEvent, including STATUS_PENDING.

--*/
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;
    return KswordARKCallbackIoctlWaitEvent(Request, OutputBufferLength, BytesReturned);
}

NTSTATUS
KswordARKCallbackIoctlAnswerEventHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT by forwarding the answer packet
    to the pending-decision runtime.

Arguments:

    Device - WDF device used for failure logging.
    Request - Current IOCTL request.
    InputBufferLength - Answer request length.
    OutputBufferLength - Caller output length; unused for this IOCTL.
    BytesReturned - Receives callback module completion bytes.

Return Value:

    NTSTATUS from KswordARKCallbackIoctlAnswerEvent.

--*/
{
    NTSTATUS status;
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKCallbackIoctlAnswerEvent(Request, InputBufferLength, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKCallbackIoctlLog(Device, "Warn", "Callback answer failed, status=0x%08X.", (unsigned int)status);
    }
    return status;
}
NTSTATUS
KswordARKCallbackIoctlCancelAllPendingHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS. No input or output
    buffer is required; the callback runtime owns the cancellation logic.

Arguments:

    Device - WDF device used for failure logging.
    Request - Current IOCTL request, intentionally unused.
    InputBufferLength - Caller input length, intentionally unused.
    OutputBufferLength - Caller output length, intentionally unused.
    BytesReturned - Receives callback module completion bytes.

Return Value:

    NTSTATUS from KswordARKCallbackIoctlCancelAllPending.

--*/
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;
    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_CALLBACK_CANCEL_PENDING;
        safetyContext.TargetProcessId = 0UL;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKCallbackIoctlLog(Device, "Warn", "Cancel-all pending decisions denied by safety policy, status=0x%08X.", (unsigned int)status);
            return status;
        }
    }

    status = KswordARKCallbackIoctlCancelAllPending(BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKCallbackIoctlLog(Device, "Warn", "Cancel-all pending decisions failed, status=0x%08X.", (unsigned int)status);
    }
    return status;
}

NTSTATUS
KswordARKCallbackIoctlRemoveExternalCallbackHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK. The handler keeps the
    explicit FILE_WRITE_ACCESS validation before calling the remove feature.

Arguments:

    Device - WDF device used for audit logging.
    Request - Current IOCTL request.
    InputBufferLength - Remove request length.
    OutputBufferLength - Remove response buffer length.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from access validation or KswordARKCallbackIoctlRemoveExternalCallback.

--*/
{
    NTSTATUS status;

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;
    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_CALLBACK_REMOVE_EXTERNAL;
        safetyContext.TargetProcessId = 0UL;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKCallbackIoctlLog(Device, "Warn", "Remove external callback denied by safety policy, status=0x%08X.", (unsigned int)status);
            return status;
        }
    }

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKCallbackIoctlLog(Device, "Warn", "Remove external callback denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKCallbackIoctlRemoveExternalCallback(Request, InputBufferLength, OutputBufferLength, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKCallbackIoctlLog(Device, "Warn", "Remove external callback failed, status=0x%08X.", (unsigned int)status);
    }
    return status;
}

NTSTATUS
KswordARKCallbackIoctlEnumCallbacksHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ENUM_CALLBACKS. The operation is read-only and keeps
    all business traversal inside the callback feature module.

Arguments:

    Device - WDF device used for diagnostic logging.
    Request - Current IOCTL request.
    InputBufferLength - Enumeration request length.
    OutputBufferLength - Enumeration response buffer length.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from KswordARKCallbackIoctlEnumCallbacks.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKCallbackIoctlEnumCallbacks(
        Request,
        InputBufferLength,
        OutputBufferLength,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKCallbackIoctlLog(
            Device,
            "Warn",
            "Callback enumeration failed, status=0x%08X.",
            (unsigned int)status);
    }
    return status;
}
