#pragma once

#include "ark/ark_driver.h"
#include "hook_scan_support.h"

EXTERN_C_START

typedef struct _KSW_DRIVER_INTEGRITY_BUILDER
{
    KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE* Response;
    ULONG Capacity;
    ULONG RowLimit;
} KSW_DRIVER_INTEGRITY_BUILDER, *PKSW_DRIVER_INTEGRITY_BUILDER;

typedef struct _KSW_DRIVER_INTEGRITY_LDR_TARGET
{
    BOOLEAN Available;
    BOOLEAN Found;
    ULONGLONG ListHeadAddress;
    ULONGLONG EntryAddress;
    ULONGLONG LinkAddress;
    ULONGLONG DllBase;
    ULONG SizeOfImage;
    WCHAR BaseDllName[KSWORD_ARK_DRIVER_INTEGRITY_OWNER_CHARS];
} KSW_DRIVER_INTEGRITY_LDR_TARGET, *PKSW_DRIVER_INTEGRITY_LDR_TARGET;

VOID
KswordARKDriverIntegrityAddEvidence(
    _Inout_ KSW_DRIVER_INTEGRITY_BUILDER* Builder,
    _In_ ULONG EvidenceClass,
    _In_ ULONGLONG ObjectAddress,
    _In_ ULONGLONG TargetAddress,
    _In_ ULONG RiskFlags,
    _In_ ULONG SourceMask,
    _In_ ULONG Confidence,
    _In_ ULONG ProcessorGroup,
    _In_ ULONG ProcessorNumber,
    _In_ ULONG Vector,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* OwnerModule,
    _In_opt_z_ PCWSTR DetailText
    );

const KSW_HOOK_SYSTEM_MODULE_ENTRY*
KswordARKDriverIntegrityFindModuleForAddress(
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONGLONG Address
    );

BOOLEAN
KswordARKDriverIntegrityIsCoreKernelModule(
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry
    );

VOID
KswordARKDriverIntegrityCopyWide(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_opt_z_ PCWSTR Source
    );

BOOLEAN
KswordARKDriverIntegrityOffsetPresent(
    _In_ ULONG Offset
    );

ULONGLONG
KswordARKDriverIntegrityNtosAddressFromRva(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ ULONG Rva,
    _In_ SIZE_T ProbeBytes
    );

NTSTATUS
KswordARKDriverIntegrityFindLoadedModule(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ ULONGLONG DriverStart,
    _Out_ KSW_DRIVER_INTEGRITY_LDR_TARGET* TargetOut
    );

EXTERN_C_END
