/*++

Module Name:

    process_resolver.c

Abstract:

    This file resolves optional kernel process routines dynamically.

Environment:

    Kernel-mode Driver Framework

--*/

#include "process_resolver.h"

// Resolve PsSuspendProcess first; this export is available on more systems.
KSWORD_PS_SUSPEND_PROCESS_FN
KswordARKDriverResolvePsSuspendProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsSuspendProcess");
    return (KSWORD_PS_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

// Fallback resolver for Zw/Nt suspend APIs that use process handle input.
KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN
KswordARKDriverResolveZwOrNtSuspendProcess(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwSuspendProcess");
    {
        KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN routineAddress =
            (KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
        if (routineAddress != NULL) {
            return routineAddress;
        }
    }

    RtlInitUnicodeString(&routineName, L"NtSuspendProcess");
    return (KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

KSWORD_PS_IS_PROTECTED_PROCESS_FN
KswordARKDriverResolvePsIsProtectedProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsIsProtectedProcess");
    return (KSWORD_PS_IS_PROTECTED_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

KSWORD_PS_IS_PROTECTED_PROCESS_LIGHT_FN
KswordARKDriverResolvePsIsProtectedProcessLight(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsIsProtectedProcessLight");
    return (KSWORD_PS_IS_PROTECTED_PROCESS_LIGHT_FN)MmGetSystemRoutineAddress(&routineName);
}

// PPLcontrol-style parser: read the immediate displacement at +2 from PsIsProtected*.
static LONG
KswordARKDriverReadProtectedRoutineOffset(
    _In_ PVOID routineAddress
    )
{
    USHORT offsetValue = 0U;

    if (routineAddress == NULL) {
        return -1;
    }

    __try {
        RtlCopyMemory(
            &offsetValue,
            ((const UCHAR*)routineAddress) + 2U,
            sizeof(offsetValue));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    if (offsetValue == 0U || offsetValue > 0x0FFFU) {
        return -1;
    }

    return (LONG)offsetValue;
}

LONG
KswordARKDriverResolveProcessProtectionOffset(
    VOID
    )
{
    KSWORD_PS_IS_PROTECTED_PROCESS_FN psIsProtectedProcess = NULL;
    KSWORD_PS_IS_PROTECTED_PROCESS_LIGHT_FN psIsProtectedProcessLight = NULL;
    LONG protectionOffsetA = -1;
    LONG protectionOffsetB = -1;

    psIsProtectedProcess = KswordARKDriverResolvePsIsProtectedProcess();
    psIsProtectedProcessLight = KswordARKDriverResolvePsIsProtectedProcessLight();
    if (psIsProtectedProcess == NULL || psIsProtectedProcessLight == NULL) {
        return -1;
    }

    protectionOffsetA = KswordARKDriverReadProtectedRoutineOffset((PVOID)psIsProtectedProcess);
    protectionOffsetB = KswordARKDriverReadProtectedRoutineOffset((PVOID)psIsProtectedProcessLight);
    if (protectionOffsetA <= 0 ||
        protectionOffsetB <= 0 ||
        protectionOffsetA != protectionOffsetB) {
        return -1;
    }

    return protectionOffsetA;
}

LONG
KswordARKDriverResolveProcessSignatureLevelOffset(
    VOID
    )
{
    LONG protectionOffset = KswordARKDriverResolveProcessProtectionOffset();
    if (protectionOffset <= (LONG)(2U * sizeof(UCHAR))) {
        return -1;
    }

    return protectionOffset - (LONG)(2U * sizeof(UCHAR));
}

LONG
KswordARKDriverResolveProcessSectionSignatureLevelOffset(
    VOID
    )
{
    LONG protectionOffset = KswordARKDriverResolveProcessProtectionOffset();
    if (protectionOffset <= (LONG)sizeof(UCHAR)) {
        return -1;
    }

    return protectionOffset - (LONG)sizeof(UCHAR);
}

// Resolve ZwSetInformationProcess dynamically for broad WDK compatibility.
KSWORD_ZW_SET_INFORMATION_PROCESS_FN
KswordARKDriverResolveZwSetInformationProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"ZwSetInformationProcess");
    return (KSWORD_ZW_SET_INFORMATION_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}
