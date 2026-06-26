#include "KernelDeviceDriverObjectsFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateDeviceDriverObjectsDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::DeviceDriverObjects;
    descriptor.title = L"设备与驱动对象";
    descriptor.category = L"对象命名空间";
    descriptor.summary = L"\\Device、\\Driver、\\FileSystem 对象目录视图，并对 \\Driver 项追加 ArkDriverClient R0 DriverObject 详情。";
    descriptor.backend = KernelFeatureBackend::Hybrid;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = true;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
