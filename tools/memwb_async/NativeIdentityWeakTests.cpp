#include "WorkbenchFrozenWriteContext.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

// Deliberately no Windows.h and no actual native API linkage. The exact Time
// and NativeIdentity definitions below are extracted from the isolated native
// TU by the test build, and every ::Win32 call binds to these fake functions.
using DWORD = std::uint32_t;
using BOOL = int;
using HANDLE = void*;
struct FILETIME { DWORD dwLowDateTime = 0; DWORD dwHighDateTime = 0; };
constexpr DWORD PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
constexpr BOOL FALSE = 0;
constexpr DWORD STILL_ACTIVE = 259;

namespace fake
{
    struct Win32
    {
        bool canOpen = false;
        bool canQueryTimes = true;
        bool canQueryCode = true;
        bool canQueryBits = true;
        DWORD pid = 77;
        DWORD code = STILL_ACTIVE;
        std::uint64_t creation = 100;
        std::uint64_t exit = 0;
        bool wow64 = false;
        unsigned opens = 0, closes = 0, times = 0;
    } api;
    FILETIME Time(const std::uint64_t value)
    { return {static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)}; }
}

HANDLE OpenProcess(const DWORD access, const BOOL inherit, const DWORD pid)
{
    ++fake::api.opens;
    if (access != PROCESS_QUERY_LIMITED_INFORMATION || inherit != FALSE || pid != fake::api.pid)
        throw std::runtime_error("unexpected fake OpenProcess arguments");
    return fake::api.canOpen ? &fake::api : nullptr;
}
BOOL CloseHandle(const HANDLE handle)
{ if (handle != &fake::api) throw std::runtime_error("unexpected fake CloseHandle"); ++fake::api.closes; return 1; }
DWORD GetProcessId(const HANDLE handle)
{ return handle == &fake::api ? fake::api.pid : 0; }
BOOL GetProcessTimes(const HANDLE handle, FILETIME* creation, FILETIME* exit, FILETIME* kernel, FILETIME* user)
{
    ++fake::api.times;
    if (handle != &fake::api || !fake::api.canQueryTimes) return 0;
    *creation = fake::Time(fake::api.creation); *exit = fake::Time(fake::api.exit);
    *kernel = {}; *user = {};
    return 1;
}
BOOL GetExitCodeProcess(const HANDLE handle, DWORD* code)
{ if (handle != &fake::api || !fake::api.canQueryCode) return 0; *code = fake::api.code; return 1; }
BOOL IsWow64Process(const HANDLE handle, BOOL* wow64)
{ if (handle != &fake::api || !fake::api.canQueryBits) return 0; *wow64 = fake::api.wow64 ? 1 : 0; return 1; }

#include "NativeIdentity.extract.inc"

namespace mem = ksword::memwb;
namespace ui = ks::ui;
namespace
{
    unsigned checks = 0;
    void Check(const bool passed, const char* text)
    { ++checks; if (!passed) throw std::runtime_error(text); }
    struct IoState
    {
        unsigned creates = 0, reads = 0, writes = 0;
        std::vector<mem::MemoryTargetSession> sessions;
        std::vector<std::size_t> writeSizes;
        std::map<std::uint64_t, std::uint8_t> bytes;
    };
    class FakePort final : public mem::IMemoryIoPort
    {
    public:
        explicit FakePort(std::shared_ptr<IoState> state) : state_(std::move(state)) { ++state_->creates; }
        mem::IoLimits Limits(const mem::MemoryTargetSession&) const override { return {}; }
        mem::IoReadResult Read(const mem::MemoryTargetSession& session, const std::uint64_t address,
            const std::uint64_t length) override
        {
            ++state_->reads; state_->sessions.push_back(session);
            mem::IoReadResult result; result.status = mem::IoReadStatus::Ok;
            for (std::uint64_t i = 0; i < length; ++i) result.data.push_back(state_->bytes[address + i]);
            return result;
        }
        mem::IoWriteResult Write(const mem::MemoryTargetSession& session, const std::uint64_t address,
            const std::vector<std::uint8_t>& bytes, const bool approved) override
        {
            Check(!approved, "fake weak route never invents backend approval");
            ++state_->writes; state_->sessions.push_back(session); state_->writeSizes.push_back(bytes.size());
            for (std::size_t i = 0; i < bytes.size(); ++i) state_->bytes[address + i] = bytes[i];
            mem::IoWriteResult result; result.ok = true; result.bytesDone = bytes.size(); return result;
        }
    private: std::shared_ptr<IoState> state_;
    };

    mem::MemoryTargetSession Session(const mem::Channel channel, const std::uint64_t creation = 0)
    {
        mem::MemoryTargetSession session; session.pid = fake::api.pid;
        session.processCreateTime100ns = creation; session.channel = channel; session.ddmaGeneration = 7;
        return session;
    }
    ui::FrozenWriteCapture Capture(const mem::MemoryTargetSession& session)
    {
        ui::FrozenWriteCapture capture; capture.session = session;
        capture.authorization = std::make_shared<ui::FrozenWriteAuthorization>();
        capture.writeLease = ui::TryAcquireWorkbenchWriteLease();
        Check(capture.writeLease != nullptr, "fake context owns global write admission");
        return capture;
    }
    ui::FrozenWriteFactories Factories(const std::shared_ptr<IoState>& state)
    {
        ui::FrozenWriteFactories factories;
        factories.io = [state] { return std::make_unique<FakePort>(state); };
        factories.identity = [](const auto& session, std::string& reason) -> std::unique_ptr<ui::IFrozenProcessIdentity> {
            auto identity = std::make_unique<ui::NativeIdentity>(session.pid);
            if (!identity->Validate(session, reason)) return {};
            return identity;
        };
        factories.resolvePointer = [](const auto&, const auto&, auto&) { return ksword::pointer_chain::Result{}; };
        factories.ddmaGeneration = [] { return 7; };
        return factories;
    }

    void ProtectedWeakRoute(const mem::Channel channel)
    {
        fake::api = {};
        auto state = std::make_shared<IoState>();
        auto capture = Capture(Session(channel)); auto authority = capture.authorization;
        std::string reason;
        auto context = ui::WorkbenchFrozenWriteContext::Create(std::move(capture), Factories(state), reason);
        Check(context != nullptr && fake::api.opens == 1 && fake::api.times == 0,
            "exact native weak open failure preserves original route");
        Check(context->Session().processCreateTime100ns == 0, "selected weak identity is not promoted");
        const std::uint64_t address = 4094;
        const std::vector<std::uint8_t> bytes(channel == mem::Channel::Hvm ? 2049 : 8, 0x39);
        const auto before = context->Store().Read(address, bytes.size());
        Check(before.ok && before.data == std::vector<std::uint8_t>(bytes.size(), 0), "weak expected-before read uses fake port");
        const auto write = context->Store().Write(address, bytes, false);
        Check(write.ok && write.bytesDone == bytes.size(), "weak R0/HVM/DDMA write reaches fake port through real store");
        const auto after = context->Store().Read(address, bytes.size());
        Check(after.ok && after.data == bytes, "weak final readback remains available");
        const auto expectedFragments = channel == mem::Channel::Hvm ? 3U : (channel == mem::Channel::Ddma ? 2U : 1U);
        Check(state->writes == expectedFragments, "weak route retains actual channel fragmentation");
        for (const auto& observed : state->sessions)
            Check(observed.channel == channel && observed.pid == fake::api.pid && observed.processCreateTime100ns == 0,
                "every port call sees original weak target identity");
        const auto admittedWrites = state->writes;
        authority->Invalidate();
        Check(!context->Store().Write(address, {4}, false).ok && state->writes == admittedWrites,
            "weak fallback retains GUI epoch admission guard");
        authority->ownerAlive.store(false);
        Check(context->Store().Read(address, bytes.size()).data == bytes, "weak fallback permits already-started final observation after close");
        context.reset();
        Check(fake::api.closes == 0, "weak fallback never closes a nonexistent native handle");
    }

    void StrongIdentityBeforeIo()
    {
        fake::api = {}; fake::api.canOpen = true;
        auto state = std::make_shared<IoState>(); std::string reason;
        auto context = ui::WorkbenchFrozenWriteContext::Create(Capture(Session(mem::Channel::StandardDriver, 101)), Factories(state), reason);
        Check(!context && state->creates == 0 && state->reads == 0 && state->writes == 0,
            "wrong selected strong creation time fails before all port IO");
        Check(reason == "frozen-write-process-identity-changed" && fake::api.closes == 1, "strong mismatch releases fake handle");

        fake::api = {}; state = std::make_shared<IoState>();
        context = ui::WorkbenchFrozenWriteContext::Create(Capture(Session(mem::Channel::StandardDriver, 100)), Factories(state), reason);
        Check(!context && state->creates == 0 && reason == "frozen-write-process-identity-unavailable",
            "strong native open failure still fails before port creation");

        fake::api = {}; fake::api.canOpen = true; fake::api.canQueryTimes = false; state = std::make_shared<IoState>();
        context = ui::WorkbenchFrozenWriteContext::Create(Capture(Session(mem::Channel::StandardDriver, 100)), Factories(state), reason);
        Check(!context && state->creates == 0, "unverifiable strong identity still fails before IO");
    }

    void WeakPrivatePin()
    {
        fake::api = {}; fake::api.canOpen = true;
        auto state = std::make_shared<IoState>(); std::string reason;
        auto context = ui::WorkbenchFrozenWriteContext::Create(Capture(Session(mem::Channel::StandardDriver)), Factories(state), reason);
        Check(context != nullptr && context->Session().processCreateTime100ns == 0, "openable weak selection stays weak");
        Check(context->Store().Write(0x2000, {1, 2}, false).ok && state->writes == 1, "private pin allows matching instance");
        fake::api.creation = 101;
        Check(!context->Store().Write(0x2002, {3}, false).ok && state->writes == 1,
            "exact native private observed creation change rejects next mutation");
        Check(!context->Store().Read(0x2000, 2).ok, "changed private pinned instance is not used for redirected readback");
        context.reset();
        Check(fake::api.closes == 1 && fake::api.times >= 4, "openable weak pin rechecks and releases owned fake handle");

        fake::api = {}; fake::api.canOpen = true; state = std::make_shared<IoState>();
        context = ui::WorkbenchFrozenWriteContext::Create(Capture(Session(mem::Channel::Hvm)), Factories(state), reason);
        Check(context != nullptr, "weak exit check context");
        fake::api.exit = 55; // Exit code 259 must not look alive after exit.
        Check(!context->Store().Write(0x3000, {1}, false).ok && state->writes == 0,
            "exact native exit FILETIME rejects real STILL_ACTIVE exit code");
    }
}

int main()
{
    try
    {
        ProtectedWeakRoute(mem::Channel::StandardDriver);
        ProtectedWeakRoute(mem::Channel::Hvm);
        ProtectedWeakRoute(mem::Channel::Ddma);
        StrongIdentityBeforeIo(); WeakPrivatePin();
        std::cout << "PASS " << checks << " exact NativeIdentity checks; all Win32 calls fake\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
