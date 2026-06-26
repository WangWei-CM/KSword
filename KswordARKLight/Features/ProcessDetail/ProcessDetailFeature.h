#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::ProcessDetail {

// CreateProcessDetailPage is the integration facade for the process module.
// Inputs are a parent HWND, target PID and initial bounds; processing creates
// the independent ProcessDetailPage; output is the child HWND or nullptr on
// registration/window creation failure.
HWND CreateProcessDetailPage(HWND parent, DWORD processId, const RECT& bounds);

} // namespace Ksword::Features::ProcessDetail
