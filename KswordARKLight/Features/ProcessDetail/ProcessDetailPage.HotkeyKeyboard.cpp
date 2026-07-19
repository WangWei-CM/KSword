#include "../../Core/Win32Lean.h"

#include <commctrl.h>
#include <tlhelp32.h>

#include "ProcessDetailPage.h"

#include "../../Ui/Theme.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ksword::Features::ProcessDetail {
namespace {

constexpr int kPageMargin = 6;
constexpr int kGroupInset = 8;
constexpr int kTopBarY = 28;
constexpr int kTopBarHeight = 28;
constexpr int kTableY = 62;
constexpr int kKeyboardTableY = 90;

struct HotkeyRow {
    std::wstring objectText;
    std::uint32_t hotkeyId = 0;
    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;
    std::wstring hotkeyText;
    DWORD processId = 0;
    DWORD threadId = 0;
    std::wstring processName;
    std::wstring sourceText;
    std::wstring detailText;
};

struct WindowCollectionContext {
    DWORD processId = 0;
    std::unordered_set<std::uintptr_t>* seen = nullptr;
    std::vector<HWND>* windows = nullptr;
};

std::wstring HexValue(const std::uint64_t value, const int minimumDigits = 0) {
    std::wostringstream text;
    text << L"0x" << std::uppercase << std::hex;
    if (minimumDigits > 0) {
        text << std::setfill(L'0') << std::setw(minimumDigits);
    }
    text << value;
    return text.str();
}

std::wstring ProcessNameFromPath(const std::wstring& path) {
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return path;
    }
    return path.substr(separator + 1);
}

std::wstring WindowTitle(HWND window) {
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (copied <= 0) {
        return {};
    }
    title.resize(static_cast<std::size_t>(copied));
    return title;
}

std::uint32_t HotkeyModifiersFromFlags(const std::uint32_t flags) {
    std::uint32_t modifiers = 0;
    if ((flags & HOTKEYF_ALT) != 0U) {
        modifiers |= MOD_ALT;
    }
    if ((flags & HOTKEYF_CONTROL) != 0U) {
        modifiers |= MOD_CONTROL;
    }
    if ((flags & HOTKEYF_SHIFT) != 0U) {
        modifiers |= MOD_SHIFT;
    }
    return modifiers;
}

std::wstring VirtualKeyName(const std::uint32_t virtualKey) {
    if ((virtualKey >= L'A' && virtualKey <= L'Z') ||
        (virtualKey >= L'0' && virtualKey <= L'9')) {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1U);
    }

    switch (virtualKey) {
    case VK_BACK: return L"Backspace";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_END: return L"End";
    case VK_HOME: return L"Home";
    case VK_LEFT: return L"Left";
    case VK_UP: return L"Up";
    case VK_RIGHT: return L"Right";
    case VK_DOWN: return L"Down";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_SNAPSHOT: return L"PrintScreen";
    case VK_PAUSE: return L"Pause";
    case VK_APPS: return L"Apps";
    case VK_OEM_PLUS: return L"+";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_COMMA: return L",";
    case VK_OEM_PERIOD: return L".";
    default: break;
    }

    const UINT scanCode = ::MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    if (scanCode != 0U) {
        LONG keyNameParameter = static_cast<LONG>(scanCode << 16);
        switch (virtualKey) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            keyNameParameter |= (1L << 24);
            break;
        default:
            break;
        }
        wchar_t keyName[64]{};
        if (::GetKeyNameTextW(keyNameParameter, keyName, static_cast<int>(std::size(keyName))) > 0) {
            return keyName;
        }
    }
    return L"VK_" + HexValue(virtualKey, 2);
}

std::wstring HotkeyText(const std::uint32_t modifiers, const std::uint32_t virtualKey) {
    std::wstring text;
    const auto append = [&text](const wchar_t* part) {
        if (!text.empty()) {
            text += L'+';
        }
        text += part;
    };
    if ((modifiers & MOD_CONTROL) != 0U) {
        append(L"Ctrl");
    }
    if ((modifiers & MOD_SHIFT) != 0U) {
        append(L"Shift");
    }
    if ((modifiers & MOD_ALT) != 0U) {
        append(L"Alt");
    }
    if ((modifiers & MOD_WIN) != 0U) {
        append(L"Win");
    }
    const std::wstring key = VirtualKeyName(virtualKey);
    if (!text.empty()) {
        text += L'+';
    }
    text += key;
    return text;
}

BOOL CALLBACK CollectProcessWindow(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<WindowCollectionContext*>(parameter);
    if (!context || !context->seen || !context->windows) {
        return TRUE;
    }
    DWORD ownerProcessId = 0;
    ::GetWindowThreadProcessId(window, &ownerProcessId);
    if (ownerProcessId != context->processId) {
        return TRUE;
    }
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(window);
    if (context->seen->insert(key).second) {
        context->windows->push_back(window);
    }
    return TRUE;
}

std::vector<DWORD> CollectThreadIds(const DWORD processId) {
    std::vector<DWORD> threadIds;
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return threadIds;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL hasEntry = ::Thread32First(snapshot, &entry);
         hasEntry != FALSE;
         hasEntry = ::Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == processId) {
            threadIds.push_back(entry.th32ThreadID);
        }
    }
    ::CloseHandle(snapshot);
    return threadIds;
}

std::vector<HWND> CollectProcessWindows(const DWORD processId) {
    std::vector<HWND> windows;
    std::unordered_set<std::uintptr_t> seen;
    WindowCollectionContext context{ processId, &seen, &windows };
    ::EnumWindows(CollectProcessWindow, reinterpret_cast<LPARAM>(&context));
    for (const DWORD threadId : CollectThreadIds(processId)) {
        ::EnumThreadWindows(threadId, CollectProcessWindow, reinterpret_cast<LPARAM>(&context));
    }
    return windows;
}

std::vector<HotkeyRow> CollectWindowHotkeys(
    const DWORD processId,
    const std::wstring& processName) {
    std::vector<HotkeyRow> rows;
    for (HWND window : CollectProcessWindows(processId)) {
        DWORD ownerProcessId = 0;
        const DWORD threadId = ::GetWindowThreadProcessId(window, &ownerProcessId);
        DWORD_PTR result = 0;
        if (::SendMessageTimeoutW(
                window,
                WM_GETHOTKEY,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                100,
                &result) == 0 || result == 0) {
            continue;
        }

        const std::uint32_t virtualKey = static_cast<std::uint32_t>(LOBYTE(LOWORD(result)));
        const std::uint32_t hotkeyFlags = static_cast<std::uint32_t>(HIBYTE(LOWORD(result)));
        if (virtualKey == 0U) {
            continue;
        }

        HotkeyRow row{};
        row.objectText = L"HWND=" + HexValue(reinterpret_cast<std::uintptr_t>(window));
        row.modifiers = HotkeyModifiersFromFlags(hotkeyFlags);
        row.virtualKey = virtualKey;
        row.hotkeyText = HotkeyText(row.modifiers, row.virtualKey);
        row.processId = ownerProcessId == 0 ? processId : ownerProcessId;
        row.threadId = threadId;
        row.processName = processName;
        row.sourceText = L"窗口热键";
        row.detailText = WindowTitle(window);
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const HotkeyRow& left, const HotkeyRow& right) {
        if (left.hotkeyText != right.hotkeyText) {
            return left.hotkeyText < right.hotkeyText;
        }
        if (left.sourceText != right.sourceText) {
            return left.sourceText < right.sourceText;
        }
        return left.objectText < right.objectText;
    });
    return rows;
}

std::vector<std::wstring> HotkeyCells(const HotkeyRow& row) {
    std::wostringstream vkModifiers;
    vkModifiers << L"VK=0x" << std::uppercase << std::hex << std::setfill(L'0') << std::setw(2)
                << row.virtualKey << L" MOD=0x" << std::setw(2) << row.modifiers;
    return {
        row.objectText,
        row.hotkeyId == 0U ? L"0" : HexValue(row.hotkeyId),
        row.hotkeyText,
        std::to_wstring(row.processId),
        row.threadId == 0 ? L"-" : std::to_wstring(row.threadId),
        row.processName,
        row.sourceText,
        vkModifiers.str(),
        row.detailText
    };
}

void FillHotkeyList(HWND list, const std::vector<HotkeyRow>& rows) {
    if (!list) {
        return;
    }
    ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const std::vector<std::wstring> cells = HotkeyCells(rows[index]);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<LPWSTR>(cells[0].c_str());
        const int inserted = ListView_InsertItem(list, &item);
        if (inserted < 0) {
            continue;
        }
        for (int column = 1; column < static_cast<int>(cells.size()); ++column) {
            ListView_SetItemText(list, inserted, column, const_cast<LPWSTR>(cells[column].c_str()));
        }
    }
    ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list, nullptr, TRUE);
}

void AddTooltip(HWND owner, HWND control, const wchar_t* text) {
    if (!owner || !control || !text) {
        return;
    }
    HWND tooltip = ::CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        owner,
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (!tooltip) {
        return;
    }
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = owner;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    tool.lpszText = const_cast<LPWSTR>(text);
    ::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    ::SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 720);
}

void InsertInnerTab(HWND tab, const int index, const wchar_t* title) {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(title);
    ::SendMessageW(tab, TCM_INSERTITEMW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
}

} // namespace

bool ProcessDetailPage::CreateHotkeyTab() {
    const TabIndex tab = TabIndex::Hotkeys;
    HWND group = AddGroup(tab, L"进程热键检测", kPageMargin, kPageMargin, -kPageMargin, -kPageMargin);
    HWND refresh = AddButton(
        tab,
        HotkeyRefresh,
        L"刷新热键",
        kPageMargin + kGroupInset,
        kTopBarY,
        96,
        kTopBarHeight);
    HWND status = AddLabel(
        tab,
        HotkeyStatus,
        L"● 尚未刷新",
        kPageMargin + kGroupInset + 104,
        kTopBarY,
        -(kPageMargin + kGroupInset),
        kTopBarHeight);
    HWND list = AddList(
        tab,
        HotkeyList,
        kPageMargin + kGroupInset,
        kTableY,
        -(kPageMargin + kGroupInset),
        -(kPageMargin + kGroupInset));
    if (!group || !refresh || !status || !list) {
        return false;
    }

    const std::array<std::pair<const wchar_t*, int>, 9> columns{{
        { L"对象", 260 },
        { L"热键ID", 80 },
        { L"热键", 130 },
        { L"进程ID", 80 },
        { L"线程ID", 80 },
        { L"进程名", 120 },
        { L"来源", 130 },
        { L"VK/Mod", 120 },
        { L"详情", 100 }
    }};
    for (int index = 0; index < static_cast<int>(columns.size()); ++index) {
        AddListColumn(list, index, columns[static_cast<std::size_t>(index)].first,
                      columns[static_cast<std::size_t>(index)].second);
    }
    listColumnCounts_[list] = static_cast<int>(columns.size());
    ListView_SetBkColor(list, Ksword::Ui::AppTheme().panelColor);
    ListView_SetTextBkColor(list, Ksword::Ui::AppTheme().panelColor);
    ListView_SetTextColor(list, Ksword::Ui::AppTheme().textColor);
    AddTooltip(
        pages_[static_cast<std::size_t>(tab)].hwnd,
        refresh,
        L"扫描当前进程的窗口热键、菜单快捷键、Accelerator资源、快捷方式热键和R0热键表");
    return true;
}

bool ProcessDetailPage::CreateKeyboardTab() {
    const TabIndex tab = TabIndex::Keyboard;
    HWND group = AddGroup(tab, L"键盘检测", kPageMargin, kPageMargin, -kPageMargin, -kPageMargin);
    HWND refresh = AddButton(
        tab,
        KeyboardRefresh,
        L"刷新键盘",
        kPageMargin + kGroupInset,
        kTopBarY,
        96,
        kTopBarHeight);
    HWND status = AddLabel(
        tab,
        KeyboardStatus,
        L"● 尚未刷新",
        kPageMargin + kGroupInset + 104,
        kTopBarY,
        -(kPageMargin + kGroupInset),
        kTopBarHeight);
    HWND innerTab = AddControl(
        tab,
        0,
        WC_TABCONTROLW,
        L"",
        WS_TABSTOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        KeyboardInnerTab,
        kPageMargin + kGroupInset,
        kTableY,
        -(kPageMargin + kGroupInset),
        -(kPageMargin + kGroupInset));
    HWND hotkeyList = AddList(
        tab,
        KeyboardHotkeyList,
        kPageMargin + kGroupInset + 2,
        kKeyboardTableY,
        -(kPageMargin + kGroupInset + 2),
        -(kPageMargin + kGroupInset + 2));
    HWND hookList = AddList(
        tab,
        KeyboardHookList,
        kPageMargin + kGroupInset + 2,
        kKeyboardTableY,
        -(kPageMargin + kGroupInset + 2),
        -(kPageMargin + kGroupInset + 2));
    if (!group || !refresh || !status || !innerTab || !hotkeyList || !hookList) {
        return false;
    }

    InsertInnerTab(innerTab, 0, L"热键");
    InsertInnerTab(innerTab, 1, L"键盘钩子");
    ::SendMessageW(innerTab, TCM_SETCURSEL, 0, 0);

    const std::array<std::pair<const wchar_t*, int>, 9> hotkeyColumns{{
        { L"对象", 260 },
        { L"热键ID", 80 },
        { L"热键", 130 },
        { L"进程ID", 80 },
        { L"线程ID", 80 },
        { L"进程名", 120 },
        { L"来源", 150 },
        { L"VK/Mod", 120 },
        { L"详情", 100 }
    }};
    for (int index = 0; index < static_cast<int>(hotkeyColumns.size()); ++index) {
        AddListColumn(hotkeyList, index, hotkeyColumns[static_cast<std::size_t>(index)].first,
                      hotkeyColumns[static_cast<std::size_t>(index)].second);
    }
    listColumnCounts_[hotkeyList] = static_cast<int>(hotkeyColumns.size());

    const std::array<std::pair<const wchar_t*, int>, 10> hookColumns{{
        { L"对象", 180 },
        { L"类型", 120 },
        { L"范围", 90 },
        { L"进程ID", 80 },
        { L"线程ID", 80 },
        { L"函数/偏移", 150 },
        { L"模块", 130 },
        { L"来源", 160 },
        { L"Flags", 80 },
        { L"详情", 100 }
    }};
    for (int index = 0; index < static_cast<int>(hookColumns.size()); ++index) {
        AddListColumn(hookList, index, hookColumns[static_cast<std::size_t>(index)].first,
                      hookColumns[static_cast<std::size_t>(index)].second);
    }
    listColumnCounts_[hookList] = static_cast<int>(hookColumns.size());

    for (HWND list : { hotkeyList, hookList }) {
        ListView_SetBkColor(list, Ksword::Ui::AppTheme().panelColor);
        ListView_SetTextBkColor(list, Ksword::Ui::AppTheme().panelColor);
        ListView_SetTextColor(list, Ksword::Ui::AppTheme().textColor);
    }
    ::ShowWindow(hotkeyList, SW_SHOW);
    ::ShowWindow(hookList, SW_HIDE);
    AddTooltip(
        pages_[static_cast<std::size_t>(tab)].hwnd,
        refresh,
        L"扫描热键表和 WH_KEYBOARD/WH_KEYBOARD_LL 钩子链");
    return true;
}

void ProcessDetailPage::PopulateHotkeyTab() {
    RefreshHotkeys(false);
}

void ProcessDetailPage::PopulateKeyboardTab() {
    RefreshHotkeys(true);
}

bool ProcessDetailPage::HandleHotkeyCommand(const int controlId) {
    if (controlId != HotkeyRefresh) {
        return false;
    }
    RefreshHotkeys(false);
    return true;
}

bool ProcessDetailPage::HandleKeyboardCommand(const int controlId) {
    if (controlId != KeyboardRefresh) {
        return false;
    }
    RefreshHotkeys(true);
    return true;
}

void ProcessDetailPage::RefreshHotkeys(const bool includeHooks) {
    HWND hotkeyButton = Control(TabIndex::Hotkeys, HotkeyRefresh);
    HWND keyboardButton = Control(TabIndex::Keyboard, KeyboardRefresh);
    HWND activeButton = includeHooks ? keyboardButton : hotkeyButton;
    if (activeButton && !::IsWindowEnabled(activeButton)) {
        return;
    }

    if (activeButton) {
        ::EnableWindow(activeButton, FALSE);
    }
    if (includeHooks) {
        SetPageStatus(TabIndex::Keyboard, KeyboardStatus, L"● 正在扫描键盘热键与钩子...");
        if (HWND status = Control(TabIndex::Keyboard, KeyboardStatus)) {
            ::UpdateWindow(status);
        }
    } else {
        SetPageStatus(TabIndex::Hotkeys, HotkeyStatus, L"● 正在扫描进程热键...");
        if (HWND status = Control(TabIndex::Hotkeys, HotkeyStatus)) {
            ::UpdateWindow(status);
        }
    }

    const auto started = std::chrono::steady_clock::now();
    std::wstring processName = ProcessNameFromPath(snapshot_.basic.imagePath);
    if (processName.empty()) {
        processName = L"PID " + std::to_wstring(processId_);
    }
    const std::vector<HotkeyRow> rows = CollectWindowHotkeys(processId_, processName);
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());

    FillHotkeyList(Control(TabIndex::Hotkeys, HotkeyList), rows);
    FillHotkeyList(Control(TabIndex::Keyboard, KeyboardHotkeyList), rows);
    ClearList(Control(TabIndex::Keyboard, KeyboardHookList));

    if (includeHooks) {
        std::wostringstream status;
        status << L"● 刷新完成 " << elapsed << L" ms | 热键:" << rows.size()
               << L" | 键盘钩子:0 | R3窗口热键；R0热键表/键盘钩子暂未接入";
        SetPageStatus(TabIndex::Keyboard, KeyboardStatus, status.str());
        SetPageStatus(TabIndex::Hotkeys, HotkeyStatus, status.str());
        keyboardLoaded_ = true;
        hotkeysLoaded_ = true;
    } else {
        std::wostringstream status;
        status << L"● 刷新完成 " << elapsed << L" ms | 热键:" << rows.size()
               << L" | R3窗口热键；R0热键表暂未接入";
        SetPageStatus(TabIndex::Hotkeys, HotkeyStatus, status.str());
        hotkeysLoaded_ = true;
    }

    if (activeButton) {
        ::EnableWindow(activeButton, TRUE);
    }
}

} // namespace Ksword::Features::ProcessDetail
