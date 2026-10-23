#include "KswordDebuggerBackend.h"

#include <cstring>
#include <cstdio>

namespace ksword::debugger
{
    namespace
    {
        DWORD controlFailure(LONG status)
        {
            if (status >= 0) return ERROR_NOT_SUPPORTED;
            using Convert = ULONG(WINAPI*)(LONG);
            static const auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            return convert == nullptr ? ERROR_GEN_FAILURE : convert(status);
        }
    }
    void Backend::overlayShadowBytes(DWORD pid, std::uint64_t address, void* data, SIZE_T bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (pid != attachedPid_ || data == nullptr) return;
        for (const auto point : shadowInt3_)
            if (point >= address && point - address < bytes)
                static_cast<unsigned char*>(data)[static_cast<SIZE_T>(point - address)] = 0xcc;
    }
    bool Backend::shadowPatchByte(DWORD pid, std::uint64_t address, unsigned char& byte)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (pid == attachedPid_ && shadowInt3_.count(address) != 0) { byte = 0xcc; return true; }
        const auto target = shadowWrites_.find(pid);
        if (target == shadowWrites_.end()) return false;
        const auto page = target->second.pages.find(address & ~0xfffULL);
        const auto offset = static_cast<SIZE_T>(address & 0xfffULL);
        if (page == target->second.pages.end() || !page->second.mask.test(offset)) return false;
        byte = page->second.bytes[offset]; return true;
    }

    DWORD Backend::nativeExecutionBreakpointPath(std::uint64_t address)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& pair : breakpoints_)
        {
            const auto& context = pair.second.requested;
            const std::array<std::uint64_t, 4> addresses{context.Dr0, context.Dr1, context.Dr2, context.Dr3};
            for (SIZE_T i = 0; i < addresses.size(); ++i)
                if (addresses[i] == address && pair.second.ids[i] != 0)
                    return pair.second.shadow ? KSWORD_DEBUGGER_PATH_SHADOW : KSWORD_DEBUGGER_PATH_EPT;
        }
        return KSWORD_DEBUGGER_PATH_NATIVE;
    }

    bool Backend::nativeDataOnlyRecord(const ThreadBreakpoints& record)
    {
        if (!record.shadow) return false;
        for (DWORD slot = 0; slot < 4; ++slot)
            if (record.ids[slot] != 0 || ((record.requested.Dr7 & (3ULL << (2 * slot))) != 0 &&
                ((record.requested.Dr7 >> (16 + 4 * slot)) & 3) == 0)) return false;
        return true;
    }
    bool Backend::hasHvmBreakpointRules() const
    {
        for (const auto& entry : breakpoints_)
            for (const auto id : entry.second.ids) if (id != 0) return true;
        return false;
    }
    DWORD Backend::ensureShadowHvm()
    {
        const auto refused = [this](DWORD error, const char* reason) {
            logRepeated("Shadow preparation refused: error=" + std::to_string(error) + "; " + reason +
                "; existing bindings retained; no visible native DR fallback or foreign HVM takeover");
            return error;
        };
        if (shadowRecoveryRequired_) return refused(ERROR_INVALID_STATE, "restore owned Shadow patches after the incomplete prior transaction first");
        if (!useHvm_) return refused(ERROR_INVALID_STATE, "select HVM before installing an execution view");
        for (const auto& pair : breakpoints_) if (!pair.second.shadow)
            return refused(ERROR_BUSY, "this adapter still owns normal EPT breakpoints; pause and clear them before selecting the Shadow route");
        if (ownsResident_) return shadowPrepared_ ? ERROR_SUCCESS :
            refused(ERROR_BUSY, "this adapter owns a normal EPT preparation; retire its bindings/preparation before selecting Shadow");
        KSWORD_ARK_DEBUGGER_REQUEST query{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        const DWORD capability = nativeRequest(query, response);
        if (capability != ERROR_SUCCESS) return refused(capability, "driver Shadow capability query failed");
        if ((response.capabilities & KSWORD_ARK_DEBUGGER_CAP_SHADOW_INT3) == 0)
            return refused(ERROR_NOT_SUPPORTED, "driver does not support hidden INT3 execution views");
        const auto state = client_.queryHvmStatus();
        if (!state.io.ok) return refused(state.io.win32Error, "driver HVM ownership/state query failed");
        if ((state.response.stateFlags & (KSWORD_ARK_HVM_STATE_RESOURCES_READY | KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE)) != 0)
            return refused(ERROR_BUSY, "pre-existing HVM resources/residency belong to another caller (for example Main HVM or another debugger); stop and release them in the owning UI before retrying");
        const auto prepared = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_PREPARE, state.response.generation,
            false, true, true, false, false, false, false, false, false, true);
        if (!prepared.io.ok) return refused(prepared.io.win32Error, "driver rejected this adapter's Shadow PREPARE request");
        if (prepared.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK)
            return refused(controlFailure(prepared.response.lastStatus), "Shadow PREPARE did not complete; no breakpoint/context writes attempted");
        ownsResident_ = true; shadowPrepared_ = true;
        const auto tested = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_SELF_TEST, prepared.response.newGeneration,
            true, true, true);
        if (!tested.io.ok || tested.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK)
        {
            (void)releaseOwnedHvm();
            return refused(tested.io.ok ? controlFailure(tested.response.lastStatus) : tested.io.win32Error,
                "Shadow self-test failed; only this adapter's preparation cleanup was attempted");
        }
        log("Shadow Page execution views prepared with EPTP switching (MTF not required)");
        return ERROR_SUCCESS;
    }

    DWORD Backend::removeShadowInt3(std::uint64_t address)
    {
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_SHADOW_REMOVE;
        request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = attachedPid_; request.address = address; request.bytes = 1;
        const DWORD error = nativeRequest(request, response);
        if (error == ERROR_SUCCESS || error == ERROR_NOT_FOUND)
        {
            const bool tracked = shadowInt3_.erase(address) != 0;
            if (tracked)
            {
                char message[192]{};
                std::snprintf(message, sizeof(message),
                    "ShadowPage INT3 removed: pid=%lu va=0x%llX; physical execution view removed, native engine may retain a logical breakpoint",
                    attachedPid_, static_cast<unsigned long long>(address));
                log(message);
            }
            return ERROR_SUCCESS;
        }
        return error;
    }

    DWORD Backend::addShadowInt3(std::uint64_t address)
    {
        std::unordered_set<std::uint64_t> pages;
        for (const auto point : shadowInt3_) pages.insert(point & ~0xfffULL);
        const auto target = shadowWrites_.find(attachedPid_);
        if (target != shadowWrites_.end()) for (const auto& page : target->second.pages) pages.insert(page.first);
        pages.insert(address & ~0xfffULL);
        const DWORD otherPages = shadowWritePageCount() - (target == shadowWrites_.end() ? 0U : static_cast<DWORD>(target->second.pages.size()));
        if (pages.size() + otherPages > options_.maxShadowPages) return ERROR_NOT_ENOUGH_MEMORY;
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_SHADOW_ADD; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = attachedPid_; request.address = address; request.bytes = 1;
        const DWORD error = nativeRequest(request, response);
        if (error == ERROR_SUCCESS)
        {
            shadowInt3_.insert(address);
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "ShadowPage INT3 installed: pid=%lu va=0x%llX original-pa=0x%llX view-id=%llu; execution byte=CC, original code unchanged",
                attachedPid_, static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(response.address),
                static_cast<unsigned long long>(response.reserved0));
            log(message);
        }
        return error;
    }

    DWORD Backend::removeShadowRecord(DWORD tid)
    {
        const auto found = breakpoints_.find(tid);
        if (found == breakpoints_.end()) return ERROR_SUCCESS;
        const CONTEXT& context = found->second.requested;
        const std::uint64_t addresses[] = {context.Dr0, context.Dr1, context.Dr2, context.Dr3};
        for (DWORD slot = 0; slot < 4; ++slot)
        {
            if (found->second.ids[slot] == 0) continue;
            if ((context.Dr7 & (3ULL << (2 * slot))) == 0 || ((context.Dr7 >> (16 + 4 * slot)) & 3) != 0) continue;
            const auto reference = shadowReferences_.find(addresses[slot]);
            if (reference == shadowReferences_.end()) continue;
            if (reference->second > 1) --reference->second;
            else
            {
                if (shadowInt3_.count(addresses[slot]) != 0)
                {
                    const DWORD error = removeShadowInt3(addresses[slot]);
                    if (error != ERROR_SUCCESS) return error;
                }
                shadowReferences_.erase(reference);
            }
        }
        breakpoints_.erase(found); return ERROR_SUCCESS;
    }

    BOOL Backend::applyShadowContext(HANDLE thread, const CONTEXT* context, TargetLease* bound)
    {
        // 现有事务只延长目标引用，不改变任何断点安装或执行分支。
        TargetLease localLease;
        TargetLease& lease = bound != nullptr ? *bound : localLease;
        if (bound == nullptr && !pinTarget(thread, true, lease)) return FALSE;
        thread = lease.identity.native();
        if (!validTargetThread(lease)) { SetLastError(ERROR_INVALID_STATE); return FALSE; }
        const DWORD tid = lease.tid;
        const std::uint64_t addresses[] = {context->Dr0, context->Dr1, context->Dr2, context->Dr3};
        bool execute = false;
        for (DWORD slot = 0; slot < 4; ++slot)
        {
            if ((context->Dr7 & (3ULL << (2 * slot))) == 0) continue;
            const DWORD type = static_cast<DWORD>((context->Dr7 >> (16 + 4 * slot)) & 3);
            if (options_.mode == KSWORD_DEBUGGER_MODE_STEALTH && type != 0)
            { logRepeated("Stealth data breakpoint refused: Shadow execution views cannot hide data watchpoints"); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
            if (type != 0 && options_.allowFallback == 0)
            { logRepeated("Data breakpoint fallback refused: native hardware registers are required on the Shadow path"); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
            if (addresses[slot] == 0 || type == 2 || (type == 0 && ((context->Dr7 >> (18 + 4 * slot)) & 3) != 0))
            { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
            execute |= type == 0;
        }
        const auto previous = breakpoints_.find(tid);
        const bool hadPrevious = previous != breakpoints_.end();
        const ThreadBreakpoints old = hadPrevious ? previous->second : ThreadBreakpoints{};
        if (hadPrevious && (!old.shadow || !matchesThreadRecord(thread, old)))
        { SetLastError(ERROR_INVALID_STATE); return FALSE; }
        HANDLE retained = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &retained, 0, FALSE, DUPLICATE_SAME_ACCESS)) return FALSE;
        ThreadBreakpoints replacement{}; replacement.requested = *context;
        replacement.identity = std::make_shared<ark::DriverHandle>(retained);
        replacement.generation = sessionGeneration_; replacement.shadow = true;
        if (options_.mode == KSWORD_DEBUGGER_MODE_NORMAL && options_.allowFallback != 0 && !execute &&
            (!hadPrevious || nativeDataOnlyRecord(old)))
        {
            // Visible data DRs do not change EPT/Shadow views. Retain identity
            // metadata for the policy guard, but never acquire/pause a foreign
            // resident for installation, replacement, or removal of this path.
            CONTEXT native = *context;
            const BOOL completed = nativeContext(thread, &native, true, &lease);
            const DWORD actualError = GetLastError();
            if (completed)
            {
                if ((context->Dr7 & 0xffU) != 0) breakpoints_[tid] = replacement;
                else breakpoints_.erase(tid);
            }
            if ((context->Dr7 & 0xffU) != 0)
                recordFallback("EPT data watchpoint", completed ? "visible native hardware debug register" : "native data watchpoint rejected", ERROR_NOT_SUPPORTED,
                    "data-only watchpoints require visible DR addresses; native context result error=" + std::to_string(completed ? ERROR_SUCCESS : actualError) +
                    "; no HVM acquisition, stop, or rollback; previous metadata retained on failure");
            else logRepeated("Native data watchpoint clear: result error=" + std::to_string(completed ? ERROR_SUCCESS : actualError) +
                "; no HVM acquisition, stop, or rollback");
            SetLastError(actualError); return completed;
        }
        DWORD error = execute ? ensureShadowHvm() : ERROR_SUCCESS;
        // Preparation/ownership rejection precedes all breakpoint and thread
        // context writes. No previous record was removed, so no rollback is
        // warranted and no other caller's resident state may be stopped.
        if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
        CONTEXT saved{}; saved.ContextFlags = context->ContextFlags;
        if (!nativeContext(thread, &saved, false, &lease))
        {
            error = GetLastError();
            if (!hasShadowViews()) (void)releaseOwnedHvm();
            logRepeated("Shadow context rejected before breakpoint/context writes: error=" + std::to_string(error) + "; snapshot read failed; no rollback attempted");
            SetLastError(error); return FALSE;
        }
        bool restart = false;
        error = pauseOwnedHvm(restart);
        if (error != ERROR_SUCCESS)
        {
            logRepeated("Shadow context rejected before breakpoint/context writes: error=" + std::to_string(error) + "; cannot pause owned HVM safely; no rollback or foreign HVM takeover attempted");
            SetLastError(error); return FALSE;
        }
        if (error == ERROR_SUCCESS && hadPrevious) error = removeShadowRecord(tid);
        const auto install = [&](const ThreadBreakpoints& record) -> DWORD {
            breakpoints_[tid] = record;
            breakpoints_[tid].ids = {};
            const auto& value = record.requested;
            const std::uint64_t points[] = {value.Dr0,value.Dr1,value.Dr2,value.Dr3};
            for (DWORD slot = 0; slot < 4; ++slot)
            {
                if ((value.Dr7 & (3ULL << (2 * slot))) == 0 || ((value.Dr7 >> (16 + 4 * slot)) & 3) != 0) continue;
                if (shadowReferences_.count(points[slot]) == 0)
                {
                    const DWORD added = addShadowInt3(points[slot]);
                    if (added != ERROR_SUCCESS) return added;
                }
                ++shadowReferences_[points[slot]];
                breakpoints_[tid].ids[slot] = 1;
            }
            return ERROR_SUCCESS;
        };
        if (error == ERROR_SUCCESS && (context->Dr7 & 0xff) != 0) error = install(replacement);
        CONTEXT native = *context;
        std::uint64_t* nativeAddresses[] = {&native.Dr0,&native.Dr1,&native.Dr2,&native.Dr3};
        for (DWORD slot = 0; slot < 4; ++slot)
            if ((context->Dr7 & (3ULL << (2 * slot))) != 0 && ((context->Dr7 >> (16 + 4 * slot)) & 3) == 0)
            { *nativeAddresses[slot] = 0; native.Dr7 &= ~(3ULL << (2 * slot)); }
        if (error == ERROR_SUCCESS)
            for (DWORD slot = 0; slot < 4; ++slot)
                if ((context->Dr7 & (3ULL << (2 * slot))) != 0 && ((context->Dr7 >> (16 + 4 * slot)) & 3) != 0)
                    recordFallback("EPT data watchpoint", "native hardware debug register", ERROR_NOT_SUPPORTED,
                        "Shadow execution views do not implement data watchpoints; physical DR slot " + std::to_string(slot) + " will be visible");
        if (error == ERROR_SUCCESS && !nativeContext(thread, &native, true, &lease)) error = GetLastError();
        if (error == ERROR_SUCCESS && hasShadowViews()) error = startOwnedHvm();
        if (error != ERROR_SUCCESS)
        {
            bool ignored = false;
            DWORD rollback = pauseOwnedHvm(ignored);
            if (rollback == ERROR_SUCCESS) rollback = removeShadowRecord(tid);
            if (rollback == ERROR_SUCCESS && hadPrevious) rollback = install(old);
            if (rollback == ERROR_SUCCESS && !nativeContext(thread, &saved, true, &lease)) rollback = GetLastError();
            if (rollback == ERROR_SUCCESS && hasShadowViews()) rollback = startOwnedHvm();
            log("ShadowPage context transaction failed: " + std::to_string(error) + "; rollback " + std::to_string(rollback));
            if (rollback != ERROR_SUCCESS) lastError_ = rollback;
        }
        if (!hasShadowViews()) (void)releaseOwnedHvm();
        SetLastError(error); return error == ERROR_SUCCESS;
    }

    DWORD Backend::beginShadowStep(DWORD tid, bool automatic)
    {
        const auto found = shadowStops_.find(tid);
        if (found == shadowStops_.end() || found->second.stepping) return ERROR_SUCCESS;
        ark::DriverHandle thread(openThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid));
        if (!thread.isValid()) return GetLastError();
        CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
        if (!nativeContext(thread.native(), &context, false)) return GetLastError();
        bool restart = false;
        DWORD error = pauseOwnedHvm(restart);
        if (error == ERROR_SUCCESS) error = removeShadowInt3(found->second.address);
        if (error != ERROR_SUCCESS) return error;
        found->second.userStep = !automatic && (context.EFlags & 0x100) != 0;
        context.Rip = found->second.address; context.EFlags |= 0x100;
        context.Dr6 &= ~static_cast<DWORD64>(found->second.mask);
        if (!nativeContext(thread.native(), &context, true)) return GetLastError();
        found->second.stepping = true;
        return !hasShadowViews() ? ERROR_SUCCESS : startOwnedHvm();
    }

    DWORD Backend::shadowEvent(DEBUG_EVENT& event, bool& consume)
    {
        consume = false;
        if (event.dwDebugEventCode != EXCEPTION_DEBUG_EVENT) return ERROR_SUCCESS;
        const auto pending = shadowStops_.find(event.dwThreadId);
        if (pending != shadowStops_.end() && pending->second.stepping)
        {
            const auto stop = pending->second;
            bool restart = false;
            DWORD error = pauseOwnedHvm(restart);
            if (error == ERROR_SUCCESS && shadowReferences_.count(stop.address) != 0) error = addShadowInt3(stop.address);
            if (error == ERROR_SUCCESS && hasShadowViews()) error = startOwnedHvm();
            if (error != ERROR_SUCCESS) return error;
            ark::DriverHandle thread(openThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId));
            CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
            if (!thread.isValid() || !nativeContext(thread.native(), &context, false)) return GetLastError();
            if (!stop.userStep) context.EFlags &= ~0x100UL;
            context.Dr6 &= ~static_cast<DWORD64>(stop.mask);
            if (!nativeContext(thread.native(), &context, true)) return GetLastError();
            consume = !stop.userStep && event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP;
            shadowStops_.erase(pending);
            if (!hasShadowViews()) (void)releaseOwnedHvm();
            return ERROR_SUCCESS;
        }
        if (event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) return ERROR_SUCCESS;
        const auto address = reinterpret_cast<std::uintptr_t>(event.u.Exception.ExceptionRecord.ExceptionAddress);
        if (shadowReferences_.count(address) == 0) return ERROR_SUCCESS;
        DWORD mask = 0;
        const auto record = breakpoints_.find(event.dwThreadId);
        if (record != breakpoints_.end() && record->second.shadow)
        {
            const auto& value = record->second.requested;
            const std::uint64_t points[] = {value.Dr0,value.Dr1,value.Dr2,value.Dr3};
            for (DWORD slot = 0; slot < 4; ++slot)
                if (points[slot] == address && (value.Dr7 & (3ULL << (2 * slot))) != 0 && ((value.Dr7 >> (16 + slot * 4)) & 3) == 0)
                    mask |= 1UL << slot;
        }
        shadowStops_[event.dwThreadId] = {address,mask,false,false};
        log("ShadowPage INT3 stop: TID " + std::to_string(event.dwThreadId) +
            ", address " + std::to_string(address) + ", slot mask " + std::to_string(mask));
        if (mask == 0) { consume = true; return beginShadowStep(event.dwThreadId, true); }
        ark::DriverHandle thread(openThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId));
        CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
        if (!thread.isValid() || !nativeContext(thread.native(), &context, false)) return GetLastError();
        context.Rip = address; context.Dr6 |= mask;
        if (!nativeContext(thread.native(), &context, true)) return GetLastError();
        // Translate this held, owned INT3 stop into CE's hardware-stop contract.
        event.u.Exception.ExceptionRecord.ExceptionCode = EXCEPTION_SINGLE_STEP;
        return ERROR_SUCCESS;
    }

    bool Backend::shadowWrite(DWORD pid, std::uint64_t address, const void* data, SIZE_T bytes,
        SIZE_T& transferred, bool& handled)
    {
        handled = false;
        const bool removing = shadowInt3_.find(address) != shadowInt3_.end();
        const bool adding = pid == attachedPid_ && attachedIdentity_.isValid() &&
            bytes == 1 && data != nullptr && *static_cast<const unsigned char*>(data) == 0xcc;
        if (adding && !shadowPrepared_ && !preferShadowExecution() && options_.shadowMemoryWrites == 0 &&
            status().eptBreakpointProtocol != 0) return false;
        if (adding && (options_.shadowMemoryWrites != 0 || options_.mode == KSWORD_DEBUGGER_MODE_STEALTH))
        {
            const auto region = client_.queryVirtualMemory(pid, address, 0, &driver_);
            if (region.io.ok && (region.protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
                return false; // A CE data freeze may happen to write the byte CC.
        }
        if (!removing && !adding)
        {
            if (options_.shadowMemoryWrites != 0 || options_.mode == KSWORD_DEBUGGER_MODE_STEALTH) return false;
            for (const auto point : shadowInt3_)
                if (bytes != 0 && address < (point & ~0xfffULL) + 4096 && address + bytes > (point & ~0xfffULL))
                { handled = true; log("Ordinary write refused: it overlaps an owned hidden INT3 page; select Shadow code patches or remove the breakpoint"); SetLastError(ERROR_BUSY); return false; }
            return false;
        }
        handled = true;
        if (adding && !preferShadowExecution() && options_.shadowMemoryWrites == 0)
        {
            if (options_.allowFallback == 0)
            { log("Software breakpoint Shadow fallback refused: fallback is disabled"); SetLastError(ERROR_NOT_SUPPORTED); return false; }
            recordFallback("EPT execution breakpoint", "Shadow Page hidden INT3", ERROR_NOT_SUPPORTED,
                "the resident EPT breakpoint protocol is unavailable");
        }
        if (pid != attachedPid_ || bytes != 1 || lastEvent_.dwProcessId != pid)
        { SetLastError(ERROR_INVALID_STATE); return false; }
        const bool set = *static_cast<const unsigned char*>(data) == 0xcc;
        if (removing && !set)
        {
            unsigned char original = 0; SIZE_T read = 0;
            if (!transfer(false, pid, address, &original, 1, read)) return false;
            if (original != *static_cast<const unsigned char*>(data))
            {
                if (options_.shadowMemoryWrites != 0 || options_.mode == KSWORD_DEBUGGER_MODE_STEALTH)
                { handled = false; return false; }
                log("Hidden INT3 restore refused: the replacement byte differs from original code"); SetLastError(ERROR_BUSY); return false;
            }
        }
        DWORD error = set ? ensureShadowHvm() : ERROR_SUCCESS;
        bool restart = false;
        if (error == ERROR_SUCCESS) error = pauseOwnedHvm(restart);
        if (error == ERROR_SUCCESS)
        {
            if (set)
            {
                error = addShadowInt3(address);
            }
            else error = removeShadowInt3(address);
        }
        if (error == ERROR_SUCCESS)
            error = !hasShadowViews() ? releaseOwnedHvm() : startOwnedHvm();
        else if (restart && hasShadowViews()) (void)startOwnedHvm();
        else if (!hasShadowViews()) (void)releaseOwnedHvm();
        transferred = error == ERROR_SUCCESS ? 1 : 0;
        if (error != ERROR_SUCCESS) log("ShadowPage INT3 operation failed: " + std::to_string(error));
        SetLastError(error); return error == ERROR_SUCCESS;
    }

    DWORD Backend::nativeRequest(KSWORD_ARK_DEBUGGER_REQUEST& request, KSWORD_ARK_DEBUGGER_RESPONSE& response,
        ark::DriverHandle* targetDevice)
    {
        request.version = KSWORD_ARK_DEBUGGER_VERSION;
        request.size = sizeof(request);
        const auto result = client_.deviceIoControl(IOCTL_KSWORD_ARK_DEBUGGER,
            &request, sizeof(request), &response, sizeof(response), targetDevice != nullptr ? targetDevice : &driver_);
        if (!result.ok) return result.win32Error;
        if (result.bytesReturned != sizeof(response) || response.version != KSWORD_ARK_DEBUGGER_VERSION ||
            response.size != sizeof(response)) return ERROR_INVALID_DATA;
        if (response.status >= 0) return ERROR_SUCCESS;
        using Convert = ULONG(WINAPI*)(LONG);
        const auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
        return convert == nullptr ? ERROR_GEN_FAILURE : convert(response.status);
    }

    HANDLE Backend::brokerOpenHandle(DWORD pid, DWORD tid, DWORD desiredAccess, BOOL inherit,
        ark::DriverHandle* targetDevice)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        // 身份租约文件只改变引用的生命周期，不改变代理请求的权限或策略。
        auto* const device = targetDevice != nullptr ? targetDevice : &driver_;
        if (!device->isValid()) { SetLastError(ERROR_DEVICE_NOT_CONNECTED); return nullptr; }
        if (pid == 0 || desiredAccess == 0) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        request.operation = KSWORD_ARK_DEBUGGER_OPEN_HANDLE;
        request.processId = pid;
        request.threadId = tid;
        // The protocol packs ACCESS_MASK into the ULONGLONG address field, zero-extended.
        request.address = static_cast<std::uint64_t>(desiredAccess);
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        const DWORD transportError = nativeRequest(request, response, device);
        if (transportError != ERROR_SUCCESS) { SetLastError(transportError); return nullptr; }
        if (response.status < 0)
        {
            using Convert = ULONG(WINAPI*)(LONG);
            const auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            SetLastError(convert == nullptr ? ERROR_GEN_FAILURE : convert(response.status));
            return nullptr;
        }
        // 驱动将已打开的内核句柄复制到本次 IOCTL 请求者的用户句柄表；
        // reserved0 是本进程可直接传给 Win32 并最终由 CloseHandle 关闭的句柄。
        const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(response.reserved0));
        if (handle == nullptr) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
        // 代理返回的是新插入的用户句柄，继承语义须与 Win32 Open* 一致。
        if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT,
            inherit ? HANDLE_FLAG_INHERIT : 0))
        {
            const DWORD error = GetLastError();
            // 已获得的句柄仍交给前端使用；继承位调整失败不能关闭服务路径。
            log("Broker handle inherit flag could not be adjusted: " + std::to_string(error));
        }
        SetLastError(ERROR_SUCCESS);
        return handle;
    }

    BOOL Backend::nativeContext(HANDLE thread, CONTEXT* context, bool write, TargetLease* bound)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (context == nullptr || sizeof(*context) != KSWORD_ARK_DEBUGGER_CONTEXT_BYTES)
        { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
        TargetLease localLease;
        TargetLease& lease = bound != nullptr ? *bound : localLease;
        if (bound == nullptr && !pinTarget(thread, true, lease)) return FALSE;
        const auto windowsContext = [this, target = lease.identity.native(), context, write](DWORD sourceError, const char* reason) {
            const BOOL completed = write ? ::SetThreadContext(target, context) : ::GetThreadContext(target, context);
            const DWORD actualError = GetLastError();
            recordFallback("R0 thread context", "Windows thread context", sourceError,
                std::string(reason) + "; Windows " + (write ? "write" : "read") +
                (completed ? " succeeded" : " failed, error=" + std::to_string(actualError)));
            SetLastError(actualError); return completed;
        };
        if (nativeContextFallback_)
        {
            return windowsContext(nativeContextFallbackError_,
                "cached R0 context failure in this adapter session; preserving Windows debug-port semantics");
        }
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = write ? KSWORD_ARK_DEBUGGER_SET_CONTEXT : KSWORD_ARK_DEBUGGER_GET_CONTEXT;
        request.flags = write ? KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED : 0;
        request.processId = lease.pid; request.threadId = lease.tid;
        request.contextFlags = context->ContextFlags;
        std::memcpy(request.context, context, sizeof(*context));
        const DWORD error = nativeRequest(request, response, lease.device(driver_));
        // Some Windows builds export PsGet/SetContextThread but reject the
        // target's context APC. Preserve actual Windows debug-port semantics.
        // Identity, access and policy rejections must not trigger this fallback.
        if ((error == ERROR_NOT_SUPPORTED || (!write && error == ERROR_GEN_FAILURE)) &&
            options_.allowFallback != 0 && options_.nativeContextFallback != 0)
        {
            const BOOL completed = windowsContext(error,
                "R0 context routine unavailable; preserving complete native register state");
            if (completed)
            {
                nativeContextFallback_ = true;
                nativeContextFallbackError_ = error;
            }
            return completed;
        }
        if (error != ERROR_SUCCESS) logRepeated("R0 context " + std::string(write ? "write" : "read") +
            " failed, PID " + std::to_string(request.processId) + ", TID " + std::to_string(request.threadId) +
            ", error " + std::to_string(error));
        if (error == ERROR_SUCCESS && !write) std::memcpy(context, response.context, sizeof(*context));
        SetLastError(error); return error == ERROR_SUCCESS ? TRUE : FALSE;
    }

    DWORD Backend::changeSuspendCount(HANDLE thread, bool suspend)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        TargetLease lease;
        if (!pinTarget(thread, true, lease)) return MAXDWORD;
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = suspend ? KSWORD_ARK_DEBUGGER_SUSPEND : KSWORD_ARK_DEBUGGER_RESUME;
        request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = lease.pid; request.threadId = lease.tid;
        const DWORD error = nativeRequest(request, response, lease.device(driver_));
        if (error == ERROR_NOT_SUPPORTED && options_.allowFallback != 0 && options_.nativeSuspendFallback != 0)
        {
            recordFallback("R0 suspend count", suspend ? "Windows SuspendThread" : "Windows ResumeThread", error,
                "optional kernel suspend-count export is unavailable");
            return suspend ? ::SuspendThread(lease.identity.native()) : ::ResumeThread(lease.identity.native());
        }
        if (error != ERROR_SUCCESS) log("R0 suspend-count operation failed without fallback, error " + std::to_string(error));
        SetLastError(error); return error == ERROR_SUCCESS ? response.previousSuspendCount : MAXDWORD;
    }
}
