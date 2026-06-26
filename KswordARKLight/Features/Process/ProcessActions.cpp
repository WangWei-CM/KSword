#include "ProcessActions.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../Core\Common.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <shellapi.h>
#include <sstream>
#include <string>

namespace Ksword::Features::Process {
namespace {
constexpr ULONG kProcessBreakOnTerminationInfoClass = 29UL;
constexpr ULONG kProcessPowerThrottlingInfoClass = 4UL;
constexpr ULONG kProcessPowerThrottlingCurrentVersion = 1UL;
constexpr ULONG kProcessPowerThrottlingExecutionSpeed = 0x1UL;
constexpr DWORD kProcessSuspendResumeAccess = 0x0800UL;

using NtSuspendProcessFn = LONG(NTAPI*)(HANDLE);
using NtResumeProcessFn = LONG(NTAPI*)(HANDLE);
using NtSetInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, ULONG, LPVOID, DWORD);

// ProcessPowerThrottlingStateNative mirrors PROCESS_POWER_THROTTLING_STATE
// without requiring a new SDK. Inputs are written by SetEfficiencyModeForPid;
// processing passes the structure to SetProcessInformation; it returns no value.
struct ProcessPowerThrottlingStateNative {
    ULONG version = 0;
    ULONG controlMask = 0;
    ULONG stateMask = 0;
};

// Utf8ToWide converts ArkDriverClient diagnostic messages into the Win32 UI
// encoding. Input is a UTF-8/narrow diagnostic string; processing asks Windows
// for the exact UTF-16 size and falls back to byte widening when conversion is
// impossible; output is safe for status text and message boxes.
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (required > 0) {
        std::wstring wide(static_cast<std::size_t>(required), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), wide.data(), required);
        return wide;
    }

    std::wstring fallback;
    fallback.reserve(text.size());
    for (const unsigned char ch : text) {
        fallback.push_back(static_cast<wchar_t>(ch));
    }
    return fallback;
}

std::wstring PidListText(const std::vector<DWORD>& pids) {
    std::wstring text;
    for (std::size_t i = 0; i < pids.size(); ++i) {
        if (i != 0) {
            text += L", ";
        }
        text += std::to_wstring(pids[i]);
    }
    return text.empty() ? L"<none>" : text;
}

// WriteClipboardText copies Unicode operation handoff text to the clipboard.
// Input is owner HWND (optional) and text; processing transfers GMEM_MOVEABLE
// memory to the OS clipboard; output reports whether the copy succeeded.
bool WriteClipboardText(HWND owner, const std::wstring& text) {
    if (!::OpenClipboard(owner)) {
        return false;
    }
    ::EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }
    void* target = ::GlobalLock(memory);
    if (!target) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    const bool ok = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!ok) {
        ::GlobalFree(memory);
    }
    ::CloseClipboard();
    return ok;
}

const ProcessSnapshotRow* FindRowByPid(const std::vector<ProcessSnapshotRow>& rows, DWORD pid) {
    const auto it = std::find_if(rows.begin(), rows.end(), [pid](const ProcessSnapshotRow& row) {
        return row.processId == pid;
    });
    return it == rows.end() ? nullptr : &*it;
}

ProcessActionResult FailureResult(const wchar_t* title, const std::vector<DWORD>& pids, const wchar_t* reason) {
    ProcessActionResult result;
    result.success = false;
    result.title = title;
    result.detail = std::wstring(reason) + L"\r\nTarget PID(s): " + PidListText(pids);
    return result;
}

// Win32ErrorText formats the current or supplied Win32 error. Input is the error
// code; processing delegates message formatting to Core; output is display text.
std::wstring Win32ErrorText(const DWORD error) {
    return L"Win32 " + std::to_wstring(error) + L": " + Ksword::Core::LastErrorMessage(error);
}

// Hex32 formats NTSTATUS-style signed LONG values without losing the raw bits.
// Input is an NTSTATUS-compatible value; output is uppercase 8-digit hex text.
std::wstring Hex32(const LONG status) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
           << static_cast<std::uint32_t>(status);
    return stream.str();
}

// Hex64 formats pointer-sized diagnostics without truncating kernel/user
// addresses. Input is a 64-bit value from ArkDriverClient; output is a stable
// uppercase hexadecimal string used only for display.
std::wstring Hex64(const std::uint64_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << value;
    return stream.str();
}

// AsciiLiteralToWide widens short export names or fixed ASCII diagnostics.
// Input is a null-terminated ASCII string; processing widens byte-for-byte;
// output is empty when input is null.
std::wstring AsciiLiteralToWide(const char* text) {
    if (!text) {
        return {};
    }
    std::wstring wide;
    while (*text) {
        wide.push_back(static_cast<wchar_t>(*text));
        ++text;
    }
    return wide;
}

// NtProc resolves one ntdll export by name. Input is an ANSI export name;
// processing uses the already-loaded ntdll module or loads it; output is null
// when the export cannot be found.
FARPROC NtProc(const char* name) {
    if (!name || name[0] == '\0') {
        return nullptr;
    }
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        ntdll = ::LoadLibraryW(L"ntdll.dll");
    }
    return ntdll ? ::GetProcAddress(ntdll, name) : nullptr;
}

// EnableCurrentProcessPrivilege enables one privilege on the current token.
// Input is a privilege name such as SE_DEBUG_NAME; processing adjusts the
// process token; output reports whether Windows accepted and assigned it.
bool EnableCurrentProcessPrivilege(const wchar_t* privilegeName, std::wstring& detail) {
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) {
        detail = L"OpenProcessToken failed: " + Win32ErrorText(::GetLastError());
        return false;
    }
    Ksword::Core::UniqueHandle token(rawToken);

    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        detail = L"LookupPrivilegeValueW failed: " + Win32ErrorText(::GetLastError());
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        detail = L"AdjustTokenPrivileges failed: " + Win32ErrorText(::GetLastError());
        return false;
    }

    const DWORD adjustError = ::GetLastError();
    if (adjustError != ERROR_SUCCESS) {
        detail = L"AdjustTokenPrivileges did not assign privilege: " + Win32ErrorText(adjustError);
        return false;
    }
    detail = std::wstring(privilegeName ? privilegeName : L"<null>") + L" enabled";
    return true;
}

// AppendIoLine records one per-PID operation result. Inputs are a mutable
// details buffer, PID, operation label, success bit and driver/Win32 message;
// processing emits compact multiline diagnostics; no value is returned.
void AppendIoLine(std::wstring& detail, DWORD pid, const wchar_t* operation, bool ok, const std::wstring& message) {
    detail += L"PID " + std::to_wstring(pid) + L" ";
    detail += operation;
    detail += ok ? L": OK" : L": FAIL";
    if (!message.empty()) {
        detail += L" | ";
        detail += message;
    }
    detail += L"\r\n";
}

// AppendIoLine records a global non-PID operation such as clearing hidden marks.
// Inputs mirror the PID overload except there is no target process id.
void AppendIoLine(std::wstring& detail, const wchar_t* operation, bool ok, const std::wstring& message) {
    detail += operation;
    detail += ok ? L": OK" : L": FAIL";
    if (!message.empty()) {
        detail += L" | ";
        detail += message;
    }
    detail += L"\r\n";
}

// IsProtectedSystemPid blocks obviously invalid targets before sending mutating
// process IOCTLs. Input is a PID; output is true for PID 0..4, matching the
// original KswordARK R0 helpers.
bool IsProtectedSystemPid(DWORD pid) {
    return pid == 0 || pid <= 4;
}

// OpenProcessForAction opens a process for a concrete local Win32/NtAPI action.
// Inputs are PID and desired access; processing blocks obvious system PIDs and
// opens the handle; output is an owning handle plus a diagnostic on failure.
Ksword::Core::UniqueHandle OpenProcessForAction(DWORD pid, DWORD access, std::wstring& errorText) {
    if (IsProtectedSystemPid(pid)) {
        errorText = L"protected system PID";
        return Ksword::Core::UniqueHandle();
    }

    HANDLE process = ::OpenProcess(access, FALSE, pid);
    if (!process) {
        errorText = L"OpenProcess failed: " + Win32ErrorText(::GetLastError());
        return Ksword::Core::UniqueHandle();
    }
    return Ksword::Core::UniqueHandle(process);
}

// NtSuspendOrResumeProcess invokes NtSuspendProcess or NtResumeProcess for one
// PID. Inputs are PID and desired direction; processing uses ntdll dynamically;
// output is true on NT_SUCCESS and a diagnostic otherwise.
bool NtSuspendOrResumeProcess(DWORD pid, bool resume, std::wstring& message) {
    const char* exportName = resume ? "NtResumeProcess" : "NtSuspendProcess";
    const FARPROC proc = NtProc(exportName);
    if (!proc) {
        message = AsciiLiteralToWide(exportName) + L" not available";
        return false;
    }

    std::wstring openError;
    Ksword::Core::UniqueHandle process = OpenProcessForAction(pid, kProcessSuspendResumeAccess, openError);
    if (!process.valid()) {
        message = openError;
        return false;
    }

    const LONG status = resume
        ? reinterpret_cast<NtResumeProcessFn>(proc)(process.get())
        : reinterpret_cast<NtSuspendProcessFn>(proc)(process.get());
    if (status >= 0) {
        message = Hex32(status);
        return true;
    }
    message = AsciiLiteralToWide(exportName) + L" failed: " + Hex32(status);
    return false;
}

// SetCriticalFlagForPid sets ProcessBreakOnTermination for one process. Inputs
// are PID and target state; processing enables SeDebugPrivilege best-effort then
// calls NtSetInformationProcess; output reports operation success.
bool SetCriticalFlagForPid(DWORD pid, bool enable, std::wstring& message) {
    std::wstring privilegeDetail;
    (void)EnableCurrentProcessPrivilege(SE_DEBUG_NAME, privilegeDetail);

    const FARPROC proc = NtProc("NtSetInformationProcess");
    if (!proc) {
        message = L"NtSetInformationProcess not available";
        return false;
    }

    std::wstring openError;
    Ksword::Core::UniqueHandle process = OpenProcessForAction(pid, PROCESS_SET_INFORMATION, openError);
    if (!process.valid()) {
        message = openError;
        return false;
    }

    ULONG critical = enable ? 1UL : 0UL;
    const LONG status = reinterpret_cast<NtSetInformationProcessFn>(proc)(
        process.get(),
        kProcessBreakOnTerminationInfoClass,
        &critical,
        static_cast<ULONG>(sizeof(critical)));
    if (status >= 0) {
        message = privilegeDetail.empty() ? Hex32(status) : privilegeDetail + L"; " + Hex32(status);
        return true;
    }
    message = L"NtSetInformationProcess(ProcessBreakOnTermination) failed: " + Hex32(status);
    if (!privilegeDetail.empty()) {
        message += L"; " + privilegeDetail;
    }
    return false;
}

// SetEfficiencyModeForPid toggles Windows process power throttling. Inputs are
// PID and target state; processing calls SetProcessInformation dynamically;
// output reports operation success and a concrete Win32 diagnostic.
bool SetEfficiencyModeForPid(DWORD pid, bool enable, std::wstring& message) {
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const FARPROC proc = kernel32 ? ::GetProcAddress(kernel32, "SetProcessInformation") : nullptr;
    if (!proc) {
        message = L"SetProcessInformation(ProcessPowerThrottling) not available";
        return false;
    }

    std::wstring openError;
    Ksword::Core::UniqueHandle process = OpenProcessForAction(pid, PROCESS_SET_INFORMATION, openError);
    if (!process.valid()) {
        message = openError;
        return false;
    }

    ProcessPowerThrottlingStateNative powerState{};
    powerState.version = kProcessPowerThrottlingCurrentVersion;
    powerState.controlMask = kProcessPowerThrottlingExecutionSpeed;
    powerState.stateMask = enable ? kProcessPowerThrottlingExecutionSpeed : 0UL;
    const BOOL ok = reinterpret_cast<SetProcessInformationFn>(proc)(
        process.get(),
        kProcessPowerThrottlingInfoClass,
        &powerState,
        static_cast<DWORD>(sizeof(powerState)));
    if (ok) {
        message = enable ? L"Efficiency mode enabled" : L"Efficiency mode disabled";
        return true;
    }
    message = L"SetProcessInformation(ProcessPowerThrottling) failed: " + Win32ErrorText(::GetLastError());
    return false;
}

// ProtectionLevelForAction maps the menu PPL commands to the one-byte
// PS_PROTECTION level accepted by IOCTL_KSWORD_ARK_SET_PPL_LEVEL. Input is a
// menu id; output is false when the id is not a PPL command.
bool ProtectionLevelForAction(ProcessActionId actionId, std::uint8_t& levelOut) {
    switch (actionId) {
    case ProcessActionId::R0SetPplNone: levelOut = 0x00; return true;
    case ProcessActionId::R0SetPplAuthenticode: levelOut = 0x11; return true;
    case ProcessActionId::R0SetPplCodeGen: levelOut = 0x21; return true;
    case ProcessActionId::R0SetPplAntimalware: levelOut = 0x31; return true;
    case ProcessActionId::R0SetPplLsa: levelOut = 0x41; return true;
    case ProcessActionId::R0SetPplWindows: levelOut = 0x51; return true;
    case ProcessActionId::R0SetPplWinTcb: levelOut = 0x61; return true;
    default: levelOut = 0; return false;
    }
}

// VisibilityRequestForAction maps R0 process-hide menu ids onto the shared
// KswordArkProcessIoctl visibility action/flag contract. Input is a menu id;
// output is false when another action family should handle the command.
bool VisibilityRequestForAction(ProcessActionId actionId, unsigned long& actionOut, unsigned long& flagsOut) {
    switch (actionId) {
    case ProcessActionId::R0HideUnlinkOnly:
        actionOut = KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE;
        flagsOut = KSWORD_ARK_PROCESS_VISIBILITY_FLAG_UNLINK_ACTIVE_LIST;
        return true;
    case ProcessActionId::R0HidePatchPidOnly:
        actionOut = KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE;
        flagsOut = KSWORD_ARK_PROCESS_VISIBILITY_FLAG_PATCH_UNIQUE_PID;
        return true;
    case ProcessActionId::R0HideLegacyBoth:
        actionOut = KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE;
        flagsOut = KSWORD_ARK_PROCESS_VISIBILITY_FLAG_LEGACY_BOTH;
        return true;
    case ProcessActionId::R0UnhideProcess:
        actionOut = KSWORD_ARK_PROCESS_VISIBILITY_ACTION_UNHIDE;
        flagsOut = 0;
        return true;
    case ProcessActionId::R0ClearHiddenMarks:
        actionOut = KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL;
        flagsOut = 0;
        return true;
    default:
        actionOut = 0;
        flagsOut = 0;
        return false;
    }
}

// SpecialProcessActionForMenu maps BreakOnTermination/APC menu entries onto
// IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS action values. Input is a menu id;
// output is false when the command belongs to another operation family.
bool SpecialProcessActionForMenu(ProcessActionId actionId, unsigned long& actionOut) {
    switch (actionId) {
    case ProcessActionId::R0EnableBreakOnTermination:
        actionOut = KSWORD_ARK_PROCESS_SPECIAL_ACTION_ENABLE_BREAK_ON_TERMINATION;
        return true;
    case ProcessActionId::R0DisableBreakOnTermination:
        actionOut = KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_BREAK_ON_TERMINATION;
        return true;
    case ProcessActionId::R0DisableApcInsertion:
        actionOut = KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_APC_INSERTION;
        return true;
    default:
        actionOut = 0;
        return false;
    }
}

bool SetPriorityForPid(DWORD pid, DWORD priorityClass, std::wstring& detail) {
    HANDLE process = ::OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        detail += L"PID " + std::to_wstring(pid) + L": OpenProcess failed " + std::to_wstring(::GetLastError()) + L"\r\n";
        return false;
    }
    const BOOL ok = ::SetPriorityClass(process, priorityClass);
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(process);
    detail += L"PID " + std::to_wstring(pid) + (ok ? L": SetPriorityClass OK" : L": SetPriorityClass failed ") +
        (ok ? L"" : std::to_wstring(error)) + L"\r\n";
    return ok != FALSE;
}

// KeyboardEnumOk checks shared keyboard enumeration status values. Input is the
// R0 aggregate status; output accepts OK and PARTIAL because partial still gives
// usable rows for the UI.
bool KeyboardEnumOk(std::uint32_t status) {
    return status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK ||
        status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
}

// ExecuteKeyboardHotkeyScan queries R0 win32k hotkey/hook evidence for one
// process. Inputs are the selected PID; processing uses ArkDriverClient only;
// output is a ProcessActionResult suitable for the context menu status dialog.
ProcessActionResult ExecuteKeyboardHotkeyScan(DWORD pid) {
    ProcessActionResult result;
    result.title = L"扫描进程热键";
    const ksword::ark::DriverClient driverClient;

    const ksword::ark::KeyboardHotkeyEnumResult hotkeys = driverClient.enumerateKeyboardHotkeys(
        static_cast<std::uint32_t>(pid),
        KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS,
        2048UL);
    const ksword::ark::KeyboardHookEnumResult hooks = driverClient.enumerateKeyboardHooks(
        static_cast<std::uint32_t>(pid),
        KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS,
        2048UL);

    const bool hotkeyOk = hotkeys.io.ok && KeyboardEnumOk(hotkeys.status);
    const bool hookOk = hooks.io.ok && KeyboardEnumOk(hooks.status);
    result.success = hotkeyOk || hookOk;

    std::wostringstream detail;
    detail << L"PID " << pid << L"\r\n"
           << L"Hotkeys: IO=" << (hotkeys.io.ok ? L"OK" : L"FAIL")
           << L", status=" << hotkeys.status
           << L", total=" << hotkeys.totalCount
           << L", returned=" << hotkeys.returnedCount
           << L", parsed=" << hotkeys.entries.size()
           << L", message=" << Utf8ToWide(hotkeys.io.message) << L"\r\n"
           << L"Hooks: IO=" << (hooks.io.ok ? L"OK" : L"FAIL")
           << L", status=" << hooks.status
           << L", total=" << hooks.totalCount
           << L", returned=" << hooks.returnedCount
           << L", parsed=" << hooks.entries.size()
           << L", message=" << Utf8ToWide(hooks.io.message) << L"\r\n";

    const std::size_t hotkeyLimit = std::min<std::size_t>(hotkeys.entries.size(), 16U);
    for (std::size_t i = 0; i < hotkeyLimit; ++i) {
        const ksword::ark::KeyboardHotkeyEntry& row = hotkeys.entries[i];
        detail << L"Hotkey[" << i << L"] vk=0x" << std::hex << std::uppercase << row.virtualKey
               << L", modifiers=0x" << row.modifiers
               << L", tid=" << std::dec << row.threadId
               << L", object=" << Hex64(row.hotkeyObject)
               << L", detail=" << row.detail << L"\r\n";
    }

    const std::size_t hookLimit = std::min<std::size_t>(hooks.entries.size(), 16U);
    for (std::size_t i = 0; i < hookLimit; ++i) {
        const ksword::ark::KeyboardHookEntry& row = hooks.entries[i];
        detail << L"Hook[" << i << L"] type=" << row.hookType
               << L", scope=" << row.hookScope
               << L", tid=" << row.threadId
               << L", proc=" << Hex64(row.procedureAddress)
               << L", detail=" << row.detail << std::dec << L"\r\n";
    }

    result.detail = detail.str();
    return result;
}

// ExecuteLocalProcessAction applies one local Win32/NtAPI action to all selected
// PIDs. Inputs are action id and target list; processing dispatches to a concrete
// helper; output aggregates per-PID status for the UI.
ProcessActionResult ExecuteLocalProcessAction(ProcessActionId actionId, const std::vector<DWORD>& selectedPids) {
    ProcessActionResult result;
    result.success = true;
    bool handled = true;
    const wchar_t* operation = L"";
    switch (actionId) {
    case ProcessActionId::SuspendProcess:
        result.title = L"挂起进程";
        operation = L"NtSuspendProcess";
        break;
    case ProcessActionId::ResumeProcess:
        result.title = L"恢复进程";
        operation = L"NtResumeProcess";
        break;
    case ProcessActionId::EnableEfficiencyMode:
        result.title = L"开启效率模式";
        operation = L"Efficiency on";
        break;
    case ProcessActionId::DisableEfficiencyMode:
        result.title = L"关闭效率模式";
        operation = L"Efficiency off";
        break;
    case ProcessActionId::SetCriticalProcess:
        result.title = L"设为关键进程";
        operation = L"Critical on";
        break;
    case ProcessActionId::ClearCriticalProcess:
        result.title = L"取消关键进程";
        operation = L"Critical off";
        break;
    default:
        handled = false;
        break;
    }

    if (!handled) {
        return FailureResult(L"进程动作", selectedPids, L"未知本地进程动作。");
    }

    for (DWORD pid : selectedPids) {
        std::wstring message;
        bool ok = false;
        switch (actionId) {
        case ProcessActionId::SuspendProcess:
            ok = NtSuspendOrResumeProcess(pid, false, message);
            break;
        case ProcessActionId::ResumeProcess:
            ok = NtSuspendOrResumeProcess(pid, true, message);
            break;
        case ProcessActionId::EnableEfficiencyMode:
            ok = SetEfficiencyModeForPid(pid, true, message);
            break;
        case ProcessActionId::DisableEfficiencyMode:
            ok = SetEfficiencyModeForPid(pid, false, message);
            break;
        case ProcessActionId::SetCriticalProcess:
            ok = SetCriticalFlagForPid(pid, true, message);
            break;
        case ProcessActionId::ClearCriticalProcess:
            ok = SetCriticalFlagForPid(pid, false, message);
            break;
        default:
            message = L"unknown action";
            ok = false;
            break;
        }
        AppendIoLine(result.detail, pid, operation, ok, message);
        result.success = result.success && ok;
    }
    return result;
}

// ExecutePplRefresh queries the R0 process enumeration table and extracts the
// selected PIDs' protection bytes. Inputs are selected PIDs; processing uses
// ArkDriverClient enumerateProcesses; output is an aggregated diagnostic result.
ProcessActionResult ExecutePplRefresh(const std::vector<DWORD>& selectedPids) {
    ProcessActionResult result;
    result.title = L"手动刷新PPL保护级别";
    const ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessEnumResult query = driverClient.enumerateProcesses(0);
    if (!query.io.ok) {
        result.success = false;
        result.detail = L"R0 process enumeration failed: " + Utf8ToWide(query.io.message);
        return result;
    }

    result.success = true;
    for (DWORD pid : selectedPids) {
        const auto it = std::find_if(query.entries.begin(), query.entries.end(), [pid](const ksword::ark::ProcessEntry& row) {
            return row.processId == static_cast<std::uint32_t>(pid);
        });
        if (it == query.entries.end()) {
            AppendIoLine(result.detail, pid, L"PPL refresh", false, L"PID not returned by R0 enumeration");
            result.success = false;
            continue;
        }

        std::wostringstream line;
        line << L"protection=0x" << std::hex << std::uppercase << static_cast<unsigned int>(it->protection)
             << L", signature=0x" << static_cast<unsigned int>(it->signatureLevel)
             << L", sectionSignature=0x" << static_cast<unsigned int>(it->sectionSignatureLevel)
             << L", fieldFlags=0x" << it->fieldFlags
             << L", r0Status=" << std::dec << it->r0Status;
        const bool hasProtection = (it->fieldFlags & KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT) != 0;
        AppendIoLine(result.detail, pid, L"PPL refresh", hasProtection, line.str());
        result.success = result.success && hasProtection;
    }
    return result;
}
} // namespace

ProcessActionResult ExecuteProcessAction(
    ProcessActionId actionId,
    const std::vector<DWORD>& selectedPids,
    const std::vector<ProcessSnapshotRow>& snapshotRows) {
    if (selectedPids.empty()) {
        return FailureResult(L"进程动作", selectedPids, L"没有选中进程。");
    }

    const DWORD priorityClass = PriorityClassForAction(actionId);
    if (priorityClass != 0) {
        ProcessActionResult result;
        result.title = L"设置进程优先级";
        result.success = true;
        for (DWORD pid : selectedPids) {
            if (!SetPriorityForPid(pid, priorityClass, result.detail)) {
                result.success = false;
            }
        }
        return result;
    }

    if (actionId == ProcessActionId::OpenFolder) {
        const ProcessSnapshotRow* row = FindRowByPid(snapshotRows, selectedPids.front());
        if (!row || row->imagePath.empty()) {
            return FailureResult(L"打开所在目录", selectedPids, L"选中进程的映像路径不可用。");
        }
        const std::wstring args = L"/select,\"" + row->imagePath + L"\"";
        const HINSTANCE shellResult = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        ProcessActionResult result;
        result.title = L"打开所在目录";
        result.success = reinterpret_cast<INT_PTR>(shellResult) > 32;
        result.detail = result.success ? L"Explorer launch requested." : L"ShellExecuteW failed.";
        return result;
    }

    if (actionId == ProcessActionId::TerminateProcess) {
        ProcessActionResult result;
        result.title = L"结束进程";
        result.success = true;
        for (DWORD pid : selectedPids) {
            if (IsProtectedSystemPid(pid)) {
                AppendIoLine(result.detail, pid, L"TerminateProcess", false, L"protected system PID");
                result.success = false;
                continue;
            }
            HANDLE process = ::OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) {
                AppendIoLine(result.detail, pid, L"TerminateProcess", false, L"OpenProcess error " + std::to_wstring(::GetLastError()));
                result.success = false;
                continue;
            }
            const BOOL ok = ::TerminateProcess(process, static_cast<UINT>(0xC0000005u));
            const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(process);
            AppendIoLine(result.detail, pid, L"TerminateProcess", ok != FALSE, ok ? L"" : L"Win32 error " + std::to_wstring(error));
            result.success = result.success && ok != FALSE;
        }
        return result;
    }

    if (actionId == ProcessActionId::R0TerminateProcess) {
        ProcessActionResult result;
        result.title = L"R0结束进程";
        result.success = true;
        const ksword::ark::DriverClient driverClient;
        for (DWORD pid : selectedPids) {
            if (IsProtectedSystemPid(pid)) {
                AppendIoLine(result.detail, pid, L"R0 terminate", false, L"protected system PID");
                result.success = false;
                continue;
            }
            const ksword::ark::IoResult io = driverClient.terminateProcess(static_cast<std::uint32_t>(pid), static_cast<long>(0xC0000005u));
            AppendIoLine(result.detail, pid, L"R0 terminate", io.ok, Utf8ToWide(io.message));
            result.success = result.success && io.ok;
        }
        return result;
    }

    if (actionId == ProcessActionId::R0SuspendProcess) {
        ProcessActionResult result;
        result.title = L"R0挂起进程";
        result.success = true;
        const ksword::ark::DriverClient driverClient;
        for (DWORD pid : selectedPids) {
            if (IsProtectedSystemPid(pid)) {
                AppendIoLine(result.detail, pid, L"R0 suspend", false, L"protected system PID");
                result.success = false;
                continue;
            }
            const ksword::ark::IoResult io = driverClient.suspendProcess(static_cast<std::uint32_t>(pid));
            AppendIoLine(result.detail, pid, L"R0 suspend", io.ok, Utf8ToWide(io.message));
            result.success = result.success && io.ok;
        }
        return result;
    }

    std::uint8_t protectionLevel = 0;
    if (ProtectionLevelForAction(actionId, protectionLevel)) {
        ProcessActionResult result;
        result.title = L"R0设置PPL层级";
        result.success = true;
        const ksword::ark::DriverClient driverClient;
        for (DWORD pid : selectedPids) {
            if (IsProtectedSystemPid(pid)) {
                AppendIoLine(result.detail, pid, L"set PPL", false, L"protected system PID");
                result.success = false;
                continue;
            }
            const ksword::ark::IoResult io = driverClient.setProcessProtection(static_cast<std::uint32_t>(pid), protectionLevel);
            AppendIoLine(result.detail, pid, L"set PPL", io.ok, Utf8ToWide(io.message));
            result.success = result.success && io.ok;
        }
        return result;
    }

    unsigned long visibilityAction = 0;
    unsigned long visibilityFlags = 0;
    if (VisibilityRequestForAction(actionId, visibilityAction, visibilityFlags)) {
        ProcessActionResult result;
        result.title = L"R0进程可见性";
        result.success = true;
        const ksword::ark::DriverClient driverClient;
        if (actionId == ProcessActionId::R0ClearHiddenMarks) {
            const ksword::ark::ProcessVisibilityResult io = driverClient.setProcessVisibility(0, visibilityAction, visibilityFlags);
            const bool ok = io.io.ok && io.lastStatus >= 0 && io.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED;
            AppendIoLine(result.detail, L"R0 clear hidden marks", ok, Utf8ToWide(io.io.message));
            result.success = ok;
            return result;
        }
        for (DWORD pid : selectedPids) {
            if (visibilityAction == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE && IsProtectedSystemPid(pid)) {
                AppendIoLine(result.detail, pid, L"visibility", false, L"protected system PID");
                result.success = false;
                continue;
            }
            const ksword::ark::ProcessVisibilityResult io = driverClient.setProcessVisibility(static_cast<std::uint32_t>(pid), visibilityAction, visibilityFlags);
            const bool ok = io.io.ok && io.lastStatus >= 0 &&
                (io.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN ||
                 io.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_VISIBLE ||
                 io.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED);
            AppendIoLine(result.detail, pid, L"visibility", ok, Utf8ToWide(io.io.message));
            result.success = result.success && ok;
        }
        return result;
    }

    unsigned long specialAction = 0;
    if (SpecialProcessActionForMenu(actionId, specialAction)) {
        ProcessActionResult result;
        result.title = L"R0进程特殊标志";
        result.success = true;
        const ksword::ark::DriverClient driverClient;
        for (DWORD pid : selectedPids) {
            if (IsProtectedSystemPid(pid)) {
                AppendIoLine(result.detail, pid, L"special flags", false, L"protected system PID");
                result.success = false;
                continue;
            }
            const ksword::ark::ProcessSpecialFlagsResult io = driverClient.setProcessSpecialFlags(static_cast<std::uint32_t>(pid), specialAction);
            const bool ok = io.io.ok && io.lastStatus >= 0 && io.status == KSWORD_ARK_PROCESS_SPECIAL_STATUS_APPLIED;
            AppendIoLine(result.detail, pid, L"special flags", ok, Utf8ToWide(io.io.message));
            result.success = result.success && ok;
        }
        return result;
    }

    if (actionId == ProcessActionId::R0DkomRemoveFromCidTable) {
        ProcessActionResult result;
        result.title = L"R0 DKOM从PspCidTable删除";
        result.success = true;
        const ksword::ark::DriverClient driverClient;
        for (DWORD pid : selectedPids) {
            if (IsProtectedSystemPid(pid)) {
                AppendIoLine(result.detail, pid, L"DKOM CID remove", false, L"protected system PID");
                result.success = false;
                continue;
            }
            const ksword::ark::ProcessDkomResult io = driverClient.dkomProcess(static_cast<std::uint32_t>(pid), KSWORD_ARK_PROCESS_DKOM_ACTION_REMOVE_FROM_PSP_CID_TABLE);
            const bool ok = io.io.ok && io.lastStatus >= 0 && io.status == KSWORD_ARK_PROCESS_DKOM_STATUS_REMOVED && io.removedEntries > 0;
            AppendIoLine(result.detail, pid, L"DKOM CID remove", ok, Utf8ToWide(io.io.message));
            result.success = result.success && ok;
        }
        return result;
    }

    switch (actionId) {
    case ProcessActionId::SuspendProcess:
    case ProcessActionId::ResumeProcess:
    case ProcessActionId::EnableEfficiencyMode:
    case ProcessActionId::DisableEfficiencyMode:
    case ProcessActionId::SetCriticalProcess:
    case ProcessActionId::ClearCriticalProcess:
        return ExecuteLocalProcessAction(actionId, selectedPids);
    case ProcessActionId::RefreshPplProtectionLevel:
        return ExecutePplRefresh(selectedPids);
    case ProcessActionId::OpenMemoryOperation: {
        ProcessActionResult result;
        result.title = L"复制到内存读写页输入";
        const std::wstring pidText = PidListText(selectedPids);
        result.success = WriteClipboardText(nullptr, pidText);
        result.detail = result.success
            ? L"已复制 PID 列表，可粘贴到驱动内存读写页的目标 PID 输入框: " + pidText
            : L"复制 PID 列表到剪贴板失败: " + Win32ErrorText(::GetLastError());
        return result;
    }
    case ProcessActionId::ScanHotkeys:
        if (selectedPids.size() != 1) {
            return FailureResult(L"扫描进程热键", selectedPids, L"扫描进程热键需要单选一个进程。");
        }
        return ExecuteKeyboardHotkeyScan(selectedPids.front());
    case ProcessActionId::OpenDetails:
        return FailureResult(L"进程详细信息", selectedPids, L"该动作由进程列表窗口直接打开详细信息页。");
    default:
        return FailureResult(L"进程动作", selectedPids, L"未知进程动作。");
    }
}

DWORD PriorityClassForAction(ProcessActionId actionId) {
    switch (actionId) {
    case ProcessActionId::SetPriorityIdle: return IDLE_PRIORITY_CLASS;
    case ProcessActionId::SetPriorityBelowNormal: return BELOW_NORMAL_PRIORITY_CLASS;
    case ProcessActionId::SetPriorityNormal: return NORMAL_PRIORITY_CLASS;
    case ProcessActionId::SetPriorityAboveNormal: return ABOVE_NORMAL_PRIORITY_CLASS;
    case ProcessActionId::SetPriorityHigh: return HIGH_PRIORITY_CLASS;
    case ProcessActionId::SetPriorityRealtime: return REALTIME_PRIORITY_CLASS;
    default: return 0;
    }
}

} // namespace Ksword::Features::Process
