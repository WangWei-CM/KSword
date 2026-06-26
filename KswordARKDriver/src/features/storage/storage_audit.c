/*++

Module Name:

    storage_audit.c

Abstract:

    Read-only Storage, BitLocker/FVE, MountMgr, and file-system integrity audit
    backends for KswordARK. This file never exports encryption secrets, never
    mutates device stacks, and never patches dispatch tables.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_storage.h"
#include "../kernel/hook_scan_support.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSW_STORAGE_RESPONSE_HEADER_SIZE(ResponseType, RowType) (sizeof(ResponseType) - sizeof(RowType))
#define KSW_STORAGE_TAG 'tSsK'
#define KSW_STORAGE_FVEVOL_ABSENT_INDEX 0xFFFFFFFFUL
#define KSW_STORAGE_FAST_IO_FIELD(FieldName) { FIELD_OFFSET(FAST_IO_DISPATCH, FieldName), #FieldName }

typedef struct _KSW_STORAGE_FAST_IO_FIELD
{
    ULONG Offset;
    PCSTR Name;
} KSW_STORAGE_FAST_IO_FIELD, *PKSW_STORAGE_FAST_IO_FIELD;

typedef struct _KSW_STORAGE_LIMITS
{
    ULONG MaxRows;
    ULONG MaxDepth;
} KSW_STORAGE_LIMITS, *PKSW_STORAGE_LIMITS;

typedef struct _KSW_STORAGE_STACK_RESULT
{
    ULONG TotalRows;
    ULONG ReturnedRows;
    ULONG FieldFlags;
    ULONG ResponseFlags;
    ULONG FvevolPresent;
    ULONG FvevolPosition;
    NTSTATUS LastStatus;
    BOOLEAN Truncated;
} KSW_STORAGE_STACK_RESULT, *PKSW_STORAGE_STACK_RESULT;

NTSYSAPI
NTSTATUS
NTAPI
ObReferenceObjectByName(
    _In_ PUNICODE_STRING ObjectName,
    _In_ ULONG Attributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Inout_opt_ PVOID ParseContext,
    _Out_ PVOID* Object
    );

extern POBJECT_TYPE* IoDriverObjectType;

static const KSW_STORAGE_FAST_IO_FIELD g_KswordStorageFastIoFields[] = {
    KSW_STORAGE_FAST_IO_FIELD(FastIoCheckIfPossible),
    KSW_STORAGE_FAST_IO_FIELD(FastIoRead),
    KSW_STORAGE_FAST_IO_FIELD(FastIoWrite),
    KSW_STORAGE_FAST_IO_FIELD(FastIoQueryBasicInfo),
    KSW_STORAGE_FAST_IO_FIELD(FastIoQueryStandardInfo),
    KSW_STORAGE_FAST_IO_FIELD(FastIoLock),
    KSW_STORAGE_FAST_IO_FIELD(FastIoUnlockSingle),
    KSW_STORAGE_FAST_IO_FIELD(FastIoUnlockAll),
    KSW_STORAGE_FAST_IO_FIELD(FastIoUnlockAllByKey),
    KSW_STORAGE_FAST_IO_FIELD(FastIoDeviceControl),
    KSW_STORAGE_FAST_IO_FIELD(AcquireFileForNtCreateSection),
    KSW_STORAGE_FAST_IO_FIELD(ReleaseFileForNtCreateSection),
    KSW_STORAGE_FAST_IO_FIELD(FastIoDetachDevice),
    KSW_STORAGE_FAST_IO_FIELD(FastIoQueryNetworkOpenInfo),
    KSW_STORAGE_FAST_IO_FIELD(AcquireForModWrite),
    KSW_STORAGE_FAST_IO_FIELD(MdlRead),
    KSW_STORAGE_FAST_IO_FIELD(MdlReadComplete),
    KSW_STORAGE_FAST_IO_FIELD(PrepareMdlWrite),
    KSW_STORAGE_FAST_IO_FIELD(MdlWriteComplete),
    KSW_STORAGE_FAST_IO_FIELD(FastIoReadCompressed),
    KSW_STORAGE_FAST_IO_FIELD(FastIoWriteCompressed),
    KSW_STORAGE_FAST_IO_FIELD(MdlReadCompleteCompressed),
    KSW_STORAGE_FAST_IO_FIELD(MdlWriteCompleteCompressed),
    KSW_STORAGE_FAST_IO_FIELD(FastIoQueryOpen),
    KSW_STORAGE_FAST_IO_FIELD(ReleaseForModWrite),
    KSW_STORAGE_FAST_IO_FIELD(AcquireForCcFlush),
    KSW_STORAGE_FAST_IO_FIELD(ReleaseForCcFlush)
};

static const PCWSTR g_KswordStorageFileSystemDriverNames[] = {
    L"\\FileSystem\\Ntfs",
    L"\\FileSystem\\Refs",
    L"\\FileSystem\\Fastfat",
    L"\\FileSystem\\exFat"
};

static VOID
KswordStorageCopyWide(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_opt_z_ PCWSTR Source
    )
/*++
Routine Description:
    Copy a NUL-terminated wide string into a fixed protocol buffer and always
    terminate the destination.
Arguments:
    Destination - Fixed protocol buffer to receive text.
    DestinationChars - Destination capacity in WCHAR units.
    Source - Optional source text.
Return Value:
    None. Invalid inputs leave no output beyond best-effort NUL termination.
--*/
{
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }
    Destination[0] = L'\0';
    if (Source == NULL) {
        return;
    }
    (VOID)RtlStringCchCopyW(Destination, DestinationChars, Source);
    Destination[DestinationChars - 1UL] = L'\0';
}

static VOID
KswordStorageCopyUnicode(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_opt_ PCUNICODE_STRING Source
    )
/*++
Routine Description:
    Copy a bounded UNICODE_STRING into a fixed protocol buffer without assuming
    that the source is NUL terminated.
Arguments:
    Destination - Fixed protocol buffer to receive text.
    DestinationChars - Destination capacity in WCHAR units.
    Source - Optional kernel UNICODE_STRING.
Return Value:
    None. Empty or invalid sources produce an empty string.
--*/
{
    ULONG charsToCopy = 0UL;
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }
    Destination[0] = L'\0';
    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0U) {
        return;
    }
    charsToCopy = (ULONG)(Source->Length / sizeof(WCHAR));
    if (charsToCopy >= DestinationChars) {
        charsToCopy = DestinationChars - 1UL;
    }
    RtlCopyMemory(Destination, Source->Buffer, (SIZE_T)charsToCopy * sizeof(WCHAR));
    Destination[charsToCopy] = L'\0';
}

static VOID
KswordStorageFormatDetail(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_z_ PCWSTR FormatText,
    ...
    )
/*++
Routine Description:
    Format one diagnostic detail string for a bounded protocol row.
Arguments:
    Destination - Fixed protocol buffer to receive formatted text.
    DestinationChars - Destination capacity in WCHAR units.
    FormatText - printf-style wide format string.
    ... - Format arguments.
Return Value:
    None. Formatting failures leave a terminated best-effort buffer.
--*/
{
    va_list arguments;
    if (Destination == NULL || DestinationChars == 0UL || FormatText == NULL) {
        return;
    }
    Destination[0] = L'\0';
    va_start(arguments, FormatText);
    (VOID)RtlStringCbVPrintfW(Destination, (SIZE_T)DestinationChars * sizeof(WCHAR), FormatText, arguments);
    va_end(arguments);
    Destination[DestinationChars - 1UL] = L'\0';
}

static BOOLEAN
KswordStorageDriverNameIsFvevol(
    _In_reads_(TextChars) const WCHAR* Text,
    _In_ ULONG TextChars
    )
/*++
Routine Description:
    Check whether a bounded DriverObject name identifies fvevol.
Arguments:
    Text - Bounded driver-name buffer.
    TextChars - Maximum number of WCHARs to inspect.
Return Value:
    TRUE for names containing fvevol; otherwise FALSE.
--*/
{
    ULONG index = 0UL;
    if (Text == NULL || TextChars == 0UL) {
        return FALSE;
    }
    while (index + 5UL < TextChars && Text[index] != L'\0') {
        WCHAR c0 = RtlDowncaseUnicodeChar(Text[index]);
        WCHAR c1 = RtlDowncaseUnicodeChar(Text[index + 1UL]);
        WCHAR c2 = RtlDowncaseUnicodeChar(Text[index + 2UL]);
        WCHAR c3 = RtlDowncaseUnicodeChar(Text[index + 3UL]);
        WCHAR c4 = RtlDowncaseUnicodeChar(Text[index + 4UL]);
        WCHAR c5 = RtlDowncaseUnicodeChar(Text[index + 5UL]);
        if (c0 == L'f' && c1 == L'v' && c2 == L'e' && c3 == L'v' && c4 == L'o' && c5 == L'l') {
            return TRUE;
        }
        ++index;
    }
    return FALSE;
}

static KSW_STORAGE_LIMITS
KswordStorageMakeLimits(
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request
    )
/*++
Routine Description:
    Normalize caller-supplied row and depth budgets to conservative hard caps.
Arguments:
    Request - Optional request packet.
Return Value:
    Normalized limits used by the read-only collectors.
--*/
{
    KSW_STORAGE_LIMITS limits;
    limits.MaxRows = KSWORD_ARK_STORAGE_DEFAULT_MAX_ROWS;
    limits.MaxDepth = KSWORD_ARK_STORAGE_DEFAULT_STACK_DEPTH;
    if (Request != NULL && Request->maxRows != 0UL) {
        limits.MaxRows = Request->maxRows;
    }
    if (Request != NULL && Request->maxDepth != 0UL) {
        limits.MaxDepth = Request->maxDepth;
    }
    if (limits.MaxRows > KSWORD_ARK_STORAGE_HARD_MAX_ROWS) {
        limits.MaxRows = KSWORD_ARK_STORAGE_HARD_MAX_ROWS;
    }
    if (limits.MaxDepth > KSWORD_ARK_STORAGE_HARD_STACK_DEPTH) {
        limits.MaxDepth = KSWORD_ARK_STORAGE_HARD_STACK_DEPTH;
    }
    return limits;
}

static NTSTATUS
KswordStorageBuildRequestPath(
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ UNICODE_STRING* Path
    )
/*++
Routine Description:
    Validate and expose the optional request NT volume path as a bounded
    UNICODE_STRING.
Arguments:
    Request - Optional storage audit request.
    Path - Receives the bounded path view when present.
Return Value:
    STATUS_SUCCESS when a non-empty path is present; STATUS_NOT_FOUND when the
    caller did not provide a path; STATUS_INVALID_PARAMETER for malformed input.
--*/
{
    if (Path == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Path, sizeof(*Path));
    if (Request == NULL || Request->volumePathLengthChars == 0U) {
        return STATUS_NOT_FOUND;
    }
    if (Request->volumePathLengthChars >= KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }
    Path->Buffer = (PWCHAR)Request->volumePath;
    Path->Length = (USHORT)(Request->volumePathLengthChars * sizeof(WCHAR));
    Path->MaximumLength = Path->Length;
    return STATUS_SUCCESS;
}

static ULONG
KswordStorageComputeCapacityRows(
    _In_ size_t OutputBufferLength,
    _In_ size_t HeaderBytes,
    _In_ size_t RowBytes
    )
/*++
Routine Description:
    Convert an output buffer size into the number of variable rows that fit.
Arguments:
    OutputBufferLength - Caller output buffer length.
    HeaderBytes - Response header size without the first row.
    RowBytes - Size of one row.
Return Value:
    Number of rows that fit after the response header.
--*/
{
    if (OutputBufferLength <= HeaderBytes || RowBytes == 0U) {
        return 0UL;
    }
    return (ULONG)((OutputBufferLength - HeaderBytes) / RowBytes);
}

static NTSTATUS
KswordStorageReferenceVolumeDevice(
    _In_ const UNICODE_STRING* VolumePath,
    _Outptr_ PFILE_OBJECT* FileObjectOut,
    _Outptr_ PDEVICE_OBJECT* DeviceObjectOut
    )
/*++
Routine Description:
    Reference a volume device object by NT path using IoGetDeviceObjectPointer.
    The routine requests only read-attribute access and never sends state
    changing controls to the target volume.
Arguments:
    VolumePath - NT path supplied by the caller.
    FileObjectOut - Receives the referenced file object that owns the device
        reference lifetime.
    DeviceObjectOut - Receives the related device object.
Return Value:
    NTSTATUS from validation or IoGetDeviceObjectPointer.
--*/
{
    if (VolumePath == NULL || VolumePath->Buffer == NULL || FileObjectOut == NULL || DeviceObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FileObjectOut = NULL;
    *DeviceObjectOut = NULL;
    return IoGetDeviceObjectPointer((PUNICODE_STRING)VolumePath, FILE_READ_ATTRIBUTES, FileObjectOut, DeviceObjectOut);
}

static VOID
KswordStorageAppendStackRow(
    _Inout_ KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE* Response,
    _In_ ULONG CapacityRows,
    _In_ ULONG StackIndex,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PCUNICODE_STRING RequestPath,
    _Inout_ KSW_STORAGE_STACK_RESULT* Result
    )
/*++
Routine Description:
    Append one read-only device stack row and update derived FVE position flags.
Arguments:
    Response - Volume stack response under construction.
    CapacityRows - Number of rows that fit in the caller output buffer.
    StackIndex - Zero-based stack walk index.
    DeviceObject - Device object sampled for this row.
    RequestPath - Optional request path copied into each row for correlation.
    Result - Mutable aggregate counters and flags.
Return Value:
    None. Rows beyond capacity are counted and marked truncated.
--*/
{
    KSWORD_ARK_VOLUME_STACK_ROW* row = NULL;
    if (Response == NULL || Result == NULL || DeviceObject == NULL) {
        return;
    }
    Result->TotalRows += 1UL;
    if (Result->ReturnedRows >= CapacityRows) {
        Result->Truncated = TRUE;
        Result->ResponseFlags |= KSWORD_ARK_STORAGE_RISK_STACK_TRUNCATED;
        return;
    }
    row = &Response->rows[Result->ReturnedRows];
    RtlZeroMemory(row, sizeof(*row));
    row->rowType = 1UL;
    row->stackIndex = StackIndex;
    row->deviceType = DeviceObject->DeviceType;
    row->deviceCharacteristics = DeviceObject->Characteristics;
    row->deviceObjectAddress = (ULONGLONG)(ULONG_PTR)DeviceObject;
    row->attachedDeviceAddress = (ULONGLONG)(ULONG_PTR)DeviceObject->AttachedDevice;
    row->driverObjectAddress = (ULONGLONG)(ULONG_PTR)DeviceObject->DriverObject;
    row->confidence = KSWORD_ARK_STORAGE_CONFIDENCE_STACK_DERIVED;
    row->lastStatus = STATUS_SUCCESS;
    row->fieldFlags = KSWORD_ARK_STORAGE_FIELD_VOLUME_DEVICE_PRESENT;
    if (RequestPath != NULL) {
        KswordStorageCopyUnicode(row->volumeDeviceName, KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS, RequestPath);
        row->fieldFlags |= KSWORD_ARK_STORAGE_FIELD_NT_DEVICE_PATH_PRESENT;
    }
    if (DeviceObject->DriverObject != NULL) {
        KswordStorageCopyUnicode(row->driverName, KSWORD_ARK_STORAGE_DRIVER_NAME_CHARS, &DeviceObject->DriverObject->DriverName);
        row->fieldFlags |= KSWORD_ARK_STORAGE_FIELD_DRIVER_NAME_PRESENT;
    }
    if (KswordStorageDriverNameIsFvevol(row->driverName, KSWORD_ARK_STORAGE_DRIVER_NAME_CHARS)) {
        row->fieldFlags |= KSWORD_ARK_STORAGE_FIELD_FVEVOL_PRESENT;
        Result->FvevolPresent = KSWORD_ARK_STORAGE_FVE_STATUS_PRESENT;
        if (Result->FvevolPosition == KSW_STORAGE_FVEVOL_ABSENT_INDEX) {
            Result->FvevolPosition = StackIndex;
        }
    }
    KswordStorageFormatDetail(row->detail, KSWORD_ARK_STORAGE_DETAIL_CHARS, L"Read-only stack row captured from DeviceObject=0x%p.", DeviceObject);
    Result->ReturnedRows += 1UL;
}

static NTSTATUS
KswordStorageCollectVolumeStackIntoResponse(
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Inout_ KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE* Response,
    _In_ ULONG CapacityRows,
    _Out_ KSW_STORAGE_STACK_RESULT* Result
    )
/*++
Routine Description:
    Resolve an optional volume path and walk the attached/lower device stack in
    a bounded read-only fashion.
Arguments:
    Request - Optional storage audit request with an NT volume path filter.
    Response - Response whose rows receive stack entries.
    CapacityRows - Number of row slots available in Response.
    Result - Receives aggregate status and fvevol position information.
Return Value:
    STATUS_SUCCESS for completed or supported-empty output; otherwise the first
    resolution or stack-walk failure status.
--*/
{
    PFILE_OBJECT fileObject = NULL;
    PDEVICE_OBJECT baseDevice = NULL;
    PDEVICE_OBJECT currentDevice = NULL;
    PDEVICE_OBJECT lowerDevice = NULL;
    UNICODE_STRING requestPath;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG depth = 0UL;
    ULONG index = 0UL;
    KSW_STORAGE_LIMITS limits = KswordStorageMakeLimits(Request);

    if (Response == NULL || Result == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Result, sizeof(*Result));
    Result->FvevolPresent = KSWORD_ARK_STORAGE_FVE_STATUS_NOT_PRESENT;
    Result->FvevolPosition = KSW_STORAGE_FVEVOL_ABSENT_INDEX;
    Result->LastStatus = STATUS_SUCCESS;

    status = KswordStorageBuildRequestPath(Request, &requestPath);
    if (status == STATUS_NOT_FOUND) {
        Result->LastStatus = status;
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        Result->LastStatus = status;
        return status;
    }

    status = KswordStorageReferenceVolumeDevice(&requestPath, &fileObject, &baseDevice);
    if (!NT_SUCCESS(status)) {
        Result->LastStatus = status;
        return status;
    }

    currentDevice = IoGetAttachedDeviceReference(baseDevice);
    while (currentDevice != NULL && depth < limits.MaxDepth) {
        lowerDevice = IoGetLowerDeviceObject(currentDevice);
        KswordStorageAppendStackRow(Response, CapacityRows, index, currentDevice, &requestPath, Result);
        if (Result->ReturnedRows != 0UL && Result->ReturnedRows <= CapacityRows) {
            Response->rows[Result->ReturnedRows - 1UL].lowerDeviceAddress = (ULONGLONG)(ULONG_PTR)lowerDevice;
        }
        ObDereferenceObject(currentDevice);
        currentDevice = lowerDevice;
        lowerDevice = NULL;
        ++depth;
        ++index;
    }

    if (currentDevice != NULL) {
        Result->Truncated = TRUE;
        Result->ResponseFlags |= KSWORD_ARK_STORAGE_RISK_STACK_TRUNCATED;
        ObDereferenceObject(currentDevice);
    }
    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    if (Result->FvevolPresent != KSWORD_ARK_STORAGE_FVE_STATUS_PRESENT) {
        Result->ResponseFlags |= KSWORD_ARK_STORAGE_RISK_FVEVOL_ABSENT;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKStorageQueryVolumeStackAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Build a bounded read-only volume device stack response for an optional NT
    volume path filter.
Arguments:
    OutputBuffer - Response buffer supplied by WDF.
    OutputBufferLength - Response buffer length.
    Request - Optional storage audit request.
    BytesWrittenOut - Receives the number of response bytes written.
Return Value:
    NTSTATUS from validation or stack collection.
--*/
{
    const size_t headerBytes = KSW_STORAGE_RESPONSE_HEADER_SIZE(KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE, KSWORD_ARK_VOLUME_STACK_ROW);
    KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE* response = (KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE*)OutputBuffer;
    KSW_STORAGE_LIMITS limits = KswordStorageMakeLimits(Request);
    KSW_STORAGE_STACK_RESULT result;
    ULONG capacityRows = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL || OutputBufferLength < headerBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    *BytesWrittenOut = 0;
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    capacityRows = KswordStorageComputeCapacityRows(OutputBufferLength, headerBytes, sizeof(KSWORD_ARK_VOLUME_STACK_ROW));
    if (capacityRows > limits.MaxRows) {
        capacityRows = limits.MaxRows;
    }
    response->version = KSWORD_ARK_STORAGE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->rowSize = sizeof(KSWORD_ARK_VOLUME_STACK_ROW);
    response->maxRows = limits.MaxRows;
    response->fvevolPosition = KSW_STORAGE_FVEVOL_ABSENT_INDEX;

    status = KswordStorageCollectVolumeStackIntoResponse(Request, response, capacityRows, &result);
    response->lastStatus = result.LastStatus;
    response->responseFlags = result.ResponseFlags;
    response->fieldFlags = result.FieldFlags;
    response->totalRows = result.TotalRows;
    response->returnedRows = result.ReturnedRows;
    response->fvevolPresent = result.FvevolPresent;
    response->fvevolPosition = result.FvevolPosition;
    response->queryStatus = (result.ReturnedRows == 0UL) ? KSWORD_ARK_STORAGE_QUERY_STATUS_EMPTY : KSWORD_ARK_STORAGE_QUERY_STATUS_OK;
    if (result.Truncated) {
        response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_PARTIAL;
    }
    *BytesWrittenOut = headerBytes + ((size_t)response->returnedRows * sizeof(KSWORD_ARK_VOLUME_STACK_ROW));
    return status;
}

NTSTATUS
KswordARKStorageQueryBitLockerFveAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Return a safe BitLocker/FVE status row derived only from the visible volume
    stack position of fvevol. No key or protector payload is read or serialized.
Arguments:
    OutputBuffer - Response buffer supplied by WDF.
    OutputBufferLength - Response buffer length.
    Request - Optional storage audit request with a volume path filter.
    BytesWrittenOut - Receives the number of response bytes written.
Return Value:
    STATUS_SUCCESS for safe output, or buffer validation status.
--*/
{
    const size_t headerBytes = KSW_STORAGE_RESPONSE_HEADER_SIZE(KSWORD_ARK_QUERY_BITLOCKER_FVE_RESPONSE, KSWORD_ARK_BITLOCKER_FVE_ROW);
    KSWORD_ARK_QUERY_BITLOCKER_FVE_RESPONSE* response = (KSWORD_ARK_QUERY_BITLOCKER_FVE_RESPONSE*)OutputBuffer;
    KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE* stackResponse = NULL;
    KSW_STORAGE_STACK_RESULT stackResult;
    UNICODE_STRING requestPath;
    ULONG capacityRows = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL || OutputBufferLength < headerBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    *BytesWrittenOut = 0;
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    capacityRows = KswordStorageComputeCapacityRows(OutputBufferLength, headerBytes, sizeof(KSWORD_ARK_BITLOCKER_FVE_ROW));
    if (capacityRows == 0UL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    stackResponse = (KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE*)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE) + (KSWORD_ARK_STORAGE_HARD_STACK_DEPTH * sizeof(KSWORD_ARK_VOLUME_STACK_ROW)), KSW_STORAGE_TAG);
#pragma warning(pop)
    if (stackResponse == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(stackResponse, sizeof(KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE) + (KSWORD_ARK_STORAGE_HARD_STACK_DEPTH * sizeof(KSWORD_ARK_VOLUME_STACK_ROW)));

    response->version = KSWORD_ARK_STORAGE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->rowSize = sizeof(KSWORD_ARK_BITLOCKER_FVE_ROW);
    response->maxRows = KswordStorageMakeLimits(Request).MaxRows;
    status = KswordStorageCollectVolumeStackIntoResponse(Request, stackResponse, KSWORD_ARK_STORAGE_HARD_STACK_DEPTH, &stackResult);

    response->lastStatus = stackResult.LastStatus;
    response->returnedRows = 1UL;
    response->totalRows = 1UL;
    response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_OK;
    response->rows[0].fvevolPresent = stackResult.FvevolPresent;
    response->rows[0].fvevolStackPosition = stackResult.FvevolPosition;
    response->rows[0].protectionStatus = KSWORD_ARK_STORAGE_FVE_STATUS_UNKNOWN;
    response->rows[0].conversionStatus = KSWORD_ARK_STORAGE_FVE_STATUS_UNKNOWN;
    response->rows[0].lockStatus = KSWORD_ARK_STORAGE_FVE_STATUS_UNKNOWN;
    response->rows[0].lastStatus = stackResult.LastStatus;
    response->rows[0].confidence = (stackResult.ReturnedRows == 0UL) ? KSWORD_ARK_STORAGE_CONFIDENCE_PARTIAL : KSWORD_ARK_STORAGE_CONFIDENCE_STACK_DERIVED;
    response->rows[0].fieldFlags = KSWORD_ARK_STORAGE_FIELD_STATUS_DERIVED_FROM_STACK;
    response->rows[0].riskFlags = stackResult.ResponseFlags | KSWORD_ARK_STORAGE_RISK_STATUS_UNCONFIRMED;
    if (NT_SUCCESS(KswordStorageBuildRequestPath(Request, &requestPath))) {
        KswordStorageCopyUnicode(response->rows[0].volumeDeviceName, KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS, &requestPath);
        response->rows[0].fieldFlags |= KSWORD_ARK_STORAGE_FIELD_NT_DEVICE_PATH_PRESENT;
    }
    if (stackResult.FvevolPresent == KSWORD_ARK_STORAGE_FVE_STATUS_PRESENT) {
        response->fieldFlags |= KSWORD_ARK_STORAGE_FIELD_FVEVOL_PRESENT;
        response->rows[0].fieldFlags |= KSWORD_ARK_STORAGE_FIELD_FVEVOL_PRESENT;
        KswordStorageFormatDetail(response->rows[0].detail, KSWORD_ARK_STORAGE_DETAIL_CHARS, L"fvevol is present in the read-only device stack; FVE private state and protector material are not read.");
    }
    else {
        KswordStorageFormatDetail(response->rows[0].detail, KSWORD_ARK_STORAGE_DETAIL_CHARS, L"fvevol was not observed or the volume path was unavailable; FVE private state remains unknown by design.");
    }

    ExFreePoolWithTag(stackResponse, KSW_STORAGE_TAG);
    *BytesWrittenOut = headerBytes + sizeof(KSWORD_ARK_BITLOCKER_FVE_ROW);
    return status;
}

NTSTATUS
KswordARKStorageQueryMountMgrMappingAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Enumerate simple drive-letter symbolic links from the DOS device namespace.
    This read-only seed does not parse or modify the MountMgr database.
Arguments:
    OutputBuffer - Response buffer supplied by WDF.
    OutputBufferLength - Response buffer length.
    Request - Optional request; currently used only for common row limits.
    BytesWrittenOut - Receives the number of response bytes written.
Return Value:
    STATUS_SUCCESS for bounded symbolic-link output, or buffer validation status.
--*/
{
    const size_t headerBytes = KSW_STORAGE_RESPONSE_HEADER_SIZE(KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_RESPONSE, KSWORD_ARK_MOUNTMGR_MAPPING_ROW);
    KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_RESPONSE* response = (KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_RESPONSE*)OutputBuffer;
    KSW_STORAGE_LIMITS limits = KswordStorageMakeLimits(Request);
    ULONG capacityRows = 0UL;
    WCHAR linkNameBuffer[] = L"\\DosDevices\\A:";
    WCHAR driveBuffer[] = L"A:";
    WCHAR targetBuffer[KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS];
    ULONG letterIndex = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL || OutputBufferLength < headerBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    *BytesWrittenOut = 0;
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    capacityRows = KswordStorageComputeCapacityRows(OutputBufferLength, headerBytes, sizeof(KSWORD_ARK_MOUNTMGR_MAPPING_ROW));
    if (capacityRows > limits.MaxRows) {
        capacityRows = limits.MaxRows;
    }
    response->version = KSWORD_ARK_STORAGE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->rowSize = sizeof(KSWORD_ARK_MOUNTMGR_MAPPING_ROW);
    response->maxRows = limits.MaxRows;
    response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_EMPTY;

    for (letterIndex = 0UL; letterIndex < 26UL; ++letterIndex) {
        UNICODE_STRING linkName;
        UNICODE_STRING targetName;
        OBJECT_ATTRIBUTES attributes;
        HANDLE linkHandle = NULL;
        NTSTATUS status;
        ULONG rowIndex;

        linkNameBuffer[12] = (WCHAR)(L'A' + letterIndex);
        driveBuffer[0] = (WCHAR)(L'A' + letterIndex);
        RtlInitUnicodeString(&linkName, linkNameBuffer);
        InitializeObjectAttributes(&attributes, &linkName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        status = ZwOpenSymbolicLinkObject(&linkHandle, SYMBOLIC_LINK_QUERY, &attributes);
        if (!NT_SUCCESS(status)) {
            continue;
        }
        RtlZeroMemory(targetBuffer, sizeof(targetBuffer));
        targetName.Buffer = targetBuffer;
        targetName.Length = 0U;
        targetName.MaximumLength = sizeof(targetBuffer);
        status = ZwQuerySymbolicLinkObject(linkHandle, &targetName, NULL);
        ZwClose(linkHandle);
        if (!NT_SUCCESS(status)) {
            continue;
        }

        response->totalRows += 1UL;
        if (response->returnedRows >= capacityRows) {
            response->responseFlags |= KSWORD_ARK_STORAGE_RISK_STACK_TRUNCATED;
            response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_PARTIAL;
            continue;
        }
        rowIndex = response->returnedRows;
        response->returnedRows += 1UL;
        response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_OK;
        response->rows[rowIndex].fieldFlags = KSWORD_ARK_STORAGE_FIELD_DOS_NAME_PRESENT | KSWORD_ARK_STORAGE_FIELD_NT_DEVICE_PATH_PRESENT;
        response->rows[rowIndex].confidence = KSWORD_ARK_STORAGE_CONFIDENCE_STACK_DERIVED;
        response->rows[rowIndex].lastStatus = STATUS_SUCCESS;
        KswordStorageCopyWide(response->rows[rowIndex].driveLetter, KSWORD_ARK_STORAGE_DRIVE_LETTER_CHARS, driveBuffer);
        KswordStorageCopyUnicode(response->rows[rowIndex].ntDevicePath, KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS, &targetName);
        KswordStorageFormatDetail(response->rows[rowIndex].detail, KSWORD_ARK_STORAGE_DETAIL_CHARS, L"Drive-letter symbolic link queried read-only; Volume GUID correlation is left to R3 or future reviewed MountMgr schema.");
    }

    response->fieldFlags = (response->returnedRows == 0UL) ? 0UL : (KSWORD_ARK_STORAGE_FIELD_DOS_NAME_PRESENT | KSWORD_ARK_STORAGE_FIELD_NT_DEVICE_PATH_PRESENT);
    response->lastStatus = STATUS_SUCCESS;
    *BytesWrittenOut = headerBytes + ((size_t)response->returnedRows * sizeof(KSWORD_ARK_MOUNTMGR_MAPPING_ROW));
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordStorageReferenceFileSystemDriver(
    _In_z_ PCWSTR DriverName,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut
    )
/*++
Routine Description:
    Reference one file-system DriverObject by namespace name for read-only
    dispatch and FastIo sampling.
Arguments:
    DriverName - NUL-terminated driver object name.
    DriverObjectOut - Receives a referenced DriverObject on success.
Return Value:
    NTSTATUS from validation or ObReferenceObjectByName.
--*/
{
    UNICODE_STRING objectName;
    if (DriverName == NULL || DriverObjectOut == NULL || IoDriverObjectType == NULL || *IoDriverObjectType == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *DriverObjectOut = NULL;
    RtlInitUnicodeString(&objectName, DriverName);
    return ObReferenceObjectByName(&objectName, OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID*)DriverObjectOut);
}

static VOID
KswordStorageFillOwnerModule(
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONGLONG TargetAddress,
    _Inout_ KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW* Row
    )
/*++
Routine Description:
    Resolve a dispatch target address to a loaded module name using the existing
    SystemModuleInformation helper.
Arguments:
    ModuleInfo - Optional module snapshot.
    TargetAddress - Function pointer being classified.
    Row - Row to receive owner module fields and risk flags.
Return Value:
    None. Unresolved targets are explicitly flagged as unknown.
--*/
{
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* owner = NULL;
    const UCHAR* fileName = NULL;
    ULONG fileNameBytes = 0UL;
    if (Row == NULL || TargetAddress == 0ULL) {
        return;
    }
    owner = KswordARKHookFindModuleForAddress(ModuleInfo, (ULONG_PTR)TargetAddress);
    if (owner == NULL) {
        Row->riskFlags |= KSWORD_ARK_STORAGE_RISK_OWNER_UNKNOWN | KSWORD_ARK_STORAGE_RISK_TARGET_OUTSIDE_MODULES;
        Row->confidence = KSWORD_ARK_STORAGE_CONFIDENCE_PARTIAL;
        return;
    }
    Row->fieldFlags |= KSWORD_ARK_STORAGE_FIELD_OWNER_MODULE_PRESENT;
    Row->ownerModuleBase = (ULONGLONG)(ULONG_PTR)owner->ImageBase;
    Row->ownerModuleSize = owner->ImageSize;
    KswordARKHookGetModuleFileName(owner, &fileName, &fileNameBytes);
    KswordARKHookCopyBoundedAnsiToWide(fileName, fileNameBytes, Row->ownerModuleName, KSWORD_ARK_STORAGE_MODULE_NAME_CHARS);
}

static VOID
KswordStorageAppendFsIntegrityRow(
    _Inout_ KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_RESPONSE* Response,
    _In_ ULONG CapacityRows,
    _In_ ULONG FileSystemKind,
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG SlotType,
    _In_ ULONG SlotIndex,
    _In_ ULONGLONG SlotAddress,
    _In_ ULONGLONG TargetAddress,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_z_ PCWSTR DetailText
    )
/*++
Routine Description:
    Append one DriverObject dispatch or FastIo evidence row to a bounded
    response.
Arguments:
    Response - File-system integrity response under construction.
    CapacityRows - Number of rows available in the response buffer.
    FileSystemKind - Stable index of the file-system driver being sampled.
    DriverObject - Referenced DriverObject being described.
    SlotType - MajorFunction or FastIo slot classifier.
    SlotIndex - Slot index within the classifier.
    SlotAddress - Address of the sampled slot field.
    TargetAddress - Function pointer value read from the slot.
    ModuleInfo - Optional module snapshot used for owner attribution.
    DetailText - Human-readable row detail.
Return Value:
    None. Rows beyond capacity are counted and mark the response partial.
--*/
{
    KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW* row = NULL;
    if (Response == NULL || DriverObject == NULL) {
        return;
    }
    Response->totalRows += 1UL;
    if (Response->returnedRows >= CapacityRows) {
        Response->responseFlags |= KSWORD_ARK_STORAGE_RISK_STACK_TRUNCATED;
        Response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_PARTIAL;
        return;
    }
    row = &Response->rows[Response->returnedRows];
    RtlZeroMemory(row, sizeof(*row));
    row->fileSystemKind = FileSystemKind;
    row->slotType = SlotType;
    row->slotIndex = SlotIndex;
    row->driverObjectAddress = (ULONGLONG)(ULONG_PTR)DriverObject;
    row->driverStart = (ULONGLONG)(ULONG_PTR)DriverObject->DriverStart;
    row->driverSize = DriverObject->DriverSize;
    row->slotAddress = SlotAddress;
    row->targetAddress = TargetAddress;
    row->confidence = KSWORD_ARK_STORAGE_CONFIDENCE_CONFIRMED;
    row->lastStatus = STATUS_SUCCESS;
    row->fieldFlags = KSWORD_ARK_STORAGE_FIELD_DRIVER_NAME_PRESENT;
    KswordStorageCopyUnicode(row->driverName, KSWORD_ARK_STORAGE_DRIVER_NAME_CHARS, &DriverObject->DriverName);
    KswordStorageFillOwnerModule(ModuleInfo, TargetAddress, row);
    KswordStorageCopyWide(row->detail, KSWORD_ARK_STORAGE_DETAIL_CHARS, DetailText);
    Response->returnedRows += 1UL;
}

NTSTATUS
KswordARKStorageQueryFileSystemIntegrityAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Query NTFS/ReFS/FAT/exFAT DriverObject dispatch and FastIo targets in a
    read-only manner. The routine records owner-module evidence but never
    rewrites or disables any pointer.
Arguments:
    OutputBuffer - Response buffer supplied by WDF.
    OutputBufferLength - Response buffer length.
    Request - Optional storage audit request controlling row budget and flags.
    BytesWrittenOut - Receives the number of response bytes written.
Return Value:
    STATUS_SUCCESS for completed or supported-empty output, or buffer status.
--*/
{
    const size_t headerBytes = KSW_STORAGE_RESPONSE_HEADER_SIZE(KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_RESPONSE, KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW);
    KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_RESPONSE* response = (KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_RESPONSE*)OutputBuffer;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_STORAGE_LIMITS limits = KswordStorageMakeLimits(Request);
    ULONG capacityRows = 0UL;
    ULONG fsIndex = 0UL;
    ULONG flags = KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_DEFAULT;
    NTSTATUS moduleStatus;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL || OutputBufferLength < headerBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    *BytesWrittenOut = 0;
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    capacityRows = KswordStorageComputeCapacityRows(OutputBufferLength, headerBytes, sizeof(KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW));
    if (capacityRows > limits.MaxRows) {
        capacityRows = limits.MaxRows;
    }
    if (Request != NULL && Request->flags != 0UL) {
        flags = Request->flags;
    }
    response->version = KSWORD_ARK_STORAGE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->rowSize = sizeof(KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW);
    response->maxRows = limits.MaxRows;
    response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_EMPTY;

    moduleStatus = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = moduleStatus;
    UNREFERENCED_PARAMETER(moduleInfoBytes);

    for (fsIndex = 0UL; fsIndex < RTL_NUMBER_OF(g_KswordStorageFileSystemDriverNames); ++fsIndex) {
        PDRIVER_OBJECT driverObject = NULL;
        NTSTATUS status;

        status = KswordStorageReferenceFileSystemDriver(g_KswordStorageFileSystemDriverNames[fsIndex], &driverObject);
        if (!NT_SUCCESS(status)) {
            continue;
        }

        if ((flags & KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_DISPATCH) != 0UL) {
            ULONG majorIndex;
            for (majorIndex = 0UL; majorIndex <= IRP_MJ_MAXIMUM_FUNCTION; ++majorIndex) {
                PVOID target = (PVOID)driverObject->MajorFunction[majorIndex];
                WCHAR detail[KSWORD_ARK_STORAGE_DETAIL_CHARS];
                KswordStorageFormatDetail(detail, RTL_NUMBER_OF(detail), L"MajorFunction[%lu] sampled read-only.", majorIndex);
                KswordStorageAppendFsIntegrityRow(response, capacityRows, fsIndex, driverObject, KSWORD_ARK_STORAGE_SLOT_TYPE_MAJOR_FUNCTION, majorIndex, (ULONGLONG)(ULONG_PTR)&driverObject->MajorFunction[majorIndex], (ULONGLONG)(ULONG_PTR)target, NT_SUCCESS(moduleStatus) ? moduleInfo : NULL, detail);
            }
        }

        if ((flags & KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_FAST_IO) != 0UL && driverObject->FastIoDispatch != NULL) {
            ULONG fastIndex;
            for (fastIndex = 0UL; fastIndex < RTL_NUMBER_OF(g_KswordStorageFastIoFields); ++fastIndex) {
                const UCHAR* fieldAddress = (const UCHAR*)driverObject->FastIoDispatch + g_KswordStorageFastIoFields[fastIndex].Offset;
                PVOID target = NULL;
                WCHAR detail[KSWORD_ARK_STORAGE_DETAIL_CHARS];
                RtlCopyMemory(&target, fieldAddress, sizeof(target));
                KswordStorageFormatDetail(detail, RTL_NUMBER_OF(detail), L"FastIoDispatch.%S sampled read-only.", g_KswordStorageFastIoFields[fastIndex].Name);
                KswordStorageAppendFsIntegrityRow(response, capacityRows, fsIndex, driverObject, KSWORD_ARK_STORAGE_SLOT_TYPE_FAST_IO, fastIndex, (ULONGLONG)(ULONG_PTR)fieldAddress, (ULONGLONG)(ULONG_PTR)target, NT_SUCCESS(moduleStatus) ? moduleInfo : NULL, detail);
            }
        }

        ObDereferenceObject(driverObject);
    }

    if (moduleInfo != NULL) {
        ExFreePool(moduleInfo);
    }
    if (response->totalRows != 0UL && response->queryStatus == KSWORD_ARK_STORAGE_QUERY_STATUS_EMPTY) {
        response->queryStatus = KSWORD_ARK_STORAGE_QUERY_STATUS_OK;
    }
    *BytesWrittenOut = headerBytes + ((size_t)response->returnedRows * sizeof(KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW));
    return STATUS_SUCCESS;
}
