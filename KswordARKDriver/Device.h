/*++

Module Name:

    device.h

Abstract:

    This file contains the device definitions.

Environment:

    Kernel-mode Driver Framework

--*/

#include "public.h"

// Maximum bytes per single log frame (including trailing NUL room).
#define KSWORD_ARK_LOG_ENTRY_MAX_BYTES 512

// Ring queue capacity in log-frame units.
#define KSWORD_ARK_LOG_RING_CAPACITY 64

EXTERN_C_START

//
// The device context performs the same job as a WDM device extension.
//
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

//
// This macro will generate an inline function called DeviceGetContext
// which will be used to get a pointer to the device context memory
// in a type safe manner.
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

//
// Function to initialize the device's queues and callbacks.
//
NTSTATUS
KswordARKDriverCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

NTSTATUS
KswordARKDriverCreateControlDevice(
    _In_ WDFDRIVER Driver
    );

NTSTATUS
KswordARKDriverInitializeLogChannel(
    _In_ WDFDEVICE Device
    );

NTSTATUS
KswordARKDriverEnqueueLogLine(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR FormattedLogLine
    );

NTSTATUS
KswordARKDriverEnqueueLogFrame(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR MessageText
    );

NTSTATUS
KswordARKDriverReadNextLogLine(
    _In_ WDFDEVICE Device,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

//
// Function to select the device's USB configuration and get a WDFUSBDEVICE
// handle.
//
EVT_WDF_DEVICE_PREPARE_HARDWARE KswordARKDriverEvtDevicePrepareHardware;

EXTERN_C_END
