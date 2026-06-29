#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::File {

// CreateFileFeaturePage creates the File feature host. Inputs are the dock
// parent HWND and initial bounds; processing hosts the retained browser page
// plus read-only audit tabs for FileObject/Section/filter/storage visibility;
// output is the created child HWND or nullptr on failure.
HWND CreateFileFeaturePage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::File
