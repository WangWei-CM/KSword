#include "KernelR0ExtendedFeature.h"

namespace Ksword::Features::Kernel {
namespace {

// Descriptor creates one ArkDriverClient read-only feature descriptor. Inputs
// are the feature id, title, category, and summary; output is catalog metadata
// only, with no driver calls or UI ownership.
KernelFeatureDescriptor Descriptor(
    const KernelFeatureId id,
    const std::wstring& title,
    const std::wstring& category,
    const std::wstring& summary) {
    KernelFeatureDescriptor descriptor;
    descriptor.id = id;
    descriptor.title = title;
    descriptor.category = category;
    descriptor.summary = summary;
    descriptor.backend = KernelFeatureBackend::ArkDriverClient;
    descriptor.requiresAdministrator = true;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace

std::vector<KernelFeatureDescriptor> CreateR0ExtendedDescriptors() {
    // These entries expose existing ArkDriverClient read-only protocol paths in
    // KernelDock instead of leaving them reachable only from other legacy docks.
    return {
        Descriptor(
            KernelFeatureId::KernelExecutableMemory,
            L"内核可执行内存",
            L"内核信息",
            L"通过 ArkDriverClient 扫描 R0 可执行内核页、页权限和模块归属。"),
        Descriptor(
            KernelFeatureId::KernelMemoryEvidence,
            L"内核内存证据",
            L"内核信息",
            L"通过 ArkDriverClient 聚合内核内存风险证据、BigPool、模块和样本摘要。"),
        Descriptor(
            KernelFeatureId::ProcessCrossView,
            L"进程 CrossView",
            L"内核信息",
            L"通过 ArkDriverClient 比对 EPROCESS ActiveProcessLinks/CID 等来源。"),
        Descriptor(
            KernelFeatureId::ThreadCrossView,
            L"线程 CrossView",
            L"内核信息",
            L"通过 ArkDriverClient 比对 ETHREAD/KTHREAD/CID 等来源。"),
        Descriptor(
            KernelFeatureId::DriverIntegrity,
            L"驱动完整性",
            L"驱动诊断",
            L"通过 ArkDriverClient 聚合 DriverObject/LDR/FastIo/MajorFunction 完整性证据。"),
        Descriptor(
            KernelFeatureId::KernelCpuIntegrity,
            L"CPU/IDT 完整性",
            L"驱动诊断",
            L"通过 ArkDriverClient 查询 CPU、IDT、描述符表和相关完整性证据。"),
        Descriptor(
            KernelFeatureId::CpuHardwareSnapshot,
            L"CPU 硬件快照",
            L"硬件",
            L"通过 ArkDriverClient 读取 R0 CPUID/处理器数量与特征位摘要。"),
        Descriptor(
            KernelFeatureId::PhysicalMemoryLayout,
            L"物理内存布局",
            L"硬件",
            L"通过 ArkDriverClient 读取 MmGetPhysicalMemoryRanges 聚合物理内存范围。"),
        Descriptor(
            KernelFeatureId::MutationAudit,
            L"内核修改审计",
            L"驱动诊断",
            L"通过 ArkDriverClient 只读展示 mutation transaction 审计环。"),
        Descriptor(
            KernelFeatureId::KeyboardHotkeys,
            L"键盘热键枚举",
            L"内核信息",
            L"通过 ArkDriverClient 枚举 win32k RegisterHotKey 内部表。"),
        Descriptor(
            KernelFeatureId::KeyboardHooks,
            L"键盘钩子枚举",
            L"内核信息",
            L"通过 ArkDriverClient 枚举 win32k WH_KEYBOARD/WH_KEYBOARD_LL 钩子链。"),
        Descriptor(
            KernelFeatureId::DynDataCapabilities,
            L"DynData 能力",
            L"驱动诊断",
            L"通过 ArkDriverClient 查询轻量 DynData capability mask 与状态。"),
        Descriptor(
            KernelFeatureId::MinifilterBypassPids,
            L"Minifilter 放行 PID",
            L"回调",
            L"通过 ArkDriverClient 查询 R0 minifilter bypass PID 白名单。")
    };
}

} // namespace Ksword::Features::Kernel
