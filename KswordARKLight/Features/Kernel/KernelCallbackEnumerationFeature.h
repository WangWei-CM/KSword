#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateCallbackEnumerationDescriptor returns metadata for the retained "回调遍历" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateCallbackEnumerationDescriptor();

} // namespace Ksword::Features::Kernel
