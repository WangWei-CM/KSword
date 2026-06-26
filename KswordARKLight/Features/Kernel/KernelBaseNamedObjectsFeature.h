#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateBaseNamedObjectsDescriptor returns metadata for the retained "BaseNamedObjects" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateBaseNamedObjectsDescriptor();

} // namespace Ksword::Features::Kernel
