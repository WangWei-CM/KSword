#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateObjectDirectoryRecursiveDescriptor returns metadata for the retained "目录递归" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateObjectDirectoryRecursiveDescriptor();

} // namespace Ksword::Features::Kernel
