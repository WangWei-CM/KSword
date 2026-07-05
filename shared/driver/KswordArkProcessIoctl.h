#pragma once

// ============================================================
// KswordArkProcessIoctl.h
// Purpose:
// - Shared IOCTL code and request struct for R3 <-> R0 process actions.
// - This file must be included by both user mode and kernel mode.
// ============================================================

#if defined(_WIN32) && !defined(_KERNEL_MODE) && !defined(_NTDDK_) && !defined(_NTIFS_)
// User-mode translation units may include this shared protocol header before
// any Windows SDK I/O-control header.  Pull in windows.h for SDK base types and
// winioctl.h for CTL_CODE/access-mask macros first; the fallback definitions
// below remain reserved for unusual minimal include environments.
#include <windows.h>
#include <winioctl.h>
#endif

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
#define KSWORD_ARK_IOCTL_FUNCTION_SET_PROCESS_VISIBILITY 0x822UL
#define KSWORD_ARK_IOCTL_FUNCTION_SET_PROCESS_SPECIAL_FLAGS 0x824UL
#define KSWORD_ARK_IOCTL_FUNCTION_DKOM_PROCESS 0x825UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_CROSSVIEW 0x836UL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_DETAIL 0x83CUL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_RUNTIME_FIELDS 0x83EUL

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
#define KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI 0x00000004UL
// CID-table specific evidence flags.
// Inputs:
// - CID_TABLE_ENUMERATED marks rows observed directly through PspCidTable.
// - CID_TABLE_REFERENCE_FAILED marks rows whose CID slot decoded to a process
//   object type, but R0 could not take a stable reference for detail sampling.
// - TERMINATING_OR_EXITED marks rows whose EPROCESS.ObjectTable is already NULL.
// Processing:
// - R3 should still display these rows because the CID table evidence exists.
// Return behavior:
// - These flags are display/diagnostic hints only; the row PID remains the CID
//   table value so R0 actions can still attempt object-based resolution.
#define KSWORD_ARK_PROCESS_FLAG_CID_TABLE_ENUMERATED       0x00000008UL
#define KSWORD_ARK_PROCESS_FLAG_CID_TABLE_REFERENCE_FAILED 0x00000010UL
#define KSWORD_ARK_PROCESS_FLAG_TERMINATING_OR_EXITED      0x00000020UL

#define KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE 1UL
#define KSWORD_ARK_PROCESS_VISIBILITY_ACTION_UNHIDE 2UL
#define KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL 3UL

// Process visibility flags select the exact reversible R0 operation.
// Inputs:
// - PATCH_UNIQUE_PID changes _EPROCESS.UniqueProcessId to a Ksword-tagged PID.
// - UNLINK_ACTIVE_LIST removes _EPROCESS.ActiveProcessLinks from the active list.
// Processing:
// - HIDE accepts either flag or both flags; flags==0 keeps the legacy combined mode
//   for old R3 clients.
// Return behavior:
// - The fixed response status remains KSWORD_ARK_PROCESS_VISIBILITY_STATUS_*.
#define KSWORD_ARK_PROCESS_VISIBILITY_FLAG_PATCH_UNIQUE_PID 0x00000001UL
#define KSWORD_ARK_PROCESS_VISIBILITY_FLAG_UNLINK_ACTIVE_LIST 0x00000002UL
#define KSWORD_ARK_PROCESS_VISIBILITY_FLAG_LEGACY_BOTH \
    (KSWORD_ARK_PROCESS_VISIBILITY_FLAG_PATCH_UNIQUE_PID | \
     KSWORD_ARK_PROCESS_VISIBILITY_FLAG_UNLINK_ACTIVE_LIST)

#define KSWORD_ARK_PROCESS_VISIBILITY_STATUS_UNKNOWN 0UL
#define KSWORD_ARK_PROCESS_VISIBILITY_STATUS_VISIBLE 1UL
#define KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN 2UL
#define KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED 3UL

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
#define KSWORD_ARK_PROCESS_FIELD_SOURCE_PDB_PROFILE             4UL

// R0 status summarizes how complete the extended row is for UI presentation.
#define KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE     0UL
#define KSWORD_ARK_PROCESS_R0_STATUS_OK              1UL
#define KSWORD_ARK_PROCESS_R0_STATUS_PARTIAL         2UL
#define KSWORD_ARK_PROCESS_R0_STATUS_DYNDATA_MISSING 3UL
#define KSWORD_ARK_PROCESS_R0_STATUS_READ_FAILED     4UL

#define KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS 260U

// Cross-view source bits are shared by the process and thread protocols so R3
// can render one evidence column regardless of row type.
#define KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK 0x00000001UL
#define KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST 0x00000002UL
#define KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE   0x00000004UL
#define KSWORD_ARK_CROSSVIEW_SOURCE_THREAD_LIST 0x00000008UL

// Cross-view anomaly bits are intentionally evidence-only. The R0 collectors
// never repair, hide, unlink, clear, kill, or otherwise mutate target objects.
#define KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY                     0x00000001UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY                  0x00000002UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST     0x00000004UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE       0x00000008UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN                0x00000010UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST   0x00000020UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE 0x00000040UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT              0x00000080UL
#define KSWORD_ARK_CROSSVIEW_ANOMALY_PID_FIELD_MISMATCH           0x00000100UL

#define KSWORD_ARK_CROSSVIEW_STATUS_UNKNOWN            0UL
#define KSWORD_ARK_CROSSVIEW_STATUS_OK                 1UL
#define KSWORD_ARK_CROSSVIEW_STATUS_PARTIAL            2UL
#define KSWORD_ARK_CROSSVIEW_STATUS_CAPABILITY_MISSING 3UL
#define KSWORD_ARK_CROSSVIEW_STATUS_READ_FAILED        4UL

// Cross-view detail statuses describe the row-level collector outcome after
// capability gating, guarded reads, and source merge checks.
#define KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_UNKNOWN       0UL
#define KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_OK            1UL
#define KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_PARTIAL       2UL
#define KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_UNSUPPORTED   3UL
#define KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_READ_FAILED   4UL
#define KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_DATA_MISMATCH 5UL

// Cross-view denoise flags make transient or partial evidence explicit without
// guessing undocumented terminating fields when the PDB profile lacks them.
#define KSWORD_ARK_CROSSVIEW_DENOISE_PARTIAL_EVIDENCE       0x00000001UL
#define KSWORD_ARK_CROSSVIEW_DENOISE_READ_FAILURE           0x00000002UL
#define KSWORD_ARK_CROSSVIEW_DENOISE_REFERENCE_FAILURE      0x00000004UL
#define KSWORD_ARK_CROSSVIEW_DENOISE_POSSIBLE_TERMINATING   0x00000008UL
#define KSWORD_ARK_CROSSVIEW_DENOISE_UNSUPPORTED_PDB_FIELD  0x00000010UL

// Cross-view field provenance values mirror DynData item sources so R3 can show
// whether offsets came from PDB, runtime pattern resolution, or were missing.
#define KSWORD_ARK_CROSSVIEW_FIELD_SOURCE_UNAVAILABLE     0UL
#define KSWORD_ARK_CROSSVIEW_FIELD_SOURCE_SYSTEM_INFORMER 1UL
#define KSWORD_ARK_CROSSVIEW_FIELD_SOURCE_RUNTIME_PATTERN 2UL
#define KSWORD_ARK_CROSSVIEW_FIELD_SOURCE_EXTRA_TABLE     3UL
#define KSWORD_ARK_CROSSVIEW_FIELD_SOURCE_PDB_PROFILE     4UL

#define KSWORD_ARK_CROSSVIEW_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_CROSSVIEW_DETAIL_CHARS 96U
#define KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES 8192UL
#define KSWORD_ARK_CROSSVIEW_HARD_MAX_NODES 16384UL

#define KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_PUBLIC_WALK 0x00000001UL
#define KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_ACTIVE_LIST 0x00000002UL
#define KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_CID_TABLE   0x00000004UL
#define KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_PUBLIC_WALK | \
     KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_ACTIVE_LIST | \
     KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_CID_TABLE)

typedef struct _KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS
{
    unsigned long epUniqueProcessId;
    unsigned long epActiveProcessLinks;
    unsigned long epThreadListHead;
    unsigned long epImageFileName;
    unsigned long etCid;
    unsigned long etThreadListEntry;
    unsigned long etStartAddress;
    unsigned long etWin32StartAddress;
    unsigned long ktProcess;
    unsigned long htTableCode;
    unsigned long hteLowValue;
    unsigned long pspCidTableRva;
    unsigned long long pspCidTableAddress;
    unsigned long reserved;
    unsigned long epUniqueProcessIdSource;
    unsigned long epActiveProcessLinksSource;
    unsigned long epThreadListHeadSource;
    unsigned long epImageFileNameSource;
    unsigned long etCidSource;
    unsigned long etThreadListEntrySource;
    unsigned long etStartAddressSource;
    unsigned long etWin32StartAddressSource;
    unsigned long ktProcessSource;
    unsigned long htTableCodeSource;
    unsigned long hteLowValueSource;
    unsigned long pspCidTableSource;
} KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS;

typedef struct _KSWORD_ARK_PROCESS_CROSSVIEW_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long startPid;
    unsigned long endPid;
    unsigned long maxNodes;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long reserved2;
} KSWORD_ARK_PROCESS_CROSSVIEW_REQUEST;

typedef struct _KSWORD_ARK_PROCESS_CROSSVIEW_ROW
{
    unsigned long long objectAddress;
    unsigned long long startAddress;
    unsigned long processId;
    unsigned long parentProcessId;
    unsigned long sourceMask;
    unsigned long anomalyFlags;
    unsigned long long dynDataCapabilityMask;
    KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS fieldOffsets;
    long lastStatus;
    unsigned long confidence;
    char imageName[16];
    char detail[KSWORD_ARK_CROSSVIEW_DETAIL_CHARS];
    unsigned long publicProcessId;
    unsigned long activeListProcessId;
    unsigned long cidTableProcessId;
    long publicWalkStatus;
    long activeListStatus;
    long cidTableStatus;
    unsigned long detailStatus;
    unsigned long denoiseFlags;
} KSWORD_ARK_PROCESS_CROSSVIEW_ROW;

typedef struct _KSWORD_ARK_PROCESS_CROSSVIEW_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long reserved;
    unsigned long long dynDataCapabilityMask;
    unsigned long long missingCapabilityMask;
    long lastStatus;
    unsigned long reserved2;
    KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS fieldOffsets;
    KSWORD_ARK_PROCESS_CROSSVIEW_ROW entries[1];
} KSWORD_ARK_PROCESS_CROSSVIEW_RESPONSE;

#define IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_CROSSVIEW, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

// 进程运行时详情协议：
// - 输入：只接受 PID 和展示 flags，不接受 R3 传入的 EPROCESS 地址；
// - 处理：R0 通过 PsLookupProcessByProcessId 定位对象，再按 DynData/PDB 偏移只读采样；
// - 输出：固定响应包，字段缺失时用 fieldFlags/missingCapabilityMask/detail 解释原因。
#define IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_DETAIL, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_PROCESS_RUNTIME_FIELDS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_RUNTIME_FIELDS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_RUNTIME_DETAIL_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS 256U
#define KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS 16U

// 通用 runtime field sample 协议：
// - 输入：R3 只提交 PDB deep JSON 中的 runtimeItemId、offset、size；
// - 处理：R0 仅从自身 lookup/reference 得到的 EPROCESS/ETHREAD 基址读取小字段；
// - 返回：每个字段的状态、原始小字节和值摘要，不接受也不回写任意 R3 内核指针。
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_PROTOCOL_VERSION 1UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_ITEMS 64UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_VALUE_BYTES 16UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_OFFSET 0x8000UL

#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_UNKNOWN         0UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_OK              1UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_PARTIAL         2UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_LOOKUP_FAILED   3UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_INVALID_REQUEST 4UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_TRUNCATED       5UL

#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_UNKNOWN         0UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_OK              1UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_OFFSET_REJECTED 2UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_SIZE_REJECTED   3UL
#define KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_READ_FAILED     4UL

typedef struct _KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST
{
    unsigned long runtimeItemId;
    unsigned long offset;
    unsigned long size;
    unsigned long flags;
} KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST;

typedef struct _KSWORD_ARK_PROCESS_RUNTIME_FIELD_SAMPLE_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long processId;
    unsigned long itemCount;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long reserved2;
    unsigned long reserved3;
    KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST items[1];
} KSWORD_ARK_PROCESS_RUNTIME_FIELD_SAMPLE_REQUEST;

typedef struct _KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW
{
    unsigned long runtimeItemId;
    unsigned long offset;
    unsigned long size;
    unsigned long status;
    unsigned long bytesRead;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long valueU64;
    unsigned char sampleBytes[KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_VALUE_BYTES];
} KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW;

typedef struct _KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long totalCount;
    unsigned long returnedCount;
    unsigned long entrySize;
    unsigned long flags;
    long lastStatus;
    unsigned long reserved;
    unsigned long long objectAddress;
    unsigned long long dynDataCapabilityMask;
    KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW entries[1];
} KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE;

#define KSWORD_ARK_DETAIL_STATUS_UNKNOWN            0UL
#define KSWORD_ARK_DETAIL_STATUS_OK                 1UL
#define KSWORD_ARK_DETAIL_STATUS_PARTIAL            2UL
#define KSWORD_ARK_DETAIL_STATUS_UNSUPPORTED        3UL
#define KSWORD_ARK_DETAIL_STATUS_LOOKUP_FAILED      4UL
#define KSWORD_ARK_DETAIL_STATUS_CAPABILITY_MISSING 5UL
#define KSWORD_ARK_DETAIL_STATUS_READ_FAILED        6UL

#define KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_PUBLIC_IDENTITY 0x00000001UL
#define KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_LIST_LINKS      0x00000002UL
#define KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_OBJECT_POINTERS 0x00000004UL
#define KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_TOKEN_FASTREF   0x00000008UL
#define KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_PROTECTION      0x00000010UL
#define KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_PUBLIC_IDENTITY | \
     KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_LIST_LINKS | \
     KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_OBJECT_POINTERS | \
     KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_TOKEN_FASTREF | \
     KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_PROTECTION)

#define KSWORD_ARK_PROCESS_DETAIL_FIELD_PUBLIC_IDENTITY       0x00000001UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_OBJECT_ADDRESS        0x00000002UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_UNIQUE_PROCESS_ID     0x00000004UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_ACTIVE_PROCESS_LINKS  0x00000008UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_THREAD_LIST_HEAD      0x00000010UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_IMAGE_FILE_NAME       0x00000020UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_TOKEN_FASTREF         0x00000040UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_OBJECT_TABLE          0x00000080UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_SECTION_OBJECT        0x00000100UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_PROTECTION            0x00000200UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_SIGNATURE_LEVEL       0x00000400UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_SECTION_SIGNATURE     0x00000800UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_OFFSET_SOURCES        0x00001000UL
#define KSWORD_ARK_PROCESS_DETAIL_FIELD_KERNEL_GLOBALS        0x00002000UL

// 运行时 detail 通用内核全局 RVA 包：
// - 输入：R0 DynData/PDB profile EX 中已校验的 GlobalRva item；
// - 处理：R0 返回 RVA、来源以及按当前 ntoskrnl imageBase 推导出的只读地址；
// - 返回：结构体本身无返回值，仅供 R3 展示 PspCidTable/模块链表等证据来源。
typedef struct _KSWORD_ARK_RUNTIME_KERNEL_GLOBALS
{
    unsigned long pspCidTableRva;
    unsigned long psLoadedModuleListRva;
    unsigned long mmUnloadedDriversRva;
    unsigned long piDdbCacheTableRva;
    unsigned long keServiceDescriptorTableShadowRva;
    unsigned long mmLastUnloadedDriverRva;
    unsigned long pspCidTableSource;
    unsigned long psLoadedModuleListSource;
    unsigned long mmUnloadedDriversSource;
    unsigned long piDdbCacheTableSource;
    unsigned long keServiceDescriptorTableShadowSource;
    unsigned long mmLastUnloadedDriverSource;
    unsigned long long pspCidTableAddress;
    unsigned long long psLoadedModuleListAddress;
    unsigned long long mmUnloadedDriversAddress;
    unsigned long long piDdbCacheTableAddress;
    unsigned long long keServiceDescriptorTableShadowAddress;
    unsigned long long mmLastUnloadedDriverAddress;
} KSWORD_ARK_RUNTIME_KERNEL_GLOBALS;

typedef struct _KSWORD_ARK_PROCESS_DETAIL_OFFSETS
{
    unsigned long epUniqueProcessId;
    unsigned long epActiveProcessLinks;
    unsigned long epThreadListHead;
    unsigned long epImageFileName;
    unsigned long epToken;
    unsigned long epObjectTable;
    unsigned long epSectionObject;
    unsigned long epProtection;
    unsigned long epSignatureLevel;
    unsigned long epSectionSignatureLevel;
} KSWORD_ARK_PROCESS_DETAIL_OFFSETS;

// 进程 detail 偏移来源包：
// - 输入：R0 当前 KSW_DYN_STATE.KernelSources；
// - 处理：与 offsets 同名一一对应，记录 System Informer/PDB profile/runtime pattern；
// - 返回：结构体本身无返回值，仅用于 UI 把 offset 解释成人可读来源。
typedef struct _KSWORD_ARK_PROCESS_DETAIL_SOURCES
{
    unsigned long epUniqueProcessId;
    unsigned long epActiveProcessLinks;
    unsigned long epThreadListHead;
    unsigned long epImageFileName;
    unsigned long epToken;
    unsigned long epObjectTable;
    unsigned long epSectionObject;
    unsigned long epProtection;
    unsigned long epSignatureLevel;
    unsigned long epSectionSignatureLevel;
} KSWORD_ARK_PROCESS_DETAIL_SOURCES;

typedef struct _KSWORD_ARK_PROCESS_DETAIL_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long processId;
    unsigned long reserved;
} KSWORD_ARK_PROCESS_DETAIL_REQUEST;

typedef struct _KSWORD_ARK_PROCESS_DETAIL_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long requestedFlags;
    unsigned long reserved;
    long lastStatus;
    unsigned long reserved2;
    unsigned long long dynDataCapabilityMask;
    unsigned long long missingCapabilityMask;
    unsigned long long processObjectAddress;
    unsigned long long uniqueProcessIdValue;
    unsigned long long activeProcessLinksFlink;
    unsigned long long activeProcessLinksBlink;
    unsigned long long threadListHeadFlink;
    unsigned long long threadListHeadBlink;
    unsigned long long tokenFastRef;
    unsigned long long tokenObjectAddress;
    unsigned long long objectTableAddress;
    unsigned long long sectionObjectAddress;
    unsigned char protection;
    unsigned char signatureLevel;
    unsigned char sectionSignatureLevel;
    unsigned char reservedByte;
    char imageName[KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS];
    KSWORD_ARK_PROCESS_DETAIL_OFFSETS offsets;
    KSWORD_ARK_PROCESS_DETAIL_SOURCES sources;
    KSWORD_ARK_RUNTIME_KERNEL_GLOBALS kernelGlobals;
    wchar_t detail[KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS];
} KSWORD_ARK_PROCESS_DETAIL_RESPONSE;

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

#define IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SET_PROCESS_VISIBILITY, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

typedef struct _KSWORD_ARK_SET_PROCESS_VISIBILITY_REQUEST
{
    unsigned long processId;
    unsigned long action;
    unsigned long flags;
    unsigned long reserved;
} KSWORD_ARK_SET_PROCESS_VISIBILITY_REQUEST;

typedef struct _KSWORD_ARK_SET_PROCESS_VISIBILITY_RESPONSE
{
    unsigned long version;
    unsigned long processId;
    unsigned long status;
    unsigned long hiddenCount;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_SET_PROCESS_VISIBILITY_RESPONSE;

#define IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SET_PROCESS_SPECIAL_FLAGS, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define KSWORD_ARK_PROCESS_SPECIAL_ACTION_ENABLE_BREAK_ON_TERMINATION  1UL
#define KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_BREAK_ON_TERMINATION 2UL
#define KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_APC_INSERTION        3UL

#define KSWORD_ARK_PROCESS_SPECIAL_FLAG_BREAK_ON_TERMINATION 0x00000001UL
#define KSWORD_ARK_PROCESS_SPECIAL_FLAG_APC_INSERT_DISABLED  0x00000002UL

#define KSWORD_ARK_PROCESS_SPECIAL_STATUS_UNKNOWN          0UL
#define KSWORD_ARK_PROCESS_SPECIAL_STATUS_APPLIED          1UL
#define KSWORD_ARK_PROCESS_SPECIAL_STATUS_PARTIAL          2UL
#define KSWORD_ARK_PROCESS_SPECIAL_STATUS_UNSUPPORTED      3UL
#define KSWORD_ARK_PROCESS_SPECIAL_STATUS_LOOKUP_FAILED    4UL
#define KSWORD_ARK_PROCESS_SPECIAL_STATUS_OPERATION_FAILED 5UL

typedef struct _KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_REQUEST
{
    unsigned long version;
    unsigned long processId;
    unsigned long action;
    unsigned long flags;
} KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_REQUEST;

typedef struct _KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_RESPONSE
{
    unsigned long version;
    unsigned long processId;
    unsigned long action;
    unsigned long status;
    unsigned long appliedFlags;
    unsigned long touchedThreadCount;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_RESPONSE;

#define IOCTL_KSWORD_ARK_DKOM_PROCESS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DKOM_PROCESS, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define KSWORD_ARK_PROCESS_DKOM_ACTION_REMOVE_FROM_PSP_CID_TABLE 1UL

#define KSWORD_ARK_PROCESS_DKOM_STATUS_UNKNOWN          0UL
#define KSWORD_ARK_PROCESS_DKOM_STATUS_REMOVED          1UL
#define KSWORD_ARK_PROCESS_DKOM_STATUS_NOT_FOUND        2UL
#define KSWORD_ARK_PROCESS_DKOM_STATUS_UNSUPPORTED      3UL
#define KSWORD_ARK_PROCESS_DKOM_STATUS_LOOKUP_FAILED    4UL
#define KSWORD_ARK_PROCESS_DKOM_STATUS_OPERATION_FAILED 5UL

typedef struct _KSWORD_ARK_DKOM_PROCESS_REQUEST
{
    unsigned long version;
    unsigned long processId;
    unsigned long action;
    unsigned long flags;
} KSWORD_ARK_DKOM_PROCESS_REQUEST;

typedef struct _KSWORD_ARK_DKOM_PROCESS_RESPONSE
{
    unsigned long version;
    unsigned long processId;
    unsigned long action;
    unsigned long status;
    unsigned long removedEntries;
    unsigned long reserved;
    long lastStatus;
    unsigned long reserved2;
    unsigned long long pspCidTableAddress;
    unsigned long long processObjectAddress;
} KSWORD_ARK_DKOM_PROCESS_RESPONSE;
