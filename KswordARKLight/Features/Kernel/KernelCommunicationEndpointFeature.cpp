#include "KernelCommunicationEndpointFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateCommunicationEndpointDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::CommunicationEndpoint;
    descriptor.title = L"通信端点";
    descriptor.category = L"对象命名空间";
    descriptor.summary = L"保留通信端点入口：ALPC/Port/Section/Event 等端点型对象聚合视图。";
    descriptor.backend = KernelFeatureBackend::Hybrid;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
