#pragma once

#include "ark/ark_section.h"
#include "ark/ark_dyndata.h"

EXTERN_C_START

BOOLEAN
KswordARKSectionIsOffsetPresent(
    _In_ ULONG Offset
    );

ULONG
KswordARKSectionNormalizeOffset(
    _In_ ULONG Offset
    );

VOID
KswordARKSectionPrepareOffsets(
    _Inout_ KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE* Response,
    _In_ const KSW_DYN_STATE* DynState
    );

BOOLEAN
KswordARKSectionHasRequiredDynData(
    _In_ const KSW_DYN_STATE* DynState
    );

NTSTATUS
KswordARKSectionReadProcessSectionObject(
    _In_ PEPROCESS ProcessObject,
    _In_ const KSW_DYN_STATE* DynState,
    _Outptr_result_maybenull_ PVOID* SectionObjectOut
    );

NTSTATUS
KswordARKSectionReadControlArea(
    _In_ PVOID SectionObject,
    _In_ const KSW_DYN_STATE* DynState,
    _Outptr_result_maybenull_ PVOID* ControlAreaOut,
    _Out_ BOOLEAN* RemoteUnsupportedOut
    );

NTSTATUS
KswordARKSectionEnumerateMappings(
    _In_ PVOID ControlArea,
    _In_ const KSW_DYN_STATE* DynState,
    _Inout_ KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE* Response,
    _In_ size_t EntryCapacity
    );

NTSTATUS
KswordARKSectionEnumerateFileControlAreaMappings(
    _In_ PVOID ControlArea,
    _In_ ULONG SectionKind,
    _In_ const KSW_DYN_STATE* DynState,
    _Inout_ KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE* Response,
    _In_ size_t EntryCapacity
    );

EXTERN_C_END
