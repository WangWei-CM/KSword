#pragma once

#include "KernelModel.h"

#include <vector>

namespace Ksword::Features::Kernel {

// CreateR0ExtendedDescriptors returns descriptors for ArkDriverClient read-only
// kernel capabilities that were not covered by the original first pass. There
// is no input; processing constructs pure metadata; output is appended to the
// Kernel Dock catalog.
std::vector<KernelFeatureDescriptor> CreateR0ExtendedDescriptors();

} // namespace Ksword::Features::Kernel
