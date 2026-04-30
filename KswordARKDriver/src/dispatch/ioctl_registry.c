/*++

Module Name:

    ioctl_registry.c

Abstract:

    Static IOCTL registry for the KswordARK dispatch path.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ioctl_registry.h"
#include "ark/ark_ioctl.h"

// Feature handler declarations live here instead of in the central dispatch file.
NTSTATUS KswordARKProcessIoctlTerminate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlSuspend(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlSetPplLevel(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlEnumProcess(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKFileIoctlDeletePath(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlEnumSsdt(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlSetRulesHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlGetRuntimeStateHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlWaitEventHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlAnswerEventHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlCancelAllPendingHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlRemoveExternalCallbackHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);

static const KSWORD_ARK_IOCTL_ENTRY g_KswordArkIoctlTable[] = {
    { IOCTL_KSWORD_ARK_TERMINATE_PROCESS, KswordARKProcessIoctlTerminate, "IOCTL_KSWORD_ARK_TERMINATE_PROCESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SUSPEND_PROCESS, KswordARKProcessIoctlSuspend, "IOCTL_KSWORD_ARK_SUSPEND_PROCESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SET_PPL_LEVEL, KswordARKProcessIoctlSetPplLevel, "IOCTL_KSWORD_ARK_SET_PPL_LEVEL", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_PROCESS, KswordARKProcessIoctlEnumProcess, "IOCTL_KSWORD_ARK_ENUM_PROCESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_DELETE_PATH, KswordARKFileIoctlDeletePath, "IOCTL_KSWORD_ARK_DELETE_PATH", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_SSDT, KswordARKKernelIoctlEnumSsdt, "IOCTL_KSWORD_ARK_ENUM_SSDT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SET_CALLBACK_RULES, KswordARKCallbackIoctlSetRulesHandler, "IOCTL_KSWORD_ARK_SET_CALLBACK_RULES", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE, KswordARKCallbackIoctlGetRuntimeStateHandler, "IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT, KswordARKCallbackIoctlWaitEventHandler, "IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT, KswordARKCallbackIoctlAnswerEventHandler, "IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS, KswordARKCallbackIoctlCancelAllPendingHandler, "IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK, KswordARKCallbackIoctlRemoveExternalCallbackHandler, "IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE }
};

_Must_inspect_result_
const KSWORD_ARK_IOCTL_ENTRY*
KswordARKLookupIoctlEntry(
    _In_ ULONG IoControlCode
    )
/*++

Routine Description:

    Locate the registered IOCTL entry for the supplied control code. The table is
    exact-match only so unsupported IOCTLs fail closed.

Arguments:

    IoControlCode - Control code received by the queue dispatch callback.

Return Value:

    Pointer to a registry entry when found; NULL when unsupported.

--*/
{
    ULONG entryIndex = 0;

    for (entryIndex = 0; entryIndex < (sizeof(g_KswordArkIoctlTable) / sizeof(g_KswordArkIoctlTable[0])); ++entryIndex) {
        if (g_KswordArkIoctlTable[entryIndex].IoControlCode == IoControlCode) {
            return &g_KswordArkIoctlTable[entryIndex];
        }
    }

    return NULL;
}
