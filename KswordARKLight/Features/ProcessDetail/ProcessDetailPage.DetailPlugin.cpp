#include "../../Core/Win32Lean.h"

#include <commctrl.h>
#include <shellapi.h>

#include "ProcessDetailPage.h"

#include <algorithm>
#include <array>
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

std::wstring WorkingSetText(const ULONGLONG bytes, const bool known) {
    if (!known) {
        return L"-";
    }
    std::wostringstream text;
    text << std::fixed << std::setprecision(1)
         << (static_cast<double>(bytes) / (1024.0 * 1024.0))
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

void ProcessDetailPage::PopulateDetailTab() {
    const ProcessBasicInfo& basic = snapshot_.basic;
    const std::wstring processName = DisplayText(basic.processName, L"Unknown");

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
        if (basic.parentProcessName.empty()) {
            SetControlText(
                TabIndex::Detail,
                DetailParentText,
                L"父进程已退出或不可访问 (PID: " + std::to_wstring(basic.parentProcessId) + L")");
            ::ShowWindow(Control(TabIndex::Detail, DetailGotoParent), SW_HIDE);
        } else {
            SetControlText(
                TabIndex::Detail,
                DetailParentText,
                basic.parentProcessName + L" (PID: " + std::to_wstring(basic.parentProcessId) + L")");
            ::ShowWindow(Control(TabIndex::Detail, DetailGotoParent), SW_SHOW);
        }
    }

    SetControlText(TabIndex::Detail, DetailStartTime, DisplayText(basic.startTimeText));
    SetControlText(TabIndex::Detail, DetailUser, DisplayText(basic.userName));
    SetControlText(
        TabIndex::Detail,
        DetailAdmin,
        basic.adminKnown ? (basic.isAdmin ? L"■ 是" : L"■ 否") : L"■ 未知");
    SetControlText(TabIndex::Detail, DetailArchitecture, DisplayText(basic.bitness, L"Unknown"));
    SetControlText(TabIndex::Detail, DetailPriority, DisplayText(basic.priorityText, L"Unknown"));
    SetControlText(TabIndex::Detail, DetailSession, std::to_wstring(basic.sessionId));
    SetControlText(TabIndex::Detail, DetailThreadCount, std::to_wstring(LatestThreadCount()));
    SetControlText(
        TabIndex::Detail,
        DetailHandleCount,
        snapshot_.basicSucceeded ? std::to_wstring(basic.handleCount) : L"-");
    SetControlText(TabIndex::Detail, DetailCpu, L"-");
    SetControlText(TabIndex::Detail, DetailRam, WorkingSetText(basic.workingSetBytes, snapshot_.basicSucceeded));
    SetControlText(TabIndex::Detail, DetailDisk, L"-");
    SetControlText(TabIndex::Detail, DetailSignature, L"-");

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

} // namespace Ksword::Features::ProcessDetail
