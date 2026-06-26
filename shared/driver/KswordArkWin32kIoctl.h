#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkWin32kIoctl.h
// Purpose:
// - Define shared R3/R0 protocol packets for read-only win32k GUI audit.
// - Keep every packet diagnostic-only; no message hook, payload capture, or
//   state-changing operation is represented here.
// ============================================================

#define KSWORD_ARK_WIN32K_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_PROFILE_STATUS 0x890UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_WINDOWS        0x891UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_GUI_THREADS    0x892UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_HOTKEYS_PDB    0x893UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_HOOKS_PDB      0x894UL

#define IOCTL_KSWORD_ARK_QUERY_WIN32K_PROFILE_STATUS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_PROFILE_STATUS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_WIN32K_WINDOWS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_WINDOWS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_WIN32K_GUI_THREADS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_GUI_THREADS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_WIN32K_HOTKEYS_PDB \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_HOTKEYS_PDB, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_WIN32K_HOOKS_PDB \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_HOOKS_PDB, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_WIN32K_STATUS_UNKNOWN             0UL
#define KSWORD_ARK_WIN32K_STATUS_OK                  1UL
#define KSWORD_ARK_WIN32K_STATUS_PARTIAL             2UL
#define KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED         3UL
#define KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING     4UL
#define KSWORD_ARK_WIN32K_STATUS_WIN32K_NOT_FOUND    5UL
#define KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED    6UL
#define KSWORD_ARK_WIN32K_STATUS_READ_FAILED         7UL
#define KSWORD_ARK_WIN32K_STATUS_ENUM_FAILED         8UL

#define KSWORD_ARK_WIN32K_PROFILE_STATE_UNKNOWN      0UL
#define KSWORD_ARK_WIN32K_PROFILE_STATE_NOT_LOADED   1UL
#define KSWORD_ARK_WIN32K_PROFILE_STATE_MISSING      2UL
#define KSWORD_ARK_WIN32K_PROFILE_STATE_MATCHED      3UL
#define KSWORD_ARK_WIN32K_PROFILE_STATE_MISMATCHED   4UL

#define KSWORD_ARK_WIN32K_CAP_WIN32K_LOADED              0x0000000000000001ULL
#define KSWORD_ARK_WIN32K_CAP_WIN32KBASE_LOADED          0x0000000000000002ULL
#define KSWORD_ARK_WIN32K_CAP_WIN32KFULL_LOADED          0x0000000000000004ULL
#define KSWORD_ARK_WIN32K_CAP_USER_GET_SILO_GLOBALS      0x0000000000000008ULL
#define KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC          0x0000000000000010ULL
#define KSWORD_ARK_WIN32K_CAP_WIN32KBASE_PROFILE         0x0000000000000020ULL
#define KSWORD_ARK_WIN32K_CAP_WIN32KFULL_PROFILE         0x0000000000000040ULL
#define KSWORD_ARK_WIN32K_CAP_TAGWND_PROFILE             0x0000000000000080ULL
#define KSWORD_ARK_WIN32K_CAP_TAGTHREADINFO_PROFILE      0x0000000000000100ULL
#define KSWORD_ARK_WIN32K_CAP_TAGQ_PROFILE               0x0000000000000200ULL
#define KSWORD_ARK_WIN32K_CAP_HOTKEY_PROFILE             0x0000000000000400ULL
#define KSWORD_ARK_WIN32K_CAP_HOOK_PROFILE               0x0000000000000800ULL

#define KSWORD_ARK_WIN32K_QUERY_FLAG_CURRENT_SESSION_ONLY 0x00000001UL
#define KSWORD_ARK_WIN32K_QUERY_FLAG_INCLUDE_DIAGNOSTICS  0x00000002UL
#define KSWORD_ARK_WIN32K_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_WIN32K_QUERY_FLAG_CURRENT_SESSION_ONLY | \
     KSWORD_ARK_WIN32K_QUERY_FLAG_INCLUDE_DIAGNOSTICS)

#define KSWORD_ARK_WIN32K_FIELD_HWND_PRESENT        0x00000001UL
#define KSWORD_ARK_WIN32K_FIELD_TAGWND_PRESENT      0x00000002UL
#define KSWORD_ARK_WIN32K_FIELD_THREADINFO_PRESENT  0x00000004UL
#define KSWORD_ARK_WIN32K_FIELD_QUEUE_PRESENT       0x00000008UL
#define KSWORD_ARK_WIN32K_FIELD_DESKTOP_PRESENT     0x00000010UL
#define KSWORD_ARK_WIN32K_FIELD_STYLE_PRESENT       0x00000020UL
#define KSWORD_ARK_WIN32K_FIELD_RECT_PRESENT        0x00000040UL
#define KSWORD_ARK_WIN32K_FIELD_TITLE_PRESENT       0x00000080UL
#define KSWORD_ARK_WIN32K_FIELD_CLASS_PRESENT       0x00000100UL

#define KSWORD_ARK_WIN32K_READ_STATUS_NOT_REQUESTED 0UL
#define KSWORD_ARK_WIN32K_READ_STATUS_OK            1UL
#define KSWORD_ARK_WIN32K_READ_STATUS_UNSUPPORTED   2UL
#define KSWORD_ARK_WIN32K_READ_STATUS_PROFILE_MISSING 3UL
#define KSWORD_ARK_WIN32K_READ_STATUS_READ_FAILED   4UL
#define KSWORD_ARK_WIN32K_READ_STATUS_TRUNCATED     5UL

#define KSWORD_ARK_WIN32K_MODULE_NAME_CHARS 32U
#define KSWORD_ARK_WIN32K_DETAIL_CHARS 128U
#define KSWORD_ARK_WIN32K_TITLE_CHARS 96U
#define KSWORD_ARK_WIN32K_CLASS_CHARS 96U
#define KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES 1024UL
#define KSWORD_ARK_WIN32K_HARD_MAX_ENTRIES 8192UL
#define KSWORD_ARK_WIN32K_OFFSET_UNAVAILABLE 0xFFFFFFFFUL

typedef struct _KSWORD_ARK_WIN32K_RECT
{
    long left;
    long top;
    long right;
    long bottom;
} KSWORD_ARK_WIN32K_RECT;

typedef struct _KSWORD_ARK_WIN32K_QUERY_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long sessionId;
    unsigned long processId;
    unsigned long threadId;
    unsigned long maxEntries;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_WIN32K_QUERY_REQUEST;

typedef struct _KSWORD_ARK_WIN32K_MODULE_STATE
{
    unsigned long loaded;
    unsigned long profileState;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long long imageBase;
    unsigned long imageSize;
    unsigned long reserved2;
    wchar_t moduleName[KSWORD_ARK_WIN32K_MODULE_NAME_CHARS];
} KSWORD_ARK_WIN32K_MODULE_STATE;

typedef struct _KSWORD_ARK_WIN32K_FIELD_OFFSETS
{
    unsigned long tagWndThreadInfo;
    unsigned long tagWndParent;
    unsigned long tagWndOwner;
    unsigned long tagWndStyle;
    unsigned long tagWndExStyle;
    unsigned long tagWndRect;
    unsigned long tagWndClientRect;
    unsigned long tagWndClass;
    unsigned long tagWndTitle;
    unsigned long tagThreadInfoQueue;
    unsigned long tagThreadInfoDesktop;
    unsigned long tagQActiveWindow;
    unsigned long tagQFocusWindow;
    unsigned long tagQCaptureWindow;
    unsigned long tagQCaretWindow;
    unsigned long tagHookNext;
    unsigned long tagHookType;
    unsigned long tagHookProcedure;
    unsigned long tagHookTargetThreadInfo;
    unsigned long hotkeyNext;
    unsigned long hotkeyThreadInfo;
    unsigned long hotkeyWindow;
    unsigned long hotkeyModifiers;
    unsigned long hotkeyVirtualKey;
    unsigned long hotkeyId;
} KSWORD_ARK_WIN32K_FIELD_OFFSETS;

typedef struct _KSWORD_ARK_WIN32K_SESSION_ENTRY
{
    unsigned long sessionId;
    unsigned long status;
    unsigned long processCount;
    unsigned long guiThreadCount;
    unsigned long representativeProcessId;
    unsigned long representativeThreadId;
    unsigned long long capabilityMask;
    long lastStatus;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
} KSWORD_ARK_WIN32K_SESSION_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long capabilityMask;
    unsigned long long missingCapabilityMask;
    unsigned long long userGetSiloGlobals;
    KSWORD_ARK_WIN32K_MODULE_STATE win32k;
    KSWORD_ARK_WIN32K_MODULE_STATE win32kbase;
    KSWORD_ARK_WIN32K_MODULE_STATE win32kfull;
    KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_WIN32K_SESSION_ENTRY entries[1];
} KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE;

typedef struct _KSWORD_ARK_WIN32K_WINDOW_ENTRY
{
    unsigned long fieldFlags;
    unsigned long status;
    unsigned long processId;
    unsigned long threadId;
    unsigned long sessionId;
    unsigned long titleStatus;
    unsigned long classStatus;
    unsigned long reserved;
    unsigned long style;
    unsigned long exStyle;
    long lastStatus;
    unsigned long reserved2;
    unsigned long long hwnd;
    unsigned long long tagWnd;
    unsigned long long threadInfo;
    unsigned long long desktopObject;
    unsigned long long parentHwnd;
    unsigned long long ownerHwnd;
    KSWORD_ARK_WIN32K_RECT windowRect;
    KSWORD_ARK_WIN32K_RECT clientRect;
    wchar_t title[KSWORD_ARK_WIN32K_TITLE_CHARS];
    wchar_t className[KSWORD_ARK_WIN32K_CLASS_CHARS];
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
} KSWORD_ARK_WIN32K_WINDOW_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long capabilityMask;
    unsigned long long missingCapabilityMask;
    KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_WIN32K_WINDOW_ENTRY entries[1];
} KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE;

typedef struct _KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY
{
    unsigned long fieldFlags;
    unsigned long status;
    unsigned long processId;
    unsigned long threadId;
    unsigned long sessionId;
    unsigned long queueStatus;
    long lastStatus;
    unsigned long reserved;
    unsigned long long ethread;
    unsigned long long threadInfo;
    unsigned long long queueObject;
    unsigned long long desktopObject;
    unsigned long long activeHwnd;
    unsigned long long focusHwnd;
    unsigned long long captureHwnd;
    unsigned long long caretHwnd;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
} KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long capabilityMask;
    unsigned long long missingCapabilityMask;
    KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY entries[1];
} KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE;

typedef struct _KSWORD_ARK_WIN32K_HOTKEY_ENTRY
{
    unsigned long source;
    unsigned long status;
    unsigned long flags;
    unsigned long sessionId;
    unsigned long processId;
    unsigned long threadId;
    unsigned long modifiers;
    unsigned long virtualKey;
    unsigned long hotkeyId;
    unsigned long depth;
    long lastStatus;
    unsigned long reserved;
    unsigned long long hotkeyObject;
    unsigned long long nextHotkeyObject;
    unsigned long long hwnd;
    unsigned long long tagWnd;
    unsigned long long threadInfo;
    unsigned long long desktopObject;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
} KSWORD_ARK_WIN32K_HOTKEY_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long capabilityMask;
    unsigned long long missingCapabilityMask;
    KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_WIN32K_HOTKEY_ENTRY entries[1];
} KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE;

typedef struct _KSWORD_ARK_WIN32K_HOOK_ENTRY
{
    unsigned long source;
    unsigned long status;
    unsigned long flags;
    unsigned long sessionId;
    unsigned long processId;
    unsigned long threadId;
    unsigned long hookType;
    unsigned long hookScope;
    long lastStatus;
    unsigned long reserved;
    unsigned long long hookObject;
    unsigned long long chainHead;
    unsigned long long nextHookObject;
    unsigned long long threadInfo;
    unsigned long long targetThreadInfo;
    unsigned long long desktopObject;
    unsigned long long procedureAddress;
    unsigned long long moduleBase;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
} KSWORD_ARK_WIN32K_HOOK_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long capabilityMask;
    unsigned long long missingCapabilityMask;
    KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_WIN32K_HOOK_ENTRY entries[1];
} KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE;
