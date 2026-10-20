#include "WorkbenchFrozenWriteContext.h"
#include "../../../../shared/evidence/memory_workbench/SessionAddressResolver.h"

#include <algorithm>
#include <utility>

namespace ks::ui
{
    namespace mem = ksword::memwb;
    namespace
    {
        std::atomic<bool>& WriteAdmission()
        {
            static auto* busy = new std::atomic<bool>{false};
            return *busy;
        }
    }

    FrozenWorkbenchWriteLease::~FrozenWorkbenchWriteLease()
    { if (ownsAdmission_) WriteAdmission().store(false, std::memory_order_release); }

    std::shared_ptr<FrozenWorkbenchWriteLease> TryAcquireWorkbenchWriteLease()
    {
        // Finish both allocations before acquiring admission. shared_ptr may
        // delete its raw token if control-block allocation fails; that inactive
        // token must not release a different writer's subsequently acquired gate.
        auto lease = std::shared_ptr<FrozenWorkbenchWriteLease>(new FrozenWorkbenchWriteLease);
        bool expected = false;
        if (!WriteAdmission().compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return {};
        lease->ownsAdmission_ = true;
        return lease;
    }

    bool FrozenWriteAuthorization::Allows(const std::uint64_t captured) const noexcept
    {
        return ownerAlive.load(std::memory_order_acquire)
            && !cancelled.load(std::memory_order_acquire)
            && epoch.load(std::memory_order_acquire) == captured;
    }

    void FrozenWriteAuthorization::Invalidate() noexcept
    {
        epoch.fetch_add(1, std::memory_order_acq_rel);
    }

    class WorkbenchFrozenWriteContext::CheckedIoPort final : public mem::IMemoryIoPort
    {
    public:
        CheckedIoPort(std::unique_ptr<mem::IMemoryIoPort> inner, WorkbenchFrozenWriteContext& owner)
            : inner_(std::move(inner)), owner_(owner) {}
        mem::IoLimits Limits(const mem::MemoryTargetSession& session) const override
        {
            auto limits = inner_->Limits(session);
            // The facade splits HVM requests internally. Keep each actual
            // mutation inside a separately validated port invocation.
            if (session.channel == mem::Channel::Hvm
                && (limits.maxWriteBytes == 0 || limits.maxWriteBytes > 1024)) limits.maxWriteBytes = 1024;
            return limits;
        }
        mem::IoReadResult Read(const mem::MemoryTargetSession& session,
            const std::uint64_t address, const std::uint64_t length) override
        {
            std::string reason;
            // Reads used for final verification/recovery are not cancelled by
            // owner loss, but must never be redirected to a reused process.
            if (!mem::SameTarget(session, owner_.session_) || !owner_.CheckIdentity(reason))
            { mem::IoReadResult result; result.failure = reason; return result; }
            return inner_->Read(session, address, length);
        }
        mem::IoWriteResult Write(const mem::MemoryTargetSession& session,
            const std::uint64_t address, const std::vector<std::uint8_t>& bytes, const bool approved) override
        {
            std::string reason;
            // Pointer resolution and span recording were done by the store's
            // validation callback. Recheck identity/authority at the last port
            // boundary without resolving the same chain a second time.
            if (!mem::SameTarget(session, owner_.session_) || !owner_.CheckAuthority(reason)
                || !owner_.CheckIdentity(reason))
            { mem::IoWriteResult result; result.failure = reason; return result; }
            if (session.channel != mem::Channel::Ddma) return inner_->Write(session, address, bytes, approved);
            const auto scope = owner_.factories_.beginDdmaWrite ? owner_.factories_.beginDdmaWrite() : nullptr;
            mem::IoWriteResult aggregate;
            std::size_t offset = 0;
            // DDMA's internal loop is page bounded. Split at exactly those
            // boundaries, so a session/token change cannot admit another page.
            // An in-flight page still finishes its own backend recovery.
            while (offset < bytes.size())
            {
                const auto cursor = address + offset;
                const auto count = (std::min)(bytes.size() - offset,
                    static_cast<std::size_t>(4096 - (cursor & 4095)));
                if (!owner_.ValidateWrite(cursor, count, reason))
                { aggregate.failure = reason; aggregate.partial = aggregate.bytesDone != 0; return aggregate; }
                const std::vector<std::uint8_t> chunk(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
                const auto result = inner_->Write(session, cursor, chunk, approved);
                aggregate.scratchAreaDirty = aggregate.scratchAreaDirty || result.scratchAreaDirty;
                aggregate.readModifyWriteWindow = aggregate.readModifyWriteWindow || result.readModifyWriteWindow;
                if (result.needsApproval)
                {
                    aggregate.needsApproval = aggregate.bytesDone == 0;
                    aggregate.partial = aggregate.bytesDone != 0;
                    aggregate.failure = result.failure;
                    return aggregate;
                }
                if (!result.ok || result.partial)
                {
                    const bool earlierPersisted = aggregate.bytesDone != 0;
                    if (!result.rolledBack) aggregate.bytesDone += (std::min)(result.bytesDone,
                        static_cast<std::uint64_t>(count));
                    aggregate.partial = result.partial || earlierPersisted;
                    aggregate.rolledBack = result.rolledBack && !earlierPersisted;
                    aggregate.failure = result.failure;
                    return aggregate;
                }
                aggregate.bytesDone += result.bytesDone;
                if (!result.failure.empty()) aggregate.failure = result.failure;
                offset += count;
                if (aggregate.scratchAreaDirty && offset < bytes.size())
                {
                    aggregate.partial = aggregate.bytesDone != 0;
                    if (aggregate.failure.empty()) aggregate.failure = "frozen-write-ddma-scratch-dirty";
                    return aggregate;
                }
            }
            aggregate.ok = true;
            return aggregate;
        }
    private:
        std::unique_ptr<mem::IMemoryIoPort> inner_;
        WorkbenchFrozenWriteContext& owner_;
    };

    class WorkbenchFrozenWriteContext::CheckedKernelPort final : public mem::IKernelMutationPort
    {
    public:
        CheckedKernelPort(std::unique_ptr<mem::IKernelMutationPort> inner, WorkbenchFrozenWriteContext& owner)
            : inner_(std::move(inner)), owner_(owner) {}
        mem::MutationPrepareResult Prepare(const std::uint64_t address,
            const std::vector<std::uint8_t>& after, const std::vector<std::uint8_t>& before) override
        {
            std::string reason;
            if (!owner_.ValidateWrite(address, after.size(), reason))
            { mem::MutationPrepareResult result; result.failure = reason; return result; }
            // Allocate bookkeeping before PREPARE so allocation cannot lose a
            // newly created backend transaction. Only one current slice is
            // prepared at once; prior entries remain available for rollback.
            auto slot = pending_.emplace(pending_.end(), 0, Span{address, after.size()});
            mem::MutationPrepareResult result;
            try { result = inner_->Prepare(address, after, before); }
            catch (...) { pending_.erase(slot); throw; }
            if (result.ok && result.transactionId != 0) slot->first = result.transactionId;
            else pending_.erase(slot);
            return result;
        }
        mem::MutationStepResult DryRunCommit(const std::uint64_t id) override
        { return Commit(id, false); }
        mem::MutationStepResult ForceCommit(const std::uint64_t id) override
        { return Commit(id, true); }
        mem::MutationStepResult Rollback(const std::uint64_t id) override
        {
            // Cleanup belongs to the accepted operation, not the GUI lifetime.
            return inner_->Rollback(id);
        }
        mem::IoReadResult ReadBack(const std::uint64_t address, const std::uint64_t length) override
        { return inner_->ReadBack(address, length); }
    private:
        struct Span { std::uint64_t address; std::uint64_t length; };
        mem::MutationStepResult Commit(const std::uint64_t id, const bool force)
        {
            const auto found = std::find_if(pending_.begin(), pending_.end(),
                [id](const auto& entry) { return entry.first == id; });
            std::string reason;
            if (found == pending_.end()
                || !owner_.ValidateWrite(found->second.address, found->second.length, reason))
                return {false, reason.empty() ? "frozen-write-unknown-kernel-transaction" : reason};
            return force ? inner_->ForceCommit(id) : inner_->DryRunCommit(id);
        }
        std::unique_ptr<mem::IKernelMutationPort> inner_;
        WorkbenchFrozenWriteContext& owner_;
        std::vector<std::pair<std::uint64_t, Span>> pending_;
    };

    WorkbenchFrozenWriteContext::WorkbenchFrozenWriteContext(FrozenWriteCapture capture,
        FrozenWriteFactories factories)
        : session_(capture.session), allowKernelPhysical_(capture.allowKernelPhysical),
          historyReplay_(capture.historyReplay), definitions_(std::move(capture.definitions)),
          authorization_(std::move(capture.authorization)), authorizationEpoch_(capture.authorizationEpoch),
          worker_(std::this_thread::get_id()), writeLease_(std::move(capture.writeLease)), factories_(std::move(factories)),
          pointerBindings_(std::move(capture.pointerBindings)) {}

    WorkbenchFrozenWriteContext::~WorkbenchFrozenWriteContext() = default;

    std::unique_ptr<WorkbenchFrozenWriteContext> WorkbenchFrozenWriteContext::Create(
        FrozenWriteCapture capture, FrozenWriteFactories factories, std::string& reason)
    {
        reason.clear();
        auto context = std::unique_ptr<WorkbenchFrozenWriteContext>(
            new WorkbenchFrozenWriteContext(std::move(capture), std::move(factories)));
        try { if (context->Initialize(reason)) return context; }
        catch (...) { reason = "frozen-write-factory-failed"; }
        return {};
    }

    bool WorkbenchFrozenWriteContext::Initialize(std::string& reason)
    {
        if (mem::Validate(session_) != mem::SessionError::None || !writeLease_ || !factories_.io
            || !factories_.resolvePointer || !CheckAuthority(reason))
        { if (reason.empty()) reason = "frozen-write-invalid-capture"; return false; }
        if (!allowKernelPhysical_ && session_.scope != mem::Scope::ProcessVirtual)
        { reason = "frozen-write-scope-policy"; return false; }
        definitionIndex_.reserve(definitions_.size());
        for (std::size_t i = 0; i < definitions_.size(); ++i)
        {
            if (!definitionIndex_.emplace(definitions_[i].id, i).second)
            { reason = "frozen-write-duplicate-definition"; return false; }
        }
        if (session_.scope == mem::Scope::ProcessVirtual)
        {
            if (!factories_.identity)
            { reason = "frozen-write-identity-factory-required"; return false; }
            identity_ = factories_.identity(session_, reason);
            if (!identity_) return false;
        }
        if (!CheckIdentity(reason)) return false;
        auto raw = factories_.io();
        if (!raw) { reason = "frozen-write-io-factory-empty"; return false; }
        port_ = std::make_unique<CheckedIoPort>(std::move(raw), *this);
        if (factories_.kernel)
        {
            auto kernel = factories_.kernel();
            if (kernel) kernel_ = std::make_unique<CheckedKernelPort>(std::move(kernel), *this);
        }
        store_ = std::make_unique<mem::MemoryIoByteStore>(*port_, session_, kernel_.get());
        store_->SetWriteValidationCallback([this](const auto& session, const auto address,
            const auto length, auto& failure) {
            return mem::SameTarget(session, session_) && ValidateWrite(address, length, failure);
        });
        return true;
    }

    mem::IByteStore& WorkbenchFrozenWriteContext::Store() noexcept { return *store_; }
    mem::PointerChainBindings WorkbenchFrozenWriteContext::PointerBindings() const { return pointerBindings_; }
    void WorkbenchFrozenWriteContext::BeginWorkerInvocation() noexcept { worker_ = std::this_thread::get_id(); }

    bool WorkbenchFrozenWriteContext::OnWorker(std::string& reason) const
    {
        if (worker_ == std::this_thread::get_id()) return true;
        reason = "frozen-write-wrong-thread";
        return false;
    }

    bool WorkbenchFrozenWriteContext::CheckAuthority(std::string& reason) const
    {
        if (OnWorker(reason) && authorization_ && authorization_->Allows(authorizationEpoch_)) return true;
        if (reason.empty()) reason = "frozen-write-authority-changed";
        return false;
    }

    bool WorkbenchFrozenWriteContext::CheckIdentity(std::string& reason)
    {
        if (!OnWorker(reason)) return false;
        if (session_.scope == mem::Scope::ProcessVirtual
            && (!identity_ || !identity_->Validate(session_, reason))) return false;
        if (session_.channel == mem::Channel::Ddma
            && (!factories_.ddmaGeneration || factories_.ddmaGeneration() != session_.ddmaGeneration))
        { reason = "frozen-write-ddma-generation-changed"; return false; }
        return true;
    }

    bool WorkbenchFrozenWriteContext::ValidateWrite(const std::uint64_t address,
        const std::uint64_t length, std::string& reason)
    {
        if (!CheckAuthority(reason) || !CheckIdentity(reason)) return false;
        const auto space = mem::ScopeAddressSpace(session_);
        if ((!allowKernelPhysical_ && session_.scope != mem::Scope::ProcessVirtual)
            || !mem::AddrSpaceContains(space, address) || (length != 0 && length - 1 > space.last - address))
        { reason = "frozen-write-address-policy"; return false; }
        const auto guarded = pointerBindings_.Validate(session_, address, length, historyReplay_,
            [this](const std::uint64_t id) -> std::optional<mem::AddressEntry> {
                const auto found = definitionIndex_.find(id);
                return found == definitionIndex_.end() ? std::nullopt : std::optional(definitions_[found->second]);
            },
            [this](const auto& entry, const auto& session) { return factories_.resolvePointer(entry, session, *port_); });
        if (!guarded.ok)
        { reason = "frozen-write-pointer-witness-changed/" + std::to_string(static_cast<int>(guarded.failure)); return false; }
        return CheckAuthority(reason) && CheckIdentity(reason);
    }
}
