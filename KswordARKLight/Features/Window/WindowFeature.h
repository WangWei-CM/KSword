#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Window {

// CreateWindowFeaturePage is the module facade for retained non-desktop window
// management. Inputs are the dock parent HWND and bounds; processing delegates to
// WindowView and uses EnumWindows/GetWindowInfo/GetClassName/GetWindowText inside
// this module; output is the created child HWND or nullptr on failure.
HWND CreateWindowFeaturePage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Window
