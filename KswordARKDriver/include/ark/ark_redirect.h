#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkRedirectIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKRedirectInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ WDFDEVICE Device
    );

VOID
KswordARKRedirectUninitialize(
    VOID
    );

NTSTATUS
KswordARKRedirectSetRules(
    _In_ const KSWORD_ARK_REDIRECT_SET_RULES_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKRedirectQueryStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
