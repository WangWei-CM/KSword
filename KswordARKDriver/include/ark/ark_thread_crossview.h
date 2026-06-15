#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkThreadIoctl.h"

EXTERN_C_START

/*
 * KswordARKDriverQueryThreadCrossView
 * Inputs:
 * - OutputBuffer/OutputBufferLength describe the METHOD_BUFFERED response
 *   storage supplied by the caller.
 * - Request optionally selects read-only evidence sources, owner PID, TID range,
 *   and traversal node budget.
 * Processing:
 * - Collects thread evidence from PsGetNextProcessThread, per-process
 *   ThreadListHead walks, and PspCidTable without accepting R3 object pointers.
 * Return behavior:
 * - Returns request-level NTSTATUS. Source failures and anomalies are encoded in
 *   the response header/rows.
 */
NTSTATUS
KswordARKDriverQueryThreadCrossView(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_THREAD_CROSSVIEW_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

/*
 * KswordARKThreadIoctlQueryCrossView
 * Inputs:
 * - Device and Request are the WDF dispatch objects for the unregistered thread
 *   cross-view IOCTL.
 * - InputBufferLength/OutputBufferLength are validated with common WDF helpers.
 * Processing:
 * - Retrieves the optional fixed request and required output buffer, then calls
 *   the read-only thread backend.
 * Return behavior:
 * - Returns the WDF buffer retrieval status or backend status and writes the
 *   response byte count through BytesReturned.
 */
NTSTATUS
KswordARKThreadIoctlQueryCrossView(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

EXTERN_C_END
