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
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_INTEGRITY 0x849UL

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

#define IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_INTEGRITY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_ENUM_SSDT_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_DRIVER_OBJECT_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_KERNEL_HOOK_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_DRIVER_INTEGRITY_PROTOCOL_VERSION 1UL

// Request flags.
#define KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED 0x00000001UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN     0x00000001UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL  0x00000002UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_MODULE_FILTER     0x00000004UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS   0x00000008UL
#define KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS   0x00000010UL
#define KSWORD_ARK_KERNEL_PATCH_FLAG_FORCE            0x00000001UL
// Driver unload flags。
// - CLEAR_DISPATCH_ON_NO_UNLOAD：目标没有 DriverUnload 时把 MajorFunction 替换为拒绝 IRP stub，并清 FastIo。
// - CLEAR_DISPATCH_AFTER_UNLOAD：DriverUnload 返回后把 MajorFunction 替换为拒绝 IRP stub，并清 FastIo。
// - CLEAR_UNLOAD_POINTER：清空 DriverObject->DriverUnload，避免重复入口被误调用。
// - DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD：目标没有 DriverUnload 时尝试删除 DeviceObject 链。
// - DELETE_DEVICE_OBJECTS_ALWAYS：最高风险实验强拆；即使 DriverUnload 返回也尝试删除 DeviceObject 链。
// - MAKE_TEMPORARY_OBJECT：调用 ObMakeTemporaryObject，配合引用释放触发对象回收。
// - TARGET_MODULE_BASE_PRESENT：请求携带模块基址，R0 先按 DriverObject->DriverStart 反查目标。
// - REMOVE_CALLBACKS_BY_MODULE_BASE：请求携带模块基址时，先批量移除该模块登记的可移除回调。
// - ALLOW_DESTRUCTIVE_CLEANUP：显式允许持久 DriverObject 改写和高危后处理。
// 注意：CLEAR_* / DELETE_* / MAKE_TEMPORARY / REMOVE_CALLBACKS 均可能破坏目标驱动后续正常卸载；
//      R0 会把缺少 ALLOW_DESTRUCTIVE_CLEANUP 的旧 FORCE_CLEANUP 请求降级为“仅调用 DriverUnload”。
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD      0x00000001UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD      0x00000002UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER             0x00000004UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD 0x00000008UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT            0x00000010UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_TARGET_MODULE_BASE_PRESENT        0x00000020UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_REMOVE_CALLBACKS_BY_MODULE_BASE   0x00000040UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP         0x00000080UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ALWAYS      0x00000100UL
#define KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP \
    (KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD | \
     KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD | \
     KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER)

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

// Driver Integrity query flags.
// - DRIVER_OBJECT: collect DriverObject, KLDR alignment, dispatch/FastIo, and device-chain evidence.
// - SERVICE: read Services\<Name> metadata without modifying service state.
// - CPU: collect per-CPU control-register, IDTR/GDTR, and MSR/LSTAR evidence.
// - IDT_ENTRIES: expand per-vector IDT handler module-attribution rows.
// - OPTIONAL_GLOBALS: report MmUnloadedDrivers/PiDDBCacheTable availability when DynData permits.
#define KSWORD_ARK_DRIVER_INTEGRITY_FLAG_DRIVER_OBJECT     0x00000001UL
#define KSWORD_ARK_DRIVER_INTEGRITY_FLAG_SERVICE           0x00000002UL
#define KSWORD_ARK_DRIVER_INTEGRITY_FLAG_CPU               0x00000004UL
#define KSWORD_ARK_DRIVER_INTEGRITY_FLAG_IDT_ENTRIES       0x00000008UL
#define KSWORD_ARK_DRIVER_INTEGRITY_FLAG_OPTIONAL_GLOBALS  0x00000010UL
#define KSWORD_ARK_DRIVER_INTEGRITY_FLAG_DEFAULT \
    (KSWORD_ARK_DRIVER_INTEGRITY_FLAG_DRIVER_OBJECT | \
     KSWORD_ARK_DRIVER_INTEGRITY_FLAG_SERVICE | \
     KSWORD_ARK_DRIVER_INTEGRITY_FLAG_CPU | \
     KSWORD_ARK_DRIVER_INTEGRITY_FLAG_IDT_ENTRIES)

// Driver Integrity source mask.
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_SYSTEM_MODULE       0x00000001UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_AUXKLIB             0x00000002UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_PS_LOADED_MODULES   0x00000004UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_DRIVER_OBJECT       0x00000008UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_DRIVER_SECTION      0x00000010UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_SERVICE_REGISTRY    0x00000020UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_CPU_REGISTER        0x00000040UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_IDT                 0x00000080UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_GDT                 0x00000100UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_MSR                 0x00000200UL
#define KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_DYNDATA             0x00000400UL

// Driver Integrity risk flags.
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE                  0x00000000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_UNAVAILABLE           0x00000001UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_QUERY_FAILED          0x00000002UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED     0x00000004UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_OWNER_MISMATCH        0x00000008UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_OUTSIDE_DRIVER_IMAGE  0x00000010UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_SECTION_MISMATCH      0x00000020UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_SERVICE_MISSING       0x00000040UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_EMPTY_UNLOAD          0x00000080UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_DEVICE_LOOP           0x00000100UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_ATTACHED_LOOP         0x00000200UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_CROSS_DRIVER_ATTACH   0x00000400UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_NULL_POINTER          0x00000800UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER    0x00001000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED       0x00002000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_NXE_DISABLED      0x00004000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMEP_DISABLED     0x00008000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMAP_DISABLED     0x00010000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_DESCRIPTOR_INVALID    0x00020000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_DYNDATA_UNAVAILABLE   0x00040000UL
#define KSWORD_ARK_DRIVER_INTEGRITY_RISK_TRUNCATED             0x00080000UL

// Driver Integrity evidence classes.
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MODULE_VIEW          1UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_PS_LOADED_MODULES    2UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DRIVER_OBJECT        3UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DRIVER_SECTION       4UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MAJOR_FUNCTION       5UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_FAST_IO              6UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DEVICE_CHAIN         7UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_SERVICE              8UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_CPU_CONTROL          9UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE     10UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY            11UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER          12UL
#define KSWORD_ARK_DRIVER_INTEGRITY_CLASS_OPTIONAL_GLOBAL      13UL

// Driver Integrity query status.
#define KSWORD_ARK_DRIVER_INTEGRITY_STATUS_UNAVAILABLE         0UL
#define KSWORD_ARK_DRIVER_INTEGRITY_STATUS_OK                  1UL
#define KSWORD_ARK_DRIVER_INTEGRITY_STATUS_PARTIAL             2UL
#define KSWORD_ARK_DRIVER_INTEGRITY_STATUS_NOT_FOUND           3UL
#define KSWORD_ARK_DRIVER_INTEGRITY_STATUS_BUFFER_TOO_SMALL    4UL
#define KSWORD_ARK_DRIVER_INTEGRITY_STATUS_QUERY_FAILED        5UL

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
#define KSWORD_ARK_DRIVER_INTEGRITY_DETAIL_CHARS 256U
#define KSWORD_ARK_DRIVER_INTEGRITY_OWNER_CHARS 96U
#define KSWORD_ARK_DRIVER_MAJOR_FUNCTION_COUNT 28U
#define KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT 96UL
#define KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT 16UL
#define KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS 4096UL
#define KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS 256UL

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

/*
 * Inline Hook 行兼容说明：
 * expectedBytes 是旧协议字段名，当前 R0 填入运行时观察基线，
 * 不代表磁盘干净/原始字节。R3 如需干净基线，应按 moduleBase
 * 与 functionAddress 计算 RVA 后从磁盘模块文件读取并自行比较。
 */
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

typedef struct _KSWORD_ARK_QUERY_DRIVER_INTEGRITY_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long maxRows;
    unsigned long maxIdtVectorsPerCpu;
    unsigned long maxDevices;
    unsigned long maxAttachedDevices;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long long targetModuleBase;
    wchar_t driverName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS];
} KSWORD_ARK_QUERY_DRIVER_INTEGRITY_REQUEST;

typedef struct _KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE
{
    unsigned long evidenceClass;
    unsigned long riskFlags;
    unsigned long sourceMask;
    unsigned long confidence;
    unsigned long processorGroup;
    unsigned long processorNumber;
    unsigned long vector;
    unsigned long ownerModuleSize;
    unsigned long long objectAddress;
    unsigned long long targetAddress;
    unsigned long long ownerModuleBase;
    wchar_t ownerModule[KSWORD_ARK_DRIVER_INTEGRITY_OWNER_CHARS];
    wchar_t detail[KSWORD_ARK_DRIVER_INTEGRITY_DETAIL_CHARS];
} KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE;

typedef struct _KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE
{
    unsigned long version;
    unsigned long queryStatus;
    unsigned long flags;
    unsigned long sourceMask;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long cpuCount;
    unsigned long moduleCount;
    unsigned long reserved;
    long lastStatus;
    unsigned long reserved1;
    KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE entries[1];
} KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE;

typedef struct _KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long timeoutMilliseconds;
    unsigned long reserved;
    wchar_t driverName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS];
    unsigned long long targetModuleBase;
} KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST;

typedef struct _KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long flags;
    unsigned long reserved; // requestedFlags：R3 原始请求 flags；R0 日志用于区分“未请求”和“preflight 降级”。
    long lastStatus;
    long waitStatus;
    unsigned long cleanupFlagsApplied;
    unsigned long deletedDeviceCount;
    unsigned long long driverObjectAddress;
    unsigned long long driverUnloadAddress;
    unsigned long callbackCandidates;
    unsigned long callbacksRemoved;
    unsigned long callbackFailures;
    long callbackLastStatus;
    wchar_t driverName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS];
} KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE;
