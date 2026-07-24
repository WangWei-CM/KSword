/*++

Module Name:

    win32k_support.c

Abstract:

    Shared helper routines for read-only win32k GUI audit collectors.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_support.h"
#include "../../platform/pool_compat.h"

#include <ntstrsafe.h>

#define KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION_CLASS 5UL
#define KSWORD_ARK_WIN32K_PROCESS_SNAPSHOT_SLACK (64UL * 1024UL)
#define KSWORD_ARK_WIN32K_PROCESS_SNAPSHOT_LIMIT (64UL * 1024UL * 1024UL)

typedef struct _KSWORD_ARK_WIN32K_SYSTEM_THREAD_INFORMATION
{
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    KPRIORITY Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
} KSWORD_ARK_WIN32K_SYSTEM_THREAD_INFORMATION;

typedef struct _KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    UCHAR Reserved1[48];
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    PVOID Reserved2;
    ULONG HandleCount;
    ULONG SessionId;
    PVOID Reserved3;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG Reserved4;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    PVOID Reserved5;
    SIZE_T QuotaPagedPoolUsage;
    PVOID Reserved6;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER Reserved7[6];
    KSWORD_ARK_WIN32K_SYSTEM_THREAD_INFORMATION Threads[1];
} KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION;

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

NTKERNELAPI
NTSTATUS
PsLookupThreadByThreadId(
    _In_ HANDLE ThreadId,
    _Outptr_ PETHREAD* Thread
    );

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_z_ PCSTR RoutineName
    );

static BOOLEAN
KswordARKWin32kQueryWindowsRevision(
    _Out_ ULONG* RevisionOut
    )
{
    UNICODE_STRING keyName;
    UNICODE_STRING valueName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    UCHAR valueBuffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = { 0 };
    PKEY_VALUE_PARTIAL_INFORMATION valueInfo =
        (PKEY_VALUE_PARTIAL_INFORMATION)valueBuffer;
    ULONG resultLength = 0UL;
    ULONG revision = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (RevisionOut == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return FALSE;
    }
    *RevisionOut = 0UL;

    RtlInitUnicodeString(
        &keyName,
        L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    RtlInitUnicodeString(&valueName, L"UBR");
    InitializeObjectAttributes(
        &attributes,
        &keyName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);
    status = ZwOpenKey(&keyHandle, KEY_QUERY_VALUE, &attributes);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    status = ZwQueryValueKey(
        keyHandle,
        &valueName,
        KeyValuePartialInformation,
        valueBuffer,
        sizeof(valueBuffer),
        &resultLength);
    ZwClose(keyHandle);
    if (!NT_SUCCESS(status) ||
        resultLength < FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG) ||
        valueInfo->Type != REG_DWORD ||
        valueInfo->DataLength < sizeof(ULONG)) {
        return FALSE;
    }

    RtlCopyMemory(&revision, valueInfo->Data, sizeof(revision));
    *RevisionOut = revision;
    return TRUE;
}

static LONG
KswordARKWin32kCompareProfileVersions(
    _In_ const KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY* Left,
    _In_ const KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY* Right
    )
{
#define KSW_COMPARE_PROFILE_FIELD(FieldName) \
    if (Left->FieldName < Right->FieldName) { return -1; } \
    if (Left->FieldName > Right->FieldName) { return 1; }
    KSW_COMPARE_PROFILE_FIELD(windowsMajorVersion);
    KSW_COMPARE_PROFILE_FIELD(windowsMinorVersion);
    KSW_COMPARE_PROFILE_FIELD(windowsBuildNumber);
    KSW_COMPARE_PROFILE_FIELD(windowsRevision);
#undef KSW_COMPARE_PROFILE_FIELD
    return 0;
}

BOOLEAN
KswordARKWin32kSelectLayoutProfile(
    _In_reads_bytes_(ProfileCount * ProfileStride) const VOID* ProfileTable,
    _In_ ULONG ProfileCount,
    _In_ SIZE_T ProfileStride,
    _In_ ULONG CurrentWin32kbaseTimeDateStamp,
    _In_ ULONG CurrentWin32kbaseImageSize,
    _In_ ULONG CurrentWin32kfullTimeDateStamp,
    _In_ ULONG CurrentWin32kfullImageSize,
    _Out_ KSWORD_ARK_WIN32K_LAYOUT_SELECTION* Selection
    )
/*++

Routine Description:

    Select an exact Win32k PE layout first. If no exact identity exists, use
    the newest profile whose Windows version is not newer than the running
    system. PE timestamps are deliberately not ordered because modern Windows
    images use reproducible-build hashes rather than chronological timestamps.

--*/
{
    const UCHAR* profileBytes = (const UCHAR*)ProfileTable;
    KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY currentVersion;
    const KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY* bestProfile = NULL;
    ULONG bestIndex = 0UL;
    ULONG index = 0UL;

    if (ProfileTable == NULL || ProfileCount == 0UL ||
        ProfileStride < sizeof(KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY) ||
        Selection == NULL) {
        return FALSE;
    }
    RtlZeroMemory(Selection, sizeof(*Selection));

    for (index = 0UL; index < ProfileCount; ++index) {
        const KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY* profile =
            (const KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY*)(profileBytes +
                ((SIZE_T)index * ProfileStride));
        if (profile->win32kbaseTimeDateStamp == CurrentWin32kbaseTimeDateStamp &&
            profile->win32kbaseImageSize == CurrentWin32kbaseImageSize &&
            profile->win32kfullTimeDateStamp == CurrentWin32kfullTimeDateStamp &&
            profile->win32kfullImageSize == CurrentWin32kfullImageSize) {
            Selection->profileIndex = index;
            Selection->source = KSWORD_ARK_WIN32K_LAYOUT_SELECTION_EXACT_IDENTITY;
            Selection->selectedIdentity = *profile;
            (VOID)PsGetVersion(
                &Selection->currentWindowsMajorVersion,
                &Selection->currentWindowsMinorVersion,
                &Selection->currentWindowsBuildNumber,
                NULL);
            (VOID)KswordARKWin32kQueryWindowsRevision(
                &Selection->currentWindowsRevision);
            return TRUE;
        }
    }

    RtlZeroMemory(&currentVersion, sizeof(currentVersion));
    (VOID)PsGetVersion(
        &currentVersion.windowsMajorVersion,
        &currentVersion.windowsMinorVersion,
        &currentVersion.windowsBuildNumber,
        NULL);
    if (!KswordARKWin32kQueryWindowsRevision(&currentVersion.windowsRevision)) {
        currentVersion.windowsRevision = MAXULONG;
    }
    if (currentVersion.windowsMajorVersion == 0UL ||
        currentVersion.windowsBuildNumber == 0UL) {
        return FALSE;
    }

    for (index = 0UL; index < ProfileCount; ++index) {
        const KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY* profile =
            (const KSWORD_ARK_WIN32K_LAYOUT_PROFILE_IDENTITY*)(profileBytes +
                ((SIZE_T)index * ProfileStride));
        if (KswordARKWin32kCompareProfileVersions(profile, &currentVersion) > 0) {
            continue;
        }
        if (bestProfile == NULL ||
            KswordARKWin32kCompareProfileVersions(profile, bestProfile) >= 0) {
            bestProfile = profile;
            bestIndex = index;
        }
    }
    if (bestProfile == NULL) {
        return FALSE;
    }

    Selection->profileIndex = bestIndex;
    Selection->source = KSWORD_ARK_WIN32K_LAYOUT_SELECTION_NEAREST_PREVIOUS;
    Selection->currentWindowsMajorVersion = currentVersion.windowsMajorVersion;
    Selection->currentWindowsMinorVersion = currentVersion.windowsMinorVersion;
    Selection->currentWindowsBuildNumber = currentVersion.windowsBuildNumber;
    Selection->currentWindowsRevision = currentVersion.windowsRevision;
    Selection->selectedIdentity = *bestProfile;
    return TRUE;
}

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

static NTSTATUS
KswordARKWin32kCaptureProcessSnapshot(
    _Outptr_result_bytebuffer_(*SnapshotBytesOut) PVOID* SnapshotOut,
    _Out_ ULONG* SnapshotBytesOut
    )
{
    PVOID snapshot = NULL;
    ULONG requiredBytes = 0UL;
    ULONG allocationBytes = 0UL;
    ULONG returnedBytes = 0UL;
    ULONG attempt = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (SnapshotOut == NULL || SnapshotBytesOut == NULL ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    *SnapshotOut = NULL;
    *SnapshotBytesOut = 0UL;

    status = ZwQuerySystemInformation(
        KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION_CLASS,
        NULL,
        0UL,
        &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH &&
        status != STATUS_BUFFER_TOO_SMALL &&
        !NT_SUCCESS(status)) {
        return status;
    }

    for (attempt = 0UL; attempt < 3UL; ++attempt) {
        if (requiredBytes < sizeof(KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION)) {
            requiredBytes = 256UL * 1024UL;
        }
        if (requiredBytes > KSWORD_ARK_WIN32K_PROCESS_SNAPSHOT_LIMIT ||
            requiredBytes > MAXULONG - KSWORD_ARK_WIN32K_PROCESS_SNAPSHOT_SLACK) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        allocationBytes = requiredBytes + KSWORD_ARK_WIN32K_PROCESS_SNAPSHOT_SLACK;
        snapshot = KswordARKAllocateNonPagedPool(
            allocationBytes,
            'sWkW');
        if (snapshot == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(snapshot, allocationBytes);

        returnedBytes = 0UL;
        status = ZwQuerySystemInformation(
            KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION_CLASS,
            snapshot,
            allocationBytes,
            &returnedBytes);
        if (NT_SUCCESS(status)) {
            if (returnedBytes == 0UL || returnedBytes > allocationBytes) {
                returnedBytes = allocationBytes;
            }
            *SnapshotOut = snapshot;
            *SnapshotBytesOut = returnedBytes;
            return STATUS_SUCCESS;
        }

        ExFreePoolWithTag(snapshot, 'sWkW');
        snapshot = NULL;
        if (status != STATUS_INFO_LENGTH_MISMATCH &&
            status != STATUS_BUFFER_TOO_SMALL) {
            return status;
        }
        requiredBytes = returnedBytes > allocationBytes
            ? returnedBytes
            : allocationBytes;
    }

    return status;
}

NTSTATUS
KswordARKWin32kBuildGuiThreadMap(
    _In_ ULONG MaximumEntries,
    _In_ ULONG PoolTag,
    _Outptr_result_buffer_(*CountOut) KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY** MapOut,
    _Out_ ULONG* CountOut,
    _Out_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    Build a bounded map from PsGetThreadWin32Thread values to PID, TID and
    Session ID. Older kernels with public process walkers use those walkers.
    Current kernels that do not export PsGetNextProcess fall back to the stable
    SystemProcessInformation PID/TID snapshot and PsLookupThreadByThreadId.

--*/
{
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_WIN32K_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_WIN32K_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN psGetProcessSessionId = NULL;
    KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY* map = NULL;
    ULONG count = 0UL;
    BOOLEAN truncated = FALSE;

    if (MapOut == NULL || CountOut == NULL || TruncatedOut == NULL ||
        MaximumEntries == 0UL ||
        MaximumEntries > MAXULONG / sizeof(*map) ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    *MapOut = NULL;
    *CountOut = 0UL;
    *TruncatedOut = FALSE;

    map = (KSWORD_ARK_WIN32K_GUI_THREAD_MAP_ENTRY*)KswordARKAllocateNonPagedPool(
        sizeof(*map) * MaximumEntries,
        PoolTag);
    if (map == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(map, sizeof(*map) * MaximumEntries);

    psGetNextProcess = KswordARKWin32kResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKWin32kResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKWin32kResolvePsGetThreadWin32Thread();
    psGetProcessSessionId = KswordARKWin32kResolvePsGetProcessSessionId();
    if (psGetThreadWin32Thread == NULL) {
        ExFreePoolWithTag(map, PoolTag);
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (psGetNextProcess != NULL && psGetNextProcessThread != NULL) {
        PEPROCESS processCursor = psGetNextProcess(NULL);

        while (processCursor != NULL) {
            PEPROCESS nextProcess = psGetNextProcess(processCursor);
            PETHREAD threadCursor = psGetNextProcessThread(processCursor, NULL);
            ULONG processId = HandleToULong(PsGetProcessId(processCursor));
            ULONG sessionId = psGetProcessSessionId != NULL
                ? psGetProcessSessionId(processCursor)
                : 0UL;

            while (threadCursor != NULL) {
                PETHREAD nextThread = psGetNextProcessThread(processCursor, threadCursor);
                PVOID threadInfo = psGetThreadWin32Thread(threadCursor);

                if (threadInfo != NULL) {
                    if (count >= MaximumEntries) {
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
    }
    else {
        PVOID snapshot = NULL;
        ULONG snapshotBytes = 0UL;
        ULONG processOffset = 0UL;
        NTSTATUS status = KswordARKWin32kCaptureProcessSnapshot(
            &snapshot,
            &snapshotBytes);

        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(map, PoolTag);
            return status;
        }

#if defined(_WIN64)
        C_ASSERT(FIELD_OFFSET(KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION, Threads) == 0x100);
        C_ASSERT(sizeof(KSWORD_ARK_WIN32K_SYSTEM_THREAD_INFORMATION) == 0x50);
#endif
        while (processOffset < snapshotBytes) {
            KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION* processInfo =
                (KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION*)((PUCHAR)snapshot + processOffset);
            ULONG remainingBytes = snapshotBytes - processOffset;
            ULONG entryBytes = processInfo->NextEntryOffset != 0UL
                ? processInfo->NextEntryOffset
                : remainingBytes;
            ULONG threadCapacity = 0UL;
            ULONG threadCount = 0UL;
            ULONG threadIndex = 0UL;

            if (remainingBytes < (ULONG)FIELD_OFFSET(KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION, Threads) ||
                entryBytes < (ULONG)FIELD_OFFSET(KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION, Threads) ||
                entryBytes > remainingBytes) {
                truncated = TRUE;
                break;
            }
            threadCapacity = (entryBytes -
                FIELD_OFFSET(KSWORD_ARK_WIN32K_SYSTEM_PROCESS_INFORMATION, Threads)) /
                sizeof(KSWORD_ARK_WIN32K_SYSTEM_THREAD_INFORMATION);
            threadCount = processInfo->NumberOfThreads;
            if (threadCount > threadCapacity) {
                threadCount = threadCapacity;
                truncated = TRUE;
            }

            for (threadIndex = 0UL; threadIndex < threadCount; ++threadIndex) {
                PETHREAD threadObject = NULL;
                HANDLE threadId = processInfo->Threads[threadIndex].ClientId.UniqueThread;

                status = PsLookupThreadByThreadId(threadId, &threadObject);
                if (NT_SUCCESS(status) && threadObject != NULL) {
                    PVOID threadInfo = psGetThreadWin32Thread(threadObject);

                    if (threadInfo != NULL) {
                        if (count >= MaximumEntries) {
                            truncated = TRUE;
                        }
                        else {
                            map[count].ThreadInfo = (ULONG64)(ULONG_PTR)threadInfo;
                            map[count].ProcessId = HandleToULong(processInfo->UniqueProcessId);
                            map[count].ThreadId = HandleToULong(threadId);
                            map[count].SessionId = processInfo->SessionId;
                            count += 1UL;
                        }
                    }
                    ObDereferenceObject(threadObject);
                }
            }

            if (processInfo->NextEntryOffset == 0UL) {
                break;
            }
            processOffset += processInfo->NextEntryOffset;
        }
        ExFreePoolWithTag(snapshot, 'sWkW');
    }

    *MapOut = map;
    *CountOut = count;
    *TruncatedOut = truncated;
    return STATUS_SUCCESS;
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

