#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkKeyboardIoctl.h
// 作用：
// - 定义 R3 <-> R0 键盘热键/钩子只读枚举协议；
// - 热键枚举面向 win32k RegisterHotKey 内部表；
// - 钩子枚举面向 WH_KEYBOARD / WH_KEYBOARD_LL 链，仅返回诊断信息。
// ============================================================

#define KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_KEYBOARD_HOTKEYS 0x847UL
#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_KEYBOARD_HOOKS   0x848UL

#define IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_KEYBOARD_HOTKEYS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_KEYBOARD_HOOKS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM        0x00000001UL
#define KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS        0x00000002UL
#define KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS  0x00000004UL
#define KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS  0x00000008UL
#define KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS   0x00000010UL
#define KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM | \
     KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS | \
     KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS | \
     KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS)

#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN             0UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK                  1UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL             2UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNSUPPORTED         3UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND    4UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_PATTERN_NOT_FOUND   5UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE 6UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED    7UL
#define KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED         8UL

#define KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE      1UL
#define KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_THREAD_HOOK_CHAIN 2UL
#define KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_GLOBAL_HOOK_CHAIN 3UL

#define KSWORD_ARK_KEYBOARD_HOOK_SCOPE_UNKNOWN 0UL
#define KSWORD_ARK_KEYBOARD_HOOK_SCOPE_THREAD  1UL
#define KSWORD_ARK_KEYBOARD_HOOK_SCOPE_GLOBAL  2UL

#define KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD    2UL
#define KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD_LL 13UL

#define KSWORD_ARK_KEYBOARD_DETAIL_CHARS 128U

typedef struct _KSWORD_ARK_ENUM_KEYBOARD_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long processId;
    unsigned long maxEntries;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long reserved2;
    unsigned long reserved3;
} KSWORD_ARK_ENUM_KEYBOARD_REQUEST;

typedef struct _KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY
{
    unsigned long source;
    unsigned long status;
    unsigned long flags;
    unsigned long bucketIndex;
    unsigned long depth;
    unsigned long modifiers;
    unsigned long modifierFlags2;
    unsigned long virtualKey;
    unsigned long hotkeyId;
    unsigned long processId;
    unsigned long threadId;
    long lastStatus;
    unsigned long long hotkeyObject;
    unsigned long long nextHotkeyObject;
    unsigned long long sessionGlobals;
    unsigned long long threadInfo;
    unsigned long long threadObject;
    unsigned long long windowObject;
    wchar_t detail[KSWORD_ARK_KEYBOARD_DETAIL_CHARS];
} KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY;

typedef struct _KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long win32kBase;
    unsigned long long sessionGlobals;
    unsigned long tableOffset;
    unsigned long hotkeyNextOffset;
    unsigned long hotkeyModifiersOffset;
    unsigned long hotkeyVkOffset;
    unsigned long hotkeyIdOffset;
    KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY entries[1];
} KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE;

typedef struct _KSWORD_ARK_KEYBOARD_HOOK_ENTRY
{
    unsigned long source;
    unsigned long status;
    unsigned long flags;
    unsigned long hookType;
    unsigned long hookScope;
    unsigned long processId;
    unsigned long threadId;
    unsigned long moduleId;
    long lastStatus;
    unsigned long reserved;
    unsigned long long hookObject;
    unsigned long long chainHead;
    unsigned long long nextHookObject;
    unsigned long long threadInfo;
    unsigned long long targetThreadInfo;
    unsigned long long desktopInfo;
    unsigned long long procedureAddress;
    unsigned long long procedureOffset;
    unsigned long long moduleBase;
    wchar_t detail[KSWORD_ARK_KEYBOARD_DETAIL_CHARS];
} KSWORD_ARK_KEYBOARD_HOOK_ENTRY;

typedef struct _KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long win32kBase;
    unsigned long threadHookArrayOffset;
    unsigned long desktopInfoOffset;
    unsigned long desktopHookArrayOffset;
    unsigned long hookNextOffset;
    unsigned long hookTypeOffset;
    unsigned long hookProcedureOffset;
    unsigned long hookFlagsOffset;
    unsigned long hookModuleIdOffset;
    unsigned long hookTargetThreadInfoOffset;
    KSWORD_ARK_KEYBOARD_HOOK_ENTRY entries[1];
} KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE;
