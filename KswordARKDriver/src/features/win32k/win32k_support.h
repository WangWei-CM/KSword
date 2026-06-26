#pragma once

#include "ark/ark_driver.h"
#include "ark/ark_ioctl.h"
#include "../kernel/hook_scan_support.h"

typedef PEPROCESS(NTAPI* KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN)(
    _In_opt_ PEPROCESS Process
    );

typedef PETHREAD(NTAPI* KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN)(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

typedef PVOID(NTAPI* KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN)(
    _In_ PETHREAD Thread
    );

typedef ULONG(NTAPI* KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN)(
    _In_ PEPROCESS Process
    );

VOID
KswordARKWin32kInitializeOffsets(
    _Out_ KSWORD_ARK_WIN32K_FIELD_OFFSETS* Offsets
    );

ULONG
KswordARKWin32kNormalizeMaxEntries(
    _In_ ULONG RequestedMaxEntries
    );

VOID
KswordARKWin32kCopyWideText(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_z_ PCWSTR Source
    );

KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN
KswordARKWin32kResolvePsGetNextProcess(
    VOID
    );

KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKWin32kResolvePsGetNextProcessThread(
    VOID
    );

KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN
KswordARKWin32kResolvePsGetThreadWin32Thread(
    VOID
    );

KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN
KswordARKWin32kResolvePsGetProcessSessionId(
    VOID
    );

BOOLEAN
KswordARKWin32kFindModuleByName(
    _In_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_z_ PCSTR ModuleName,
    _Out_ KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntryOut
    );

VOID
KswordARKWin32kFillModuleState(
    _Out_ KSWORD_ARK_WIN32K_MODULE_STATE* ModuleState,
    _In_z_ PCWSTR ModuleName,
    _In_ BOOLEAN Loaded,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry
    );

ULONG64
KswordARKWin32kModuleCapabilityMask(
    _In_ BOOLEAN Win32kLoaded,
    _In_ BOOLEAN Win32kbaseLoaded,
    _In_ BOOLEAN Win32kfullLoaded,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* Win32kbaseEntry,
    _Out_ ULONG64* MissingCapabilityMaskOut,
    _Out_ ULONG64* UserGetSiloGlobalsOut
    );

VOID
KswordARKWin32kCollectSessionSummary(
    _Inout_ KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE* Response,
    _In_ size_t EntryCapacity,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request
    );
