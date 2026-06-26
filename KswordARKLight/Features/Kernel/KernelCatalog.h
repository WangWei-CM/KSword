#pragma once

#include "KernelModel.h"

#include <vector>

namespace Ksword::Features::Kernel {

// GetKernelFeatureDescriptors returns every retained kernel feature entry from
// the original KernelDock. There is no input; processing concatenates the
// per-feature descriptor files in UI order; output is consumed by KernelPage and
// by any future module registry integration.
std::vector<KernelFeatureDescriptor> GetKernelFeatureDescriptors();

} // namespace Ksword::Features::Kernel
