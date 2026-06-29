#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkThreadIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverEnumerateThreads(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_THREAD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

/*
 * KswordARKDriverQueryThreadDetail
 * Inputs:
 * - Response/OutputBufferLength describe a fixed METHOD_BUFFERED response.
 * - Request supplies a TID and optional PID consistency check.
 * Processing:
 * - References ETHREAD by TID and samples PDB/DynData-backed fields with guarded
 *   reads.
 * Return behavior:
 * - Returns STATUS_SUCCESS when response.status contains the query outcome.
 */
NTSTATUS
KswordARKDriverQueryThreadDetail(
    _Out_writes_bytes_(OutputBufferLength) KSWORD_ARK_THREAD_DETAIL_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_THREAD_DETAIL_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

/*
 * KswordARKThreadIoctlQueryDetail
 * Inputs:
 * - WDF request buffers for IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL.
 * Processing:
 * - Retrieves fixed request/response buffers and forwards to the feature backend.
 * Return behavior:
 * - Returns WDF retrieval status or backend status; BytesReturned is fixed
 *   response size on success.
 */
NTSTATUS
KswordARKThreadIoctlQueryDetail(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

/*
 * KswordARKDriverQueryThreadRuntimeFields
 * Inputs:
 * - OutputBuffer/OutputBufferLength describe a variable METHOD_BUFFERED response.
 * - Request/InputBufferLength describe TID/PID plus PDB runtime field sample items.
 * Processing:
 * - References ETHREAD by TID and reads only bounded small fields from that object.
 * Return behavior:
 * - Returns STATUS_SUCCESS when response metadata is valid; row statuses carry
 *   individual sampling results.
 */
NTSTATUS
KswordARKDriverQueryThreadRuntimeFields(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _In_reads_bytes_(InputBufferLength) const KSWORD_ARK_THREAD_RUNTIME_FIELD_SAMPLE_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

/*
 * KswordARKThreadIoctlQueryRuntimeFields
 * Inputs:
 * - WDF request buffers for IOCTL_KSWORD_ARK_QUERY_THREAD_RUNTIME_FIELDS.
 * Processing:
 * - Retrieves variable buffers and forwards to the thread runtime sampler backend.
 * Return behavior:
 * - Returns validation/backend NTSTATUS and writes the actual response byte count.
 */
NTSTATUS
KswordARKThreadIoctlQueryRuntimeFields(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

EXTERN_C_END
