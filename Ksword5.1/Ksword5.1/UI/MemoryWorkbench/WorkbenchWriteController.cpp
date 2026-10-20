#include "WorkbenchWriteController.h"
#include "../AsyncOperation.h"

#include <QDebug>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <utility>

namespace ks::ui
{
    namespace mem = ksword::memwb;
    namespace
    {
        std::uint64_t Tick(const WorkbenchWriteController::TickProviderFn& provider)
        {
            if (provider) return provider();
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }
        bool Succeeded(const mem::CommitReport& report)
        {
            return report.outcome == mem::CommitOutcome::Committed
                || report.outcome == mem::CommitOutcome::NoChange;
        }
        std::string Diagnostic(const WorkbenchWriteController::DiagnosticFormatter& formatter,
            const std::string& canonical)
        {
            if (!formatter) return canonical;
            try { return formatter(canonical); }
            catch (...) { return canonical; }
        }
    }

    WorkbenchWriteController::WorkbenchWriteController(QObject* parent) : QObject(parent)
    {
        async_ = new AsyncOperation(this);
        terminalPoll_ = new QTimer(this);
        terminalPoll_->setInterval(20);
        connect(terminalPoll_, &QTimer::timeout, this, [this]
        {
            // Shutdown or owner-side submission failure can finish through the
            // runner's independent cleanup path. This query never waits on I/O.
            const auto operation = operation_;
            if (!operation) return;
            try
            {
                if (const auto terminal = operation->TryTerminalSnapshot())
                    consumePhase(operation, *terminal);
            }
            catch (...) { qWarning() << "workbench terminal receipt projection allocation failed"; }
        });
    }

    WorkbenchWriteController::~WorkbenchWriteController()
    {
        // No GUI join and no raw-this worker cleanup. A paused confirmation is
        // finished by the runner too; its registry lease covers that interval.
        if (async_) async_->close();
        if (authorization_) authorization_->ownerAlive.store(false, std::memory_order_release);
        if (operation_) operation_->RequestStop(true);
    }

    void WorkbenchWriteController::setOverlay(mem::MemoryDiffOverlay* value)
    {
        if (dependenciesLocked_ && overlay_ != value) { qWarning() << "write overlay late binding rejected"; return; }
        overlay_ = value;
    }
    void WorkbenchWriteController::setTarget(WorkbenchTarget* value)
    {
        if (dependenciesLocked_ && target_ != value) { qWarning() << "write target late binding rejected"; return; }
        if (target_ == value) return;
        if (target_) disconnect(target_, nullptr, this, nullptr);
        target_ = value;
        if (!target_) return;
        connect(target_, &WorkbenchTarget::writeAuthorityChanged, this,
            &WorkbenchWriteController::invalidateWriteAuthorization);
        connect(target_, &WorkbenchTarget::sessionChanged, this, [this](quint32)
        {
            invalidateWriteAuthorization();
            const QPointer<WorkbenchWriteController> self(this);
            const auto target = target_;
            if (!target) return;
            const auto captured = target->capture();
            if (!self || !target || target_ != target) return;
            if (ledger_ && !mem::SameTarget(ledger_->Session(), captured.session))
            {
                // The process registry keeps the old ledger and receipts.
                ledger_.reset();
                emit undoRedoAvailabilityChanged();
            }
        });
        connect(target_, &QObject::destroyed, this, [this]
        {
            if (authorization_) authorization_->ownerAlive.store(false, std::memory_order_release);
            if (operation_) operation_->RequestStop(true);
        });
    }
    void WorkbenchWriteController::setConfirmationSink(mem::IConfirmationSink* value)
    {
        if (dependenciesLocked_ && confirmation_ != value) { qWarning() << "write confirmation late binding rejected"; return; }
        confirmation_ = value;
    }
    void WorkbenchWriteController::setAuditSink(mem::IAuditSink* value)
    {
        if (dependenciesLocked_ && legacyAudit_ != value) { qWarning() << "write audit late binding rejected"; return; }
        legacyAudit_ = value;
    }
    void WorkbenchWriteController::setIoPortFactory(IoPortFactory value)
    { if (!dependenciesLocked_) factories_.io = std::move(value); }
    void WorkbenchWriteController::setKernelMutationPortFactory(KernelPortFactory value)
    { if (!dependenciesLocked_) factories_.kernel = std::move(value); }
    void WorkbenchWriteController::setWriteValidationCallback(WriteValidationFn)
    {
        // Kept source-compatible for old fixtures. A callback borrowing live GUI
        // definitions cannot be made safe by moving the ByteStore to a worker.
        qWarning() << "GUI write validation requires frozen capture assembly";
    }
    void WorkbenchWriteController::setFrozenWriteFactories(FrozenWriteFactories value)
    { if (!dependenciesLocked_) factories_ = std::move(value); }
    void WorkbenchWriteController::setOwnedAuditArchive(std::shared_ptr<mem::IAuditSink> value)
    { if (!dependenciesLocked_) archive_ = std::move(value); }
    void WorkbenchWriteController::setFrozenWriteCaptureCallback(FrozenCaptureFn value)
    { if (!dependenciesLocked_) capture_ = std::move(value); }
    void WorkbenchWriteController::setFrozenPointerBindingsCallback(FrozenBindingsFn value)
    { applyBindings_ = std::move(value); }
    void WorkbenchWriteController::setConfirmationReadyCallback(std::function<bool()> value)
    { confirmationReady_ = std::move(value); }
    void WorkbenchWriteController::setDiagnosticFormatter(DiagnosticFormatter value)
    { diagnosticFormatter_ = std::move(value); }
    void WorkbenchWriteController::invalidateWriteAuthorization()
    { ++authorityRevision_; if (authorization_) authorization_->Invalidate(); }
    void WorkbenchWriteController::setRereadRangeCallback(RereadRangeFn value)
    { rereadRangeCallback_ = std::move(value); }
    void WorkbenchWriteController::setTickProvider(TickProviderFn value)
    { tickProvider_ = std::move(value); }
    void WorkbenchWriteController::setCanvasReadOnlyHook(CanvasReadOnlyHook value)
    { canvasReadOnlyHook_ = std::move(value); }
    void WorkbenchWriteController::setCommitSuspendHook(CommitSuspendHook value)
    { commitSuspendHook_ = std::move(value); }

    mem::WriteMode WorkbenchWriteController::mode() const { return mode_; }
    void WorkbenchWriteController::setUiConfirmSuppressed(bool value)
    { pendingUiConfirmSuppressed_ = value; }
    bool WorkbenchWriteController::uiConfirmSuppressed() const { return pendingUiConfirmSuppressed_; }
    bool WorkbenchWriteController::isCommitting() const { return accepting_ || operation_ != nullptr || (async_ && async_->isInFlight()); }

    mem::ModeSwitchStatus WorkbenchWriteController::requestModeSwitch(mem::WriteMode desired)
    {
        if (isCommitting() || finishing_) return mem::ModeSwitchStatus::Busy;
        if (desired == mode_ || !overlay_ || !overlay_->HasPendingPatches())
        {
            mode_ = desired;
            pendingMode_.reset();
            return mem::ModeSwitchStatus::Switched;
        }
        pendingMode_ = desired;
        return mem::ModeSwitchStatus::NeedsDecision;
    }

    CommitEntryStatus WorkbenchWriteController::resolveModeSwitch(
        mem::ModeSwitchDecision decision, ModeSwitchCompletion completion)
    {
        if (isCommitting() || finishing_)
        {
            emit commitRejectedBusy(QStringLiteral("resolveModeSwitch"));
            return CommitEntryStatus::Busy;
        }
        if (!pendingMode_) return CommitEntryStatus::Rejected;
        const auto desired = *pendingMode_;
        pendingMode_.reset();
        if (decision == mem::ModeSwitchDecision::DiscardThenSwitch)
        {
            const QPointer<WorkbenchWriteController> self(this);
            const bool changed = overlay_ && overlay_->HasPendingPatches();
            if (overlay_) overlay_->DiscardAll();
            mode_ = desired;
            if (changed && target_) target_->noteContentChanged();
            if (self) emitPending();
            return CommitEntryStatus::Started;
        }
        if (decision != mem::ModeSwitchDecision::ApplyThenSwitch) return CommitEntryStatus::Rejected;
        const QPointer<WorkbenchWriteController> self(this);
        return beginCommit([self, desired, completion = std::move(completion)](const mem::CommitReport& report)
        {
            if (!self) return;
            mem::ModeSwitchResult result;
            result.commitReport = report;
            if (Succeeded(report) && self->lastProjectionMatched_
                && self->overlay_ && !self->overlay_->HasPendingPatches())
            {
                self->mode_ = desired;
                result.status = mem::ModeSwitchStatus::Switched;
            }
            else result.status = mem::ModeSwitchStatus::ApplyFailed;
            if (completion) completion(result);
        }).status;
    }

    void WorkbenchWriteController::onEditCompleted()
    {
        const QPointer<WorkbenchWriteController> self(this);
        if (!target_) return;
        // The edit already exists, even when a busy operation rejects it. Its
        // authority revision must invalidate that operation before any next write.
        target_->noteContentChanged();
        if (!self) return;
        if (isCommitting() || finishing_)
        {
            emit commitRejectedBusy(QStringLiteral("onEditCompleted"));
            if (self) emitPending();
            return;
        }
        if (mode_ == mem::WriteMode::Immediate) (void)beginCommit({});
        else emitPending();
    }

    CommitAttempt WorkbenchWriteController::commitPendingNow(CommitCompletion completion)
    { return beginCommit(std::move(completion)); }

    bool WorkbenchWriteController::updateLedger(const mem::MemoryTargetSession& session)
    {
        if (ledger_ && mem::SameTarget(ledger_->Session(), session)) return true;
        if (!archive_) return false;
        ledger_ = WorkbenchWriteLedger::CreatePersistent(session, archive_);
        return static_cast<bool>(ledger_);
    }

    CommitAttempt WorkbenchWriteController::beginCommit(CommitCompletion completion,
        std::optional<WorkbenchWriteLedger::ReplayTicket> replayTicket)
    {
        if (isCommitting() || finishing_)
        {
            emit commitRejectedBusy(QStringLiteral("commitPendingNow"));
            return {CommitEntryStatus::Busy, {}, 0};
        }
        if (!overlay_ || !target_ || !confirmation_ || !archive_ || !capture_ || !factories_.io)
            return {CommitEntryStatus::Rejected, {}, 0};
        const QPointer<WorkbenchWriteController> self(this);
        const auto target = target_;
        // Admission is raised before capture callbacks can notify/reenter.
        accepting_ = true;
        try
        {
            const TargetCapture captured = target->capture();
            if (!self) return {CommitEntryStatus::Rejected, {}, 0};
            if (!target || target_ != target) { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
            const auto authorityRevision = authorityRevision_;
            auto writeLease = TryAcquireWorkbenchWriteLease();
            if (!writeLease)
            {
                accepting_ = false;
                emit commitRejectedBusy(QStringLiteral("workbenchWriteLease"));
                return {CommitEntryStatus::Busy, {}, 0};
            }
            mem::FrozenWriteInput input;
            input.session = captured.session;
            input.revisions = captured.rev;
            input.baseAddress = overlay_->BaseAddress();
            input.baselineSize = overlay_->BaselineSize();
            input.blocks = overlay_->DiffBlocks();
            input.uiConfirmSuppressed = pendingUiConfirmSuppressed_;
            const std::string overlayIdentity = overlay_->IdentityKey();
            FrozenWriteCapture frozen;
            std::string reason;
            const auto capture = capture_;
            if (!capture(frozen, reason))
            {
                if (self) accepting_ = false;
                return {CommitEntryStatus::Rejected, {}, 0};
            }
            if (!self) return {CommitEntryStatus::Rejected, {}, 0};
            if (!target || target_ != target) { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
            const auto afterCapture = target->capture();
            if (!self) return {CommitEntryStatus::Rejected, {}, 0};
            if (!target || target_ != target || captured.rev != afterCapture.rev
                || !mem::SameTarget(captured.session, afterCapture.session)
                || overlay_->IdentityKey() != overlayIdentity || overlay_->DiffBlocks() != input.blocks)
            { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
            if (!updateLedger(captured.session) || (replayTicket && replayTicket->ledger != ledger_->Id()))
            { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
            if (replayTicket)
            {
                const auto& replay = replayTicket->replay;
                mem::MemoryDiffOverlay scratch;
                if (scratch.LoadBaseline(mem::IdentityKey(captured.session, 0, 0), replay.address,
                        replay.expectedCurrent, std::vector<std::uint8_t>(replay.expectedCurrent.size(), 1))
                    != mem::BaselineLoadStatus::Ok
                    || scratch.Stage(replay.address, replay.restore) != mem::StageStatus::Ok)
                { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
                input.baseAddress = scratch.BaseAddress();
                input.baselineSize = scratch.BaselineSize();
                input.blocks = scratch.DiffBlocks();
            }
            frozen.session = captured.session;
            frozen.historyReplay = replayTicket.has_value();
            frozen.writeLease = std::move(writeLease);
            auto authorization = std::make_shared<FrozenWriteAuthorization>();
            frozen.authorization = authorization;
            frozen.authorizationEpoch = authorization->epoch.load(std::memory_order_acquire);
            const auto factories = factories_;
            const auto ledger = ledger_;
            const auto tickProvider = tickProvider_;
            const std::optional<std::uint64_t> tick = tickProvider
                ? std::optional<std::uint64_t>{Tick(tickProvider)} : std::nullopt;
            if (!self) return {CommitEntryStatus::Rejected, {}, 0};
            if (!target || target_ != target || authorityRevision_ != authorityRevision)
            { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
            const auto beforeAccept = target->capture();
            if (!self) return {CommitEntryStatus::Rejected, {}, 0};
            if (!target || target_ != target || beforeAccept.rev != captured.rev
                || !mem::SameTarget(beforeAccept.session, captured.session)
                || authorityRevision_ != authorityRevision || overlay_->IdentityKey() != overlayIdentity
                || (!replayTicket && overlay_->DiffBlocks() != input.blocks))
            { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
            auto operation = WorkbenchWriteOperationRunner::Create(std::move(input), std::move(frozen),
                factories, ledger, tick, replayTicket, reason);
            if (!self) { if (operation) operation->RequestStop(true); return {CommitEntryStatus::Rejected, {}, 0}; }
            if (!operation) { accepting_ = false; return {CommitEntryStatus::Rejected, {}, 0}; }
            authorization_ = std::move(authorization);
            operation_ = operation;
            completion_ = std::move(completion);
            historyReplay_ = replayTicket.has_value();
            dependenciesLocked_ = true;
            accepting_ = false;
            lastProjectionMatched_ = false;
            const auto id = operation->Id();
            terminalPoll_->start();
            const auto readOnly = canvasReadOnlyHook_;
            const auto suspend = commitSuspendHook_;
            if (readOnly) readOnly(true);
            if (!self) return {CommitEntryStatus::Pending, {}, id};
            if (suspend) suspend(true);
            if (!self) return {CommitEntryStatus::Pending, {}, id};
            dispatchPhase(operation);
            return {CommitEntryStatus::Pending, {}, id};
        }
        catch (...)
        {
            if (!self) return {CommitEntryStatus::Rejected, {}, 0};
            accepting_ = false;
            if (operation_)
            {
                operation_->RequestStop();
                return {CommitEntryStatus::Pending, {}, operation_->Id()};
            }
            qWarning() << "workbench write admission failed before acceptance";
            return {CommitEntryStatus::Rejected, {}, 0};
        }
    }

    void WorkbenchWriteController::dispatchPhase(
        const std::shared_ptr<WorkbenchWriteOperationRunner>& operation, WritePhaseAnswer answer)
    {
        if (operation_ != operation) { operation->RequestStop(true); return; }
        const QPointer<WorkbenchWriteController> self(this);
        struct Request { std::shared_ptr<WorkbenchWriteOperationRunner> operation; WritePhaseAnswer answer; };
        try
        {
            const auto ticket = async_->submitTransaction<Request, WritePhaseSnapshot>(Request{operation, answer},
                [](const Request& request) { return request.operation->RunPhase(request.answer); },
                [self, operation](const WritePhaseSnapshot& phase, std::uint64_t)
                {
                    if (self) self->consumePhase(operation, phase);
                    return true;
                },
                [self, operation](std::exception_ptr)
                {
                    operation->RequestStop();
                    if (self) self->terminalPoll_->start();
                },
                [operation](AsyncOperationOutcome outcome)
                {
                    if (outcome != AsyncOperationOutcome::Applied) operation->RequestStop();
                });
            if (ticket == 0) operation->RequestStop();
        }
        catch (...) { operation->RequestStop(); }
    }

    void WorkbenchWriteController::consumePhase(
        const std::shared_ptr<WorkbenchWriteOperationRunner>& operation, const WritePhaseSnapshot& phase)
    {
        if (operation_ != operation || phase.operation != operation->Id()) return;
        if (phase.terminal) { finishOperation(operation, phase); return; }
        pendingPhase_ = phase;
        // AsyncOperation still owns this apply stack; resume after it releases
        // its per-phase busy admission. No nested event-loop wait is involved.
        QTimer::singleShot(0, this, [this] { resumePendingConfirmation(); });
    }

    void WorkbenchWriteController::resumePendingConfirmation()
    {
        if (prompting_ || !operation_ || !pendingPhase_ || !confirmation_) return;
        const QPointer<WorkbenchWriteController> self(this);
        const auto ready = confirmationReady_;
        try { if (ready && !ready()) return; }
        catch (...) { if (self && operation_) operation_->RequestStop(); return; }
        if (!self || !operation_ || !pendingPhase_) return;
        const auto operation = operation_;
        const auto phase = *pendingPhase_;
        if (phase.operation != operation->Id()) return;
        if (authorization_ && !authorization_->Allows(operation->Capture().authorizationEpoch))
        {
            pendingPhase_.reset();
            dispatchPhase(operation); // Recheck freshness; do not invent an answer.
            return;
        }
        prompting_ = true;
        pendingPhase_.reset();
        WritePhaseAnswer answer;
        answer.operation = phase.operation;
        answer.serial = phase.serial;
        try
        {
            if (phase.step == mem::MemoryWriteOperation::Step::NeedUi)
            {
                answer.kind = WritePhaseAnswer::Kind::Ui;
                answer.uiAccepted = confirmation_->ConfirmUi(phase.ui);
            }
            else if (phase.step == mem::MemoryWriteOperation::Step::NeedApproval)
            {
                answer.kind = WritePhaseAnswer::Kind::Approval;
                answer.approval = confirmation_->ConfirmApproval(phase.approval);
            }
            else { operation->RequestStop(); }
        }
        catch (...) { operation->RequestStop(); }
        // A confirmation's native nested event loop may destroy this view.
        if (!self) return;
        prompting_ = false;
        if (operation_ != operation) return;
        QTimer::singleShot(0, this, [this, operation, answer] { dispatchPhase(operation, answer); });
    }

    bool WorkbenchWriteController::projectionMatches(const WorkbenchWriteOperationRunner& operation)
    {
        const QPointer<WorkbenchWriteController> self(this);
        const auto target = target_;
        if (!target || !overlay_ || !authorization_
            || !authorization_->Allows(operation.Capture().authorizationEpoch)) return false;
        const TargetCapture current = target->capture();
        if (!self || !target || target_ != target) return false;
        const auto& input = operation.Input();
        if (!mem::SameTarget(current.session, input.session) || current.rev != input.revisions) return false;
        if (historyReplay_) return true; // Scratch replay does not accept the live overlay.
        return overlay_->BaseAddress() == input.baseAddress && overlay_->BaselineSize() == input.baselineSize
            && overlay_->DiffBlocks() == input.blocks;
    }

    void WorkbenchWriteController::finishOperation(
        const std::shared_ptr<WorkbenchWriteOperationRunner>& operation, const WritePhaseSnapshot& phase)
    {
        if (operation_ != operation) return;
        const QPointer<WorkbenchWriteController> self(this);
        bool matches = projectionMatches(*operation);
        if (!self) return;
        finishing_ = true;
        const auto clearFinishing = [self](WorkbenchWriteController*) { if (self) self->finishing_ = false; };
        std::unique_ptr<WorkbenchWriteController, decltype(clearFinishing)> finishingGuard(this, clearFinishing);
        const auto target = target_;
        const bool replaying = historyReplay_;
        auto report = phase.result.report; // Canonical worker report stays unchanged.
        const auto formatter = diagnosticFormatter_;
        if (!report.failureText.empty()) report.failureText = Diagnostic(formatter, report.failureText);
        if (!self) return;
        if (phase.receiptFailed)
        {
            const auto failures = phase.receiptFailures.empty()
                ? std::vector<std::string>{"write receipt retention failed"} : phase.receiptFailures;
            for (const auto& failure : failures)
            {
                const auto visible = Diagnostic(formatter, failure);
                if (!self) return;
                if (!report.failureText.empty()) report.failureText += "; ";
                report.failureText += visible;
            }
        }
        const auto completion = std::move(completion_);
        const auto readOnly = canvasReadOnlyHook_;
        const auto suspend = commitSuspendHook_;
        const auto reread = rereadRangeCallback_;
        const auto bindings = applyBindings_;
        bool projected = matches && !phase.receiptFailed;
        if (matches && !replaying)
        {
            // The durable ledger was already recorded on worker. Only verified
            // exact pending blocks can be accepted into this live GUI overlay.
            try
            {
                for (const auto& verified : phase.result.verifiedBlocks)
                    if (overlay_->AcceptWrite(verified.block, verified.readBack) != mem::AcceptWriteStatus::Ok)
                        projected = false;
            }
            catch (...) { projected = false; }
        }
        // Bindings are value witnesses, including failed attempted spans. Never
        // overwrite definitions/bindings that changed during this operation.
        const auto expectedPending = overlay_ ? overlay_->DiffBlocks() : std::vector<mem::DiffBlock>{};
        if (matches && bindings) bindings(phase.bindings);
        if (!self) return;
        if (matches)
        {
            const auto current = target ? target->capture() : TargetCapture{};
            if (!self) return;
            const auto& input = operation->Input();
            matches = target && target_ == target && authorization_
                && authorization_->Allows(operation->Capture().authorizationEpoch)
                && mem::SameTarget(current.session, input.session) && current.rev == input.revisions
                && overlay_ && overlay_->DiffBlocks() == expectedPending;
            projected &= matches;
        }
        if (matches && report.bytesWritten != 0 && target) target->noteContentChanged();
        if (!self) return;
        TargetCapture projectedTarget;
        if (matches && target) projectedTarget = target->capture();
        if (!self) return;
        const auto& original = operation->Input();
        matches = matches && target && target_ == target
            && mem::SameTarget(projectedTarget.session, original.session)
            && projectedTarget.rev.source == original.revisions.source
            && projectedTarget.rev.content == original.revisions.content + (report.bytesWritten != 0 ? 1U : 0U);
        projected &= matches;
        const auto stillSameProjection = [self, target, matches, projectedTarget]
        {
            if (!self || !matches || !target || self->target_ != target) return false;
            const auto current = target->capture();
            return self && target && self->target_ == target
                && mem::SameTarget(current.session, projectedTarget.session) && current.rev == projectedTarget.rev;
        };
        terminalPoll_->stop();
        operation_.reset();
        pendingPhase_.reset();
        authorization_.reset();
        historyReplay_ = false;
        lastProjectionMatched_ = projected;
        if (readOnly) readOnly(false);
        if (!self) return;
        if (suspend) suspend(false);
        if (!self) return;
        emit undoRedoAvailabilityChanged();
        if (!self) return;
        emitPending();
        if (!self) return;
        emit commitFinished(report);
        if (!self) return;
        if (!Succeeded(report) || phase.receiptFailed)
        {
            emit commitFailed(report);
            if (!self) return;
        }
        if (report.scratchAreaDirty)
        {
            emit scratchAreaDirtyReported();
            if (!self) return;
        }
        // A stale receipt must never reload a new target. Original target data
        // still lives in the process journal, even when GUI projection is stale.
        if (report.needsReread && stillSameProjection())
        {
            target->requestReload();
            if (!self) return;
        }
        if (reread && !phase.result.verifiedBlocks.empty() && stillSameProjection())
        {
            const auto& first = phase.result.verifiedBlocks.front().block;
            const auto& last = phase.result.verifiedBlocks.back().block;
            reread(first.address, last.address + static_cast<std::uint64_t>(last.after.size()) - first.address);
            if (!self) return;
        }
        if (self && !stillSameProjection()) lastProjectionMatched_ = false;
        if (!self) return;
        finishingGuard.reset(); // Completion may now start the next logical action.
        // Terminal polling may observe the runner before the old worker has
        // finished copying its value result. An accepted continuation must wait
        // for the actual per-phase lane, rather than receive false Pending.
        QTimer::singleShot(0, this, [this, completion, report]() mutable
        { completeAfterPhase(std::move(completion), std::move(report)); });
    }

    void WorkbenchWriteController::completeAfterPhase(CommitCompletion completion, mem::CommitReport report)
    {
        if (async_ && async_->isInFlight())
        {
            QTimer::singleShot(1, this, [this, completion = std::move(completion), report = std::move(report)]() mutable
            { completeAfterPhase(std::move(completion), std::move(report)); });
            return;
        }
        const QPointer<WorkbenchWriteController> self(this);
        emit undoRedoAvailabilityChanged();
        if (self && completion) completion(report);
    }

    void WorkbenchWriteController::emitPending()
    {
        if (!overlay_) { emit pendingPatchesChanged(0, 0); return; }
        const auto blocks = overlay_->DiffBlocks();
        emit pendingPatchesChanged(overlay_->PendingByteCount(), static_cast<quint64>(blocks.size()));
    }
    bool WorkbenchWriteController::canUndo() const
    { return !isCommitting() && target_ && ledger_ && ledger_->GetSummary().undoCount != 0; }
    bool WorkbenchWriteController::canRedo() const
    { return !isCommitting() && target_ && ledger_ && ledger_->GetSummary().redoCount != 0; }
    CommitEntryStatus WorkbenchWriteController::replay(bool undo, CommitCompletion completion)
    {
        if (isCommitting() || finishing_)
        {
            emit commitRejectedBusy(undo ? QStringLiteral("undo") : QStringLiteral("redo"));
            return CommitEntryStatus::Busy;
        }
        if (!ledger_) return CommitEntryStatus::Started;
        const auto ticket = undo ? ledger_->PeekUndo() : ledger_->PeekRedo();
        if (!ticket) return CommitEntryStatus::Started;
        return beginCommit(std::move(completion), ticket).status;
    }
    CommitEntryStatus WorkbenchWriteController::undo(CommitCompletion completion)
    { return replay(true, std::move(completion)); }
    CommitEntryStatus WorkbenchWriteController::redo(CommitCompletion completion)
    { return replay(false, std::move(completion)); }
}
