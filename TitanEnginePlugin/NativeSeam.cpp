#include "Proxy.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ksword::titan
{
    thread_local bool nativeLogicalRead = false;
    namespace
    {
        using ReadContext = BOOL(WINAPI*)(HANDLE, LPCONTEXT);
        using WriteContext = BOOL(WINAPI*)(HANDLE, const CONTEXT*);
        using WaitEvent = BOOL(WINAPI*)(LPDEBUG_EVENT, DWORD);
        using ContinueEvent = BOOL(WINAPI*)(DWORD, DWORD, DWORD);
        ReadContext windowsGet = &::GetThreadContext;
        WriteContext windowsSet = &::SetThreadContext;
        WaitEvent windowsWait = &::WaitForDebugEvent;
        ContinueEvent windowsContinue = &::ContinueDebugEvent;
        std::mutex pendingMutex;
        std::unordered_map<DWORD, CONTEXT> pendingDebug;
        std::atomic<DWORD> seamError{ERROR_SUCCESS};
        std::atomic<DWORD> observedPid{0};
        std::atomic<std::uint64_t> observedGeneration{0};
        std::atomic<bool> processExitPending{false};
        bool eventHeld = false;
        DEBUG_EVENT heldEvent{};
        std::atomic<std::uint64_t> eventSequence{0};

        BOOL WINAPI readMemory(HANDLE process, LPCVOID address, LPVOID data, SIZE_T bytes, SIZE_T* transferred)
        {
            if (transferred != nullptr) *transferred = 0;
            // 只有组合读取与叠加需要作用域，纯原生转发沿用 Windows 合同。
            debugger::Backend::TargetLease target;
            const bool overlay = nativeLogicalRead && hvmSelected.load();
            if (overlay)
            {
                if (!debugger::backend().retainTarget(process, false, target)) return FALSE;
                process = target.native();
            }
            SIZE_T done = 0;
            const BOOL completed = ::ReadProcessMemory(process, address, data, bytes, &done);
            const DWORD error = GetLastError();
            if (completed && overlay)
                debugger::backend().overlayShadowBytes(target.processId(), reinterpret_cast<std::uintptr_t>(address), data, done);
            if (transferred != nullptr) *transferred = done;
            SetLastError(error); return completed;
        }

        BOOL WINAPI writeMemory(HANDLE process, LPVOID address, LPCVOID data, SIZE_T bytes, SIZE_T* transferred)
        {
            if (transferred != nullptr) *transferred = 0;
            const auto options = debugger::backend().options();
            if (!hvmSelected.load() && options.shadowMemoryWrites == 0 && options.mode != KSWORD_DEBUGGER_MODE_STEALTH)
                return ::WriteProcessMemory(process, address, data, bytes, transferred);
            // 逻辑字节判断、原生读取和后端写入必须消费同一对象。
            debugger::Backend::TargetLease target;
            if (!debugger::backend().retainTarget(process, false, target)) return FALSE;
            process = target.native();
            // Titan can restore a temporarily removed breakpoint again during
            // stepping. An already matching logical byte is not a user code
            // patch and must not allocate a persistent Shadow write page.
            const DWORD pid = target.processId();
            if (nativeLogicalRead && hvmSelected.load() && bytes == 1 && data != nullptr &&
                *static_cast<const BYTE*>(data) != 0xcc &&
                isShadowBreakpointAddress(pid, reinterpret_cast<ULONG_PTR>(address)))
            {
                BYTE logical = 0; SIZE_T read = 0;
                if (::ReadProcessMemory(process, address, &logical, 1, &read) && read == 1)
                {
                    debugger::backend().overlayShadowBytes(pid, reinterpret_cast<ULONG_PTR>(address), &logical, 1);
                    if (logical == *static_cast<const BYTE*>(data))
                    {
                        if (transferred != nullptr) *transferred = 1;
                        log("Titan breakpoint restore: logical byte already matches; no Shadow code patch created");
                        SetLastError(ERROR_SUCCESS); return TRUE;
                    }
                }
            }
            if (options.shadowMemoryWrites != 0 || options.mode == KSWORD_DEBUGGER_MODE_STEALTH)
                return debugger::backend().writeMemory(process, address, data, bytes, transferred);
            if (!hvmSelected.load() || pid != observedPid.load() || observedGeneration.load() == 0)
                return ::WriteProcessMemory(process, address, data, bytes, transferred);
            return debugger::backend().writeMemory(process, address, data, bytes, transferred);
        }

        struct ContextWrite { CONTEXT* full; };
        BOOL WINAPI writeContext(HANDLE thread, const CONTEXT* debug, BOOL rollback, void* opaque)
        {
            if (rollback) return windowsSet(thread, debug);
            auto* state = static_cast<ContextWrite*>(opaque);
            state->full->Dr0 = debug->Dr0; state->full->Dr1 = debug->Dr1;
            state->full->Dr2 = debug->Dr2; state->full->Dr3 = debug->Dr3;
            state->full->Dr6 = debug->Dr6; state->full->Dr7 = debug->Dr7;
            return windowsSet(thread, state->full);
        }

        BOOL WINAPI getContext(HANDLE thread, LPCONTEXT context)
        {
            std::lock_guard<std::recursive_mutex> policy(policyMutex);
            if (!hvmSelected.load() || observedGeneration.load() == 0) return windowsGet(thread, context);
            debugger::Backend::TargetLease target;
            if (!debugger::backend().retainTarget(thread, true, target)) return FALSE;
            thread = target.native();
            if (!windowsGet(thread, context))
            {
                const DWORD error = GetLastError();
                if (hvmSelected.load() && observedGeneration.load() != 0 && target.processId() == observedPid.load()) seamError = error;
                SetLastError(error); return FALSE;
            }
            const DWORD error = GetLastError();
            if (hvmSelected.load() && observedGeneration.load() != 0 &&
                target.processId() == observedPid.load() &&
                (context->ContextFlags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS)
            {
                if (!debugger::backend().overlayNativeDebugContext(thread, context, observedGeneration.load())) return FALSE;
                std::lock_guard<std::mutex> lock(pendingMutex);
                const auto found = pendingDebug.find(target.threadId());
                if (found != pendingDebug.end())
                {
                    context->Dr0 = found->second.Dr0; context->Dr1 = found->second.Dr1;
                    context->Dr2 = found->second.Dr2; context->Dr3 = found->second.Dr3;
                    context->Dr7 = found->second.Dr7;
                }
            }
            SetLastError(error); return TRUE;
        }

        BOOL WINAPI setContext(HANDLE thread, const CONTEXT* context)
        {
            std::lock_guard<std::recursive_mutex> policy(policyMutex);
            if (context == nullptr || !hvmSelected.load() || observedGeneration.load() == 0)
                return windowsSet(thread, context);
            debugger::Backend::TargetLease target;
            if (!debugger::backend().retainTarget(thread, true, target)) return FALSE;
            thread = target.native();
            if (target.processId() != observedPid.load() ||
                (context->ContextFlags & CONTEXT_DEBUG_REGISTERS) != CONTEXT_DEBUG_REGISTERS)
                return windowsSet(thread, context);
            try
            {
                // CopyContext carries the actual XSTATE buffer, not just sizeof(CONTEXT).
                DWORD length = 0; CONTEXT* full = nullptr;
                (void)InitializeContext(nullptr, context->ContextFlags, &full, &length);
                if (length == 0)
                { const DWORD error = GetLastError(); seamError = error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error; SetLastError(seamError.load()); return FALSE; }
                std::vector<BYTE> storage(length);
                if (!InitializeContext(storage.data(), context->ContextFlags, &full, &length) ||
                    !CopyContext(full, context->ContextFlags, const_cast<CONTEXT*>(context)))
                { const DWORD error = GetLastError(); seamError = error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error; SetLastError(seamError.load()); return FALSE; }
                CONTEXT logical = *full;
                logical.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                const std::array<DWORD64, 4> addresses{logical.Dr0, logical.Dr1, logical.Dr2, logical.Dr3};
                DWORD64 incomplete = 0;
                for (unsigned i = 0; i < 4; ++i)
                    if (addresses[i] == 0 && (logical.Dr7 & (3ULL << (2 * i))) != 0) incomplete |= 3ULL << (2 * i);
                CONTEXT ready = logical;
                ready.Dr7 &= ~incomplete;
                ContextWrite write{full};
                if (!debugger::backend().applyNativeDebugContext(thread, &ready, &writeContext, &write, observedGeneration.load()))
                { seamError = GetLastError(); return FALSE; }
                // Titan programs DR7 before the address; stage that short sequence.
                const DWORD tid = target.threadId();
                {
                    std::lock_guard<std::mutex> lock(pendingMutex);
                    if (incomplete != 0) pendingDebug[tid] = logical;
                    else pendingDebug.erase(tid);
                }
                return TRUE;
            }
            catch (const std::bad_alloc&) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); seamError = ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
            catch (...) { SetLastError(ERROR_GEN_FAILURE); seamError = ERROR_GEN_FAILURE; return FALSE; }
        }

        BOOL WINAPI waitEvent(LPDEBUG_EVENT event, DWORD timeout)
        {
            if (!windowsWait(event, timeout)) return FALSE;
            const DWORD nativeError = GetLastError();
            std::lock_guard<std::recursive_mutex> policy(policyMutex);
            eventHeld = true; heldEvent = *event;
            ++eventSequence;
            if (event->dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT)
            {
                observedPid = event->dwProcessId;
                if (hvmSelected.load())
                {
                    std::uint64_t generation = 0;
                    const DWORD error = debugger::backend().observeNativeSession(event->dwProcessId, &generation);
                    if (error != ERROR_SUCCESS) { seamError = error; log("Native session adoption failed: " + std::to_string(error)); }
                    else observedGeneration = generation;
                }
            }
            if (hvmSelected.load() && observedGeneration.load() != 0)
            {
                const DWORD error = debugger::backend().observeNativeEvent(*event, observedGeneration.load());
                if (error != ERROR_SUCCESS) seamError = error;
            }
            if (event->dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT)
            {
                {
                    std::lock_guard<std::mutex> lock(pendingMutex); pendingDebug.erase(event->dwThreadId);
                }
                if (hvmSelected.load())
                {
                    const DWORD error = debugger::backend().retireNativeThread(event->dwThreadId, observedGeneration.load());
                    if (error != ERROR_SUCCESS) seamError = error;
                }
            }
            if (event->dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            {
                processExitPending = true;
            }
            if (event->dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT && hvmSelected.load())
            {
                const DWORD error = retireNativeModule(event->dwProcessId,
                    reinterpret_cast<ULONG_PTR>(event->u.UnloadDll.lpBaseOfDll));
                if (error != ERROR_SUCCESS) seamError = error;
            }
            SetLastError(nativeError); return TRUE;
        }

        BOOL WINAPI continueEvent(DWORD pid, DWORD tid, DWORD disposition)
        {
            std::lock_guard<std::recursive_mutex> policy(policyMutex);
            if (hvmSelected.load())
            {
                const DWORD error = seamError.load();
                if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
                if (observedPid.load() != 0 && observedGeneration.load() == 0)
                { SetLastError(ERROR_INVALID_STATE); return FALSE; }
            }
            if (hvmSelected.load() && observedGeneration.load() != 0)
            {
                {
                    std::lock_guard<std::mutex> lock(pendingMutex);
                    if (!pendingDebug.empty()) { SetLastError(ERROR_INVALID_STATE); log("Continue refused: incomplete native DR programming"); return FALSE; }
                }
                const DWORD failure = seamError.load();
                if (failure != ERROR_SUCCESS) { SetLastError(failure); return FALSE; }
                const DWORD error = debugger::backend().validateNativeContinue(pid, tid, observedGeneration.load());
                if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
            }
            const BOOL result = windowsContinue(pid, tid, disposition);
            const DWORD error = GetLastError();
            if (result) { eventHeld = false; heldEvent = {}; }
            if (result && processExitPending.exchange(false))
            {
                const DWORD cleanup = releaseSession();
                if (cleanup != ERROR_SUCCESS) { seamError = cleanup; log("Process exit EPT retirement failed: " + std::to_string(cleanup)); }
            }
            SetLastError(error); return result;
        }
    }

    void clearSeamError() { seamError = ERROR_SUCCESS; }
    DWORD getSeamError() { return seamError.load(); }
    std::uint64_t nativeEventSequence() { return eventSequence.load(); }
    bool hasPendingDebug() { std::lock_guard<std::mutex> lock(pendingMutex); return !pendingDebug.empty(); }
    bool nativeEventHeld() { std::lock_guard<std::recursive_mutex> lock(policyMutex); return eventHeld; }
    DWORD nativeProcessId() { return observedPid.load(); }
    DWORD adoptCurrentNativeSession()
    {
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        const DWORD pid = observedPid.load();
        if (pid == 0) return ERROR_SUCCESS;
        if (!eventHeld) return ERROR_BUSY;
        std::uint64_t generation = 0;
        const DWORD error = debugger::backend().observeNativeSession(pid, &generation);
        if (error == ERROR_SUCCESS)
        {
            observedGeneration = generation;
            if (heldEvent.dwProcessId == pid)
                return debugger::backend().observeNativeEvent(heldEvent, generation);
        }
        return error;
    }
    DWORD releaseSession()
    {
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        const DWORD error = debugger::backend().releaseNativeSession(observedGeneration.load());
        if (error == ERROR_SUCCESS)
        {
            observedPid = 0; observedGeneration = 0; seamError = ERROR_SUCCESS; processExitPending = false;
            eventHeld = false; heldEvent = {};
            { std::lock_guard<std::mutex> lock(pendingMutex); pendingDebug.clear(); }
            clearHardwareBindings();
            clearSoftwareBindings();
            clearDeferredSteps();
        }
        return error;
    }

    DWORD installNativeSeam(HMODULE module)
    {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const DWORD size = nt->OptionalHeader.SizeOfImage;
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
            directory.VirtualAddress > size || directory.Size > size - directory.VirtualAddress) return ERROR_BAD_EXE_FORMAT;
        struct Replacement { const char* name; ULONG_PTR value; bool found; };
        Replacement replacements[] = {
            {"GetThreadContext", reinterpret_cast<ULONG_PTR>(&getContext), false},
            {"SetThreadContext", reinterpret_cast<ULONG_PTR>(&setContext), false},
            {"WaitForDebugEvent", reinterpret_cast<ULONG_PTR>(&waitEvent), false},
            {"ContinueDebugEvent", reinterpret_cast<ULONG_PTR>(&continueEvent), false},
            {"ReadProcessMemory", reinterpret_cast<ULONG_PTR>(&readMemory), false},
            {"WriteProcessMemory", reinterpret_cast<ULONG_PTR>(&writeMemory), false}
        };
        struct Patch { ULONG_PTR* slot; ULONG_PTR previous; ULONG_PTR next; };
        std::vector<Patch> patches;
        const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (DWORD i = 0; (i + 1) * sizeof(*imports) <= directory.Size && imports[i].Name != 0; ++i)
        {
            if (imports[i].OriginalFirstThunk == 0 || imports[i].FirstThunk == 0) continue;
            for (DWORD index = 0; index < size / sizeof(IMAGE_THUNK_DATA64); ++index)
            {
                const SIZE_T lookupRva = imports[i].OriginalFirstThunk + static_cast<SIZE_T>(index) * sizeof(IMAGE_THUNK_DATA64);
                const SIZE_T slotRva = imports[i].FirstThunk + static_cast<SIZE_T>(index) * sizeof(IMAGE_THUNK_DATA64);
                if (lookupRva > size - sizeof(IMAGE_THUNK_DATA64) || slotRva > size - sizeof(IMAGE_THUNK_DATA64)) return ERROR_BAD_EXE_FORMAT;
                const auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + lookupRva);
                if (lookup->u1.AddressOfData == 0) break;
                if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) continue;
                if (lookup->u1.AddressOfData > size - sizeof(IMAGE_IMPORT_BY_NAME)) return ERROR_BAD_EXE_FORMAT;
                const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + lookup->u1.AddressOfData);
                const auto remaining = size - static_cast<SIZE_T>(lookup->u1.AddressOfData) - offsetof(IMAGE_IMPORT_BY_NAME, Name);
                if (std::memchr(name->Name, 0, remaining) == nullptr) return ERROR_BAD_EXE_FORMAT;
                for (auto& replacement : replacements)
                    if (std::strcmp(name->Name, replacement.name) == 0)
                    {
                        auto* slot = reinterpret_cast<ULONG_PTR*>(const_cast<BYTE*>(base) + slotRva);
                        patches.push_back({slot, *slot, replacement.value}); replacement.found = true;
                    }
            }
        }
        for (const auto& replacement : replacements) if (!replacement.found)
        { log(std::string("Required native import missing: ") + replacement.name); return ERROR_PROC_NOT_FOUND; }
        std::size_t applied = 0;
        DWORD patchError = ERROR_SUCCESS;
        for (; applied < patches.size(); ++applied)
        {
            auto& patch = patches[applied]; DWORD protection = 0;
            if (!VirtualProtect(patch.slot, sizeof(*patch.slot), PAGE_READWRITE, &protection)) { patchError = GetLastError(); break; }
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(patch.slot), reinterpret_cast<PVOID>(patch.next));
            DWORD ignored = 0;
            if (!VirtualProtect(patch.slot, sizeof(*patch.slot), protection, &ignored)) { patchError = GetLastError(); ++applied; break; }
        }
        if (patchError != ERROR_SUCCESS || applied != patches.size())
        {
            const DWORD error = patchError == ERROR_SUCCESS ? ERROR_DLL_INIT_FAILED : patchError;
            while (applied > 0)
            {
                auto& patch = patches[--applied]; DWORD protection = 0;
                if (VirtualProtect(patch.slot, sizeof(*patch.slot), PAGE_READWRITE, &protection))
                {
                    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(patch.slot), reinterpret_cast<PVOID>(patch.previous));
                    DWORD ignored = 0; (void)VirtualProtect(patch.slot, sizeof(*patch.slot), protection, &ignored);
                }
            }
            return error;
        }
        return ERROR_SUCCESS;
    }
}
