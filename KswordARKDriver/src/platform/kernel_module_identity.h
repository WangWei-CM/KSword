#pragma once

#include "ark/ark_dyndata.h"

EXTERN_C_START

// One accepted loaded-module filename and the DynData class assigned to it.
typedef struct _KSW_KERNEL_MODULE_NAME_MATCH
{
    PCSTR FileName;
    ULONG ClassId;
} KSW_KERNEL_MODULE_NAME_MATCH, *PKSW_KERNEL_MODULE_NAME_MATCH;

NTSTATUS
KswordARKQueryKernelModuleIdentity(
    _In_reads_(NameMatchCount) const KSW_KERNEL_MODULE_NAME_MATCH* NameMatches,
    _In_ ULONG NameMatchCount,
    _Out_ KSW_DYN_MODULE_IDENTITY_PACKET* IdentityOut
    );

EXTERN_C_END
