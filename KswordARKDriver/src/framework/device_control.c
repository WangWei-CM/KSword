/*++

Module Name:

    device_control.c

Abstract:

    This file contains control-device creation and compatibility PnP path.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "device_control.tmh"

#include <wdmsec.h>

#include "KswordArkLogProtocol.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, KswordARKDriverCreateDevice)
#pragma alloc_text (PAGE, KswordARKDriverCreateControlDevice)
#pragma alloc_text (PAGE, KswordARKDriverEvtDevicePrepareHardware)
#endif

// Security descriptor for control device:
// SYSTEM full access, Administrators read/write/execute, World read-only, Restricted read.
static const WCHAR g_KswordArkControlDeviceSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GR;;;WD)(A;;GR;;;RC)";

NTSTATUS
KswordARKDriverCreateControlDevice(
    _In_ WDFDRIVER Driver,
    _Out_opt_ WDFDEVICE* DeviceOut
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

    if (DeviceOut != NULL) {
        *DeviceOut = WDF_NO_HANDLE;
    }

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

    status = KswordARKCallbackInitialize(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKCallbackInitialize failed %!STATUS!", status);
        WdfObjectDelete(device);
        return status;
    }

    status = KswordARKDynDataInitialize(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "KswordARKDynDataInitialize recorded failure %!STATUS!", status);
    }

    status = KswordARKDriverEnqueueLogFrame(device, "Info", "KswordARK driver started.");
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDriverEnqueueLogFrame(startup) failed %!STATUS!", status);
    }

    WdfControlFinishInitializing(device);
    if (DeviceOut != NULL) {
        *DeviceOut = device;
    }
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

    status = KswordARKDynDataInitialize(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "KswordARKDynDataInitialize(PnP) recorded failure %!STATUS!", status);
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
