#pragma once

#include <ntddk.h>

#include "driver/KswordArkRxPfIoctl.h"

EXTERN_C_START

#define KSW_RXPF_PAGE_TABLE_CAPACITY 64UL
#define KSW_RXPF_PAGE_TABLE_MASK (KSW_RXPF_PAGE_TABLE_CAPACITY - 1UL)
#define KSW_RXPF_PAGE_TOMBSTONE 1ULL

typedef struct _KSW_RXPF_PAGE_RECORD
{
    volatile LONG State;
    volatile LONG EmulationEnabled;
    ULONG TargetKind;
    ULONG Flags;
    volatile ULONG Generation;
    volatile LONG ReferenceCount;
    volatile LONG LastStatus;
    volatile ULONG LastFailureReason;
    ULONG OriginalProtection;
    ULONG CurrentProtection;
    ULONG WritableAliasProtection;
    ULONG LastWriteOffset;
    ULONG LastWriteLength;
    DECLSPEC_ALIGN(8) volatile LONG64 RecordId;
    DECLSPEC_ALIGN(8) volatile LONG64 PageBase;
    ULONGLONG WritableAlias;
    ULONGLONG Pfn;
    ULONGLONG OwnerImageBase;
    PMDL Mdl;
    PVOID OriginalMapping;
    PVOID Backup;
    PVOID CurrentContent;
    BOOLEAN OwnsMdlPages;
    BOOLEAN PagesLockedByProbe;
    BOOLEAN MappingIsAlias;
    BOOLEAN MdlLockedForWrite;
    BOOLEAN ChangeProtectionApplied; /* MmChangeImageProtection 已生效，释放前需恢复 RX。 */
    BOOLEAN SkipBackupRestoreOnRelease; /* Phase F：ReleaseRecord 跳过自动 Backup 恢复，保留 R3 写入字节直到 PTE 回 RX。 */
    UCHAR LastWriteBytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
    volatile ULONG HookAction;          /* KSWORD_ARK_RXPF_HOOK_ACTION_* */
    ULONG HookReserved;
    DECLSPEC_ALIGN(8) volatile LONG64 HookAddress;  /* exact RIP to intercept */
    ULONGLONG RedirectTarget;           /* resume RIP for REDIRECT action */
    DECLSPEC_ALIGN(8) volatile LONG64 FaultCount;
    DECLSPEC_ALIGN(8) volatile LONG64 EmulatedCount;
    DECLSPEC_ALIGN(8) volatile LONG64 UnsupportedCount;
    DECLSPEC_ALIGN(8) volatile LONG64 RedirectCount;
} KSW_RXPF_PAGE_RECORD, *PKSW_RXPF_PAGE_RECORD;

typedef struct _KSW_RXPF_PAGE_TABLE
{
    EX_PUSH_LOCK ControlLock;
    volatile LONG Accepting;
    volatile LONG RegisteredCount;
    volatile LONG EnabledCount;
    volatile LONG64 NextRecordId;
    KSW_RXPF_PAGE_RECORD Slots[KSW_RXPF_PAGE_TABLE_CAPACITY];
} KSW_RXPF_PAGE_TABLE, *PKSW_RXPF_PAGE_TABLE;

VOID
KswRxpfPageTableInitialize(
    _Out_ PKSW_RXPF_PAGE_TABLE Table
    );

VOID
KswRxpfPageTableStopAccepting(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table
    );

VOID
KswRxpfPageTableAcquireExclusive(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table
    );

VOID
KswRxpfPageTableReleaseExclusive(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table
    );

_Must_inspect_result_
NTSTATUS
KswRxpfPageTableInsertLocked(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table,
    _In_ const KSW_RXPF_PAGE_RECORD* Source,
    _Outptr_ PKSW_RXPF_PAGE_RECORD* RecordOut
    );

_Must_inspect_result_
PKSW_RXPF_PAGE_RECORD
KswRxpfPageTableFindByIdLocked(
    _In_ PKSW_RXPF_PAGE_TABLE Table,
    _In_ ULONGLONG RecordId
    );

_Must_inspect_result_
PKSW_RXPF_PAGE_RECORD
KswRxpfPageTableLookupFault(
    _In_ PKSW_RXPF_PAGE_TABLE Table,
    _In_ ULONGLONG PageBase
    );

VOID
KswRxpfPageTableBeginRemoveLocked(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table,
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    );

VOID
KswRxpfPageTableClearRemovedLocked(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table,
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    );

EXTERN_C_END
