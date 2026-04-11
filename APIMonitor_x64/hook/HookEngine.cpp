#include "pch.h"
#include "HookEngine.h"

#include <TlHelp32.h>

namespace apimon
{
    namespace
    {
        constexpr std::size_t kAbsoluteJumpSize = 12;

        class ScopedOtherThreadsSuspender
        {
        public:
            ScopedOtherThreadsSuspender()
            {
                const DWORD currentProcessId = ::GetCurrentProcessId();
                const DWORD currentThreadId = ::GetCurrentThreadId();

                HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snapshotHandle == INVALID_HANDLE_VALUE)
                {
                    return;
                }

                THREADENTRY32 threadEntry{};
                threadEntry.dwSize = sizeof(threadEntry);
                if (::Thread32First(snapshotHandle, &threadEntry) == FALSE)
                {
                    ::CloseHandle(snapshotHandle);
                    return;
                }

                do
                {
                    if (threadEntry.th32OwnerProcessID != currentProcessId
                        || threadEntry.th32ThreadID == currentThreadId)
                    {
                        continue;
                    }

                    HANDLE threadHandle = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.th32ThreadID);
                    if (threadHandle == nullptr)
                    {
                        continue;
                    }

                    if (::SuspendThread(threadHandle) != static_cast<DWORD>(-1))
                    {
                        m_suspendedThreadHandles.push_back(threadHandle);
                    }
                    else
                    {
                        ::CloseHandle(threadHandle);
                    }
                } while (::Thread32Next(snapshotHandle, &threadEntry) != FALSE);

                ::CloseHandle(snapshotHandle);
            }

            ~ScopedOtherThreadsSuspender()
            {
                for (auto it = m_suspendedThreadHandles.rbegin(); it != m_suspendedThreadHandles.rend(); ++it)
                {
                    ::ResumeThread(*it);
                    ::CloseHandle(*it);
                }
            }

        private:
            std::vector<HANDLE> m_suspendedThreadHandles;
        };

        std::size_t ModRmLength(
            const unsigned char* codePointer,
            const std::size_t maxLength,
            bool* usesRipRelativeOut)
        {
            if (codePointer == nullptr || maxLength < 1)
            {
                return 0;
            }
            if (usesRipRelativeOut != nullptr)
            {
                *usesRipRelativeOut = false;
            }

            std::size_t totalLength = 1;
            const unsigned char modrmValue = codePointer[0];
            const unsigned char modValue = static_cast<unsigned char>((modrmValue >> 6) & 0x3);
            const unsigned char rmValue = static_cast<unsigned char>(modrmValue & 0x7);

            if (modValue != 3 && rmValue == 4)
            {
                if (maxLength < totalLength + 1)
                {
                    return 0;
                }
                const unsigned char sibValue = codePointer[totalLength++];
                const unsigned char baseValue = static_cast<unsigned char>(sibValue & 0x7);
                if (modValue == 0 && baseValue == 5)
                {
                    totalLength += 4;
                }
            }

            if (modValue == 0 && rmValue == 5)
            {
                if (usesRipRelativeOut != nullptr)
                {
                    *usesRipRelativeOut = true;
                }
                totalLength += 4;
            }
            else if (modValue == 1)
            {
                totalLength += 1;
            }
            else if (modValue == 2)
            {
                totalLength += 4;
            }

            return totalLength <= maxLength ? totalLength : 0;
        }

        std::size_t DecodeInstructionLength(const unsigned char* codePointer, const std::size_t maxLength)
        {
            if (codePointer == nullptr || maxLength == 0)
            {
                return 0;
            }

            std::size_t offsetValue = 0;
            bool operandOverride = false;
            bool rexW = false;

            while (offsetValue < maxLength)
            {
                const unsigned char prefixValue = codePointer[offsetValue];
                if (prefixValue == 0x66)
                {
                    operandOverride = true;
                    ++offsetValue;
                    continue;
                }
                if ((prefixValue >= 0x40 && prefixValue <= 0x4F))
                {
                    rexW = (prefixValue & 0x08) != 0;
                    ++offsetValue;
                    continue;
                }
                if (prefixValue == 0xF0 || prefixValue == 0xF2 || prefixValue == 0xF3
                    || prefixValue == 0x2E || prefixValue == 0x36 || prefixValue == 0x3E
                    || prefixValue == 0x26 || prefixValue == 0x64 || prefixValue == 0x65
                    || prefixValue == 0x67)
                {
                    ++offsetValue;
                    continue;
                }
                break;
            }

            if (offsetValue >= maxLength)
            {
                return 0;
            }

            const unsigned char opcodeValue = codePointer[offsetValue++];
            if ((opcodeValue >= 0x50 && opcodeValue <= 0x5F)
                || opcodeValue == 0x90
                || opcodeValue == 0xC3
                || opcodeValue == 0xCC)
            {
                return offsetValue;
            }
            if (opcodeValue == 0x6A)
            {
                return offsetValue + 1 <= maxLength ? offsetValue + 1 : 0;
            }
            if (opcodeValue == 0x68)
            {
                return offsetValue + 4 <= maxLength ? offsetValue + 4 : 0;
            }
            if (opcodeValue == 0xE8 || opcodeValue == 0xE9 || opcodeValue == 0xEB)
            {
                // 相对控制流在复制到 trampoline 后会改写语义，这里直接视为不可安全 Hook。
                return 0;
            }
            if (opcodeValue >= 0xB8 && opcodeValue <= 0xBF)
            {
                const std::size_t immLength = rexW ? 8 : (operandOverride ? 2 : 4);
                return offsetValue + immLength <= maxLength ? offsetValue + immLength : 0;
            }
            if (opcodeValue == 0x0F)
            {
                if (offsetValue >= maxLength)
                {
                    return 0;
                }

                const unsigned char secondOpcode = codePointer[offsetValue++];
                if (secondOpcode >= 0x80 && secondOpcode <= 0x8F)
                {
                    return 0;
                }
                if (secondOpcode == 0x1F)
                {
                    bool usesRipRelative = false;
                    const std::size_t modrmLength = ModRmLength(
                        codePointer + offsetValue,
                        maxLength - offsetValue,
                        &usesRipRelative);
                    if (usesRipRelative)
                    {
                        return 0;
                    }
                    return modrmLength == 0 ? 0 : offsetValue + modrmLength;
                }
                return 0;
            }

            const auto appendModRmInstruction = [&](const std::size_t immediateLength) -> std::size_t {
                bool usesRipRelative = false;
                const std::size_t modrmLength = ModRmLength(
                    codePointer + offsetValue,
                    maxLength - offsetValue,
                    &usesRipRelative);
                if (modrmLength == 0)
                {
                    return 0;
                }
                if (usesRipRelative)
                {
                    return 0;
                }
                const std::size_t totalLength = offsetValue + modrmLength + immediateLength;
                return totalLength <= maxLength ? totalLength : 0;
            };

            switch (opcodeValue)
            {
            case 0x01:
            case 0x03:
            case 0x09:
            case 0x0B:
            case 0x21:
            case 0x23:
            case 0x29:
            case 0x2B:
            case 0x31:
            case 0x33:
            case 0x39:
            case 0x3B:
            case 0x63:
            case 0x80:
            case 0x84:
            case 0x85:
            case 0x88:
            case 0x89:
            case 0x8A:
            case 0x8B:
            case 0x8D:
                return appendModRmInstruction(0);
            case 0x81:
            case 0xC7:
                return appendModRmInstruction(4);
            case 0x83:
            case 0xC6:
                return appendModRmInstruction(1);
            case 0xFF:
                // 0xFF 同时覆盖 call/jmp/push 等多种语义，这里统一保守拒绝。
                return 0;
            default:
                break;
            }

            return 0;
        }

        void BuildAbsoluteJump(unsigned char* targetBuffer, const void* destinationAddress)
        {
            targetBuffer[0] = 0x48;
            targetBuffer[1] = 0xB8;
            std::memcpy(targetBuffer + 2, &destinationAddress, sizeof(destinationAddress));
            targetBuffer[10] = 0xFF;
            targetBuffer[11] = 0xE0;
        }

        void* ResolveJumpStub(void* addressValue)
        {
            unsigned char* currentPointer = static_cast<unsigned char*>(addressValue);
            for (int depth = 0; depth < 8 && currentPointer != nullptr; ++depth)
            {
                if (currentPointer[0] == 0xE9)
                {
                    const std::int32_t relativeOffset = *reinterpret_cast<std::int32_t*>(currentPointer + 1);
                    currentPointer = currentPointer + 5 + relativeOffset;
                    continue;
                }
                if (currentPointer[0] == 0xEB)
                {
                    const std::int8_t relativeOffset = *reinterpret_cast<std::int8_t*>(currentPointer + 1);
                    currentPointer = currentPointer + 2 + relativeOffset;
                    continue;
                }
                if (currentPointer[0] == 0xFF && currentPointer[1] == 0x25)
                {
                    const std::int32_t relativeOffset = *reinterpret_cast<std::int32_t*>(currentPointer + 2);
                    void** indirectPointer = reinterpret_cast<void**>(currentPointer + 6 + relativeOffset);
                    currentPointer = static_cast<unsigned char*>(*indirectPointer);
                    continue;
                }
                if (currentPointer[0] == 0x48 && currentPointer[1] == 0xFF && currentPointer[2] == 0x25)
                {
                    const std::int32_t relativeOffset = *reinterpret_cast<std::int32_t*>(currentPointer + 3);
                    void** indirectPointer = reinterpret_cast<void**>(currentPointer + 7 + relativeOffset);
                    currentPointer = static_cast<unsigned char*>(*indirectPointer);
                    continue;
                }
                break;
            }
            return currentPointer;
        }

        std::size_t CalculatePatchSize(const unsigned char* codePointer)
        {
            std::size_t patchSize = 0;
            while (patchSize < kAbsoluteJumpSize && patchSize < 24)
            {
                const std::size_t instructionLength = DecodeInstructionLength(codePointer + patchSize, 24 - patchSize);
                if (instructionLength == 0)
                {
                    return 0;
                }
                patchSize += instructionLength;
            }
            return patchSize >= kAbsoluteJumpSize ? patchSize : 0;
        }
    }

    InlineHookInstallResult InstallInlineHook(
        const wchar_t* moduleName,
        const char* procName,
        void* detourAddress,
        InlineHookRecord* hookOut,
        void** originalOut,
        std::wstring* errorTextOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }
        if (moduleName == nullptr || procName == nullptr || detourAddress == nullptr || hookOut == nullptr || originalOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"InstallInlineHook received invalid argument.";
            }
            return InlineHookInstallResult::PermanentFailure;
        }
        if (hookOut->permanentlyDisabled)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"Hook has been permanently disabled after a previous unsafe install attempt.";
            }
            return InlineHookInstallResult::PermanentFailure;
        }

        HMODULE moduleHandle = ::GetModuleHandleW(moduleName);
        if (moduleHandle == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = std::wstring(L"Module not loaded: ") + moduleName;
            }
            return InlineHookInstallResult::RetryableFailure;
        }

        void* exportAddress = reinterpret_cast<void*>(::GetProcAddress(moduleHandle, procName));
        if (exportAddress == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"GetProcAddress failed.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        unsigned char* targetPointer = static_cast<unsigned char*>(ResolveJumpStub(exportAddress));
        const std::size_t patchSize = CalculatePatchSize(targetPointer);
        if (patchSize == 0 || patchSize > hookOut->originalBytes.size())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"Unsupported or relocation-unsafe prologue for inline hook.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        unsigned char* trampolinePointer = static_cast<unsigned char*>(::VirtualAlloc(
            nullptr,
            patchSize + kAbsoluteJumpSize,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (trampolinePointer == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"VirtualAlloc for trampoline failed.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        std::memcpy(trampolinePointer, targetPointer, patchSize);
        BuildAbsoluteJump(trampolinePointer + patchSize, targetPointer + patchSize);

        DWORD oldProtect = 0;
        if (::VirtualProtect(targetPointer, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect) == FALSE)
        {
            ::VirtualFree(trampolinePointer, 0, MEM_RELEASE);
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"VirtualProtect for target patch failed.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        ScopedOtherThreadsSuspender suspendOtherThreadsScope;
        hookOut->targetAddress = targetPointer;
        hookOut->detourAddress = detourAddress;
        hookOut->trampolineAddress = trampolinePointer;
        hookOut->patchSize = patchSize;
        hookOut->permanentlyDisabled = false;
        std::memcpy(hookOut->originalBytes.data(), targetPointer, patchSize);

        unsigned char patchBuffer[32] = {};
        BuildAbsoluteJump(patchBuffer, detourAddress);
        std::memset(patchBuffer + kAbsoluteJumpSize, 0x90, patchSize - kAbsoluteJumpSize);
        std::memcpy(targetPointer, patchBuffer, patchSize);
        ::FlushInstructionCache(::GetCurrentProcess(), targetPointer, patchSize);

        DWORD unusedProtect = 0;
        ::VirtualProtect(targetPointer, patchSize, oldProtect, &unusedProtect);
        hookOut->installed = true;
        *originalOut = trampolinePointer;
        return InlineHookInstallResult::Installed;
    }

    void UninstallInlineHook(InlineHookRecord* hookValue)
    {
        if (hookValue == nullptr || !hookValue->installed || hookValue->targetAddress == nullptr)
        {
            return;
        }

        DWORD oldProtect = 0;
        if (::VirtualProtect(hookValue->targetAddress, hookValue->patchSize, PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE)
        {
            ScopedOtherThreadsSuspender suspendOtherThreadsScope;
            std::memcpy(hookValue->targetAddress, hookValue->originalBytes.data(), hookValue->patchSize);
            ::FlushInstructionCache(::GetCurrentProcess(), hookValue->targetAddress, hookValue->patchSize);
            DWORD unusedProtect = 0;
            ::VirtualProtect(hookValue->targetAddress, hookValue->patchSize, oldProtect, &unusedProtect);
        }

        if (hookValue->trampolineAddress != nullptr)
        {
            ::VirtualFree(hookValue->trampolineAddress, 0, MEM_RELEASE);
        }

        hookValue->installed = false;
        hookValue->targetAddress = nullptr;
        hookValue->detourAddress = nullptr;
        hookValue->trampolineAddress = nullptr;
        hookValue->patchSize = 0;
        hookValue->originalBytes.fill(0);
    }
}
