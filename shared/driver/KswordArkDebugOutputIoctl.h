#pragma once

#include "KswordArkDynDataIoctl.h"

// ============================================================
// KswordArkDebugOutputIoctl.h
// 作用：
// - 定义内核 DbgPrint/DbgPrintEx/KdPrintEx 输出捕获协议；
// - R0 通过 DbgSetDebugPrintCallback 写入固定环形缓冲区；
// - R3 只通过 ArkDriverClient 控制捕获并按序读取快照。
// ============================================================

#ifndef FILE_READ_ACCESS
#define FILE_READ_ACCESS 0x0001
#endif

#ifndef FILE_WRITE_ACCESS
#define FILE_WRITE_ACCESS 0x0002
#endif

#define KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_DEBUG_OUTPUT_CONTROL 0x8F8UL
#define KSWORD_ARK_IOCTL_FUNCTION_DEBUG_OUTPUT_DRAIN   0x8F9UL

// 调试输出可能包含内核地址或设备状态，因此两个 IOCTL 都要求读写句柄。
#define IOCTL_KSWORD_ARK_DEBUG_OUTPUT_CONTROL \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DEBUG_OUTPUT_CONTROL, \
        METHOD_BUFFERED, \
        FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_DEBUG_OUTPUT_DRAIN \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DEBUG_OUTPUT_DRAIN, \
        METHOD_BUFFERED, \
        FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// 控制动作：START 会清空旧快照并注册回调；STOP 注销；QUERY 只读状态。
#define KSWORD_ARK_DEBUG_OUTPUT_ACTION_START 1UL
#define KSWORD_ARK_DEBUG_OUTPUT_ACTION_STOP  2UL
#define KSWORD_ARK_DEBUG_OUTPUT_ACTION_QUERY 3UL

// 运行时状态位：用于 R3 区分已注册、正在捕获和发生过丢弃。
#define KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_REGISTERED 0x00000001UL
#define KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_CAPTURING  0x00000002UL
#define KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_DROPPED    0x00000004UL

// Drain 响应位：OVERFLOW 表示调用方游标已经落后于环形缓冲区。
#define KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_OVERFLOW       0x00000001UL
#define KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_MORE_AVAILABLE 0x00000002UL
#define KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_SNAPSHOT_RACE  0x00000004UL

// 单条记录位：TEXT_TRUNCATED 表示原始内核调试文本超过固定上限。
#define KSWORD_ARK_DEBUG_OUTPUT_RECORD_FLAG_TEXT_TRUNCATED 0x00000001UL

// DbgPrint 单次最多传递 512 字节；保留末尾 NUL，因此正文上限为 511 字节。
#define KSWORD_ARK_DEBUG_OUTPUT_TEXT_BYTES 512U
#define KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY 256U
#define KSWORD_ARK_DEBUG_OUTPUT_DEFAULT_DRAIN_RECORDS 32U
#define KSWORD_ARK_DEBUG_OUTPUT_MAX_DRAIN_RECORDS 64U

typedef struct _KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST
{
    unsigned long version;
    unsigned long size;
    unsigned long action;
    unsigned long flags;
} KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST;

typedef struct _KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long runtimeFlags;
    unsigned long ringCapacity;
    unsigned long queuedCount;
    unsigned long reserved0;
    unsigned long long latestSequence;
    unsigned long long droppedCount;
    long registrationStatus;
    long lastStatus;
} KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE;

typedef struct _KSWORD_ARK_DEBUG_OUTPUT_RECORD
{
    unsigned long long sequence;
    unsigned long long interruptTime100ns;
    unsigned long componentId;
    unsigned long level;
    unsigned long textLengthBytes;
    unsigned long flags;
    char text[KSWORD_ARK_DEBUG_OUTPUT_TEXT_BYTES];
} KSWORD_ARK_DEBUG_OUTPUT_RECORD;

typedef struct _KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST
{
    unsigned long version;
    unsigned long size;
    unsigned long maxRecords;
    unsigned long flags;
    unsigned long long afterSequence;
} KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST;

typedef struct _KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long runtimeFlags;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long ringCapacity;
    unsigned long responseFlags;
    unsigned long reserved0;
    unsigned long long firstAvailableSequence;
    unsigned long long latestSequence;
    unsigned long long nextSequence;
    unsigned long long droppedCount;
    unsigned long long lostBeforeFirst;
    KSWORD_ARK_DEBUG_OUTPUT_RECORD records[1];
} KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE;
