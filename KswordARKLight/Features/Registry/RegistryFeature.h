#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Registry {

// CreateRegistryFeaturePage is the module-local facade for the registry dock.
// Inputs are parent HWND and initial bounds; output is the root page HWND.
HWND CreateRegistryFeaturePage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Registry
