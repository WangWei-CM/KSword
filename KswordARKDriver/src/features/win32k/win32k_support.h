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

typedef struct _KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY
{
    ULONG64 ThreadInfo;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG SessionId;
} KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY;

// 每个私有 Win32k 布局表项必须把该身份结构放在首字段。
// Windows 版本用于“最近的上一个版本”排序，PE 身份仍用于精确匹配。
typedef struct _KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY
{
    ULONG windowsMajorVersion;
    ULONG windowsMinorVersion;
    ULONG windowsBuildNumber;
    ULONG windowsRevision;
    ULONG win32kbaseTimeDateStamp;
    ULONG win32kbaseImageSize;
    ULONG win32kfullTimeDateStamp;
    ULONG win32kfullImageSize;
} KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY;

typedef struct _KSWORD_ARK_WIN32K_LAYOUT_SELECTION
{
    ULONG profileIndex;
    ULONG source;
    ULONG currentWindowsMajorVersion;
    ULONG currentWindowsMinorVersion;
    ULONG currentWindowsBuildNumber;
    ULONG currentWindowsRevision;
    KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY selectedIdentity;
} KSWORD_ARK_WIN32K_LAYOUT_SELECTION;

#define KSWORD_ARK_WIN32K_LAYOUT_SELECTION_NONE             0UL
#define KSWORD_ARK_WIN32K_LAYOUT_SELECTION_EXACT_IDENTITY   1UL
#define KSWORD_ARK_WIN32K_LAYOUT_SELECTION_NEAREST_PREVIOUS 2UL

BOOLEAN
KswordARKWin32kSelectLayoutProfile(
    _In_reads_bytes_(ProfileCount * ProfileStride) const VOID* ProfileTable,
    _In_ ULONG ProfileCount,
    _In_ SIZE_T ProfileStride,
    _In_ ULONG CurrentWin32kbaseTimeDateStamp,
    _In_ ULONG CurrentWin32kbaseImageSize,
    _In_ ULONG CurrentWin32kfullTimeDateStamp,
    _In_ ULONG CurrentWin32kfullImageSize,
    _Out_ KSWORD_ARK_WIN32K_LAYOUT_SELECTION* Selection
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

NTSTATUS
KswordARKWin32kBuildGuiThreadMap(
    _In_ ULONG MaximumEntries,
    _In_ ULONG PoolTag,
    _Outptr_result_buffer_(*CountOut) KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY** MapOut,
    _Out_ ULONG* CountOut,
    _Out_ BOOLEAN* TruncatedOut
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
