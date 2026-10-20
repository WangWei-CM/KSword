#pragma once

#include "WorkbenchTarget.h"
#include "WorkbenchFrozenWriteContext.h"
#include "WorkbenchWriteLedger.h"
#include "WorkbenchWriteOperationRunner.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>

class QTimer;

namespace ks::ui
{
    class AsyncOperation;
    class WorkbenchUndoCoordinator;

    // Pending means accepted, with the real report supplied by the continuation.
    // No caller may use a synchronous placeholder report to approve leaving.
    enum class CommitEntryStatus { Started, Pending, Busy, Rejected };
    struct CommitAttempt
    {
        CommitEntryStatus status = CommitEntryStatus::Rejected;
        ksword::memwb::CommitReport report;
        std::uint64_t operation = 0;
    };

    // GUI admission/projection only. Each accepted operation owns its value
    // session, ports, transaction and durable journal on the worker. No worker
    // borrows the overlay, target, confirmation sink or a GUI pointer resolver.
    class WorkbenchWriteController final : public QObject
    {
        Q_OBJECT
    public:
        using IoPortFactory = FrozenWriteFactories::IoFactory;
        using KernelPortFactory = FrozenWriteFactories::KernelFactory;
        using RereadRangeFn = std::function<void(std::uint64_t, std::uint64_t)>;
        using TickProviderFn = std::function<std::uint64_t()>;
        using PendingStageTicket = std::uint64_t;
        using CanvasReadOnlyHook = std::function<void(bool)>;
        using CommitSuspendHook = std::function<void(bool)>;
        using CommitCompletion = std::function<void(const ksword::memwb::CommitReport&)>;
        using ModeSwitchCompletion = std::function<void(const ksword::memwb::ModeSwitchResult&)>;
        using FrozenCaptureFn = std::function<bool(FrozenWriteCapture&, std::string&)>;
        using FrozenBindingsFn = std::function<void(const ksword::memwb::PointerChainBindings&)>;
        using WriteValidationFn = ksword::memwb::MemoryIoByteStore::WriteValidationFn;
        using DiagnosticFormatter = std::function<std::string(const std::string&)>;

        explicit WorkbenchWriteController(QObject* parent = nullptr);
        ~WorkbenchWriteController() override;
        void setOverlay(ksword::memwb::MemoryDiffOverlay*);
        void setTarget(WorkbenchTarget*);
        void setConfirmationSink(ksword::memwb::IConfirmationSink*);
        // Compatibility assembly setters. They never make a borrowed sink or
        // GUI validation closure eligible for worker use. Production must set
        // independently owned factories/archive and the value capture callback.
        void setAuditSink(ksword::memwb::IAuditSink*);
        void setIoPortFactory(IoPortFactory);
        void setKernelMutationPortFactory(KernelPortFactory);
        void setWriteValidationCallback(WriteValidationFn);
        void setFrozenWriteFactories(FrozenWriteFactories);
        void setOwnedAuditArchive(std::shared_ptr<ksword::memwb::IAuditSink>);
        void setFrozenWriteCaptureCallback(FrozenCaptureFn);
        void setFrozenPointerBindingsCallback(FrozenBindingsFn);
        void setConfirmationReadyCallback(std::function<bool()>);
        void setDiagnosticFormatter(DiagnosticFormatter);
        void resumePendingConfirmation();
        void invalidateWriteAuthorization();
        void setRereadRangeCallback(RereadRangeFn);
        void setTickProvider(TickProviderFn);
        void setCanvasReadOnlyHook(CanvasReadOnlyHook);
        void setCommitSuspendHook(CommitSuspendHook);

        ksword::memwb::WriteMode mode() const;
        ksword::memwb::ModeSwitchStatus requestModeSwitch(ksword::memwb::WriteMode);
        CommitEntryStatus resolveModeSwitch(ksword::memwb::ModeSwitchDecision,
            ModeSwitchCompletion = {});
        void setUiConfirmSuppressed(bool);
        bool uiConfirmSuppressed() const;
        void onEditCompleted();
        CommitAttempt commitPendingNow(CommitCompletion = {});
        bool isCommitting() const;
        bool isHistoryReplay() const noexcept { return historyReplay_; }
        bool lastCommitProjectionMatched() const noexcept { return lastProjectionMatched_; }
        bool canUndo() const;
        bool canRedo() const;
        CommitEntryStatus undo(CommitCompletion = {});
        CommitEntryStatus redo(CommitCompletion = {});

        PendingStageTicket beginPendingStage(std::uint64_t, const QByteArray&);
        void cancelPendingStage(PendingStageTicket);
        void notifyWindowMayCover();
        void FinalizePendingStageForTest(PendingStageTicket ticket, bool ok, const QString& reason)
        { FinalizePendingStage(ticket, ok, reason); }

    signals:
        void commitFinished(ksword::memwb::CommitReport report);
        void commitFailed(ksword::memwb::CommitReport report);
        void scratchAreaDirtyReported();
        void pendingPatchesChanged(quint64 bytesPending, quint64 blocksPending);
        // This ticket confirms staging into the overlay, never a physical write.
        void pendingStageResolved(PendingStageTicket ticket, bool ok, QString reason);
        void undoRedoAvailabilityChanged();
        void commitRejectedBusy(QString source);

    private:
        struct PendingStageEntry
        {
            std::uint64_t address = 0;
            std::vector<std::uint8_t> bytes;
            QPointer<WorkbenchTarget> target;
            std::uint64_t sourceRevision = 0;
            QTimer* timer = nullptr;
        };
        struct PendingStageAttempt { bool resolved = false, ok = false; QString reason; };
        PendingStageAttempt TryStagePendingEntry(const PendingStageEntry&);
        void FinalizePendingStage(PendingStageTicket, bool, const QString&);
        CommitAttempt beginCommit(CommitCompletion,
            std::optional<WorkbenchWriteLedger::ReplayTicket> = {});
        CommitEntryStatus replay(bool undo, CommitCompletion);
        void dispatchPhase(const std::shared_ptr<WorkbenchWriteOperationRunner>&,
            WritePhaseAnswer = {});
        void consumePhase(const std::shared_ptr<WorkbenchWriteOperationRunner>&,
            const WritePhaseSnapshot&);
        void finishOperation(const std::shared_ptr<WorkbenchWriteOperationRunner>&,
            const WritePhaseSnapshot&);
        bool projectionMatches(const WorkbenchWriteOperationRunner&);
        void completeAfterPhase(CommitCompletion, ksword::memwb::CommitReport);
        void emitPending();
        bool updateLedger(const ksword::memwb::MemoryTargetSession&);

        ksword::memwb::MemoryDiffOverlay* overlay_ = nullptr; // GUI only, non-owning.
        QPointer<WorkbenchTarget> target_;
        ksword::memwb::IConfirmationSink* confirmation_ = nullptr; // GUI only.
        ksword::memwb::IAuditSink* legacyAudit_ = nullptr; // Never used on a worker.
        FrozenWriteFactories factories_;
        std::shared_ptr<ksword::memwb::IAuditSink> archive_;
        FrozenCaptureFn capture_;
        FrozenBindingsFn applyBindings_;
        std::function<bool()> confirmationReady_;
        DiagnosticFormatter diagnosticFormatter_;
        std::shared_ptr<FrozenWriteAuthorization> authorization_;
        std::shared_ptr<WorkbenchWriteLedger> ledger_;
        std::shared_ptr<WorkbenchWriteOperationRunner> operation_;
        std::optional<WritePhaseSnapshot> pendingPhase_;
        AsyncOperation* async_ = nullptr;
        QTimer* terminalPoll_ = nullptr;
        CommitCompletion completion_;
        RereadRangeFn rereadRangeCallback_;
        CanvasReadOnlyHook canvasReadOnlyHook_;
        CommitSuspendHook commitSuspendHook_;
        TickProviderFn tickProvider_;
        std::map<PendingStageTicket, PendingStageEntry> pendingStages_;
        PendingStageTicket pendingStageCounter_ = 0;
        ksword::memwb::WriteMode mode_ = ksword::memwb::WriteMode::Immediate;
        std::optional<ksword::memwb::WriteMode> pendingMode_;
        bool pendingUiConfirmSuppressed_ = false;
        bool dependenciesLocked_ = false;
        bool accepting_ = false;
        bool finishing_ = false; // Admission guard through external finish hooks/signals.
        std::uint64_t authorityRevision_ = 0;
        bool historyReplay_ = false;
        bool prompting_ = false;
        bool lastProjectionMatched_ = false;
    };
}
