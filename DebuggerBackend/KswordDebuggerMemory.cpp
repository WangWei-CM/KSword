#include "KswordDebuggerBackend.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ksword::debugger
{
    namespace
    {
        bool sameObject(HANDLE first, HANDLE second)
        {
            using Compare = LONG(NTAPI*)(HANDLE, HANDLE);
            static const auto compare = reinterpret_cast<Compare>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCompareObjects"));
            return compare != nullptr && compare(first, second) >= 0;
        }
    }
    HANDLE Backend::openProcess(DWORD access, BOOL inherit, DWORD pid)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (pid == 0) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
        // Preserve rights for CE operations outside the SDK table (for example
        // VirtualFreeEx), then fall back to identity-only access when necessary.
        HANDLE handle = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, inherit, pid);
        if (handle == nullptr) handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, inherit, pid);
        if (handle != nullptr) return handle;
        // Win32 OpenProcess refused — a third-party Ob callback has likely
        // stripped PROCESS_* rights for user-mode opens. Try the R0 broker
        // path (ObOpenObjectByPointer with PreviousMode=KernelMode) which
        // bypasses callbacks that gate on KernelHandle==FALSE. The returned
        // value is a real Windows handle in this process, usable for
        // ReadProcessMemory / DebugActiveProcess / etc.
        const DWORD openError = GetLastError();
        HANDLE brokered = brokerOpenHandle(pid, 0,
            access | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
        if (brokered == nullptr)
        {
            brokered = brokerOpenHandle(pid, 0,
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
        }
        if (brokered != nullptr)
        {
            recordFallback("Win32 OpenProcess", "R0 handle broker", openError,
                "ObOpenObjectByPointer(KernelMode) bypassed callback-stripped access");
            return brokered;
        }
        SetLastError(openError);
        for (auto iterator = processIds_.begin(); iterator != processIds_.end();)
        {
            if (!sameObject(iterator->first, iterator->second.identity.native())) iterator = processIds_.erase(iterator);
            else ++iterator;
        }
        if (processIds_.size() >= 1024) { SetLastError(ERROR_TOO_MANY_OPEN_FILES); return nullptr; }
        handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE identity = nullptr;
        if (handle == nullptr) return nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &identity, 0, FALSE, DUPLICATE_SAME_ACCESS))
        { const DWORD error = GetLastError(); CloseHandle(handle); SetLastError(error); return nullptr; }
        processIds_[handle] = ProcessProxy{pid, ark::DriverHandle(identity)};
        return handle;
    }

    DWORD Backend::processId(HANDLE process)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const DWORD actual = GetProcessId(process);
        if (actual != 0) return actual;
        const auto iterator = processIds_.find(process);
        if (iterator == processIds_.end() || !sameObject(process, iterator->second.identity.native()))
        { if (iterator != processIds_.end()) processIds_.erase(iterator); SetLastError(ERROR_INVALID_HANDLE); return 0; }
        return iterator->second.pid;
    }

    bool Backend::transfer(bool write, DWORD pid, std::uint64_t address, void* data,
        SIZE_T bytes, SIZE_T& transferred, bool dataWrite)
    {
        transferred = 0;
        ordinaryDataFallback_ = false;
        if (pid == 0 || (bytes != 0 && (data == nullptr || address == 0)) ||
            bytes > (std::numeric_limits<std::uint64_t>::max)() - address)
        { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        if (!driver_.isValid()) { SetLastError(ERROR_DEVICE_NOT_CONNECTED); return false; }
        if (write && useHvm_ && !dataWrite)
        {
            bool handled = false;
            const bool completed = shadowWrite(pid, address, data, bytes, transferred, handled);
            if (handled) return completed;
        }
        if (write && bytes != 0 && (options_.shadowMemoryWrites != 0 || options_.mode == KSWORD_DEBUGGER_MODE_STEALTH))
        {
            bool handled = false;
            const bool completed = shadowMemoryWrite(pid, address, data, bytes, transferred, handled, dataWrite);
            if (handled) return completed;
        }
        while (transferred < bytes)
        {
            const auto count = static_cast<DWORD>((std::min)(bytes - transferred,
                static_cast<SIZE_T>(useHvm_ ? KSWORD_ARK_HVM_MEMORY_MAX_BYTES :
                    (write ? KSWORD_ARK_MEMORY_WRITE_MAX_BYTES : KSWORD_ARK_MEMORY_READ_MAX_BYTES))));
            auto* part = static_cast<unsigned char*>(data) + transferred;
            if (useHvm_)
            {
                const auto result = client_.hvmMemory(write ? KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL : KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL,
                    address + transferred, 0, count, write ? part : nullptr, true, true, pid, &driver_);
                if (!result.io.ok || result.response.status != KSWORD_ARK_HVM_MEMORY_STATUS_OK || result.response.usedDirectWindow == 0)
                {
                    const DWORD error = result.io.ok ? ERROR_PARTIAL_COPY : result.io.win32Error;
                    SetLastError(error);
                    logRepeated(std::string("HVM ") + (write ? "write" : "read") +
                        " refused: pid=" + std::to_string(pid) + " address=" + std::to_string(address + transferred) +
                        " status=" + std::to_string(result.response.status) + " ntStatus=" +
                        std::to_string(static_cast<ULONG>(result.response.ntStatus)) + " directWindow=" +
                        std::to_string(result.response.usedDirectWindow) + " error=" + std::to_string(error) +
                        "; strict private-window access has no ordinary-memory fallback");
                    return false;
                }
                const DWORD done = result.response.bytesTransferred;
                if (done > count) { SetLastError(ERROR_INVALID_DATA); return false; }
                if (!write) std::memcpy(part, result.response.data, done);
                transferred += done;
                if (done != count) { SetLastError(ERROR_PARTIAL_COPY); return false; }
            }
            else if (write)
            {
                const std::vector<std::uint8_t> payload(part, part + count);
                const auto result = client_.writeVirtualMemory(pid, address + transferred, payload,
                    KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED | KSWORD_ARK_MEMORY_WRITE_FLAG_FORCE, &driver_);
                if (!result.io.ok) { SetLastError(result.io.win32Error); return false; }
                if (result.bytesWritten > count) { SetLastError(ERROR_INVALID_DATA); return false; }
                transferred += result.bytesWritten;
                if (result.writeStatus != KSWORD_ARK_MEMORY_WRITE_STATUS_OK || result.bytesWritten != count)
                { SetLastError(ERROR_PARTIAL_COPY); return false; }
            }
            else
            {
                const auto result = client_.readVirtualMemory(pid, address + transferred, count, 0, &driver_);
                if (!result.io.ok) { SetLastError(result.io.win32Error); return false; }
                if (result.data.size() > count) { SetLastError(ERROR_INVALID_DATA); return false; }
                std::memcpy(part, result.data.data(), result.data.size());
                transferred += result.data.size();
                if (result.readStatus != KSWORD_ARK_MEMORY_READ_STATUS_OK || result.data.size() != count)
                { SetLastError(ERROR_PARTIAL_COPY); return false; }
            }
        }
        SetLastError(ERROR_SUCCESS);
        return true;
    }

    bool Backend::shadowMemoryWrite(DWORD pid, std::uint64_t address, const void* data,
        SIZE_T bytes, SIZE_T& transferred, bool& handled, bool dataWrite)
    {
        handled = true;
        if (shadowRecoveryRequired_)
        { log("Shadow write refused: previous rollback was incomplete; restore all memory patches first"); SetLastError(ERROR_INVALID_STATE); return false; }
        bool code = false, ordinary = false;
        for (std::uint64_t cursor = address; cursor < address + bytes;)
        {
            const auto region = client_.queryVirtualMemory(pid, cursor, 0, &driver_);
            if (!region.io.ok || (region.fieldFlags & KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT) == 0 ||
                region.state != MEM_COMMIT || region.regionSize == 0 || region.baseAddress > cursor ||
                region.regionSize > (std::numeric_limits<std::uint64_t>::max)() - region.baseAddress ||
                cursor >= region.baseAddress + region.regionSize || (region.protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            {
                const DWORD error = region.io.ok ? ERROR_INVALID_DATA : region.io.win32Error;
                log("Shadow write refused: cannot classify committed target pages, error " + std::to_string(error));
                SetLastError(error); return false;
            }
            const bool executable = !dataWrite && (region.protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            if (executable && region.type != MEM_PRIVATE && region.type != MEM_IMAGE && region.type != MEM_MAPPED)
            {
                log("Shadow code write refused: unsupported executable backing type");
                SetLastError(ERROR_NOT_SUPPORTED); return false;
            }
            code |= executable; ordinary |= !executable;
            cursor = (std::min)(address + bytes, region.baseAddress + region.regionSize);
        }
        if (ordinary)
        {
            if (code || options_.allowFallback == 0)
            {
                log(code ? "Shadow write refused: one write spans executable and data pages"
                    : "Shadow data write/freeze refused: execution views do not change ordinary data reads; fallback is disabled");
                SetLastError(ERROR_NOT_SUPPORTED); return false;
            }
            recordFallback("Shadow Page memory write", useHvm_ ? "HVM ordinary data write" : "R0 ordinary data write",
                ERROR_NOT_SUPPORTED, dataWrite ?
                "frontend temporarily made data RWX; use the verified original data classification; original data bytes will change" :
                "non-executable data requires a real data write/freeze; original data bytes will change");
            ordinaryDataFallback_ = true;
            handled = false; return false;
        }
        if (!useHvm_)
        {
            log("Shadow code write refused: select HVM before changing execution-view bytes");
            SetLastError(ERROR_INVALID_STATE); return false;
        }
        // One driver update is transactional. A multi-page/multi-payload edit
        // would require a batch ABI to roll back every prior page atomically.
        if (bytes > KSWORD_ARK_DEBUGGER_CONTEXT_BYTES || bytes > 4096 - (address & 0xfffULL))
        {
            log("Shadow code write refused before mutation: one atomic request must fit one page and 1232 bytes");
            SetLastError(ERROR_NOT_SUPPORTED); return false;
        }
        auto target = shadowWrites_.find(pid);
        if (target != shadowWrites_.end() && WaitForSingleObject(target->second.identity.native(), 0) != WAIT_TIMEOUT)
        {
            // R0 exit cleanup already removed the retained target's mappings.
            shadowWrites_.erase(target); target = shadowWrites_.end();
        }
        if (target == shadowWrites_.end())
        {
            ark::DriverHandle identity(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
            if (!identity.isValid()) { SetLastError(GetLastError()); return false; }
            if (GetProcessId(identity.native()) != pid || WaitForSingleObject(identity.native(), 0) != WAIT_TIMEOUT)
            { SetLastError(ERROR_INVALID_STATE); return false; }
            ShadowWriteTarget value{}; value.identity = std::move(identity);
            target = shadowWrites_.emplace(pid, std::move(value)).first;
        }
        DWORD addedPages = 0;
        for (std::uint64_t page = address & ~0xfffULL; page <= ((address + bytes - 1) & ~0xfffULL); page += 4096)
            if (target->second.pages.count(page) == 0) ++addedPages;
        std::unordered_set<std::uint64_t> breakpointPages;
        for (const auto point : shadowInt3_)
            if (pid != attachedPid_ || (target->second.pages.count(point & ~0xfffULL) == 0 &&
                ((point & ~0xfffULL) < (address & ~0xfffULL) || (point & ~0xfffULL) > ((address + bytes - 1) & ~0xfffULL))))
                breakpointPages.insert(point & ~0xfffULL);
        if (shadowWritePageCount() + addedPages + breakpointPages.size() > options_.maxShadowPages)
        {
            if (target->second.pages.empty()) shadowWrites_.erase(target);
            log("Shadow code write refused: configured page limit reached"); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false;
        }
        KSWORD_ARK_DEBUGGER_REQUEST query{}; KSWORD_ARK_DEBUGGER_RESPONSE capability{};
        DWORD error = nativeRequest(query, capability);
        if (error == ERROR_SUCCESS && (capability.capabilities & KSWORD_ARK_DEBUGGER_CAP_SHADOW_WRITES) == 0)
            error = ERROR_NOT_SUPPORTED;
        if (error == ERROR_SUCCESS) error = ensureShadowHvm();
        bool restart = false;
        if (error == ERROR_SUCCESS) error = pauseOwnedHvm(restart);
        const std::uint64_t writePage = address & ~0xfffULL;
        const auto previous = target->second.pages.find(writePage);
        const bool hadPrevious = previous != target->second.pages.end();
        const ShadowWritePage saved = hadPrevious ? previous->second : ShadowWritePage{};
        while (error == ERROR_SUCCESS && transferred < bytes)
        {
            const std::uint64_t current = address + transferred;
            const DWORD count = static_cast<DWORD>((std::min)({bytes - transferred,
                static_cast<SIZE_T>(4096 - (current & 0xfffULL)), static_cast<SIZE_T>(KSWORD_ARK_DEBUGGER_CONTEXT_BYTES)}));
            KSWORD_ARK_DEBUGGER_REQUEST request{}; KSWORD_ARK_DEBUGGER_RESPONSE response{};
            request.operation = KSWORD_ARK_DEBUGGER_SHADOW_WRITE; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
            request.processId = pid; request.address = current; request.bytes = count;
            std::memcpy(request.context, static_cast<const unsigned char*>(data) + transferred, count);
            error = nativeRequest(request, response);
            if (error != ERROR_SUCCESS)
            {
                if ((response.reserved1 & KSWORD_ARK_DEBUGGER_SHADOW_BACKING_SHARED) != 0)
                    log("Shadow code write refused, error " + std::to_string(error) +
                        ": backing is still shared; image/mapped code requires Windows COW-private backing (pinned Valid=1, Shared=0); no automatic same-byte write into a running target");
                else if ((response.reserved1 & KSWORD_ARK_DEBUGGER_SHADOW_BACKING_UNVERIFIABLE) != 0)
                    log("Shadow code write refused, error " + std::to_string(error) +
                        ": pinned private/COW backing cannot be verified; no original code write fallback");
                else if ((response.reserved1 & KSWORD_ARK_DEBUGGER_SHADOW_MAPPING_CHANGED) != 0)
                    log("Shadow code write refused, error " + std::to_string(error) +
                        ": virtual mapping no longer names the retained physical page; restore and reinstall the execution patch");
            }
            if (error == ERROR_SUCCESS)
            {
                auto& page = target->second.pages[current & ~0xfffULL];
                for (DWORD offset = 0; offset < count; ++offset)
                {
                    const auto index = static_cast<SIZE_T>((current & 0xfffULL) + offset);
                    page.mask.set(index); page.bytes[index] = static_cast<const unsigned char*>(data)[transferred + offset];
                }
                transferred += count;
            }
        }
        if (error == ERROR_SUCCESS) error = startOwnedHvm();
        if (error != ERROR_SUCCESS)
        {
            const bool wrotePatch = transferred != 0;
            DWORD rollback = ERROR_SUCCESS;
            if (transferred != 0)
            {
                bool ignored = false;
                rollback = pauseOwnedHvm(ignored);
                KSWORD_ARK_DEBUGGER_REQUEST restore{}; KSWORD_ARK_DEBUGGER_RESPONSE response{};
                restore.operation = KSWORD_ARK_DEBUGGER_SHADOW_RESTORE; restore.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
                restore.processId = pid; restore.address = address; restore.bytes = bytes;
                if (rollback == ERROR_SUCCESS) rollback = nativeRequest(restore, response);
                const auto first = static_cast<SIZE_T>(address & 0xfffULL);
                for (SIZE_T offset = first; rollback == ERROR_SUCCESS && offset < first + bytes;)
                {
                    if (!saved.mask.test(offset)) { ++offset; continue; }
                    const SIZE_T begin = offset;
                    while (offset < first + bytes && saved.mask.test(offset)) ++offset;
                    KSWORD_ARK_DEBUGGER_REQUEST prior{};
                    prior.operation = KSWORD_ARK_DEBUGGER_SHADOW_WRITE; prior.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
                    prior.processId = pid; prior.address = writePage + begin; prior.bytes = offset - begin;
                    std::memcpy(prior.context, saved.bytes.data() + begin, static_cast<SIZE_T>(prior.bytes));
                    rollback = nativeRequest(prior, response);
                }
                if (rollback == ERROR_SUCCESS)
                {
                    if (hadPrevious) target->second.pages[writePage] = saved;
                    else target->second.pages.erase(writePage);
                    transferred = 0;
                }
                else { lastError_ = rollback; shadowRecoveryRequired_ = true; }
            }
            if (target->second.pages.empty()) shadowWrites_.erase(target);
            if (wrotePatch && rollback == ERROR_SUCCESS && hasShadowViews() && restart) rollback = startOwnedHvm();
            if (rollback != ERROR_SUCCESS) shadowRecoveryRequired_ = true;
            if (shadowRecoveryRequired_)
            {
                shadowQuarantinedTargets_.insert(pid);
                KSWORD_ARK_DEBUGGER_REQUEST quarantine{}; KSWORD_ARK_DEBUGGER_RESPONSE response{};
                quarantine.operation = KSWORD_ARK_DEBUGGER_SHADOW_QUARANTINE; quarantine.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
                quarantine.processId = pid;
                const DWORD guarded = nativeRequest(quarantine, response);
                log("Shadow content recovery quarantine: PID " + std::to_string(pid) +
                    "; direct driver starts blocked until full restore; latch result " + std::to_string(guarded));
            }
            log("Shadow code write failed, error " + std::to_string(error) +
                "; rollback " + std::to_string(rollback) + "; original data reads are unchanged");
            if (!hasShadowViews()) (void)releaseOwnedHvm();
        }
        else log("Shadow code write accepted: verified private backing (including image/mapped COW); execution view only, " +
            std::to_string(transferred) + " bytes; ordinary reads retain original bytes");
        SetLastError(error); return error == ERROR_SUCCESS;
    }

    DWORD Backend::restoreShadowWrites(std::uint64_t address, std::uint64_t bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if ((address == 0) != (bytes == 0) || bytes > (std::numeric_limits<std::uint64_t>::max)() - address)
            return ERROR_INVALID_PARAMETER;
        if (shadowWrites_.empty() && shadowQuarantinedTargets_.empty())
        { if (address == 0) shadowRecoveryRequired_ = false; return ERROR_SUCCESS; }
        bool restart = false;
        DWORD error = pauseOwnedHvm(restart);
        if (error != ERROR_SUCCESS) return error;
        for (auto target = shadowWrites_.begin(); target != shadowWrites_.end();)
        {
            if (address == 0)
            {
                KSWORD_ARK_DEBUGGER_REQUEST request{}; KSWORD_ARK_DEBUGGER_RESPONSE response{};
                request.operation = KSWORD_ARK_DEBUGGER_SHADOW_RESTORE; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
                request.processId = target->first;
                error = nativeRequest(request, response);
                if (error != ERROR_SUCCESS) { log("Full Shadow memory restore failed: " + std::to_string(error)); return error; }
                shadowQuarantinedTargets_.erase(target->first);
                target = shadowWrites_.erase(target); continue;
            }
            for (auto page = target->second.pages.begin(); page != target->second.pages.end();)
            {
                const auto start = address == 0 ? page->first : (std::max)(address, page->first);
                const auto end = address == 0 ? page->first + 4096 : (std::min)(address + bytes, page->first + 4096);
                if (start >= end) { ++page; continue; }
                KSWORD_ARK_DEBUGGER_REQUEST request{}; KSWORD_ARK_DEBUGGER_RESPONSE response{};
                request.operation = KSWORD_ARK_DEBUGGER_SHADOW_RESTORE; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
                request.processId = target->first; request.address = start; request.bytes = end - start;
                error = nativeRequest(request, response);
                if (error != ERROR_SUCCESS && error != ERROR_NOT_FOUND)
                { log("Shadow memory restore failed: " + std::to_string(error)); return error; }
                for (auto offset = start - page->first; offset < end - page->first; ++offset) page->second.mask.reset(static_cast<SIZE_T>(offset));
                if (page->second.mask.none()) page = target->second.pages.erase(page); else ++page;
            }
            if (target->second.pages.empty()) target = shadowWrites_.erase(target); else ++target;
        }
        if (address == 0)
        {
            for (const auto pid : shadowQuarantinedTargets_)
            {
                KSWORD_ARK_DEBUGGER_REQUEST request{}; KSWORD_ARK_DEBUGGER_RESPONSE response{};
                request.operation = KSWORD_ARK_DEBUGGER_SHADOW_RESTORE; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
                request.processId = pid;
                error = nativeRequest(request, response);
                if (error != ERROR_SUCCESS) { log("Shadow quarantine restore failed: " + std::to_string(error)); return error; }
            }
            shadowQuarantinedTargets_.clear(); shadowRecoveryRequired_ = false;
        }
        error = hasShadowViews() || !breakpoints_.empty() ? startOwnedHvm() : releaseOwnedHvm();
        log("Shadow memory restore completed; hidden breakpoints retained, error " + std::to_string(error));
        return error;
    }

    BOOL Backend::readMemory(HANDLE process, LPCVOID address, LPVOID data, SIZE_T bytes, SIZE_T* transferred)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        SIZE_T done = 0;
        const bool ok = transfer(false, processId(process), reinterpret_cast<std::uintptr_t>(address), data, bytes, done);
        if (transferred != nullptr) *transferred = done;
        return ok ? TRUE : FALSE;
    }

    BOOL Backend::writeMemory(HANDLE process, LPVOID address, LPCVOID data, SIZE_T bytes, SIZE_T* transferred,
        MemoryWriteKind kind)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        SIZE_T done = 0;
        const bool ok = transfer(true, processId(process), reinterpret_cast<std::uintptr_t>(address), const_cast<void*>(data), bytes, done,
            kind == MemoryWriteKind::data);
        const DWORD error = GetLastError();
        if (ordinaryDataFallback_)
            log(std::string("Ordinary data fallback ") + (ok ? "completed" : "failed") +
                ": " + std::to_string(done) + "/" + std::to_string(bytes) + " bytes; error " + std::to_string(error) +
                (done != 0 ? "; original data bytes changed for the completed range" : "; original data bytes unchanged"));
        if (transferred != nullptr) *transferred = done;
        SetLastError(error);
        return ok ? TRUE : FALSE;
    }

    SIZE_T Backend::queryMemory(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (info == nullptr || bytes < sizeof(*info)) { SetLastError(ERROR_BAD_LENGTH); return 0; }
        const auto requested = reinterpret_cast<std::uintptr_t>(address);
        const auto result = client_.queryVirtualMemory(processId(process), requested, 0, &driver_);
        if (!result.io.ok || (result.queryStatus != KSWORD_ARK_MEMORY_QUERY_STATUS_OK && result.queryStatus != KSWORD_ARK_MEMORY_QUERY_STATUS_PARTIAL) ||
            (result.fieldFlags & KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT) == 0 ||
            result.regionSize == 0 || result.baseAddress > requested ||
            result.regionSize > (std::numeric_limits<std::uint64_t>::max)() - result.baseAddress ||
            requested >= result.baseAddress + result.regionSize ||
            result.baseAddress > (std::numeric_limits<std::uintptr_t>::max)() ||
            result.allocationBase > (std::numeric_limits<std::uintptr_t>::max)() ||
            result.regionSize > (std::numeric_limits<SIZE_T>::max)())
        { SetLastError(result.io.ok ? ERROR_INVALID_DATA : result.io.win32Error); return 0; }
        *info = MEMORY_BASIC_INFORMATION{};
        info->BaseAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(result.baseAddress));
        info->AllocationBase = reinterpret_cast<void*>(static_cast<std::uintptr_t>(result.allocationBase));
        info->AllocationProtect = result.allocationProtect;
        info->RegionSize = static_cast<SIZE_T>(result.regionSize);
        info->State = result.state; info->Protect = result.protect; info->Type = result.type;
        return sizeof(*info);
    }

    BOOL Backend::protectMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD protection, PDWORD previous)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (previous == nullptr) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_PROTECT; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = processId(process); request.address = reinterpret_cast<std::uintptr_t>(address);
        request.bytes = bytes; request.protection = protection;
        const DWORD error = nativeRequest(request, response);
        if (error == ERROR_SUCCESS) *previous = response.previousProtection;
        SetLastError(error); return error == ERROR_SUCCESS ? TRUE : FALSE;
    }

    LPVOID Backend::allocateMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD type, DWORD protection)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_ALLOCATE; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = processId(process); request.address = reinterpret_cast<std::uintptr_t>(address);
        request.bytes = bytes; request.allocationType = type; request.protection = protection;
        const DWORD error = nativeRequest(request, response);
        SetLastError(error);
        return error == ERROR_SUCCESS ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(response.address)) : nullptr;
    }

    BOOL Backend::freeMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD type)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_FREE; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = processId(process); request.address = reinterpret_cast<std::uintptr_t>(address);
        request.bytes = bytes; request.allocationType = type;
        const DWORD error = nativeRequest(request, response);
        SetLastError(error); return error == ERROR_SUCCESS ? TRUE : FALSE;
    }

    HANDLE Backend::createThread(HANDLE process, LPSECURITY_ATTRIBUTES attributes, SIZE_T stackBytes,
        LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD tid)
    {
        const HANDLE handle = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId(process));
        if (handle == nullptr) return nullptr;
        HANDLE result = CreateRemoteThread(handle, attributes, stackBytes, start, parameter, flags, tid);
        const DWORD error = GetLastError(); CloseHandle(handle); SetLastError(error); return result;
    }
}
