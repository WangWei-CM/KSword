#pragma execution_character_set("utf-8")

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shellapi.h>
#include <Objbase.h>

#include "KswordGUI/KswordStyle.h"
#include "resource.h"
#include "PayloadResources.h"

#include "Fl.H"
#include "Fl_Window.H"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace {
constexpr int kWindowWidth = 580;
constexpr int kWindowHeight = 720;
constexpr int kLayeredImageWidth = 405;
constexpr int kImageDrawHeight = 720;
constexpr int kPad = 30;
constexpr wchar_t kDefaultInstallDir[] = L"C:\\Program Files\\KswordARK";
constexpr wchar_t kStateArg[] = L"--install-state";
constexpr wchar_t kSettingsRel[] = L"Style\\appearance_settings.json";
constexpr wchar_t kMainExe[] = L"Ksword5.1.exe";
constexpr wchar_t kTaskmgrScript[] = L"TaskmgrHijack.ps1";

// InstallOptions is the installer input model. Values come from FLTK widgets or
// the elevation handoff JSON file; PerformInstall consumes them and returns no
// mutations through this struct after installation starts.
struct InstallOptions {
    std::wstring installDir = kDefaultInstallDir;
    bool startupAdmin = true;
    bool startupMaximized = true;
    bool replaceTaskmgr = false;
    bool testMode = false;
    bool desktopShortcut = true;
    bool startMenuShortcut = true;
    bool launchAfterInstall = true;
};

// InstallResult is the user-visible outcome. Each installation phase appends a
// line to logText, and the final page displays this text in the status control.
struct InstallResult {
    bool ok = true;
    bool rebootNow = false;
    std::wstring logText;
};

KLayeredImageWindow g_characterWindow;
KInput* g_pathInput = nullptr;
KCheckBox* g_adminCheck = nullptr;
KCheckBox* g_maxCheck = nullptr;
KCheckBox* g_taskmgrCheck = nullptr;
KCheckBox* g_testModeCheck = nullptr;
KCheckBox* g_desktopCheck = nullptr;
KCheckBox* g_startMenuCheck = nullptr;
KCheckBox* g_launchCheck = nullptr;
KTextDisplay* g_status = nullptr;
KButton* g_installButton = nullptr;
KButton* g_browseButton = nullptr;
std::wstring g_characterImagePath;

// WideToUtf8 converts UTF-16 Win32 text to UTF-8 for FLTK widgets. Input is a
// UTF-16 string, processing uses WideCharToMultiByte, and output is UTF-8 bytes.
std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// Utf8ToWide converts FLTK UTF-8 text to UTF-16 for Win32 APIs. Input is UTF-8
// bytes, processing uses MultiByteToWideChar, and output is UTF-16 text.
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n);
    return out;
}

// Trim removes surrounding whitespace from user-entered paths. Input is a string,
// processing scans both ends, and output is the cleaned string.
std::wstring Trim(const std::wstring& text) {
    const wchar_t* ws = L" \t\r\n";
    const size_t first = text.find_first_not_of(ws);
    if (first == std::wstring::npos) return {};
    const size_t last = text.find_last_not_of(ws);
    return text.substr(first, last - first + 1);
}

// Join combines a directory and child path. Inputs are two path fragments,
// processing inserts one separator when needed, and output is a Windows path.
std::wstring Join(const std::wstring& dir, const std::wstring& child) {
    if (dir.empty()) return child;
    if (child.empty()) return dir;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + child;
    return dir + L"\\" + child;
}

// Parent returns the directory portion of a file path. Input is a path string,
// processing finds the final separator, and output is empty on failure.
std::wstring Parent(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

// ExistsFile reports whether a non-directory path exists. Input is a candidate
// path, processing queries Win32 attributes, and output is a boolean result.
bool ExistsFile(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// EnsureDir creates a directory tree. Input is a directory path, processing uses
// SHCreateDirectoryExW for recursive creation, and output is true on success.
bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    const int rc = ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

// JsonEscape encodes a wide string as a JSON string body. Input is raw text;
// processing escapes backslash, quote, and control characters; output is JSON-safe.
std::wstring JsonEscape(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\b': out += L"\\b"; break;
        case L'\f': out += L"\\f"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t buf[7]{};
                swprintf_s(buf, L"\\u%04X", (unsigned int)ch);
                out += buf;
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

// JsonUnescape decodes a JSON string body. Input is an escaped UTF-8 token
// without surrounding quotes; processing handles common escape sequences and
// converts the resulting UTF-8 bytes; output is wide text for the state file.
std::wstring JsonUnescape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch != '\\' || i + 1 >= text.size()) {
            out.push_back(ch);
            continue;
        }
        char next = text[++i];
        switch (next) {
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u':
            if (i + 4 < text.size()) {
                unsigned int value = 0;
                for (int k = 0; k < 4; ++k) {
                    char hex = text[i + 1 + (size_t)k];
                    value <<= 4;
                    if (hex >= '0' && hex <= '9') value |= (unsigned int)(hex - '0');
                    else if (hex >= 'A' && hex <= 'F') value |= 10u + (unsigned int)(hex - 'A');
                    else if (hex >= 'a' && hex <= 'f') value |= 10u + (unsigned int)(hex - 'a');
                }
                if (value < 0x80) {
                    out.push_back((char)value);
                } else if (value < 0x800) {
                    out.push_back((char)(0xC0 | (value >> 6)));
                    out.push_back((char)(0x80 | (value & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (value >> 12)));
                    out.push_back((char)(0x80 | ((value >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (value & 0x3F)));
                }
                i += 4;
            }
            break;
        default:
            out.push_back(next);
            break;
        }
    }
    return Utf8ToWide(out);
}

// JsonFindString returns a string value from a small JSON object. Inputs are the
// raw UTF-8 JSON text, the member name, and a default wide string; output is the
// found string or the default when the key is absent.
std::wstring JsonFindString(const std::string& json, const char* key, const std::wstring& defValue) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return defValue;
    size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return defValue;
    size_t start = json.find('"', colon + 1);
    if (start == std::string::npos) return defValue;
    ++start;
    std::string raw;
    raw.reserve(64);
    bool escaped = false;
    for (size_t i = start; i < json.size(); ++i) {
        char ch = json[i];
        if (!escaped && ch == '"') return JsonUnescape(raw);
        if (!escaped && ch == '\\') {
            escaped = true;
            raw.push_back(ch);
            continue;
        }
        escaped = false;
        raw.push_back(ch);
    }
    return defValue;
}

// JsonFindBool returns a boolean member from a small JSON object. Inputs are the
// raw UTF-8 JSON text, the member name, and a default value; output is a parsed
// flag or the default when parsing fails.
bool JsonFindBool(const std::string& json, const char* key, bool defValue) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return defValue;
    size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return defValue;
    size_t start = json.find_first_not_of(" \t\r\n", colon + 1);
    if (start == std::string::npos) return defValue;
    if (json.compare(start, 4, "true") == 0) return true;
    if (json.compare(start, 5, "false") == 0) return false;
    return defValue;
}

// IsDefaultInstallDir reports whether the selected path is the protected
// Program Files installation root. Input is a directory path; output is a
// boolean used to decide whether UAC is required.
bool IsDefaultInstallDir(const std::wstring& dir) {
    return _wcsicmp(dir.c_str(), kDefaultInstallDir) == 0;
}

// NeedsElevation evaluates whether the requested options touch privileged
// locations. Input is install options; output is true when UAC should be used.
bool NeedsElevation(const InstallOptions& o) {
    return IsDefaultInstallDir(o.installDir) || o.replaceTaskmgr || o.testMode || o.desktopShortcut || o.startMenuShortcut;
}

// FileExistsInDir reports whether a file exists under the install root. Input is
// root and relative path; output is a boolean used to detect update installs.
bool FileExistsInDir(const std::wstring& dir, const std::wstring& rel) {
    return ExistsFile(Join(dir, rel));
}

// AppendLog appends one line to the result log and refreshes the status widget.
// Input is mutable log text and one line; processing appends CRLF; no return.
void AppendLog(std::wstring* log, const std::wstring& line) {
    if (!log) return;
    *log += line;
    *log += L"\r\n";
    if (g_status) {
        const std::string utf8 = WideToUtf8(*log);
        g_status->set_text(utf8.c_str());
        Fl::check();
    }
}

// SetStatus replaces status text directly. Input is UTF-16 text; processing
// converts to UTF-8; no value is returned.
void SetStatus(const std::wstring& text) {
    if (!g_status) return;
    const std::string utf8 = WideToUtf8(text);
    g_status->set_text(utf8.c_str());
    Fl::check();
}

// WriteBytes creates or overwrites a file with binary data. Inputs are file path,
// byte pointer and size; processing creates the parent directory; output is true
// when all bytes are written.
bool WriteBytes(const std::wstring& path, const void* data, DWORD size) {
    const std::wstring parent = Parent(path);
    if (!parent.empty() && !EnsureDir(parent)) return false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile(h, data, size, &written, nullptr);
    ::CloseHandle(h);
    return ok && written == size;
}

// ReadBytes loads a file into memory through Win32 wide-path APIs. Input is a
// file path; processing reads the complete stream; output is true on success.
bool ReadBytes(const std::wstring& path, std::vector<char>* data) {
    if (!data) return false;
    data->clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    data->resize((size_t)size.QuadPart);
    DWORD read = 0;
    const BOOL ok = data->empty() || ::ReadFile(h, data->data(), (DWORD)data->size(), &read, nullptr);
    ::CloseHandle(h);
    return ok && read == data->size();
}

// ExtractRc writes an embedded RCDATA resource to disk. Inputs are resource id
// and output path; processing locates and locks the resource; output is true when
// extraction succeeds.
bool ExtractRc(unsigned int id, const std::wstring& path) {
    HRSRC res = ::FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL loaded = ::LoadResource(nullptr, res);
    const void* bytes = loaded ? ::LockResource(loaded) : nullptr;
    const DWORD size = ::SizeofResource(nullptr, res);
    return bytes && size > 0 && WriteBytes(path, bytes, size);
}

// ExePath returns the current installer executable path. Input is none;
// processing expands a stack buffer through GetModuleFileNameW; output is empty
// on failure.
std::wstring ExePath() {
    std::vector<wchar_t> buf(1024, L'\0');
    while (buf.size() < 32768) {
        DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) return {buf.data(), n};
        buf.resize(buf.size() * 2, L'\0');
    }
    return {};
}

// IsElevated checks the current token elevation state. Input is current process;
// processing queries TokenElevation; output is true for administrator elevation.
bool IsElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD len = 0;
    BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &len);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// Quote returns a quoted command-line argument. Input is raw text, processing
// escapes double quotes, and output is suitable for ShellExecute/CreateProcess.
std::wstring Quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (wchar_t ch : arg) out += (ch == L'\"') ? L"\\\"" : std::wstring(1, ch);
    out += L"\"";
    return out;
}

// TempStatePath allocates an elevation handoff file path. Input is none;
// processing uses GetTempPath/GetTempFileName; output is a temporary path.
std::wstring TempStatePath() {
    wchar_t dir[MAX_PATH]{};
    wchar_t file[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, dir) || !::GetTempFileNameW(dir, L"ksw", 0, file)) return L"KswordSetup.state";
    return file;
}

// SaveState writes install options to a JSON file. Inputs are path and options;
// processing stores UTF-8 state used by the elevated continuation; output is
// true when the handoff file is written.
bool SaveState(const std::wstring& file, const InstallOptions& o) {
    std::wostringstream json;
    json << L"{\n"
         << L"  \"installDir\": \"" << JsonEscape(o.installDir) << L"\",\n"
         << L"  \"startupAdmin\": " << (o.startupAdmin ? L"true" : L"false") << L",\n"
         << L"  \"startupMaximized\": " << (o.startupMaximized ? L"true" : L"false") << L",\n"
         << L"  \"replaceTaskmgr\": " << (o.replaceTaskmgr ? L"true" : L"false") << L",\n"
         << L"  \"testMode\": " << (o.testMode ? L"true" : L"false") << L",\n"
         << L"  \"desktopShortcut\": " << (o.desktopShortcut ? L"true" : L"false") << L",\n"
         << L"  \"startMenuShortcut\": " << (o.startMenuShortcut ? L"true" : L"false") << L",\n"
         << L"  \"launchAfterInstall\": " << (o.launchAfterInstall ? L"true" : L"false") << L"\n"
         << L"}\n";
    const std::string utf8 = WideToUtf8(json.str());
    return WriteBytes(file, utf8.data(), (DWORD)utf8.size());
}

// LoadState reads install options from a JSON handoff file. Input is path;
// processing reads expected fields with defaults; output is reconstructed
// options for the elevated installer instance.
InstallOptions LoadState(const std::wstring& file) {
    InstallOptions o;
    std::vector<char> bytes;
    if (!ReadBytes(file, &bytes)) return o;
    const std::string json(bytes.begin(), bytes.end());
    o.installDir = JsonFindString(json, "installDir", kDefaultInstallDir);
    o.startupAdmin = JsonFindBool(json, "startupAdmin", true);
    o.startupMaximized = JsonFindBool(json, "startupMaximized", true);
    o.replaceTaskmgr = JsonFindBool(json, "replaceTaskmgr", false);
    o.testMode = JsonFindBool(json, "testMode", false);
    o.desktopShortcut = JsonFindBool(json, "desktopShortcut", true);
    o.startMenuShortcut = JsonFindBool(json, "startMenuShortcut", true);
    o.launchAfterInstall = JsonFindBool(json, "launchAfterInstall", true);
    return o;
}

// RelaunchElevated starts this installer with runas and an install-state file.
// Input is state path; processing calls ShellExecuteW; output is true if Windows
// accepted the elevated launch request.
bool RelaunchElevated(const std::wstring& stateFile) {
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    const std::wstring args = std::wstring(kStateArg) + L" " + Quote(stateFile);
    HINSTANCE rc = ::ShellExecuteW(nullptr, L"runas", exe.c_str(), args.c_str(), Parent(exe).c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

// CollectOptions reads the current widget state. Input is global FLTK widgets;
// processing applies defaults and trimming; output is an InstallOptions object.
InstallOptions CollectOptions() {
    InstallOptions o;
    if (g_pathInput && g_pathInput->value()) o.installDir = Trim(Utf8ToWide(g_pathInput->value()));
    if (o.installDir.empty()) o.installDir = kDefaultInstallDir;
    o.startupAdmin = !g_adminCheck || g_adminCheck->value() != 0;
    o.startupMaximized = !g_maxCheck || g_maxCheck->value() != 0;
    o.replaceTaskmgr = g_taskmgrCheck && g_taskmgrCheck->value() != 0;
    o.testMode = g_testModeCheck && g_testModeCheck->value() != 0;
    o.desktopShortcut = !g_desktopCheck || g_desktopCheck->value() != 0;
    o.startMenuShortcut = !g_startMenuCheck || g_startMenuCheck->value() != 0;
    o.launchAfterInstall = !g_launchCheck || g_launchCheck->value() != 0;
    return o;
}

// ApplyOptions writes saved state back into widgets. Input is install options;
// processing updates each FLTK control; no value is returned.
void ApplyOptions(const InstallOptions& o) {
    if (g_pathInput) g_pathInput->value(WideToUtf8(o.installDir).c_str());
    if (g_adminCheck) g_adminCheck->value(o.startupAdmin ? 1 : 0);
    if (g_maxCheck) g_maxCheck->value(o.startupMaximized ? 1 : 0);
    if (g_taskmgrCheck) g_taskmgrCheck->value(o.replaceTaskmgr ? 1 : 0);
    if (g_testModeCheck) g_testModeCheck->value(o.testMode ? 1 : 0);
    if (g_desktopCheck) g_desktopCheck->value(o.desktopShortcut ? 1 : 0);
    if (g_startMenuCheck) g_startMenuCheck->value(o.startMenuShortcut ? 1 : 0);
    if (g_launchCheck) g_launchCheck->value(o.launchAfterInstall ? 1 : 0);
}

// ExtractPayload releases every generated payload resource. Input is install
// options and log text; processing merges files without deleting user config;
// output is true only when all resources write successfully.
bool ExtractPayload(const InstallOptions& o, std::wstring* log) {
    if (!EnsureDir(o.installDir)) {
        AppendLog(log, L"创建安装目录失败: " + o.installDir);
        return false;
    }
    for (unsigned int i = 0; i < kKswordSetupPayloadResourceCount; ++i) {
        const auto& e = kKswordSetupPayloadResources[i];
        const std::wstring out = Join(o.installDir, e.relativePath);
        AppendLog(log, L"释放: " + std::wstring(e.relativePath));
        if (!ExtractRc(e.resourceId, out)) {
            AppendLog(log, L"释放失败，请关闭正在运行的 Ksword 相关程序后重试: " + out);
            return false;
        }
    }
    return true;
}

// WriteSettings creates or optionally overwrites Style/appearance_settings.json.
// Input is install options; processing asks before replacing existing settings;
// output is true when the final policy is applied successfully.
bool WriteSettings(const InstallOptions& o, std::wstring* log) {
    const std::wstring file = Join(o.installDir, kSettingsRel);
    if (ExistsFile(file)) {
        int choice = ::MessageBoxW(nullptr,
            L"检测到已有配置文件。\n\n选择“是”覆盖安装器生成配置；选择“否”保留原有一切配置。",
            L"KswordSetup 覆盖安装", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
        if (choice != IDYES) {
            AppendLog(log, L"保留已有配置: " + file);
            return true;
        }
    }
    std::ostringstream json;
    json << "{\n"
         << "    \"theme_mode\": \"follow_system\",\n"
         << "    \"background_image_path\": \"Style/ksword_background.png\",\n"
         << "    \"background_opacity_percent\": 35,\n"
         << "    \"startup_default_tab_key\": \"welcome\",\n"
         << "    \"startup_maximized\": " << (o.startupMaximized ? "true" : "false") << ",\n"
         << "    \"startup_topmost_enabled\": true,\n"
         << "    \"startup_auto_request_admin\": " << (o.startupAdmin ? "true" : "false") << ",\n"
         << "    \"startup_window_scale_factor\": 1.0,\n"
         << "    \"startup_scale_recommend_prompt_disabled\": false,\n"
         << "    \"unlocker_shell_context_menu_enabled\": false,\n"
         << "    \"use_wide_scroll_bars\": false,\n"
         << "    \"scroll_bar_auto_hide_enabled\": false,\n"
         << "    \"slider_wheel_adjust_enabled\": false\n"
         << "}\n";
    const std::string data = json.str();
    if (!WriteBytes(file, data.data(), (DWORD)data.size())) {
        AppendLog(log, L"写入配置失败: " + file);
        return false;
    }
    AppendLog(log, L"写入启动配置: " + file);
    return true;
}

// RunWait starts a process and waits for completion. Inputs are executable,
// arguments, working directory and timeout; processing uses CreateProcessW;
// output is true when exit code is zero.
bool RunWait(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, DWORD timeoutMs, DWORD* exitCode) {
    std::wstring cmd = Quote(exe) + (args.empty() ? L"" : L" " + args);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) return false;
    DWORD wait = ::WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) ::TerminateProcess(pi.hProcess, 1460);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return wait == WAIT_OBJECT_0 && code == 0;
}

// InstallTaskmgr calls the released TaskmgrHijack.ps1 in parameter mode. Input is
// install options; processing points IFEO at installed Ksword5.1.exe; output is
// true when PowerShell exits successfully.
bool InstallTaskmgr(const InstallOptions& o, std::wstring* log) {
    const std::wstring script = Join(o.installDir, kTaskmgrScript);
    const std::wstring target = Join(o.installDir, kMainExe);
    const std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File " + Quote(script) + L" -Install -TargetExe " + Quote(target);
    DWORD code = 1;
    const bool ok = ExistsFile(script) && ExistsFile(target) && RunWait(L"powershell.exe", args, o.installDir, 60000, &code);
    AppendLog(log, ok ? L"已替换系统任务管理器。" : L"任务管理器替换失败，退出码: " + std::to_wstring(code));
    return ok;
}

// EnableTestMode runs bcdedit /set testsigning on. Input is log text; processing
// waits for bcdedit; output is true when Windows accepts the setting.
bool EnableTestMode(std::wstring* log) {
    DWORD code = 1;
    const bool ok = RunWait(L"bcdedit.exe", L"/set testsigning on", L"", 60000, &code);
    AppendLog(log, ok ? L"已执行 bcdedit /set testsigning on。" : L"启动测试模式失败，退出码: " + std::to_wstring(code));
    return ok;
}

// KnownFolder returns a Windows known-folder path. Input is a folder id;
// processing calls SHGetKnownFolderPath; output is empty when unavailable.
std::wstring KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(id, 0, nullptr, &raw);
    if (FAILED(hr) || !raw) return {};
    std::wstring out(raw);
    ::CoTaskMemFree(raw);
    return out;
}

// CreateShortcut writes one .lnk file. Inputs are link path, target and working
// directory; processing uses IShellLinkW/IPersistFile; output is success flag.
bool CreateShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& cwd) {
    if (!EnsureDir(Parent(link))) return false;
    IShellLinkW* sl = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl);
    if (FAILED(hr) || !sl) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(cwd.c_str());
    sl->SetDescription(L"KswordARK");
    IPersistFile* pf = nullptr;
    hr = sl->QueryInterface(IID_IPersistFile, (void**)&pf);
    if (SUCCEEDED(hr) && pf) {
        hr = pf->Save(link.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
    return SUCCEEDED(hr);
}

// CreateShortcuts creates selected all-user shortcuts. Input is install options;
// processing writes Public Desktop/Common Programs links; output is true when all
// requested shortcuts succeed.
bool CreateShortcuts(const InstallOptions& o, std::wstring* log) {
    bool all = true;
    const std::wstring target = Join(o.installDir, kMainExe);
    if (o.desktopShortcut) {
        bool ok = CreateShortcut(Join(KnownFolder(FOLDERID_PublicDesktop), L"KswordARK.lnk"), target, o.installDir);
        AppendLog(log, ok ? L"已创建桌面快捷方式。" : L"创建桌面快捷方式失败。");
        all = all && ok;
    }
    if (o.startMenuShortcut) {
        bool ok = CreateShortcut(Join(Join(KnownFolder(FOLDERID_CommonPrograms), L"KswordARK"), L"KswordARK.lnk"), target, o.installDir);
        AppendLog(log, ok ? L"已创建开始菜单快捷方式。" : L"创建开始菜单快捷方式失败。");
        all = all && ok;
    }
    return all;
}

// LaunchKsword starts the installed main program. Input is install directory;
// processing uses ShellExecuteW; output is true when launch is accepted.
bool LaunchKsword(const std::wstring& dir) {
    HINSTANCE rc = ::ShellExecuteW(nullptr, L"open", Join(dir, kMainExe).c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

// PerformInstall executes the full install transaction. Input is options;
// processing extracts payload, applies optional actions, prompts for reboot, and
// output is the final result model for follow-up launch/reboot behavior.
InstallResult PerformInstall(const InstallOptions& o) {
    InstallResult r;
    AppendLog(&r.logText, L"开始安装到: " + o.installDir);
    if (FileExistsInDir(o.installDir, kMainExe)) {
        AppendLog(&r.logText, L"检测到已有安装，按合并覆盖方式更新。");
    }
    r.ok = ExtractPayload(o, &r.logText) && r.ok;
    if (!r.ok) return r;
    r.ok = WriteSettings(o, &r.logText) && r.ok;
    r.ok = CreateShortcuts(o, &r.logText) && r.ok;
    if (o.replaceTaskmgr) r.ok = InstallTaskmgr(o, &r.logText) && r.ok;
    if (o.testMode) {
        const bool testOk = EnableTestMode(&r.logText);
        r.ok = testOk && r.ok;
        if (testOk) {
            int choice = ::MessageBoxW(nullptr, L"测试模式已写入，需要重启系统后生效。是否立即重启？", L"KswordSetup", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
            r.rebootNow = choice == IDYES;
        }
    }
    AppendLog(&r.logText, r.ok ? L"安装完成。" : L"安装完成，但存在失败项，请查看日志。");
    return r;
}

// StartInstall handles the Install button and elevated continuation. Input is
// current UI state; processing performs UAC handoff when needed; no return.
void StartInstall() {
    InstallOptions o = CollectOptions();
    if (NeedsElevation(o) && !IsElevated()) {
        std::wstring state = TempStatePath();
        if (!SaveState(state, o)) {
            ::MessageBoxW(nullptr, L"写入提权状态文件失败。", L"KswordSetup", MB_ICONERROR);
            return;
        }
        if (RelaunchElevated(state)) {
            SetStatus(L"已请求管理员权限，请在 UAC 窗口确认。确认后安装将在新窗口继续。");
            return;
        }
        ::MessageBoxW(nullptr, L"管理员权限请求被取消或启动失败。", L"KswordSetup", MB_ICONWARNING);
        return;
    }
    if (g_installButton) g_installButton->deactivate();
    if (g_browseButton) g_browseButton->deactivate();
    InstallResult result = PerformInstall(o);
    if (g_installButton) g_installButton->activate();
    if (g_browseButton) g_browseButton->activate();
    if (result.rebootNow) {
        ::ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/r /t 0", nullptr, SW_HIDE);
        return;
    }
    if (result.ok && o.launchAfterInstall) LaunchKsword(o.installDir);
}

// BrowseInstallDir opens the native folder picker. Input is current UI path;
// processing uses IFileDialog in folder mode; no return value.
void BrowseInstallDir() {
    IFileDialog* dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileDialog, (void**)&dlg);
    if (FAILED(hr) || !dlg) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"选择 KswordARK 安装目录");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                if (g_pathInput) g_pathInput->value(WideToUtf8(raw).c_str());
                ::CoTaskMemFree(raw);
            }
            item->Release();
        }
    }
    dlg->Release();
}

// ExtractCharacterImage releases the left PNG to temp for KLayeredImageWindow.
// Input is none; processing extracts once; output is a UTF-8 filesystem path.
std::string ExtractCharacterImage() {
    if (!g_characterImagePath.empty() && ExistsFile(g_characterImagePath)) return WideToUtf8(g_characterImagePath);
    wchar_t temp[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, temp)) g_characterImagePath = L"KswordSetupCharacter.png";
    else g_characterImagePath = Join(temp, L"KswordSetupCharacter.png");
    ExtractRc(IDR_KSWORD_SETUP_CHARACTER_PNG, g_characterImagePath);
    return WideToUtf8(g_characterImagePath);
}

// ParseStateArgument returns the --install-state path. Input is process command
// line; processing tokenizes with CommandLineToArgvW; output is empty when absent.
std::wstring ParseStateArgument() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return {};
    std::wstring out;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == kStateArg) { out = argv[i + 1]; break; }
    }
    ::LocalFree(argv);
    return out;
}

// ConfigureRightContent builds the installer page. Input is owner window;
// processing creates K* widgets and callbacks; no return value.
void ConfigureRightContent(Fl_Window* window) {
    const KTheme& theme = KThemeManager::instance().theme();
    const int cardX = kPad;
    const int cardY = 28;
    const int cardW = kWindowWidth - kPad * 2;
    const int cardH = kWindowHeight - 56;
    KCard* card = KCreateCard(cardX, cardY, cardW, cardH, "KswordARK 安装设置");
    card->setSubtitle("选择安装路径和首次启动行为，点击安装时按需请求管理员权限。");
    card->begin();
    KText* title = KCreateText(cardX + 24, cardY + 70, cardW - 48, 28, "KswordSetup");
    title->labelsize(20);
    title->labelcolor(theme.text);
    KCreateText(cardX + 24, cardY + 112, 120, 24, "安装路径");
    g_pathInput = KCreateInput(cardX + 24, cardY + 142, cardW - 154, 32, nullptr);
    g_pathInput->value(WideToUtf8(kDefaultInstallDir).c_str());
    g_browseButton = KCreateButton(cardX + cardW - 118, cardY + 142, 94, 32, "浏览", KBUTTON_LIGHT);
    g_browseButton->callback([](Fl_Widget*, void*) { BrowseInstallDir(); });
    int y = cardY + 192;
    g_adminCheck = KCreateCheckBox(cardX + 24, y, cardW - 48, 28, "启动时自动请求管理员权限"); g_adminCheck->value(1); y += 34;
    g_maxCheck = KCreateCheckBox(cardX + 24, y, cardW - 48, 28, "启动时默认最大化"); g_maxCheck->value(1); y += 34;
    g_taskmgrCheck = KCreateCheckBox(cardX + 24, y, cardW - 48, 28, "替换系统自带任务管理器"); y += 34;
    g_testModeCheck = KCreateCheckBox(cardX + 24, y, cardW - 48, 28, "启动测试模式（testsigning）"); y += 34;
    g_desktopCheck = KCreateCheckBox(cardX + 24, y, cardW - 48, 28, "创建桌面快捷方式"); g_desktopCheck->value(1); y += 34;
    g_startMenuCheck = KCreateCheckBox(cardX + 24, y, cardW - 48, 28, "创建开始菜单快捷方式"); g_startMenuCheck->value(1); y += 34;
    g_launchCheck = KCreateCheckBox(cardX + 24, y, cardW - 48, 28, "安装完成后启动 Ksword"); g_launchCheck->value(1);
    g_status = KCreateTextDisplay(cardX + 24, cardY + 450, cardW - 48, 142, nullptr);
    g_status->set_text("准备安装。\n内嵌 Release payload 会释放到目标目录。\n覆盖安装时会询问是否保留已有配置。");
    g_installButton = KCreateButton(cardX + cardW - 244, cardY + cardH - 48, 104, 34, "安装", KBUTTON_HEAVY);
    g_installButton->callback([](Fl_Widget*, void*) { StartInstall(); });
    KButton* closeButton = KCreateButton(cardX + cardW - 124, cardY + cardH - 48, 100, 34, "关闭", KBUTTON_LIGHT);
    closeButton->callback([](Fl_Widget*, void* data) {
        g_characterWindow.destroy();
        if (auto* owner = static_cast<Fl_Window*>(data)) owner->hide();
    }, window);
    card->end();
}
} // namespace

// GuiInitMain builds the right-side FLTK installer content. Inputs are arguments
// and the owner window; processing sets style and widgets; no value is returned.
void GuiInitMain(const std::vector<std::string>& args, Fl_Window* MainWindow) {
    (void)args;
    if (!MainWindow) return;
    MainWindow->label("KswordSetup");
    MainWindow->size(kWindowWidth, kWindowHeight);
    MainWindow->border(0);
    SetWindowStyle(MainWindow);
    ConfigureRightContent(MainWindow);
    KThemeManager::instance().RefreshAll();
}

// GuiAfterShowMain creates the transparent left PNG window and starts elevated
// continuation when --install-state is present. Input is the shown owner window;
// no value is returned.
void GuiAfterShowMain(Fl_Window* MainWindow) {
    if (!MainWindow) return;
    const std::string characterPath = ExtractCharacterImage();
    g_characterWindow.setClickThrough(true);
    g_characterWindow.showPngForWindow(MainWindow, characterPath, -kLayeredImageWidth + 6, 0, kLayeredImageWidth, kImageDrawHeight);
    const std::wstring state = ParseStateArgument();
    if (!state.empty()) {
        ApplyOptions(LoadState(state));
        ::DeleteFileW(state.c_str());
        SetStatus(L"已获得管理员权限，正在继续安装...");
        Fl::add_timeout(0.20, [](void*) { StartInstall(); });
    }
}

// AsyncMain remains present for the framework entry point contract. Input args
// are unused by the installer background thread; output zero means success.
int AsyncMain(const std::vector<std::string>& args) {
    (void)args;
    return 0;
}
