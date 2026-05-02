/*++

Module Name:

    minifilter_callback.c

Abstract:

    Callback-rule enforcement for the shared KswordARK file-system minifilter.

Environment:

    Kernel-mode minifilter

--*/

#include "callback_internal.h"
#include "ark/ark_file_monitor.h"
#include "../file_monitor/file_monitor_internal.h"

static BOOLEAN
KswordArkMinifilterBuildTargetPath(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Out_writes_(TargetChars) PWCHAR TargetBuffer,
    _In_ USHORT TargetChars
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PFLT_FILE_NAME_INFORMATION nameInformation = NULL;
    UNICODE_STRING fallbackName;

    if (TargetBuffer == NULL || TargetChars == 0U) {
        return FALSE;
    }

    TargetBuffer[0] = L'\0';
    RtlZeroMemory(&fallbackName, sizeof(fallbackName));

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInformation);
    if (NT_SUCCESS(status) && nameInformation != NULL) {
        (VOID)FltParseFileNameInformation(nameInformation);
        KswordArkCopyUnicodeToFixedBuffer(&nameInformation->Name, TargetBuffer, TargetChars);
        FltReleaseFileNameInformation(nameInformation);
        return (TargetBuffer[0] != L'\0') ? TRUE : FALSE;
    }

    if (nameInformation != NULL) {
        FltReleaseFileNameInformation(nameInformation);
    }

    if (FltObjects != NULL &&
        FltObjects->FileObject != NULL &&
        FltObjects->FileObject->FileName.Buffer != NULL &&
        FltObjects->FileObject->FileName.Length != 0U) {
        fallbackName = FltObjects->FileObject->FileName;
        KswordArkCopyUnicodeToFixedBuffer(&fallbackName, TargetBuffer, TargetChars);
    }

    return (TargetBuffer[0] != L'\0') ? TRUE : FALSE;
}

static ULONG
KswordArkMinifilterGetRequestorProcessId(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    if (Data == NULL) {
        return 0UL;
    }

    return (ULONG)(ULONG_PTR)FltGetRequestorProcessId(Data);
}

FLT_PREOP_CALLBACK_STATUS
KswordArkMinifilterApplyRule(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ ULONG OperationType
    )
{
    KSWORD_ARK_CALLBACK_MATCH_RESULT matchResult;
    KSWORD_ARK_CALLBACK_EVENT_INPUT eventInput;
    UNICODE_STRING initiatorPath;
    UNICODE_STRING targetPath;
    WCHAR initiatorPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS] = { 0 };
    WCHAR targetPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS] = { 0 };
    BOOLEAN initiatorPathUnavailable = TRUE;
    BOOLEAN targetPathAvailable = FALSE;
    ULONG decision = KSWORD_ARK_DECISION_ALLOW;
    ULONG requestorProcessId = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(&matchResult, sizeof(matchResult));
    RtlZeroMemory(&eventInput, sizeof(eventInput));
    RtlZeroMemory(&initiatorPath, sizeof(initiatorPath));
    RtlZeroMemory(&targetPath, sizeof(targetPath));

    if (Data == NULL || OperationType == 0UL) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    requestorProcessId = KswordArkMinifilterGetRequestorProcessId(Data);

    (VOID)KswordArkResolveProcessImagePath(
        PsGetCurrentProcess(),
        initiatorPathBuffer,
        RTL_NUMBER_OF(initiatorPathBuffer),
        &initiatorPathUnavailable);
    RtlInitUnicodeString(&initiatorPath, initiatorPathBuffer);

    targetPathAvailable = KswordArkMinifilterBuildTargetPath(
        Data,
        FltObjects,
        targetPathBuffer,
        RTL_NUMBER_OF(targetPathBuffer));
    RtlInitUnicodeString(&targetPath, targetPathBuffer);

    status = KswordArkCallbackMatchRule(
        KSWORD_ARK_CALLBACK_TYPE_MINIFILTER,
        OperationType,
        &initiatorPath,
        &targetPath,
        &matchResult);
    if (!NT_SUCCESS(status) || !matchResult.Matched) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (matchResult.Action == KSWORD_ARK_RULE_ACTION_LOG_ONLY) {
        KswordArkCallbackLogFormat(
            "Info",
            "Minifilter callback log rule hit, op=0x%08lX, pid=%lu, groupId=%lu, ruleId=%lu.",
            (unsigned long)OperationType,
            (unsigned long)requestorProcessId,
            (unsigned long)matchResult.GroupId,
            (unsigned long)matchResult.RuleId);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (matchResult.Action == KSWORD_ARK_RULE_ACTION_DENY) {
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0U;
        KswordArkCallbackLogFormat(
            "Warn",
            "Minifilter callback denied, op=0x%08lX, pid=%lu, groupId=%lu, ruleId=%lu.",
            (unsigned long)OperationType,
            (unsigned long)requestorProcessId,
            (unsigned long)matchResult.GroupId,
            (unsigned long)matchResult.RuleId);
        return FLT_PREOP_COMPLETE;
    }

    if (matchResult.Action == KSWORD_ARK_RULE_ACTION_ALLOW) {
        KswordArkCallbackLogFormat(
            "Info",
            "Minifilter callback allow rule hit, op=0x%08lX, pid=%lu, groupId=%lu, ruleId=%lu.",
            (unsigned long)OperationType,
            (unsigned long)requestorProcessId,
            (unsigned long)matchResult.GroupId,
            (unsigned long)matchResult.RuleId);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (matchResult.Action != KSWORD_ARK_RULE_ACTION_ASK_USER) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        decision = matchResult.AskDefaultDecision;
    }
    else {
        eventInput.CallbackType = KSWORD_ARK_CALLBACK_TYPE_MINIFILTER;
        eventInput.OperationType = OperationType;
        eventInput.OriginatingPid = requestorProcessId;
        eventInput.OriginatingTid = HandleToULong(PsGetCurrentThreadId());
        eventInput.SessionId = KswordArkGetProcessSessionIdSafe(PsGetCurrentProcess());
        eventInput.PathUnavailable = (!targetPathAvailable || initiatorPathUnavailable) ? 1UL : 0UL;
        eventInput.InitiatorPath = initiatorPath;
        eventInput.TargetPath = targetPath;
        eventInput.Match = matchResult;
        decision = matchResult.AskDefaultDecision;
        status = KswordArkCallbackAskUserDecision(&eventInput, &decision);
        if (!NT_SUCCESS(status)) {
            KswordArkCallbackLogFormat(
                "Warn",
                "Minifilter ask-user fallback used, op=0x%08lX, pid=%lu, groupId=%lu, ruleId=%lu, status=0x%08X.",
                (unsigned long)OperationType,
                (unsigned long)requestorProcessId,
                (unsigned long)matchResult.GroupId,
                (unsigned long)matchResult.RuleId,
                (unsigned int)status);
        }
    }

    if (decision == KSWORD_ARK_DECISION_DENY) {
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0U;
        KswordArkCallbackLogFormat(
            "Warn",
            "Minifilter ask-user decision DENY, op=0x%08lX, pid=%lu, groupId=%lu, ruleId=%lu.",
            (unsigned long)OperationType,
            (unsigned long)requestorProcessId,
            (unsigned long)matchResult.GroupId,
            (unsigned long)matchResult.RuleId);
        return FLT_PREOP_COMPLETE;
    }

    KswordArkCallbackLogFormat(
        "Info",
        "Minifilter ask-user decision ALLOW, op=0x%08lX, pid=%lu, groupId=%lu, ruleId=%lu.",
        (unsigned long)OperationType,
        (unsigned long)requestorProcessId,
        (unsigned long)matchResult.GroupId,
        (unsigned long)matchResult.RuleId);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}
