/*++

Module Name:

    win32k_message_hook.c

Abstract:

    Read-only enumeration of classic Win32 message hook chains. The collector
    walks tagTHREADINFO/DESKTOPINFO aphkStart arrays and tagHOOK nodes only
    after the loaded win32kbase/win32kfull PE identity matches a verified
    layout. It never removes, unlinks, or changes a hook.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_query.h"
#include "win32k_support.h"
#include "../../platform/pool_compat.h"

#include <ntimage.h>

#define KSWORD_ARK_WIN32K_MESSAGE_POOL_TAG 'mWkW'
#define KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE 0x60UL
#define KSWORD_ARK_WIN32K_MESSAGE_HOOK_TYPE_COUNT 16UL
#define KSWORD_ARK_WIN32K_MESSAGE_MAX_THREAD_MAP 8192UL
#define KSWORD_ARK_WIN32K_MESSAGE_MAX_SEEN 8192UL
#define KSWORD_ARK_WIN32K_MESSAGE_MAX_CHAIN_DEPTH 1024UL
#define KSWORD_ARK_WIN32K_MESSAGE_MAX_PROCESS_WALK 4096UL
#define KSWORD_ARK_WIN32K_MESSAGE_MAX_THREAD_WALK 65536UL

// 当前 PDB/PE 矩阵中由 NtUserSetWindowsHookEx、FreeHook、GetHmodTableIndex
// 反汇编交叉验证的精确布局。未知 PE 身份返回 UNSUPPORTED。
#define KSWORD_ARK_WIN32K_MESSAGE_BASE_TIMESTAMP 0x8FC48444UL
#define KSWORD_ARK_WIN32K_MESSAGE_BASE_IMAGE_SIZE 0x002D6000UL
#define KSWORD_ARK_WIN32K_MESSAGE_FULL_TIMESTAMP 0x83C73BE4UL
#define KSWORD_ARK_WIN32K_MESSAGE_FULL_IMAGE_SIZE 0x003B4000UL

#define KSWORD_ARK_WIN32K_MESSAGE_MODULE_ATOM_TABLE_RVA 0x003391D0UL
#define KSWORD_ARK_WIN32K_MESSAGE_MODULE_ATOM_COUNT_RVA 0x00339310UL

typedef struct _KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY
{
    ULONG64 ThreadInfo;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG SessionId;
} KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY;

typedef struct _KSWORD_ARK_WIN32K_MESSAGE_SEEN_ENTRY
{
    ULONG64 HookObject;
    ULONG SessionId;
} KSWORD_ARK_WIN32K_MESSAGE_SEEN_ENTRY;

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

static BOOLEAN
KswordARKWin32kMessageIsKernelAddress(
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
KswordARKWin32kMessageReadMemory(
    _In_ ULONG64 Address,
    _Out_writes_bytes_(BytesToRead) PVOID Destination,
    _In_ SIZE_T BytesToRead
    )
{
    ULONG64 lastAddress = 0ULL;

    if (Destination == NULL || BytesToRead == 0U ||
        BytesToRead > (SIZE_T)MAXULONG ||
        !KswordARKWin32kMessageIsKernelAddress(Address) ||
        Address > MAXULONGLONG - (ULONG64)(BytesToRead - 1U)) {
        return FALSE;
    }
    lastAddress = Address + (ULONG64)(BytesToRead - 1U);
    if (!KswordARKWin32kMessageIsKernelAddress(lastAddress)) {
        return FALSE;
    }
    return KswordARKHookReadMemorySafe(
        (const VOID*)(ULONG_PTR)Address,
        Destination,
        BytesToRead);
}

static ULONG64
KswordARKWin32kMessageReadU64(
    _In_reads_bytes_(KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE) const UCHAR* ObjectBytes,
    _In_ ULONG Offset
    )
{
    ULONG64 value = 0ULL;

    if (ObjectBytes == NULL || Offset > KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE ||
        sizeof(value) > KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE - Offset) {
        return 0ULL;
    }
    RtlCopyMemory(&value, ObjectBytes + Offset, sizeof(value));
    return value;
}

static ULONG
KswordARKWin32kMessageReadU32(
    _In_reads_bytes_(KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE) const UCHAR* ObjectBytes,
    _In_ ULONG Offset
    )
{
    ULONG value = 0UL;

    if (ObjectBytes == NULL || Offset > KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE ||
        sizeof(value) > KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE - Offset) {
        return 0UL;
    }
    RtlCopyMemory(&value, ObjectBytes + Offset, sizeof(value));
    return value;
}

static VOID
KswordARKWin32kMessageSetDetail(
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
KswordARKWin32kMessageInitializeLayout(
    _Out_ KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT* Layout,
    _In_ ULONG FullTimeDateStamp,
    _In_ ULONG FullImageSize
    )
{
    RtlZeroMemory(Layout, sizeof(*Layout));
    Layout->objectSize = KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE;
    Layout->handle = 0x00UL;
    Layout->ownerThreadInfo = 0x10UL;
    Layout->desktopObject = 0x18UL;
    Layout->nextHook = 0x28UL;
    Layout->hookType = 0x30UL;
    Layout->procedureOffset = 0x38UL;
    Layout->flags = 0x40UL;
    Layout->moduleId = 0x44UL;
    Layout->targetThreadInfo = 0x48UL;
    Layout->threadHookArray = 0x390UL;
    Layout->threadDesktopInfo = 0x1D0UL;
    Layout->desktopHookArray = 0x28UL;
    Layout->moduleAtomTableRva = KSWORD_ARK_WIN32K_MESSAGE_MODULE_ATOM_TABLE_RVA;
    Layout->moduleAtomCountRva = KSWORD_ARK_WIN32K_MESSAGE_MODULE_ATOM_COUNT_RVA;
    Layout->source = KSWORD_ARK_WIN32K_MESSAGE_HOOK_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY;
    Layout->timeDateStamp = FullTimeDateStamp;
    Layout->imageSize = FullImageSize;
}

static NTSTATUS
KswordARKWin32kMessageBuildThreadMap(
    _Outptr_result_buffer_(*CountOut) KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY** MapOut,
    _Out_ ULONG* CountOut,
    _Out_ BOOLEAN* TruncatedOut
    )
{
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId = NULL;
    KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY* map = NULL;
    PEPROCESS processCursor = NULL;
    ULONG processWalkCount = 0UL;
    ULONG count = 0UL;
    BOOLEAN truncated = FALSE;

    if (MapOut == NULL || CountOut == NULL || TruncatedOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *MapOut = NULL;
    *CountOut = 0UL;
    *TruncatedOut = FALSE;

    map = (KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY*)KswordARKAllocateNonPagedPool(
        sizeof(*map) * KSWORD_ARK_WIN32K_MESSAGE_MAX_THREAD_MAP,
        KSWORD_ARK_WIN32K_MESSAGE_POOL_TAG);
    if (map == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(map, sizeof(*map) * KSWORD_ARK_WIN32K_MESSAGE_MAX_THREAD_MAP);

    psGetNextProcess = KswordARKWin32kResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKWin32kResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKWin32kResolvePsGetThreadWin32Thread();
    psGetProcessSessionId = KswordARKWin32kResolvePsGetProcessSessionId();
    if (psGetNextProcess == NULL || psGetNextProcessThread == NULL ||
        psGetThreadWin32Thread == NULL) {
        ExFreePoolWithTag(map, KSWORD_ARK_WIN32K_MESSAGE_POOL_TAG);
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    processCursor = psGetNextProcess(NULL);
    while (processCursor != NULL &&
        processWalkCount < KSWORD_ARK_WIN32K_MESSAGE_MAX_PROCESS_WALK) {
        PEPROCESS nextProcess = psGetNextProcess(processCursor);
        PETHREAD threadCursor = NULL;
        ULONG threadWalkCount = 0UL;
        ULONG processId = HandleToULong(PsGetProcessId(processCursor));
        ULONG sessionId = psGetProcessSessionId != NULL
            ? psGetProcessSessionId(processCursor)
            : 0UL;

        threadCursor = psGetNextProcessThread(processCursor, NULL);
        while (threadCursor != NULL &&
            threadWalkCount < KSWORD_ARK_WIN32K_MESSAGE_MAX_THREAD_WALK) {
            PETHREAD nextThread = psGetNextProcessThread(processCursor, threadCursor);
            PVOID threadInfo = psGetThreadWin32Thread(threadCursor);

            if (threadInfo != NULL) {
                if (count >= KSWORD_ARK_WIN32K_MESSAGE_MAX_THREAD_MAP) {
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
            threadWalkCount += 1UL;
        }
        if (threadCursor != NULL) {
            ObDereferenceObject(threadCursor);
            truncated = TRUE;
        }
        ObDereferenceObject(processCursor);
        processCursor = nextProcess;
        processWalkCount += 1UL;
    }
    if (processCursor != NULL) {
        ObDereferenceObject(processCursor);
        truncated = TRUE;
    }

    *MapOut = map;
    *CountOut = count;
    *TruncatedOut = truncated;
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKWin32kMessageLookupThread(
    _In_reads_(ThreadMapCount) const KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY* ThreadMap,
    _In_ ULONG ThreadMapCount,
    _In_ ULONG64 ThreadInfo,
    _In_ ULONG PreferredSessionId,
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
        if (ThreadMap[index].ThreadInfo == ThreadInfo &&
            (PreferredSessionId == 0UL || ThreadMap[index].SessionId == PreferredSessionId)) {
            *ProcessIdOut = ThreadMap[index].ProcessId;
            *ThreadIdOut = ThreadMap[index].ThreadId;
            *SessionIdOut = ThreadMap[index].SessionId;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
KswordARKWin32kMessageAlreadySeen(
    _In_reads_(SeenCount) const KSWORD_ARK_WIN32K_MESSAGE_SEEN_ENTRY* SeenHooks,
    _In_ ULONG SeenCount,
    _In_ ULONG64 HookObject,
    _In_ ULONG SessionId
    )
{
    ULONG index = 0UL;

    if (SeenHooks == NULL || HookObject == 0ULL) {
        return FALSE;
    }
    for (index = 0UL; index < SeenCount; ++index) {
        if (SeenHooks[index].HookObject == HookObject &&
            SeenHooks[index].SessionId == SessionId) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
KswordARKWin32kMessageMatchesRequest(
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _In_ ULONG RequestedSessionId,
    _In_ ULONG OwnerProcessId,
    _In_ ULONG OwnerThreadId,
    _In_ ULONG OwnerSessionId,
    _In_ ULONG TargetProcessId,
    _In_ ULONG TargetThreadId,
    _In_ ULONG TargetSessionId
    )
{
    if (RequestedSessionId != 0UL &&
        OwnerSessionId != RequestedSessionId &&
        TargetSessionId != RequestedSessionId) {
        return FALSE;
    }
    if (Request != NULL && Request->processId != 0UL &&
        OwnerProcessId != Request->processId &&
        TargetProcessId != Request->processId) {
        return FALSE;
    }
    if (Request != NULL && Request->threadId != 0UL &&
        OwnerThreadId != Request->threadId &&
        TargetThreadId != Request->threadId) {
        return FALSE;
    }
    return TRUE;
}

static VOID
KswordARKWin32kMessageMarkPartial(
    _Inout_ KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE* Response,
    _In_ NTSTATUS Status
    )
{
    if (Response == NULL) {
        return;
    }
    if (Response->status == KSWORD_ARK_WIN32K_STATUS_OK) {
        Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
    }
    if (!NT_SUCCESS(Status)) {
        Response->lastStatus = Status;
    }
}

static VOID
KswordARKWin32kMessageWalkChain(
    _Inout_ KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE* Response,
    _In_ ULONG MaxEntries,
    _Inout_updates_(KSWORD_ARK_WIN32K_MESSAGE_MAX_SEEN) KSWORD_ARK_WIN32K_MESSAGE_SEEN_ENTRY* SeenHooks,
    _Inout_ ULONG* SeenCount,
    _In_reads_(ThreadMapCount) const KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY* ThreadMap,
    _In_ ULONG ThreadMapCount,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _In_ ULONG RequestedSessionId,
    _In_ ULONG DiscoveryProcessId,
    _In_ ULONG DiscoveryThreadId,
    _In_ ULONG DiscoverySessionId,
    _In_ ULONG64 DiscoveryThreadInfo,
    _In_ ULONG Source,
    _In_ ULONG Scope,
    _In_ LONG ExpectedHookType,
    _In_ ULONG64 ChainHead,
    _In_ ULONG64 Win32kfullBase
    )
{
    ULONG64 currentHook = 0ULL;
    ULONG depth = 0UL;

    if (Response == NULL || SeenHooks == NULL || SeenCount == NULL ||
        ChainHead == 0ULL) {
        return;
    }
    if (!KswordARKWin32kMessageReadMemory(
            ChainHead,
            &currentHook,
            sizeof(currentHook)) ||
        currentHook == 0ULL) {
        return;
    }
    Response->discoveredChainCount += 1UL;

    while (currentHook != 0ULL && depth < KSWORD_ARK_WIN32K_MESSAGE_MAX_CHAIN_DEPTH) {
        UCHAR objectBytes[KSWORD_ARK_WIN32K_MESSAGE_OBJECT_SIZE];
        ULONG64 nextHook = 0ULL;
        ULONG64 ownerThreadInfo = 0ULL;
        ULONG64 targetThreadInfo = 0ULL;
        ULONG ownerProcessId = 0UL;
        ULONG ownerThreadId = 0UL;
        ULONG ownerSessionId = 0UL;
        ULONG targetProcessId = 0UL;
        ULONG targetThreadId = 0UL;
        ULONG targetSessionId = 0UL;
        ULONG hookFlags = 0UL;
        ULONG hookTypeRaw = 0UL;
        ULONG moduleId = 0UL;
        ULONG moduleAtom = 0UL;
        ULONG moduleCount = 0UL;
        ULONG64 procedureOffset = 0ULL;
        BOOLEAN ownerResolved = FALSE;
        BOOLEAN targetResolved = FALSE;

        Response->visitedNodeCount += 1UL;
        if (*SeenCount >= KSWORD_ARK_WIN32K_MESSAGE_MAX_SEEN) {
            Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
            Response->lastStatus = STATUS_BUFFER_OVERFLOW;
            return;
        }
        if (!KswordARKWin32kMessageIsKernelAddress(currentHook)) {
            Response->corruptLinkCount += 1UL;
            KswordARKWin32kMessageMarkPartial(Response, STATUS_INVALID_ADDRESS);
            return;
        }
        if (KswordARKWin32kMessageAlreadySeen(
                SeenHooks,
                *SeenCount,
                currentHook,
                DiscoverySessionId)) {
            Response->duplicateCount += 1UL;
            return;
        }
        SeenHooks[*SeenCount].HookObject = currentHook;
        SeenHooks[*SeenCount].SessionId = DiscoverySessionId;
        *SeenCount += 1UL;

        if (!KswordARKWin32kMessageReadMemory(
                currentHook,
                objectBytes,
                sizeof(objectBytes))) {
            Response->readFailureCount += 1UL;
            KswordARKWin32kMessageMarkPartial(Response, STATUS_PARTIAL_COPY);
            return;
        }

        nextHook = KswordARKWin32kMessageReadU64(
            objectBytes,
            Response->layout.nextHook);
        ownerThreadInfo = KswordARKWin32kMessageReadU64(
            objectBytes,
            Response->layout.ownerThreadInfo);
        targetThreadInfo = KswordARKWin32kMessageReadU64(
            objectBytes,
            Response->layout.targetThreadInfo);
        hookTypeRaw = KswordARKWin32kMessageReadU32(
            objectBytes,
            Response->layout.hookType);
        hookFlags = KswordARKWin32kMessageReadU32(
            objectBytes,
            Response->layout.flags);
        moduleId = KswordARKWin32kMessageReadU32(
            objectBytes,
            Response->layout.moduleId);
        procedureOffset = KswordARKWin32kMessageReadU64(
            objectBytes,
            Response->layout.procedureOffset);

        ownerResolved = KswordARKWin32kMessageLookupThread(
            ThreadMap,
            ThreadMapCount,
            ownerThreadInfo,
            DiscoverySessionId,
            &ownerProcessId,
            &ownerThreadId,
            &ownerSessionId);
        targetResolved = KswordARKWin32kMessageLookupThread(
            ThreadMap,
            ThreadMapCount,
            targetThreadInfo,
            DiscoverySessionId,
            &targetProcessId,
            &targetThreadId,
            &targetSessionId);
        if (!ownerResolved && ownerThreadInfo == DiscoveryThreadInfo) {
            ownerProcessId = DiscoveryProcessId;
            ownerThreadId = DiscoveryThreadId;
            ownerSessionId = DiscoverySessionId;
            ownerResolved = TRUE;
        }
        if (!targetResolved && Scope == KSWORD_ARK_WIN32K_MESSAGE_HOOK_SCOPE_THREAD) {
            targetProcessId = DiscoveryProcessId;
            targetThreadId = DiscoveryThreadId;
            targetSessionId = DiscoverySessionId;
        }

        if ((LONG)hookTypeRaw != ExpectedHookType) {
            Response->corruptLinkCount += 1UL;
            KswordARKWin32kMessageMarkPartial(Response, STATUS_DATA_ERROR);
        }

        if ((LONG)moduleId >= 0 &&
            KswordARKWin32kMessageReadMemory(
                Win32kfullBase + Response->layout.moduleAtomCountRva,
                &moduleCount,
                sizeof(moduleCount)) &&
            moduleCount <= 0x1000UL &&
            moduleId < moduleCount) {
            USHORT atomValue = 0U;
            if (KswordARKWin32kMessageReadMemory(
                    Win32kfullBase + Response->layout.moduleAtomTableRva +
                        ((ULONG64)moduleId * sizeof(atomValue)),
                    &atomValue,
                    sizeof(atomValue))) {
                moduleAtom = atomValue;
            }
        }

        if (KswordARKWin32kMessageMatchesRequest(
                Request,
                RequestedSessionId,
                ownerProcessId,
                ownerThreadId,
                ownerSessionId,
                targetProcessId,
                targetThreadId,
                targetSessionId)) {
            KSWORD_ARK_WIN32K_HOOK_ENTRY* entry = NULL;

            Response->totalCount += 1UL;
            if (Response->returnedCount >= MaxEntries) {
                Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
                Response->lastStatus = STATUS_BUFFER_OVERFLOW;
                return;
            }

            entry = &Response->entries[Response->returnedCount++];
            RtlZeroMemory(entry, sizeof(*entry));
            entry->source = Source;
            entry->status = ownerResolved
                ? KSWORD_ARK_WIN32K_STATUS_OK
                : KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            entry->flags = hookFlags;
            entry->sessionId = ownerSessionId != 0UL
                ? ownerSessionId
                : DiscoverySessionId;
            entry->processId = ownerProcessId;
            entry->threadId = ownerThreadId;
            entry->hookType = hookTypeRaw;
            entry->hookScope = Scope;
            entry->lastStatus = ownerResolved ? STATUS_SUCCESS : STATUS_NOT_FOUND;
            entry->fieldFlags = KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_HANDLE |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_OWNER |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_DESKTOP |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_NEXT |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_TYPE |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_PROCEDURE |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_FLAGS |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_MODULE |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_TARGET |
                KSWORD_ARK_WIN32K_MESSAGE_HOOK_FIELD_CHAIN;
            entry->hookObject = currentHook;
            entry->chainHead = ChainHead;
            entry->nextHookObject = nextHook;
            entry->threadInfo = ownerThreadInfo;
            entry->targetThreadInfo = targetThreadInfo;
            entry->desktopObject = KswordARKWin32kMessageReadU64(
                objectBytes,
                Response->layout.desktopObject);
            entry->procedureAddress = (LONG)moduleId < 0
                ? procedureOffset
                : 0ULL;
            entry->moduleBase = 0ULL;
            entry->hookHandle = KswordARKWin32kMessageReadU64(
                objectBytes,
                Response->layout.handle);
            entry->procedureOffset = procedureOffset;
            entry->moduleId = moduleId;
            entry->moduleAtom = moduleAtom;
            entry->targetProcessId = targetProcessId;
            entry->targetThreadId = targetThreadId;
            entry->targetSessionId = targetSessionId;
            KswordARKWin32kMessageSetDetail(
                entry->detail,
                ownerResolved
                    ? L"tagHOOK owner mapped through PsGetThreadWin32Thread; chain was read only."
                    : L"tagHOOK fields read; owner ThreadInfo was not found in the public thread map.");
        }

        if (nextHook != 0ULL &&
            (!KswordARKWin32kMessageIsKernelAddress(nextHook) ||
                nextHook == currentHook)) {
            Response->corruptLinkCount += 1UL;
            KswordARKWin32kMessageMarkPartial(Response, STATUS_DATA_ERROR);
            return;
        }
        currentHook = nextHook;
        depth += 1UL;
    }

    if (currentHook != 0ULL) {
        Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
        Response->lastStatus = STATUS_BUFFER_OVERFLOW;
    }
}

NTSTATUS
KswordARKWin32kQueryHookSnapshot(
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
    KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE* response = NULL;
    KSWORD_ARK_WIN32K_MESSAGE_THREAD_MAP_ENTRY* threadMap = NULL;
    KSWORD_ARK_WIN32K_MESSAGE_SEEN_ENTRY* seenHooks = NULL;
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId = NULL;
    PEPROCESS processCursor = NULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG threadMapCount = 0UL;
    ULONG seenCount = 0UL;
    ULONG maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    ULONG requestedSessionId = 0UL;
    ULONG processWalkCount = 0UL;
    size_t headerSize = 0U;
    size_t entryCapacity = 0U;
    BOOLEAN threadMapTruncated = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesWrittenOut == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    headerSize = sizeof(KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE) -
        sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY);
    if (OutputBufferLength < headerSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_UNKNOWN;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY);
    response->flags = Request != NULL ? Request->flags : 0UL;
    response->lastStatus = STATUS_SUCCESS;
    KswordARKWin32kInitializeOffsets(&response->fieldOffsets);

    entryCapacity = (OutputBufferLength - headerSize) /
        sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY);
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
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM |
            KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_MODULE_TABLE;
        response->lastStatus = NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
        KswordARKWin32kMessageSetDetail(
            response->detail,
            L"win32kbase/win32kfull could not be located; message hooks were not read.");
        goto Cleanup;
    }

    RtlZeroMemory(&baseHeaders, sizeof(baseHeaders));
    RtlZeroMemory(&fullHeaders, sizeof(fullHeaders));
    if (!KswordARKHookReadImageNtHeaders(&baseEntry, &baseHeaders) ||
        !KswordARKHookReadImageNtHeaders(&fullEntry, &fullHeaders)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM;
        response->lastStatus = STATUS_INVALID_IMAGE_FORMAT;
        KswordARKWin32kMessageSetDetail(
            response->detail,
            L"win32k PE headers could not be read; tagHOOK layout was not guessed.");
        goto Cleanup;
    }

    response->win32kbaseTimeDateStamp = baseHeaders.FileHeader.TimeDateStamp;
    response->win32kbaseImageSize = baseHeaders.OptionalHeader.SizeOfImage;
    response->win32kfullTimeDateStamp = fullHeaders.FileHeader.TimeDateStamp;
    response->win32kfullImageSize = fullHeaders.OptionalHeader.SizeOfImage;
    if (response->win32kbaseTimeDateStamp != KSWORD_ARK_WIN32K_MESSAGE_BASE_TIMESTAMP ||
        response->win32kbaseImageSize != KSWORD_ARK_WIN32K_MESSAGE_BASE_IMAGE_SIZE ||
        response->win32kfullTimeDateStamp != KSWORD_ARK_WIN32K_MESSAGE_FULL_TIMESTAMP ||
        response->win32kfullImageSize != KSWORD_ARK_WIN32K_MESSAGE_FULL_IMAGE_SIZE) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        KswordARKWin32kMessageSetDetail(
            response->detail,
            L"Current win32k PE identity has no verified tagHOOK layout in the offset table.");
        goto Cleanup;
    }

    KswordARKWin32kMessageInitializeLayout(
        &response->layout,
        response->win32kfullTimeDateStamp,
        response->win32kfullImageSize);
    response->fieldOffsets.tagHookNext = response->layout.nextHook;
    response->fieldOffsets.tagHookType = response->layout.hookType;
    response->fieldOffsets.tagHookProcedure = response->layout.procedureOffset;
    response->fieldOffsets.tagHookTargetThreadInfo = response->layout.targetThreadInfo;
    if ((ULONG64)response->layout.moduleAtomTableRva + sizeof(USHORT) >
            (ULONG64)response->win32kfullImageSize ||
        (ULONG64)response->layout.moduleAtomCountRva + sizeof(ULONG) >
            (ULONG64)response->win32kfullImageSize) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_MODULE_TABLE |
            KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM;
        response->lastStatus = STATUS_INVALID_ADDRESS;
        KswordARKWin32kMessageSetDetail(
            response->detail,
            L"Message hook module atom globals are outside the verified win32kfull image.");
        goto Cleanup;
    }

    status = KswordARKWin32kMessageBuildThreadMap(
        &threadMap,
        &threadMapCount,
        &threadMapTruncated);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM;
        response->lastStatus = status;
        KswordARKWin32kMessageSetDetail(
            response->detail,
            L"GUI thread map could not be built; message hook chains were not read.");
        goto Cleanup;
    }

    seenHooks = (KSWORD_ARK_WIN32K_MESSAGE_SEEN_ENTRY*)KswordARKAllocateNonPagedPool(
        sizeof(*seenHooks) * KSWORD_ARK_WIN32K_MESSAGE_MAX_SEEN,
        KSWORD_ARK_WIN32K_MESSAGE_POOL_TAG);
    if (seenHooks == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlZeroMemory(
        seenHooks,
        sizeof(*seenHooks) * KSWORD_ARK_WIN32K_MESSAGE_MAX_SEEN);

    psGetNextProcess = KswordARKWin32kResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKWin32kResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKWin32kResolvePsGetThreadWin32Thread();
    psGetProcessSessionId = KswordARKWin32kResolvePsGetProcessSessionId();
    if (psGetNextProcess == NULL || psGetNextProcessThread == NULL ||
        psGetThreadWin32Thread == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM;
        response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        goto Cleanup;
    }

    if (Request != NULL && Request->sessionId != 0UL) {
        requestedSessionId = Request->sessionId;
    }
    else if (Request != NULL &&
        (Request->flags & KSWORD_ARK_WIN32K_QUERY_FLAG_CURRENT_SESSION_ONLY) != 0UL &&
        psGetProcessSessionId != NULL) {
        requestedSessionId = psGetProcessSessionId(PsGetCurrentProcess());
    }

    response->capabilityMask = KSWORD_ARK_WIN32K_CAP_WIN32KBASE_LOADED |
        KSWORD_ARK_WIN32K_CAP_WIN32KFULL_LOADED |
        KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC |
        KSWORD_ARK_WIN32K_CAP_HOOK_PROFILE |
        KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_LAYOUT |
        KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_ENUM |
        KSWORD_ARK_WIN32K_CAP_MESSAGE_HOOK_MODULE_TABLE;
    response->status = threadMapTruncated
        ? KSWORD_ARK_WIN32K_STATUS_PARTIAL
        : KSWORD_ARK_WIN32K_STATUS_OK;
    response->lastStatus = threadMapTruncated
        ? STATUS_BUFFER_OVERFLOW
        : STATUS_SUCCESS;
    KswordARKWin32kMessageSetDetail(
        response->detail,
        L"tagHOOK layout validated by PE identity; thread and desktop chains are read only and bounded.");

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = STATUS_INVALID_DEVICE_STATE;
        goto Cleanup;
    }

    processCursor = psGetNextProcess(NULL);
    while (processCursor != NULL &&
        processWalkCount < KSWORD_ARK_WIN32K_MESSAGE_MAX_PROCESS_WALK &&
        response->status != KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED) {
        PEPROCESS nextProcess = psGetNextProcess(processCursor);
        PETHREAD threadCursor = NULL;
        ULONG threadWalkCount = 0UL;
        ULONG processId = HandleToULong(PsGetProcessId(processCursor));
        ULONG sessionId = psGetProcessSessionId != NULL
            ? psGetProcessSessionId(processCursor)
            : 0UL;
        DECLSPEC_ALIGN(16) UCHAR attachState[128];
        BOOLEAN attached = FALSE;

        if (requestedSessionId == 0UL || sessionId == requestedSessionId) {
            RtlZeroMemory(attachState, sizeof(attachState));
            KeStackAttachProcess((PVOID)processCursor, attachState);
            attached = TRUE;

            threadCursor = psGetNextProcessThread(processCursor, NULL);
            while (threadCursor != NULL &&
                threadWalkCount < KSWORD_ARK_WIN32K_MESSAGE_MAX_THREAD_WALK &&
                response->status != KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED) {
                PETHREAD nextThread = psGetNextProcessThread(processCursor, threadCursor);
                ULONG threadId = HandleToULong(PsGetThreadId(threadCursor));
                ULONG64 threadInfo = (ULONG64)(ULONG_PTR)psGetThreadWin32Thread(threadCursor);
                ULONG64 desktopInfo = 0ULL;
                ULONG hookIndex = 0UL;

                if (threadInfo != 0ULL &&
                    KswordARKWin32kMessageIsKernelAddress(threadInfo)) {
                    (VOID)KswordARKWin32kMessageReadMemory(
                        threadInfo + response->layout.threadDesktopInfo,
                        &desktopInfo,
                        sizeof(desktopInfo));

                    for (hookIndex = 0UL;
                        hookIndex < KSWORD_ARK_WIN32K_MESSAGE_HOOK_TYPE_COUNT &&
                        response->status != KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
                        ++hookIndex) {
                        LONG hookType = (LONG)hookIndex - 1L;
                        ULONG64 threadChainHead = threadInfo +
                            response->layout.threadHookArray +
                            ((ULONG64)hookIndex * sizeof(ULONG64));

                        KswordARKWin32kMessageWalkChain(
                            response,
                            maxEntries,
                            seenHooks,
                            &seenCount,
                            threadMap,
                            threadMapCount,
                            Request,
                            requestedSessionId,
                            processId,
                            threadId,
                            sessionId,
                            threadInfo,
                            KSWORD_ARK_WIN32K_MESSAGE_HOOK_SOURCE_THREAD,
                            KSWORD_ARK_WIN32K_MESSAGE_HOOK_SCOPE_THREAD,
                            hookType,
                            threadChainHead,
                            (ULONG64)(ULONG_PTR)fullEntry.ImageBase);

                        if (desktopInfo != 0ULL &&
                            KswordARKWin32kMessageIsKernelAddress(desktopInfo)) {
                            ULONG64 desktopChainHead = desktopInfo +
                                response->layout.desktopHookArray +
                                ((ULONG64)hookIndex * sizeof(ULONG64));
                            KswordARKWin32kMessageWalkChain(
                                response,
                                maxEntries,
                                seenHooks,
                                &seenCount,
                                threadMap,
                                threadMapCount,
                                Request,
                                requestedSessionId,
                                processId,
                                threadId,
                                sessionId,
                                threadInfo,
                                KSWORD_ARK_WIN32K_MESSAGE_HOOK_SOURCE_GLOBAL,
                                KSWORD_ARK_WIN32K_MESSAGE_HOOK_SCOPE_GLOBAL,
                                hookType,
                                desktopChainHead,
                                (ULONG64)(ULONG_PTR)fullEntry.ImageBase);
                        }
                    }
                }

                ObDereferenceObject(threadCursor);
                threadCursor = nextThread;
                threadWalkCount += 1UL;
            }
            if (threadCursor != NULL) {
                ObDereferenceObject(threadCursor);
                KswordARKWin32kMessageMarkPartial(response, STATUS_BUFFER_OVERFLOW);
            }
        }

        if (attached) {
            KeUnstackDetachProcess(attachState);
        }
        ObDereferenceObject(processCursor);
        processCursor = nextProcess;
        processWalkCount += 1UL;
    }
    if (processCursor != NULL) {
        ObDereferenceObject(processCursor);
        if (response->status != KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED) {
            KswordARKWin32kMessageMarkPartial(response, STATUS_BUFFER_OVERFLOW);
        }
    }

Cleanup:
    if (seenHooks != NULL) {
        ExFreePoolWithTag(seenHooks, KSWORD_ARK_WIN32K_MESSAGE_POOL_TAG);
    }
    if (threadMap != NULL) {
        ExFreePoolWithTag(threadMap, KSWORD_ARK_WIN32K_MESSAGE_POOL_TAG);
    }
    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }
    UNREFERENCED_PARAMETER(moduleInfoBytes);
    *BytesWrittenOut = headerSize +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY));
    return STATUS_SUCCESS;
}
