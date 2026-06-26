#include "KernelInlineHookFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateInlineHookDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::InlineHook;
    descriptor.title = L"Inline Hook 检测 & 摘除";
    descriptor.category = L"内核钩子";
    descriptor.summary = L"保留 Inline Hook 检测和摘除入口；摘除动作必须由 UI 二次确认后通过 facade 调用。";
    descriptor.backend = KernelFeatureBackend::ArkDriverClient;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = true;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
