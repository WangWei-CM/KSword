#pragma once

// ============================================================
// Int3Controller.h
// 作用：
// - int3 补丁（往目标进程某地址写一个 0xCC、记下原字节，之后能原样还原）的 Qt 装配层。
//   本类自己不做任何真实读写，持有 Core 账本 ksword::memwb::Int3PatchLedger 记账，
//   并通过调用方注入的工厂把账本要求的 IPatchByteStore 落到真实或假的 IMemoryIoPort 上。
// - 用户决策（设计文档第 0 节第 17 条、第 6 节）：int3 补丁零摩擦，写入与还原都不弹二次确认框。
//   本类内部除 RequestLeave 的"退出前还有未还原补丁"三选一外，**不会创建任何其它弹框**。
// - 内核范围、物理范围与磁盘传输通道上的安装请求，在调用账本之前就被拒绝（EvaluateCurrentRoute），
//   不会发起任何端口调用；还原固定使用安装时记录的通道（m_installChannel 这张"旁路表"），
//   不随当前会话通道切换而漂移——这是设计文档明确要求的"还原通道必须等于安装通道"。
// - 全文不出现"断点"二字：这不是调试器的断点（没有命中计数、没有捕获执行），
//   只是往内存里写一个字节、之后能写回去，界面措辞统一叫"int3 补丁"。
//
// 线程模型：仅 UI 线程调用。本类是 QObject（需要信号），不要求 Qt-free。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/Int3PatchLedger.h"
#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class QWidget;

namespace ks::ui
{
    // Int3RouteReject：安装请求在"连账本都没有调用"的阶段就被拒绝的原因。
    // 界面（Int3PatchPanel）按这个枚举翻译成中文提示，本类自己不产生面向用户的文案。
    enum class Int3RouteReject
    {
        None,                // 未被拒绝，允许继续走账本
        UnsupportedScope,    // 当前范围不是进程虚拟范围（内核 / 物理）
        UnsupportedChannel   // 当前通道是磁盘传输（Ddma）
    };

    // Int3LeaveScenario：触发"退出前提示"的场景。三种场景目前共用同一条判断
    // （HasUnrestored() 为真就提示），单独列举是为了日后按场景改措辞时不必改签名。
    enum class Int3LeaveScenario
    {
        DockDetach,       // 从目标进程分离
        ProcessChange,    // 即将附加到另一个进程
        MainWindowClose   // 关闭主窗口
    };

    // Int3LeaveChoice：三选一提示里用户的选择。默认按钮是 RestoreAllThenContinue
    // （规格见 docs/内存工作台Phase3集成设计.md 第 0 节第 6 条："默认全部还原后继续"），
    // Esc 的结果是 Cancel——默认按钮与 Esc 是分开设置的两件事，不要假设它们永远相同
    // （曾经犯过这个错，详见 2026 年审核报告缺陷 2）。
    enum class Int3LeaveChoice
    {
        RestoreAllThenContinue,  // 全部还原后继续
        KeepAndContinue,         // 保留补丁继续
        Cancel                   // 取消
    };

    // Int3InstallOutcome：一次 Install 的完整结果。
    // routeReject 非 None 时，status 恒为 None——表示这次请求连账本都没有被调用。
    struct Int3InstallOutcome
    {
        Int3RouteReject routeReject = Int3RouteReject::None;             // 路由预检结果
        ksword::memwb::InstallStatus status = ksword::memwb::InstallStatus::None; // 账本结果
        std::uint64_t id = 0;              // 已安装或失败后仍需恢复的条目 id；无条目时为 0
        bool rollbackAttempted = false;    // 兼容字段；当前账本不自动回滚，恒为 false
        bool rollbackWriteOk = false;      // 兼容字段；没有回滚写入，恒为 false
    };

    // Int3RestoreOutcome：一次 Restore 的结果。还原不做范围/通道预检（通道已经由安装时
    // 固定），所以没有 routeReject 字段。
    struct Int3RestoreOutcome
    {
        ksword::memwb::RestoreStatus status = ksword::memwb::RestoreStatus::None; // 账本结果
        bool hasObservedByte = false;       // observedByte 是否有效
        std::uint8_t observedByte = 0;      // Diverged 时为当前字节，VerifyFailed 时为回读字节
    };

    // Int3Controller：见文件头说明。
    class Int3Controller final : public QObject
    {
        Q_OBJECT
    public:
        // ByteStoreFactory：按 (target, channel) 构造一个已绑定好的 IPatchByteStore。
        // 传入：target 要写入/还原的目标身份；channel 必须使用的通道（安装时取"当前通道"，
        //       还原时取"安装时记录的通道"，两者由调用方——本类——负责区分，工厂不需要关心）。
        // 传出：构造失败（例如目标此刻已不可达）返回空指针，账本不会被调用。
        // 真实实现在后续工作包里把 (target, channel) 转译成 WorkbenchIoPorts 的真实端口并
        // 包一层 ksword::memwb::MemoryPatchByteStore；本类对工厂的内部实现一无所知。
        using ByteStoreFactory = std::function<std::unique_ptr<ksword::memwb::IPatchByteStore>(
            const ksword::memwb::PatchTarget& target,
            ksword::memwb::Channel channel)>;

        // 构造：factory 必须非空可调用，parent 走 QObject 常规所有权。
        explicit Int3Controller(ByteStoreFactory factory, QObject* parent = nullptr);

        // SetCurrentContext：外部（最终由 WorkbenchTarget 的会话变化驱动，当前由调用方/测试
        // 直接调用）更新"当前目标"。Install 的路由预检、Restore/RestoreAll 的目标匹配都基于
        // 这里记录的值。调用方法：目标、范围、通道三者任一变化都重新调用一次。
        void SetCurrentContext(
            const ksword::memwb::PatchTarget& target,
            ksword::memwb::Scope scope,
            ksword::memwb::Channel channel);

        // 以下三个只读查询：当前目标身份 / 当前范围 / 当前通道。
        const ksword::memwb::PatchTarget& CurrentTarget() const { return m_currentTarget; }
        ksword::memwb::Scope CurrentScope() const { return m_currentScope; }
        ksword::memwb::Channel CurrentChannel() const { return m_currentChannel; }

        // EvaluateCurrentRoute：只读查询，不做任何 I/O。面板用它提前把"写入"按钮置灰，
        // 不必等用户点了按钮才发现这个范围/通道不支持。
        Int3RouteReject EvaluateCurrentRoute() const;

        // Install：在当前目标的 address 处写入 0xCC。
        // 传入：target 调用方此刻认为的目标身份（通常就是 CurrentTarget()）；address 目标地址；
        //       nowTick 由调用方给定的时钟读数（保持本类可测，不自己读时钟）。
        // 传出：Int3InstallOutcome，见上。任何有效恢复条目的安装通道都会记入旁路表。
        Int3InstallOutcome Install(
            const ksword::memwb::PatchTarget& target,
            std::uint64_t address,
            std::uint64_t nowTick);

        // Restore：还原一条记录。固定使用该记录安装时的通道（旁路表），与当前通道无关；
        // 旁路表查不到时（异常情况：从未产生恢复条目的 id）退化为用当前通道兜底，
        // 这种情况下 id 本身会在账本里查不到，直接得到 NotFound，不会真的发起端口调用。
        Int3RestoreOutcome Restore(std::uint64_t id);

        // RestoreAll：还原属于"当前目标"的全部记录。逐条调用内部的还原逻辑，一条失败不中止
        // 后续；整个调用只发一次 changed。
        std::vector<ksword::memwb::PatchRestoreOutcome> RestoreAll();

        // OnTargetGone：转发给账本，把 (pid, createTime) 这个目标实例的待还原条目移入孤立列表。
        std::vector<ksword::memwb::PatchEntry> OnTargetGone(
            std::uint32_t pid,
            std::uint64_t processCreateTime100ns);

        // HasUnrestored / Entries / OrphanedEntries：直接转发账本的只读查询，口径是
        // "全账本"——包含当前目标之外、仍挂在账本里等待处理的其它目标的条目。
        bool HasUnrestored() const { return m_ledger.HasUnrestored(); }
        const std::vector<ksword::memwb::PatchEntry>& Entries() const { return m_ledger.Entries(); }
        const std::vector<ksword::memwb::PatchEntry>& OrphanedEntries() const { return m_ledger.OrphanedEntries(); }

        // HasUnrestoredForCurrentTarget / CountForCurrentTarget：与上面两个"全账本口径"不同，
        // 这两个只看"当前目标"（SetCurrentContext 设置的 pid + 进程创建时间）。
        // 退出前提示（RequestLeave 内部，以及未来 WP-J/K 把 Dock 分离 / 切换目标接到这里时）
        // 必须用这两个，不能用 HasUnrestored()/Entries().size()：后两者会把其它已经离开的
        // 目标遗留的条目也算进来，导致提示的"这次要处理多少处"与实际不符、"全部还原"对
        // 无关目标什么都没做却汇报成功（2026 年审核报告缺陷 1）。调用方法：不传参数，
        // 直接按当前上下文查询。
        bool HasUnrestoredForCurrentTarget() const;
        int CountForCurrentTarget() const;

        // Discard：丢弃一条待还原记录（不做任何 I/O），用于 Restore 返回 Diverged 之后。
        bool Discard(std::uint64_t id);

        // ClearOrphaned：清空孤立列表（界面提示过孤立项之后调用，对应面板的分组"清除"）。
        void ClearOrphaned();

        // InstalledChannel：查询已安装或失败恢复条目的原通道；查不到（从未产生条目，或已被还原 /
        // 丢弃 / 清理）返回 nullopt。供面板撰写"换个通道试试"一类提示。
        std::optional<ksword::memwb::Channel> InstalledChannel(std::uint64_t id) const;

        // ---- 退出前提示：纯逻辑部分可以不经 QMessageBox 单独测试 ----

        // NeedsLeavePrompt：给定"是否存在待还原补丁"，判断该场景是否需要提示三选一。
        // 三种场景目前判断一致；scenario 参数保留给日后场景差异化，不参与当前逻辑。
        static bool NeedsLeavePrompt(bool hasUnrestored, Int3LeaveScenario scenario);

        // ApplyLeaveChoice：按用户选择执行动作。
        // RestoreAllThenContinue：快照调用时的上下文，只有该目标的全部条目都真正被还原
        //   才返回 true；changed 回调切换当前上下文也不会改变此次还原对象与成功判据。
        //   否则返回 false，
        //   补丁仍留在账本里，调用方应据此继续阻止退出并转告原因。
        // KeepAndContinue：不碰账本，恒返回 true。
        // Cancel：不碰账本，恒返回 false。
        bool ApplyLeaveChoice(Int3LeaveChoice choice);

        // RequestLeave：HasUnrestoredForCurrentTarget() 为假时直接返回 true，不弹任何框——
        // 判据与弹框文案都只看"当前目标"的条目；其它已经离开的目标遗留的条目既不会拦这次
        // 退出，也不会被这次"全部还原"假装处理掉（它们原样留在账本里，面板表格本身就会
        // 按各自的目标标签把它们显示出来，不是被隐藏）。否则弹出"全部还原后继续｜保留补丁
        // 继续｜取消"三选一（QMessageBox，默认按钮是"全部还原后继续"，Esc 的结果是
        // "取消"——两者是分开设置的两件事，不是同一个按钮）。并把用户选择交给
        // 弹框前快照的上下文。模态期间其它工作台修改共享上下文也不会改变提示的目标，
        // 执行后也不会覆盖其它工作台的新上下文。owner 或弹框被删除时返回 false。
        // parentWidget 仅用于定位弹框，可传空。
        struct LeavePlan
        {
            Int3LeaveChoice choice = Int3LeaveChoice::KeepAndContinue;
            ksword::memwb::PatchTarget target;
            ksword::memwb::Scope scope = ksword::memwb::Scope::ProcessVirtual;
            ksword::memwb::Channel channel = ksword::memwb::Channel::UserMode;
            std::vector<std::uint64_t> ids;
            bool allowed() const noexcept { return choice != Int3LeaveChoice::Cancel; }
        };
        // Prepare only records a decision. Apply rejects a changed ledger rather
        // than restoring a patch added after the user approved this plan.
        LeavePlan PrepareLeave(QWidget* parentWidget, Int3LeaveScenario scenario);
        bool ApplyPreparedLeave(const LeavePlan& plan);
        bool RequestLeave(QWidget* parentWidget, Int3LeaveScenario scenario);

    signals:
        // changed：账本内容（待还原 / 孤立列表）可能已经变化，面板据此重建表格。
        void changed();

    private:
        // ContextSnapshot：一次操作的目标、范围与通道；嵌套事件循环不能改变该副本。
        // 还原仍按条目安装通道执行，scope 只记录提示时的完整上下文，不作为还原限制。
        struct ContextSnapshot
        {
            ksword::memwb::PatchTarget target;
            ksword::memwb::Scope scope;
            ksword::memwb::Channel channel;
        };

        // SnapshotContext：复制共享的当前上下文，不修改它。
        ContextSnapshot SnapshotContext() const;

        // RestoreInternal：Restore 与 RestoreAll 共用的单条还原逻辑，不发 changed
        // （由调用方在一次公开操作结束时统一发一次），目标取此次操作的快照。
        Int3RestoreOutcome RestoreInternal(std::uint64_t id, const ContextSnapshot& context);

        // RestoreAllForContext / ApplyLeaveChoiceForContext：执行已提示目标的动作，不覆盖
        // m_currentTarget 等共享上下文；发信号后用 QPointer 检查 owner 是否仍存在。
        std::vector<ksword::memwb::PatchRestoreOutcome> RestoreAllForContext(const ContextSnapshot& context);
        bool ApplyLeaveChoiceForContext(Int3LeaveChoice choice, const ContextSnapshot& context);

        // HasUnrestoredForTarget：退出动作完成后仍按提示的目标判定是否有残留。
        bool HasUnrestoredForTarget(const ksword::memwb::PatchTarget& target) const;

        // IsTarget：按 pid + 创建时间比较指定目标，附加代次不影响已落地补丁的归属。
        static bool IsTarget(const ksword::memwb::PatchEntry& entry, const ksword::memwb::PatchTarget& target);

        // IsCurrentTarget：当前目标查询使用 IsTarget 的同一判据，避免与快照操作的
        // pid / 创建时间匹配规则漂移。
        bool IsCurrentTarget(const ksword::memwb::PatchEntry& entry) const;

        ByteStoreFactory m_factory;                       // 构造 IPatchByteStore 的工厂
        ksword::memwb::Int3PatchLedger m_ledger;           // Core 账本，唯一的真相来源
        ksword::memwb::PatchTarget m_currentTarget;        // 当前目标身份
        ksword::memwb::Scope m_currentScope = ksword::memwb::Scope::ProcessVirtual; // 当前范围
        ksword::memwb::Channel m_currentChannel = ksword::memwb::Channel::UserMode; // 当前通道
        std::unordered_map<std::uint64_t, ksword::memwb::Channel> m_installChannel; // id -> 安装通道
    };
}
