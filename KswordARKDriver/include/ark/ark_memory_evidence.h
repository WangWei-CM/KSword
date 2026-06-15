#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkMemoryIoctl.h"

EXTERN_C_START

/*
 * Kernel memory evidence backend. Inputs are the shared R3/R0 request, a
 * METHOD_BUFFERED output buffer, and a byte count output. Processing is
 * read-only: it queries module metadata, SystemBigPoolInformation, page-table
 * state, and memory bytes for hashing/sampling; it never writes PTEs, changes
 * CR0.WP, patches kernel memory, repairs, or quarantines.
 */
NTSTATUS
KswordARKDriverScanKernelMemoryEvidence(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
