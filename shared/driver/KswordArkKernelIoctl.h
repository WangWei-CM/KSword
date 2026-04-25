#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkKernelIoctl.h
// Purpose:
// - Shared IOCTL code and structs for R3 <-> R0 kernel inspection.
// - Current scope: SSDT traversal snapshot.
// ============================================================

#define KSWORD_ARK_IOCTL_FUNCTION_ENUM_SSDT 0x806

#define IOCTL_KSWORD_ARK_ENUM_SSDT \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUM_SSDT, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_ENUM_SSDT_PROTOCOL_VERSION 1UL

// Request flags.
#define KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED 0x00000001UL

// Entry flags.
#define KSWORD_ARK_SSDT_ENTRY_FLAG_INDEX_RESOLVED 0x00000001UL
#define KSWORD_ARK_SSDT_ENTRY_FLAG_TABLE_ADDRESS_VALID 0x00000002UL

#define KSWORD_ARK_SSDT_ENTRY_MAX_NAME 96U
#define KSWORD_ARK_SSDT_ENTRY_MAX_MODULE 64U

typedef struct _KSWORD_ARK_ENUM_SSDT_REQUEST
{
    unsigned long flags;
    unsigned long reserved;
} KSWORD_ARK_ENUM_SSDT_REQUEST;

typedef struct _KSWORD_ARK_SSDT_ENTRY
{
    unsigned long serviceIndex;
    unsigned long flags;
    unsigned long long zwRoutineAddress;
    unsigned long long serviceRoutineAddress;
    char serviceName[KSWORD_ARK_SSDT_ENTRY_MAX_NAME];
    char moduleName[KSWORD_ARK_SSDT_ENTRY_MAX_MODULE];
} KSWORD_ARK_SSDT_ENTRY;

typedef struct _KSWORD_ARK_ENUM_SSDT_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long long serviceTableBase;
    unsigned long serviceCountFromTable;
    unsigned long reserved;
    KSWORD_ARK_SSDT_ENTRY entries[1];
} KSWORD_ARK_ENUM_SSDT_RESPONSE;

