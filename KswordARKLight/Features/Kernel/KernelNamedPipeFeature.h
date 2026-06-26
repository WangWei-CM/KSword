#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateNamedPipeDescriptor returns metadata for the retained "命名管道" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateNamedPipeDescriptor();

} // namespace Ksword::Features::Kernel
