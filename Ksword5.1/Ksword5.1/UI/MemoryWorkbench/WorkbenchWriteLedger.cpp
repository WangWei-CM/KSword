#include "WorkbenchWriteLedger.h"

#include <algorithm>
#include <utility>

namespace ks::ui
{
    namespace mem = ksword::memwb;
    namespace
    {
        struct Registry
        {
            std::mutex mutex;
            std::uint64_t next = 0;
            std::vector<std::shared_ptr<WorkbenchWriteLedger>> ledgers;
        };
        Registry& ProcessRegistry()
        {
            // Deliberate process lifetime. No global destruction ordering can
            // drop accepted receipts while another worker is finishing cleanup.
            static auto* registry = new Registry;
            return *registry;
        }
        bool SameReplay(const mem::JournalReplay& a, const mem::JournalReplay& b)
        { return a.address == b.address && a.expectedCurrent == b.expectedCurrent && a.restore == b.restore; }
    }

    WorkbenchWriteLedger::WorkbenchWriteLedger(const std::uint64_t id,
        const mem::MemoryTargetSession& session, std::shared_ptr<mem::IAuditSink> archive)
        : id_(id), session_(session), archive_(std::move(archive)) { journal_.BindTarget(session_); }

    std::shared_ptr<WorkbenchWriteLedger> WorkbenchWriteLedger::CreatePersistent(
        const mem::MemoryTargetSession& session, std::shared_ptr<mem::IAuditSink> archive)
    {
        auto& registry = ProcessRegistry();
        std::lock_guard lock(registry.mutex);
        auto ledger = std::shared_ptr<WorkbenchWriteLedger>(
            new WorkbenchWriteLedger(++registry.next, session, std::move(archive)));
        registry.ledgers.push_back(ledger);
        return ledger;
    }

    std::vector<std::shared_ptr<WorkbenchWriteLedger>> WorkbenchWriteLedger::RetainedLedgers()
    {
        auto& registry = ProcessRegistry();
        std::lock_guard lock(registry.mutex);
        return registry.ledgers;
    }

    std::uint64_t WorkbenchWriteLedger::NextOperation()
    { std::lock_guard lock(mutex_); return ++operation_; }

    void WorkbenchWriteLedger::Record(const mem::AuditRecord& record)
    {
        std::lock_guard lock(mutex_);
        // Archive order equals canonical receipt order. The owned production
        // sink never invokes GUI callbacks. If an injected sink throws, retain
        // the canonical value and let the caller decide how to surface failure.
        audit_.push_back({static_cast<std::uint64_t>(audit_.size()) + 1, record});
        auditCount_.store(audit_.size(), std::memory_order_release);
        if (archive_) archive_->Record(record);
    }

    WorkbenchWriteLedger::RecordResult WorkbenchWriteLedger::Record(const VerifiedWriteReceipt& receipt)
    {
        std::lock_guard lock(mutex_);
        RecordResult result;
        const auto& block = receipt.block;
        if (receipt.operation == 0 || block.before.empty() || block.before.size() != block.after.size()
            || receipt.readBack != block.after) return result;
        const auto key = std::make_pair(receipt.operation, receipt.blockIndex);
        const auto found = verifiedIndex_.find(key);
        if (found != verifiedIndex_.end())
        {
            const auto& old = verified_[found->second];
            result.duplicate = old.block.address == block.address && old.block.before == block.before
                && old.block.after == block.after && old.readBack == receipt.readBack
                && old.historyReplay == receipt.historyReplay;
            result.accepted = result.duplicate;
            return result;
        }
        // Keep complete accepted evidence even when the existing bounded undo
        // journal cannot retain a very large block or evicts older UI history.
        const auto slot = verifiedIndex_.emplace(key, verified_.size()).first;
        try { verified_.push_back(receipt); }
        catch (...) { verifiedIndex_.erase(slot); throw; }
        if (!receipt.historyReplay)
        {
            // Invalidate outstanding tickets before a possibly allocating
            // journal operation. Accepted evidence is already durable above.
            ++revision_;
            try { result.journal = journal_.Record(block.address, block.before, block.after, receipt.tick); }
            catch (...)
            {
                result.journal.historyCleared = journal_.UndoCount() != 0 || journal_.RedoCount() != 0;
                result.journalFailed = true;
                journal_.Clear();
            }
        }
        result.accepted = true;
        PublishSummaryLocked();
        return result;
    }

    WorkbenchWriteLedger::Summary WorkbenchWriteLedger::SummaryLocked() const
    { return {audit_.size(), verified_.size(), journal_.UndoCount(), journal_.RedoCount()}; }

    WorkbenchWriteLedger::Summary WorkbenchWriteLedger::GetSummary() const
    {
        return {auditCount_.load(std::memory_order_acquire), verifiedCount_.load(std::memory_order_acquire),
            undoCount_.load(std::memory_order_acquire), redoCount_.load(std::memory_order_acquire)};
    }

    void WorkbenchWriteLedger::PublishSummaryLocked() noexcept
    {
        auditCount_.store(audit_.size(), std::memory_order_release);
        verifiedCount_.store(verified_.size(), std::memory_order_release);
        undoCount_.store(journal_.UndoCount(), std::memory_order_release);
        redoCount_.store(journal_.RedoCount(), std::memory_order_release);
    }

    WorkbenchWriteLedger::Snapshot WorkbenchWriteLedger::TakeSnapshot() const
    { std::lock_guard lock(mutex_); return {session_, audit_, verified_, SummaryLocked()}; }

    std::optional<WorkbenchWriteLedger::ReplayTicket> WorkbenchWriteLedger::PeekLocked(const bool undo) const
    {
        const auto replay = undo ? journal_.PeekUndo() : journal_.PeekRedo();
        if (!replay) return {};
        return ReplayTicket{id_, revision_, undo, *replay};
    }
    std::optional<WorkbenchWriteLedger::ReplayTicket> WorkbenchWriteLedger::PeekUndo() const
    { std::unique_lock lock(mutex_, std::try_to_lock); return lock ? PeekLocked(true) : std::nullopt; }
    std::optional<WorkbenchWriteLedger::ReplayTicket> WorkbenchWriteLedger::PeekRedo() const
    { std::unique_lock lock(mutex_, std::try_to_lock); return lock ? PeekLocked(false) : std::nullopt; }

    bool WorkbenchWriteLedger::FinishReplay(const ReplayTicket& ticket, const mem::CommitReport& report)
    {
        std::lock_guard lock(mutex_);
        std::uint64_t changedBytes = 0;
        std::uint64_t changedRuns = 0;
        bool inRun = false;
        if (ticket.replay.restore.size() != ticket.replay.expectedCurrent.size()) return false;
        for (std::size_t i = 0; i < ticket.replay.restore.size(); ++i)
        {
            const bool changed = ticket.replay.restore[i] != ticket.replay.expectedCurrent[i];
            if (changed) { ++changedBytes; if (!inRun) ++changedRuns; }
            inRun = changed;
        }
        if (ticket.ledger != id_ || ticket.revision != revision_
            || report.outcome != mem::CommitOutcome::Committed || changedBytes == 0
            || report.blocksWritten != changedRuns || report.bytesWritten != changedBytes) return false;
        const auto current = PeekLocked(ticket.undo);
        if (!current || !SameReplay(current->replay, ticket.replay)) return false;
        const bool moved = ticket.undo ? journal_.MarkUndone() : journal_.MarkRedone();
        if (moved) { ++revision_; PublishSummaryLocked(); }
        return moved;
    }
}
