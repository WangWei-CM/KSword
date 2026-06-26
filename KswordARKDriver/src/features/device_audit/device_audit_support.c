/*++

Module Name:

    device_audit_support.c

Abstract:

    Shared validation, logging, string, status, and append helpers for device audit queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "device_audit_internal.h"

VOID
KswDeviceAuditLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one compact device-audit log line.  The helper does not
    include user data; it only reports status, profile, and row counts.

Arguments:

    Device - WDF device that owns the existing log channel.
    LevelText - ANSI log level string consumed by the log channel.
    FormatText - printf-style ANSI format string.
    ... - Format arguments used to build the final log line.

Return Value:

    None.  Formatting or queueing failures are intentionally ignored.

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

VOID
KswDeviceAuditZeroResponse(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ ULONG ProfileFlags
    )
/*++

Routine Description:

    Initialize the variable-length response header before rows are appended.

Arguments:

    OutputBuffer - Caller output buffer retrieved from WDF.
    OutputBufferLength - Byte length of OutputBuffer.
    ProfileFlags - Effective audit profile represented by the response.

Return Value:

    None.  The response header is initialized in-place.

--*/
{
    KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE* response = NULL;

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE*)OutputBuffer;
    response->size = sizeof(KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE);
    response->version = KSWORD_ARK_DEVICE_AUDIT_PROTOCOL_VERSION;
    response->queryStatus = KSWORD_ARK_DEVICE_AUDIT_STATUS_OK;
    response->profileFlags = ProfileFlags;
    response->entrySize = sizeof(KSWORD_ARK_DEVICE_AUDIT_ENTRY);
    response->lastStatus = STATUS_SUCCESS;
}

ULONG
KswDeviceAuditNormalizeMaxRows(
    _In_ ULONG RequestedRows
    )
/*++

Routine Description:

    Clamp the requested row count to the protocol default and hard maximum.

Arguments:

    RequestedRows - User requested row count, where zero means default.

Return Value:

    Effective row count used by the read-only audit query.

--*/
{
    ULONG effectiveRows = RequestedRows;

    if (effectiveRows == 0UL) {
        effectiveRows = KSWORD_ARK_DEVICE_AUDIT_DEFAULT_MAX_ROWS;
    }
    if (effectiveRows > KSWORD_ARK_DEVICE_AUDIT_HARD_MAX_ROWS) {
        effectiveRows = KSWORD_ARK_DEVICE_AUDIT_HARD_MAX_ROWS;
    }
    return effectiveRows;
}

ULONG
KswDeviceAuditNormalizeAttachedDepth(
    _In_ ULONG RequestedDepth
    )
/*++

Routine Description:

    Clamp the attached-device traversal depth forwarded to the existing integrity
    backend.  The backend remains the only code that walks DeviceObject links.

Arguments:

    RequestedDepth - User requested attached depth, where zero means default.

Return Value:

    Effective attached-depth limit.

--*/
{
    ULONG effectiveDepth = RequestedDepth;

    if (effectiveDepth == 0UL) {
        effectiveDepth = KSWORD_ARK_DEVICE_AUDIT_DEFAULT_MAX_ATTACHED_DEPTH;
    }
    if (effectiveDepth > KSWORD_ARK_DEVICE_AUDIT_HARD_MAX_ATTACHED_DEPTH) {
        effectiveDepth = KSWORD_ARK_DEVICE_AUDIT_HARD_MAX_ATTACHED_DEPTH;
    }
    return effectiveDepth;
}

BOOLEAN
KswDeviceAuditStringPresent(
    _In_reads_(MaxChars) const WCHAR* Text,
    _In_ ULONG MaxChars
    )
/*++

Routine Description:

    Check whether a fixed-size protocol string contains a bounded, terminated,
    non-empty value.

Arguments:

    Text - Fixed-size protocol string buffer.
    MaxChars - Maximum characters available in Text.

Return Value:

    TRUE when Text has a terminator and at least one non-NUL character.

--*/
{
    ULONG index = 0UL;

    if (Text == NULL || MaxChars == 0UL) {
        return FALSE;
    }
    while (index < MaxChars) {
        if (Text[index] == L'\0') {
            return (index != 0UL) ? TRUE : FALSE;
        }
        ++index;
    }
    return FALSE;
}

VOID
KswDeviceAuditCopyWide(
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _In_opt_z_ PCWSTR Source
    )
/*++

Routine Description:

    Copy a NUL-terminated wide string into a fixed protocol field.

Arguments:

    Destination - Fixed-size output field.
    DestinationChars - Character capacity of Destination.
    Source - Optional NUL-terminated source string.

Return Value:

    None.  Destination is always NUL-terminated when capacity is non-zero.

--*/
{
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }
    Destination[0] = L'\0';
    if (Source == NULL || Source[0] == L'\0') {
        return;
    }
    (VOID)RtlStringCchCopyW(Destination, DestinationChars, Source);
    Destination[DestinationChars - 1UL] = L'\0';
}

static PCWSTR
KswDeviceAuditLeafName(
    _In_z_ PCWSTR DriverName
    )
/*++

Routine Description:

    Return the final component of a DriverObject namespace path.

Arguments:

    DriverName - NUL-terminated driver object name or service name.

Return Value:

    Pointer inside DriverName that starts at the final component.

--*/
{
    PCWSTR leaf = DriverName;
    PCWSTR cursor = DriverName;

    if (DriverName == NULL) {
        return L"";
    }
    while (*cursor != L'\0') {
        if (*cursor == L'\\') {
            leaf = cursor + 1;
        }
        ++cursor;
    }
    return leaf;
}

VOID
KswDeviceAuditCopyServiceName(
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _In_z_ PCWSTR DriverName
    )
/*++

Routine Description:

    Derive a service-like name from a DriverObject namespace path for R3
    correlation.  The routine does not open or modify the service key.

Arguments:

    Destination - Fixed protocol field that receives the leaf name.
    DestinationChars - Character capacity of Destination.
    DriverName - DriverObject namespace path or plain service name.

Return Value:

    None.  Destination is always NUL-terminated when capacity is non-zero.

--*/
{
    KswDeviceAuditCopyWide(Destination, DestinationChars, KswDeviceAuditLeafName(DriverName));
}

ULONG
KswDeviceAuditOutputCapacity(
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    Calculate how many variable-length audit rows fit after the response header.

Arguments:

    OutputBufferLength - Actual WDF output buffer length in bytes.

Return Value:

    Row capacity available in the caller buffer.

--*/
{
    size_t payloadBytes = 0U;

    if (OutputBufferLength <= KSW_DEVICE_AUDIT_RESPONSE_HEADER_SIZE) {
        return 0UL;
    }
    payloadBytes = OutputBufferLength - KSW_DEVICE_AUDIT_RESPONSE_HEADER_SIZE;
    return (ULONG)(payloadBytes / sizeof(KSWORD_ARK_DEVICE_AUDIT_ENTRY));
}

ULONG
KswDeviceAuditMapRiskFlags(
    _In_ ULONG IntegrityRiskFlags
    )
/*++

Routine Description:

    Translate generic DriverObject integrity risk bits into the device-audit risk
    namespace used by the new protocol.

Arguments:

    IntegrityRiskFlags - Risk flags produced by KswordARKDriverQueryDriverIntegrity.

Return Value:

    Equivalent device-audit risk flags.  Unknown integrity bits are ignored so
    future integrity rows remain forward-compatible.

--*/
{
    ULONG riskFlags = KSWORD_ARK_DEVICE_AUDIT_RISK_NONE;

    if ((IntegrityRiskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_UNAVAILABLE) != 0UL) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_UNAVAILABLE;
    }
    if ((IntegrityRiskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_QUERY_FAILED) != 0UL) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_QUERY_FAILED;
    }
    if ((IntegrityRiskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DEVICE_LOOP) != 0UL) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_DEVICE_LOOP;
    }
    if ((IntegrityRiskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_ATTACHED_LOOP) != 0UL) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_ATTACHED_LOOP;
    }
    if ((IntegrityRiskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CROSS_DRIVER_ATTACH) != 0UL) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_CROSS_DRIVER_ATTACH;
    }
    if ((IntegrityRiskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OWNER_MISMATCH) != 0UL) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_ROLE_AMBIGUOUS;
    }
    if ((IntegrityRiskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_TRUNCATED) != 0UL) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_STACK_TRUNCATED;
    }
    if (IntegrityRiskFlags != KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE) {
        riskFlags |= KSWORD_ARK_DEVICE_AUDIT_RISK_INTEGRITY_PARTIAL;
    }
    return riskFlags;
}

ULONG
KswDeviceAuditMapStatus(
    _In_ ULONG IntegrityStatus,
    _In_ ULONG IntegrityRiskFlags
    )
/*++

Routine Description:

    Convert an integrity entry status to the device-audit status model.

Arguments:

    IntegrityStatus - Per-row status from an integrity evidence row.
    IntegrityRiskFlags - Per-row risk flags used as a fallback classifier.

Return Value:

    Device-audit row status.

--*/
{
    if (IntegrityStatus == KSWORD_ARK_DRIVER_INTEGRITY_STATUS_OK &&
        IntegrityRiskFlags == KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE) {
        return KSWORD_ARK_DEVICE_AUDIT_STATUS_OK;
    }
    if (IntegrityStatus == KSWORD_ARK_DRIVER_INTEGRITY_STATUS_NOT_FOUND) {
        return KSWORD_ARK_DEVICE_AUDIT_STATUS_NOT_FOUND;
    }
    if (IntegrityStatus == KSWORD_ARK_DRIVER_INTEGRITY_STATUS_BUFFER_TOO_SMALL) {
        return KSWORD_ARK_DEVICE_AUDIT_STATUS_BUFFER_TRUNCATED;
    }
    if (IntegrityStatus == KSWORD_ARK_DRIVER_INTEGRITY_STATUS_QUERY_FAILED) {
        return KSWORD_ARK_DEVICE_AUDIT_STATUS_QUERY_FAILED;
    }
    if (IntegrityStatus == KSWORD_ARK_DRIVER_INTEGRITY_STATUS_UNAVAILABLE) {
        return KSWORD_ARK_DEVICE_AUDIT_STATUS_UNAVAILABLE;
    }
    return KSWORD_ARK_DEVICE_AUDIT_STATUS_PARTIAL;
}

VOID
KswDeviceAuditSetResponsePartial(
    _Inout_ KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE* Response,
    _In_ NTSTATUS LastStatus
    )
/*++

Routine Description:

    Mark the response as partial while preserving the first failure status useful
    to diagnostics.

Arguments:

    Response - Device-audit response header to update.
    LastStatus - NTSTATUS value observed during backend conversion.

Return Value:

    None.  Response is updated in-place.

--*/
{
    if (Response == NULL) {
        return;
    }
    Response->responseFlags |= KSWORD_ARK_DEVICE_AUDIT_RESPONSE_FLAG_PARTIAL;
    if (Response->queryStatus == KSWORD_ARK_DEVICE_AUDIT_STATUS_OK) {
        Response->queryStatus = KSWORD_ARK_DEVICE_AUDIT_STATUS_PARTIAL;
    }
    if (Response->lastStatus == STATUS_SUCCESS && !NT_SUCCESS(LastStatus)) {
        Response->lastStatus = LastStatus;
    }
}

NTSTATUS
KswDeviceAuditAppendEntry(
    _Inout_ KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE* Response,
    _In_ ULONG Capacity,
    _In_ ULONG MaxRows,
    _In_ const KSWORD_ARK_DEVICE_AUDIT_ENTRY* SourceEntry
    )
/*++

Routine Description:

    Append one row to the caller response if both protocol and caller buffer
    limits allow it.

Arguments:

    Response - Initialized variable-length response header.
    Capacity - Physical row capacity derived from output buffer size.
    MaxRows - Logical row limit requested by the caller.
    SourceEntry - Fully initialized row to copy into the response.

Return Value:

    STATUS_SUCCESS when the row is copied.  STATUS_BUFFER_TOO_SMALL when the row
    could not be copied and the response was marked truncated.

--*/
{
    ULONG logicalLimit = MaxRows;

    if (Response == NULL || SourceEntry == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (logicalLimit == 0UL || logicalLimit > KSWORD_ARK_DEVICE_AUDIT_HARD_MAX_ROWS) {
        logicalLimit = KSWORD_ARK_DEVICE_AUDIT_HARD_MAX_ROWS;
    }
    Response->totalCount += 1UL;
    if (Response->returnedCount >= Capacity || Response->returnedCount >= logicalLimit) {
        Response->responseFlags |= KSWORD_ARK_DEVICE_AUDIT_RESPONSE_FLAG_TRUNCATED;
        Response->queryStatus = KSWORD_ARK_DEVICE_AUDIT_STATUS_PARTIAL;
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(&Response->entries[Response->returnedCount], SourceEntry, sizeof(*SourceEntry));
    Response->returnedCount += 1UL;
    return STATUS_SUCCESS;
}

