#pragma once

#include <ntddk.h>
#include "driver/KswordArkTrustIoctl.h"

EXTERN_C_START

VOID
KswordARKTrustInitialize(
    VOID
    );

NTSTATUS
KswordARKDriverQueryImageTrust(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_IMAGE_TRUST_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
