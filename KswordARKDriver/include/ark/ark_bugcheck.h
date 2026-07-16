#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "driver/KswordArkBugcheckIoctl.h"

EXTERN_C_START

// Probe VMware SVGA-II, prepare the nonpaged diagnostic cache, and register
// bugcheck callbacks only when the supported virtual display is usable.
NTSTATUS
KswordARKBugcheckInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ WDFDEVICE ControlDevice
    );

// Deregister callbacks before releasing the mapped VMware framebuffer/FIFO.
VOID
KswordARKBugcheckUninitialize(
    VOID
    );

// Optional bitmap upload adapter registered through ioctl_registry.c.
NTSTATUS
KswordARKBugcheckIoctlSetBitmap(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

EXTERN_C_END

