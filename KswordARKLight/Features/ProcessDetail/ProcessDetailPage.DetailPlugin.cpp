#include "../../Core/Win32Lean.h"

#include <commctrl.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "ProcessDetailPage.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>

namespace Ksword::Features::ProcessDetail {
namespace {

constexpr int kDetailMargin = 6;
constexpr int kDetailLabelWidth = 126;
constexpr int kDetailValueLeft = 166;

std::wstring DisplayText(const std::wstring& text, const wchar_t* fallback = L"-") {
    return text.empty() ? std::wstring(fallback) : text;
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::wstring ProcessNameById(DWORD processId) {
    if (processId == 0) {
        return {};
    }

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {};
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring name;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == processId) {
                name = entry.szExeFile;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return name;
}

std::wstring ProcessStartTime(HANDLE process) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!process || !::GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return L"-";
    }

    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!::FileTimeToSystemTime(&creation, &utc) ||
        !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return L"-";
    }

    wchar_t text[64]{};
    swprintf_s(
        text,
        L"%04u-%02u-%02u %02u:%02u:%02u",
        local.wYear,
        local.wMonth,
        local.wDay,
        local.wHour,
        local.wMinute,
        local.wSecond);
    return text;
}

std::wstring PriorityClassText(HANDLE process) {
    if (!process) {
        return L"Unknown";
    }
    switch (::GetPriorityClass(process)) {
    case IDLE_PRIORITY_CLASS: return L"Idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return L"Below normal";
    case NORMAL_PRIORITY_CLASS: return L"Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above normal";
    case HIGH_PRIORITY_CLASS: return L"High";
    case REALTIME_PRIORITY_CLASS: return L"Realtime";
    default: return L"Unknown";
    }
}

std::wstring ElevatedText(HANDLE process) {
    if (!process) {
        return L"■ 未知";
    }

    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return L"■ 未知";
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried = ::GetTokenInformation(
        token,
        TokenElevation,
        &elevation,
        sizeof(elevation),
        &returned);
    ::CloseHandle(token);
    if (!queried) {
        return L"■ 未知";
    }
    return elevation.TokenIsElevated ? L"■ 是" : L"■ 否";
}

std::wstring HandleCountText(HANDLE process) {
    DWORD count = 0;
    if (!process || !::GetProcessHandleCount(process, &count)) {
        return L"-";
    }
    return std::to_wstring(count);
}

std::wstring WorkingSetText(HANDLE process) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!process || !::GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return L"-";
    }

    std::wostringstream text;
    text << std::fixed << std::setprecision(1)
         << (static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0))
         << L" MB";
    return text.str();
}

void MakeRightAligned(HWND label) {
    if (!label) {
        return;
    }
    LONG_PTR style = ::GetWindowLongPtrW(label, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(SS_TYPEMASK);
    style |= SS_RIGHT | SS_CENTERIMAGE | SS_NOTIFY;
    ::SetWindowLongPtrW(label, GWL_STYLE, style);
}

bool AllCreated(std::initializer_list<HWND> controls) {
    return std::all_of(controls.begin(), controls.end(), [](HWND control) {
        return control != nullptr;
    });
}

} // namespace

bool ProcessDetailPage::CreateDetailTab() {
    const TabIndex tab = TabIndex::Detail;

    HWND title = AddLabel(tab, DetailTitle, L"Unknown  (PID: 0)", 6, 6, -12, 40);
    ApplyFont(title, titleFont_);

    HWND pathLabel = AddLabel(tab, 0, L"程序路径:", 6, 54, 92, 28);
    MakeRightAligned(pathLabel);
    HWND path = AddEdit(tab, DetailPath, L"-", true, false, 104, 54, -260, 28);
    HWND copyPath = AddButton(tab, DetailCopyPath, L"复制", -224, 54, 78, 28);
    HWND openFolder = AddButton(tab, DetailOpenFolder, L"打开文件夹", -138, 54, 132, 28);

    HWND commandLabel = AddLabel(tab, 0, L"启动命令行:", 6, 88, 92, 28);
    MakeRightAligned(commandLabel);
    HWND command = AddEdit(tab, DetailCommandLine, L"-", true, false, 104, 88, -162, 28);
    HWND copyCommand = AddButton(tab, DetailCopyCommand, L"复制", -84, 88, 78, 28);

    HWND parentLabel = AddLabel(tab, 0, L"父进程:", 6, 122, 92, 28);
    MakeRightAligned(parentLabel);
    HWND parentText = AddLabel(tab, DetailParentText, L"无父进程信息", 104, 122, -344, 28);
    HWND openHandles = AddButton(tab, DetailOpenHandles, L"句柄", -210, 122, 32, 28);
    HWND gotoParent = AddButton(tab, DetailGotoParent, L"转到父进程", -170, 122, 164, 28);
    ::ShowWindow(gotoParent, SW_HIDE);

    HWND detailGroup = AddGroup(tab, L"更多进程详细数据", 6, 158, -12, -12);

    struct DetailField {
        const wchar_t* label;
        int controlId;
    };
    constexpr std::array<DetailField, 12> fields{
        DetailField{ L"启动时间", DetailStartTime },
        DetailField{ L"用户", DetailUser },
        DetailField{ L"管理员", DetailAdmin },
        DetailField{ L"架构", DetailArchitecture },
        DetailField{ L"优先级", DetailPriority },
        DetailField{ L"Session ID", DetailSession },
        DetailField{ L"线程数量", DetailThreadCount },
        DetailField{ L"句柄数量", DetailHandleCount },
        DetailField{ L"CPU 占用", DetailCpu },
        DetailField{ L"RAM 占用", DetailRam },
        DetailField{ L"DISK 吞吐", DetailDisk },
        DetailField{ L"数字签名", DetailSignature }
    };

    bool fieldsCreated = true;
    int y = 184;
    for (const DetailField& field : fields) {
        HWND label = AddLabel(tab, 0, field.label, 24, y, kDetailLabelWidth, 25);
        MakeRightAligned(label);
        HWND value = AddLabel(tab, field.controlId, L"-", kDetailValueLeft, y, -188, 25);
        fieldsCreated = fieldsCreated && label && value;
        y += 30;
    }

    return fieldsCreated && AllCreated({
        title,
        pathLabel,
        path,
        copyPath,
        openFolder,
        commandLabel,
        command,
        copyCommand,
        parentLabel,
        parentText,
        openHandles,
        gotoParent,
        detailGroup
    });
}

bool ProcessDetailPage::CreatePluginTab() {
    const TabIndex tab = TabIndex::Plugins;
    HWND title = AddLabel(tab, PluginTitle, L"进程插件", 14, 14, -28, 28);
    ApplyFont(title, titleFont_);
    HWND description = AddLabel(
        tab,
        PluginDescription,
        L"选择适用于当前进程的插件进行分析。",
        14,
        50,
        -28,
        38);
    HWND pluginMenu = AddButton(tab, PluginMenu, L"插件  ▼", 14, 98, 118, 32);
    HWND pluginManager = AddButton(tab, PluginManager, L"插件管理", 142, 98, 118, 32);
    return AllCreated({ title, description, pluginMenu, pluginManager });
}

void ProcessDetailPage::PopulateDetailTab() {
    const ProcessBasicInfo& basic = snapshot_.basic;
    const std::wstring processName = [&basic]() {
        std::wstring name = FileNameFromPath(basic.imagePath);
        if (name.empty()) {
            name = ProcessNameById(basic.processId);
        }
        return name.empty() ? std::wstring(L"Unknown") : name;
    }();

    SetControlText(
        TabIndex::Detail,
        DetailTitle,
        processName + L"  (PID: " + std::to_wstring(basic.processId) + L")");
    SetControlText(TabIndex::Detail, DetailPath, DisplayText(basic.imagePath));
    SetControlText(TabIndex::Detail, DetailCommandLine, DisplayText(basic.commandLine));

    if (basic.parentProcessId == 0) {
        SetControlText(TabIndex::Detail, DetailParentText, L"无父进程信息");
        ::ShowWindow(Control(TabIndex::Detail, DetailGotoParent), SW_HIDE);
    } else {
        const std::wstring parentName = ProcessNameById(basic.parentProcessId);
        if (parentName.empty()) {
            SetControlText(
                TabIndex::Detail,
                DetailParentText,
                L"父进程已退出或不可访问 (PID: " + std::to_wstring(basic.parentProcessId) + L")");
            ::ShowWindow(Control(TabIndex::Detail, DetailGotoParent), SW_HIDE);
        } else {
            SetControlText(
                TabIndex::Detail,
                DetailParentText,
                parentName + L" (PID: " + std::to_wstring(basic.parentProcessId) + L")");
            ::ShowWindow(Control(TabIndex::Detail, DetailGotoParent), SW_SHOW);
        }
    }

    HANDLE process = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE,
        processId_);
    SetControlText(TabIndex::Detail, DetailStartTime, ProcessStartTime(process));
    SetControlText(TabIndex::Detail, DetailUser, DisplayText(basic.userName));
    SetControlText(TabIndex::Detail, DetailAdmin, ElevatedText(process));
    SetControlText(TabIndex::Detail, DetailArchitecture, DisplayText(basic.bitness, L"Unknown"));
    SetControlText(TabIndex::Detail, DetailPriority, PriorityClassText(process));
    SetControlText(TabIndex::Detail, DetailSession, std::to_wstring(basic.sessionId));
    SetControlText(TabIndex::Detail, DetailThreadCount, std::to_wstring(snapshot_.threads.size()));
    SetControlText(TabIndex::Detail, DetailHandleCount, HandleCountText(process));
    SetControlText(TabIndex::Detail, DetailCpu, L"-");
    SetControlText(TabIndex::Detail, DetailRam, WorkingSetText(process));
    SetControlText(TabIndex::Detail, DetailDisk, L"-");
    SetControlText(TabIndex::Detail, DetailSignature, L"-");
    if (process) {
        ::CloseHandle(process);
    }

    ::EnableWindow(Control(TabIndex::Detail, DetailOpenHandles), processId_ != 0);
    ::EnableWindow(
        Control(TabIndex::Detail, DetailOpenFolder),
        !basic.imagePath.empty());
}

bool ProcessDetailPage::HandleDetailCommand(int controlId) {
    switch (controlId) {
    case DetailCopyPath:
        CopyText(hwnd_, ControlText(TabIndex::Detail, DetailPath));
        return true;
    case DetailOpenFolder: {
        const std::wstring path = ControlText(TabIndex::Detail, DetailPath);
        if (path.empty() || path == L"-") {
            return true;
        }
        const std::wstring arguments = L"/select,\"" + path + L"\"";
        const HINSTANCE result = ::ShellExecuteW(
            hwnd_,
            L"open",
            L"explorer.exe",
            arguments.c_str(),
            nullptr,
            SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            ::MessageBoxW(hwnd_, L"无法打开程序所在文件夹。", L"进程详细信息", MB_OK | MB_ICONWARNING);
        }
        return true;
    }
    case DetailCopyCommand:
        CopyText(hwnd_, ControlText(TabIndex::Detail, DetailCommandLine));
        return true;
    case DetailOpenHandles:
        ::MessageBoxW(
            hwnd_,
            L"纯 Win32 详情宿主尚未提供句柄 Dock 跳转。",
            L"进程详细信息",
            MB_OK | MB_ICONINFORMATION);
        return true;
    case DetailGotoParent:
        ::MessageBoxW(
            hwnd_,
            L"纯 Win32 详情宿主尚未提供父进程窗口跳转。",
            L"进程详细信息",
            MB_OK | MB_ICONINFORMATION);
        return true;
    default:
        return false;
    }
}

bool ProcessDetailPage::HandlePluginCommand(int controlId) {
    const wchar_t* status = nullptr;
    if (controlId == PluginMenu) {
        status = L"纯 Win32 宿主未提供插件运行时，当前无法展开进程插件。";
    } else if (controlId == PluginManager) {
        status = L"纯 Win32 宿主未提供插件管理器。";
    } else {
        return false;
    }

    SetControlText(TabIndex::Plugins, PluginDescription, status);
    ::MessageBoxW(hwnd_, status, L"插件", MB_OK | MB_ICONINFORMATION);
    return true;
}

} // namespace Ksword::Features::ProcessDetail
