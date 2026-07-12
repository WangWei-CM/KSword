#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Hardware {

// CreateHardwareHwidDispatchView creates the Light HWID Dispatch page. Inputs
// are the parent HWND and bounds; processing mirrors Ksword5.1's
// HardwareHwidDispatchPage ArkDriverClient query/control calls; output is the
// child HWND or nullptr on creation failure.
HWND CreateHardwareHwidDispatchView(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Hardware
