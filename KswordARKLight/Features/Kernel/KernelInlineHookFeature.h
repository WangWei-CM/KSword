#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateInlineHookDescriptor returns metadata for the retained "Inline Hook 检测 & 摘除" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateInlineHookDescriptor();

} // namespace Ksword::Features::Kernel
