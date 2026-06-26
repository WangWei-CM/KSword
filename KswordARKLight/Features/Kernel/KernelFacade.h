#pragma once

#include "KernelModel.h"

namespace Ksword::Features::Kernel {

// KernelFacade is the module-local boundary between Win32 UI and future
// ArkDriverClient/native workers. Inputs are KernelRequest packets from UI;
// processing routes by KernelFeatureId; outputs are generic operation packets.
// No HWND, control id, or drawing state is accepted here by design.
class KernelFacade final {
public:
    KernelFacade() = default;
    ~KernelFacade() = default;

    KernelFacade(const KernelFacade&) = delete;
    KernelFacade& operator=(const KernelFacade&) = delete;

    // QueryFeature requests a read-only snapshot for one retained kernel entry.
    // Input is a KernelRequest; processing calls the matching private query
    // method; output contains support state, diagnostics, and any rows.
    KernelOperationResult QueryFeature(const KernelRequest& request) const;

    // ExecuteAction runs one explicit row/menu operation. Input is a validated
    // KernelActionRequest from KernelPage; processing still uses ArkDriverClient
    // rather than raw driver handles; output is displayed in the same result grid.
    KernelOperationResult ExecuteAction(const KernelActionRequest& request) const;

private:
    KernelOperationResult QueryArkDriverFeature(const KernelRequest& request) const;
};

} // namespace Ksword::Features::Kernel
