#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// CreateNtQueryLegacyDescriptor returns metadata for the retained "历史 NtQuery" entry.
// There is no input; processing fills a descriptor matching the original
// KernelDock entry; output is consumed by the lightweight Win32 catalog.
KernelFeatureDescriptor CreateNtQueryLegacyDescriptor();

} // namespace Ksword::Features::Kernel
