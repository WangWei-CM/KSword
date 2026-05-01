#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkSectionIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverQueryProcessSection(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryFileSectionMappings(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
