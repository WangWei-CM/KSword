#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Memory {

// CreateDriverMemoryView creates the Win32 child surface for driver-only memory
// read/write. Inputs are parent window and initial bounds; processing registers
// the page class and creates child controls; output is the page HWND or null on
// failure.
HWND CreateDriverMemoryView(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Memory
