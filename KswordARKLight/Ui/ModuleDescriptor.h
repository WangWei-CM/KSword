#pragma once

#include "../Core/Win32Lean.h"

#include <string>

namespace Ksword::Ui {

// ModulePageFactory creates one dockable module page. Inputs are the dock host
// HWND and initial host-relative bounds; processing is owned by each feature
// module; output is the created child HWND or nullptr so the shell can fall back
// to a diagnostic blank page.
using ModulePageFactory = HWND (*)(HWND parent, const RECT& bounds);

// ModuleDescriptor describes one dockable feature page. Inputs are static
// strings and an optional page factory created by feature modules; the main
// window copies and registers them as tabs inside the docking host.
struct ModuleDescriptor {
    int commandId = 0;
    std::wstring title;
    std::wstring summary;
    ModulePageFactory createPage = nullptr;
};

} // namespace Ksword::Ui
