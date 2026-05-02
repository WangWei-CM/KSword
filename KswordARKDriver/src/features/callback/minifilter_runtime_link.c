/*++

Module Name:

    minifilter_runtime_link.c

Abstract:

    Links the callback-rule runtime to the shared file-monitor minifilter.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_internal.h"

VOID
KswordArkMinifilterCallbackUpdateState(
    _In_opt_ PFLT_FILTER FilterHandle,
    _In_ NTSTATUS RegisterStatus,
    _In_ NTSTATUS StartStatus,
    _In_ BOOLEAN Started
    )
{
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();

    if (runtime == NULL) {
        return;
    }

    if (runtime->MiniFilterHandle == NULL && FilterHandle != NULL) {
        KswordArkCallbackLogFrame("Info", "Minifilter custom callback linked to file-monitor runtime.");
    }

    runtime->MiniFilterHandle = FilterHandle;
    runtime->MiniFilterRegisterStatus = RegisterStatus;
    runtime->MiniFilterStartStatus = StartStatus;
    runtime->MiniFilterStarted = Started;

    if (FilterHandle != NULL && NT_SUCCESS(RegisterStatus)) {
        runtime->RegisteredCallbacksMask |= KSWORD_ARK_CALLBACK_REGISTERED_MINIFILTER;
    }
    else {
        runtime->RegisteredCallbacksMask &= ~KSWORD_ARK_CALLBACK_REGISTERED_MINIFILTER;
    }
}

VOID
KswordArkMinifilterCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    )
{
    if (runtime == NULL) {
        return;
    }

    runtime->MiniFilterHandle = NULL;
    runtime->MiniFilterStarted = FALSE;
    runtime->MiniFilterRegisterStatus = STATUS_NOT_SUPPORTED;
    runtime->MiniFilterStartStatus = STATUS_NOT_SUPPORTED;
    runtime->RegisteredCallbacksMask &= ~KSWORD_ARK_CALLBACK_REGISTERED_MINIFILTER;
    KswordArkCallbackLogFrame("Info", "Minifilter custom callback unlinked from callback runtime.");
}
