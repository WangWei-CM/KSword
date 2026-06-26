#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateCallbackInterceptDescriptor returns metadata for the retained "驱动回调" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateCallbackInterceptDescriptor();

} // namespace Ksword::Features::Kernel
