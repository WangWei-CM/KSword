/*++

Module Name:

    image_callback.c

Abstract:

    Image load callback registration and log-only dispatch.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_internal.h"

static VOID
KswordArkLoadImageNotify(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ HANDLE ProcessId,
    _In_ PIMAGE_INFO ImageInfo
    )
{
    KSWORD_ARK_CALLBACK_MATCH_RESULT matchResult;
    UNICODE_STRING initiatorPath = { 0 };
    UNICODE_STRING targetPath = { 0 };
    WCHAR initiatorPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS] = { 0 };
    WCHAR targetPathBuffer[KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS] = { 0 };
    NTSTATUS matchStatus = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(ImageInfo);

    (VOID)KswordArkResolveProcessImagePath(
        PsGetCurrentProcess(),
        initiatorPathBuffer,
        RTL_NUMBER_OF(initiatorPathBuffer),
        NULL);
    RtlInitUnicodeString(&initiatorPath, initiatorPathBuffer);

    if (FullImageName != NULL && FullImageName->Buffer != NULL && FullImageName->Length > 0U) {
        targetPath = *FullImageName;
    }
    else {
        (VOID)RtlStringCbPrintfW(
            targetPathBuffer,
            sizeof(targetPathBuffer),
            L"ProcessId=%lu,ImagePathUnavailable=1",
            (unsigned long)HandleToULong(ProcessId));
        RtlInitUnicodeString(&targetPath, targetPathBuffer);
    }

    matchStatus = KswordArkCallbackMatchRule(
        KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD,
        KSWORD_ARK_IMAGE_OP_LOAD,
        &initiatorPath,
        &targetPath,
        &matchResult);
    if (!NT_SUCCESS(matchStatus) || !matchResult.Matched) {
        return;
    }

    KswordArkCallbackLogFormat(
        "Info",
        "Image callback log rule hit, processId=%lu, groupId=%lu, ruleId=%lu.",
        (unsigned long)HandleToULong(ProcessId),
        (unsigned long)matchResult.GroupId,
        (unsigned long)matchResult.RuleId);
}

NTSTATUS
KswordArkImageCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(runtime);

    status = PsSetLoadImageNotifyRoutine(KswordArkLoadImageNotify);
    if (NT_SUCCESS(status)) {
        KswordArkCallbackLogFrame("Info", "Image load callback registered.");
    }
    return status;
}

VOID
KswordArkImageCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(runtime);

    status = PsRemoveLoadImageNotifyRoutine(KswordArkLoadImageNotify);
    if (NT_SUCCESS(status)) {
        KswordArkCallbackLogFrame("Info", "Image load callback unregistered.");
    }
}
