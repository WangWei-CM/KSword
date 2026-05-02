#pragma once

#include "KswordArkSafetyIoctl.h"

// ============================================================
// KswordArkMemoryIoctl.h
// 作用：
// - 定义 Phase-11 进程虚拟内存查询、读取和差异写入协议；
// - R3/R0 共享本文件中的结构，不在 UI 或 Client 侧重复定义；
// - 写入协议只承载已编辑的差异块，不提供分配、释放或保护修改。
// ============================================================

#define KSWORD_ARK_MEMORY_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_VIRTUAL_MEMORY 0x813UL
#define KSWORD_ARK_IOCTL_FUNCTION_READ_VIRTUAL_MEMORY 0x814UL
#define KSWORD_ARK_IOCTL_FUNCTION_WRITE_VIRTUAL_MEMORY 0x81DUL

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

#define IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_WRITE_VIRTUAL_MEMORY, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_MAPPED_FILE_NAME 0x00000001UL
#define KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_MAPPED_FILE_NAME)

#define KSWORD_ARK_MEMORY_READ_FLAG_ZERO_FILL_UNREADABLE 0x00000001UL

#define KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED 0x00000001UL
#define KSWORD_ARK_MEMORY_WRITE_FLAG_FORCE        0x00000002UL

#define KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT             0x00000001UL
#define KSWORD_ARK_MEMORY_FIELD_MAPPED_FILE_NAME_PRESENT  0x00000002UL
#define KSWORD_ARK_MEMORY_FIELD_MAPPED_FILE_NAME_TRUNCATED 0x00000004UL
#define KSWORD_ARK_MEMORY_FIELD_PARTIAL_COPY              0x00000008UL
#define KSWORD_ARK_MEMORY_FIELD_READ_DATA_PRESENT         0x00000010UL
#define KSWORD_ARK_MEMORY_FIELD_ADDRESS_USER_RANGE        0x00000020UL
#define KSWORD_ARK_MEMORY_FIELD_ZERO_FILLED_UNREADABLE    0x00000040UL
#define KSWORD_ARK_MEMORY_FIELD_WRITE_DATA_PRESENT        0x00000080UL
#define KSWORD_ARK_MEMORY_FIELD_FORCE_WRITE_REQUIRED      0x00000100UL
#define KSWORD_ARK_MEMORY_FIELD_FORCE_WRITE_USED          0x00000200UL

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
#define KSWORD_ARK_MEMORY_READ_STATUS_ZERO_FILLED         7UL

#define KSWORD_ARK_MEMORY_WRITE_STATUS_UNAVAILABLE        0UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_OK                 1UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_PARTIAL_COPY       2UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_PROCESS_LOOKUP_FAILED 3UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_COPY_FAILED        4UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_RANGE_REJECTED     5UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_BUFFER_TOO_SMALL   6UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_ACCESS_DENIED      7UL
#define KSWORD_ARK_MEMORY_WRITE_STATUS_FORCE_REQUIRED     8UL

#define KSWORD_ARK_MEMORY_SOURCE_R0_ZW_QUERY_VIRTUAL_MEMORY 1UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_MM_COPY_VIRTUAL_MEMORY  2UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_MM_WRITE_VIRTUAL_MEMORY 3UL

#define KSWORD_ARK_MEMORY_MAPPED_FILE_NAME_CHARS 512U
#define KSWORD_ARK_MEMORY_READ_MAX_BYTES (1024UL * 1024UL)
#define KSWORD_ARK_MEMORY_WRITE_MAX_BYTES (256UL * 1024UL)

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

typedef struct _KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long long baseAddress;
    unsigned long bytesToWrite;
    unsigned long reserved;
    unsigned char data[1];
} KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST;

typedef struct _KSWORD_ARK_WRITE_VIRTUAL_MEMORY_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long writeStatus;
    long lookupStatus;
    long copyStatus;
    unsigned long source;
    unsigned long long requestedBaseAddress;
    unsigned long requestedBytes;
    unsigned long bytesWritten;
    unsigned long maxBytesPerRequest;
} KSWORD_ARK_WRITE_VIRTUAL_MEMORY_RESPONSE;
