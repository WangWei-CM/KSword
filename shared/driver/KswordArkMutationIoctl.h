#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkMutationIoctl.h
// Purpose:
// - Defines the shared R3/R0 protocol for controlled kernel mutation
//   transactions.
// - The protocol is intentionally not registered in this phase; a later
//   integration pass must add the IOCTL table entries.
// - All future inline hook restore, process protection byte restore, and
//   callback unlink actions should be represented as prepare/snapshot,
//   commit, rollback, and audit operations before any write is attempted.
// ============================================================

#define KSWORD_ARK_MUTATION_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_MUTATION_PREPARE     0x832UL
#define KSWORD_ARK_IOCTL_FUNCTION_MUTATION_COMMIT      0x833UL
#define KSWORD_ARK_IOCTL_FUNCTION_MUTATION_ROLLBACK    0x834UL
#define KSWORD_ARK_IOCTL_FUNCTION_MUTATION_QUERY_AUDIT 0x835UL

#define IOCTL_KSWORD_ARK_MUTATION_PREPARE \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_MUTATION_PREPARE, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_MUTATION_COMMIT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_MUTATION_COMMIT, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_MUTATION_ROLLBACK \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_MUTATION_ROLLBACK, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_MUTATION_QUERY_AUDIT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_MUTATION_QUERY_AUDIT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_MUTATION_MAX_BYTES 64U
#define KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES 3U
#define KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY 64U

#define KSWORD_ARK_MUTATION_TARGET_UNKNOWN 0UL
#define KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL 1UL
#define KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES 2UL
#define KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN 3UL

#define KSWORD_ARK_MUTATION_OPERATION_UNKNOWN 0UL
#define KSWORD_ARK_MUTATION_OPERATION_PREPARE 1UL
#define KSWORD_ARK_MUTATION_OPERATION_COMMIT 2UL
#define KSWORD_ARK_MUTATION_OPERATION_ROLLBACK 3UL
#define KSWORD_ARK_MUTATION_OPERATION_QUERY_AUDIT 4UL

#define KSWORD_ARK_MUTATION_FLAG_FORCE 0x00000001UL
#define KSWORD_ARK_MUTATION_FLAG_UI_CONFIRMED 0x00000002UL
#define KSWORD_ARK_MUTATION_FLAG_DRY_RUN 0x00000004UL
#define KSWORD_ARK_MUTATION_FLAG_EXPECTED_BEFORE_PRESENT 0x00000008UL

#define KSWORD_ARK_MUTATION_STATUS_UNKNOWN 0UL
#define KSWORD_ARK_MUTATION_STATUS_PREPARED 1UL
#define KSWORD_ARK_MUTATION_STATUS_DRY_RUN 2UL
#define KSWORD_ARK_MUTATION_STATUS_COMMITTED 3UL
#define KSWORD_ARK_MUTATION_STATUS_ROLLED_BACK 4UL
#define KSWORD_ARK_MUTATION_STATUS_ALREADY_AT_BEFORE 5UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_INVALID_REQUEST 6UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_UNKNOWN_TARGET 7UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_SIZE_LIMIT 8UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_SAFETY_POLICY 9UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_BEFORE_MISMATCH 10UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_UNSUPPORTED_TARGET 11UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_PLAN_ONLY 12UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_NOT_FOUND 13UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_BUSY 14UL
#define KSWORD_ARK_MUTATION_STATUS_REJECTED_TARGET_CHANGED 15UL
#define KSWORD_ARK_MUTATION_STATUS_READ_FAILED 16UL
#define KSWORD_ARK_MUTATION_STATUS_WRITE_FAILED 17UL

#define KSWORD_ARK_MUTATION_RISK_NONE 0x00000000UL
#define KSWORD_ARK_MUTATION_RISK_FORCE_REQUIRED 0x00000001UL
#define KSWORD_ARK_MUTATION_RISK_FORCE_USED 0x00000002UL
#define KSWORD_ARK_MUTATION_RISK_DRY_RUN 0x00000004UL
#define KSWORD_ARK_MUTATION_RISK_POLICY_REQUIRED 0x00000008UL
#define KSWORD_ARK_MUTATION_RISK_POLICY_DENIED 0x00000010UL
#define KSWORD_ARK_MUTATION_RISK_BEFORE_MISMATCH 0x00000020UL
#define KSWORD_ARK_MUTATION_RISK_WRITE_BLOCKED_BY_DESIGN 0x00000040UL
#define KSWORD_ARK_MUTATION_RISK_PLAN_ONLY 0x00000080UL
#define KSWORD_ARK_MUTATION_RISK_DYNDATA_REQUIRED 0x00000100UL
#define KSWORD_ARK_MUTATION_RISK_DYNDATA_CONFIRMED 0x00000200UL
#define KSWORD_ARK_MUTATION_RISK_CANONICAL_REQUIRED 0x00000400UL
#define KSWORD_ARK_MUTATION_RISK_NONPAGED_REQUIRED 0x00000800UL
#define KSWORD_ARK_MUTATION_RISK_READ_SNAPSHOT_TAKEN 0x00001000UL
#define KSWORD_ARK_MUTATION_RISK_ROLLBACK_IDEMPOTENT 0x00002000UL
#define KSWORD_ARK_MUTATION_RISK_KERNEL_PATCH_SURFACE 0x00004000UL
#define KSWORD_ARK_MUTATION_RISK_PROCESS_PROTECTION_SURFACE 0x00008000UL
#define KSWORD_ARK_MUTATION_RISK_CALLBACK_UNLINK_SURFACE 0x00010000UL
#define KSWORD_ARK_MUTATION_RISK_SIZE_LIMITED 0x00020000UL
#define KSWORD_ARK_MUTATION_RISK_TARGET_CHANGED 0x00040000UL

#define KSWORD_ARK_MUTATION_QUERY_AUDIT_FLAG_INCLUDE_BYTES 0x00000001UL

typedef struct _KSWORD_ARK_MUTATION_PREPARE_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long flags;
    unsigned long targetKind;
    unsigned long processId;
    unsigned long bytes;
    unsigned long reserved;
    unsigned long reserved2;
    unsigned long long targetAddress;
    unsigned long long targetContext;
    unsigned char afterBytes[KSWORD_ARK_MUTATION_MAX_BYTES];
    unsigned char expectedBeforeBytes[KSWORD_ARK_MUTATION_MAX_BYTES];
} KSWORD_ARK_MUTATION_PREPARE_REQUEST;

typedef struct _KSWORD_ARK_MUTATION_TRANSACTION_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long flags;
    unsigned long reserved;
    unsigned long long transactionId;
} KSWORD_ARK_MUTATION_TRANSACTION_REQUEST;

typedef struct _KSWORD_ARK_MUTATION_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long status;
    unsigned long targetKind;
    unsigned long processId;
    unsigned long bytes;
    unsigned long riskFlags;
    long lastStatus;
    unsigned long long transactionId;
    unsigned long long targetAddress;
    unsigned long long targetContext;
    unsigned long long beforeHash;
    unsigned long long afterHash;
    unsigned long long timestampTick;
    unsigned char beforeBytes[KSWORD_ARK_MUTATION_MAX_BYTES];
    unsigned char afterBytes[KSWORD_ARK_MUTATION_MAX_BYTES];
} KSWORD_ARK_MUTATION_RESPONSE;

typedef struct _KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long flags;
    unsigned long maxEntries;
    unsigned long long startSequence;
} KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST;

typedef struct _KSWORD_ARK_MUTATION_AUDIT_ENTRY
{
    unsigned long size;
    unsigned long version;
    unsigned long operation;
    unsigned long status;
    long lastStatus;
    unsigned long targetKind;
    unsigned long riskFlags;
    unsigned long flags;
    unsigned long processId;
    unsigned long bytes;
    unsigned long reserved;
    unsigned long reserved2;
    unsigned long long transactionId;
    unsigned long long sequence;
    unsigned long long targetAddress;
    unsigned long long targetContext;
    unsigned long long beforeHash;
    unsigned long long afterHash;
    unsigned long long timestampTick;
    unsigned char byteData[KSWORD_ARK_MUTATION_MAX_BYTES];
} KSWORD_ARK_MUTATION_AUDIT_ENTRY;

typedef struct _KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long entrySize;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long lostCount;
    unsigned long reserved;
    unsigned long reserved2;
    unsigned long long oldestSequence;
    unsigned long long nextSequence;
    KSWORD_ARK_MUTATION_AUDIT_ENTRY entries[1];
} KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE;
