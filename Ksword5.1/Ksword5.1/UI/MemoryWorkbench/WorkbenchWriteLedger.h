#pragma once

#include "../../../../shared/evidence/memory_workbench/MemoryEditJournal.h"
#include "../../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

#include <memory>
#include <map>
#include <mutex>
#include <atomic>

namespace ks::ui
{
    struct VerifiedWriteReceipt
    {
        std::uint64_t operation = 0;
        std::uint64_t blockIndex = 0;
        ksword::memwb::DiffBlock block;
        std::vector<std::uint8_t> readBack;
        std::uint64_t tick = 0;
        bool historyReplay = false;
    };

    class WorkbenchWriteLedger final : public ksword::memwb::IAuditSink
    {
    public:
        struct AuditReceipt
        {
            std::uint64_t sequence = 0;
            ksword::memwb::AuditRecord record;
        };
        struct ReplayTicket
        {
            std::uint64_t ledger = 0;
            std::uint64_t revision = 0;
            bool undo = true;
            ksword::memwb::JournalReplay replay;
        };
        struct Summary
        {
            std::size_t auditCount = 0;
            std::size_t verifiedCount = 0;
            std::size_t undoCount = 0;
            std::size_t redoCount = 0;
        };
        struct Snapshot
        {
            ksword::memwb::MemoryTargetSession session;
            std::vector<AuditReceipt> audit;
            std::vector<VerifiedWriteReceipt> verified;
            Summary summary;
        };
        struct RecordResult
        {
            bool accepted = false;
            bool duplicate = false;
            bool journalFailed = false;
            ksword::memwb::JournalRecordResult journal;
        };

        // The process registry retains every ledger, including after its GUI
        // and accepted operation owners vanish. The optional sink must be an
        // independently owned thread-safe archive, never AuditSinkForwarder.
        static std::shared_ptr<WorkbenchWriteLedger> CreatePersistent(
            const ksword::memwb::MemoryTargetSession& session,
            std::shared_ptr<ksword::memwb::IAuditSink> archive = {});
        static std::vector<std::shared_ptr<WorkbenchWriteLedger>> RetainedLedgers();
        std::uint64_t Id() const noexcept { return id_; }
        std::uint64_t NextOperation();
        const ksword::memwb::MemoryTargetSession& Session() const noexcept { return session_; }
        void Record(const ksword::memwb::AuditRecord& record) override;
        RecordResult Record(const VerifiedWriteReceipt& receipt);
        // Nonblocking approximate counters for GUI enablement; exact history
        // decisions use the independently checked replay ticket below.
        Summary GetSummary() const;
        // Full immutable archive copy; call on a worker, never in GUI refresh.
        Snapshot TakeSnapshot() const;
        // Nonblocking; a busy ledger returns nullopt without changing history.
        std::optional<ReplayTicket> PeekUndo() const;
        std::optional<ReplayTicket> PeekRedo() const;
        // Advance the cursor only for the exact still-current ticket and a
        // completely verified replay, including merged spans whose unchanged
        // bytes split the actual diff into several runs. Failed writes keep it.
        bool FinishReplay(const ReplayTicket& ticket, const ksword::memwb::CommitReport& report);

    private:
        WorkbenchWriteLedger(std::uint64_t id, const ksword::memwb::MemoryTargetSession& session,
            std::shared_ptr<ksword::memwb::IAuditSink> archive);
        Summary SummaryLocked() const;
        void PublishSummaryLocked() noexcept;
        std::optional<ReplayTicket> PeekLocked(bool undo) const;
        const std::uint64_t id_;
        const ksword::memwb::MemoryTargetSession session_;
        const std::shared_ptr<ksword::memwb::IAuditSink> archive_;
        mutable std::mutex mutex_;
        std::uint64_t revision_ = 0;
        std::uint64_t operation_ = 0;
        std::atomic<std::size_t> auditCount_{0};
        std::atomic<std::size_t> verifiedCount_{0};
        std::atomic<std::size_t> undoCount_{0};
        std::atomic<std::size_t> redoCount_{0};
        std::vector<AuditReceipt> audit_;
        std::vector<VerifiedWriteReceipt> verified_;
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> verifiedIndex_;
        // Existing user-facing undo limits/coalescing stay unchanged. Canonical
        // verified/audit receipts above have no cap and are never evicted.
        ksword::memwb::MemoryEditJournal journal_;
    };
}
