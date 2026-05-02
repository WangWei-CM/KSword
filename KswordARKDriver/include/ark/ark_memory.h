#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkMemoryIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverQueryVirtualMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_VIRTUAL_MEMORY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverReadVirtualMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverWriteVirtualMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST* Request,
    _In_ size_t RequestBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverReadPhysicalMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverWritePhysicalMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST* Request,
    _In_ size_t RequestBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverTranslateVirtualAddress(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryPageTableEntry(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
