#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateObjectNamespaceDescriptor returns metadata for the retained "对象命名空间" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateObjectNamespaceDescriptor();

} // namespace Ksword::Features::Kernel
