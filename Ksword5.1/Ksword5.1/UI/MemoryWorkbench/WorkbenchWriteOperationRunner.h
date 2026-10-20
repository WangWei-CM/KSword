#pragma once

#include "WorkbenchFrozenWriteContext.h"
#include "WorkbenchWriteLedger.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWriteOperation.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ks::ui
{
    struct WritePhaseAnswer
    {
        enum class Kind { None, Ui, Approval };
        Kind kind = Kind::None;
        std::uint64_t operation = 0;
        std::uint64_t serial = 0;
        bool uiAccepted = false;
        ksword::memwb::ApprovalAnswer approval = ksword::memwb::ApprovalAnswer::Deny;
    };

    struct WritePhaseSnapshot
    {
        std::uint64_t operation = 0;
        std::uint64_t serial = 0;
        ksword::memwb::MemoryWriteOperation::Step step = ksword::memwb::MemoryWriteOperation::Step::Finished;
        ksword::memwb::UiConfirmRequest ui;
        ksword::memwb::ApprovalRequest approval;
        ksword::memwb::WriteOperationResult result;
        ksword::memwb::PointerChainBindings bindings;
        bool terminal = false;
        bool receiptFailed = false;
        std::string receiptFailure;
        std::vector<std::string> receiptFailures;
    };

    // No QObject or borrowed GUI state. The shared owner retains the frozen
    // context and shutdown lease across value-only confirmation continuations.
    class WorkbenchWriteOperationRunner final
        : public std::enable_shared_from_this<WorkbenchWriteOperationRunner>
    {
    public:
        static std::shared_ptr<WorkbenchWriteOperationRunner> Create(
            ksword::memwb::FrozenWriteInput input, FrozenWriteCapture capture,
            FrozenWriteFactories factories, std::shared_ptr<WorkbenchWriteLedger> ledger,
            std::optional<std::uint64_t> tick, std::optional<WorkbenchWriteLedger::ReplayTicket> replay,
            std::string& reason);
        ~WorkbenchWriteOperationRunner();
        WorkbenchWriteOperationRunner(const WorkbenchWriteOperationRunner&) = delete;
        WorkbenchWriteOperationRunner& operator=(const WorkbenchWriteOperationRunner&) = delete;

        WritePhaseSnapshot RunPhase(WritePhaseAnswer answer = {});
        // GUI polling never waits for a worker or I/O lock. Copies only an
        // already terminal value snapshot; allocation failure may propagate.
        std::optional<WritePhaseSnapshot> TryTerminalSnapshot() const;
        void RequestStop(bool ownerGone = false) noexcept;
        std::uint64_t Id() const noexcept;
        const ksword::memwb::FrozenWriteInput& Input() const noexcept;
        const FrozenWriteCapture& Capture() const noexcept;

    private:
        struct State;
        explicit WorkbenchWriteOperationRunner(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}
