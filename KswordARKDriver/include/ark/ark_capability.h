#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkCapabilityIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKCapabilityQuery(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

VOID
KswordARKCapabilityInitialize(
    VOID
    );

VOID
KswordARKCapabilityRecordLastError(
    _In_ NTSTATUS Status,
    _In_z_ PCSTR SourceText,
    _In_z_ PCSTR SummaryText
    );

BOOLEAN
KswordARKCapabilityIsIoctlAllowed(
    _In_ ULONG64 RequiredCapability,
    _Out_opt_ NTSTATUS* DeniedStatusOut
    );

EXTERN_C_END
