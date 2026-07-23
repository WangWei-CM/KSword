/*++

Module Name:

    win32k_event_hook.c

Abstract:

    Read-only enumeration of tagEVENTHOOK objects through
    win32kbase!gpWinEventHooks. The collector is version-gated by exact
    win32kbase/win32kfull PE identities and never unhooks or changes a node.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_query.h"
#include "win32k_support.h"
#include "../../platform/pool_compat.h"

#include <ntimage.h>

#define KSWORD_ARK_WIN32K_EVENT_POOL_TAG 'eWkW'
#define KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE 0x60UL
#define KSWORD_ARK_WIN32K_EVENT_GLOBAL_RVA 0x00257EE8UL
#define KSWORD_ARK_WIN32K_EVENT_MAX_THREAD_MAP 8192UL
#define KSWORD_ARK_WIN32K_EVENT_MAX_SEEN 8192UL
#define KSWORD_ARK_WIN32K_EVENT_INTERNAL_PUBLIC_MASK 0x0000001EUL
#define KSWORD_ARK_WIN32K_EVENT_INTERNAL_IN_CONTEXT 0x00000008UL

// 当前缓存中已由 _SetWinEventHook/DestroyEventHook 反汇编复核的布局。
// 未命中同一 PE 身份时返回 UNSUPPORTED，避免猜读 tagEVENTHOOK。
#define KSWORD_ARK_WIN32K_EVENT_BASE_TIMESTAMP 0x8FC48444UL
#define KSWORD_ARK_WIN32K_EVENT_BASE_IMAGE_SIZE 0x002D6000UL
#define KSWORD_ARK_WIN32K_EVENT_FULL_TIMESTAMP 0x83C73BE4UL
#define KSWORD_ARK_WIN32K_EVENT_FULL_IMAGE_SIZE 0x003B4000UL

typedef struct _KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY
{
    ULONG64 ThreadInfo;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG SessionId;
} KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY;

static BOOLEAN
KswordARKWin32kEventIsKernelAddress(
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
KswordARKWin32kEventReadMemory(
    _In_ ULONG64 Address,
    _Out_writes_bytes_(BytesToRead) PVOID Destination,
    _In_ SIZE_T BytesToRead
    )
{
    ULONG64 lastAddress = 0ULL;

    if (Destination == NULL || BytesToRead == 0U ||
        BytesToRead > (SIZE_T)MAXULONG ||
        !KswordARKWin32kEventIsKernelAddress(Address) ||
        Address > MAXULONGLONG - (ULONG64)(BytesToRead - 1U)) {
        return FALSE;
    }
    lastAddress = Address + (ULONG64)(BytesToRead - 1U);
    if (!KswordARKWin32kEventIsKernelAddress(lastAddress)) {
        return FALSE;
    }
    return KswordARKHookReadMemorySafe(
        (const VOID*)(ULONG_PTR)Address,
        Destination,
        BytesToRead);
}

static ULONG64
KswordARKWin32kEventReadU64(
    _In_reads_bytes_(KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE) const UCHAR* ObjectBytes,
    _In_ ULONG Offset
    )
{
    ULONG64 value = 0ULL;

    if (ObjectBytes == NULL || Offset > KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE ||
        sizeof(value) > KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE - Offset) {
        return 0ULL;
    }
    RtlCopyMemory(&value, ObjectBytes + Offset, sizeof(value));
    return value;
}

static ULONG
KswordARKWin32kEventReadU32(
    _In_reads_bytes_(KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE) const UCHAR* ObjectBytes,
    _In_ ULONG Offset
    )
{
    ULONG value = 0UL;

    if (ObjectBytes == NULL || Offset > KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE ||
        sizeof(value) > KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE - Offset) {
        return 0UL;
    }
    RtlCopyMemory(&value, ObjectBytes + Offset, sizeof(value));
    return value;
}

static VOID
KswordARKWin32kEventSetDetail(
    _Out_writes_(KSWORD_ARK_WIN32K_DETAIL_CHARS) PWCHAR Destination,
    _In_z_ PCWSTR Text
    )
{
    KswordARKWin32kCopyWideText(
        Destination,
        KSWORD_ARK_WIN32K_DETAIL_CHARS,
        Text);
}

static VOID
KswordARKWin32kEventInitializeLayout(
    _Out_ KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT* Layout,
    _In_ ULONG FullTimeDateStamp,
    _In_ ULONG FullImageSize
    )
{
    RtlZeroMemory(Layout, sizeof(*Layout));
    Layout->objectSize = KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE;
    Layout->handle = 0x00UL;
    Layout->ownerThreadInfo = 0x10UL;
    Layout->nextHook = 0x18UL;
    Layout->eventMin = 0x20UL;
    Layout->eventMax = 0x24UL;
    Layout->internalFlags = 0x28UL;
    Layout->targetProcessId = 0x30UL;
    Layout->targetThreadId = 0x38UL;
    Layout->callbackOffset = 0x40UL;
    Layout->moduleAtom = 0x48UL;
    Layout->timestamp = 0x58UL;
    Layout->globalRva = KSWORD_ARK_WIN32K_EVENT_GLOBAL_RVA;
    Layout->source = KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY;
    Layout->timeDateStamp = FullTimeDateStamp;
    Layout->imageSize = FullImageSize;
}

static NTSTATUS
KswordARKWin32kEventBuildThreadMap(
    _Outptr_result_buffer_(*CountOut) KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY** MapOut,
    _Out_ ULONG* CountOut,
    _Out_ BOOLEAN* TruncatedOut
    )
{
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId = NULL;
    KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY* map = NULL;
    PEPROCESS processCursor = NULL;
    ULONG count = 0UL;
    BOOLEAN truncated = FALSE;

    if (MapOut == NULL || CountOut == NULL || TruncatedOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *MapOut = NULL;
    *CountOut = 0UL;
    *TruncatedOut = FALSE;

    map = (KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY*)KswordARKAllocateNonPagedPool(
        sizeof(*map) * KSWORD_ARK_WIN32K_EVENT_MAX_THREAD_MAP,
        KSWORD_ARK_WIN32K_EVENT_POOL_TAG);
    if (map == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(map, sizeof(*map) * KSWORD_ARK_WIN32K_EVENT_MAX_THREAD_MAP);

    psGetNextProcess = KswordARKWin32kResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKWin32kResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKWin32kResolvePsGetThreadWin32Thread();
    psGetProcessSessionId = KswordARKWin32kResolvePsGetProcessSessionId();
    if (psGetNextProcess == NULL || psGetNextProcessThread == NULL ||
        psGetThreadWin32Thread == NULL) {
        ExFreePoolWithTag(map, KSWORD_ARK_WIN32K_EVENT_POOL_TAG);
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    processCursor = psGetNextProcess(NULL);
    while (processCursor != NULL) {
        PEPROCESS nextProcess = psGetNextProcess(processCursor);
        PETHREAD threadCursor = NULL;
        ULONG processId = HandleToULong(PsGetProcessId(processCursor));
        ULONG sessionId = psGetProcessSessionId != NULL
            ? psGetProcessSessionId(processCursor)
            : 0UL;

        threadCursor = psGetNextProcessThread(processCursor, NULL);
        while (threadCursor != NULL) {
            PETHREAD nextThread = psGetNextProcessThread(processCursor, threadCursor);
            PVOID threadInfo = psGetThreadWin32Thread(threadCursor);

            if (threadInfo != NULL) {
                if (count >= KSWORD_ARK_WIN32K_EVENT_MAX_THREAD_MAP) {
                    truncated = TRUE;
                }
                else {
                    map[count].ThreadInfo = (ULONG64)(ULONG_PTR)threadInfo;
                    map[count].ProcessId = processId;
                    map[count].ThreadId = HandleToULong(PsGetThreadId(threadCursor));
                    map[count].SessionId = sessionId;
                    count += 1UL;
                }
            }
            ObDereferenceObject(threadCursor);
            threadCursor = nextThread;
        }
        ObDereferenceObject(processCursor);
        processCursor = nextProcess;
    }

    *MapOut = map;
    *CountOut = count;
    *TruncatedOut = truncated;
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKWin32kEventLookupThread(
    _In_reads_(ThreadMapCount) const KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY* ThreadMap,
    _In_ ULONG ThreadMapCount,
    _In_ ULONG64 ThreadInfo,
    _Out_ ULONG* ProcessIdOut,
    _Out_ ULONG* ThreadIdOut,
    _Out_ ULONG* SessionIdOut
    )
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
KswordARKWin32kEventAlreadySeen(
    _In_reads_(SeenCount) const ULONG64* SeenHooks,
    _In_ ULONG SeenCount,
    _In_ ULONG64 HookAddress
    )
{
    ULONG index = 0UL;

    if (SeenHooks == NULL || HookAddress == 0ULL) {
        return FALSE;
    }
    for (index = 0UL; index < SeenCount; ++index) {
        if (SeenHooks[index] == HookAddress) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
KswordARKWin32kEventMatchesRequest(
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _In_ ULONG RequestedSessionId,
    _In_ ULONG ProcessId,
    _In_ ULONG ThreadId,
    _In_ ULONG SessionId
    )
{
    if (RequestedSessionId != 0UL && RequestedSessionId != SessionId) {
        return FALSE;
    }
    if (Request != NULL && Request->processId != 0UL &&
        Request->processId != ProcessId) {
        return FALSE;
    }
    if (Request != NULL && Request->threadId != 0UL &&
        Request->threadId != ThreadId) {
        return FALSE;
    }
    return TRUE;
}

NTSTATUS
KswordARKWin32kQueryEventHookSnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY baseEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY fullEntry;
    IMAGE_NT_HEADERS baseHeaders;
    IMAGE_NT_HEADERS fullHeaders;
    KSWORD_ARK_WIN32K_EVENT_HOOK_SNAPSHOT_RESPONSE* response = NULL;
    KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY* threadMap = NULL;
    ULONG64* seenHooks = NULL;
    ULONG64 hookListPointer = 0ULL;
    ULONG64 currentHook = 0ULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG threadMapCount = 0UL;
    ULONG seenCount = 0UL;
    ULONG maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    ULONG requestedSessionId = 0UL;
    size_t headerSize = 0U;
    size_t entryCapacity = 0U;
    BOOLEAN threadMapTruncated = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesWrittenOut == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    headerSize = sizeof(KSWORD_ARK_WIN32K_EVENT_HOOK_SNAPSHOT_RESPONSE) -
        sizeof(KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY);
    if (OutputBufferLength < headerSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_EVENT_HOOK_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_UNKNOWN;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY);
    response->flags = Request != NULL ? Request->flags : 0UL;
    response->lastStatus = STATUS_SUCCESS;
    entryCapacity = (OutputBufferLength - headerSize) /
        sizeof(KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY);
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
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
        goto Cleanup;
    }

    RtlZeroMemory(&baseHeaders, sizeof(baseHeaders));
    RtlZeroMemory(&fullHeaders, sizeof(fullHeaders));
    if (!KswordARKHookReadImageNtHeaders(&baseEntry, &baseHeaders) ||
        !KswordARKHookReadImageNtHeaders(&fullEntry, &fullHeaders)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = STATUS_INVALID_IMAGE_FORMAT;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"win32k PE header could not be read; tagEVENTHOOK layout was not guessed.");
        goto Cleanup;
    }

    response->win32kbaseTimeDateStamp = baseHeaders.FileHeader.TimeDateStamp;
    response->win32kbaseImageSize = baseHeaders.OptionalHeader.SizeOfImage;
    response->win32kfullTimeDateStamp = fullHeaders.FileHeader.TimeDateStamp;
    response->win32kfullImageSize = fullHeaders.OptionalHeader.SizeOfImage;
    if (response->win32kbaseTimeDateStamp != KSWORD_ARK_WIN32K_EVENT_BASE_TIMESTAMP ||
        response->win32kbaseImageSize != KSWORD_ARK_WIN32K_EVENT_BASE_IMAGE_SIZE ||
        response->win32kfullTimeDateStamp != KSWORD_ARK_WIN32K_EVENT_FULL_TIMESTAMP ||
        response->win32kfullImageSize != KSWORD_ARK_WIN32K_EVENT_FULL_IMAGE_SIZE) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"Current win32k PE identity has no verified tagEVENTHOOK layout in the offset table.");
        goto Cleanup;
    }

    KswordARKWin32kEventInitializeLayout(
        &response->layout,
        response->win32kfullTimeDateStamp,
        response->win32kfullImageSize);
    if ((ULONG64)KSWORD_ARK_WIN32K_EVENT_GLOBAL_RVA + sizeof(ULONG64) >
        (ULONG64)response->win32kbaseImageSize) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = STATUS_INVALID_ADDRESS;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"gpWinEventHooks RVA is outside the verified win32kbase image.");
        goto Cleanup;
    }

    hookListPointer = (ULONG64)(ULONG_PTR)baseEntry.ImageBase +
        (ULONG64)KSWORD_ARK_WIN32K_EVENT_GLOBAL_RVA;
    response->hookListPointer = hookListPointer;
    if (!KswordARKWin32kEventReadMemory(
            hookListPointer,
            &currentHook,
            sizeof(currentHook))) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = STATUS_PARTIAL_COPY;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"gpWinEventHooks pointer could not be read in the current session.");
        goto Cleanup;
    }
    response->hookListHead = currentHook;
    response->capabilityMask = KSWORD_ARK_WIN32K_CAP_WIN32KBASE_LOADED |
        KSWORD_ARK_WIN32K_CAP_WIN32KFULL_LOADED |
        KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
        KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_LAYOUT |
        KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
    response->status = KSWORD_ARK_WIN32K_STATUS_OK;
    KswordARKWin32kEventSetDetail(
        response->detail,
        L"tagEVENTHOOK layout validated by PE identity; global chain is read-only and bounded.");

    status = KswordARKWin32kEventBuildThreadMap(
        &threadMap,
        &threadMapCount,
        &threadMapTruncated);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        response->lastStatus = status;
        response->missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"Event hook chain is available, but owner ThreadInfo mapping failed.");
    }
    else {
        response->capabilityMask |= KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC;
        if (threadMapTruncated) {
            response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            response->lastStatus = STATUS_BUFFER_OVERFLOW;
        }
    }

    seenHooks = (ULONG64*)KswordARKAllocateNonPagedPool(
        sizeof(*seenHooks) * KSWORD_ARK_WIN32K_EVENT_MAX_SEEN,
        KSWORD_ARK_WIN32K_EVENT_POOL_TAG);
    if (seenHooks == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlZeroMemory(seenHooks, sizeof(*seenHooks) * KSWORD_ARK_WIN32K_EVENT_MAX_SEEN);

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

    while (currentHook != 0ULL) {
        UCHAR objectBytes[KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE];
        ULONG64 nextHook = 0ULL;
        ULONG64 ownerThreadInfo = 0ULL;
        ULONG processId = 0UL;
        ULONG threadId = 0UL;
        ULONG sessionId = 0UL;
        BOOLEAN ownerResolved = FALSE;

        response->visitedNodeCount += 1UL;
        if (seenCount >= KSWORD_ARK_WIN32K_EVENT_MAX_SEEN) {
            response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
            response->lastStatus = STATUS_BUFFER_OVERFLOW;
            break;
        }
        if (!KswordARKWin32kEventIsKernelAddress(currentHook)) {
            response->corruptLinkCount += 1UL;
            response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            response->lastStatus = STATUS_INVALID_ADDRESS;
            break;
        }
        if (KswordARKWin32kEventAlreadySeen(seenHooks, seenCount, currentHook)) {
            response->duplicateCount += 1UL;
            response->corruptLinkCount += 1UL;
            response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            response->lastStatus = STATUS_DATA_ERROR;
            break;
        }
        seenHooks[seenCount++] = currentHook;
        if (!KswordARKWin32kEventReadMemory(
                currentHook,
                objectBytes,
                sizeof(objectBytes))) {
            response->readFailureCount += 1UL;
            response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            response->lastStatus = STATUS_PARTIAL_COPY;
            break;
        }

        nextHook = KswordARKWin32kEventReadU64(
            objectBytes,
            response->layout.nextHook);
        ownerThreadInfo = KswordARKWin32kEventReadU64(
            objectBytes,
            response->layout.ownerThreadInfo);
        ownerResolved = KswordARKWin32kEventLookupThread(
            threadMap,
            threadMapCount,
            ownerThreadInfo,
            &processId,
            &threadId,
            &sessionId);

        if (KswordARKWin32kEventMatchesRequest(
                Request,
                requestedSessionId,
                processId,
                threadId,
                sessionId)) {
            KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY* entry = NULL;
            ULONG internalFlags = KswordARKWin32kEventReadU32(
                objectBytes,
                response->layout.internalFlags);
            ULONG64 callbackOffset = KswordARKWin32kEventReadU64(
                objectBytes,
                response->layout.callbackOffset);

            response->totalCount += 1UL;
            if (response->returnedCount >= maxEntries) {
                response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
                response->lastStatus = STATUS_BUFFER_OVERFLOW;
                break;
            }
            entry = &response->entries[response->returnedCount++];
            RtlZeroMemory(entry, sizeof(*entry));
            entry->fieldFlags = KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_HANDLE |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_OWNER |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_RANGE |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_FLAGS |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_TARGET |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_CALLBACK |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_MODULE_ATOM |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_NEXT |
                KSWORD_ARK_WIN32K_EVENT_HOOK_FIELD_TIMESTAMP;
            entry->status = ownerResolved
                ? KSWORD_ARK_WIN32K_STATUS_OK
                : KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            entry->processId = processId;
            entry->threadId = threadId;
            entry->sessionId = sessionId;
            entry->flags = (internalFlags & KSWORD_ARK_WIN32K_EVENT_INTERNAL_PUBLIC_MASK) >> 1U;
            entry->internalFlags = internalFlags;
            entry->eventMin = KswordARKWin32kEventReadU32(
                objectBytes,
                response->layout.eventMin);
            entry->eventMax = KswordARKWin32kEventReadU32(
                objectBytes,
                response->layout.eventMax);
            entry->targetProcessId = (ULONG)KswordARKWin32kEventReadU64(
                objectBytes,
                response->layout.targetProcessId);
            entry->targetThreadId = KswordARKWin32kEventReadU32(
                objectBytes,
                response->layout.targetThreadId);
            entry->moduleAtom = KswordARKWin32kEventReadU32(
                objectBytes,
                response->layout.moduleAtom);
            entry->installTime = KswordARKWin32kEventReadU32(
                objectBytes,
                response->layout.timestamp);
            entry->lastStatus = ownerResolved ? STATUS_SUCCESS : STATUS_NOT_FOUND;
            entry->hookHandle = KswordARKWin32kEventReadU64(
                objectBytes,
                response->layout.handle);
            entry->hookObject = currentHook;
            entry->nextHookObject = nextHook;
            entry->ownerThreadInfo = ownerThreadInfo;
            entry->callbackOffset = callbackOffset;
            if ((internalFlags & KSWORD_ARK_WIN32K_EVENT_INTERNAL_IN_CONTEXT) == 0UL) {
                entry->callbackAddress = callbackOffset;
            }
            KswordARKWin32kEventSetDetail(
                entry->detail,
                ownerResolved
                    ? L"tagEVENTHOOK owner mapped through PsGetThreadWin32Thread."
                    : L"tagEVENTHOOK fields read; owner ThreadInfo was not found in the public thread map.");
        }

        if (nextHook != 0ULL &&
            (!KswordARKWin32kEventIsKernelAddress(nextHook) || nextHook == currentHook)) {
            response->corruptLinkCount += 1UL;
            response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            response->lastStatus = STATUS_DATA_ERROR;
            break;
        }
        currentHook = nextHook;
    }

Cleanup:
    if (seenHooks != NULL) {
        ExFreePoolWithTag(seenHooks, KSWORD_ARK_WIN32K_EVENT_POOL_TAG);
    }
    if (threadMap != NULL) {
        ExFreePoolWithTag(threadMap, KSWORD_ARK_WIN32K_EVENT_POOL_TAG);
    }
    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }
    UNREFERENCED_PARAMETER(moduleInfoBytes);
    *BytesWrittenOut = headerSize +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_WIN32K_EVENT_HOOK_ENTRY));
    return STATUS_SUCCESS;
}
