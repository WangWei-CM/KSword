#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "driver/KswordArkCallbackIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKCallbackInitialize(
    _In_ WDFDEVICE Device
    );

VOID
KswordARKCallbackUninitialize(
    VOID
    );

NTSTATUS
KswordARKCallbackIoctlSetRules(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

NTSTATUS
KswordARKCallbackIoctlGetRuntimeState(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

NTSTATUS
KswordARKCallbackIoctlWaitEvent(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

NTSTATUS
KswordARKCallbackIoctlAnswerEvent(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

NTSTATUS
KswordARKCallbackIoctlCancelAllPending(
    _Out_ size_t* CompleteBytesOut
    );

EXTERN_C_END

