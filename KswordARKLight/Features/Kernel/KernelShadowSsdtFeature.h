#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateShadowSsdtDescriptor returns metadata for the retained "SSSDT 解析" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateShadowSsdtDescriptor();

} // namespace Ksword::Features::Kernel
