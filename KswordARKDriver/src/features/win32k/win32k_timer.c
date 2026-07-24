/*++

Module Name:

    win32k_timer.c

Abstract:

    Read-only enumeration of win32k tagTIMER objects through the exported
    gTimerHashTable. The collector prefers exact PE identity and can fall back
    to the nearest previous Windows layout. It never unlinks or modifies a timer.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_query.h"
#include "win32k_support.h"
#include "../../platform/pool_compat.h"

#include <ntimage.h>

#define KSWORD_ARK_WIN32K_TIMER_POOL_TAG 'mTkW'
#define KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE 0x88UL
#define KSWORD_ARK_WIN32K_TIMER_BUCKET_COUNT 64UL
#define KSWORD_ARK_WIN32K_TIMER_BUCKET_STRIDE sizeof(LIST_ENTRY)
#define KSWORD_ARK_WIN32K_TIMER_MAX_THREAD_MAP 8192UL
#define KSWORD_ARK_WIN32K_TIMER_MAX_SEEN 8192UL
#define KSWORD_ARK_WIN32K_TIMER_MAX_NODES 32768UL
#define KSWORD_ARK_WIN32K_TIMER_MAX_NODES_PER_BUCKET 8192UL

typedef struct _KSWORD_ARK_WIN32K_TIMER_PROFILE
{
    KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY identity;
    KSWORD_ARK_WIN32K_TIMER_LAYOUT layout;
} KSWORD_ARK_WIN32K_TIMER_PROFILE;

// 按 Windows 版本从旧到新维护。精确 PE 身份优先；未命中时由公共选择器
// 使用不高于当前 Windows 版本的最近表项。
static const KSWORD_ARK_WIN32K_TIMER_PROFILE g_KswordArkWin32kTimerProfiles[] =
{
    {
        // 22H2 19045 is an enablement-package version. Private win32k
        // binaries remain on the 19041 servicing branch; use that branch for
        // nearest-previous ordering so PsGetVersion/NtBuildNumber cannot make
        // the verified profile look newer than the running kernel.
        { 10UL, 0UL, 19041UL, 6456UL,
          0x8FC48444UL, 0x002D6000UL, 0x83C73BE4UL, 0x003B4000UL },
        { KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE,
          0x18UL, 0x20UL, 0x28UL, 0x2CUL, 0x30UL, 0x34UL, 0x48UL,
          0x58UL, 0x60UL, 0x68UL, 0x70UL, 0x80UL,
          KSWORD_ARK_WIN32K_TIMER_BUCKET_COUNT,
          KSWORD_ARK_WIN32K_TIMER_BUCKET_STRIDE,
          KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_UNKNOWN,
          0x83C73BE4UL, 0x003B4000UL }
    }
};

typedef KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY
    KSWORD_ARK_WIN32K_TIMER_THREAD_MAP_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_TIMER_WALK_CONTEXT
{
    KSWORD_ARK_WIN32K_TIMER_SNAPSHOT_RESPONSE* Response;
    const KSWORD_ARK_WIN32K_TIMER_LAYOUT* Layout;
    const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request;
    const KSWORD_ARK_WIN32K_TIMER_THREAD_MAP_ENTRY* ThreadMap;
    ULONG ThreadMapCount;
    ULONG RequestedSessionId;
    ULONG MaxEntries;
    ULONG64* SeenTimers;
    ULONG SeenCount;
    ULONG TraversalCount;
    BOOLEAN Stop;
} KSWORD_ARK_WIN32K_TIMER_WALK_CONTEXT;

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_z_ PCSTR RoutineName
    );

NTKERNELAPI
VOID
KeStackAttachProcess(
    _Inout_ PVOID Process,
    _Out_ PVOID ApcState
    );

NTKERNELAPI
VOID
KeUnstackDetachProcess(
    _In_ PVOID ApcState
    );

NTKERNELAPI
NTSTATUS
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

static BOOLEAN
KswordARKWin32kTimerIsKernelAddress(
    _In_ ULONG64 Address
    )
/*++

Routine Description:

    Validate that a pointer belongs to the canonical kernel virtual address
    range before passing it to MmCopyMemory.

Arguments:

    Address - Candidate pointer value.

Return Value:

    TRUE when the value is a canonical kernel address.

--*/
{
#if defined(_M_AMD64) || defined(_M_X64)
    return Address >= (ULONG64)(ULONG_PTR)MmSystemRangeStart &&
        (Address >> 48U) == 0xFFFFULL;
#else
    return Address >= (ULONG64)(ULONG_PTR)MmSystemRangeStart;
#endif
}

static BOOLEAN
KswordARKWin32kTimerReadMemory(
    _In_ ULONG64 Address,
    _Out_writes_bytes_(BytesToRead) PVOID Destination,
    _In_ SIZE_T BytesToRead
    )
/*++

Routine Description:

    Read one timer or list fragment with the shared safe kernel-memory helper.

Arguments:

    Address - Kernel virtual address to read.
    Destination - Destination buffer.
    BytesToRead - Number of bytes to read.

Return Value:

    TRUE when the complete range was copied.

--*/
{
    ULONG64 lastAddress = 0ULL;

    if (Destination == NULL || BytesToRead == 0U ||
        BytesToRead > (SIZE_T)MAXULONG ||
        !KswordARKWin32kTimerIsKernelAddress(Address) ||
        Address > MAXULONGLONG - (ULONG64)(BytesToRead - 1U)) {
        return FALSE;
    }

    lastAddress = Address + (ULONG64)(BytesToRead - 1U);
    if (!KswordARKWin32kTimerIsKernelAddress(lastAddress)) {
        return FALSE;
    }

    return KswordARKHookReadMemorySafe(
        (const VOID*)(ULONG_PTR)Address,
        Destination,
        BytesToRead);
}

static ULONG64
KswordARKWin32kTimerReadU64(
    _In_reads_bytes_(KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE) const UCHAR* TimerBytes,
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Read a pointer-sized tagTIMER field from a validated local copy.

Arguments:

    TimerBytes - Local timer object copy.
    Offset - Field offset.

Return Value:

    Field value, or zero when the offset is outside the known object.

--*/
{
    ULONG64 value = 0ULL;

    if (TimerBytes == NULL || Offset > KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE ||
        sizeof(value) > KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE - Offset) {
        return 0ULL;
    }
    RtlCopyMemory(&value, TimerBytes + Offset, sizeof(value));
    return value;
}

static ULONG
KswordARKWin32kTimerReadU32(
    _In_reads_bytes_(KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE) const UCHAR* TimerBytes,
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Read a 32-bit tagTIMER field from a validated local copy.

Arguments:

    TimerBytes - Local timer object copy.
    Offset - Field offset.

Return Value:

    Field value, or zero when the offset is outside the known object.

--*/
{
    ULONG value = 0UL;

    if (TimerBytes == NULL || Offset > KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE ||
        sizeof(value) > KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE - Offset) {
        return 0UL;
    }
    RtlCopyMemory(&value, TimerBytes + Offset, sizeof(value));
    return value;
}

static VOID
KswordARKWin32kTimerSetDetail(
    _Out_writes_(KSWORD_ARK_WIN32K_DETAIL_CHARS) PWCHAR Destination,
    _In_z_ PCWSTR Text
    )
/*++

Routine Description:

    Copy a bounded timer diagnostic detail string.

Arguments:

    Destination - Fixed protocol text field.
    Text - Constant diagnostic text.

Return Value:

    None.

--*/
{
    KswordARKWin32kCopyWideText(
        Destination,
        KSWORD_ARK_WIN32K_DETAIL_CHARS,
        Text);
}

static VOID
KswordARKWin32kTimerInitializeLayout(
    _Out_ KSWORD_ARK_WIN32K_TIMER_LAYOUT* Layout,
    _In_ const KSWORD_ARK_WIN32K_TIMER_PROFILE* Profile,
    _In_ ULONG Source
    )
/*++

Routine Description:

    Initialize the exact tagTIMER offsets verified for the supported PE pair.

Arguments:

    Layout - Receives the timer layout packet.
    Profile - Selected exact or nearest-previous layout profile.
    Source - Layout selection source.

Return Value:

    None.

--*/
{
    if (Layout == NULL || Profile == NULL) {
        return;
    }
    *Layout = Profile->layout;
    Layout->source = Source == KSWORD_ARK_WIN32K_LAYOUT_SELECTION_EXACT_IDENTITY
        ? KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY
        : KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_NEAREST_PREVIOUS;
}

static NTSTATUS
KswordARKWin32kTimerBuildThreadMap(
    _Outptr_result_buffer_(*CountOut) KSWORD_ARK_WIN32K_TIMER_THREAD_MAP_ENTRY** MapOut,
    _Out_ ULONG* CountOut,
    _Out_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    Build a bounded map from public PsGetThreadWin32Thread values to PID/TID.
    This avoids depending on private tagTHREADINFO fields for ownership.

Arguments:

    MapOut - Receives the allocated map.
    CountOut - Receives the number of valid entries.
    TruncatedOut - Receives TRUE if the hard map limit was reached.

Return Value:

    STATUS_SUCCESS or the first missing public routine/allocation failure.

--*/
{
    return KswordARKWin32kBuildGuiThreadMap(
        KSWORD_ARK_WIN32K_TIMER_MAX_THREAD_MAP,
        KSWORD_ARK_WIN32K_TIMER_POOL_TAG,
        MapOut,
        CountOut,
        TruncatedOut);
}

static NTSTATUS
KswordARKWin32kTimerReferenceSessionProcess(
    _In_reads_(ThreadMapCount) const KSWORD_ARK_WIN32K_TIMER_THREAD_MAP_ENTRY* ThreadMap,
    _In_ ULONG ThreadMapCount,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Inout_ ULONG* RequestedSessionId,
    _Outptr_ PEPROCESS* ProcessOut
    )
{
    ULONG preferredProcessId = Request != NULL ? Request->processId : 0UL;
    ULONG pass = 0UL;

    if (ThreadMap == NULL || ThreadMapCount == 0UL ||
        RequestedSessionId == NULL || ProcessOut == NULL) {
        return STATUS_NOT_FOUND;
    }
    *ProcessOut = NULL;

    for (pass = 0UL; pass < 4UL; ++pass) {
        ULONG index = 0UL;

        for (index = 0UL; index < ThreadMapCount; ++index) {
            const KSWORD_ARK_WIN32K_TIMER_THREAD_MAP_ENTRY* entry = &ThreadMap[index];
            BOOLEAN candidate = FALSE;
            PEPROCESS process = NULL;
            NTSTATUS status = STATUS_SUCCESS;

            switch (pass) {
            case 0UL:
                candidate = preferredProcessId != 0UL &&
                    entry->ProcessId == preferredProcessId &&
                    (*RequestedSessionId == 0UL || entry->SessionId == *RequestedSessionId);
                break;
            case 1UL:
                candidate = *RequestedSessionId != 0UL &&
                    entry->SessionId == *RequestedSessionId;
                break;
            case 2UL:
                candidate = *RequestedSessionId == 0UL && entry->SessionId != 0UL;
                break;
            default:
                candidate = *RequestedSessionId == 0UL;
                break;
            }
            if (!candidate) {
                continue;
            }

            status = PsLookupProcessByProcessId(
                ULongToHandle(entry->ProcessId),
                &process);
            if (NT_SUCCESS(status) && process != NULL) {
                KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId =
                    KswordARKWin32kResolvePsGetProcessSessionId();

                if (psGetProcessSessionId == NULL ||
                    psGetProcessSessionId(process) == entry->SessionId) {
                    *RequestedSessionId = entry->SessionId;
                    *ProcessOut = process;
                    return STATUS_SUCCESS;
                }
                ObDereferenceObject(process);
            }
        }
    }

    return STATUS_NOT_FOUND;
}

static BOOLEAN
KswordARKWin32kTimerLookupThread(
    _In_reads_(ThreadMapCount) const KSWORD_ARK_WIN32K_TIMER_THREAD_MAP_ENTRY* ThreadMap,
    _In_ ULONG ThreadMapCount,
    _In_ ULONG64 ThreadInfo,
    _Out_ ULONG* ProcessIdOut,
    _Out_ ULONG* ThreadIdOut,
    _Out_ ULONG* SessionIdOut
    )
/*++

Routine Description:

    Resolve one private tagTHREADINFO pointer through the public thread map.

Arguments:

    ThreadMap - Bounded map produced by the process/thread walker.
    ThreadMapCount - Number of valid map rows.
    ThreadInfo - tagTHREADINFO pointer stored in tagTIMER.
    ProcessIdOut - Receives PID.
    ThreadIdOut - Receives TID.
    SessionIdOut - Receives session id.

Return Value:

    TRUE when the pointer was matched.

--*/
{
    ULONG index = 0UL;

    if (ProcessIdOut == NULL || ThreadIdOut == NULL || SessionIdOut == NULL) {
        return FALSE;
    }
    *ProcessIdOut = 0UL;
    *ThreadIdOut = 0UL;
    *SessionIdOut = 0UL;
    if (ThreadMap == NULL || ThreadInfo == 0ULL) {
        return FALSE;
    }

    for (index = 0UL; index < ThreadMapCount; ++index) {
        if (ThreadMap[index].ThreadInfo == ThreadInfo) {
            *ProcessIdOut = ThreadMap[index].ProcessId;
            *ThreadIdOut = ThreadMap[index].ThreadId;
            *SessionIdOut = ThreadMap[index].SessionId;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
KswordARKWin32kTimerAlreadySeen(
    _In_reads_(SeenCount) const ULONG64* SeenTimers,
    _In_ ULONG SeenCount,
    _In_ ULONG64 TimerAddress
    )
/*++

Routine Description:

    Check the bounded duplicate set used while hash buckets are traversed.

Arguments:

    SeenTimers - Previously emitted timer addresses.
    SeenCount - Number of valid addresses.
    TimerAddress - Candidate timer address.

Return Value:

    TRUE if the address was already observed.

--*/
{
    ULONG index = 0UL;

    if (SeenTimers == NULL || TimerAddress == 0ULL) {
        return FALSE;
    }
    for (index = 0UL; index < SeenCount; ++index) {
        if (SeenTimers[index] == TimerAddress) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
KswordARKWin32kTimerMatchesRequest(
    _In_ const KSWORD_ARK_WIN32K_TIMER_WALK_CONTEXT* Context,
    _In_ ULONG ProcessId,
    _In_ ULONG ThreadId,
    _In_ ULONG SessionId
    )
/*++

Routine Description:

    Apply the user supplied session/PID/TID filters to one resolved timer owner.

Arguments:

    Context - Timer traversal context.
    ProcessId - Resolved owner PID.
    ThreadId - Resolved owner TID.
    SessionId - Resolved owner session.

Return Value:

    TRUE when the row belongs in the response.

--*/
{
    if (Context == NULL) {
        return FALSE;
    }
    if (Context->RequestedSessionId != 0UL &&
        Context->RequestedSessionId != SessionId) {
        return FALSE;
    }
    if (Context->Request != NULL && Context->Request->processId != 0UL &&
        Context->Request->processId != ProcessId) {
        return FALSE;
    }
    if (Context->Request != NULL && Context->Request->threadId != 0UL &&
        Context->Request->threadId != ThreadId) {
        return FALSE;
    }
    return TRUE;
}

static VOID
KswordARKWin32kTimerAppend(
    _Inout_ KSWORD_ARK_WIN32K_TIMER_WALK_CONTEXT* Context,
    _In_ ULONG64 TimerAddress,
    _In_ ULONG64 HashLinkAddress
    )
/*++

Routine Description:

    Read one tagTIMER object, resolve its owner, apply filters, and append a row.

Arguments:

    Context - Mutable traversal context and response.
    TimerAddress - Container address derived from the hash LIST_ENTRY.
    HashLinkAddress - Address of the hash LIST_ENTRY node.

Return Value:

    None. Read failures are counted and do not abort other buckets.

--*/
{
    UCHAR timerBytes[KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE];
    ULONG64 primaryThreadInfo = 0ULL;
    ULONG64 alternateThreadInfo = 0ULL;
    ULONG processId = 0UL;
    ULONG threadId = 0UL;
    ULONG sessionId = 0UL;
    BOOLEAN ownerResolved = FALSE;
    KSWORD_ARK_WIN32K_TIMER_ENTRY* entry = NULL;

    if (Context == NULL || Context->Response == NULL || Context->Layout == NULL ||
        TimerAddress == 0ULL) {
        return;
    }
    Context->Response->visitedNodeCount += 1UL;
    Context->TraversalCount += 1UL;
    if (Context->TraversalCount > KSWORD_ARK_WIN32K_TIMER_MAX_NODES) {
        Context->Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
        Context->Response->lastStatus = STATUS_BUFFER_OVERFLOW;
        Context->Stop = TRUE;
        return;
    }

    if (!KswordARKWin32kTimerIsKernelAddress(TimerAddress) ||
        !KswordARKWin32kTimerReadMemory(
            TimerAddress,
            timerBytes,
            sizeof(timerBytes))) {
        Context->Response->readFailureCount += 1UL;
        Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        Context->Response->lastStatus = STATUS_PARTIAL_COPY;
        return;
    }

    primaryThreadInfo = KswordARKWin32kTimerReadU64(
        timerBytes,
        Context->Layout->primaryThreadInfo);
    alternateThreadInfo = KswordARKWin32kTimerReadU64(
        timerBytes,
        Context->Layout->alternateThreadInfo);
    ownerResolved = KswordARKWin32kTimerLookupThread(
        Context->ThreadMap,
        Context->ThreadMapCount,
        primaryThreadInfo,
        &processId,
        &threadId,
        &sessionId);
    if (!ownerResolved) {
        ownerResolved = KswordARKWin32kTimerLookupThread(
            Context->ThreadMap,
            Context->ThreadMapCount,
            alternateThreadInfo,
            &processId,
            &threadId,
            &sessionId);
    }
    if (!ownerResolved) {
        processId = 0UL;
        threadId = 0UL;
        // gTimerHashTable was read while attached to RequestedSessionId. Keep
        // the object visible even when a nearest-previous private pti offset
        // cannot be joined to the public PsGetThreadWin32Thread map.
        sessionId = Context->RequestedSessionId;
    }

    if (!KswordARKWin32kTimerMatchesRequest(
            Context,
            processId,
            threadId,
            sessionId)) {
        return;
    }
    Context->Response->totalCount += 1UL;
    if (Context->Response->returnedCount >= Context->MaxEntries) {
        Context->Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
        Context->Response->lastStatus = STATUS_BUFFER_OVERFLOW;
        Context->Stop = TRUE;
        return;
    }

    entry = &Context->Response->entries[Context->Response->returnedCount];
    RtlZeroMemory(entry, sizeof(*entry));
    entry->fieldFlags = KSWORD_ARK_WIN32K_TIMER_FIELD_OBJECT |
        KSWORD_ARK_WIN32K_TIMER_FIELD_CALLBACK |
        KSWORD_ARK_WIN32K_TIMER_FIELD_INTERVAL |
        KSWORD_ARK_WIN32K_TIMER_FIELD_FLAGS |
        KSWORD_ARK_WIN32K_TIMER_FIELD_WINDOW |
        KSWORD_ARK_WIN32K_TIMER_FIELD_ID |
        KSWORD_ARK_WIN32K_TIMER_FIELD_HASH_LINK;
    if (ownerResolved) {
        entry->fieldFlags |= KSWORD_ARK_WIN32K_TIMER_FIELD_THREAD;
    }
    if (alternateThreadInfo != 0ULL) {
        entry->fieldFlags |= KSWORD_ARK_WIN32K_TIMER_FIELD_ALTERNATE_THREAD;
    }
    entry->status = ownerResolved
        ? KSWORD_ARK_WIN32K_STATUS_OK
        : KSWORD_ARK_WIN32K_STATUS_PARTIAL;
    entry->processId = processId;
    entry->threadId = threadId;
    entry->sessionId = sessionId;
    entry->flags = KswordARKWin32kTimerReadU32(timerBytes, Context->Layout->flags);
    entry->intervalMs = KswordARKWin32kTimerReadU32(timerBytes, Context->Layout->interval);
    entry->countdownMs = KswordARKWin32kTimerReadU32(timerBytes, Context->Layout->countdown);
    entry->toleranceMs = KswordARKWin32kTimerReadU32(timerBytes, Context->Layout->tolerance);
    entry->lastStatus = STATUS_SUCCESS;
    entry->timerObject = TimerAddress;
    entry->callbackAddress = KswordARKWin32kTimerReadU64(timerBytes, Context->Layout->callback);
    entry->primaryThreadInfo = primaryThreadInfo;
    entry->alternateThreadInfo = alternateThreadInfo;
    entry->windowObject = KswordARKWin32kTimerReadU64(timerBytes, Context->Layout->window);
    entry->timerId = KswordARKWin32kTimerReadU64(timerBytes, Context->Layout->timerId);
    entry->hashLink = HashLinkAddress;
    if (ownerResolved) {
        KswordARKWin32kTimerSetDetail(
            entry->detail,
            L"tagTIMER via gTimerHashTable; owner mapped through PsGetThreadWin32Thread.");
    }
    else {
        KswordARKWin32kTimerSetDetail(
            entry->detail,
            L"tagTIMER read succeeded; tagTHREADINFO owner was not present in the public thread map.");
    }
    Context->Response->returnedCount += 1UL;
}

static VOID
KswordARKWin32kTimerWalkBucket(
    _Inout_ KSWORD_ARK_WIN32K_TIMER_WALK_CONTEXT* Context,
    _In_ ULONG64 ListHeadAddress
    )
/*++

Routine Description:

    Traverse one gTimerHashTable LIST_ENTRY bucket with bounded integrity checks.

Arguments:

    Context - Mutable timer traversal context.
    ListHeadAddress - Address of the bucket LIST_ENTRY head.

Return Value:

    None. The response records partial/corrupt/read-failure evidence.

--*/
{
    LIST_ENTRY headLinks;
    ULONG64 currentAddress = 0ULL;
    ULONG64 expectedBackLink = ListHeadAddress;
    ULONG traversalCount = 0UL;

    if (Context == NULL || Context->Response == NULL ||
        !KswordARKWin32kTimerReadMemory(
            ListHeadAddress,
            &headLinks,
            sizeof(headLinks))) {
        if (Context != NULL && Context->Response != NULL) {
            Context->Response->readFailureCount += 1UL;
            Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            Context->Response->lastStatus = STATUS_PARTIAL_COPY;
        }
        return;
    }

    currentAddress = (ULONG64)(ULONG_PTR)headLinks.Flink;
    if (currentAddress == ListHeadAddress) {
        return;
    }
    if (!KswordARKWin32kTimerIsKernelAddress(currentAddress)) {
        Context->Response->corruptBucketCount += 1UL;
        Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        Context->Response->lastStatus = STATUS_DATA_ERROR;
        return;
    }

    while (currentAddress != ListHeadAddress && !Context->Stop) {
        LIST_ENTRY currentLinks;
        ULONG64 timerAddress = 0ULL;
        ULONG64 nextAddress = 0ULL;

        if (traversalCount >= KSWORD_ARK_WIN32K_TIMER_MAX_NODES_PER_BUCKET) {
            Context->Response->corruptBucketCount += 1UL;
            Context->Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
            Context->Response->lastStatus = STATUS_BUFFER_OVERFLOW;
            break;
        }
        if (!KswordARKWin32kTimerIsKernelAddress(currentAddress) ||
            currentAddress < (ULONG64)Context->Layout->hashListEntry) {
            Context->Response->corruptBucketCount += 1UL;
            Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            Context->Response->lastStatus = STATUS_DATA_ERROR;
            break;
        }
        timerAddress = currentAddress - (ULONG64)Context->Layout->hashListEntry;
        RtlZeroMemory(&currentLinks, sizeof(currentLinks));
        if (!KswordARKWin32kTimerReadMemory(
                currentAddress,
                &currentLinks,
                sizeof(currentLinks))) {
            Context->Response->readFailureCount += 1UL;
            Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            Context->Response->lastStatus = STATUS_PARTIAL_COPY;
            break;
        }
        if ((ULONG64)(ULONG_PTR)currentLinks.Blink != expectedBackLink) {
            Context->Response->corruptBucketCount += 1UL;
            Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            Context->Response->lastStatus = STATUS_DATA_ERROR;
            break;
        }
        if (KswordARKWin32kTimerAlreadySeen(
                Context->SeenTimers,
                Context->SeenCount,
                timerAddress)) {
            Context->Response->duplicateCount += 1UL;
        }
        else {
            if (Context->SeenCount < KSWORD_ARK_WIN32K_TIMER_MAX_SEEN) {
                Context->SeenTimers[Context->SeenCount] = timerAddress;
                Context->SeenCount += 1UL;
            }
            else {
                Context->Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
                Context->Response->lastStatus = STATUS_BUFFER_OVERFLOW;
                Context->Stop = TRUE;
            }
            if (!Context->Stop) {
                KswordARKWin32kTimerAppend(Context, timerAddress, currentAddress);
            }
        }
        if (Context->Stop) {
            break;
        }
        nextAddress = (ULONG64)(ULONG_PTR)currentLinks.Flink;
        if (nextAddress != ListHeadAddress &&
            (!KswordARKWin32kTimerIsKernelAddress(nextAddress) ||
             nextAddress == currentAddress)) {
            Context->Response->corruptBucketCount += 1UL;
            Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            Context->Response->lastStatus = STATUS_DATA_ERROR;
            break;
        }
        expectedBackLink = currentAddress;
        currentAddress = nextAddress;
        traversalCount += 1UL;
    }
    if (!Context->Stop && currentAddress == ListHeadAddress &&
        (ULONG64)(ULONG_PTR)headLinks.Blink != expectedBackLink) {
        Context->Response->corruptBucketCount += 1UL;
        Context->Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        Context->Response->lastStatus = STATUS_DATA_ERROR;
    }
}

NTSTATUS
KswordARKWin32kQueryTimerSnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Enumerate current USER window timers from the win32k hash table.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional session/PID/TID filters and row budget.
    BytesWrittenOut - Receives the variable response size.

Return Value:

    STATUS_SUCCESS with an explicit response status for unsupported or partial
    runtime conditions.

--*/
{
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY baseEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY fullEntry;
    IMAGE_NT_HEADERS baseHeaders;
    IMAGE_NT_HEADERS fullHeaders;
    KSWORD_ARK_WIN32K_LAYOUT_SELECTION layoutSelection;
    const KSWORD_ARK_WIN32K_TIMER_PROFILE* layoutProfile = NULL;
    KSWORD_ARK_WIN32K_TIMER_SNAPSHOT_RESPONSE* response = NULL;
    KSWORD_ARK_WIN32K_TIMER_THREAD_MAP_ENTRY* threadMap = NULL;
    ULONG64* seenTimers = NULL;
    PEPROCESS sessionProcess = NULL;
    PVOID timerHashTable = NULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG threadMapCount = 0UL;
    ULONG maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    ULONG requestedSessionId = 0UL;
    ULONG bucketIndex = 0UL;
    size_t headerSize = 0U;
    size_t entryCapacity = 0U;
    BOOLEAN threadMapTruncated = FALSE;
    BOOLEAN attached = FALSE;
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesWrittenOut == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    headerSize = sizeof(KSWORD_ARK_WIN32K_TIMER_SNAPSHOT_RESPONSE) -
        sizeof(KSWORD_ARK_WIN32K_TIMER_ENTRY);
    if (OutputBufferLength < headerSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_TIMER_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_UNKNOWN;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_TIMER_ENTRY);
    response->flags = Request != NULL ? Request->flags : 0UL;
    response->lastStatus = STATUS_SUCCESS;
    entryCapacity = (OutputBufferLength - headerSize) / sizeof(KSWORD_ARK_WIN32K_TIMER_ENTRY);
    maxEntries = Request != NULL
        ? KswordARKWin32kNormalizeMaxEntries(Request->maxEntries)
        : KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    if (entryCapacity < (size_t)maxEntries) {
        maxEntries = (ULONG)entryCapacity;
    }
    if (maxEntries == 0UL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (!NT_SUCCESS(status) ||
        !KswordARKWin32kFindModuleByName(moduleInfo, "win32kbase.sys", &baseEntry) ||
        !KswordARKWin32kFindModuleByName(moduleInfo, "win32kfull.sys", &fullEntry)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_WIN32K_NOT_FOUND;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_TIMER_HASH_EXPORT |
            KSWORD_ARK_WIN32K_CAP_TIMER_LAYOUT | KSWORD_ARK_WIN32K_CAP_TIMER_ENUM;
        response->lastStatus = NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
        goto Cleanup;
    }

    RtlZeroMemory(&baseHeaders, sizeof(baseHeaders));
    RtlZeroMemory(&fullHeaders, sizeof(fullHeaders));
    if (!KswordARKHookReadImageNtHeaders(&baseEntry, &baseHeaders) ||
        !KswordARKHookReadImageNtHeaders(&fullEntry, &fullHeaders)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_TIMER_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_TIMER_ENUM;
        response->lastStatus = STATUS_INVALID_IMAGE_FORMAT;
        KswordARKWin32kTimerSetDetail(
            response->detail,
            L"win32k PE header could not be read; timer layout was not guessed.");
        goto Cleanup;
    }

    response->win32kbaseTimeDateStamp = baseHeaders.FileHeader.TimeDateStamp;
    response->win32kbaseImageSize = baseHeaders.OptionalHeader.SizeOfImage;
    response->win32kfullTimeDateStamp = fullHeaders.FileHeader.TimeDateStamp;
    response->win32kfullImageSize = fullHeaders.OptionalHeader.SizeOfImage;
    if (!KswordARKWin32kSelectLayoutProfile(
            g_KswordArkWin32kTimerProfiles,
            RTL_NUMBER_OF(g_KswordArkWin32kTimerProfiles),
            sizeof(g_KswordArkWin32kTimerProfiles[0]),
            response->win32kbaseTimeDateStamp,
            response->win32kbaseImageSize,
            response->win32kfullTimeDateStamp,
            response->win32kfullImageSize,
            &layoutSelection)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_TIMER_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_TIMER_ENUM;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        KswordARKWin32kTimerSetDetail(
            response->detail,
            L"No exact or nearest-previous Windows tagTIMER profile is available.");
        goto Cleanup;
    }
    layoutProfile = &g_KswordArkWin32kTimerProfiles[layoutSelection.profileIndex];

    KswordARKWin32kTimerInitializeLayout(
        &response->layout,
        layoutProfile,
        layoutSelection.source);
    if (response->layout.objectSize == 0UL ||
        response->layout.objectSize > KSWORD_ARK_WIN32K_TIMER_OBJECT_SIZE ||
        response->layout.bucketCount == 0UL ||
        response->layout.bucketCount > 256UL ||
        response->layout.bucketStride < sizeof(LIST_ENTRY)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_TIMER_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_TIMER_ENUM;
        response->lastStatus = STATUS_INVALID_PARAMETER;
        KswordARKWin32kTimerSetDetail(
            response->detail,
            L"Selected tagTIMER profile failed local size and bucket validation.");
        goto Cleanup;
    }
    status = KswordARKWin32kTimerBuildThreadMap(
        &threadMap,
        &threadMapCount,
        &threadMapTruncated);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = status;
        response->missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC;
        KswordARKWin32kTimerSetDetail(
            response->detail,
            L"GUI thread mapping failed before the target win32k session could be attached.");
        goto Cleanup;
    }

    if (Request != NULL && Request->sessionId != 0UL) {
        requestedSessionId = Request->sessionId;
    }
    else if (Request != NULL &&
        (Request->flags & KSWORD_ARK_WIN32K_QUERY_FLAG_CURRENT_SESSION_ONLY) != 0UL) {
        KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId =
            KswordARKWin32kResolvePsGetProcessSessionId();
        if (psGetProcessSessionId != NULL) {
            requestedSessionId = psGetProcessSessionId(PsGetCurrentProcess());
        }
    }

    status = KswordARKWin32kTimerReferenceSessionProcess(
        threadMap,
        threadMapCount,
        Request,
        &requestedSessionId,
        &sessionProcess);
    if (!NT_SUCCESS(status) || sessionProcess == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
        response->missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_TIMER_HASH_EXPORT |
            KSWORD_ARK_WIN32K_CAP_TIMER_ENUM;
        KswordARKWin32kTimerSetDetail(
            response->detail,
            L"No live GUI process was available for the requested win32k session.");
        goto Cleanup;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = STATUS_INVALID_DEVICE_STATE;
        goto Cleanup;
    }

    RtlZeroMemory(attachState, sizeof(attachState));
    KeStackAttachProcess((PVOID)sessionProcess, attachState);
    attached = TRUE;

    timerHashTable = RtlFindExportedRoutineByName(
        baseEntry.ImageBase,
        "gTimerHashTable");
    if (timerHashTable == NULL ||
        !KswordARKWin32kTimerIsKernelAddress((ULONG64)(ULONG_PTR)timerHashTable)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_TIMER_HASH_EXPORT |
            KSWORD_ARK_WIN32K_CAP_TIMER_ENUM;
        response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        KswordARKWin32kTimerSetDetail(
            response->detail,
            L"win32kbase does not expose a readable gTimerHashTable export.");
        goto Cleanup;
    }

    response->timerHashTable = (ULONG64)(ULONG_PTR)timerHashTable;
    response->capabilityMask = KSWORD_ARK_WIN32K_CAP_WIN32KBASE_LOADED |
        KSWORD_ARK_WIN32K_CAP_WIN32KFULL_LOADED |
        KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC |
        KSWORD_ARK_WIN32K_CAP_TIMER_HASH_EXPORT |
        KSWORD_ARK_WIN32K_CAP_TIMER_LAYOUT |
        KSWORD_ARK_WIN32K_CAP_TIMER_ENUM;
    response->status = threadMapTruncated
        ? KSWORD_ARK_WIN32K_STATUS_PARTIAL
        : KSWORD_ARK_WIN32K_STATUS_OK;
    response->lastStatus = threadMapTruncated
        ? STATUS_BUFFER_OVERFLOW
        : STATUS_SUCCESS;
    KswordARKWin32kTimerSetDetail(
        response->detail,
        layoutSelection.source == KSWORD_ARK_WIN32K_LAYOUT_SELECTION_EXACT_IDENTITY
            ? L"tagTIMER layout matched the exact PE identity; gTimerHashTable is being read in the requested GUI session."
            : L"The nearest previous tagTIMER layout and the current gTimerHashTable export are active in the requested GUI session; results may be partial.");

    seenTimers = (ULONG64*)KswordARKAllocateNonPagedPool(
        sizeof(*seenTimers) * KSWORD_ARK_WIN32K_TIMER_MAX_SEEN,
        KSWORD_ARK_WIN32K_TIMER_POOL_TAG);
    if (seenTimers == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlZeroMemory(seenTimers, sizeof(*seenTimers) * KSWORD_ARK_WIN32K_TIMER_MAX_SEEN);

    {
        KSWORD_ARK_WIN32K_TIMER_WALK_CONTEXT context;
        RtlZeroMemory(&context, sizeof(context));
        context.Response = response;
        context.Layout = &response->layout;
        context.Request = Request;
        context.ThreadMap = threadMap;
        context.ThreadMapCount = threadMapCount;
        context.RequestedSessionId = requestedSessionId;
        context.MaxEntries = maxEntries;
        context.SeenTimers = seenTimers;

        for (bucketIndex = 0UL;
            bucketIndex < response->layout.bucketCount && !context.Stop;
            ++bucketIndex) {
            ULONG64 listHeadAddress = (ULONG64)(ULONG_PTR)timerHashTable +
                ((ULONG64)bucketIndex * (ULONG64)response->layout.bucketStride);
            KswordARKWin32kTimerWalkBucket(&context, listHeadAddress);
        }
    }

Cleanup:
    if (attached) {
        KeUnstackDetachProcess(attachState);
        attached = FALSE;
    }
    if (sessionProcess != NULL) {
        ObDereferenceObject(sessionProcess);
        sessionProcess = NULL;
    }
    if (seenTimers != NULL) {
        ExFreePoolWithTag(seenTimers, KSWORD_ARK_WIN32K_TIMER_POOL_TAG);
    }
    if (threadMap != NULL) {
        ExFreePoolWithTag(threadMap, KSWORD_ARK_WIN32K_TIMER_POOL_TAG);
    }
    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }
    UNREFERENCED_PARAMETER(moduleInfoBytes);
    *BytesWrittenOut = headerSize +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_WIN32K_TIMER_ENTRY));
    return STATUS_SUCCESS;
}
