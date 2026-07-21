#include "ProcessDetailPage.h"

#include "../Process/ProcessActions.h"

#include <commdlg.h>
#include <tlhelp32.h>

#include <array>
#include <string>
#include <vector>

namespace Ksword::Features::ProcessDetail {
namespace {

using Ksword::Features::Process::ProcessActionId;

constexpr LPARAM kTerminateModeMultiMethod = 1;
constexpr LPARAM kTerminateModeWin32 = 2;
constexpr LPARAM kTerminateModeAllThreads = 3;

void AddComboItem(HWND combo, const wchar_t* text, LPARAM data) {
    const LRESULT index = ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    if (index >= 0) {
        ::SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), data);
    }
}

bool ConfirmDanger(HWND owner, const wchar_t* text) {
    return ::MessageBoxW(owner, text, L"高风险操作确认", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES;
}

bool TerminateAllThreads(DWORD processId, std::wstring& detail) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        detail = L"无法创建线程快照。";
        return false;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int succeeded = 0;
    int failed = 0;
    if (::Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId) {
                continue;
            }
            HANDLE thread = ::OpenThread(THREAD_TERMINATE, FALSE, entry.th32ThreadID);
            if (thread && ::TerminateThread(thread, 1)) {
                ++succeeded;
            } else {
                ++failed;
            }
            if (thread) {
                ::CloseHandle(thread);
            }
        } while (::Thread32Next(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    detail = L"TerminateThread 完成：成功 " + std::to_wstring(succeeded) +
        L"，失败 " + std::to_wstring(failed) + L"。";
    return succeeded > 0 && failed == 0;
}

} // namespace

bool ProcessDetailPage::CreateActionTab() {
    const TabIndex tab = TabIndex::Actions;

    AddGroup(tab, L"结束与控制", 6, 6, -6, 196);
    AddLabel(tab, 0, L"结束方案", 18, 32, 88, 28);
    HWND terminateMode = AddCombo(tab, ActionTerminateMode, 110, 30, -104, 220);
    AddButton(tab, ActionTerminate, L"执行", -86, 30, 74, 32);
    AddComboItem(terminateMode, L"结束进程(组合方法链)", kTerminateModeMultiMethod);
    AddComboItem(terminateMode, L"TerminateProcess", kTerminateModeWin32);
    AddComboItem(terminateMode, L"TerminateThread(全部线程)", kTerminateModeAllThreads);
    ::SendMessageW(terminateMode, CB_SETCURSEL, 0, 0);

    AddLabel(tab, 0, L"运行控制", 18, 70, 88, 28);
    AddButton(tab, ActionSuspend, L"挂起", 110, 68, 82, 32);
    AddButton(tab, ActionResume, L"恢复", 200, 68, 82, 32);
    AddLabel(tab, 0, L"关键进程", 18, 108, 88, 28);
    AddButton(tab, ActionSetCritical, L"设为关键", 110, 106, 96, 32);
    AddButton(tab, ActionClearCritical, L"取消关键", 214, 106, 96, 32);
    AddLabel(tab, 0, L"优先级", 18, 146, 88, 28);
    HWND priority = AddCombo(tab, ActionPriority, 110, 144, -104, 220);
    AddButton(tab, ActionApplyPriority, L"应用", -86, 144, 74, 32);
    for (const wchar_t* item : { L"Idle", L"Below Normal", L"Normal", L"Above Normal", L"High", L"Realtime" }) {
        AddComboItem(priority, item, 0);
    }
    ::SendMessageW(priority, CB_SETCURSEL, 2, 0);

    AddGroup(tab, L"右键菜单同步能力", 6, 212, -6, 152);
    AddLabel(tab, 0, L"辅助", 18, 238, 88, 28);
    AddButton(tab, ActionOpenFolder, L"打开目录", 110, 236, 94, 32);
    AddButton(tab, ActionRefreshPpl, L"刷新PPL", 212, 236, 94, 32);
    AddLabel(tab, 0, L"效率模式", 18, 276, 88, 28);
    AddButton(tab, ActionEfficiencyOn, L"开效率", 110, 274, 94, 32);
    AddButton(tab, ActionEfficiencyOff, L"关效率", 212, 274, 94, 32);
    AddLabel(tab, 0, L"R0", 18, 314, 88, 28);
    AddButton(tab, ActionR0Terminate, L"R0结束", 110, 312, 92, 32);
    AddButton(tab, ActionR0Suspend, L"R0挂起", 210, 312, 92, 32);
    AddButton(tab, ActionR0Ppl, L"R0 PPL", 310, 312, 92, 32);
    AddButton(tab, ActionR0Hide, L"R0隐藏", 410, 312, 92, 32);
    AddButton(tab, ActionR0Danger, L"R0危险", 510, 312, 92, 32);

    AddGroup(tab, L"注入与载入", 6, 374, -6, 144);
    AddLabel(tab, 0, L"模式", 18, 400, 88, 28);
    HWND injectionMode = AddCombo(tab, ActionInjectionMode, 110, 398, -12, 220);
    AddComboItem(injectionMode, L"R3", 0);
    AddComboItem(injectionMode, L"R0驱动", 1);
    ::SendMessageW(injectionMode, CB_SETCURSEL, 0, 0);
    AddLabel(tab, 0, L"DLL", 18, 438, 88, 28);
    HWND dllPath = AddEdit(tab, ActionDllPath, L"", false, false, 110, 436, -184, 28);
    ::SendMessageW(dllPath, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"请选择要注入的 DLL 路径"));
    AddButton(tab, ActionBrowseDll, L"浏览", -166, 434, 72, 32);
    AddButton(tab, ActionInjectDll, L"注入", -86, 434, 72, 32);
    AddLabel(tab, 0, L"Shellcode", 18, 476, 88, 28);
    HWND shellcodePath = AddEdit(tab, ActionShellcodePath, L"", false, false, 110, 474, -184, 28);
    ::SendMessageW(shellcodePath, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"请选择原始 shellcode 二进制文件"));
    AddButton(tab, ActionBrowseShellcode, L"浏览", -166, 472, 72, 32);
    AddButton(tab, ActionInjectShellcode, L"执行", -86, 472, 72, 32);

    return terminateMode && priority && injectionMode && dllPath && shellcodePath;
}

bool ProcessDetailPage::HandleActionCommand(int controlId) {
    switch (controlId) {
    case ActionTerminate: {
        HWND combo = Control(TabIndex::Actions, ActionTerminateMode);
        const int selected = static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
        const LPARAM mode = selected >= 0
            ? ::SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0)
            : -1;
        if (mode == kTerminateModeAllThreads) {
            if (!ConfirmDanger(hwnd_, L"将逐个终止目标进程的全部线程。该操作不可撤销，是否继续？")) {
                return true;
            }
            std::wstring detail;
            const bool ok = TerminateAllThreads(processId_, detail);
            ::MessageBoxW(hwnd_, detail.c_str(), L"结束进程", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
            return true;
        }
        if (mode == kTerminateModeMultiMethod) {
            if (!ConfirmDanger(hwnd_, L"将按多种结束方法依次处理目标进程。未保存的数据会丢失，是否继续？")) {
                return true;
            }
            ExecuteProcessAction(static_cast<int>(ProcessActionId::TerminateProcessMultiMethod));
            return true;
        }
        if (!ConfirmDanger(hwnd_, L"即将结束目标进程。未保存的数据会丢失，是否继续？")) {
            return true;
        }
        ExecuteProcessAction(static_cast<int>(ProcessActionId::TerminateProcess));
        return true;
    }
    case ActionSuspend: ExecuteProcessAction(static_cast<int>(ProcessActionId::SuspendProcess)); return true;
    case ActionResume: ExecuteProcessAction(static_cast<int>(ProcessActionId::ResumeProcess)); return true;
    case ActionSetCritical:
        if (ConfirmDanger(hwnd_, L"把普通进程设为关键进程可能导致系统蓝屏，是否继续？")) {
            ExecuteProcessAction(static_cast<int>(ProcessActionId::SetCriticalProcess));
        }
        return true;
    case ActionClearCritical: ExecuteProcessAction(static_cast<int>(ProcessActionId::ClearCriticalProcess)); return true;
    case ActionApplyPriority: {
        const int selected = static_cast<int>(::SendMessageW(Control(TabIndex::Actions, ActionPriority), CB_GETCURSEL, 0, 0));
        constexpr std::array<ProcessActionId, 6> actions{
            ProcessActionId::SetPriorityIdle,
            ProcessActionId::SetPriorityBelowNormal,
            ProcessActionId::SetPriorityNormal,
            ProcessActionId::SetPriorityAboveNormal,
            ProcessActionId::SetPriorityHigh,
            ProcessActionId::SetPriorityRealtime
        };
        if (selected >= 0 && selected < static_cast<int>(actions.size())) {
            ExecuteProcessAction(static_cast<int>(actions[static_cast<std::size_t>(selected)]));
        }
        return true;
    }
    case ActionOpenFolder: ExecuteProcessAction(static_cast<int>(ProcessActionId::OpenFolder)); return true;
    case ActionRefreshPpl: ExecuteProcessAction(static_cast<int>(ProcessActionId::RefreshPplProtectionLevel)); return true;
    case ActionEfficiencyOn: ExecuteProcessAction(static_cast<int>(ProcessActionId::EnableEfficiencyMode)); return true;
    case ActionEfficiencyOff: ExecuteProcessAction(static_cast<int>(ProcessActionId::DisableEfficiencyMode)); return true;
    case ActionR0Terminate: ExecuteProcessAction(static_cast<int>(ProcessActionId::R0TerminateProcess)); return true;
    case ActionR0Suspend: ExecuteProcessAction(static_cast<int>(ProcessActionId::R0SuspendProcess)); return true;
    case ActionR0Ppl: {
        HMENU menu = ::CreatePopupMenu();
        const std::array<std::pair<const wchar_t*, ProcessActionId>, 7> items{{
            { L"关闭PPL保护 (0x00)", ProcessActionId::R0SetPplNone },
            { L"Authenticode [0x11]", ProcessActionId::R0SetPplAuthenticode },
            { L"CodeGen [0x21]", ProcessActionId::R0SetPplCodeGen },
            { L"Antimalware [0x31]", ProcessActionId::R0SetPplAntimalware },
            { L"Lsa [0x41]", ProcessActionId::R0SetPplLsa },
            { L"Windows [0x51]", ProcessActionId::R0SetPplWindows },
            { L"WinTcb [0x61]", ProcessActionId::R0SetPplWinTcb }
        }};
        for (std::size_t i = 0; i < items.size(); ++i) {
            ::AppendMenuW(menu, MF_STRING, 1 + static_cast<UINT>(i), items[i].first);
        }
        RECT button{}; ::GetWindowRect(Control(TabIndex::Actions, ActionR0Ppl), &button);
        const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD, button.left, button.bottom, 0, hwnd_, nullptr);
        ::DestroyMenu(menu);
        if (command && ConfirmDanger(hwnd_, L"修改 PPL 字段依赖正确的内核偏移，错误操作可能导致系统崩溃。是否继续？")) {
            ExecuteProcessAction(static_cast<int>(items[command - 1].second));
        }
        return true;
    }
    case ActionR0Hide: {
        HMENU menu = ::CreatePopupMenu();
        const std::array<std::pair<const wchar_t*, ProcessActionId>, 5> items{{
            { L"隐藏：只断链", ProcessActionId::R0HideUnlinkOnly },
            { L"隐藏：只改PID", ProcessActionId::R0HidePatchPidOnly },
            { L"隐藏：改PID+断链(高风险)", ProcessActionId::R0HideLegacyBoth },
            { L"取消隐藏", ProcessActionId::R0UnhideProcess },
            { L"清空全部隐藏标记", ProcessActionId::R0ClearHiddenMarks }
        }};
        for (std::size_t i = 0; i < items.size(); ++i) {
            ::AppendMenuW(menu, MF_STRING, 1 + static_cast<UINT>(i), items[i].first);
        }
        RECT button{}; ::GetWindowRect(Control(TabIndex::Actions, ActionR0Hide), &button);
        const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD, button.left, button.bottom, 0, hwnd_, nullptr);
        ::DestroyMenu(menu);
        if (command && ConfirmDanger(hwnd_, L"DKOM 隐藏操作存在竞态和系统崩溃风险，是否继续？")) {
            ExecuteProcessAction(static_cast<int>(items[command - 1].second));
        }
        return true;
    }
    case ActionR0Danger: {
        HMENU menu = ::CreatePopupMenu();
        const std::array<std::pair<const wchar_t*, ProcessActionId>, 4> items{{
            { L"启用 BreakOnTermination", ProcessActionId::R0EnableBreakOnTermination },
            { L"关闭 BreakOnTermination", ProcessActionId::R0DisableBreakOnTermination },
            { L"禁止APC插入(现有线程)", ProcessActionId::R0DisableApcInsertion },
            { L"DKOM从PspCidTable删除", ProcessActionId::R0DkomRemoveFromCidTable }
        }};
        for (std::size_t i = 0; i < items.size(); ++i) {
            ::AppendMenuW(menu, MF_STRING, 1 + static_cast<UINT>(i), items[i].first);
        }
        RECT button{}; ::GetWindowRect(Control(TabIndex::Actions, ActionR0Danger), &button);
        const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD, button.left, button.bottom, 0, hwnd_, nullptr);
        ::DestroyMenu(menu);
        if (command && ConfirmDanger(hwnd_, L"该 R0 操作可能造成进程不可恢复或系统崩溃，是否继续？")) {
            ExecuteProcessAction(static_cast<int>(items[command - 1].second));
        }
        return true;
    }
    case ActionBrowseDll: BrowseForPayload(true); return true;
    case ActionBrowseShellcode: BrowseForPayload(false); return true;
    case ActionInjectDll:
    case ActionInjectShellcode: {
        const bool dllMode = controlId == ActionInjectDll;
        const int mode = static_cast<int>(::SendMessageW(Control(TabIndex::Actions, ActionInjectionMode), CB_GETCURSEL, 0, 0));
        const std::wstring path = ControlText(TabIndex::Actions, dllMode ? ActionDllPath : ActionShellcodePath);
        if (path.empty()) {
            ::MessageBoxW(hwnd_, L"请先选择载入文件。", L"注入与载入", MB_OK | MB_ICONWARNING);
            return true;
        }
        if (mode == 0) {
            ::MessageBoxW(hwnd_, L"Light 纯 Win32 版本尚未提供 R3 注入后端。", L"注入与载入", MB_OK | MB_ICONINFORMATION);
            return true;
        }
        const auto result = dllMode
            ? Ksword::Features::Process::ExecuteR0ProcessDllInjection({ processId_ }, path)
            : Ksword::Features::Process::ExecuteR0ProcessShellcodeInjection({ processId_ }, path);
        ::MessageBoxW(hwnd_, result.detail.c_str(), result.title.c_str(), MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONWARNING));
        return true;
    }
    default:
        return false;
    }
}

void ProcessDetailPage::ExecuteProcessAction(int actionId) {
    using namespace Ksword::Features::Process;
    ProcessSnapshotRow row{};
    row.processId = processId_;
    row.parentProcessId = snapshot_.basic.parentProcessId;
    row.imageName = snapshot_.basic.processName;
    row.imagePath = snapshot_.basic.imagePath;
    const ProcessActionResult result = Ksword::Features::Process::ExecuteProcessAction(
        static_cast<ProcessActionId>(actionId), { processId_ }, { row });
    if (!result.detail.empty()) {
        ::MessageBoxW(hwnd_, result.detail.c_str(), result.title.c_str(), MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    if (result.success) {
        RefreshAll();
    }
}

void ProcessDetailPage::BrowseForPayload(bool dllMode) {
    wchar_t path[MAX_PATH * 4]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrTitle = dllMode ? L"选择要注入的 DLL" : L"选择原始 Shellcode 文件";
    dialog.lpstrFilter = dllMode
        ? L"DLL Files (*.dll)\0*.dll\0All Files (*.*)\0*.*\0\0"
        : L"Binary Files (*.bin;*.dat)\0*.bin;*.dat\0All Files (*.*)\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (::GetOpenFileNameW(&dialog)) {
        SetControlText(TabIndex::Actions, dllMode ? ActionDllPath : ActionShellcodePath, path);
    }
}

} // namespace Ksword::Features::ProcessDetail
