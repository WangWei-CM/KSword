#pragma once

#include "KswordArkAlpcIoctl.h"

// ============================================================
// KswordArkKernelObjectIoctl.h
// 作用：
// - 定义 CID table、Kernel Object 摘要、IPC 摘要的只读 R3/R0 协议；
// - 当前协议只返回审计证据和降级状态，不提供 patch/delete/unlink/remove；
// - 所有 IOCTL 均使用 METHOD_BUFFERED + FILE_ANY_ACCESS。
// ============================================================

#define KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_CID_TABLE             0x878UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_KERNEL_OBJECT_SUMMARY 0x879UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_IPC_SUMMARY          0x87AUL

#define IOCTL_KSWORD_ARK_ENUM_CID_TABLE \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_CID_TABLE, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_KERNEL_OBJECT_SUMMARY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_IPC_SUMMARY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_IPC_SUMMARY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_PROCESS 0x00000001UL
#define KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_THREAD  0x00000002UL
#define KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_PROCESS | KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_THREAD)

#define KSWORD_ARK_CID_OBJECT_KIND_UNKNOWN 0UL
#define KSWORD_ARK_CID_OBJECT_KIND_PROCESS 1UL
#define KSWORD_ARK_CID_OBJECT_KIND_THREAD  2UL

#define KSWORD_ARK_CID_ENTRY_FLAG_DANGLING      0x00000001UL
#define KSWORD_ARK_CID_ENTRY_FLAG_TYPE_MISMATCH 0x00000002UL
#define KSWORD_ARK_CID_ENTRY_FLAG_REFERENCED    0x00000004UL

#define KSWORD_ARK_CID_ENUM_STATUS_UNAVAILABLE          0UL
#define KSWORD_ARK_CID_ENUM_STATUS_OK                   1UL
#define KSWORD_ARK_CID_ENUM_STATUS_PARTIAL              2UL
#define KSWORD_ARK_CID_ENUM_STATUS_DYNDATA_MISSING      3UL
#define KSWORD_ARK_CID_ENUM_STATUS_PSPCID_UNAVAILABLE   4UL
#define KSWORD_ARK_CID_ENUM_STATUS_TYPE_UNAVAILABLE     5UL
#define KSWORD_ARK_CID_ENUM_STATUS_BUFFER_TRUNCATED     6UL
#define KSWORD_ARK_CID_ENUM_STATUS_BUDGET_EXHAUSTED     7UL

#define KSWORD_ARK_OBJECT_SUMMARY_FLAG_BY_CID           0x00000001UL
#define KSWORD_ARK_OBJECT_SUMMARY_FLAG_INCLUDE_TYPE     0x00000002UL
#define KSWORD_ARK_OBJECT_SUMMARY_FLAG_INCLUDE_COUNTERS 0x00000004UL
#define KSWORD_ARK_OBJECT_SUMMARY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_OBJECT_SUMMARY_FLAG_BY_CID | \
     KSWORD_ARK_OBJECT_SUMMARY_FLAG_INCLUDE_TYPE | \
     KSWORD_ARK_OBJECT_SUMMARY_FLAG_INCLUDE_COUNTERS)

#define KSWORD_ARK_OBJECT_SUMMARY_FIELD_OBJECT_PRESENT        0x00000001UL
#define KSWORD_ARK_OBJECT_SUMMARY_FIELD_TYPE_PRESENT          0x00000002UL
#define KSWORD_ARK_OBJECT_SUMMARY_FIELD_TYPE_NAME_PRESENT     0x00000004UL
#define KSWORD_ARK_OBJECT_SUMMARY_FIELD_TYPE_INDEX_PRESENT    0x00000008UL
#define KSWORD_ARK_OBJECT_SUMMARY_FIELD_POINTER_COUNT_PRESENT 0x00000010UL
#define KSWORD_ARK_OBJECT_SUMMARY_FIELD_HANDLE_COUNT_PRESENT  0x00000020UL

#define KSWORD_ARK_OBJECT_HEADER_STATUS_UNAVAILABLE       0UL
#define KSWORD_ARK_OBJECT_HEADER_STATUS_PROFILE_MISSING   1UL
#define KSWORD_ARK_OBJECT_HEADER_STATUS_PARTIAL_PROFILE   2UL
#define KSWORD_ARK_OBJECT_HEADER_STATUS_AVAILABLE         3UL

#define KSWORD_ARK_OBJECT_SUMMARY_STATUS_UNAVAILABLE           0UL
#define KSWORD_ARK_OBJECT_SUMMARY_STATUS_OK                    1UL
#define KSWORD_ARK_OBJECT_SUMMARY_STATUS_PARTIAL               2UL
#define KSWORD_ARK_OBJECT_SUMMARY_STATUS_UNSUPPORTED_TARGET    3UL
#define KSWORD_ARK_OBJECT_SUMMARY_STATUS_LOOKUP_FAILED         4UL
#define KSWORD_ARK_OBJECT_SUMMARY_STATUS_TYPE_QUERY_FAILED     5UL
#define KSWORD_ARK_OBJECT_SUMMARY_STATUS_COUNTERS_UNAVAILABLE  6UL

#define KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALPC     0x00000001UL
#define KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_PIPE     0x00000002UL
#define KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_MAILSLOT 0x00000004UL
#define KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALPC | \
     KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_PIPE | \
     KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_MAILSLOT)

#define KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_IPC_SUMMARY_STATUS_OK          1UL
#define KSWORD_ARK_IPC_SUMMARY_STATUS_PARTIAL     2UL
#define KSWORD_ARK_IPC_SUMMARY_STATUS_STUB        3UL
#define KSWORD_ARK_IPC_SUMMARY_STATUS_FAILED      4UL

#define KSWORD_ARK_KERNEL_OBJECT_TYPE_NAME_CHARS 96U
#define KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS 160U
#define KSWORD_ARK_KERNEL_OBJECT_OFFSET_UNAVAILABLE 0xFFFFFFFFUL

typedef struct _KSWORD_ARK_ENUM_CID_TABLE_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long maxEntries;
    unsigned long maxVisitCount;
    unsigned long startCid;
    unsigned long endCid;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_ENUM_CID_TABLE_REQUEST;

typedef struct _KSWORD_ARK_CID_TABLE_ENTRY
{
    unsigned long cidValue;
    unsigned long handleIndex;
    unsigned long expectedObjectKind;
    unsigned long lookupStatus;
    unsigned long flags;
    long referenceStatus;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long long objectAddress;
} KSWORD_ARK_CID_TABLE_ENTRY;

typedef struct _KSWORD_ARK_ENUM_CID_TABLE_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    unsigned long visitedCount;
    unsigned long maxVisitCount;
    long lastStatus;
    unsigned long reserved;
    unsigned long long pspCidTableAddress;
    unsigned long long dynDataCapabilityMask;
    unsigned long htTableCodeOffset;
    unsigned long hteLowValueOffset;
    KSWORD_ARK_CID_TABLE_ENTRY entries[1];
} KSWORD_ARK_ENUM_CID_TABLE_RESPONSE;

typedef struct _KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long targetKind;
    unsigned long cidValue;
    unsigned long long expectedObjectAddress;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_REQUEST;

typedef struct _KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long status;
    unsigned long fieldFlags;
    unsigned long targetKind;
    unsigned long cidValue;
    long lookupStatus;
    long typeStatus;
    long counterStatus;
    unsigned long objectHeaderStatus;
    unsigned long typeIndex;
    unsigned long pointerCount;
    unsigned long handleCount;
    unsigned long reserved0;
    unsigned long long objectAddress;
    unsigned long long expectedObjectAddress;
    unsigned long long objectTypeAddress;
    unsigned long long dynDataCapabilityMask;
    unsigned long otNameOffset;
    unsigned long otIndexOffset;
    wchar_t typeName[KSWORD_ARK_KERNEL_OBJECT_TYPE_NAME_CHARS];
    wchar_t detail[KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS];
} KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_RESPONSE;

typedef struct _KSWORD_ARK_QUERY_IPC_SUMMARY_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long processId;
    unsigned long reserved0;
    unsigned long long handleValue;
    unsigned long maxEntries;
    unsigned long reserved1;
} KSWORD_ARK_QUERY_IPC_SUMMARY_REQUEST;

typedef struct _KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long status;
    unsigned long fieldFlags;
    unsigned long processId;
    unsigned long alpcStatus;
    unsigned long namedPipeStatus;
    unsigned long mailslotStatus;
    long lastStatus;
    unsigned long reserved0;
    unsigned long long handleValue;
    unsigned long long alpcObjectAddress;
    unsigned long long dynDataCapabilityMask;
    wchar_t alpcTypeName[KSWORD_ARK_KERNEL_OBJECT_TYPE_NAME_CHARS];
    wchar_t detail[KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS];
} KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE;
