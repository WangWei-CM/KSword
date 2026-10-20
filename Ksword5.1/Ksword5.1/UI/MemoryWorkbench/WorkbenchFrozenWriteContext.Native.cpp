#include "WorkbenchFrozenWriteContext.h"
#include "../../MemoryDock/WorkbenchIoPorts.h"
#include "../../MemoryDock/WorkbenchPointerChainAccess.h"
#include "../../MemoryDock/MemoryDock.WorkbenchServices.Internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mutex>

namespace ks::ui
{
    namespace
    {
        std::uint64_t Time(const FILETIME& value) noexcept
        { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; }

        class NativeIdentity final : public IFrozenProcessIdentity
        {
        public:
            explicit NativeIdentity(const std::uint32_t pid)
                : handle_(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {}
            ~NativeIdentity() override { if (handle_) ::CloseHandle(handle_); }
            bool Validate(const ksword::memwb::MemoryTargetSession& session, std::string& reason) override
            {
                FILETIME creation{}, exit{}, kernel{}, user{};
                DWORD code = 0;
                BOOL wow64 = FALSE;
                if (!handle_)
                {
                    // N4: an ordinary workbench selection may deliberately be
                    // weak when native access is unavailable (R0-only targets).
                    // Retain that route without promoting its identity or
                    // claiming a native lease/PID-reuse proof. Atomic GUI
                    // authority, expected-before and final readback still apply.
                    if (session.processCreateTime100ns == 0) return true;
                    reason = "frozen-write-process-identity-unavailable";
                    return false;
                }
                // Keep the independent handle until all readback/recovery ends.
                // Checking exit FILETIME also rejects an actual exit code 259.
                if (::GetProcessId(handle_) != session.pid
                    || !::GetProcessTimes(handle_, &creation, &exit, &kernel, &user)
                    || Time(creation) == 0 || Time(exit) != 0
                    || !::GetExitCodeProcess(handle_, &code) || code != STILL_ACTIVE
                    || !::IsWow64Process(handle_, &wow64) || session.addressBits != (wow64 ? 32U : 64U))
                { reason = "frozen-write-process-identity-changed"; return false; }
                // Ordinary workbench targets may have a weak selected identity.
                // Privately pin the instance observed at operation creation;
                // never replace/promote the GUI's zero creation-time identity.
                if (observedCreation_ == 0) observedCreation_ = Time(creation);
                if (Time(creation) != observedCreation_
                    || (session.processCreateTime100ns != 0 && Time(creation) != session.processCreateTime100ns))
                { reason = "frozen-write-process-identity-changed"; return false; }
                return true;
            }
        private:
            HANDLE handle_ = nullptr;
            std::uint64_t observedCreation_ = 0;
        };

        class NativeDdmaWriteScope final : public IFrozenDdmaWriteScope
        {
        public:
            NativeDdmaWriteScope() : lock_(ksword::memory_backend::ddmaTransactionMutex()) {}
        private:
            std::unique_lock<std::recursive_mutex> lock_;
        };
    }

    FrozenWriteFactories MakeNativeFrozenWriteFactories()
    {
        FrozenWriteFactories factories;
        factories.io = [] { return std::make_unique<ksword::memwb_ports::WorkbenchIoPort>(); };
        factories.kernel = [] { return std::make_unique<ksword::memwb_ports::WorkbenchKernelMutationPort>(); };
        factories.identity = [](const auto& session, std::string& reason) -> std::unique_ptr<IFrozenProcessIdentity> {
            auto identity = std::make_unique<NativeIdentity>(session.pid);
            if (!identity->Validate(session, reason)) return {};
            return identity;
        };
        factories.resolvePointer = [](const auto& entry, const auto& session, auto& port) {
            return ksword::memwb_pointer_access::Resolve(entry, session, port).result;
        };
        factories.ddmaGeneration = [] { return ksword::memory_backend::ddmaSessionGeneration(); };
        factories.beginDdmaWrite = [] { return std::make_unique<NativeDdmaWriteScope>(); };
        return factories;
    }

    std::shared_ptr<ksword::memwb::IAuditSink> MakeNativeFrozenAuditSink()
    {
        // Own the thread-safe production sink itself. The shared GUI forwarder
        // and WorkbenchShared lifetime are deliberately absent from this path.
        return std::shared_ptr<ksword::memwb::IAuditSink>(workbench_dock::detail::CreateAuditLogSink());
    }
}
