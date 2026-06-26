#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateIatEatHookDescriptor returns metadata for the retained "IAT/EAT 钩子检测" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateIatEatHookDescriptor();

} // namespace Ksword::Features::Kernel
