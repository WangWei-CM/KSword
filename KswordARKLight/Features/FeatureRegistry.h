#pragma once

#include "../Ui/ModuleDescriptor.h"

#include <vector>

namespace Ksword::Features {

// GetModuleDescriptors returns every first-stage module exposed by the Win32
// shell. There is no input; processing returns static descriptors for docked
// module pages; output is copied by the main window.
std::vector<Ksword::Ui::ModuleDescriptor> GetModuleDescriptors();

} // namespace Ksword::Features
