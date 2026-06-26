#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Memory {

// CreateMemoryFeaturePage is the module-local facade for the memory feature UI.
// Inputs are the dock parent and initial bounds; processing creates the
// driver-only memory read/write view; output is the page HWND or null on failure.
HWND CreateMemoryFeaturePage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Memory
