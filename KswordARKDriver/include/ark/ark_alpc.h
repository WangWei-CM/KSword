#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkAlpcIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverQueryAlpcPort(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_ALPC_PORT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
