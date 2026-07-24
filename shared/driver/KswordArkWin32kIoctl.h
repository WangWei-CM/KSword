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
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_WINDOW_DETAIL  0x895UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_TIMERS          0x896UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_EVENT_HOOKS     0x897UL

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

// 单窗口运行时详情协议：
// - 输入：HWND/PID/TID 过滤信息只作为定位线索，不信任 R3 传入 tagWND 地址；
// - 处理：当前阶段返回 profile/capability/offset readiness，后续可由 tagWND PDB profile 扩展；
// - 输出：固定响应包，确保 UI 有稳定 unsupported/profile-missing 解释而不是空表。
#define IOCTL_KSWORD_ARK_QUERY_WIN32K_WINDOW_DETAIL \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_WINDOW_DETAIL, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

// 窗口定时器查询只读遍历 win32kbase 导出的 gTimerHashTable，不提供删除或修改入口。
#define IOCTL_KSWORD_ARK_QUERY_WIN32K_TIMERS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_TIMERS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

// WinEvent Hook 查询只读遍历 win32kbase!gpWinEventHooks，不提供 unhook 入口。
#define IOCTL_KSWORD_ARK_QUERY_WIN32K_EVENT_HOOKS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WIN32K_EVENT_HOOKS, \
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
#define KSWORD_ARK_WIN32K_CAP_TIMER_HASH_EXPORT          0x0000000000001000ULL
#define KSWORD_ARK_WIN32K_CAP_TIMER_LAYOUT               0x0000000000002000ULL
#define KSWORD_ARK_WIN32K_CAP_TIMER_ENUM                 0x0000000000004000ULL
#define KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL          0x0000000000008000ULL
#define KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_LAYOUT          0x0000000000010000ULL
#define KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM            0x0000000000020000ULL
#define KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_LAYOUT        0x0000000000040000ULL
#define KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM          0x0000000000080000ULL
#define KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_MODULE_TABLE  0x0000000000100000ULL

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
#define KSWORD_ARK_WIN32K_FIELD_DETAIL_PROFILE      0x00000200UL
#define KSWORD_ARK_WIN32K_FIELD_DETAIL_IDENTITY     0x00000400UL
#define KSWORD_ARK_WIN32K_FIELD_DETAIL_OFFSETS      0x00000800UL

#define KSWORD_ARK_WIN32K_TIMER_FIELD_OBJECT         0x00000001UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_THREAD         0x00000002UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_CALLBACK       0x00000004UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_INTERVAL       0x00000008UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_FLAGS          0x00000010UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_WINDOW         0x00000020UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_ID             0x00000040UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_ALTERNATE_THREAD 0x00000080UL
#define KSWORD_ARK_WIN32K_TIMER_FIELD_HASH_LINK      0x00000100UL

#define KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_UNKNOWN               0UL
#define KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY 1UL
#define KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_NEAREST_PREVIOUS      2UL

#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_HANDLE          0x00000001UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_OWNER           0x00000002UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_RANGE           0x00000004UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_FLAGS           0x00000008UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_TARGET          0x00000010UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_CALLBACK        0x00000020UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_MODULE_ATOM     0x00000040UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_NEXT            0x00000080UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_TIMESTAMP       0x00000100UL

#define KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT_SOURCE_UNKNOWN               0UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY 1UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT_SOURCE_NEAREST_PREVIOUS      2UL

#define KSWORD_ARK_WIN32K_EVENT_HOOK_FLAG_SKIP_OWN_THREAD  0x00000001UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FLAG_SKIP_OWN_PROCESS 0x00000002UL
#define KSWORD_ARK_WIN32K_EVENT_HOOK_FLAG_IN_CONTEXT       0x00000004UL

#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_HANDLE        0x00000001UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_OWNER         0x00000002UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_DESKTOP       0x00000004UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_NEXT          0x00000008UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_TYPE          0x00000010UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_PROCEDURE     0x00000020UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_FLAGS         0x00000040UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_MODULE        0x00000080UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_TARGET        0x00000100UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_CHAIN         0x00000200UL

#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT_SOURCE_UNKNOWN               0UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY 1UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT_SOURCE_NEAREST_PREVIOUS      2UL

#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_SCOPE_UNKNOWN 0UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_SCOPE_THREAD  1UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_SCOPE_GLOBAL  2UL

#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_SOURCE_THREAD 2UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_SOURCE_GLOBAL 3UL

#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_GLOBAL     0x00000001UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_ANSI       0x00000002UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_NEED_SKIP  0x00000004UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_HUNG       0x00000008UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_FAULTED    0x00000010UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_NO_DELAY   0x00000020UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_WOW64_DLL  0x00000040UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_FLAG_DESTROYED  0x00000080UL

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

// tagTIMER 的布局单独传输，避免改变已有 KSWORD_ARK_WIN32K_FIELD_OFFSETS ABI。
typedef struct _KSWORD_ARK_WIN32K_TIMER_LAYOUT
{
    unsigned long objectSize;
    unsigned long primaryThreadInfo;
    unsigned long callback;
    unsigned long countdown;
    unsigned long tolerance;
    unsigned long flags;
    unsigned long interval;
    unsigned long globalListEntry;
    unsigned long window;
    unsigned long timerId;
    unsigned long alternateThreadInfo;
    unsigned long hashListEntry;
    unsigned long timestamp;
    unsigned long bucketCount;
    unsigned long bucketStride;
    unsigned long source;
    unsigned long timeDateStamp;
    unsigned long imageSize;
} KSWORD_ARK_WIN32K_TIMER_LAYOUT;

// tagEVENTHOOK 布局独立传输，避免改变旧 Win32k profile ABI。
typedef struct _KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT
{
    unsigned long objectSize;
    unsigned long handle;
    unsigned long ownerThreadInfo;
    unsigned long nextHook;
    unsigned long eventMin;
    unsigned long eventMax;
    unsigned long internalFlags;
    unsigned long targetProcessId;
    unsigned long targetThreadId;
    unsigned long callbackOffset;
    unsigned long moduleAtom;
    unsigned long timestamp;
    unsigned long globalRva;
    unsigned long source;
    unsigned long timeDateStamp;
    unsigned long imageSize;
} KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT;

// tagHOOK/tagTHREADINFO/DESKTOPINFO 布局独立传输；source 区分精确身份与最近旧版回退。
typedef struct _KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT
{
    unsigned long objectSize;
    unsigned long handle;
    unsigned long ownerThreadInfo;
    unsigned long desktopObject;
    unsigned long nextHook;
    unsigned long hookType;
    unsigned long procedureOffset;
    unsigned long flags;
    unsigned long moduleId;
    unsigned long targetThreadInfo;
    unsigned long threadHookArray;
    unsigned long threadDesktopInfo;
    unsigned long desktopHookArray;
    unsigned long moduleAtomTableRva;
    unsigned long moduleAtomCountRva;
    unsigned long source;
    unsigned long timeDateStamp;
    unsigned long imageSize;
} KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT;

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

typedef struct _KSWORD_ARK_WIN32K_WINDOW_DETAIL_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long processId;
    unsigned long threadId;
    unsigned long long hwnd;
} KSWORD_ARK_WIN32K_WINDOW_DETAIL_REQUEST;

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
    unsigned long fieldFlags;
    unsigned long long hookObject;
    unsigned long long chainHead;
    unsigned long long nextHookObject;
    unsigned long long threadInfo;
    unsigned long long targetThreadInfo;
    unsigned long long desktopObject;
    unsigned long long procedureAddress;
    unsigned long long moduleBase;
    unsigned long long hookHandle;
    unsigned long long procedureOffset;
    unsigned long moduleId;
    unsigned long moduleAtom;
    unsigned long targetProcessId;
    unsigned long targetThreadId;
    unsigned long targetSessionId;
    unsigned long reserved;
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
    KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT layout;
    unsigned long discoveredChainCount;
    unsigned long visitedNodeCount;
    unsigned long readFailureCount;
    unsigned long corruptLinkCount;
    unsigned long duplicateCount;
    unsigned long win32kbaseTimeDateStamp;
    unsigned long win32kbaseImageSize;
    unsigned long win32kfullTimeDateStamp;
    unsigned long win32kfullImageSize;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
    KSWORD_ARK_WIN32K_HOOK_ENTRY entries[1];
} KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE;

typedef struct _KSWORD_ARK_WIN32K_WINDOW_DETAIL_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long processId;
    unsigned long threadId;
    unsigned long fieldFlags;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long hwnd;
    unsigned long long tagWnd;
    unsigned long long threadInfo;
    unsigned long long queueObject;
    unsigned long long desktopObject;
    unsigned long long capabilityMask;
    unsigned long long missingCapabilityMask;
    KSWORD_ARK_WIN32K_MODULE_STATE win32k;
    KSWORD_ARK_WIN32K_MODULE_STATE win32kbase;
    KSWORD_ARK_WIN32K_MODULE_STATE win32kfull;
    KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets;
    wchar_t title[KSWORD_ARK_WIN32K_TITLE_CHARS];
    wchar_t className[KSWORD_ARK_WIN32K_CLASS_CHARS];
    wchar_t detail[KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS];
} KSWORD_ARK_WIN32K_WINDOW_DETAIL_RESPONSE;

typedef struct _KSWORD_ARK_WIN32K_TIMER_ENTRY
{
    unsigned long fieldFlags;
    unsigned long status;
    unsigned long processId;
    unsigned long threadId;
    unsigned long sessionId;
    unsigned long flags;
    unsigned long intervalMs;
    unsigned long countdownMs;
    unsigned long toleranceMs;
    long lastStatus;
    unsigned long reserved;
    unsigned long long timerObject;
    unsigned long long callbackAddress;
    unsigned long long primaryThreadInfo;
    unsigned long long alternateThreadInfo;
    unsigned long long windowObject;
    unsigned long long timerId;
    unsigned long long hashLink;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
} KSWORD_ARK_WIN32K_TIMER_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_TIMER_SNAPSHOT_RESPONSE
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
    unsigned long long timerHashTable;
    unsigned long visitedNodeCount;
    unsigned long readFailureCount;
    unsigned long corruptBucketCount;
    unsigned long duplicateCount;
    unsigned long win32kbaseTimeDateStamp;
    unsigned long win32kbaseImageSize;
    unsigned long win32kfullTimeDateStamp;
    unsigned long win32kfullImageSize;
    KSWORD_ARK_WIN32K_TIMER_LAYOUT layout;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
    KSWORD_ARK_WIN32K_TIMER_ENTRY entries[1];
} KSWORD_ARK_WIN32K_TIMER_SNAPSHOT_RESPONSE;

typedef struct _KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY
{
    unsigned long fieldFlags;
    unsigned long status;
    unsigned long processId;
    unsigned long threadId;
    unsigned long sessionId;
    unsigned long flags;
    unsigned long internalFlags;
    unsigned long eventMin;
    unsigned long eventMax;
    unsigned long targetProcessId;
    unsigned long targetThreadId;
    unsigned long moduleAtom;
    unsigned long installTime;
    long lastStatus;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long long hookHandle;
    unsigned long long hookObject;
    unsigned long long nextHookObject;
    unsigned long long ownerThreadInfo;
    unsigned long long callbackAddress;
    unsigned long long callbackOffset;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
} KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_EVENT_HOOK_SNAPSHOT_RESPONSE
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
    unsigned long long hookListPointer;
    unsigned long long hookListHead;
    unsigned long visitedNodeCount;
    unsigned long readFailureCount;
    unsigned long corruptLinkCount;
    unsigned long duplicateCount;
    unsigned long win32kbaseTimeDateStamp;
    unsigned long win32kbaseImageSize;
    unsigned long win32kfullTimeDateStamp;
    unsigned long win32kfullImageSize;
    KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT layout;
    wchar_t detail[KSWORD_ARK_WIN32K_DETAIL_CHARS];
    KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY entries[1];
} KSWORD_ARK_WIN32K_EVENT_HOOK_SNAPSHOT_RESPONSE;
