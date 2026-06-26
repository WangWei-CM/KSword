#include "ProcessDetailCollector.h"

#include "../../Core/Common.h"

#include <algorithm>
#include <cstddef>
#include <psapi.h>
#include <sstream>
#include <tlhelp32.h>
#include <utility>
#include <winternl.h>

namespace Ksword::Features::ProcessDetail {
namespace {

constexpr DWORD kProcessBasicAccess = PROCESS_QUERY_LIMITED_INFORMATION;
constexpr DWORD kProcessReadAccess = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
constexpr DWORD kThreadQueryAccess = THREAD_QUERY_LIMITED_INFORMATION;
constexpr DWORD kMaxPathBufferChars = 32768;
constexpr ULONG kProcessBasicInformationClass = 0;
constexpr LONG kThreadQuerySetWin32StartAddressClass = 9;

using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, LONG, PVOID, ULONG, PULONG);
using EnumProcessModulesExFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
using GetModuleInformationFn = BOOL(WINAPI*)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
using GetModuleFileNameExWFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);

// NativeProcessBasicInformation is the stable native layout for class 0.
// Inputs come from NtQueryInformationProcess; processing reads only the PEB
// address and inherited PID; output avoids SDK field-name differences.
struct NativeProcessBasicInformation {
    LONG exitStatus = 0;
    PVOID pebBaseAddress = nullptr;
    ULONG_PTR affinityMask = 0;
    LONG basePriority = 0;
    ULONG_PTR uniqueProcessId = 0;
    ULONG_PTR inheritedFromUniqueProcessId = 0;
};

// RemotePeb mirrors only the early PEB fields needed to reach ProcessParameters.
// Input is target memory copied by ReadProcessMemory; processing uses the
// processParameters pointer only; output is not written back to the target.
struct RemotePeb {
    BYTE reserved1[2];
    BYTE beingDebugged = 0;
    BYTE reserved2[1];
    PVOID reserved3[2];
    PVOID ldr = nullptr;
    PVOID processParameters = nullptr;
};

// ModuleApi stores dynamically resolved PSAPI/K32 module enumeration exports.
// Inputs are loader module handles from LoadModuleApi; processing never requires
// adding a PSAPI import library to the project; callers check available() first.
struct ModuleApi {
    HMODULE library = nullptr;
    EnumProcessModulesExFn enumProcessModulesEx = nullptr;
    GetModuleInformationFn getModuleInformation = nullptr;
    GetModuleFileNameExWFn getModuleFileNameExW = nullptr;

    // available reports whether every module enumeration export was resolved.
    // There is no input; processing checks stored function pointers; output is
    // true only when CollectModules can call the API set safely.
    bool available() const {
        return enumProcessModulesEx && getModuleInformation && getModuleFileNameExW;
    }
};

// NtThreadApi stores optional ntdll thread metadata exports. Inputs are dynamic
// loader results; processing is read-only and optional; callers may continue
// with Toolhelp-only rows when the export is unavailable.
struct NtThreadApi {
    NtQueryInformationThreadFn queryInformationThread = nullptr;

    // available reports whether NtQueryInformationThread was resolved. There is
    // no input; processing checks the stored pointer; output is false when the
    // Threads page must fall back to Toolhelp-only metadata.
    bool available() const {
        return queryInformationThread != nullptr;
    }
};

// RemoteProcessParameters mirrors only the offsets needed for same-bitness
// command-line reading. The structure is deliberately partial because the page
// does not mutate remote memory and only reads ImagePathName/CommandLine.
struct RemoteProcessParameters {
    BYTE reserved1[16];
    PVOID reserved2[10];
    UNICODE_STRING imagePathName;
    UNICODE_STRING commandLine;
};

// FormatHexPointer formats an address for list-view display. Input is an
// integer pointer value; processing emits fixed-width hexadecimal text; output
// is a string such as 0x00007FF612340000.
std::wstring FormatHexPointer(std::uintptr_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase;
    if (sizeof(void*) == 8) {
        stream.width(16);
    } else {
        stream.width(8);
    }
    stream.fill(L'0');
    stream << value;
    return stream.str();
}

// Win32ErrorText returns a compact Win32 failure string. Input is an operation
// label and error code; processing appends the formatted system message; output
// is suitable for per-row or per-section status text.
std::wstring Win32ErrorText(const wchar_t* operation, DWORD errorCode) {
    return std::wstring(operation) + L" failed: " + Ksword::Core::LastErrorMessage(errorCode);
}

// ResolveProc resolves one function by exact export name. Inputs are a module
// handle and ASCII export name; processing calls GetProcAddress; output is a
// typed function pointer or nullptr.
template <typename Fn>
Fn ResolveProc(HMODULE module, const char* name) {
    return module ? reinterpret_cast<Fn>(::GetProcAddress(module, name)) : nullptr;
}

// LoadModuleApi resolves module enumeration APIs from kernel32 K32* exports or
// psapi.dll fallback exports. There is no input; processing may load psapi.dll;
// output reports function pointers and keeps the library loaded for process
// lifetime so pointers remain valid.
ModuleApi LoadModuleApi() {
    ModuleApi api{};

    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    api.library = kernel32;
    api.enumProcessModulesEx = ResolveProc<EnumProcessModulesExFn>(kernel32, "K32EnumProcessModulesEx");
    api.getModuleInformation = ResolveProc<GetModuleInformationFn>(kernel32, "K32GetModuleInformation");
    api.getModuleFileNameExW = ResolveProc<GetModuleFileNameExWFn>(kernel32, "K32GetModuleFileNameExW");
    if (api.available()) {
        return api;
    }

    HMODULE psapi = ::GetModuleHandleW(L"psapi.dll");
    if (!psapi) {
        psapi = ::LoadLibraryW(L"psapi.dll");
    }
    api.library = psapi;
    api.enumProcessModulesEx = ResolveProc<EnumProcessModulesExFn>(psapi, "EnumProcessModulesEx");
    api.getModuleInformation = ResolveProc<GetModuleInformationFn>(psapi, "GetModuleInformation");
    api.getModuleFileNameExW = ResolveProc<GetModuleFileNameExWFn>(psapi, "GetModuleFileNameExW");
    return api;
}

// LoadNtThreadApi resolves NtQueryInformationThread. There is no input;
// processing reads ntdll from the current process; output may be unavailable on
// unusual systems but does not fail the thread snapshot.
NtThreadApi LoadNtThreadApi() {
    NtThreadApi api{};
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        ntdll = ::LoadLibraryW(L"ntdll.dll");
    }
    api.queryInformationThread = ResolveProc<NtQueryInformationThreadFn>(ntdll, "NtQueryInformationThread");
    return api;
}

// QueryProcessImagePath reads the target image path through QueryFullProcess-
// ImageNameW. Input is an opened process handle; processing grows a local fixed
// buffer; output is the path or a diagnostic string.
std::wstring QueryProcessImagePath(HANDLE process) {
    std::wstring path(kMaxPathBufferChars, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (::QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
        path.resize(length);
        return path;
    }
    return L"<image path unavailable: " + Ksword::Core::LastErrorMessage() + L">";
}

// QueryProcessSession reads the Terminal Services session for one PID. Input is
// processId; processing calls ProcessIdToSessionId; output is zero on failure
// and the caller records a status message separately.
DWORD QueryProcessSession(DWORD processId, bool& okOut) {
    DWORD sessionId = 0;
    okOut = ::ProcessIdToSessionId(processId, &sessionId) != FALSE;
    return sessionId;
}

// QueryTokenText opens the process token for user and integrity strings. Inputs
// are a process handle and output references; processing uses read-only token
// queries; output strings are diagnostic-safe even when access is denied.
void QueryTokenText(HANDLE process, std::wstring& userOut, std::wstring& integrityOut) {
    Ksword::Core::UniqueHandle token;
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &rawToken)) {
        const std::wstring error = Ksword::Core::LastErrorMessage();
        userOut = L"<token unavailable: " + error + L">";
        integrityOut = L"<token unavailable: " + error + L">";
        return;
    }
    token.reset(rawToken);

    DWORD userBytes = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &userBytes);
    if (userBytes > 0) {
        std::vector<BYTE> userBuffer(userBytes);
        if (::GetTokenInformation(token.get(), TokenUser, userBuffer.data(), userBytes, &userBytes)) {
            const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
            wchar_t name[256]{};
            wchar_t domain[256]{};
            DWORD nameChars = static_cast<DWORD>(_countof(name));
            DWORD domainChars = static_cast<DWORD>(_countof(domain));
            SID_NAME_USE use{};
            if (::LookupAccountSidW(nullptr, tokenUser->User.Sid, name, &nameChars, domain, &domainChars, &use)) {
                userOut = std::wstring(domain) + L"\\" + name;
            } else {
                userOut = L"<sid lookup failed: " + Ksword::Core::LastErrorMessage() + L">";
            }
        }
    }
    if (userOut.empty()) {
        userOut = L"<user unavailable>";
    }

    DWORD integrityBytes = 0;
    ::GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &integrityBytes);
    if (integrityBytes > 0) {
        std::vector<BYTE> integrityBuffer(integrityBytes);
        if (::GetTokenInformation(token.get(), TokenIntegrityLevel, integrityBuffer.data(), integrityBytes, &integrityBytes)) {
            const TOKEN_MANDATORY_LABEL* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrityBuffer.data());
            const DWORD rid = *::GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*::GetSidSubAuthorityCount(label->Label.Sid) - 1));
            if (rid >= SECURITY_MANDATORY_SYSTEM_RID) {
                integrityOut = L"System";
            } else if (rid >= SECURITY_MANDATORY_HIGH_RID) {
                integrityOut = L"High";
            } else if (rid >= SECURITY_MANDATORY_MEDIUM_RID) {
                integrityOut = L"Medium";
            } else if (rid >= SECURITY_MANDATORY_LOW_RID) {
                integrityOut = L"Low";
            } else {
                integrityOut = L"Untrusted";
            }
        }
    }
    if (integrityOut.empty()) {
        integrityOut = L"<integrity unavailable>";
    }
}

// QueryBitnessText determines process architecture without injecting or
// executing target code. Input is process handle; processing prefers
// IsWow64Process2 then falls back to IsWow64Process; output is UI text.
std::wstring QueryBitnessText(HANDLE process) {
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const auto isWow64Process2 = kernel32
        ? reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(kernel32, "IsWow64Process2"))
        : nullptr;
    if (isWow64Process2) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN) {
                return sizeof(void*) == 8 ? L"64-bit native" : L"32-bit native";
            }
            return L"32-bit WOW64";
        }
    }

    BOOL wow64 = FALSE;
    if (::IsWow64Process(process, &wow64)) {
        if (wow64) {
            return L"32-bit WOW64";
        }
        return sizeof(void*) == 8 ? L"64-bit native" : L"32-bit native";
    }
    return L"<bitness unavailable: " + Ksword::Core::LastErrorMessage() + L">";
}

// QueryParentProcessId uses ProcessBasicInformation when available. Input is an
// opened process handle; processing dynamically resolves NtQueryInformation-
// Process; output is zero when unavailable.
DWORD QueryParentProcessId(HANDLE process) {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto queryProcess = ntdll
        ? reinterpret_cast<NtQueryInformationProcessFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess"))
        : nullptr;
    if (!queryProcess) {
        return 0;
    }

    NativeProcessBasicInformation basic{};
    ULONG returned = 0;
    const LONG status = queryProcess(process, kProcessBasicInformationClass, &basic, sizeof(basic), &returned);
    if (status < 0) {
        return 0;
    }
    return static_cast<DWORD>(basic.inheritedFromUniqueProcessId);
}

// ReadRemoteUnicodeString copies a UNICODE_STRING value from the target process.
// Inputs are a process handle and a remote string descriptor; processing caps
// the read size and calls ReadProcessMemory once; output is text or a diagnostic.
std::wstring ReadRemoteUnicodeString(HANDLE process, const UNICODE_STRING& remoteText) {
    if (!remoteText.Buffer || remoteText.Length == 0) {
        return L"";
    }
    if (remoteText.Length > kMaxPathBufferChars * sizeof(wchar_t)) {
        return L"<remote string too large>";
    }

    std::wstring text(remoteText.Length / sizeof(wchar_t), L'\0');
    SIZE_T bytesRead = 0;
    if (!::ReadProcessMemory(process, remoteText.Buffer, text.data(), remoteText.Length, &bytesRead)) {
        return L"<ReadProcessMemory failed: " + Ksword::Core::LastErrorMessage() + L">";
    }
    text.resize(bytesRead / sizeof(wchar_t));
    return text;
}

// QueryCommandLineText reads the target process command line from the PEB when
// readable. Input is a process handle; processing uses NtQueryInformationProcess
// for the PEB address and ReadProcessMemory for ProcessParameters; output is the
// command line or a failure reason without blocking the rest of the page.
std::wstring QueryCommandLineText(HANDLE process) {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto queryProcess = ntdll
        ? reinterpret_cast<NtQueryInformationProcessFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess"))
        : nullptr;
    if (!queryProcess) {
        return L"<NtQueryInformationProcess unavailable>";
    }

    NativeProcessBasicInformation basic{};
    ULONG returned = 0;
    const LONG status = queryProcess(process, kProcessBasicInformationClass, &basic, sizeof(basic), &returned);
    if (status < 0 || !basic.pebBaseAddress) {
        return L"<ProcessBasicInformation unavailable>";
    }

    RemotePeb peb{};
    SIZE_T bytesRead = 0;
    if (!::ReadProcessMemory(process, basic.pebBaseAddress, &peb, sizeof(peb), &bytesRead)) {
        return L"<PEB read failed: " + Ksword::Core::LastErrorMessage() + L">";
    }

    RemoteProcessParameters parameters{};
    if (!::ReadProcessMemory(process, peb.processParameters, &parameters, sizeof(parameters), &bytesRead)) {
        return L"<ProcessParameters read failed: " + Ksword::Core::LastErrorMessage() + L">";
    }

    std::wstring commandLine = ReadRemoteUnicodeString(process, parameters.commandLine);
    if (commandLine.empty()) {
        commandLine = L"<empty command line>";
    }
    return commandLine;
}

// CollectBasicInfo reads the basic tab fields. Input is a PID; processing opens
// the process with limited read access and tolerates partial failure; output is
// a filled ProcessBasicInfo plus a success bit.
ProcessBasicInfo CollectBasicInfo(DWORD processId, bool& succeededOut) {
    ProcessBasicInfo info{};
    info.processId = processId;
    succeededOut = false;

    HANDLE rawProcess = ::OpenProcess(kProcessBasicAccess, FALSE, processId);
    Ksword::Core::UniqueHandle process(rawProcess);
    if (!process.valid()) {
        info.statusText = Win32ErrorText(L"OpenProcess", ::GetLastError());
        return info;
    }

    succeededOut = true;
    info.parentProcessId = QueryParentProcessId(process.get());
    info.imagePath = QueryProcessImagePath(process.get());
    Ksword::Core::UniqueHandle readableProcess(::OpenProcess(kProcessReadAccess, FALSE, processId));
    info.commandLine = readableProcess.valid()
        ? QueryCommandLineText(readableProcess.get())
        : L"<command line unavailable: " + Ksword::Core::LastErrorMessage() + L">";
    info.bitness = QueryBitnessText(process.get());
    bool sessionOk = false;
    info.sessionId = QueryProcessSession(processId, sessionOk);
    QueryTokenText(process.get(), info.userName, info.integrityLevel);
    info.statusText = sessionOk ? L"OK" : L"OK; session unavailable: " + Ksword::Core::LastErrorMessage();
    return info;
}

// CollectThreads enumerates threads owned by the target PID. Input is processId;
// processing uses CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD) and never touches
// process handles; output rows include Toolhelp metadata and status text.
std::vector<ProcessThreadInfo> CollectThreads(DWORD processId, bool& succeededOut, std::wstring& statusOut) {
    succeededOut = false;
    statusOut.clear();
    std::vector<ProcessThreadInfo> rows;
    const NtThreadApi threadApi = LoadNtThreadApi();

    Ksword::Core::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot.valid()) {
        statusOut = Win32ErrorText(L"CreateToolhelp32Snapshot(THREAD)", ::GetLastError());
        return rows;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!::Thread32First(snapshot.get(), &entry)) {
        statusOut = Win32ErrorText(L"Thread32First", ::GetLastError());
        return rows;
    }

    do {
        if (entry.th32OwnerProcessID != processId) {
            continue;
        }

        ProcessThreadInfo row{};
        row.threadId = entry.th32ThreadID;
        row.ownerProcessId = entry.th32OwnerProcessID;
        row.basePriority = entry.tpBasePri;
        row.deltaPriority = entry.tpDeltaPri;
        row.suspendCount = 0;

        Ksword::Core::UniqueHandle thread(::OpenThread(kThreadQueryAccess, FALSE, row.threadId));
        if (thread.valid() && threadApi.available()) {
            PVOID startAddress = nullptr;
            const LONG status = threadApi.queryInformationThread(
                thread.get(),
                kThreadQuerySetWin32StartAddressClass,
                &startAddress,
                sizeof(startAddress),
                nullptr);
            if (status >= 0) {
                row.startAddress = reinterpret_cast<std::uintptr_t>(startAddress);
                row.statusText = L"OK";
            } else {
                row.statusText = L"NtQueryInformationThread failed";
            }
        } else if (thread.valid()) {
            row.statusText = L"OK; NtQueryInformationThread unavailable";
        } else {
            row.statusText = L"OpenThread limited info failed: " + Ksword::Core::LastErrorMessage();
        }
        rows.push_back(std::move(row));
    } while (::Thread32Next(snapshot.get(), &entry));

    succeededOut = true;
    statusOut = L"OK";
    std::sort(rows.begin(), rows.end(), [](const ProcessThreadInfo& left, const ProcessThreadInfo& right) {
        return left.threadId < right.threadId;
    });
    return rows;
}

// BaseNameFromPath extracts the final path component. Input is a full path;
// processing searches slash and backslash separators; output is never longer
// than the input and may equal the input for bare names.
std::wstring BaseNameFromPath(const std::wstring& path) {
    const std::size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos + 1 >= path.size()) {
        return path;
    }
    return path.substr(pos + 1);
}

// CollectModules enumerates modules loaded in the process. Input is processId;
// processing uses EnumProcessModulesEx and GetModuleInformation/GetModuleFile-
// NameExW; output is sorted by base address with per-row status.
std::vector<ProcessModuleInfo> CollectModules(DWORD processId, bool& succeededOut, std::wstring& statusOut) {
    succeededOut = false;
    statusOut.clear();
    std::vector<ProcessModuleInfo> rows;

    const ModuleApi moduleApi = LoadModuleApi();
    if (!moduleApi.available()) {
        statusOut = L"Module enumeration API unavailable.";
        return rows;
    }

    Ksword::Core::UniqueHandle process(::OpenProcess(kProcessReadAccess, FALSE, processId));
    if (!process.valid()) {
        statusOut = Win32ErrorText(L"OpenProcess", ::GetLastError());
        return rows;
    }

    DWORD neededBytes = 0;
    std::vector<HMODULE> modules(256);
    if (!moduleApi.enumProcessModulesEx(process.get(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &neededBytes, LIST_MODULES_ALL)) {
        statusOut = Win32ErrorText(L"EnumProcessModulesEx", ::GetLastError());
        return rows;
    }
    if (neededBytes > modules.size() * sizeof(HMODULE)) {
        modules.resize(neededBytes / sizeof(HMODULE));
        if (!moduleApi.enumProcessModulesEx(process.get(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &neededBytes, LIST_MODULES_ALL)) {
            statusOut = Win32ErrorText(L"EnumProcessModulesEx retry", ::GetLastError());
            return rows;
        }
    }
    modules.resize(neededBytes / sizeof(HMODULE));

    for (HMODULE module : modules) {
        ProcessModuleInfo row{};
        MODULEINFO moduleInfo{};
        if (moduleApi.getModuleInformation(process.get(), module, &moduleInfo, sizeof(moduleInfo))) {
            row.baseAddress = reinterpret_cast<std::uintptr_t>(moduleInfo.lpBaseOfDll);
            row.imageSize = moduleInfo.SizeOfImage;
        }

        std::wstring path(MAX_PATH, L'\0');
        DWORD copied = moduleApi.getModuleFileNameExW(process.get(), module, path.data(), static_cast<DWORD>(path.size()));
        if (copied >= path.size() - 1) {
            path.resize(kMaxPathBufferChars, L'\0');
            copied = moduleApi.getModuleFileNameExW(process.get(), module, path.data(), static_cast<DWORD>(path.size()));
        }
        if (copied > 0) {
            path.resize(copied);
            row.modulePath = path;
            row.moduleName = BaseNameFromPath(path);
            row.statusText = L"OK";
        } else {
            row.moduleName = FormatHexPointer(reinterpret_cast<std::uintptr_t>(module));
            row.modulePath = L"<module path unavailable>";
            row.statusText = Win32ErrorText(L"GetModuleFileNameExW", ::GetLastError());
        }
        rows.push_back(std::move(row));
    }

    succeededOut = true;
    statusOut = L"OK";
    std::sort(rows.begin(), rows.end(), [](const ProcessModuleInfo& left, const ProcessModuleInfo& right) {
        return left.baseAddress < right.baseAddress;
    });
    return rows;
}

// AttachRepresentativeThreads maps already-collected thread start addresses to
// loaded module address ranges. Inputs are the module rows and thread rows from
// the same PID snapshot; processing chooses the first thread whose Win32 start
// address lies inside each module; no value is returned because modules are
// updated in place for the Modules tab context menu.
void AttachRepresentativeThreads(
    std::vector<ProcessModuleInfo>& modules,
    const std::vector<ProcessThreadInfo>& threads) {
    if (modules.empty() || threads.empty()) {
        return;
    }

    for (ProcessModuleInfo& module : modules) {
        const std::uintptr_t moduleStart = module.baseAddress;
        const std::uintptr_t moduleEnd = moduleStart + static_cast<std::uintptr_t>(module.imageSize);
        if (moduleStart == 0 || moduleEnd <= moduleStart) {
            continue;
        }

        for (const ProcessThreadInfo& thread : threads) {
            if (thread.startAddress >= moduleStart && thread.startAddress < moduleEnd) {
                module.representativeThreadId = thread.threadId;
                break;
            }
        }
    }
}

} // namespace

ProcessDetailSnapshot ProcessDetailCollector::Collect(DWORD processId) const {
    ProcessDetailSnapshot snapshot{};
    snapshot.basic = CollectBasicInfo(processId, snapshot.basicSucceeded);

    std::wstring threadStatus;
    snapshot.threads = CollectThreads(processId, snapshot.threadsSucceeded, threadStatus);
    if (!snapshot.threadsSucceeded && !threadStatus.empty()) {
        snapshot.errorText += L"Threads: " + threadStatus + L"\r\n";
    }

    std::wstring moduleStatus;
    snapshot.modules = CollectModules(processId, snapshot.modulesSucceeded, moduleStatus);
    if (!snapshot.modulesSucceeded && !moduleStatus.empty()) {
        snapshot.errorText += L"Modules: " + moduleStatus + L"\r\n";
    }
    AttachRepresentativeThreads(snapshot.modules, snapshot.threads);

    if (!snapshot.basicSucceeded && !snapshot.basic.statusText.empty()) {
        snapshot.errorText = L"Basic: " + snapshot.basic.statusText + L"\r\n" + snapshot.errorText;
    }
    if (snapshot.errorText.empty()) {
        snapshot.errorText = L"OK";
    }
    return snapshot;
}

} // namespace Ksword::Features::ProcessDetail
