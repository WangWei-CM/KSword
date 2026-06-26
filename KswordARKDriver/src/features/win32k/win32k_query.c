/*++

Module Name:

    win32k_query.c

Abstract:

    Read-only win32k GUI audit collectors.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_query.h"
#include "win32k_support.h"

#define KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY))

#define KSWORD_ARK_WIN32K_WINDOW_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_WINDOW_ENTRY))

#define KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY))

#define KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_HOTKEY_ENTRY))

#define KSWORD_ARK_WIN32K_HOOK_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY))

#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif

static NTSTATUS
KswordARKWin32kPrepareProfileHeader(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Outptr_ KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE** ResponseOut,
    _Out_ size_t* EntryCapacityOut
    )
/*++

Routine Description:

    Validate and initialize a Win32k profile/status response header.

Arguments:

    OutputBuffer - METHOD_BUFFERED output.
    OutputBufferLength - Writable output bytes.
    ResponseOut - Receives the response pointer.
    EntryCapacityOut - Receives variable session-entry capacity.

Return Value:

    STATUS_SUCCESS or a buffer validation failure.

--*/
{
    if (OutputBuffer == NULL || ResponseOut == NULL || EntryCapacityOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    *ResponseOut = (KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE*)OutputBuffer;
    *EntryCapacityOut =
        (OutputBufferLength - KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY);
    (*ResponseOut)->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    (*ResponseOut)->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    (*ResponseOut)->entrySize = sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY);
    (*ResponseOut)->lastStatus = STATUS_SUCCESS;
    KswordARKWin32kInitializeOffsets(&(*ResponseOut)->fieldOffsets);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKWin32kQueryProfileStatus(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Query loaded win32k module state and currently observable GUI sessions.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional filter and budget packet.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS on a completed diagnostic response.

--*/
{
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kbaseEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kfullEntry;
    BOOLEAN win32kLoaded = FALSE;
    BOOLEAN win32kbaseLoaded = FALSE;
    BOOLEAN win32kfullLoaded = FALSE;
    KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE* response = NULL;
    size_t entryCapacity = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    status = KswordARKWin32kPrepareProfileHeader(
        OutputBuffer,
        OutputBufferLength,
        &response,
        &entryCapacity);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (NT_SUCCESS(status)) {
        win32kLoaded = KswordARKWin32kFindModuleByName(moduleInfo, "win32k.sys", &win32kEntry);
        win32kbaseLoaded = KswordARKWin32kFindModuleByName(moduleInfo, "win32kbase.sys", &win32kbaseEntry);
        win32kfullLoaded = KswordARKWin32kFindModuleByName(moduleInfo, "win32kfull.sys", &win32kfullEntry);
    }

    KswordARKWin32kFillModuleState(
        &response->win32k,
        L"win32k.sys",
        win32kLoaded,
        win32kLoaded ? &win32kEntry : NULL);
    KswordARKWin32kFillModuleState(
        &response->win32kbase,
        L"win32kbase.sys",
        win32kbaseLoaded,
        win32kbaseLoaded ? &win32kbaseEntry : NULL);
    KswordARKWin32kFillModuleState(
        &response->win32kfull,
        L"win32kfull.sys",
        win32kfullLoaded,
        win32kfullLoaded ? &win32kfullEntry : NULL);

    response->capabilityMask = KswordARKWin32kModuleCapabilityMask(
        win32kLoaded,
        win32kbaseLoaded,
        win32kfullLoaded,
        win32kbaseLoaded ? &win32kbaseEntry : NULL,
        &response->missingCapabilityMask,
        &response->userGetSiloGlobals);

    if (!win32kbaseLoaded || !win32kfullLoaded) {
        response->status = KSWORD_ARK_WIN32K_STATUS_WIN32K_NOT_FOUND;
    }
    else {
        response->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    }

    KswordARKWin32kCollectSessionSummary(response, entryCapacity, Request);

    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }

    *BytesWrittenOut =
        KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY));
    UNREFERENCED_PARAMETER(moduleInfoBytes);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKWin32kQueryWindowSnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Return a profile-gated tagWND snapshot header without guessing private offsets.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional request; currently used only for flags.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS with PROFILE_MISSING until a win32k PDB profile is plumbed.

--*/
{
    KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE* response = NULL;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < KSWORD_ARK_WIN32K_WINDOW_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_WINDOW_ENTRY);
    response->flags = (Request != NULL) ? Request->flags : 0UL;
    response->lastStatus = STATUS_NOT_FOUND;
    response->missingCapabilityMask =
        KSWORD_ARK_WIN32K_CAP_WIN32KBASE_PROFILE |
        KSWORD_ARK_WIN32K_CAP_WIN32KFULL_PROFILE |
        KSWORD_ARK_WIN32K_CAP_TAGWND_PROFILE;
    KswordARKWin32kInitializeOffsets(&response->fieldOffsets);

    *BytesWrittenOut = KSWORD_ARK_WIN32K_WINDOW_RESPONSE_HEADER_SIZE;
    return STATUS_SUCCESS;
}

static VOID
KswordARKWin32kAppendGuiThreadEntry(
    _Inout_ KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE* Response,
    _In_ size_t EntryCapacity,
    _In_ ULONG SessionId,
    _In_ PETHREAD ThreadObject,
    _In_ PVOID ThreadInfo
    )
/*++

Routine Description:

    Append one GUI thread row discovered through PsGetThreadWin32Thread.

Arguments:

    Response - Mutable response packet.
    EntryCapacity - Writable row capacity.
    SessionId - Owning process session id.
    ThreadObject - Referenced ETHREAD from PsGetNextProcessThread.
    ThreadInfo - tagTHREADINFO pointer returned by the kernel.

Return Value:

    None. The response records truncation when capacity is exhausted.

--*/
{
    KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY* entry = NULL;

    if (Response == NULL || ThreadObject == NULL || ThreadInfo == NULL) {
        return;
    }

    Response->totalCount += 1UL;
    if ((size_t)Response->returnedCount >= EntryCapacity) {
        Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
        Response->lastStatus = STATUS_BUFFER_OVERFLOW;
        return;
    }

    entry = &Response->entries[Response->returnedCount];
    RtlZeroMemory(entry, sizeof(*entry));
    entry->fieldFlags = KSWORD_ARK_WIN32K_FIELD_THREADINFO_PRESENT;
    entry->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
    entry->queueStatus = KSWORD_ARK_WIN32K_READ_STATUS_PROFILE_MISSING;
    entry->processId = HandleToULong(PsGetThreadProcessId(ThreadObject));
    entry->threadId = HandleToULong(PsGetThreadId(ThreadObject));
    entry->sessionId = SessionId;
    entry->ethread = (ULONG64)(ULONG_PTR)ThreadObject;
    entry->threadInfo = (ULONG64)(ULONG_PTR)ThreadInfo;
    entry->lastStatus = STATUS_SUCCESS;
    KswordARKWin32kCopyWideText(
        entry->detail,
        KSWORD_ARK_WIN32K_DETAIL_CHARS,
        L"GUI thread found through PsGetThreadWin32Thread; tagQ fields require win32k PDB profile.");
    Response->returnedCount += 1UL;
}

NTSTATUS
KswordARKWin32kQueryGuiThreadSnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Enumerate GUI threads by public kernel walkers and PsGetThreadWin32Thread.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional PID/TID/session filters and max entry budget.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when a bounded snapshot was produced.

--*/
{
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId = NULL;
    KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE* response = NULL;
    size_t entryCapacity = 0U;
    ULONG maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    PEPROCESS processCursor = NULL;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_OK;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY);
    response->flags = (Request != NULL) ? Request->flags : 0UL;
    response->capabilityMask = KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC;
    response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_TAGTHREADINFO_PROFILE | KSWORD_ARK_WIN32K_CAP_TAGQ_PROFILE;
    response->lastStatus = STATUS_SUCCESS;
    KswordARKWin32kInitializeOffsets(&response->fieldOffsets);

    entryCapacity =
        (OutputBufferLength - KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY);
    if (Request != NULL) {
        maxEntries = KswordARKWin32kNormalizeMaxEntries(Request->maxEntries);
    }
    if (entryCapacity > (size_t)maxEntries) {
        entryCapacity = (size_t)maxEntries;
    }
    if ((size_t)maxEntries > entryCapacity) {
        maxEntries = (ULONG)entryCapacity;
    }

    psGetNextProcess = KswordARKWin32kResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKWin32kResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKWin32kResolvePsGetThreadWin32Thread();
    psGetProcessSessionId = KswordARKWin32kResolvePsGetProcessSessionId();
    if (psGetNextProcess == NULL || psGetNextProcessThread == NULL || psGetThreadWin32Thread == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        *BytesWrittenOut = KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    processCursor = psGetNextProcess(NULL);
    while (processCursor != NULL && response->returnedCount < maxEntries) {
        PEPROCESS nextProcess = psGetNextProcess(processCursor);
        ULONG processId = HandleToULong(PsGetProcessId(processCursor));
        ULONG sessionId = (psGetProcessSessionId != NULL) ? psGetProcessSessionId(processCursor) : 0UL;
        PETHREAD threadCursor = NULL;

        if (Request != NULL &&
            Request->processId != 0UL &&
            Request->processId != processId) {
            ObDereferenceObject(processCursor);
            processCursor = nextProcess;
            continue;
        }
        if (Request != NULL &&
            Request->sessionId != 0UL &&
            Request->sessionId != sessionId) {
            ObDereferenceObject(processCursor);
            processCursor = nextProcess;
            continue;
        }

        threadCursor = psGetNextProcessThread(processCursor, NULL);
        while (threadCursor != NULL && response->returnedCount < maxEntries) {
            PETHREAD nextThread = psGetNextProcessThread(processCursor, threadCursor);
            ULONG threadId = HandleToULong(PsGetThreadId(threadCursor));
            PVOID threadInfo = psGetThreadWin32Thread(threadCursor);

            if (Request != NULL &&
                Request->threadId != 0UL &&
                Request->threadId != threadId) {
                ObDereferenceObject(threadCursor);
                threadCursor = nextThread;
                continue;
            }
            if (threadInfo != NULL) {
                KswordARKWin32kAppendGuiThreadEntry(
                    response,
                    entryCapacity,
                    sessionId,
                    threadCursor,
                    threadInfo);
            }
            ObDereferenceObject(threadCursor);
            threadCursor = nextThread;
        }
        if (threadCursor != NULL) {
            ObDereferenceObject(threadCursor);
            threadCursor = NULL;
            response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            response->lastStatus = STATUS_BUFFER_OVERFLOW;
        }

        ObDereferenceObject(processCursor);
        processCursor = nextProcess;
    }

    if (processCursor != NULL) {
        ObDereferenceObject(processCursor);
        processCursor = NULL;
        response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        response->lastStatus = STATUS_BUFFER_OVERFLOW;
    }

    *BytesWrittenOut =
        KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKWin32kQueryHotkeySnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Return a PDB-gated hotkey snapshot header that coexists with keyboard IOCTLs.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional request; currently used only for flags.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS with PROFILE_MISSING until hotkey PDB fields are supplied.

--*/
{
    KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE* response = NULL;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_HOTKEY_ENTRY);
    response->flags = (Request != NULL) ? Request->flags : 0UL;
    response->lastStatus = STATUS_NOT_FOUND;
    response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_HOTKEY_PROFILE;
    KswordARKWin32kInitializeOffsets(&response->fieldOffsets);

    *BytesWrittenOut = KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKWin32kQueryHookSnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Return a PDB-gated window hook snapshot header without walking hardcoded chains.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional request; currently used only for flags.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS with PROFILE_MISSING until hook PDB fields are supplied.

--*/
{
    KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE* response = NULL;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < KSWORD_ARK_WIN32K_HOOK_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY);
    response->flags = (Request != NULL) ? Request->flags : 0UL;
    response->lastStatus = STATUS_NOT_FOUND;
    response->missingCapabilityMask = KSWORD_ARK_WIN32K_CAP_HOOK_PROFILE;
    KswordARKWin32kInitializeOffsets(&response->fieldOffsets);

    *BytesWrittenOut = KSWORD_ARK_WIN32K_HOOK_RESPONSE_HEADER_SIZE;
    return STATUS_SUCCESS;
}
