#pragma once

#include "KswordArkDynDataIoctl.h"

// ============================================================
// KswordArkHandleIoctl.h
// 作用：
// - 定义 R3/R0 进程 HandleTable 直接枚举协议；
// - 只暴露诊断和差异分析所需字段，不把对象地址当作后续操作凭据；
// - 所有私有结构字段均由 DynData capability 门控。
// ============================================================

#define KSWORD_ARK_HANDLE_PROTOCOL_VERSION 2UL

#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_PROCESS_HANDLES 0x80CUL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_HANDLE_OBJECT 0x80DUL

#define IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_PROCESS_HANDLES, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_HANDLE_OBJECT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_OBJECT      0x00000001UL
#define KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_TYPE_INDEX  0x00000002UL
#define KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_OBJECT | KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_TYPE_INDEX)

#define KSWORD_ARK_HANDLE_FIELD_OBJECT_PRESENT          0x00000001UL
#define KSWORD_ARK_HANDLE_FIELD_GRANTED_ACCESS_PRESENT  0x00000002UL
#define KSWORD_ARK_HANDLE_FIELD_ATTRIBUTES_PRESENT      0x00000004UL
#define KSWORD_ARK_HANDLE_FIELD_TYPE_INDEX_PRESENT      0x00000008UL
#define KSWORD_ARK_HANDLE_FIELD_OBJECT_HEADER_PRESENT   0x00000010UL
#define KSWORD_ARK_HANDLE_FIELD_OBJECT_HEADER_BODY_PRESENT 0x00000020UL
#define KSWORD_ARK_HANDLE_FIELD_POINTER_COUNT_PRESENT   0x00000040UL
#define KSWORD_ARK_HANDLE_FIELD_HANDLE_COUNT_PRESENT    0x00000080UL
#define KSWORD_ARK_HANDLE_FIELD_HEADER_TYPE_INDEX_PRESENT 0x00000100UL
#define KSWORD_ARK_HANDLE_FIELD_INFO_MASK_PRESENT       0x00000200UL
#define KSWORD_ARK_HANDLE_FIELD_OBJECT_TYPE_PRESENT     0x00000400UL

#define KSWORD_ARK_HANDLE_DECODE_STATUS_UNAVAILABLE        0UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_OK                 1UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_PARTIAL            2UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_DYNDATA_MISSING    3UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_LOOKUP_FAILED 4UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_EXITING    5UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_HANDLE_TABLE_MISSING 6UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_OBJECT_DECODE_FAILED 7UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_TYPE_DECODE_FAILED 8UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_READ_FAILED        9UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_BUFFER_TOO_SMALL   10UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_HEADER_DYNDATA_MISSING 11UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_HEADER_READ_FAILED 12UL
#define KSWORD_ARK_HANDLE_DECODE_STATUS_ACCESS_DECODE_FAILED 13UL

#define KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE 0xFFFFFFFFUL

// Type source values explain whether R0 decoded the type index from OBJECT_TYPE,
// OBJECT_HEADER, or both; mismatch is reported instead of hidden.
#define KSWORD_ARK_OBJECT_TYPE_SOURCE_NONE               0UL
#define KSWORD_ARK_OBJECT_TYPE_SOURCE_OBJECT_TYPE_INDEX   1UL
#define KSWORD_ARK_OBJECT_TYPE_SOURCE_OBJECT_HEADER       2UL
#define KSWORD_ARK_OBJECT_TYPE_SOURCE_BOTH_MATCH          3UL
#define KSWORD_ARK_OBJECT_TYPE_SOURCE_BOTH_MISMATCH       4UL

// Type-name source values identify whether the name came from DynData OtName.
#define KSWORD_ARK_OBJECT_TYPE_NAME_SOURCE_NONE           0UL
#define KSWORD_ARK_OBJECT_TYPE_NAME_SOURCE_DYNDATA_OTNAME 1UL
#define KSWORD_ARK_OBJECT_TYPE_NAME_SOURCE_QUERY_FAILED   2UL

// Name-info status is per-entry/per-query so name read failures never fail the
// full handle enumeration.
#define KSWORD_ARK_OBJECT_NAME_INFO_STATUS_NOT_REQUESTED  0UL
#define KSWORD_ARK_OBJECT_NAME_INFO_STATUS_UNKNOWN        1UL
#define KSWORD_ARK_OBJECT_NAME_INFO_STATUS_HEADER_MASK_EMPTY 2UL
#define KSWORD_ARK_OBJECT_NAME_INFO_STATUS_HEADER_MASK_NONZERO 3UL
#define KSWORD_ARK_OBJECT_NAME_INFO_STATUS_QUERY_OK       4UL
#define KSWORD_ARK_OBJECT_NAME_INFO_STATUS_QUERY_FAILED   5UL
#define KSWORD_ARK_OBJECT_NAME_INFO_STATUS_QUERY_TRUNCATED 6UL

#define KSWORD_ARK_OBJECT_TYPE_NAME_CHARS 96U
#define KSWORD_ARK_OBJECT_NAME_CHARS 512U

#define KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_TYPE_NAME 0x00000001UL
#define KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_OBJECT_NAME 0x00000002UL
#define KSWORD_ARK_QUERY_OBJECT_FLAG_REQUEST_PROXY_HANDLE 0x00000004UL
#define KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_TYPE_NAME | KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_OBJECT_NAME)

#define KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_PRESENT      0x00000001UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_TYPE_INDEX_PRESENT  0x00000002UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_TYPE_NAME_PRESENT   0x00000004UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_NAME_PRESENT 0x00000008UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_PROXY_HANDLE_PRESENT 0x00000010UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_HEADER_PRESENT 0x00000020UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_HEADER_BODY_PRESENT 0x00000040UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_POINTER_COUNT_PRESENT 0x00000080UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_HANDLE_COUNT_PRESENT 0x00000100UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_HEADER_TYPE_INDEX_PRESENT 0x00000200UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_INFO_MASK_PRESENT   0x00000400UL
#define KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_TYPE_PRESENT 0x00000800UL

#define KSWORD_ARK_OBJECT_QUERY_STATUS_UNAVAILABLE       0UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_OK                1UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_PARTIAL           2UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_DYNDATA_MISSING   3UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_PROCESS_LOOKUP_FAILED 4UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_HANDLE_REFERENCE_FAILED 5UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_TYPE_QUERY_FAILED 6UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_QUERY_FAILED 7UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_TRUNCATED    8UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_HEADER_DYNDATA_MISSING 9UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_HEADER_QUERY_FAILED 10UL
#define KSWORD_ARK_OBJECT_QUERY_STATUS_ACCESS_DECODE_FAILED 11UL

#define KSWORD_ARK_OBJECT_PROXY_STATUS_NOT_REQUESTED     0UL
#define KSWORD_ARK_OBJECT_PROXY_STATUS_OPENED            1UL
#define KSWORD_ARK_OBJECT_PROXY_STATUS_DENIED_BY_POLICY  2UL
#define KSWORD_ARK_OBJECT_PROXY_STATUS_OPEN_FAILED       3UL
#define KSWORD_ARK_OBJECT_PROXY_STATUS_REQUESTOR_FAILED  4UL

#define KSWORD_ARK_OBJECT_PROXY_POLICY_DOWNGRADED        0x00000001UL
#define KSWORD_ARK_OBJECT_PROXY_POLICY_TYPE_WHITELISTED  0x00000002UL
#define KSWORD_ARK_OBJECT_PROXY_POLICY_RETURNED_USER_HANDLE 0x00000004UL

typedef struct _KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST;

typedef struct _KSWORD_ARK_HANDLE_ENTRY
{
    unsigned long processId;
    unsigned long handleValue;
    unsigned long fieldFlags;
    unsigned long decodeStatus;
    unsigned long grantedAccess;
    unsigned long attributes;
    unsigned long objectTypeIndex;
    unsigned long reserved;
    unsigned long long objectAddress;
    unsigned long long dynDataCapabilityMask;
    unsigned long epObjectTableOffset;
    unsigned long htHandleContentionEventOffset;
    unsigned long obDecodeShift;
    unsigned long obAttributesShift;
    unsigned long otNameOffset;
    unsigned long otIndexOffset;
    unsigned long objectHeaderDecodeStatus;
    long objectHeaderReadStatus;
    unsigned long grantedAccessDecodeStatus;
    long grantedAccessReadStatus;
    unsigned long objectTypeIndexSource;
    unsigned long objectTypeNameSource;
    unsigned long nameInfoStatus;
    unsigned long objectHeaderTypeIndex;
    unsigned long objectHeaderInfoMask;
    unsigned long objectHeaderFlags;
    unsigned long objectHeaderTraceFlags;
    long long pointerCount;
    unsigned long long handleCount;
    unsigned long long objectHeaderAddress;
    unsigned long long objectTypeAddress;
} KSWORD_ARK_HANDLE_ENTRY;

typedef struct _KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long processId;
    unsigned long overallStatus;
    long lastStatus;
    unsigned long reserved;
    KSWORD_ARK_HANDLE_ENTRY entries[1];
} KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE;

typedef struct _KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long long handleValue;
    unsigned long requestedAccess;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST;

typedef struct _KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long long handleValue;
    unsigned long long objectAddress;
    unsigned long objectTypeIndex;
    unsigned long queryStatus;
    long objectReferenceStatus;
    long typeStatus;
    long nameStatus;
    unsigned long proxyStatus;
    long proxyNtStatus;
    unsigned long proxyPolicyFlags;
    unsigned long requestedAccess;
    unsigned long actualGrantedAccess;
    unsigned long long proxyHandle;
    unsigned long long dynDataCapabilityMask;
    unsigned long otNameOffset;
    unsigned long otIndexOffset;
    unsigned long objectHeaderDecodeStatus;
    long objectHeaderReadStatus;
    unsigned long grantedAccessDecodeStatus;
    long grantedAccessReadStatus;
    unsigned long objectTypeIndexSource;
    unsigned long objectTypeNameSource;
    unsigned long nameInfoStatus;
    unsigned long objectHeaderTypeIndex;
    unsigned long objectHeaderInfoMask;
    unsigned long objectHeaderFlags;
    unsigned long objectHeaderTraceFlags;
    long long pointerCount;
    unsigned long long handleCount;
    unsigned long long objectHeaderAddress;
    unsigned long long objectTypeAddress;
    wchar_t typeName[KSWORD_ARK_OBJECT_TYPE_NAME_CHARS];
    wchar_t objectName[KSWORD_ARK_OBJECT_NAME_CHARS];
} KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE;
