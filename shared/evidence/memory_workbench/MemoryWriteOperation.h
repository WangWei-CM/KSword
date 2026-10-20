#pragma once

#include "MemoryWriteTransaction.h"

#include <functional>
#include <optional>

namespace ksword::memwb
{
    // Value-only capture made on the owner thread before accepting a write job.
    // This object never borrows the live overlay, session, revisions or a widget.
    struct FrozenWriteInput
    {
        MemoryTargetSession session;
        RevisionSnapshot revisions;
        std::uint64_t baseAddress = 0;
        std::uint64_t baselineSize = 0;
        std::vector<DiffBlock> blocks;
        bool uiConfirmSuppressed = false;
    };

    // The callback must read a synchronized, value-only mirror. In particular it
    // must not read a QObject, live SessionRevisions or the GUI pointer resolver.
    // Ports and their fragment validation own the actual target/chain checks.
    struct WriteOperationCurrent
    {
        MemoryTargetSession session;
        RevisionSnapshot revisions;
        bool stopRequested = false;
        bool ownerAlive = true;
    };

    struct VerifiedWriteBlock
    {
        DiffBlock block;
        std::vector<std::uint8_t> readBack;
    };

    // Additional evidence after a failed/throwing Write. It never promotes a
    // failed receipt to success or removes its pending patch, even if bytes match.
    struct FailedWriteReadback
    {
        std::uint64_t blockIndex = 0;
        bool writeOutcomeUnknown = false;
        AccessResult observed;
    };

    struct WriteOperationResult
    {
        CommitReport report;
        std::vector<VerifiedWriteBlock> verifiedBlocks;
        // Canonical receipts are owned here, independently of UI delivery.
        std::vector<AuditRecord> audit;
        std::vector<FailedWriteReadback> failedWriteReadbacks;
        bool interrupted = false;
        bool writeOutcomeUnknown = false;
    };

    // Worker-thread confined continuation of MemoryWriteTransaction::Commit.
    // The worker owns store and the synchronized current-state source for this
    // object's entire lifetime. Calls may move between worker invocations, but
    // must never overlap. The owner handles busy admission outside this class.
    //
    // Advance performs worker I/O until it needs a value-only confirmation, or
    // finishes. It never waits for UI, invokes UI callbacks or starts an event
    // loop. Deliver answers to the worker and call Advance again. Serial checks
    // reject duplicate/late answers without replaying a physical write.
    //
    // Each first Write has explicitApproval=false. Approval is only attached to
    // the retry just requested by the backend. RestOfBatch retains the old
    // canonical audit semantics: only answers actually asked of the user count.
    //
    // A started Write and its readback/recovery are one safe unit: stop/owner
    // loss is checked before further I/O admission, never between Write and its
    // readback. Failed/throwing writes get an extra final observation (the old
    // generic ByteStore/transaction did not do this). Its result is evidence,
    // not acceptance, rollback, or an automatic retry.
    class MemoryWriteOperation
    {
    public:
        enum class Step { NeedUi, NeedApproval, Finished };
        using State = MemoryWriteTransaction::State;
        using CurrentStateFn = std::function<WriteOperationCurrent()>;

        MemoryWriteOperation(FrozenWriteInput input, IByteStore& store, CurrentStateFn current);
        MemoryWriteOperation(const MemoryWriteOperation&) = delete;
        MemoryWriteOperation& operator=(const MemoryWriteOperation&) = delete;

        [[nodiscard]] Step Advance();
        [[nodiscard]] bool AnswerUi(std::uint64_t requestSerial, bool accepted);
        [[nodiscard]] bool AnswerApproval(std::uint64_t requestSerial, ApprovalAnswer answer);

        [[nodiscard]] const FrozenWriteInput& Input() const noexcept { return input_; }
        [[nodiscard]] const WriteOperationResult& Result() const noexcept { return result_; }
        [[nodiscard]] const UiConfirmRequest& UiRequest() const noexcept { return uiRequest_; }
        [[nodiscard]] const ApprovalRequest& PendingApproval() const noexcept { return approvalRequest_; }
        [[nodiscard]] std::uint64_t RequestSerial() const noexcept { return requestSerial_; }
        [[nodiscard]] State CurrentState() const noexcept { return state_; }
        [[nodiscard]] bool IsFinished() const noexcept { return phase_ == Phase::Finished; }

    private:
        enum class Phase { Start, AwaitUi, AfterUi, Write, AwaitApproval, AfterApproval, Finished };
        bool Boundary(bool checkFreshness = true);
        bool Precheck(std::size_t firstBlock);
        bool WriteBlock(bool approvedRetry);
        bool VerifyBlock();
        void ObserveFailedWrite(bool unknown);
        void Absorb(const AccessResult& access);
        void Abort(CommitOutcome outcome, const std::string& text, bool failure, bool rolledBack = false);
        Step Finish();
        void RecordConfirm(AuditEvent event, const std::string& text);
        void RecordApproval(ApprovalAnswer answer);
        bool ValidBlocks() const;

        const FrozenWriteInput input_;
        IByteStore& store_;
        CurrentStateFn current_;
        const std::string targetIdentity_;
        WriteOperationResult result_;
        UiConfirmRequest uiRequest_;
        ApprovalRequest approvalRequest_;
        std::uint64_t requestSerial_ = 0;
        std::size_t blockIndex_ = 0;
        bool batchApproved_ = false;
        Phase phase_ = Phase::Start;
        State state_ = State::Idle;
    };
}
