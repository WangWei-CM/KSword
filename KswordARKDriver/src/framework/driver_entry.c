/*++

Module Name:

    driver_entry.c

Abstract:

    This file contains the driver entry points and callbacks.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "driver_entry.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, KswordARKDriverEvtDriverUnload)
#pragma alloc_text (PAGE, KswordARKDriverEvtDriverContextCleanup)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    DriverEntry initializes the driver and is the first routine called by the
    system after the driver is loaded.

Arguments:

    DriverObject - represents the instance of the function driver that is loaded
    into memory.
    RegistryPath - represents the driver specific path in the Registry.

Return Value:

    NTSTATUS

--*/
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDRIVER driverHandle = WDF_NO_HANDLE;

    // Initialize WPP tracing as soon as possible.
    WPP_INIT_TRACING(DriverObject, RegistryPath);
    KswordARKCapabilityInitialize();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // Register cleanup callback for WPP_CLEANUP during framework teardown.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = KswordARKDriverEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = KswordARKDriverEvtDriverUnload;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        &driverHandle);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    status = KswordARKDriverCreateControlDevice(driverHandle);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "KswordARKDriverCreateControlDevice failed %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return STATUS_SUCCESS;
}

VOID
KswordARKDriverEvtDriverUnload(
    _In_ WDFDRIVER Driver
    )
/*++

Routine Description:

    Called when SCM requests to unload the non-PnP control driver.

Arguments:

    Driver - Handle to a WDF Driver object.

Return Value:

    VOID

--*/
{
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
    KswordARKCallbackUninitialize();
    KswordARKDynDataUninitialize();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
}

VOID
KswordARKDriverEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
    )
/*++

Routine Description:

    Free all the resources allocated in DriverEntry.

Arguments:

    DriverObject - handle to a WDF Driver object.

Return Value:

    VOID.

--*/
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // Stop WPP tracing.
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
}
