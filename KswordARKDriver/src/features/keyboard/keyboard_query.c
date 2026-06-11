/*++

Module Name:

    keyboard_query.c

Abstract:

    Read-only win32k keyboard hotkey and keyboard hook enumeration helpers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_keyboard.h"
#include "../kernel/hook_scan_support.h"

#include <ntstrsafe.h>

#define KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY))

#define KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY))

#define KSWORD_ARK_KEYBOARD_HOTKEY_BUCKETS 0x80UL
#define KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT 512UL

// 当前 win32kfull!NtUserSetWindowsHookEx/IsHotKey 形态下的保守默认偏移。
#define KSWORD_ARK_KEYBOARD_HOTKEY_THREADINFO_OFFSET 0x00UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_WINDOW_OFFSET     0x10UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_FLAGS2_OFFSET     0x22UL

#define KSWORD_ARK_HOOK_THREAD_ARRAY_OFFSET        0x3C0UL
#define KSWORD_ARK_HOOK_DESKTOP_INFO_OFFSET        0x1F8UL
#define KSWORD_ARK_HOOK_DESKTOP_ARRAY_OFFSET       0x28UL
#define KSWORD_ARK_HOOK_NEXT_OFFSET                0x28UL
#define KSWORD_ARK_HOOK_TYPE_OFFSET                0x30UL
#define KSWORD_ARK_HOOK_PROCEDURE_OFFSET           0x38UL
#define KSWORD_ARK_HOOK_FLAGS_OFFSET               0x40UL
#define KSWORD_ARK_HOOK_MODULE_ID_OFFSET           0x44UL
#define KSWORD_ARK_HOOK_TARGET_THREAD_INFO_OFFSET  0x48UL

typedef PVOID(NTAPI* KSWORD_USER_GET_SILO_GLOBALS_FN)(VOID);

typedef PEPROCESS(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_FN)(
    _In_opt_ PEPROCESS Process
    );

typedef PETHREAD(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

typedef PVOID(NTAPI* KSWORD_PS_GET_THREAD_WIN32_THREAD_FN)(
    _In_ PETHREAD Thread
    );

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_z_ PCSTR RoutineName
    );

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
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

static BOOLEAN
KswordARKKeyboardLooksLikeKernelPointer(
    _In_ ULONG_PTR Value
    )
{
#if defined(_M_AMD64)
    return Value >= 0xFFFF000000000000ULL ? TRUE : FALSE;
#else
    return Value >= 0x80000000UL ? TRUE : FALSE;
#endif
}

static BOOLEAN
KswordARKKeyboardReadPointer(
    _In_ ULONG_PTR Address,
    _Out_ ULONG_PTR* ValueOut
    )
{
    PVOID pointerValue = NULL;

    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0U;
    if (Address == 0U) {
        return FALSE;
    }

    if (!KswordARKHookReadMemorySafe((const VOID*)Address, &pointerValue, sizeof(pointerValue))) {
        return FALSE;
    }

    *ValueOut = (ULONG_PTR)pointerValue;
    return TRUE;
}

static BOOLEAN
KswordARKKeyboardReadUlong(
    _In_ ULONG_PTR Address,
    _Out_ ULONG* ValueOut
    )
{
    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0UL;
    if (Address == 0U) {
        return FALSE;
    }
    return KswordARKHookReadMemorySafe((const VOID*)Address, ValueOut, sizeof(*ValueOut));
}

static BOOLEAN
KswordARKKeyboardReadUshort(
    _In_ ULONG_PTR Address,
    _Out_ USHORT* ValueOut
    )
{
    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0U;
    if (Address == 0U) {
        return FALSE;
    }
    return KswordARKHookReadMemorySafe((const VOID*)Address, ValueOut, sizeof(*ValueOut));
}

static BOOLEAN
KswordARKKeyboardFindModuleByName(
    _In_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_z_ PCSTR ModuleName,
    _Out_ KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntryOut
    )
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

static KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKKeyboardResolvePsGetNextProcessThread(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcessThread");
    return (KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_PS_GET_THREAD_WIN32_THREAD_FN
KswordARKKeyboardResolvePsGetThreadWin32Thread(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetThreadWin32Thread");
    return (KSWORD_PS_GET_THREAD_WIN32_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_USER_GET_SILO_GLOBALS_FN
KswordARKKeyboardResolveUserGetSiloGlobals(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* Win32kbaseEntry
    )
{
    if (Win32kbaseEntry == NULL || Win32kbaseEntry->ImageBase == NULL) {
        return NULL;
    }

    return (KSWORD_USER_GET_SILO_GLOBALS_FN)RtlFindExportedRoutineByName(
        Win32kbaseEntry->ImageBase,
        "UserGetSiloGlobals");
}

static BOOLEAN
KswordARKKeyboardResolveIsHotKeyBody(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* Win32kfullEntry,
    _Out_ ULONG_PTR* IsHotKeyAddressOut
    )
{
    ULONG_PTR editionAddress = 0U;
    UCHAR bytes[32] = { 0 };
    ULONG byteIndex = 0UL;

    if (Win32kfullEntry == NULL || IsHotKeyAddressOut == NULL) {
        return FALSE;
    }
    *IsHotKeyAddressOut = 0U;

    editionAddress = (ULONG_PTR)RtlFindExportedRoutineByName(
        Win32kfullEntry->ImageBase,
        "EditionIsHotKey");
    if (editionAddress == 0U ||
        editionAddress < (ULONG_PTR)Win32kfullEntry->ImageBase ||
        editionAddress >= ((ULONG_PTR)Win32kfullEntry->ImageBase + Win32kfullEntry->ImageSize)) {
        return FALSE;
    }

    if (!KswordARKHookReadMemorySafe((const VOID*)editionAddress, bytes, sizeof(bytes))) {
        return FALSE;
    }

    for (byteIndex = 0UL; byteIndex + 5UL <= sizeof(bytes); ++byteIndex) {
        if (bytes[byteIndex] == 0xE8U) {
            LONG relativeOffset = 0;
            ULONG_PTR targetAddress = 0U;

            RtlCopyMemory(&relativeOffset, bytes + byteIndex + 1UL, sizeof(relativeOffset));
            targetAddress = editionAddress + byteIndex + 5UL + (LONG_PTR)relativeOffset;
            if (targetAddress >= (ULONG_PTR)Win32kfullEntry->ImageBase &&
                targetAddress < ((ULONG_PTR)Win32kfullEntry->ImageBase + Win32kfullEntry->ImageSize)) {
                *IsHotKeyAddressOut = targetAddress;
                return TRUE;
            }
        }
    }

    return FALSE;
}

static BOOLEAN
KswordARKKeyboardResolveHotkeyLayout(
    _In_ ULONG_PTR IsHotKeyAddress,
    _Out_ ULONG* TableOffsetOut,
    _Out_ ULONG* NextOffsetOut,
    _Out_ ULONG* ModifiersOffsetOut,
    _Out_ ULONG* VkOffsetOut,
    _Out_ ULONG* IdOffsetOut
    )
{
    UCHAR bytes[192] = { 0 };
    ULONG index = 0UL;
    ULONG tableOffset = 0UL;
    ULONG nextOffset = 0UL;
    ULONG modifiersOffset = 0UL;
    ULONG vkOffset = 0UL;

    if (IsHotKeyAddress == 0U ||
        TableOffsetOut == NULL ||
        NextOffsetOut == NULL ||
        ModifiersOffsetOut == NULL ||
        VkOffsetOut == NULL ||
        IdOffsetOut == NULL) {
        return FALSE;
    }

    *TableOffsetOut = 0UL;
    *NextOffsetOut = 0UL;
    *ModifiersOffsetOut = 0UL;
    *VkOffsetOut = 0UL;
    *IdOffsetOut = 0UL;

    if (!KswordARKHookReadMemorySafe((const VOID*)IsHotKeyAddress, bytes, sizeof(bytes))) {
        return FALSE;
    }

    for (index = 0UL; index + 8UL <= sizeof(bytes); ++index) {
        if (bytes[index] == 0x4AU &&
            bytes[index + 1UL] == 0x8BU &&
            bytes[index + 2UL] == 0xBCU &&
            bytes[index + 3UL] == 0xC0U) {
            RtlCopyMemory(&tableOffset, bytes + index + 4UL, sizeof(tableOffset));
        }
        if (bytes[index] == 0x0FU &&
            bytes[index + 1UL] == 0xB7U &&
            bytes[index + 2UL] == 0x47U) {
            modifiersOffset = (ULONG)bytes[index + 3UL];
        }
        if (bytes[index] == 0x39U &&
            bytes[index + 1UL] == 0x77U) {
            vkOffset = (ULONG)bytes[index + 2UL];
        }
        if (bytes[index] == 0x48U &&
            bytes[index + 1UL] == 0x8BU &&
            bytes[index + 2UL] == 0x7FU) {
            nextOffset = (ULONG)bytes[index + 3UL];
        }
    }

    if (tableOffset == 0UL || nextOffset == 0UL || modifiersOffset == 0UL || vkOffset == 0UL) {
        return FALSE;
    }

    *TableOffsetOut = tableOffset;
    *NextOffsetOut = nextOffset;
    *ModifiersOffsetOut = modifiersOffset;
    *VkOffsetOut = vkOffset;
    *IdOffsetOut = vkOffset + sizeof(ULONG);
    return TRUE;
}

static VOID
KswordARKKeyboardFillHotkeyThreadIdentity(
    _In_ ULONG_PTR ThreadInfo,
    _Out_ ULONG_PTR* ThreadObjectOut,
    _Out_ ULONG* ProcessIdOut,
    _Out_ ULONG* ThreadIdOut
    )
{
    ULONG_PTR threadObject = 0U;

    if (ThreadObjectOut != NULL) {
        *ThreadObjectOut = 0U;
    }
    if (ProcessIdOut != NULL) {
        *ProcessIdOut = 0UL;
    }
    if (ThreadIdOut != NULL) {
        *ThreadIdOut = 0UL;
    }

    if (ThreadInfo == 0U ||
        !KswordARKKeyboardReadPointer(ThreadInfo, &threadObject) ||
        !KswordARKKeyboardLooksLikeKernelPointer(threadObject)) {
        return;
    }

    if (ThreadObjectOut != NULL) {
        *ThreadObjectOut = threadObject;
    }

    __try {
        if (ProcessIdOut != NULL) {
            *ProcessIdOut = HandleToULong(PsGetThreadProcessId((PETHREAD)threadObject));
        }
        if (ThreadIdOut != NULL) {
            *ThreadIdOut = HandleToULong(PsGetThreadId((PETHREAD)threadObject));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (ProcessIdOut != NULL) {
            *ProcessIdOut = 0UL;
        }
        if (ThreadIdOut != NULL) {
            *ThreadIdOut = 0UL;
        }
    }
}

static VOID
KswordARKKeyboardAppendHotkeyEntry(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ ULONG_PTR SessionGlobals,
    _In_ ULONG BucketIndex,
    _In_ ULONG Depth,
    _In_ ULONG_PTR HotkeyObject,
    _In_ ULONG NextOffset,
    _In_ ULONG ModifiersOffset,
    _In_ ULONG VkOffset,
    _In_ ULONG IdOffset,
    _In_ ULONG RequestFlags,
    _In_ ULONG FilterProcessId
    )
{
    KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY tempEntry;
    USHORT modifiers = 0U;
    USHORT flags2 = 0U;
    ULONG_PTR nextHotkey = 0U;
    ULONG_PTR threadInfo = 0U;
    ULONG_PTR threadObject = 0U;
    ULONG_PTR windowObject = 0U;

    if (Response == NULL || HotkeyObject == 0U) {
        return;
    }

    RtlZeroMemory(&tempEntry, sizeof(tempEntry));
    tempEntry.source = KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE;
    tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    tempEntry.bucketIndex = BucketIndex;
    tempEntry.depth = Depth;
    tempEntry.hotkeyObject = (ULONG64)HotkeyObject;
    tempEntry.sessionGlobals = (ULONG64)SessionGlobals;

    if (!KswordARKKeyboardReadPointer(HotkeyObject + NextOffset, &nextHotkey)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
    }
    if (!KswordARKKeyboardReadPointer(HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_THREADINFO_OFFSET, &threadInfo)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
    }
    (VOID)KswordARKKeyboardReadPointer(HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_WINDOW_OFFSET, &windowObject);
    if (!KswordARKKeyboardReadUshort(HotkeyObject + ModifiersOffset, &modifiers)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
    }
    if (!KswordARKKeyboardReadUshort(HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_FLAGS2_OFFSET, &flags2)) {
        flags2 = 0U;
    }
    if (!KswordARKKeyboardReadUlong(HotkeyObject + VkOffset, &tempEntry.virtualKey)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
    }
    if (!KswordARKKeyboardReadUlong(HotkeyObject + IdOffset, &tempEntry.hotkeyId)) {
        tempEntry.hotkeyId = 0UL;
    }

    tempEntry.nextHotkeyObject = (ULONG64)nextHotkey;
    tempEntry.threadInfo = (ULONG64)threadInfo;
    tempEntry.windowObject = (ULONG64)windowObject;
    tempEntry.modifiers = (ULONG)modifiers;
    tempEntry.modifierFlags2 = (ULONG)flags2;
    KswordARKKeyboardFillHotkeyThreadIdentity(
        threadInfo,
        &threadObject,
        &tempEntry.processId,
        &tempEntry.threadId);
    tempEntry.threadObject = (ULONG64)threadObject;
    if ((RequestFlags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS) != 0UL &&
        FilterProcessId != 0UL &&
        tempEntry.processId != 0UL &&
        tempEntry.processId != FilterProcessId) {
        return;
    }

    if (Response->totalCount < MaxEntries) {
        Response->totalCount += 1UL;
    }

    if (Response->returnedCount >= EntryCapacity) {
        Response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED;
        return;
    }

    (VOID)RtlStringCchPrintfW(
        tempEntry.detail,
        KSWORD_ARK_KEYBOARD_DETAIL_CHARS,
        L"bucket=%lu depth=%lu",
        BucketIndex,
        Depth);

    RtlCopyMemory(
        &Response->entries[Response->returnedCount],
        &tempEntry,
        sizeof(tempEntry));
    Response->returnedCount += 1UL;
}

static BOOLEAN
KswordARKKeyboardHookAlreadySeen(
    _In_ const KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG_PTR HookObject
    )
{
    ULONG index = 0UL;

    if (Response == NULL || HookObject == 0U) {
        return FALSE;
    }

    for (index = 0UL; index < Response->returnedCount; ++index) {
        if ((ULONG_PTR)Response->entries[index].hookObject == HookObject) {
            return TRUE;
        }
    }

    return FALSE;
}

static VOID
KswordARKKeyboardAppendHookEntry(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ ULONG Source,
    _In_ ULONG HookScope,
    _In_ ULONG_PTR ChainHead,
    _In_ ULONG_PTR HookObject,
    _In_ ULONG ProcessId,
    _In_ ULONG ThreadId
    )
{
    KSWORD_ARK_KEYBOARD_HOOK_ENTRY tempEntry;
    ULONG hookType = 0UL;
    ULONG hookFlags = 0UL;
    ULONG moduleId = 0UL;
    ULONG_PTR nextHook = 0U;
    ULONG_PTR procedureOffset = 0U;
    ULONG_PTR targetThreadInfo = 0U;

    if (Response == NULL || HookObject == 0U) {
        return;
    }

    if (KswordARKKeyboardHookAlreadySeen(Response, HookObject)) {
        return;
    }

    if (Response->totalCount < MaxEntries) {
        Response->totalCount += 1UL;
    }

    if (Response->returnedCount >= EntryCapacity) {
        Response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED;
        return;
    }

    RtlZeroMemory(&tempEntry, sizeof(tempEntry));
    tempEntry.source = Source;
    tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    tempEntry.hookScope = HookScope;
    tempEntry.processId = ProcessId;
    tempEntry.threadId = ThreadId;
    tempEntry.hookObject = (ULONG64)HookObject;
    tempEntry.chainHead = (ULONG64)ChainHead;

    if (!KswordARKKeyboardReadUlong(HookObject + KSWORD_ARK_HOOK_TYPE_OFFSET, &hookType)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
    }
    if (hookType != KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD &&
        hookType != KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD_LL) {
        return;
    }

    (VOID)KswordARKKeyboardReadPointer(HookObject + KSWORD_ARK_HOOK_NEXT_OFFSET, &nextHook);
    (VOID)KswordARKKeyboardReadUlong(HookObject + KSWORD_ARK_HOOK_FLAGS_OFFSET, &hookFlags);
    (VOID)KswordARKKeyboardReadUlong(HookObject + KSWORD_ARK_HOOK_MODULE_ID_OFFSET, &moduleId);
    (VOID)KswordARKKeyboardReadPointer(HookObject + KSWORD_ARK_HOOK_PROCEDURE_OFFSET, &procedureOffset);
    (VOID)KswordARKKeyboardReadPointer(HookObject + KSWORD_ARK_HOOK_TARGET_THREAD_INFO_OFFSET, &targetThreadInfo);

    tempEntry.hookType = hookType;
    tempEntry.flags = hookFlags;
    tempEntry.moduleId = moduleId;
    tempEntry.nextHookObject = (ULONG64)nextHook;
    tempEntry.procedureOffset = (ULONG64)procedureOffset;
    tempEntry.targetThreadInfo = (ULONG64)targetThreadInfo;
    (VOID)RtlStringCchPrintfW(
        tempEntry.detail,
        KSWORD_ARK_KEYBOARD_DETAIL_CHARS,
        L"scope=%lu chain=0x%p",
        HookScope,
        (PVOID)ChainHead);

    RtlCopyMemory(
        &Response->entries[Response->returnedCount],
        &tempEntry,
        sizeof(tempEntry));
    Response->returnedCount += 1UL;
}

static VOID
KswordARKKeyboardWalkHookChain(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ ULONG Source,
    _In_ ULONG HookScope,
    _In_ ULONG_PTR ChainHead,
    _In_ ULONG ProcessId,
    _In_ ULONG ThreadId
    )
{
    ULONG_PTR hookObject = 0U;
    ULONG depth = 0UL;

    if (Response == NULL || ChainHead == 0U) {
        return;
    }

    if (!KswordARKKeyboardReadPointer(ChainHead, &hookObject)) {
        return;
    }

    while (hookObject != 0U && depth < KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT) {
        ULONG_PTR nextHook = 0U;

        KswordARKKeyboardAppendHookEntry(
            Response,
            EntryCapacity,
            MaxEntries,
            Source,
            HookScope,
            ChainHead,
            hookObject,
            ProcessId,
            ThreadId);

        if (!KswordARKKeyboardReadPointer(hookObject + KSWORD_ARK_HOOK_NEXT_OFFSET, &nextHook)) {
            break;
        }
        if (nextHook == hookObject) {
            break;
        }
        hookObject = nextHook;
        ++depth;
    }
}

static VOID
KswordARKKeyboardEnumerateHookChainsForThread(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ PETHREAD ThreadObject,
    _In_ ULONG ProcessId,
    _In_ KSWORD_PS_GET_THREAD_WIN32_THREAD_FN PsGetThreadWin32Thread,
    _In_ ULONG Flags
    )
{
    static const ULONG hookTypes[] = {
        KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD,
        KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD_LL
    };
    PVOID threadInfoPointer = NULL;
    ULONG_PTR threadInfo = 0U;
    ULONG threadId = 0UL;
    ULONG index = 0UL;

    if (Response == NULL || ThreadObject == NULL || PsGetThreadWin32Thread == NULL) {
        return;
    }

    __try {
        threadInfoPointer = PsGetThreadWin32Thread(ThreadObject);
        threadId = HandleToULong(PsGetThreadId(ThreadObject));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        threadInfoPointer = NULL;
    }

    threadInfo = (ULONG_PTR)threadInfoPointer;
    if (threadInfo == 0U) {
        return;
    }

    for (index = 0UL; index < RTL_NUMBER_OF(hookTypes); ++index) {
        ULONG hookType = hookTypes[index];
        ULONG_PTR chainHead = 0U;

        if ((Flags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS) != 0UL) {
            chainHead = threadInfo +
                KSWORD_ARK_HOOK_THREAD_ARRAY_OFFSET +
                ((ULONG_PTR)(hookType + 1UL) * sizeof(PVOID));
            KswordARKKeyboardWalkHookChain(
                Response,
                EntryCapacity,
                MaxEntries,
                KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_THREAD_HOOK_CHAIN,
                KSWORD_ARK_KEYBOARD_HOOK_SCOPE_THREAD,
                chainHead,
                ProcessId,
                threadId);
        }

        if ((Flags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS) != 0UL) {
            ULONG_PTR desktopInfo = 0U;

            if (KswordARKKeyboardReadPointer(threadInfo + KSWORD_ARK_HOOK_DESKTOP_INFO_OFFSET, &desktopInfo) &&
                desktopInfo != 0U) {
                chainHead = desktopInfo +
                    KSWORD_ARK_HOOK_DESKTOP_ARRAY_OFFSET +
                    ((ULONG_PTR)(hookType + 1UL) * sizeof(PVOID));
                KswordARKKeyboardWalkHookChain(
                    Response,
                    EntryCapacity,
                    MaxEntries,
                    KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_GLOBAL_HOOK_CHAIN,
                    KSWORD_ARK_KEYBOARD_HOOK_SCOPE_GLOBAL,
                    chainHead,
                    ProcessId,
                    threadId);
            }
        }
    }
}

NTSTATUS
KswordARKDriverEnumerateKeyboardHotkeys(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_KEYBOARD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* response = NULL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kfullEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kbaseEntry;
    KSWORD_USER_GET_SILO_GLOBALS_FN userGetSiloGlobals = NULL;
    ULONG requestFlags = KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM;
    ULONG maxEntries = 1024UL;
    ULONG entryCapacity = 0UL;
    ULONG_PTR isHotKeyAddress = 0U;
    ULONG_PTR sessionGlobals = 0U;
    ULONG tableOffset = 0UL;
    ULONG nextOffset = 0UL;
    ULONG modifiersOffset = 0UL;
    ULONG vkOffset = 0UL;
    ULONG idOffset = 0UL;
    PEPROCESS attachProcess = NULL;
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    BOOLEAN attached = FALSE;
    BOOLEAN referencedProcess = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bucketIndex = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Request != NULL) {
        requestFlags = Request->flags;
        if (Request->maxEntries != 0UL) {
            maxEntries = Request->maxEntries;
        }
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY);
    response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
    response->flags = requestFlags;
    entryCapacity = (ULONG)((OutputBufferLength - KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY));
    if (entryCapacity > maxEntries) {
        entryCapacity = maxEntries;
    }

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (!NT_SUCCESS(status) || moduleInfo == NULL) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND;
        *BytesWrittenOut = KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    if (!KswordARKKeyboardFindModuleByName(moduleInfo, "win32kfull.sys", &win32kfullEntry) ||
        !KswordARKKeyboardFindModuleByName(moduleInfo, "win32kbase.sys", &win32kbaseEntry)) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND;
        goto Cleanup;
    }

    response->win32kBase = (ULONG64)(ULONG_PTR)win32kfullEntry.ImageBase;
    userGetSiloGlobals = KswordARKKeyboardResolveUserGetSiloGlobals(&win32kbaseEntry);
    if (userGetSiloGlobals == NULL ||
        !KswordARKKeyboardResolveIsHotKeyBody(&win32kfullEntry, &isHotKeyAddress) ||
        !KswordARKKeyboardResolveHotkeyLayout(
            isHotKeyAddress,
            &tableOffset,
            &nextOffset,
            &modifiersOffset,
            &vkOffset,
            &idOffset)) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PATTERN_NOT_FOUND;
        goto Cleanup;
    }

    response->tableOffset = tableOffset;
    response->hotkeyNextOffset = nextOffset;
    response->hotkeyModifiersOffset = modifiersOffset;
    response->hotkeyVkOffset = vkOffset;
    response->hotkeyIdOffset = idOffset;

    if (Request != NULL && Request->processId != 0UL) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &attachProcess);
        response->lastStatus = status;
        if (!NT_SUCCESS(status)) {
            attachProcess = NULL;
        }
        else {
            referencedProcess = TRUE;
        }
    }

    if (attachProcess != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        RtlZeroMemory(attachState, sizeof(attachState));
        KeStackAttachProcess((PVOID)attachProcess, attachState);
        attached = TRUE;
    }

    __try {
        sessionGlobals = (ULONG_PTR)userGetSiloGlobals();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        sessionGlobals = 0U;
        response->lastStatus = GetExceptionCode();
    }

    if (sessionGlobals == 0U) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE;
        goto Cleanup;
    }
    response->sessionGlobals = (ULONG64)sessionGlobals;

    for (bucketIndex = 0UL; bucketIndex < KSWORD_ARK_KEYBOARD_HOTKEY_BUCKETS; ++bucketIndex) {
        ULONG_PTR listHeadAddress = sessionGlobals + tableOffset + ((ULONG_PTR)bucketIndex * sizeof(PVOID));
        ULONG_PTR hotkeyObject = 0U;
        ULONG depth = 0UL;

        if (!KswordARKKeyboardReadPointer(listHeadAddress, &hotkeyObject)) {
            continue;
        }

        while (hotkeyObject != 0U && depth < KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT) {
            ULONG_PTR nextHotkey = 0U;

            KswordARKKeyboardAppendHotkeyEntry(
                response,
                entryCapacity,
                maxEntries,
                sessionGlobals,
                bucketIndex,
                depth,
                hotkeyObject,
                nextOffset,
                modifiersOffset,
                vkOffset,
                idOffset,
                requestFlags,
                (Request != NULL) ? Request->processId : 0UL);

            if (!KswordARKKeyboardReadPointer(hotkeyObject + nextOffset, &nextHotkey)) {
                break;
            }
            if (nextHotkey == hotkeyObject) {
                break;
            }
            hotkeyObject = nextHotkey;
            ++depth;
        }
    }

    if (response->status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    }

Cleanup:
    if (attached) {
        KeUnstackDetachProcess(attachState);
    }
    if (referencedProcess && attachProcess != NULL) {
        ObDereferenceObject(attachProcess);
    }
    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }

    *BytesWrittenOut = KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnumerateKeyboardHooks(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_KEYBOARD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* response = NULL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kfullEntry;
    KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    ULONG requestFlags = KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS |
        KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS;
    ULONG maxEntries = 1024UL;
    ULONG entryCapacity = 0UL;
    PEPROCESS processObject = NULL;
    BOOLEAN referencedProcess = FALSE;
    PETHREAD threadCursor = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG processId = 0UL;
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    BOOLEAN attached = FALSE;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Request != NULL) {
        requestFlags = Request->flags;
        if (Request->maxEntries != 0UL) {
            maxEntries = Request->maxEntries;
        }
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY);
    response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
    response->flags = requestFlags;
    response->threadHookArrayOffset = KSWORD_ARK_HOOK_THREAD_ARRAY_OFFSET;
    response->desktopInfoOffset = KSWORD_ARK_HOOK_DESKTOP_INFO_OFFSET;
    response->desktopHookArrayOffset = KSWORD_ARK_HOOK_DESKTOP_ARRAY_OFFSET;
    response->hookNextOffset = KSWORD_ARK_HOOK_NEXT_OFFSET;
    response->hookTypeOffset = KSWORD_ARK_HOOK_TYPE_OFFSET;
    response->hookProcedureOffset = KSWORD_ARK_HOOK_PROCEDURE_OFFSET;
    response->hookFlagsOffset = KSWORD_ARK_HOOK_FLAGS_OFFSET;
    response->hookModuleIdOffset = KSWORD_ARK_HOOK_MODULE_ID_OFFSET;
    response->hookTargetThreadInfoOffset = KSWORD_ARK_HOOK_TARGET_THREAD_INFO_OFFSET;
    entryCapacity = (ULONG)((OutputBufferLength - KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY));
    if (entryCapacity > maxEntries) {
        entryCapacity = maxEntries;
    }

    psGetNextProcessThread = KswordARKKeyboardResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKKeyboardResolvePsGetThreadWin32Thread();
    if (psGetNextProcessThread == NULL || psGetThreadWin32Thread == NULL) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNSUPPORTED;
        goto Cleanup;
    }

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (NT_SUCCESS(status) && moduleInfo != NULL &&
        KswordARKKeyboardFindModuleByName(moduleInfo, "win32kfull.sys", &win32kfullEntry)) {
        response->win32kBase = (ULONG64)(ULONG_PTR)win32kfullEntry.ImageBase;
    }

    if (Request != NULL && Request->processId != 0UL) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
        response->lastStatus = status;
        if (!NT_SUCCESS(status)) {
            response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
            goto Cleanup;
        }
        referencedProcess = TRUE;
    }
    else {
        processObject = PsGetCurrentProcess();
    }

    processId = HandleToULong(PsGetProcessId(processObject));
    if (processObject != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        RtlZeroMemory(attachState, sizeof(attachState));
        KeStackAttachProcess((PVOID)processObject, attachState);
        attached = TRUE;
    }

    threadCursor = psGetNextProcessThread(processObject, NULL);
    while (threadCursor != NULL) {
        PETHREAD nextThread = psGetNextProcessThread(processObject, threadCursor);

        KswordARKKeyboardEnumerateHookChainsForThread(
            response,
            entryCapacity,
            maxEntries,
            threadCursor,
            processId,
            psGetThreadWin32Thread,
            requestFlags);

        ObDereferenceObject(threadCursor);
        threadCursor = nextThread;
    }

    if (response->status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    }

Cleanup:
    if (attached) {
        KeUnstackDetachProcess(attachState);
    }
    if (referencedProcess && processObject != NULL) {
        ObDereferenceObject(processObject);
    }
    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }

    *BytesWrittenOut = KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY));
    return STATUS_SUCCESS;
}
