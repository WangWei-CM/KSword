#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "ark/ark_dyndata.h"
#include "driver/KswordArkProcessIoctl.h"

EXTERN_C_START

/*
 * KSW_CROSSVIEW_CID_ENTRY
 * Inputs:
 * - Filled by the read-only CID-table walker for one decoded process/thread
 *   candidate.
 * Processing:
 * - The walker validates type and references live objects when possible before
 *   invoking the callback. Dangling rows are reported without trusting R3 input.
 * Return behavior:
 * - Plain callback payload; ownership of referenced Object remains with the
 *   walker and is released immediately after the callback returns.
 */
typedef struct _KSW_CROSSVIEW_CID_ENTRY
{
    PVOID Object;
    ULONG64 ObjectAddress;
    ULONG CidValue;
    BOOLEAN Referenced;
    BOOLEAN TypeMatched;
    NTSTATUS ReferenceStatus;
} KSW_CROSSVIEW_CID_ENTRY, *PKSW_CROSSVIEW_CID_ENTRY;

/*
 * KSW_CROSSVIEW_CID_CALLBACK
 * Inputs:
 * - Entry describes one type-matched CID table object or a dangling candidate.
 * - Context is the caller-owned builder state.
 * Processing:
 * - The callback merges the evidence row into its process or thread response.
 * Return behavior:
 * - No return value; enumeration continues until the bounded walker stops.
 */
typedef VOID (*KSW_CROSSVIEW_CID_CALLBACK)(
    _In_ const KSW_CROSSVIEW_CID_ENTRY* Entry,
    _Inout_opt_ PVOID Context
    );

BOOLEAN
KswordARKCrossViewOffsetPresent(
    _In_ ULONG Offset
    );

ULONG
KswordARKCrossViewNormalizeOffset(
    _In_ ULONG Offset
    );

BOOLEAN
KswordARKCrossViewPointerAligned(
    _In_ ULONG_PTR Address
    );

VOID
KswordARKCrossViewFillFieldOffsets(
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS* Offsets
    );

NTSTATUS
KswordARKCrossViewReadMemory(
    _In_ const VOID* Address,
    _Out_writes_bytes_(BytesToRead) VOID* Buffer,
    _In_ SIZE_T BytesToRead
    );

NTSTATUS
KswordARKCrossViewReadPointerAddress(
    _In_ const VOID* Address,
    _Out_ PVOID* PointerOut
    );

NTSTATUS
KswordARKCrossViewReadUlong64Address(
    _In_ const VOID* Address,
    _Out_ ULONGLONG* ValueOut
    );

NTSTATUS
KswordARKCrossViewReadPointerField(
    _In_ const VOID* Object,
    _In_ ULONG Offset,
    _Out_ PVOID* PointerOut
    );

NTSTATUS
KswordARKCrossViewResolvePspCidTableAddress(
    _In_ const KSW_DYN_STATE* DynState,
    _Inout_ KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS* Offsets,
    _Out_ PVOID* PspCidTableAddressOut,
    _Out_ ULONG64* MissingCapabilityMaskOut,
    _Out_ BOOLEAN* UsedDynDataGlobalOut
    );

NTSTATUS
KswordARKCrossViewWalkCidTable(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ PVOID PspCidTableAddress,
    _In_ POBJECT_TYPE ExpectedObjectType,
    _In_ ULONG MaxNodes,
    _In_ KSW_CROSSVIEW_CID_CALLBACK Callback,
    _Inout_opt_ PVOID Context,
    _Out_opt_ ULONG* VisitedEntriesOut
    );

NTSTATUS
KswordARKDriverQueryProcessCrossView(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_PROCESS_CROSSVIEW_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKProcessIoctlQueryCrossView(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

EXTERN_C_END
