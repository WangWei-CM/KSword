/*++

Module Name:

    thread_query.c

Abstract:

    Phase-3 KTHREAD stack and I/O counter enumeration.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_thread.h"

#include "ark/ark_dyndata.h"

#define KSWORD_ARK_THREAD_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_THREAD_RESPONSE) - sizeof(KSWORD_ARK_THREAD_ENTRY))

typedef PEPROCESS(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_FN)(
    _In_opt_ PEPROCESS Process
    );

typedef PETHREAD(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

static BOOLEAN
KswordARKThreadIsOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Normalize DynData offset availability before reading a KTHREAD field.

Arguments:

    Offset - Candidate offset from the DynData state.

Return Value:

    TRUE when the offset can be used, otherwise FALSE.

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static ULONG
KswordARKThreadNormalizeOffset(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Convert internal DynData sentinel values into the shared protocol sentinel.

Arguments:

    Offset - Raw offset from KSW_DYN_STATE.

Return Value:

    Usable offset or KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE.

--*/
{
    if (!KswordARKThreadIsOffsetPresent(Offset)) {
        return KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
    }

    return Offset;
}

static KSWORD_PS_GET_NEXT_PROCESS_FN
KswordARKThreadResolvePsGetNextProcess(
    VOID
    )
/*++

Routine Description:

    Resolve PsGetNextProcess dynamically so the driver can enumerate active
    processes without depending on a fixed import surface.

Arguments:

    None.

Return Value:

    Function pointer when exported; otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcess");
    return (KSWORD_PS_GET_NEXT_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKThreadResolvePsGetNextProcessThread(
    VOID
    )
/*++

Routine Description:

    Resolve PsGetNextProcessThread dynamically for per-process thread walking.

Arguments:

    None.

Return Value:

    Function pointer when exported; otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcessThread");
    return (KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static NTSTATUS
KswordARKThreadReadPointerField(
    _In_ PETHREAD ThreadObject,
    _In_ ULONG Offset,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    Safely read one pointer-sized field from KTHREAD/ETHREAD.

Arguments:

    ThreadObject - Target thread object.
    Offset - Field offset.
    ValueOut - Receives the pointer as ULONG64.

Return Value:

    STATUS_SUCCESS or the structured-exception code.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID pointerValue = NULL;

    if (ThreadObject == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKThreadIsOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        RtlCopyMemory(&pointerValue, (PUCHAR)ThreadObject + Offset, sizeof(pointerValue));
        *ValueOut = (ULONG64)(ULONG_PTR)pointerValue;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    return status;
}

static NTSTATUS
KswordARKThreadReadUlong64Field(
    _In_ PETHREAD ThreadObject,
    _In_ ULONG Offset,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    Safely read one ULONG64 field from KTHREAD/ETHREAD.

Arguments:

    ThreadObject - Target thread object.
    Offset - Field offset.
    ValueOut - Receives the integer value.

Return Value:

    STATUS_SUCCESS or the structured-exception code.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (ThreadObject == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKThreadIsOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        RtlCopyMemory(ValueOut, (PUCHAR)ThreadObject + Offset, sizeof(*ValueOut));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    return status;
}

static VOID
KswordARKThreadMarkFailure(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ NTSTATUS Status
    )
/*++

Routine Description:

    Mark an entry as read-failed when an attempted optional field read fails.

Arguments:

    Entry - Mutable thread response row.
    Status - Field read status.

Return Value:

    None.

--*/
{
    if (Entry == NULL || NT_SUCCESS(Status)) {
        return;
    }

    Entry->r0Status = KSWORD_ARK_THREAD_R0_STATUS_READ_FAILED;
}

static VOID
KswordARKThreadPopulateStackFields(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ PETHREAD ThreadObject,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Populate KTHREAD stack boundary fields when the capability is present.

Arguments:

    Entry - Mutable thread response row.
    ThreadObject - Target thread object.
    DynState - DynData snapshot.

Return Value:

    None.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Entry == NULL || ThreadObject == NULL || DynState == NULL) {
        return;
    }

    if ((DynState->CapabilityMask & KSW_CAP_THREAD_STACK_FIELDS) != KSW_CAP_THREAD_STACK_FIELDS) {
        return;
    }

    Entry->stackFieldSource = KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER;
    status = KswordARKThreadReadPointerField(ThreadObject, DynState->Kernel.KtInitialStack, &Entry->initialStack);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_INITIAL_STACK_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadPointerField(ThreadObject, DynState->Kernel.KtStackLimit, &Entry->stackLimit);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_STACK_LIMIT_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadPointerField(ThreadObject, DynState->Kernel.KtStackBase, &Entry->stackBase);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_STACK_BASE_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadPointerField(ThreadObject, DynState->Kernel.KtKernelStack, &Entry->kernelStack);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_KERNEL_STACK_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);
}

static VOID
KswordARKThreadPopulateIoFields(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ PETHREAD ThreadObject,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Populate KTHREAD I/O counter fields when the capability is present.

Arguments:

    Entry - Mutable thread response row.
    ThreadObject - Target thread object.
    DynState - DynData snapshot.

Return Value:

    None.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Entry == NULL || ThreadObject == NULL || DynState == NULL) {
        return;
    }

    if ((DynState->CapabilityMask & KSW_CAP_THREAD_IO_COUNTERS) != KSW_CAP_THREAD_IO_COUNTERS) {
        return;
    }

    Entry->ioFieldSource = KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER;
    status = KswordARKThreadReadUlong64Field(ThreadObject, DynState->Kernel.KtReadOperationCount, &Entry->readOperationCount);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_READ_OPERATION_COUNT_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadUlong64Field(ThreadObject, DynState->Kernel.KtWriteOperationCount, &Entry->writeOperationCount);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_WRITE_OPERATION_COUNT_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadUlong64Field(ThreadObject, DynState->Kernel.KtOtherOperationCount, &Entry->otherOperationCount);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_OTHER_OPERATION_COUNT_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadUlong64Field(ThreadObject, DynState->Kernel.KtReadTransferCount, &Entry->readTransferCount);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_READ_TRANSFER_COUNT_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadUlong64Field(ThreadObject, DynState->Kernel.KtWriteTransferCount, &Entry->writeTransferCount);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_WRITE_TRANSFER_COUNT_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);

    status = KswordARKThreadReadUlong64Field(ThreadObject, DynState->Kernel.KtOtherTransferCount, &Entry->otherTransferCount);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_THREAD_FIELD_OTHER_TRANSFER_COUNT_PRESENT;
    }
    KswordARKThreadMarkFailure(Entry, status);
}

static VOID
KswordARKThreadPrepareEntryOffsets(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Copy active KTHREAD offsets into the response row for UI diagnostics.

Arguments:

    Entry - Mutable thread response row.
    DynState - DynData snapshot.

Return Value:

    None.

--*/
{
    if (Entry == NULL || DynState == NULL) {
        return;
    }

    Entry->ktInitialStackOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtInitialStack);
    Entry->ktStackLimitOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtStackLimit);
    Entry->ktStackBaseOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtStackBase);
    Entry->ktKernelStackOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtKernelStack);
    Entry->ktReadOperationCountOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtReadOperationCount);
    Entry->ktWriteOperationCountOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtWriteOperationCount);
    Entry->ktOtherOperationCountOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtOtherOperationCount);
    Entry->ktReadTransferCountOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtReadTransferCount);
    Entry->ktWriteTransferCountOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtWriteTransferCount);
    Entry->ktOtherTransferCountOffset = KswordARKThreadNormalizeOffset(DynState->Kernel.KtOtherTransferCount);
    Entry->dynDataCapabilityMask = DynState->CapabilityMask;
}

static VOID
KswordARKThreadAppendEntry(
    _Inout_ KSWORD_ARK_ENUM_THREAD_RESPONSE* Response,
    _In_ size_t EntryCapacity,
    _In_ PETHREAD ThreadObject,
    _In_ ULONG ProcessId,
    _In_ ULONG RequestFlags,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Append one thread row and populate optional KTHREAD fields.

Arguments:

    Response - Mutable response header and entry array.
    EntryCapacity - Number of entries that fit in the output buffer.
    ThreadObject - Referenced thread object owned by the caller.
    ProcessId - Owner process ID.
    RequestFlags - Request flags controlling optional field groups.
    DynState - DynData snapshot.

Return Value:

    None.

--*/
{
    KSWORD_ARK_THREAD_ENTRY* entry = NULL;

    if (Response == NULL || ThreadObject == NULL || DynState == NULL) {
        return;
    }

    if (Response->totalCount != MAXULONG) {
        Response->totalCount += 1UL;
    }

    if ((size_t)Response->returnedCount >= EntryCapacity) {
        return;
    }

    entry = &Response->entries[Response->returnedCount];
    RtlZeroMemory(entry, sizeof(*entry));
    entry->threadId = HandleToULong(PsGetThreadId(ThreadObject));
    entry->processId = ProcessId;
    entry->r0Status = KSWORD_ARK_THREAD_R0_STATUS_DYNDATA_MISSING;
    entry->stackFieldSource = KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
    entry->ioFieldSource = KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
    KswordARKThreadPrepareEntryOffsets(entry, DynState);

    if ((RequestFlags & KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_STACK) != 0UL) {
        KswordARKThreadPopulateStackFields(entry, ThreadObject, DynState);
    }
    if ((RequestFlags & KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_IO) != 0UL) {
        KswordARKThreadPopulateIoFields(entry, ThreadObject, DynState);
    }

    if (entry->r0Status != KSWORD_ARK_THREAD_R0_STATUS_READ_FAILED) {
        if (entry->fieldFlags != 0UL) {
            entry->r0Status = KSWORD_ARK_THREAD_R0_STATUS_OK;
        }
        else if ((DynState->CapabilityMask & (KSW_CAP_THREAD_STACK_FIELDS | KSW_CAP_THREAD_IO_COUNTERS)) != 0ULL) {
            entry->r0Status = KSWORD_ARK_THREAD_R0_STATUS_PARTIAL;
        }
    }

    Response->returnedCount += 1UL;
}

static VOID
KswordARKThreadWalkProcessThreads(
    _Inout_ KSWORD_ARK_ENUM_THREAD_RESPONSE* Response,
    _In_ size_t EntryCapacity,
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG RequestProcessId,
    _In_ ULONG RequestFlags,
    _In_ const KSW_DYN_STATE* DynState,
    _In_ KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN PsGetNextProcessThread
    )
/*++

Routine Description:

    Walk every thread in one process via PsGetNextProcessThread.

Arguments:

    Response - Mutable response packet.
    EntryCapacity - Output entry capacity.
    ProcessObject - Target process object.
    RequestProcessId - Optional PID filter.
    RequestFlags - Optional field group flags.
    DynState - DynData snapshot.
    PsGetNextProcessThread - Resolved thread iterator.

Return Value:

    None.

--*/
{
    PETHREAD threadCursor = NULL;
    ULONG processId = 0UL;

    if (Response == NULL || ProcessObject == NULL || DynState == NULL || PsGetNextProcessThread == NULL) {
        return;
    }

    processId = HandleToULong(PsGetProcessId(ProcessObject));
    if (RequestProcessId != 0UL && processId != RequestProcessId) {
        return;
    }

    threadCursor = PsGetNextProcessThread(ProcessObject, NULL);
    while (threadCursor != NULL) {
        PETHREAD nextThread = PsGetNextProcessThread(ProcessObject, threadCursor);
        KswordARKThreadAppendEntry(Response, EntryCapacity, threadCursor, processId, RequestFlags, DynState);
        ObDereferenceObject(threadCursor);
        threadCursor = nextThread;
    }
}

NTSTATUS
KswordARKDriverEnumerateThreads(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_THREAD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Enumerate active threads and enrich them with DynData-gated KTHREAD fields.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output byte count.
    Request - Optional enumeration request.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when enumeration completed; otherwise a validation status.

--*/
{
    KSWORD_ARK_ENUM_THREAD_RESPONSE* response = NULL;
    KSWORD_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSW_DYN_STATE dynState;
    size_t entryCapacity = 0U;
    size_t totalBytesWritten = 0U;
    ULONG requestFlags = KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_ALL;
    ULONG requestProcessId = 0UL;
    PEPROCESS processCursor = NULL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_THREAD_ENUM_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    psGetNextProcess = KswordARKThreadResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKThreadResolvePsGetNextProcessThread();
    if (psGetNextProcess == NULL || psGetNextProcessThread == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (Request != NULL) {
        requestFlags = Request->flags;
        requestProcessId = Request->processId;
    }
    if ((requestFlags & KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_ALL) == 0UL) {
        requestFlags = KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_ALL;
    }

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_THREAD_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_THREAD_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_THREAD_ENTRY);
    entryCapacity =
        (OutputBufferLength - KSWORD_ARK_THREAD_ENUM_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_THREAD_ENTRY);

    processCursor = psGetNextProcess(NULL);
    while (processCursor != NULL) {
        PEPROCESS nextProcess = psGetNextProcess(processCursor);
        KswordARKThreadWalkProcessThreads(
            response,
            entryCapacity,
            processCursor,
            requestProcessId,
            requestFlags,
            &dynState,
            psGetNextProcessThread);
        ObDereferenceObject(processCursor);
        processCursor = nextProcess;
    }

    totalBytesWritten =
        KSWORD_ARK_THREAD_ENUM_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_THREAD_ENTRY));
    *BytesWrittenOut = totalBytesWritten;
    return STATUS_SUCCESS;
}
