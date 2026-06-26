#pragma once

#include "Win32Lean.h"

#include <string>

namespace Ksword::Core {

// DriverRuntimeStatus is the compact state shown by the R0 toolbar button. The
// fields are populated by QueryDriverStatus/InstallAndStartDriver and consumed
// by the main window status text.
struct DriverRuntimeStatus {
    bool driverFilePresent = false;
    bool serviceInstalled = false;
    bool serviceRunning = false;
    bool controlDeviceOpen = false;
    DWORD serviceState = 0;
    DWORD controlDeviceError = ERROR_SUCCESS;
    std::wstring driverPath;
    std::wstring message;
};

// ResolveDriverPath locates KswordARK.sys next to the executable. There is no
// input; processing joins ModuleDirectory() and the driver filename; output is
// the expected absolute path whether or not the file exists.
std::wstring ResolveDriverPath();

// QueryDriverStatus inspects the SCM service and driver file. There is no input;
// processing opens the service control manager and KswordARK service; output is
// a complete status object suitable for UI display.
DriverRuntimeStatus QueryDriverStatus();

// InstallAndStartDriver ensures the service exists and is running. There is no
// input; processing creates/updates the kernel-driver service from KswordARK.sys
// beside the executable and starts it; output describes success/failure details.
DriverRuntimeStatus InstallAndStartDriver();

// StopDriverService asks SCM to stop the service. There is no input; processing
// sends SERVICE_CONTROL_STOP when the service exists; output is a status object
// after the requested operation.
DriverRuntimeStatus StopDriverService();

} // namespace Ksword::Core
