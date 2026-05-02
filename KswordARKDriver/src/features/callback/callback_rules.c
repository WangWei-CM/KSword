/*++

Module Name:

    callback_rules.c

Abstract:

    Rule blob validation/build, active snapshot swap and hot-path matching.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_internal.h"

typedef struct _KSWORD_ARK_GROUP_VIEW
{
    ULONG GroupId;
    ULONG Flags;
    ULONG Priority;
    ULONG NameOffsetBytes;
    USHORT NameLengthChars;
} KSWORD_ARK_GROUP_VIEW;

static ULONG
KswordArkCallbackCrc32Step(
    _In_reads_bytes_(dataLength) const UCHAR* dataBuffer,
    _In_ ULONG dataLength,
    _In_ ULONG currentCrc
    )
{
    ULONG crcValue = currentCrc;
    ULONG byteIndex = 0;
    ULONG bitIndex = 0;

    if (dataBuffer == NULL || dataLength == 0U) {
        return crcValue;
    }

    for (byteIndex = 0; byteIndex < dataLength; ++byteIndex) {
        crcValue ^= (ULONG)dataBuffer[byteIndex];
        for (bitIndex = 0; bitIndex < 8U; ++bitIndex) {
            if ((crcValue & 1U) != 0U) {
                crcValue = (crcValue >> 1U) ^ 0xEDB88320UL;
            }
            else {
                crcValue >>= 1U;
            }
        }
    }

    return crcValue;
}

static ULONG
KswordArkCallbackCrc32(
    _In_reads_bytes_(dataLength) const UCHAR* dataBuffer,
    _In_ ULONG dataLength
    )
{
    ULONG runningCrc = 0xFFFFFFFFUL;
    runningCrc = KswordArkCallbackCrc32Step(dataBuffer, dataLength, runningCrc);
    return ~runningCrc;
}

static BOOLEAN
KswordArkCallbackValidateBlobString(
    _In_reads_bytes_(stringBytes) const UCHAR* stringBase,
    _In_ ULONG stringBytes,
    _In_ ULONG offsetBytes,
    _In_ USHORT lengthChars
    )
{
    ULONG requiredBytes = 0;
    const WCHAR* stringPointer = NULL;

    if (lengthChars == 0U) {
        return TRUE;
    }

    if (stringBase == NULL || stringBytes == 0U) {
        return FALSE;
    }

    if ((offsetBytes % sizeof(WCHAR)) != 0U) {
        return FALSE;
    }

    requiredBytes = ((ULONG)lengthChars + 1UL) * (ULONG)sizeof(WCHAR);
    if (offsetBytes > stringBytes || requiredBytes > stringBytes || (offsetBytes + requiredBytes) > stringBytes) {
        return FALSE;
    }

    stringPointer = (const WCHAR*)(stringBase + offsetBytes);
    if (stringPointer[lengthChars] != L'\0') {
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
KswordArkCallbackActionIsValidForType(
    _In_ ULONG callbackType,
    _In_ ULONG actionType,
    _In_ ULONG matchMode
    )
{
    switch (callbackType) {
    case KSWORD_ARK_CALLBACK_TYPE_REGISTRY:
        if (actionType != KSWORD_ARK_RULE_ACTION_ALLOW &&
            actionType != KSWORD_ARK_RULE_ACTION_DENY &&
            actionType != KSWORD_ARK_RULE_ACTION_ASK_USER &&
            actionType != KSWORD_ARK_RULE_ACTION_LOG_ONLY) {
            return FALSE;
        }
        if (matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
            actionType != KSWORD_ARK_RULE_ACTION_ASK_USER) {
            return FALSE;
        }
        return TRUE;

    case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE:
        if (actionType == KSWORD_ARK_RULE_ACTION_ALLOW ||
            actionType == KSWORD_ARK_RULE_ACTION_DENY ||
            actionType == KSWORD_ARK_RULE_ACTION_LOG_ONLY) {
            return TRUE;
        }
        return FALSE;

    case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE:
    case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD:
        return (actionType == KSWORD_ARK_RULE_ACTION_LOG_ONLY) ? TRUE : FALSE;

    case KSWORD_ARK_CALLBACK_TYPE_OBJECT:
        if (actionType == KSWORD_ARK_RULE_ACTION_ALLOW ||
            actionType == KSWORD_ARK_RULE_ACTION_STRIP_ACCESS ||
            actionType == KSWORD_ARK_RULE_ACTION_LOG_ONLY) {
            return TRUE;
        }
        return FALSE;

    case KSWORD_ARK_CALLBACK_TYPE_MINIFILTER:
        if (actionType == KSWORD_ARK_RULE_ACTION_ALLOW ||
            actionType == KSWORD_ARK_RULE_ACTION_DENY ||
            actionType == KSWORD_ARK_RULE_ACTION_ASK_USER ||
            actionType == KSWORD_ARK_RULE_ACTION_LOG_ONLY) {
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static int __cdecl
KswordArkCallbackRuntimeRuleCompare(
    _In_ const VOID* leftRule,
    _In_ const VOID* rightRule
    )
{
    const KSWORD_ARK_RUNTIME_RULE* left = (const KSWORD_ARK_RUNTIME_RULE*)leftRule;
    const KSWORD_ARK_RUNTIME_RULE* right = (const KSWORD_ARK_RUNTIME_RULE*)rightRule;

    if (left->CallbackType != right->CallbackType) {
        return (left->CallbackType < right->CallbackType) ? -1 : 1;
    }
    if (left->GroupPriority != right->GroupPriority) {
        return (left->GroupPriority < right->GroupPriority) ? -1 : 1;
    }
    if (left->RulePriority != right->RulePriority) {
        return (left->RulePriority < right->RulePriority) ? -1 : 1;
    }
    if (left->GroupId != right->GroupId) {
        return (left->GroupId < right->GroupId) ? -1 : 1;
    }
    if (left->RuleId != right->RuleId) {
        return (left->RuleId < right->RuleId) ? -1 : 1;
    }
    return 0;
}

static VOID
KswordArkCallbackSortRuntimeRules(
    _Inout_updates_(ruleCount) KSWORD_ARK_RUNTIME_RULE* runtimeRules,
    _In_ ULONG ruleCount
    )
{
    ULONG outerIndex = 0;
    ULONG innerIndex = 0;

    if (runtimeRules == NULL || ruleCount <= 1U) {
        return;
    }

    for (outerIndex = 1U; outerIndex < ruleCount; ++outerIndex) {
        KSWORD_ARK_RUNTIME_RULE currentRule = runtimeRules[outerIndex];
        innerIndex = outerIndex;

        while (innerIndex > 0U &&
            KswordArkCallbackRuntimeRuleCompare(&runtimeRules[innerIndex - 1U], &currentRule) > 0) {
            runtimeRules[innerIndex] = runtimeRules[innerIndex - 1U];
            innerIndex -= 1U;
        }
        runtimeRules[innerIndex] = currentRule;
    }
}

static KSWORD_ARK_GROUP_VIEW*
KswordArkCallbackFindGroupView(
    _Inout_updates_(groupCount) KSWORD_ARK_GROUP_VIEW* groupViews,
    _In_ ULONG groupCount,
    _In_ ULONG groupId
    )
{
    ULONG groupIndex = 0;

    if (groupViews == NULL || groupCount == 0U || groupId == 0U) {
        return NULL;
    }

    for (groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        if (groupViews[groupIndex].GroupId == groupId) {
            return &groupViews[groupIndex];
        }
    }
    return NULL;
}

static KSWORD_ARK_CALLBACK_RULE_SNAPSHOT*
KswordArkCallbackAcquireSnapshot(
    VOID
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot = NULL;

    if (runtime == NULL) {
        return NULL;
    }

    ExAcquirePushLockShared(&runtime->SnapshotLock);
    snapshot = runtime->ActiveSnapshot;
    if (snapshot != NULL) {
        if (!ExAcquireRundownProtection(&snapshot->RundownRef)) {
            snapshot = NULL;
        }
    }
    ExReleasePushLockShared(&runtime->SnapshotLock);
    return snapshot;
}

static VOID
KswordArkCallbackReleaseSnapshot(
    _In_opt_ KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot
    )
{
    if (snapshot != NULL) {
        ExReleaseRundownProtection(&snapshot->RundownRef);
    }
}

NTSTATUS
KswordArkCallbackBuildSnapshotFromBlob(
    _In_reads_bytes_(blobBytes) const VOID* blobData,
    _In_ size_t blobBytes,
    _Outptr_ KSWORD_ARK_CALLBACK_RULE_SNAPSHOT** snapshotOut
    )
{
    const UCHAR* blobBytesPtr = (const UCHAR*)blobData;
    const KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER* header = NULL;
    const KSWORD_ARK_CALLBACK_GROUP_BLOB* groupTable = NULL;
    const KSWORD_ARK_CALLBACK_RULE_BLOB* ruleTable = NULL;
    const UCHAR* stringPool = NULL;
    KSWORD_ARK_GROUP_VIEW groupViews[KSWORD_ARK_CALLBACK_MAX_GROUP_COUNT] = { 0 };
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot = NULL;
    KSWORD_ARK_RUNTIME_RULE* runtimeRules = NULL;
    UCHAR* snapshotStringPool = NULL;
    ULONG activeRuleCount = 0;
    ULONG ruleIndex = 0;
    ULONG groupIndex = 0;
    ULONG expectedCrc32 = 0;
    ULONG calculatedCrc32 = 0;
    KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER headerCopy = { 0 };
    ULONG runningCrc = 0xFFFFFFFFUL;
    size_t snapshotRulesBytes = 0;
    size_t snapshotBytes = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (snapshotOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *snapshotOut = NULL;

    if (blobData == NULL || blobBytes < sizeof(KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    header = (const KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER*)blobData;
    if (header->magic != KSWORD_ARK_CALLBACK_RULE_BLOB_MAGIC ||
        header->protocolVersion != KSWORD_ARK_CALLBACK_PROTOCOL_VERSION ||
        header->schemaVersion != KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }

    if (header->size > blobBytes ||
        header->size < sizeof(KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    if (header->groupCount > KSWORD_ARK_CALLBACK_MAX_GROUP_COUNT ||
        header->ruleCount > KSWORD_ARK_CALLBACK_MAX_RULE_COUNT ||
        header->stringBytes > KSWORD_ARK_CALLBACK_MAX_STRING_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    if (header->groupOffsetBytes > header->size ||
        header->ruleOffsetBytes > header->size ||
        header->stringOffsetBytes > header->size) {
        return STATUS_INVALID_PARAMETER;
    }

    if (header->groupCount != 0U) {
        size_t groupTableBytes = (size_t)header->groupCount * sizeof(KSWORD_ARK_CALLBACK_GROUP_BLOB);
        if ((size_t)header->groupOffsetBytes + groupTableBytes > header->size) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (header->ruleCount != 0U) {
        size_t ruleTableBytes = (size_t)header->ruleCount * sizeof(KSWORD_ARK_CALLBACK_RULE_BLOB);
        if ((size_t)header->ruleOffsetBytes + ruleTableBytes > header->size) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    if ((size_t)header->stringOffsetBytes + (size_t)header->stringBytes > header->size) {
        return STATUS_INVALID_PARAMETER;
    }

    expectedCrc32 = header->crc32;
    headerCopy = *header;
    headerCopy.crc32 = 0U;
    runningCrc = KswordArkCallbackCrc32Step((const UCHAR*)&headerCopy, sizeof(headerCopy), runningCrc);
    if (header->size > sizeof(headerCopy)) {
        runningCrc = KswordArkCallbackCrc32Step(
            blobBytesPtr + sizeof(headerCopy),
            (ULONG)(header->size - sizeof(headerCopy)),
            runningCrc);
    }
    calculatedCrc32 = ~runningCrc;
    if (expectedCrc32 != calculatedCrc32) {
        return STATUS_CRC_ERROR;
    }

    groupTable = (const KSWORD_ARK_CALLBACK_GROUP_BLOB*)(blobBytesPtr + header->groupOffsetBytes);
    ruleTable = (const KSWORD_ARK_CALLBACK_RULE_BLOB*)(blobBytesPtr + header->ruleOffsetBytes);
    stringPool = blobBytesPtr + header->stringOffsetBytes;

    for (groupIndex = 0; groupIndex < header->groupCount; ++groupIndex) {
        const KSWORD_ARK_CALLBACK_GROUP_BLOB* groupBlob = &groupTable[groupIndex];
        KSWORD_ARK_GROUP_VIEW* existingGroup = NULL;

        if ((groupBlob->flags & (~KSWORD_ARK_CALLBACK_GROUP_FLAG_ENABLED)) != 0U) {
            return STATUS_INVALID_PARAMETER;
        }
        if (groupBlob->groupId == 0U) {
            return STATUS_INVALID_PARAMETER;
        }

        existingGroup = KswordArkCallbackFindGroupView(groupViews, groupIndex, groupBlob->groupId);
        if (existingGroup != NULL) {
            return STATUS_OBJECT_NAME_COLLISION;
        }

        if (!KswordArkCallbackValidateBlobString(
            stringPool,
            header->stringBytes,
            groupBlob->nameOffsetBytes,
            groupBlob->nameLengthChars)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (!KswordArkCallbackValidateBlobString(
            stringPool,
            header->stringBytes,
            groupBlob->commentOffsetBytes,
            groupBlob->commentLengthChars)) {
            return STATUS_INVALID_PARAMETER;
        }

        groupViews[groupIndex].GroupId = groupBlob->groupId;
        groupViews[groupIndex].Flags = groupBlob->flags;
        groupViews[groupIndex].Priority = groupBlob->priority;
        groupViews[groupIndex].NameOffsetBytes = groupBlob->nameOffsetBytes;
        groupViews[groupIndex].NameLengthChars = groupBlob->nameLengthChars;
    }

    for (ruleIndex = 0; ruleIndex < header->ruleCount; ++ruleIndex) {
        const KSWORD_ARK_CALLBACK_RULE_BLOB* ruleBlob = &ruleTable[ruleIndex];
        KSWORD_ARK_GROUP_VIEW* groupView = NULL;

        if ((ruleBlob->flags & (~KSWORD_ARK_CALLBACK_RULE_FLAG_ENABLED)) != 0U) {
            return STATUS_INVALID_PARAMETER;
        }

        if (ruleBlob->callbackType == KSWORD_ARK_CALLBACK_TYPE_NONE ||
            ruleBlob->callbackType > KSWORD_ARK_CALLBACK_TYPE_MINIFILTER) {
            return STATUS_INVALID_PARAMETER;
        }

        if (ruleBlob->matchMode < KSWORD_ARK_MATCH_MODE_EXACT ||
            ruleBlob->matchMode > KSWORD_ARK_MATCH_MODE_REGEX) {
            return STATUS_INVALID_PARAMETER;
        }

        if (!KswordArkCallbackActionIsValidForType(
            ruleBlob->callbackType,
            ruleBlob->action,
            ruleBlob->matchMode)) {
            return STATUS_NOT_SUPPORTED;
        }

        if (ruleBlob->matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
            (ruleBlob->callbackType != KSWORD_ARK_CALLBACK_TYPE_REGISTRY ||
                ruleBlob->action != KSWORD_ARK_RULE_ACTION_ASK_USER)) {
            return STATUS_NOT_SUPPORTED;
        }

        groupView = KswordArkCallbackFindGroupView(groupViews, header->groupCount, ruleBlob->groupId);
        if (groupView == NULL) {
            return STATUS_NOT_FOUND;
        }

        if (!KswordArkCallbackValidateBlobString(
            stringPool,
            header->stringBytes,
            ruleBlob->initiatorOffsetBytes,
            ruleBlob->initiatorLengthChars)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (!KswordArkCallbackValidateBlobString(
            stringPool,
            header->stringBytes,
            ruleBlob->targetOffsetBytes,
            ruleBlob->targetLengthChars)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (!KswordArkCallbackValidateBlobString(
            stringPool,
            header->stringBytes,
            ruleBlob->ruleNameOffsetBytes,
            ruleBlob->ruleNameLengthChars)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (!KswordArkCallbackValidateBlobString(
            stringPool,
            header->stringBytes,
            ruleBlob->commentOffsetBytes,
            ruleBlob->commentLengthChars)) {
            return STATUS_INVALID_PARAMETER;
        }

        if ((groupView->Flags & KSWORD_ARK_CALLBACK_GROUP_FLAG_ENABLED) != 0U &&
            (ruleBlob->flags & KSWORD_ARK_CALLBACK_RULE_FLAG_ENABLED) != 0U) {
            activeRuleCount += 1UL;
        }
    }

    if (activeRuleCount == 0U) {
        snapshotRulesBytes = sizeof(KSWORD_ARK_RUNTIME_RULE);
    }
    else {
        snapshotRulesBytes = (size_t)activeRuleCount * sizeof(KSWORD_ARK_RUNTIME_RULE);
    }

    if ((FIELD_OFFSET(KSWORD_ARK_CALLBACK_RULE_SNAPSHOT, Rules) + snapshotRulesBytes) < snapshotRulesBytes) {
        return STATUS_INTEGER_OVERFLOW;
    }

    snapshotBytes = FIELD_OFFSET(KSWORD_ARK_CALLBACK_RULE_SNAPSHOT, Rules) + snapshotRulesBytes + header->stringBytes;
    if (snapshotBytes < snapshotRulesBytes) {
        return STATUS_INTEGER_OVERFLOW;
    }

    snapshot = (KSWORD_ARK_CALLBACK_RULE_SNAPSHOT*)KswordArkAllocateNonPaged(
        snapshotBytes,
        KSWORD_ARK_CALLBACK_TAG_SNAPSHOT);
    if (snapshot == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(snapshot, snapshotBytes);

    runtimeRules = &snapshot->Rules[0];
    snapshotStringPool = ((UCHAR*)snapshot) + FIELD_OFFSET(KSWORD_ARK_CALLBACK_RULE_SNAPSHOT, Rules) + snapshotRulesBytes;
    if (header->stringBytes > 0U) {
        RtlCopyMemory(snapshotStringPool, stringPool, header->stringBytes);
    }

    snapshot->GlobalFlags = header->globalFlags;
    snapshot->GroupCount = header->groupCount;
    snapshot->RuleCount = header->ruleCount;
    snapshot->ActiveRuleCount = activeRuleCount;
    snapshot->StringPoolBytes = header->stringBytes;
    snapshot->RuleVersion = header->ruleVersion;
    KswordArkGetSystemTimeUtc100ns(&snapshot->AppliedAtUtc100ns);

    activeRuleCount = 0U;
    for (ruleIndex = 0; ruleIndex < header->ruleCount; ++ruleIndex) {
        const KSWORD_ARK_CALLBACK_RULE_BLOB* ruleBlob = &ruleTable[ruleIndex];
        KSWORD_ARK_GROUP_VIEW* groupView = KswordArkCallbackFindGroupView(groupViews, header->groupCount, ruleBlob->groupId);
        KSWORD_ARK_RUNTIME_RULE* runtimeRule = NULL;
        WCHAR* namePointer = NULL;
        WCHAR* ruleNamePointer = NULL;
        WCHAR* initiatorPointer = NULL;
        WCHAR* targetPointer = NULL;

        if (groupView == NULL) {
            status = STATUS_NOT_FOUND;
            break;
        }

        if ((groupView->Flags & KSWORD_ARK_CALLBACK_GROUP_FLAG_ENABLED) == 0U ||
            (ruleBlob->flags & KSWORD_ARK_CALLBACK_RULE_FLAG_ENABLED) == 0U) {
            continue;
        }

        runtimeRule = &runtimeRules[activeRuleCount];
        runtimeRule->GroupId = ruleBlob->groupId;
        runtimeRule->RuleId = ruleBlob->ruleId;
        runtimeRule->GroupPriority = groupView->Priority;
        runtimeRule->RulePriority = ruleBlob->priority;
        runtimeRule->CallbackType = ruleBlob->callbackType;
        runtimeRule->OperationMask = ruleBlob->operationMask;
        runtimeRule->Action = ruleBlob->action;
        runtimeRule->MatchMode = ruleBlob->matchMode;
        runtimeRule->AskTimeoutMs = ruleBlob->askTimeoutMs;
        runtimeRule->AskDefaultDecision = ruleBlob->askDefaultDecision;
        if (runtimeRule->AskDefaultDecision != KSWORD_ARK_DECISION_ALLOW &&
            runtimeRule->AskDefaultDecision != KSWORD_ARK_DECISION_DENY) {
            runtimeRule->AskDefaultDecision = KSWORD_ARK_DECISION_ALLOW;
        }
        if (runtimeRule->AskTimeoutMs == 0U) {
            runtimeRule->AskTimeoutMs = 5000UL;
        }

        initiatorPointer = (WCHAR*)(snapshotStringPool + ruleBlob->initiatorOffsetBytes);
        targetPointer = (WCHAR*)(snapshotStringPool + ruleBlob->targetOffsetBytes);
        namePointer = (WCHAR*)(snapshotStringPool + groupView->NameOffsetBytes);
        ruleNamePointer = (WCHAR*)(snapshotStringPool + ruleBlob->ruleNameOffsetBytes);

        RtlInitUnicodeString(&runtimeRule->InitiatorPattern, initiatorPointer);
        runtimeRule->InitiatorPattern.Length = ruleBlob->initiatorLengthChars * (USHORT)sizeof(WCHAR);
        runtimeRule->InitiatorPattern.MaximumLength =
            runtimeRule->InitiatorPattern.Length + (USHORT)sizeof(WCHAR);

        RtlInitUnicodeString(&runtimeRule->TargetPattern, targetPointer);
        runtimeRule->TargetPattern.Length = ruleBlob->targetLengthChars * (USHORT)sizeof(WCHAR);
        runtimeRule->TargetPattern.MaximumLength =
            runtimeRule->TargetPattern.Length + (USHORT)sizeof(WCHAR);

        RtlInitUnicodeString(&runtimeRule->GroupName, namePointer);
        runtimeRule->GroupName.Length = groupView->NameLengthChars * (USHORT)sizeof(WCHAR);
        runtimeRule->GroupName.MaximumLength =
            runtimeRule->GroupName.Length + (USHORT)sizeof(WCHAR);

        RtlInitUnicodeString(&runtimeRule->RuleName, ruleNamePointer);
        runtimeRule->RuleName.Length = ruleBlob->ruleNameLengthChars * (USHORT)sizeof(WCHAR);
        runtimeRule->RuleName.MaximumLength =
            runtimeRule->RuleName.Length + (USHORT)sizeof(WCHAR);

        activeRuleCount += 1UL;
    }

    if (!NT_SUCCESS(status)) {
        KswordArkCallbackFreeSnapshot(snapshot);
        return status;
    }

    KswordArkCallbackSortRuntimeRules(runtimeRules, snapshot->ActiveRuleCount);

    for (groupIndex = 0; groupIndex <= KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED; ++groupIndex) {
        snapshot->BucketStart[groupIndex] = 0U;
        snapshot->BucketCount[groupIndex] = 0U;
    }

    for (ruleIndex = 0; ruleIndex < snapshot->ActiveRuleCount; ++ruleIndex) {
        ULONG callbackType = runtimeRules[ruleIndex].CallbackType;
        if (callbackType <= KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED) {
            if (snapshot->BucketCount[callbackType] == 0U) {
                snapshot->BucketStart[callbackType] = ruleIndex;
            }
            snapshot->BucketCount[callbackType] += 1U;
        }
    }

    ExInitializeRundownProtection(&snapshot->RundownRef);
    *snapshotOut = snapshot;
    return STATUS_SUCCESS;
}

VOID
KswordArkCallbackFreeSnapshot(
    _In_opt_ KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot
    )
{
    if (snapshot != NULL) {
        ExFreePoolWithTag(snapshot, KSWORD_ARK_CALLBACK_TAG_SNAPSHOT);
    }
}

NTSTATUS
KswordArkCallbackSwapSnapshot(
    _In_ KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* newSnapshot
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* oldSnapshot = NULL;

    if (runtime == NULL || newSnapshot == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    ExAcquirePushLockExclusive(&runtime->SnapshotLock);
    oldSnapshot = runtime->ActiveSnapshot;
    runtime->ActiveSnapshot = newSnapshot;
    ExReleasePushLockExclusive(&runtime->SnapshotLock);

    if (oldSnapshot != NULL) {
        ExWaitForRundownProtectionRelease(&oldSnapshot->RundownRef);
        KswordArkCallbackFreeSnapshot(oldSnapshot);
    }

    KswordArkCallbackLogFormat(
        "Info",
        "Callback snapshot switched, ruleVersion=%I64u, groups=%lu, rules=%lu, active=%lu.",
        (unsigned long long)newSnapshot->RuleVersion,
        (unsigned long)newSnapshot->GroupCount,
        (unsigned long)newSnapshot->RuleCount,
        (unsigned long)newSnapshot->ActiveRuleCount);
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordArkCallbackOperationMatch(
    _In_ ULONG ruleOperationMask,
    _In_ ULONG operationType
    )
{
    if (ruleOperationMask == 0U || operationType == 0U) {
        return TRUE;
    }
    return ((ruleOperationMask & operationType) != 0U) ? TRUE : FALSE;
}

static BOOLEAN
KswordArkCallbackTextMatch(
    _In_ ULONG matchMode,
    _In_ PCUNICODE_STRING patternText,
    _In_opt_ PCUNICODE_STRING valueText
    )
{
    if (patternText == NULL || patternText->Length == 0U) {
        return TRUE;
    }
    if (valueText == NULL || valueText->Buffer == NULL) {
        return FALSE;
    }

    switch (matchMode) {
    case KSWORD_ARK_MATCH_MODE_EXACT:
        return RtlEqualUnicodeString(patternText, valueText, TRUE) ? TRUE : FALSE;

    case KSWORD_ARK_MATCH_MODE_PREFIX:
        return RtlPrefixUnicodeString(patternText, valueText, TRUE) ? TRUE : FALSE;

    case KSWORD_ARK_MATCH_MODE_WILDCARD:
        __try {
            return FsRtlIsNameInExpression(
                (PUNICODE_STRING)patternText,
                (PUNICODE_STRING)valueText,
                TRUE,
                NULL)
                ? TRUE
                : FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return FALSE;
        }

    case KSWORD_ARK_MATCH_MODE_REGEX:
        // Driver side keeps regex rules deterministic-light and defers exact regex
        // confirmation to user-mode.
        return TRUE;

    default:
        return FALSE;
    }
}

NTSTATUS
KswordArkCallbackMatchRule(
    _In_ ULONG callbackType,
    _In_ ULONG operationType,
    _In_opt_ PCUNICODE_STRING initiatorPath,
    _In_opt_ PCUNICODE_STRING targetPath,
    _Out_ KSWORD_ARK_CALLBACK_MATCH_RESULT* matchResultOut
    )
{
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot = NULL;
    ULONG startIndex = 0;
    ULONG ruleCount = 0;
    ULONG currentIndex = 0;

    if (matchResultOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(matchResultOut, sizeof(*matchResultOut));
    if (callbackType == KSWORD_ARK_CALLBACK_TYPE_NONE ||
        callbackType > KSWORD_ARK_CALLBACK_TYPE_MINIFILTER) {
        return STATUS_INVALID_PARAMETER;
    }

    snapshot = KswordArkCallbackAcquireSnapshot();
    if (snapshot == NULL) {
        return STATUS_NOT_FOUND;
    }

    if ((snapshot->GlobalFlags & KSWORD_ARK_CALLBACK_GLOBAL_FLAG_ENABLED) == 0U) {
        KswordArkCallbackReleaseSnapshot(snapshot);
        return STATUS_NOT_FOUND;
    }

    startIndex = snapshot->BucketStart[callbackType];
    ruleCount = snapshot->BucketCount[callbackType];
    for (currentIndex = 0; currentIndex < ruleCount; ++currentIndex) {
        const KSWORD_ARK_RUNTIME_RULE* rule = &snapshot->Rules[startIndex + currentIndex];
        if (!KswordArkCallbackOperationMatch(rule->OperationMask, operationType)) {
            continue;
        }

        if (!KswordArkCallbackTextMatch(rule->MatchMode, &rule->InitiatorPattern, initiatorPath)) {
            continue;
        }
        if (!KswordArkCallbackTextMatch(rule->MatchMode, &rule->TargetPattern, targetPath)) {
            continue;
        }

        matchResultOut->Matched = TRUE;
        matchResultOut->CallbackType = callbackType;
        matchResultOut->RuleOperationMask = rule->OperationMask;
        matchResultOut->Action = rule->Action;
        matchResultOut->MatchMode = rule->MatchMode;
        matchResultOut->AskTimeoutMs = rule->AskTimeoutMs;
        matchResultOut->AskDefaultDecision = rule->AskDefaultDecision;
        matchResultOut->GroupId = rule->GroupId;
        matchResultOut->RuleId = rule->RuleId;
        matchResultOut->GroupPriority = rule->GroupPriority;
        matchResultOut->RulePriority = rule->RulePriority;
        KswordArkCopyUnicodeToFixedBuffer(&rule->GroupName, matchResultOut->GroupName, RTL_NUMBER_OF(matchResultOut->GroupName));
        KswordArkCopyUnicodeToFixedBuffer(&rule->RuleName, matchResultOut->RuleName, RTL_NUMBER_OF(matchResultOut->RuleName));
        KswordArkCopyUnicodeToFixedBuffer(&rule->InitiatorPattern, matchResultOut->RuleInitiatorPattern, RTL_NUMBER_OF(matchResultOut->RuleInitiatorPattern));
        KswordArkCopyUnicodeToFixedBuffer(&rule->TargetPattern, matchResultOut->RuleTargetPattern, RTL_NUMBER_OF(matchResultOut->RuleTargetPattern));
        KswordArkCallbackReleaseSnapshot(snapshot);
        return STATUS_SUCCESS;
    }

    KswordArkCallbackReleaseSnapshot(snapshot);
    return STATUS_NOT_FOUND;
}

VOID
KswordArkCallbackQueryRuntimeState(
    _Out_ KSWORD_ARK_CALLBACK_RUNTIME_STATE* runtimeStateOut
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot = NULL;

    if (runtimeStateOut == NULL) {
        return;
    }

    RtlZeroMemory(runtimeStateOut, sizeof(*runtimeStateOut));
    runtimeStateOut->size = sizeof(*runtimeStateOut);
    runtimeStateOut->version = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;
    runtimeStateOut->driverOnline = (runtime != NULL) ? 1UL : 0UL;

    if (runtime == NULL) {
        return;
    }

    runtimeStateOut->callbacksRegisteredMask = runtime->RegisteredCallbacksMask;
    runtimeStateOut->pendingDecisionCount = KswordArkCallbackGetPendingDecisionCount();
    runtimeStateOut->waitingReceiverCount = KswordArkCallbackGetWaitingRequestCount();

    snapshot = KswordArkCallbackAcquireSnapshot();
    if (snapshot == NULL) {
        return;
    }

    runtimeStateOut->globalEnabled =
        ((snapshot->GlobalFlags & KSWORD_ARK_CALLBACK_GLOBAL_FLAG_ENABLED) != 0U) ? 1UL : 0UL;
    runtimeStateOut->rulesApplied = (snapshot->ActiveRuleCount > 0U) ? 1UL : 0UL;
    runtimeStateOut->groupCount = snapshot->GroupCount;
    runtimeStateOut->ruleCount = snapshot->ActiveRuleCount;
    runtimeStateOut->appliedRuleVersion = snapshot->RuleVersion;
    runtimeStateOut->appliedAtUtc100ns = (ULONG64)snapshot->AppliedAtUtc100ns.QuadPart;

    KswordArkCallbackReleaseSnapshot(snapshot);
}
