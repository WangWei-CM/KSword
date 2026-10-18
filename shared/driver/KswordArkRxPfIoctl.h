#pragma once

#include "KswordArkProcessIoctl.h"

/*
 * RXPF is an explicitly experimental, administrator-only research protocol.
 * The protocol never treats a partial build/ABI match as permission to call an
 * internal memory-manager routine or replace vector 14.
 */
#define KSWORD_ARK_RXPF_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_SUPPORT   0x900UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_REGISTER_PAGE   0x901UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_CHANGE_PAGE     0x902UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_PAGE      0x903UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_WRITE_PAGE      0x904UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_EMULATION   0x905UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_STATS     0x906UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_DRAIN_EVENTS    0x907UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_UNREGISTER_PAGE 0x908UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_RUN_SELF_TEST   0x909UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_HOOK_POINT 0x90AUL

#define IOCTL_KSWORD_ARK_RXPF_QUERY_SUPPORT \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_SUPPORT, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_REGISTER_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_REGISTER_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_CHANGE_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_CHANGE_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_QUERY_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_WRITE_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_WRITE_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_SET_EMULATION \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_EMULATION, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_QUERY_STATS \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_STATS, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_DRAIN_EVENTS \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_DRAIN_EVENTS, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_UNREGISTER_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_UNREGISTER_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_RUN_SELF_TEST \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_RUN_SELF_TEST, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_SET_HOOK_POINT \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_HOOK_POINT, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define KSWORD_ARK_RXPF_CONFIRMATION_TOKEN 0x46505852UL
#define KSWORD_ARK_RXPF_MAX_WRITE_BYTES 64UL
#define KSWORD_ARK_RXPF_MAX_EVENT_ROWS 64UL

#define KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED 0x00000001UL
#define KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP 0x00000002UL

#define KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST 1UL
#define KSWORD_ARK_RXPF_TARGET_SELF_IMAGE_TEST 2UL
#define KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE 3UL

#define KSWORD_ARK_RXPF_HOOK_ACTION_NONE     0UL
#define KSWORD_ARK_RXPF_HOOK_ACTION_REDIRECT 1UL
#define KSWORD_ARK_RXPF_HOOK_ACTION_LOG      2UL

#define KSWORD_ARK_RXPF_PAGE_STATE_EMPTY       0UL
#define KSWORD_ARK_RXPF_PAGE_STATE_RX          1UL
#define KSWORD_ARK_RXPF_PAGE_STATE_RW_NX       2UL
#define KSWORD_ARK_RXPF_PAGE_STATE_TERMINATING 3UL
#define KSWORD_ARK_RXPF_PAGE_STATE_ERROR       4UL

#define KSWORD_ARK_RXPF_SUPPORT_INITIALIZED             0x00000001UL
#define KSWORD_ARK_RXPF_SUPPORT_BUILD_MATCH             0x00000002UL
#define KSWORD_ARK_RXPF_SUPPORT_ABI_VERIFIED            0x00000004UL
#define KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PAGE     0x00000008UL
#define KSWORD_ARK_RXPF_SUPPORT_SELF_IMAGE_TEST_PAGE    0x00000010UL
#define KSWORD_ARK_RXPF_SUPPORT_IDT_SHADOW              0x00000020UL
#define KSWORD_ARK_RXPF_SUPPORT_EMULATOR                 0x00000040UL
#define KSWORD_ARK_RXPF_SUPPORT_EXTERNAL_IMAGE_COMPILED 0x00000080UL
#define KSWORD_ARK_RXPF_SUPPORT_IDT_INSTALLED            0x00000100UL
#define KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PASSED    0x00000200UL

#define KSWORD_ARK_RXPF_BUILD_STATUS_UNINITIALIZED       0UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SUPPORTED           1UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_OS_BUILD_MISMATCH   2UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_IMAGE_MISMATCH      3UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_RSDS_MISMATCH       4UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SIGNATURE_MISMATCH  5UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SECTION_INVALID     6UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SYMBOL_UNAVAILABLE  7UL

#define KSWORD_ARK_RXPF_DECODE_NONE               0UL
#define KSWORD_ARK_RXPF_DECODE_NOP                1UL
#define KSWORD_ARK_RXPF_DECODE_MOV                2UL
#define KSWORD_ARK_RXPF_DECODE_LEA                3UL
#define KSWORD_ARK_RXPF_DECODE_PUSH               4UL
#define KSWORD_ARK_RXPF_DECODE_POP                5UL
#define KSWORD_ARK_RXPF_DECODE_ADD                6UL
#define KSWORD_ARK_RXPF_DECODE_SUB                7UL
#define KSWORD_ARK_RXPF_DECODE_XOR                8UL
#define KSWORD_ARK_RXPF_DECODE_AND                9UL
#define KSWORD_ARK_RXPF_DECODE_OR                 10UL
#define KSWORD_ARK_RXPF_DECODE_CMP                11UL
#define KSWORD_ARK_RXPF_DECODE_TEST               12UL
#define KSWORD_ARK_RXPF_DECODE_JMP                13UL
#define KSWORD_ARK_RXPF_DECODE_JCC                14UL
#define KSWORD_ARK_RXPF_DECODE_CALL               15UL
#define KSWORD_ARK_RXPF_DECODE_RET                16UL
#define KSWORD_ARK_RXPF_DECODE_HOOK_REDIRECT      17UL

#define KSWORD_ARK_RXPF_EMULATION_NONE                    0UL
#define KSWORD_ARK_RXPF_EMULATION_SUCCESS                 1UL
#define KSWORD_ARK_RXPF_EMULATION_NOT_MANAGED             2UL
#define KSWORD_ARK_RXPF_EMULATION_NOT_EXECUTE_FAULT       3UL
#define KSWORD_ARK_RXPF_EMULATION_USER_FAULT              4UL
#define KSWORD_ARK_RXPF_EMULATION_RECURSIVE               5UL
#define KSWORD_ARK_RXPF_EMULATION_UNSUPPORTED_INSTRUCTION 6UL
#define KSWORD_ARK_RXPF_EMULATION_CROSS_PAGE              7UL
#define KSWORD_ARK_RXPF_EMULATION_INVALID_ADDRESS         8UL
#define KSWORD_ARK_RXPF_EMULATION_STACK_RANGE             9UL
#define KSWORD_ARK_RXPF_EMULATION_TOPOLOGY_CHANGED        10UL
#define KSWORD_ARK_RXPF_EMULATION_INTERNAL_ERROR          11UL
#define KSWORD_ARK_RXPF_EMULATION_REDIRECTED             12UL

typedef struct _KSWORD_ARK_RXPF_REQUEST_HEADER
{
    unsigned long version;
    unsigned long size;
    unsigned long flags;
    unsigned long confirmationToken;
} KSWORD_ARK_RXPF_REQUEST_HEADER;

typedef struct _KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long supportFlags;
    unsigned long buildStatus;
    unsigned long ntBuildNumber;
    unsigned long imageTimeDateStamp;
    unsigned long imageSize;
    unsigned long imageCheckSum;
    unsigned long functionRva;
    unsigned long functionOperationRx;
    unsigned long functionOperationReadOnlyNx;
    unsigned long functionParameterCount;
    unsigned long pdbAge;
    unsigned long processorCount;
    long lastStatus;
    unsigned long reserved;
    unsigned char pdbGuid[16];
    unsigned char signature[64];
    unsigned char signatureMask[64];
} KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long targetKind;
    unsigned long reserved;
    unsigned long long targetAddress;
} KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST;

typedef struct _KSWORD_ARK_RXPF_PAGE_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long state;
    unsigned long targetKind;
    unsigned long flags;
    unsigned long generation;
    unsigned long referenceCount;
    unsigned long emulationEnabled;
    unsigned long long recordId;
    unsigned long long pageBase;
    unsigned long long writableAlias;
    unsigned long long pfn;
    unsigned long long ownerImageBase;
    unsigned long long faultCount;
    unsigned long long emulatedCount;
    unsigned long long unsupportedCount;
    long lastStatus;
    unsigned long lastFailureReason;
    unsigned long originalProtection;
    unsigned long currentProtection;
    unsigned long writableAliasProtection;
    unsigned long lastWriteOffset;
    unsigned long lastWriteLength;
    unsigned char lastWriteBytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
} KSWORD_ARK_RXPF_PAGE_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_RECORD_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
} KSWORD_ARK_RXPF_RECORD_REQUEST;

typedef struct _KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
    unsigned long offset;
    unsigned long length;
    unsigned char bytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
} KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST;

typedef struct _KSWORD_ARK_RXPF_SET_EMULATION_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
    unsigned long enable;
    unsigned long reserved;
} KSWORD_ARK_RXPF_SET_EMULATION_REQUEST;

typedef struct _KSWORD_ARK_RXPF_STATS_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long generation;
    unsigned long registeredPages;
    unsigned long enabledPages;
    unsigned long idtInstalled;
    unsigned long processorCount;
    unsigned long activeHandlers;
    unsigned long long totalFaults;
    unsigned long long managedFaults;
    unsigned long long emulatedInstructions;
    unsigned long long chainedFaults;
    unsigned long long recursiveFaults;
    unsigned long long unsupportedInstructions;
    unsigned long long droppedEvents;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_RXPF_STATS_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_EVENT_ROW
{
    unsigned long long sequence;
    unsigned long long timestamp;
    unsigned long long cr2;
    unsigned long long rip;
    unsigned long long errorCode;
    unsigned long long recordId;
    unsigned long long newRip;
    unsigned long processorIndex;
    unsigned long decodedInstruction;
    unsigned long emulationResult;
    long status;
} KSWORD_ARK_RXPF_EVENT_ROW;

typedef struct _KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long afterSequence;
    unsigned long maxRows;
    unsigned long reserved;
} KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST;

typedef struct _KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long returnedRows;
    unsigned long availableRows;
    unsigned long long newestSequence;
    unsigned long long droppedRows;
    KSWORD_ARK_RXPF_EVENT_ROW rows[KSWORD_ARK_RXPF_MAX_EVENT_ROWS];
} KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_SELF_TEST_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long result;
    unsigned long instructionCount;
    unsigned long long recordId;
    unsigned long long returnedValue;
    unsigned long long expectedValue;
    unsigned long long faultsObserved;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_RXPF_SELF_TEST_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
    unsigned long long hookAddress;
    unsigned long hookAction;
    unsigned long reserved;
    unsigned long long redirectTarget;
} KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST;

typedef struct _KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long long recordId;
    unsigned long long hookAddress;
    unsigned long hookAction;
    unsigned long state;
    unsigned long long redirectTarget;
    unsigned long long redirectCount;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE;
