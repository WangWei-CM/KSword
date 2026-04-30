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

EXTERN_C_END
