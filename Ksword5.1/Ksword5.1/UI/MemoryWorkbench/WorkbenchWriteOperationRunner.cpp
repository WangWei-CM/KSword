#include "WorkbenchWriteOperationRunner.h"
#include "../AsyncCleanupRegistry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace ks::ui
{
    namespace mem = ksword::memwb;
    namespace
    {
        std::string CurrentException(const char* context)
        {
            try { throw; }
            catch (const std::exception& error) { return std::string(context) + error.what(); }
            catch (...) { return std::string(context) + "unknown exception"; }
        }

        void CompleteAudit(mem::WriteOperationResult& result, const mem::FrozenWriteInput& input)
        {
            const auto identity = mem::IdentityKey(input.session, input.baseAddress, input.baselineSize);
            if (result.audit.empty())
            {
                mem::AuditRecord started;
                started.event = mem::AuditEvent::CommitStarted;
                started.targetIdentity = identity;
                started.text = "commit started";
                result.audit.push_back(std::move(started));
            }
            if (result.audit.back().event != mem::AuditEvent::CommitFinished)
            {
                mem::AuditRecord finished;
                finished.event = mem::AuditEvent::CommitFinished;
                finished.targetIdentity = identity;
                finished.blocksTotal = result.report.blocksTotal;
                finished.outcome = result.report.outcome;
                finished.blocksWritten = result.report.blocksWritten;
                finished.bytesWritten = result.report.bytesWritten;
                finished.scratchAreaDirty = result.report.scratchAreaDirty;
                finished.readModifyWriteWindow = result.report.readModifyWriteWindow;
                finished.text = result.report.failureText;
                result.audit.push_back(std::move(finished));
            }
        }
    }

    struct WorkbenchWriteOperationRunner::State
    {
        // Prestarted before accepting a runner. Stop admission only links its
        // existing node: no functor/thread allocation and no long runner lock.
        struct StopQueue
        {
            static StopQueue& Get()
            {
                // Process lifetime, like the durable ledger registry. The
                // shutdown registry reaches zero only after native cleanup.
                static auto* queue = new StopQueue;
                return *queue;
            }
            StopQueue() : worker([this] { Run(); }) {}
            void Retain(State& state, const std::shared_ptr<WorkbenchWriteOperationRunner>& owner)
            { std::lock_guard lock(mutex); state.selfKeepalive = owner; }
            void Unlink(State& state)
            {
                if (!state.stopQueued) return;
                if (state.stopPrevious) state.stopPrevious->stopNext = state.stopNext;
                else first = state.stopNext;
                if (state.stopNext) state.stopNext->stopPrevious = state.stopPrevious;
                else last = state.stopPrevious;
                state.stopPrevious = state.stopNext = nullptr;
                state.stopQueued = false;
            }
            void Forget(State& state)
            {
                std::shared_ptr<WorkbenchWriteOperationRunner> retired;
                {
                    std::lock_guard lock(mutex);
                    Unlink(state);
                    retired = std::move(state.selfKeepalive);
                }
            }
            void Enqueue(State& state)
            {
                {
                    std::lock_guard lock(mutex);
                    if (state.stopQueued || !state.selfKeepalive) return;
                    state.stopPrevious = last;
                    if (last) last->stopNext = &state;
                    else first = &state;
                    last = &state;
                    state.stopQueued = true;
                }
                available.notify_one();
            }
            void Run()
            {
                for (;;)
                {
                    std::shared_ptr<WorkbenchWriteOperationRunner> owner;
                    {
                        std::unique_lock lock(mutex);
                        available.wait(lock, [this] { return first != nullptr; });
                        auto* state = first;
                        owner = state->selfKeepalive;
                        Unlink(*state);
                    }
                    try { if (owner) (void)owner->RunPhase(); }
                    catch (...) { /* A terminal value copy may fail after cleanup. */ }
                }
            }
            std::mutex mutex;
            std::condition_variable available;
            State* first = nullptr;
            State* last = nullptr;
            std::thread worker;
        };

        State(mem::FrozenWriteInput frozenInput, FrozenWriteCapture frozenCapture,
            FrozenWriteFactories frozenFactories, std::shared_ptr<WorkbenchWriteLedger> durable,
            std::optional<std::uint64_t> operationTick, std::optional<WorkbenchWriteLedger::ReplayTicket> ticket)
            : input(std::move(frozenInput)), writeLease(std::move(frozenCapture.writeLease)),
              capture(std::move(frozenCapture)), factories(std::move(frozenFactories)),
              ledger(std::move(durable)), tick(operationTick), replay(std::move(ticket)), id(ledger->NextOperation())
        {
            snapshot.operation = id;
            snapshot.bindings = capture.pointerBindings;
        }

        mem::WriteOperationCurrent Current()
        {
            // The core calls this at each safe boundary, after recording the
            // real confirmation answer and before admitting another mutation.
            // Persist that prefix directly, without a full result copy.
            if (operation) FlushFrom(operation->Result());
            mem::WriteOperationCurrent current;
            current.session = input.session;
            current.revisions = input.revisions;
            const auto& authorization = capture.authorization;
            current.ownerAlive = authorization && authorization->ownerAlive.load(std::memory_order_acquire);
            current.stopRequested = authorization && authorization->cancelled.load(std::memory_order_acquire);
            if (authorization && authorization->epoch.load(std::memory_order_acquire) != capture.authorizationEpoch)
                ++current.revisions.content; // Distinct even when unsigned content wraps.
            return current;
        }

        void ReceiptFailure(const std::string& failure)
        {
            snapshot.receiptFailed = true;
            if (capture.authorization)
                capture.authorization->cancelled.store(true, std::memory_order_release);
            snapshot.receiptFailures.push_back(failure);
            if (!snapshot.receiptFailure.empty()) snapshot.receiptFailure += "; ";
            snapshot.receiptFailure += failure;
        }

        void FlushFrom(const mem::WriteOperationResult& result, const bool includeVerified = false)
        {
            // A throwing archive may already have retained the canonical audit
            // value. Never replay that event or duplicate its physical work.
            while (flushedAudit < result.audit.size())
            {
                const auto index = flushedAudit++;
                const auto priorCount = ledger->GetSummary().auditCount;
                try { ledger->Record(result.audit[index]); }
                catch (...)
                {
                    durableIncomplete |= ledger->GetSummary().auditCount == priorCount;
                    ReceiptFailure(CurrentException("write audit archive failed: "));
                }
            }
            // Match the old complete-Commit journal boundary. While awaiting
            // another answer, verified values remain owned by this runner/core;
            // they must not change undo coalescing with a premature timestamp.
            while (includeVerified && flushedVerified < result.verifiedBlocks.size())
            {
                const auto index = flushedVerified++;
                const auto& verified = result.verifiedBlocks[index];
                const auto priorCount = ledger->GetSummary().verifiedCount;
                try
                {
                    VerifiedWriteReceipt receipt;
                    receipt.operation = id;
                    receipt.blockIndex = static_cast<std::uint64_t>(index);
                    receipt.block = verified.block;
                    receipt.readBack = verified.readBack;
                    receipt.tick = *journalTick;
                    receipt.historyReplay = replay.has_value();
                    const auto recorded = ledger->Record(receipt);
                    if (!recorded.accepted)
                    { durableIncomplete = true; ReceiptFailure("verified write receipt was rejected"); }
                    else if (recorded.journalFailed) ReceiptFailure("verified write journal failed");
                }
                catch (...)
                {
                    durableIncomplete |= ledger->GetSummary().verifiedCount == priorCount;
                    ReceiptFailure(CurrentException("verified write receipt failed: "));
                }
            }
        }

        void Flush() { FlushFrom(snapshot.result); }

        void ReadCoreSnapshot(const mem::MemoryWriteOperation::Step step)
        {
            snapshot.serial = operation->RequestSerial();
            snapshot.step = step;
            snapshot.ui = operation->UiRequest();
            snapshot.approval = operation->PendingApproval();
            snapshot.result = operation->Result();
            snapshot.bindings = context->PointerBindings();
            snapshot.terminal = step == mem::MemoryWriteOperation::Step::Finished;
        }

        void InitialFailure(const mem::CommitOutcome outcome, const std::string& reason,
            const bool interrupted = false)
        {
            snapshot.result.report.blocksTotal = static_cast<std::uint64_t>(input.blocks.size());
            snapshot.result.report.outcome = outcome;
            snapshot.result.report.failureText = reason;
            snapshot.result.report.sessionError = outcome == mem::CommitOutcome::InvalidSession
                ? mem::Validate(input.session) : mem::SessionError::None;
            snapshot.result.report.needsReread = outcome == mem::CommitOutcome::WriteFailed
                || outcome == mem::CommitOutcome::Stale;
            snapshot.result.interrupted = interrupted;
            CompleteAudit(snapshot.result, input);
            snapshot.step = mem::MemoryWriteOperation::Step::Finished;
            snapshot.terminal = true;
        }

        void Finalize()
        {
            if (!journalTick)
            {
                const auto completion = std::chrono::steady_clock::now().time_since_epoch();
                journalTick = tick ? *tick : static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(completion).count());
            }
            // All verified blocks from this commit share its completion tick.
            // Archive failure cannot prevent their independent durable receipt.
            FlushFrom(snapshot.result, true);
            if (replay && !replayFinished)
            {
                replayFinished = true;
                if (snapshot.result.report.outcome == mem::CommitOutcome::Committed)
                {
                    try
                    {
                        if (!ledger->FinishReplay(*replay, snapshot.result.report))
                            ReceiptFailure("verified history replay cursor was not advanced");
                    }
                    catch (...) { ReceiptFailure(CurrentException("history replay receipt failed: ")); }
                }
            }
            // Only the exclusive worker invocation destroys I/O/identity state
            // and releases admission. The GUI may retain the immutable snapshot.
            operation.reset();
            context.reset();
            factories = {};
            writeLease.reset();
            cleanupLease.reset();
            terminal.store(true, std::memory_order_release);
            // If the durable allocator failed, keep this pure value owner for
            // process lifetime. Native context and shutdown leases are gone.
            if (!durableIncomplete) StopQueue::Get().Forget(*this);
        }

        WritePhaseSnapshot ValueSnapshot() const
        {
            auto value = snapshot;
            // A low-memory fallback retains the pure core result owner. Native
            // context is already gone; a later value copy cannot lose or invent
            // its actual persisted-byte/verified-block/canonical audit report.
            if (authoritativeResultPending && operation) value.result = operation->Result();
            return value;
        }

        const mem::FrozenWriteInput input;
        std::shared_ptr<FrozenWorkbenchWriteLease> writeLease;
        const FrozenWriteCapture capture; // GUI-readable metadata; no write lease.
        FrozenWriteFactories factories;
        const std::shared_ptr<WorkbenchWriteLedger> ledger;
        const std::optional<std::uint64_t> tick;
        std::optional<std::uint64_t> journalTick;
        const std::optional<WorkbenchWriteLedger::ReplayTicket> replay;
        const std::uint64_t id;
        mutable std::mutex mutex;
        std::atomic<bool> terminal{false};
        std::atomic<bool> ready{false};
        std::shared_ptr<WorkbenchWriteOperationRunner> selfKeepalive;
        State* stopPrevious = nullptr;
        State* stopNext = nullptr;
        bool stopQueued = false;
        std::shared_ptr<AsyncCleanupRegistry::Lease> cleanupLease;
        std::unique_ptr<WorkbenchFrozenWriteContext> context;
        std::unique_ptr<mem::MemoryWriteOperation> operation;
        WritePhaseSnapshot snapshot;
        std::size_t flushedAudit = 0;
        std::size_t flushedVerified = 0;
        bool replayFinished = false;
        bool authoritativeResultPending = false;
        bool durableIncomplete = false;
    };

    WorkbenchWriteOperationRunner::WorkbenchWriteOperationRunner(std::unique_ptr<State> state)
        : state_(std::move(state)) {}
    WorkbenchWriteOperationRunner::~WorkbenchWriteOperationRunner() = default;

    std::shared_ptr<WorkbenchWriteOperationRunner> WorkbenchWriteOperationRunner::Create(
        mem::FrozenWriteInput input, FrozenWriteCapture capture, FrozenWriteFactories factories,
        std::shared_ptr<WorkbenchWriteLedger> ledger, std::optional<std::uint64_t> tick,
        std::optional<WorkbenchWriteLedger::ReplayTicket> replay, std::string& reason)
    {
        reason.clear();
        if (!ledger || !capture.authorization || !capture.writeLease
            || !mem::SameTarget(input.session, capture.session)
            || !mem::SameTarget(input.session, ledger->Session()))
        { reason = "frozen write runner capture is incomplete or changed"; return {}; }
        auto& stopQueue = State::StopQueue::Get(); // Thread startup can reject before acceptance.
        auto runner = std::shared_ptr<WorkbenchWriteOperationRunner>(new WorkbenchWriteOperationRunner(
            std::make_unique<State>(std::move(input), std::move(capture), std::move(factories),
                std::move(ledger), tick, std::move(replay))));
        const std::weak_ptr<WorkbenchWriteOperationRunner> weak = runner;
        runner->state_->cleanupLease = AsyncCleanupRegistry::global().acquire([weak] {
            if (const auto owner = weak.lock()) owner->RequestStop();
        });
        if (!runner->state_->cleanupLease)
        { reason = "application shutdown is waiting for accepted cleanup"; return {}; }
        stopQueue.Retain(*runner->state_, runner);
        // beginShutdown can invoke the weak callback as soon as acquire returns.
        // Publish the installed lease before permitting its cleanup phase.
        runner->state_->ready.store(true, std::memory_order_release);
        const auto& authorization = runner->state_->capture.authorization;
        if (authorization->cancelled.load(std::memory_order_acquire)
            || !authorization->ownerAlive.load(std::memory_order_acquire)) runner->RequestStop();
        return runner;
    }

    WritePhaseSnapshot WorkbenchWriteOperationRunner::RunPhase(const WritePhaseAnswer answer)
    {
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        if (state.terminal.load(std::memory_order_acquire)) return state.ValueSnapshot();
        try
        {
            if (answer.kind != WritePhaseAnswer::Kind::None
                && (answer.operation != state.id || !state.operation)) return state.ValueSnapshot();
            if (!state.operation)
            {
                const auto current = state.Current();
                if (state.input.blocks.empty()) state.InitialFailure(mem::CommitOutcome::NoChange, {});
                else if (mem::Validate(state.input.session) != mem::SessionError::None)
                    state.InitialFailure(mem::CommitOutcome::InvalidSession, "target session is not self-consistent");
                else if (current.stopRequested || !current.ownerAlive)
                    state.InitialFailure(mem::CommitOutcome::UserCancelled,
                        current.ownerAlive ? "write operation stopped at a safe boundary"
                            : "write operation owner was lost at a safe boundary", true);
                else if (current.revisions != state.input.revisions)
                    state.InitialFailure(mem::CommitOutcome::Stale, "content revision changed while waiting for confirmation");
                else
                {
                    auto capture = state.capture;
                    capture.writeLease = state.writeLease;
                    std::string reason;
                    state.context = WorkbenchFrozenWriteContext::Create(std::move(capture), state.factories, reason);
                    state.factories = {};
                    if (!state.context)
                        state.InitialFailure(mem::Validate(state.input.session) != mem::SessionError::None
                            ? mem::CommitOutcome::InvalidSession : mem::CommitOutcome::WriteFailed, reason);
                    else
                        state.operation = std::make_unique<mem::MemoryWriteOperation>(state.input,
                            state.context->Store(), [statePtr = &state] { return statePtr->Current(); });
                }
            }
            if (state.operation)
            {
                state.context->BeginWorkerInvocation();
                bool accepted = true;
                if (answer.kind == WritePhaseAnswer::Kind::Ui)
                    accepted = state.operation->AnswerUi(answer.serial, answer.uiAccepted);
                else if (answer.kind == WritePhaseAnswer::Kind::Approval)
                    accepted = state.operation->AnswerApproval(answer.serial, answer.approval);
                if (!accepted) return state.ValueSnapshot();
                state.ReadCoreSnapshot(state.operation->Advance());
            }
            if (state.snapshot.terminal) state.Finalize();
            else state.Flush();
        }
        catch (...)
        {
            // Allocation/archive/phase bookkeeping failure must not abandon a
            // paused owner. Stop at a core boundary, retaining its real report.
            if (state.capture.authorization)
                state.capture.authorization->cancelled.store(true, std::memory_order_release);
            try
            {
                const auto failure = CurrentException("write phase failed: ");
                if (state.operation)
                {
                    state.context->BeginWorkerInvocation();
                    state.ReadCoreSnapshot(state.operation->Advance());
                }
                else state.InitialFailure(mem::CommitOutcome::WriteFailed, failure);
                state.ReceiptFailure(failure);
                state.snapshot.terminal = true;
                state.snapshot.step = mem::MemoryWriteOperation::Step::Finished;
                state.Finalize();
            }
            catch (...)
            {
                // Even a failing receipt allocator cannot move native teardown
                // back to the GUI or keep shutdown admission permanently held.
                state.authoritativeResultPending = state.operation != nullptr;
                state.context.reset();
                state.factories = {};
                state.writeLease.reset();
                state.cleanupLease.reset();
                state.snapshot.terminal = true;
                state.snapshot.receiptFailed = true;
                state.snapshot.step = mem::MemoryWriteOperation::Step::Finished;
                state.terminal.store(true, std::memory_order_release);
                // The core may be the only complete authoritative value after
                // repeated allocation failure. Its process self-keepalive must
                // survive a closed GUI and the rescue worker's local owner.
                if (!state.authoritativeResultPending && !state.durableIncomplete)
                    State::StopQueue::Get().Forget(state);
            }
        }
        return state.ValueSnapshot();
    }

    std::optional<WritePhaseSnapshot> WorkbenchWriteOperationRunner::TryTerminalSnapshot() const
    {
        if (!state_->terminal.load(std::memory_order_acquire)) return {};
        const std::unique_lock lock(state_->mutex, std::try_to_lock);
        if (!lock) return {};
        return state_->ValueSnapshot();
    }

    void WorkbenchWriteOperationRunner::RequestStop(const bool ownerGone) noexcept
    {
        // Delivery loss after verified terminal cleanup cannot revoke the
        // immutable completed receipt. Real GUI source/content invalidations
        // still change their independent authority epoch before projection.
        if (state_->terminal.load(std::memory_order_acquire)) return;
        const auto& authorization = state_->capture.authorization;
        if (authorization)
        {
            authorization->cancelled.store(true, std::memory_order_release);
            if (ownerGone) authorization->ownerAlive.store(false, std::memory_order_release);
        }
        if (!state_->ready.load(std::memory_order_acquire)
            || state_->terminal.load(std::memory_order_acquire)) return;
        // The service and intrusive node were prepared before acceptance, so
        // this wakeup never depends on QThreadPool allocation during shutdown.
        State::StopQueue::Get().Enqueue(*state_);
    }

    std::uint64_t WorkbenchWriteOperationRunner::Id() const noexcept { return state_->id; }
    const mem::FrozenWriteInput& WorkbenchWriteOperationRunner::Input() const noexcept { return state_->input; }
    const FrozenWriteCapture& WorkbenchWriteOperationRunner::Capture() const noexcept { return state_->capture; }
}
