#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkFileMonitorIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKFileMonitorInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_opt_ WDFDEVICE Device
    );

VOID
KswordARKFileMonitorUninitialize(
    VOID
    );

NTSTATUS
KswordARKFileMonitorControl(
    _In_ const KSWORD_ARK_FILE_MONITOR_CONTROL_REQUEST* Request
    );

NTSTATUS
KswordARKFileMonitorQueryStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKFileMonitorDrain(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_FILE_MONITOR_DRAIN_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
