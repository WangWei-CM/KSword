#pragma once

#include "../../Core/Win32Lean.h"
#include "KernelModel.h"

#include <vector>

namespace Ksword::Features::Kernel {

// GetKernelFeatureCatalog exposes this module's retained kernel feature list to
// the feature registry. There is no input; processing returns the
// module-local catalog; output is a copy of descriptor rows for menus/pages.
std::vector<KernelFeatureDescriptor> GetKernelFeatureCatalog();

// CreateKernelFeaturePage creates the Win32-light kernel page for this module.
// Inputs are parent HWND, control id, and bounds; processing delegates to the
// module-local UI implementation; output is a child HWND or nullptr on failure.
HWND CreateKernelFeaturePage(HWND parent, int controlId, const RECT& bounds);

// CreateKernelSingleFeaturePage creates the kernel UI with one requested
// feature selected. Inputs are parent HWND, control id, bounds, and the stable
// feature id; output is a child HWND or nullptr on failure.
HWND CreateKernelSingleFeaturePage(HWND parent, int controlId, const RECT& bounds, KernelFeatureId featureId);

} // namespace Ksword::Features::Kernel
