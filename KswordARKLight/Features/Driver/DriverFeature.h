#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Driver {

// CreateDriverFeaturePage creates the root driver feature page. Inputs are the
// parent HWND and client bounds; processing owns the tab control, refresh and
// export buttons, and the two read-only subviews; output is the created page
// HWND or nullptr on failure.
HWND CreateDriverFeaturePage(HWND parent, const RECT& bounds);

// ResizeDriverFeaturePage moves an existing driver page. Inputs are a page HWND
// and new client bounds; processing delegates to MoveWindow; no value is
// returned.
void ResizeDriverFeaturePage(HWND page, const RECT& bounds);

} // namespace Ksword::Features::Driver
