#pragma once
#include "ApiMonitorPlatform.h"
#include <TlHelp32.h>
#include "ApiMonitorRemoteExports.h"

namespace ks::winapi_monitor
{
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
    inline std::uintptr_t remoteLoaderAddress(DWORD pid, std::wstring* error)
    {
        auto loader = ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HMODULE owner = nullptr;
        if (!loader || !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(loader), &owner)) {
            platformFailure(error, L"resolve LoadLibraryW owner", ::GetLastError()); return 0;
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
        if (!result) platformFailure(error, L"target loader image unavailable", ERROR_MOD_NOT_FOUND);
        return result;
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
        std::uintptr_t loader = 0;
        if (valid && machine == currentMachine()) loader = remoteLoaderAddress(pid, error);
#ifdef _WIN64
        else if (valid) {
            loader = findRemoteExport(process, pid, machine, L"KernelBase.dll", "LoadLibraryW");
            if (!loader) loader = findRemoteExport(process, pid, machine, L"kernel32.dll", "LoadLibraryW");
            if (!loader) platformFailure(error, L"resolve target LoadLibraryW export", ERROR_PROC_NOT_FOUND);
        }
#endif
        if (!loader) { const auto code = ::GetLastError(); ::CloseHandle(process); ::SetLastError(code); return false; }
        const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
        void* remote = ::VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        SIZE_T written = 0;
        if (!remote || !::WriteProcessMemory(process, remote, path.c_str(), bytes, &written) || written != bytes) {
            const auto code = ::GetLastError(); if (remote) ::VirtualFreeEx(process, remote, 0, MEM_RELEASE); ::CloseHandle(process);
            return platformFailure(error, L"write remote Agent path", code);
        }
        HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loader), remote, 0, nullptr);
        if (!thread) {
            const auto code = ::GetLastError(); ::VirtualFreeEx(process, remote, 0, MEM_RELEASE); ::CloseHandle(process);
            return platformFailure(error, L"CreateRemoteThread Agent loader", code);
        }
        HANDLE waits[] = {thread, cancelEvent};
        const DWORD wait = cancelEvent ? ::WaitForMultipleObjects(2, waits, FALSE, 10000) : ::WaitForSingleObject(thread, 10000);
        // A 64-bit HMODULE is truncated by GetExitCodeThread, but a non-zero return
        // still proves that LoadLibraryW returned a module. The module snapshot remains
        // a stronger identity check when available; it is not reliable for every
        // protected/restricted target and must not turn a successful load into 1114.
        DWORD loaderExitCode = 0;
        const bool loaderExitRead = wait == WAIT_OBJECT_0 && ::GetExitCodeThread(thread, &loaderExitCode) != FALSE;
        DWORD moduleCheckError = ERROR_SUCCESS;
        const bool moduleFound = wait == WAIT_OBJECT_0 && remoteImageLoaded(pid, path, &moduleCheckError);
        const bool loaded = wait == WAIT_OBJECT_0 && (moduleFound || (loaderExitRead && loaderExitCode != 0));
        const DWORD failure = wait == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED
            : wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : wait == WAIT_FAILED ? ::GetLastError() : ERROR_DLL_INIT_FAILED;
        // Timed-out remote LoadLibraryW may still read the path. Keep it until target exit.
        if (wait == WAIT_OBJECT_0) ::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        ::CloseHandle(thread); ::CloseHandle(process);
        if (!loaded)
        {
            if (error != nullptr)
            {
                *error = L"remote Agent load failed or timed out; error=" + std::to_wstring(failure);
                if (wait == WAIT_OBJECT_0)
                {
                    *error += L"; loaderThreadExit=";
                    *error += loaderExitRead ? std::to_wstring(loaderExitCode) : L"unavailable";
                    *error += L"; moduleCheck=";
                    *error += std::to_wstring(moduleCheckError);
                }
            }
            ::SetLastError(failure);
            return false;
        }
        return true;
    }
    inline bool injectAgent(DWORD pid, const std::wstring& path, std::wstring* error)
    {
        if (error) error->clear();
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return platformFailure(error, L"OpenProcess Agent target", ::GetLastError());
        USHORT machine = 0, image = 0;
        const auto creation = processCreationIdentity(process);
        const bool valid = creation && queryProcessMachine(process, &machine, error) && queryImageMachine(path, &image, error);
        const DWORD saved = ::GetLastError(); ::CloseHandle(process);
        if (!valid) { ::SetLastError(saved); return false; }
        if (image != machine) return platformFailure(error, L"Agent DLL does not match target architecture", ERROR_BAD_EXE_FORMAT);
        return injectAgentNative(pid, path, error, creation);
    }
}
