#pragma once

#include "KswordArkDynDataIoctl.h"

// Shared protocol headers are included by both user-mode UI/client code and
// kernel-mode driver code.  Some translation units reach this file before the
// SDK/WDK headers that normally define CTL_CODE and the legacy oplock FSCTL
// constants, so keep small SDK-compatible fallbacks here.  The values match
// winioctl.h/ntifs.h and only activate when the platform headers have not
// already supplied the definitions.
#ifndef FILE_DEVICE_FILE_SYSTEM
#define FILE_DEVICE_FILE_SYSTEM 0x00000009
#endif

#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif

#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#ifndef FSCTL_REQUEST_OPLOCK_LEVEL_1
#define FSCTL_REQUEST_OPLOCK_LEVEL_1 CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_OPLOCK_LEVEL_2
#define FSCTL_REQUEST_OPLOCK_LEVEL_2 CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_BATCH_OPLOCK
#define FSCTL_REQUEST_BATCH_OPLOCK CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_OPLOCK_BREAK_ACKNOWLEDGE
#define FSCTL_OPLOCK_BREAK_ACKNOWLEDGE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_OPBATCH_ACK_CLOSE_PENDING
#define FSCTL_OPBATCH_ACK_CLOSE_PENDING CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_OPLOCK_BREAK_NOTIFY
#define FSCTL_OPLOCK_BREAK_NOTIFY CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 5, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_OPLOCK
#define FSCTL_REQUEST_OPLOCK CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 144, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_FILTER_OPLOCK
#define FSCTL_REQUEST_FILTER_OPLOCK CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 23, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

// ============================================================
// KswordArkFileMonitorIoctl.h
// 作用：
// - 定义 Phase-12 文件系统 minifilter 实时事件协议；
// - R0 只负责采集和缓冲，R3 负责虚拟化列表、过滤展示和导出；
// - 第一版过滤条件保持简单，复杂路径/扩展名过滤后续由 R3 或规则层扩展。
// ============================================================

#define KSWORD_ARK_FILE_MONITOR_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_FILE_MONITOR_CONTROL 0x815UL
#define KSWORD_ARK_IOCTL_FUNCTION_FILE_MONITOR_DRAIN 0x816UL
#define KSWORD_ARK_IOCTL_FUNCTION_FILE_MONITOR_QUERY_STATUS 0x817UL

#define IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_FILE_MONITOR_CONTROL, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_FILE_MONITOR_DRAIN \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_FILE_MONITOR_DRAIN, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_FILE_MONITOR_QUERY_STATUS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_FILE_MONITOR_ACTION_START 1UL
#define KSWORD_ARK_FILE_MONITOR_ACTION_STOP  2UL
#define KSWORD_ARK_FILE_MONITOR_ACTION_CLEAR 3UL

#define KSWORD_ARK_FILE_MONITOR_OPERATION_CREATE  0x00000001UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_READ    0x00000002UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_WRITE   0x00000004UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_SETINFO 0x00000008UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_RENAME  0x00000010UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_DELETE  0x00000020UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_CLEANUP 0x00000040UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_CLOSE   0x00000080UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_FSCTL   0x00000100UL
#define KSWORD_ARK_FILE_MONITOR_OPERATION_ALL \
    (KSWORD_ARK_FILE_MONITOR_OPERATION_CREATE | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_READ | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_WRITE | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_SETINFO | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_RENAME | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_DELETE | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_CLEANUP | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_CLOSE | \
     KSWORD_ARK_FILE_MONITOR_OPERATION_FSCTL)

#define KSWORD_ARK_FILE_MONITOR_FIELD_PATH_PRESENT        0x00000001UL
#define KSWORD_ARK_FILE_MONITOR_FIELD_PATH_TRUNCATED      0x00000002UL
#define KSWORD_ARK_FILE_MONITOR_FIELD_RESULT_PRESENT      0x00000004UL
#define KSWORD_ARK_FILE_MONITOR_FIELD_ACCESS_PRESENT      0x00000008UL
#define KSWORD_ARK_FILE_MONITOR_FIELD_POST_OPERATION      0x00000010UL
#define KSWORD_ARK_FILE_MONITOR_FIELD_SYSTEM_PROCESS      0x00000020UL
#define KSWORD_ARK_FILE_MONITOR_FIELD_FSCTL_PRESENT       0x00000040UL

#define KSWORD_ARK_FILE_MONITOR_RUNTIME_REGISTERED 0x00000001UL
#define KSWORD_ARK_FILE_MONITOR_RUNTIME_STARTED    0x00000002UL
#define KSWORD_ARK_FILE_MONITOR_RUNTIME_DROPPED    0x00000004UL

#define KSWORD_ARK_FILE_MONITOR_PATH_CHARS 520U
#define KSWORD_ARK_FILE_MONITOR_RING_CAPACITY 1024U

typedef struct _KSWORD_ARK_FILE_MONITOR_CONTROL_REQUEST
{
    unsigned long action;
    unsigned long operationMask;
    unsigned long processId;
    unsigned long flags;
} KSWORD_ARK_FILE_MONITOR_CONTROL_REQUEST;

typedef struct _KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long runtimeFlags;
    unsigned long operationMask;
    unsigned long processIdFilter;
    unsigned long ringCapacity;
    unsigned long queuedCount;
    unsigned long droppedCount;
    unsigned long long sequence;
    long registerStatus;
    long startStatus;
    long lastErrorStatus;
} KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE;

typedef struct _KSWORD_ARK_FILE_MONITOR_EVENT
{
    unsigned long version;
    unsigned long size;
    unsigned long operationType;
    unsigned long majorFunction;
    unsigned long minorFunction;
    unsigned long processId;
    unsigned long threadId;
    unsigned long fieldFlags;
    unsigned long desiredAccess;
    unsigned long shareAccess;
    unsigned long createOptions;
    unsigned long fileInformationClass;
    long resultStatus;
    unsigned long pathLengthChars;
    unsigned long reserved0;
    unsigned long long sequence;
    long long timeUtc100ns;
    unsigned long long fileObjectAddress;
    wchar_t path[KSWORD_ARK_FILE_MONITOR_PATH_CHARS];
    unsigned long fsControlCode;
    unsigned long fsInputBufferLength;
    unsigned long fsOutputBufferLength;
    unsigned long fsctlReserved0;
} KSWORD_ARK_FILE_MONITOR_EVENT;

typedef struct _KSWORD_ARK_FILE_MONITOR_DRAIN_REQUEST
{
    unsigned long maxEvents;
    unsigned long flags;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_FILE_MONITOR_DRAIN_REQUEST;

typedef struct _KSWORD_ARK_FILE_MONITOR_DRAIN_RESPONSE
{
    unsigned long version;
    unsigned long totalQueuedBeforeDrain;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long droppedCount;
    unsigned long runtimeFlags;
    unsigned long ringCapacity;
    unsigned long reserved;
    KSWORD_ARK_FILE_MONITOR_EVENT events[1];
} KSWORD_ARK_FILE_MONITOR_DRAIN_RESPONSE;

static inline const wchar_t* KswordARKFileMonitorFsctlCodeToText(const unsigned long fsControlCode)
{
    switch (fsControlCode)
    {
    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
        return L"FSCTL_REQUEST_OPLOCK_LEVEL_1";
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
        return L"FSCTL_REQUEST_OPLOCK_LEVEL_2";
    case FSCTL_REQUEST_BATCH_OPLOCK:
        return L"FSCTL_REQUEST_BATCH_OPLOCK";
    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
        return L"FSCTL_OPLOCK_BREAK_ACKNOWLEDGE";
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
        return L"FSCTL_OPBATCH_ACK_CLOSE_PENDING";
    case FSCTL_OPLOCK_BREAK_NOTIFY:
        return L"FSCTL_OPLOCK_BREAK_NOTIFY";
    case FSCTL_REQUEST_FILTER_OPLOCK:
        return L"FSCTL_REQUEST_FILTER_OPLOCK";
    case FSCTL_REQUEST_OPLOCK:
        return L"FSCTL_REQUEST_OPLOCK";
    default:
        break;
    }
    return NULL;
}

static inline BOOLEAN KswordARKFileMonitorFsctlIsOplockRelated(const unsigned long fsControlCode)
{
    switch (fsControlCode)
    {
    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_REQUEST_FILTER_OPLOCK:
    case FSCTL_REQUEST_OPLOCK:
        return TRUE;
    default:
        break;
    }
    return FALSE;
}
