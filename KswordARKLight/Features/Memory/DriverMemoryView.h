#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Memory {

// CreateDriverMemoryView creates the Win32 child surface for driver-only memory
// read/write. Inputs are parent window and initial bounds; processing registers
// the page class and creates child controls; output is the page HWND or null on
// failure.
HWND CreateDriverMemoryView(HWND parent, const RECT& bounds);

// CreateProcessMemoryEvidenceView creates the read-only process VA evidence
// surface. Inputs are parent window and initial bounds; processing uses only R3
// VirtualQueryEx / QueryWorkingSetEx fallback evidence and never issues write,
// patch, unlink or protection-changing actions; output is the page HWND or null
// on failure.
HWND CreateProcessMemoryEvidenceView(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Memory
