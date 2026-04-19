#pragma once

// ============================================================
// KswordArkProcessIoctl.h
// Purpose:
// - Shared IOCTL code and request struct for R3 <-> R0 process termination.
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
