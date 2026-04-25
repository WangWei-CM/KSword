/*++

Module Name:

    callback_waiter.c

Abstract:

    WAIT/ANSWER/CANCEL model and pending-decision context management.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_internal.h"

static VOID
KswordArkPendingDecisionReference(
    _In_ KSWORD_ARK_PENDING_DECISION* pendingDecision
    )
{
    if (pendingDecision != NULL) {
        (VOID)InterlockedIncrement(&pendingDecision->RefCount);
    }
}

static VOID
KswordArkPendingDecisionRelease(
    _In_opt_ KSWORD_ARK_PENDING_DECISION* pendingDecision
    )
{
    if (pendingDecision == NULL) {
        return;
    }

    if (InterlockedDecrement(&pendingDecision->RefCount) == 0) {
        ExFreePoolWithTag(pendingDecision, KSWORD_ARK_CALLBACK_TAG_PENDING);
    }
}

static KSWORD_ARK_PENDING_DECISION*
KswordArkFindPendingDecisionByGuidLocked(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime,
    _In_ const KSWORD_ARK_GUID128* eventGuid
    )
{
    PLIST_ENTRY currentEntry = NULL;

    if (runtime == NULL || eventGuid == NULL) {
        return NULL;
    }

    currentEntry = runtime->PendingDecisionList.Flink;
    while (currentEntry != &runtime->PendingDecisionList) {
        KSWORD_ARK_PENDING_DECISION* currentDecision =
            CONTAINING_RECORD(currentEntry, KSWORD_ARK_PENDING_DECISION, Link);
        if (KswordArkGuidEquals(&currentDecision->EventGuid, eventGuid)) {
            return currentDecision;
        }
        currentEntry = currentEntry->Flink;
    }

    return NULL;
}

static VOID
KswordArkInsertPendingDecision(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime,
    _In_ KSWORD_ARK_PENDING_DECISION* pendingDecision
    )
{
    ExAcquirePushLockExclusive(&runtime->PendingLock);
    InsertTailList(&runtime->PendingDecisionList, &pendingDecision->Link);
    KswordArkPendingDecisionReference(pendingDecision); // list reference
    (VOID)InterlockedIncrement(&runtime->PendingDecisionCount);
    ExReleasePushLockExclusive(&runtime->PendingLock);
}

static VOID
KswordArkRemovePendingDecision(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime,
    _In_ KSWORD_ARK_PENDING_DECISION* pendingDecision
    )
{
    BOOLEAN removed = FALSE;

    ExAcquirePushLockExclusive(&runtime->PendingLock);
    if (pendingDecision->Link.Flink != NULL && pendingDecision->Link.Blink != NULL) {
        RemoveEntryList(&pendingDecision->Link);
        pendingDecision->Link.Flink = NULL;
        pendingDecision->Link.Blink = NULL;
        removed = TRUE;
    }
    ExReleasePushLockExclusive(&runtime->PendingLock);

    if (removed) {
        (VOID)InterlockedDecrement(&runtime->PendingDecisionCount);
        KswordArkPendingDecisionRelease(pendingDecision); // drop list reference
    }
}

static VOID
KswordArkBuildEventPacket(
    _In_ const KSWORD_ARK_PENDING_DECISION* pendingDecision,
    _Out_ KSWORD_ARK_CALLBACK_EVENT_PACKET* packetOut
    )
{
    if (pendingDecision == NULL || packetOut == NULL) {
        return;
    }

    RtlZeroMemory(packetOut, sizeof(*packetOut));
    packetOut->size = sizeof(*packetOut);
    packetOut->version = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;
    packetOut->eventGuid = pendingDecision->EventGuid;
    packetOut->callbackType = pendingDecision->CallbackType;
    packetOut->operationType = pendingDecision->OperationType;
    packetOut->action = pendingDecision->Match.Action;
    packetOut->matchMode = pendingDecision->Match.MatchMode;
    packetOut->defaultDecision = pendingDecision->DefaultDecision;
    packetOut->timeoutMs = pendingDecision->TimeoutMs;
    packetOut->groupId = pendingDecision->Match.GroupId;
    packetOut->ruleId = pendingDecision->Match.RuleId;
    packetOut->groupPriority = pendingDecision->Match.GroupPriority;
    packetOut->rulePriority = pendingDecision->Match.RulePriority;
    packetOut->originatingPid = pendingDecision->OriginatingPid;
    packetOut->originatingTid = pendingDecision->OriginatingTid;
    packetOut->sessionId = pendingDecision->SessionId;
    packetOut->pathUnavailable = pendingDecision->PathUnavailable;
    packetOut->createdAtUtc100ns = (ULONG64)pendingDecision->CreatedAtUtc100ns.QuadPart;
    packetOut->deadlineUtc100ns = (ULONG64)pendingDecision->DeadlineUtc100ns.QuadPart;

    KswordArkCopyWideStringToFixedBuffer(
        pendingDecision->InitiatorPath,
        packetOut->initiatorPath,
        RTL_NUMBER_OF(packetOut->initiatorPath));
    KswordArkCopyWideStringToFixedBuffer(
        pendingDecision->TargetPath,
        packetOut->targetPath,
        RTL_NUMBER_OF(packetOut->targetPath));
    KswordArkCopyWideStringToFixedBuffer(
        pendingDecision->Match.RuleInitiatorPattern,
        packetOut->ruleInitiatorPattern,
        RTL_NUMBER_OF(packetOut->ruleInitiatorPattern));
    KswordArkCopyWideStringToFixedBuffer(
        pendingDecision->Match.RuleTargetPattern,
        packetOut->ruleTargetPattern,
        RTL_NUMBER_OF(packetOut->ruleTargetPattern));
    KswordArkCopyWideStringToFixedBuffer(
        pendingDecision->Match.GroupName,
        packetOut->groupName,
        RTL_NUMBER_OF(packetOut->groupName));
    KswordArkCopyWideStringToFixedBuffer(
        pendingDecision->Match.RuleName,
        packetOut->ruleName,
        RTL_NUMBER_OF(packetOut->ruleName));
}

static NTSTATUS
KswordArkDispatchEventToWaitingRequest(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime,
    _In_ const KSWORD_ARK_PENDING_DECISION* pendingDecision
    )
{
    WDFREQUEST waitRequest = WDF_NO_HANDLE;
    KSWORD_ARK_CALLBACK_EVENT_PACKET eventPacket;
    NTSTATUS status = STATUS_SUCCESS;
    PVOID outputBuffer = NULL;
    size_t outputLength = 0;

    if (runtime == NULL || runtime->WaitQueue == WDF_NO_HANDLE || pendingDecision == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = WdfIoQueueRetrieveNextRequest(runtime->WaitQueue, &waitRequest);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfRequestRetrieveOutputBuffer(
        waitRequest,
        sizeof(KSWORD_ARK_CALLBACK_EVENT_PACKET),
        &outputBuffer,
        &outputLength);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(waitRequest, status, 0U);
        return status;
    }

    KswordArkBuildEventPacket(pendingDecision, &eventPacket);
    RtlCopyMemory(outputBuffer, &eventPacket, sizeof(eventPacket));
    WdfRequestCompleteWithInformation(waitRequest, STATUS_SUCCESS, sizeof(eventPacket));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordArkCallbackWaiterInitialize(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    NTSTATUS status = STATUS_SUCCESS;

    if (runtime == NULL || runtime->Device == WDF_NO_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ParentObject = runtime->Device;

    status = WdfIoQueueCreate(
        runtime->Device,
        &queueConfig,
        &queueAttributes,
        &runtime->WaitQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

VOID
KswordArkCallbackWaiterUninitialize(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    if (runtime == NULL) {
        return;
    }

    (VOID)KswordArkCallbackCancelAllPendingInternal();
    if (runtime->WaitQueue != WDF_NO_HANDLE) {
        WdfIoQueuePurgeSynchronously(runtime->WaitQueue);
        WdfObjectDelete(runtime->WaitQueue);
        runtime->WaitQueue = WDF_NO_HANDLE;
    }
}

NTSTATUS
KswordArkCallbackIoctlWaitEventInternal(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    if (runtime == NULL || runtime->WaitQueue == WDF_NO_HANDLE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = WdfRequestForwardToIoQueue(Request, runtime->WaitQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_PENDING;
}

NTSTATUS
KswordArkCallbackIoctlAnswerEventInternal(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    KSWORD_ARK_CALLBACK_ANSWER_REQUEST* answerRequest = NULL;
    size_t answerLength = 0;
    KSWORD_ARK_PENDING_DECISION* matchedDecision = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    if (runtime == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_CALLBACK_ANSWER_REQUEST),
        (PVOID*)&answerRequest,
        &answerLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (InputBufferLength < sizeof(KSWORD_ARK_CALLBACK_ANSWER_REQUEST) ||
        answerRequest->size < sizeof(KSWORD_ARK_CALLBACK_ANSWER_REQUEST) ||
        answerRequest->version != KSWORD_ARK_CALLBACK_PROTOCOL_VERSION) {
        return STATUS_INVALID_PARAMETER;
    }

    if (answerRequest->decision != KSWORD_ARK_DECISION_ALLOW &&
        answerRequest->decision != KSWORD_ARK_DECISION_DENY) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquirePushLockShared(&runtime->PendingLock);
    matchedDecision = KswordArkFindPendingDecisionByGuidLocked(runtime, &answerRequest->eventGuid);
    if (matchedDecision != NULL) {
        KswordArkPendingDecisionReference(matchedDecision);
    }
    ExReleasePushLockShared(&runtime->PendingLock);

    if (matchedDecision == NULL) {
        return STATUS_NOT_FOUND;
    }

    if (InterlockedCompareExchange(&matchedDecision->Answered, 1L, 0L) != 0L) {
        KswordArkPendingDecisionRelease(matchedDecision);
        return STATUS_ALREADY_COMMITTED;
    }

    matchedDecision->FinalDecision = answerRequest->decision;
    KeSetEvent(&matchedDecision->DecisionEvent, IO_NO_INCREMENT, FALSE);
    *CompleteBytesOut = sizeof(KSWORD_ARK_CALLBACK_ANSWER_REQUEST);
    KswordArkPendingDecisionRelease(matchedDecision);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordArkCallbackCancelAllPendingInternal(
    VOID
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    LIST_ENTRY localList;

    if (runtime == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    InitializeListHead(&localList);

    ExAcquirePushLockExclusive(&runtime->PendingLock);
    while (!IsListEmpty(&runtime->PendingDecisionList)) {
        PLIST_ENTRY entry = RemoveHeadList(&runtime->PendingDecisionList);
        InsertTailList(&localList, entry);
        (VOID)InterlockedDecrement(&runtime->PendingDecisionCount);
    }
    ExReleasePushLockExclusive(&runtime->PendingLock);

    while (!IsListEmpty(&localList)) {
        PLIST_ENTRY entry = RemoveHeadList(&localList);
        KSWORD_ARK_PENDING_DECISION* pendingDecision =
            CONTAINING_RECORD(entry, KSWORD_ARK_PENDING_DECISION, Link);
        pendingDecision->Link.Flink = NULL;
        pendingDecision->Link.Blink = NULL;
        if (InterlockedCompareExchange(&pendingDecision->Answered, 1L, 0L) == 0L) {
            pendingDecision->FinalDecision = pendingDecision->DefaultDecision;
            KeSetEvent(&pendingDecision->DecisionEvent, IO_NO_INCREMENT, FALSE);
        }
        KswordArkPendingDecisionRelease(pendingDecision); // drop list reference
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordArkCallbackAskUserDecision(
    _In_ const KSWORD_ARK_CALLBACK_EVENT_INPUT* eventInput,
    _Out_ ULONG* decisionOut
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    KSWORD_ARK_PENDING_DECISION* pendingDecision = NULL;
    NTSTATUS dispatchStatus = STATUS_SUCCESS;
    NTSTATUS waitStatus = STATUS_SUCCESS;
    LARGE_INTEGER timeoutInterval = { 0 };
    ULONGLONG timeout100ns = 0;
    ULONG waitTimeoutMs = 0;

    if (decisionOut == NULL || eventInput == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *decisionOut = KSWORD_ARK_DECISION_ALLOW;

    if (runtime == NULL || runtime->WaitQueue == WDF_NO_HANDLE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (KeGetCurrentIrql() > APC_LEVEL) {
        *decisionOut = eventInput->Match.AskDefaultDecision;
        if (*decisionOut != KSWORD_ARK_DECISION_DENY) {
            *decisionOut = KSWORD_ARK_DECISION_ALLOW;
        }
        return STATUS_UNSUCCESSFUL;
    }

    pendingDecision = (KSWORD_ARK_PENDING_DECISION*)KswordArkAllocateNonPaged(
        sizeof(KSWORD_ARK_PENDING_DECISION),
        KSWORD_ARK_CALLBACK_TAG_PENDING);
    if (pendingDecision == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(pendingDecision, sizeof(*pendingDecision));

    pendingDecision->RefCount = 1;
    pendingDecision->Answered = 0;
    KswordArkGuidGenerate(&pendingDecision->EventGuid);
    KeInitializeEvent(&pendingDecision->DecisionEvent, NotificationEvent, FALSE);

    pendingDecision->CallbackType = eventInput->CallbackType;
    pendingDecision->OperationType = eventInput->OperationType;
    pendingDecision->OriginatingPid = eventInput->OriginatingPid;
    pendingDecision->OriginatingTid = eventInput->OriginatingTid;
    pendingDecision->SessionId = eventInput->SessionId;
    pendingDecision->PathUnavailable = eventInput->PathUnavailable;
    pendingDecision->Match = eventInput->Match;
    pendingDecision->TimeoutMs = eventInput->Match.AskTimeoutMs;
    if (pendingDecision->TimeoutMs == 0U) {
        pendingDecision->TimeoutMs = 5000U;
    }
    pendingDecision->DefaultDecision = eventInput->Match.AskDefaultDecision;
    if (pendingDecision->DefaultDecision != KSWORD_ARK_DECISION_ALLOW &&
        pendingDecision->DefaultDecision != KSWORD_ARK_DECISION_DENY) {
        pendingDecision->DefaultDecision = KSWORD_ARK_DECISION_ALLOW;
    }
    pendingDecision->FinalDecision = pendingDecision->DefaultDecision;
    KswordArkGetSystemTimeUtc100ns(&pendingDecision->CreatedAtUtc100ns);
    pendingDecision->DeadlineUtc100ns.QuadPart =
        pendingDecision->CreatedAtUtc100ns.QuadPart + ((LONGLONG)pendingDecision->TimeoutMs * 10000LL);

    KswordArkCopyUnicodeToFixedBuffer(
        &eventInput->InitiatorPath,
        pendingDecision->InitiatorPath,
        RTL_NUMBER_OF(pendingDecision->InitiatorPath));
    KswordArkCopyUnicodeToFixedBuffer(
        &eventInput->TargetPath,
        pendingDecision->TargetPath,
        RTL_NUMBER_OF(pendingDecision->TargetPath));

    KswordArkInsertPendingDecision(runtime, pendingDecision);
    dispatchStatus = KswordArkDispatchEventToWaitingRequest(runtime, pendingDecision);
    if (!NT_SUCCESS(dispatchStatus)) {
        KswordArkRemovePendingDecision(runtime, pendingDecision);
        *decisionOut = pendingDecision->DefaultDecision;
        KswordArkPendingDecisionRelease(pendingDecision); // owner release
        KswordArkCallbackLogFormat(
            "Warn",
            "AskUser fallback default: no waiting receiver, callback=%lu, op=0x%08lX, groupId=%lu, ruleId=%lu.",
            (unsigned long)eventInput->CallbackType,
            (unsigned long)eventInput->OperationType,
            (unsigned long)eventInput->Match.GroupId,
            (unsigned long)eventInput->Match.RuleId);
        return STATUS_NOT_FOUND;
    }

    waitTimeoutMs = pendingDecision->TimeoutMs;
    if (waitTimeoutMs > 600000UL) {
        waitTimeoutMs = 600000UL;
    }
    timeout100ns = (ULONGLONG)waitTimeoutMs * 10000ULL;
    if (timeout100ns > (ULONGLONG)MAXLONGLONG) {
        timeout100ns = (ULONGLONG)MAXLONGLONG;
    }
    timeoutInterval.QuadPart = -(LONGLONG)timeout100ns;

    waitStatus = KeWaitForSingleObject(
        &pendingDecision->DecisionEvent,
        Executive,
        KernelMode,
        FALSE,
        &timeoutInterval);
    if (waitStatus == STATUS_TIMEOUT) {
        pendingDecision->FinalDecision = pendingDecision->DefaultDecision;
        *decisionOut = pendingDecision->DefaultDecision;
        KswordArkCallbackLogFormat(
            "Warn",
            "AskUser timeout default applied, callback=%lu, op=0x%08lX, groupId=%lu, ruleId=%lu.",
            (unsigned long)eventInput->CallbackType,
            (unsigned long)eventInput->OperationType,
            (unsigned long)eventInput->Match.GroupId,
            (unsigned long)eventInput->Match.RuleId);
    }
    else {
        *decisionOut = pendingDecision->FinalDecision;
    }

    KswordArkRemovePendingDecision(runtime, pendingDecision);
    KswordArkPendingDecisionRelease(pendingDecision); // owner release
    return STATUS_SUCCESS;
}

ULONG
KswordArkCallbackGetWaitingRequestCount(
    VOID
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    ULONG queueRequests = 0U;

    if (runtime == NULL || runtime->WaitQueue == WDF_NO_HANDLE) {
        return 0U;
    }

    (VOID)WdfIoQueueGetState(runtime->WaitQueue, &queueRequests, NULL);
    return queueRequests;
}

ULONG
KswordArkCallbackGetPendingDecisionCount(
    VOID
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    if (runtime == NULL) {
        return 0U;
    }

    return (ULONG)InterlockedCompareExchange(&runtime->PendingDecisionCount, 0L, 0L);
}
