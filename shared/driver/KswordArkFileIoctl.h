#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkFileIoctl.h
// Purpose:
// - Shared IOCTL code and request struct for R3 <-> R0 file actions.
// - Current scope: delete a single file-system path by NT path.
// ============================================================

#define KSWORD_ARK_IOCTL_FUNCTION_DELETE_PATH 0x804

#define IOCTL_KSWORD_ARK_DELETE_PATH \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DELETE_PATH, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY 0x00000001UL
#define KSWORD_ARK_DELETE_PATH_MAX_CHARS 1024U

typedef struct _KSWORD_ARK_DELETE_PATH_REQUEST
{
    unsigned long flags;
    unsigned short pathLengthChars;
    unsigned short reserved;
    wchar_t path[KSWORD_ARK_DELETE_PATH_MAX_CHARS];
} KSWORD_ARK_DELETE_PATH_REQUEST;
