#pragma once

#include "dyndata_fallback_resolver.h"

EXTERN_C_START

// 已知且可信的 MmUnloadedDrivers RVA 可直接参与相同的多记录布局验证。
BOOLEAN
KswordARKDriverResolveMmUnloadedLayout(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* NtoskrnlIdentity,
    _In_ ULONG PointerRva,
    _Out_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    );

VOID
KswordARKDriverResolveKernelCacheFallback(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* NtoskrnlIdentity,
    _Inout_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    );

// Exact, trusted global RVAs can seed the same locked multi-entry validation
// after PDB application, even when initialization's export scan missed them.
NTSTATUS
KswordARKDriverResolvePiDdbLayout(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* NtoskrnlIdentity,
    _In_ ULONG TableRva,
    _In_ ULONG LockRva,
    _Out_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    );

EXTERN_C_END
