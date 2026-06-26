#include "KernelNtQueryLegacyFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateNtQueryLegacyDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::NtQueryLegacy;
    descriptor.title = L"历史 NtQuery";
    descriptor.category = L"内核信息";
    descriptor.summary = L"保留历史 NtQuery 页入口：旧版系统/对象/导出查询结果聚合展示。";
    descriptor.backend = KernelFeatureBackend::UserModeNative;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
