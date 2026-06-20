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
    runtime->MiniFilterBypassPidCount = 0U;
    RtlZeroMemory(runtime->MiniFilterBypassPids, sizeof(runtime->MiniFilterBypassPids));
    runtime->RegisteredCallbacksMask = 0U;
    runtime->Initialized = FALSE;
    ExInitializePushLock(&runtime->SnapshotLock);
    ExInitializePushLock(&runtime->PendingLock);
    ExInitializePushLock(&runtime->MiniFilterBypassPidLock);
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
KswordArkCallbackSetMinifilterBypassPids(
    _In_reads_opt_(PidCount) const ULONG* ProcessIds,
    _In_ ULONG PidCount
    )
/*++

Routine Description:

    Replace the in-memory minifilter bypass PID list used by the hot-path
    pre-operation callback. PID zero is ignored because it is not a user-mode
    process identity that should be allowlisted from the UI.

Arguments:

    ProcessIds - Optional caller-owned PID array. It may be NULL only when
        PidCount is zero.
    PidCount - Number of input PID slots to inspect.

Return Value:

    STATUS_SUCCESS when the runtime list has been replaced. An NTSTATUS error
    is returned for invalid packet shape, over-limit counts or missing runtime.

--*/
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    ULONG uniquePids[KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT];
    ULONG readIndex = 0UL;
    ULONG scanIndex = 0UL;
    ULONG writeIndex = 0UL;

    if (PidCount > KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    if (PidCount != 0UL && ProcessIds == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (runtime == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    RtlZeroMemory(uniquePids, sizeof(uniquePids));
    for (readIndex = 0UL; readIndex < PidCount; ++readIndex) {
        BOOLEAN duplicatePid = FALSE;
        ULONG candidatePid = ProcessIds[readIndex];

        if (candidatePid == 0UL) {
            continue;
        }

        for (scanIndex = 0UL; scanIndex < writeIndex; ++scanIndex) {
            if (uniquePids[scanIndex] == candidatePid) {
                duplicatePid = TRUE;
                break;
            }
        }

        if (!duplicatePid && writeIndex < KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT) {
            uniquePids[writeIndex] = candidatePid;
            ++writeIndex;
        }
    }

    ExAcquirePushLockExclusive(&runtime->MiniFilterBypassPidLock);
    RtlZeroMemory(runtime->MiniFilterBypassPids, sizeof(runtime->MiniFilterBypassPids));
    if (writeIndex != 0UL) {
        RtlCopyMemory(runtime->MiniFilterBypassPids, uniquePids, (SIZE_T)writeIndex * sizeof(ULONG));
    }
    runtime->MiniFilterBypassPidCount = writeIndex;
    ExReleasePushLockExclusive(&runtime->MiniFilterBypassPidLock);

    KswordArkCallbackLogFormat(
        "Info",
        "Minifilter bypass PID whitelist updated, count=%lu.",
        (unsigned long)writeIndex);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordArkCallbackQueryMinifilterBypassPids(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Copy the current minifilter bypass PID list into the shared R3/R0 response
    structure. The copy is protected by the runtime push lock so user mode sees
    one consistent snapshot.

Arguments:

    OutputBuffer - Caller output buffer that receives the fixed response packet.
    OutputBufferLength - Output buffer size supplied by WDF.
    BytesWrittenOut - Receives sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE).

Return Value:

    STATUS_SUCCESS when the response packet was written; otherwise an NTSTATUS
    validation or runtime availability error.

--*/
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE* response = NULL;
    ULONG pidCount = 0UL;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBuffer == NULL ||
        OutputBufferLength < sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (runtime == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    response = (KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE*)OutputBuffer;
    RtlZeroMemory(response, sizeof(*response));
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;

    ExAcquirePushLockShared(&runtime->MiniFilterBypassPidLock);
    pidCount = runtime->MiniFilterBypassPidCount;
    if (pidCount > KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT) {
        pidCount = KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT;
    }
    response->pidCount = pidCount;
    if (pidCount != 0UL) {
        RtlCopyMemory(response->processIds, runtime->MiniFilterBypassPids, (SIZE_T)pidCount * sizeof(ULONG));
    }
    ExReleasePushLockShared(&runtime->MiniFilterBypassPidLock);

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

BOOLEAN
KswordArkCallbackIsMinifilterBypassPid(
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    Check whether a requestor PID is present in the minifilter bypass whitelist.
    The minifilter calls this before callback rules, redirect rewriting and file
    monitor capture so allowlisted requests pass through to the filesystem stack.

Arguments:

    ProcessId - Requestor process identifier from FltGetRequestorProcessId.

Return Value:

    TRUE when ProcessId is allowlisted; FALSE otherwise.

--*/
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    ULONG pidIndex = 0UL;
    ULONG pidCount = 0UL;
    BOOLEAN matchedPid = FALSE;

    if (ProcessId == 0UL || runtime == NULL) {
        return FALSE;
    }

    ExAcquirePushLockShared(&runtime->MiniFilterBypassPidLock);
    pidCount = runtime->MiniFilterBypassPidCount;
    if (pidCount > KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT) {
        pidCount = KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT;
    }
    for (pidIndex = 0UL; pidIndex < pidCount; ++pidIndex) {
        if (runtime->MiniFilterBypassPids[pidIndex] == ProcessId) {
            matchedPid = TRUE;
            break;
        }
    }
    ExReleasePushLockShared(&runtime->MiniFilterBypassPidLock);

    return matchedPid;
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
KswordARKCallbackIoctlSetMinifilterBypassPids(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
/*++

Routine Description:

    Validate the R3 minifilter bypass PID packet and replace the runtime PID
    whitelist. The actual storage update is delegated to the callback runtime
    helper so dispatch remains thin.

Arguments:

    Request - WDF request that carries KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST.
    InputBufferLength - Caller supplied input buffer length.
    CompleteBytesOut - Receives the consumed input byte count on success.

Return Value:

    STATUS_SUCCESS when the whitelist is updated; otherwise an NTSTATUS
    validation or WDF buffer retrieval error.

--*/
{
    KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST* requestPacket = NULL;
    PVOID inputBuffer = NULL;
    size_t inputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    if (InputBufferLength < sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST),
        &inputBuffer,
        &inputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (inputLength < sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    requestPacket = (KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST*)inputBuffer;
    if (requestPacket->size < sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST) ||
        requestPacket->version != KSWORD_ARK_CALLBACK_PROTOCOL_VERSION ||
        requestPacket->pidCount > KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordArkCallbackSetMinifilterBypassPids(
        requestPacket->processIds,
        requestPacket->pidCount);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *CompleteBytesOut = sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKCallbackIoctlQueryMinifilterBypassPids(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
/*++

Routine Description:

    Retrieve the caller output buffer and return the current minifilter bypass
    PID whitelist as one fixed shared-protocol response packet.

Arguments:

    Request - WDF request that owns the output buffer.
    OutputBufferLength - Caller supplied output buffer length.
    CompleteBytesOut - Receives the response byte count on success.

Return Value:

    STATUS_SUCCESS when the response is filled; otherwise an NTSTATUS
    validation or WDF buffer retrieval error.

--*/
{
    KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE* responsePacket = NULL;
    size_t outputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    if (OutputBufferLength < sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE),
        (PVOID*)&responsePacket,
        &outputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return KswordArkCallbackQueryMinifilterBypassPids(
        responsePacket,
        outputLength,
        CompleteBytesOut);
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
