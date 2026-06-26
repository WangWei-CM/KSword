#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::File {

// CreateFileFeaturePage creates the Windows-API-only file browser page. Inputs
// are the dock parent HWND and initial bounds; processing delegates to FileView,
// which owns path enumeration and context-menu actions; output is the created
// child HWND or nullptr on failure.
HWND CreateFileFeaturePage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::File
