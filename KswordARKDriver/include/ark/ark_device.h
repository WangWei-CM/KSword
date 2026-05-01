#pragma once

#include <wdf.h>
#include <wdfusb.h>

#include "ark_log.h"
#include "Public.h"

EXTERN_C_START

// The device context performs the same job as a WDM device extension.
typedef struct _DEVICE_CONTEXT {

    WDFUSBDEVICE UsbDevice;
    ULONG PrivateDeviceData;
    WDFSPINLOCK LogQueueLock;
    ULONG LogQueueHeadIndex;
    ULONG LogQueueTailIndex;
    ULONG LogQueueCount;
    ULONG LogEntryLength[KSWORD_ARK_LOG_RING_CAPACITY];
    CHAR LogEntryText[KSWORD_ARK_LOG_RING_CAPACITY][KSWORD_ARK_LOG_ENTRY_MAX_BYTES];

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

// This macro will generate an inline function called DeviceGetContext
// which will be used to get a pointer to the device context memory
// in a type safe manner.
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

NTSTATUS
KswordARKDriverCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

NTSTATUS
KswordARKDriverCreateControlDevice(
    _In_ WDFDRIVER Driver,
    _Out_opt_ WDFDEVICE* DeviceOut
    );

// Function to select the device's USB configuration and get a WDFUSBDEVICE
// handle.
EVT_WDF_DEVICE_PREPARE_HARDWARE KswordARKDriverEvtDevicePrepareHardware;

EXTERN_C_END
