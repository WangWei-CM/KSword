#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateAtomTableDescriptor returns metadata for the retained "原子表遍历" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateAtomTableDescriptor();

} // namespace Ksword::Features::Kernel
