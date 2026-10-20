#pragma once

#include "../../../../shared/evidence/memory_workbench/MemoryIoByteStore.h"
#include "../../../../shared/evidence/memory_workbench/PointerChainBindings.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

namespace ks::ui
{
    class FrozenWorkbenchWriteLease final
    {
    public:
        ~FrozenWorkbenchWriteLease();
        FrozenWorkbenchWriteLease(const FrozenWorkbenchWriteLease&) = delete;
        FrozenWorkbenchWriteLease& operator=(const FrozenWorkbenchWriteLease&) = delete;
    private:
        FrozenWorkbenchWriteLease() = default;
        bool ownsAdmission_ = false;
        friend std::shared_ptr<FrozenWorkbenchWriteLease> TryAcquireWorkbenchWriteLease();
    };
    // Nonblocking process-wide admission for every workbench view. Keep the
    // token through confirmation continuations and final readback/recovery.
    std::shared_ptr<FrozenWorkbenchWriteLease> TryAcquireWorkbenchWriteLease();

    // The GUI only invalidates this value-only authority. A worker never calls
    // back into the GUI to authorize an I/O fragment. Create one per view and
    // invalidate it on source/content/definition changes; close clears alive.
    struct FrozenWriteAuthorization
    {
        std::atomic<bool> ownerAlive{true};
        std::atomic<bool> cancelled{false};
        std::atomic<std::uint64_t> epoch{0};
        bool Allows(std::uint64_t captured) const noexcept;
        void Invalidate() noexcept;
    };

    class IFrozenProcessIdentity
    {
    public:
        virtual ~IFrozenProcessIdentity() = default;
        virtual bool Validate(const ksword::memwb::MemoryTargetSession&, std::string& reason) = 0;
    };

    class IFrozenDdmaWriteScope
    {
    public:
        virtual ~IFrozenDdmaWriteScope() = default;
    };

    struct FrozenWriteCapture
    {
        ksword::memwb::MemoryTargetSession session;
        bool allowKernelPhysical = true;
        bool historyReplay = false;
        ksword::memwb::PointerChainBindings pointerBindings;
        // Complete value copies. Lookup never reads AddressBookStore on worker.
        std::vector<ksword::memwb::AddressEntry> definitions;
        std::shared_ptr<FrozenWriteAuthorization> authorization;
        std::uint64_t authorizationEpoch = 0;
        std::shared_ptr<FrozenWorkbenchWriteLease> writeLease;
    };

    struct FrozenWriteFactories
    {
        using IoFactory = std::function<std::unique_ptr<ksword::memwb::IMemoryIoPort>()>;
        using KernelFactory = std::function<std::unique_ptr<ksword::memwb::IKernelMutationPort>()>;
        using IdentityFactory = std::function<std::unique_ptr<IFrozenProcessIdentity>(
            const ksword::memwb::MemoryTargetSession&, std::string& reason)>;
        using PointerResolver = std::function<ksword::pointer_chain::Result(
            const ksword::memwb::AddressEntry&, const ksword::memwb::MemoryTargetSession&,
            ksword::memwb::IMemoryIoPort&)>;
        // All factories execute on the worker. Closures must own only values or
        // independently owned thread-safe services, never WorkbenchShared/GUI.
        IoFactory io;
        KernelFactory kernel;
        IdentityFactory identity;
        PointerResolver resolvePointer;
        std::function<std::uint64_t()> ddmaGeneration;
        // Optional for fake ports. Native scope preserves the existing raw
        // DDMA Write gate across this adapter's separately checked page calls.
        std::function<std::unique_ptr<IFrozenDdmaWriteScope>()> beginDdmaWrite;
    };

    // Concrete stateless production ports, independent native identity lease,
    // existing native pointer resolver and atomic backend generation getter.
    FrozenWriteFactories MakeNativeFrozenWriteFactories();
    std::shared_ptr<ksword::memwb::IAuditSink> MakeNativeFrozenAuditSink();

    // Exclusive worker phases may migrate threads. Owns both ports and the
    // immutable session referenced by MemoryIoByteStore. Neither TargetCheckedIoPort nor
    // a live target/revision object can enter through this interface.
    class WorkbenchFrozenWriteContext final
    {
    public:
        static std::unique_ptr<WorkbenchFrozenWriteContext> Create(
            FrozenWriteCapture capture, FrozenWriteFactories factories, std::string& reason);
        ~WorkbenchFrozenWriteContext();
        WorkbenchFrozenWriteContext(const WorkbenchFrozenWriteContext&) = delete;
        WorkbenchFrozenWriteContext& operator=(const WorkbenchFrozenWriteContext&) = delete;
        ksword::memwb::IByteStore& Store() noexcept;
        const ksword::memwb::MemoryTargetSession& Session() const noexcept { return session_; }
        ksword::memwb::PointerChainBindings PointerBindings() const;
        bool CheckIdentity(std::string& reason);
        // Serial continuation phases may migrate between pool threads. Call
        // only at the start of an exclusive worker invocation, never from GUI.
        void BeginWorkerInvocation() noexcept;

    private:
        class CheckedIoPort;
        class CheckedKernelPort;
        WorkbenchFrozenWriteContext(FrozenWriteCapture capture, FrozenWriteFactories factories);
        bool Initialize(std::string& reason);
        bool CheckAuthority(std::string& reason) const;
        bool ValidateWrite(std::uint64_t address, std::uint64_t length, std::string& reason);
        bool OnWorker(std::string& reason) const;
        const ksword::memwb::MemoryTargetSession session_;
        const bool allowKernelPhysical_;
        const bool historyReplay_;
        const std::vector<ksword::memwb::AddressEntry> definitions_;
        const std::shared_ptr<FrozenWriteAuthorization> authorization_;
        const std::uint64_t authorizationEpoch_;
        std::thread::id worker_;
        const std::shared_ptr<FrozenWorkbenchWriteLease> writeLease_;
        FrozenWriteFactories factories_;
        ksword::memwb::PointerChainBindings pointerBindings_;
        std::unordered_map<std::uint64_t, std::size_t> definitionIndex_;
        std::unique_ptr<IFrozenProcessIdentity> identity_;
        std::unique_ptr<CheckedIoPort> port_;
        std::unique_ptr<CheckedKernelPort> kernel_;
        std::unique_ptr<ksword::memwb::MemoryIoByteStore> store_;
    };
}
