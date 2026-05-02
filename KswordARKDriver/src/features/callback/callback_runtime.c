/*++

Module Name:

    callback_runtime.c

Abstract:

    Callback interception runtime bootstrap and IOCTL entry wrappers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_internal.h"

NTSYSAPI
PCHAR
NTAPI
PsGetProcessImageFileName(
    _In_ PEPROCESS Process
    );

NTSYSAPI
ULONG
NTAPI
PsGetProcessSessionId(
    _In_ PEPROCESS Process
    );

typedef PVOID
(NTAPI* KSWORD_ARK_EX_ALLOCATE_POOL2)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

static EX_PUSH_LOCK g_KswordArkCallbackRuntimeLock;
static KSWORD_ARK_CALLBACK_RUNTIME* g_KswordArkCallbackRuntime = NULL;
static KSWORD_ARK_EX_ALLOCATE_POOL2 g_KswordArkExAllocatePool2 = NULL;
static volatile LONG g_KswordArkPoolAllocatorResolved = 0;

KSWORD_ARK_CALLBACK_RUNTIME*
KswordArkCallbackGetRuntime(
    VOID
    )
{
    return g_KswordArkCallbackRuntime;
}

PVOID
KswordArkAllocateNonPaged(
    _In_ SIZE_T bytes,
    _In_ ULONG poolTag
    )
{
    if (bytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&g_KswordArkPoolAllocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        g_KswordArkExAllocatePool2 =
            (KSWORD_ARK_EX_ALLOCATE_POOL2)MmGetSystemRoutineAddress(&routineName);
    }

    if (g_KswordArkExAllocatePool2 != NULL) {
        return g_KswordArkExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, poolTag);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPool, bytes, poolTag);
#pragma warning(pop)
}

VOID
KswordArkCallbackLogFrame(
    _In_z_ PCSTR levelText,
    _In_z_ PCSTR messageText
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    if (runtime == NULL || runtime->Device == WDF_NO_HANDLE) {
        return;
    }

    (VOID)KswordARKDriverEnqueueLogFrame(
        runtime->Device,
        levelText != NULL ? levelText : "Info",
        messageText != NULL ? messageText : "");
}

VOID
KswordArkCallbackLogFormat(
    _In_z_ PCSTR levelText,
    _In_z_ _Printf_format_string_ PCSTR formatText,
    ...
    )
{
    CHAR logBuffer[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list argList;

    if (formatText == NULL) {
        KswordArkCallbackLogFrame(levelText, "");
        return;
    }

    va_start(argList, formatText);
    (VOID)RtlStringCbVPrintfA(logBuffer, sizeof(logBuffer), formatText, argList);
    va_end(argList);

    KswordArkCallbackLogFrame(levelText, logBuffer);
}

VOID
KswordArkGetSystemTimeUtc100ns(
    _Out_ LARGE_INTEGER* utcOut
    )
{
    if (utcOut == NULL) {
        return;
    }
    KeQuerySystemTimePrecise(utcOut);
}

VOID
KswordArkCopyUnicodeToFixedBuffer(
    _In_opt_ PCUNICODE_STRING sourceText,
    _Out_writes_(destinationChars) PWCHAR destinationBuffer,
    _In_ USHORT destinationChars
    )
{
    USHORT sourceChars = 0;
    USHORT copyChars = 0;

    if (destinationBuffer == NULL || destinationChars == 0U) {
        return;
    }

    destinationBuffer[0] = L'\0';
    if (sourceText == NULL || sourceText->Buffer == NULL || sourceText->Length == 0U) {
        return;
    }

    sourceChars = (USHORT)(sourceText->Length / sizeof(WCHAR));
    copyChars = sourceChars;
    if (copyChars >= destinationChars) {
        copyChars = (USHORT)(destinationChars - 1U);
    }

    if (copyChars > 0U) {
        RtlCopyMemory(destinationBuffer, sourceText->Buffer, copyChars * sizeof(WCHAR));
    }
    destinationBuffer[copyChars] = L'\0';
}

VOID
KswordArkCopyWideStringToFixedBuffer(
    _In_opt_z_ PCWSTR sourceText,
    _Out_writes_(destinationChars) PWCHAR destinationBuffer,
    _In_ USHORT destinationChars
    )
{
    size_t sourceChars = 0;
    size_t copyChars = 0;

    if (destinationBuffer == NULL || destinationChars == 0U) {
        return;
    }

    destinationBuffer[0] = L'\0';
    if (sourceText == NULL) {
        return;
    }

    if (!NT_SUCCESS(RtlStringCchLengthW(sourceText, destinationChars, &sourceChars))) {
        sourceChars = destinationChars - 1U;
    }

    copyChars = sourceChars;
    if (copyChars >= destinationChars) {
        copyChars = destinationChars - 1U;
    }

    if (copyChars > 0U) {
        RtlCopyMemory(destinationBuffer, sourceText, copyChars * sizeof(WCHAR));
    }
    destinationBuffer[copyChars] = L'\0';
}

BOOLEAN
KswordArkResolveProcessImagePath(
    _In_opt_ PEPROCESS processObject,
    _Out_writes_(destinationChars) PWCHAR destinationBuffer,
    _In_ USHORT destinationChars,
    _Out_opt_ BOOLEAN* pathUnavailableOut
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PUNICODE_STRING imagePath = NULL;
    BOOLEAN localUnavailable = TRUE;
    PEPROCESS targetProcess = processObject;

    if (destinationBuffer == NULL || destinationChars == 0U) {
        return FALSE;
    }

    destinationBuffer[0] = L'\0';
    if (targetProcess == NULL) {
        targetProcess = PsGetCurrentProcess();
    }

    status = SeLocateProcessImageName(targetProcess, &imagePath);
    if (NT_SUCCESS(status) && imagePath != NULL && imagePath->Buffer != NULL && imagePath->Length > 0U) {
        KswordArkCopyUnicodeToFixedBuffer(imagePath, destinationBuffer, destinationChars);
        ExFreePool(imagePath);
        localUnavailable = FALSE;
    }
    else {
        PCHAR shortImageName = PsGetProcessImageFileName(targetProcess);
        if (shortImageName != NULL && shortImageName[0] != '\0') {
            (VOID)RtlStringCbPrintfW(destinationBuffer, destinationChars * sizeof(WCHAR), L"%S", shortImageName);
            localUnavailable = TRUE;
        }
    }

    if (pathUnavailableOut != NULL) {
        *pathUnavailableOut = localUnavailable;
    }
    return (destinationBuffer[0] != L'\0') ? TRUE : FALSE;
}

ULONG
KswordArkGetProcessSessionIdSafe(
    _In_opt_ PEPROCESS processObject
    )
{
    PEPROCESS targetProcess = processObject;
    if (targetProcess == NULL) {
        targetProcess = PsGetCurrentProcess();
    }
    return PsGetProcessSessionId(targetProcess);
}

BOOLEAN
KswordArkGuidEquals(
    _In_ const KSWORD_ARK_GUID128* leftGuid,
    _In_ const KSWORD_ARK_GUID128* rightGuid
    )
{
    if (leftGuid == NULL || rightGuid == NULL) {
        return FALSE;
    }

    return (RtlCompareMemory(leftGuid->bytes, rightGuid->bytes, sizeof(leftGuid->bytes)) ==
        sizeof(leftGuid->bytes))
        ? TRUE
        : FALSE;
}

VOID
KswordArkGuidGenerate(
    _Out_ KSWORD_ARK_GUID128* guidOut
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    LARGE_INTEGER nowUtc = { 0 };
    ULONGLONG sequenceValue = 0;
    ULONG pidValue = HandleToULong(PsGetCurrentProcessId());
    ULONG tidValue = HandleToULong(PsGetCurrentThreadId());

    if (guidOut == NULL) {
        return;
    }

    KswordArkGetSystemTimeUtc100ns(&nowUtc);
    if (runtime != NULL) {
        sequenceValue = (ULONGLONG)InterlockedIncrement64(&runtime->EventSequence);
    }

    RtlZeroMemory(guidOut, sizeof(*guidOut));
    RtlCopyMemory(&guidOut->bytes[0], &nowUtc.QuadPart, sizeof(nowUtc.QuadPart));
    RtlCopyMemory(&guidOut->bytes[8], &sequenceValue, sizeof(sequenceValue));
    guidOut->bytes[0] ^= (UCHAR)(pidValue & 0xFFU);
    guidOut->bytes[1] ^= (UCHAR)((pidValue >> 8) & 0xFFU);
    guidOut->bytes[2] ^= (UCHAR)(tidValue & 0xFFU);
    guidOut->bytes[3] ^= (UCHAR)((tidValue >> 8) & 0xFFU);
}

static VOID
KswordArkCallbackDestroyRuntime(
    _In_opt_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* oldSnapshot = NULL;

    if (runtime == NULL) {
        return;
    }

    KswordArkMinifilterCallbackUnregister(runtime);
    KswordArkObjectCallbackUnregister(runtime);
    KswordArkImageCallbackUnregister(runtime);
    KswordArkThreadCallbackUnregister(runtime);
    KswordArkProcessCallbackUnregister(runtime);
    KswordArkRegistryCallbackUnregister(runtime);
    KswordArkCallbackWaiterUninitialize(runtime);

    ExAcquirePushLockExclusive(&runtime->SnapshotLock);
    oldSnapshot = runtime->ActiveSnapshot;
    runtime->ActiveSnapshot = NULL;
    ExReleasePushLockExclusive(&runtime->SnapshotLock);

    if (oldSnapshot != NULL) {
        ExWaitForRundownProtectionRelease(&oldSnapshot->RundownRef);
        KswordArkCallbackFreeSnapshot(oldSnapshot);
    }

    ExFreePoolWithTag(runtime, KSWORD_ARK_CALLBACK_TAG_RUNTIME);
}

NTSTATUS
KswordARKCallbackInitialize(
    _In_ WDFDEVICE Device
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Device == WDF_NO_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquirePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
    if (g_KswordArkCallbackRuntime != NULL) {
        ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
        return STATUS_SUCCESS;
    }

    runtime = (KSWORD_ARK_CALLBACK_RUNTIME*)KswordArkAllocateNonPaged(
        sizeof(KSWORD_ARK_CALLBACK_RUNTIME),
        KSWORD_ARK_CALLBACK_TAG_RUNTIME);
    if (runtime == NULL) {
        ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(runtime, sizeof(*runtime));
    runtime->Device = Device;
    runtime->WaitQueue = WDF_NO_HANDLE;
    runtime->ObRegistrationHandle = NULL;
    runtime->MiniFilterHandle = NULL;
    runtime->MiniFilterStarted = FALSE;
    runtime->MiniFilterRegisterStatus = STATUS_NOT_SUPPORTED;
    runtime->MiniFilterStartStatus = STATUS_NOT_SUPPORTED;
    runtime->RegisteredCallbacksMask = 0U;
    runtime->Initialized = FALSE;
    ExInitializePushLock(&runtime->SnapshotLock);
    ExInitializePushLock(&runtime->PendingLock);
    InitializeListHead(&runtime->PendingDecisionList);

    status = KswordArkCallbackWaiterInitialize(runtime);
    if (!NT_SUCCESS(status)) {
        ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
        KswordArkCallbackDestroyRuntime(runtime);
        return status;
    }

    status = KswordArkRegistryCallbackRegister(runtime);
    if (NT_SUCCESS(status)) {
        runtime->RegisteredCallbacksMask |= KSWORD_ARK_CALLBACK_REGISTERED_REGISTRY;
    }
    else {
        ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
        KswordArkCallbackDestroyRuntime(runtime);
        return status;
    }

    status = KswordArkProcessCallbackRegister(runtime);
    if (NT_SUCCESS(status)) {
        runtime->RegisteredCallbacksMask |= KSWORD_ARK_CALLBACK_REGISTERED_PROCESS;
    }
    else {
        ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
        KswordArkCallbackDestroyRuntime(runtime);
        return status;
    }

    status = KswordArkThreadCallbackRegister(runtime);
    if (NT_SUCCESS(status)) {
        runtime->RegisteredCallbacksMask |= KSWORD_ARK_CALLBACK_REGISTERED_THREAD;
    }
    else {
        ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
        KswordArkCallbackDestroyRuntime(runtime);
        return status;
    }

    status = KswordArkImageCallbackRegister(runtime);
    if (NT_SUCCESS(status)) {
        runtime->RegisteredCallbacksMask |= KSWORD_ARK_CALLBACK_REGISTERED_IMAGE;
    }
    else {
        ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
        KswordArkCallbackDestroyRuntime(runtime);
        return status;
    }

    status = KswordArkObjectCallbackRegister(runtime);
    if (NT_SUCCESS(status)) {
        runtime->RegisteredCallbacksMask |= KSWORD_ARK_CALLBACK_REGISTERED_OBJECT;
    }
    else {
        // Degrade gracefully when object callback registration is denied.
        // Typical case: ObRegisterCallbacks returns STATUS_ACCESS_DENIED due
        // integrity/signing constraints. Keep driver alive without object callbacks.
        if (status == STATUS_ACCESS_DENIED || status == STATUS_INVALID_IMAGE_HASH) {
            KswordArkCallbackLogFormat(
                "Warn",
                "Object callback registration skipped, status=0x%08lX. "
                "Driver continues without object callback feature.",
                (unsigned long)status);
            status = STATUS_SUCCESS;
        }
        else {
            ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
            KswordArkCallbackDestroyRuntime(runtime);
            return status;
        }
    }

    runtime->Initialized = TRUE;
    g_KswordArkCallbackRuntime = runtime;
    ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);

    KswordArkCallbackLogFormat(
        "Info",
        "Callback runtime initialized, registeredMask=0x%08lX.",
        (unsigned long)runtime->RegisteredCallbacksMask);
    return STATUS_SUCCESS;
}

VOID
KswordARKCallbackUninitialize(
    VOID
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = NULL;

    ExAcquirePushLockExclusive(&g_KswordArkCallbackRuntimeLock);
    runtime = g_KswordArkCallbackRuntime;
    g_KswordArkCallbackRuntime = NULL;
    ExReleasePushLockExclusive(&g_KswordArkCallbackRuntimeLock);

    if (runtime != NULL) {
        KswordArkCallbackDestroyRuntime(runtime);
    }
}

NTSTATUS
KswordARKCallbackIoctlSetRules(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
{
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot = NULL;
    PVOID inputBuffer = NULL;
    size_t inputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER),
        &inputBuffer,
        &inputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (InputBufferLength < sizeof(KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = KswordArkCallbackBuildSnapshotFromBlob(inputBuffer, inputLength, &snapshot);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordArkCallbackSwapSnapshot(snapshot);
    if (!NT_SUCCESS(status)) {
        KswordArkCallbackFreeSnapshot(snapshot);
        return status;
    }

    *CompleteBytesOut = inputLength;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKCallbackIoctlGetRuntimeState(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME_STATE* stateBuffer = NULL;
    size_t stateBufferLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_CALLBACK_RUNTIME_STATE),
        (PVOID*)&stateBuffer,
        &stateBufferLength);
    if (!NT_SUCCESS(status)) {
        UNREFERENCED_PARAMETER(OutputBufferLength);
        return status;
    }

    KswordArkCallbackQueryRuntimeState(stateBuffer);
    *CompleteBytesOut = sizeof(KSWORD_ARK_CALLBACK_RUNTIME_STATE);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKCallbackIoctlWaitEvent(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
{
    return KswordArkCallbackIoctlWaitEventInternal(
        Request,
        OutputBufferLength,
        CompleteBytesOut);
}

NTSTATUS
KswordARKCallbackIoctlAnswerEvent(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
{
    return KswordArkCallbackIoctlAnswerEventInternal(
        Request,
        InputBufferLength,
        CompleteBytesOut);
}

NTSTATUS
KswordARKCallbackIoctlCancelAllPending(
    _Out_ size_t* CompleteBytesOut
    )
{
    NTSTATUS status = KswordArkCallbackCancelAllPendingInternal();
    if (CompleteBytesOut != NULL) {
        *CompleteBytesOut = 0U;
    }
    return status;
}
