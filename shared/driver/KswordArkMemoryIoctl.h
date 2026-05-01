#pragma once

#include "KswordArkDynDataIoctl.h"

// ============================================================
// KswordArkMemoryIoctl.h
// 作用：
// - 定义 Phase-11 进程虚拟内存只读查询协议；
// - R3/R0 共享本文件中的结构，不在 UI 或 Client 侧重复定义；
// - 第一版只允许查询和读取，不提供写入、分配、释放或保护修改。
// ============================================================

#define KSWORD_ARK_MEMORY_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_VIRTUAL_MEMORY 0x813UL
#define KSWORD_ARK_IOCTL_FUNCTION_READ_VIRTUAL_MEMORY 0x814UL

#define IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_VIRTUAL_MEMORY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_READ_VIRTUAL_MEMORY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_MAPPED_FILE_NAME 0x00000001UL
#define KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_MAPPED_FILE_NAME)

#define KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT             0x00000001UL
#define KSWORD_ARK_MEMORY_FIELD_MAPPED_FILE_NAME_PRESENT  0x00000002UL
#define KSWORD_ARK_MEMORY_FIELD_MAPPED_FILE_NAME_TRUNCATED 0x00000004UL
#define KSWORD_ARK_MEMORY_FIELD_PARTIAL_COPY              0x00000008UL
#define KSWORD_ARK_MEMORY_FIELD_READ_DATA_PRESENT         0x00000010UL
#define KSWORD_ARK_MEMORY_FIELD_ADDRESS_USER_RANGE        0x00000020UL

#define KSWORD_ARK_MEMORY_QUERY_STATUS_UNAVAILABLE        0UL
#define KSWORD_ARK_MEMORY_QUERY_STATUS_OK                 1UL
#define KSWORD_ARK_MEMORY_QUERY_STATUS_PARTIAL            2UL
#define KSWORD_ARK_MEMORY_QUERY_STATUS_PROCESS_OPEN_FAILED 3UL
#define KSWORD_ARK_MEMORY_QUERY_STATUS_QUERY_FAILED       4UL
#define KSWORD_ARK_MEMORY_QUERY_STATUS_NAME_FAILED        5UL
#define KSWORD_ARK_MEMORY_QUERY_STATUS_BUFFER_TOO_SMALL   6UL

#define KSWORD_ARK_MEMORY_READ_STATUS_UNAVAILABLE         0UL
#define KSWORD_ARK_MEMORY_READ_STATUS_OK                  1UL
#define KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY        2UL
#define KSWORD_ARK_MEMORY_READ_STATUS_PROCESS_LOOKUP_FAILED 3UL
#define KSWORD_ARK_MEMORY_READ_STATUS_COPY_FAILED         4UL
#define KSWORD_ARK_MEMORY_READ_STATUS_RANGE_REJECTED      5UL
#define KSWORD_ARK_MEMORY_READ_STATUS_BUFFER_TOO_SMALL    6UL

#define KSWORD_ARK_MEMORY_SOURCE_R0_ZW_QUERY_VIRTUAL_MEMORY 1UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_MM_COPY_VIRTUAL_MEMORY  2UL

#define KSWORD_ARK_MEMORY_MAPPED_FILE_NAME_CHARS 512U
#define KSWORD_ARK_MEMORY_READ_MAX_BYTES (1024UL * 1024UL)

typedef struct _KSWORD_ARK_QUERY_VIRTUAL_MEMORY_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long long baseAddress;
    unsigned long reserved;
} KSWORD_ARK_QUERY_VIRTUAL_MEMORY_REQUEST;

typedef struct _KSWORD_ARK_QUERY_VIRTUAL_MEMORY_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long queryStatus;
    long openStatus;
    long basicStatus;
    long mappedFileNameStatus;
    unsigned long source;
    unsigned long mappedFileNameLengthChars;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long long requestedBaseAddress;
    unsigned long long baseAddress;
    unsigned long long allocationBase;
    unsigned long long regionSize;
    unsigned long allocationProtect;
    unsigned long state;
    unsigned long protect;
    unsigned long type;
    wchar_t mappedFileName[KSWORD_ARK_MEMORY_MAPPED_FILE_NAME_CHARS];
} KSWORD_ARK_QUERY_VIRTUAL_MEMORY_RESPONSE;

typedef struct _KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long long baseAddress;
    unsigned long bytesToRead;
    unsigned long reserved;
} KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST;

typedef struct _KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE
{
    unsigned long version;
    unsigned long headerSize;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long readStatus;
    long lookupStatus;
    long copyStatus;
    unsigned long source;
    unsigned long long requestedBaseAddress;
    unsigned long requestedBytes;
    unsigned long bytesRead;
    unsigned long maxBytesPerRequest;
    unsigned char data[1];
} KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE;
