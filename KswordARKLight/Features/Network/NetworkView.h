#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Network {

// CreateNetworkFeatureView creates the read-only Network audit page. Inputs are
// the parent HWND and parent-relative bounds; processing creates a tab host and
// live audit tables for TCP/UDP cross-view, WFP and NDIS plus explicit
// documented R3 projection rows for protocols that do not yet have shared wrappers;
// output is the created child HWND or nullptr on failure.
HWND CreateNetworkFeatureView(HWND parent, const RECT& bounds);

// ResizeNetworkFeatureView moves an existing Network audit page. Inputs are the
// child HWND and new bounds; processing delegates layout to WM_SIZE; no value is
// returned.
void ResizeNetworkFeatureView(HWND view, const RECT& bounds);

} // namespace Ksword::Features::Network
