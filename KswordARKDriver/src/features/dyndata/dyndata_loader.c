/*++

Module Name:

    dyndata_loader.c

Abstract:

    Lightweight System Informer DynData loader and Ksword offset activator.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_dyndata.h"
#include "ark/ark_dyndata_fields.h"
#include "ark/ark_log.h"
#include "../../platform/kernel_module_identity.h"
#include "../../platform/process_resolver.h"
#include "ksw_si_dynconfig.h"

#include <ntstrsafe.h>

#define STATUS_SI_DYNDATA_UNSUPPORTED_KERNEL ((NTSTATUS)0xE0020001L)
#define STATUS_SI_DYNDATA_VERSION_MISMATCH   ((NTSTATUS)0xE0020002L)
#define STATUS_SI_DYNDATA_INVALID_LENGTH     ((NTSTATUS)0xE0020003L)

static KSW_DYN_STATE g_KswordDynDataState;

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynNtosNames[] = {
    { "ntoskrnl.exe", KSW_DYN_PROFILE_CLASS_NTOSKRNL },
    { "ntkrnlmp.exe", KSW_DYN_PROFILE_CLASS_NTOSKRNL },
    { "ntkrla57.exe", KSW_DYN_PROFILE_CLASS_NTKRLA57 }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynLxcoreNames[] = {
    { "lxcore.sys", KSW_DYN_PROFILE_CLASS_LXCORE }
};

static VOID
KswordARKDynDataSetReason(
    _Inout_ KSW_DYN_STATE* State,
    _In_z_ PCWSTR ReasonText
    )
/*++

Routine Description:

    Store a bounded human-readable DynData unavailability reason in the state.

Arguments:

    State - Mutable DynData state.
    ReasonText - NUL-terminated reason text.

Return Value:

    None.

--*/
{
    if (State == NULL) {
        return;
    }

    State->UnavailableReason[0] = L'\0';
    if (ReasonText == NULL) {
        return;
    }

    (VOID)RtlStringCchCopyW(
        State->UnavailableReason,
        KSW_DYN_REASON_CHARS,
        ReasonText);
    State->UnavailableReason[KSW_DYN_REASON_CHARS - 1U] = L'\0';
}

static VOID
KswordARKDynDataInitializeKernelOffsets(
    _Out_ KSW_DYN_KERNEL_OFFSETS* Offsets
    )
/*++

Routine Description:

    Initialize every kernel offset to the explicit unavailable sentinel.

Arguments:

    Offsets - Kernel offset block to initialize.

Return Value:

    None.

--*/
{
    if (Offsets == NULL) {
        return;
    }

    Offsets->EpObjectTable = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpSectionObject = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->HtHandleContentionEvent = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->OtName = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->OtIndex = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->ObDecodeShift = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->ObAttributesShift = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EgeGuid = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EreGuidEntry = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtInitialStack = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtStackLimit = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtStackBase = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtKernelStack = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtReadOperationCount = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtWriteOperationCount = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtOtherOperationCount = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtReadTransferCount = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtWriteTransferCount = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtOtherTransferCount = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->MmSectionControlArea = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->MmControlAreaListHead = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->MmControlAreaLock = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcCommunicationInfo = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcOwnerProcess = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcConnectionPort = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcServerCommunicationPort = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcClientCommunicationPort = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcHandleTable = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcHandleTableLock = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcAttributes = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcAttributesFlags = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcPortContext = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcPortObjectLock = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcSequenceNo = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->AlpcState = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpProtection = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpSignatureLevel = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpSectionSignatureLevel = KSW_DYN_OFFSET_UNAVAILABLE;
}

static VOID
KswordARKDynDataInitializeLxcoreOffsets(
    _Out_ KSW_DYN_LXCORE_OFFSETS* Offsets
    )
/*++

Routine Description:

    Initialize every lxcore offset to the explicit unavailable sentinel.

Arguments:

    Offsets - Lxcore offset block to initialize.

Return Value:

    None.

--*/
{
    if (Offsets == NULL) {
        return;
    }

    Offsets->LxPicoProc = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->LxPicoProcInfo = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->LxPicoProcInfoPID = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->LxPicoThrdInfo = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->LxPicoThrdInfoTID = KSW_DYN_OFFSET_UNAVAILABLE;
}

static ULONG
KswordARKDynDataConvertOffset(
    _In_ USHORT SourceOffset
    )
/*++

Routine Description:

    Convert a System Informer USHORT offset into Ksword's ULONG offset format.

Arguments:

    SourceOffset - System Informer raw field value.

Return Value:

    ULONG field offset, or KSW_DYN_OFFSET_UNAVAILABLE for the 0xffff sentinel.

--*/
{
    if (SourceOffset == 0xFFFFU) {
        return KSW_DYN_OFFSET_UNAVAILABLE;
    }

    return (ULONG)SourceOffset;
}

static BOOLEAN
KswordARKDynDataReadBlob(
    _In_ SIZE_T Offset,
    _Out_writes_bytes_(BytesToRead) PVOID Destination,
    _In_ SIZE_T BytesToRead
    )
/*++

Routine Description:

    Copy a bounded object from the vendored DynData byte blob after validating
    the requested range.

Arguments:

    Offset - Byte offset inside KphDynConfig.
    Destination - Destination buffer.
    BytesToRead - Number of bytes to copy.

Return Value:

    TRUE when the requested range is valid; otherwise FALSE.

--*/
{
    if (Destination == NULL) {
        return FALSE;
    }
    if (Offset > (SIZE_T)KphDynConfigLength) {
        return FALSE;
    }
    if (BytesToRead > ((SIZE_T)KphDynConfigLength - Offset)) {
        return FALSE;
    }

    RtlCopyMemory(Destination, KphDynConfig + Offset, BytesToRead);
    return TRUE;
}

static NTSTATUS
KswordARKDynDataReadConfigHeader(
    _Out_ ULONG* VersionOut,
    _Out_ ULONG* CountOut,
    _Out_ SIZE_T* DataOffsetOut,
    _Out_ SIZE_T* FieldBaseOffsetOut
    )
/*++

Routine Description:

    Read the packed System Informer DynData header and compute the data/field
    base offsets without relying on naturally aligned structure reads.

Arguments:

    VersionOut - Receives KPH_DYN_CONFIG.Version.
    CountOut - Receives KPH_DYN_CONFIG.Count.
    DataOffsetOut - Receives byte offset of the KPH_DYN_DATA array.
    FieldBaseOffsetOut - Receives byte offset of the raw field payload block.

Return Value:

    STATUS_SUCCESS when the blob header is valid; otherwise invalid-length or
    version-mismatch status.

--*/
{
    ULONG version = 0UL;
    ULONG count = 0UL;
    SIZE_T dataOffset = FIELD_OFFSET(KPH_DYN_CONFIG, Data);
    SIZE_T countOffset = FIELD_OFFSET(KPH_DYN_CONFIG, Count);
    SIZE_T fieldBaseOffset = 0U;

    if (VersionOut == NULL || CountOut == NULL || DataOffsetOut == NULL || FieldBaseOffsetOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!KswordARKDynDataReadBlob(FIELD_OFFSET(KPH_DYN_CONFIG, Version), &version, sizeof(version)) ||
        !KswordARKDynDataReadBlob(countOffset, &count, sizeof(count))) {
        return STATUS_SI_DYNDATA_INVALID_LENGTH;
    }
    if (version != KPH_DYN_CONFIGURATION_VERSION) {
        return STATUS_SI_DYNDATA_VERSION_MISMATCH;
    }
    if (count == 0UL || count > 100000UL) {
        return STATUS_SI_DYNDATA_INVALID_LENGTH;
    }
    if (dataOffset > (SIZE_T)KphDynConfigLength) {
        return STATUS_SI_DYNDATA_INVALID_LENGTH;
    }
    if ((SIZE_T)count > (((SIZE_T)KphDynConfigLength - dataOffset) / sizeof(KPH_DYN_DATA))) {
        return STATUS_SI_DYNDATA_INVALID_LENGTH;
    }

    fieldBaseOffset = dataOffset + ((SIZE_T)count * sizeof(KPH_DYN_DATA));
    if (fieldBaseOffset > (SIZE_T)KphDynConfigLength) {
        return STATUS_SI_DYNDATA_INVALID_LENGTH;
    }

    *VersionOut = version;
    *CountOut = count;
    *DataOffsetOut = dataOffset;
    *FieldBaseOffsetOut = fieldBaseOffset;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDynDataFindProfile(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* Identity,
    _In_ SIZE_T ExpectedPayloadBytes,
    _Out_ KPH_DYN_DATA* MatchedDataOut,
    _Out_ SIZE_T* PayloadOffsetOut
    )
/*++

Routine Description:

    Locate one exact DynData profile by class, machine, timestamp, and image
    size. Duplicate matches are rejected as data corruption.

Arguments:

    Identity - Loaded module identity packet.
    ExpectedPayloadBytes - Minimum bytes required for the target field body.
    MatchedDataOut - Receives the matched KPH_DYN_DATA row.
    PayloadOffsetOut - Receives the byte offset of the raw field payload.

Return Value:

    STATUS_SUCCESS on exactly one match, STATUS_SI_DYNDATA_UNSUPPORTED_KERNEL on
    no match, or STATUS_SI_DYNDATA_VERSION_MISMATCH on duplicate match.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG version = 0UL;
    ULONG count = 0UL;
    SIZE_T dataOffset = 0U;
    SIZE_T fieldBaseOffset = 0U;
    ULONG index = 0UL;
    ULONG matchCount = 0UL;
    KPH_DYN_DATA matchedData = { 0 };
    SIZE_T matchedPayloadOffset = 0U;

    if (Identity == NULL || MatchedDataOut == NULL || PayloadOffsetOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Identity->present == 0UL) {
        return STATUS_NOT_FOUND;
    }

    status = KswordARKDynDataReadConfigHeader(&version, &count, &dataOffset, &fieldBaseOffset);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    UNREFERENCED_PARAMETER(version);

    for (index = 0UL; index < count; ++index) {
        KPH_DYN_DATA dynData = { 0 };
        SIZE_T entryOffset = dataOffset + ((SIZE_T)index * sizeof(KPH_DYN_DATA));
        SIZE_T payloadOffset = 0U;

        if (!KswordARKDynDataReadBlob(entryOffset, &dynData, sizeof(dynData))) {
            return STATUS_SI_DYNDATA_INVALID_LENGTH;
        }
        if ((ULONG)dynData.Class != Identity->classId ||
            (ULONG)dynData.Machine != Identity->machine ||
            dynData.TimeDateStamp != Identity->timeDateStamp ||
            dynData.SizeOfImage != Identity->sizeOfImage) {
            continue;
        }

        payloadOffset = fieldBaseOffset + (SIZE_T)dynData.Offset;
        if (payloadOffset > (SIZE_T)KphDynConfigLength ||
            ExpectedPayloadBytes > ((SIZE_T)KphDynConfigLength - payloadOffset)) {
            return STATUS_SI_DYNDATA_INVALID_LENGTH;
        }

        matchCount += 1UL;
        matchedData = dynData;
        matchedPayloadOffset = payloadOffset;
    }

    if (matchCount == 0UL) {
        return STATUS_SI_DYNDATA_UNSUPPORTED_KERNEL;
    }
    if (matchCount > 1UL) {
        return STATUS_SI_DYNDATA_VERSION_MISMATCH;
    }

    *MatchedDataOut = matchedData;
    *PayloadOffsetOut = matchedPayloadOffset;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDynDataActivateKernelFields(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* Identity,
    _Inout_ KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Match and convert the current ntoskrnl/ntkrla57 System Informer field body
    into Ksword's internal kernel offset block.

Arguments:

    Identity - Current kernel image identity.
    State - Mutable DynData state that receives converted offsets.

Return Value:

    STATUS_SUCCESS when the kernel profile was activated; otherwise match or
    validation failure status.

--*/
{
    KPH_DYN_DATA dynData = { 0 };
    KPH_DYN_KERNEL_FIELDS fields = { 0 };
    SIZE_T payloadOffset = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (Identity == NULL || State == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKDynDataFindProfile(
        Identity,
        sizeof(fields),
        &dynData,
        &payloadOffset);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!KswordARKDynDataReadBlob(payloadOffset, &fields, sizeof(fields))) {
        return STATUS_SI_DYNDATA_INVALID_LENGTH;
    }

    State->NtosActive = TRUE;
    State->MatchedProfileClass = (ULONG)dynData.Class;
    State->MatchedProfileOffset = dynData.Offset;
    State->MatchedFieldsId = 0UL;
    State->Kernel.EpObjectTable = KswordARKDynDataConvertOffset(fields.EpObjectTable);
    State->Kernel.EpSectionObject = KswordARKDynDataConvertOffset(fields.EpSectionObject);
    State->Kernel.HtHandleContentionEvent = KswordARKDynDataConvertOffset(fields.HtHandleContentionEvent);
    State->Kernel.OtName = KswordARKDynDataConvertOffset(fields.OtName);
    State->Kernel.OtIndex = KswordARKDynDataConvertOffset(fields.OtIndex);
    State->Kernel.ObDecodeShift = KswordARKDynDataConvertOffset(fields.ObDecodeShift);
    State->Kernel.ObAttributesShift = KswordARKDynDataConvertOffset(fields.ObAttributesShift);
    State->Kernel.EgeGuid = KswordARKDynDataConvertOffset(fields.EgeGuid);
    State->Kernel.EreGuidEntry = KswordARKDynDataConvertOffset(fields.EreGuidEntry);
    State->Kernel.KtInitialStack = KswordARKDynDataConvertOffset(fields.KtInitialStack);
    State->Kernel.KtStackLimit = KswordARKDynDataConvertOffset(fields.KtStackLimit);
    State->Kernel.KtStackBase = KswordARKDynDataConvertOffset(fields.KtStackBase);
    State->Kernel.KtKernelStack = KswordARKDynDataConvertOffset(fields.KtKernelStack);
    State->Kernel.KtReadOperationCount = KswordARKDynDataConvertOffset(fields.KtReadOperationCount);
    State->Kernel.KtWriteOperationCount = KswordARKDynDataConvertOffset(fields.KtWriteOperationCount);
    State->Kernel.KtOtherOperationCount = KswordARKDynDataConvertOffset(fields.KtOtherOperationCount);
    State->Kernel.KtReadTransferCount = KswordARKDynDataConvertOffset(fields.KtReadTransferCount);
    State->Kernel.KtWriteTransferCount = KswordARKDynDataConvertOffset(fields.KtWriteTransferCount);
    State->Kernel.KtOtherTransferCount = KswordARKDynDataConvertOffset(fields.KtOtherTransferCount);
    State->Kernel.MmSectionControlArea = KswordARKDynDataConvertOffset(fields.MmSectionControlArea);
    State->Kernel.MmControlAreaListHead = KswordARKDynDataConvertOffset(fields.MmControlAreaListHead);
    State->Kernel.MmControlAreaLock = KswordARKDynDataConvertOffset(fields.MmControlAreaLock);
    State->Kernel.AlpcCommunicationInfo = KswordARKDynDataConvertOffset(fields.AlpcCommunicationInfo);
    State->Kernel.AlpcOwnerProcess = KswordARKDynDataConvertOffset(fields.AlpcOwnerProcess);
    State->Kernel.AlpcConnectionPort = KswordARKDynDataConvertOffset(fields.AlpcConnectionPort);
    State->Kernel.AlpcServerCommunicationPort = KswordARKDynDataConvertOffset(fields.AlpcServerCommunicationPort);
    State->Kernel.AlpcClientCommunicationPort = KswordARKDynDataConvertOffset(fields.AlpcClientCommunicationPort);
    State->Kernel.AlpcHandleTable = KswordARKDynDataConvertOffset(fields.AlpcHandleTable);
    State->Kernel.AlpcHandleTableLock = KswordARKDynDataConvertOffset(fields.AlpcHandleTableLock);
    State->Kernel.AlpcAttributes = KswordARKDynDataConvertOffset(fields.AlpcAttributes);
    State->Kernel.AlpcAttributesFlags = KswordARKDynDataConvertOffset(fields.AlpcAttributesFlags);
    State->Kernel.AlpcPortContext = KswordARKDynDataConvertOffset(fields.AlpcPortContext);
    State->Kernel.AlpcPortObjectLock = KswordARKDynDataConvertOffset(fields.AlpcPortObjectLock);
    State->Kernel.AlpcSequenceNo = KswordARKDynDataConvertOffset(fields.AlpcSequenceNo);
    State->Kernel.AlpcState = KswordARKDynDataConvertOffset(fields.AlpcState);
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDynDataActivateLxcoreFields(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* Identity,
    _Inout_ KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Match and convert optional lxcore.sys DynData fields for WSL/Pico support.

Arguments:

    Identity - Current lxcore.sys identity when the module is loaded.
    State - Mutable DynData state that receives lxcore offsets.

Return Value:

    STATUS_SUCCESS when lxcore fields were activated; STATUS_NOT_FOUND when the
    module is absent; otherwise exact-match failure status.

--*/
{
    KPH_DYN_DATA dynData = { 0 };
    KPH_DYN_LXCORE_FIELDS fields = { 0 };
    SIZE_T payloadOffset = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (Identity == NULL || State == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Identity->present == 0UL) {
        return STATUS_NOT_FOUND;
    }

    status = KswordARKDynDataFindProfile(
        Identity,
        sizeof(fields),
        &dynData,
        &payloadOffset);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!KswordARKDynDataReadBlob(payloadOffset, &fields, sizeof(fields))) {
        return STATUS_SI_DYNDATA_INVALID_LENGTH;
    }

    State->LxcoreActive = TRUE;
    State->LxcoreOffsets.LxPicoProc = KswordARKDynDataConvertOffset(fields.LxPicoProc);
    State->LxcoreOffsets.LxPicoProcInfo = KswordARKDynDataConvertOffset(fields.LxPicoProcInfo);
    State->LxcoreOffsets.LxPicoProcInfoPID = KswordARKDynDataConvertOffset(fields.LxPicoProcInfoPID);
    State->LxcoreOffsets.LxPicoThrdInfo = KswordARKDynDataConvertOffset(fields.LxPicoThrdInfo);
    State->LxcoreOffsets.LxPicoThrdInfoTID = KswordARKDynDataConvertOffset(fields.LxPicoThrdInfoTID);
    UNREFERENCED_PARAMETER(dynData);
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKDynDataStoreRuntimeOffset(
    _In_ LONG ResolvedOffset,
    _Out_ ULONG* DestinationOffset
    )
/*++

Routine Description:

    Store one runtime-pattern offset if the resolver produced a positive value.

Arguments:

    ResolvedOffset - Signed resolver result; negative values mean unavailable.
    DestinationOffset - Offset field to update.

Return Value:

    TRUE when an offset was stored; otherwise FALSE.

--*/
{
    if (DestinationOffset == NULL || ResolvedOffset <= 0) {
        return FALSE;
    }

    *DestinationOffset = (ULONG)ResolvedOffset;
    return TRUE;
}

static VOID
KswordARKDynDataActivateRuntimeOffsets(
    _Inout_ KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Resolve Ksword-specific EPROCESS protection offsets by existing runtime
    pattern helpers and cache them in the same DynData state.

Arguments:

    State - Mutable DynData state.

Return Value:

    None.

--*/
{
    BOOLEAN protectionPresent = FALSE;
    BOOLEAN signaturePresent = FALSE;
    BOOLEAN sectionSignaturePresent = FALSE;

    if (State == NULL) {
        return;
    }

    protectionPresent = KswordARKDynDataStoreRuntimeOffset(
        KswordARKDriverResolveProcessProtectionOffset(),
        &State->Kernel.EpProtection);
    signaturePresent = KswordARKDynDataStoreRuntimeOffset(
        KswordARKDriverResolveProcessSignatureLevelOffset(),
        &State->Kernel.EpSignatureLevel);
    sectionSignaturePresent = KswordARKDynDataStoreRuntimeOffset(
        KswordARKDriverResolveProcessSectionSignatureLevelOffset(),
        &State->Kernel.EpSectionSignatureLevel);

    State->ExtraActive = (protectionPresent && signaturePresent && sectionSignaturePresent) ? TRUE : FALSE;
}

static NTSTATUS
KswordARKDynDataBuildState(
    _Out_ KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Build one immutable DynData state snapshot from loaded module identities,
    the vendored System Informer blob, and Ksword runtime resolvers.

Arguments:

    State - Output state snapshot.

Return Value:

    STATUS_SUCCESS when ntoskrnl DynData was active; otherwise the primary
    ntoskrnl match/identity status. Optional lxcore failure does not override
    successful ntoskrnl activation.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS ntosStatus = STATUS_SUCCESS;
    NTSTATUS lxcoreStatus = STATUS_SUCCESS;
    ULONG configVersion = 0UL;
    ULONG configCount = 0UL;
    SIZE_T dataOffset = 0U;
    SIZE_T fieldBaseOffset = 0U;

    if (State == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(State, sizeof(*State));
    State->SystemInformerDataLength = KphDynConfigLength;
    State->LastStatus = STATUS_UNSUCCESSFUL;
    KswordARKDynDataInitializeKernelOffsets(&State->Kernel);
    KswordARKDynDataInitializeLxcoreOffsets(&State->LxcoreOffsets);
    KswordARKDynDataSetReason(State, L"DynData initialization has not completed.");

    status = KswordARKDynDataReadConfigHeader(
        &configVersion,
        &configCount,
        &dataOffset,
        &fieldBaseOffset);
    if (!NT_SUCCESS(status)) {
        State->LastStatus = status;
        KswordARKDynDataSetReason(State, L"System Informer DynData blob header is invalid.");
        return status;
    }
    State->SystemInformerDataVersion = configVersion;
    State->SystemInformerDataLength = KphDynConfigLength;
    UNREFERENCED_PARAMETER(configCount);
    UNREFERENCED_PARAMETER(dataOffset);
    UNREFERENCED_PARAMETER(fieldBaseOffset);

    ntosStatus = KswordARKQueryKernelModuleIdentity(
        g_KswordDynNtosNames,
        (ULONG)(sizeof(g_KswordDynNtosNames) / sizeof(g_KswordDynNtosNames[0])),
        &State->Ntoskrnl);
    if (NT_SUCCESS(ntosStatus)) {
        ntosStatus = KswordARKDynDataActivateKernelFields(&State->Ntoskrnl, State);
    }

    lxcoreStatus = KswordARKQueryKernelModuleIdentity(
        g_KswordDynLxcoreNames,
        (ULONG)(sizeof(g_KswordDynLxcoreNames) / sizeof(g_KswordDynLxcoreNames[0])),
        &State->Lxcore);
    if (NT_SUCCESS(lxcoreStatus)) {
        lxcoreStatus = KswordARKDynDataActivateLxcoreFields(&State->Lxcore, State);
    }

    KswordARKDynDataActivateRuntimeOffsets(State);
    State->CapabilityMask = KswordARKDynDataComputeCapabilities(State);
    State->Initialized = TRUE;
    State->LastStatus = ntosStatus;

    if (NT_SUCCESS(ntosStatus)) {
        if (State->Lxcore.present != 0UL && !NT_SUCCESS(lxcoreStatus)) {
            KswordARKDynDataSetReason(State, L"ntoskrnl DynData is active; lxcore.sys is loaded but did not match exactly.");
        }
        else {
            KswordARKDynDataSetReason(State, L"DynData profile matched exactly.");
        }
        return STATUS_SUCCESS;
    }

    if (ntosStatus == STATUS_NOT_FOUND) {
        KswordARKDynDataSetReason(State, L"ntoskrnl.exe/ntkrla57.exe module identity was not found.");
    }
    else if (ntosStatus == STATUS_SI_DYNDATA_UNSUPPORTED_KERNEL) {
        KswordARKDynDataSetReason(State, L"No exact System Informer profile matched this kernel image.");
    }
    else {
        KswordARKDynDataSetReason(State, L"DynData profile activation failed before capability gating.");
    }

    return ntosStatus;
}

NTSTATUS
KswordARKDynDataInitialize(
    _In_opt_ WDFDEVICE Device
    )
/*++

Routine Description:

    Initialize the global DynData state. This routine never blocks driver load;
    failures are recorded in the queryable state and reported through logs.

Arguments:

    Device - Optional WDF device used to enqueue a startup diagnostic log.

Return Value:

    STATUS_SUCCESS so callers do not fail driver startup because private offset
    data is unavailable.

--*/
{
    KSW_DYN_STATE newState;
    NTSTATUS stateStatus = STATUS_SUCCESS;
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };

    stateStatus = KswordARKDynDataBuildState(&newState);
    RtlCopyMemory(&g_KswordDynDataState, &newState, sizeof(g_KswordDynDataState));

    if (Device != NULL) {
        (VOID)RtlStringCbPrintfA(
            logMessage,
            sizeof(logMessage),
            "DynData init: ntosActive=%u, lxcoreActive=%u, extraActive=%u, caps=0x%I64X, status=0x%08X.",
            (unsigned int)newState.NtosActive,
            (unsigned int)newState.LxcoreActive,
            (unsigned int)newState.ExtraActive,
            newState.CapabilityMask,
            (unsigned int)stateStatus);
        (VOID)KswordARKDriverEnqueueLogFrame(
            Device,
            NT_SUCCESS(stateStatus) ? "Info" : "Warn",
            logMessage);
    }

    return STATUS_SUCCESS;
}

VOID
KswordARKDynDataUninitialize(
    VOID
    )
/*++

Routine Description:

    Clear the global DynData state during driver unload.

Arguments:

    None.

Return Value:

    None.

--*/
{
    RtlZeroMemory(&g_KswordDynDataState, sizeof(g_KswordDynDataState));
}

VOID
KswordARKDynDataSnapshot(
    _Out_ KSW_DYN_STATE* StateOut
    )
/*++

Routine Description:

    Copy the current global DynData state into a caller-owned buffer.

Arguments:

    StateOut - Receives the global state snapshot.

Return Value:

    None.

--*/
{
    if (StateOut == NULL) {
        return;
    }

    RtlCopyMemory(StateOut, &g_KswordDynDataState, sizeof(*StateOut));
}
