#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Hardware {

// CreateHardwareDeviceManagerView creates the device-manager-only hardware page.
// Inputs are parent HWND and parent-relative bounds; processing registers the
// module window class, creates tree/detail controls, and performs an initial
// SetupAPI enumeration; output is the child page HWND or nullptr on failure.
HWND CreateHardwareDeviceManagerView(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Hardware
