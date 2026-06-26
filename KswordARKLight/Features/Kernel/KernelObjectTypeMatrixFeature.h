#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateObjectTypeMatrixDescriptor returns metadata for the retained "对象类型矩阵" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateObjectTypeMatrixDescriptor();

} // namespace Ksword::Features::Kernel
