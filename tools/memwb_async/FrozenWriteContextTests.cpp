#include "WorkbenchFrozenWriteContext.h"
#include "WorkbenchWriteLedger.h"

#include <QCoreApplication>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace mem = ksword::memwb;
namespace ui = ks::ui;
namespace
{
    unsigned checks = 0;
    void Check(const bool value, const char* text)
    { ++checks; if (!value) throw std::runtime_error(text); }

    struct State
    {
        std::map<std::uint64_t, std::uint8_t> bytes;
        std::vector<std::pair<std::uint64_t, std::size_t>> writes;
        std::vector<std::string> transcript;
        std::function<void()> afterWrite;
        std::function<void()> afterDry;
        bool identity = true;
        bool approval = false;
        bool partial = false;
        bool rollback = false;
        bool failForce = false;
        bool mismatch = false;
        bool delayed = false;
        bool dirtyOnSuccess = false;
        std::uint64_t generation = 7;
        std::uint64_t maxWrite = 0;
        std::uint64_t identityCalls = 0;
    };
    class Identity final : public ui::IFrozenProcessIdentity
    {
    public:
        explicit Identity(std::shared_ptr<State> state) : state_(std::move(state)) {}
        bool Validate(const mem::MemoryTargetSession&, std::string& reason) override
        { ++state_->identityCalls; if (!state_->identity) reason = "fake-identity-change"; return state_->identity; }
    private: std::shared_ptr<State> state_;
    };
    class Io final : public mem::IMemoryIoPort
    {
    public:
        explicit Io(std::shared_ptr<State> state) : state_(std::move(state)) {}
        mem::IoLimits Limits(const mem::MemoryTargetSession&) const override
        { return {0, state_->maxWrite}; }
        mem::IoReadResult Read(const mem::MemoryTargetSession&, const std::uint64_t address, const std::uint64_t length) override
        {
            state_->transcript.push_back("read");
            mem::IoReadResult result; result.status = mem::IoReadStatus::Ok;
            for (std::uint64_t offset = 0; offset < length; ++offset) result.data.push_back(state_->bytes[address + offset]);
            return result;
        }
        mem::IoWriteResult Write(const mem::MemoryTargetSession&, const std::uint64_t address,
            const std::vector<std::uint8_t>& bytes, const bool approved) override
        {
            state_->transcript.push_back("write");
            if (state_->delayed) std::this_thread::sleep_for(std::chrono::milliseconds(70));
            mem::IoWriteResult result;
            if (state_->approval && !approved) { result.needsApproval = true; return result; }
            const auto count = state_->partial ? bytes.size() / 2 : bytes.size();
            for (std::size_t offset = 0; offset < count; ++offset) state_->bytes[address + offset] = bytes[offset];
            state_->writes.emplace_back(address, count);
            result.ok = !state_->partial;
            result.partial = state_->partial;
            result.bytesDone = count;
            result.scratchAreaDirty = state_->partial || state_->dirtyOnSuccess;
            result.readModifyWriteWindow = true;
            if (state_->afterWrite) state_->afterWrite();
            return result;
        }
    private: std::shared_ptr<State> state_;
    };
    class Kernel final : public mem::IKernelMutationPort
    {
    public:
        explicit Kernel(std::shared_ptr<State> state) : state_(std::move(state)) {}
        mem::MutationPrepareResult Prepare(const std::uint64_t address,
            const std::vector<std::uint8_t>& after, const std::vector<std::uint8_t>& before) override
        {
            state_->transcript.push_back("prepare");
            pending_[++next_] = {address, after, before};
            return {true, next_, before, {}};
        }
        mem::MutationStepResult DryRunCommit(const std::uint64_t) override
        { state_->transcript.push_back("dry"); if (state_->afterDry) state_->afterDry(); return {true, {}}; }
        mem::MutationStepResult ForceCommit(const std::uint64_t id) override
        {
            state_->transcript.push_back("force"); const auto& entry = pending_.at(id);
            for (std::size_t i = 0; i < entry.after.size(); ++i) state_->bytes[entry.address + i] = entry.after[i];
            if (state_->afterWrite) state_->afterWrite();
            if (state_->mismatch) state_->bytes[entry.address] ^= 1;
            return {!state_->failForce, {}};
        }
        mem::MutationStepResult Rollback(const std::uint64_t id) override
        {
            state_->transcript.push_back("rollback"); state_->rollback = true; const auto& entry = pending_.at(id);
            for (std::size_t i = 0; i < entry.before.size(); ++i) state_->bytes[entry.address + i] = entry.before[i];
            return {true, {}};
        }
        mem::IoReadResult ReadBack(const std::uint64_t address, const std::uint64_t length) override
        {
            state_->transcript.push_back("kernel-readback");
            mem::IoReadResult result; result.status = mem::IoReadStatus::Ok;
            for (std::uint64_t i = 0; i < length; ++i) result.data.push_back(state_->bytes[address + i]);
            return result;
        }
    private:
        struct Pending { std::uint64_t address; std::vector<std::uint8_t> after, before; };
        std::uint64_t next_ = 0;
        std::map<std::uint64_t, Pending> pending_;
        std::shared_ptr<State> state_;
    };

    mem::MemoryTargetSession Session(const mem::Channel channel = mem::Channel::UserMode)
    { mem::MemoryTargetSession session; session.pid = 123; session.processCreateTime100ns = 456; session.channel = channel; session.ddmaGeneration = 7; return session; }
    ui::FrozenWriteCapture Capture(const mem::MemoryTargetSession& session)
    {
        ui::FrozenWriteCapture capture; capture.session = session;
        capture.authorization = std::make_shared<ui::FrozenWriteAuthorization>();
        capture.writeLease = ui::TryAcquireWorkbenchWriteLease();
        return capture;
    }
    ui::FrozenWriteFactories Factories(const std::shared_ptr<State>& state)
    {
        ui::FrozenWriteFactories factories;
        factories.io = [state] { return std::make_unique<Io>(state); };
        factories.kernel = [state] { return std::make_unique<Kernel>(state); };
        factories.identity = [state](const auto&, auto&) { return std::make_unique<Identity>(state); };
        factories.ddmaGeneration = [state] { return state->generation; };
        factories.resolvePointer = [](const auto&, const auto&, auto&) { return ksword::pointer_chain::Result{}; };
        return factories;
    }
    std::unique_ptr<ui::WorkbenchFrozenWriteContext> Create(ui::FrozenWriteCapture capture,
        const std::shared_ptr<State>& state, ui::FrozenWriteFactories factories = {})
    {
        if (!factories.io) factories = Factories(state);
        std::string reason;
        auto context = ui::WorkbenchFrozenWriteContext::Create(std::move(capture), std::move(factories), reason);
        Check(context != nullptr, reason.c_str());
        return context;
    }

    void IdentityAndPolicy()
    {
        auto state = std::make_shared<State>(); auto capture = Capture(Session()); auto auth = capture.authorization;
        auto context = Create(std::move(capture), state);
        Check(!ui::TryAcquireWorkbenchWriteLease(), "one process writer");
        Check(context->Store().Write(0x1000, {1, 2}, false).ok, "basic write");
        auth->Invalidate();
        Check(!context->Store().Write(0x1002, {3}, false).ok && state->writes.size() == 1, "epoch rejects next write");
        auth->ownerAlive.store(false);
        Check(context->Store().Read(0x1000, 2).data == std::vector<std::uint8_t>({1, 2}), "closed owner retains final read");
        state->identity = false;
        Check(!context->Store().Read(0x1000, 2).ok, "identity failure blocks target redirection");
        context.reset(); Check(static_cast<bool>(ui::TryAcquireWorkbenchWriteLease()), "cleanup releases writer");

        state = std::make_shared<State>(); capture = Capture(Session());
        capture.session.processCreateTime100ns = 0;
        context = Create(std::move(capture), state);
        Check(context->Session().processCreateTime100ns == 0 && context->Store().Write(0x1000, {9}, false).ok,
            "weak selected identity stays weak with private lease");
        Check(!context->Store().Write(mem::kKernelSplit, {3}, false).ok, "scope rejects kernel address");
        context.reset();
        capture = Capture(Session()); capture.session.scope = mem::Scope::Physical; capture.session.pid = 0; capture.allowKernelPhysical = false;
        std::string reason;
        Check(!ui::WorkbenchFrozenWriteContext::Create(std::move(capture), Factories(state), reason), "embedded policy");
    }

    void Fragments()
    {
        auto state = std::make_shared<State>(); auto capture = Capture(Session(mem::Channel::Ddma));
        auto auth = capture.authorization; auto context = Create(std::move(capture), state);
        state->afterWrite = [auth] { auth->Invalidate(); };
        const auto stopped = context->Store().Write(4094, std::vector<std::uint8_t>(8, 4), false);
        Check(!stopped.ok && stopped.partial && stopped.bytesDone == 2, "DDMA stops on page boundary");
        Check(state->writes.size() == 1 && state->writes.front().second == 2, "DDMA single actual page");
        context.reset();

        state = std::make_shared<State>(); capture = Capture(Session(mem::Channel::Ddma));
        context = Create(std::move(capture), state);
        state->afterWrite = [state] { ++state->generation; };
        const auto generation = context->Store().Write(4094, std::vector<std::uint8_t>(8, 4), false);
        Check(!generation.ok && generation.partial && generation.bytesDone == 2, "DDMA generation stops next page");
        context.reset();

        state = std::make_shared<State>(); capture = Capture(Session(mem::Channel::Hvm)); auth = capture.authorization;
        context = Create(std::move(capture), state); state->afterWrite = [auth] { auth->ownerAlive.store(false); };
        const auto hvm = context->Store().Write(0x1000, std::vector<std::uint8_t>(3000, 5), false);
        Check(!hvm.ok && hvm.bytesDone == 1024 && state->writes.size() == 1, "HVM actual fragment authority");
        Check(context->Store().Read(0x1000, 1024).ok, "HVM final read after close");
        context.reset();

        state = std::make_shared<State>(); state->approval = true; capture = Capture(Session(mem::Channel::Ddma));
        context = Create(std::move(capture), state);
        Check(context->Store().Write(4094, std::vector<std::uint8_t>(8, 4), false).needsExplicitApproval,
            "first DDMA approval keeps no write");
        Check(state->writes.empty(), "approval no side effects");
        Check(context->Store().Write(4094, std::vector<std::uint8_t>(8, 4), true).ok && state->writes.size() == 2,
            "approved DDMA preserves page order");
        context.reset();

        state = std::make_shared<State>(); state->partial = true; capture = Capture(Session(mem::Channel::Ddma));
        context = Create(std::move(capture), state);
        const auto partial = context->Store().Write(4094, std::vector<std::uint8_t>(8, 4), false);
        Check(!partial.ok && partial.partial && partial.bytesDone == 1 && partial.scratchAreaDirty
            && partial.readModifyWriteWindow && state->writes.size() == 1, "partial DDMA flags and exact persisted bytes");
        context.reset();

        state = std::make_shared<State>(); state->dirtyOnSuccess = true; capture = Capture(Session(mem::Channel::Ddma));
        context = Create(std::move(capture), state);
        const auto dirty = context->Store().Write(4094, std::vector<std::uint8_t>(8, 4), false);
        Check(!dirty.ok && dirty.partial && dirty.bytesDone == 2 && dirty.scratchAreaDirty
            && state->writes.size() == 1, "successful dirty page keeps actual bytes and stops next page");
        context.reset();
        state = std::make_shared<State>(); state->dirtyOnSuccess = true; capture = Capture(Session(mem::Channel::Ddma));
        context = Create(std::move(capture), state);
        const auto completedDirty = context->Store().Write(4094, {3, 4}, false);
        Check(completedDirty.ok && completedDirty.bytesDone == 2 && completedDirty.scratchAreaDirty,
            "completed dirty page retains original successful write receipt");
    }

    void KernelCleanup()
    {
        auto state = std::make_shared<State>(); auto session = Session(mem::Channel::StandardDriver);
        session.pid = 0; session.scope = mem::Scope::KernelVirtual;
        auto capture = Capture(session); auto auth = capture.authorization;
        auto context = Create(std::move(capture), state);
        state->afterDry = [auth] { auth->Invalidate(); };
        auto outcome = context->Store().Write(mem::kKernelSplit, {1, 2, 3}, false);
        Check(!outcome.ok && outcome.bytesDone == 0, "kernel invalidation before force");
        Check(std::find(state->transcript.begin(), state->transcript.end(), "force") == state->transcript.end(), "no new force after invalidation");
        Check(std::find(state->transcript.begin(), state->transcript.end(), "kernel-readback") != state->transcript.end(), "prepared action recovery read continues");
        context.reset();

        state = std::make_shared<State>(); capture = Capture(session); auth = capture.authorization;
        context = Create(std::move(capture), state); state->mismatch = true;
        state->afterWrite = [auth] { auth->ownerAlive.store(false); };
        outcome = context->Store().Write(mem::kKernelSplit, {1, 2, 3}, false);
        Check(!outcome.ok && outcome.rolledBack && outcome.bytesDone == 0 && state->rollback,
            "owner loss during force still finishes readback and rollback");
        Check(state->bytes[mem::kKernelSplit] == 0, "rollback verified");
    }

    void PointerWitness()
    {
        auto state = std::make_shared<State>(); auto capture = Capture(Session()); auto auth = capture.authorization;
        mem::AddressEntry entry; entry.id = 1; entry.kind = mem::EntryKind::Bookmark; entry.moduleName = "fake.exe";
        entry.targetKey = "fake"; entry.rva = 0x10; entry.pointerChain = mem::PointerBookmarkDefinition{};
        auto& def = *entry.pointerChain; def.processPath = "C:/fake.exe"; def.modulePath = "C:/fake.exe";
        def.moduleSize = 4096; def.moduleFileSize = 4096; def.moduleFileTime = 1; def.offsets = {0};
        ksword::pointer_chain::Result expected; expected.status = ksword::pointer_chain::Status::Success;
        expected.address = 0x3000; expected.steps.push_back({0x1010, 0x3000, 0x3000, 0});
        Check(capture.pointerBindings.Bind({entry, capture.session, expected}), "valid pointer binding");
        capture.definitions.push_back(entry);
        auto factories = Factories(state); unsigned resolves = 0;
        factories.resolvePointer = [expected, &resolves](const auto&, const auto&, auto& port) {
            ++resolves; (void)port; return expected;
        };
        auto context = Create(std::move(capture), state, std::move(factories));
        Check(context->Store().Write(0x3000, {4}, false).ok, "frozen pointer resolves on worker");
        Check(resolves != 0 && context->PointerBindings().SpanCount() == 1, "attempted provenance retained");
        auth->Invalidate();
        Check(!context->Store().Write(0x3001, {5}, false).ok && state->writes.size() == 1, "definition revision cancels before next pointer IO");
        context.reset();

        capture = Capture(Session()); Check(capture.pointerBindings.Bind({entry, capture.session, expected}), "second binding");
        capture.definitions.push_back(entry); factories = Factories(state);
        factories.resolvePointer = [expected](const auto&, const auto&, auto&) mutable {
            // Same endpoint, different intermediate witness must still fail.
            auto changed = expected; changed.steps.front().readAddress += 8; return changed;
        };
        context = Create(std::move(capture), state, std::move(factories));
        Check(!context->Store().Write(0x3000, {7}, false).ok, "full path witness mismatch");
    }

    void DurableLedger()
    {
        auto ledger = ui::WorkbenchWriteLedger::CreatePersistent(Session()); const auto id = ledger->Id();
        const auto operation = ledger->NextOperation();
        ui::VerifiedWriteReceipt receipt{operation, 0, {0x1000, {1, 2}, {3, 4}}, {3, 4}, 1};
        Check(ledger->Record(receipt).accepted, "verified receipt accepted");
        Check(ledger->Record(receipt).duplicate && ledger->GetSummary().verifiedCount == 1, "receipt idempotence");
        receipt.readBack[0] = 0; Check(!ledger->Record(receipt).accepted, "conflicting receipt rejected");
        auto undo = ledger->PeekUndo(); Check(undo.has_value(), "undo available");
        mem::CommitReport report; report.outcome = mem::CommitOutcome::VerifyMismatch; report.blocksWritten = 0;
        Check(!ledger->FinishReplay(*undo, report) && ledger->PeekUndo().has_value(), "failed replay keeps cursor");
        receipt = {ledger->NextOperation(), 0, {0x1000, {3, 4}, {1, 2}}, {1, 2}, 2, true};
        Check(ledger->Record(receipt).accepted && ledger->GetSummary().undoCount == 1, "replay receipt creates no new undo step");
        report.outcome = mem::CommitOutcome::Committed; report.blocksWritten = 1; report.bytesWritten = 2;
        Check(ledger->FinishReplay(*undo, report) && !ledger->PeekUndo() && ledger->PeekRedo(), "verified replay advances once");
        Check(!ledger->FinishReplay(*undo, report), "duplicate finish cannot advance twice");
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < 4; ++t) workers.emplace_back([ledger] {
            for (unsigned i = 0; i < 200; ++i) { mem::AuditRecord record; record.text = std::to_string(i); ledger->Record(record); }
        });
        for (auto& worker : workers) worker.join();
        Check(ledger->GetSummary().auditCount == 800, "all concurrent audit values retained");
        const auto snapshot = ledger->TakeSnapshot();
        for (std::size_t i = 0; i < snapshot.audit.size(); ++i)
            Check(snapshot.audit[i].sequence == i + 1, "audit canonical order");
        ledger.reset();
        const auto retained = ui::WorkbenchWriteLedger::RetainedLedgers();
        const auto found = std::find_if(retained.begin(), retained.end(), [id](const auto& item) { return item->Id() == id; });
        Check(found != retained.end() && (*found)->GetSummary().auditCount == 800
            && (*found)->GetSummary().verifiedCount == 2, "controller close retains audit and verified blocks");

        ledger = ui::WorkbenchWriteLedger::CreatePersistent(Session());
        auto first = ledger->NextOperation();
        Check(ledger->Record({first, 0, {0x2000, {0, 0, 0}, {1, 1, 1}}, {1, 1, 1}, 1}).accepted,
            "merge initial receipt");
        Check(ledger->Record({ledger->NextOperation(), 0, {0x2001, {1}, {0}}, {0}, 2}).accepted,
            "merge middle back to original");
        undo = ledger->PeekUndo();
        Check(undo && undo->replay.expectedCurrent == std::vector<std::uint8_t>({1, 0, 1}),
            "merged replay has unchanged middle byte");
        report.blocksWritten = 2; report.bytesWritten = 2;
        Check(ledger->FinishReplay(*undo, report) && ledger->GetSummary().redoCount == 1,
            "split diff replay moves journal only after full success");

        ledger = ui::WorkbenchWriteLedger::CreatePersistent(Session());
        for (std::uint64_t i = 0; i < 300; ++i)
            Check(ledger->Record({ledger->NextOperation(), 0, {0x10000 + i * 8, {0}, {1}}, {1}, i * 1000}).accepted,
                "unbounded accepted receipt");
        Check(ledger->GetSummary().verifiedCount == 300 && ledger->GetSummary().undoCount == mem::kEditJournalMaxSteps,
            "canonical receipts survive bounded undo eviction");
        undo = ledger->PeekUndo();
        Check(ledger->Record({ledger->NextOperation(), 0, {0x20000, {0}, {1}}, {1}, 999999}).accepted,
            "new verified write advances journal epoch");
        report.blocksWritten = 1; report.bytesWritten = 1;
        Check(!ledger->FinishReplay(*undo, report), "stale replay ticket cannot move changed journal");
    }

    class HeldArchive final : public mem::IAuditSink
    {
    public:
        void Record(const mem::AuditRecord&) override
        {
            entered.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::atomic<bool> entered{false};
    };

    void NonblockingLedgerProjection()
    {
        auto archive = std::make_shared<HeldArchive>();
        auto ledger = ui::WorkbenchWriteLedger::CreatePersistent(Session(), archive);
        std::thread recording([ledger] { ledger->Record(mem::AuditRecord{}); });
        while (!archive->entered.load()) std::this_thread::yield();
        const auto started = std::chrono::steady_clock::now();
        const auto summary = ledger->GetSummary();
        const auto undo = ledger->PeekUndo();
        const auto redo = ledger->PeekRedo();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        recording.join();
        Check(summary.auditCount == 1 && summary.undoCount == 0 && summary.redoCount == 0,
            "atomic GUI summary publishes before held archive");
        Check(elapsed < std::chrono::milliseconds(70), "GUI summary and try-peek do not wait for worker audit lock");
        Check(!undo && !redo && ledger->TakeSnapshot().audit.size() == 1,
            "busy peeks preserve canonical retained archive");
    }

    void WriterStress()
    {
        std::atomic<unsigned> active = 0, maximum = 0, acquired = 0;
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
            for (unsigned attempt = 0; attempt < 1000; ++attempt)
            {
                const auto lease = ui::TryAcquireWorkbenchWriteLease();
                if (!lease) { std::this_thread::yield(); continue; }
                const auto count = active.fetch_add(1) + 1;
                auto before = maximum.load();
                while (before < count && !maximum.compare_exchange_weak(before, count)) {}
                ++acquired;
                std::this_thread::yield();
                --active;
            }
        });
        for (auto& worker : workers) worker.join();
        Check(acquired != 0 && maximum == 1 && active == 0, "nonblocking process admission never overlaps accepted writers");
        Check(static_cast<bool>(ui::TryAcquireWorkbenchWriteLease()), "writer lease returns after concurrent stress");

        auto state = std::make_shared<State>();
        auto context = Create(Capture(Session()), state);
        bool migrated = false;
        std::thread nextPhase([&] {
            context->BeginWorkerInvocation();
            migrated = context->Store().Write(0x1000, {6}, false).ok;
        });
        nextPhase.join();
        Check(migrated, "exclusive worker continuation may migrate pool threads");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        std::atomic<bool> done = false; unsigned heartbeats = 0; std::exception_ptr error;
        QTimer timer; timer.setInterval(5); QObject::connect(&timer, &QTimer::timeout, [&] {
            ++heartbeats; if (done.load()) app.quit();
        }); timer.start();
        std::thread worker([&] {
            try
            {
                IdentityAndPolicy(); Fragments(); KernelCleanup(); PointerWitness(); DurableLedger();
                NonblockingLedgerProjection(); WriterStress();
                auto state = std::make_shared<State>(); state->delayed = true;
                auto context = Create(Capture(Session()), state);
                Check(context->Store().Write(0x1000, {3}, false).ok, "delayed fake write");
            }
            catch (...) { error = std::current_exception(); }
            done.store(true);
        });
        QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        app.exec(); worker.join(); if (error) std::rethrow_exception(error);
        Check(heartbeats >= 4, "QCore heartbeat during delayed fake write");
        std::cout << "PASS " << checks << " checks; heartbeat=" << heartbeats << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
