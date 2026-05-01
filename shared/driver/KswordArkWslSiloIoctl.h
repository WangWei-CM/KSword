#pragma once

#include "KswordArkDynDataIoctl.h"

// ============================================================
// KswordArkWslSiloIoctl.h
// 作用：
// - 定义 Phase-13 WSL/Pico 与 Silo 只读诊断协议；
// - lxcore 私有字段仍由 DynData capability 门控；
// - 第一版不跨线程强制 APC，只对当前线程上下文解析 Linux PID/TID。
// ============================================================

#define KSWORD_ARK_WSL_SILO_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_WSL_SILO 0x818UL

#define IOCTL_KSWORD_ARK_QUERY_WSL_SILO \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_WSL_SILO, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_PROCESS 0x00000001UL
#define KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_THREAD  0x00000002UL
#define KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_SILO    0x00000004UL
#define KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_PROCESS | \
     KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_THREAD | \
     KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_SILO)

#define KSWORD_ARK_WSL_FIELD_LXCORE_PRESENT             0x00000001UL
#define KSWORD_ARK_WSL_FIELD_LXCORE_DYNDATA_ACTIVE      0x00000002UL
#define KSWORD_ARK_WSL_FIELD_PROCESS_SUBSYSTEM_PRESENT  0x00000004UL
#define KSWORD_ARK_WSL_FIELD_THREAD_SUBSYSTEM_PRESENT   0x00000008UL
#define KSWORD_ARK_WSL_FIELD_LINUX_PID_PRESENT          0x00000010UL
#define KSWORD_ARK_WSL_FIELD_LINUX_TID_PRESENT          0x00000020UL
#define KSWORD_ARK_WSL_FIELD_CURRENT_THREAD_CONTEXT     0x00000040UL
#define KSWORD_ARK_WSL_FIELD_SILO_ROUTINES_PRESENT      0x00000080UL

#define KSWORD_ARK_WSL_QUERY_STATUS_UNAVAILABLE          0UL
#define KSWORD_ARK_WSL_QUERY_STATUS_OK                   1UL
#define KSWORD_ARK_WSL_QUERY_STATUS_PARTIAL              2UL
#define KSWORD_ARK_WSL_QUERY_STATUS_WSL_NOT_LOADED       3UL
#define KSWORD_ARK_WSL_QUERY_STATUS_DYNDATA_MISSING      4UL
#define KSWORD_ARK_WSL_QUERY_STATUS_NOT_WSL_SUBSYSTEM    5UL
#define KSWORD_ARK_WSL_QUERY_STATUS_NOT_CURRENT_THREAD   6UL
#define KSWORD_ARK_WSL_QUERY_STATUS_READ_FAILED          7UL

#define KSWORD_ARK_WSL_SUBSYSTEM_WIN32 0UL
#define KSWORD_ARK_WSL_SUBSYSTEM_WSL   1UL
#define KSWORD_ARK_WSL_SUBSYSTEM_UNKNOWN 0xFFFFFFFFUL

typedef struct _KSWORD_ARK_QUERY_WSL_SILO_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long threadId;
    unsigned long reserved;
} KSWORD_ARK_QUERY_WSL_SILO_REQUEST;

typedef struct _KSWORD_ARK_QUERY_WSL_SILO_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long fieldFlags;
    unsigned long queryStatus;
    long processLookupStatus;
    long threadLookupStatus;
    long processSubsystemStatus;
    long threadSubsystemStatus;
    long linuxPidStatus;
    long linuxTidStatus;
    long siloStatus;
    unsigned long processId;
    unsigned long threadId;
    unsigned long processSubsystemType;
    unsigned long threadSubsystemType;
    unsigned long linuxProcessId;
    unsigned long linuxThreadId;
    unsigned long siloRoutinesMask;
    unsigned long lxPicoProcOffset;
    unsigned long lxPicoProcInfoOffset;
    unsigned long lxPicoProcInfoPidOffset;
    unsigned long lxPicoThrdInfoOffset;
    unsigned long lxPicoThrdInfoTidOffset;
    unsigned long long dynDataCapabilityMask;
    KSW_DYN_MODULE_IDENTITY_PACKET lxcore;
} KSWORD_ARK_QUERY_WSL_SILO_RESPONSE;
