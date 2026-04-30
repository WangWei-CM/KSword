#pragma once

#include "ark/ark_driver.h"

EXTERN_C_START

VOID
KswordARKProcessPopulateExtendedEntry(
    _Inout_ KSWORD_ARK_PROCESS_ENTRY* Entry,
    _In_ PEPROCESS ProcessObject
    );

NTSTATUS
KswordARKProcessPatchProtectionByDynData(
    _In_ ULONG ProcessId,
    _In_ UCHAR ProtectionLevel,
    _In_ UCHAR SignatureLevel,
    _In_ UCHAR SectionSignatureLevel
    );

EXTERN_C_END
