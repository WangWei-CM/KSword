#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkHandleIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverEnumerateProcessHandles(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryHandleObject(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
