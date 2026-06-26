#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkFileIoctl.h
// Purpose:
// - Shared IOCTL code and request struct for R3 <-> R0 file actions.
// - Current scope: delete a single file-system path by NT path.
// - Phase 10 adds a read-only file basic information query packet.
// ============================================================

#define KSWORD_ARK_FILE_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_DELETE_PATH 0x804
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_FILE_INFO 0x812UL

#define IOCTL_KSWORD_ARK_DELETE_PATH \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DELETE_PATH, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_FILE_INFO \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_FILE_INFO, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY 0x00000001UL
#define KSWORD_ARK_DELETE_PATH_MAX_CHARS 1024U

#define KSWORD_ARK_QUERY_FILE_INFO_FLAG_DIRECTORY 0x00000001UL
#define KSWORD_ARK_QUERY_FILE_INFO_FLAG_OPEN_REPARSE_POINT 0x00000002UL
#define KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_OBJECT_NAME 0x00000004UL
#define KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_SECTION_POINTERS 0x00000008UL
#define KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_QUERY_FILE_INFO_FLAG_OPEN_REPARSE_POINT | \
     KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_OBJECT_NAME | \
     KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_SECTION_POINTERS)

#define KSWORD_ARK_FILE_INFO_FIELD_BASIC_PRESENT 0x00000001UL
#define KSWORD_ARK_FILE_INFO_FIELD_STANDARD_PRESENT 0x00000002UL
#define KSWORD_ARK_FILE_INFO_FIELD_OBJECT_NAME_PRESENT 0x00000004UL
#define KSWORD_ARK_FILE_INFO_FIELD_FILE_OBJECT_PRESENT 0x00000008UL
#define KSWORD_ARK_FILE_INFO_FIELD_SECTION_POINTERS_PRESENT 0x00000010UL
#define KSWORD_ARK_FILE_INFO_FIELD_DATA_SECTION_PRESENT 0x00000020UL
#define KSWORD_ARK_FILE_INFO_FIELD_IMAGE_SECTION_PRESENT 0x00000040UL
#define KSWORD_ARK_FILE_INFO_FIELD_DIRECTORY 0x00000080UL
#define KSWORD_ARK_FILE_INFO_FIELD_REQUEST_PATH_PRESENT 0x00000100UL
#define KSWORD_ARK_FILE_INFO_FIELD_DEVICE_OBJECT_PRESENT 0x00000200UL
#define KSWORD_ARK_FILE_INFO_FIELD_VPB_PRESENT 0x00000400UL
#define KSWORD_ARK_FILE_INFO_FIELD_FS_CONTEXT_PRESENT 0x00000800UL
#define KSWORD_ARK_FILE_INFO_FIELD_SHARE_ACCESS_PRESENT 0x00001000UL
#define KSWORD_ARK_FILE_INFO_FIELD_SHARED_CACHE_MAP_PRESENT 0x00002000UL

#define KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_FILE_INFO_STATUS_OK 1UL
#define KSWORD_ARK_FILE_INFO_STATUS_PARTIAL 2UL
#define KSWORD_ARK_FILE_INFO_STATUS_OPEN_FAILED 3UL
#define KSWORD_ARK_FILE_INFO_STATUS_BASIC_FAILED 4UL
#define KSWORD_ARK_FILE_INFO_STATUS_STANDARD_FAILED 5UL
#define KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED 6UL
#define KSWORD_ARK_FILE_INFO_STATUS_NAME_FAILED 7UL

#define KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS 1024U
#define KSWORD_ARK_FILE_INFO_OBJECT_NAME_MAX_CHARS 1024U
#define KSWORD_ARK_FILE_INFO_DEVICE_NAME_MAX_CHARS 512U
#define KSWORD_ARK_FILE_INFO_VOLUME_LABEL_MAX_CHARS 64U

typedef struct _KSWORD_ARK_DELETE_PATH_REQUEST
{
    unsigned long flags;
    unsigned short pathLengthChars;
    unsigned short reserved;
    wchar_t path[KSWORD_ARK_DELETE_PATH_MAX_CHARS];
} KSWORD_ARK_DELETE_PATH_REQUEST;

typedef struct _KSWORD_ARK_QUERY_FILE_INFO_REQUEST
{
    unsigned long flags;
    unsigned short pathLengthChars;
    unsigned short reserved;
    wchar_t path[KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS];
} KSWORD_ARK_QUERY_FILE_INFO_REQUEST;

typedef struct _KSWORD_ARK_QUERY_FILE_INFO_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long fieldFlags;
    unsigned long queryStatus;
    long openStatus;
    long basicStatus;
    long standardStatus;
    long objectStatus;
    long nameStatus;
    unsigned long fileAttributes;
    unsigned long reserved0;
    unsigned long reserved1;
    long long allocationSize;
    long long endOfFile;
    long long creationTime;
    long long lastAccessTime;
    long long lastWriteTime;
    long long changeTime;
    unsigned long long fileObjectAddress;
    unsigned long long deviceObjectAddress;
    unsigned long long vpbAddress;
    unsigned long long fsContextAddress;
    unsigned long long fsContext2Address;
    unsigned long long sectionObjectPointersAddress;
    unsigned long long dataSectionObjectAddress;
    unsigned long long imageSectionObjectAddress;
    unsigned long long sharedCacheMapAddress;
    unsigned long deletePending;
    unsigned long readAccess;
    unsigned long writeAccess;
    unsigned long deleteAccess;
    unsigned long sharedRead;
    unsigned long sharedWrite;
    unsigned long sharedDelete;
    unsigned long vpbFlags;
    unsigned long vpbSerialNumber;
    unsigned long reserved2;
    wchar_t ntPath[KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS];
    wchar_t objectName[KSWORD_ARK_FILE_INFO_OBJECT_NAME_MAX_CHARS];
    wchar_t deviceName[KSWORD_ARK_FILE_INFO_DEVICE_NAME_MAX_CHARS];
    wchar_t volumeLabel[KSWORD_ARK_FILE_INFO_VOLUME_LABEL_MAX_CHARS];
} KSWORD_ARK_QUERY_FILE_INFO_RESPONSE;
