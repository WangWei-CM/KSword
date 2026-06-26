#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// IsNativeKernelFeature reports whether a KernelDock entry can be answered by
// the lightweight process itself with Win32/Native API calls. Input is a stable
// feature id; processing is a switch over the retained R3 pages; return is true
// only for pages implemented in KernelNativeQueries.cpp.
bool IsNativeKernelFeature(KernelFeatureId id);

// QueryNativeKernelFeature executes one read-only R3/Native kernel information
// query. Input is the UI request; processing calls ntdll/Win32 APIs without any
// direct driver transport; return contains rows and diagnostics for KernelPage.
KernelOperationResult QueryNativeKernelFeature(const KernelRequest& request);

// ExecuteNativeKernelAction runs read-only row actions for R3 Native pages. The
// input is a selected-row action packet copied by KernelPage; processing uses
// Nt*/Win32 APIs only and never opens the KswordARK driver directly; output is a
// generic operation result that can replace the result grid.
KernelOperationResult ExecuteNativeKernelAction(const KernelActionRequest& request);

} // namespace Ksword::Features::Kernel
