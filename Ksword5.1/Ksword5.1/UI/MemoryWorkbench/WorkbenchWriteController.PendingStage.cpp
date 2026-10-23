// ============================================================
// WorkbenchWriteController.PendingStage.cpp
// 作用：
// - 地址簿"值"列编辑落在当前基线窗口外时的异步暂存票据系统：beginPendingStage/
//   cancelPendingStage/notifyWindowMayCover 三个公开方法，以及它们共用的"尝试一次
//   Stage"/"判定票据结果"两个私有成员方法（声明见 WorkbenchWriteController.h）。
// - 分工边界（任务书 wpJ4 明确要求写清楚）：移动窗口使之覆盖该地址是装配层
//   （MemoryWorkbenchView，订阅 WorkbenchBaselineFeeder::baselineRefreshed 或画布
//   contentChanged 后调用 notifyWindowMayCover）的职责；本类只管票据的创建、
//   2 秒超时、显式取消与"窗口可能已经移动，重试一次"，自己从不触碰
//   WorkbenchBaselineFeeder/WorkbenchPageProvider。
// - 头文件解冻后的变化：TryStagePendingEntry/FinalizePendingStage 原本是
//   Internal.h 里两个接收 controller 指针的 detail:: 自由函数（绕开访问权限去碰
//   "以 this 指针为键的关联表"），现在改为 WorkbenchWriteController 的私有成员
//   方法，直接访问 overlay_/pendingStages_，不再需要绕路，本文件也不再需要
//   #include Internal.h。
// ============================================================

#include "WorkbenchWriteController.h"
#include "WorkbenchWorkerValue.h"
#include "../AsyncOperation.h"

#include <QTimer>

#include <algorithm>

namespace ks::ui
{
    // ------------------------------------------------------------
    // beginPendingStage：创建票据，立即尝试一次（地址恰好已经在当前窗口内时不必
    // 等待窗口移动），再挂一个 2 秒单次计时器兜底。
    // ------------------------------------------------------------
    WorkbenchWriteController::PendingStageTicket WorkbenchWriteController::beginPendingStage(
        std::uint64_t address, const QByteArray& bytes, StageCompletion completion)
    {
        if (bytes.isEmpty())
        {
            // 没有字节：按"地址/字节非法"处理，不创建任何票据（头文件注释原文）。
            return 0;
        }
        const auto length = static_cast<std::uint64_t>(bytes.size());
        if (!ksword::memwb::MemoryDiffOverlay::RangeRepresentable(address, length))
        {
            // 地址 + 长度超过 uint64 上限：同上，不创建票据。
            return 0;
        }

        return beginPendingOverlayValue(address, bytes, std::move(completion), false, false, 0, {});
    }

    WorkbenchWriteController::PendingStageTicket WorkbenchWriteController::beginPendingDiscard(
        std::uint64_t address, std::uint64_t length, bool all, DiscardCompletion completion)
    {
        if (isCommitting() || finishing_ || !overlay_ || !target_) return 0;
        if (!all && (!length || !ksword::memwb::MemoryDiffOverlay::RangeRepresentable(address, length)))
            return 0;
        return beginPendingOverlayValue(address, {}, {}, true, all, length, std::move(completion));
    }

    WorkbenchWriteController::PendingStageTicket WorkbenchWriteController::beginPendingOverlayValue(
        std::uint64_t address, const QByteArray& bytes, StageCompletion completion,
        bool discard, bool all, std::uint64_t length, DiscardCompletion discardCompletion)
    {
        // Capture and projection use exactly the same authority/revision guards
        // as Stage. Discard mutates only a worker-owned overlay value too.
        // 绑定发起时的目标轴；capture 的同步通知可能销毁控制器或目标，先用弱引用守卫。
        const QPointer<WorkbenchWriteController> self(this);
        const QPointer<WorkbenchTarget> target(target_);
        if (!target)
        {
            return 0;
        }
        const TargetCapture captured = target->capture();
        if (!self || !target || target_ != target.data())
        {
            return 0;
        }

        // pendingStageCounter_ 从 1 开始自增，0 永远是无效票据（头文件约定）。
        const PendingStageTicket ticket = ++pendingStageCounter_;

        PendingStageEntry entry;
        entry.ticket = ticket;
        entry.address = address;
        entry.completion = std::move(completion);
        entry.target = target;
        entry.sourceRevision = captured.rev.source;
        entry.bytes = bytes; // Implicitly shared, detached/converted on worker.
        entry.discard = discard; entry.discardAll = all; entry.discardLength = length;
        entry.discardCompletion = std::move(discardCompletion);
        // 计时器 parent=this：本对象销毁时 Qt 会级联删除它，不需要在析构函数里
        // 逐个手动 delete；下面的 lambda 定义在成员函数体内，因此对 this 的私有
        // 成员/信号拥有和本函数一样的访问权限，即使它在之后的事件循环里才被调用。
        entry.timer = new QTimer(this);
        entry.timer->setSingleShot(true);
        QObject::connect(entry.timer, &QTimer::timeout, this,
            [this, ticket]()
            {
                // 2 秒内没有任何一次 Stage 尝试成功：票据超时，不再重试（头文件
                // 注释原文："不再重试"），显式调用 FinalizePendingStage 而不是
                // 让它悬在表里。
                FinalizePendingStage(ticket, false, QStringLiteral("timeout"));
            });

        pendingStages_.emplace(ticket, entry);
        entry.timer->start(2000);

        // 立即尝试一次：地址可能恰好已经落在当前基线窗口内，这样不需要白等
        // 2 秒才生效。
        const PendingStageAttempt attempt = TryStagePendingEntry(entry);
        if (!self)
        {
            return 0;
        }
        if (attempt.resolved)
        {
            if (discard)
            {
                const auto found = pendingStages_.find(ticket);
                if (found != pendingStages_.end()) found->second.discardCompletion = {};
                FinalizePendingStage(ticket, false, attempt.reason);
                return 0; // Rejected before an actual value worker was accepted.
            }
            FinalizePendingStage(ticket, attempt.ok, attempt.reason);
        }
        return ticket;
    }

    // ------------------------------------------------------------
    // cancelPendingStage：显式取消一个尚未解决的票据。票据不存在或已解决时是
    // 空操作（不发信号）；存在时按"有结果（显式取消）"的语义收尾，同样会发一次
    // pendingStageResolved（头文件信号注释列出的三种结果之一）。
    // ------------------------------------------------------------
    void WorkbenchWriteController::cancelPendingStage(PendingStageTicket ticket)
    {
        if (pendingStages_.find(ticket) == pendingStages_.end())
        {
            return; // 不存在或已经被解决过：空操作。
        }
        FinalizePendingStage(ticket, false, QStringLiteral("cancelled"));
    }

    // ------------------------------------------------------------
    // notifyWindowMayCover：装配层通知"基线窗口可能变了，重新试一次"。对每个仍
    // 未解决的票据重试一次 Stage，不要求调用方传任何范围——本类自己对每一张票据
    // 各试一次。
    // ------------------------------------------------------------
    void WorkbenchWriteController::notifyWindowMayCover()
    {
        const QPointer<WorkbenchWriteController> self(this);
        // 先整表拷贝一份快照再逐个尝试：FinalizePendingStage 会修改
        // pendingStages_（erase），在遍历原表的同时直接 erase 会使迭代器失效。
        const std::map<PendingStageTicket, PendingStageEntry> snapshot = pendingStages_;
        for (const auto& item : snapshot)
        {
            const PendingStageTicket ticket = item.first;
            const PendingStageEntry& entry = item.second;
            // 前一张票据的同步结果槽可能取消后面的票据；快照不再代表当前授权。
            if (pendingStages_.find(ticket) == pendingStages_.end())
            {
                continue;
            }
            const PendingStageAttempt attempt = TryStagePendingEntry(entry);
            // capture()/编辑通知可能同步销毁控制器，不能再进入收尾或下一轮。
            if (!self)
            {
                return;
            }
            if (attempt.resolved)
            {
                FinalizePendingStage(ticket, attempt.ok, attempt.reason);
                if (!self)
                {
                    return;
                }
            }
        }
    }

    // ------------------------------------------------------------
    // TryStagePendingEntry / FinalizePendingStage：私有成员方法，声明见
    // WorkbenchWriteController.h，被 beginPendingStage 与 notifyWindowMayCover
    // 共用。
    // ------------------------------------------------------------

    WorkbenchWriteController::PendingStageAttempt WorkbenchWriteController::TryStagePendingEntry(
        const PendingStageEntry& entry)
    {
        PendingStageAttempt attempt;
        // 窗口异步到达时重新核对来源；不把上一个 PID/通道的编辑应用到新会话。
        const QPointer<WorkbenchWriteController> self(this);
        const QPointer<WorkbenchTarget> target(entry.target);
        if (!target || target_ != target.data())
        {
            return PendingStageAttempt{true, false, QStringLiteral("cancelled")};
        }
        const TargetCapture current = target->capture();
        if (!self || !target || target_ != target.data() || current.rev.source != entry.sourceRevision)
        {
            return PendingStageAttempt{true, false, QStringLiteral("cancelled")};
        }
        if (overlay_ == nullptr)
        {
            // 没有叠加层可暂存：不可恢复，立即判定失败（不会因为之后装上 overlay
            // 就自动变好——票据生命周期里 overlay_ 不应该从空变为非空，这是防御）。
            attempt.resolved = true;
            attempt.ok = false;
            attempt.reason = QStringLiteral("no overlay");
            return attempt;
        }
        if (stageJob_)
        {
            if (stageTicket_ == entry.ticket) return attempt;
            return {true, false, QStringLiteral("cancelled")};
        }
        if (isCommitting() || finishing_)
            return {true, false, QStringLiteral("cancelled")};
        // Only perform constant-time bounds checks here. Per-byte validity,
        // payload conversion, merge/split work and all map allocation run on
        // the value copy. A not-yet-loaded window still keeps the 2s ticket.
        const auto length = static_cast<std::uint64_t>(entry.bytes.size());
        if (!entry.discard && (!overlay_->HasBaseline() || entry.address < overlay_->BaseAddress()
            || entry.address - overlay_->BaseAddress() > overlay_->BaselineSize()
            || length > overlay_->BaselineSize() - (entry.address - overlay_->BaseAddress())))
            return attempt;
        auto job = MakeWorkerValue<StageValueJob>(*overlay_, entry.address, entry.bytes);
        job->contentRevision = current.rev.content;
        job->discard = entry.discard; job->discardAll = entry.discardAll;
        job->discardLength = entry.discardLength;
        if (entry.timer) job->waitingBudgetMs = (std::max)(1, entry.timer->remainingTime());
        stageJob_ = job;
        stageTicket_ = entry.ticket;
        terminalPoll_->start();
        if (entry.timer) entry.timer->stop(); // Timeout covers missing baseline, not accepted CPU work.
        const auto readOnly = canvasReadOnlyHook_;
        const auto suspend = commitSuspendHook_;
        if (readOnly) readOnly(true);
        if (!self) return attempt;
        if (suspend) suspend(true);
        if (!self) return attempt;
        try
        {
            const auto ticket = stageAsync_->submit<std::shared_ptr<StageValueJob>, std::shared_ptr<StageValueJob>>(job,
                [](const std::shared_ptr<StageValueJob>& value, const AsyncOperationToken& token)
                {
                    try
                    {
                        if (!token.isCanceled())
                        {
                            if (value->discard)
                            {
                                if (value->discardAll)
                                {
                                    value->discardedBytes = value->prepared.PendingByteCount();
                                    value->prepared.DiscardAll();
                                }
                                else value->discardedBytes = value->prepared.Discard(value->address, value->discardLength);
                                value->status = ksword::memwb::StageStatus::Ok;
                            }
                            else
                            {
                                const auto* first = reinterpret_cast<const std::uint8_t*>(value->bytes.constData());
                                const std::vector<std::uint8_t> payload(first, first + value->bytes.size());
                                value->status = value->prepared.Stage(value->address, payload);
                            }
                        }
                        else value->failed = true;
                    }
                    catch (...) { value->failed = true; }
                    value->completed.store(true, std::memory_order_release);
                    return value;
                },
                [self, ticket = entry.ticket](const std::shared_ptr<StageValueJob>& result, std::uint64_t)
                { if (self) self->consumeStage(ticket, result); return true; },
                [self, ticket = entry.ticket](std::exception_ptr)
                {
                    if (!self) return;
                    self->stageJob_.reset(); self->stageTicket_ = 0;
                    self->releaseStageHooks();
                    if (self) self->FinalizePendingStage(ticket, false, QStringLiteral("cancelled"));
                });
            if (!ticket)
            {
                stageJob_.reset(); stageTicket_ = 0;
                releaseStageHooks();
                if (self) return {true, false, QStringLiteral("cancelled")};
            }
        }
        catch (...)
        {
            if (!self) return attempt;
            stageJob_.reset(); stageTicket_ = 0;
            releaseStageHooks();
            if (self) return {true, false, QStringLiteral("cancelled")};
        }
        return attempt;
    }

    void WorkbenchWriteController::releaseStageHooks()
    {
        const QPointer<WorkbenchWriteController> self(this);
        const auto readOnly = canvasReadOnlyHook_;
        const auto suspend = commitSuspendHook_;
        if (readOnly) readOnly(false);
        if (self && suspend) suspend(false);
    }

    void WorkbenchWriteController::consumeStage(PendingStageTicket ticket,
        const std::shared_ptr<StageValueJob>& job)
    {
        if (stageJob_ != job || stageTicket_ != ticket) return;
        // The apply callback still owns its real AsyncOperation lane. Poll only
        // after it has released that lane before exposing completion/reentry.
        if (stageAsync_ && stageAsync_->isInFlight())
        {
            QTimer::singleShot(1, this, [this, ticket, job] { consumeStage(ticket, job); });
            return;
        }
        const QPointer<WorkbenchWriteController> self(this);
        const auto found = pendingStages_.find(ticket);
        const auto target = found == pendingStages_.end() ? QPointer<WorkbenchTarget>{} : found->second.target;
        const auto source = found == pendingStages_.end() ? 0 : found->second.sourceRevision;
        const auto current = target ? target->capture() : TargetCapture{};
        if (!self) return;
        bool valid = !job->failed && target && target_ == target && current.rev.source == source && current.rev.content == job->contentRevision && overlay_
            && overlay_->SameRevision(job->original) && pendingStages_.find(ticket) != pendingStages_.end();
        bool staged = valid && job->status == ksword::memwb::StageStatus::Ok
            && overlay_->TryAdoptPrepared(job->original, job->prepared);
        if (staged) job->projected = overlay_->CaptureRevision();
        if (!staged && overlay_)
        {
            // Finished rejected/canceled values need no strong reference to the
            // current state when the feeder resumes. Removing that reference
            // cannot destroy the visible state; unrelated prepared states stay
            // worker-owned for their normal background disposal.
            if (job->original.SameRevision(*overlay_)) job->original = ksword::memwb::MemoryDiffOverlay{};
            if (job->prepared.SameRevision(*overlay_)) job->prepared = ksword::memwb::MemoryDiffOverlay{};
        }
        // Keep busy admission through external unlock hooks. They may notify,
        // close or replace a target, but cannot start a new stage/write.
        releaseStageHooks();
        if (!self) return;
        const auto afterHooks = target ? target->capture() : TargetCapture{};
        if (!self) return;
        valid = valid && target && target_ == target && afterHooks.rev.source == source && afterHooks.rev.content == job->contentRevision;
        if (staged) valid = valid && overlay_ && job->projected && overlay_->MatchesRevision(*job->projected);
        staged &= valid;
        stageJob_.reset(); stageTicket_ = 0;
        if (!operation_) terminalPoll_->stop();
        // Resume after the AsyncOperation apply stack has released its busy
        // state; no synchronous callback can cause a false Busy rejection.
        if (staged)
        {
            if (job->discard)
            {
                finishing_ = true;
                const auto clear = [self](WorkbenchWriteController*) { if (self) self->finishing_ = false; };
                std::unique_ptr<WorkbenchWriteController, decltype(clear)> guard(this, clear);
                if (job->discardedBytes && target_) target_->noteContentChanged();
                if (self) emitPending();
            }
            else onEditCompleted();
            if (!self) return;
            const auto afterEdit = target ? target->capture() : TargetCapture{};
            if (!self) return;
            const bool changed = !job->discard || job->discardedBytes != 0;
            const bool stillProjected = target && target_ == target && afterEdit.rev.source == source
                && afterEdit.rev.content == job->contentRevision + (changed ? 1U : 0U)
                && overlay_ && job->projected && overlay_->MatchesRevision(*job->projected);
            FinalizePendingStage(ticket, stillProjected, stillProjected ? QString() : QStringLiteral("cancelled"),
                stillProjected ? job->discardedBytes : 0);
            return;
        }
        if (valid && (job->status == ksword::memwb::StageStatus::OutOfWindow
            || job->status == ksword::memwb::StageStatus::UnreadBytes))
        {
            const auto it = pendingStages_.find(ticket);
            if (it != pendingStages_.end() && it->second.timer) it->second.timer->start(job->waitingBudgetMs);
            return;
        }
        FinalizePendingStage(ticket, false, valid
            ? QStringLiteral("stage rejected (%1)").arg(static_cast<int>(job->status))
            : QStringLiteral("cancelled"));
    }

    void WorkbenchWriteController::FinalizePendingStage(
        PendingStageTicket ticket, bool ok, const QString& reason, std::uint64_t discarded)
    {
        auto it = pendingStages_.find(ticket);
        if (it == pendingStages_.end())
        {
            // 票据不存在（例如超时计时器与显式取消几乎同时触发，先到的一方已经
            // 清理过）：不重复发信号，也不重复停计时器。
            return;
        }
        if (it->second.timer != nullptr)
        {
            it->second.timer->stop();
            // 立刻从 controller 的子对象列表摘除：这样即使 deleteLater() 的事件
            // 还没被处理，之后任何基于 findChildren/children() 的枚举（本类目前
            // 不这么做，纯防御）也不会再看到这个即将销毁的计时器。
            it->second.timer->setParent(nullptr);
            it->second.timer->deleteLater();
        }
        // 先把 it 失效化前需要的数据都用完，再 erase（erase 之后 it 不可再用）。
        const auto completion = std::move(it->second.completion);
        const auto discardCompletion = std::move(it->second.discardCompletion);
        const bool discard = it->second.discard;
        pendingStages_.erase(it);
        const QPointer<WorkbenchWriteController> self(this);
        if (completion) completion(ok, reason);
        if (!self) return;
        if (discardCompletion) discardCompletion(ok, discarded);
        if (!self || discard) return;
        // 发出结果信号：成功提交 / 超时 / 显式取消三种情形都从这里统一发出
        // （头文件信号注释列出的三种结果）。信号是 moc 生成的公开成员函数，这里
        // 直接调用等价于 emit。
        pendingStageResolved(ticket, ok, reason);
    }
}
