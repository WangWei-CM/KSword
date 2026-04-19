/*++

Module Name:

    device.c - Device handling events for KswordARK driver.

Abstract:

    This file contains control-device creation and log queue helpers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "device.tmh"

#include <ntstrsafe.h>
#include <wdmsec.h>

#include "../shared/KswordArkLogProtocol.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, KswordARKDriverCreateDevice)
#pragma alloc_text (PAGE, KswordARKDriverCreateControlDevice)
#pragma alloc_text (PAGE, KswordARKDriverEvtDevicePrepareHardware)
#endif

// Security descriptor for control device:
// SYSTEM full access, Administrators read/write/execute, World read/write/execute, Restricted read.
static const WCHAR g_KswordArkControlDeviceSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GR;;;RC)";

// KswordARKDriverAdvanceRingIndex:
// - Increment ring index and wrap around at queue capacity.
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

NTSTATUS
KswordARKDriverCreateControlDevice(
    _In_ WDFDRIVER Driver
    )
/*++

Routine Description:

    Create a control device used by R3 to read driver log stream.

Arguments:

    Driver - WDF driver handle created in DriverEntry.

Return Value:

    NTSTATUS

--*/
{
    PWDFDEVICE_INIT deviceInit = NULL;
    WDFDEVICE device = WDF_NO_HANDLE;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    UNICODE_STRING sddlText;
    NTSTATUS status = STATUS_SUCCESS;

    DECLARE_CONST_UNICODE_STRING(deviceName, KSWORD_ARK_LOG_DEVICE_NT_NAME);
    DECLARE_CONST_UNICODE_STRING(symbolicName, KSWORD_ARK_LOG_DOS_NAME);

    PAGED_CODE();

    RtlInitUnicodeString(&sddlText, g_KswordArkControlDeviceSddl);
    deviceInit = WdfControlDeviceInitAllocate(Driver, &sddlText);
    if (deviceInit == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfControlDeviceInitAllocate failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceInitAssignName failed %!STATUS!", status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreate(control) failed %!STATUS!", status);
        if (deviceInit != NULL) {
            WdfDeviceInitFree(deviceInit);
        }
        return status;
    }

    status = KswordARKDriverInitializeLogChannel(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDriverInitializeLogChannel failed %!STATUS!", status);
        WdfObjectDelete(device);
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(device, &symbolicName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreateSymbolicLink failed %!STATUS!", status);
        WdfObjectDelete(device);
        return status;
    }

    status = KswordARKDriverQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDriverQueueInitialize(control) failed %!STATUS!", status);
        WdfObjectDelete(device);
        return status;
    }

    status = KswordARKDriverEnqueueLogFrame(device, "Info", "KswordARK driver started.");
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDriverEnqueueLogFrame(startup) failed %!STATUS!", status);
    }

    WdfControlFinishInitializing(device);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Control log device created successfully");
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
/*++

Routine Description:

    Keep a minimal PnP path for compatibility; primary path is control device.

Arguments:

    DeviceInit - Framework-allocated init structure.

Return Value:

    NTSTATUS

--*/
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device = WDF_NO_HANDLE;
    PDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = KswordARKDriverEvtDevicePrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreate(PnP) failed %!STATUS!", status);
        return status;
    }

    deviceContext = DeviceGetContext(device);
    deviceContext->UsbDevice = WDF_NO_HANDLE;
    deviceContext->PrivateDeviceData = 0U;

    status = KswordARKDriverInitializeLogChannel(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_KswordARKDriver,
        NULL);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreateDeviceInterface failed %!STATUS!", status);
        return status;
    }

    status = KswordARKDriverQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    (void)KswordARKDriverEnqueueLogFrame(device, "Info", "KswordARK PnP device initialized.");
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated
    )
/*++

Routine Description:

    Placeholder callback retained for compatibility.

Arguments:

    Device - Framework device handle.
    ResourceList - Raw resource list.
    ResourceListTranslated - Translated resource list.

Return Value:

    NTSTATUS

--*/
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");
    return STATUS_SUCCESS;
}
