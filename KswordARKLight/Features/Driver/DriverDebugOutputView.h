#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Driver {

// CreateDriverDebugOutputView creates the bounded R0 DbgPrint capture page.
// Capture control and draining are always performed in workers; the returned
// child window owns its timer and cancels pending work on destruction.
HWND CreateDriverDebugOutputView(HWND parent, const RECT& bounds);

// ResizeDriverDebugOutputView moves the retained child view with its tab page.
void ResizeDriverDebugOutputView(HWND view, const RECT& bounds);

} // namespace Ksword::Features::Driver
