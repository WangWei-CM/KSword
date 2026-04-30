#pragma once

#include "ark_dyndata.h"

EXTERN_C_START

ULONG
KswordARKDynDataCountFieldDescriptors(
    VOID
    );

ULONG
KswordARKDynDataCopyFieldDescriptors(
    _In_ const KSW_DYN_STATE* State,
    _Out_writes_opt_(EntryCapacity) KSW_DYN_FIELD_ENTRY* Entries,
    _In_ ULONG EntryCapacity
    );

ULONG64
KswordARKDynDataComputeCapabilities(
    _In_ const KSW_DYN_STATE* State
    );

EXTERN_C_END
