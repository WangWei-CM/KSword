#include "KernelCallbackInterceptFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateCallbackInterceptDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::CallbackIntercept;
    descriptor.title = L"驱动回调";
    descriptor.category = L"回调";
    descriptor.summary = L"保留回调拦截规则管理入口：注册表、进程、线程、镜像、对象和 minifilter 规则。";
    descriptor.backend = KernelFeatureBackend::ArkDriverClient;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = true;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
