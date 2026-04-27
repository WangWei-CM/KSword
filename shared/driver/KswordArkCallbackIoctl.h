#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkCallbackIoctl.h
// Purpose:
// - Shared callback interception protocol for R3 <-> R0.
// - Defines callback rule blob, runtime state, ask/answer events and IOCTLs.
// ============================================================

#define KSWORD_ARK_CALLBACK_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION 1UL

// Shared IOCTL function codes on existing \\.\KswordARKLog control device.
#define KSWORD_ARK_IOCTL_FUNCTION_SET_CALLBACK_RULES 0x880
#define KSWORD_ARK_IOCTL_FUNCTION_GET_CALLBACK_RUNTIME_STATE 0x881
#define KSWORD_ARK_IOCTL_FUNCTION_WAIT_CALLBACK_EVENT 0x882
#define KSWORD_ARK_IOCTL_FUNCTION_ANSWER_CALLBACK_EVENT 0x883
#define KSWORD_ARK_IOCTL_FUNCTION_CANCEL_ALL_PENDING_DECISIONS 0x884
#define KSWORD_ARK_IOCTL_FUNCTION_REMOVE_EXTERNAL_CALLBACK 0x885

#define IOCTL_KSWORD_ARK_SET_CALLBACK_RULES \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SET_CALLBACK_RULES, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_GET_CALLBACK_RUNTIME_STATE, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_WAIT_CALLBACK_EVENT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ANSWER_CALLBACK_EVENT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_CANCEL_ALL_PENDING_DECISIONS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_REMOVE_EXTERNAL_CALLBACK, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

// Callback type.
#define KSWORD_ARK_CALLBACK_TYPE_NONE 0UL
#define KSWORD_ARK_CALLBACK_TYPE_REGISTRY 1UL
#define KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE 2UL
#define KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE 3UL
#define KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD 4UL
#define KSWORD_ARK_CALLBACK_TYPE_OBJECT 5UL
#define KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED 6UL

// External callback remove type.
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS 1UL
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD 2UL
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE 3UL
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT 4UL
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY 5UL
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER 6UL
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT 7UL
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER 8UL

// External callback remove protocol version.
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION 1UL

// External callback remove response text bounds.
#define KSWORD_ARK_EXTERNAL_CALLBACK_MODULE_NAME_MAX_CHARS 260U
#define KSWORD_ARK_EXTERNAL_CALLBACK_SERVICE_NAME_MAX_CHARS 260U

// External callback remove flags.
#define KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_NONE 0UL

// Match mode.
#define KSWORD_ARK_MATCH_MODE_NONE 0UL
#define KSWORD_ARK_MATCH_MODE_EXACT 1UL
#define KSWORD_ARK_MATCH_MODE_PREFIX 2UL
#define KSWORD_ARK_MATCH_MODE_WILDCARD 3UL
#define KSWORD_ARK_MATCH_MODE_REGEX 4UL

// Action type.
#define KSWORD_ARK_RULE_ACTION_NONE 0UL
#define KSWORD_ARK_RULE_ACTION_ALLOW 1UL
#define KSWORD_ARK_RULE_ACTION_DENY 2UL
#define KSWORD_ARK_RULE_ACTION_ASK_USER 3UL
#define KSWORD_ARK_RULE_ACTION_LOG_ONLY 4UL
#define KSWORD_ARK_RULE_ACTION_STRIP_ACCESS 5UL

// Decision type.
#define KSWORD_ARK_DECISION_NONE 0UL
#define KSWORD_ARK_DECISION_ALLOW 1UL
#define KSWORD_ARK_DECISION_DENY 2UL

// Global/group/rule enable flags.
#define KSWORD_ARK_CALLBACK_GLOBAL_FLAG_ENABLED 0x00000001UL
#define KSWORD_ARK_CALLBACK_GROUP_FLAG_ENABLED 0x00000001UL
#define KSWORD_ARK_CALLBACK_RULE_FLAG_ENABLED 0x00000001UL

// Registry operation mask.
#define KSWORD_ARK_REG_OP_CREATE_KEY 0x00000001UL
#define KSWORD_ARK_REG_OP_OPEN_KEY 0x00000002UL
#define KSWORD_ARK_REG_OP_DELETE_KEY 0x00000004UL
#define KSWORD_ARK_REG_OP_SET_VALUE 0x00000008UL
#define KSWORD_ARK_REG_OP_DELETE_VALUE 0x00000010UL
#define KSWORD_ARK_REG_OP_RENAME_KEY 0x00000020UL
#define KSWORD_ARK_REG_OP_SET_INFO 0x00000040UL
#define KSWORD_ARK_REG_OP_QUERY_VALUE 0x00000080UL
#define KSWORD_ARK_REG_OP_ALL 0xFFFFFFFFUL

// Process/thread/image/object operation mask.
#define KSWORD_ARK_PROCESS_OP_CREATE 0x00000001UL
#define KSWORD_ARK_THREAD_OP_CREATE 0x00000001UL
#define KSWORD_ARK_THREAD_OP_EXIT 0x00000002UL
#define KSWORD_ARK_IMAGE_OP_LOAD 0x00000001UL
#define KSWORD_ARK_OBJECT_OP_HANDLE_CREATE 0x00000001UL
#define KSWORD_ARK_OBJECT_OP_HANDLE_DUPLICATE 0x00000002UL
#define KSWORD_ARK_OBJECT_OP_TYPE_PROCESS 0x00000100UL
#define KSWORD_ARK_OBJECT_OP_TYPE_THREAD 0x00000200UL

// Blob format constants.
#define KSWORD_ARK_CALLBACK_RULE_BLOB_MAGIC 0x5242434BUL // "KCBR"
#define KSWORD_ARK_CALLBACK_MAX_GROUP_COUNT 256UL
#define KSWORD_ARK_CALLBACK_MAX_RULE_COUNT 4096UL
#define KSWORD_ARK_CALLBACK_MAX_STRING_BYTES (1024UL * 1024UL)

// Event packet fixed text bounds.
#define KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS 260U
#define KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS 520U
#define KSWORD_ARK_CALLBACK_EVENT_MAX_PATTERN_CHARS 260U
#define KSWORD_ARK_CALLBACK_EVENT_MAX_NAME_CHARS 64U

typedef struct _KSWORD_ARK_GUID128
{
    unsigned char bytes[16];
} KSWORD_ARK_GUID128;

typedef struct _KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER
{
    unsigned long size;
    unsigned long magic;
    unsigned long protocolVersion;
    unsigned long schemaVersion;
    unsigned long globalFlags;
    unsigned long groupCount;
    unsigned long ruleCount;
    unsigned long groupOffsetBytes;
    unsigned long ruleOffsetBytes;
    unsigned long stringOffsetBytes;
    unsigned long stringBytes;
    unsigned long crc32;
    unsigned long reserved;
    unsigned long long ruleVersion;
} KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER;

typedef struct _KSWORD_ARK_CALLBACK_GROUP_BLOB
{
    unsigned long groupId;
    unsigned long flags;
    unsigned long priority;
    unsigned long reserved;
    unsigned long nameOffsetBytes;
    unsigned short nameLengthChars;
    unsigned short reserved2;
    unsigned long commentOffsetBytes;
    unsigned short commentLengthChars;
    unsigned short reserved3;
} KSWORD_ARK_CALLBACK_GROUP_BLOB;

typedef struct _KSWORD_ARK_CALLBACK_RULE_BLOB
{
    unsigned long ruleId;
    unsigned long groupId;
    unsigned long flags;
    unsigned long callbackType;
    unsigned long operationMask;
    unsigned long action;
    unsigned long matchMode;
    unsigned long priority;

    unsigned long initiatorOffsetBytes;
    unsigned short initiatorLengthChars;
    unsigned short reserved1;
    unsigned long targetOffsetBytes;
    unsigned short targetLengthChars;
    unsigned short reserved2;

    unsigned long askTimeoutMs;
    unsigned long askDefaultDecision;

    unsigned long ruleNameOffsetBytes;
    unsigned short ruleNameLengthChars;
    unsigned short reserved3;
    unsigned long commentOffsetBytes;
    unsigned short commentLengthChars;
    unsigned short reserved4;
} KSWORD_ARK_CALLBACK_RULE_BLOB;

typedef struct _KSWORD_ARK_CALLBACK_WAIT_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long waiterTag;
    unsigned long reserved;
} KSWORD_ARK_CALLBACK_WAIT_REQUEST;

typedef struct _KSWORD_ARK_CALLBACK_EVENT_PACKET
{
    unsigned long size;
    unsigned long version;
    KSWORD_ARK_GUID128 eventGuid;

    unsigned long callbackType;
    unsigned long operationType;
    unsigned long action;
    unsigned long matchMode;

    unsigned long defaultDecision;
    unsigned long timeoutMs;
    unsigned long groupId;
    unsigned long ruleId;

    unsigned long groupPriority;
    unsigned long rulePriority;
    unsigned long originatingPid;
    unsigned long originatingTid;
    unsigned long sessionId;
    unsigned long pathUnavailable;
    unsigned long reserved;

    unsigned long long createdAtUtc100ns;
    unsigned long long deadlineUtc100ns;

    wchar_t initiatorPath[KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS];
    wchar_t targetPath[KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS];
    wchar_t ruleInitiatorPattern[KSWORD_ARK_CALLBACK_EVENT_MAX_PATTERN_CHARS];
    wchar_t ruleTargetPattern[KSWORD_ARK_CALLBACK_EVENT_MAX_PATTERN_CHARS];
    wchar_t groupName[KSWORD_ARK_CALLBACK_EVENT_MAX_NAME_CHARS];
    wchar_t ruleName[KSWORD_ARK_CALLBACK_EVENT_MAX_NAME_CHARS];
} KSWORD_ARK_CALLBACK_EVENT_PACKET;

typedef struct _KSWORD_ARK_CALLBACK_ANSWER_REQUEST
{
    unsigned long size;
    unsigned long version;
    KSWORD_ARK_GUID128 eventGuid;
    unsigned long decision;
    unsigned long sourceSessionId;
    unsigned long reserved;
    unsigned long long answeredAtUtc100ns;
} KSWORD_ARK_CALLBACK_ANSWER_REQUEST;

typedef struct _KSWORD_ARK_CALLBACK_RUNTIME_STATE
{
    unsigned long size;
    unsigned long version;

    unsigned long driverOnline;
    unsigned long callbacksRegisteredMask;
    unsigned long globalEnabled;
    unsigned long rulesApplied;

    unsigned long groupCount;
    unsigned long ruleCount;
    unsigned long pendingDecisionCount;
    unsigned long waitingReceiverCount;

    unsigned long long appliedRuleVersion;
    unsigned long long appliedAtUtc100ns;
} KSWORD_ARK_CALLBACK_RUNTIME_STATE;

typedef struct _KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long callbackClass;
    unsigned long flags;
    unsigned long long callbackAddress;
} KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST;

typedef struct _KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long callbackClass;
    unsigned long reserved;
    unsigned long long callbackAddress;
    long ntstatus;
    unsigned long long moduleBase;
    unsigned long moduleSize;
    unsigned long mappingFlags;
    wchar_t modulePath[KSWORD_ARK_EXTERNAL_CALLBACK_MODULE_NAME_MAX_CHARS];
    wchar_t serviceName[KSWORD_ARK_EXTERNAL_CALLBACK_SERVICE_NAME_MAX_CHARS];
} KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE;
