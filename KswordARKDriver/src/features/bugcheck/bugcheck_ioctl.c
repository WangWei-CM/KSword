/*++

Module Name:

    bugcheck_ioctl.c

Abstract:

    Silent optional BGRA32 bitmap upload for the VMware bugcheck panel.

--*/

#include "bugcheck_internal.h"

NTSTATUS
KswordARKBugcheckIoctlSetBitmap(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
{
    KSWORD_ARK_BUGCHECK_BITMAP_HEADER* header;
    ULONGLONG expectedStride;
    ULONGLONG expectedBytes;
    size_t requiredBytes;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_BUGCHECK_BITMAP_HEADER),
        (PVOID*)&header,
        NULL);
    if (!NT_SUCCESS(status) || InputBufferLength < sizeof(*header)) {
        return NT_SUCCESS(status) ? STATUS_BUFFER_TOO_SMALL : status;
    }

    expectedStride = (ULONGLONG)header->width * 4ULL;
    expectedBytes = expectedStride * (ULONGLONG)header->height;
    if (header->version != KSWORD_ARK_BUGCHECK_BITMAP_PROTOCOL_VERSION ||
        header->size != sizeof(*header) ||
        header->magic != KSWORD_ARK_BUGCHECK_BITMAP_MAGIC ||
        header->format != KSWORD_ARK_BUGCHECK_BITMAP_FORMAT_BGRA32 ||
        header->flags != 0 || header->reserved0 != 0 || header->reserved1 != 0 ||
        header->width == 0 || header->height == 0 ||
        header->width > KSWORD_ARK_BUGCHECK_BITMAP_MAX_WIDTH ||
        header->height > KSWORD_ARK_BUGCHECK_BITMAP_MAX_HEIGHT ||
        expectedStride != header->stride ||
        expectedBytes == 0 ||
        expectedBytes > KSWORD_ARK_BUGCHECK_BITMAP_MAX_BYTES ||
        expectedBytes != header->dataLength) {
        return STATUS_INVALID_PARAMETER;
    }

    requiredBytes = sizeof(*header) + (size_t)header->dataLength;
    if (requiredBytes < sizeof(*header) || InputBufferLength < requiredBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // A valid packet is an intentional no-op outside a fully initialized
    // VMware SVGA environment. This keeps branding optional and silent.
    if (InterlockedCompareExchange(&g_KswordArkBugcheckState.Active, 1, 1) == 0) {
        return STATUS_SUCCESS;
    }
    if (InterlockedCompareExchange(
            &g_KswordArkBugcheckState.Bitmap.Uploading,
            1,
            0) != 0) {
        return STATUS_DEVICE_BUSY;
    }

    // Make the crash path fall back to the built-in text while the single
    // backing buffer is changing. Metadata becomes visible before Valid=1.
    InterlockedExchange(&g_KswordArkBugcheckState.Bitmap.Valid, 0);
    KeMemoryBarrier();
    RtlCopyMemory(
        g_KswordArkBugcheckBitmapPixels,
        ((PUCHAR)header) + sizeof(*header),
        header->dataLength);
    g_KswordArkBugcheckState.Bitmap.Width = header->width;
    g_KswordArkBugcheckState.Bitmap.Height = header->height;
    g_KswordArkBugcheckState.Bitmap.Stride = header->stride;
    g_KswordArkBugcheckState.Bitmap.DataLength = header->dataLength;
    g_KswordArkBugcheckState.Bitmap.BrandColorRgb =
        header->brandColorRgb & 0x00FFFFFFUL;
    if (g_KswordArkBugcheckState.Bitmap.BrandColorRgb == 0) {
        g_KswordArkBugcheckState.Bitmap.BrandColorRgb = 0x0078D4UL;
    }
    KeMemoryBarrier();
    InterlockedExchange(&g_KswordArkBugcheckState.Bitmap.Valid, 1);
    InterlockedExchange(&g_KswordArkBugcheckState.Bitmap.Uploading, 0);
    return STATUS_SUCCESS;
}
