#pragma once
#include "ApiMonitorPlatform.h"
#include <TlHelp32.h>
#include "ApiMonitorRemoteExports.h"

namespace ks::winapi_monitor
{
    inline void appendInjectionTargetPolicy(HANDLE process, std::wstring* detail)
    {
        if (process == nullptr || detail == nullptr)
        {
            return;
        }

        PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
        if (::GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature, sizeof(signature)))
        {
            DWORD flags = 0;
            std::memcpy(&flags, &signature, sizeof(flags));
            *detail += L"; signaturePolicy=" + std::to_wstring(flags);
        }
        else
        {
            *detail += L"; signaturePolicyError=" + std::to_wstring(::GetLastError());
        }

        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
        if (::GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dynamicCode, sizeof(dynamicCode)))
        {
            DWORD flags = 0;
            std::memcpy(&flags, &dynamicCode, sizeof(flags));
            *detail += L"; dynamicCodePolicy=" + std::to_wstring(flags);
        }
        else
        {
            *detail += L"; dynamicCodePolicyError=" + std::to_wstring(::GetLastError());
        }

        PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
        if (::GetProcessMitigationPolicy(process, ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad)))
        {
            DWORD flags = 0;
            std::memcpy(&flags, &imageLoad, sizeof(flags));
            *detail += L"; imageLoadPolicy=" + std::to_wstring(flags);
        }
        else
        {
            *detail += L"; imageLoadPolicyError=" + std::to_wstring(::GetLastError());
        }

        using NtQueryInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
        const auto queryProcess = reinterpret_cast<NtQueryInformationProcessFn>(
            ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        unsigned char protection = 0;
        const LONG protectionStatus = queryProcess == nullptr
            ? static_cast<LONG>(0xC0000002L)
            : queryProcess(process, 61, &protection, sizeof(protection), nullptr);
        if (protectionStatus >= 0)
        {
            *detail += L"; processProtection=" + std::to_wstring(static_cast<unsigned>(protection));
        }
        else
        {
            *detail += L"; processProtectionStatus=" + std::to_wstring(static_cast<unsigned long>(protectionStatus));
        }
    }

    inline std::uint64_t processCreationIdentity(HANDLE process)
    {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!::GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;
        return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    }
    inline std::wstring quoteWindowsArgument(const std::wstring& argument)
    {
        std::wstring quoted = L"\"";
        unsigned slashes = 0;
        for (wchar_t ch : argument) {
            if (ch == L'\\') { ++slashes; continue; }
            quoted.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0;
            quoted.push_back(ch);
        }
        quoted.append(slashes * 2, L'\\'); quoted.push_back(L'"'); return quoted;
    }
    inline bool remoteImageLoaded(DWORD pid, const std::wstring& path, DWORD* failureOut = nullptr)
    {
        if (failureOut != nullptr)
        {
            *failureOut = ERROR_SUCCESS;
        }

        HANDLE snapshot = INVALID_HANDLE_VALUE;
        DWORD snapshotError = ERROR_SUCCESS;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot != INVALID_HANDLE_VALUE)
            {
                break;
            }
            snapshotError = ::GetLastError();
            if (snapshotError != ERROR_BAD_LENGTH)
            {
                break;
            }
        }
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            if (failureOut != nullptr)
            {
                *failureOut = snapshotError;
            }
            return false;
        }

        MODULEENTRY32W module{}; module.dwSize = sizeof(module); bool loaded = false;
        BOOL found = ::Module32FirstW(snapshot, &module);
        while (found)
        {
            if (_wcsicmp(module.szExePath, path.c_str()) == 0) { loaded = true; break; }
            found = ::Module32NextW(snapshot, &module);
        }
        DWORD enumerationError = ERROR_SUCCESS;
        if (!loaded)
        {
            const DWORD lastError = ::GetLastError();
            enumerationError = lastError == ERROR_NO_MORE_FILES || lastError == ERROR_SUCCESS
                ? ERROR_MOD_NOT_FOUND
                : lastError;
        }
        ::CloseHandle(snapshot);
        if (failureOut != nullptr)
        {
            *failureOut = enumerationError;
        }
        return loaded;
    }
    inline std::uintptr_t remoteSystemExportAddress(DWORD pid, const char* procName, std::wstring* error)
    {
        auto loader = ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), procName);
        HMODULE owner = nullptr;
        if (!loader || !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(loader), &owner)) {
            platformFailure(error, L"resolve remote system export owner", ::GetLastError()); return 0;
        }
        wchar_t ownerPath[32768]{};
        if (!::GetModuleFileNameW(owner, ownerPath, static_cast<DWORD>(std::size(ownerPath)))) {
            platformFailure(error, L"resolve loader image path", ::GetLastError()); return 0;
        }
        const auto rva = reinterpret_cast<std::uintptr_t>(loader) - reinterpret_cast<std::uintptr_t>(owner);
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_BAD_LENGTH) break;
        }
        if (snapshot == INVALID_HANDLE_VALUE) { platformFailure(error, L"enumerate target loader modules", ::GetLastError()); return 0; }
        MODULEENTRY32W module{}; module.dwSize = sizeof(module); std::uintptr_t result = 0;
        for (BOOL found = ::Module32FirstW(snapshot, &module); found; found = ::Module32NextW(snapshot, &module)) {
            if (_wcsicmp(module.szExePath, ownerPath) == 0 && rva < module.modBaseSize) {
                result = reinterpret_cast<std::uintptr_t>(module.modBaseAddr) + rva; break;
            }
        }
        ::CloseHandle(snapshot);
        if (!result) platformFailure(error, L"target system export image unavailable", ERROR_MOD_NOT_FOUND);
        return result;
    }

    inline std::uintptr_t remoteLoaderAddress(DWORD pid, std::wstring* error)
    {
        return remoteSystemExportAddress(pid, "LoadLibraryW", error);
    }

    struct RemoteLoadPacket32
    {
        std::uint32_t loadLibraryEx = 0;
        std::uint32_t getLastError = 0;
        std::uint32_t path = 0;
        std::uint32_t module = 0;
        std::uint32_t error = 0;
    };

    struct RemoteLoadPacket64
    {
        std::uint64_t loadLibraryEx = 0;
        std::uint64_t getLastError = 0;
        std::uint64_t path = 0;
        std::uint64_t module = 0;
        std::uint32_t error = 0;
        std::uint32_t reserved = 0;
    };

    inline const unsigned char* remoteLoadStub32(std::size_t* sizeOut)
    {
        static constexpr unsigned char code[] = {
            0x56,                         // push esi
            0x8B, 0x74, 0x24, 0x08,     // mov esi,[esp+8]
            0x8B, 0x06,                 // mov eax,[esi]
            0x68, 0x00, 0x09, 0x00, 0x00, // push 0x900
            0x6A, 0x00,                 // push NULL hFile
            0xFF, 0x76, 0x08,           // push [esi+path]
            0xFF, 0xD0,                 // call eax
            0x89, 0x46, 0x0C,           // mov [esi+module],eax
            0x85, 0xC0,                 // test eax,eax
            0x75, 0x0A,                 // jnz done
            0x8B, 0x46, 0x04,           // mov eax,[esi+getLastError]
            0xFF, 0xD0,                 // call eax
            0x89, 0x46, 0x10,           // mov [esi+error],eax
            0x31, 0xC0,                 // xor eax,eax
            0x5E,                       // pop esi
            0xC2, 0x04, 0x00           // ret 4
        };
        if (sizeOut != nullptr)
        {
            *sizeOut = sizeof(code);
        }
        return code;
    }

    inline const unsigned char* remoteLoadStub64(std::size_t* sizeOut)
    {
        static constexpr unsigned char code[] = {
            0x53,                       // push rbx
            0x48, 0x83, 0xEC, 0x20,    // shadow space and alignment
            0x48, 0x89, 0xCB,           // mov rbx,rcx
            0x48, 0x8B, 0x43, 0x00,    // mov rax,[rbx+loadLibraryEx]
            0x48, 0x8B, 0x4B, 0x10,    // mov rcx,[rbx+path]
            0x31, 0xD2,                 // xor edx,edx
            0x41, 0xB8, 0x00, 0x09, 0x00, 0x00, // mov r8d,0x900
            0xFF, 0xD0,                 // call rax
            0x48, 0x89, 0x43, 0x18,    // mov [rbx+module],rax
            0x48, 0x85, 0xC0,           // test rax,rax
            0x75, 0x06,                 // jnz done
            0xFF, 0x53, 0x08,           // call [rbx+getLastError]
            0x89, 0x43, 0x20,           // mov [rbx+error],eax
            0x48, 0x83, 0xC4, 0x20,    // release shadow space
            0x31, 0xC0,                 // xor eax,eax
            0x5B,                       // pop rbx
            0xC3                        // ret
        };
        if (sizeOut != nullptr)
        {
            *sizeOut = sizeof(code);
        }
        return code;
    }

    inline bool injectAgentNative(DWORD pid, const std::wstring& path, std::wstring* error,
        std::uint64_t expectedCreation = 0, HANDLE cancelEvent = nullptr)
    {
        if (error) error->clear();
        HANDLE process = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
        if (!process) return platformFailure(error, L"OpenProcess Agent injection", ::GetLastError());
        USHORT machine = 0, image = 0;
        bool valid = queryProcessMachine(process, &machine, error) && queryImageMachine(path, &image, error);
        if (valid && (machine != image
#ifndef _WIN64
            || machine != currentMachine()
#endif
            ))
            valid = platformFailure(error, L"native injector architecture mismatch", ERROR_BAD_EXE_FORMAT);
        if (valid && expectedCreation && processCreationIdentity(process) != expectedCreation)
            valid = platformFailure(error, L"target process identity changed", ERROR_INVALID_PARAMETER);
        std::uintptr_t loadLibraryEx = 0;
        std::uintptr_t getLastError = 0;
        if (valid && machine == currentMachine())
        {
            loadLibraryEx = remoteSystemExportAddress(pid, "LoadLibraryExW", error);
            if (loadLibraryEx)
            {
                getLastError = remoteSystemExportAddress(pid, "GetLastError", error);
            }
        }
#ifdef _WIN64
        else if (valid) {
            loadLibraryEx = findRemoteExport(process, pid, machine, L"KernelBase.dll", "LoadLibraryExW");
            if (!loadLibraryEx) loadLibraryEx = findRemoteExport(process, pid, machine, L"kernel32.dll", "LoadLibraryExW");
            getLastError = findRemoteExport(process, pid, machine, L"KernelBase.dll", "GetLastError");
            if (!getLastError) getLastError = findRemoteExport(process, pid, machine, L"kernel32.dll", "GetLastError");
        }
#endif
        if (!loadLibraryEx || !getLastError)
        {
            const auto code = ::GetLastError();
            ::CloseHandle(process);
            ::SetLastError(code);
            return false;
        }

        if (path.empty() || path.size() > 32767)
        {
            ::CloseHandle(process);
            return platformFailure(error, L"invalid remote Agent path", ERROR_BAD_PATHNAME);
        }

        std::size_t stubSize = 0;
        const unsigned char* stub = nullptr;
        const bool target32 = machine == IMAGE_FILE_MACHINE_I386;
        if (target32)
        {
            stub = remoteLoadStub32(&stubSize);
        }
        else
        {
            stub = remoteLoadStub64(&stubSize);
        }
        const SIZE_T pathBytes = (path.size() + 1) * sizeof(wchar_t);
        const SIZE_T packetBytes = target32 ? sizeof(RemoteLoadPacket32) : sizeof(RemoteLoadPacket64);
        void* remotePacket = ::VirtualAllocEx(process, nullptr, packetBytes + pathBytes,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        void* remoteCode = ::VirtualAllocEx(process, nullptr, stubSize,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!remotePacket || !remoteCode)
        {
            const auto code = ::GetLastError();
            if (remoteCode) ::VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
            if (remotePacket) ::VirtualFreeEx(process, remotePacket, 0, MEM_RELEASE);
            ::CloseHandle(process);
            return platformFailure(error, L"allocate remote Agent loader", code);
        }

        const auto remotePacketAddress = reinterpret_cast<std::uintptr_t>(remotePacket);
        const auto remoteCodeAddress = reinterpret_cast<std::uintptr_t>(remoteCode);
        const auto remotePathAddress = remotePacketAddress + packetBytes;
        if (target32 && (remotePacketAddress > UINT32_MAX || remoteCodeAddress > UINT32_MAX || remotePathAddress > UINT32_MAX))
        {
            ::VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
            ::VirtualFreeEx(process, remotePacket, 0, MEM_RELEASE);
            ::CloseHandle(process);
            return platformFailure(error, L"remote 32-bit loader address exceeds range", ERROR_NOT_ENOUGH_MEMORY);
        }

        std::vector<unsigned char> packet(packetBytes, 0);
        if (target32)
        {
            RemoteLoadPacket32 value{};
            value.loadLibraryEx = static_cast<std::uint32_t>(loadLibraryEx);
            value.getLastError = static_cast<std::uint32_t>(getLastError);
            value.path = static_cast<std::uint32_t>(remotePathAddress);
            std::memcpy(packet.data(), &value, sizeof(value));
        }
        else
        {
            RemoteLoadPacket64 value{};
            value.loadLibraryEx = static_cast<std::uint64_t>(loadLibraryEx);
            value.getLastError = static_cast<std::uint64_t>(getLastError);
            value.path = static_cast<std::uint64_t>(remotePathAddress);
            std::memcpy(packet.data(), &value, sizeof(value));
        }

        SIZE_T written = 0;
        if (!::WriteProcessMemory(process, remotePacket, packet.data(), packet.size(), &written) || written != packet.size())
        {
            const auto code = ::GetLastError();
            ::VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
            ::VirtualFreeEx(process, remotePacket, 0, MEM_RELEASE);
            ::CloseHandle(process);
            return platformFailure(error, L"write remote Agent loader packet", code);
        }
        if (!::WriteProcessMemory(process, reinterpret_cast<void*>(remotePathAddress), path.c_str(), pathBytes, &written) || written != pathBytes
            || !::WriteProcessMemory(process, remoteCode, stub, stubSize, &written) || written != stubSize)
        {
            const auto code = ::GetLastError();
            ::VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
            ::VirtualFreeEx(process, remotePacket, 0, MEM_RELEASE);
            ::CloseHandle(process);
            return platformFailure(error, L"write remote Agent loader code", code);
        }
        DWORD oldProtect = 0;
        if (!::VirtualProtectEx(process, remoteCode, stubSize, PAGE_EXECUTE_READ, &oldProtect))
        {
            const auto code = ::GetLastError();
            ::VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
            ::VirtualFreeEx(process, remotePacket, 0, MEM_RELEASE);
            ::CloseHandle(process);
            return platformFailure(error, L"protect remote Agent loader code", code);
        }

        HANDLE thread = ::CreateRemoteThread(process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteCode), remotePacket, 0, nullptr);
        if (!thread) {
            const auto code = ::GetLastError();
            ::VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
            ::VirtualFreeEx(process, remotePacket, 0, MEM_RELEASE);
            ::CloseHandle(process);
            return platformFailure(error, L"CreateRemoteThread Agent loader", code);
        }
        HANDLE waits[] = {thread, cancelEvent};
        const DWORD wait = cancelEvent ? ::WaitForMultipleObjects(2, waits, FALSE, 10000) : ::WaitForSingleObject(thread, 10000);
        DWORD loaderExitCode = 0;
        const bool loaderExitRead = wait == WAIT_OBJECT_0 && ::GetExitCodeThread(thread, &loaderExitCode) != FALSE;
        bool packetRead = false;
        std::uint64_t remoteModule = 0;
        DWORD remoteLoaderError = ERROR_SUCCESS;
        if (wait == WAIT_OBJECT_0)
        {
            std::vector<unsigned char> received(packetBytes, 0);
            SIZE_T read = 0;
            packetRead = ::ReadProcessMemory(process, remotePacket, received.data(), received.size(), &read)
                && read == received.size();
            if (packetRead)
            {
                if (target32)
                {
                    RemoteLoadPacket32 value{};
                    std::memcpy(&value, received.data(), sizeof(value));
                    remoteModule = value.module;
                    remoteLoaderError = value.error;
                }
                else
                {
                    RemoteLoadPacket64 value{};
                    std::memcpy(&value, received.data(), sizeof(value));
                    remoteModule = value.module;
                    remoteLoaderError = value.error;
                }
            }
        }
        DWORD moduleCheckError = ERROR_SUCCESS;
        const bool moduleFound = wait == WAIT_OBJECT_0 && remoteImageLoaded(pid, path, &moduleCheckError);
        const bool loaded = wait == WAIT_OBJECT_0 && (moduleFound || remoteModule != 0);
        DWORD failure = wait == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED
            : wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : wait == WAIT_FAILED ? ::GetLastError() : ERROR_DLL_INIT_FAILED;
        if (wait == WAIT_OBJECT_0 && packetRead && remoteModule == 0 && remoteLoaderError != ERROR_SUCCESS)
        {
            failure = remoteLoaderError;
        }
        // Timed-out remote loader may still use both allocations. Keep them until target exit.
        if (wait == WAIT_OBJECT_0)
        {
            ::VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
            ::VirtualFreeEx(process, remotePacket, 0, MEM_RELEASE);
        }
        ::CloseHandle(thread);
        if (!loaded)
        {
            if (error != nullptr)
            {
                *error = L"remote Agent load failed or timed out; error=" + std::to_wstring(failure);
                if (wait == WAIT_OBJECT_0)
                {
                    *error += L"; loaderThreadExit=";
                    *error += loaderExitRead ? std::to_wstring(loaderExitCode) : L"unavailable";
                    *error += L"; loaderError=";
                    *error += packetRead ? std::to_wstring(remoteLoaderError) : L"unavailable";
                    *error += L"; moduleCheck=";
                    *error += std::to_wstring(moduleCheckError);
                }
                appendInjectionTargetPolicy(process, error);
            }
            ::CloseHandle(process);
            ::SetLastError(failure);
            return false;
        }
        ::CloseHandle(process);
        return true;
    }
    inline bool injectAgent(DWORD pid, const std::wstring& path, std::wstring* error,
        HANDLE cancelEvent = nullptr, std::uint64_t expectedCreation = 0)
    {
        if (error) error->clear();
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return platformFailure(error, L"OpenProcess Agent target", ::GetLastError());
        USHORT machine = 0, image = 0;
        const auto creation = processCreationIdentity(process);
        if (expectedCreation && creation != expectedCreation)
        {
            ::CloseHandle(process);
            return platformFailure(error, L"target process identity changed", ERROR_INVALID_HANDLE);
        }
        const bool valid = creation && queryProcessMachine(process, &machine, error) && queryImageMachine(path, &image, error);
        const DWORD saved = ::GetLastError(); ::CloseHandle(process);
        if (!valid) { ::SetLastError(saved); return false; }
        if (image != machine) return platformFailure(error, L"Agent DLL does not match target architecture", ERROR_BAD_EXE_FORMAT);
        return injectAgentNative(pid, path, error, creation, cancelEvent);
    }
}
