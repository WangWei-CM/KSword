/*++

Module Name:

    memory_physical_layout.c

Abstract:

    Read-only physical memory layout snapshot for HardwareDock. The query
    exposes aggregate geometry only: count, span, gap estimate and extrema.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

typedef struct _KSW_PHYSICAL_MEMORY_RANGE
{
    PHYSICAL_ADDRESS BaseAddress;
    PHYSICAL_ADDRESS NumberOfBytes;
} KSW_PHYSICAL_MEMORY_RANGE, *PKSW_PHYSICAL_MEMORY_RANGE;

NTSTATUS
KswordARKDriverQueryPhysicalMemoryLayout(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Build a read-only summary of the system physical memory map using
    MmGetPhysicalMemoryRanges.

Arguments:

    OutputBuffer - Caller-supplied response buffer.
    OutputBufferLength - Output buffer size in bytes.
    BytesWrittenOut - Receives sizeof(KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT_RESPONSE).

Return Value:

    STATUS_SUCCESS on success; STATUS_BUFFER_TOO_SMALL or STATUS_INVALID_PARAMETER
    for malformed caller buffers.

--*/
{
    KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT_RESPONSE* response = NULL;
    PKSW_PHYSICAL_MEMORY_RANGE ranges = NULL;
    ULONGLONG totalBytes = 0ULL;
    ULONGLONG highestAddress = 0ULL;
    ULONGLONG largestRange = 0ULL;
    ULONGLONG smallestRange = 0ULL;
    ULONGLONG firstBase = 0ULL;
    ULONGLONG lastEnd = 0ULL;
    ULONGLONG estimatedGap = 0ULL;
    ULONG rangeCount = 0UL;
    ULONG zeroLengthCount = 0UL;
    BOOLEAN haveRange = FALSE;

    if (BytesWrittenOut == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    response = (KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT_RESPONSE*)OutputBuffer;
    RtlZeroMemory(response, sizeof(*response));
    response->version = KSWORD_ARK_PHYSICAL_MEMORY_LAYOUT_PROTOCOL_VERSION;
    response->lastStatus = STATUS_SUCCESS;

    ranges = (PKSW_PHYSICAL_MEMORY_RANGE)MmGetPhysicalMemoryRanges();
    if (ranges == NULL) {
        response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (PKSW_PHYSICAL_MEMORY_RANGE range = ranges; range->NumberOfBytes.QuadPart != 0; ++range) {
        const ULONGLONG base = (ULONGLONG)range->BaseAddress.QuadPart;
        const ULONGLONG bytes = (ULONGLONG)range->NumberOfBytes.QuadPart;
        const ULONGLONG end = bytes > 0ULL ? (base + bytes - 1ULL) : base;

        ++rangeCount;
        if (bytes == 0ULL) {
            ++zeroLengthCount;
            continue;
        }

        if (!haveRange) {
            firstBase = base;
            smallestRange = bytes;
            haveRange = TRUE;
        }
        else {
            if (base > lastEnd + 1ULL) {
                estimatedGap += base - (lastEnd + 1ULL);
            }
            if (bytes < smallestRange) {
                smallestRange = bytes;
            }
        }

        totalBytes += bytes;
        if (bytes > largestRange) {
            largestRange = bytes;
        }
        if (end > highestAddress) {
            highestAddress = end;
        }
        lastEnd = end;
    }

    ExFreePool(ranges);

    response->fieldFlags = haveRange ? KSWORD_ARK_PHYSICAL_MEMORY_LAYOUT_FIELD_RANGES_PRESENT : 0UL;
    response->rangeCount = rangeCount;
    response->zeroLengthRangeCount = zeroLengthCount;
    response->truncated = 0UL;
    response->totalPhysicalBytes = totalBytes;
    response->highestPhysicalAddress = highestAddress;
    response->largestRangeBytes = largestRange;
    response->smallestRangeBytes = smallestRange;
    response->firstBaseAddress = firstBase;
    response->lastEndAddress = lastEnd;
    response->estimatedAddressSpaceGapBytes = estimatedGap;

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
