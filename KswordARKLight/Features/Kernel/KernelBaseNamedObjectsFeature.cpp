#include "KernelBaseNamedObjectsFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateBaseNamedObjectsDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::BaseNamedObjects;
    descriptor.title = L"BaseNamedObjects";
    descriptor.category = L"对象命名空间";
    descriptor.summary = L"保留 BaseNamedObjects 聚合页入口：Global、Session 和 BaseNamedObjects 只读快照。";
    descriptor.backend = KernelFeatureBackend::UserModeNative;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
