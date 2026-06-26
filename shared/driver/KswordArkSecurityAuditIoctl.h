#pragma once

#include "KswordArkTrustIoctl.h"

// ============================================================
// KswordArkSecurityAuditIoctl.h
// Purpose:
// - Defines read-only Security/CI/VBS/SKCI/Hyper-V posture audit IOCTLs.
// - The protocol returns observable status, source counts, and conflict flags.
// - The protocol intentionally has no policy mutation, bypass, unlink, or disable operation.
// ============================================================

#define KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_SECURITY_STATUS      0x8D0UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_TRUST_VIEW    0x8D1UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_HYPERV_SUMMARY       0x8D2UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_APP_CONTROL_STATUS   0x8D3UL

#define IOCTL_KSWORD_ARK_QUERY_SECURITY_STATUS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_SECURITY_STATUS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_TRUST_VIEW, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_HYPERV_SUMMARY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_HYPERV_SUMMARY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_APP_CONTROL_STATUS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_APP_CONTROL_STATUS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_SECURITY_AUDIT_STATE_UNKNOWN     0UL
#define KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT     1UL
#define KSWORD_ARK_SECURITY_AUDIT_STATE_ABSENT      2UL
#define KSWORD_ARK_SECURITY_AUDIT_STATE_ENABLED     3UL
#define KSWORD_ARK_SECURITY_AUDIT_STATE_DISABLED    4UL
#define KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE 5UL
#define KSWORD_ARK_SECURITY_AUDIT_STATE_DEGRADED    6UL

#define KSWORD_ARK_SECURITY_AUDIT_SOURCE_NONE           0x00000000UL
#define KSWORD_ARK_SECURITY_AUDIT_SOURCE_SYSTEM_QUERY   0x00000001UL
#define KSWORD_ARK_SECURITY_AUDIT_SOURCE_MODULE_LIST    0x00000002UL
#define KSWORD_ARK_SECURITY_AUDIT_SOURCE_CPUID          0x00000004UL
#define KSWORD_ARK_SECURITY_AUDIT_SOURCE_EXPORT         0x00000008UL
#define KSWORD_ARK_SECURITY_AUDIT_SOURCE_SIGNING_CACHE  0x00000010UL

#define KSWORD_ARK_SECURITY_STATUS_FIELD_CI_OPTIONS       0x00000001UL
#define KSWORD_ARK_SECURITY_STATUS_FIELD_SECURE_BOOT      0x00000002UL
#define KSWORD_ARK_SECURITY_STATUS_FIELD_CI_FLAGS         0x00000004UL
#define KSWORD_ARK_SECURITY_STATUS_FIELD_KD_FLAGS         0x00000008UL
#define KSWORD_ARK_SECURITY_STATUS_FIELD_CI_MODULE        0x00000010UL
#define KSWORD_ARK_SECURITY_STATUS_FIELD_SECUREKERNEL     0x00000020UL
#define KSWORD_ARK_SECURITY_STATUS_FIELD_SKCI             0x00000040UL
#define KSWORD_ARK_SECURITY_STATUS_FIELD_VBS_HVCI         0x00000080UL

#define KSWORD_ARK_DRIVER_TRUST_FLAG_SIGNING_PRESENT       0x00000001UL
#define KSWORD_ARK_DRIVER_TRUST_FLAG_SYSTEM_MODULE_PRESENT 0x00000002UL
#define KSWORD_ARK_DRIVER_TRUST_FLAG_PATH_HASH_PRESENT     0x00000004UL
#define KSWORD_ARK_DRIVER_TRUST_FLAG_NAME_PRESENT          0x00000008UL

#define KSWORD_ARK_DRIVER_TRUST_CONFLICT_NONE                 0x00000000UL
#define KSWORD_ARK_DRIVER_TRUST_CONFLICT_EMPTY_IMAGE_RANGE    0x00000001UL
#define KSWORD_ARK_DRIVER_TRUST_CONFLICT_PATH_UNAVAILABLE     0x00000002UL
#define KSWORD_ARK_DRIVER_TRUST_CONFLICT_SIGNING_UNAVAILABLE  0x00000004UL
#define KSWORD_ARK_DRIVER_TRUST_CONFLICT_UNSIGNED_OR_UNKNOWN  0x00000008UL

#define KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_INCLUDE_SIGNING_LEVEL 0x00000001UL
#define KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_DEFAULT \
    (KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_INCLUDE_SIGNING_LEVEL)

#define KSWORD_ARK_SECURITY_AUDIT_MAX_DRIVER_ROWS 256UL
#define KSWORD_ARK_SECURITY_AUDIT_DEFAULT_DRIVER_ROWS 64UL
#define KSWORD_ARK_SECURITY_AUDIT_NAME_CHARS 64U
#define KSWORD_ARK_SECURITY_AUDIT_VENDOR_CHARS 16U

// Request for fixed-size security status queries.
typedef struct _KSWORD_ARK_QUERY_SECURITY_STATUS_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long flags;
    unsigned long reserved;
} KSWORD_ARK_QUERY_SECURITY_STATUS_REQUEST;

// Response for CI/VBS/Secure Kernel/SKCI/test-signing/debug posture.
typedef struct _KSWORD_ARK_QUERY_SECURITY_STATUS_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long fieldFlags;
    unsigned long sourceMask;
    long queryStatus;
    unsigned long codeIntegrityOptions;
    unsigned long secureBootEnabled;
    unsigned long secureBootCapable;
    unsigned long ciEnabled;
    unsigned long umciEnabled;
    unsigned long hvciKmciEnabled;
    unsigned long hvciAuditMode;
    unsigned long hvciStrictMode;
    unsigned long hvciIumEnabled;
    unsigned long vbsPresent;
    unsigned long testSigningEnabled;
    unsigned long ciDebugModeEnabled;
    unsigned long testBuild;
    unsigned long flightBuild;
    unsigned long flightingEnabled;
    unsigned long kernelDebuggerEnabled;
    unsigned long kernelDebuggerNotPresent;
    unsigned long ciModuleLoaded;
    unsigned long secureKernelModuleLoaded;
    unsigned long skciModuleLoaded;
    long codeIntegrityStatus;
    long secureBootStatus;
    long moduleQueryStatus;
    long debuggerStatus;
} KSWORD_ARK_QUERY_SECURITY_STATUS_RESPONSE;

// Request for bounded loaded-driver trust cross-view rows.
typedef struct _KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long flags;
    unsigned long maxEntries;
} KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST;

// One bounded driver trust row sourced from SystemModuleInformation and optional signing cache.
typedef struct _KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY
{
    unsigned long size;
    unsigned long fieldFlags;
    unsigned long sourceMask;
    unsigned long sourceCount;
    unsigned long conflictFlags;
    unsigned long signingLevel;
    unsigned long signingLevelFlags;
    unsigned long imageSize;
    unsigned long pathHash;
    long signingStatus;
    unsigned long long imageBase;
    wchar_t moduleName[KSWORD_ARK_SECURITY_AUDIT_NAME_CHARS];
} KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY;

// Variable-length response; entries are present only when the output buffer has room.
typedef struct _KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long fieldFlags;
    unsigned long sourceMask;
    long queryStatus;
    unsigned long totalModuleCount;
    unsigned long entryCount;
    unsigned long maxEntriesAccepted;
    unsigned long truncated;
    long moduleQueryStatus;
    long signingResolverStatus;
    unsigned long reserved;
    KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY entries[1];
} KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE;

// Response for read-only Hyper-V availability/status skeleton.
typedef struct _KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long fieldFlags;
    unsigned long sourceMask;
    long queryStatus;
    unsigned long hypervisorPresent;
    unsigned long rootPartitionStatus;
    unsigned long vmbusStatus;
    unsigned long vSwitchStatus;
    unsigned long vPciStatus;
    unsigned long hvSocketStatus;
    unsigned long winHvStatus;
    unsigned long winHvRuntimeStatus;
    unsigned long hvLoaderStatus;
    long moduleQueryStatus;
    wchar_t hypervisorVendor[KSWORD_ARK_SECURITY_AUDIT_VENDOR_CHARS];
} KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE;

// Response for AppID/AppLocker/mssecflt presence/status/callback-owner posture.
typedef struct _KSWORD_ARK_QUERY_APP_CONTROL_STATUS_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long fieldFlags;
    unsigned long sourceMask;
    long queryStatus;
    unsigned long appidStatus;
    unsigned long appidPolicyStatus;
    unsigned long appLockerFilterStatus;
    unsigned long appLockerCallbackOwnerStatus;
    unsigned long mssecfltStatus;
    unsigned long mssecfltCallbackOwnerStatus;
    unsigned long ahcacheStatus;
    unsigned long bamStatus;
    long moduleQueryStatus;
    wchar_t appLockerOwnerModule[KSWORD_ARK_SECURITY_AUDIT_NAME_CHARS];
    wchar_t mssecfltOwnerModule[KSWORD_ARK_SECURITY_AUDIT_NAME_CHARS];
} KSWORD_ARK_QUERY_APP_CONTROL_STATUS_RESPONSE;
