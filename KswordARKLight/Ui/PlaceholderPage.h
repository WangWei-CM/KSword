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

// SetPlaceholderPageLoading updates the visible state before a lazy dock page
// is materialized. The update is deliberately paint-only and never invokes the
// module factory, so a tab switch can provide immediate feedback.
void SetPlaceholderPageLoading(HWND page, bool loading, const std::wstring& status = L"");

} // namespace Ksword::Ui
