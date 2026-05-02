#pragma once

#include "KswordArkSafetyIoctl.h"

// ============================================================
// KswordArkMemoryIoctl.h
// 作用：
// - 定义 Phase-11 进程虚拟内存查询、读取和差异写入协议；
// - 定义 R0 物理内存读取、受控写入和 x64 页表解析协议；
// - R3/R0 共享本文件中的结构，不在 UI 或 Client 侧重复定义；
// - 写入协议只承载已编辑的差异块，不提供分配、释放或保护修改；
// - 页表协议只做只读解析，不默认提供 PTE/PDE 修改能力。
// ============================================================

#define KSWORD_ARK_MEMORY_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_VIRTUAL_MEMORY 0x813UL
#define KSWORD_ARK_IOCTL_FUNCTION_READ_VIRTUAL_MEMORY 0x814UL
#define KSWORD_ARK_IOCTL_FUNCTION_WRITE_VIRTUAL_MEMORY 0x81DUL
#define KSWORD_ARK_IOCTL_FUNCTION_READ_PHYSICAL_MEMORY 0x82BUL
#define KSWORD_ARK_IOCTL_FUNCTION_WRITE_PHYSICAL_MEMORY 0x82CUL
#define KSWORD_ARK_IOCTL_FUNCTION_TRANSLATE_VIRTUAL_ADDRESS 0x82DUL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_PAGE_TABLE_ENTRY 0x82EUL

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

#define IOCTL_KSWORD_ARK_READ_PHYSICAL_MEMORY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_READ_PHYSICAL_MEMORY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_WRITE_PHYSICAL_MEMORY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_WRITE_PHYSICAL_MEMORY, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_TRANSLATE_VIRTUAL_ADDRESS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_PAGE_TABLE_ENTRY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

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
#define KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT  0x00000400UL
#define KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_PRESENT        0x00000800UL
#define KSWORD_ARK_MEMORY_FIELD_PML4E_PRESENT             0x00001000UL
#define KSWORD_ARK_MEMORY_FIELD_PDPTE_PRESENT             0x00002000UL
#define KSWORD_ARK_MEMORY_FIELD_PDE_PRESENT               0x00004000UL
#define KSWORD_ARK_MEMORY_FIELD_PTE_PRESENT               0x00008000UL
#define KSWORD_ARK_MEMORY_FIELD_LARGE_PAGE_1GB            0x00010000UL
#define KSWORD_ARK_MEMORY_FIELD_LARGE_PAGE_2MB            0x00020000UL
#define KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_WALK_COMPLETE  0x00040000UL

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

#define KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_OK          1UL
#define KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_PARTIAL     2UL
#define KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_COPY_FAILED 3UL
#define KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_RANGE_REJECTED 4UL
#define KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_BUFFER_TOO_SMALL 5UL
#define KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_IRQL_REJECTED 6UL

#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_OK          1UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_MAP_FAILED  2UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_COPY_FAILED 3UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_RANGE_REJECTED 4UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_BUFFER_TOO_SMALL 5UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_ACCESS_DENIED 6UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_FORCE_REQUIRED 7UL
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_IRQL_REJECTED 8UL

#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_UNAVAILABLE      0UL
#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_OK               1UL
#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_PRESENT      2UL
#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_PROCESS_LOOKUP_FAILED 3UL
#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_READ_FAILED      4UL
#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_INVALID_ADDRESS  5UL
#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_SUPPORTED    6UL
#define KSWORD_ARK_MEMORY_TRANSLATE_STATUS_IRQL_REJECTED    7UL

#define KSWORD_ARK_MEMORY_SOURCE_R0_ZW_QUERY_VIRTUAL_MEMORY 1UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_MM_COPY_VIRTUAL_MEMORY  2UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_MM_WRITE_VIRTUAL_MEMORY 3UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_MM_COPY_PHYSICAL_MEMORY 4UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_MM_MAP_PHYSICAL_MEMORY  5UL
#define KSWORD_ARK_MEMORY_SOURCE_R0_PAGE_TABLE_WALK         6UL

#define KSWORD_ARK_MEMORY_MAPPED_FILE_NAME_CHARS 512U
#define KSWORD_ARK_MEMORY_READ_MAX_BYTES (1024UL * 1024UL)
#define KSWORD_ARK_MEMORY_WRITE_MAX_BYTES (256UL * 1024UL)
#define KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES (64UL * 1024UL)
#define KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES (4UL * 1024UL)

#define KSWORD_ARK_PHYSICAL_WRITE_FLAG_UI_CONFIRMED 0x00000001UL
#define KSWORD_ARK_PHYSICAL_WRITE_FLAG_FORCE        0x00000002UL

#define KSWORD_ARK_PAGE_TABLE_FLAG_PRESENT       0x00000001UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_WRITABLE      0x00000002UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_USER          0x00000004UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_WRITE_THROUGH 0x00000008UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_CACHE_DISABLE 0x00000010UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_ACCESSED      0x00000020UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_DIRTY         0x00000040UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_LARGE_PAGE    0x00000080UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_GLOBAL        0x00000100UL
#define KSWORD_ARK_PAGE_TABLE_FLAG_NX            0x00000200UL

#define KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_NONE 0UL
#define KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_2MB  1UL
#define KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_1GB  2UL

#define KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_4KB (4UL * 1024UL)
#define KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_2MB (2UL * 1024UL * 1024UL)
#define KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_1GB (1024UL * 1024UL * 1024UL)

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

typedef struct _KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST
{
    unsigned long flags;
    unsigned long reserved;
    unsigned long long physicalAddress;
    unsigned long bytesToRead;
    unsigned long reserved2;
} KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST;

typedef struct _KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE
{
    unsigned long version;
    unsigned long headerSize;
    unsigned long fieldFlags;
    unsigned long readStatus;
    long copyStatus;
    unsigned long source;
    unsigned long requestedBytes;
    unsigned long bytesRead;
    unsigned long maxBytesPerRequest;
    unsigned long reserved;
    unsigned long long requestedPhysicalAddress;
    unsigned char data[1];
} KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE;

typedef struct _KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST
{
    unsigned long flags;
    unsigned long reserved;
    unsigned long long physicalAddress;
    unsigned long bytesToWrite;
    unsigned long reserved2;
    unsigned char data[1];
} KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST;

typedef struct _KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long fieldFlags;
    unsigned long writeStatus;
    long mapStatus;
    long copyStatus;
    unsigned long source;
    unsigned long requestedBytes;
    unsigned long bytesWritten;
    unsigned long maxBytesPerRequest;
    unsigned long reserved;
    unsigned long long requestedPhysicalAddress;
} KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE;

typedef struct _KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long long virtualAddress;
    unsigned long reserved;
} KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST;

typedef struct _KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long long virtualAddress;
    unsigned long reserved;
} KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST;

typedef struct _KSWORD_ARK_PAGE_TABLE_ENTRY_INFO
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long queryStatus;
    long lookupStatus;
    long walkStatus;
    unsigned long source;
    unsigned long pml4Index;
    unsigned long pdptIndex;
    unsigned long pdIndex;
    unsigned long ptIndex;
    unsigned long pml4eFlags;
    unsigned long pdpteFlags;
    unsigned long pdeFlags;
    unsigned long pteFlags;
    unsigned long effectiveFlags;
    unsigned long largePageType;
    unsigned long pageSize;
    unsigned long resolved;
    unsigned long long virtualAddress;
    unsigned long long physicalAddress;
    unsigned long long cr3PhysicalAddress;
    unsigned long long pml4ePhysicalAddress;
    unsigned long long pdptePhysicalAddress;
    unsigned long long pdePhysicalAddress;
    unsigned long long ptePhysicalAddress;
    unsigned long long pml4eValue;
    unsigned long long pdpteValue;
    unsigned long long pdeValue;
    unsigned long long pteValue;
} KSWORD_ARK_PAGE_TABLE_ENTRY_INFO;

typedef struct _KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE
{
    KSWORD_ARK_PAGE_TABLE_ENTRY_INFO info;
} KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE;

typedef struct _KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE
{
    KSWORD_ARK_PAGE_TABLE_ENTRY_INFO info;
} KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE;
