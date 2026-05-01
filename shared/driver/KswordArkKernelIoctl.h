#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkKernelIoctl.h
// 作用：
// - 定义 R3 <-> R0 内核检查协议；
// - 当前覆盖 SSDT 快照与 Phase-9 DriverObject/DeviceObject 查询；
// - 所有结构只用于诊断展示，不把内核地址作为后续操作凭据。
// ============================================================

#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_SSDT 0x806
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_OBJECT 0x811

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

#define KSWORD_ARK_ENUM_SSDT_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_DRIVER_OBJECT_PROTOCOL_VERSION 1UL

// Request flags.
#define KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED 0x00000001UL

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

// DriverObject 查询状态。
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_UNAVAILABLE      0UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK               1UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL          2UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NAME_INVALID     3UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NOT_FOUND        4UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_REFERENCE_FAILED 5UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_BUFFER_TOO_SMALL 6UL
#define KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_QUERY_FAILED     7UL

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
