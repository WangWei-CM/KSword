#pragma once

#include "KswordArkDynDataIoctl.h"

// ============================================================
// KswordArkThreadIoctl.h
// 作用：
// - 定义 R3/R0 线程扩展枚举协议；
// - 线程基础列表仍可由 R3 NtQuerySystemInformation 提供；
// - KTHREAD 栈边界和 I/O counter 由本协议按 DynData capability 补齐。
// ============================================================

#define KSWORD_ARK_THREAD_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_THREAD 0x80B
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_THREAD_CROSSVIEW 0x837UL

#define IOCTL_KSWORD_ARK_ENUM_THREAD \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_THREAD, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_THREAD_CROSSVIEW, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_STACK    0x00000001UL
#define KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_IO       0x00000002UL
#define KSWORD_ARK_ENUM_THREAD_FLAG_SCAN_CID_TABLE   0x00000004UL
#define KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_STACK | KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_IO)

#define KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_PUBLIC_WALK 0x00000001UL
#define KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_THREAD_LIST 0x00000002UL
#define KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_CID_TABLE   0x00000004UL
#define KSWORD_ARK_THREAD_CROSSVIEW_FLAG_VALIDATE_START      0x00000008UL
#define KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_PUBLIC_WALK | \
     KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_THREAD_LIST | \
     KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_CID_TABLE | \
     KSWORD_ARK_THREAD_CROSSVIEW_FLAG_VALIDATE_START)

#define KSWORD_ARK_THREAD_FLAG_KERNEL_ENUMERATED                0x00000001UL
#define KSWORD_ARK_THREAD_FLAG_HIDDEN_FROM_ACTIVE_THREAD_LIST   0x00000002UL
#define KSWORD_ARK_THREAD_FLAG_OWNER_PROCESS_HIDDEN             0x00000004UL

#define KSWORD_ARK_THREAD_FIELD_INITIAL_STACK_PRESENT          0x00000001UL
#define KSWORD_ARK_THREAD_FIELD_STACK_LIMIT_PRESENT            0x00000002UL
#define KSWORD_ARK_THREAD_FIELD_STACK_BASE_PRESENT             0x00000004UL
#define KSWORD_ARK_THREAD_FIELD_KERNEL_STACK_PRESENT           0x00000008UL
#define KSWORD_ARK_THREAD_FIELD_READ_OPERATION_COUNT_PRESENT   0x00000010UL
#define KSWORD_ARK_THREAD_FIELD_WRITE_OPERATION_COUNT_PRESENT  0x00000020UL
#define KSWORD_ARK_THREAD_FIELD_OTHER_OPERATION_COUNT_PRESENT  0x00000040UL
#define KSWORD_ARK_THREAD_FIELD_READ_TRANSFER_COUNT_PRESENT    0x00000080UL
#define KSWORD_ARK_THREAD_FIELD_WRITE_TRANSFER_COUNT_PRESENT   0x00000100UL
#define KSWORD_ARK_THREAD_FIELD_OTHER_TRANSFER_COUNT_PRESENT   0x00000200UL

#define KSWORD_ARK_THREAD_R0_STATUS_UNAVAILABLE     0UL
#define KSWORD_ARK_THREAD_R0_STATUS_OK              1UL
#define KSWORD_ARK_THREAD_R0_STATUS_PARTIAL         2UL
#define KSWORD_ARK_THREAD_R0_STATUS_DYNDATA_MISSING 3UL
#define KSWORD_ARK_THREAD_R0_STATUS_READ_FAILED     4UL

typedef struct _KSWORD_ARK_ENUM_THREAD_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_ENUM_THREAD_REQUEST;

typedef struct _KSWORD_ARK_THREAD_ENTRY
{
    unsigned long threadId;
    unsigned long processId;
    unsigned long flags;
    unsigned long fieldFlags;
    unsigned long r0Status;
    unsigned long stackFieldSource;
    unsigned long ioFieldSource;
    unsigned long reserved;
    unsigned long long initialStack;
    unsigned long long stackLimit;
    unsigned long long stackBase;
    unsigned long long kernelStack;
    unsigned long long readOperationCount;
    unsigned long long writeOperationCount;
    unsigned long long otherOperationCount;
    unsigned long long readTransferCount;
    unsigned long long writeTransferCount;
    unsigned long long otherTransferCount;
    unsigned long ktInitialStackOffset;
    unsigned long ktStackLimitOffset;
    unsigned long ktStackBaseOffset;
    unsigned long ktKernelStackOffset;
    unsigned long ktReadOperationCountOffset;
    unsigned long ktWriteOperationCountOffset;
    unsigned long ktOtherOperationCountOffset;
    unsigned long ktReadTransferCountOffset;
    unsigned long ktWriteTransferCountOffset;
    unsigned long ktOtherTransferCountOffset;
    unsigned long long dynDataCapabilityMask;
} KSWORD_ARK_THREAD_ENTRY;

typedef struct _KSWORD_ARK_ENUM_THREAD_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    KSWORD_ARK_THREAD_ENTRY entries[1];
} KSWORD_ARK_ENUM_THREAD_RESPONSE;

typedef struct _KSWORD_ARK_THREAD_CROSSVIEW_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long processId;
    unsigned long startTid;
    unsigned long endTid;
    unsigned long maxNodes;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_THREAD_CROSSVIEW_REQUEST;

typedef struct _KSWORD_ARK_THREAD_CROSSVIEW_ROW
{
    unsigned long long objectAddress;
    unsigned long long processObjectAddress;
    unsigned long long startAddress;
    unsigned long processId;
    unsigned long threadId;
    unsigned long sourceMask;
    unsigned long anomalyFlags;
    unsigned long long dynDataCapabilityMask;
    KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS fieldOffsets;
    long lastStatus;
    unsigned long confidence;
    char imageName[16];
    char detail[KSWORD_ARK_CROSSVIEW_DETAIL_CHARS];
    unsigned long publicThreadId;
    unsigned long threadListThreadId;
    unsigned long cidTableThreadId;
    unsigned long publicProcessId;
    unsigned long threadListProcessId;
    unsigned long cidTableProcessId;
    long publicWalkStatus;
    long threadListStatus;
    long cidTableStatus;
    long startAddressStatus;
    unsigned long detailStatus;
    unsigned long denoiseFlags;
} KSWORD_ARK_THREAD_CROSSVIEW_ROW;

typedef struct _KSWORD_ARK_THREAD_CROSSVIEW_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long reserved;
    unsigned long long dynDataCapabilityMask;
    unsigned long long missingCapabilityMask;
    long lastStatus;
    unsigned long reserved2;
    KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_THREAD_CROSSVIEW_ROW entries[1];
} KSWORD_ARK_THREAD_CROSSVIEW_RESPONSE;
