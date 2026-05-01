#pragma once

#include "KswordArkDynDataIoctl.h"

// ============================================================
// KswordArkSectionIoctl.h
// 作用：
// - 定义进程 SectionObject / ControlArea 查询协议；
// - 输入只接受 PID，禁止 R3 把任意内核 Section/ControlArea 地址作为凭据；
// - 映射关系只返回诊断信息，私有字段全部由 DynData capability 门控。
// ============================================================

#define KSWORD_ARK_SECTION_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_SECTION 0x80FUL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_FILE_SECTION_MAPPINGS 0x810UL

#define IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_SECTION, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_FILE_SECTION_MAPPINGS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_CONTROL_AREA 0x00000001UL
#define KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_MAPPINGS     0x00000002UL
#define KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_CONTROL_AREA | \
     KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_MAPPINGS)

#define KSWORD_ARK_SECTION_FIELD_SECTION_OBJECT_PRESENT 0x00000001UL
#define KSWORD_ARK_SECTION_FIELD_CONTROL_AREA_PRESENT   0x00000002UL
#define KSWORD_ARK_SECTION_FIELD_MAPPING_LIST_PRESENT   0x00000004UL
#define KSWORD_ARK_SECTION_FIELD_MAPPING_TRUNCATED      0x00000008UL
#define KSWORD_ARK_SECTION_FIELD_REMOTE_MAPPING_UNSUPPORTED 0x00000010UL

#define KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_DATA_IMAGE 0x00000001UL
#define KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_IMAGE      0x00000002UL
#define KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_DATA_IMAGE | \
     KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_IMAGE)

#define KSWORD_ARK_FILE_SECTION_FIELD_FILE_OBJECT_PRESENT        0x00000001UL
#define KSWORD_ARK_FILE_SECTION_FIELD_SECTION_POINTERS_PRESENT   0x00000002UL
#define KSWORD_ARK_FILE_SECTION_FIELD_DATA_CONTROL_AREA_PRESENT  0x00000004UL
#define KSWORD_ARK_FILE_SECTION_FIELD_IMAGE_CONTROL_AREA_PRESENT 0x00000008UL
#define KSWORD_ARK_FILE_SECTION_FIELD_MAPPING_LIST_PRESENT       0x00000010UL
#define KSWORD_ARK_FILE_SECTION_FIELD_MAPPING_TRUNCATED          0x00000020UL

#define KSWORD_ARK_SECTION_MAP_TYPE_UNKNOWN      0UL
#define KSWORD_ARK_SECTION_MAP_TYPE_PROCESS      1UL
#define KSWORD_ARK_SECTION_MAP_TYPE_SESSION      2UL
#define KSWORD_ARK_SECTION_MAP_TYPE_SYSTEM_CACHE 3UL

#define KSWORD_ARK_SECTION_QUERY_STATUS_UNAVAILABLE              0UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_OK                       1UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_PARTIAL                  2UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_DYNDATA_MISSING          3UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_PROCESS_LOOKUP_FAILED    4UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_SECTION_OBJECT_MISSING   5UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_CONTROL_AREA_MISSING     6UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_REMOTE_UNSUPPORTED       7UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_MAPPING_QUERY_FAILED     8UL
#define KSWORD_ARK_SECTION_QUERY_STATUS_BUFFER_TOO_SMALL         9UL

#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_UNAVAILABLE             0UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_OK                      1UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_PARTIAL                 2UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_DYNDATA_MISSING         3UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OPEN_FAILED        4UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OBJECT_FAILED      5UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_SECTION_POINTERS_MISSING 6UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_CONTROL_AREA_MISSING    7UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_MAPPING_QUERY_FAILED    8UL
#define KSWORD_ARK_FILE_SECTION_QUERY_STATUS_BUFFER_TOO_SMALL        9UL

#define KSWORD_ARK_SECTION_OFFSET_UNAVAILABLE 0xFFFFFFFFUL
#define KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT 512UL
#define KSWORD_ARK_SECTION_MAPPING_LIMIT_MAX 4096UL
#define KSWORD_ARK_FILE_SECTION_PATH_MAX_CHARS 1024U

#define KSWORD_ARK_FILE_SECTION_KIND_UNKNOWN 0UL
#define KSWORD_ARK_FILE_SECTION_KIND_DATA    1UL
#define KSWORD_ARK_FILE_SECTION_KIND_IMAGE   2UL

typedef struct _KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long maxMappings;
    unsigned long reserved0;
} KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST;

typedef struct _KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST
{
    unsigned long flags;
    unsigned long maxMappings;
    unsigned short pathLengthChars;
    unsigned short reserved0;
    wchar_t path[KSWORD_ARK_FILE_SECTION_PATH_MAX_CHARS];
} KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST;

typedef struct _KSWORD_ARK_SECTION_MAPPING_ENTRY
{
    unsigned long viewMapType;
    unsigned long processId;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long long startVa;
    unsigned long long endVa;
} KSWORD_ARK_SECTION_MAPPING_ENTRY;

typedef struct _KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY
{
    unsigned long sectionKind;
    unsigned long viewMapType;
    unsigned long processId;
    unsigned long reserved0;
    unsigned long long controlAreaAddress;
    unsigned long long startVa;
    unsigned long long endVa;
} KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY;

typedef struct _KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long queryStatus;
    long lastStatus;
    unsigned long long sectionObjectAddress;
    unsigned long long controlAreaAddress;
    unsigned long long dynDataCapabilityMask;
    unsigned long epSectionObjectOffset;
    unsigned long mmSectionControlAreaOffset;
    unsigned long mmControlAreaListHeadOffset;
    unsigned long mmControlAreaLockOffset;
    unsigned long reserved0;
    KSWORD_ARK_SECTION_MAPPING_ENTRY mappings[1];
} KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE;

typedef struct _KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long fieldFlags;
    unsigned long queryStatus;
    long lastStatus;
    unsigned long reserved0;
    unsigned long long fileObjectAddress;
    unsigned long long sectionObjectPointersAddress;
    unsigned long long dataControlAreaAddress;
    unsigned long long imageControlAreaAddress;
    unsigned long long dynDataCapabilityMask;
    unsigned long mmControlAreaListHeadOffset;
    unsigned long mmControlAreaLockOffset;
    unsigned long reserved1;
    unsigned long reserved2;
    KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY mappings[1];
} KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE;
