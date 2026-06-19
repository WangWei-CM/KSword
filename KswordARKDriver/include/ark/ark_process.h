#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkProcessIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverTerminateProcessByPid(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    );

NTSTATUS
KswordARKDriverSuspendProcessByPid(
    _In_ ULONG processId
    );

NTSTATUS
KswordARKDriverSetProcessPplLevelByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    );

NTSTATUS
KswordARKDriverEnumerateProcesses(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_PROCESS_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    );

NTSTATUS
KswordARKDriverSetProcessVisibility(
    _In_ ULONG ProcessId,
    _In_ ULONG Action,
    _In_ ULONG Flags,
    _Out_ ULONG* StatusOut,
    _Out_ ULONG* HiddenCountOut
    );

NTSTATUS
KswordARKDriverSetProcessSpecialFlags(
    _In_ ULONG ProcessId,
    _In_ ULONG Action,
    _In_ ULONG Flags,
    _Out_ ULONG* OperationStatusOut,
    _Out_ ULONG* AppliedFlagsOut,
    _Out_ ULONG* TouchedThreadCountOut
    );

NTSTATUS
KswordARKDriverDkomProcess(
    _In_ ULONG ProcessId,
    _In_ ULONG Action,
    _In_ ULONG Flags,
    _Out_ ULONG* OperationStatusOut,
    _Out_ ULONG* RemovedEntriesOut,
    _Out_ ULONG64* PspCidTableAddressOut,
    _Out_ ULONG64* ProcessObjectAddressOut
    );

/*
 * KswordARKDriverQueryProcessCrossView
 * Inputs:
 * - OutputBuffer/OutputBufferLength describe the caller-owned METHOD_BUFFERED
 *   response storage.
 * - Request optionally selects read-only evidence sources and PID bounds.
 * Processing:
 * - Collects process evidence from public enumeration, ActiveProcessLinks, and
 *   PspCidTable without modifying process objects or kernel tables.
 * Return behavior:
 * - Returns an NTSTATUS for request-level validation/allocation. Per-source
 *   read/capability results are reported in the response header and rows.
 */
NTSTATUS
KswordARKDriverQueryProcessCrossView(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_PROCESS_CROSSVIEW_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

/*
 * KswordARKProcessIoctlQueryCrossView
 * Inputs:
 * - Device and Request are the WDF dispatch objects for the unregistered
 *   process cross-view IOCTL.
 * - InputBufferLength/OutputBufferLength are validated through WDF helpers.
 * Processing:
 * - Retrieves an optional fixed request plus required output buffer, then calls
 *   the read-only backend. It never accepts R3-provided EPROCESS addresses.
 * Return behavior:
 * - Returns the WDF buffer retrieval status or backend status and writes the
 *   byte count through BytesReturned.
 */
NTSTATUS
KswordARKProcessIoctlQueryCrossView(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

EXTERN_C_END
