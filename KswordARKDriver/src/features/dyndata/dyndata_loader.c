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
static EX_PUSH_LOCK g_KswordDynDataStateLock;

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
    Offsets->EpUniqueProcessId = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpActiveProcessLinks = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpThreadListHead = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpImageFileName = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EpToken = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EtCid = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EtThreadListEntry = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EtStartAddress = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->EtWin32StartAddress = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KtProcess = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->HtHandleContentionEvent = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->HtTableCode = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->HtHandleCount = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->HteLowValue = KSW_DYN_OFFSET_UNAVAILABLE;
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
    Offsets->KldrInLoadOrderLinks = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KldrDllBase = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KldrSizeOfImage = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KldrFullDllName = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KldrBaseDllName = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->KldrFlags = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->DoDriverStart = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->DoDriverSize = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->DoDriverSection = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->DoMajorFunction = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->DoFastIoDispatch = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->DoDriverUnload = KSW_DYN_OFFSET_UNAVAILABLE;
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

static VOID
KswordARKDynDataInitializeKernelGlobals(
    _Out_ KSW_DYN_KERNEL_GLOBALS* Globals
    )
/*++

Routine Description:

    Initialize every non-callback kernel global RVA slot to the unavailable
    sentinel. These entries are optional PDB profile data for future cross-view,
    driver integrity, and kernel-memory attribution features.

Arguments:

    Globals - Kernel global RVA block to initialize.

Return Value:

    None.

--*/
{
    if (Globals == NULL) {
        return;
    }

    Globals->PspCidTable = KSW_DYN_OFFSET_UNAVAILABLE;
    Globals->PsLoadedModuleList = KSW_DYN_OFFSET_UNAVAILABLE;
    Globals->MmUnloadedDrivers = KSW_DYN_OFFSET_UNAVAILABLE;
    Globals->PiDDBCacheTable = KSW_DYN_OFFSET_UNAVAILABLE;
}

static VOID
KswordARKDynDataInitializeCallbackGlobals(
    _Out_ KSW_DYN_CALLBACK_GLOBALS* Globals
    )
/*++

Routine Description:

    Initialize every callback global RVA slot to the explicit unavailable
    sentinel. PDB profile v2 stores RVAs rather than kernel virtual addresses so
    identity checks stay tied to the loaded ntoskrnl image.

Arguments:

    Globals - Callback global RVA block to initialize.

Return Value:

    None.

--*/
{
    if (Globals == NULL) {
        return;
    }

    Globals->PspCreateProcessNotifyRoutine = KSW_DYN_OFFSET_UNAVAILABLE;
    Globals->PspCreateThreadNotifyRoutine = KSW_DYN_OFFSET_UNAVAILABLE;
    Globals->PspLoadImageNotifyRoutine = KSW_DYN_OFFSET_UNAVAILABLE;
    Globals->PspNotifyEnableMask = KSW_DYN_OFFSET_UNAVAILABLE;
    Globals->CmCallbackListHead = KSW_DYN_OFFSET_UNAVAILABLE;
}

static VOID
KswordARKDynDataInitializeCallbackOffsets(
    _Out_ KSW_DYN_CALLBACK_OFFSETS* Offsets
    )
/*++

Routine Description:

    Initialize every callback structure offset to the explicit unavailable
    sentinel. These offsets are consumed only by callback enumeration/removal
    code after the matching ntoskrnl PDB profile has been applied.

Arguments:

    Offsets - Callback structure offset block to initialize.

Return Value:

    None.

--*/
{
    if (Offsets == NULL) {
        return;
    }

    Offsets->ObjectTypeCallbackList = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->CallbackEntryItemEntryList = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->CallbackEntryItemPreOperation = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->CallbackEntryItemPostOperation = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->CallbackEntryItemOperations = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->CallbackEntryItemCallbackEntry = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->CallbackEntryAltitude = KSW_DYN_OFFSET_UNAVAILABLE;
    Offsets->CallbackEntryRegistrationContext = KSW_DYN_OFFSET_UNAVAILABLE;
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
KswordARKDynDataOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Test whether one normalized DynData offset is usable. The helper is kept in
    the loader because profile application and System Informer conversion both
    need the same sentinel handling before they assign provenance.

Arguments:

    Offset - Normalized ULONG offset value.

Return Value:

    TRUE when the offset can be used; otherwise FALSE.

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static VOID
KswordARKDynDataStoreSourcedOffset(
    _In_ ULONG Offset,
    _In_ ULONG Source,
    _Out_ ULONG* DestinationOffset,
    _Out_ ULONG* DestinationSource
    )
/*++

Routine Description:

    Store a normalized offset and its provenance together. Missing offsets always
    clear the source to UNAVAILABLE so later query paths do not guess from static
    descriptor defaults.

Arguments:

    Offset - Normalized offset value.
    Source - KSW_DYN_FIELD_SOURCE_* value for a present offset.
    DestinationOffset - Mutable offset field.
    DestinationSource - Mutable source field parallel to DestinationOffset.

Return Value:

    None.

--*/
{
    if (DestinationOffset == NULL || DestinationSource == NULL) {
        return;
    }

    *DestinationOffset = Offset;
    *DestinationSource = KswordARKDynDataOffsetPresent(Offset) ? Source : KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
}

static VOID
KswordARKDynDataStoreSystemInformerOffset(
    _In_ USHORT SourceOffset,
    _Out_ ULONG* DestinationOffset,
    _Out_ ULONG* DestinationSource
    )
/*++

Routine Description:

    Convert one System Informer USHORT offset and tag it with System Informer
    provenance when it is present.

Arguments:

    SourceOffset - Raw System Informer offset or 0xffff sentinel.
    DestinationOffset - Mutable normalized offset field.
    DestinationSource - Mutable source field parallel to DestinationOffset.

Return Value:

    None.

--*/
{
    KswordARKDynDataStoreSourcedOffset(
        KswordARKDynDataConvertOffset(SourceOffset),
        KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER,
        DestinationOffset,
        DestinationSource);
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
    KswordARKDynDataStoreSystemInformerOffset(fields.EpObjectTable, &State->Kernel.EpObjectTable, &State->KernelSources.EpObjectTable);
    KswordARKDynDataStoreSystemInformerOffset(fields.EpSectionObject, &State->Kernel.EpSectionObject, &State->KernelSources.EpSectionObject);
    KswordARKDynDataStoreSystemInformerOffset(fields.HtHandleContentionEvent, &State->Kernel.HtHandleContentionEvent, &State->KernelSources.HtHandleContentionEvent);
    KswordARKDynDataStoreSystemInformerOffset(fields.OtName, &State->Kernel.OtName, &State->KernelSources.OtName);
    KswordARKDynDataStoreSystemInformerOffset(fields.OtIndex, &State->Kernel.OtIndex, &State->KernelSources.OtIndex);
    KswordARKDynDataStoreSystemInformerOffset(fields.ObDecodeShift, &State->Kernel.ObDecodeShift, &State->KernelSources.ObDecodeShift);
    KswordARKDynDataStoreSystemInformerOffset(fields.ObAttributesShift, &State->Kernel.ObAttributesShift, &State->KernelSources.ObAttributesShift);
    KswordARKDynDataStoreSystemInformerOffset(fields.EgeGuid, &State->Kernel.EgeGuid, &State->KernelSources.EgeGuid);
    KswordARKDynDataStoreSystemInformerOffset(fields.EreGuidEntry, &State->Kernel.EreGuidEntry, &State->KernelSources.EreGuidEntry);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtInitialStack, &State->Kernel.KtInitialStack, &State->KernelSources.KtInitialStack);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtStackLimit, &State->Kernel.KtStackLimit, &State->KernelSources.KtStackLimit);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtStackBase, &State->Kernel.KtStackBase, &State->KernelSources.KtStackBase);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtKernelStack, &State->Kernel.KtKernelStack, &State->KernelSources.KtKernelStack);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtReadOperationCount, &State->Kernel.KtReadOperationCount, &State->KernelSources.KtReadOperationCount);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtWriteOperationCount, &State->Kernel.KtWriteOperationCount, &State->KernelSources.KtWriteOperationCount);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtOtherOperationCount, &State->Kernel.KtOtherOperationCount, &State->KernelSources.KtOtherOperationCount);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtReadTransferCount, &State->Kernel.KtReadTransferCount, &State->KernelSources.KtReadTransferCount);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtWriteTransferCount, &State->Kernel.KtWriteTransferCount, &State->KernelSources.KtWriteTransferCount);
    KswordARKDynDataStoreSystemInformerOffset(fields.KtOtherTransferCount, &State->Kernel.KtOtherTransferCount, &State->KernelSources.KtOtherTransferCount);
    KswordARKDynDataStoreSystemInformerOffset(fields.MmSectionControlArea, &State->Kernel.MmSectionControlArea, &State->KernelSources.MmSectionControlArea);
    KswordARKDynDataStoreSystemInformerOffset(fields.MmControlAreaListHead, &State->Kernel.MmControlAreaListHead, &State->KernelSources.MmControlAreaListHead);
    KswordARKDynDataStoreSystemInformerOffset(fields.MmControlAreaLock, &State->Kernel.MmControlAreaLock, &State->KernelSources.MmControlAreaLock);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcCommunicationInfo, &State->Kernel.AlpcCommunicationInfo, &State->KernelSources.AlpcCommunicationInfo);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcOwnerProcess, &State->Kernel.AlpcOwnerProcess, &State->KernelSources.AlpcOwnerProcess);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcConnectionPort, &State->Kernel.AlpcConnectionPort, &State->KernelSources.AlpcConnectionPort);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcServerCommunicationPort, &State->Kernel.AlpcServerCommunicationPort, &State->KernelSources.AlpcServerCommunicationPort);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcClientCommunicationPort, &State->Kernel.AlpcClientCommunicationPort, &State->KernelSources.AlpcClientCommunicationPort);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcHandleTable, &State->Kernel.AlpcHandleTable, &State->KernelSources.AlpcHandleTable);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcHandleTableLock, &State->Kernel.AlpcHandleTableLock, &State->KernelSources.AlpcHandleTableLock);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcAttributes, &State->Kernel.AlpcAttributes, &State->KernelSources.AlpcAttributes);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcAttributesFlags, &State->Kernel.AlpcAttributesFlags, &State->KernelSources.AlpcAttributesFlags);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcPortContext, &State->Kernel.AlpcPortContext, &State->KernelSources.AlpcPortContext);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcPortObjectLock, &State->Kernel.AlpcPortObjectLock, &State->KernelSources.AlpcPortObjectLock);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcSequenceNo, &State->Kernel.AlpcSequenceNo, &State->KernelSources.AlpcSequenceNo);
    KswordARKDynDataStoreSystemInformerOffset(fields.AlpcState, &State->Kernel.AlpcState, &State->KernelSources.AlpcState);
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
    KswordARKDynDataStoreSystemInformerOffset(fields.LxPicoProc, &State->LxcoreOffsets.LxPicoProc, &State->LxcoreSources.LxPicoProc);
    KswordARKDynDataStoreSystemInformerOffset(fields.LxPicoProcInfo, &State->LxcoreOffsets.LxPicoProcInfo, &State->LxcoreSources.LxPicoProcInfo);
    KswordARKDynDataStoreSystemInformerOffset(fields.LxPicoProcInfoPID, &State->LxcoreOffsets.LxPicoProcInfoPID, &State->LxcoreSources.LxPicoProcInfoPID);
    KswordARKDynDataStoreSystemInformerOffset(fields.LxPicoThrdInfo, &State->LxcoreOffsets.LxPicoThrdInfo, &State->LxcoreSources.LxPicoThrdInfo);
    KswordARKDynDataStoreSystemInformerOffset(fields.LxPicoThrdInfoTID, &State->LxcoreOffsets.LxPicoThrdInfoTID, &State->LxcoreSources.LxPicoThrdInfoTID);
    UNREFERENCED_PARAMETER(dynData);
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKDynDataStoreRuntimeOffset(
    _In_ LONG ResolvedOffset,
    _Out_ ULONG* DestinationOffset,
    _Out_ ULONG* DestinationSource
    )
/*++

Routine Description:

    Store one runtime-pattern offset and tag it with runtime provenance if the
    resolver produced a positive value.

Arguments:

    ResolvedOffset - Signed resolver result; negative values mean unavailable.
    DestinationOffset - Offset field to update.
    DestinationSource - Source field parallel to DestinationOffset.

Return Value:

    TRUE when an offset was stored; otherwise FALSE.

--*/
{
    if (DestinationOffset == NULL || DestinationSource == NULL || ResolvedOffset <= 0) {
        return FALSE;
    }

    KswordARKDynDataStoreSourcedOffset(
        (ULONG)ResolvedOffset,
        KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN,
        DestinationOffset,
        DestinationSource);
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
        &State->Kernel.EpProtection,
        &State->KernelSources.EpProtection);
    signaturePresent = KswordARKDynDataStoreRuntimeOffset(
        KswordARKDriverResolveProcessSignatureLevelOffset(),
        &State->Kernel.EpSignatureLevel,
        &State->KernelSources.EpSignatureLevel);
    sectionSignaturePresent = KswordARKDynDataStoreRuntimeOffset(
        KswordARKDriverResolveProcessSectionSignatureLevelOffset(),
        &State->Kernel.EpSectionSignatureLevel,
        &State->KernelSources.EpSectionSignatureLevel);

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
    KswordARKDynDataInitializeKernelGlobals(&State->KernelGlobals);
    KswordARKDynDataInitializeCallbackGlobals(&State->CallbackGlobals);
    KswordARKDynDataInitializeCallbackOffsets(&State->CallbackOffsets);
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

    ExInitializePushLock(&g_KswordDynDataStateLock);
    KswordARKDynDataV4Initialize();
    stateStatus = KswordARKDynDataBuildState(&newState);
    ExAcquirePushLockExclusive(&g_KswordDynDataStateLock);
    RtlCopyMemory(&g_KswordDynDataState, &newState, sizeof(g_KswordDynDataState));
    ExReleasePushLockExclusive(&g_KswordDynDataStateLock);

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
    KswordARKDynDataV4Uninitialize();
    ExAcquirePushLockExclusive(&g_KswordDynDataStateLock);
    RtlZeroMemory(&g_KswordDynDataState, sizeof(g_KswordDynDataState));
    ExReleasePushLockExclusive(&g_KswordDynDataStateLock);
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

    ExAcquirePushLockShared(&g_KswordDynDataStateLock);
    RtlCopyMemory(StateOut, &g_KswordDynDataState, sizeof(*StateOut));
    ExReleasePushLockShared(&g_KswordDynDataStateLock);
}

static ULONG
KswordARKDynDataPublicStatusFlags(
    _In_ const KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Convert one internal DynData state into public status bits. The loader keeps
    this private copy so profile-apply responses can be produced without
    reaching into dyndata_query.c static helpers.

Arguments:

    State - DynData state to summarize.

Return Value:

    KSW_DYN_STATUS_FLAG_* bit mask.

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
    if (State->CallbackProfileActive) {
        flags |= KSW_DYN_STATUS_FLAG_CALLBACK_PROFILE_ACTIVE;
    }

    return flags;
}

static VOID
KswordARKDynDataApplySetMessage(
    _Out_writes_(KSW_DYN_REASON_CHARS) WCHAR* Destination,
    _In_z_ PCWSTR Message
    )
/*++

Routine Description:

    Store a bounded profile-apply message for R3 diagnostics.

Arguments:

    Destination - Fixed response message buffer.
    Message - NUL-terminated message text.

Return Value:

    None.

--*/
{
    if (Destination == NULL) {
        return;
    }

    Destination[0] = L'\0';
    if (Message == NULL) {
        return;
    }

    (VOID)RtlStringCchCopyW(Destination, KSW_DYN_REASON_CHARS, Message);
    Destination[KSW_DYN_REASON_CHARS - 1U] = L'\0';
}

static BOOLEAN
KswordARKDynDataIdentityMatches(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* CurrentIdentity,
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* RequestedIdentity
    )
/*++

Routine Description:

    Compare the identity tuple that makes a PDB profile safe to apply. Image
    base and module name are diagnostic only; class, machine, timestamp, and
    image size are the exact-match key.

Arguments:

    CurrentIdentity - Module identity captured by R0.
    RequestedIdentity - Module identity supplied by the R3 JSON profile manager.

Return Value:

    TRUE when the request targets the currently loaded kernel image.

--*/
{
    if (CurrentIdentity == NULL || RequestedIdentity == NULL) {
        return FALSE;
    }
    if (CurrentIdentity->present == 0UL || RequestedIdentity->present == 0UL) {
        return FALSE;
    }
    if (RequestedIdentity->classId != KSW_DYN_PROFILE_CLASS_NTOSKRNL &&
        RequestedIdentity->classId != KSW_DYN_PROFILE_CLASS_NTKRLA57) {
        return FALSE;
    }

    return CurrentIdentity->classId == RequestedIdentity->classId &&
        CurrentIdentity->machine == RequestedIdentity->machine &&
        CurrentIdentity->timeDateStamp == RequestedIdentity->timeDateStamp &&
        CurrentIdentity->sizeOfImage == RequestedIdentity->sizeOfImage;
}

static BOOLEAN
KswordARKDynDataKernelFieldPointers(
    _In_ ULONG FieldId,
    _Inout_ KSW_DYN_STATE* State,
    _Outptr_ ULONG** OffsetOut,
    _Outptr_ ULONG** SourceOut
    )
/*++

Routine Description:

    Resolve one public ntoskrnl DynData field ID into the internal offset and
    source slots. v1 profile application intentionally handles only kernel
    fields; lxcore remains System Informer sourced until a separate profile class
    is added.

Arguments:

    FieldId - KSW_DYN_FIELD_ID_* value supplied by R3.
    State - Mutable state snapshot that owns destination fields.
    OffsetOut - Receives a pointer to the target offset slot.
    SourceOut - Receives a pointer to the target source slot.

Return Value:

    TRUE when FieldId is known and belongs to the ntoskrnl field set.

--*/
{
    if (State == NULL || OffsetOut == NULL || SourceOut == NULL) {
        return FALSE;
    }

    *OffsetOut = NULL;
    *SourceOut = NULL;

    switch (FieldId) {
    case KSW_DYN_FIELD_ID_EP_OBJECT_TABLE:
        *OffsetOut = &State->Kernel.EpObjectTable;
        *SourceOut = &State->KernelSources.EpObjectTable;
        break;
    case KSW_DYN_FIELD_ID_EP_SECTION_OBJECT:
        *OffsetOut = &State->Kernel.EpSectionObject;
        *SourceOut = &State->KernelSources.EpSectionObject;
        break;
    case KSW_DYN_FIELD_ID_EP_UNIQUE_PROCESS_ID:
        *OffsetOut = &State->Kernel.EpUniqueProcessId;
        *SourceOut = &State->KernelSources.EpUniqueProcessId;
        break;
    case KSW_DYN_FIELD_ID_EP_ACTIVE_PROCESS_LINKS:
        *OffsetOut = &State->Kernel.EpActiveProcessLinks;
        *SourceOut = &State->KernelSources.EpActiveProcessLinks;
        break;
    case KSW_DYN_FIELD_ID_EP_THREAD_LIST_HEAD:
        *OffsetOut = &State->Kernel.EpThreadListHead;
        *SourceOut = &State->KernelSources.EpThreadListHead;
        break;
    case KSW_DYN_FIELD_ID_EP_IMAGE_FILE_NAME:
        *OffsetOut = &State->Kernel.EpImageFileName;
        *SourceOut = &State->KernelSources.EpImageFileName;
        break;
    case KSW_DYN_FIELD_ID_EP_TOKEN:
        *OffsetOut = &State->Kernel.EpToken;
        *SourceOut = &State->KernelSources.EpToken;
        break;
    case KSW_DYN_FIELD_ID_ET_CID:
        *OffsetOut = &State->Kernel.EtCid;
        *SourceOut = &State->KernelSources.EtCid;
        break;
    case KSW_DYN_FIELD_ID_ET_THREAD_LIST_ENTRY:
        *OffsetOut = &State->Kernel.EtThreadListEntry;
        *SourceOut = &State->KernelSources.EtThreadListEntry;
        break;
    case KSW_DYN_FIELD_ID_ET_START_ADDRESS:
        *OffsetOut = &State->Kernel.EtStartAddress;
        *SourceOut = &State->KernelSources.EtStartAddress;
        break;
    case KSW_DYN_FIELD_ID_ET_WIN32_START_ADDRESS:
        *OffsetOut = &State->Kernel.EtWin32StartAddress;
        *SourceOut = &State->KernelSources.EtWin32StartAddress;
        break;
    case KSW_DYN_FIELD_ID_KT_PROCESS:
        *OffsetOut = &State->Kernel.KtProcess;
        *SourceOut = &State->KernelSources.KtProcess;
        break;
    case KSW_DYN_FIELD_ID_HT_HANDLE_CONTENTION_EVENT:
        *OffsetOut = &State->Kernel.HtHandleContentionEvent;
        *SourceOut = &State->KernelSources.HtHandleContentionEvent;
        break;
    case KSW_DYN_FIELD_ID_HT_TABLE_CODE:
        *OffsetOut = &State->Kernel.HtTableCode;
        *SourceOut = &State->KernelSources.HtTableCode;
        break;
    case KSW_DYN_FIELD_ID_HT_HANDLE_COUNT:
        *OffsetOut = &State->Kernel.HtHandleCount;
        *SourceOut = &State->KernelSources.HtHandleCount;
        break;
    case KSW_DYN_FIELD_ID_HTE_LOW_VALUE:
        *OffsetOut = &State->Kernel.HteLowValue;
        *SourceOut = &State->KernelSources.HteLowValue;
        break;
    case KSW_DYN_FIELD_ID_OT_NAME:
        *OffsetOut = &State->Kernel.OtName;
        *SourceOut = &State->KernelSources.OtName;
        break;
    case KSW_DYN_FIELD_ID_OT_INDEX:
        *OffsetOut = &State->Kernel.OtIndex;
        *SourceOut = &State->KernelSources.OtIndex;
        break;
    case KSW_DYN_FIELD_ID_OB_DECODE_SHIFT:
        *OffsetOut = &State->Kernel.ObDecodeShift;
        *SourceOut = &State->KernelSources.ObDecodeShift;
        break;
    case KSW_DYN_FIELD_ID_OB_ATTRIBUTES_SHIFT:
        *OffsetOut = &State->Kernel.ObAttributesShift;
        *SourceOut = &State->KernelSources.ObAttributesShift;
        break;
    case KSW_DYN_FIELD_ID_KT_INITIAL_STACK:
        *OffsetOut = &State->Kernel.KtInitialStack;
        *SourceOut = &State->KernelSources.KtInitialStack;
        break;
    case KSW_DYN_FIELD_ID_KT_STACK_LIMIT:
        *OffsetOut = &State->Kernel.KtStackLimit;
        *SourceOut = &State->KernelSources.KtStackLimit;
        break;
    case KSW_DYN_FIELD_ID_KT_STACK_BASE:
        *OffsetOut = &State->Kernel.KtStackBase;
        *SourceOut = &State->KernelSources.KtStackBase;
        break;
    case KSW_DYN_FIELD_ID_KT_KERNEL_STACK:
        *OffsetOut = &State->Kernel.KtKernelStack;
        *SourceOut = &State->KernelSources.KtKernelStack;
        break;
    case KSW_DYN_FIELD_ID_KT_READ_OPERATION_COUNT:
        *OffsetOut = &State->Kernel.KtReadOperationCount;
        *SourceOut = &State->KernelSources.KtReadOperationCount;
        break;
    case KSW_DYN_FIELD_ID_KT_WRITE_OPERATION_COUNT:
        *OffsetOut = &State->Kernel.KtWriteOperationCount;
        *SourceOut = &State->KernelSources.KtWriteOperationCount;
        break;
    case KSW_DYN_FIELD_ID_KT_OTHER_OPERATION_COUNT:
        *OffsetOut = &State->Kernel.KtOtherOperationCount;
        *SourceOut = &State->KernelSources.KtOtherOperationCount;
        break;
    case KSW_DYN_FIELD_ID_KT_READ_TRANSFER_COUNT:
        *OffsetOut = &State->Kernel.KtReadTransferCount;
        *SourceOut = &State->KernelSources.KtReadTransferCount;
        break;
    case KSW_DYN_FIELD_ID_KT_WRITE_TRANSFER_COUNT:
        *OffsetOut = &State->Kernel.KtWriteTransferCount;
        *SourceOut = &State->KernelSources.KtWriteTransferCount;
        break;
    case KSW_DYN_FIELD_ID_KT_OTHER_TRANSFER_COUNT:
        *OffsetOut = &State->Kernel.KtOtherTransferCount;
        *SourceOut = &State->KernelSources.KtOtherTransferCount;
        break;
    case KSW_DYN_FIELD_ID_MM_SECTION_CONTROL_AREA:
        *OffsetOut = &State->Kernel.MmSectionControlArea;
        *SourceOut = &State->KernelSources.MmSectionControlArea;
        break;
    case KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LIST_HEAD:
        *OffsetOut = &State->Kernel.MmControlAreaListHead;
        *SourceOut = &State->KernelSources.MmControlAreaListHead;
        break;
    case KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LOCK:
        *OffsetOut = &State->Kernel.MmControlAreaLock;
        *SourceOut = &State->KernelSources.MmControlAreaLock;
        break;
    case KSW_DYN_FIELD_ID_ALPC_COMMUNICATION_INFO:
        *OffsetOut = &State->Kernel.AlpcCommunicationInfo;
        *SourceOut = &State->KernelSources.AlpcCommunicationInfo;
        break;
    case KSW_DYN_FIELD_ID_ALPC_OWNER_PROCESS:
        *OffsetOut = &State->Kernel.AlpcOwnerProcess;
        *SourceOut = &State->KernelSources.AlpcOwnerProcess;
        break;
    case KSW_DYN_FIELD_ID_ALPC_CONNECTION_PORT:
        *OffsetOut = &State->Kernel.AlpcConnectionPort;
        *SourceOut = &State->KernelSources.AlpcConnectionPort;
        break;
    case KSW_DYN_FIELD_ID_ALPC_SERVER_COMMUNICATION_PORT:
        *OffsetOut = &State->Kernel.AlpcServerCommunicationPort;
        *SourceOut = &State->KernelSources.AlpcServerCommunicationPort;
        break;
    case KSW_DYN_FIELD_ID_ALPC_CLIENT_COMMUNICATION_PORT:
        *OffsetOut = &State->Kernel.AlpcClientCommunicationPort;
        *SourceOut = &State->KernelSources.AlpcClientCommunicationPort;
        break;
    case KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE:
        *OffsetOut = &State->Kernel.AlpcHandleTable;
        *SourceOut = &State->KernelSources.AlpcHandleTable;
        break;
    case KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE_LOCK:
        *OffsetOut = &State->Kernel.AlpcHandleTableLock;
        *SourceOut = &State->KernelSources.AlpcHandleTableLock;
        break;
    case KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES:
        *OffsetOut = &State->Kernel.AlpcAttributes;
        *SourceOut = &State->KernelSources.AlpcAttributes;
        break;
    case KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES_FLAGS:
        *OffsetOut = &State->Kernel.AlpcAttributesFlags;
        *SourceOut = &State->KernelSources.AlpcAttributesFlags;
        break;
    case KSW_DYN_FIELD_ID_ALPC_PORT_CONTEXT:
        *OffsetOut = &State->Kernel.AlpcPortContext;
        *SourceOut = &State->KernelSources.AlpcPortContext;
        break;
    case KSW_DYN_FIELD_ID_ALPC_PORT_OBJECT_LOCK:
        *OffsetOut = &State->Kernel.AlpcPortObjectLock;
        *SourceOut = &State->KernelSources.AlpcPortObjectLock;
        break;
    case KSW_DYN_FIELD_ID_ALPC_SEQUENCE_NO:
        *OffsetOut = &State->Kernel.AlpcSequenceNo;
        *SourceOut = &State->KernelSources.AlpcSequenceNo;
        break;
    case KSW_DYN_FIELD_ID_ALPC_STATE:
        *OffsetOut = &State->Kernel.AlpcState;
        *SourceOut = &State->KernelSources.AlpcState;
        break;
    case KSW_DYN_FIELD_ID_EP_PROTECTION:
        *OffsetOut = &State->Kernel.EpProtection;
        *SourceOut = &State->KernelSources.EpProtection;
        break;
    case KSW_DYN_FIELD_ID_EP_SIGNATURE_LEVEL:
        *OffsetOut = &State->Kernel.EpSignatureLevel;
        *SourceOut = &State->KernelSources.EpSignatureLevel;
        break;
    case KSW_DYN_FIELD_ID_EP_SECTION_SIGNATURE_LEVEL:
        *OffsetOut = &State->Kernel.EpSectionSignatureLevel;
        *SourceOut = &State->KernelSources.EpSectionSignatureLevel;
        break;
    case KSW_DYN_FIELD_ID_EGE_GUID:
        *OffsetOut = &State->Kernel.EgeGuid;
        *SourceOut = &State->KernelSources.EgeGuid;
        break;
    case KSW_DYN_FIELD_ID_ERE_GUID_ENTRY:
        *OffsetOut = &State->Kernel.EreGuidEntry;
        *SourceOut = &State->KernelSources.EreGuidEntry;
        break;
    case KSW_DYN_FIELD_ID_KLDR_IN_LOAD_ORDER_LINKS:
        *OffsetOut = &State->Kernel.KldrInLoadOrderLinks;
        *SourceOut = &State->KernelSources.KldrInLoadOrderLinks;
        break;
    case KSW_DYN_FIELD_ID_KLDR_DLL_BASE:
        *OffsetOut = &State->Kernel.KldrDllBase;
        *SourceOut = &State->KernelSources.KldrDllBase;
        break;
    case KSW_DYN_FIELD_ID_KLDR_SIZE_OF_IMAGE:
        *OffsetOut = &State->Kernel.KldrSizeOfImage;
        *SourceOut = &State->KernelSources.KldrSizeOfImage;
        break;
    case KSW_DYN_FIELD_ID_KLDR_FULL_DLL_NAME:
        *OffsetOut = &State->Kernel.KldrFullDllName;
        *SourceOut = &State->KernelSources.KldrFullDllName;
        break;
    case KSW_DYN_FIELD_ID_KLDR_BASE_DLL_NAME:
        *OffsetOut = &State->Kernel.KldrBaseDllName;
        *SourceOut = &State->KernelSources.KldrBaseDllName;
        break;
    case KSW_DYN_FIELD_ID_KLDR_FLAGS:
        *OffsetOut = &State->Kernel.KldrFlags;
        *SourceOut = &State->KernelSources.KldrFlags;
        break;
    case KSW_DYN_FIELD_ID_DO_DRIVER_START:
        *OffsetOut = &State->Kernel.DoDriverStart;
        *SourceOut = &State->KernelSources.DoDriverStart;
        break;
    case KSW_DYN_FIELD_ID_DO_DRIVER_SIZE:
        *OffsetOut = &State->Kernel.DoDriverSize;
        *SourceOut = &State->KernelSources.DoDriverSize;
        break;
    case KSW_DYN_FIELD_ID_DO_DRIVER_SECTION:
        *OffsetOut = &State->Kernel.DoDriverSection;
        *SourceOut = &State->KernelSources.DoDriverSection;
        break;
    case KSW_DYN_FIELD_ID_DO_MAJOR_FUNCTION:
        *OffsetOut = &State->Kernel.DoMajorFunction;
        *SourceOut = &State->KernelSources.DoMajorFunction;
        break;
    case KSW_DYN_FIELD_ID_DO_FAST_IO_DISPATCH:
        *OffsetOut = &State->Kernel.DoFastIoDispatch;
        *SourceOut = &State->KernelSources.DoFastIoDispatch;
        break;
    case KSW_DYN_FIELD_ID_DO_DRIVER_UNLOAD:
        *OffsetOut = &State->Kernel.DoDriverUnload;
        *SourceOut = &State->KernelSources.DoDriverUnload;
        break;
    default:
        return FALSE;
    }

    return (*OffsetOut != NULL && *SourceOut != NULL) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKDynDataKernelGlobalRvaPointers(
    _In_ ULONG FieldId,
    _Inout_ KSW_DYN_STATE* State,
    _Outptr_ ULONG** RvaOut,
    _Outptr_ ULONG** SourceOut
    )
/*++

Routine Description:

    Resolve one non-callback ntoskrnl global item ID into the internal
    RVA/source slots. These optional globals support future process/thread
    cross-view walks, module list validation, unloaded-driver attribution, and
    PiDDB cache checks.

Arguments:

    FieldId - KSW_DYN_FIELD_ID_KG_* global item ID supplied by R3.
    State - Mutable state snapshot that owns destination fields.
    RvaOut - Receives a pointer to the target RVA slot.
    SourceOut - Receives a pointer to the target source slot.

Return Value:

    TRUE when the item ID is a supported kernel global RVA; otherwise FALSE.

--*/
{
    if (State == NULL || RvaOut == NULL || SourceOut == NULL) {
        return FALSE;
    }

    *RvaOut = NULL;
    *SourceOut = NULL;

    switch (FieldId) {
    case KSW_DYN_FIELD_ID_KG_PSP_CID_TABLE:
        *RvaOut = &State->KernelGlobals.PspCidTable;
        *SourceOut = &State->KernelGlobalSources.PspCidTable;
        break;
    case KSW_DYN_FIELD_ID_KG_PS_LOADED_MODULE_LIST:
        *RvaOut = &State->KernelGlobals.PsLoadedModuleList;
        *SourceOut = &State->KernelGlobalSources.PsLoadedModuleList;
        break;
    case KSW_DYN_FIELD_ID_KG_MM_UNLOADED_DRIVERS:
        *RvaOut = &State->KernelGlobals.MmUnloadedDrivers;
        *SourceOut = &State->KernelGlobalSources.MmUnloadedDrivers;
        break;
    case KSW_DYN_FIELD_ID_KG_PIDDB_CACHE_TABLE:
        *RvaOut = &State->KernelGlobals.PiDDBCacheTable;
        *SourceOut = &State->KernelGlobalSources.PiDDBCacheTable;
        break;
    default:
        return FALSE;
    }

    return (*RvaOut != NULL && *SourceOut != NULL) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKDynDataCallbackGlobalPointers(
    _In_ ULONG FieldId,
    _Inout_ KSW_DYN_STATE* State,
    _Outptr_ ULONG** RvaOut,
    _Outptr_ ULONG** SourceOut
    )
/*++

Routine Description:

    Resolve one callback global item ID into the internal RVA/source slots. EX
    profile requests carry global addresses as RVAs so the driver can validate
    them against SizeOfImage before callback code converts them to VAs.

Arguments:

    FieldId - KSW_DYN_FIELD_ID_CB_* global item ID supplied by R3.
    State - Mutable state snapshot that owns destination fields.
    RvaOut - Receives a pointer to the target RVA slot.
    SourceOut - Receives a pointer to the target source slot.

Return Value:

    TRUE when the item ID is a supported callback global; otherwise FALSE.

--*/
{
    if (State == NULL || RvaOut == NULL || SourceOut == NULL) {
        return FALSE;
    }

    *RvaOut = NULL;
    *SourceOut = NULL;

    switch (FieldId) {
    case KSW_DYN_FIELD_ID_CB_PSP_CREATE_PROCESS_NOTIFY_ROUTINE:
        *RvaOut = &State->CallbackGlobals.PspCreateProcessNotifyRoutine;
        *SourceOut = &State->CallbackGlobalSources.PspCreateProcessNotifyRoutine;
        break;
    case KSW_DYN_FIELD_ID_CB_PSP_CREATE_THREAD_NOTIFY_ROUTINE:
        *RvaOut = &State->CallbackGlobals.PspCreateThreadNotifyRoutine;
        *SourceOut = &State->CallbackGlobalSources.PspCreateThreadNotifyRoutine;
        break;
    case KSW_DYN_FIELD_ID_CB_PSP_LOAD_IMAGE_NOTIFY_ROUTINE:
        *RvaOut = &State->CallbackGlobals.PspLoadImageNotifyRoutine;
        *SourceOut = &State->CallbackGlobalSources.PspLoadImageNotifyRoutine;
        break;
    case KSW_DYN_FIELD_ID_CB_PSP_NOTIFY_ENABLE_MASK:
        *RvaOut = &State->CallbackGlobals.PspNotifyEnableMask;
        *SourceOut = &State->CallbackGlobalSources.PspNotifyEnableMask;
        break;
    case KSW_DYN_FIELD_ID_CB_CM_CALLBACK_LIST_HEAD:
        *RvaOut = &State->CallbackGlobals.CmCallbackListHead;
        *SourceOut = &State->CallbackGlobalSources.CmCallbackListHead;
        break;
    default:
        return FALSE;
    }

    return (*RvaOut != NULL && *SourceOut != NULL) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKDynDataCallbackOffsetPointers(
    _In_ ULONG FieldId,
    _Inout_ KSW_DYN_STATE* State,
    _Outptr_ ULONG** OffsetOut,
    _Outptr_ ULONG** SourceOut
    )
/*++

Routine Description:

    Resolve one callback structure item ID into the internal offset/source
    slots. These fields describe private callback-related structures and are
    applied only after the ntoskrnl identity has matched exactly.

Arguments:

    FieldId - KSW_DYN_FIELD_ID_CB_* structure item ID supplied by R3.
    State - Mutable state snapshot that owns destination fields.
    OffsetOut - Receives a pointer to the target offset slot.
    SourceOut - Receives a pointer to the target source slot.

Return Value:

    TRUE when the item ID is a supported callback structure offset; otherwise
    FALSE.

--*/
{
    if (State == NULL || OffsetOut == NULL || SourceOut == NULL) {
        return FALSE;
    }

    *OffsetOut = NULL;
    *SourceOut = NULL;

    switch (FieldId) {
    case KSW_DYN_FIELD_ID_CB_OBJECT_TYPE_CALLBACK_LIST:
        *OffsetOut = &State->CallbackOffsets.ObjectTypeCallbackList;
        *SourceOut = &State->CallbackOffsetSources.ObjectTypeCallbackList;
        break;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_ENTRY_LIST:
        *OffsetOut = &State->CallbackOffsets.CallbackEntryItemEntryList;
        *SourceOut = &State->CallbackOffsetSources.CallbackEntryItemEntryList;
        break;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_PRE_OPERATION:
        *OffsetOut = &State->CallbackOffsets.CallbackEntryItemPreOperation;
        *SourceOut = &State->CallbackOffsetSources.CallbackEntryItemPreOperation;
        break;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_POST_OPERATION:
        *OffsetOut = &State->CallbackOffsets.CallbackEntryItemPostOperation;
        *SourceOut = &State->CallbackOffsetSources.CallbackEntryItemPostOperation;
        break;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_OPERATIONS:
        *OffsetOut = &State->CallbackOffsets.CallbackEntryItemOperations;
        *SourceOut = &State->CallbackOffsetSources.CallbackEntryItemOperations;
        break;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_CALLBACK_ENTRY:
        *OffsetOut = &State->CallbackOffsets.CallbackEntryItemCallbackEntry;
        *SourceOut = &State->CallbackOffsetSources.CallbackEntryItemCallbackEntry;
        break;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ALTITUDE:
        *OffsetOut = &State->CallbackOffsets.CallbackEntryAltitude;
        *SourceOut = &State->CallbackOffsetSources.CallbackEntryAltitude;
        break;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_REGISTRATION_CONTEXT:
        *OffsetOut = &State->CallbackOffsets.CallbackEntryRegistrationContext;
        *SourceOut = &State->CallbackOffsetSources.CallbackEntryRegistrationContext;
        break;
    default:
        return FALSE;
    }

    return (*OffsetOut != NULL && *SourceOut != NULL) ? TRUE : FALSE;
}

NTSTATUS
KswordARKDynDataApplyProfile(
    _In_reads_bytes_(InputBufferLength) const KSW_APPLY_DYN_PROFILE_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSW_APPLY_DYN_PROFILE_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Validate and merge one R3-supplied PDB profile into the global DynData state.
    The driver accepts only packed field IDs and offsets; it never parses JSON or
    PDB data. The merge is copy-on-success: all request checks and all field
    checks complete against a local state copy before the global state is
    replaced, so invalid profiles cannot corrupt the active DynData snapshot.

Arguments:

    Request - Packed profile request from R3.
    InputBufferLength - Total request bytes available.
    Response - Fixed response packet.
    OutputBufferLength - Writable response bytes.
    BytesWrittenOut - Receives sizeof(KSW_APPLY_DYN_PROFILE_RESPONSE) on success
        and on handled validation failure.

Return Value:

    STATUS_SUCCESS when a profile was applied; validation status on rejected
    requests. Response->status mirrors the same result for R3 diagnostics.

--*/
{
    KSW_DYN_STATE candidateState;
    NTSTATUS status = STATUS_SUCCESS;
    size_t requiredBytes = 0U;
    ULONG index = 0UL;
    ULONG appliedCount = 0UL;
    ULONG rejectedCount = 0UL;
    ULONG unknownCount = 0UL;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (Response == NULL || OutputBufferLength < sizeof(KSW_APPLY_DYN_PROFILE_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Response, OutputBufferLength);
    Response->size = sizeof(*Response);
    Response->version = KSWORD_ARK_DYNDATA_PROTOCOL_VERSION;
    Response->status = STATUS_UNSUCCESSFUL;
    KswordARKDynDataApplySetMessage(Response->message, L"PDB profile apply did not run.");
    *BytesWrittenOut = sizeof(*Response);

    if (Request == NULL) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile request is null.");
        Response->status = status;
        return status;
    }
    if (InputBufferLength < KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE) {
        status = STATUS_BUFFER_TOO_SMALL;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile request header is too small.");
        Response->status = status;
        return status;
    }
    if (Request->version != KSWORD_ARK_DYNDATA_PROTOCOL_VERSION) {
        status = STATUS_REVISION_MISMATCH;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile protocol version mismatch.");
        Response->status = status;
        return status;
    }
    if (Request->fieldCount == 0UL || Request->fieldCount > KSW_DYN_PROFILE_MAX_FIELDS) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile field count is invalid.");
        Response->status = status;
        return status;
    }
    if (Request->size < KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile request size is invalid.");
        Response->status = status;
        return status;
    }
    if ((Request->fieldCount - 1UL) >
        ((MAXSIZE_T - KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE) / sizeof(KSW_DYN_PROFILE_FIELD_PACKET))) {
        status = STATUS_INTEGER_OVERFLOW;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile request size overflow.");
        Response->status = status;
        return status;
    }

    requiredBytes = KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE +
        ((size_t)Request->fieldCount * sizeof(KSW_DYN_PROFILE_FIELD_PACKET));
    if ((size_t)Request->size < requiredBytes || InputBufferLength < requiredBytes) {
        status = STATUS_BUFFER_TOO_SMALL;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile request does not contain all fields.");
        Response->status = status;
        return status;
    }

    ExAcquirePushLockShared(&g_KswordDynDataStateLock);
    RtlCopyMemory(&candidateState, &g_KswordDynDataState, sizeof(candidateState));
    ExReleasePushLockShared(&g_KswordDynDataStateLock);

    if (!KswordARKDynDataIdentityMatches(&candidateState.Ntoskrnl, &Request->ntoskrnl)) {
        status = STATUS_NOT_SUPPORTED;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile does not match the current ntoskrnl identity.");
        Response->status = status;
        Response->statusFlags = KswordARKDynDataPublicStatusFlags(&candidateState);
        Response->capabilityMask = candidateState.CapabilityMask;
        return status;
    }

    for (index = 0UL; index < Request->fieldCount; ++index) {
        const KSW_DYN_PROFILE_FIELD_PACKET* field = &Request->fields[index];
        ULONG* destinationOffset = NULL;
        ULONG* destinationSource = NULL;

        if (field->fieldId == 0UL || field->fieldId > KSW_DYN_FIELD_ID_MAX ||
            field->offset == KSW_DYN_OFFSET_UNAVAILABLE ||
            field->offset > KSW_DYN_PROFILE_OFFSET_MAX) {
            rejectedCount += 1UL;
            continue;
        }

        if (!KswordARKDynDataKernelFieldPointers(
            field->fieldId,
            &candidateState,
            &destinationOffset,
            &destinationSource)) {
            unknownCount += 1UL;
            continue;
        }

        KswordARKDynDataStoreSourcedOffset(
            field->offset,
            KSW_DYN_FIELD_SOURCE_PDB_PROFILE,
            destinationOffset,
            destinationSource);
        appliedCount += 1UL;
    }

    Response->appliedFieldCount = appliedCount;
    Response->rejectedFieldCount = rejectedCount;
    Response->unknownFieldCount = unknownCount;

    if (appliedCount == 0UL || rejectedCount != 0UL || unknownCount != 0UL) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile contained invalid or unsupported fields; active state was left unchanged.");
        Response->status = status;
        Response->statusFlags = KswordARKDynDataPublicStatusFlags(&candidateState);
        Response->capabilityMask = candidateState.CapabilityMask;
        return status;
    }

    candidateState.PdbProfileActive = TRUE;
    candidateState.NtosActive = TRUE;
    candidateState.CapabilityMask = KswordARKDynDataComputeCapabilities(&candidateState);
    candidateState.LastStatus = STATUS_SUCCESS;
    KswordARKDynDataSetReason(&candidateState, L"PDB profile applied and merged with runtime DynData.");

    ExAcquirePushLockExclusive(&g_KswordDynDataStateLock);
    RtlCopyMemory(&g_KswordDynDataState, &candidateState, sizeof(g_KswordDynDataState));
    ExReleasePushLockExclusive(&g_KswordDynDataStateLock);

    Response->status = STATUS_SUCCESS;
    Response->statusFlags = KswordARKDynDataPublicStatusFlags(&candidateState);
    Response->capabilityMask = candidateState.CapabilityMask;
    KswordARKDynDataApplySetMessage(Response->message, L"PDB profile applied successfully.");
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDynDataApplyProfileEx(
    _In_reads_bytes_(InputBufferLength) const KSW_APPLY_DYN_PROFILE_EX_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSW_APPLY_DYN_PROFILE_EX_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Validate and merge one extended R3-supplied PDB profile into the typed
    DynData portion of the global state. This v2/v3 path accepts only typed
    items: structure offsets and ntoskrnl global RVAs. The driver still never
    parses JSON/PDB/pack content; R3 must send compact numeric IDs after local
    schema validation. The merge is copy-on-success so malformed items never
    poison the active DynData snapshot.

Arguments:

    Request - Packed extended profile request from R3.
    InputBufferLength - Total request bytes available.
    Response - Fixed extended response packet.
    OutputBufferLength - Writable response bytes.
    BytesWrittenOut - Receives sizeof(KSW_APPLY_DYN_PROFILE_EX_RESPONSE) on
        success and on handled validation failure.

Return Value:

    STATUS_SUCCESS when at least one extended item was applied; validation
    status on rejected requests. Response->status mirrors the semantic result.

--*/
{
    KSW_DYN_STATE candidateState;
    NTSTATUS status = STATUS_SUCCESS;
    size_t requiredBytes = 0U;
    ULONG index = 0UL;
    ULONG appliedCount = 0UL;
    ULONG rejectedCount = 0UL;
    ULONG unknownCount = 0UL;
    BOOLEAN callbackItemApplied = FALSE;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (Response == NULL || OutputBufferLength < sizeof(KSW_APPLY_DYN_PROFILE_EX_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Response, OutputBufferLength);
    Response->size = sizeof(*Response);
    Response->version = KSWORD_ARK_DYNDATA_PROTOCOL_VERSION;
    Response->status = STATUS_UNSUCCESSFUL;
    KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX apply did not run.");
    *BytesWrittenOut = sizeof(*Response);

    if (Request == NULL) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX request is null.");
        Response->status = status;
        return status;
    }
    if (InputBufferLength < KSW_APPLY_DYN_PROFILE_EX_REQUEST_HEADER_SIZE) {
        status = STATUS_BUFFER_TOO_SMALL;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX request header is too small.");
        Response->status = status;
        return status;
    }
    if (Request->version != KSWORD_ARK_DYNDATA_PROTOCOL_VERSION) {
        status = STATUS_REVISION_MISMATCH;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX protocol version mismatch.");
        Response->status = status;
        return status;
    }
    if (Request->itemCount == 0UL || Request->itemCount > KSW_DYN_PROFILE_EX_MAX_ITEMS) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX item count is invalid.");
        Response->status = status;
        return status;
    }
    if (Request->size < KSW_APPLY_DYN_PROFILE_EX_REQUEST_HEADER_SIZE) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX request size is invalid.");
        Response->status = status;
        return status;
    }
    if ((Request->itemCount - 1UL) >
        ((MAXSIZE_T - KSW_APPLY_DYN_PROFILE_EX_REQUEST_HEADER_SIZE) / sizeof(KSW_DYN_PROFILE_EX_ITEM_PACKET))) {
        status = STATUS_INTEGER_OVERFLOW;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX request size overflow.");
        Response->status = status;
        return status;
    }

    requiredBytes = KSW_APPLY_DYN_PROFILE_EX_REQUEST_HEADER_SIZE +
        ((size_t)Request->itemCount * sizeof(KSW_DYN_PROFILE_EX_ITEM_PACKET));
    if ((size_t)Request->size < requiredBytes || InputBufferLength < requiredBytes) {
        status = STATUS_BUFFER_TOO_SMALL;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX request does not contain all items.");
        Response->status = status;
        return status;
    }

    ExAcquirePushLockShared(&g_KswordDynDataStateLock);
    RtlCopyMemory(&candidateState, &g_KswordDynDataState, sizeof(candidateState));
    ExReleasePushLockShared(&g_KswordDynDataStateLock);

    if (!KswordARKDynDataIdentityMatches(&candidateState.Ntoskrnl, &Request->ntoskrnl)) {
        status = STATUS_NOT_SUPPORTED;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX does not match the current ntoskrnl identity.");
        Response->status = status;
        Response->statusFlags = KswordARKDynDataPublicStatusFlags(&candidateState);
        Response->capabilityMask = candidateState.CapabilityMask;
        return status;
    }

    for (index = 0UL; index < Request->itemCount; ++index) {
        const KSW_DYN_PROFILE_EX_ITEM_PACKET* item = &Request->items[index];
        ULONG* destinationValue = NULL;
        ULONG* destinationSource = NULL;

        if (item->itemId == 0UL || item->itemId > KSW_DYN_FIELD_ID_MAX ||
            (item->flags & ~(KSW_DYN_PROFILE_EX_ITEM_FLAG_REQUIRED | KSW_DYN_PROFILE_EX_ITEM_FLAG_CALLBACK)) != 0UL) {
            rejectedCount += 1UL;
            continue;
        }

        if (item->itemKind == KSW_DYN_PROFILE_EX_ITEM_KIND_GLOBAL_RVA) {
            if (item->value == 0UL ||
                item->value >= candidateState.Ntoskrnl.sizeOfImage ||
                item->value > KSW_DYN_PROFILE_GLOBAL_RVA_MAX) {
                rejectedCount += 1UL;
                continue;
            }
            if (!KswordARKDynDataCallbackGlobalPointers(
                    item->itemId,
                    &candidateState,
                    &destinationValue,
                    &destinationSource) &&
                !KswordARKDynDataKernelGlobalRvaPointers(
                    item->itemId,
                    &candidateState,
                    &destinationValue,
                    &destinationSource)) {
                unknownCount += 1UL;
                continue;
            }
        }
        else if (item->itemKind == KSW_DYN_PROFILE_EX_ITEM_KIND_STRUCT_OFFSET) {
            if (item->value == KSW_DYN_OFFSET_UNAVAILABLE ||
                item->value > KSW_DYN_PROFILE_OFFSET_MAX) {
                rejectedCount += 1UL;
                continue;
            }
            if (!KswordARKDynDataCallbackOffsetPointers(
                    item->itemId,
                    &candidateState,
                    &destinationValue,
                    &destinationSource) &&
                !KswordARKDynDataKernelFieldPointers(
                    item->itemId,
                    &candidateState,
                    &destinationValue,
                    &destinationSource)) {
                unknownCount += 1UL;
                continue;
            }
        }
        else {
            rejectedCount += 1UL;
            continue;
        }

        KswordARKDynDataStoreSourcedOffset(
            item->value,
            KSW_DYN_FIELD_SOURCE_PDB_PROFILE,
            destinationValue,
            destinationSource);
        if ((item->flags & KSW_DYN_PROFILE_EX_ITEM_FLAG_CALLBACK) != 0UL) {
            callbackItemApplied = TRUE;
        }
        appliedCount += 1UL;
    }

    Response->appliedItemCount = appliedCount;
    Response->rejectedItemCount = rejectedCount;
    Response->unknownItemCount = unknownCount;

    if (appliedCount == 0UL || rejectedCount != 0UL || unknownCount != 0UL) {
        status = STATUS_INVALID_PARAMETER;
        KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX contained invalid or unsupported items; active state was left unchanged.");
        Response->status = status;
        Response->statusFlags = KswordARKDynDataPublicStatusFlags(&candidateState);
        Response->capabilityMask = candidateState.CapabilityMask;
        return status;
    }

    candidateState.PdbProfileActive = TRUE;
    candidateState.CallbackProfileActive = callbackItemApplied ? TRUE : candidateState.CallbackProfileActive;
    candidateState.NtosActive = TRUE;
    candidateState.CapabilityMask = KswordARKDynDataComputeCapabilities(&candidateState);
    candidateState.LastStatus = STATUS_SUCCESS;
    KswordARKDynDataSetReason(&candidateState, L"PDB profile EX applied and merged with callback DynData.");

    ExAcquirePushLockExclusive(&g_KswordDynDataStateLock);
    RtlCopyMemory(&g_KswordDynDataState, &candidateState, sizeof(g_KswordDynDataState));
    ExReleasePushLockExclusive(&g_KswordDynDataStateLock);

    Response->status = STATUS_SUCCESS;
    Response->statusFlags = KswordARKDynDataPublicStatusFlags(&candidateState);
    Response->capabilityMask = candidateState.CapabilityMask;
    KswordARKDynDataApplySetMessage(Response->message, L"PDB profile EX applied successfully.");
    return STATUS_SUCCESS;
}
