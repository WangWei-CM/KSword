#include "KernelObjectDirectoryRecursiveFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateObjectDirectoryRecursiveDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::ObjectDirectoryRecursive;
    descriptor.title = L"目录递归";
    descriptor.category = L"对象命名空间";
    descriptor.summary = L"保留目录递归页入口：递归枚举 Object Manager 目录并展示 Directory/SymbolicLink 等对象。";
    descriptor.backend = KernelFeatureBackend::UserModeNative;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
