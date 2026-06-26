#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "driver/KswordArkKernelObjectIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKKernelObjectIoctlEnumCidTable(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

NTSTATUS
KswordARKKernelObjectIoctlQueryObjectSummary(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

NTSTATUS
KswordARKKernelObjectIoctlQueryIpcSummary(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

NTSTATUS
KswordARKDriverEnumerateCidTable(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_CID_TABLE_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryKernelObjectSummary(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryIpcSummary(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_IPC_SUMMARY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
