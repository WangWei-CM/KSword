/*++

Module Name:

    thread_worker_state.c

Abstract:

    DynData v4-backed _ETHREAD.ActiveExWorker classification.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "thread_worker_state.h"

VOID
KswordARKThreadMarkFailure(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ NTSTATUS Status
    )
/*++

Routine Description:

    Mark a thread response row when an attempted optional-field read fails.

Return Value:

    None.

--*/
{
    if (Entry == NULL || NT_SUCCESS(Status)) {
        return;
    }
    Entry->r0Status = KSWORD_ARK_THREAD_R0_STATUS_READ_FAILED;
}

static NTSTATUS
KswordARKThreadReadBitField(
    _In_ PETHREAD ThreadObject,
    _In_ const KSW_DYN_V4_BIT_FIELD_LAYOUT* Field,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    Read one bounded PDB-described ETHREAD bit field without assuming a fixed
    Windows structure layout.

Arguments:

    ThreadObject - Referenced ETHREAD object.
    Field - Validated v4 offset, bit range, and storage width.
    ValueOut - Receives the normalized field value.

Return Value:

    STATUS_SUCCESS or a validation/read exception status.

--*/
{
    ULONG64 storageValue = 0ULL;
    ULONG64 valueMask = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ThreadObject == NULL || Field == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!(Field->StorageBytes == 1UL ||
          Field->StorageBytes == 2UL ||
          Field->StorageBytes == 4UL ||
          Field->StorageBytes == 8UL) ||
        Field->BitCount == 0UL ||
        Field->BitCount > 64UL ||
        Field->BitOffset + Field->BitCount > Field->StorageBytes * 8UL) {
        return STATUS_DATA_ERROR;
    }

    __try {
        RtlCopyMemory(
            &storageValue,
            (PUCHAR)ThreadObject + Field->Offset,
            Field->StorageBytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    valueMask = Field->BitCount == 64UL
        ? MAXULONGLONG
        : ((1ULL << Field->BitCount) - 1ULL);
    *ValueOut = (storageValue >> Field->BitOffset) & valueMask;
    return STATUS_SUCCESS;
}

VOID
KswordARKThreadPopulateWorkerField(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ PETHREAD ThreadObject,
    _In_ const KSW_DYN_V4_BIT_FIELD_LAYOUT* ActiveExWorkerField
    )
/*++

Routine Description:

    Classify one ETHREAD with the v4 _ETHREAD.ActiveExWorker bit. A separate
    field-present bit distinguishes false from an unavailable layout/read.

Return Value:

    None.

--*/
{
    ULONG64 activeExWorker = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Entry == NULL || ThreadObject == NULL || ActiveExWorkerField == NULL) {
        return;
    }

    status = KswordARKThreadReadBitField(
        ThreadObject,
        ActiveExWorkerField,
        &activeExWorker);
    if (!NT_SUCCESS(status)) {
        KswordARKThreadMarkFailure(Entry, status);
        return;
    }

    Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_ACTIVE_EX_WORKER_PRESENT;
    if (activeExWorker != 0ULL) {
        Entry->flags |= KSWORD_ARK_THREAD_FLAG_ACTIVE_EX_WORKER;
    }
}
