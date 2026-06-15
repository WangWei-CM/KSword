#pragma once

#include "driver_integrity.h"

EXTERN_C_START

NTSTATUS
KswordARKCpuIntegrityCollect(
    _Inout_ KSW_DRIVER_INTEGRITY_BUILDER* Builder,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONG Flags,
    _In_ ULONG MaxIdtVectorsPerCpu,
    _Out_ ULONG* CpuCountOut
    );

EXTERN_C_END
