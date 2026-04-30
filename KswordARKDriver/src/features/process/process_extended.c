/*++

Module Name:

    process_extended.c

Abstract:

    Phase-2 process extended EPROCESS field reader.

Environment:

    Kernel-mode Driver Framework

--*/

#include "process_extended.h"

#include <ntstrsafe.h>

typedef NTSTATUS(NTAPI* KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN)(
    _In_ HANDLE ProcessHandle,
    _In_ ULONG ProcessInformationClass,
    _Out_writes_bytes_(ProcessInformationLength) PVOID ProcessInformation,
    _In_ ULONG ProcessInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

typedef ULONG(NTAPI* KSWORD_PS_GET_PROCESS_SESSION_ID_FN)(
    _In_ PEPROCESS Process
    );

typedef struct _KSWORD_PROCESS_IMAGE_FILE_NAME_INFORMATION
{
    UNICODE_STRING ImageName;
    WCHAR NameBuffer[KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS];
} KSWORD_PROCESS_IMAGE_FILE_NAME_INFORMATION, *PKSWORD_PROCESS_IMAGE_FILE_NAME_INFORMATION;

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION (0x1000)
#endif

#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION (0x0400)
#endif

#ifndef OBJ_KERNEL_HANDLE
#define OBJ_KERNEL_HANDLE 0x00000200L
#endif

#define KSWORD_PROCESS_IMAGE_FILE_NAME_CLASS 27UL

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTKERNELAPI
NTSTATUS
ObOpenObjectByPointer(
    _In_ PVOID Object,
    _In_ ULONG HandleAttributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PHANDLE Handle
    );

static BOOLEAN
KswordARKProcessIsOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Normalize DynData offset availability before any EPROCESS byte read.

Arguments:

    Offset - Candidate offset from the unified DynData state.

Return Value:

    TRUE when the offset can be used, otherwise FALSE.

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static ULONG
KswordARKProcessSourceForOffset(
    _In_ ULONG Offset,
    _In_ ULONG DynDataSource
    )
/*++

Routine Description:

    Convert an available DynData offset and its source into the process IOCTL
    source vocabulary used by R3.

Arguments:

    Offset - Candidate offset.
    DynDataSource - KSW_DYN_FIELD_SOURCE_* value for the field.

Return Value:

    KSWORD_ARK_PROCESS_FIELD_SOURCE_* value.

--*/
{
    if (!KswordARKProcessIsOffsetPresent(Offset)) {
        return KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
    }

    if (DynDataSource == KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN) {
        return KSWORD_ARK_PROCESS_FIELD_SOURCE_RUNTIME_PATTERN;
    }
    if (DynDataSource == KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER) {
        return KSWORD_ARK_PROCESS_FIELD_SOURCE_SYSTEM_INFORMER_DYNDATA;
    }
    return KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
}

static ULONG
KswordARKProcessNormalizeProtocolOffset(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Convert driver-side DynData sentinel values into the process IOCTL sentinel.

Arguments:

    Offset - Raw offset from KSW_DYN_STATE.

Return Value:

    Usable offset, or KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE.

--*/
{
    if (!KswordARKProcessIsOffsetPresent(Offset)) {
        return KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
    }

    return Offset;
}

static NTSTATUS
KswordARKProcessReadByteField(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG Offset,
    _Out_ UCHAR* ValueOut
    )
/*++

Routine Description:

    Safely read one UCHAR from EPROCESS using a DynData supplied offset.

Arguments:

    ProcessObject - Target EPROCESS pointer.
    Offset - Field offset inside EPROCESS.
    ValueOut - Receives the byte value.

Return Value:

    STATUS_SUCCESS or the structured-exception code.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKProcessIsOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        RtlCopyMemory(ValueOut, (PUCHAR)ProcessObject + Offset, sizeof(*ValueOut));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    return status;
}

static NTSTATUS
KswordARKProcessReadPointerField(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG Offset,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    Safely read one pointer-sized field from EPROCESS and expose it as ULONG64.

Arguments:

    ProcessObject - Target EPROCESS pointer.
    Offset - Field offset inside EPROCESS.
    ValueOut - Receives the pointer value as an integer.

Return Value:

    STATUS_SUCCESS or the structured-exception code.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID pointerValue = NULL;

    if (ProcessObject == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKProcessIsOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        RtlCopyMemory(&pointerValue, (PUCHAR)ProcessObject + Offset, sizeof(pointerValue));
        *ValueOut = (ULONG64)(ULONG_PTR)pointerValue;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    return status;
}

static KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN
KswordARKProcessResolveZwQueryInformationProcess(
    VOID
    )
/*++

Routine Description:

    Resolve ZwQueryInformationProcess dynamically for full image path queries.

Arguments:

    None.

Return Value:

    Function pointer when exported, otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwQueryInformationProcess");
    return (KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_PS_GET_PROCESS_SESSION_ID_FN
KswordARKProcessResolvePsGetProcessSessionId(
    VOID
    )
/*++

Routine Description:

    Resolve the public PsGetProcessSessionId routine dynamically.

Arguments:

    None.

Return Value:

    Function pointer when present, otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetProcessSessionId");
    return (KSWORD_PS_GET_PROCESS_SESSION_ID_FN)MmGetSystemRoutineAddress(&routineName);
}

static VOID
KswordARKProcessPopulateSessionId(
    _Inout_ KSWORD_ARK_PROCESS_ENTRY* Entry,
    _In_ PEPROCESS ProcessObject
    )
/*++

Routine Description:

    Fill the process session ID through a public kernel API when available.

Arguments:

    Entry - Process entry being populated.
    ProcessObject - Target EPROCESS pointer.

Return Value:

    None. Missing routine leaves the field unavailable.

--*/
{
    KSWORD_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId = NULL;

    if (Entry == NULL || ProcessObject == NULL) {
        return;
    }

    Entry->sessionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
    psGetProcessSessionId = KswordARKProcessResolvePsGetProcessSessionId();
    if (psGetProcessSessionId == NULL) {
        return;
    }

    __try {
        Entry->sessionId = psGetProcessSessionId(ProcessObject);
        Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_SESSION_PRESENT;
        Entry->sessionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_PUBLIC_API;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Entry->sessionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
    }
}

static VOID
KswordARKProcessCopyImagePathToEntry(
    _Inout_ KSWORD_ARK_PROCESS_ENTRY* Entry,
    _In_reads_(SourceChars) const WCHAR* SourceText,
    _In_ ULONG SourceChars
    )
/*++

Routine Description:

    Copy a bounded UTF-16 image path into the shared process entry.

Arguments:

    Entry - Output process entry.
    SourceText - UTF-16 source path.
    SourceChars - Source length in UTF-16 code units, excluding terminator.

Return Value:

    None.

--*/
{
    ULONG copyChars = 0UL;

    if (Entry == NULL || SourceText == NULL || SourceChars == 0UL) {
        return;
    }

    copyChars = SourceChars;
    if (copyChars >= KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS) {
        copyChars = KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS - 1UL;
    }

    RtlCopyMemory(
        Entry->imagePath,
        SourceText,
        (SIZE_T)copyChars * sizeof(WCHAR));
    Entry->imagePath[copyChars] = 0U;
    Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_IMAGE_PATH_PRESENT;
    Entry->imagePathSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_PUBLIC_API;
}

static VOID
KswordARKProcessPopulateImagePath(
    _Inout_ KSWORD_ARK_PROCESS_ENTRY* Entry,
    _In_ PEPROCESS ProcessObject
    )
/*++

Routine Description:

    Query the full image path using the public process-information API.

Arguments:

    Entry - Process entry being populated.
    ProcessObject - Target EPROCESS pointer.

Return Value:

    None. Missing image path leaves source as unavailable.

--*/
{
    KSWORD_ZW_QUERY_INFORMATION_PROCESS_FN zwQueryInformationProcess = NULL;
    KSWORD_PROCESS_IMAGE_FILE_NAME_INFORMATION imageInformation;
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG returnedBytes = 0UL;

    if (Entry == NULL || ProcessObject == NULL) {
        return;
    }

    Entry->imagePathSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
    zwQueryInformationProcess = KswordARKProcessResolveZwQueryInformationProcess();
    if (zwQueryInformationProcess == NULL) {
        return;
    }

    status = ObOpenObjectByPointer(
        ProcessObject,
        OBJ_KERNEL_HANDLE,
        NULL,
        PROCESS_QUERY_LIMITED_INFORMATION,
        *PsProcessType,
        KernelMode,
        &processHandle);
    if (!NT_SUCCESS(status)) {
        status = ObOpenObjectByPointer(
            ProcessObject,
            OBJ_KERNEL_HANDLE,
            NULL,
            PROCESS_QUERY_INFORMATION,
            *PsProcessType,
            KernelMode,
            &processHandle);
    }
    if (!NT_SUCCESS(status)) {
        return;
    }

    RtlZeroMemory(&imageInformation, sizeof(imageInformation));
    status = zwQueryInformationProcess(
        processHandle,
        KSWORD_PROCESS_IMAGE_FILE_NAME_CLASS,
        &imageInformation,
        sizeof(imageInformation),
        &returnedBytes);
    ZwClose(processHandle);

    if (!NT_SUCCESS(status)) {
        return;
    }
    if (imageInformation.ImageName.Buffer == NULL ||
        imageInformation.ImageName.Length == 0U) {
        return;
    }

    KswordARKProcessCopyImagePathToEntry(
        Entry,
        imageInformation.ImageName.Buffer,
        (ULONG)(imageInformation.ImageName.Length / sizeof(WCHAR)));
}

static VOID
KswordARKProcessMarkReadFailure(
    _Inout_ KSWORD_ARK_PROCESS_ENTRY* Entry,
    _In_ NTSTATUS ReadStatus
    )
/*++

Routine Description:

    Record that an optional kernel-field read failed without aborting process
    enumeration.

Arguments:

    Entry - Output process entry.
    ReadStatus - Optional read status.

Return Value:

    None.

--*/
{
    if (Entry == NULL || NT_SUCCESS(ReadStatus)) {
        return;
    }

    Entry->r0Status = KSWORD_ARK_PROCESS_R0_STATUS_READ_FAILED;
}

VOID
KswordARKProcessPopulateExtendedEntry(
    _Inout_ KSWORD_ARK_PROCESS_ENTRY* Entry,
    _In_ PEPROCESS ProcessObject
    )
/*++

Routine Description:

    Populate Phase-2 process fields from public APIs and unified DynData offsets.

Arguments:

    Entry - Entry with v1 PID/name fields already initialized.
    ProcessObject - EPROCESS pointer for the target process.

Return Value:

    None. Optional failures are represented through flags and R0 status.

--*/
{
    KSW_DYN_STATE dynState;
    NTSTATUS readStatus = STATUS_SUCCESS;

    if (Entry == NULL || ProcessObject == NULL) {
        return;
    }

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    Entry->r0Status = KSWORD_ARK_PROCESS_R0_STATUS_DYNDATA_MISSING;
    Entry->sessionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
    Entry->dynDataCapabilityMask = dynState.CapabilityMask;
    Entry->protectionOffset =
        KswordARKProcessNormalizeProtocolOffset(dynState.Kernel.EpProtection);
    Entry->signatureLevelOffset =
        KswordARKProcessNormalizeProtocolOffset(dynState.Kernel.EpSignatureLevel);
    Entry->sectionSignatureLevelOffset =
        KswordARKProcessNormalizeProtocolOffset(dynState.Kernel.EpSectionSignatureLevel);
    Entry->objectTableOffset =
        KswordARKProcessNormalizeProtocolOffset(dynState.Kernel.EpObjectTable);
    Entry->sectionObjectOffset =
        KswordARKProcessNormalizeProtocolOffset(dynState.Kernel.EpSectionObject);

    KswordARKProcessPopulateSessionId(Entry, ProcessObject);
    KswordARKProcessPopulateImagePath(Entry, ProcessObject);

    Entry->protectionSource = KswordARKProcessSourceForOffset(
        dynState.Kernel.EpProtection,
        KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN);
    Entry->signatureLevelSource = KswordARKProcessSourceForOffset(
        dynState.Kernel.EpSignatureLevel,
        KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN);
    Entry->sectionSignatureLevelSource = KswordARKProcessSourceForOffset(
        dynState.Kernel.EpSectionSignatureLevel,
        KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN);
    Entry->objectTableSource = KswordARKProcessSourceForOffset(
        dynState.Kernel.EpObjectTable,
        KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER);
    Entry->sectionObjectSource = KswordARKProcessSourceForOffset(
        dynState.Kernel.EpSectionObject,
        KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER);

    if ((dynState.CapabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) ==
        KSW_CAP_PROCESS_PROTECTION_PATCH) {
        readStatus = KswordARKProcessReadByteField(
            ProcessObject,
            dynState.Kernel.EpProtection,
            &Entry->protection);
        if (NT_SUCCESS(readStatus)) {
            Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT;
        }
        KswordARKProcessMarkReadFailure(Entry, readStatus);

        readStatus = KswordARKProcessReadByteField(
            ProcessObject,
            dynState.Kernel.EpSignatureLevel,
            &Entry->signatureLevel);
        if (NT_SUCCESS(readStatus)) {
            Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_SIGNATURE_LEVEL_PRESENT;
        }
        KswordARKProcessMarkReadFailure(Entry, readStatus);

        readStatus = KswordARKProcessReadByteField(
            ProcessObject,
            dynState.Kernel.EpSectionSignatureLevel,
            &Entry->sectionSignatureLevel);
        if (NT_SUCCESS(readStatus)) {
            Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_SECTION_SIGNATURE_LEVEL_PRESENT;
        }
        KswordARKProcessMarkReadFailure(Entry, readStatus);
    }

    if (KswordARKProcessIsOffsetPresent(dynState.Kernel.EpObjectTable)) {
        Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_AVAILABLE;
        readStatus = KswordARKProcessReadPointerField(
            ProcessObject,
            dynState.Kernel.EpObjectTable,
            &Entry->objectTableAddress);
        if (NT_SUCCESS(readStatus) && Entry->objectTableAddress != 0ULL) {
            Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_VALUE_PRESENT;
        }
        KswordARKProcessMarkReadFailure(Entry, readStatus);
    }

    if (KswordARKProcessIsOffsetPresent(dynState.Kernel.EpSectionObject)) {
        Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_AVAILABLE;
        readStatus = KswordARKProcessReadPointerField(
            ProcessObject,
            dynState.Kernel.EpSectionObject,
            &Entry->sectionObjectAddress);
        if (NT_SUCCESS(readStatus) && Entry->sectionObjectAddress != 0ULL) {
            Entry->fieldFlags |= KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_VALUE_PRESENT;
        }
        KswordARKProcessMarkReadFailure(Entry, readStatus);
    }

    if (Entry->r0Status != KSWORD_ARK_PROCESS_R0_STATUS_READ_FAILED) {
        if (Entry->fieldFlags & (
            KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT |
            KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_AVAILABLE |
            KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_AVAILABLE)) {
            Entry->r0Status = KSWORD_ARK_PROCESS_R0_STATUS_OK;
        }
        else if (Entry->fieldFlags & (
            KSWORD_ARK_PROCESS_FIELD_SESSION_PRESENT |
            KSWORD_ARK_PROCESS_FIELD_IMAGE_PATH_PRESENT)) {
            Entry->r0Status = KSWORD_ARK_PROCESS_R0_STATUS_PARTIAL;
        }
    }
}

NTSTATUS
KswordARKProcessPatchProtectionByDynData(
    _In_ ULONG ProcessId,
    _In_ UCHAR ProtectionLevel,
    _In_ UCHAR SignatureLevel,
    _In_ UCHAR SectionSignatureLevel
    )
/*++

Routine Description:

    Patch EPROCESS protection bytes by using the unified DynData state.

Arguments:

    ProcessId - Target process ID.
    ProtectionLevel - Target PS_PROTECTION raw byte.
    SignatureLevel - Target process signature level.
    SectionSignatureLevel - Target section signature level.

Return Value:

    STATUS_SUCCESS or a defensive failure status.

--*/
{
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);
    if ((dynState.CapabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) !=
        KSW_CAP_PROCESS_PROTECTION_PATCH) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKProcessIsOffsetPresent(dynState.Kernel.EpProtection) ||
        !KswordARKProcessIsOffsetPresent(dynState.Kernel.EpSignatureLevel) ||
        !KswordARKProcessIsOffsetPresent(dynState.Kernel.EpSectionSignatureLevel)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        PUCHAR processBase = (PUCHAR)processObject;
        UCHAR* protectionByte = processBase + dynState.Kernel.EpProtection;
        UCHAR* signatureByte = processBase + dynState.Kernel.EpSignatureLevel;
        UCHAR* sectionSignatureByte = processBase + dynState.Kernel.EpSectionSignatureLevel;
        UCHAR verifyProtection = 0U;
        UCHAR verifySignature = 0U;
        UCHAR verifySectionSignature = 0U;

        RtlCopyMemory(protectionByte, &ProtectionLevel, sizeof(ProtectionLevel));
        RtlCopyMemory(signatureByte, &SignatureLevel, sizeof(SignatureLevel));
        RtlCopyMemory(sectionSignatureByte, &SectionSignatureLevel, sizeof(SectionSignatureLevel));
        RtlCopyMemory(&verifyProtection, protectionByte, sizeof(verifyProtection));
        RtlCopyMemory(&verifySignature, signatureByte, sizeof(verifySignature));
        RtlCopyMemory(&verifySectionSignature, sectionSignatureByte, sizeof(verifySectionSignature));

        if (verifyProtection != ProtectionLevel ||
            verifySignature != SignatureLevel ||
            verifySectionSignature != SectionSignatureLevel) {
            status = STATUS_UNSUCCESSFUL;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ObDereferenceObject(processObject);
    return status;
}
