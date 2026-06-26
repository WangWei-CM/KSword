#include "KernelAtomTableFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateAtomTableDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::AtomTable;
    descriptor.title = L"原子表遍历";
    descriptor.category = L"内核信息";
    descriptor.summary = L"保留原子表遍历入口：GlobalAtom 与 ClipboardFormat 相关只读枚举。";
    descriptor.backend = KernelFeatureBackend::UserModeNative;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
