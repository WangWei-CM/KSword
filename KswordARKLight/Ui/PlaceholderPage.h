#pragma once

#include "ModuleDescriptor.h"
#include "../Core/Win32Lean.h"

namespace Ksword::Ui {

// PlaceholderPage creates a blank child page for modules that fail to initialize.
// Inputs are parent, descriptor and rectangle; processing creates a child window;
// output is the page HWND.
HWND CreatePlaceholderPage(HWND parent, const ModuleDescriptor& descriptor, const RECT& bounds);

// UpdatePlaceholderPage rewrites the blank page title. Inputs are page HWND and
// descriptor; processing updates window text; no value is returned.
void UpdatePlaceholderPage(HWND page, const ModuleDescriptor& descriptor);

} // namespace Ksword::Ui
