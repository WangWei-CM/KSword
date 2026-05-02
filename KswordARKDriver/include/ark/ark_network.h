#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkNetworkIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKNetworkInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ WDFDEVICE Device
    );

VOID
KswordARKNetworkUninitialize(
    VOID
    );

NTSTATUS
KswordARKNetworkSetRules(
    _In_ const KSWORD_ARK_NETWORK_SET_RULES_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKNetworkQueryStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

BOOLEAN
KswordARKNetworkShouldHidePort(
    _In_ ULONG Protocol,
    _In_ USHORT LocalPort,
    _In_ USHORT RemotePort,
    _In_ ULONG ProcessId
    );

EXTERN_C_END
