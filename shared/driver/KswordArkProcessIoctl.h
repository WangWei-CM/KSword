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

#define KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE 0x00000001UL

#define KSWORD_ARK_PROCESS_FLAG_KERNEL_ENUMERATED 0x00000001UL
#define KSWORD_ARK_PROCESS_FLAG_HIDDEN_FROM_ACTIVE_LIST 0x00000002UL

typedef struct _KSWORD_ARK_ENUM_PROCESS_REQUEST
{
    unsigned long flags;
    unsigned long startPid;
    unsigned long endPid;
    unsigned long reserved;
} KSWORD_ARK_ENUM_PROCESS_REQUEST;

typedef struct _KSWORD_ARK_PROCESS_ENTRY
{
    unsigned long processId;
    unsigned long parentProcessId;
    unsigned long flags;
    unsigned long reserved;
    char imageName[16];
} KSWORD_ARK_PROCESS_ENTRY;

typedef struct _KSWORD_ARK_ENUM_PROCESS_RESPONSE
{
    unsigned long version;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    KSWORD_ARK_PROCESS_ENTRY entries[1];
} KSWORD_ARK_ENUM_PROCESS_RESPONSE;
