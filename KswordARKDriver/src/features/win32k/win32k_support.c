/*++

Module Name:

    win32k_support.c

Abstract:

    Shared helper routines for read-only win32k GUI audit collectors.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_support.h"

#include <ntstrsafe.h>

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_z_ PCSTR RoutineName
    );

VOID
KswordARKWin32kInitializeOffsets(
    _Out_ KSWORD_ARK_WIN32K_FIELD_OFFSETS* Offsets
    )
/*++

Routine Description:

    Fill every Win32k private-field offset with the shared unavailable sentinel.

Arguments:

    Offsets - Receives an initialized offset packet.

Return Value:

    None. NULL input is ignored.

--*/
{
    if (Offsets == NULL) {
        return;
    }

    RtlFillMemory(Offsets, sizeof(*Offsets), 0xFF);
}

ULONG
KswordARKWin32kNormalizeMaxEntries(
    _In_ ULONG RequestedMaxEntries
    )
/*++

Routine Description:

    Normalize caller supplied traversal limits for all Win32k snapshots.

Arguments:

    RequestedMaxEntries - Optional caller budget.

Return Value:

    A bounded entry count that is never zero.

--*/
{
    if (RequestedMaxEntries == 0UL) {
        return KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    }
    if (RequestedMaxEntries > KSWORD_ARK_WIN32K_HARD_MAX_ENTRIES) {
        return KSWORD_ARK_WIN32K_HARD_MAX_ENTRIES;
    }
    return RequestedMaxEntries;
}

VOID
KswordARKWin32kCopyWideText(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_z_ PCWSTR Source
    )
/*++

Routine Description:

    Copy a constant WCHAR string into a fixed protocol field.

Arguments:

    Destination - Protocol text field.
    DestinationChars - Protocol text field capacity in WCHARs.
    Source - Constant source string.

Return Value:

    None. The output is always terminated when capacity is nonzero.

--*/
{
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL) {
        return;
    }

    (VOID)RtlStringCchCopyW(Destination, DestinationChars, Source);
    Destination[DestinationChars - 1UL] = L'\0';
}

KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN
KswordARKWin32kResolvePsGetNextProcess(
    VOID
    )
/*++

Routine Description:

    Resolve PsGetNextProcess dynamically for read-only process walking.

Arguments:

    None.

Return Value:

    Function pointer when available; otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcess");
    return (KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKWin32kResolvePsGetNextProcessThread(
    VOID
    )
/*++

Routine Description:

    Resolve PsGetNextProcessThread dynamically for read-only thread walking.

Arguments:

    None.

Return Value:

    Function pointer when available; otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcessThread");
    return (KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN
KswordARKWin32kResolvePsGetThreadWin32Thread(
    VOID
    )
/*++

Routine Description:

    Resolve PsGetThreadWin32Thread for safe GUI-thread discovery.

Arguments:

    None.

Return Value:

    Function pointer when available; otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetThreadWin32Thread");
    return (KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN
KswordARKWin32kResolvePsGetProcessSessionId(
    VOID
    )
/*++

Routine Description:

    Resolve PsGetProcessSessionId so session labels remain optional and fail-soft.

Arguments:

    None.

Return Value:

    Function pointer when exported; otherwise NULL.

--*/
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetProcessSessionId");
    return (KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN)MmGetSystemRoutineAddress(&routineName);
}

BOOLEAN
KswordARKWin32kFindModuleByName(
    _In_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_z_ PCSTR ModuleName,
    _Out_ KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntryOut
    )
/*++

Routine Description:

    Locate one loaded kernel module by basename in a SystemModuleInformation snapshot.

Arguments:

    ModuleInfo - Module snapshot.
    ModuleName - ASCII basename to match.
    ModuleEntryOut - Receives the matching row.

Return Value:

    TRUE on match; FALSE otherwise.

--*/
{
    ULONG moduleIndex = 0UL;

    if (ModuleInfo == NULL || ModuleName == NULL || ModuleEntryOut == NULL) {
        return FALSE;
    }
    RtlZeroMemory(ModuleEntryOut, sizeof(*ModuleEntryOut));

    for (moduleIndex = 0UL; moduleIndex < ModuleInfo->NumberOfModules; ++moduleIndex) {
        const KSW_HOOK_SYSTEM_MODULE_ENTRY* moduleEntry = &ModuleInfo->Modules[moduleIndex];
        const UCHAR* fileName = NULL;
        ULONG fileNameBytes = 0UL;

        KswordARKHookGetModuleFileName(moduleEntry, &fileName, &fileNameBytes);
        if (KswordARKHookBoundedAnsiEqualsInsensitive(fileName, fileNameBytes, ModuleName)) {
            RtlCopyMemory(ModuleEntryOut, moduleEntry, sizeof(*ModuleEntryOut));
            return TRUE;
        }
    }

    return FALSE;
}

VOID
KswordARKWin32kFillModuleState(
    _Out_ KSWORD_ARK_WIN32K_MODULE_STATE* ModuleState,
    _In_z_ PCWSTR ModuleName,
    _In_ BOOLEAN Loaded,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry
    )
/*++

Routine Description:

    Populate one fixed module-state row for win32k, win32kbase, or win32kfull.

Arguments:

    ModuleState - Output module-state packet.
    ModuleName - Protocol display name.
    Loaded - TRUE when SystemModuleInformation contained this module.
    ModuleEntry - Optional loaded module evidence.

Return Value:

    None. Invalid output input is ignored.

--*/
{
    if (ModuleState == NULL) {
        return;
    }

    RtlZeroMemory(ModuleState, sizeof(*ModuleState));
    ModuleState->loaded = Loaded ? 1UL : 0UL;
    ModuleState->profileState = Loaded
        ? KSWORD_ARK_WIN32K_PROFILE_STATE_MISSING
        : KSWORD_ARK_WIN32K_PROFILE_STATE_NOT_LOADED;
    KswordARKWin32kCopyWideText(
        ModuleState->moduleName,
        KSWORD_ARK_WIN32K_MODULE_NAME_CHARS,
        ModuleName);

    if (Loaded && ModuleEntry != NULL) {
        ModuleState->imageBase = (ULONG64)(ULONG_PTR)ModuleEntry->ImageBase;
        ModuleState->imageSize = ModuleEntry->ImageSize;
    }
}

ULONG64
KswordARKWin32kModuleCapabilityMask(
    _In_ BOOLEAN Win32kLoaded,
    _In_ BOOLEAN Win32kbaseLoaded,
    _In_ BOOLEAN Win32kfullLoaded,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* Win32kbaseEntry,
    _Out_ ULONG64* MissingCapabilityMaskOut,
    _Out_ ULONG64* UserGetSiloGlobalsOut
    )
/*++

Routine Description:

    Build current module/profile capability evidence without claiming profile support.

Arguments:

    Win32kLoaded - win32k.sys module presence.
    Win32kbaseLoaded - win32kbase.sys module presence.
    Win32kfullLoaded - win32kfull.sys module presence.
    Win32kbaseEntry - Optional win32kbase module entry for export lookup.
    MissingCapabilityMaskOut - Receives PDB capability gaps.
    UserGetSiloGlobalsOut - Receives resolved UserGetSiloGlobals address.

Return Value:

    Capability bitmask for the current kernel state.

--*/
{
    ULONG64 capabilityMask = 0ULL;
    ULONG64 missingCapabilityMask = 0ULL;
    ULONG_PTR userGetSiloGlobals = 0U;

    if (Win32kLoaded) {
        capabilityMask |= KSWORD_ARK_WIN32K_CAP_WIN32K_LOADED;
    }
    if (Win32kbaseLoaded) {
        capabilityMask |= KSWORD_ARK_WIN32K_CAP_WIN32KBASE_LOADED;
    }
    if (Win32kfullLoaded) {
        capabilityMask |= KSWORD_ARK_WIN32K_CAP_WIN32KFULL_LOADED;
    }

    if (Win32kbaseLoaded && Win32kbaseEntry != NULL && Win32kbaseEntry->ImageBase != NULL) {
        userGetSiloGlobals = (ULONG_PTR)RtlFindExportedRoutineByName(
            Win32kbaseEntry->ImageBase,
            "UserGetSiloGlobals");
        if (userGetSiloGlobals != 0U) {
            capabilityMask |= KSWORD_ARK_WIN32K_CAP_USER_GET_SILO_GLOBALS;
        }
    }

    if (KswordARKWin32kResolvePsGetThreadWin32Thread() != NULL) {
        capabilityMask |= KSWORD_ARK_WIN32K_CAP_THREADINFO_PUBLIC;
    }

    missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_WIN32KBASE_PROFILE;
    missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_WIN32KFULL_PROFILE;
    missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_TAGWND_PROFILE;
    missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_TAGTHREADINFO_PROFILE;
    missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_TAGQ_PROFILE;
    missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_HOTKEY_PROFILE;
    missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_HOOK_PROFILE;

    if (MissingCapabilityMaskOut != NULL) {
        *MissingCapabilityMaskOut = missingCapabilityMask;
    }
    if (UserGetSiloGlobalsOut != NULL) {
        *UserGetSiloGlobalsOut = (ULONG64)userGetSiloGlobals;
    }
    return capabilityMask;
}

static KSWORD_ARK_WIN32K_SESSION_ENTRY*
KswordARKWin32kFindOrAppendSession(
    _Inout_ KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE* Response,
    _In_ size_t EntryCapacity,
    _In_ ULONG SessionId
    )
/*++

Routine Description:

    Return a per-session summary row, appending it when first observed.

Arguments:

    Response - Mutable profile/status response.
    EntryCapacity - Writable session-entry capacity.
    SessionId - Session id from PsGetProcessSessionId.

Return Value:

    Session row pointer, or NULL if the output buffer is full.

--*/
{
    ULONG index = 0UL;

    if (Response == NULL) {
        return NULL;
    }

    for (index = 0UL; index < Response->returnedCount; ++index) {
        if (Response->entries[index].sessionId == SessionId) {
            return &Response->entries[index];
        }
    }

    Response->totalCount += 1UL;
    if ((size_t)Response->returnedCount >= EntryCapacity) {
        Response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
        return NULL;
    }

    RtlZeroMemory(&Response->entries[Response->returnedCount], sizeof(Response->entries[0]));
    Response->entries[Response->returnedCount].sessionId = SessionId;
    Response->entries[Response->returnedCount].status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    Response->entries[Response->returnedCount].capabilityMask = Response->capabilityMask;
    Response->entries[Response->returnedCount].lastStatus = STATUS_SUCCESS;
    KswordARKWin32kCopyWideText(
        Response->entries[Response->returnedCount].detail,
        KSWORD_ARK_WIN32K_DETAIL_CHARS,
        L"Session observed; win32k PDB profile is not loaded in this R0 skeleton.");
    Response->returnedCount += 1UL;
    return &Response->entries[Response->returnedCount - 1UL];
}

VOID
KswordARKWin32kCollectSessionSummary(
    _Inout_ KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE* Response,
    _In_ size_t EntryCapacity,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request
    )
/*++

Routine Description:

    Build a bounded per-session summary from PsGetNextProcess/PsGetNextProcessThread.

Arguments:

    Response - Mutable profile/status response.
    EntryCapacity - Writable session-entry capacity.
    Request - Optional caller filter.

Return Value:

    None. Failures are recorded in the response status and lastStatus fields.

--*/
{
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId = NULL;
    PEPROCESS processCursor = NULL;
    ULONG maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    ULONG visitedProcesses = 0UL;

    psGetNextProcess = KswordARKWin32kResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKWin32kResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKWin32kResolvePsGetThreadWin32Thread();
    psGetProcessSessionId = KswordARKWin32kResolvePsGetProcessSessionId();
    if (psGetNextProcess == NULL ||
        psGetNextProcessThread == NULL ||
        psGetThreadWin32Thread == NULL ||
        psGetProcessSessionId == NULL) {
        Response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        return;
    }

    if (Request != NULL) {
        maxEntries = KswordARKWin32kNormalizeMaxEntries(Request->maxEntries);
    }

    processCursor = psGetNextProcess(NULL);
    while (processCursor != NULL && visitedProcesses < maxEntries) {
        PEPROCESS nextProcess = psGetNextProcess(processCursor);
        ULONG sessionId = psGetProcessSessionId(processCursor);
        KSWORD_ARK_WIN32K_SESSION_ENTRY* sessionEntry = NULL;
        PETHREAD threadCursor = NULL;

        if (Request != NULL &&
            Request->sessionId != 0UL &&
            Request->sessionId != sessionId) {
            ObDereferenceObject(processCursor);
            processCursor = nextProcess;
            visitedProcesses += 1UL;
            continue;
        }

        sessionEntry = KswordARKWin32kFindOrAppendSession(Response, EntryCapacity, sessionId);
        if (sessionEntry != NULL) {
            sessionEntry->processCount += 1UL;
            if (sessionEntry->representativeProcessId == 0UL) {
                sessionEntry->representativeProcessId = HandleToULong(PsGetProcessId(processCursor));
            }

            threadCursor = psGetNextProcessThread(processCursor, NULL);
            while (threadCursor != NULL) {
                PETHREAD nextThread = psGetNextProcessThread(processCursor, threadCursor);
                PVOID threadInfo = psGetThreadWin32Thread(threadCursor);

                if (threadInfo != NULL) {
                    sessionEntry->guiThreadCount += 1UL;
                    if (sessionEntry->representativeThreadId == 0UL) {
                        sessionEntry->representativeThreadId = HandleToULong(PsGetThreadId(threadCursor));
                    }
                }
                ObDereferenceObject(threadCursor);
                threadCursor = nextThread;
            }
        }

        ObDereferenceObject(processCursor);
        processCursor = nextProcess;
        visitedProcesses += 1UL;
    }

    if (processCursor != NULL) {
        ObDereferenceObject(processCursor);
        processCursor = NULL;
        Response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
        Response->lastStatus = STATUS_BUFFER_OVERFLOW;
    }
}

