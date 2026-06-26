#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkStorageIoctl.h
// Purpose:
// - Shared R3/R0 protocol for read-only storage, BitLocker/FVE,
//   MountMgr, and file-system integrity audit queries.
// - The protocol only serializes topology, status labels, owner
//   modules, diagnostic addresses, and bounded fixed strings.
// - The protocol never carries BitLocker key material, protector
//   payloads, encrypted metadata blobs, patch data, or bypass data.
// ============================================================

#define KSWORD_ARK_STORAGE_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_VOLUME_STACK_AUDIT 0x8C0UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_BITLOCKER_FVE_AUDIT 0x8C1UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_MOUNTMGR_MAPPING_AUDIT 0x8C2UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_FILESYSTEM_INTEGRITY_AUDIT 0x8C3UL

#define IOCTL_KSWORD_ARK_QUERY_VOLUME_STACK_AUDIT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_VOLUME_STACK_AUDIT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_BITLOCKER_FVE_AUDIT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_BITLOCKER_FVE_AUDIT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_AUDIT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_MOUNTMGR_MAPPING_AUDIT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_AUDIT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_FILESYSTEM_INTEGRITY_AUDIT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_NAMES 0x00000001UL
#define KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_ATTACHED_STACK 0x00000002UL
#define KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_DISPATCH 0x00000004UL
#define KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_FAST_IO 0x00000008UL
#define KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_DEFAULT \
    (KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_NAMES | \
     KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_ATTACHED_STACK | \
     KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_DISPATCH | \
     KSWORD_ARK_STORAGE_AUDIT_FLAG_INCLUDE_FAST_IO)

#define KSWORD_ARK_STORAGE_QUERY_STATUS_OK 1UL
#define KSWORD_ARK_STORAGE_QUERY_STATUS_EMPTY 2UL
#define KSWORD_ARK_STORAGE_QUERY_STATUS_PARTIAL 3UL
#define KSWORD_ARK_STORAGE_QUERY_STATUS_UNAVAILABLE 4UL
#define KSWORD_ARK_STORAGE_QUERY_STATUS_INVALID_REQUEST 5UL

#define KSWORD_ARK_STORAGE_FIELD_VOLUME_DEVICE_PRESENT 0x00000001UL
#define KSWORD_ARK_STORAGE_FIELD_DRIVER_NAME_PRESENT 0x00000002UL
#define KSWORD_ARK_STORAGE_FIELD_FVEVOL_PRESENT 0x00000004UL
#define KSWORD_ARK_STORAGE_FIELD_DOS_NAME_PRESENT 0x00000008UL
#define KSWORD_ARK_STORAGE_FIELD_VOLUME_GUID_PRESENT 0x00000010UL
#define KSWORD_ARK_STORAGE_FIELD_NT_DEVICE_PATH_PRESENT 0x00000020UL
#define KSWORD_ARK_STORAGE_FIELD_OWNER_MODULE_PRESENT 0x00000040UL
#define KSWORD_ARK_STORAGE_FIELD_STATUS_DERIVED_FROM_STACK 0x00000080UL

#define KSWORD_ARK_STORAGE_RISK_NONE 0x00000000UL
#define KSWORD_ARK_STORAGE_RISK_OWNER_UNKNOWN 0x00000001UL
#define KSWORD_ARK_STORAGE_RISK_TARGET_OUTSIDE_MODULES 0x00000002UL
#define KSWORD_ARK_STORAGE_RISK_STACK_TRUNCATED 0x00000004UL
#define KSWORD_ARK_STORAGE_RISK_STACK_LOOP_SUSPECTED 0x00000008UL
#define KSWORD_ARK_STORAGE_RISK_FVEVOL_ABSENT 0x00000010UL
#define KSWORD_ARK_STORAGE_RISK_STATUS_UNCONFIRMED 0x00000020UL

#define KSWORD_ARK_STORAGE_CONFIDENCE_UNAVAILABLE 0UL
#define KSWORD_ARK_STORAGE_CONFIDENCE_PARTIAL 35UL
#define KSWORD_ARK_STORAGE_CONFIDENCE_STACK_DERIVED 65UL
#define KSWORD_ARK_STORAGE_CONFIDENCE_CONFIRMED 90UL

#define KSWORD_ARK_STORAGE_SLOT_TYPE_MAJOR_FUNCTION 1UL
#define KSWORD_ARK_STORAGE_SLOT_TYPE_FAST_IO 2UL

#define KSWORD_ARK_STORAGE_FVE_STATUS_UNKNOWN 0UL
#define KSWORD_ARK_STORAGE_FVE_STATUS_NOT_PRESENT 1UL
#define KSWORD_ARK_STORAGE_FVE_STATUS_PRESENT 2UL

#define KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS 260U
#define KSWORD_ARK_STORAGE_DRIVER_NAME_CHARS 96U
#define KSWORD_ARK_STORAGE_MODULE_NAME_CHARS 160U
#define KSWORD_ARK_STORAGE_DETAIL_CHARS 192U
#define KSWORD_ARK_STORAGE_DRIVE_LETTER_CHARS 16U
#define KSWORD_ARK_STORAGE_VOLUME_GUID_CHARS 96U
#define KSWORD_ARK_STORAGE_DEFAULT_MAX_ROWS 128UL
#define KSWORD_ARK_STORAGE_HARD_MAX_ROWS 1024UL
#define KSWORD_ARK_STORAGE_DEFAULT_STACK_DEPTH 32UL
#define KSWORD_ARK_STORAGE_HARD_STACK_DEPTH 64UL

// Request shared by storage audit queries. The optional NT volume path lets R3
// scope R0 work to one known volume while still allowing supported-empty output.
typedef struct _KSWORD_ARK_STORAGE_AUDIT_REQUEST
{
    unsigned long version;
    unsigned long size;
    unsigned long flags;
    unsigned long maxRows;
    unsigned long maxDepth;
    unsigned long reserved;
    unsigned short volumePathLengthChars;
    unsigned short reservedChars;
    wchar_t volumePath[KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS];
} KSWORD_ARK_STORAGE_AUDIT_REQUEST;

// One device-stack row. Addresses are diagnostic display values only and must
// not be treated as credentials for later mutation requests.
typedef struct _KSWORD_ARK_VOLUME_STACK_ROW
{
    unsigned long rowType;
    unsigned long stackIndex;
    unsigned long deviceType;
    unsigned long deviceCharacteristics;
    unsigned long fieldFlags;
    unsigned long riskFlags;
    unsigned long confidence;
    long lastStatus;
    unsigned long long deviceObjectAddress;
    unsigned long long driverObjectAddress;
    unsigned long long attachedDeviceAddress;
    unsigned long long lowerDeviceAddress;
    wchar_t volumeDeviceName[KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS];
    wchar_t driverName[KSWORD_ARK_STORAGE_DRIVER_NAME_CHARS];
    wchar_t detail[KSWORD_ARK_STORAGE_DETAIL_CHARS];
} KSWORD_ARK_VOLUME_STACK_ROW;

// Volume stack response contains a bounded flat stack and the derived fvevol
// position. fvevolPosition is 0xFFFFFFFF when no fvevol row is present.
typedef struct _KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long rowSize;
    unsigned long queryStatus;
    unsigned long responseFlags;
    unsigned long fieldFlags;
    unsigned long totalRows;
    unsigned long returnedRows;
    unsigned long maxRows;
    unsigned long fvevolPresent;
    unsigned long fvevolPosition;
    long lastStatus;
    KSWORD_ARK_VOLUME_STACK_ROW rows[1];
} KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE;

// BitLocker/FVE row intentionally reports only safe status labels and counts.
// All key/protector payloads are excluded from the protocol by design.
typedef struct _KSWORD_ARK_BITLOCKER_FVE_ROW
{
    unsigned long fieldFlags;
    unsigned long fvevolPresent;
    unsigned long fvevolStackPosition;
    unsigned long protectionStatus;
    unsigned long conversionStatus;
    unsigned long lockStatus;
    unsigned long keyProtectorTypeCountTpm;
    unsigned long keyProtectorTypeCountTpmPin;
    unsigned long keyProtectorTypeCountRecoveryPassword;
    unsigned long keyProtectorTypeCountRecoveryKey;
    unsigned long keyProtectorTypeCountStartupKey;
    unsigned long keyProtectorTypeCountClearOrSuspended;
    unsigned long confidence;
    unsigned long riskFlags;
    long lastStatus;
    wchar_t volumeDeviceName[KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS];
    wchar_t detail[KSWORD_ARK_STORAGE_DETAIL_CHARS];
} KSWORD_ARK_BITLOCKER_FVE_ROW;

typedef struct _KSWORD_ARK_QUERY_BITLOCKER_FVE_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long rowSize;
    unsigned long queryStatus;
    unsigned long responseFlags;
    unsigned long fieldFlags;
    unsigned long totalRows;
    unsigned long returnedRows;
    unsigned long maxRows;
    long lastStatus;
    KSWORD_ARK_BITLOCKER_FVE_ROW rows[1];
} KSWORD_ARK_QUERY_BITLOCKER_FVE_RESPONSE;

// MountMgr mapping row carries symbolic names only. Volume GUID is optional and
// remains empty when R0 cannot derive it safely.
typedef struct _KSWORD_ARK_MOUNTMGR_MAPPING_ROW
{
    unsigned long fieldFlags;
    unsigned long confidence;
    unsigned long riskFlags;
    long lastStatus;
    wchar_t driveLetter[KSWORD_ARK_STORAGE_DRIVE_LETTER_CHARS];
    wchar_t volumeGuid[KSWORD_ARK_STORAGE_VOLUME_GUID_CHARS];
    wchar_t ntDevicePath[KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS];
    wchar_t detail[KSWORD_ARK_STORAGE_DETAIL_CHARS];
} KSWORD_ARK_MOUNTMGR_MAPPING_ROW;

typedef struct _KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long rowSize;
    unsigned long queryStatus;
    unsigned long responseFlags;
    unsigned long fieldFlags;
    unsigned long totalRows;
    unsigned long returnedRows;
    unsigned long maxRows;
    long lastStatus;
    KSWORD_ARK_MOUNTMGR_MAPPING_ROW rows[1];
} KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_RESPONSE;

// File-system integrity row reports DriverObject dispatch and FastIo owner
// evidence for NTFS/ReFS/FAT/exFAT without changing any pointer.
typedef struct _KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW
{
    unsigned long fileSystemKind;
    unsigned long slotType;
    unsigned long slotIndex;
    unsigned long fieldFlags;
    unsigned long riskFlags;
    unsigned long confidence;
    long lastStatus;
    unsigned long long driverObjectAddress;
    unsigned long long driverStart;
    unsigned long driverSize;
    unsigned long reserved;
    unsigned long long slotAddress;
    unsigned long long targetAddress;
    unsigned long long ownerModuleBase;
    unsigned long ownerModuleSize;
    wchar_t driverName[KSWORD_ARK_STORAGE_DRIVER_NAME_CHARS];
    wchar_t ownerModuleName[KSWORD_ARK_STORAGE_MODULE_NAME_CHARS];
    wchar_t detail[KSWORD_ARK_STORAGE_DETAIL_CHARS];
} KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW;

typedef struct _KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long rowSize;
    unsigned long queryStatus;
    unsigned long responseFlags;
    unsigned long fieldFlags;
    unsigned long totalRows;
    unsigned long returnedRows;
    unsigned long maxRows;
    long lastStatus;
    KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW rows[1];
} KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_RESPONSE;
