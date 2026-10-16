#pragma once

#include <ntddk.h>

#include "driver/KswordArkRxPfIoctl.h"

EXTERN_C_START

NTSTATUS
KswRxpfRuntimeInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    );

VOID
KswRxpfRuntimeUninitialize(
    VOID
    );

NTSTATUS
KswRxpfRuntimeQuerySupport(
    _Out_ KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeRegisterPage(
    _In_ const KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeChangePage(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeQueryPage(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeWritePage(
    _In_ const KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeSetEmulation(
    _In_ const KSWORD_ARK_RXPF_SET_EMULATION_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeQueryStats(
    _Out_ KSWORD_ARK_RXPF_STATS_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeDrainEvents(
    _In_ const KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeUnregisterPage(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeRunSelfTest(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_SELF_TEST_RESPONSE* Response
    );

NTSTATUS
KswRxpfRuntimeSetHookPoint(
    _In_ const KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE* Response
    );

ULONGLONG
KswRxpfInvokeTestPage(
    _In_ PVOID PageAddress
    );

EXTERN_C_END
