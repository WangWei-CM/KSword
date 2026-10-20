#include "MemoryWriteOperation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace ksword::memwb;

namespace
{
    int checks = 0;
    void Check(const bool condition, const std::string& text)
    {
        ++checks;
        if (!condition) throw std::runtime_error(text);
    }

    constexpr std::uint64_t Base = 0x1000;
    std::string Address(const std::uint64_t value) { return std::to_string(value); }
    enum class Fault { None, Fail, Partial, OkPartial, Rollback, ApprovalPersisted, AgainApproval, ThrowWrite };

    struct Script
    {
        bool approval = false;
        Fault fault = Fault::None;
        std::uint64_t faultAddress = Base;
        std::uint64_t badPrecheckAddress = 0;
        bool partialPrecheck = false;
        bool failedPrecheck = false;
        bool mismatchVerify = false;
        bool partialVerify = false;
        bool failedVerify = false;
        bool dirtyFirstRead = false;
        bool windowFirstWrite = false;
    };

    struct FakeStore final : IByteStore
    {
        Script script;
        std::map<std::uint64_t, std::uint8_t> memory;
        std::map<std::uint64_t, int> written;
        std::vector<std::string> calls;
        std::function<void(const std::string&)> after;
        int reads = 0;
        int writes = 0;
        explicit FakeStore(Script value = {}) : script(value)
        {
            for (std::uint64_t offset = 0; offset < 12; ++offset)
                memory[Base + offset] = static_cast<std::uint8_t>(offset + 1);
        }
        AccessResult Read(const std::uint64_t address, const std::uint64_t length) override
        {
            const std::string call = "R" + Address(address) + ":" + Address(length);
            calls.push_back(call);
            AccessResult result;
            result.ok = true;
            result.bytesDone = length;
            for (std::uint64_t i = 0; i < length; ++i) result.data.push_back(memory.at(address + i));
            if (script.dirtyFirstRead && reads == 0) result.scratchAreaDirty = true;
            if (address == script.badPrecheckAddress && written[address] == 0)
            {
                if (script.failedPrecheck) { result.ok = false; result.failureText = "precheck fault"; }
                if (script.partialPrecheck) { result.partial = true; result.data.resize(1); result.bytesDone = 1; }
            }
            if (written[address] != 0 && address == script.faultAddress)
            {
                if (script.mismatchVerify) result.data.front() ^= 1;
                if (script.partialVerify) { result.partial = true; result.data.resize(1); result.bytesDone = 1; }
                if (script.failedVerify) { result.ok = false; result.failureText = "verify fault"; }
            }
            ++reads;
            if (after) after(call);
            return result;
        }
        AccessResult Write(const std::uint64_t address, const std::vector<std::uint8_t>& bytes,
            const bool approved) override
        {
            const std::string call = "W" + Address(address) + ":" + (approved ? "1" : "0");
            calls.push_back(call);
            AccessResult result;
            const bool faultHere = address == script.faultAddress;
            if ((script.approval && !approved)
                || (faultHere && script.fault == Fault::AgainApproval && approved)
                || (faultHere && script.fault == Fault::ApprovalPersisted))
            {
                result.needsExplicitApproval = true;
                result.failureText = "backend approval required";
                if (faultHere && script.fault == Fault::ApprovalPersisted)
                {
                    memory[address] = bytes.front();
                    result.bytesDone = 1;
                    ++written[address];
                }
            }
            else if (faultHere && script.fault != Fault::None)
            {
                result.failureText = "write fault";
                if (script.fault == Fault::Rollback) result.rolledBack = true;
                else
                {
                    memory[address] = bytes.front();
                    result.bytesDone = 1;
                    ++written[address];
                }
                result.partial = script.fault == Fault::Partial || script.fault == Fault::OkPartial;
                result.ok = script.fault == Fault::OkPartial;
            }
            else
            {
                for (std::size_t i = 0; i < bytes.size(); ++i) memory[address + i] = bytes[i];
                ++written[address];
                result.ok = true;
                result.bytesDone = static_cast<std::uint64_t>(bytes.size());
            }
            if (script.windowFirstWrite && writes == 0) result.readModifyWriteWindow = true;
            ++writes;
            if (after) after(call);
            if (faultHere && script.fault == Fault::ThrowWrite) throw std::runtime_error("throw after prefix");
            return result;
        }
    };

    struct Recorder final : IAuditSink
    {
        std::vector<AuditRecord> records;
        void Record(const AuditRecord& value) override { records.push_back(value); }
    };
    struct Confirmer final : IConfirmationSink
    {
        bool accepted = true;
        std::vector<ApprovalAnswer> answers;
        std::vector<UiConfirmRequest> ui;
        std::vector<ApprovalRequest> approvals;
        std::function<void()> onUi;
        std::function<void(std::size_t)> onApproval;
        bool ConfirmUi(const UiConfirmRequest& request) override
        {
            ui.push_back(request);
            if (onUi) onUi();
            return accepted;
        }
        ApprovalAnswer ConfirmApproval(const ApprovalRequest& request) override
        {
            const auto index = approvals.size();
            approvals.push_back(request);
            if (onApproval) onApproval(index);
            return index < answers.size() ? answers[index] : ApprovalAnswer::ThisBlockOnly;
        }
    };

    struct Fixture
    {
        MemoryDiffOverlay overlay;
        MemoryTargetSession session;
        SessionRevisions revisions;
        FakeStore store;
        Confirmer confirmer;
        Recorder audit;
        bool ownerAlive = true;
        bool stopRequested = false;
        explicit Fixture(Script script = {}) : store(script)
        {
            session.pid = 42;
            session.processCreateTime100ns = 123456;
            session.attachGeneration = 9;
            std::vector<std::uint8_t> baseline;
            for (std::uint8_t i = 1; i <= 12; ++i) baseline.push_back(i);
            Check(overlay.LoadBaseline("fixture", Base, baseline, std::vector<std::uint8_t>(12, 1))
                == BaselineLoadStatus::Ok, "baseline load");
            Check(overlay.Stage(Base, { 91, 92 }) == StageStatus::Ok, "stage first");
            Check(overlay.Stage(Base + 4, { 95, 96 }) == StageStatus::Ok, "stage second");
            Check(overlay.Stage(Base + 8, { 99, 100 }) == StageStatus::Ok, "stage third");
        }
        FrozenWriteInput Input(const bool suppressed = false) const
        {
            return { session, revisions.Capture(), overlay.BaseAddress(), overlay.BaselineSize(),
                overlay.DiffBlocks(), suppressed };
        }
        WriteOperationCurrent Current() const
        {
            return { session, revisions.Capture(), stopRequested, ownerAlive };
        }
    };

    std::string AuditText(const AuditRecord& record)
    {
        return std::to_string(static_cast<int>(record.event)) + "|" + record.targetIdentity + "|"
            + Address(record.blocksTotal) + "|" + Address(record.blockIndex) + "|" + Address(record.address)
            + "|" + Address(record.length) + "|" + std::to_string(static_cast<int>(record.approvalAnswer))
            + "|" + std::to_string(static_cast<int>(record.outcome)) + "|" + Address(record.blocksWritten)
            + "|" + Address(record.bytesWritten) + "|" + std::to_string(record.scratchAreaDirty)
            + "|" + std::to_string(record.readModifyWriteWindow) + "|" + record.text;
    }

    void SameReport(const CommitReport& a, const CommitReport& b, const std::string& name)
    {
        Check(a.outcome == b.outcome, name + " outcome");
        Check(a.blocksTotal == b.blocksTotal, name + " blocks total");
        Check(a.blocksWritten == b.blocksWritten, name + " verified count");
        Check(a.bytesWritten == b.bytesWritten, name + " persisted count");
        Check(a.failureText == b.failureText, name + " failure text");
        Check(a.scratchAreaDirty == b.scratchAreaDirty, name + " dirty OR");
        Check(a.readModifyWriteWindow == b.readModifyWriteWindow, name + " window OR");
        Check(a.approvalsAsked == b.approvalsAsked, name + " approvals asked");
        Check(a.needsReread == b.needsReread, name + " reread");
        Check(a.sessionError == b.sessionError, name + " session error");
    }

    using Configure = std::function<void(Fixture&)>;
    void Compare(const std::string& name, const Script script = {}, Configure configure = {},
        const bool suppressed = false)
    {
        Fixture old(script);
        Fixture next(script);
        if (configure) { configure(old); configure(next); }
        MemoryWriteTransaction transaction(old.overlay, old.session, old.revisions,
            old.store, old.confirmer, old.audit);
        transaction.SetUiConfirmSuppressed(suppressed);
        const CommitReport expected = transaction.Commit();
        MemoryWriteOperation operation(next.Input(suppressed), next.store, [&next] { return next.Current(); });
        auto step = operation.Advance();
        int resumes = 0;
        while (step != MemoryWriteOperation::Step::Finished)
        {
            Check(++resumes < 10, name + " bounded resumes");
            const auto serial = operation.RequestSerial();
            const auto priorCalls = next.store.calls;
            if (step == MemoryWriteOperation::Step::NeedUi)
            {
                const bool answer = next.confirmer.ConfirmUi(operation.UiRequest());
                Check(operation.AnswerUi(serial, answer), name + " UI answer accepted");
                Check(!operation.AnswerUi(serial, answer), name + " duplicate UI rejected");
            }
            else
            {
                const auto answer = next.confirmer.ConfirmApproval(operation.PendingApproval());
                Check(operation.AnswerApproval(serial, answer), name + " approval accepted");
                Check(!operation.AnswerApproval(serial, answer), name + " duplicate approval rejected");
            }
            Check(next.store.calls == priorCalls, name + " answers do no IO");
            step = operation.Advance();
        }
        const auto& actual = operation.Result();
        SameReport(expected, actual.report, name);
        Check(transaction.CurrentState() == operation.CurrentState(), name + " terminal state");
        Check(actual.audit.size() == old.audit.records.size(), name + " audit length");
        for (std::size_t i = 0; i < actual.audit.size(); ++i)
            Check(AuditText(actual.audit[i]) == AuditText(old.audit.records[i]), name + " ordered audit " + Address(i));
        auto calls = next.store.calls;
        if (!actual.failedWriteReadbacks.empty())
        {
            Check(actual.failedWriteReadbacks.size() == 1, name + " one final evidence read");
            Check(!calls.empty() && calls.back().front() == 'R', name + " failure final observation");
            calls.pop_back(); // Explicit new evidence-only delta, after the failed Write.
        }
        Check(calls == old.store.calls, name + " complete ordered port transcript");
        Check(next.store.memory == old.store.memory, name + " same target memory");
        Check(next.confirmer.ui.size() == old.confirmer.ui.size(), name + " UI request count");
        Check(next.confirmer.approvals.size() == old.confirmer.approvals.size(), name + " approval request count");
        for (std::size_t i = 0; i < next.confirmer.approvals.size(); ++i)
        {
            const auto& a = old.confirmer.approvals[i];
            const auto& b = next.confirmer.approvals[i];
            Check(a.targetIdentity == b.targetIdentity && a.blockIndex == b.blockIndex
                && a.blocksTotal == b.blocksTotal && a.address == b.address && a.length == b.length
                && a.backendText == b.backendText, name + " approval request fields");
        }
        Check(actual.verifiedBlocks.size() == expected.blocksWritten, name + " verified result count");
        for (const auto& verified : actual.verifiedBlocks)
            Check(next.overlay.AcceptWrite(verified.block, verified.readBack) == AcceptWriteStatus::Ok,
                name + " GUI accepts verified value");
        Check(next.overlay.DiffBlocks() == old.overlay.DiffBlocks(), name + " failed+remaining patches stay pending");
        const auto frozenCalls = next.store.calls;
        const auto frozenAudit = actual.audit.size();
        Check(operation.Advance() == MemoryWriteOperation::Step::Finished, name + " idempotent finish");
        Check(!operation.AnswerUi(operation.RequestSerial(), true), name + " finished answer rejected");
        Check(next.store.calls == frozenCalls && actual.audit.size() == frozenAudit, name + " finish never replays");
    }

    void EquivalenceTests()
    {
        Compare("success");
        Compare("suppressed", {}, {}, true);
        Compare("deny UI", {}, [](Fixture& f) { f.confirmer.accepted = false; });
        Compare("empty before invalid session", {}, [](Fixture& f) { f.overlay.DiscardAll(); f.session.pid = 0; });
        Compare("invalid session", {}, [](Fixture& f) { f.session.pid = 0; });
        Compare("all-block expected-before", {}, [](Fixture& f) { f.store.memory[Base + 8] ^= 1; });
        Compare("target changed in UI", {}, [](Fixture& f) { f.confirmer.onUi = [&f] { ++f.session.attachGeneration; }; });
        Compare("source changed in UI", {}, [](Fixture& f) { f.confirmer.onUi = [&f] { f.revisions.BumpSource(); }; });
        Compare("content changed in UI", {}, [](Fixture& f) { f.confirmer.onUi = [&f] { f.revisions.BumpContent(); }; });
        for (const bool fail : { false, true })
        {
            Script script;
            script.badPrecheckAddress = Base + 8;
            script.failedPrecheck = fail;
            script.partialPrecheck = !fail;
            Compare(fail ? "failed last precheck" : "partial last precheck", script);
        }
        Script approval;
        approval.approval = true;
        Compare("approval each block", approval);
        Compare("approval batch", approval, [](Fixture& f) { f.confirmer.answers = { ApprovalAnswer::RestOfBatch }; });
        Compare("approval denied", approval, [](Fixture& f) { f.confirmer.answers = { ApprovalAnswer::Deny }; });
        Compare("approval invalid enum", approval, [](Fixture& f) { f.confirmer.answers = { static_cast<ApprovalAnswer>(999) }; });
        Compare("approval denied later", approval, [](Fixture& f) {
            f.confirmer.answers = { ApprovalAnswer::ThisBlockOnly, ApprovalAnswer::Deny }; });
        Compare("target changes in approval", approval, [](Fixture& f) {
            f.confirmer.onApproval = [&f](std::size_t) { ++f.session.processCreateTime100ns; }; });
        Compare("source changes in later approval", approval, [](Fixture& f) {
            f.confirmer.onApproval = [&f](const std::size_t index) { if (index == 1) f.revisions.BumpSource(); }; });
        Compare("content changes in approval", approval, [](Fixture& f) {
            f.confirmer.onApproval = [&f](std::size_t) { f.revisions.BumpContent(); }; });
        Compare("remaining before changes in approval", approval, [](Fixture& f) {
            f.confirmer.onApproval = [&f](const std::size_t index) { if (index == 1) f.store.memory[Base + 8] ^= 1; }; });
        for (const Fault fault : { Fault::Fail, Fault::Partial, Fault::OkPartial, Fault::Rollback,
            Fault::ApprovalPersisted, Fault::AgainApproval })
        {
            for (const auto address : { Base, Base + 4 })
            {
                Script script;
                script.fault = fault;
                script.faultAddress = address;
                script.approval = fault == Fault::AgainApproval;
                Compare("fault " + std::to_string(static_cast<int>(fault)) + " at " + Address(address), script);
            }
        }
        for (int fault = 0; fault < 3; ++fault)
        {
            Script script;
            script.faultAddress = Base + 4;
            script.mismatchVerify = fault == 0;
            script.partialVerify = fault == 1;
            script.failedVerify = fault == 2;
            Compare("verify " + std::to_string(fault), script);
        }
        Script dirty;
        dirty.dirtyFirstRead = true;
        dirty.windowFirstWrite = true;
        Compare("warning OR across success", dirty);
        dirty.fault = Fault::Fail;
        dirty.faultAddress = Base + 4;
        Compare("warning OR across failure", dirty);
    }

    void ContinuationTests()
    {
        Fixture f;
        MemoryWriteOperation operation(f.Input(), f.store, [&f] { return f.Current(); });
        Check(operation.Advance() == MemoryWriteOperation::Step::NeedUi, "first yield is UI");
        Check(f.store.calls.empty(), "UI yield precedes all IO");
        const auto serial = operation.RequestSerial();
        Check(!operation.AnswerApproval(serial, ApprovalAnswer::RestOfBatch), "wrong kind answer rejected");
        Check(!operation.AnswerUi(serial + 1, true), "wrong serial rejected");
        Check(operation.Advance() == MemoryWriteOperation::Step::NeedUi, "await remains stable");
        Check(operation.Result().audit.size() == 1 && f.store.calls.empty(), "repeated await has no effects");
        Check(operation.AnswerUi(serial, true), "correct serial accepted");
        Check(operation.Advance() == MemoryWriteOperation::Step::Finished, "resume completes");
        Check(f.store.calls.size() == 9, "three prechecks plus write/read pairs");
        Check(f.store.calls[0].front() == 'R' && f.store.calls[1].front() == 'R'
            && f.store.calls[2].front() == 'R' && f.store.calls[3].front() == 'W', "all reads precede first write");
        Check(f.overlay.PendingByteCount() == 6, "worker never mutates GUI overlay");
        Check(operation.Result().verifiedBlocks.size() == 3, "all verified snapshots owned");

        for (const bool ownerLoss : { false, true })
        {
            Fixture stopped;
            MemoryWriteOperation pending(stopped.Input(), stopped.store, [&stopped] { return stopped.Current(); });
            Check(pending.Advance() == MemoryWriteOperation::Step::NeedUi, "stop awaits UI");
            if (ownerLoss) stopped.ownerAlive = false; else stopped.stopRequested = true;
            Check(pending.Advance() == MemoryWriteOperation::Step::Finished, "stop retires unanswered UI");
            Check(stopped.store.calls.empty() && pending.Result().interrupted, "stop before first mutation");
            Check(pending.Result().audit.size() == 2, "start+finish survive lost owner");
            Check(!pending.AnswerUi(pending.RequestSerial(), true), "late answer after stop rejected");
        }
        for (const bool ownerLoss : { false, true })
        {
            Fixture stopped;
            stopped.store.after = [&stopped, ownerLoss](const std::string& call) {
                if (call.front() == 'W') { if (ownerLoss) stopped.ownerAlive = false; else stopped.stopRequested = true; }
            };
            MemoryWriteOperation active(stopped.Input(true), stopped.store, [&stopped] { return stopped.Current(); });
            Check(active.Advance() == MemoryWriteOperation::Step::Finished, "loss during write completes cleanup");
            Check(stopped.store.calls.size() == 5 && stopped.store.calls.back().front() == 'R', "write always gets readback");
            Check(active.Result().report.blocksWritten == 1 && active.Result().report.bytesWritten == 2,
                "lost owner retains verified actual write");
            Check(active.Result().report.outcome == CommitOutcome::WriteFailed && active.Result().interrupted,
                "remaining writes stop at next safe boundary");
            Check(active.Result().audit.back().event == AuditEvent::CommitFinished, "lost owner retains final audit");
        }
        {
            Fixture stale;
            stale.store.after = [&stale](const std::string& call) {
                if (call == "R4104:2") stale.revisions.BumpContent();
            };
            MemoryWriteOperation active(stale.Input(true), stale.store, [&stale] { return stale.Current(); });
            Check(active.Advance() == MemoryWriteOperation::Step::Finished, "freshness before write dispatch");
            Check(stale.store.writes == 0 && active.Result().report.outcome == CommitOutcome::Stale,
                "change after final precheck blocks write");
        }
        {
            Script script;
            script.fault = Fault::ThrowWrite;
            Fixture uncertain(script);
            uncertain.store.after = [&uncertain](const std::string& call) { if (call.front() == 'W') uncertain.ownerAlive = false; };
            MemoryWriteOperation active(uncertain.Input(true), uncertain.store, [&uncertain] { return uncertain.Current(); });
            Check(active.Advance() == MemoryWriteOperation::Step::Finished, "throwing write is terminal receipt");
            Check(active.Result().writeOutcomeUnknown && active.Result().failedWriteReadbacks.size() == 1,
                "unknown outcome final observation retained");
            Check(uncertain.store.calls.back().front() == 'R' && active.Result().verifiedBlocks.empty(),
                "failed write observed without accepting pending");
            Check(uncertain.overlay.PendingByteCount() == 6 && active.Result().report.needsReread,
                "unknown outcome keeps entire pending overlay");
            Check(active.Result().audit.back().event == AuditEvent::CommitFinished, "exception has canonical finish");
        }
        {
            Fixture malformed;
            auto input = malformed.Input(true);
            input.blocks[1].address = input.blocks[0].address;
            MemoryWriteOperation invalid(std::move(input), malformed.store, [&malformed] { return malformed.Current(); });
            Check(invalid.Advance() == MemoryWriteOperation::Step::Finished && malformed.store.calls.empty(),
                "overlap rejected before IO");
        }
        {
            Fixture malformed;
            auto input = malformed.Input(true);
            input.blocks.front().address = (std::numeric_limits<std::uint64_t>::max)() - 1;
            input.blocks.resize(1);
            MemoryWriteOperation invalid(std::move(input), malformed.store, [&malformed] { return malformed.Current(); });
            Check(invalid.Advance() == MemoryWriteOperation::Step::Finished && malformed.store.calls.empty(),
                "overlay exclusive-end overflow rejected before IO");
        }
        {
            Fixture capture;
            auto input = capture.Input(true);
            MemoryWriteOperation frozen(input, capture.store, [&capture] { return capture.Current(); });
            input.blocks[0].after[0] = 1;
            Check(frozen.Advance() == MemoryWriteOperation::Step::Finished, "owned capture completes");
            Check(capture.store.memory[Base] == 91 && frozen.Input().blocks.front().after.front() == 91,
                "caller mutation cannot rewrite frozen payload");
            Check(capture.overlay.Stage(Base, { 7, 8 }) == StageStatus::Ok, "new GUI pending patch");
            const auto& verified = frozen.Result().verifiedBlocks.front();
            Check(capture.overlay.AcceptWrite(verified.block, verified.readBack) == AcceptWriteStatus::Ok,
                "projection checks can apply verified owned bytes");
            Check(capture.overlay.EffectiveByte(Base) == 7 && capture.overlay.BaselineByte(Base) == 91,
                "projection preserves new pending after and updates before");
        }
    }

    void BoundaryTests()
    {
        for (const auto stopAt : { std::string("R4096:2"), std::string("R4100:2"), std::string("R4104:2") })
        {
            Fixture f;
            f.store.after = [&f, stopAt](const std::string& call) { if (call == stopAt) f.stopRequested = true; };
            MemoryWriteOperation operation(f.Input(true), f.store, [&f] { return f.Current(); });
            Check(operation.Advance() == MemoryWriteOperation::Step::Finished, "precheck stop terminates");
            Check(f.store.writes == 0 && operation.Result().verifiedBlocks.empty(), "precheck stop issues zero writes");
            Check(operation.Result().interrupted && operation.Result().report.bytesWritten == 0,
                "precheck cancellation never claims mutation");
        }
        {
            Script script;
            script.approval = true;
            Fixture f(script);
            MemoryWriteOperation operation(f.Input(), f.store, [&f] { return f.Current(); });
            Check(operation.Advance() == MemoryWriteOperation::Step::NeedUi, "approval phase starts at UI");
            const auto uiSerial = operation.RequestSerial();
            Check(operation.AnswerUi(uiSerial, true), "UI arms worker resume");
            Check(operation.Advance() == MemoryWriteOperation::Step::NeedApproval, "backend yields approval");
            const auto approvalSerial = operation.RequestSerial();
            Check(approvalSerial != uiSerial && operation.PendingApproval().backendText == "backend approval required",
                "approval owns fresh serial and backend text");
            Check(!operation.AnswerApproval(uiSerial, ApprovalAnswer::RestOfBatch), "old UI serial cannot approve backend");
            Check(!operation.AnswerUi(approvalSerial, true), "UI answer cannot approve backend");
            const auto calls = f.store.calls;
            Check(operation.Advance() == MemoryWriteOperation::Step::NeedApproval && f.store.calls == calls,
                "waiting approval never repeats Write false");
            f.ownerAlive = false;
            Check(operation.Advance() == MemoryWriteOperation::Step::Finished, "owner loss retires unanswered approval");
            Check(operation.Result().report.bytesWritten == 0 && operation.Result().verifiedBlocks.empty(),
                "unanswered approval cannot mutate");
            Check(operation.Result().audit.size() == 3 && operation.Result().audit.back().event == AuditEvent::CommitFinished,
                "owner loss preserves actual UI and final audit only");
            Check(!operation.AnswerApproval(approvalSerial, ApprovalAnswer::RestOfBatch), "late approval rejected after retire");
        }
        {
            Script script;
            script.approval = true;
            Fixture f(script);
            f.store.after = [&f](const std::string& call) { if (call.front() == 'W') f.ownerAlive = false; };
            MemoryWriteOperation operation(f.Input(true), f.store, [&f] { return f.Current(); });
            Check(operation.Advance() == MemoryWriteOperation::Step::Finished, "owner lost on approval response ends admission");
            Check(operation.Result().report.approvalsAsked == 0 && f.store.writes == 1,
                "owner loss before yielding prompt asks no UI and never retries");
        }
        for (const bool source : { false, true })
        {
            Script script;
            script.approval = true;
            Fixture f(script);
            MemoryWriteOperation operation(f.Input(true), f.store, [&f] { return f.Current(); });
            Check(operation.Advance() == MemoryWriteOperation::Step::NeedApproval, "approval returned without waiting");
            const auto serial = operation.RequestSerial();
            Check(operation.AnswerApproval(serial, ApprovalAnswer::ThisBlockOnly), "approval answer returns without IO");
            f.store.after = [&f, source](const std::string& call) {
                if (call == "R4104:2") { if (source) f.revisions.BumpSource(); else f.revisions.BumpContent(); }
            };
            Check(operation.Advance() == MemoryWriteOperation::Step::Finished, "post-approval stale check completes");
            Check(f.store.writes == 1 && operation.Result().report.outcome == CommitOutcome::Stale,
                "revision change after remaining-before blocks approved retry");
        }
        {
            Script script;
            script.fault = Fault::Partial;
            Fixture f(script);
            f.store.after = [&f](const std::string& call) {
                if (call.front() == 'W') f.ownerAlive = false;
            };
            MemoryWriteOperation operation(f.Input(true), f.store, [&f] { return f.Current(); });
            Check(operation.Advance() == MemoryWriteOperation::Step::Finished, "failed write cleanup ignores lost owner");
            Check(operation.Result().failedWriteReadbacks.size() == 1 && f.store.calls.back().front() == 'R',
                "partial write final observation always retained");
            Check(operation.Result().report.bytesWritten == 1 && operation.Result().report.blocksWritten == 0,
                "partial persistence never becomes verification");
            Check(operation.Result().report.outcome == CommitOutcome::WriteFailed && f.overlay.PendingByteCount() == 6,
                "failed partial remains pending despite final observation");
        }
        {
            Script script;
            script.fault = Fault::Fail;
            Fixture f(script);
            f.store.after = [&f](const std::string& call) {
                if (call.front() == 'R' && f.store.writes != 0) throw std::runtime_error("final read fault");
            };
            MemoryWriteOperation operation(f.Input(true), f.store, [&f] { return f.Current(); });
            Check(operation.Advance() == MemoryWriteOperation::Step::Finished, "throwing finalread still finishes");
            Check(operation.Result().failedWriteReadbacks.size() == 1
                && !operation.Result().failedWriteReadbacks.front().observed.ok,
                "throwing finalread is owned failed evidence");
            Check(operation.Result().audit.back().event == AuditEvent::CommitFinished && operation.Result().report.bytesWritten == 1,
                "throwing finalread preserves original write receipt and audit");
        }
        {
            Fixture f;
            f.store.after = [](const std::string& call) { if (call.front() == 'R') throw std::runtime_error("read fault"); };
            MemoryWriteOperation operation(f.Input(true), f.store, [&f] { return f.Current(); });
            Check(operation.Advance() == MemoryWriteOperation::Step::Finished && f.store.writes == 0,
                "precheck exception fails closed without mutation");
            Check(operation.Result().report.outcome == CommitOutcome::TargetChanged
                && operation.Result().audit.back().event == AuditEvent::CommitFinished,
                "precheck exception retained as terminal canonical receipt");
        }
        {
            Fixture f;
            MemoryWriteOperation missing(f.Input(true), f.store, {});
            Check(missing.Advance() == MemoryWriteOperation::Step::Finished && f.store.calls.empty(),
                "missing synchronized current source fails closed");
        }
        {
            Fixture f;
            MemoryWriteOperation throwing(f.Input(true), f.store, []() -> WriteOperationCurrent {
                throw std::runtime_error("mirror unavailable");
            });
            Check(throwing.Advance() == MemoryWriteOperation::Step::Finished && f.store.calls.empty(),
                "throwing current source fails closed");
        }
    }

    void HeldWorkerTest()
    {
        Fixture f;
        const auto input = f.Input(true);
        const auto capturedCurrent = f.Current();
        std::atomic<bool> ownerAlive{ true };
        std::mutex mutex;
        std::condition_variable condition;
        int entered = 0;
        int released = 0;
        int firstReads = 0;
        f.store.after = [&](const std::string& call) {
            int stage = 0;
            if (call == "R4096:2") stage = ++firstReads == 1 ? 1 : 3;
            else if (call == "W4096:0") stage = 2;
            if (stage == 0) return;
            std::unique_lock lock(mutex);
            entered = stage;
            condition.notify_all();
            condition.wait(lock, [&] { return released >= stage; });
        };
        MemoryWriteOperation operation(input, f.store, [&] {
            auto current = capturedCurrent;
            current.ownerAlive = ownerAlive.load(std::memory_order_acquire);
            return current;
        });
        auto worker = std::async(std::launch::async, [&] { return operation.Advance(); });
        int heartbeats = 0;
        bool allReached = true;
        for (int stage = 1; stage <= 3; ++stage)
        {
            std::unique_lock lock(mutex);
            const bool reached = condition.wait_for(lock, std::chrono::seconds(2), [&] { return entered == stage; });
            if (!reached)
            {
                allReached = false;
                released = 3;
                condition.notify_all();
                break;
            }
            lock.unlock();
            // Model a separate event-loop heartbeat while a fake I/O call is
            // held on the worker. No widgets, event loop, or real I/O involved.
            for (int beat = 0; beat < 5; ++beat)
            {
                ++heartbeats;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (stage == 2) ownerAlive.store(false, std::memory_order_release);
            lock.lock();
            released = stage;
            condition.notify_all();
        }
        const auto step = worker.get();
        Check(allReached && heartbeats == 15, "heartbeat continues through held precheck/write/readback");
        Check(step == MemoryWriteOperation::Step::Finished && operation.Result().interrupted,
            "atomic owner-loss mirror stops at safe boundary");
        Check(operation.Result().verifiedBlocks.size() == 1 && f.store.writes == 1,
            "held write finishes readback and no next write starts");
        Check(operation.Result().audit.back().event == AuditEvent::CommitFinished,
            "worker result retains canonical journal after owner loss");
    }
}

int main()
{
    try
    {
        EquivalenceTests();
        ContinuationTests();
        BoundaryTests();
        HeldWorkerTest();
        std::cout << "PASS " << checks << " checks; ordered old/new audit and port transcripts\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
