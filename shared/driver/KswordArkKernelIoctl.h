#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkKernelIoctl.h
// 作用：
// - 定义 R3 <-> R0 内核检查协议；
// - 当前覆盖 SSDT/SSSDT 快照、Inline Hook、IAT/EAT Hook 与 DriverObject 查询；
// - 所有结构只用于诊断展示，不把内核地址作为后续操作凭据。
// ============================================================

#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_SSDT 0x806
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_OBJECT 0x811
#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_SHADOW_SSDT 0x81EUL
#define KSWORD_ARK_IOCTL_FUNCTION_SCAN_INLINE_HOOKS 0x81FUL
#define KSWORD_ARK_IOCTL_FUNCTION_PATCH_INLINE_HOOK 0x820UL
#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_IAT_EAT_HOOKS 0x821UL
#define KSWORD_ARK_IOCTL_FUNCTION_FORCE_UNLOAD_DRIVER 0x826UL

#define IOCTL_KSWORD_ARK_ENUM_SSDT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_SSDT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_OBJECT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_SHADOW_SSDT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SCAN_INLINE_HOOKS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_PATCH_INLINE_HOOK, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_IAT_EAT_HOOKS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_FORCE_UNLOAD_DRIVER, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define KSWORD_ARK_ENUM_SSDT_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_DRIVER_OBJECT_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_KERNEL_HOOK_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION 1UL

// Request flags.
#define KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED 0x00000001UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN     0x00000001UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL  0x00000002UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_MODULE_FILTER     0x00000004UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS   0x00000008UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS   0x00000010UL
#define KSWORD_ARK_KERNEL_PATCH_FLAG_FORCE            0x00000001UL
// Driver unload flags。
// - CLEAR_DISPATCH_ON_NO_UNLOAD：目标没有 DriverUnload 时清空 FastIo/MajorFunction。
// - CLEAR_DISPATCH_AFTER_UNLOAD：DriverUnload 返回后仍清空 FastIo/MajorFunction。
// - CLEAR_UNLOAD_POINTER：清空 DriverObject->DriverUnload，避免重复入口被误调用。
// - DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD：目标没有 DriverUnload 时尝试删除 DeviceObject 链。
// - MAKE_TEMPORARY_OBJECT：调用 ObMakeTemporaryObject，配合引用释放触发对象回收。
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD      0x00000001UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD      0x00000002UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER             0x00000004UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD 0x00000008UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT            0x00000010UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP \
    (KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD | \
     KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD | \
     KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER | \
     KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD | \
     KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT)

// DriverObject 查询 flags。
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_MAJOR_FUNCTIONS 0x00000001UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_DEVICES         0x00000002UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_NAMES           0x00000004UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ATTACHED        0x00000008UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_MAJOR_FUNCTIONS | \
     KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_DEVICES | \
     KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_NAMES | \
     KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ATTACHED)

// Entry flags.
#define KSWORD_ARK_SSDT_ENTRY_FLAG_INDEX_RESOLVED 0x00000001UL
#define KSWORD_ARK_SSDT_ENTRY_FLAG_TABLE_ADDRESS_VALID 0x00000002UL
#define KSWORD_ARK_SSDT_ENTRY_FLAG_SHADOW_TABLE        0x00000004UL
#define KSWORD_ARK_SSDT_ENTRY_FLAG_STUB_EXPORT         0x00000008UL

// Kernel hook 行状态。
#define KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN             0UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN               1UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS          2UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH     3UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED         4UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED        5UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED      6UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED             7UL
#define KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED        8UL

// Inline Hook 类型。
#define KSWORD_ARK_INLINE_HOOK_TYPE_NONE                  0UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL32             1UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL8              2UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_JMP_RIP_INDIRECT      3UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_MOV_RAX_JMP_RAX       4UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_MOV_R11_JMP_R11       5UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_RET_PATCH             6UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_INT3_PATCH            7UL
#define KSWORD_ARK_INLINE_HOOK_TYPE_UNKNOWN_PATCH         8UL

// Inline 修复模式。
#define KSWORD_ARK_INLINE_PATCH_MODE_NOP_BRANCH           1UL
#define KSWORD_ARK_INLINE_PATCH_MODE_RESTORE_BYTES        2UL

// IAT/EAT Hook 类型。
#define KSWORD_ARK_IAT_EAT_HOOK_CLASS_IAT                 1UL
#define KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT                 2UL

// DriverObject 查询状态。
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_UNAVAILABLE      0UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK               1UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL          2UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NAME_INVALID     3UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NOT_FOUND        4UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_REFERENCE_FAILED 5UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_BUFFER_TOO_SMALL 6UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_QUERY_FAILED     7UL

// Driver force-unload 状态。
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNKNOWN            0UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED           1UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOAD_ROUTINE_MISSING 2UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_REFERENCE_FAILED    3UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_THREAD_FAILED       4UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_WAIT_TIMEOUT        5UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED    6UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP      7UL
#define KSWORD_ARK_DRIVER_UNLOAD_STATUS_CLEANUP_FAILED      8UL

// DriverObject field flags。
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_BASIC_PRESENT         0x00000001UL
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_DRIVER_NAME_PRESENT   0x00000002UL
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_SERVICE_KEY_PRESENT   0x00000004UL
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_IMAGE_PATH_PRESENT    0x00000008UL
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_MAJOR_PRESENT         0x00000010UL
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_DEVICE_PRESENT        0x00000020UL
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_DEVICE_TRUNCATED      0x00000040UL
#define KSWORD_ARK_DRIVER_OBJECT_FIELD_ATTACHED_TRUNCATED    0x00000080UL

#define KSWORD_ARK_SSDT_ENTRY_MAX_NAME 96U
#define KSWORD_ARK_SSDT_ENTRY_MAX_MODULE 64U
#define KSWORD_ARK_KERNEL_HOOK_NAME_CHARS 128U
#define KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS 64U
#define KSWORD_ARK_KERNEL_HOOK_BYTES 16U
#define KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES 32U
#define KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES 4096UL
#define KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS 260U
#define KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS 512U
#define KSWORD_ARK_DRIVER_SERVICE_KEY_CHARS 260U
#define KSWORD_ARK_DRIVER_DEVICE_NAME_CHARS 260U
#define KSWORD_ARK_DRIVER_MODULE_NAME_CHARS 64U
#define KSWORD_ARK_DRIVER_MAJOR_FUNCTION_COUNT 28U
#define KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT 96UL
#define KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT 16UL

typedef struct _KSWORD_ARK_ENUM_SSDT_REQUEST
{
    unsigned long flags;
    unsigned long reserved;
} KSWORD_ARK_ENUM_SSDT_REQUEST;

typedef struct _KSWORD_ARK_SSDT_ENTRY
{
    unsigned long serviceIndex;
    unsigned long flags;
    unsigned long long zwRoutineAddress;
    unsigned long long serviceRoutineAddress;
    char serviceName[KSWORD_ARK_SSDT_ENTRY_MAX_NAME];
    char moduleName[KSWORD_ARK_SSDT_ENTRY_MAX_MODULE];
} KSWORD_ARK_SSDT_ENTRY;

typedef struct _KSWORD_ARK_ENUM_SSDT_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long long serviceTableBase;
    unsigned long serviceCountFromTable;
    unsigned long reserved;
    KSWORD_ARK_SSDT_ENTRY entries[1];
} KSWORD_ARK_ENUM_SSDT_RESPONSE;

typedef struct _KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST
{
    unsigned long flags;
    unsigned long maxEntries;
    unsigned long reserved0;
    unsigned long reserved1;
    wchar_t moduleName[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS];
} KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST;

typedef struct _KSWORD_ARK_INLINE_HOOK_ENTRY
{
    unsigned long status;
    unsigned long hookType;
    unsigned long flags;
    unsigned long originalByteCount;
    unsigned long currentByteCount;
    unsigned long reserved;
    unsigned long long functionAddress;
    unsigned long long targetAddress;
    unsigned long long moduleBase;
    unsigned long long targetModuleBase;
    char functionName[KSWORD_ARK_KERNEL_HOOK_NAME_CHARS];
    wchar_t moduleName[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS];
    wchar_t targetModuleName[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS];
    unsigned char currentBytes[KSWORD_ARK_KERNEL_HOOK_BYTES];
    unsigned char expectedBytes[KSWORD_ARK_KERNEL_HOOK_BYTES];
} KSWORD_ARK_INLINE_HOOK_ENTRY;

typedef struct _KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long moduleCount;
    long lastStatus;
    unsigned long reserved;
    KSWORD_ARK_INLINE_HOOK_ENTRY entries[1];
} KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE;

typedef struct _KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST
{
    unsigned long flags;
    unsigned long mode;
    unsigned long patchBytes;
    unsigned long reserved;
    unsigned long long functionAddress;
    unsigned char expectedCurrentBytes[KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES];
    unsigned char restoreBytes[KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES];
} KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST;

typedef struct _KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long bytesPatched;
    unsigned long fieldFlags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long functionAddress;
    unsigned char beforeBytes[KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES];
    unsigned char afterBytes[KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES];
} KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE;

typedef struct _KSWORD_ARK_IAT_EAT_HOOK_ENTRY
{
    unsigned long hookClass;
    unsigned long status;
    unsigned long flags;
    unsigned long ordinal;
    unsigned long long moduleBase;
    unsigned long long thunkAddress;
    unsigned long long currentTarget;
    unsigned long long expectedTarget;
    unsigned long long targetModuleBase;
    char functionName[KSWORD_ARK_KERNEL_HOOK_NAME_CHARS];
    wchar_t moduleName[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS];
    wchar_t importModuleName[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS];
    wchar_t targetModuleName[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS];
} KSWORD_ARK_IAT_EAT_HOOK_ENTRY;

typedef struct _KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long moduleCount;
    long lastStatus;
    unsigned long reserved;
    KSWORD_ARK_IAT_EAT_HOOK_ENTRY entries[1];
} KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE;

typedef struct _KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST
{
    unsigned long flags;
    unsigned long maxDevices;
    unsigned long maxAttachedDevices;
    unsigned long reserved;
    wchar_t driverName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS];
} KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST;

typedef struct _KSWORD_ARK_DRIVER_MAJOR_FUNCTION_ENTRY
{
    unsigned long majorFunction;
    unsigned long flags;
    unsigned long long dispatchAddress;
    unsigned long long moduleBase;
    wchar_t moduleName[KSWORD_ARK_DRIVER_MODULE_NAME_CHARS];
} KSWORD_ARK_DRIVER_MAJOR_FUNCTION_ENTRY;

typedef struct _KSWORD_ARK_DRIVER_DEVICE_ENTRY
{
    unsigned long relationDepth;
    unsigned long deviceType;
    unsigned long flags;
    unsigned long characteristics;
    unsigned long stackSize;
    unsigned long alignmentRequirement;
    long nameStatus;
    unsigned long reserved;
    unsigned long long rootDeviceObjectAddress;
    unsigned long long deviceObjectAddress;
    unsigned long long nextDeviceObjectAddress;
    unsigned long long attachedDeviceObjectAddress;
    unsigned long long driverObjectAddress;
    wchar_t deviceName[KSWORD_ARK_DRIVER_DEVICE_NAME_CHARS];
} KSWORD_ARK_DRIVER_DEVICE_ENTRY;

typedef struct _KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE
{
    unsigned long version;
    unsigned long queryStatus;
    unsigned long fieldFlags;
    unsigned long majorFunctionCount;
    unsigned long totalDeviceCount;
    unsigned long returnedDeviceCount;
    unsigned long deviceEntrySize;
    unsigned long reserved;
    long lastStatus;
    unsigned long driverFlags;
    unsigned long driverSize;
    unsigned long reserved1;
    unsigned long long driverObjectAddress;
    unsigned long long driverStart;
    unsigned long long driverSection;
    unsigned long long driverUnload;
    wchar_t driverName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS];
    wchar_t serviceKeyName[KSWORD_ARK_DRIVER_SERVICE_KEY_CHARS];
    wchar_t imagePath[KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS];
    KSWORD_ARK_DRIVER_MAJOR_FUNCTION_ENTRY majorFunctions[KSWORD_ARK_DRIVER_MAJOR_FUNCTION_COUNT];
    KSWORD_ARK_DRIVER_DEVICE_ENTRY devices[1];
} KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE;

typedef struct _KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long timeoutMilliseconds;
    unsigned long reserved;
    wchar_t driverName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS];
} KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST;

typedef struct _KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long flags;
    unsigned long reserved;
    long lastStatus;
    long waitStatus;
    unsigned long reserved1;
    unsigned long reserved2;
    unsigned long long driverObjectAddress;
    unsigned long long driverUnloadAddress;
    wchar_t driverName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS];
} KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE;
