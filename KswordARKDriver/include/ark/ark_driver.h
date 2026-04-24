#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <initguid.h>

#include "ark_device.h"
#include "ark_queue.h"
#include "ark_process.h"
#include "ark_log.h"
#include "Trace.h"

EXTERN_C_START

// WDFDRIVER Events
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD KswordARKDriverEvtDriverUnload;
EVT_WDF_OBJECT_CONTEXT_CLEANUP KswordARKDriverEvtDriverContextCleanup;

EXTERN_C_END
