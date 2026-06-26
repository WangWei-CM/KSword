#include "KernelIatEatHookFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateIatEatHookDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::IatEatHook;
    descriptor.title = L"IAT/EAT 钩子检测";
    descriptor.category = L"内核钩子";
    descriptor.summary = L"保留内核模块 IAT/EAT 钩子检测入口：只读扫描可疑导入/导出目标。";
    descriptor.backend = KernelFeatureBackend::ArkDriverClient;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
