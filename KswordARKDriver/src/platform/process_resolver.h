#pragma once

#include "ark/ark_driver.h"

typedef NTSTATUS(NTAPI* KSWORD_PS_SUSPEND_PROCESS_FN)(
    _In_ PEPROCESS Process
    );

typedef BOOLEAN(NTAPI* KSWORD_PS_IS_PROTECTED_PROCESS_FN)(
    _In_ PEPROCESS Process
    );

typedef BOOLEAN(NTAPI* KSWORD_PS_IS_PROTECTED_PROCESS_LIGHT_FN)(
    _In_ PEPROCESS Process
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)(
    _In_ HANDLE ProcessHandle
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_SET_INFORMATION_PROCESS_FN)(
    _In_ HANDLE ProcessHandle,
    _In_ ULONG ProcessInformationClass,
    _In_reads_bytes_(ProcessInformationLength) PVOID ProcessInformation,
    _In_ ULONG ProcessInformationLength
    );

KSWORD_PS_SUSPEND_PROCESS_FN
KswordARKDriverResolvePsSuspendProcess(
    VOID
    );

KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN
KswordARKDriverResolveZwOrNtSuspendProcess(
    VOID
    );

KSWORD_PS_IS_PROTECTED_PROCESS_FN
KswordARKDriverResolvePsIsProtectedProcess(
    VOID
    );

KSWORD_PS_IS_PROTECTED_PROCESS_LIGHT_FN
KswordARKDriverResolvePsIsProtectedProcessLight(
    VOID
    );

LONG
KswordARKDriverResolveProcessProtectionOffset(
    VOID
    );

LONG
KswordARKDriverResolveProcessSignatureLevelOffset(
    VOID
    );

LONG
KswordARKDriverResolveProcessSectionSignatureLevelOffset(
    VOID
    );

KSWORD_ZW_SET_INFORMATION_PROCESS_FN
KswordARKDriverResolveZwSetInformationProcess(
    VOID
    );
