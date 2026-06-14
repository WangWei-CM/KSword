/*++

Module Name:

    dyndata_query.c

Abstract:

    DynData state snapshot query helpers and IOCTL handlers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_dyndata.h"
#include "ark/ark_dyndata_fields.h"
#include "ark/ark_log.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSW_DYN_FIELDS_RESPONSE_HEADER_SIZE \
    (sizeof(KSW_QUERY_DYN_FIELDS_RESPONSE) - sizeof(KSW_DYN_FIELD_ENTRY))

#define KSW_DYN_PROFILE_IOCTL_POOL_TAG 'pDsK'

typedef PVOID
(NTAPI* KSW_DYN_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

static PVOID
KswordARKDynDataAllocateProfileCopy(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    Allocate the METHOD_BUFFERED input copy used by the PDB profile apply IOCTL.
    The helper prefers ExAllocatePool2 when the running kernel exports it, while
    preserving the existing old-kernel fallback style used by other feature
    modules in this driver.

Arguments:

    BufferBytes - Number of nonpaged bytes required for the copied request.

Return Value:

    A nonpaged allocation on success; NULL on invalid size or allocation failure.

--*/
{
    static volatile LONG allocatorResolved = 0;
    static KSW_DYN_EX_ALLOCATE_POOL2_FN exAllocatePool2Fn = NULL;

    if (BufferBytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&allocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        exAllocatePool2Fn = (KSW_DYN_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }

    if (exAllocatePool2Fn != NULL) {
        return exAllocatePool2Fn(POOL_FLAG_NON_PAGED, BufferBytes, KSW_DYN_PROFILE_IOCTL_POOL_TAG);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSW_DYN_PROFILE_IOCTL_POOL_TAG);
#pragma warning(pop)
}

static ULONG
KswordARKDynDataStatusFlagsFromState(
    _In_ const KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Convert internal boolean DynData state into public KSW_DYN_STATUS_FLAG_* bits.

Arguments:

    State - State snapshot to convert.

Return Value:

    Public status flag bit mask.

--*/
{
    ULONG flags = 0UL;

    if (State == NULL) {
        return 0UL;
    }
    if (State->Initialized) {
        flags |= KSW_DYN_STATUS_FLAG_INITIALIZED;
    }
    if (State->NtosActive) {
        flags |= KSW_DYN_STATUS_FLAG_NTOS_ACTIVE;
    }
    if (State->LxcoreActive) {
        flags |= KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE;
    }
    if (State->ExtraActive) {
        flags |= KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE;
    }
    if (State->PdbProfileActive) {
        flags |= KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE;
    }

    return flags;
}

static VOID
KswordARKDynDataIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one DynData IOCTL diagnostic message.

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string.
    FormatText - printf-style message template.
    ... - Template arguments.

Return Value:

    None. Formatting or queue failures are intentionally ignored.

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

ULONG
KswordARKDynDataBuildFieldEntries(
    _Out_writes_opt_(EntryCapacity) KSW_DYN_FIELD_ENTRY* Entries,
    _In_ ULONG EntryCapacity
    )
/*++

Routine Description:

    Build public field entries from a global DynData state snapshot.

Arguments:

    Entries - Optional destination entry array.
    EntryCapacity - Number of entries that fit in Entries.

Return Value:

    Number of entries copied when Entries is present; otherwise total descriptor
    count so callers can size variable responses.

--*/
{
    KSW_DYN_STATE state;

    if (Entries == NULL || EntryCapacity == 0UL) {
        return KswordARKDynDataCountFieldDescriptors();
    }

    KswordARKDynDataSnapshot(&state);
    return KswordARKDynDataCopyFieldDescriptors(&state, Entries, EntryCapacity);
}

NTSTATUS
KswordARKDynDataQueryStatus(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Write the fixed DynData status response for R3 diagnostics.

Arguments:

    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Writable output byte count.
    BytesWrittenOut - Receives bytes written on success.

Return Value:

    STATUS_SUCCESS when the fixed response is written; otherwise validation
    status.

--*/
{
    KSW_DYN_STATE state;
    KSW_QUERY_DYN_STATUS_RESPONSE* response = NULL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSW_QUERY_DYN_STATUS_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    KswordARKDynDataSnapshot(&state);
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSW_QUERY_DYN_STATUS_RESPONSE*)OutputBuffer;
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_DYNDATA_PROTOCOL_VERSION;
    response->statusFlags = KswordARKDynDataStatusFlagsFromState(&state);
    response->systemInformerDataVersion = state.SystemInformerDataVersion;
    response->systemInformerDataLength = state.SystemInformerDataLength;
    response->lastStatus = (LONG)state.LastStatus;
    response->matchedProfileClass = state.MatchedProfileClass;
    response->matchedProfileOffset = state.MatchedProfileOffset;
    response->matchedFieldsId = state.MatchedFieldsId;
    response->fieldCount = KswordARKDynDataCountFieldDescriptors();
    response->capabilityMask = state.CapabilityMask;
    response->ntoskrnl = state.Ntoskrnl;
    response->lxcore = state.Lxcore;
    RtlCopyMemory(
        response->unavailableReason,
        state.UnavailableReason,
        sizeof(response->unavailableReason));
    response->unavailableReason[KSW_DYN_REASON_CHARS - 1U] = L'\0';

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDynDataQueryFields(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Write a variable-length DynData field descriptor response. The response can
    be partially filled; totalCount tells R3 how many rows exist.

Arguments:

    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Writable output byte count.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when at least the response header is written; otherwise
    validation status.

--*/
{
    KSW_QUERY_DYN_FIELDS_RESPONSE* response = NULL;
    ULONG entryCapacity = 0UL;
    ULONG returnedCount = 0UL;
    ULONG totalCount = KswordARKDynDataCountFieldDescriptors();

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSW_DYN_FIELDS_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSW_QUERY_DYN_FIELDS_RESPONSE*)OutputBuffer;
    response->size = (ULONG)KSW_DYN_FIELDS_RESPONSE_HEADER_SIZE;
    response->version = KSWORD_ARK_DYNDATA_PROTOCOL_VERSION;
    response->totalCount = totalCount;
    response->entrySize = sizeof(KSW_DYN_FIELD_ENTRY);

    entryCapacity = (ULONG)((OutputBufferLength - KSW_DYN_FIELDS_RESPONSE_HEADER_SIZE) / sizeof(KSW_DYN_FIELD_ENTRY));
    if (entryCapacity > 0UL) {
        returnedCount = KswordARKDynDataBuildFieldEntries(response->entries, entryCapacity);
    }

    response->returnedCount = returnedCount;
    response->size = (ULONG)(KSW_DYN_FIELDS_RESPONSE_HEADER_SIZE + ((size_t)returnedCount * sizeof(KSW_DYN_FIELD_ENTRY)));
    *BytesWrittenOut = response->size;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDynDataQueryCapabilities(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Write the fixed DynData capability response for quick R3 feature gating.

Arguments:

    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Writable output byte count.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when the fixed response is written; otherwise validation
    status.

--*/
{
    KSW_DYN_STATE state;
    KSW_QUERY_CAPABILITIES_RESPONSE* response = NULL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSW_QUERY_CAPABILITIES_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    KswordARKDynDataSnapshot(&state);
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSW_QUERY_CAPABILITIES_RESPONSE*)OutputBuffer;
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_DYNDATA_PROTOCOL_VERSION;
    response->statusFlags = KswordARKDynDataStatusFlagsFromState(&state);
    response->capabilityMask = state.CapabilityMask;

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDynDataIoctlQueryStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_DYN_STATUS by returning the fixed status packet.

Arguments:

    Device - WDF device used for log emission.
    Request - Current IOCTL request.
    InputBufferLength - Unused because this query has no input packet.
    OutputBufferLength - Supplied output bytes; WDF retrieval validates minimum.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from output retrieval or status response construction.

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
        sizeof(KSW_QUERY_DYN_STATUS_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDynDataIoctlLog(Device, "Error", "DynData status output buffer invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDynDataQueryStatus(outputBuffer, actualOutputLength, BytesReturned);
    KswordARKDynDataIoctlLog(Device, NT_SUCCESS(status) ? "Info" : "Error", "DynData status query completed: status=0x%08X, bytes=%Iu.", (unsigned int)status, *BytesReturned);
    return status;
}

NTSTATUS
KswordARKDynDataIoctlQueryFields(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS by returning public field rows.

Arguments:

    Device - WDF device used for log emission.
    Request - Current IOCTL request.
    InputBufferLength - Unused because this query has no input packet.
    OutputBufferLength - Supplied output bytes; WDF retrieval validates minimum.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from output retrieval or field response construction.

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
        KSW_DYN_FIELDS_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDynDataIoctlLog(Device, "Error", "DynData fields output buffer invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDynDataQueryFields(outputBuffer, actualOutputLength, BytesReturned);
    KswordARKDynDataIoctlLog(Device, NT_SUCCESS(status) ? "Info" : "Error", "DynData fields query completed: status=0x%08X, bytes=%Iu.", (unsigned int)status, *BytesReturned);
    return status;
}

NTSTATUS
KswordARKDynDataIoctlQueryCapabilities(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_CAPABILITIES by returning quick feature flags.

Arguments:

    Device - WDF device used for log emission.
    Request - Current IOCTL request.
    InputBufferLength - Unused because this query has no input packet.
    OutputBufferLength - Supplied output bytes; WDF retrieval validates minimum.
    BytesReturned - Receives response byte count.

Return Value:

    NTSTATUS from output retrieval or capability response construction.

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
        sizeof(KSW_QUERY_CAPABILITIES_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDynDataIoctlLog(Device, "Error", "DynData capability output buffer invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDynDataQueryCapabilities(outputBuffer, actualOutputLength, BytesReturned);
    KswordARKDynDataIoctlLog(Device, NT_SUCCESS(status) ? "Info" : "Error", "DynData capability query completed: status=0x%08X, bytes=%Iu.", (unsigned int)status, *BytesReturned);
    return status;
}

NTSTATUS
KswordARKDynDataIoctlApplyProfile(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE. The handler performs only WDF
    buffer/access validation and leaves semantic validation to the DynData owner
    so the global state update remains centralized.

Arguments:

    Device - WDF device used for log emission.
    Request - Current IOCTL request.
    InputBufferLength - Supplied input bytes.
    OutputBufferLength - Supplied output bytes.
    BytesReturned - Receives fixed response byte count when a response is built.

Return Value:

    NTSTATUS from access validation, buffer retrieval, or profile application.

--*/
{
    KSW_APPLY_DYN_PROFILE_REQUEST* inputBuffer = NULL;
    KSW_APPLY_DYN_PROFILE_REQUEST* inputCopy = NULL;
    KSW_APPLY_DYN_PROFILE_RESPONSE* outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    size_t copyLength = 0U;
    const size_t maxProfileRequestBytes =
        KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE +
        ((size_t)KSW_DYN_PROFILE_MAX_FIELDS * sizeof(KSW_DYN_PROFILE_FIELD_PACKET));
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKDynDataIoctlLog(Device, "Warn", "DynData apply profile denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE,
        (PVOID*)&inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDynDataIoctlLog(Device, "Error", "DynData apply profile input invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    copyLength = actualInputLength;
    if (copyLength > maxProfileRequestBytes) {
        copyLength = maxProfileRequestBytes;
    }
    inputCopy = (KSW_APPLY_DYN_PROFILE_REQUEST*)KswordARKDynDataAllocateProfileCopy(copyLength);
    if (inputCopy == NULL) {
        KswordARKDynDataIoctlLog(Device, "Error", "DynData apply profile input copy allocation failed, bytes=%Iu.", copyLength);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(inputCopy, inputBuffer, copyLength);

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSW_APPLY_DYN_PROFILE_RESPONSE),
        (PVOID*)&outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(inputCopy, KSW_DYN_PROFILE_IOCTL_POOL_TAG);
        KswordARKDynDataIoctlLog(Device, "Error", "DynData apply profile output invalid: 0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDynDataApplyProfile(
        inputCopy,
        copyLength,
        outputBuffer,
        actualOutputLength,
        BytesReturned);
    ExFreePoolWithTag(inputCopy, KSW_DYN_PROFILE_IOCTL_POOL_TAG);
    KswordARKDynDataIoctlLog(
        Device,
        NT_SUCCESS(status) ? "Info" : "Warn",
        "DynData apply profile completed: status=0x%08X, applied=%lu, rejected=%lu, unknown=%lu, caps=0x%I64X.",
        (unsigned int)status,
        (unsigned long)outputBuffer->appliedFieldCount,
        (unsigned long)outputBuffer->rejectedFieldCount,
        (unsigned long)outputBuffer->unknownFieldCount,
        outputBuffer->capabilityMask);
    if (*BytesReturned >= sizeof(KSW_APPLY_DYN_PROFILE_RESPONSE)) {
        return STATUS_SUCCESS;
    }

    return status;
}
