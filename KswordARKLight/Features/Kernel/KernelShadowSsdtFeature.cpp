#include "KernelShadowSsdtFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateShadowSsdtDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::ShadowSsdt;
    descriptor.title = L"SSSDT 解析";
    descriptor.category = L"系统调用";
    descriptor.summary = L"保留 Shadow SSDT 解析入口：win32k/win32u shadow syscall 快照。";
    descriptor.backend = KernelFeatureBackend::ArkDriverClient;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
