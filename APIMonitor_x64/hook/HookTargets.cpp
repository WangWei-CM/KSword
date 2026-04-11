#include "pch.h"
#include "HookTargets.h"
#include "HookEngine.h"
#include "../MonitorAgent.h"
#include "../core/MonitorPipe.h"

#include <WinReg.h>

namespace apimon
{
    namespace
    {
        using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
        using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
        using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
        using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
        using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
        using RegCreateKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
        using RegSetValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
        using ConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
        using SendFn = int (WSAAPI*)(SOCKET, const char*, int, int);
        using RecvFn = int (WSAAPI*)(SOCKET, char*, int, int);

        InlineHookRecord g_createFileWHook{};
        InlineHookRecord g_readFileHook{};
        InlineHookRecord g_writeFileHook{};
        InlineHookRecord g_createProcessWHook{};
        InlineHookRecord g_loadLibraryWHook{};
        InlineHookRecord g_loadLibraryExWHook{};
        InlineHookRecord g_regOpenKeyExWHook{};
        InlineHookRecord g_regCreateKeyExWHook{};
        InlineHookRecord g_regSetValueExWHook{};
        InlineHookRecord g_connectHook{};
        InlineHookRecord g_sendHook{};
        InlineHookRecord g_recvHook{};
        std::mutex g_hookOperationMutex;

        CreateFileWFn g_createFileWOriginal = nullptr;
        ReadFileFn g_readFileOriginal = nullptr;
        WriteFileFn g_writeFileOriginal = nullptr;
        CreateProcessWFn g_createProcessWOriginal = nullptr;
        LoadLibraryWFn g_loadLibraryWOriginal = nullptr;
        LoadLibraryExWFn g_loadLibraryExWOriginal = nullptr;
        RegOpenKeyExWFn g_regOpenKeyExWOriginal = nullptr;
        RegCreateKeyExWFn g_regCreateKeyExWOriginal = nullptr;
        RegSetValueExWFn g_regSetValueExWOriginal = nullptr;
        ConnectFn g_connectOriginal = nullptr;
        SendFn g_sendOriginal = nullptr;
        RecvFn g_recvOriginal = nullptr;

        thread_local bool g_hookReentryGuard = false;

        class ScopedHookGuard
        {
        public:
            ScopedHookGuard()
            {
                m_bypass = g_hookReentryGuard;
                if (!m_bypass)
                {
                    g_hookReentryGuard = true;
                }
            }

            ~ScopedHookGuard()
            {
                if (!m_bypass)
                {
                    g_hookReentryGuard = false;
                }
            }

            bool bypass() const
            {
                return m_bypass;
            }

        private:
            bool m_bypass = false;
        };

        struct HookBinding
        {
            const wchar_t* moduleName;                              // moduleName：导出所在模块名。
            const char* procName;                                   // procName：导出函数名。
            ks::winapi_monitor::EventCategory categoryValue;        // categoryValue：对应监控分类。
            InlineHookRecord* hookRecord;                           // hookRecord：该 API 对应的 Hook 状态记录。
            void* detourAddress;                                    // detourAddress：Detour 函数地址。
            void** originalOut;                                     // originalOut：Trampoline 返回地址。
        };

        std::wstring ProcNameToWide(const char* procNamePointer)
        {
            if (procNamePointer == nullptr)
            {
                return std::wstring();
            }

            std::wstring wideText;
            while (*procNamePointer != '\0')
            {
                wideText.push_back(static_cast<wchar_t>(*procNamePointer));
                ++procNamePointer;
            }
            return wideText;
        }

        void AppendHookFailureText(
            std::wstring* detailTextOut,
            const HookBinding& bindingValue,
            const InlineHookInstallResult installResult,
            const std::wstring& errorText)
        {
            if (detailTextOut == nullptr)
            {
                return;
            }

            if (!detailTextOut->empty())
            {
                detailTextOut->append(L" | ");
            }

            detailTextOut->append(bindingValue.moduleName != nullptr ? bindingValue.moduleName : L"<module>");
            detailTextOut->append(L"!");
            detailTextOut->append(ProcNameToWide(bindingValue.procName));
            detailTextOut->append(
                installResult == InlineHookInstallResult::RetryableFailure
                ? L": retryable failure - "
                : L": disabled - ");
            detailTextOut->append(errorText.empty() ? L"unknown reason" : errorText);
        }

        std::wstring SafeWideText(const wchar_t* textPointer)
        {
            return textPointer != nullptr ? std::wstring(textPointer) : std::wstring();
        }

        std::wstring HexValue(const std::uint64_t value)
        {
            wchar_t textBuffer[32] = {};
            ::swprintf_s(textBuffer, L"0x%llX", static_cast<unsigned long long>(value));
            return std::wstring(textBuffer);
        }

        std::wstring HandleText(const HANDLE handleValue)
        {
            return HexValue(reinterpret_cast<std::uint64_t>(handleValue));
        }

        bool CategoryEnabled(const ks::winapi_monitor::EventCategory categoryValue)
        {
            const MonitorConfig& configValue = ActiveConfig();
            switch (categoryValue)
            {
            case ks::winapi_monitor::EventCategory::File:
                return configValue.enableFile;
            case ks::winapi_monitor::EventCategory::Registry:
                return configValue.enableRegistry;
            case ks::winapi_monitor::EventCategory::Network:
                return configValue.enableNetwork;
            case ks::winapi_monitor::EventCategory::Process:
                return configValue.enableProcess;
            case ks::winapi_monitor::EventCategory::Loader:
                return configValue.enableLoader;
            default:
                break;
            }
            return true;
        }

        std::wstring TrimDetail(const std::wstring& detailText)
        {
            const std::size_t detailLimit = std::min<std::size_t>(
                ActiveConfig().detailLimitChars,
                ks::winapi_monitor::kMaxDetailChars - 1);
            return detailText.size() > detailLimit ? detailText.substr(0, detailLimit) : detailText;
        }

        std::wstring FormatRegistryRoot(const HKEY rootKey)
        {
            if (rootKey == HKEY_CLASSES_ROOT) { return L"HKCR"; }
            if (rootKey == HKEY_CURRENT_USER) { return L"HKCU"; }
            if (rootKey == HKEY_LOCAL_MACHINE) { return L"HKLM"; }
            if (rootKey == HKEY_USERS) { return L"HKU"; }
            if (rootKey == HKEY_CURRENT_CONFIG) { return L"HKCC"; }
            return HandleText(rootKey);
        }

        std::wstring FormatSocketAddress(const sockaddr* addressPointer, const int addressLength)
        {
            if (addressPointer == nullptr || addressLength <= 0)
            {
                return L"<null>";
            }

            wchar_t hostBuffer[NI_MAXHOST] = {};
            wchar_t serviceBuffer[NI_MAXSERV] = {};
            const int resultValue = ::GetNameInfoW(
                addressPointer,
                addressLength,
                hostBuffer,
                NI_MAXHOST,
                serviceBuffer,
                NI_MAXSERV,
                NI_NUMERICHOST | NI_NUMERICSERV);
            if (resultValue != 0)
            {
                return L"<unknown>";
            }
            return std::wstring(hostBuffer) + L":" + serviceBuffer;
        }

        bool TryInstallBinding(HookBinding& bindingValue, std::wstring* detailTextOut)
        {
            if (!CategoryEnabled(bindingValue.categoryValue)
                || bindingValue.hookRecord->installed
                || bindingValue.hookRecord->permanentlyDisabled)
            {
                return bindingValue.hookRecord->installed;
            }

            std::wstring errorText;
            const InlineHookInstallResult installResult = InstallInlineHook(
                bindingValue.moduleName,
                bindingValue.procName,
                bindingValue.detourAddress,
                bindingValue.hookRecord,
                bindingValue.originalOut,
                &errorText);
            if (installResult == InlineHookInstallResult::Installed)
            {
                return true;
            }
            if (installResult == InlineHookInstallResult::PermanentFailure)
            {
                bindingValue.hookRecord->permanentlyDisabled = true;
            }
            AppendHookFailureText(detailTextOut, bindingValue, installResult, errorText);
            return false;
        }

        HMODULE WINAPI HookedLoadLibraryW(LPCWSTR fileNamePointer);
        HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR fileNamePointer, HANDLE fileHandle, DWORD flagsValue);
        HANDLE WINAPI HookedCreateFileW(LPCWSTR fileNamePointer, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile);
        BOOL WINAPI HookedReadFile(HANDLE fileHandle, LPVOID bufferPointer, DWORD bytesToRead, LPDWORD bytesReadPointer, LPOVERLAPPED overlappedPointer);
        BOOL WINAPI HookedWriteFile(HANDLE fileHandle, LPCVOID bufferPointer, DWORD bytesToWrite, LPDWORD bytesWrittenPointer, LPOVERLAPPED overlappedPointer);
        BOOL WINAPI HookedCreateProcessW(LPCWSTR applicationNamePointer, LPWSTR commandLinePointer, LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags, LPVOID environmentPointer, LPCWSTR currentDirectoryPointer, LPSTARTUPINFOW startupInfoPointer, LPPROCESS_INFORMATION processInfoPointer);
        LSTATUS WINAPI HookedRegOpenKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer);
        LSTATUS WINAPI HookedRegCreateKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD reservedValue, LPWSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer);
        LSTATUS WINAPI HookedRegSetValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, DWORD reservedValue, DWORD typeValue, const BYTE* dataPointer, DWORD dataSize);
        int WSAAPI HookedConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength);
        int WSAAPI HookedSend(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue);
        int WSAAPI HookedRecv(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue);

        HookBinding g_bindings[] = {
            { L"KernelBase.dll", "CreateFileW", ks::winapi_monitor::EventCategory::File, &g_createFileWHook, reinterpret_cast<void*>(&HookedCreateFileW), reinterpret_cast<void**>(&g_createFileWOriginal) },
            { L"KernelBase.dll", "ReadFile", ks::winapi_monitor::EventCategory::File, &g_readFileHook, reinterpret_cast<void*>(&HookedReadFile), reinterpret_cast<void**>(&g_readFileOriginal) },
            { L"KernelBase.dll", "WriteFile", ks::winapi_monitor::EventCategory::File, &g_writeFileHook, reinterpret_cast<void*>(&HookedWriteFile), reinterpret_cast<void**>(&g_writeFileOriginal) },
            { L"KernelBase.dll", "CreateProcessW", ks::winapi_monitor::EventCategory::Process, &g_createProcessWHook, reinterpret_cast<void*>(&HookedCreateProcessW), reinterpret_cast<void**>(&g_createProcessWOriginal) },
            { L"Kernel32.dll", "LoadLibraryW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryWHook, reinterpret_cast<void*>(&HookedLoadLibraryW), reinterpret_cast<void**>(&g_loadLibraryWOriginal) },
            { L"Kernel32.dll", "LoadLibraryExW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryExWHook, reinterpret_cast<void*>(&HookedLoadLibraryExW), reinterpret_cast<void**>(&g_loadLibraryExWOriginal) },
            { L"Advapi32.dll", "RegCreateKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyExWHook, reinterpret_cast<void*>(&HookedRegCreateKeyExW), reinterpret_cast<void**>(&g_regCreateKeyExWOriginal) },
            { L"Advapi32.dll", "RegSetValueExW", ks::winapi_monitor::EventCategory::Registry, &g_regSetValueExWHook, reinterpret_cast<void*>(&HookedRegSetValueExW), reinterpret_cast<void**>(&g_regSetValueExWOriginal) },
            { L"Ws2_32.dll", "connect", ks::winapi_monitor::EventCategory::Network, &g_connectHook, reinterpret_cast<void*>(&HookedConnect), reinterpret_cast<void**>(&g_connectOriginal) },
            { L"Ws2_32.dll", "send", ks::winapi_monitor::EventCategory::Network, &g_sendHook, reinterpret_cast<void*>(&HookedSend), reinterpret_cast<void**>(&g_sendOriginal) },
            { L"Ws2_32.dll", "recv", ks::winapi_monitor::EventCategory::Network, &g_recvHook, reinterpret_cast<void*>(&HookedRecv), reinterpret_cast<void**>(&g_recvOriginal) }
        };

        HMODULE WINAPI HookedLoadLibraryW(const LPCWSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryWOriginal(fileNamePointer);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryWOriginal(fileNamePointer);
            const DWORD lastError = ::GetLastError();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Loader,
                L"Kernel32",
                L"LoadLibraryW",
                moduleHandle != nullptr ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"path=" + fileNameText));
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HMODULE WINAPI HookedLoadLibraryExW(const LPCWSTR fileNamePointer, HANDLE fileHandle, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryExWOriginal(fileNamePointer, fileHandle, flagsValue);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryExWOriginal(fileNamePointer, fileHandle, flagsValue);
            const DWORD lastError = ::GetLastError();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Loader,
                L"Kernel32",
                L"LoadLibraryExW",
                moduleHandle != nullptr ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"path=" + fileNameText + L" flags=" + HexValue(flagsValue)));
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HANDLE WINAPI HookedCreateFileW(
            LPCWSTR fileNamePointer,
            DWORD desiredAccess,
            DWORD shareMode,
            LPSECURITY_ATTRIBUTES securityAttributes,
            DWORD creationDisposition,
            DWORD flagsAndAttributes,
            HANDLE templateFile)
        {
            (void)securityAttributes;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createFileWOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HANDLE resultHandle = g_createFileWOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            const DWORD lastError = ::GetLastError();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::File,
                L"KernelBase",
                L"CreateFileW",
                resultHandle != INVALID_HANDLE_VALUE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(
                    L"path=" + fileNameText
                    + L" access=" + HexValue(desiredAccess)
                    + L" share=" + HexValue(shareMode)
                    + L" disposition=" + std::to_wstring(creationDisposition)
                    + L" flags=" + HexValue(flagsAndAttributes)
                    + L" handle=" + HandleText(resultHandle)));
            ::SetLastError(lastError);
            return resultHandle;
        }

        BOOL WINAPI HookedReadFile(HANDLE fileHandle, LPVOID bufferPointer, DWORD bytesToRead, LPDWORD bytesReadPointer, LPOVERLAPPED overlappedPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            }

            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            }

            const BOOL resultValue = g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD bytesReadValue = bytesReadPointer != nullptr ? *bytesReadPointer : 0;
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::File,
                L"KernelBase",
                L"ReadFile",
                resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"handle=" + HandleText(fileHandle) + L" request=" + std::to_wstring(bytesToRead) + L" read=" + std::to_wstring(bytesReadValue)));
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedWriteFile(HANDLE fileHandle, LPCVOID bufferPointer, DWORD bytesToWrite, LPDWORD bytesWrittenPointer, LPOVERLAPPED overlappedPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            }

            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            }

            const BOOL resultValue = g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD bytesWrittenValue = bytesWrittenPointer != nullptr ? *bytesWrittenPointer : 0;
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::File,
                L"KernelBase",
                L"WriteFile",
                resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"handle=" + HandleText(fileHandle) + L" request=" + std::to_wstring(bytesToWrite) + L" written=" + std::to_wstring(bytesWrittenValue)));
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCreateProcessW(
            LPCWSTR applicationNamePointer,
            LPWSTR commandLinePointer,
            LPSECURITY_ATTRIBUTES processAttributes,
            LPSECURITY_ATTRIBUTES threadAttributes,
            BOOL inheritHandles,
            DWORD creationFlags,
            LPVOID environmentPointer,
            LPCWSTR currentDirectoryPointer,
            LPSTARTUPINFOW startupInfoPointer,
            LPPROCESS_INFORMATION processInfoPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createProcessWOriginal(applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInfoPointer);
            }

            const std::wstring appNameText = SafeWideText(applicationNamePointer);
            const std::wstring commandLineText = commandLinePointer != nullptr ? std::wstring(commandLinePointer) : std::wstring();
            const std::wstring currentDirectoryText = SafeWideText(currentDirectoryPointer);

            const BOOL resultValue = g_createProcessWOriginal(
                applicationNamePointer,
                commandLinePointer,
                processAttributes,
                threadAttributes,
                inheritHandles,
                creationFlags,
                environmentPointer,
                currentDirectoryPointer,
                startupInfoPointer,
                processInfoPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD childPid = (resultValue != FALSE && processInfoPointer != nullptr) ? processInfoPointer->dwProcessId : 0;
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Process,
                L"KernelBase",
                L"CreateProcessW",
                resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(
                    L"app=" + appNameText
                    + L" cmd=" + commandLineText
                    + L" cwd=" + currentDirectoryText
                    + L" flags=" + HexValue(creationFlags)
                    + L" inherit=" + std::to_wstring(inheritHandles != FALSE)
                    + L" childPid=" + std::to_wstring(childPid)));
            ::SetLastError(lastError);
            return resultValue;
        }

        LSTATUS WINAPI HookedRegOpenKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regOpenKeyExWOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer);
            }

            const std::wstring subKeyText = SafeWideText(subKeyPointer);
            const LSTATUS statusValue = g_regOpenKeyExWOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer);
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegOpenKeyExW",
                static_cast<std::int32_t>(statusValue),
                TrimDetail(L"key=" + FormatRegistryRoot(rootKey) + L"\\" + subKeyText + L" sam=" + HexValue(samDesired)));
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCreateKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD reservedValue, LPWSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer)
        {
            (void)reservedValue;
            (void)classPointer;
            (void)securityAttributes;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regCreateKeyExWOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer);
            }

            const std::wstring subKeyText = SafeWideText(subKeyPointer);
            const LSTATUS statusValue = g_regCreateKeyExWOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer);
            const DWORD dispositionValue = dispositionPointer != nullptr ? *dispositionPointer : 0;
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegCreateKeyExW",
                static_cast<std::int32_t>(statusValue),
                TrimDetail(L"key=" + FormatRegistryRoot(rootKey) + L"\\" + subKeyText + L" options=" + HexValue(optionsValue) + L" disposition=" + std::to_wstring(dispositionValue)));
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSetValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, DWORD reservedValue, DWORD typeValue, const BYTE* dataPointer, DWORD dataSize)
        {
            (void)reservedValue;
            (void)dataPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regSetValueExWOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize);
            }

            const std::wstring valueNameText = SafeWideText(valueNamePointer);
            const LSTATUS statusValue = g_regSetValueExWOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize);
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegSetValueExW",
                static_cast<std::int32_t>(statusValue),
                TrimDetail(L"hkey=" + HandleText(keyHandle) + L" value=" + valueNameText + L" type=" + std::to_wstring(typeValue) + L" size=" + std::to_wstring(dataSize)));
            return statusValue;
        }

        int WSAAPI HookedConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_connectOriginal(socketValue, namePointer, nameLength);
            }

            const std::wstring addressText = FormatSocketAddress(namePointer, nameLength);
            const int resultValue = g_connectOriginal(socketValue, namePointer, nameLength);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"connect",
                errorValue,
                TrimDetail(L"socket=" + HexValue(static_cast<std::uint64_t>(socketValue)) + L" remote=" + addressText));
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedSend(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_sendOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            }

            const int resultValue = g_sendOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"send",
                errorValue,
                TrimDetail(L"socket=" + HexValue(static_cast<std::uint64_t>(socketValue)) + L" len=" + std::to_wstring(bufferLength) + L" sent=" + std::to_wstring(resultValue) + L" flags=" + HexValue(flagsValue)));
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedRecv(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_recvOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            }

            const int resultValue = g_recvOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"recv",
                errorValue,
                TrimDetail(L"socket=" + HexValue(static_cast<std::uint64_t>(socketValue)) + L" len=" + std::to_wstring(bufferLength) + L" received=" + std::to_wstring(resultValue) + L" flags=" + HexValue(flagsValue)));
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }
    }

    bool InstallConfiguredHooks(std::wstring* errorTextOut)
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        bool hasEnabledCategory = false;
        bool installedAny = false;
        std::wstring failureText;
        for (HookBinding& bindingValue : g_bindings)
        {
            hasEnabledCategory = CategoryEnabled(bindingValue.categoryValue) || hasEnabledCategory;
            installedAny = TryInstallBinding(bindingValue, &failureText) || installedAny;
        }
        if (!hasEnabledCategory)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"No hook category is enabled in current config.";
            }
            return false;
        }
        if (!installedAny && errorTextOut != nullptr)
        {
            *errorTextOut = failureText.empty()
                ? std::wstring(L"No hook installed successfully.")
                : failureText;
            return false;
        }
        if (installedAny && errorTextOut != nullptr)
        {
            *errorTextOut = failureText;
        }
        return installedAny;
    }

    void UninstallConfiguredHooks()
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        for (HookBinding& bindingValue : g_bindings)
        {
            UninstallInlineHook(bindingValue.hookRecord);
            if (bindingValue.originalOut != nullptr)
            {
                *bindingValue.originalOut = nullptr;
            }
        }
    }

    void RetryPendingHooks()
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        for (HookBinding& bindingValue : g_bindings)
        {
            (void)TryInstallBinding(bindingValue, nullptr);
        }
    }
}

