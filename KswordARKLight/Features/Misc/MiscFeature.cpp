#include "MiscFeature.h"

#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Ksword::Features::Misc {
namespace {

constexpr wchar_t kMiscHostClass[] = L"KswordARKLight.MiscFeaturePage";
constexpr wchar_t kMiscAuditViewClass[] = L"KswordARKLight.MiscAuditView";
constexpr int kTabId = 69101;
constexpr int kListId = 69102;
constexpr int kRefreshButtonId = 69103;
constexpr int kCopyButtonId = 69104;
constexpr int kCiTabIndex = 0;
constexpr int kVbsTabIndex = 1;
constexpr int kHyperVTabIndex = 2;
constexpr int kAppLockerTabIndex = 3;
constexpr int kAuxiliaryTabIndex = 4;
constexpr int kHeaderHeight = 34;
constexpr int kGap = 6;
constexpr UINT kMenuRefresh = 69201;
constexpr UINT kMenuCopyRow = 69202;
constexpr UINT kMenuCopyAll = 69203;

enum class MiscAuditPageId {
    CodeIntegrity,
    VbsHvciSkci,
    HyperV,
    AppLocker,
    BamAhcache,
};

// MiscAuditRow is the value-only evidence record rendered into each ListView.
// Inputs come from read-only R3 commands, registry reads, service status queries
// and ArkDriverClient diagnostics. Processing stores only display strings;
// output is consumed by PopulateAuditList and clipboard export helpers.
struct MiscAuditRow {
    std::wstring category;
    std::wstring item;
    std::wstring state;
    std::wstring source;
    std::wstring risk;
    std::wstring detail;
};

// CommandResult captures bounded stdout/stderr from an external read-only query.
// Inputs are filled by RunCaptureCommand; processing later turns exit code and
// captured text into UI rows. No handles are retained after the command returns.
struct CommandResult {
    bool started = false;
    DWORD exitCode = 0;
    DWORD win32Error = 0;
    std::wstring output;
    std::wstring errorText;
};

// MiscAuditViewState owns one tab page and its row snapshot. Inputs arrive from
// Win32 messages; processing refreshes only the local page and never performs
// patch/delete/bypass/remove/unlink operations. There is no shared global state.
struct MiscAuditViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND copyButton = nullptr;
    HWND list = nullptr;
    MiscAuditPageId pageId = MiscAuditPageId::CodeIntegrity;
    std::wstring title;
    std::wstring statusText;
    std::vector<MiscAuditRow> rows;
};

// MiscFeaturePageState owns the tab host and retained child pages. Inputs arrive
// through the host window procedure; processing switches visibility only, so each
// tab keeps its last snapshot and diagnostics until explicitly refreshed.
struct MiscFeaturePageState {
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HWND ciView = nullptr;
    HWND vbsView = nullptr;
    HWND hypervView = nullptr;
    HWND appLockerView = nullptr;
    HWND auxiliaryView = nullptr;
    int currentTab = kCiTabIndex;
};

// Width returns a non-negative RECT width. Input is a Win32 RECT; output is the
// client width used by layout code.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative RECT height. Input is a Win32 RECT; output is
// the client height used by layout code.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// StateFromAuditView reads the MiscAuditViewState pointer from a child HWND.
// Input is the page HWND; output is null before WM_NCCREATE or after destroy.
MiscAuditViewState* StateFromAuditView(HWND hwnd) {
    return reinterpret_cast<MiscAuditViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// StateFromHost reads the MiscFeaturePageState pointer from the host HWND. Input
// is the host HWND; output is null before creation or after destruction.
MiscFeaturePageState* StateFromHost(HWND hwnd) {
    return reinterpret_cast<MiscFeaturePageState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// TrimCopy removes leading and trailing whitespace from display command output.
// Input is any captured string; processing does not alter interior newlines;
// output is a new trimmed string.
std::wstring TrimCopy(const std::wstring& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::iswspace(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::iswspace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// CollapseWhitespace converts multi-line command output into compact cell text.
// Input is a captured string; processing replaces CR/LF/TAB runs with spaces;
// output is bounded by the caller when inserted into the ListView.
std::wstring CollapseWhitespace(const std::wstring& text) {
    std::wstring out;
    bool inWhitespace = false;
    for (wchar_t ch : text) {
        if (std::iswspace(ch)) {
            if (!inWhitespace && !out.empty()) {
                out.push_back(L' ');
            }
            inWhitespace = true;
            continue;
        }
        inWhitespace = false;
        out.push_back(ch);
    }
    return TrimCopy(out);
}

// QuotePowerShellCommand wraps a script body in a PowerShell script block.
// Input is one read-only command body; processing keeps the body executable by
// powershell.exe while avoiding cmd.exe parsing; output is used only locally.
std::wstring QuotePowerShellCommand(const std::wstring& text) {
    return L"\"& { " + text + L" }\"";
}

// GetLastErrorText formats a Win32 error. Input is a DWORD error code; output is
// a readable message with the numeric value retained for diagnostics.
std::wstring GetLastErrorText(const DWORD error) {
    if (error == 0) {
        return L"0";
    }
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD chars = ::FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = L"Win32=" + std::to_wstring(error);
    if (chars != 0 && message) {
        result += L" (" + TrimCopy(message) + L")";
    }
    if (message) {
        ::LocalFree(message);
    }
    return result;
}

// AppendRow appends one evidence row to a vector. Inputs are display fields and
// optional detail; processing copies them into the row list; no value is returned.
void AppendRow(
    std::vector<MiscAuditRow>& rows,
    std::wstring category,
    std::wstring item,
    std::wstring state,
    std::wstring source,
    std::wstring risk,
    std::wstring detail) {
    rows.push_back(MiscAuditRow{
        std::move(category),
        std::move(item),
        std::move(state),
        std::move(source),
        std::move(risk),
        std::move(detail),
    });
}

// RunCaptureCommand starts a bounded child process and captures stdout/stderr.
// Inputs are the full command line and timeout in milliseconds; processing uses
// anonymous pipes, waits without injecting input, and terminates only the helper
// process if it exceeds the local UI budget; output includes failure reason.
CommandResult RunCaptureCommand(const std::wstring& commandLine, const DWORD timeoutMs = 12000) {
    CommandResult result;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &security, 0)) {
        result.win32Error = ::GetLastError();
        result.errorText = L"CreatePipe failed: " + GetLastErrorText(result.win32Error);
        return result;
    }
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = commandLine;
    const BOOL created = ::CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);
    ::CloseHandle(writePipe);

    if (!created) {
        result.win32Error = ::GetLastError();
        result.errorText = L"CreateProcessW failed: " + GetLastErrorText(result.win32Error);
        ::CloseHandle(readPipe);
        return result;
    }

    result.started = true;
    std::string bytes;
    std::array<char, 4096> buffer{};
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    bool processFinished = false;
    bool outputLimitHit = false;
    while (!processFinished) {
        DWORD available = 0;
        while (!outputLimitHit &&
            ::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) &&
            available > 0) {
            DWORD read = 0;
            const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!::ReadFile(readPipe, buffer.data(), chunk, &read, nullptr) || read == 0) {
                break;
            }
            bytes.append(buffer.data(), buffer.data() + read);
            if (bytes.size() > 128 * 1024) {
                result.errorText += L" 输出超过 128KB，已截断。";
                outputLimitHit = true;
                break;
            }
        }

        const DWORD wait = ::WaitForSingleObject(process.hProcess, 25);
        if (wait == WAIT_OBJECT_0) {
            processFinished = true;
        } else if (wait == WAIT_FAILED) {
            result.win32Error = ::GetLastError();
            result.errorText = L"WaitForSingleObject failed: " + GetLastErrorText(result.win32Error);
            processFinished = true;
        } else if (::GetTickCount64() >= deadline) {
            ::TerminateProcess(process.hProcess, 258);
            result.exitCode = 258;
            result.errorText = L"查询超时，已停止本地只读辅助进程。";
            processFinished = true;
        }
    }

    if (!outputLimitHit) {
        DWORD available = 0;
        while (::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!::ReadFile(readPipe, buffer.data(), chunk, &read, nullptr) || read == 0) {
                break;
            }
            bytes.append(buffer.data(), buffer.data() + read);
            if (bytes.size() > 128 * 1024) {
                result.errorText += L" 输出超过 128KB，已截断。";
                break;
            }
        }
    }

    DWORD exitCode = 0;
    if (::GetExitCodeProcess(process.hProcess, &exitCode)) {
        result.exitCode = exitCode;
    }
    if (result.exitCode == STILL_ACTIVE) {
        result.exitCode = 258;
    }

    if (!bytes.empty()) {
        const int required = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (required > 0) {
            result.output.assign(static_cast<std::size_t>(required), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), result.output.data(), required);
        } else {
            const int fallback = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (fallback > 0) {
                result.output.assign(static_cast<std::size_t>(fallback), L'\0');
                ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), result.output.data(), fallback);
            }
        }
    }

    ::CloseHandle(readPipe);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return result;
}

// PowerShellCommand builds a hidden, non-profile PowerShell invocation for local
// read-only posture queries. Input is a script body; output is a command line for
// RunCaptureCommand. The script is expected to avoid mutation cmdlets.
std::wstring PowerShellCommand(const std::wstring& script) {
    return L"powershell.exe -NoLogo -NoProfile -NonInteractive -Command " +
        QuotePowerShellCommand(script);
}

// RunPowerShellScalar executes one PowerShell query and returns compact output.
// Inputs are a script body and timeout; processing captures stdout/stderr and
// keeps the failure reason if PowerShell/WMI is unavailable; output is a command
// result suitable for AddCommandRow.
CommandResult RunPowerShellScalar(const std::wstring& script, const DWORD timeoutMs = 12000) {
    return RunCaptureCommand(PowerShellCommand(script), timeoutMs);
}

// AddCommandRow converts a command result into one evidence row. Inputs identify
// the evidence and command source; processing stores either output or an explicit
// failure reason; no value is returned.
void AddCommandRow(
    std::vector<MiscAuditRow>& rows,
    const std::wstring& category,
    const std::wstring& item,
    const std::wstring& source,
    const CommandResult& command,
    const std::wstring& cleanHint = L"已查询") {
    const std::wstring output = CollapseWhitespace(command.output);
    if (command.started && command.exitCode == 0 && !output.empty()) {
        AppendRow(rows, category, item, cleanHint, source, L"Info", output);
        return;
    }

    std::wstring detail = command.errorText;
    if (!output.empty()) {
        if (!detail.empty()) {
            detail += L" ";
        }
        detail += output;
    }
    if (detail.empty()) {
        detail = command.started
            ? (L"查询进程退出码=" + std::to_wstring(command.exitCode))
            : (L"查询未启动，" + GetLastErrorText(command.win32Error));
    }
    AppendRow(rows, category, item, L"Unavailable", source, L"Unknown", detail);
}

// QueryRegistryValueString reads one HKLM value without writing registry state.
// Inputs are a subkey and value name; processing uses RegOpenKeyEx/RegQueryValueEx
// with KEY_READ only; output is true when a display value was extracted.
bool QueryRegistryValueString(const std::wstring& subKey, const std::wstring& valueName, std::wstring& valueOut, std::wstring& errorOut) {
    HKEY key = nullptr;
    const LONG open = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (open != ERROR_SUCCESS) {
        errorOut = L"RegOpenKeyExW failed: " + GetLastErrorText(static_cast<DWORD>(open));
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LONG query = ::RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &bytes);
    if (query != ERROR_SUCCESS) {
        ::RegCloseKey(key);
        errorOut = L"RegQueryValueExW(size) failed: " + GetLastErrorText(static_cast<DWORD>(query));
        return false;
    }

    std::vector<unsigned char> data(std::max<DWORD>(bytes, sizeof(wchar_t)) + sizeof(wchar_t), 0);
    query = ::RegQueryValueExW(key, valueName.c_str(), nullptr, &type, data.data(), &bytes);
    ::RegCloseKey(key);
    if (query != ERROR_SUCCESS) {
        errorOut = L"RegQueryValueExW(data) failed: " + GetLastErrorText(static_cast<DWORD>(query));
        return false;
    }

    if (type == REG_DWORD && bytes >= sizeof(DWORD)) {
        DWORD value = 0;
        std::memcpy(&value, data.data(), sizeof(value));
        valueOut = std::to_wstring(value) + L" (0x";
        std::wostringstream stream;
        stream << std::hex << std::uppercase << value;
        valueOut += stream.str() + L")";
        return true;
    }
    if ((type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t)) {
        valueOut.assign(reinterpret_cast<const wchar_t*>(data.data()));
        return true;
    }
    if (type == REG_MULTI_SZ && bytes >= sizeof(wchar_t)) {
        const wchar_t* multi = reinterpret_cast<const wchar_t*>(data.data());
        const std::size_t chars = bytes / sizeof(wchar_t);
        std::wstring joined;
        std::size_t offset = 0;
        while (offset < chars && multi[offset] != L'\0') {
            std::wstring part = &multi[offset];
            if (!joined.empty()) {
                joined += L"; ";
            }
            joined += part;
            offset += part.size() + 1;
        }
        valueOut = joined;
        return true;
    }

    valueOut = L"Type=" + std::to_wstring(type) + L", Bytes=" + std::to_wstring(bytes);
    return true;
}

// AddRegistryRow appends one read-only registry evidence row. Inputs are HKLM
// path/value labels; processing never writes or creates keys; no value is returned.
void AddRegistryRow(
    std::vector<MiscAuditRow>& rows,
    const std::wstring& category,
    const std::wstring& item,
    const std::wstring& subKey,
    const std::wstring& valueName) {
    std::wstring value;
    std::wstring error;
    if (QueryRegistryValueString(subKey, valueName, value, error)) {
        AppendRow(rows, category, item, L"已查询", L"Registry HKLM", L"Info", value);
    } else {
        AppendRow(rows, category, item, L"Unavailable", L"Registry HKLM", L"Unknown", error);
    }
}

// QueryServiceStatusText reads one service/driver status with the Service Control
// Manager using query access only. Inputs are a service name; output is true with
// status text or false with a failure reason.
bool QueryServiceStatusText(const std::wstring& serviceName, std::wstring& statusOut, std::wstring& errorOut) {
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        errorOut = L"OpenSCManagerW failed: " + GetLastErrorText(::GetLastError());
        return false;
    }
    SC_HANDLE service = ::OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!service) {
        const DWORD error = ::GetLastError();
        ::CloseServiceHandle(scm);
        errorOut = L"OpenServiceW failed: " + GetLastErrorText(error);
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)) {
        const DWORD error = ::GetLastError();
        ::CloseServiceHandle(service);
        ::CloseServiceHandle(scm);
        errorOut = L"QueryServiceStatusEx failed: " + GetLastErrorText(error);
        return false;
    }

    const wchar_t* stateText = L"Unknown";
    switch (status.dwCurrentState) {
    case SERVICE_STOPPED: stateText = L"Stopped"; break;
    case SERVICE_START_PENDING: stateText = L"StartPending"; break;
    case SERVICE_STOP_PENDING: stateText = L"StopPending"; break;
    case SERVICE_RUNNING: stateText = L"Running"; break;
    case SERVICE_CONTINUE_PENDING: stateText = L"ContinuePending"; break;
    case SERVICE_PAUSE_PENDING: stateText = L"PausePending"; break;
    case SERVICE_PAUSED: stateText = L"Paused"; break;
    default: break;
    }
    statusOut = stateText;
    statusOut += L"; Type=0x";
    std::wostringstream stream;
    stream << std::hex << std::uppercase << status.dwServiceType;
    statusOut += stream.str();
    if (status.dwProcessId != 0) {
        statusOut += L"; PID=" + std::to_wstring(status.dwProcessId);
    }

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(scm);
    return true;
}

// AddServiceRow appends one SCM status row. Inputs are a display category/item
// and SCM service name; processing uses query-only SCM handles; no return value.
void AddServiceRow(
    std::vector<MiscAuditRow>& rows,
    const std::wstring& category,
    const std::wstring& item,
    const std::wstring& serviceName) {
    std::wstring status;
    std::wstring error;
    if (QueryServiceStatusText(serviceName, status, error)) {
        const bool running = status.find(L"Running") != std::wstring::npos;
        AppendRow(rows, category, item, running ? L"Present" : L"Not running", L"SCM query", running ? L"Info" : L"Unknown", serviceName + L": " + status);
    } else {
        AppendRow(rows, category, item, L"Unavailable", L"SCM query", L"Unknown", serviceName + L": " + error);
    }
}

// AddDriverCapabilityRow queries existing ArkDriverClient capability status as
// the only R0-facing check in this module. Inputs are a row list and scope label;
// processing calls the existing wrapper and never opens DeviceIoControl directly;
// no value is returned.
void AddDriverCapabilityRow(std::vector<MiscAuditRow>& rows, const std::wstring& scope) {
    const ksword::ark::DriverClient client;
    const ksword::ark::DriverCapabilitiesQueryResult capability = client.queryDriverCapabilities();
    if (capability.io.ok) {
        std::wostringstream detail;
        detail << L"Protocol=" << capability.driverProtocolVersion
               << L"; StatusFlags=0x" << std::hex << std::uppercase << capability.statusFlags
               << L"; DynData=0x" << capability.dynDataStatusFlags
               << L"; Returned=" << std::dec << capability.returnedFeatureCount << L"/" << capability.totalFeatureCount;
        AppendRow(rows, scope, L"KswordARK R0 capability", L"Online", L"ArkDriverClient::queryDriverCapabilities", L"Info", detail.str());
    } else {
        std::wstring detail = L"R0 能力查询失败：Win32=" + std::to_wstring(capability.io.win32Error) + L"; " +
            std::wstring(capability.io.message.begin(), capability.io.message.end());
        AppendRow(rows, scope, L"KswordARK R0 capability", L"Unavailable", L"ArkDriverClient::queryDriverCapabilities", L"Unknown", detail);
    }
}

// AddSecurityAuditRows 调用新增 ArkDriverClient 安全审计 wrapper。
// 输入：目标 rows 和页面 scope。
// 处理：按当前页面追加 Security/DriverTrust/HyperV/AppControl 的只读摘要。
// 返回：无返回值；所有失败原因保留在详情列，不裸 DeviceIoControl。
void AddSecurityAuditRows(std::vector<MiscAuditRow>& rows, const std::wstring& scope) {
    const ksword::ark::DriverClient client;
    const auto security = client.querySecurityStatus();
    {
        std::wostringstream detail;
        detail << L"Win32=" << security.io.win32Error
               << L"; QueryStatus=0x" << std::hex << std::uppercase << static_cast<unsigned long>(security.response.queryStatus)
               << L"; CIOptions=0x" << security.response.codeIntegrityOptions
               << L"; SecureBoot=" << std::dec << security.response.secureBootEnabled
               << L"; VBS=" << security.response.vbsPresent
               << L"; HVCI=" << security.response.hvciKmciEnabled
               << L"; TestSigning=" << security.response.testSigningEnabled;
        AppendRow(rows, scope, L"R0 SecurityStatus", security.io.ok ? L"OK" : (security.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::querySecurityStatus", security.io.ok ? L"Info" : L"Unknown", detail.str());
    }

    const auto trust = client.queryDriverTrustView();
    {
        std::wostringstream detail;
        detail << L"Win32=" << trust.io.win32Error
               << L"; total=" << trust.totalCount
               << L"; returned=" << trust.returnedCount
               << L"; truncated=" << trust.truncated
               << L"; moduleStatus=0x" << std::hex << std::uppercase << static_cast<unsigned long>(trust.moduleQueryStatus);
        AppendRow(rows, scope, L"R0 DriverTrustView", trust.io.ok ? L"OK" : (trust.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::queryDriverTrustView", trust.truncated ? L"Partial" : L"Info", detail.str());
    }

    const auto hyperv = client.queryHyperVSummary();
    {
        std::wostringstream detail;
        detail << L"Win32=" << hyperv.io.win32Error
               << L"; Hypervisor=" << hyperv.response.hypervisorPresent
               << L"; VMBus=" << hyperv.response.vmbusStatus
               << L"; vSwitch=" << hyperv.response.vSwitchStatus
               << L"; vPCI=" << hyperv.response.vPciStatus
               << L"; vendor=" << hyperv.response.hypervisorVendor;
        AppendRow(rows, scope, L"R0 HyperVSummary", hyperv.io.ok ? L"OK" : (hyperv.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::queryHyperVSummary", hyperv.io.ok ? L"Info" : L"Unknown", detail.str());
    }

    const auto appControl = client.queryAppControlStatus();
    {
        std::wostringstream detail;
        detail << L"Win32=" << appControl.io.win32Error
               << L"; AppID=" << appControl.response.appidStatus
               << L"; AppLockerFilter=" << appControl.response.appLockerFilterStatus
               << L"; mssecflt=" << appControl.response.mssecfltStatus
               << L"; BAM=" << appControl.response.bamStatus
               << L"; owner=" << appControl.response.appLockerOwnerModule;
        AppendRow(rows, scope, L"R0 AppControlStatus", appControl.io.ok ? L"OK" : (appControl.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::queryAppControlStatus", appControl.io.ok ? L"Info" : L"Unknown", detail.str());
    }
}

// CollectCodeIntegrityRows gathers CI/WDAC posture evidence through documented
// R3 queries and registry reads. There is no input; output is a row vector for
// the Code Integrity / WDAC tab.
std::vector<MiscAuditRow> CollectCodeIntegrityRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"Code Integrity / WDAC");
    AddSecurityAuditRows(rows, L"Code Integrity / WDAC");
    AddCommandRow(rows, L"Code Integrity / WDAC", L"SystemCodeIntegrityInformation", L"PowerShell Get-CimInstance Win32_DeviceGuard", RunPowerShellScalar(
        L"$dg=Get-CimInstance -Namespace root\\Microsoft\\Windows\\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction Stop; "
        L"'AvailableSecurityProperties=' + (($dg.AvailableSecurityProperties)-join ',') + '; SecurityServicesConfigured=' + (($dg.SecurityServicesConfigured)-join ',') + '; SecurityServicesRunning=' + (($dg.SecurityServicesRunning)-join ',') + '; CodeIntegrityPolicyEnforcementStatus=' + $dg.CodeIntegrityPolicyEnforcementStatus + '; UsermodeCodeIntegrityPolicyEnforcementStatus=' + $dg.UsermodeCodeIntegrityPolicyEnforcementStatus"));
    AddCommandRow(rows, L"Code Integrity / WDAC", L"CI policy files", L"PowerShell Get-ChildItem", RunPowerShellScalar(
        L"$paths=@('$env:windir\\System32\\CodeIntegrity\\CiPolicies\\Active','$env:windir\\System32\\CodeIntegrity'); "
        L"foreach($p in $paths){ if(Test-Path $p){ $c=(Get-ChildItem -LiteralPath $p -File -ErrorAction SilentlyContinue | Measure-Object).Count; Write-Output ($p + '=' + $c) } else { Write-Output ($p + '=missing') } }"));
    AddRegistryRow(rows, L"Code Integrity / WDAC", L"Policy UpgradedSystem", L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy", L"UpgradedSystem");
    AddRegistryRow(rows, L"Code Integrity / WDAC", L"Code Integrity Enabled", L"SYSTEM\\CurrentControlSet\\Control\\CI\\Config", L"Enabled");
    AddRegistryRow(rows, L"Code Integrity / WDAC", L"Secure Boot state cache", L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled");
    AddServiceRow(rows, L"Code Integrity / WDAC", L"Code Integrity driver", L"CI");
    return rows;
}

// CollectVbsRows gathers VBS/HVCI/SKCI evidence from DeviceGuard CIM, systeminfo
// and service/module presence. There is no input; output is a row vector for the
// VBS/HVCI/SKCI tab.
std::vector<MiscAuditRow> CollectVbsRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"VBS / HVCI / SKCI");
    AddSecurityAuditRows(rows, L"VBS / HVCI / SKCI");
    AddCommandRow(rows, L"VBS / HVCI / SKCI", L"DeviceGuard status", L"PowerShell CIM root/Microsoft/Windows/DeviceGuard", RunPowerShellScalar(
        L"$dg=Get-CimInstance -Namespace root\\Microsoft\\Windows\\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction Stop; "
        L"'VirtualizationBasedSecurityStatus=' + $dg.VirtualizationBasedSecurityStatus + '; RequiredSecurityProperties=' + (($dg.RequiredSecurityProperties)-join ',') + '; AvailableSecurityProperties=' + (($dg.AvailableSecurityProperties)-join ',') + '; Running=' + (($dg.SecurityServicesRunning)-join ',') + '; Configured=' + (($dg.SecurityServicesConfigured)-join ',')"));
    AddCommandRow(rows, L"VBS / HVCI / SKCI", L"HVCI memory integrity", L"PowerShell registry", RunPowerShellScalar(
        L"$p='HKLM:\\SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity'; "
        L"if(Test-Path $p){ Get-ItemProperty -LiteralPath $p | Select-Object -Property Enabled,WasEnabledBy,Locked | Format-List | Out-String } else { 'HVCI scenario key missing' }"));
    AddCommandRow(rows, L"VBS / HVCI / SKCI", L"Secure Kernel modules", L"PowerShell Get-ProcessModule/System32", RunPowerShellScalar(
        L"$names=@('securekernel.exe','skci.dll','ci.dll'); foreach($n in $names){ $p=Join-Path $env:windir ('System32\\' + $n); if(Test-Path $p){ Write-Output ($n + '=present') } else { Write-Output ($n + '=missing') } }"));
    AddRegistryRow(rows, L"VBS / HVCI / SKCI", L"DeviceGuard EnableVirtualizationBasedSecurity", L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"EnableVirtualizationBasedSecurity");
    AddRegistryRow(rows, L"VBS / HVCI / SKCI", L"DeviceGuard RequirePlatformSecurityFeatures", L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"RequirePlatformSecurityFeatures");
    AddRegistryRow(rows, L"VBS / HVCI / SKCI", L"LsaCfgFlags", L"SYSTEM\\CurrentControlSet\\Control\\Lsa", L"LsaCfgFlags");
    return rows;
}

// CollectHyperVRows gathers Hyper-V, VMBus, vSwitch, vPCI and HvSocket posture
// with read-only service/module/CIM evidence. There is no input; output is a row
// vector for the Hyper-V tab.
std::vector<MiscAuditRow> CollectHyperVRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"Hyper-V / VMBus / HvSocket");
    AddSecurityAuditRows(rows, L"Hyper-V / VMBus / HvSocket");
    AddCommandRow(rows, L"Hyper-V / VMBus / HvSocket", L"ComputerSystem hypervisor", L"PowerShell CIM Win32_ComputerSystem", RunPowerShellScalar(
        L"$cs=Get-CimInstance Win32_ComputerSystem -ErrorAction Stop; 'HypervisorPresent=' + $cs.HypervisorPresent + '; Manufacturer=' + $cs.Manufacturer + '; Model=' + $cs.Model"));
    AddCommandRow(rows, L"Hyper-V / VMBus / HvSocket", L"Hyper-V optional features", L"PowerShell Get-WindowsOptionalFeature", RunPowerShellScalar(
        L"$names=@('Microsoft-Hyper-V-All','Microsoft-Hyper-V-Hypervisor','VirtualMachinePlatform','Microsoft-Windows-Subsystem-Linux'); foreach($n in $names){ $f=Get-WindowsOptionalFeature -Online -FeatureName $n -ErrorAction SilentlyContinue; if($f){ Write-Output ($n + '=' + $f.State) } else { Write-Output ($n + '=Unavailable') } }"));
    AddCommandRow(rows, L"Hyper-V / VMBus / HvSocket", L"Hyper-V network adapters", L"PowerShell CIM Win32_PnPEntity", RunPowerShellScalar(
        L"Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'Hyper-V|VMBus|vmbus|Virtual Switch|vEthernet|HvSocket' } | Select-Object -First 40 -Property Name,PNPClass,Status | Format-Table -AutoSize | Out-String"));
    AddServiceRow(rows, L"Hyper-V / VMBus / HvSocket", L"VMBus kernel driver", L"vmbus");
    AddServiceRow(rows, L"Hyper-V / VMBus / HvSocket", L"Hyper-V Virtual Switch Extension Adapter", L"VMSMP");
    AddServiceRow(rows, L"Hyper-V / VMBus / HvSocket", L"Hyper-V socket service", L"HvHost");
    AddServiceRow(rows, L"Hyper-V / VMBus / HvSocket", L"Virtual PCI bus", L"vpci");
    AddRegistryRow(rows, L"Hyper-V / VMBus / HvSocket", L"Hypervisor launch type", L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"HypervisorEnforcedCodeIntegrity");
    return rows;
}

// CollectAppLockerRows gathers AppID/AppLocker/appid.sys/applockerfltr/mssecflt
// evidence without policy mutation or rule export. There is no input; output is
// a row vector for the AppLocker tab.
std::vector<MiscAuditRow> CollectAppLockerRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"AppLocker / AppID");
    AddSecurityAuditRows(rows, L"AppLocker / AppID");
    AddCommandRow(rows, L"AppLocker / AppID", L"Effective AppLocker policy count", L"PowerShell Get-AppLockerPolicy", RunPowerShellScalar(
        L"try { $p=Get-AppLockerPolicy -Effective -ErrorAction Stop; $xml=[xml]($p.ToXml()); $rules=($xml.AppLockerPolicy.RuleCollection | ForEach-Object { $_.ChildNodes.Count } | Measure-Object -Sum).Sum; 'RuleCollections=' + $xml.AppLockerPolicy.RuleCollection.Count + '; RuleCount=' + $rules } catch { 'Get-AppLockerPolicy failed: ' + $_.Exception.Message; exit 1 }"));
    AddCommandRow(rows, L"AppLocker / AppID", L"AppID service", L"PowerShell Get-Service", RunPowerShellScalar(
        L"Get-Service -Name AppIDSvc -ErrorAction Stop | Select-Object Name,Status,StartType | Format-List | Out-String"));
    AddCommandRow(rows, L"AppLocker / AppID", L"Application Control event logs", L"PowerShell Get-WinEvent", RunPowerShellScalar(
        L"$logs=@('Microsoft-Windows-AppLocker/EXE and DLL','Microsoft-Windows-AppLocker/MSI and Script','Microsoft-Windows-CodeIntegrity/Operational'); foreach($l in $logs){ $log=Get-WinEvent -ListLog $l -ErrorAction SilentlyContinue; if($log){ Write-Output ($l + '=enabled:' + $log.IsEnabled + '; records:' + $log.RecordCount) } else { Write-Output ($l + '=Unavailable') } }"));
    AddServiceRow(rows, L"AppLocker / AppID", L"AppID kernel driver", L"AppID");
    AddServiceRow(rows, L"AppLocker / AppID", L"AppLocker minifilter", L"applockerfltr");
    AddServiceRow(rows, L"AppLocker / AppID", L"Microsoft security filter", L"mssecflt");
    AddRegistryRow(rows, L"AppLocker / AppID", L"SRP identifiers policy", L"SOFTWARE\\Policies\\Microsoft\\Windows\\Safer\\CodeIdentifiers", L"DefaultLevel");
    return rows;
}

// CollectAuxiliaryRows gathers BAM and ahcache availability in privacy-preserving
// summary mode. There is no input; output is a row vector for the BAM/ahcache tab.
std::vector<MiscAuditRow> CollectAuxiliaryRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"BAM / ahcache");
    AddSecurityAuditRows(rows, L"BAM / ahcache");
    AddCommandRow(rows, L"BAM / ahcache", L"BAM registry summary", L"PowerShell registry count", RunPowerShellScalar(
        L"$p='HKLM:\\SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings'; if(Test-Path $p){ $users=(Get-ChildItem -LiteralPath $p -ErrorAction SilentlyContinue | Measure-Object).Count; 'UserSettingsKeys=' + $users + '; privacyMode=SummaryOnly' } else { 'BAM UserSettings key missing' }"));
    AddCommandRow(rows, L"BAM / ahcache", L"Amcache availability", L"PowerShell file summary", RunPowerShellScalar(
        L"$p=Join-Path $env:windir 'AppCompat\\Programs\\Amcache.hve'; if(Test-Path $p){ $i=Get-Item -LiteralPath $p; 'AmcachePresent=true; Length=' + $i.Length + '; LastWriteUtc=' + $i.LastWriteTimeUtc.ToString('o') + '; privacyMode=SummaryOnly' } else { 'Amcache.hve missing' }"));
    AddCommandRow(rows, L"BAM / ahcache", L"AppCompat cache service keys", L"PowerShell registry summary", RunPowerShellScalar(
        L"$keys=@('HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\AppCompatCache','HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags'); foreach($k in $keys){ if(Test-Path $k){ Write-Output ($k + '=present') } else { Write-Output ($k + '=missing') } }"));
    AddServiceRow(rows, L"BAM / ahcache", L"BAM driver", L"bam");
    AddServiceRow(rows, L"BAM / ahcache", L"Application Compatibility Cache", L"ahcache");
    AppendRow(rows, L"BAM / ahcache", L"Privacy boundary", L"SummaryOnly", L"UI policy", L"Info", L"默认只显示状态、计数和可用性，不枚举用户执行历史明细、不导出路径时间线。");
    return rows;
}

// CollectRowsForPage dispatches a tab id to its read-only collector. Input is a
// stable page id; processing performs local R3 queries; output is the fresh row
// snapshot used by RefreshAuditView.
std::vector<MiscAuditRow> CollectRowsForPage(const MiscAuditPageId pageId) {
    switch (pageId) {
    case MiscAuditPageId::CodeIntegrity:
        return CollectCodeIntegrityRows();
    case MiscAuditPageId::VbsHvciSkci:
        return CollectVbsRows();
    case MiscAuditPageId::HyperV:
        return CollectHyperVRows();
    case MiscAuditPageId::AppLocker:
        return CollectAppLockerRows();
    case MiscAuditPageId::BamAhcache:
        return CollectAuxiliaryRows();
    default:
        return {};
    }
}

// AuditColumns returns the fixed ListView schema shared by every Misc tab. There
// is no input; output is consumed by AddListViewColumns during page creation.
std::vector<Ksword::Ui::ListViewColumn> AuditColumns() {
    return {
        { 0, 190, LVCFMT_LEFT, L"类别" },
        { 1, 250, LVCFMT_LEFT, L"项目" },
        { 2, 130, LVCFMT_LEFT, L"状态" },
        { 3, 260, LVCFMT_LEFT, L"来源" },
        { 4, 100, LVCFMT_LEFT, L"风险" },
        { 5, 620, LVCFMT_LEFT, L"详情 / 失败原因" },
    };
}

// PopulateAuditList renders a row snapshot into the report ListView. Input is a
// page state; processing replaces visible rows only; no value is returned.
void PopulateAuditList(MiscAuditViewState& state) {
    if (!state.list) {
        return;
    }
    Ksword::Ui::ScopedListViewRedrawLock lock(state.list);
    Ksword::Ui::ClearListViewRows(state.list);
    for (const MiscAuditRow& row : state.rows) {
        Ksword::Ui::InsertListViewTextRow(state.list, {
            row.category,
            row.item,
            row.state,
            row.source,
            row.risk,
            row.detail,
        });
    }
}

// BuildRowsTsv converts current rows to clipboard-friendly TSV. Input is a row
// vector; processing does not escape beyond replacing line breaks; output is text
// suitable for copy/paste into a spreadsheet.
std::wstring BuildRowsTsv(const std::vector<MiscAuditRow>& rows) {
    auto clean = [](std::wstring text) {
        std::replace(text.begin(), text.end(), L'\r', L' ');
        std::replace(text.begin(), text.end(), L'\n', L' ');
        std::replace(text.begin(), text.end(), L'\t', L' ');
        return text;
    };

    std::wostringstream stream;
    stream << L"类别\t项目\t状态\t来源\t风险\t详情\r\n";
    for (const MiscAuditRow& row : rows) {
        stream << clean(row.category) << L'\t'
               << clean(row.item) << L'\t'
               << clean(row.state) << L'\t'
               << clean(row.source) << L'\t'
               << clean(row.risk) << L'\t'
               << clean(row.detail) << L"\r\n";
    }
    return stream.str();
}

// WriteClipboardText copies Unicode text to the clipboard. Inputs are owner HWND
// and text; processing transfers a movable global allocation to Windows; output
// reports success for status messages.
bool WriteClipboardText(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
        return false;
    }
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
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

// SelectedRowIndex returns the selected ListView row index. Input is a page state;
// output is -1 when no row is selected.
int SelectedRowIndex(const MiscAuditViewState& state) {
    return state.list ? ListView_GetNextItem(state.list, -1, LVNI_SELECTED) : -1;
}

// CopySelectedRow copies one evidence row as TSV. Input is a page state; process
// reads only the current in-memory row; no return value is produced.
void CopySelectedRow(MiscAuditViewState& state) {
    const int index = SelectedRowIndex(state);
    if (index < 0 || index >= static_cast<int>(state.rows.size())) {
        state.statusText = L"没有选中可复制的行。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }
    const bool ok = WriteClipboardText(state.hwnd, BuildRowsTsv({ state.rows[static_cast<std::size_t>(index)] }));
    state.statusText = ok ? L"已复制当前行。" : L"复制当前行失败。";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

// CopyAllRows copies the full current tab as TSV. Input is a page state; process
// serializes existing rows and does not trigger a refresh; no value is returned.
void CopyAllRows(MiscAuditViewState& state) {
    const bool ok = WriteClipboardText(state.hwnd, BuildRowsTsv(state.rows));
    state.statusText = ok ? L"已复制全部审计行。" : L"复制全部审计行失败。";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

// RefreshAuditView runs the collector for the current tab and updates the UI.
// Input is a page state; processing may launch bounded PowerShell helpers and
// query SCM/registry/ArkDriverClient; no value is returned.
void RefreshAuditView(MiscAuditViewState& state) {
    state.statusText = L"正在刷新只读审计证据...";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
    state.rows = CollectRowsForPage(state.pageId);
    PopulateAuditList(state);
    std::size_t unavailable = 0;
    for (const MiscAuditRow& row : state.rows) {
        if (row.state == L"Unavailable") {
            ++unavailable;
        }
    }
    state.statusText = L"Rows=" + std::to_wstring(state.rows.size()) +
        L"; Unavailable=" + std::to_wstring(unavailable) +
        L"; 默认只读审计，失败原因保留在详情列。";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

// LayoutAuditView places toolbar and ListView inside one Misc tab. Input is page
// state; processing uses current client size; no value is returned.
void LayoutAuditView(MiscAuditViewState& state) {
    if (!state.hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    ::MoveWindow(state.refreshButton, kGap, kGap, 86, 24, TRUE);
    ::MoveWindow(state.copyButton, kGap + 94, kGap, 106, 24, TRUE);
    const int top = kHeaderHeight + kGap;
    ::MoveWindow(state.list, kGap, top, std::max(0, width - (kGap * 2)), std::max(0, height - top - kGap), TRUE);
}

// ShowAuditContextMenu displays only read-only actions. Inputs are page state and
// a screen point; processing can refresh or copy rows; no mutation command exists.
void ShowAuditContextMenu(MiscAuditViewState& state, POINT screenPoint) {
    if (!state.list) {
        return;
    }
    POINT clientPoint = screenPoint;
    ::ScreenToClient(state.list, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int item = ListView_HitTest(state.list, &hit);
    if (item >= 0) {
        ListView_SetItemState(state.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(state.list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    const bool hasSelection = SelectedRowIndex(state) >= 0;
    ::AppendMenuW(menu, MF_STRING, kMenuRefresh, L"刷新只读审计");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyRow, L"复制当前行");
    ::AppendMenuW(menu, MF_STRING, kMenuCopyAll, L"复制全部行");

    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    switch (command) {
    case kMenuRefresh:
        RefreshAuditView(state);
        break;
    case kMenuCopyRow:
        CopySelectedRow(state);
        break;
    case kMenuCopyAll:
        CopyAllRows(state);
        break;
    default:
        break;
    }
}

// CreateAuditChildControls creates one tab page's toolbar and report ListView.
// Input is the page state and HWND; processing adds fixed columns; output is true
// when all controls exist.
bool CreateAuditChildControls(MiscAuditViewState& state, HWND hwnd) {
    state.refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"Refresh", 0, 0, 80, 24);
    state.copyButton = Ksword::Ui::CreateButton(hwnd, kCopyButtonId, L"Copy TSV", 0, 0, 100, 24);
    state.list = Ksword::Ui::CreateReportListView(hwnd, kListId, 0, 0, 0, 0);
    if (!state.refreshButton || !state.copyButton || !state.list) {
        return false;
    }
    Ksword::Ui::AddListViewColumns(state.list, AuditColumns());
    return true;
}

// RegisterAuditViewClass registers the retained child page class once. There is
// no input; output is true when CreateWindowExW can instantiate the class.
bool RegisterAuditViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        MiscAuditViewState* state = StateFromAuditView(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<MiscAuditViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }

        switch (msg) {
        case WM_CREATE:
            if (state) {
                if (!CreateAuditChildControls(*state, hwnd)) {
                    delete state;
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    return -1;
                }
                LayoutAuditView(*state);
                RefreshAuditView(*state);
            }
            return 0;
        case WM_SIZE:
            if (state) {
                LayoutAuditView(*state);
            }
            return 0;
        case WM_COMMAND:
            if (state && LOWORD(wParam) == kRefreshButtonId) {
                RefreshAuditView(*state);
                return 0;
            }
            if (state && LOWORD(wParam) == kCopyButtonId) {
                CopyAllRows(*state);
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->list && header->code == NM_RCLICK) {
                    POINT pt{};
                    ::GetCursorPos(&pt);
                    ShowAuditContextMenu(*state, pt);
                    return 0;
                }
            }
            break;
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->list) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->list, &rc);
                    pt = { rc.left + 24, rc.top + 24 };
                }
                ShowAuditContextMenu(*state, pt);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().panelBrush());
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().panelBrush());
            RECT textRc{ kGap + 210, 7, rc.right - kGap, kHeaderHeight };
            const std::wstring text = state ? (state->title + L" - " + state->statusText) : L"Misc audit";
            Ksword::Ui::DrawTextLine(dc, text, textRc, Ksword::Ui::AppTheme().mutedTextColor, Ksword::Ui::SystemUIFont(), DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().panelBrush();
    wc.lpszClassName = kMiscAuditViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

// CreateAuditView creates one retained Misc child page. Inputs are parent,
// bounds, page id and title; processing allocates state for the child window;
// output is the child HWND or nullptr on failure.
HWND CreateAuditView(HWND parent, const RECT& bounds, MiscAuditPageId pageId, std::wstring title) {
    if (!parent || !RegisterAuditViewClass()) {
        return nullptr;
    }
    auto* state = new MiscAuditViewState();
    state->pageId = pageId;
    state->title = std::move(title);
    HWND hwnd = ::CreateWindowExW(
        0,
        kMiscAuditViewClass,
        L"MiscAuditView",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        Width(bounds),
        Height(bounds),
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

// ShowHostPages toggles retained tab pages without destroying their state. Input
// is host state; processing uses ShowWindow only; no value is returned.
void ShowHostPages(MiscFeaturePageState& state) {
    if (state.ciView) {
        ::ShowWindow(state.ciView, state.currentTab == kCiTabIndex ? SW_SHOW : SW_HIDE);
    }
    if (state.vbsView) {
        ::ShowWindow(state.vbsView, state.currentTab == kVbsTabIndex ? SW_SHOW : SW_HIDE);
    }
    if (state.hypervView) {
        ::ShowWindow(state.hypervView, state.currentTab == kHyperVTabIndex ? SW_SHOW : SW_HIDE);
    }
    if (state.appLockerView) {
        ::ShowWindow(state.appLockerView, state.currentTab == kAppLockerTabIndex ? SW_SHOW : SW_HIDE);
    }
    if (state.auxiliaryView) {
        ::ShowWindow(state.auxiliaryView, state.currentTab == kAuxiliaryTabIndex ? SW_SHOW : SW_HIDE);
    }
}

// LayoutHostChildren sizes the tab control and every retained child page. Input
// is host state; processing uses the tab display rect; no value is returned.
void LayoutHostChildren(MiscFeaturePageState& state) {
    if (!state.hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    ::MoveWindow(state.tab, 0, 0, Width(rc), Height(rc), TRUE);
    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    const int pageWidth = Width(display);
    const int pageHeight = Height(display);
    const std::array<HWND, 5> pages{ state.ciView, state.vbsView, state.hypervView, state.appLockerView, state.auxiliaryView };
    for (HWND page : pages) {
        if (page) {
            ::MoveWindow(page, display.left, display.top, pageWidth, pageHeight, TRUE);
        }
    }
    ShowHostPages(state);
}

// CreateHostChildControls creates the tab host and five security posture pages.
// Input is host state with hwnd already assigned; output is true when every page
// was created successfully.
bool CreateHostChildControls(MiscFeaturePageState& state) {
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kTabId, 0, 0, 0, 0);
    if (!state.tab) {
        return false;
    }
    Ksword::Ui::AddTabPage(state.tab, kCiTabIndex, { L"Code Integrity / WDAC" });
    Ksword::Ui::AddTabPage(state.tab, kVbsTabIndex, { L"VBS / HVCI / SKCI" });
    Ksword::Ui::AddTabPage(state.tab, kHyperVTabIndex, { L"Hyper-V / VMBus" });
    Ksword::Ui::AddTabPage(state.tab, kAppLockerTabIndex, { L"AppLocker" });
    Ksword::Ui::AddTabPage(state.tab, kAuxiliaryTabIndex, { L"BAM / ahcache" });
    ::SendMessageW(state.tab, TCM_SETCURSEL, static_cast<WPARAM>(kCiTabIndex), 0);

    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    const RECT childBounds{ 0, 0, std::max(1, Width(display)), std::max(1, Height(display)) };
    state.ciView = CreateAuditView(state.tab, childBounds, MiscAuditPageId::CodeIntegrity, L"Code Integrity / WDAC");
    state.vbsView = CreateAuditView(state.tab, childBounds, MiscAuditPageId::VbsHvciSkci, L"VBS / HVCI / SKCI");
    state.hypervView = CreateAuditView(state.tab, childBounds, MiscAuditPageId::HyperV, L"Hyper-V / VMBus / HvSocket");
    state.appLockerView = CreateAuditView(state.tab, childBounds, MiscAuditPageId::AppLocker, L"AppLocker / AppID");
    state.auxiliaryView = CreateAuditView(state.tab, childBounds, MiscAuditPageId::BamAhcache, L"BAM / ahcache");
    return state.ciView && state.vbsView && state.hypervView && state.appLockerView && state.auxiliaryView;
}

// RegisterMiscFeatureClass registers the outer Misc tab host. There is no input;
// output is true when CreateMiscFeaturePage can create the class.
bool RegisterMiscFeatureClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        MiscFeaturePageState* state = StateFromHost(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<MiscFeaturePageState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }

        switch (msg) {
        case WM_CREATE:
            if (state) {
                if (!CreateHostChildControls(*state)) {
                    delete state;
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    return -1;
                }
                LayoutHostChildren(*state);
            }
            return 0;
        case WM_SIZE:
            if (state) {
                LayoutHostChildren(*state);
            }
            return 0;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
                    const LRESULT selected = ::SendMessageW(state->tab, TCM_GETCURSEL, 0, 0);
                    if (selected >= 0) {
                        state->currentTab = static_cast<int>(selected);
                    }
                    ShowHostPages(*state);
                    return 0;
                }
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
        }
        case WM_NCDESTROY:
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kMiscHostClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateMiscFeaturePage(HWND parent, const RECT& bounds) {
    // Inputs are the shell parent HWND and initial child bounds. Processing only
    // creates the read-only Misc audit host and retained child tabs; registration
    // into FeatureRegistry/vcxproj is intentionally left to thread13 per the user
    // constraint. Return value is the host HWND or nullptr on failure.
    if (!parent || !RegisterMiscFeatureClass()) {
        return nullptr;
    }
    auto* state = new MiscFeaturePageState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kMiscHostClass,
        L"Misc",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        Width(bounds),
        Height(bounds),
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::Misc
