/*++

Module Name:

    timer_dpc_query.c

Abstract:

    Read-only, DynData v4-backed KTIMER/KDPC enumeration.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../dyndata/dyndata_v4_internal.h"
#include "../../platform/pool_compat.h"

#define KSW_TIMER_DPC_POOL_TAG 'pDTK'

typedef struct _KSW_TIMER_DPC_BUILDER
{
    KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE* Response;
    SIZE_T OutputBufferLength;
    ULONG Capacity;
    ULONG MaxEntries;
    ULONG MaxEntriesPerBucket;
    ULONG64* SeenTimers;
    ULONG SeenCount;
    KSW_DYN_V4_TIMER_DPC_LAYOUT Layout;
} KSW_TIMER_DPC_BUILDER;

static BOOLEAN
KswordARKTimerDpcIsKernelAddress(
    _In_ ULONG64 Address
    )
{
#if defined(_M_AMD64) || defined(_M_X64)
    return Address >= (ULONG64)(ULONG_PTR)MmSystemRangeStart &&
        (Address >> 48U) == 0xFFFFULL;
#else
    return Address >= (ULONG64)(ULONG_PTR)MmSystemRangeStart;
#endif
}

static BOOLEAN
KswordARKTimerDpcAddAddress(
    _In_ ULONG64 Base,
    _In_ ULONG Offset,
    _Out_ ULONG64* AddressOut
    )
{
    if (AddressOut == NULL || Base > MAXULONGLONG - (ULONG64)Offset) {
        return FALSE;
    }
    *AddressOut = Base + (ULONG64)Offset;
    return KswordARKTimerDpcIsKernelAddress(*AddressOut);
}

static BOOLEAN
KswordARKTimerDpcReadMemory(
    _In_ ULONG64 Address,
    _Out_writes_bytes_(BytesToRead) PVOID Destination,
    _In_ SIZE_T BytesToRead
    )
{
    MM_COPY_ADDRESS sourceAddress;
    SIZE_T copiedBytes = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (Destination == NULL || BytesToRead == 0U ||
        !KswordARKTimerDpcIsKernelAddress(Address) ||
        Address > MAXULONGLONG - (ULONG64)(BytesToRead - 1U) ||
        !KswordARKTimerDpcIsKernelAddress(Address + (ULONG64)(BytesToRead - 1U)) ||
        KeGetCurrentIrql() > APC_LEVEL) {
        return FALSE;
    }

    RtlZeroMemory(&sourceAddress, sizeof(sourceAddress));
    sourceAddress.VirtualAddress = (PVOID)(ULONG_PTR)Address;
    __try {
        status = MmCopyMemory(
            Destination,
            sourceAddress,
            BytesToRead,
            MM_COPY_MEMORY_VIRTUAL,
            &copiedBytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }

    if (!NT_SUCCESS(status) || copiedBytes != BytesToRead) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN
KswordARKTimerDpcLayoutValid(
    _In_ const KSW_DYN_V4_TIMER_DPC_LAYOUT* Layout,
    _Out_ ULONG* BucketCountOut
    )
{
    ULONG remainingBytes = 0UL;
    ULONG bucketCount = 0UL;

    if (Layout == NULL || BucketCountOut == NULL) {
        return FALSE;
    }
    *BucketCountOut = 0UL;

    if (Layout->KprcbTimerTable == 0UL ||
        Layout->TimerTableTypeSize == 0UL ||
        Layout->TimerTableEntryTypeSize < sizeof(LIST_ENTRY) ||
        Layout->TimerTableEntryTypeSize > 0x100UL ||
        Layout->TimerTableTimerEntries >= Layout->TimerTableTypeSize ||
        Layout->TimerTableEntryEntry > Layout->TimerTableEntryTypeSize - sizeof(LIST_ENTRY) ||
        Layout->TimerTypeSize < sizeof(LIST_ENTRY) ||
        Layout->TimerTimerListEntry > Layout->TimerTypeSize - sizeof(LIST_ENTRY) ||
        Layout->TimerDueTime > Layout->TimerTypeSize - sizeof(LONGLONG) ||
        Layout->TimerDpc > Layout->TimerTypeSize - sizeof(PVOID) ||
        Layout->TimerType >= Layout->TimerTypeSize ||
        Layout->TimerPeriod > Layout->TimerTypeSize - sizeof(LONG) ||
        Layout->DpcTypeSize < sizeof(PVOID) ||
        Layout->DpcDeferredRoutine > Layout->DpcTypeSize - sizeof(PVOID) ||
        Layout->DpcDeferredContext > Layout->DpcTypeSize - sizeof(PVOID)) {
        return FALSE;
    }

    remainingBytes = Layout->TimerTableTypeSize - Layout->TimerTableTimerEntries;
    bucketCount = remainingBytes / Layout->TimerTableEntryTypeSize;
    if (bucketCount == 0UL || bucketCount > KSWORD_ARK_TIMER_DPC_MAX_BUCKETS) {
        return FALSE;
    }

    *BucketCountOut = bucketCount;
    return TRUE;
}

static BOOLEAN
KswordARKTimerDpcAlreadySeen(
    _Inout_ KSW_TIMER_DPC_BUILDER* Builder,
    _In_ ULONG64 TimerAddress
    )
{
    ULONG index = 0UL;
    for (index = 0UL; index < Builder->SeenCount; ++index) {
        if (Builder->SeenTimers[index] == TimerAddress) {
            Builder->Response->duplicateCount += 1UL;
            Builder->Response->statusFlags |= KSWORD_ARK_TIMER_DPC_STATUS_DUPLICATE_SKIPPED;
            return TRUE;
        }
    }
    if (Builder->SeenCount < Builder->MaxEntries) {
        Builder->SeenTimers[Builder->SeenCount] = TimerAddress;
        Builder->SeenCount += 1UL;
    }
    return FALSE;
}

static VOID
KswordARKTimerDpcMarkReadFailure(
    _Inout_ KSW_TIMER_DPC_BUILDER* Builder,
    _In_ BOOLEAN CorruptChain
    )
{
    Builder->Response->readFailureCount += 1UL;
    Builder->Response->statusFlags |=
        KSWORD_ARK_TIMER_DPC_STATUS_PARTIAL |
        KSWORD_ARK_TIMER_DPC_STATUS_READ_FAILED;
    if (CorruptChain) {
        Builder->Response->corruptBucketCount += 1UL;
        Builder->Response->statusFlags |= KSWORD_ARK_TIMER_DPC_STATUS_CORRUPT_CHAIN;
    }
}

static VOID
KswordARKTimerDpcEmitTimer(
    _Inout_ KSW_TIMER_DPC_BUILDER* Builder,
    _In_ USHORT ProcessorGroup,
    _In_ UCHAR ProcessorNumber,
    _In_ ULONG BucketIndex,
    _In_ ULONG64 TimerAddress
    )
{
    KSWORD_ARK_TIMER_DPC_ENTRY row;
    ULONG64 fieldAddress = 0ULL;
    ULONG64 dpcAddress = 0ULL;
    UCHAR timerType = 0U;
    BOOLEAN timerFieldsComplete = TRUE;

    RtlZeroMemory(&row, sizeof(row));
    row.processorGroup = ProcessorGroup;
    row.processorNumber = ProcessorNumber;
    row.bucketIndex = BucketIndex;
    row.timerAddress = TimerAddress;

    if (!KswordARKTimerDpcAddAddress(TimerAddress, Builder->Layout.TimerDueTime, &fieldAddress) ||
        !KswordARKTimerDpcReadMemory(fieldAddress, &row.dueTime, sizeof(row.dueTime))) {
        timerFieldsComplete = FALSE;
    }
    if (!KswordARKTimerDpcAddAddress(TimerAddress, Builder->Layout.TimerPeriod, &fieldAddress) ||
        !KswordARKTimerDpcReadMemory(fieldAddress, &row.period, sizeof(row.period))) {
        timerFieldsComplete = FALSE;
    }
    if (!KswordARKTimerDpcAddAddress(TimerAddress, Builder->Layout.TimerType, &fieldAddress) ||
        !KswordARKTimerDpcReadMemory(fieldAddress, &timerType, sizeof(timerType))) {
        timerFieldsComplete = FALSE;
    }
    row.timerType = timerType;
    if (row.period != 0) {
        row.flags |= KSWORD_ARK_TIMER_DPC_ENTRY_PERIODIC;
    }

    if (KswordARKTimerDpcAddAddress(TimerAddress, Builder->Layout.TimerDpc, &fieldAddress) &&
        KswordARKTimerDpcReadMemory(fieldAddress, &dpcAddress, sizeof(dpcAddress))) {
        row.dpcAddress = dpcAddress;
        if (dpcAddress != 0ULL) {
            row.flags |= KSWORD_ARK_TIMER_DPC_ENTRY_DPC_PRESENT;
            if (KswordARKTimerDpcIsKernelAddress(dpcAddress) &&
                KswordARKTimerDpcAddAddress(dpcAddress, Builder->Layout.DpcDeferredRoutine, &fieldAddress) &&
                KswordARKTimerDpcReadMemory(fieldAddress, &row.deferredRoutine, sizeof(row.deferredRoutine)) &&
                KswordARKTimerDpcAddAddress(dpcAddress, Builder->Layout.DpcDeferredContext, &fieldAddress) &&
                KswordARKTimerDpcReadMemory(fieldAddress, &row.deferredContext, sizeof(row.deferredContext))) {
                row.flags |= KSWORD_ARK_TIMER_DPC_ENTRY_DPC_FIELDS_PRESENT;
            }
            else {
                timerFieldsComplete = FALSE;
            }
        }
    }
    else {
        timerFieldsComplete = FALSE;
    }

    if (!timerFieldsComplete) {
        row.flags |= KSWORD_ARK_TIMER_DPC_ENTRY_READ_PARTIAL;
        Builder->Response->readFailureCount += 1UL;
        Builder->Response->statusFlags |=
            KSWORD_ARK_TIMER_DPC_STATUS_PARTIAL |
            KSWORD_ARK_TIMER_DPC_STATUS_READ_FAILED;
    }

    Builder->Response->totalCount += 1UL;
    if (Builder->Response->returnedCount < Builder->Capacity) {
        Builder->Response->entries[Builder->Response->returnedCount] = row;
        Builder->Response->returnedCount += 1UL;
    }
    else {
        Builder->Response->statusFlags |=
            KSWORD_ARK_TIMER_DPC_STATUS_PARTIAL |
            KSWORD_ARK_TIMER_DPC_STATUS_TRUNCATED;
    }
}

static BOOLEAN
KswordARKTimerDpcEnumerateBucket(
    _Inout_ KSW_TIMER_DPC_BUILDER* Builder,
    _In_ USHORT ProcessorGroup,
    _In_ UCHAR ProcessorNumber,
    _In_ ULONG BucketIndex,
    _In_ ULONG64 ListHeadAddress
    )
{
    LIST_ENTRY headLinks;
    ULONG64 currentAddress = 0ULL;
    ULONG64 expectedBackLink = ListHeadAddress;
    ULONG traversalCount = 0UL;

    RtlZeroMemory(&headLinks, sizeof(headLinks));
    if (!KswordARKTimerDpcReadMemory(ListHeadAddress, &headLinks, sizeof(headLinks))) {
        KswordARKTimerDpcMarkReadFailure(Builder, TRUE);
        return TRUE;
    }

    currentAddress = (ULONG64)(ULONG_PTR)headLinks.Flink;
    if (currentAddress == ListHeadAddress) {
        return TRUE;
    }
    if (!KswordARKTimerDpcIsKernelAddress(currentAddress)) {
        KswordARKTimerDpcMarkReadFailure(Builder, TRUE);
        return TRUE;
    }

    while (currentAddress != ListHeadAddress) {
        LIST_ENTRY currentLinks;
        ULONG64 timerAddress = 0ULL;
        ULONG64 nextAddress = 0ULL;

        if (Builder->SeenCount >= Builder->MaxEntries) {
            Builder->Response->statusFlags |=
                KSWORD_ARK_TIMER_DPC_STATUS_PARTIAL |
                KSWORD_ARK_TIMER_DPC_STATUS_TRUNCATED;
            return FALSE;
        }
        if (traversalCount >= Builder->MaxEntriesPerBucket) {
            Builder->Response->corruptBucketCount += 1UL;
            Builder->Response->statusFlags |=
                KSWORD_ARK_TIMER_DPC_STATUS_PARTIAL |
                KSWORD_ARK_TIMER_DPC_STATUS_TRUNCATED |
                KSWORD_ARK_TIMER_DPC_STATUS_CORRUPT_CHAIN;
            return TRUE;
        }

        RtlZeroMemory(&currentLinks, sizeof(currentLinks));
        if (!KswordARKTimerDpcReadMemory(currentAddress, &currentLinks, sizeof(currentLinks))) {
            KswordARKTimerDpcMarkReadFailure(Builder, TRUE);
            return TRUE;
        }
        if ((ULONG64)(ULONG_PTR)currentLinks.Blink != expectedBackLink ||
            currentAddress < (ULONG64)Builder->Layout.TimerTimerListEntry) {
            KswordARKTimerDpcMarkReadFailure(Builder, TRUE);
            return TRUE;
        }

        timerAddress = currentAddress - (ULONG64)Builder->Layout.TimerTimerListEntry;
        if (!KswordARKTimerDpcIsKernelAddress(timerAddress)) {
            KswordARKTimerDpcMarkReadFailure(Builder, TRUE);
            return TRUE;
        }
        if (!KswordARKTimerDpcAlreadySeen(Builder, timerAddress)) {
            KswordARKTimerDpcEmitTimer(
                Builder,
                ProcessorGroup,
                ProcessorNumber,
                BucketIndex,
                timerAddress);
        }

        nextAddress = (ULONG64)(ULONG_PTR)currentLinks.Flink;
        if (nextAddress != ListHeadAddress &&
            (!KswordARKTimerDpcIsKernelAddress(nextAddress) || nextAddress == currentAddress)) {
            KswordARKTimerDpcMarkReadFailure(Builder, TRUE);
            return TRUE;
        }
        expectedBackLink = currentAddress;
        currentAddress = nextAddress;
        traversalCount += 1UL;
    }
    return TRUE;
}

NTSTATUS
KswordARKDriverEnumerateTimerDpc(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_TIMER_DPC_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_ENUM_TIMER_DPC_REQUEST requestCopy;
    KSW_TIMER_DPC_BUILDER builder;
    ULONG bucketCount = 0UL;
    ULONG activeProcessorCount = 0UL;
    ULONG processorIndex = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL ||
        OutputBufferLength < KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    *BytesWrittenOut = 0U;
    RtlZeroMemory(&requestCopy, sizeof(requestCopy));
    requestCopy.version = KSWORD_ARK_TIMER_DPC_PROTOCOL_VERSION;
    requestCopy.maxEntries = KSWORD_ARK_TIMER_DPC_DEFAULT_MAX_ENTRIES;
    requestCopy.maxEntriesPerBucket = KSWORD_ARK_TIMER_DPC_DEFAULT_BUCKET_BUDGET;
    if (Request != NULL) {
        requestCopy = *Request;
    }
    if (requestCopy.version != KSWORD_ARK_TIMER_DPC_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }
    if (requestCopy.maxEntries == 0UL || requestCopy.maxEntries > KSWORD_ARK_TIMER_DPC_MAX_ENTRIES) {
        requestCopy.maxEntries = KSWORD_ARK_TIMER_DPC_DEFAULT_MAX_ENTRIES;
    }
    if (requestCopy.maxEntriesPerBucket == 0UL ||
        requestCopy.maxEntriesPerBucket > KSWORD_ARK_TIMER_DPC_MAX_BUCKET_BUDGET) {
        requestCopy.maxEntriesPerBucket = KSWORD_ARK_TIMER_DPC_DEFAULT_BUCKET_BUDGET;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    RtlZeroMemory(&builder, sizeof(builder));
    builder.Response = (KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE*)OutputBuffer;
    builder.OutputBufferLength = OutputBufferLength;
    builder.Capacity = (ULONG)((OutputBufferLength - KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_TIMER_DPC_ENTRY));
    builder.MaxEntries = requestCopy.maxEntries;
    builder.MaxEntriesPerBucket = requestCopy.maxEntriesPerBucket;
    builder.Response->version = KSWORD_ARK_TIMER_DPC_PROTOCOL_VERSION;
    builder.Response->queryStatus = KSWORD_ARK_TIMER_DPC_QUERY_STATUS_OK;
    builder.Response->entrySize = sizeof(KSWORD_ARK_TIMER_DPC_ENTRY);

#if !defined(_M_AMD64) && !defined(_M_X64)
    builder.Response->queryStatus = KSWORD_ARK_TIMER_DPC_QUERY_STATUS_NOT_SUPPORTED;
    builder.Response->lastStatus = STATUS_NOT_SUPPORTED;
    *BytesWrittenOut = KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE_HEADER_SIZE;
    return STATUS_SUCCESS;
#else
    status = KswordARKDynDataV4SnapshotTimerDpcLayout(&builder.Layout);
    if (!NT_SUCCESS(status)) {
        builder.Response->queryStatus = KSWORD_ARK_TIMER_DPC_QUERY_STATUS_DYNDATA_MISSING;
        builder.Response->statusFlags = KSWORD_ARK_TIMER_DPC_STATUS_DYNDATA_MISSING;
        builder.Response->lastStatus = status;
        *BytesWrittenOut = KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    if (!KswordARKTimerDpcLayoutValid(&builder.Layout, &bucketCount)) {
        builder.Response->queryStatus = KSWORD_ARK_TIMER_DPC_QUERY_STATUS_INVALID_LAYOUT;
        builder.Response->lastStatus = STATUS_DATA_ERROR;
        *BytesWrittenOut = KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    builder.Response->bucketCount = bucketCount;

    builder.SeenTimers = (ULONG64*)KswordARKAllocateNonPagedPool(
        (SIZE_T)builder.MaxEntries * sizeof(ULONG64),
        KSW_TIMER_DPC_POOL_TAG);
    if (builder.SeenTimers == NULL) {
        builder.Response->queryStatus = KSWORD_ARK_TIMER_DPC_QUERY_STATUS_PARTIAL;
        builder.Response->statusFlags = KSWORD_ARK_TIMER_DPC_STATUS_PARTIAL;
        builder.Response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        *BytesWrittenOut = KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(builder.SeenTimers, (SIZE_T)builder.MaxEntries * sizeof(ULONG64));

    activeProcessorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    for (processorIndex = 0UL; processorIndex < activeProcessorCount; ++processorIndex) {
        PROCESSOR_NUMBER processorNumber;
        GROUP_AFFINITY targetAffinity;
        GROUP_AFFINITY previousAffinity;
        ULONG64 prcbAddress = 0ULL;
        ULONG64 timerTableAddress = 0ULL;
        ULONG bucketIndex = 0UL;
        BOOLEAN continueProcessors = TRUE;

        RtlZeroMemory(&processorNumber, sizeof(processorNumber));
        if (!NT_SUCCESS(KeGetProcessorNumberFromIndex(processorIndex, &processorNumber)) ||
            processorNumber.Number >= (sizeof(KAFFINITY) * 8UL)) {
            KswordARKTimerDpcMarkReadFailure(&builder, FALSE);
            continue;
        }

        RtlZeroMemory(&targetAffinity, sizeof(targetAffinity));
        RtlZeroMemory(&previousAffinity, sizeof(previousAffinity));
        targetAffinity.Group = processorNumber.Group;
        targetAffinity.Mask = ((KAFFINITY)1) << processorNumber.Number;
        KeSetSystemGroupAffinityThread(&targetAffinity, &previousAffinity);
        __try {
            prcbAddress = (ULONG64)(ULONG_PTR)KeGetPcr()->CurrentPrcb;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            prcbAddress = 0ULL;
        }

        if (!KswordARKTimerDpcIsKernelAddress(prcbAddress) ||
            !KswordARKTimerDpcAddAddress(prcbAddress, builder.Layout.KprcbTimerTable, &timerTableAddress)) {
            KswordARKTimerDpcMarkReadFailure(&builder, FALSE);
        }
        else {
            builder.Response->processorCount += 1UL;
            for (bucketIndex = 0UL; bucketIndex < bucketCount; ++bucketIndex) {
                ULONG64 bucketOffset =
                    (ULONG64)builder.Layout.TimerTableTimerEntries +
                    ((ULONG64)bucketIndex * (ULONG64)builder.Layout.TimerTableEntryTypeSize) +
                    (ULONG64)builder.Layout.TimerTableEntryEntry;
                ULONG64 listHeadAddress = 0ULL;

                if (bucketOffset > MAXULONG ||
                    !KswordARKTimerDpcAddAddress(timerTableAddress, (ULONG)bucketOffset, &listHeadAddress)) {
                    KswordARKTimerDpcMarkReadFailure(&builder, TRUE);
                    break;
                }
                builder.Response->bucketsVisited += 1UL;
                continueProcessors = KswordARKTimerDpcEnumerateBucket(
                    &builder,
                    processorNumber.Group,
                    processorNumber.Number,
                    bucketIndex,
                    listHeadAddress);
                if (!continueProcessors) {
                    break;
                }
            }
        }
        KeRevertToUserGroupAffinityThread(&previousAffinity);
        if (!continueProcessors) {
            break;
        }
    }

    ExFreePoolWithTag(builder.SeenTimers, KSW_TIMER_DPC_POOL_TAG);
    builder.SeenTimers = NULL;
    if (builder.Response->statusFlags != 0UL) {
        builder.Response->queryStatus = KSWORD_ARK_TIMER_DPC_QUERY_STATUS_PARTIAL;
    }
    builder.Response->lastStatus = STATUS_SUCCESS;
    *BytesWrittenOut = KSWORD_ARK_ENUM_TIMER_DPC_RESPONSE_HEADER_SIZE +
        ((SIZE_T)builder.Response->returnedCount * sizeof(KSWORD_ARK_TIMER_DPC_ENTRY));
    return STATUS_SUCCESS;
#endif
}
