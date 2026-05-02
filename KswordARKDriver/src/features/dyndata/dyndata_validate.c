/*++

Module Name:

    dyndata_validate.c

Abstract:

    Field descriptor and capability-gating helpers for Ksword DynData.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_dyndata_fields.h"

#include <ntstrsafe.h>

typedef struct _KSW_DYN_FIELD_BINDING
{
    ULONG FieldId;
    PCSTR FieldName;
    PCSTR FeatureName;
    ULONG64 CapabilityMask;
    BOOLEAN Required;
    ULONG Source;
    SIZE_T OffsetInState;
} KSW_DYN_FIELD_BINDING, *PKSW_DYN_FIELD_BINDING;

#define KSW_FIELD_BINDING(FieldIdValue, FieldNameText, FeatureText, CapabilityValue, RequiredValue, SourceValue, MemberPath) \
    { FieldIdValue, FieldNameText, FeatureText, CapabilityValue, RequiredValue, SourceValue, FIELD_OFFSET(KSW_DYN_STATE, MemberPath) }

static const KSW_DYN_FIELD_BINDING g_KswordDynFieldBindings[] = {
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_OBJECT_TABLE, "EpObjectTable", "Process HandleTable", KSW_CAP_PROCESS_OBJECT_TABLE | KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EpObjectTable),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_SECTION_OBJECT, "EpSectionObject", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EpSectionObject),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_HT_HANDLE_CONTENTION_EVENT, "HtHandleContentionEvent", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE, FALSE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.HtHandleContentionEvent),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OT_NAME, "OtName", "Object Type", KSW_CAP_OBJECT_TYPE_FIELDS | KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.OtName),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OT_INDEX, "OtIndex", "Object Type", KSW_CAP_OBJECT_TYPE_FIELDS | KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.OtIndex),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OB_DECODE_SHIFT, "ObDecodeShift", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.ObDecodeShift),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OB_ATTRIBUTES_SHIFT, "ObAttributesShift", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.ObAttributesShift),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EGE_GUID, "EgeGuid", "ETW GUID Entry", KSW_CAP_ETW_GUID_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EgeGuid),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ERE_GUID_ENTRY, "EreGuidEntry", "ETW Registration Entry", KSW_CAP_ETW_GUID_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EreGuidEntry),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_INITIAL_STACK, "KtInitialStack", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtInitialStack),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_STACK_LIMIT, "KtStackLimit", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtStackLimit),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_STACK_BASE, "KtStackBase", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtStackBase),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_KERNEL_STACK, "KtKernelStack", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtKernelStack),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_READ_OPERATION_COUNT, "KtReadOperationCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtReadOperationCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_WRITE_OPERATION_COUNT, "KtWriteOperationCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtWriteOperationCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_OTHER_OPERATION_COUNT, "KtOtherOperationCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtOtherOperationCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_READ_TRANSFER_COUNT, "KtReadTransferCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtReadTransferCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_WRITE_TRANSFER_COUNT, "KtWriteTransferCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtWriteTransferCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_OTHER_TRANSFER_COUNT, "KtOtherTransferCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtOtherTransferCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_MM_SECTION_CONTROL_AREA, "MmSectionControlArea", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.MmSectionControlArea),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LIST_HEAD, "MmControlAreaListHead", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.MmControlAreaListHead),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LOCK, "MmControlAreaLock", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.MmControlAreaLock),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_COMMUNICATION_INFO, "AlpcCommunicationInfo", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcCommunicationInfo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_OWNER_PROCESS, "AlpcOwnerProcess", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcOwnerProcess),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_CONNECTION_PORT, "AlpcConnectionPort", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcConnectionPort),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_SERVER_COMMUNICATION_PORT, "AlpcServerCommunicationPort", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcServerCommunicationPort),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_CLIENT_COMMUNICATION_PORT, "AlpcClientCommunicationPort", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcClientCommunicationPort),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE, "AlpcHandleTable", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcHandleTable),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE_LOCK, "AlpcHandleTableLock", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcHandleTableLock),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES, "AlpcAttributes", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcAttributes),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES_FLAGS, "AlpcAttributesFlags", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcAttributesFlags),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_PORT_CONTEXT, "AlpcPortContext", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcPortContext),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_PORT_OBJECT_LOCK, "AlpcPortObjectLock", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcPortObjectLock),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_SEQUENCE_NO, "AlpcSequenceNo", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcSequenceNo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_STATE, "AlpcState", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcState),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_PROC, "LxPicoProc", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoProc),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO, "LxPicoProcInfo", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoProcInfo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO_PID, "LxPicoProcInfoPID", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoProcInfoPID),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO, "LxPicoThrdInfo", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoThrdInfo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO_TID, "LxPicoThrdInfoTID", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoThrdInfoTID),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_PROTECTION, "EpProtection", "Process Protection Patch", KSW_CAP_PROCESS_PROTECTION_PATCH, TRUE, KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN, Kernel.EpProtection),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_SIGNATURE_LEVEL, "EpSignatureLevel", "Process Protection Patch", KSW_CAP_PROCESS_PROTECTION_PATCH, TRUE, KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN, Kernel.EpSignatureLevel),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_SECTION_SIGNATURE_LEVEL, "EpSectionSignatureLevel", "Process Protection Patch", KSW_CAP_PROCESS_PROTECTION_PATCH, TRUE, KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN, Kernel.EpSectionSignatureLevel)
};

static ULONG
KswordARKDynDataReadBoundOffset(
    _In_ const KSW_DYN_STATE* State,
    _In_ const KSW_DYN_FIELD_BINDING* Binding
    )
/*++

Routine Description:

    Read a bound offset from a DynData state snapshot. Bindings use FIELD_OFFSET
    so query and capability code can share one field list.

Arguments:

    State - State snapshot that owns all offsets.
    Binding - Field binding row to read.

Return Value:

    Offset value, or KSW_DYN_OFFSET_UNAVAILABLE on invalid input.

--*/
{
    const UCHAR* stateBytes = (const UCHAR*)State;

    if (State == NULL || Binding == NULL || Binding->OffsetInState > sizeof(KSW_DYN_STATE) - sizeof(ULONG)) {
        return KSW_DYN_OFFSET_UNAVAILABLE;
    }

    return *(const ULONG*)(stateBytes + Binding->OffsetInState);
}

static VOID
KswordARKDynDataCopyAnsi(
    _Out_writes_bytes_(DestinationBytes) CHAR* Destination,
    _In_ size_t DestinationBytes,
    _In_opt_z_ PCSTR Source
    )
/*++

Routine Description:

    Copy a small descriptor string into an IOCTL response row while guaranteeing
    NUL termination.

Arguments:

    Destination - Output char buffer.
    DestinationBytes - Size of Destination in bytes.
    Source - Optional source string.

Return Value:

    None.

--*/
{
    if (Destination == NULL || DestinationBytes == 0U) {
        return;
    }

    Destination[0] = '\0';
    if (Source == NULL) {
        return;
    }

    (VOID)RtlStringCbCopyNA(Destination, DestinationBytes, Source, DestinationBytes - 1U);
    Destination[DestinationBytes - 1U] = '\0';
}

static PCSTR
KswordARKDynDataSourceName(
    _In_ ULONG Source
    )
/*++

Routine Description:

    Convert a field source code into a stable short diagnostic label.

Arguments:

    Source - KSW_DYN_FIELD_SOURCE_* value.

Return Value:

    Static string label.

--*/
{
    switch (Source) {
    case KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER:
        return "System Informer";
    case KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN:
        return "Runtime pattern";
    case KSW_DYN_FIELD_SOURCE_KSWORD_EXTRA_TABLE:
        return "Ksword extra";
    default:
        return "Unavailable";
    }
}

static BOOLEAN
KswordARKDynDataIsOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Normalize DynData field availability. System Informer marks missing fields
    as 0xffff; Ksword stores unavailable fields as ULONG_MAX.

Arguments:

    Offset - Offset value to test.

Return Value:

    TRUE when usable, FALSE when unavailable.

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

ULONG
KswordARKDynDataCountFieldDescriptors(
    VOID
    )
/*++

Routine Description:

    Return the number of field descriptors exported to user mode.

Arguments:

    None.

Return Value:

    Descriptor count.

--*/
{
    return (ULONG)(sizeof(g_KswordDynFieldBindings) / sizeof(g_KswordDynFieldBindings[0]));
}

ULONG
KswordARKDynDataCopyFieldDescriptors(
    _In_ const KSW_DYN_STATE* State,
    _Out_writes_opt_(EntryCapacity) KSW_DYN_FIELD_ENTRY* Entries,
    _In_ ULONG EntryCapacity
    )
/*++

Routine Description:

    Build the public field table from the active state and static descriptor
    metadata. The caller may pass NULL Entries to query only total count.

Arguments:

    State - DynData state snapshot.
    Entries - Optional output entry array.
    EntryCapacity - Number of writable output entries.

Return Value:

    Number of entries copied to Entries.

--*/
{
    ULONG index = 0;
    ULONG copied = 0;
    ULONG totalCount = KswordARKDynDataCountFieldDescriptors();

    if (State == NULL || Entries == NULL || EntryCapacity == 0U) {
        return 0U;
    }

    for (index = 0; index < totalCount && copied < EntryCapacity; ++index) {
        const KSW_DYN_FIELD_BINDING* binding = &g_KswordDynFieldBindings[index];
        const ULONG offset = KswordARKDynDataReadBoundOffset(State, binding);
        KSW_DYN_FIELD_ENTRY* entry = &Entries[copied];

        RtlZeroMemory(entry, sizeof(*entry));
        entry->fieldId = binding->FieldId;
        entry->offset = offset;
        entry->source = KswordARKDynDataIsOffsetPresent(offset) ? binding->Source : KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
        entry->capabilityMask = binding->CapabilityMask;
        entry->flags = binding->Required ? KSW_DYN_FIELD_FLAG_REQUIRED : KSW_DYN_FIELD_FLAG_OPTIONAL;
        if (KswordARKDynDataIsOffsetPresent(offset)) {
            entry->flags |= KSW_DYN_FIELD_FLAG_PRESENT;
        }

        KswordARKDynDataCopyAnsi(entry->fieldName, sizeof(entry->fieldName), binding->FieldName);
        KswordARKDynDataCopyAnsi(entry->featureName, sizeof(entry->featureName), binding->FeatureName);
        KswordARKDynDataCopyAnsi(entry->sourceName, sizeof(entry->sourceName), KswordARKDynDataSourceName(entry->source));
        copied += 1U;
    }

    return copied;
}

static BOOLEAN
KswordARKDynDataHasAll(
    _In_ const ULONG* Offsets,
    _In_ ULONG Count
    )
/*++

Routine Description:

    Test a small dependency list for all-required field availability.

Arguments:

    Offsets - Dependency offset array.
    Count - Number of offsets in the array.

Return Value:

    TRUE when all fields are present; otherwise FALSE.

--*/
{
    ULONG index = 0;

    if (Offsets == NULL || Count == 0U) {
        return FALSE;
    }

    for (index = 0; index < Count; ++index) {
        if (!KswordARKDynDataIsOffsetPresent(Offsets[index])) {
            return FALSE;
        }
    }

    return TRUE;
}

ULONG64
KswordARKDynDataComputeCapabilities(
    _In_ const KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Compute feature capability flags from field availability. Capabilities are
    strict: a missing required field disables the dependent feature.

Arguments:

    State - DynData state snapshot with converted offsets.

Return Value:

    KSW_CAP_* bit mask.

--*/
{
    ULONG64 capabilities = KSW_DYN_CAPABILITY_NONE;

    if (State == NULL) {
        return KSW_DYN_CAPABILITY_NONE;
    }

    const ULONG objectTypeFields[] = { State->Kernel.OtName, State->Kernel.OtIndex };
    const ULONG handleTableFields[] = { State->Kernel.EpObjectTable, State->Kernel.ObDecodeShift, State->Kernel.ObAttributesShift, State->Kernel.OtName, State->Kernel.OtIndex };
    const ULONG threadStackFields[] = { State->Kernel.KtInitialStack, State->Kernel.KtStackLimit, State->Kernel.KtStackBase, State->Kernel.KtKernelStack };
    const ULONG threadIoFields[] = { State->Kernel.KtReadOperationCount, State->Kernel.KtWriteOperationCount, State->Kernel.KtOtherOperationCount, State->Kernel.KtReadTransferCount, State->Kernel.KtWriteTransferCount, State->Kernel.KtOtherTransferCount };
    const ULONG alpcFields[] = { State->Kernel.AlpcCommunicationInfo, State->Kernel.AlpcOwnerProcess, State->Kernel.AlpcConnectionPort, State->Kernel.AlpcServerCommunicationPort, State->Kernel.AlpcClientCommunicationPort, State->Kernel.AlpcHandleTable, State->Kernel.AlpcHandleTableLock, State->Kernel.AlpcAttributes, State->Kernel.AlpcAttributesFlags, State->Kernel.AlpcPortContext, State->Kernel.AlpcPortObjectLock, State->Kernel.AlpcSequenceNo, State->Kernel.AlpcState };
    const ULONG sectionFields[] = { State->Kernel.EpSectionObject, State->Kernel.MmSectionControlArea, State->Kernel.MmControlAreaListHead, State->Kernel.MmControlAreaLock };
    const ULONG protectionFields[] = { State->Kernel.EpProtection, State->Kernel.EpSignatureLevel, State->Kernel.EpSectionSignatureLevel };
    const ULONG etwFields[] = { State->Kernel.EgeGuid, State->Kernel.EreGuidEntry };
    const ULONG lxcoreFields[] = { State->LxcoreOffsets.LxPicoProc, State->LxcoreOffsets.LxPicoProcInfo, State->LxcoreOffsets.LxPicoProcInfoPID, State->LxcoreOffsets.LxPicoThrdInfo, State->LxcoreOffsets.LxPicoThrdInfoTID };

    if (State->NtosActive) {
        capabilities |= KSW_CAP_DYN_NTOS_ACTIVE;
    }
    if (State->LxcoreActive) {
        capabilities |= KSW_CAP_DYN_LXCORE_ACTIVE;
    }
    if (KswordARKDynDataHasAll(objectTypeFields, RTL_NUMBER_OF(objectTypeFields))) {
        capabilities |= KSW_CAP_OBJECT_TYPE_FIELDS;
    }
    if (KswordARKDynDataHasAll(handleTableFields, RTL_NUMBER_OF(handleTableFields))) {
        capabilities |= KSW_CAP_HANDLE_TABLE_DECODE | KSW_CAP_PROCESS_OBJECT_TABLE;
    }
    if (KswordARKDynDataHasAll(threadStackFields, RTL_NUMBER_OF(threadStackFields))) {
        capabilities |= KSW_CAP_THREAD_STACK_FIELDS;
    }
    if (KswordARKDynDataHasAll(threadIoFields, RTL_NUMBER_OF(threadIoFields))) {
        capabilities |= KSW_CAP_THREAD_IO_COUNTERS;
    }
    if (KswordARKDynDataHasAll(alpcFields, RTL_NUMBER_OF(alpcFields))) {
        capabilities |= KSW_CAP_ALPC_FIELDS;
    }
    if (KswordARKDynDataHasAll(sectionFields, RTL_NUMBER_OF(sectionFields))) {
        capabilities |= KSW_CAP_SECTION_CONTROL_AREA;
    }
    if (KswordARKDynDataHasAll(protectionFields, RTL_NUMBER_OF(protectionFields))) {
        capabilities |= KSW_CAP_PROCESS_PROTECTION_PATCH;
    }
    if (KswordARKDynDataHasAll(etwFields, RTL_NUMBER_OF(etwFields))) {
        capabilities |= KSW_CAP_ETW_GUID_FIELDS;
    }
    if (State->LxcoreActive && KswordARKDynDataHasAll(lxcoreFields, RTL_NUMBER_OF(lxcoreFields))) {
        capabilities |= KSW_CAP_WSL_LXCORE_FIELDS;
    }

    return capabilities;
}
