/*++

Module Name:

    process_callback.c

Abstract:

    Process create callback registration and dispatch.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_internal.h"

static VOID
KswordArkProcessCreateNotifyEx(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    KSWORD_ARK_CALLBACK_MATCH_RESULT matchResult;
    UNICODE_STRING initiatorPath = { 0 };
    UNICODE_STRING targetPath = { 0 };
    WCHAR initiatorPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS] = { 0 };
    WCHAR targetPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS] = { 0 };
    BOOLEAN initiatorPathUnavailable = TRUE;
    ULONG operationType = KSWORD_ARK_PROCESS_OP_CREATE;
    NTSTATUS matchStatus = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Process);

    if (CreateInfo == NULL) {
        return;
    }

    (VOID)KswordArkResolveProcessImagePath(
        PsGetCurrentProcess(),
        initiatorPathBuffer,
        RTL_NUMBER_OF(initiatorPathBuffer),
        &initiatorPathUnavailable);
    RtlInitUnicodeString(&initiatorPath, initiatorPathBuffer);

    if (CreateInfo->ImageFileName != NULL &&
        CreateInfo->ImageFileName->Buffer != NULL &&
        CreateInfo->ImageFileName->Length > 0U) {
        targetPath = *CreateInfo->ImageFileName;
    }
    else {
        RtlInitUnicodeString(&targetPath, targetPathBuffer);
    }

    matchStatus = KswordArkCallbackMatchRule(
        KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE,
        operationType,
        &initiatorPath,
        &targetPath,
        &matchResult);
    if (!NT_SUCCESS(matchStatus) || !matchResult.Matched) {
        return;
    }

    if (matchResult.Action == KSWORD_ARK_RULE_ACTION_LOG_ONLY) {
        KswordArkCallbackLogFormat(
            "Info",
            "Process callback log rule hit, creatorPid=%lu, targetPid=%lu, groupId=%lu, ruleId=%lu.",
            (unsigned long)HandleToULong(PsGetCurrentProcessId()),
            (unsigned long)HandleToULong(ProcessId),
            (unsigned long)matchResult.GroupId,
            (unsigned long)matchResult.RuleId);
        return;
    }

    if (matchResult.Action == KSWORD_ARK_RULE_ACTION_DENY) {
        CreateInfo->CreationStatus = STATUS_ACCESS_DENIED;
        KswordArkCallbackLogFormat(
            "Warn",
            "Process creation denied, creatorPid=%lu, targetPid=%lu, groupId=%lu, ruleId=%lu.",
            (unsigned long)HandleToULong(PsGetCurrentProcessId()),
            (unsigned long)HandleToULong(ProcessId),
            (unsigned long)matchResult.GroupId,
            (unsigned long)matchResult.RuleId);
        return;
    }

    if (matchResult.Action == KSWORD_ARK_RULE_ACTION_ALLOW) {
        KswordArkCallbackLogFormat(
            "Info",
            "Process callback allow rule hit, creatorPid=%lu, targetPid=%lu, groupId=%lu, ruleId=%lu.",
            (unsigned long)HandleToULong(PsGetCurrentProcessId()),
            (unsigned long)HandleToULong(ProcessId),
            (unsigned long)matchResult.GroupId,
            (unsigned long)matchResult.RuleId);
    }
}

NTSTATUS
KswordArkProcessCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(runtime);

    status = PsSetCreateProcessNotifyRoutineEx(KswordArkProcessCreateNotifyEx, FALSE);
    if (NT_SUCCESS(status)) {
        KswordArkCallbackLogFrame("Info", "Process create callback registered.");
    }
    return status;
}

VOID
KswordArkProcessCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(runtime);

    status = PsSetCreateProcessNotifyRoutineEx(KswordArkProcessCreateNotifyEx, TRUE);
    if (NT_SUCCESS(status)) {
        KswordArkCallbackLogFrame("Info", "Process create callback unregistered.");
    }
}
