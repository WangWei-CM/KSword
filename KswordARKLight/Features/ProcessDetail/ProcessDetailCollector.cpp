#include "ProcessDetailCollector.h"

#include "../../Core/Common.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <limits>
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

// NarrowToWide converts ArkDriverClient diagnostics and row details to UI text.
// Input is UTF-8/ASCII from the shared client; output is best-effort UTF-16.
std::wstring NarrowToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    int chars = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (chars <= 0) {
        codePage = CP_ACP;
        chars = ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (chars <= 0) {
        return L"<decode failed>";
    }

    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), wide.data(), chars);
    return wide;
}

// HexMaskText formats raw source/anomaly masks for diagnostics. Input is a
// protocol mask; output remains stable when future bits are added.
std::wstring HexMaskText(ULONG value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

// CrossViewSourceText renders shared process/thread source bits. Input is the
// R0 sourceMask; output uses protocol source names shown by the GUI pages.
std::wstring CrossViewSourceText(ULONG sourceMask) {
    std::vector<std::wstring> parts;
    if ((sourceMask & KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK) != 0) {
        parts.push_back(L"Public");
    }
    if ((sourceMask & KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST) != 0) {
        parts.push_back(L"ActiveProcessLinks");
    }
    if ((sourceMask & KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE) != 0) {
        parts.push_back(L"CID");
    }
    if ((sourceMask & KSWORD_ARK_CROSSVIEW_SOURCE_THREAD_LIST) != 0) {
        parts.push_back(L"ThreadListHead");
    }
    if (parts.empty()) {
        return L"无来源";
    }

    std::wstring text;
    for (const std::wstring& part : parts) {
        if (!text.empty()) {
            text += L"+";
        }
        text += part;
    }
    return text;
}

// CrossViewAnomalyText maps known anomaly flags to readable labels. Input is
// the raw protocol mask; output keeps unknown bits visible for audit export.
std::wstring CrossViewAnomalyText(ULONG anomalyFlags) {
    if (anomalyFlags == 0) {
        return L"未见异常";
    }

    std::vector<std::wstring> parts;
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) != 0) {
        parts.push_back(L"CID-only");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) != 0) {
        parts.push_back(L"Active-only");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) != 0) {
        parts.push_back(L"缺ActiveProcessLinks");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) != 0) {
        parts.push_back(L"缺CID");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) != 0) {
        parts.push_back(L"孤儿线程");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) != 0) {
        parts.push_back(L"缺ThreadListHead");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) != 0) {
        parts.push_back(L"入口不在模块");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_PID_FIELD_MISMATCH) != 0) {
        parts.push_back(L"PID不一致");
    }

    std::wstring text;
    for (const std::wstring& part : parts) {
        if (!text.empty()) {
            text += L"; ";
        }
        text += part;
    }
    if (text.empty()) {
        text = L"未知异常位 " + HexMaskText(anomalyFlags);
    }
    return text;
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

// LeafNameFromPath returns the final path component. Input may be a full DOS
// path or a bare image name; output is empty only when the input is empty.
std::wstring LeafNameFromPath(const std::wstring& path) {
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return path;
    }
    return separator + 1U < path.size() ? path.substr(separator + 1U) : std::wstring{};
}

// QuerySnapshotIdentity uses the public Toolhelp process snapshot to fill the
// target/parent names and the target's snapshot thread count. Inputs are the
// target and parent PIDs; output fields remain unchanged when rows disappeared.
void QuerySnapshotIdentity(
    DWORD processId,
    DWORD& parentProcessIdInOut,
    std::wstring& processNameOut,
    std::wstring& parentProcessNameOut,
    DWORD& threadCountOut) {
    Ksword::Core::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) {
        return;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry)) {
        return;
    }
    do {
        if (entry.th32ProcessID == processId) {
            processNameOut = entry.szExeFile;
            threadCountOut = entry.cntThreads;
            if (parentProcessIdInOut == 0) {
                parentProcessIdInOut = entry.th32ParentProcessID;
            }
        }
        if (parentProcessIdInOut != 0 && entry.th32ProcessID == parentProcessIdInOut) {
            parentProcessNameOut = entry.szExeFile;
        }
    } while (::Process32NextW(snapshot.get(), &entry));

    // The parent row can sort before the target row, so restart once when the
    // target supplied its PPID after that row had already passed.
    if (parentProcessIdInOut != 0 && parentProcessNameOut.empty()) {
        entry = {};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshot.get(), &entry)) {
            do {
                if (entry.th32ProcessID == parentProcessIdInOut) {
                    parentProcessNameOut = entry.szExeFile;
                    break;
                }
            } while (::Process32NextW(snapshot.get(), &entry));
        }
    }
}

// FormatProcessStartTime converts a creation FILETIME into local wall-clock
// text. Input is UTC FILETIME; output is YYYY-MM-DD HH:MM:SS or unavailable.
std::wstring FormatProcessStartTime(const FILETIME& creationTime) {
    FILETIME localTime{};
    SYSTEMTIME systemTime{};
    if (!::FileTimeToLocalFileTime(&creationTime, &localTime) ||
        !::FileTimeToSystemTime(&localTime, &systemTime)) {
        return L"<start time unavailable>";
    }

    wchar_t text[32]{};
    _snwprintf_s(
        text,
        _countof(text),
        _TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u",
        systemTime.wYear,
        systemTime.wMonth,
        systemTime.wDay,
        systemTime.wHour,
        systemTime.wMinute,
        systemTime.wSecond);
    return text;
}

// PriorityClassText maps GetPriorityClass values to stable user-facing text.
// Input is zero on query failure; output preserves failure as unavailable.
std::wstring PriorityClassText(DWORD priorityClass) {
    switch (priorityClass) {
    case IDLE_PRIORITY_CLASS: return L"Idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return L"Below Normal";
    case NORMAL_PRIORITY_CLASS: return L"Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above Normal";
    case HIGH_PRIORITY_CLASS: return L"High";
    case REALTIME_PRIORITY_CLASS: return L"Realtime";
    default: return L"<priority unavailable>";
    }
}

// SaturatingAdd64 aggregates monotonically increasing I/O counters without
// wrapping when a long-lived process approaches the unsigned 64-bit limit.
ULONGLONG SaturatingAdd64(ULONGLONG left, ULONGLONG right) {
    const ULONGLONG maximum = (std::numeric_limits<ULONGLONG>::max)();
    return right > maximum - left ? maximum : left + right;
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
void QueryTokenText(
    HANDLE process,
    std::wstring& userOut,
    std::wstring& integrityOut,
    bool& isAdminOut,
    bool& adminKnownOut) {
    isAdminOut = false;
    adminKnownOut = false;
    Ksword::Core::UniqueHandle token;
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &rawToken)) {
        const std::wstring error = Ksword::Core::LastErrorMessage();
        userOut = L"<token unavailable: " + error + L">";
        integrityOut = L"<token unavailable: " + error + L">";
        return;
    }
    token.reset(rawToken);

    TOKEN_ELEVATION elevation{};
    DWORD elevationBytes = 0;
    if (::GetTokenInformation(
            token.get(),
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &elevationBytes)) {
        isAdminOut = elevation.TokenIsElevated != 0;
        adminKnownOut = true;
    }

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

// QueryNativeProcessBasicInformation reads stable class-zero process metadata.
// Input is an opened process handle; output carries PPID, PEB and affinity.
bool QueryNativeProcessBasicInformation(HANDLE process, NativeProcessBasicInformation& basicOut) {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto queryProcess = ntdll
        ? reinterpret_cast<NtQueryInformationProcessFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess"))
        : nullptr;
    if (!queryProcess) {
        return false;
    }

    basicOut = {};
    ULONG returned = 0;
    return queryProcess(
        process,
        kProcessBasicInformationClass,
        &basicOut,
        sizeof(basicOut),
        &returned) >= 0;
}

// QueryParentProcessId uses ProcessBasicInformation when available. Input is an
// opened process handle; output is zero when unavailable.
DWORD QueryParentProcessId(HANDLE process) {
    NativeProcessBasicInformation basic{};
    return QueryNativeProcessBasicInformation(process, basic)
        ? static_cast<DWORD>(basic.inheritedFromUniqueProcessId)
        : 0;
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
    NativeProcessBasicInformation basic{};
    if (!QueryNativeProcessBasicInformation(process, basic) || !basic.pebBaseAddress) {
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

    QuerySnapshotIdentity(
        processId,
        info.parentProcessId,
        info.processName,
        info.parentProcessName,
        info.threadCount);

    HANDLE rawProcess = ::OpenProcess(kProcessBasicAccess, FALSE, processId);
    Ksword::Core::UniqueHandle process(rawProcess);
    if (!process.valid()) {
        info.statusText = Win32ErrorText(L"OpenProcess", ::GetLastError());
        return info;
    }

    succeededOut = true;
    const DWORD nativeParentProcessId = QueryParentProcessId(process.get());
    if (nativeParentProcessId != 0) {
        info.parentProcessId = nativeParentProcessId;
    }
    info.imagePath = QueryProcessImagePath(process.get());
    if (!info.imagePath.empty() && info.imagePath.front() != L'<') {
        info.processName = LeafNameFromPath(info.imagePath);
    }
    QuerySnapshotIdentity(
        processId,
        info.parentProcessId,
        info.processName,
        info.parentProcessName,
        info.threadCount);

    NativeProcessBasicInformation nativeBasic{};
    if (QueryNativeProcessBasicInformation(process.get(), nativeBasic)) {
        if (nativeBasic.pebBaseAddress) {
            info.pebAddress = reinterpret_cast<std::uintptr_t>(nativeBasic.pebBaseAddress);
            info.pebAddressKnown = true;
        }
        if (nativeBasic.affinityMask != 0) {
            info.affinityMask = static_cast<std::uint64_t>(nativeBasic.affinityMask);
            info.affinityKnown = true;
        }
    }

    Ksword::Core::UniqueHandle readableProcess(::OpenProcess(kProcessReadAccess, FALSE, processId));
    info.commandLine = readableProcess.valid()
        ? QueryCommandLineText(readableProcess.get())
        : L"<command line unavailable: " + Ksword::Core::LastErrorMessage() + L">";
    info.bitness = QueryBitnessText(process.get());
    bool sessionOk = false;
    info.sessionId = QueryProcessSession(processId, sessionOk);
    const DWORD sessionError = sessionOk ? ERROR_SUCCESS : ::GetLastError();
    QueryTokenText(
        process.get(),
        info.userName,
        info.integrityLevel,
        info.isAdmin,
        info.adminKnown);

    FILETIME creationTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    info.startTimeText = ::GetProcessTimes(
        process.get(),
        &creationTime,
        &exitTime,
        &kernelTime,
        &userTime)
        ? FormatProcessStartTime(creationTime)
        : L"<start time unavailable>";

    info.priorityText = PriorityClassText(::GetPriorityClass(process.get()));
    DWORD handleCount = 0;
    if (::GetProcessHandleCount(process.get(), &handleCount)) {
        info.handleCount = handleCount;
    }

    DWORD_PTR processAffinity = 0;
    DWORD_PTR systemAffinity = 0;
    if (::GetProcessAffinityMask(process.get(), &processAffinity, &systemAffinity)) {
        info.affinityMask = static_cast<std::uint64_t>(processAffinity);
        info.affinityKnown = true;
    }

    PROCESS_MEMORY_COUNTERS_EX memoryCounters{};
    memoryCounters.cb = sizeof(memoryCounters);
    HANDLE memoryProcess = readableProcess.valid() ? readableProcess.get() : process.get();
    if (::GetProcessMemoryInfo(
            memoryProcess,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryCounters),
            sizeof(memoryCounters))) {
        info.workingSetBytes = static_cast<ULONGLONG>(memoryCounters.WorkingSetSize);
        info.privateBytes = static_cast<ULONGLONG>(memoryCounters.PrivateUsage);
    }

    IO_COUNTERS ioCounters{};
    if (::GetProcessIoCounters(process.get(), &ioCounters)) {
        info.ioBytes = SaturatingAdd64(
            SaturatingAdd64(ioCounters.ReadTransferCount, ioCounters.WriteTransferCount),
            ioCounters.OtherTransferCount);
    }
    info.statusText = sessionOk
        ? L"OK"
        : L"OK; session unavailable: " + Ksword::Core::LastErrorMessage(sessionError);
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

// AddR0AuditRow appends one read-only driver evidence row to the ProcessDetail
// R0 tab. Inputs are the target vector and display-ready scalar fields;
// processing preserves object addresses only for display and never feeds them
// into write operations; there is no return value.
void AddR0AuditRow(
    std::vector<ProcessR0AuditInfo>& rows,
    const std::wstring& scope,
    DWORD processId,
    DWORD threadId,
    std::uint64_t objectAddress,
    std::uint64_t relatedObjectAddress,
    std::uint64_t startAddress,
    const std::wstring& sourceText,
    const std::wstring& statusText,
    ULONG confidence,
    const std::wstring& detailText) {
    ProcessR0AuditInfo row{};
    row.scope = scope;
    row.processId = processId;
    row.threadId = threadId;
    row.objectAddress = static_cast<std::uintptr_t>(objectAddress);
    row.relatedObjectAddress = static_cast<std::uintptr_t>(relatedObjectAddress);
    row.startAddress = static_cast<std::uintptr_t>(startAddress);
    row.confidence = confidence;
    row.sourceText = sourceText;
    row.anomalyText = statusText;
    row.detailText = detailText;
    rows.push_back(std::move(row));
}

// StatusHexText renders an NTSTATUS/LONG as fixed hexadecimal text. Input is a
// signed status value from the driver protocol; output is a UI diagnostic token.
std::wstring StatusHexText(long status) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(status);
    return stream.str();
}

// BuildProcessRuntimeSampleItems derives safe field-sample requests from the
// fixed process detail response. Inputs are offsets already returned by R0;
// processing keeps only known, bounded EPROCESS fields; output is suitable for
// ArkDriverClient::queryProcessRuntimeFieldSamples.
std::vector<ksword::ark::RuntimeFieldSampleRequestItem> BuildProcessRuntimeSampleItems(
    const KSWORD_ARK_PROCESS_DETAIL_RESPONSE& detail) {
    std::vector<ksword::ark::RuntimeFieldSampleRequestItem> items;
    const auto add = [&](std::uint32_t id, std::uint32_t offset, std::uint32_t size, const char* name, const char* type) {
        if (offset == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE || size == 0 || size > KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_VALUE_BYTES) {
            return;
        }
        ksword::ark::RuntimeFieldSampleRequestItem item{};
        item.runtimeItemId = id;
        item.offset = offset;
        item.size = size;
        item.name = name;
        item.type = type;
        items.push_back(std::move(item));
    };
    add(KSW_DYN_FIELD_ID_EP_UNIQUE_PROCESS_ID, detail.offsets.epUniqueProcessId, sizeof(std::uint64_t), "EP.UniqueProcessId", "HANDLE");
    add(KSW_DYN_FIELD_ID_EP_ACTIVE_PROCESS_LINKS, detail.offsets.epActiveProcessLinks, sizeof(std::uint64_t), "EP.ActiveProcessLinks", "LIST_ENTRY.Flink");
    add(KSW_DYN_FIELD_ID_EP_THREAD_LIST_HEAD, detail.offsets.epThreadListHead, sizeof(std::uint64_t), "EP.ThreadListHead", "LIST_ENTRY.Flink");
    add(KSW_DYN_FIELD_ID_EP_TOKEN, detail.offsets.epToken, sizeof(std::uint64_t), "EP.Token", "EX_FAST_REF");
    add(KSW_DYN_FIELD_ID_EP_OBJECT_TABLE, detail.offsets.epObjectTable, sizeof(std::uint64_t), "EP.ObjectTable", "EXHANDLE_TABLE*");
    add(KSW_DYN_FIELD_ID_EP_SECTION_OBJECT, detail.offsets.epSectionObject, sizeof(std::uint64_t), "EP.SectionObject", "SECTION_OBJECT*");
    add(KSW_DYN_FIELD_ID_EP_PROTECTION, detail.offsets.epProtection, sizeof(std::uint8_t), "EP.Protection", "PS_PROTECTION");
    add(KSW_DYN_FIELD_ID_EP_SIGNATURE_LEVEL, detail.offsets.epSignatureLevel, sizeof(std::uint8_t), "EP.SignatureLevel", "UCHAR");
    add(KSW_DYN_FIELD_ID_EP_SECTION_SIGNATURE_LEVEL, detail.offsets.epSectionSignatureLevel, sizeof(std::uint8_t), "EP.SectionSignatureLevel", "UCHAR");
    return items;
}

// BuildThreadRuntimeSampleItems derives safe field-sample requests from the
// fixed thread detail response. Inputs are R0-provided ETHREAD/KTHREAD offsets;
// processing keeps known field sizes under the protocol cap; output is suitable
// for ArkDriverClient::queryThreadRuntimeFieldSamples.
std::vector<ksword::ark::RuntimeFieldSampleRequestItem> BuildThreadRuntimeSampleItems(
    const KSWORD_ARK_THREAD_DETAIL_RESPONSE& detail) {
    std::vector<ksword::ark::RuntimeFieldSampleRequestItem> items;
    const auto add = [&](std::uint32_t id, std::uint32_t offset, std::uint32_t size, const char* name, const char* type) {
        if (offset == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE || size == 0 || size > KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_VALUE_BYTES) {
            return;
        }
        ksword::ark::RuntimeFieldSampleRequestItem item{};
        item.runtimeItemId = id;
        item.offset = offset;
        item.size = size;
        item.name = name;
        item.type = type;
        items.push_back(std::move(item));
    };
    add(KSW_DYN_FIELD_ID_ET_CID, detail.offsets.etCid, sizeof(std::uint64_t) * 2U, "ET.Cid", "CLIENT_ID");
    add(KSW_DYN_FIELD_ID_ET_THREAD_LIST_ENTRY, detail.offsets.etThreadListEntry, sizeof(std::uint64_t), "ET.ThreadListEntry", "LIST_ENTRY.Flink");
    add(KSW_DYN_FIELD_ID_ET_START_ADDRESS, detail.offsets.etStartAddress, sizeof(std::uint64_t), "ET.StartAddress", "PVOID");
    add(KSW_DYN_FIELD_ID_ET_WIN32_START_ADDRESS, detail.offsets.etWin32StartAddress, sizeof(std::uint64_t), "ET.Win32StartAddress", "PVOID");
    add(KSW_DYN_FIELD_ID_KT_PROCESS, detail.offsets.ktProcess, sizeof(std::uint64_t), "KT.Process", "KPROCESS*");
    add(KSW_DYN_FIELD_ID_KT_INITIAL_STACK, detail.offsets.ktInitialStack, sizeof(std::uint64_t), "KT.InitialStack", "PVOID");
    add(KSW_DYN_FIELD_ID_KT_STACK_LIMIT, detail.offsets.ktStackLimit, sizeof(std::uint64_t), "KT.StackLimit", "PVOID");
    add(KSW_DYN_FIELD_ID_KT_STACK_BASE, detail.offsets.ktStackBase, sizeof(std::uint64_t), "KT.StackBase", "PVOID");
    add(KSW_DYN_FIELD_ID_KT_KERNEL_STACK, detail.offsets.ktKernelStack, sizeof(std::uint64_t), "KT.KernelStack", "PVOID");
    add(KSW_DYN_FIELD_ID_KT_READ_OPERATION_COUNT, detail.offsets.ktReadOperationCount, sizeof(std::uint64_t), "KT.ReadOperationCount", "ULONGLONG");
    add(KSW_DYN_FIELD_ID_KT_WRITE_OPERATION_COUNT, detail.offsets.ktWriteOperationCount, sizeof(std::uint64_t), "KT.WriteOperationCount", "ULONGLONG");
    add(KSW_DYN_FIELD_ID_KT_OTHER_OPERATION_COUNT, detail.offsets.ktOtherOperationCount, sizeof(std::uint64_t), "KT.OtherOperationCount", "ULONGLONG");
    return items;
}

// CollectR0AuditRows queries process/thread cross-view through ArkDriverClient.
// Inputs are a PID and status outputs; processing is read-only and bounded by
// the shared cross-view max-node defaults; output rows are UI evidence only.
std::vector<ProcessR0AuditInfo> CollectR0AuditRows(DWORD processId, bool& succeededOut, std::wstring& statusOut) {
    succeededOut = false;
    statusOut.clear();
    std::vector<ProcessR0AuditInfo> rows;

    const ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessCrossViewResult processAudit = driverClient.queryProcessCrossView(
        KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_ALL,
        processId,
        processId,
        KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES);
    if (!processAudit.io.ok) {
        statusOut = L"Process cross-view failed: " + NarrowToWide(processAudit.io.message);
    } else {
        for (const ksword::ark::ProcessCrossViewEntry& entry : processAudit.entries) {
            if (entry.processId != processId) {
                continue;
            }
            ProcessR0AuditInfo row{};
            row.scope = L"Process";
            row.processId = static_cast<DWORD>(entry.processId);
            row.objectAddress = static_cast<std::uintptr_t>(entry.objectAddress);
            row.startAddress = static_cast<std::uintptr_t>(entry.startAddress);
            row.sourceMask = entry.sourceMask;
            row.anomalyFlags = entry.anomalyFlags;
            row.confidence = entry.confidence;
            row.sourceText = CrossViewSourceText(row.sourceMask);
            row.anomalyText = CrossViewAnomalyText(row.anomalyFlags);
            row.detailText = NarrowToWide(entry.detail);
            rows.push_back(std::move(row));
        }
    }

    const ksword::ark::ThreadCrossViewResult threadAudit = driverClient.queryThreadCrossView(
        KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_ALL,
        processId,
        0,
        0,
        KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES);
    if (!threadAudit.io.ok) {
        if (!statusOut.empty()) {
            statusOut += L"; ";
        }
        statusOut += L"Thread cross-view failed: " + NarrowToWide(threadAudit.io.message);
    } else {
        for (const ksword::ark::ThreadCrossViewEntry& entry : threadAudit.entries) {
            if (entry.processId != processId) {
                continue;
            }
            ProcessR0AuditInfo row{};
            row.scope = L"Thread";
            row.processId = static_cast<DWORD>(entry.processId);
            row.threadId = static_cast<DWORD>(entry.threadId);
            row.objectAddress = static_cast<std::uintptr_t>(entry.objectAddress);
            row.relatedObjectAddress = static_cast<std::uintptr_t>(entry.processObjectAddress);
            row.startAddress = static_cast<std::uintptr_t>(entry.startAddress);
            row.sourceMask = entry.sourceMask;
            row.anomalyFlags = entry.anomalyFlags;
            row.confidence = entry.confidence;
            row.sourceText = CrossViewSourceText(row.sourceMask);
            row.anomalyText = CrossViewAnomalyText(row.anomalyFlags);
            row.detailText = NarrowToWide(entry.detail);
            rows.push_back(std::move(row));
        }
    }

    const ksword::ark::ProcessRuntimeDetailResult processDetail =
        driverClient.queryProcessRuntimeDetail(processId);
    if (!processDetail.io.ok) {
        AddR0AuditRow(
            rows,
            L"ProcessDetail",
            processId,
            0,
            0,
            0,
            0,
            L"ArkDriverClient::queryProcessRuntimeDetail",
            processDetail.unsupported ? L"Unsupported" : L"Unavailable",
            0,
            NarrowToWide(processDetail.io.message));
    } else {
        const KSWORD_ARK_PROCESS_DETAIL_RESPONSE& detail = processDetail.response;
        std::wostringstream text;
        text << L"fields=" << HexMaskText(detail.fieldFlags)
             << L"; requested=" << HexMaskText(detail.requestedFlags)
             << L"; dyn=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.dynDataCapabilityMask))
             << L"; missing=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.missingCapabilityMask))
             << L"; token=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.tokenObjectAddress))
             << L"; objectTable=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.objectTableAddress))
             << L"; section=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.sectionObjectAddress))
             << L"; detail=" << std::wstring(detail.detail);
        AddR0AuditRow(
            rows,
            L"ProcessDetail",
            processId,
            0,
            detail.processObjectAddress,
            detail.tokenObjectAddress,
            0,
            L"IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL",
            L"status=" + std::to_wstring(detail.status) + L", last=" + StatusHexText(detail.lastStatus),
            100,
            text.str());

        const std::vector<ksword::ark::RuntimeFieldSampleRequestItem> sampleItems =
            BuildProcessRuntimeSampleItems(detail);
        if (!sampleItems.empty()) {
            const ksword::ark::RuntimeFieldSampleResult samples =
                driverClient.queryProcessRuntimeFieldSamples(processId, sampleItems);
            AddR0AuditRow(
                rows,
                L"ProcessRuntimeFields",
                processId,
                0,
                samples.objectAddress,
                0,
                0,
                L"IOCTL_KSWORD_ARK_QUERY_PROCESS_RUNTIME_FIELDS",
                samples.io.ok ? L"OK" : (samples.unsupported ? L"Unsupported" : L"Unavailable"),
                samples.io.ok ? 100UL : 0UL,
                L"returned=" + std::to_wstring(samples.returnedCount) +
                    L"/" + std::to_wstring(samples.totalCount) +
                    L"; status=" + std::to_wstring(samples.status) +
                    L"; " + NarrowToWide(samples.io.message));
            for (const ksword::ark::RuntimeFieldSampleEntry& entry : samples.entries) {
                AddR0AuditRow(
                    rows,
                    L"ProcessField",
                    processId,
                    0,
                    samples.objectAddress,
                    0,
                    0,
                    NarrowToWide(entry.name.empty() ? std::string("runtime-field") : entry.name),
                    L"rowStatus=" + std::to_wstring(entry.status),
                    entry.status == KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_OK ? 100UL : 50UL,
                    L"id=" + std::to_wstring(entry.runtimeItemId) +
                        L"; offset=" + HexMaskText(entry.offset) +
                        L"; size=" + std::to_wstring(entry.size) +
                        L"; bytesRead=" + std::to_wstring(entry.bytesRead) +
                        L"; value=" + FormatHexPointer(static_cast<std::uintptr_t>(entry.valueU64)) +
                        L"; last=" + StatusHexText(entry.lastStatus));
            }
        }
    }

    std::size_t threadDetailCount = 0U;
    for (const ksword::ark::ThreadCrossViewEntry& entry : threadAudit.entries) {
        if (entry.processId != processId || entry.threadId == 0U) {
            continue;
        }
        if (threadDetailCount >= 128U) {
            AddR0AuditRow(
                rows,
                L"ThreadDetail",
                processId,
                0,
                0,
                0,
                0,
                L"ArkDriverClient::queryThreadRuntimeDetail",
                L"Truncated",
                0,
                L"Thread runtime detail rows are capped at 128 per refresh to keep the Light GUI responsive.");
            break;
        }
        ++threadDetailCount;
        const ksword::ark::ThreadRuntimeDetailResult threadDetail =
            driverClient.queryThreadRuntimeDetail(entry.threadId, processId);
        if (!threadDetail.io.ok) {
            AddR0AuditRow(
                rows,
                L"ThreadDetail",
                processId,
                entry.threadId,
                entry.objectAddress,
                entry.processObjectAddress,
                entry.startAddress,
                L"ArkDriverClient::queryThreadRuntimeDetail",
                threadDetail.unsupported ? L"Unsupported" : L"Unavailable",
                0,
                NarrowToWide(threadDetail.io.message));
            continue;
        }

        const KSWORD_ARK_THREAD_DETAIL_RESPONSE& detail = threadDetail.response;
        std::wostringstream text;
        text << L"fields=" << HexMaskText(detail.fieldFlags)
             << L"; requested=" << HexMaskText(detail.requestedFlags)
             << L"; cidPid=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.cidUniqueProcess))
             << L"; cidTid=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.cidUniqueThread))
             << L"; start=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.startAddress))
             << L"; win32Start=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.win32StartAddress))
             << L"; stack=" << FormatHexPointer(static_cast<std::uintptr_t>(detail.stackLimit))
             << L"-" << FormatHexPointer(static_cast<std::uintptr_t>(detail.stackBase))
             << L"; detail=" << std::wstring(detail.detail);
        AddR0AuditRow(
            rows,
            L"ThreadDetail",
            processId,
            detail.threadId,
            detail.threadObjectAddress,
            detail.processObjectAddress,
            detail.startAddress,
            L"IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL",
            L"status=" + std::to_wstring(detail.status) + L", last=" + StatusHexText(detail.lastStatus),
            100,
            text.str());

        const std::vector<ksword::ark::RuntimeFieldSampleRequestItem> sampleItems =
            BuildThreadRuntimeSampleItems(detail);
        if (!sampleItems.empty()) {
            const ksword::ark::RuntimeFieldSampleResult samples =
                driverClient.queryThreadRuntimeFieldSamples(detail.threadId, processId, sampleItems);
            AddR0AuditRow(
                rows,
                L"ThreadRuntimeFields",
                processId,
                detail.threadId,
                samples.objectAddress,
                detail.processObjectAddress,
                detail.startAddress,
                L"IOCTL_KSWORD_ARK_QUERY_THREAD_RUNTIME_FIELDS",
                samples.io.ok ? L"OK" : (samples.unsupported ? L"Unsupported" : L"Unavailable"),
                samples.io.ok ? 100UL : 0UL,
                L"returned=" + std::to_wstring(samples.returnedCount) +
                    L"/" + std::to_wstring(samples.totalCount) +
                    L"; status=" + std::to_wstring(samples.status) +
                    L"; " + NarrowToWide(samples.io.message));
            for (const ksword::ark::RuntimeFieldSampleEntry& sample : samples.entries) {
                AddR0AuditRow(
                    rows,
                    L"ThreadField",
                    processId,
                    detail.threadId,
                    samples.objectAddress,
                    detail.processObjectAddress,
                    detail.startAddress,
                    NarrowToWide(sample.name.empty() ? std::string("runtime-field") : sample.name),
                    L"rowStatus=" + std::to_wstring(sample.status),
                    sample.status == KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_OK ? 100UL : 50UL,
                    L"id=" + std::to_wstring(sample.runtimeItemId) +
                        L"; offset=" + HexMaskText(sample.offset) +
                        L"; size=" + std::to_wstring(sample.size) +
                        L"; bytesRead=" + std::to_wstring(sample.bytesRead) +
                        L"; value=" + FormatHexPointer(static_cast<std::uintptr_t>(sample.valueU64)) +
                        L"; last=" + StatusHexText(sample.lastStatus));
            }
        }
    }

    succeededOut = statusOut.empty();
    if (statusOut.empty()) {
        statusOut = L"OK";
    }
    return rows;
}

} // namespace

ProcessDetailSnapshot ProcessDetailCollector::Collect(DWORD processId) const {
    ProcessDetailSnapshot snapshot{};
    snapshot.basic = CollectBasicInfo(processId, snapshot.basicSucceeded);

    std::wstring threadStatus;
    snapshot.threads = CollectThreads(processId, snapshot.threadsSucceeded, threadStatus);
    if (snapshot.threadsSucceeded) {
        const std::size_t boundedThreadCount = (std::min)(
            snapshot.threads.size(),
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
        snapshot.basic.threadCount = static_cast<DWORD>(boundedThreadCount);
    }
    if (!snapshot.threadsSucceeded && !threadStatus.empty()) {
        snapshot.errorText += L"Threads: " + threadStatus + L"\r\n";
    }

    std::wstring moduleStatus;
    snapshot.modules = CollectModules(processId, snapshot.modulesSucceeded, moduleStatus);
    if (!snapshot.modulesSucceeded && !moduleStatus.empty()) {
        snapshot.errorText += L"Modules: " + moduleStatus + L"\r\n";
    }
    AttachRepresentativeThreads(snapshot.modules, snapshot.threads);

    std::wstring r0AuditStatus;
    snapshot.r0AuditRows = CollectR0AuditRows(processId, snapshot.r0AuditSucceeded, r0AuditStatus);
    if (!snapshot.r0AuditSucceeded && !r0AuditStatus.empty()) {
        snapshot.errorText += L"R0Audit: " + r0AuditStatus + L"\r\n";
    }

    if (!snapshot.basicSucceeded && !snapshot.basic.statusText.empty()) {
        snapshot.errorText = L"Basic: " + snapshot.basic.statusText + L"\r\n" + snapshot.errorText;
    }
    if (snapshot.errorText.empty()) {
        snapshot.errorText = L"OK";
    }
    return snapshot;
}

} // namespace Ksword::Features::ProcessDetail
