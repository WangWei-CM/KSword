#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkKeyboardIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverEnumerateKeyboardHotkeys(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_KEYBOARD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverEnumerateKeyboardHooks(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_KEYBOARD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
