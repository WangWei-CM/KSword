/*++

Module Name:

    mutation_ioctl.c

Abstract:

    IOCTL handlers for the controlled mutation transaction backend.

Environment:

    Kernel-mode Driver Framework

--*/

#include "mutation_transaction.h"
#include "../../dispatch/ioctl_validation.h"
#include "ark/ark_log.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKMutationIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...)
/*++

Routine Description:

    Formats one mutation IOCTL diagnostic log entry. The message records only
    transaction metadata and never dumps before/after bytes.

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level text.
    FormatText - printf-style format string.
    ... - Format arguments.

Return Value:

    None. Formatting or enqueue failures are ignored.

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
KswordARKMutationIoctlPrepare(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned)
/*++

Routine Description:

    Handles the future IOCTL_KSWORD_ARK_MUTATION_PREPARE entry. The handler
    validates write access, copies METHOD_BUFFERED input before output is cleared,
    and delegates target validation and before-byte snapshotting to the backend.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Input buffer length supplied by dispatch.
    OutputBufferLength - Output buffer length supplied by dispatch.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from access checks, buffer retrieval, or the transaction backend.

--*/
{
    KSWORD_ARK_MUTATION_PREPARE_REQUEST* inputRequest = NULL;
    KSWORD_ARK_MUTATION_PREPARE_REQUEST requestCopy;
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
        KswordARKMutationIoctlLog(Device, "Warn", "Mutation prepare denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_MUTATION_PREPARE_REQUEST),
        (PVOID*)&inputRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation prepare input invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }

    RtlZeroMemory(&requestCopy, sizeof(requestCopy));
    RtlCopyMemory(&requestCopy, inputRequest, sizeof(requestCopy));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_MUTATION_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation prepare output invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKMutationPrepare(
        Device,
        &requestCopy,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Warn", "Mutation prepare backend rejected: kind=%lu, status=0x%08X.", (unsigned long)requestCopy.targetKind, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_MUTATION_RESPONSE)) {
        KSWORD_ARK_MUTATION_RESPONSE* response = (KSWORD_ARK_MUTATION_RESPONSE*)outputBuffer;
        KswordARKMutationIoctlLog(Device, "Info", "Mutation prepare response: tx=%I64u, kind=%lu, status=%lu, last=0x%08X.", response->transactionId, (unsigned long)response->targetKind, (unsigned long)response->status, (unsigned int)response->lastStatus);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMutationIoctlCommit(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned)
/*++

Routine Description:

    Handles the future IOCTL_KSWORD_ARK_MUTATION_COMMIT entry. The handler only
    accepts a transaction request, copies it before output reuse, and leaves all
    before-match, FORCE, safety policy, and write decisions to the backend.

Arguments:

    Device - WDF device used for logging and safety policy logs.
    Request - Current IOCTL request.
    InputBufferLength - Input buffer length supplied by dispatch.
    OutputBufferLength - Output buffer length supplied by dispatch.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from access checks, buffer retrieval, or the transaction backend.

--*/
{
    KSWORD_ARK_MUTATION_TRANSACTION_REQUEST* inputRequest = NULL;
    KSWORD_ARK_MUTATION_TRANSACTION_REQUEST requestCopy;
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
        KswordARKMutationIoctlLog(Device, "Warn", "Mutation commit denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_MUTATION_TRANSACTION_REQUEST),
        (PVOID*)&inputRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation commit input invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }

    RtlZeroMemory(&requestCopy, sizeof(requestCopy));
    RtlCopyMemory(&requestCopy, inputRequest, sizeof(requestCopy));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_MUTATION_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation commit output invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKMutationCommit(
        Device,
        &requestCopy,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Warn", "Mutation commit backend failed: tx=%I64u, status=0x%08X.", requestCopy.transactionId, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_MUTATION_RESPONSE)) {
        KSWORD_ARK_MUTATION_RESPONSE* response = (KSWORD_ARK_MUTATION_RESPONSE*)outputBuffer;
        KswordARKMutationIoctlLog(Device, "Info", "Mutation commit response: tx=%I64u, status=%lu, last=0x%08X.", response->transactionId, (unsigned long)response->status, (unsigned int)response->lastStatus);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMutationIoctlRollback(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned)
/*++

Routine Description:

    Handles the future IOCTL_KSWORD_ARK_MUTATION_ROLLBACK entry. Rollback is
    driven only by transactionId and is idempotent when current bytes already
    match the before snapshot.

Arguments:

    Device - WDF device used for logging and safety policy logs.
    Request - Current IOCTL request.
    InputBufferLength - Input buffer length supplied by dispatch.
    OutputBufferLength - Output buffer length supplied by dispatch.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from access checks, buffer retrieval, or the transaction backend.

--*/
{
    KSWORD_ARK_MUTATION_TRANSACTION_REQUEST* inputRequest = NULL;
    KSWORD_ARK_MUTATION_TRANSACTION_REQUEST requestCopy;
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
        KswordARKMutationIoctlLog(Device, "Warn", "Mutation rollback denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_MUTATION_TRANSACTION_REQUEST),
        (PVOID*)&inputRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation rollback input invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }

    RtlZeroMemory(&requestCopy, sizeof(requestCopy));
    RtlCopyMemory(&requestCopy, inputRequest, sizeof(requestCopy));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_MUTATION_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation rollback output invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKMutationRollback(
        Device,
        &requestCopy,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Warn", "Mutation rollback backend failed: tx=%I64u, status=0x%08X.", requestCopy.transactionId, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_MUTATION_RESPONSE)) {
        KSWORD_ARK_MUTATION_RESPONSE* response = (KSWORD_ARK_MUTATION_RESPONSE*)outputBuffer;
        KswordARKMutationIoctlLog(Device, "Info", "Mutation rollback response: tx=%I64u, status=%lu, last=0x%08X.", response->transactionId, (unsigned long)response->status, (unsigned int)response->lastStatus);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMutationIoctlQueryAudit(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned)
/*++

Routine Description:

    Handles the future IOCTL_KSWORD_ARK_MUTATION_QUERY_AUDIT entry. The input is
    optional; when omitted the backend returns the newest entries that fit in the
    caller's output buffer without byteData.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Input buffer length supplied by dispatch.
    OutputBufferLength - Output buffer length supplied by dispatch.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from buffer retrieval or audit query backend.

--*/
{
    KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST requestCopy;
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

    RtlZeroMemory(&requestCopy, sizeof(requestCopy));
    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation audit query input invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }
    if (hasInput) {
        RtlCopyMemory(&requestCopy, inputBuffer, sizeof(requestCopy));
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_MUTATION_AUDIT_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Error", "Mutation audit query output invalid: status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKMutationQueryAudit(
        outputBuffer,
        actualOutputLength,
        hasInput ? &requestCopy : NULL,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKMutationIoctlLog(Device, "Warn", "Mutation audit query backend failed: status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_MUTATION_AUDIT_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE* response = (KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE*)outputBuffer;
        KswordARKMutationIoctlLog(Device, "Info", "Mutation audit query response: total=%lu, returned=%lu, next=%I64u.", (unsigned long)response->totalCount, (unsigned long)response->returnedCount, response->nextSequence);
    }

    return STATUS_SUCCESS;
}
