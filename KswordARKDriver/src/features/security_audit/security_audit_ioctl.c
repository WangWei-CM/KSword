/*++

Module Name:

    security_audit_ioctl.c

Abstract:

    IOCTL handlers for read-only security posture audit queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "security_audit_internal.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordSecurityAuditIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Emit compact security-audit IOCTL logs without returning paths or private data.

Arguments:

    Device - WDF device used by the log channel.
    LevelText - Text log level.
    FormatText - printf-style format string.
    ... - Format arguments.

Return Value:

    None. The routine only attempts best-effort log enqueue.

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
KswordARKSecurityAuditIoctlQuerySecurityStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_SECURITY_STATUS. The handler accepts an optional
    versioned request and delegates all posture collection to security_audit_query.c.

Arguments:

    Device - WDF device object.
    Request - Current IOCTL request.
    InputBufferLength - Caller-supplied input length.
    OutputBufferLength - Caller-supplied output length.
    BytesReturned - Receives the completed byte count.

Return Value:

    NTSTATUS from validation or query backend.

--*/
{
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    BOOLEAN inputPresent = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_QUERY_SECURITY_STATUS_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &inputPresent);
    if (!NT_SUCCESS(status)) {
        KswordSecurityAuditIoctlLog(Device, "Error", "security-status input invalid: 0x%08X.", (unsigned int)status);
        return status;
    }
    if (inputPresent) {
        const KSWORD_ARK_QUERY_SECURITY_STATUS_REQUEST* queryRequest = (const KSWORD_ARK_QUERY_SECURITY_STATUS_REQUEST*)inputBuffer;
        if (queryRequest->size < sizeof(*queryRequest) || queryRequest->version > KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION || queryRequest->flags != 0UL) {
            return STATUS_INVALID_PARAMETER;
        }
        UNREFERENCED_PARAMETER(actualInputLength);
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_SECURITY_STATUS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordSecurityAuditIoctlLog(Device, "Error", "security-status output invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKSecurityAuditQuerySecurityStatus(outputBuffer, actualOutputLength, BytesReturned);
    KswordSecurityAuditIoctlLog(Device, NT_SUCCESS(status) ? "Info" : "Error", "security-status completed: status=0x%08X bytes=%Iu.", (unsigned int)status, *BytesReturned);
    return status;
}

NTSTATUS
KswordARKSecurityAuditIoctlQueryDriverTrustView(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW. The handler validates only
    row bounds and flags; loaded-module and signing-cache work stays in the feature backend.

Arguments:

    Device - WDF device object.
    Request - Current IOCTL request.
    InputBufferLength - Caller-supplied input length.
    OutputBufferLength - Caller-supplied output length.
    BytesReturned - Receives the completed byte count.

Return Value:

    NTSTATUS from validation or query backend.

--*/
{
    const KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    BOOLEAN inputPresent = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &inputPresent);
    if (!NT_SUCCESS(status)) {
        KswordSecurityAuditIoctlLog(Device, "Error", "driver-trust-view input invalid: 0x%08X.", (unsigned int)status);
        return status;
    }
    if (inputPresent) {
        queryRequest = (const KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST*)inputBuffer;
        if (queryRequest->size < sizeof(*queryRequest) || queryRequest->version > KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION) {
            return STATUS_INVALID_PARAMETER;
        }
        if ((queryRequest->flags & ~KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_INCLUDE_SIGNING_LEVEL) != 0UL) {
            return STATUS_INVALID_PARAMETER;
        }
        UNREFERENCED_PARAMETER(actualInputLength);
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        FIELD_OFFSET(KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE, entries),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordSecurityAuditIoctlLog(Device, "Error", "driver-trust-view output invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKSecurityAuditQueryDriverTrustView(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    KswordSecurityAuditIoctlLog(Device, NT_SUCCESS(status) ? "Info" : "Error", "driver-trust-view completed: status=0x%08X bytes=%Iu.", (unsigned int)status, *BytesReturned);
    return status;
}

NTSTATUS
KswordARKSecurityAuditIoctlQueryHyperVSummary(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_HYPERV_SUMMARY by retrieving the fixed output buffer.

Arguments:

    Device - WDF device object.
    Request - Current IOCTL request.
    InputBufferLength - Caller-supplied input length.
    OutputBufferLength - Caller-supplied output length.
    BytesReturned - Receives the completed byte count.

Return Value:

    NTSTATUS from validation or query backend.

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
        sizeof(KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordSecurityAuditIoctlLog(Device, "Error", "hyperv-summary output invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKSecurityAuditQueryHyperVSummary(outputBuffer, actualOutputLength, BytesReturned);
    KswordSecurityAuditIoctlLog(Device, NT_SUCCESS(status) ? "Info" : "Error", "hyperv-summary completed: status=0x%08X bytes=%Iu.", (unsigned int)status, *BytesReturned);
    return status;
}

NTSTATUS
KswordARKSecurityAuditIoctlQueryAppControlStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_APP_CONTROL_STATUS by retrieving the fixed output buffer.

Arguments:

    Device - WDF device object.
    Request - Current IOCTL request.
    InputBufferLength - Caller-supplied input length.
    OutputBufferLength - Caller-supplied output length.
    BytesReturned - Receives the completed byte count.

Return Value:

    NTSTATUS from validation or query backend.

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
        sizeof(KSWORD_ARK_QUERY_APP_CONTROL_STATUS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordSecurityAuditIoctlLog(Device, "Error", "app-control-status output invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKSecurityAuditQueryAppControlStatus(outputBuffer, actualOutputLength, BytesReturned);
    KswordSecurityAuditIoctlLog(Device, NT_SUCCESS(status) ? "Info" : "Error", "app-control-status completed: status=0x%08X bytes=%Iu.", (unsigned int)status, *BytesReturned);
    return status;
}
