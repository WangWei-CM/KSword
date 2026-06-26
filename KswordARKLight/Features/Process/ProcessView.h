#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Process {

// CreateProcessView creates the Win32 child page for the process list feature.
// Inputs are the parent HWND and initial bounds. Processing registers a private
// window class, creates toolbar/status/list controls, and starts a first process
// snapshot refresh. Return value is the page HWND, or null when registration or
// child-window creation fails.
HWND CreateProcessView(HWND parent, const RECT& bounds);

// ResizeProcessView moves the already-created process page. Inputs are the page
// HWND and new bounds in parent-client coordinates. Processing calls MoveWindow;
// there is no return value because invalid HWNDs are ignored by Win32.
void ResizeProcessView(HWND view, const RECT& bounds);

} // namespace Ksword::Features::Process
