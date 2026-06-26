#include "KernelDynDataFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateDynDataDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::DynData;
    descriptor.title = L"动态偏移";
    descriptor.category = L"驱动诊断";
    descriptor.summary = L"保留 DynData 状态、字段、capability 和本地 PDB profile 应用入口。";
    descriptor.backend = KernelFeatureBackend::ArkDriverClient;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
