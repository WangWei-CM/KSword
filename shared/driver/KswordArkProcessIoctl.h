#pragma once

// ============================================================
// KswordArkProcessIoctl.h
// Purpose:
// - Shared IOCTL code and request struct for R3 <-> R0 process actions.
// - This file must be included by both user mode and kernel mode.
// ============================================================

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif

#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif

#ifndef FILE_WRITE_ACCESS
#define FILE_WRITE_ACCESS 0x0002
#endif

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#define KSWORD_ARK_IOCTL_DEVICE_TYPE FILE_DEVICE_UNKNOWN
#define KSWORD_ARK_IOCTL_FUNCTION_TERMINATE_PROCESS 0x801
#define KSWORD_ARK_IOCTL_FUNCTION_SUSPEND_PROCESS 0x802
#define KSWORD_ARK_IOCTL_FUNCTION_SET_PPL_LEVEL 0x803
#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_PROCESS 0x805

#define IOCTL_KSWORD_ARK_TERMINATE_PROCESS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_TERMINATE_PROCESS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

typedef struct _KSWORD_ARK_TERMINATE_PROCESS_REQUEST
{
    unsigned long processId;
    long exitStatus;
} KSWORD_ARK_TERMINATE_PROCESS_REQUEST;

#define IOCTL_KSWORD_ARK_SUSPEND_PROCESS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SUSPEND_PROCESS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

typedef struct _KSWORD_ARK_SUSPEND_PROCESS_REQUEST
{
    unsigned long processId;
} KSWORD_ARK_SUSPEND_PROCESS_REQUEST;

#define IOCTL_KSWORD_ARK_SET_PPL_LEVEL \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SET_PPL_LEVEL, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

typedef struct _KSWORD_ARK_SET_PPL_LEVEL_REQUEST
{
    unsigned long processId;
    unsigned char protectionLevel;
    unsigned char reserved[3];
} KSWORD_ARK_SET_PPL_LEVEL_REQUEST;

#define IOCTL_KSWORD_ARK_ENUM_PROCESS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_PROCESS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION 2UL
#define KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE 0x00000001UL

// Phase-2 EPROCESS offset sentinel shared by R0 protocol and R3 UI models.
// Keep it local to the process protocol so user mode does not include driver-only
// ark_dyndata.h just to compare unavailable offsets.
#define KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE 0xFFFFFFFFUL

#define KSWORD_ARK_PROCESS_FLAG_KERNEL_ENUMERATED 0x00000001UL
#define KSWORD_ARK_PROCESS_FLAG_HIDDEN_FROM_ACTIVE_LIST 0x00000002UL

// Process field flags describe which optional Phase-2 values are valid.
// The old v1 fields remain at the beginning of KSWORD_ARK_PROCESS_ENTRY.
#define KSWORD_ARK_PROCESS_FIELD_SESSION_PRESENT                 0x00000001UL
#define KSWORD_ARK_PROCESS_FIELD_IMAGE_PATH_PRESENT              0x00000002UL
#define KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT              0x00000004UL
#define KSWORD_ARK_PROCESS_FIELD_SIGNATURE_LEVEL_PRESENT         0x00000008UL
#define KSWORD_ARK_PROCESS_FIELD_SECTION_SIGNATURE_LEVEL_PRESENT 0x00000010UL
#define KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_AVAILABLE          0x00000020UL
#define KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_VALUE_PRESENT      0x00000040UL
#define KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_AVAILABLE        0x00000080UL
#define KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_VALUE_PRESENT    0x00000100UL

// Field source labels are intentionally protocol-local so ProcessIoctl.h does
// not depend on the DynData protocol header and can remain the base include.
#define KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE             0UL
#define KSWORD_ARK_PROCESS_FIELD_SOURCE_PUBLIC_API              1UL
#define KSWORD_ARK_PROCESS_FIELD_SOURCE_SYSTEM_INFORMER_DYNDATA 2UL
#define KSWORD_ARK_PROCESS_FIELD_SOURCE_RUNTIME_PATTERN         3UL

// R0 status summarizes how complete the extended row is for UI presentation.
#define KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE     0UL
#define KSWORD_ARK_PROCESS_R0_STATUS_OK              1UL
#define KSWORD_ARK_PROCESS_R0_STATUS_PARTIAL         2UL
#define KSWORD_ARK_PROCESS_R0_STATUS_DYNDATA_MISSING 3UL
#define KSWORD_ARK_PROCESS_R0_STATUS_READ_FAILED     4UL

#define KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS 260U

typedef struct _KSWORD_ARK_ENUM_PROCESS_REQUEST
{
    unsigned long flags;
    unsigned long startPid;
    unsigned long endPid;
    unsigned long reserved;
} KSWORD_ARK_ENUM_PROCESS_REQUEST;

typedef struct _KSWORD_ARK_PROCESS_ENTRY
{
    // v1 fixed fields: keep these first for backward-compatible parsers.
    unsigned long processId;
    unsigned long parentProcessId;
    unsigned long flags;
    unsigned long reserved;
    char imageName[16];

    // v2 public/process fields.
    unsigned long sessionId;
    unsigned long fieldFlags;
    unsigned long r0Status;
    unsigned long sessionSource;

    // v2 EPROCESS protection bytes and their field provenance.
    unsigned char protection;
    unsigned char signatureLevel;
    unsigned char sectionSignatureLevel;
    unsigned char reservedByte;
    unsigned long protectionSource;
    unsigned long signatureLevelSource;
    unsigned long sectionSignatureLevelSource;

    // v2 EPROCESS object pointers and DynData provenance.
    unsigned long objectTableSource;
    unsigned long sectionObjectSource;
    unsigned long imagePathSource;
    unsigned long reserved2;
    unsigned long protectionOffset;
    unsigned long signatureLevelOffset;
    unsigned long sectionSignatureLevelOffset;
    unsigned long objectTableOffset;
    unsigned long sectionObjectOffset;
    unsigned long long objectTableAddress;
    unsigned long long sectionObjectAddress;
    unsigned long long dynDataCapabilityMask;

    // v2 full image path. UTF-16 code units are used without requiring WCHAR in
    // this shared header, so R3 can copy them into std::wstring directly.
    unsigned short imagePath[KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS];
} KSWORD_ARK_PROCESS_ENTRY;

typedef struct _KSWORD_ARK_ENUM_PROCESS_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    KSWORD_ARK_PROCESS_ENTRY entries[1];
} KSWORD_ARK_ENUM_PROCESS_RESPONSE;
