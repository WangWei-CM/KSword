#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Hardware {

// CreateHardwareFeaturePage is the module facade for hardware device management.
// Inputs are the dock parent HWND and parent-relative bounds; processing delegates
// to HardwareView and performs SetupAPI/Configuration Manager enumeration inside
// this module; output is the created child HWND or nullptr on failure.
HWND CreateHardwareFeaturePage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Hardware
