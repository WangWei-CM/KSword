#include "KernelSymbolicLinkFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateSymbolicLinkDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::SymbolicLink;
    descriptor.title = L"符号链接";
    descriptor.category = L"对象命名空间";
    descriptor.summary = L"保留符号链接页入口：NtOpenSymbolicLinkObject/NtQuerySymbolicLinkObject 目标解析。";
    descriptor.backend = KernelFeatureBackend::UserModeNative;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
