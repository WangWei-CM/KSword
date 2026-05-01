#pragma once

#include <ntddk.h>

#include "driver/KswordArkKernelIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverEnumerateSsdt(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_SSDT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryDriverObject(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_ const KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    );

EXTERN_C_END
