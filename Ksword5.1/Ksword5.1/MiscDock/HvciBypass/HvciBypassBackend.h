#pragma once

// ============================================================
// HvciBypassBackend.h
// 作用：
// - HVCI 绕过实验页的后端接口；测试 MmChangeImageProtection
//   能否在 HVCI 启用的环境下将内核映像页从 RX 改成只读 NX，
//   再通过 MDL 别名映射写入修改后的页面内容。
// - 本文件只定义数据结构与函数声明，不含 QWidget 依赖；
//   UI 由 HvciBypassPage 负责。
// - IOCTL 通信细节由实现文件通过 ArkDriverClient 完成，
//   本头文件不直接引用 shared/driver/ 的协议结构。
// ============================================================

#include <QString>

#include <cstdint>
#include <vector>

// 复用 DisableDse 已有的 CI/HVCI 姿态查询。
#include "../DisableDse/DisableDseBackend.h"

namespace ks::misc::hvci_bypass
{
    // ----------------------------------------------------------
    // 实验标志位，与 R0 协议中的 KSWORD_ARK_IMAGE_PROT_FLAG_* 对应。
    // 此处重新声明供 R3 UI 使用，避免 UI 直接引用协议头。
    // ----------------------------------------------------------

    // kFlagWriteTest：请求 R0 在别名映射上执行写入测试。
    inline constexpr unsigned long kFlagWriteTest = 0x00000001UL;

    // kFlagRestore：请求 R0 在写入测试后将页面恢复为 RX。
    inline constexpr unsigned long kFlagRestore = 0x00000002UL;

    // kFlagUnmapBeforeRestore：恢复前先解除别名映射。
    inline constexpr unsigned long kFlagUnmapBeforeRestore = 0x00000004UL;

    // ----------------------------------------------------------
    // 操作类型，与 R0 协议中的 KSWORD_ARK_IMAGE_PROT_OP_* 对应。
    // ----------------------------------------------------------

    // kOperationRx：将页面保护设为可读可执行（RX）。
    inline constexpr unsigned long kOperationRx = 1UL;

    // kOperationReadOnlyNx：将页面保护设为只读不可执行（NX）。
    inline constexpr unsigned long kOperationReadOnlyNx = 2UL;

    // ----------------------------------------------------------
    // KernelModuleInfo：内核模块信息，用于选择实验目标。
    // ----------------------------------------------------------
    struct KernelModuleInfo
    {
        QString name;                       // 模块名（如 CI.dll、ntoskrnl.exe）。
        std::uint64_t baseAddress = 0;      // 内核加载基址。
        std::uint32_t imageSize = 0;        // 映像大小（字节）。
    };

    // ----------------------------------------------------------
    // ExperimentResult：一次 HVCI 绕过实验的完整结果。
    // 字段从 R0 响应转译而来，byte 级字段由 R0 把 PTE 槽位
    // 复用为单字节读回值（详见协议头注释）。
    // ----------------------------------------------------------
    struct ExperimentResult
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        long resolverStatus = 0;            // MmChangeImageProtection 函数解析状态。
        long mdlAllocStatus = 0;            // MDL 分配状态。
        long lockStatus = 0;                // 页面锁定状态。
        long imageProtectStatus = 0;        // 映像保护变更状态（RX → NX）。
        long aliasMapStatus = 0;            // 别名映射状态。
        long writeTestStatus = 0;           // 通过别名写入测试状态。
        long restoreStatus = 0;             // RX 恢复状态。

        std::uint64_t resolvedFunctionVa = 0;   // R0 解析到的 MmChangeImageProtection 内核地址。
        std::uint8_t preModifyByte = 0;          // 修改前从目标 VA 读到的原始字节。
        std::uint8_t postWriteByte = 0;          // 通过别名写入后从原始 VA 读回的字节。
        std::uint8_t postRestoreByte = 0;        // 恢复 RX 后从原始 VA 读回的字节。
        std::uint64_t aliasVa = 0;               // R0 分配的别名映射虚拟地址。

        unsigned long flags = 0;            // 本次实验使用的标志位组合。
        QString ioErrorText;                // IOCTL 失败时的错误描述文本。
    };

    // ----------------------------------------------------------
    // 函数声明
    // ----------------------------------------------------------

    // queryCodeIntegrityPosture：
    // - 作用：查询当前系统的 CI / HVCI / 安全启动姿态；
    //   内部委托 disable_dse::queryPosture()，复用已有的 R0/R3 双通道逻辑；
    // - 返回：CodeIntegrityPosture 结构，queried 为 false 时看 failureText。
    disable_dse::CodeIntegrityPosture queryCodeIntegrityPosture();

    // ----------------------------------------------------------
    // ModuleEnumResult：模块枚举结果，区分成功但空和查询失败。
    // ----------------------------------------------------------
    struct ModuleEnumResult
    {
        bool ok = false;                        // ok：查询是否成功完成。
        QString errorText;                      // errorText：失败时的错误描述。
        std::vector<KernelModuleInfo> modules;  // modules：枚举到的模块列表。
    };

    // enumerateKernelModules：
    // - 作用：通过 R0 枚举当前已加载的内核模块列表，供用户选择实验目标；
    //   目标通常是 CI.dll 或其他不频繁换页的系统模块的 .text 节首页；
    // - 返回：ModuleEnumResult；ok=false 时 errorText 说明原因。
    ModuleEnumResult enumerateKernelModules();

    // runExperiment：
    // - 输入 targetVa：目标内核虚拟地址（通常选模块 .text 节的某一页起始）；
    //        flags：实验标志位组合（kFlagWriteTest / kFlagRestore 等）；
    //        functionOverride：非零时跳过签名扫描，直接使用该地址作为
    //                          MmChangeImageProtection 的入口（调试用途）；
    // - 作用：向 R0 发送 IOCTL_KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION，
    //   执行 "解析函数 → 分配 MDL → 锁定页面 → 变更映像保护 → 别名映射 →
    //   写入测试 → 恢复" 的完整实验流程；
    // - 返回：ExperimentResult，包含每个阶段的 NTSTATUS 和读回字节。
    ExperimentResult runExperiment(
        std::uint64_t targetVa,
        unsigned long flags,
        std::uint64_t functionOverride);

    // verdictText：
    // - 输入 result：一次实验的完整结果；
    // - 作用：根据各阶段状态生成面向用户的可读判定文本，说明
    //   HVCI 是否真正拦截了保护变更、写入是否穿透、恢复是否成功；
    // - 返回：多行 QString，可直接显示在结果区。
    QString verdictText(const ExperimentResult& result);

} // namespace ks::misc::hvci_bypass
