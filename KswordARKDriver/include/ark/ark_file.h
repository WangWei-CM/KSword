#pragma once

#include <ntddk.h>
#include "driver/KswordArkFileIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverDeletePath(
    _In_reads_(pathLengthChars) PCWSTR pathText,
    _In_ USHORT pathLengthChars,
    _In_ BOOLEAN isDirectory
    );

NTSTATUS
KswordARKDriverQueryFileInfo(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_FILE_INFO_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
