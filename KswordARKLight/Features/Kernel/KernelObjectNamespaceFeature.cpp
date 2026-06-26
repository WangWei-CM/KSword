#include "KernelObjectNamespaceFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateObjectNamespaceDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::ObjectNamespaceOverview;
    descriptor.title = L"对象命名空间";
    descriptor.category = L"对象命名空间";
    descriptor.summary = L"保留原 KernelDock 默认页：Object Manager 命名空间总览、树形浏览、属性详情与过滤入口。";
    descriptor.backend = KernelFeatureBackend::Hybrid;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
