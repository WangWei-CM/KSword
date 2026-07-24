/*++

Module Name:

    win32k_event_hook.c

Abstract:

    Read-only enumeration of tagEVENTHOOK objects through
    win32kbase!gpWinEventHooks. The collector prefers exact PE identity and can
    fall back to the nearest previous Windows layout. It never changes a node.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_query.h"
#include "win32k_support.h"
#include "../../platform/pool_compat.h"

#include <ntimage.h>
#include <ntstrsafe.h>

#define KSWORD_ARK_WIN32K_EVENT_POOL_TAG 'eWkW'
#define KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE 0x60UL
#define KSWORD_ARK_WIN32K_EVENT_GLOBAL_RVA 0x00257EE8UL
#define KSWORD_ARK_WIN32K_EVENT_MAX_THREAD_MAP 8192UL
#define KSWORD_ARK_WIN32K_EVENT_MAX_SEEN 8192UL
#define KSWORD_ARK_WIN32K_EVENT_INTERNAL_PUBLIC_MASK 0x0000001EUL
#define KSWORD_ARK_WIN32K_EVENT_INTERNAL_IN_CONTEXT 0x00000008UL

typedef struct _KSWORD_ARK_WIN32K_EVENT_PROFILE
{
    KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY identity;
    KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT layout;
} KSWORD_ARK_WIN32K_EVENT_PROFILE;

// 按 Windows 版本从旧到新维护；精确身份缺失时选择最近的上一个版本。
static const KSWORD_ARK_WIN32K_EVENT_PROFILE g_KswordArkWin32kEventProfiles[] =
{
    {
        // Windows 10 22H2 reports 19045 through the enablement package while
        // win32k stays on the 19041 servicing branch.
        { 10UL, 0UL, 19041UL, 6456UL,
          0x8FC48444UL, 0x002D6000UL, 0x83C73BE4UL, 0x003B4000UL },
        { KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE,
          0x00UL, 0x10UL, 0x18UL, 0x20UL, 0x24UL, 0x28UL,
          0x30UL, 0x38UL, 0x40UL, 0x48UL, 0x58UL,
          KSWORD_ARK_WIN32K_EVENT_GLOBAL_RVA,
          KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT_SOURCE_UNKNOWN,
          0x83C73BE4UL, 0x003B4000UL }
    }
};

typedef KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY
    KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY;

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

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_z_ PCSTR RoutineName
    );

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
    _In_ const KSWORD_ARK_WIN32K_EVENT_PROFILE* Profile,
    _In_ ULONG Source
    )
{
    if (Layout == NULL || Profile == NULL) {
        return;
    }
    *Layout = Profile->layout;
    Layout->source = Source == KSWORD_ARK_WIN32K_LAYOUT_SELECTION_EXACT_IDENTITY
        ? KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY
        : KSWORD_ARK_WIN32K_EVENT_HOOK_LAYOUT_SOURCE_NEAREST_PREVIOUS;
}

static NTSTATUS
KswordARKWin32kEventBuildThreadMap(
    _Outptr_result_buffer_(*CountOut) KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY** MapOut,
    _Out_ ULONG* CountOut,
    _Out_ BOOLEAN* TruncatedOut
    )
{
    return KswordARKWin32kBuildGuiThreadMap(
        KSWORD_ARK_WIN32K_EVENT_MAX_THREAD_MAP,
        KSWORD_ARK_WIN32K_EVENT_POOL_TAG,
        MapOut,
        CountOut,
        TruncatedOut);
}

static NTSTATUS
KswordARKWin32kEventReferenceSessionProcess(
    _In_reads_(ThreadMapCount) const KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY* ThreadMap,
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
            const KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY* entry = &ThreadMap[index];
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
KswordARKWin32kEventLookupThread(
    _In_reads_(ThreadMapCount) const KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY* ThreadMap,
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
            (PreferredSessionId == 0UL ||
                ThreadMap[index].SessionId == PreferredSessionId)) {
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
    _In_ ULONG ProcessId,
    _In_ ULONG ThreadId,
    _In_ BOOLEAN OwnerResolved
    )
{
    if (Request != NULL && Request->processId != 0UL &&
        (!OwnerResolved || Request->processId != ProcessId)) {
        return FALSE;
    }
    if (Request != NULL && Request->threadId != 0UL &&
        (!OwnerResolved || Request->threadId != ThreadId)) {
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
    KSWORD_ARK_WIN32K_LAYOUT_SELECTION layoutSelection;
    const KSWORD_ARK_WIN32K_EVENT_PROFILE* layoutProfile = NULL;
    KSWORD_ARK_WIN32K_EVENT_HOOK_SNAPSHOT_RESPONSE* response = NULL;
    KSWORD_ARK_WIN32K_EVENT_THREAD_MAP_ENTRY* threadMap = NULL;
    ULONG64* seenHooks = NULL;
    PEPROCESS sessionProcess = NULL;
    PVOID hookListExport = NULL;
    ULONG64 hookListPointer = 0ULL;
    ULONG64 currentHook = 0ULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG threadMapCount = 0UL;
    ULONG seenCount = 0UL;
    ULONG maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    ULONG requestedSessionId = 0UL;
    ULONG unresolvedOwnerCount = 0UL;
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
    if (!KswordARKWin32kSelectLayoutProfile(
            g_KswordArkWin32kEventProfiles,
            RTL_NUMBER_OF(g_KswordArkWin32kEventProfiles),
            sizeof(g_KswordArkWin32kEventProfiles[0]),
            response->win32kbaseTimeDateStamp,
            response->win32kbaseImageSize,
            response->win32kfullTimeDateStamp,
            response->win32kfullImageSize,
            &layoutSelection)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_LAYOUT |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"No exact or nearest-previous Windows tagEVENTHOOK profile is available.");
        goto Cleanup;
    }
    layoutProfile = &g_KswordArkWin32kEventProfiles[layoutSelection.profileIndex];

    KswordARKWin32kEventInitializeLayout(
        &response->layout,
        layoutProfile,
        layoutSelection.source);
    if (response->layout.objectSize == 0UL ||
        response->layout.objectSize > KSWORD_ARK_WIN32K_EVENT_OBJECT_SIZE) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = STATUS_INVALID_PARAMETER;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"Selected tagEVENTHOOK profile failed local object-size validation.");
        goto Cleanup;
    }

    status = KswordARKWin32kEventBuildThreadMap(
        &threadMap,
        &threadMapCount,
        &threadMapTruncated);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = status;
        response->missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC;
        KswordARKWin32kEventSetDetail(
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

    status = KswordARKWin32kEventReferenceSessionProcess(
        threadMap,
        threadMapCount,
        Request,
        &requestedSessionId,
        &sessionProcess);
    if (!NT_SUCCESS(status) || sessionProcess == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
        response->missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        KswordARKWin32kEventSetDetail(
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

    hookListExport = RtlFindExportedRoutineByName(
        baseEntry.ImageBase,
        "gpWinEventHooks");
    hookListPointer = (ULONG64)(ULONG_PTR)hookListExport;
    if (hookListExport == NULL ||
        !KswordARKWin32kEventIsKernelAddress(hookListPointer) ||
        hookListPointer < (ULONG64)(ULONG_PTR)baseEntry.ImageBase ||
        hookListPointer - (ULONG64)(ULONG_PTR)baseEntry.ImageBase > MAXULONG ||
        hookListPointer - (ULONG64)(ULONG_PTR)baseEntry.ImageBase + sizeof(ULONG64) >
            (ULONG64)response->win32kbaseImageSize) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
            KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
        response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        KswordARKWin32kEventSetDetail(
            response->detail,
            L"win32kbase does not expose a valid gpWinEventHooks data export.");
        goto Cleanup;
    }
    response->layout.globalRva = (ULONG)(hookListPointer -
        (ULONG64)(ULONG_PTR)baseEntry.ImageBase);
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
            L"gpWinEventHooks could not be read after attaching the requested GUI session.");
        goto Cleanup;
    }

    response->hookListHead = currentHook;
    response->capabilityMask = KSWORD_ARK_WIN32K_CAP_WIN32KBASE_LOADED |
        KSWORD_ARK_WIN32K_CAP_WIN32KFULL_LOADED |
        KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC |
        KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_GLOBAL |
        KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_LAYOUT |
        KSWORD_ARK_WIN32K_CAP_EVENT_HOOK_ENUM;
    response->status = threadMapTruncated
        ? KSWORD_ARK_WIN32K_STATUS_PARTIAL
        : KSWORD_ARK_WIN32K_STATUS_OK;
    response->lastStatus = threadMapTruncated
        ? STATUS_BUFFER_OVERFLOW
        : STATUS_SUCCESS;
    KswordARKWin32kEventSetDetail(
        response->detail,
        layoutSelection.source == KSWORD_ARK_WIN32K_LAYOUT_SELECTION_EXACT_IDENTITY
            ? L"tagEVENTHOOK layout matched the exact PE identity; gpWinEventHooks is being read in the requested GUI session."
            : L"The nearest previous tagEVENTHOOK layout and the current gpWinEventHooks export are active in the requested GUI session; results may be partial.");

    seenHooks = (ULONG64*)KswordARKAllocateNonPagedPool(
        sizeof(*seenHooks) * KSWORD_ARK_WIN32K_EVENT_MAX_SEEN,
        KSWORD_ARK_WIN32K_EVENT_POOL_TAG);
    if (seenHooks == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlZeroMemory(seenHooks, sizeof(*seenHooks) * KSWORD_ARK_WIN32K_EVENT_MAX_SEEN);

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
            requestedSessionId,
            &processId,
            &threadId,
            &sessionId);
        if (!ownerResolved) {
            unresolvedOwnerCount += 1UL;
            // gpWinEventHooks is session-local. Preserve the attached session
            // so an unresolved private pti field does not discard the node.
            processId = 0UL;
            threadId = 0UL;
            sessionId = requestedSessionId;
        }

        if (KswordARKWin32kEventMatchesRequest(
                Request,
                processId,
                threadId,
                ownerResolved)) {
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

    (VOID)RtlStringCchPrintfW(
        response->detail,
        KSWORD_ARK_WIN32K_DETAIL_CHARS,
        L"collector=session-filter-v2; session=%lu, requestPid=%lu, requestTid=%lu, threadMap=%lu, visited=%lu, accepted=%lu, unresolved=%lu.",
        requestedSessionId,
        Request != NULL ? Request->processId : 0UL,
        Request != NULL ? Request->threadId : 0UL,
        threadMapCount,
        response->visitedNodeCount,
        response->totalCount,
        unresolvedOwnerCount);

Cleanup:
    if (attached) {
        KeUnstackDetachProcess(attachState);
        attached = FALSE;
    }
    if (sessionProcess != NULL) {
        ObDereferenceObject(sessionProcess);
        sessionProcess = NULL;
    }
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
