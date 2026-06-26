#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateSsdtDescriptor returns metadata for the retained "SSDT 遍历" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateSsdtDescriptor();

} // namespace Ksword::Features::Kernel
