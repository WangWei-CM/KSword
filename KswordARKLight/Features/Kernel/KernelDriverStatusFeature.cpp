#include "KernelDriverStatusFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateDriverStatusDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::DriverStatus;
    descriptor.title = L"驱动状态";
    descriptor.category = L"驱动诊断";
    descriptor.summary = L"保留统一驱动状态和能力矩阵入口：协议、安全策略、DynData 与功能可用性。";
    descriptor.backend = KernelFeatureBackend::ArkDriverClient;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
