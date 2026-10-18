/*++

Module Name:

    page_state_table.c

Abstract:

    Fixed-capacity page-keyed table with lock-free exception-path lookups.

Environment:

    Kernel mode.  Lookup is nonpaged and performs no allocation or waiting.

--*/

#include "page_state_table.h"

static ULONG
KswRxpfPageTableHash(
    _In_ ULONGLONG PageBase
    )
{
    ULONGLONG pageNumber = PageBase >> PAGE_SHIFT;

    /* Fold high address bits before applying the power-of-two table mask. */
    pageNumber ^= pageNumber >> 17;
    pageNumber ^= pageNumber >> 31;
    return (ULONG)pageNumber & KSW_RXPF_PAGE_TABLE_MASK;
}

VOID
KswRxpfPageTableInitialize(
    _Out_ PKSW_RXPF_PAGE_TABLE Table
    )
{
    /* Publish a completely empty table before accepting any control request. */
    RtlZeroMemory(Table, sizeof(*Table));
    ExInitializePushLock(&Table->ControlLock);
    Table->NextRecordId = 0;
    InterlockedExchange(&Table->Accepting, 1);
}

VOID
KswRxpfPageTableStopAccepting(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table
    )
{
    /* Prevent all future insertions before records enter termination. */
    InterlockedExchange(&Table->Accepting, 0);
    KeMemoryBarrier();
}

VOID
KswRxpfPageTableAcquireExclusive(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table
    )
{
    /* Control-path writers may wait; vector 14 never calls this routine. */
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Table->ControlLock);
}

VOID
KswRxpfPageTableReleaseExclusive(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table
    )
{
    /* Release the writer lock before restoring normal kernel APC delivery. */
    ExReleasePushLockExclusive(&Table->ControlLock);
    KeLeaveCriticalRegion();
}

NTSTATUS
KswRxpfPageTableInsertLocked(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table,
    _In_ const KSW_RXPF_PAGE_RECORD* Source,
    _Outptr_ PKSW_RXPF_PAGE_RECORD* RecordOut
    )
{
    ULONG startIndex = 0UL;
    ULONG probe = 0UL;
    PKSW_RXPF_PAGE_RECORD firstTombstone = NULL;

    /* Reject invalid contracts before inspecting fixed table slots. */
    if (Table == NULL || Source == NULL || RecordOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *RecordOut = NULL;

    /* Insertions stop permanently once unload begins. */
    if (InterlockedCompareExchange(&Table->Accepting, 1, 1) == 0) {
        return STATUS_DELETE_PENDING;
    }
    if ((ULONGLONG)Source->PageBase <= KSW_RXPF_PAGE_TOMBSTONE ||
        (((ULONGLONG)Source->PageBase) & (PAGE_SIZE - 1ULL)) != 0ULL) {
        return STATUS_DATATYPE_MISALIGNMENT;
    }

    /* Probe the complete bounded open-addressing sequence. */
    startIndex = KswRxpfPageTableHash((ULONGLONG)Source->PageBase);
    for (probe = 0UL; probe < KSW_RXPF_PAGE_TABLE_CAPACITY; ++probe) {
        PKSW_RXPF_PAGE_RECORD slot =
            &Table->Slots[(startIndex + probe) & KSW_RXPF_PAGE_TABLE_MASK];
        ULONGLONG observedPage = (ULONGLONG)slot->PageBase;

        /* Duplicate virtual-page keys are never ambiguous. */
        if (observedPage == (ULONGLONG)Source->PageBase) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        /* Remember the first reusable tombstone but continue duplicate checks. */
        if (observedPage == KSW_RXPF_PAGE_TOMBSTONE &&
            firstTombstone == NULL) {
            firstTombstone = slot;
            continue;
        }
        /* An empty slot terminates the probe chain and can accept the record. */
        if (observedPage == 0ULL) {
            PKSW_RXPF_PAGE_RECORD destination =
                firstTombstone != NULL ? firstTombstone : slot;
            KSW_RXPF_PAGE_RECORD unpublished;
            LONG64 recordId = InterlockedIncrement64(&Table->NextRecordId);

            /* Copy unpublished fields while PageBase remains empty/tombstone. */
            RtlCopyMemory(&unpublished, Source, sizeof(unpublished));
            unpublished.PageBase = destination->PageBase;
            unpublished.RecordId = recordId;
            unpublished.ReferenceCount = 1;
            unpublished.EmulationEnabled = 0;
            unpublished.State = KSWORD_ARK_RXPF_PAGE_STATE_RX;
            RtlCopyMemory(destination, &unpublished, sizeof(*destination));
            KeMemoryBarrier();
            InterlockedExchange64(
                &destination->PageBase,
                Source->PageBase);
            InterlockedIncrement(&Table->RegisteredCount);
            *RecordOut = destination;
            return STATUS_SUCCESS;
        }
    }

    /* A table containing only occupied/tombstone slots may reuse a tombstone. */
    if (firstTombstone != NULL) {
        KSW_RXPF_PAGE_RECORD unpublished;
        LONG64 recordId = InterlockedIncrement64(&Table->NextRecordId);

        /* Publish immutable record fields before restoring the page key. */
        RtlCopyMemory(&unpublished, Source, sizeof(unpublished));
        unpublished.PageBase = firstTombstone->PageBase;
        unpublished.RecordId = recordId;
        unpublished.ReferenceCount = 1;
        unpublished.EmulationEnabled = 0;
        unpublished.State = KSWORD_ARK_RXPF_PAGE_STATE_RX;
        RtlCopyMemory(firstTombstone, &unpublished, sizeof(*firstTombstone));
        KeMemoryBarrier();
        InterlockedExchange64(
            &firstTombstone->PageBase,
            Source->PageBase);
        InterlockedIncrement(&Table->RegisteredCount);
        *RecordOut = firstTombstone;
        return STATUS_SUCCESS;
    }

    /* Fixed capacity is intentional so the exception path never allocates. */
    return STATUS_INSUFFICIENT_RESOURCES;
}

PKSW_RXPF_PAGE_RECORD
KswRxpfPageTableFindByIdLocked(
    _In_ PKSW_RXPF_PAGE_TABLE Table,
    _In_ ULONGLONG RecordId
    )
{
    ULONG index = 0UL;

    /* Record identifiers start at one and never alias an empty slot. */
    if (Table == NULL || RecordId == 0ULL) {
        return NULL;
    }
    for (index = 0UL; index < KSW_RXPF_PAGE_TABLE_CAPACITY; ++index) {
        PKSW_RXPF_PAGE_RECORD slot = &Table->Slots[index];

        /* Return only a published, non-terminating record. */
        if ((ULONGLONG)slot->PageBase > KSW_RXPF_PAGE_TOMBSTONE &&
            (ULONGLONG)slot->RecordId == RecordId &&
            slot->State != KSWORD_ARK_RXPF_PAGE_STATE_TERMINATING) {
            return slot;
        }
    }
    return NULL;
}

PKSW_RXPF_PAGE_RECORD
KswRxpfPageTableLookupFault(
    _In_ PKSW_RXPF_PAGE_TABLE Table,
    _In_ ULONGLONG PageBase
    )
{
    ULONG startIndex = 0UL;
    ULONG probe = 0UL;

    /* Reject malformed keys without touching shared slots. */
    if (Table == NULL ||
        PageBase <= KSW_RXPF_PAGE_TOMBSTONE ||
        (PageBase & (PAGE_SIZE - 1ULL)) != 0ULL) {
        return NULL;
    }

    /* Readers use immutable page keys and acquire publication via a barrier. */
    startIndex = KswRxpfPageTableHash(PageBase);
    for (probe = 0UL; probe < KSW_RXPF_PAGE_TABLE_CAPACITY; ++probe) {
        PKSW_RXPF_PAGE_RECORD slot =
            &Table->Slots[(startIndex + probe) & KSW_RXPF_PAGE_TABLE_MASK];
        ULONGLONG observedPage = (ULONGLONG)slot->PageBase;

        /* Empty terminates this open-addressing chain; tombstone does not. */
        if (observedPage == 0ULL) {
            return NULL;
        }
        if (observedPage != PageBase) {
            continue;
        }
        KeMemoryBarrier();
        /* Only a fully transitioned and explicitly enabled page is usable. */
        if (slot->State == KSWORD_ARK_RXPF_PAGE_STATE_RW_NX &&
            InterlockedCompareExchange(
                &slot->EmulationEnabled,
                1,
                1) != 0) {
            return slot;
        }
        return NULL;
    }
    return NULL;
}

VOID
KswRxpfPageTableBeginRemoveLocked(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table,
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    )
{
    LONG wasEnabled = 0;

    /* Unpublish eligibility before the caller waits for active handlers. */
    wasEnabled = InterlockedExchange(&Record->EmulationEnabled, 0);
    InterlockedExchange(
        &Record->State,
        KSWORD_ARK_RXPF_PAGE_STATE_TERMINATING);
    KeMemoryBarrier();
    if (wasEnabled != 0) {
        InterlockedDecrement(&Table->EnabledCount);
    }
}

VOID
KswRxpfPageTableClearRemovedLocked(
    _Inout_ PKSW_RXPF_PAGE_TABLE Table,
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    )
{
    /* Replace the key with a tombstone before clearing private resources. */
    InterlockedExchange64(
        &Record->PageBase,
        (LONG64)KSW_RXPF_PAGE_TOMBSTONE);
    KeMemoryBarrier();
    RtlZeroMemory(
        (PUCHAR)Record + sizeof(Record->State) +
            sizeof(Record->EmulationEnabled),
        FIELD_OFFSET(KSW_RXPF_PAGE_RECORD, PageBase) -
            sizeof(Record->State) - sizeof(Record->EmulationEnabled));
    RtlZeroMemory(
        (PUCHAR)Record + FIELD_OFFSET(KSW_RXPF_PAGE_RECORD, PageBase) +
            sizeof(Record->PageBase),
        sizeof(*Record) - FIELD_OFFSET(KSW_RXPF_PAGE_RECORD, PageBase) -
            sizeof(Record->PageBase));
    Record->State = KSWORD_ARK_RXPF_PAGE_STATE_EMPTY;
    Record->EmulationEnabled = 0;
    InterlockedDecrement(&Table->RegisteredCount);
}
