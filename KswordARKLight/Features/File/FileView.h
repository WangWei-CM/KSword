#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::File {

// CreateFileViewPage creates the native Win32 file page used by KswordARKLight.
// Inputs are the parent HWND and initial bounds; processing creates a toolbar,
// path box, report list, status line and right-click action surface; output is
// the page HWND, or null if class/window creation fails.
HWND CreateFileViewPage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::File
