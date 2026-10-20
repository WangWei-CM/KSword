// Int3Controller.cpp
// 作用：Int3Controller.h 的实现。设计动机、各枚举含义见该头文件顶部注释。
//
// 文件结构：构造与上下文设置 -> 路由预检 -> Install -> Restore 系列（内部+公开+全部）->
// OnTargetGone/Discard/ClearOrphaned/查询 -> 退出前提示（纯逻辑 + QMessageBox 封装）。

#include "Int3Controller.h"

#include <QAbstractButton>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QWidget>

namespace ks::ui
{
    using ksword::memwb::Channel;
    using ksword::memwb::InstallResult;
    using ksword::memwb::InstallStatus;
    using ksword::memwb::IPatchByteStore;
    using ksword::memwb::PatchEntry;
    using ksword::memwb::PatchRestoreOutcome;
    using ksword::memwb::PatchTarget;
    using ksword::memwb::RestoreResult;
    using ksword::memwb::RestoreStatus;
    using ksword::memwb::Scope;

    // 构造：只保存工厂，账本用默认构造（空）。
    Int3Controller::Int3Controller(ByteStoreFactory factory, QObject* parent)
        : QObject(parent)
        , m_factory(std::move(factory))
    {
    }

    // SetCurrentContext：更新当前目标三元组。也发一次 changed——目标变化会影响面板里
    // "这一行是否属于当前目标"的展示，即使账本条目本身没有增删，面板也需要重绘。
    void Int3Controller::SetCurrentContext(
        const PatchTarget& target,
        const Scope scope,
        const Channel channel)
    {
        m_currentTarget = target;
        m_currentScope = scope;
        m_currentChannel = channel;
        emit changed();
    }

    // EvaluateCurrentRoute：只比较两个字段，不做任何 I/O。
    // 顺序固定：先查范围，再查通道——范围不对时通道检查已经没有意义，但即使都不对，
    // 调用方只需要知道第一个被发现的原因。
    Int3RouteReject Int3Controller::EvaluateCurrentRoute() const
    {
        if (m_currentScope != Scope::ProcessVirtual)
        {
            return Int3RouteReject::UnsupportedScope;
        }
        if (m_currentChannel == Channel::Ddma)
        {
            return Int3RouteReject::UnsupportedChannel;
        }
        return Int3RouteReject::None;
    }

    // Install：先路由预检（零 I/O 拒绝），再向工厂要一个绑定当前通道的存取器，最后交给账本。
    Int3InstallOutcome Int3Controller::Install(
        const PatchTarget& target,
        const std::uint64_t address,
        const std::uint64_t nowTick)
    {
        Int3InstallOutcome outcome;

        // 第一步：路由预检。被拒绝时不调用工厂、不碰账本，调用次数恒为 0。
        const Int3RouteReject reject = EvaluateCurrentRoute();
        if (reject != Int3RouteReject::None)
        {
            outcome.routeReject = reject;
            emit changed();
            return outcome;
        }

        // 第二步：向工厂要存取器，绑定"当前通道"（这就是将被记入旁路表的安装通道）。
        const std::unique_ptr<IPatchByteStore> store = m_factory(target, m_currentChannel);
        if (!store)
        {
            // 工厂自己拒绝构造（例如目标此刻已不可达）：对调用方而言与写入失败没有区别，
            // 不消耗账本 id，也不记安装通道。
            outcome.status = InstallStatus::WriteFailed;
            emit changed();
            return outcome;
        }

        // 第三步：真正调用账本。检查顺序、各拒绝状态的含义见 Int3PatchLedger.h。
        const InstallResult result = m_ledger.Install(target, address, *store, nowTick);
        outcome.status = result.status;
        outcome.id = result.id;
        outcome.rollbackAttempted = result.rollbackAttempted;
        outcome.rollbackWriteOk = result.rollbackWriteOk;

        // 安装成功和验证失败的恢复条目都须记住通道；之后的还原必须使用它。
        if (result.id != 0)
        {
            m_installChannel[result.id] = m_currentChannel;
        }

        emit changed();
        return outcome;
    }

    // RestoreInternal：Restore 与 RestoreAll 共用，不发 changed（留给公开入口统一发）。
    Int3RestoreOutcome Int3Controller::RestoreInternal(
        const std::uint64_t id, const ContextSnapshot& context)
    {
        Int3RestoreOutcome outcome;

        // 查旁路表取"安装时的通道"；查不到时用当前通道兜底——这种情况下 id 在账本里也必然
        // 查不到（旁路表与账本的"待还原集合"同步增删），账本会直接给 NotFound，不会真的发起
        // 端口调用，所以这里的兜底值选什么都不影响正确性。
        const auto channelIt = m_installChannel.find(id);
        const Channel channel = (channelIt != m_installChannel.end()) ? channelIt->second : context.channel;

        const std::unique_ptr<IPatchByteStore> store = m_factory(context.target, channel);
        if (!store)
        {
            outcome.status = RestoreStatus::WriteFailed;
            return outcome;
        }

        const RestoreResult result = m_ledger.Restore(id, context.target, *store);
        outcome.status = result.status;
        outcome.hasObservedByte = result.hasObservedByte;
        outcome.observedByte = result.observedByte;

        // 真正还原成功才能把旁路表里这一条清掉；其余情形（包括 Diverged）条目仍留在账本里，
        // 旁路表也必须保留，否则下一次重试会用错通道。
        if (result.status == RestoreStatus::Restored)
        {
            m_installChannel.erase(id);
        }
        return outcome;
    }

    // Restore：公开入口，补一次 changed。
    Int3RestoreOutcome Int3Controller::Restore(const std::uint64_t id)
    {
        const Int3RestoreOutcome outcome = RestoreInternal(id, SnapshotContext());
        emit changed();
        return outcome;
    }

    // RestoreAll：先快照属于当前目标的 (id, address)，再逐条调用 RestoreInternal。
    // 必须先快照：RestoreInternal 成功会从账本删条目，一边遍历一边改会错位。
    std::vector<PatchRestoreOutcome> Int3Controller::RestoreAll()
    {
        return RestoreAllForContext(SnapshotContext());
    }

    // RestoreAllForContext：过滤与逐条还原都使用同一副本，不借用可变的共享上下文。
    std::vector<PatchRestoreOutcome> Int3Controller::RestoreAllForContext(const ContextSnapshot& context)
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> snapshot;
        for (const PatchEntry& entry : m_ledger.Entries())
        {
            if (IsTarget(entry, context.target))
            {
                snapshot.emplace_back(entry.id, entry.address);
            }
        }

        std::vector<PatchRestoreOutcome> outcomes;
        outcomes.reserve(snapshot.size());
        for (const auto& idAndAddress : snapshot)
        {
            const Int3RestoreOutcome single = RestoreInternal(idAndAddress.first, context);
            PatchRestoreOutcome outcome;
            outcome.id = idAndAddress.first;
            outcome.address = idAndAddress.second;
            outcome.result.status = single.status;
            outcome.result.hasObservedByte = single.hasObservedByte;
            outcome.result.observedByte = single.observedByte;
            outcomes.push_back(outcome);
        }

        emit changed();
        return outcomes;
    }

    // OnTargetGone：转发给账本，并同步清理这些条目在旁路表里的安装通道记录——一旦条目被
    // 孤立，账本对它的 Restore 永远直接返回 Orphaned、不会发起任何 I/O，旁路表里那一条也
    // 就永远不会再被用到（ClearOrphaned 里也有一遍同样的清理兜底，防止今后出现别的能产生
    // 孤立条目的路径；两处都清是为了不依赖调用顺序）。不清理的后果：m_installChannel 随
    // "安装→目标退出→清除"的循环次数无界增长，是一个慢速内存泄漏（2026 年审核报告缺陷 3）。
    std::vector<PatchEntry> Int3Controller::OnTargetGone(
        const std::uint32_t pid,
        const std::uint64_t processCreateTime100ns)
    {
        const std::vector<PatchEntry> orphaned = m_ledger.OnTargetGone(pid, processCreateTime100ns);
        for (const PatchEntry& entry : orphaned)
        {
            m_installChannel.erase(entry.id);
        }
        emit changed();
        return orphaned;
    }

    // Discard：转发账本，成功时顺手清掉旁路表里的那一条（这个 id 永远不会再被还原）。
    bool Int3Controller::Discard(const std::uint64_t id)
    {
        const bool ok = m_ledger.Discard(id);
        if (ok)
        {
            m_installChannel.erase(id);
        }
        emit changed();
        return ok;
    }

    // ClearOrphaned：转发账本之前先把孤立列表里每个 id 的旁路表记录擦掉——必须在
    // m_ledger.ClearOrphaned() 之前读 OrphanedEntries()，清空之后就什么都查不到了。
    // 正常情况下 OnTargetGone 已经清过一遍，这里是兜底（见 OnTargetGone 的注释）。
    void Int3Controller::ClearOrphaned()
    {
        for (const PatchEntry& entry : m_ledger.OrphanedEntries())
        {
            m_installChannel.erase(entry.id);
        }
        m_ledger.ClearOrphaned();
        emit changed();
    }

    // InstalledChannel：旁路表的只读查询。
    std::optional<Channel> Int3Controller::InstalledChannel(const std::uint64_t id) const
    {
        const auto it = m_installChannel.find(id);
        if (it == m_installChannel.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    // SnapshotContext：操作期间持有目标、范围和通道的值副本。
    Int3Controller::ContextSnapshot Int3Controller::SnapshotContext() const
    {
        return {m_currentTarget, m_currentScope, m_currentChannel};
    }

    // IsTarget：判据见头文件注释，当前目标查询和已快照操作共用。
    bool Int3Controller::IsTarget(const PatchEntry& entry, const PatchTarget& target)
    {
        return entry.pid == target.pid &&
               entry.processCreateTime100ns == target.processCreateTime100ns;
    }

    bool Int3Controller::IsCurrentTarget(const PatchEntry& entry) const
    {
        return IsTarget(entry, m_currentTarget);
    }

    // HasUnrestoredForCurrentTarget：线性扫描——账本条目数量是人手点出来的量级。
    bool Int3Controller::HasUnrestoredForCurrentTarget() const
    {
        return HasUnrestoredForTarget(m_currentTarget);
    }

    bool Int3Controller::HasUnrestoredForTarget(const PatchTarget& target) const
    {
        for (const PatchEntry& entry : m_ledger.Entries())
        {
            if (IsTarget(entry, target))
            {
                return true;
            }
        }
        return false;
    }

    // CountForCurrentTarget：与 HasUnrestoredForCurrentTarget 同一条判据，供退出提示的
    // 文案"还有 %1 处"取数；两者口径必须一致，否则会出现"弹了框但数字是别的目标的"。
    int Int3Controller::CountForCurrentTarget() const
    {
        int count = 0;
        for (const PatchEntry& entry : m_ledger.Entries())
        {
            if (IsCurrentTarget(entry))
            {
                ++count;
            }
        }
        return count;
    }

    // NeedsLeavePrompt：当前三种场景共用同一条判断。静态函数，不碰任何实例状态，
    // 方便测试直接调用而不需要先构造一个 Int3Controller。
    bool Int3Controller::NeedsLeavePrompt(const bool hasUnrestored, const Int3LeaveScenario /*scenario*/)
    {
        return hasUnrestored;
    }

    // ApplyLeaveChoice：三个分支各自的后果见头文件注释。
    bool Int3Controller::ApplyLeaveChoice(const Int3LeaveChoice choice)
    {
        return ApplyLeaveChoiceForContext(choice, SnapshotContext());
    }

    // ApplyLeaveChoiceForContext：信号回调可切换上下文或删除 owner，判据不能依赖当前值。
    bool Int3Controller::ApplyLeaveChoiceForContext(
        const Int3LeaveChoice choice, const ContextSnapshot& context)
    {
        const QPointer<Int3Controller> self(this);
        switch (choice)
        {
        case Int3LeaveChoice::RestoreAllThenContinue:
            RestoreAllForContext(context);
            return self && !self->HasUnrestoredForTarget(context.target);
        case Int3LeaveChoice::KeepAndContinue:
            return true;
        case Int3LeaveChoice::Cancel:
            return false;
        }
        // switch 覆盖了全部枚举值；留一个兜底防御性返回，避免编译器在某些配置下警告
        // "并非所有路径都返回值"。
        return false;
    }

    // RequestLeave：判据与文案都只看"当前目标"——没有待还原补丁（当前目标口径）时不弹
    // 任何框，直接放行；其它已离开目标遗留的条目既不拦这次退出，也不计入这次的"还有 %1
    // 处"（2026 年审核报告缺陷 1）。默认按钮是"全部还原后继续"（规格要求），Esc 的结果是
    // "取消"（2026 年审核报告缺陷 2：曾经两者都是"取消"，与规格相反）。
    Int3Controller::LeavePlan Int3Controller::PrepareLeave(QWidget* parentWidget, const Int3LeaveScenario scenario)
    {
        const ContextSnapshot context = SnapshotContext();
        LeavePlan plan;
        plan.target = context.target;
        plan.scope = context.scope;
        plan.channel = context.channel;
        for (const auto& entry : m_ledger.Entries())
            if (IsTarget(entry, context.target)) plan.ids.push_back(entry.id);
        const bool hasUnrestoredForTarget = HasUnrestoredForTarget(context.target);
        if (!NeedsLeavePrompt(hasUnrestoredForTarget, scenario))
        {
            return plan;
        }

        const int patchCount = CountForCurrentTarget();
        // 对话框由 parentWidget 管理；若模态期间父窗口被销毁，QPointer 会清空，
        // 避免栈上的 QObject 子对象被父窗口删除后再次析构。owner 也必须独立守卫。
        const QPointer<Int3Controller> self(this);
        QPointer<QMessageBox> box = new QMessageBox(parentWidget);
        box->setIcon(QMessageBox::Warning);
        box->setWindowTitle(QStringLiteral("退出确认"));
        box->setText(QStringLiteral(
            "目标进程中还有 %1 处 int3 补丁（0xCC）未还原；不还原就退出，补丁会一直留在目标里。")
            .arg(patchCount));

        QPushButton* restoreAllButton = box->addButton(QStringLiteral("全部还原后继续"), QMessageBox::AcceptRole);
        QPushButton* keepButton = box->addButton(QStringLiteral("保留补丁继续"), QMessageBox::DestructiveRole);
        QPushButton* cancelButton = box->addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        // 默认按钮=全部还原后继续（规格：docs/内存工作台Phase3集成设计.md 第 0 节第 6 条）；
        // Esc 的结果单独设成取消——这是安全默认，设计文档没有要求连带改掉它。
        box->setDefaultButton(restoreAllButton);
        box->setEscapeButton(cancelButton);

        box->exec();

        if (!self || !box)
        {
            delete box.data();
            plan.choice = Int3LeaveChoice::Cancel;
            return plan;
        }

        const QAbstractButton* clicked = box->clickedButton();
        Int3LeaveChoice choice = Int3LeaveChoice::Cancel;
        if (clicked == static_cast<QAbstractButton*>(restoreAllButton))
        {
            choice = Int3LeaveChoice::RestoreAllThenContinue;
        }
        else if (clicked == static_cast<QAbstractButton*>(keepButton))
        {
            choice = Int3LeaveChoice::KeepAndContinue;
        }
        // 其余情形（包括 Esc 关闭、点了取消）都落在默认值 Cancel。

        delete box.data();
        plan.choice = choice;
        return plan;
    }
    bool Int3Controller::ApplyPreparedLeave(const LeavePlan& plan)
    {
        std::vector<std::uint64_t> currentIds;
        for (const auto& entry : m_ledger.Entries())
            if (IsTarget(entry, plan.target)) currentIds.push_back(entry.id);
        if (currentIds != plan.ids) return false;
        return ApplyLeaveChoiceForContext(plan.choice, {plan.target, plan.scope, plan.channel});
    }

    bool Int3Controller::RequestLeave(QWidget* parentWidget, const Int3LeaveScenario scenario)
    {
        const QPointer<Int3Controller> self(this);
        const auto plan = PrepareLeave(parentWidget, scenario);
        return self && ApplyPreparedLeave(plan);
    }

}
