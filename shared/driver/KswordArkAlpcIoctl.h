#pragma once

#include "KswordArkDynDataIoctl.h"

// ============================================================
// KswordArkAlpcIoctl.h
// 作用：
// - 定义 R3/R0 ALPC Port 查询协议；
// - 输入只允许 PID + HandleValue，禁止把内核对象地址作为操作凭据；
// - 所有 ALPC_PORT 私有字段都通过 DynData capability 门控。
// ============================================================

#define KSWORD_ARK_ALPC_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_ALPC_PORT 0x80EUL

#define IOCTL_KSWORD_ARK_QUERY_ALPC_PORT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_ALPC_PORT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_BASIC          0x00000001UL
#define KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_COMMUNICATION  0x00000002UL
#define KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_NAMES          0x00000004UL
#define KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_BASIC | \
     KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_COMMUNICATION | \
     KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_NAMES)

#define KSWORD_ARK_ALPC_RESPONSE_FIELD_OBJECT_PRESENT       0x00000001UL
#define KSWORD_ARK_ALPC_RESPONSE_FIELD_TYPE_NAME_PRESENT    0x00000002UL
#define KSWORD_ARK_ALPC_RESPONSE_FIELD_QUERY_PORT_PRESENT   0x00000004UL
#define KSWORD_ARK_ALPC_RESPONSE_FIELD_CONNECTION_PRESENT   0x00000008UL
#define KSWORD_ARK_ALPC_RESPONSE_FIELD_SERVER_PRESENT       0x00000010UL
#define KSWORD_ARK_ALPC_RESPONSE_FIELD_CLIENT_PRESENT       0x00000020UL
#define KSWORD_ARK_ALPC_RESPONSE_FIELD_COMMUNICATION_PRESENT \
    (KSWORD_ARK_ALPC_RESPONSE_FIELD_CONNECTION_PRESENT | \
     KSWORD_ARK_ALPC_RESPONSE_FIELD_SERVER_PRESENT | \
     KSWORD_ARK_ALPC_RESPONSE_FIELD_CLIENT_PRESENT)

#define KSWORD_ARK_ALPC_PORT_FIELD_OBJECT_PRESENT       0x00000001UL
#define KSWORD_ARK_ALPC_PORT_FIELD_OWNER_PID_PRESENT    0x00000002UL
#define KSWORD_ARK_ALPC_PORT_FIELD_FLAGS_PRESENT        0x00000004UL
#define KSWORD_ARK_ALPC_PORT_FIELD_CONTEXT_PRESENT      0x00000008UL
#define KSWORD_ARK_ALPC_PORT_FIELD_SEQUENCE_PRESENT     0x00000010UL
#define KSWORD_ARK_ALPC_PORT_FIELD_STATE_PRESENT        0x00000020UL
#define KSWORD_ARK_ALPC_PORT_FIELD_NAME_PRESENT         0x00000040UL

#define KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE              0UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_OK                       1UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL                  2UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_DYNDATA_MISSING          3UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_PROCESS_LOOKUP_FAILED    4UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_HANDLE_REFERENCE_FAILED  5UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_TYPE_MISMATCH            6UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_BASIC_QUERY_FAILED       7UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_COMMUNICATION_FAILED     8UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_NAME_QUERY_FAILED        9UL
#define KSWORD_ARK_ALPC_QUERY_STATUS_NAME_TRUNCATED           10UL

#define KSWORD_ARK_ALPC_PORT_RELATION_QUERY       0UL
#define KSWORD_ARK_ALPC_PORT_RELATION_CONNECTION  1UL
#define KSWORD_ARK_ALPC_PORT_RELATION_SERVER      2UL
#define KSWORD_ARK_ALPC_PORT_RELATION_CLIENT      3UL

#define KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE 0xFFFFFFFFUL
#define KSWORD_ARK_ALPC_TYPE_NAME_CHARS 96U
#define KSWORD_ARK_ALPC_PORT_NAME_CHARS 512U

typedef struct _KSWORD_ARK_QUERY_ALPC_PORT_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long long handleValue;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_QUERY_ALPC_PORT_REQUEST;

typedef struct _KSWORD_ARK_ALPC_PORT_INFO
{
    unsigned long relation;
    unsigned long fieldFlags;
    unsigned long ownerProcessId;
    unsigned long flags;
    unsigned long state;
    unsigned long sequenceNo;
    long basicStatus;
    long nameStatus;
    unsigned long long objectAddress;
    unsigned long long portContext;
    wchar_t portName[KSWORD_ARK_ALPC_PORT_NAME_CHARS];
} KSWORD_ARK_ALPC_PORT_INFO;

typedef struct _KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long long handleValue;
    unsigned long queryStatus;
    long objectReferenceStatus;
    long typeStatus;
    long basicStatus;
    long communicationStatus;
    long nameStatus;
    unsigned long reserved0;
    unsigned long long dynDataCapabilityMask;
    unsigned long alpcCommunicationInfoOffset;
    unsigned long alpcOwnerProcessOffset;
    unsigned long alpcConnectionPortOffset;
    unsigned long alpcServerCommunicationPortOffset;
    unsigned long alpcClientCommunicationPortOffset;
    unsigned long alpcHandleTableOffset;
    unsigned long alpcHandleTableLockOffset;
    unsigned long alpcAttributesOffset;
    unsigned long alpcAttributesFlagsOffset;
    unsigned long alpcPortContextOffset;
    unsigned long alpcPortObjectLockOffset;
    unsigned long alpcSequenceNoOffset;
    unsigned long alpcStateOffset;
    wchar_t typeName[KSWORD_ARK_ALPC_TYPE_NAME_CHARS];
    KSWORD_ARK_ALPC_PORT_INFO queryPort;
    KSWORD_ARK_ALPC_PORT_INFO connectionPort;
    KSWORD_ARK_ALPC_PORT_INFO serverPort;
    KSWORD_ARK_ALPC_PORT_INFO clientPort;
} KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE;
