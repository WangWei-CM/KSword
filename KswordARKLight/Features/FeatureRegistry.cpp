#include "FeatureRegistry.h"

#include "Driver/DriverFeature.h"
#include "File/FileFeature.h"
#include "Hardware/HardwareFeature.h"
#include "Kernel/KernelFeature.h"
#include "Memory/MemoryFeature.h"
#include "Monitor/MonitorFeature.h"
#include "Process/ProcessFeature.h"
#include "Registry/RegistryFeature.h"
#include "Startup/StartupFeature.h"
#include "Window/WindowFeature.h"

namespace Ksword::Features {

std::vector<Ksword::Ui::ModuleDescriptor> GetModuleDescriptors() {
    return {
        { 40001, L"进程", L"NtQuerySystemInformation 进程列表、友好视图/详细视图、多选、图标、拖动选择和进程右键菜单。", Process::CreateProcessFeaturePage },
        { 40002, L"内存", L"仅保留通过 KswordARK R0 驱动执行的内存读取和写入。", Memory::CreateMemoryFeaturePage },
        { 40010, L"注册表", L"WinAPI/R0 双模式注册表浏览与读写、创建、删除、重命名。", Registry::CreateRegistryFeaturePage },
        { 40003, L"文件", L"Windows API 路径枚举和文件右键菜单；文件属性页已移除。", File::CreateFileFeaturePage },
        { 40004, L"驱动", L"驱动概览和对象信息。", Driver::CreateDriverFeaturePage },
        { 40005, L"内核", L"保留 SSDT、Shadow SSDT、Hook、对象命名空间、回调等内核功能入口。", [](HWND parent, const RECT& bounds) -> HWND {
            return Kernel::CreateKernelFeaturePage(parent, 40005, bounds);
        } },
        { 40006, L"监控", L"ETW 监控主页面，筛选器通过弹窗配置。", Monitor::CreateMonitorFeaturePage },
        { 40007, L"硬件", L"仅保留设备管理。", Hardware::CreateHardwareFeaturePage },
        { 40008, L"窗口", L"窗口管理和详细信息；桌面管理已移除。", Window::CreateWindowFeaturePage },
        { 40009, L"启动项", L"启动项管理。", Startup::CreateStartupFeaturePage }
    };
}

} // namespace Ksword::Features
