#pragma once

#include "KswordArkProcessIoctl.h"

// Optional R3 -> R0 branding packet for the VMware-only bugcheck panel.
// The diagnostic feature itself is detected and enabled entirely in R0.
#ifndef FILE_WRITE_ACCESS
#define FILE_WRITE_ACCESS 0x0002
#endif

#define KSWORD_ARK_BUGCHECK_BITMAP_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_BUGCHECK_BITMAP_MAGIC 0x4942534BUL /* 'KSBI' */
#define KSWORD_ARK_BUGCHECK_BITMAP_FORMAT_BGRA32 1UL

#define KSWORD_ARK_BUGCHECK_BITMAP_MAX_WIDTH 1024UL
#define KSWORD_ARK_BUGCHECK_BITMAP_MAX_HEIGHT 384UL
#define KSWORD_ARK_BUGCHECK_BITMAP_MAX_BYTES \
    (KSWORD_ARK_BUGCHECK_BITMAP_MAX_WIDTH * \
     KSWORD_ARK_BUGCHECK_BITMAP_MAX_HEIGHT * 4UL)

#define KSWORD_ARK_IOCTL_FUNCTION_SET_BUGCHECK_BITMAP 0x8FAUL

#define IOCTL_KSWORD_ARK_SET_BUGCHECK_BITMAP \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SET_BUGCHECK_BITMAP, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

typedef struct _KSWORD_ARK_BUGCHECK_BITMAP_HEADER
{
    unsigned long version;
    unsigned long size;
    unsigned long magic;
    unsigned long width;
    unsigned long height;
    unsigned long stride;
    unsigned long format;
    unsigned long brandColorRgb;
    unsigned long dataLength;
    unsigned long flags;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_BUGCHECK_BITMAP_HEADER;

