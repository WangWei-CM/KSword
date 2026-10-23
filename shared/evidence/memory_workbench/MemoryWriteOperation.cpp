#include "MemoryWriteOperation.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace ksword::memwb
{
    namespace
    {
        bool NeedsReread(const CommitReport& report)
        {
            switch (report.outcome)
            {
            case CommitOutcome::WriteFailed:
            case CommitOutcome::VerifyMismatch:
            case CommitOutcome::TargetChanged:
            case CommitOutcome::Stale:
                return true;
            case CommitOutcome::ApprovalDenied:
                return report.bytesWritten != 0;
            default:
                return false;
            }
        }

        std::string ExceptionText(const char* context)
        {
            try { throw; }
            catch (const std::exception& error) { return std::string(context) + error.what(); }
            catch (...) { return std::string(context) + "unknown exception"; }
        }
    }

    MemoryWriteOperation::MemoryWriteOperation(
        FrozenWriteInput input, IByteStore& store, CurrentStateFn current)
        : input_(std::move(input)), store_(store), current_(std::move(current)),
          targetIdentity_(IdentityKey(input_.session, input_.baseAddress, input_.baselineSize))
    {
    }

    bool MemoryWriteOperation::ValidBlocks() const
    {
        std::uint64_t previousEnd = 0;
        std::uint64_t total = 0;
        bool first = true;
        for (const DiffBlock& block : input_.blocks)
        {
            const auto length = static_cast<std::uint64_t>(block.after.size());
            if (length == 0 || block.before.size() != block.after.size()
                || block.address > (std::numeric_limits<std::uint64_t>::max)() - length
                || (!first && block.address <= previousEnd)
                || total > (std::numeric_limits<std::uint64_t>::max)() - length)
                return false;
            total += length;
            previousEnd = block.address + length - 1;
            first = false;
        }
        return true;
    }

    void MemoryWriteOperation::Absorb(const AccessResult& access)
    {
        result_.report.scratchAreaDirty |= access.scratchAreaDirty;
        result_.report.readModifyWriteWindow |= access.readModifyWriteWindow;
    }

    void MemoryWriteOperation::Abort(
        const CommitOutcome outcome, const std::string& text, const bool failure, const bool rolledBack)
    {
        result_.report.outcome = outcome;
        result_.report.failureText = text;
        state_ = failure ? (rolledBack ? State::RolledBack : State::Failed)
            : (result_.report.bytesWritten != 0 ? State::Failed : State::Staged);
    }

    bool MemoryWriteOperation::Boundary(const bool checkFreshness)
    {
        WriteOperationCurrent now;
        try
        {
            if (!current_)
            {
                Abort(CommitOutcome::Stale, "current target state is unavailable", false);
                return false;
            }
            now = current_();
        }
        catch (...)
        {
            Abort(CommitOutcome::Stale, ExceptionText("current target state failed: "), false);
            return false;
        }
        if (now.stopRequested || !now.ownerAlive)
        {
            result_.interrupted = true;
            Abort(result_.report.bytesWritten != 0 ? CommitOutcome::WriteFailed : CommitOutcome::UserCancelled,
                now.ownerAlive ? "write operation stopped at a safe boundary"
                    : "write operation owner was lost at a safe boundary",
                result_.report.bytesWritten != 0);
            return false;
        }
        if (!checkFreshness) return true;
        if (!SameTarget(input_.session, now.session))
        {
            Abort(CommitOutcome::Stale, "target session changed while waiting for confirmation", false);
            return false;
        }
        if (input_.revisions != now.revisions)
        {
            Abort(CommitOutcome::Stale, now.revisions.source != input_.revisions.source
                ? "source revision changed while waiting for confirmation"
                : "content revision changed while waiting for confirmation", false);
            return false;
        }
        return true;
    }

    bool MemoryWriteOperation::Precheck(const std::size_t firstBlock)
    {
        for (std::size_t index = firstBlock; index < input_.blocks.size(); ++index)
        {
            if (!Boundary()) return false;
            const DiffBlock& block = input_.blocks[index];
            AccessResult current;
            try { current = store_.Read(block.address, static_cast<std::uint64_t>(block.before.size())); }
            catch (...)
            {
                Abort(CommitOutcome::TargetChanged, ExceptionText("pre-write re-read failed: "), false);
                return false;
            }
            Absorb(current);
            if (!current.ok)
            {
                Abort(CommitOutcome::TargetChanged, "pre-write re-read failed: " + current.failureText, false);
                return false;
            }
            if (current.partial)
            {
                Abort(CommitOutcome::TargetChanged, "pre-write re-read was partial", false);
                return false;
            }
            if (current.data != block.before)
            {
                Abort(CommitOutcome::TargetChanged, "target bytes differ from the staged original bytes", false);
                return false;
            }
        }
        return true;
    }

    void MemoryWriteOperation::ObserveFailedWrite(const bool unknown)
    {
        FailedWriteReadback observation;
        observation.blockIndex = static_cast<std::uint64_t>(blockIndex_);
        observation.writeOutcomeUnknown = unknown;
        result_.writeOutcomeUnknown |= unknown;
        const DiffBlock& block = input_.blocks[blockIndex_];
        // Never gate this read on owner/stop/current: a write has already begun.
        try { observation.observed = store_.Read(block.address, static_cast<std::uint64_t>(block.after.size())); }
        catch (...) { observation.observed.failureText = ExceptionText("final read-back failed: "); }
        Absorb(observation.observed);
        result_.failedWriteReadbacks.push_back(std::move(observation));
    }

    bool MemoryWriteOperation::WriteBlock(const bool approvedRetry)
    {
        if (!Boundary()) return false;
        state_ = State::Writing;
        const DiffBlock& block = input_.blocks[blockIndex_];
        const auto length = static_cast<std::uint64_t>(block.after.size());
        const auto persistedBefore = result_.report.bytesWritten;
        AccessResult access;
        try { access = store_.Write(block.address, block.after, approvedRetry); }
        catch (...)
        {
            const std::string failure = ExceptionText("write failed: ");
            ObserveFailedWrite(true);
            Abort(CommitOutcome::WriteFailed, failure, true);
            return false;
        }
        Absorb(access);
        if (access.needsExplicitApproval)
        {
            if (approvedRetry)
            {
                if (!access.rolledBack) result_.report.bytesWritten += (std::min)(access.bytesDone, length);
                ObserveFailedWrite(false);
                Abort(CommitOutcome::WriteFailed, "backend requested approval again after it was granted", true);
                return false;
            }
            if (!access.rolledBack && access.bytesDone != 0)
            {
                result_.report.bytesWritten += (std::min)(access.bytesDone, length);
                ObserveFailedWrite(false);
                Abort(CommitOutcome::WriteFailed,
                    "backend requested approval after persisting part of the block", true);
                return false;
            }
            if (batchApproved_) return WriteBlock(true);
            if (!Boundary()) return false;
            approvalRequest_.targetIdentity = targetIdentity_;
            approvalRequest_.blockIndex = static_cast<std::uint64_t>(blockIndex_);
            approvalRequest_.blocksTotal = result_.report.blocksTotal;
            approvalRequest_.address = block.address;
            approvalRequest_.length = length;
            approvalRequest_.backendText = access.failureText;
            ++result_.report.approvalsAsked;
            ++requestSerial_;
            phase_ = Phase::AwaitApproval;
            return true;
        }
        if (!access.ok || access.partial)
        {
            if (!access.rolledBack) result_.report.bytesWritten += (std::min)(access.bytesDone, length);
            ObserveFailedWrite(false);
            Abort(CommitOutcome::WriteFailed, "write failed: " + access.failureText, true,
                access.rolledBack && persistedBefore == 0);
            return false;
        }
        // Preserve the authoritative receipt semantics: successful writes count
        // the entire block; only a successful, complete readback verifies it.
        result_.report.bytesWritten += length;
        return VerifyBlock();
    }

    bool MemoryWriteOperation::VerifyBlock()
    {
        state_ = State::Verifying;
        const DiffBlock& block = input_.blocks[blockIndex_];
        AccessResult readBack;
        // This is part of the started write, even if its owner disappeared.
        try { readBack = store_.Read(block.address, static_cast<std::uint64_t>(block.after.size())); }
        catch (...) { readBack.failureText = ExceptionText("read-back failed: "); }
        Absorb(readBack);
        if (!readBack.ok || readBack.partial || readBack.data != block.after)
        {
            Abort(CommitOutcome::VerifyMismatch, "read-back verification failed: " + readBack.failureText, true);
            return false;
        }
        result_.verifiedBlocks.push_back({ block, std::move(readBack.data) });
        ++result_.report.blocksWritten;
        ++blockIndex_;
        phase_ = Phase::Write;
        return true;
    }

    void MemoryWriteOperation::RecordConfirm(const AuditEvent event, const std::string& text)
    {
        AuditRecord record;
        record.event = event;
        record.targetIdentity = targetIdentity_;
        record.blocksTotal = result_.report.blocksTotal;
        record.text = text;
        result_.audit.push_back(std::move(record));
    }

    void MemoryWriteOperation::RecordApproval(const ApprovalAnswer answer)
    {
        AuditRecord record;
        record.event = AuditEvent::ApprovalAnswered;
        record.targetIdentity = targetIdentity_;
        record.blocksTotal = result_.report.blocksTotal;
        record.blockIndex = approvalRequest_.blockIndex;
        record.address = approvalRequest_.address;
        record.length = approvalRequest_.length;
        record.approvalAnswer = answer;
        record.text = "explicit approval answered";
        result_.audit.push_back(std::move(record));
    }

    MemoryWriteOperation::Step MemoryWriteOperation::Finish()
    {
        if (phase_ != Phase::Finished)
        {
            result_.report.needsReread = NeedsReread(result_.report);
            AuditRecord record;
            record.event = AuditEvent::CommitFinished;
            record.targetIdentity = targetIdentity_;
            record.blocksTotal = result_.report.blocksTotal;
            record.outcome = result_.report.outcome;
            record.blocksWritten = result_.report.blocksWritten;
            record.bytesWritten = result_.report.bytesWritten;
            record.scratchAreaDirty = result_.report.scratchAreaDirty;
            record.readModifyWriteWindow = result_.report.readModifyWriteWindow;
            record.text = result_.report.failureText;
            result_.audit.push_back(std::move(record));
            phase_ = Phase::Finished;
        }
        return Step::Finished;
    }

    bool MemoryWriteOperation::AnswerUi(const std::uint64_t serial, const bool accepted)
    {
        if (phase_ != Phase::AwaitUi || serial != requestSerial_) return false;
        RecordConfirm(accepted ? AuditEvent::UiConfirmAccepted : AuditEvent::UiConfirmDenied,
            accepted ? "user accepted the write confirmation" : "user declined the write confirmation");
        if (!accepted)
        {
            Abort(CommitOutcome::UserCancelled, "user declined the write confirmation", false);
            Finish();
        }
        else phase_ = Phase::AfterUi;
        return true;
    }

    bool MemoryWriteOperation::AnswerApproval(const std::uint64_t serial, const ApprovalAnswer answer)
    {
        if (phase_ != Phase::AwaitApproval || serial != requestSerial_) return false;
        RecordApproval(answer);
        if (answer == ApprovalAnswer::RestOfBatch) batchApproved_ = true;
        else if (answer != ApprovalAnswer::ThisBlockOnly)
        {
            Abort(CommitOutcome::ApprovalDenied,
                "user denied the explicit approval requested by the backend", true);
            Finish();
            return true;
        }
        phase_ = Phase::AfterApproval;
        return true;
    }

    MemoryWriteOperation::Step MemoryWriteOperation::Advance()
    {
        if (phase_ == Phase::Finished) return Step::Finished;
        if (phase_ == Phase::Start)
        {
            AuditRecord started;
            started.event = AuditEvent::CommitStarted;
            started.targetIdentity = targetIdentity_;
            started.text = "commit started";
            result_.audit.push_back(std::move(started));
            result_.report.blocksTotal = static_cast<std::uint64_t>(input_.blocks.size());
            if (input_.blocks.empty()) return Finish();
            const SessionError error = Validate(input_.session);
            if (error != SessionError::None)
            {
                result_.report.sessionError = error;
                result_.report.outcome = CommitOutcome::InvalidSession;
                result_.report.failureText = "target session is not self-consistent";
                return Finish();
            }
            if (!ValidBlocks())
            {
                Abort(CommitOutcome::WriteFailed, "frozen write blocks are invalid or overlap", true);
                return Finish();
            }
            if (!Boundary(false)) return Finish();
            state_ = State::ConfirmPending;
            if (!input_.uiConfirmSuppressed)
            {
                uiRequest_.targetIdentity = targetIdentity_;
                uiRequest_.blocksTotal = result_.report.blocksTotal;
                for (const DiffBlock& block : input_.blocks)
                {
                    const auto length = static_cast<std::uint64_t>(block.after.size());
                    uiRequest_.blocks.push_back({ block.address, length });
                    uiRequest_.bytesTotal += length;
                }
                ++requestSerial_;
                phase_ = Phase::AwaitUi;
                return Step::NeedUi;
            }
            RecordConfirm(AuditEvent::UiConfirmSuppressed, "ui confirmation suppressed by setting");
            phase_ = Phase::AfterUi;
        }
        if (phase_ == Phase::AwaitUi || phase_ == Phase::AwaitApproval)
        {
            // A no-answer resume also retires stale prompts. It must not
            // synthesize a user answer merely to release an invalidated job.
            if (!Boundary()) return Finish();
            return phase_ == Phase::AwaitUi ? Step::NeedUi : Step::NeedApproval;
        }
        if (phase_ == Phase::AfterUi)
        {
            if (!Boundary() || !Precheck(0)) return Finish();
            phase_ = Phase::Write;
        }
        if (phase_ == Phase::AfterApproval)
        {
            if (!Boundary() || !Precheck(blockIndex_)) return Finish();
            if (!WriteBlock(true)) return Finish();
        }
        while (phase_ == Phase::Write && blockIndex_ < input_.blocks.size())
        {
            if (!WriteBlock(false)) return Finish();
        }
        if (phase_ == Phase::AwaitApproval) return Step::NeedApproval;
        state_ = State::Committed;
        result_.report.outcome = CommitOutcome::Committed;
        return Finish();
    }
}
