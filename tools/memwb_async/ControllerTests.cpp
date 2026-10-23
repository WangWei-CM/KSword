#include "WorkbenchWriteController.h"
#include "WorkbenchWorkerValue.h"
#include "../AsyncOperation.h"
#include <QEvent>
#include "../AsyncCleanupRegistry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace mem = ksword::memwb;
namespace ui = ks::ui;
namespace
{
    std::atomic<int> checks{0};
    int heartbeat = 0;
    void Check(bool value, const char* text)
    { ++checks; if (!value) throw std::runtime_error(text); }
    template<class Predicate> void Until(Predicate predicate)
    {
        QElapsedTimer elapsed; elapsed.start();
        while (!predicate())
        {
            if (elapsed.elapsed() >= 7000) Check(false, "fake asynchronous action timed out");
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        QCoreApplication::processEvents();
    }
    struct State
    {
        std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(32, 1);
        std::vector<std::string> transcript;
        std::atomic<unsigned> writeCalls{0}, landed{0}, reads{0};
        std::atomic<bool> writeEntered{false};
        bool approval = false, partial = false, mismatch = false;
        int delayMs = 0;
        std::function<void()> afterWrite;
    };
    void WorkerOnly()
    { Check(QThread::currentThread() != QCoreApplication::instance()->thread(), "transport touched GUI thread"); }
    class Identity final : public ui::IFrozenProcessIdentity
    {
    public: bool Validate(const mem::MemoryTargetSession&, std::string&) override { WorkerOnly(); return true; }
    };
    class Io final : public mem::IMemoryIoPort
    {
    public:
        explicit Io(std::shared_ptr<State> state) : state_(std::move(state)) {}
        mem::IoLimits Limits(const mem::MemoryTargetSession&) const override { return {}; }
        mem::IoReadResult Read(const mem::MemoryTargetSession&, std::uint64_t address, std::uint64_t length) override
        {
            WorkerOnly(); ++state_->reads;
            state_->transcript.push_back("R" + std::to_string(address));
            mem::IoReadResult result; result.status = mem::IoReadStatus::Ok;
            for (std::uint64_t n = 0; n < length; ++n)
                result.data.push_back(state_->bytes.at(static_cast<std::size_t>(address + n)));
            if (state_->mismatch && state_->landed.load() != 0 && !result.data.empty()) result.data[0] ^= 1;
            return result;
        }
        mem::IoWriteResult Write(const mem::MemoryTargetSession&, std::uint64_t address,
            const std::vector<std::uint8_t>& bytes, bool approved) override
        {
            WorkerOnly(); ++state_->writeCalls;
            state_->transcript.push_back(std::string(approved ? "A" : "W") + std::to_string(address));
            mem::IoWriteResult result;
            if (state_->approval && !approved) { result.needsApproval = true; return result; }
            state_->writeEntered.store(true);
            if (state_->delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(state_->delayMs));
            const auto count = state_->partial ? bytes.size() / 2 : bytes.size();
            for (std::size_t n = 0; n < count; ++n) state_->bytes.at(static_cast<std::size_t>(address) + n) = bytes[n];
            ++state_->landed;
            result.ok = !state_->partial;
            result.partial = state_->partial;
            result.bytesDone = count;
            result.scratchAreaDirty = state_->partial;
            if (state_->afterWrite) state_->afterWrite();
            return result;
        }
    private: std::shared_ptr<State> state_;
    };
    class Archive final : public mem::IAuditSink
    {
    public:
        bool throwOnStart = false;
        void Record(const mem::AuditRecord& record) override
        {
            WorkerOnly(); std::lock_guard lock(mutex); records.push_back(record);
            if (throwOnStart && record.event == mem::AuditEvent::CommitStarted)
                throw std::runtime_error("fake archive unavailable");
        }
        std::vector<mem::AuditRecord> Get() { std::lock_guard lock(mutex); return records; }
    private: std::mutex mutex; std::vector<mem::AuditRecord> records;
    };
    class Confirm final : public mem::IConfirmationSink
    {
    public:
        int uiCalls = 0, approvals = 0;
        bool accept = true;
        mem::ApprovalAnswer answer = mem::ApprovalAnswer::ThisBlockOnly;
        std::function<void()> onUi, onApproval;
        bool ConfirmUi(const mem::UiConfirmRequest&) override
        {
            Check(QThread::currentThread() == QCoreApplication::instance()->thread(), "confirmation is not GUI-owned");
            ++uiCalls; if (onUi) onUi(); return accept;
        }
        mem::ApprovalAnswer ConfirmApproval(const mem::ApprovalRequest&) override
        {
            Check(QThread::currentThread() == QCoreApplication::instance()->thread(), "approval is not GUI-owned");
            ++approvals; if (onApproval) onApproval(); return answer;
        }
    };
    class Services final : public ui::IWorkbenchServices
    {
    public:
        ui::ModuleEnumResult enumerateProcessModules(std::uint32_t, std::uint64_t) override { return {}; }
        ui::ModuleEnumResult enumerateKernelModules() override { return {}; }
        std::vector<mem::ProcessCandidate> processCandidates() override { return {}; }
        ui::PointerReadResult readPointer(const mem::MemoryTargetSession&, std::uint64_t, std::uint32_t) override { return {}; }
        std::uint64_t ddmaGeneration() override { return 1; }
    };
    struct Harness
    {
        std::shared_ptr<State> state = std::make_shared<State>();
        std::shared_ptr<Archive> archive = std::make_shared<Archive>();
        Confirm confirm;
        mem::MemoryDiffOverlay overlay;
        ui::WorkbenchTarget target{std::make_unique<Services>()};
        std::unique_ptr<ui::WorkbenchWriteController> controller;
        mem::CommitReport last;
        int finished = 0, failed = 0, dirty = 0, completions = 0, rereads = 0;
        std::vector<bool> readOnly, suspended;
        Harness()
        {
            Check(target.requestPin(42, 1042) == ui::LeaveEntryStatus::Completed, "fake pin failed");
            const auto session = target.session();
            Check(overlay.LoadBaseline(mem::IdentityKey(session, 0, state->bytes.size()), 0, state->bytes,
                std::vector<std::uint8_t>(state->bytes.size(), 1)) == mem::BaselineLoadStatus::Ok, "baseline failed");
            controller = std::make_unique<ui::WorkbenchWriteController>();
            controller->setOverlay(&overlay); controller->setTarget(&target); controller->setConfirmationSink(&confirm);
            ui::FrozenWriteFactories factories;
            factories.io = [state = state] { return std::make_unique<Io>(state); };
            factories.identity = [](const mem::MemoryTargetSession&, std::string&) { return std::make_unique<Identity>(); };
            factories.resolvePointer = [](const mem::AddressEntry&, const mem::MemoryTargetSession&, mem::IMemoryIoPort&)
                -> ksword::pointer_chain::Result { throw std::runtime_error("unexpected pointer resolution in empty fake definitions"); };
            controller->setFrozenWriteFactories(std::move(factories));
            controller->setOwnedAuditArchive(archive);
            controller->setFrozenWriteCaptureCallback([](ui::FrozenWriteCapture&, std::string&) { return true; });
            controller->setCanvasReadOnlyHook([this](bool value) { readOnly.push_back(value); });
            controller->setCommitSuspendHook([this](bool value) { suspended.push_back(value); });
            controller->setRereadRangeCallback([this](std::uint64_t, std::uint64_t) { ++rereads; });
            QObject::connect(controller.get(), &ui::WorkbenchWriteController::commitFinished, &target,
                [this](mem::CommitReport report) { last = std::move(report); ++finished; });
            QObject::connect(controller.get(), &ui::WorkbenchWriteController::commitFailed, &target,
                [this](mem::CommitReport) { ++failed; });
            QObject::connect(controller.get(), &ui::WorkbenchWriteController::scratchAreaDirtyReported, &target,
                [this] { ++dirty; });
        }
        void Stage(std::uint64_t at = 0, std::vector<std::uint8_t> bytes = {2})
        { Check(overlay.Stage(at, bytes) == mem::StageStatus::Ok, "stage failed"); target.noteContentChanged(); }
        ui::CommitAttempt Start()
        { return controller->commitPendingNow([this](const mem::CommitReport&) { ++completions; }); }
        void Done() { Until([this] { return controller && !controller->isCommitting() && completions != 0; }); }
    };
    void AuditComplete(Harness& h)
    {
        const auto records = h.archive->Get();
        Check(!records.empty() && records.front().event == mem::AuditEvent::CommitStarted, "canonical start lost");
        Check(records.back().event == mem::AuditEvent::CommitFinished, "canonical finish lost");
        Check(records.back().outcome == h.last.outcome, "receipt and projected report disagree");
        std::vector<mem::AuditEvent> expected{mem::AuditEvent::CommitStarted};
        if (h.confirm.uiCalls != 0)
            expected.push_back(h.confirm.accept ? mem::AuditEvent::UiConfirmAccepted : mem::AuditEvent::UiConfirmDenied);
        else if (h.last.blocksTotal != 0 && h.controller && h.controller->uiConfirmSuppressed())
            expected.push_back(mem::AuditEvent::UiConfirmSuppressed);
        for (int n = 0; n < h.confirm.approvals; ++n) expected.push_back(mem::AuditEvent::ApprovalAnswered);
        expected.push_back(mem::AuditEvent::CommitFinished);
        Check(records.size() == expected.size(), "duplicate or missing canonical middle event");
        for (std::size_t n = 0; n < records.size(); ++n)
            Check(records[n].event == expected[n], "canonical event order changed");
    }
}

// Only native-anchor/module boundaries are fake. The target, controller, phase
// runner, shared AsyncOperation, frozen ports, core and ledger are actual code.
namespace ks::ui
{
    AnchorInfo AcquireAnchorForPid(std::uint32_t pid)
    { return {false, 1000 + pid, 64, reinterpret_cast<void*>(static_cast<std::uintptr_t>(pid)), false, 0}; }
    AnchorInfo AcquireAnchorFromDockHandle(void*) { return {}; }
    void ReleaseAnchorHandle(void*) noexcept {}
    std::optional<bool> QueryAnchorAlive(void*) noexcept { return true; }
    void WorkbenchTarget::refreshProcessModules(bool) {}
    void WorkbenchTarget::refreshKernelModules(bool) {}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTimer pulse; QObject::connect(&pulse, &QTimer::timeout, &app, [] { ++heartbeat; }); pulse.start(1);
    try
    {
        {
            Harness h; h.Stage(); h.state->delayMs = 200; const int beats = heartbeat;
            Check(h.Start().status == ui::CommitEntryStatus::Pending, "commit did not return Pending");
            Check(h.completions == 0 && h.finished == 0, "commit returned synchronous receipt");
            Check(h.Start().status == ui::CommitEntryStatus::Busy, "busy commit accepted");
            h.Done(); Check(heartbeat - beats > 20, "GUI heartbeat blocked by fake write");
            Check(h.last.outcome == mem::CommitOutcome::Committed && h.state->bytes[0] == 2, "delayed write failed");
            Check(!h.overlay.HasPendingPatches() && h.controller->canUndo(), "verified write not projected/journaled");
            Check(h.readOnly == std::vector<bool>{true, false} && h.suspended == h.readOnly, "busy hooks did not span transaction");
            Check(h.finished == 1 && h.completions == 1 && h.state->landed == 1, "physical write or completion repeated");
            AuditComplete(h);
        }
        {
            Harness h; h.Stage(0); h.Stage(2); h.Start(); h.Done();
            Check(h.state->transcript == std::vector<std::string>{"R0", "R2", "W0", "R0", "W2", "R2"}, "all-block expected-before/readback order changed");
        }
        {
            Harness h; h.Stage(); h.confirm.accept = false; h.Start(); h.Done();
            Check(h.state->writeCalls == 0 && h.overlay.HasPendingPatches(), "denial changed target or pending bytes");
            Check(h.last.outcome == mem::CommitOutcome::UserCancelled && h.confirm.uiCalls == 1, "denial semantics changed");
            QCoreApplication::processEvents(); Check(h.confirm.uiCalls == 1, "denied operation auto-retried"); AuditComplete(h);
        }
        {
            Harness h; h.Stage(); h.confirm.onUi = [&] { h.Stage(2); }; h.Start(); h.Done();
            Check(h.last.outcome == mem::CommitOutcome::Stale && h.state->writeCalls == 0, "content changed during confirmation was written");
            Check(h.overlay.PendingByteCount() == 2 && !h.controller->lastCommitProjectionMatched(), "stale projection erased pending edit");
        }
        {
            Harness h; h.Stage(0); h.Stage(2); h.state->approval = true;
            h.confirm.answer = mem::ApprovalAnswer::RestOfBatch; h.Start(); h.Done();
            Check(h.confirm.approvals == 1 && h.last.approvalsAsked == 1 && h.state->writeCalls == 4, "backend-driven RestOfBatch semantics changed");
            Check(h.state->transcript == std::vector<std::string>{"R0","R2","W0","R0","R2","A0","R0","W2","A2","R2"}, "approval recheck/first-false transcript changed");
            AuditComplete(h);
        }
        {
            Harness h; h.Stage(0, {2, 2}); h.state->partial = true; h.Start(); h.Done();
            Check(h.last.outcome == mem::CommitOutcome::WriteFailed && h.last.bytesWritten == 1, "partial receipt lost persisted bytes");
            Check(h.last.blocksWritten == 0 && h.overlay.HasPendingPatches() && !h.controller->canUndo(), "partial was accepted or journaled");
            Check(h.dirty == 1 && h.state->reads >= 2, "partial final observation/dirty report lost"); AuditComplete(h);
        }
        {
            Harness h; h.Stage(); h.state->mismatch = true; h.Start(); h.Done();
            Check(h.last.outcome == mem::CommitOutcome::VerifyMismatch && h.last.bytesWritten == 1, "verify mismatch receipt changed");
            Check(h.overlay.HasPendingPatches() && !h.controller->canUndo(), "mismatch incorrectly accepted");
        }
        {
            Harness h; h.Stage(); h.controller->setConfirmationReadyCallback([] { return false; }); h.Start();
            Until([&] { return h.archive->Get().size() >= 1; });
            Check(h.confirm.uiCalls == 0 && h.controller->isCommitting(), "hidden page presented confirmation");
            h.controller.reset();
            Until([&] { return h.archive->Get().size() >= 2 && ui::AsyncCleanupRegistry::global().pending() == 0; });
            Check(h.state->writeCalls == 0 && h.archive->Get().back().event == mem::AuditEvent::CommitFinished, "closed paused owner lost final receipt");
            Check(h.archive->Get().size() == 2, "unanswered close fabricated confirmation audit");
        }
        {
            Harness h; h.Stage(); h.state->delayMs = 200; h.Start();
            Until([&] { return h.state->writeEntered.load(); });
            h.controller.reset();
            Until([&] { return ui::AsyncCleanupRegistry::global().pending() == 0; });
            Check(h.state->landed == 1 && h.state->reads >= 2, "close interrupted started write/readback");
            const auto ledgers = ui::WorkbenchWriteLedger::RetainedLedgers();
            Check(ledgers.back()->GetSummary().verifiedCount == 1, "closed owner lost verified receipt");
            Check(h.archive->Get().back().event == mem::AuditEvent::CommitFinished, "closed started owner lost canonical finish");
        }
        {
            Harness h; Check(h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply) == mem::ModeSwitchStatus::Switched, "empty mode switch failed");
            h.Stage(); Check(h.controller->requestModeSwitch(mem::WriteMode::Immediate) == mem::ModeSwitchStatus::NeedsDecision, "pending mode decision missing");
            bool switched = false;
            Check(h.controller->resolveModeSwitch(mem::ModeSwitchDecision::ApplyThenSwitch, [&](const mem::ModeSwitchResult& result)
                { Check(result.status == mem::ModeSwitchStatus::Switched && result.commitReport.has_value(), "mode continuation lacked actual report"); switched = true; })
                == ui::CommitEntryStatus::Pending, "apply-mode did not return Pending");
            Check(h.controller->mode() == mem::WriteMode::StagedThenApply && !switched, "mode switched before verified write");
            Until([&] { return switched; }); Check(h.controller->mode() == mem::WriteMode::Immediate, "successful apply did not switch mode");
        }
        {
            Harness h; h.Stage(); h.Start(); h.Done();
            Check(h.controller->undo([&](const mem::CommitReport&) { ++h.completions; }) == ui::CommitEntryStatus::Pending, "undo is not asynchronous");
            Until([&] { return h.completions == 2; });
            Check(h.state->bytes[0] == 1 && !h.controller->canUndo() && h.controller->canRedo(), "undo replay did not move cursor after verification");
            Check(h.controller->redo([&](const mem::CommitReport&) { ++h.completions; }) == ui::CommitEntryStatus::Pending, "redo is not asynchronous");
            Until([&] { return h.completions == 3; });
            Check(h.state->bytes[0] == 2 && h.controller->canUndo() && !h.controller->canRedo(), "redo cursor or bytes wrong");
        }
        {
            Harness h; h.Stage(); h.controller->setConfirmationReadyCallback([] { return false; }); h.Start();
            Until([&] { return !h.archive->Get().empty(); });
            Check(ui::AsyncCleanupRegistry::global().pending() != 0, "paused transaction has no shutdown lease");
            ui::AsyncCleanupRegistry::global().beginShutdown();
            h.Done(); Until([] { return ui::AsyncCleanupRegistry::global().pending() == 0; });
            Check(h.state->landed == 0 && h.overlay.HasPendingPatches(), "shutdown admitted write or erased pending");
            Check(h.archive->Get().size() == 2, "shutdown fabricated confirmation or approval answers");
            ui::AsyncCleanupRegistry::global().cancelShutdown(); AuditComplete(h);
        }
        {
            Harness h; h.Stage(); int refused = 0;
            h.controller->setCanvasReadOnlyHook([&](bool value)
            {
                h.readOnly.push_back(value);
                if (!value) { Check(h.Start().status == ui::CommitEntryStatus::Busy, "finish hook reentered new writer"); ++refused; }
            });
            h.Start(); h.Done(); Check(refused == 1 && h.state->landed == 1, "finish hook corrupted admission");
        }
        {
            Harness h; h.Stage(); h.controller->setTickProvider([&] { h.target.requestReload(); return 1; });
            Check(h.Start().status == ui::CommitEntryStatus::Rejected && h.state->landed == 0, "reentrant tick authorized stale source");
            Check(h.overlay.HasPendingPatches(), "rejected admission erased patch");
        }
        {
            Harness h; h.Stage(); h.controller->setFrozenPointerBindingsCallback([&](const mem::PointerChainBindings&) { h.target.requestReload(); });
            h.Start(); h.Done(); Check(h.state->landed == 1 && h.rereads == 0 && !h.controller->lastCommitProjectionMatched(), "binding callback relabeled old result as new target");
        }
        {
            Harness h; Check(h.Start().status == ui::CommitEntryStatus::Pending, "NoChange does not have an actual async completion");
            h.Done(); Check(h.last.outcome == mem::CommitOutcome::NoChange && h.state->writeCalls == 0, "NoChange touched transport");
            Check(h.controller->lastCommitProjectionMatched(), "NoChange projection rejected the matching owner"); AuditComplete(h);
        }
        {
            Harness h;
            mem::AddressEntry entry; entry.id = 1; entry.kind = mem::EntryKind::Bookmark;
            entry.moduleName = "fake.exe"; entry.targetKey = "fake"; entry.rva = 0x10;
            entry.pointerChain = mem::PointerBookmarkDefinition{};
            auto& definition = *entry.pointerChain;
            definition.processPath = definition.modulePath = "C:/fake.exe";
            definition.moduleSize = definition.moduleFileSize = 4096; definition.moduleFileTime = 1; definition.offsets = {0};
            ksword::pointer_chain::Result resolved; resolved.status = ksword::pointer_chain::Status::Success;
            resolved.address = 0x3000; resolved.steps.push_back({0x1010, 0x3000, 0x3000, 0});
            mem::PointerChainBindings original;
            Check(original.Bind({entry, h.target.session(), resolved}), "fake binding invalid");
            Check(original.Validate(h.target.session(), 0x3000, 1, false,
                [&](std::uint64_t) { return std::optional<mem::AddressEntry>{entry}; },
                [&](const mem::AddressEntry&, const mem::MemoryTargetSession&) { return resolved; }).ok, "fake provenance span invalid");
            std::size_t returnedSpans = 0;
            h.controller->setFrozenWriteCaptureCallback([&](ui::FrozenWriteCapture& capture, std::string&)
                { capture.pointerBindings = original; capture.definitions = {entry}; return true; });
            h.controller->setFrozenPointerBindingsCallback([&](const mem::PointerChainBindings& value) { returnedSpans = value.SpanCount(); });
            h.Start(); h.Done();
            Check(returnedSpans == 1 && h.state->writeCalls == 0, "NoChange erased frozen pointer witnesses");
        }
        {
            Harness first, second; first.Stage(); second.Stage(); first.state->delayMs = 200;
            first.Start(); Check(second.Start().status == ui::CommitEntryStatus::Busy, "another workbench admitted a physical writer");
            Check(second.state->writeCalls == 0 && second.overlay.HasPendingPatches(), "busy view lost its pending bytes");
            first.Done(); Check(second.Start().status == ui::CommitEntryStatus::Pending, "global admission did not reopen after real cleanup"); second.Done();
        }
        {
            Harness h; h.Stage(); h.confirm.onUi = [&] { h.controller.reset(); };
            h.Start(); Until([&] { return !h.controller && ui::AsyncCleanupRegistry::global().pending() == 0; });
            Check(h.state->writeCalls == 0 && h.completions == 0, "destroyed modal owner resumed GUI or wrote memory");
            Check(h.archive->Get().back().event == mem::AuditEvent::CommitFinished, "destroyed modal owner lost final audit");
        }
        {
            Harness h; h.Stage(); h.controller->setCanvasReadOnlyHook([&](bool value) { if (value) h.controller.reset(); });
            Check(h.Start().status == ui::CommitEntryStatus::Pending, "accepted hook-close changed admission to a synchronous result");
            Until([] { return ui::AsyncCleanupRegistry::global().pending() == 0; });
            Check(!h.controller && h.state->writeCalls == 0, "close before first phase failed safe admission");
            Check(h.archive->Get().back().event == mem::AuditEvent::CommitFinished, "hook-close lost canonical finish");
        }
        {
            Harness h; h.Stage(); h.confirm.onUi = [&] { h.controller->invalidateWriteAuthorization(); }; h.Start(); h.Done();
            Check(h.state->writeCalls == 0 && h.last.outcome == mem::CommitOutcome::Stale, "definition authority change did not retire accepted write");
        }
        {
            Harness h; h.Stage(); h.state->afterWrite = [&]
            { QMetaObject::invokeMethod(&h.target, [&] { h.target.requestReload(); }, Qt::QueuedConnection); };
            h.Start(); h.Done();
            Check(h.last.outcome == mem::CommitOutcome::Committed && h.overlay.HasPendingPatches(), "stale GUI projection erased newer source patches");
            Check(h.rereads == 0 && !h.controller->lastCommitProjectionMatched(), "old source completion issued stale local reread");
            Check(ui::WorkbenchWriteLedger::RetainedLedgers().back()->GetSummary().verifiedCount == 1, "stale source lost physical history");
        }
        {
            Harness h; h.Stage(0, {2, 2}); h.Start(); h.Done(); h.state->partial = true;
            Check(h.controller->undo([&](const mem::CommitReport&) { ++h.completions; }) == ui::CommitEntryStatus::Pending, "failed replay did not start");
            Until([&] { return h.completions == 2; });
            Check(h.last.outcome == mem::CommitOutcome::WriteFailed && h.controller->canUndo() && !h.controller->canRedo(), "failed historical write moved cursor");
        }
        {
            Harness h; Check(h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply) == mem::ModeSwitchStatus::Switched, "staged setup failed");
            h.Stage(); h.confirm.accept = false;
            Check(h.controller->requestModeSwitch(mem::WriteMode::Immediate) == mem::ModeSwitchStatus::NeedsDecision, "failed-mode decision missing");
            bool done = false;
            Check(h.controller->resolveModeSwitch(mem::ModeSwitchDecision::ApplyThenSwitch, [&](const mem::ModeSwitchResult& result)
                { Check(result.status == mem::ModeSwitchStatus::ApplyFailed, "declined apply reported successful mode switch"); done = true; })
                == ui::CommitEntryStatus::Pending, "failed mode switch did not return Pending");
            Until([&] { return done; }); Check(h.controller->mode() == mem::WriteMode::StagedThenApply && h.overlay.HasPendingPatches(), "decline switched mode or discarded bytes");
        }
        {
            Harness h; h.state->delayMs = 200; h.Stage(0); h.Stage(2); h.Start();
            Until([&] { return h.state->writeEntered.load(); }); h.controller.reset();
            Until([] { return ui::AsyncCleanupRegistry::global().pending() == 0; });
            Check(h.state->landed == 1 && h.state->reads >= 3, "owner loss admitted another block or skipped readback");
            const auto records = h.archive->Get();
            Check(records.back().blocksWritten == 1 && records.back().bytesWritten == 1, "closed partial batch lost verified prefix counts");
            Check(ui::WorkbenchWriteLedger::RetainedLedgers().back()->GetSummary().verifiedCount == 1, "closed batch lost prefix journal");
        }
        {
            Harness h; h.Stage(); h.archive->throwOnStart = true; h.controller->setUiConfirmSuppressed(true); h.Start(); h.Done();
            Check(h.state->writeCalls == 0, "audit startup failure admitted a suppressed physical write");
            Check(!h.controller->lastCommitProjectionMatched() && h.failed == 1, "receipt failure was not surfaced independently");
            const auto retained = ui::WorkbenchWriteLedger::RetainedLedgers().back()->TakeSnapshot();
            Check(retained.audit.front().record.event == mem::AuditEvent::CommitStarted
                && retained.audit.back().record.event == mem::AuditEvent::CommitFinished, "throwing archive lost canonical in-process receipts");
        }
        {
            Harness h; h.state->delayMs = 200; int staged = 0;
            QObject::connect(h.controller.get(), &ui::WorkbenchWriteController::pendingStageResolved, &h.target,
                [&](ui::WorkbenchWriteController::PendingStageTicket, bool ok, const QString&) { Check(ok, "covered pending-stage rejected"); ++staged; });
            const auto ticket = h.controller->beginPendingStage(0, QByteArray(1, char(2)));
            Check(ticket != 0 && staged == 0 && h.state->landed == 0, "pending-stage ticket pretended to be synchronous staging or physical completion");
            Until([&] { return !h.controller->isCommitting() && staged == 1; });
            Check(h.last.outcome == mem::CommitOutcome::Committed && h.state->bytes[0] == 2, "pending-stage immediate continuation failed");
        }
        {
            Harness h; h.state->delayMs = 1605; h.Stage(0); h.Start(); h.Done();
            h.state->delayMs = 0; h.Stage(1, {3}); h.Start(); h.Done();
            Check(ui::WorkbenchWriteLedger::RetainedLedgers().back()->GetSummary().undoCount == 1,
                "slow operation admission time changed original completion-time undo coalescing");
        }

        {
            // COW values may diverge, including equal-content mutations. The
            // original's immutable storage and serial are the projection proof.
            mem::MemoryDiffOverlay original(8);
            Check(original.LoadBaseline("value", 100, {1,2,3,4}, {1,1,1,1}) == mem::BaselineLoadStatus::Ok, "value baseline");
            auto snapshot = original;
            auto candidate = snapshot;
            Check(original.SameRevision(snapshot), "snapshot copied bulk state or lost revision");
            Check(candidate.Stage(101, {9,8}) == mem::StageStatus::Ok, "value stage");
            Check(original.EffectiveByte(101) == 2 && candidate.EffectiveByte(101) == 9, "COW stage mutated reader");
            const auto pointer = &original;
            Check(original.TryAdoptPrepared(snapshot, candidate) && &original == pointer, "projection replaced overlay object");
            Check(original.PendingByteCount() == 2 && original.PendingBlockCount() == 1, "projection lost block metadata");
            Check(!original.TryAdoptPrepared(snapshot, candidate), "duplicate projection accepted");
            const auto current = original;
            auto newer = current;
            Check(newer.Stage(101, {7}) == mem::StageStatus::Ok, "new candidate stage");
            original.DiscardAll();
            Check(!original.TryAdoptPrepared(current, newer), "discard did not invalidate candidate");
            auto beforeReload = original;
            Check(original.RefreshBaseline("value",100,{1,2,3,4},{1,1,1,1}) == mem::BaselineLoadStatus::Ok, "reload failed");
            Check(!original.TryAdoptPrepared(beforeReload, newer), "equal-content refresh ABA accepted candidate");
            mem::MemoryDiffOverlay limit(1);
            limit.LoadBaseline("limit",0,{1,1,1},{1,1,1}); limit.Stage(0,{2});
            const auto beforeReject = limit;
            Check(limit.Stage(2,{3}) == mem::StageStatus::TooLarge && limit.SameRevision(beforeReject), "rejected merged-size stage changed revision");
            Check(limit.Stage(0,{}) == mem::StageStatus::Empty && limit.SameRevision(beforeReject), "empty stage changed revision");
            Check(limit.LoadBaseline("bad",0,{1},{}) == mem::BaselineLoadStatus::MaskLengthMismatch && limit.SameRevision(beforeReject), "bad baseline changed revision");
            Check(limit.AcceptWrite({0,{1},{2}}, {}) == mem::AcceptWriteStatus::ReadBackLengthMismatch && limit.SameRevision(beforeReject), "bad accept changed revision");
            auto accepted = beforeReject;
            Check(accepted.AcceptWrite({0,{1},{2}}, {2}) == mem::AcceptWriteStatus::Ok, "prepared accept failed");
            Check(limit.EffectiveByte(0) == 2 && limit.BaselineByte(0) == 1 && accepted.BaselineByte(0) == 2, "prepared accept leaked baseline mutation");
        }
        {
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int calls = 0; bool ok = true;
            const auto ticket = h.controller->beginPendingStage(0, QByteArray(1024*1024, char(2)),
                [&](bool success,const QString&) { ++calls; ok = success; });
            // Bounds are absent: cancellation resolves the waiting ticket once.
            Check(ticket != 0, "waiting stage not accepted"); h.controller->cancelPendingStage(ticket);
            Check(calls == 1 && !ok && !h.overlay.HasPendingPatches(), "waiting cancellation lost or staged edit");
            h.controller->notifyWindowMayCover(); Check(calls == 1, "cancelled waiting ticket revived");
        }
        {
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int calls = 0; bool ok = true;
            const auto ticket = h.controller->beginPendingStage(0, QByteArray(4, char(2)),
                [&](bool success,const QString&) { ++calls; ok = success; });
            Check(ticket != 0 && h.controller->isCommitting(), "stage did not hold busy admission");
            h.controller->cancelPendingStage(ticket);
            Until([&] { return !h.controller->isCommitting(); });
            Check(calls == 1 && !ok && !h.overlay.HasPendingPatches() && h.state->writeCalls == 0, "active cancellation projected or wrote");
            Check(h.readOnly == std::vector<bool>{true,false} && h.suspended == h.readOnly, "cancel stage hooks unbalanced");
        }
        {
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int calls = 0; bool ok = true;
            h.controller->beginPendingStage(0, QByteArray(4, char(2)),
                [&](bool success,const QString&) { ++calls; ok = success; });
            h.overlay.Stage(7,{9}); // Deliberately do not notify target: overlay serial alone must reject.
            Until([&] { return !h.controller->isCommitting(); });
            Check(calls == 1 && !ok && h.overlay.PendingByteCount() == 1 && h.overlay.EffectiveByte(7) == 9, "stage serial check overwrote newer patch");
        }
        {
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int calls = 0; bool ok = true;
            h.controller->beginPendingStage(0, QByteArray(4, char(2)),
                [&](bool success,const QString&) { ++calls; ok = success; });
            h.target.noteContentChanged();
            Until([&] { return !h.controller->isCommitting(); });
            Check(calls == 1 && !ok && !h.overlay.HasPendingPatches(), "content-only invalidation accepted stage");
        }
        {
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int calls = 0; bool ok = true;
            h.controller->beginPendingStage(0, QByteArray(4, char(2)),
                [&](bool success,const QString&) { ++calls; ok = success; });
            h.target.requestReload();
            Until([&] { return !h.controller->isCommitting(); });
            Check(calls == 1 && !ok && !h.overlay.HasPendingPatches(), "source invalidation accepted stage");
        }
        {
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int calls = 0;
            h.controller->beginPendingStage(0, QByteArray(4,char(2)), [&](bool,const QString&) { ++calls; });
            h.controller.reset(); QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            Check(calls == 0 && !h.overlay.HasPendingPatches() && h.state->writeCalls == 0, "closed staging owner received result or mutation");
        }
        {
            // First source/identity mutation may be hidden by an external
            // unlock hook. The new target must never get onEditCompleted().
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            bool changed = false; bool ok = true; int calls = 0;
            h.controller->setCanvasReadOnlyHook([&](bool busy)
            { if (!busy && !changed) { changed = true; h.target.requestReload(); } });
            h.controller->beginPendingStage(0,QByteArray(4,char(2)),[&](bool success,const QString&) { ++calls;ok=success; });
            Until([&] { return !h.controller->isCommitting(); });
            Check(calls == 1 && !ok && h.state->writeCalls == 0, "unlock reentry approved stale staging");
        }


        {
            mem::MemoryDiffOverlay a,b;
            a.LoadBaseline("same",0,{1,2},{1,1}); b.LoadBaseline("same",0,{1,2},{1,1});
            const auto original = a; auto prepared = a;
            Check(a.MutationSerial() == b.MutationSerial() && !a.SameRevision(b),"same-serial distinct storage has no identity");
            prepared.Stage(0,{3}); a=b;
            Check(!a.TryAdoptPrepared(original,prepared),"copy-assignment same-serial ABA projected stale value");
            const auto noChange = a; auto noChangePrepared = a;
            Check(a.TryAdoptPrepared(noChange,noChangePrepared),"no-change projection rejected");
            Check(!a.TryAdoptPrepared(noChange,noChangePrepared),"same-storage repeated no-change projection ignored serial");
        }
        {
            // Remove only the shared controller delivery functor after the
            // actual worker completes. Its independent value completion must
            // reconcile busy and project the edit exactly once.
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int calls = 0; bool staged = false;
            h.controller->beginPendingStage(0,QByteArray(4,char(2)),[&](bool ok,const QString&){++calls;staged=ok;});
            QThreadPool::globalInstance()->waitForDone();
            for (auto* child : h.controller->children())
                if (dynamic_cast<ui::AsyncOperation*>(child)) QCoreApplication::removePostedEvents(child,QEvent::MetaCall);
            Until([&]{return !h.controller->isCommitting();});
            Check(calls == 1 && staged && h.overlay.PendingByteCount() == 4 && h.state->writeCalls == 0,"dropped stage receipt left busy or lost edit");
            h.controller->beginPendingStage(8,QByteArray(2,char(3)),[&](bool ok,const QString&){++calls;staged=ok;});
            Until([&]{return !h.controller->isCommitting();});
            Check(calls == 2 && staged && h.overlay.PendingByteCount() == 6,"dropped stage prevented next explicit request");
        }
        {
            Harness h; h.Stage(); h.controller->setUiConfirmSuppressed(true); h.Start();
            QThreadPool::globalInstance()->waitForDone();
            for (auto* child : h.controller->children())
                if (dynamic_cast<ui::AsyncOperation*>(child)) QCoreApplication::removePostedEvents(child,QEvent::MetaCall);
            h.Done();
            Check(h.finished == 1 && h.completions == 1 && h.state->landed == 1 && !h.overlay.HasPendingPatches(),"dropped terminal delivery lost or repeated actual write");
        }
        {
            Harness h; h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            bool once = false; bool staged = true; int calls = 0;
            h.controller->setCanvasReadOnlyHook([&](bool busy)
            { if (!busy && !once) { once=true; h.overlay.DiscardAll(); } });
            h.controller->beginPendingStage(0,QByteArray(4,char(2)),[&](bool ok,const QString&){++calls;staged=ok;});
            Until([&]{return !h.controller->isCommitting();});
            Check(calls == 1 && !staged && !h.overlay.HasPendingPatches(),"unlock overlay mutation was reported as matching stage");
        }
        {
            std::atomic<bool> destroyed{false}, onWorker{false};
            struct Probe
            {
                std::atomic<bool>& destroyed; std::atomic<bool>& onWorker; std::thread::id gui;
                ~Probe() { onWorker.store(std::this_thread::get_id()!=gui);destroyed.store(true); }
            };
            auto value=ui::MakeWorkerValue<Probe>(destroyed,onWorker,std::this_thread::get_id());
            value.reset(); Until([&]{return destroyed.load();});
            Check(onWorker.load(),"pure value final destruction ran on GUI");
        }

        for (const auto size : {std::size_t{64*1024}, std::size_t{1024*1024}, std::size_t{16*1024*1024}, std::size_t{64*1024*1024}})
        {
            Harness h;
            h.state->bytes.assign(size,1);
            Check(h.overlay.LoadBaseline(mem::IdentityKey(h.target.session(),0,size),0,h.state->bytes,
                std::vector<std::uint8_t>(size,1)) == mem::BaselineLoadStatus::Ok,"scale baseline failed");
            h.controller->requestModeSwitch(mem::WriteMode::StagedThenApply);
            int stages = 0; bool staged = false;
            QByteArray payload(static_cast<qsizetype>(size),char(2));
            const int beats = heartbeat;
            double maxDispatchMs = 0;
            const auto wait = [&](const auto& ready)
            {
                QElapsedTimer deadline; deadline.start();
                while (!ready())
                {
                    if (deadline.elapsed() >= 60000) Check(false,"scale timeout");
                    const auto start = std::chrono::steady_clock::now();
                    QCoreApplication::processEvents();
                    const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    maxDispatchMs = (std::max)(maxDispatchMs,ms);
                    QThread::msleep(1);
                }
            };
            auto start = std::chrono::steady_clock::now();
            const auto ticket = h.controller->beginPendingStage(0,payload,[&](bool ok,const QString&) {++stages;staged=ok;});
            const auto submitMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            Check(ticket != 0 && stages == 0,"scale staging was synchronous or rejected");
            wait([&]{return stages == 1 && !h.controller->isCommitting();});
            const auto stageMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            Check(staged && h.overlay.PendingByteCount() == size && h.overlay.PendingBlockCount() == 1,"scale stage lost bytes");
            Check(h.overlay.BaselineByte(size-1) == 1 && h.overlay.EffectiveByte(size-1) == 2,"scale staging changed baseline or omitted tail");
            Check(h.state->writeCalls == 0,"staged mode performed target write");
            start = std::chrono::steady_clock::now();
            Check(h.Start().status == ui::CommitEntryStatus::Pending,"scale commit not accepted");
            const auto commitSubmitMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            wait([&]{return !h.controller->isCommitting() && h.completions == 1;});
            const auto commitMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            Check(h.last.outcome == mem::CommitOutcome::Committed && h.last.bytesWritten == size && !h.overlay.HasPendingPatches(),"scale commit lost full verified projection");
            Check(h.overlay.BaselineByte(size-1) == 2 && h.overlay.ChangeKind(size-1) == mem::ByteChangeKind::SelfWritten,"scale accept missing tail/selfwritten semantics");
            Check(h.state->bytes.front() == 2 && h.state->bytes.back() == 2,"scale transport result truncated");
            Check(h.controller->lastCommitProjectionMatched(),"scale projection refused matching immutable source");
            std::printf("scale,size=%zu,stage_submit_ms=%.6f,commit_submit_ms=%.6f,stage_worker_wall_ms=%.3f,commit_worker_wall_ms=%.3f,max_gui_process_events_ms=%.3f,heartbeat=%d\n",
                size,submitMs,commitSubmitMs,stageMs,commitMs,maxDispatchMs,heartbeat-beats);
            AuditComplete(h);
        }

        {
            Harness h; h.Stage(); int continuations = 0; bool secondPending = false;
            mem::CommitReport second;
            h.controller->commitPendingNow([&](const mem::CommitReport& first)
            {
                ++continuations;
                if (first.outcome != mem::CommitOutcome::Committed) return;
                h.overlay.Stage(2,{3}); h.target.noteContentChanged();
                secondPending = h.controller->commitPendingNow([&](const mem::CommitReport& report)
                    { second=report; ++continuations; }).status == ui::CommitEntryStatus::Pending;
            });
            Until([&]{return continuations == 2 && !h.controller->isCommitting();});
            Check(secondPending && second.outcome == mem::CommitOutcome::Committed && h.state->bytes[2] == 3
                && h.state->landed == 2,"completion continuation returned false Pending then cancelled second write");
        }
        Until([] { return ui::AsyncCleanupRegistry::global().pending() == 0; });
        std::printf("checks=%d heartbeat=%d boundary=QtCore actual controller/frozen worker; fake transport/identity/confirmation only\n", checks.load(), heartbeat);
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s checks=%d\n", error.what(), checks.load()); return 1; }
}
