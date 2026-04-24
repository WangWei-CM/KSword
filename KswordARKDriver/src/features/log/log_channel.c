/*++

Module Name:

    log_channel.c

Abstract:

    This file contains log ring buffer helper functions.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "log_channel.tmh"

#include <ntstrsafe.h>

#include "KswordArkLogProtocol.h"

// Increment ring index and wrap around at queue capacity.
static ULONG
KswordARKDriverAdvanceRingIndex(
    _In_ ULONG currentIndex
    )
{
    return (currentIndex + 1U) % KSWORD_ARK_LOG_RING_CAPACITY;
}

NTSTATUS
KswordARKDriverInitializeLogChannel(
    _In_ WDFDEVICE Device
    )
{
    PDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES spinLockAttributes;

    if (Device == WDF_NO_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = DeviceGetContext(Device);
    if (deviceContext == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (deviceContext->LogQueueLock != WDF_NO_HANDLE) {
        return STATUS_SUCCESS;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&spinLockAttributes);
    spinLockAttributes.ParentObject = Device;
    status = WdfSpinLockCreate(&spinLockAttributes, &deviceContext->LogQueueLock);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfSpinLockCreate failed %!STATUS!", status);
        return status;
    }

    deviceContext->LogQueueHeadIndex = 0U;
    deviceContext->LogQueueTailIndex = 0U;
    deviceContext->LogQueueCount = 0U;
    RtlZeroMemory(deviceContext->LogEntryLength, sizeof(deviceContext->LogEntryLength));
    RtlZeroMemory(deviceContext->LogEntryText, sizeof(deviceContext->LogEntryText));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnqueueLogLine(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR FormattedLogLine
    )
{
    PDEVICE_CONTEXT deviceContext = NULL;
    size_t lineLengthBytes = 0;
    ULONG slotIndex = 0;

    if (Device == WDF_NO_HANDLE || FormattedLogLine == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = DeviceGetContext(Device);
    if (deviceContext == NULL || deviceContext->LogQueueLock == WDF_NO_HANDLE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!NT_SUCCESS(RtlStringCbLengthA(
        FormattedLogLine,
        KSWORD_ARK_LOG_ENTRY_MAX_BYTES,
        &lineLengthBytes))) {
        lineLengthBytes = KSWORD_ARK_LOG_ENTRY_MAX_BYTES - 1U;
    }

    WdfSpinLockAcquire(deviceContext->LogQueueLock);

    slotIndex = deviceContext->LogQueueTailIndex;
    RtlZeroMemory(deviceContext->LogEntryText[slotIndex], KSWORD_ARK_LOG_ENTRY_MAX_BYTES);
    if (lineLengthBytes > 0U) {
        RtlCopyMemory(deviceContext->LogEntryText[slotIndex], FormattedLogLine, lineLengthBytes);
    }
    deviceContext->LogEntryText[slotIndex][lineLengthBytes] = '\0';
    deviceContext->LogEntryLength[slotIndex] = (ULONG)lineLengthBytes;

    if (deviceContext->LogQueueCount == KSWORD_ARK_LOG_RING_CAPACITY) {
        deviceContext->LogQueueHeadIndex =
            KswordARKDriverAdvanceRingIndex(deviceContext->LogQueueHeadIndex);
    }
    else {
        deviceContext->LogQueueCount += 1U;
    }

    deviceContext->LogQueueTailIndex =
        KswordARKDriverAdvanceRingIndex(deviceContext->LogQueueTailIndex);

    WdfSpinLockRelease(deviceContext->LogQueueLock);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnqueueLogFrame(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR MessageText
    )
{
    CHAR frameBuffer[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    PCSTR safeLevelText = (LevelText != NULL) ? LevelText : "Info";
    PCSTR safeMessageText = (MessageText != NULL) ? MessageText : "";
    NTSTATUS status = STATUS_SUCCESS;

    status = RtlStringCbPrintfA(
        frameBuffer,
        sizeof(frameBuffer),
        "[%s]%s%s",
        safeLevelText,
        safeMessageText,
        KSWORD_ARK_LOG_END_MARKER);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "RtlStringCbPrintfA failed %!STATUS!", status);
        return status;
    }

    return KswordARKDriverEnqueueLogLine(Device, frameBuffer);
}

NTSTATUS
KswordARKDriverReadNextLogLine(
    _In_ WDFDEVICE Device,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
{
    PDEVICE_CONTEXT deviceContext = NULL;
    ULONG slotIndex = 0;
    ULONG lineLength = 0;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (Device == WDF_NO_HANDLE || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = DeviceGetContext(Device);
    if (deviceContext == NULL || deviceContext->LogQueueLock == WDF_NO_HANDLE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    WdfSpinLockAcquire(deviceContext->LogQueueLock);

    if (deviceContext->LogQueueCount == 0U) {
        WdfSpinLockRelease(deviceContext->LogQueueLock);
        return STATUS_NO_MORE_ENTRIES;
    }

    slotIndex = deviceContext->LogQueueHeadIndex;
    lineLength = deviceContext->LogEntryLength[slotIndex];
    if (OutputBufferLength < lineLength) {
        WdfSpinLockRelease(deviceContext->LogQueueLock);
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (lineLength > 0U) {
        RtlCopyMemory(OutputBuffer, deviceContext->LogEntryText[slotIndex], lineLength);
    }
    *BytesWrittenOut = lineLength;

    deviceContext->LogEntryLength[slotIndex] = 0U;
    deviceContext->LogEntryText[slotIndex][0] = '\0';
    deviceContext->LogQueueHeadIndex =
        KswordARKDriverAdvanceRingIndex(deviceContext->LogQueueHeadIndex);
    deviceContext->LogQueueCount -= 1U;

    WdfSpinLockRelease(deviceContext->LogQueueLock);
    return STATUS_SUCCESS;
}
