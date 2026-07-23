#pragma once

#include "ark/ark_dyndata.h"

EXTERN_C_START

// Per-capability-group v4 state. The public row is stored directly so query
// IOCTLs can copy stable coverage counters without recomputing them.
typedef struct _KSW_DYN_V4_GROUP_STATE
{
    KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY PublicEntry;
} KSW_DYN_V4_GROUP_STATE;

// Per-loaded-module v4 profile state. Occupied marks whether a module profile
// has passed identity validation, StoredItemCount bounds Items, PublicEntry is
// the module status returned to R3, Groups holds derived capability coverage,
// and Items preserves the accepted compact PDB facts for future consumers.
typedef struct _KSW_DYN_V4_MODULE_STATE
{
    BOOLEAN Occupied;
    ULONG StoredItemCount;
    KSW_DYN_V4_MODULE_STATUS_ENTRY PublicEntry;
    KSW_DYN_V4_GROUP_STATE Groups[KSW_DYN_V4_MAX_CAPABILITY_GROUPS_PER_MODULE];
    KSW_DYN_V4_ITEM_PACKET Items[KSW_DYN_V4_MAX_ITEMS_PER_MODULE];
} KSW_DYN_V4_MODULE_STATE;

// Global v4 state. Modules is indexed by the compact v4 module-slot mapping,
// Missing stores bounded required/optional diagnostics, and MissingCount bounds
// the number of valid Missing rows.
typedef struct _KSW_DYN_V4_STATE
{
    KSW_DYN_V4_MODULE_STATE Modules[KSW_DYN_V4_MAX_MODULES];
    KSW_DYN_V4_MISSING_ITEM_ENTRY Missing[KSW_DYN_V4_MAX_MISSING_SUMMARY];
    ULONG MissingCount;
} KSW_DYN_V4_STATE;

extern EX_PUSH_LOCK g_KswordDynDataV4Lock;
extern KSW_DYN_V4_STATE g_KswordDynDataV4State;

// Timer/DPC 消费者只接收完成的一次性布局快照，不直接持有 v4 全局锁。
typedef struct _KSW_DYN_V4_TIMER_DPC_LAYOUT
{
    ULONG KprcbTimerTable;
    ULONG TimerTableTimerEntries;
    ULONG TimerTableEntryLock;
    ULONG TimerTableEntryEntry;
    ULONG TimerTableEntryTime;
    ULONG TimerTimerListEntry;
    ULONG TimerDueTime;
    ULONG TimerDpc;
    ULONG TimerType;
    ULONG TimerPeriod;
    ULONG DpcDeferredRoutine;
    ULONG DpcDeferredContext;
    ULONG TimerTableTypeSize;
    ULONG TimerTableEntryTypeSize;
    ULONG TimerTypeSize;
    ULONG DpcTypeSize;
} KSW_DYN_V4_TIMER_DPC_LAYOUT;

// 位域消费者使用固定快照，Offset 是对象内存偏移，BitOffset/BitCount 描述
// StorageBytes 宽度整数中的位范围。
typedef struct _KSW_DYN_V4_BIT_FIELD_LAYOUT
{
    ULONG Offset;
    ULONG BitOffset;
    ULONG BitCount;
    ULONG StorageBytes;
} KSW_DYN_V4_BIT_FIELD_LAYOUT;

NTSTATUS
KswordARKDynDataV4SnapshotTimerDpcLayout(
    _Out_ KSW_DYN_V4_TIMER_DPC_LAYOUT* LayoutOut
    );

// 线程消费者获取 _ETHREAD.ActiveExWorker 的精确 PDB 位域布局。
NTSTATUS
KswordARKDynDataV4SnapshotActiveExWorkerField(
    _Out_ KSW_DYN_V4_BIT_FIELD_LAYOUT* FieldOut
    );

EXTERN_C_END
