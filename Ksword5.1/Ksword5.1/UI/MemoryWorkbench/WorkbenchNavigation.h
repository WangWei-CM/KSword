#pragma once

// ============================================================
// WorkbenchNavigation.h
// 作用：
// - 定义内存工作台"跳转到某处"的统一值类型：NavRequest（一次跳转请求）与
//   NavStatus（跳转结果）。这里只是值类型，不含任何逻辑；真正的入口
//   `bool MemoryDock::navigateWorkbench(const NavRequest&)` 在后续工作包（K）
//   落地到 MemoryDock.Workbench.cpp，内部会依次调用 WorkbenchTarget 的
//   requestScope/requestIdentity/requestChannel 等再做 evaluate+跳转。
// - 设计文档《内存工作台Phase3集成设计.md》第 0 节第 1 项已把调研稿里单独的
//   WorkbenchOpenRequest 并入这里的 NavRequest；原 target.md 第 4 节表格中的
//   "Done" 在本文件写作 Ok（与本工作包的任务描述保持一致，含义相同：跳转已完成）。
//
// 本文件只用标准库 + Qt 基础类型（QString）+ Core 的 Scope/Channel 枚举，
// 不包含 Windows.h、不包含 Framework.h。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <QString>

#include <cstdint>
#include <optional>

namespace ks::ui
{
    // NavOrigin：这次跳转请求是从界面的哪个来源发出的，仅用于日志/诊断抽屉展示，
    // 不参与任何跳转逻辑的判定。
    enum class NavOrigin : std::uint32_t
    {
        AddressBar = 0,   // 地址条回车
        ModuleTable = 1,  // 模块表双击
        RegionTable = 2,  // 区域表双击/右键
        SearchHit = 3,    // 搜索结果表
        AddressBook = 4,  // 地址簿侧栏
        Pte = 5,          // PTE 页
        Evidence = 6,     // 证据/篡改等页面的右键"在内存工作台打开"
        External = 7,     // 其它（MainWindow 的反射调用、测试等）
    };

    // NavRequest：一次跳转请求。字段含义与 target.md 第 4 节一致：
    // - scope/pid/createTime：目标身份。pid==0 表示"跟随 Dock 当前附加的进程"
    //   （此时 createTime 被忽略）；pid!=0 表示钉住该 pid（createTime 非零时
    //   同时核对进程实例，防止 pid 被系统复用后指向了另一个进程）。
    // - address/selectLength：要跳转到的地址与选中长度（字节），selectLength
    //   至少为 1。
    // - channel：可选的目标通道；为空表示"不改通道，沿用当前会话的通道"。
    // - origin/note：来源标签与一句可选备注，仅用于诊断，不参与判定。
    // - focusView：是否要把工作台页签切到前台（跳转同时也要把插入点显示出来时为真；
    //   后台静默更新——例如内嵌窗口随 Dock 附加事件联动——时可置假）。
    struct NavRequest
    {
        ksword::memwb::Scope scope = ksword::memwb::Scope::ProcessVirtual;
        std::uint32_t pid = 0;
        std::uint64_t createTime = 0;
        std::uint64_t address = 0;
        std::uint64_t selectLength = 1;
        std::optional<ksword::memwb::Channel> channel;
        NavOrigin origin = NavOrigin::External;
        QString note;
        bool focusView = true;
    };

    // NavStatus：一次 navigateWorkbench 调用的结果。
    enum class NavStatus : std::uint32_t
    {
        // Ok：已完成跳转（含"同目标内跳转"的轻量路径）。
        Ok = 0,
        // NeedsAttach：没有可用目标（既没有钉住的进程，Dock 也没有附加），
        // 不把请求排队等待未来附加，调用方应保留地址条文本并提示用户先选择目标。
        NeedsAttach = 1,
        // NeedsScopeSwitch：请求的地址落在当前范围之外但在别的范围里有效
        // （典型：进程范围下输入了内核半区地址），调用方应提供"切换范围并跳转"
        // 的按钮，不得静默切换范围。
        NeedsScopeSwitch = 2,
        // TargetMismatch：请求指定的身份（scope/pid/createTime）与当前会话
        // 派生结果不一致（例如钉住后 tracker 校验失败），调用方应报告一致性问题。
        TargetMismatch = 3,
        // LeaveRefused：身份类变更前的离开守卫被用户否决（还有未写入的暂存，
        // 或未还原的 int3 补丁等），本次请求完全没有生效。
        LeaveRefused = 4,
        // Unavailable：请求本身合法，但当前策略不允许（例如内嵌窗口禁止
        // 内核/物理范围，或目标通道当前不可用），不弹模态框，由状态条报告原因。
        Unavailable = 5,
        // TargetGone：请求钉住的 pid 在打开时已经不存在（打开进程时系统报告
        // "这不是任何进程的 pid"，而不是单纯的权限不足）。与 TargetMismatch
        // 的区别：TargetMismatch 是"pid 存在但创建时间不是我们期望的那个实例"
        // （典型：pid 被系统复用成了另一个进程），TargetGone 是"pid 根本不存在
        // 任何进程"。两者都应该在问离开守卫之前就直接失败，不应该先问用户一次
        // 再告诉他目标不对（见 WorkbenchTarget.h 的 IdentityRequest::expectCreateTime
        // 与 WorkbenchTarget::requestIdentity 的实现）。
        TargetGone = 6,
        // Accepted leave/apply request; original navigation continues after completion.
        Pending = 7,
    };
}
