/*++

Module Name:

    thread_callback.c

Abstract:

    Thread create callback registration and log-only dispatch.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_internal.h"

VOID
KswordArkThreadCreateNotify(
    _In_ HANDLE ProcessId,
    _In_ HANDLE ThreadId,
    _In_ BOOLEAN Create
    )
{
    KSWORD_ARK_CALLBACK_MATCH_RESULT matchResult;
    UNICODE_STRING initiatorPath = { 0 };
    UNICODE_STRING targetPath = { 0 };
    WCHAR initiatorPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS] = { 0 };
    WCHAR targetPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS] = { 0 };
    PEPROCESS targetProcess = NULL;
    ULONG operationType = Create ? KSWORD_ARK_THREAD_OP_CREATE : KSWORD_ARK_THREAD_OP_EXIT;
    NTSTATUS matchStatus = STATUS_SUCCESS;

    (VOID)KswordArkResolveProcessImagePath(
        PsGetCurrentProcess(),
        initiatorPathBuffer,
        RTL_NUMBER_OF(initiatorPathBuffer),
        NULL);
    RtlInitUnicodeString(&initiatorPath, initiatorPathBuffer);

    if (NT_SUCCESS(PsLookupProcessByProcessId(ProcessId, &targetProcess))) {
        (VOID)KswordArkResolveProcessImagePath(
            targetProcess,
            targetPathBuffer,
            RTL_NUMBER_OF(targetPathBuffer),
            NULL);
        ObDereferenceObject(targetProcess);
    }

    if (targetPathBuffer[0] == L'\0') {
        (VOID)RtlStringCbPrintfW(
            targetPathBuffer,
            sizeof(targetPathBuffer),
            L"PID=%lu,TID=%lu",
            (unsigned long)HandleToULong(ProcessId),
            (unsigned long)HandleToULong(ThreadId));
    }
    RtlInitUnicodeString(&targetPath, targetPathBuffer);

    matchStatus = KswordArkCallbackMatchRule(
        KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE,
        operationType,
        &initiatorPath,
        &targetPath,
        &matchResult);
    if (!NT_SUCCESS(matchStatus) || !matchResult.Matched) {
        return;
    }

    KswordArkCallbackLogFormat(
        "Info",
        "Thread callback log rule hit, processId=%lu, threadId=%lu, create=%lu, groupId=%lu, ruleId=%lu.",
        (unsigned long)HandleToULong(ProcessId),
        (unsigned long)HandleToULong(ThreadId),
        (unsigned long)(Create ? 1UL : 0UL),
        (unsigned long)matchResult.GroupId,
        (unsigned long)matchResult.RuleId);
}

NTSTATUS
KswordArkThreadCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(runtime);

    status = PsSetCreateThreadNotifyRoutine(KswordArkThreadCreateNotify);
    if (NT_SUCCESS(status)) {
        KswordArkCallbackLogFrame("Info", "Thread create callback registered.");
    }
    return status;
}

VOID
KswordArkThreadCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(runtime);

    status = PsRemoveCreateThreadNotifyRoutine(KswordArkThreadCreateNotify);
    if (NT_SUCCESS(status)) {
        KswordArkCallbackLogFrame("Info", "Thread create callback unregistered.");
    }
}
