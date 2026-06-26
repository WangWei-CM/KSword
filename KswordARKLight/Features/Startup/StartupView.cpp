#include "StartupView.h"

#include "StartupActions.h"
#include "StartupEnumerator.h"
#include "StartupModel.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ksword::Features::Startup {
namespace {
constexpr wchar_t kStartupViewClass[] = L"KswordARKLight.Startup.FeatureView";
constexpr int kRefreshButtonId = 63001;
constexpr int kEnableButtonId = 63002;
constexpr int kDisableButtonId = 63003;
constexpr int kDeleteButtonId = 63004;
constexpr int kOpenButtonId = 63005;
constexpr int kEntryListId = 63006;
constexpr int kDetailListId = 63007;
constexpr int kHeaderHeight = 34;
constexpr int kGap = 6;
constexpr int kDetailHeight = 220;
constexpr UINT kStartupMenuEnable = 63601;
constexpr UINT kStartupMenuDisable = 63602;
constexpr UINT kStartupMenuDelete = 63603;
constexpr UINT kStartupMenuOpen = 63604;
constexpr UINT kStartupMenuCopyDetail = 63605;
constexpr UINT kStartupMenuRefresh = 63606;

// Width returns non-negative rectangle width. Input is a RECT; output is pixels
// available to this module's child controls.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns non-negative rectangle height. Input is a RECT; output is pixels
// available to this module's child controls.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// StartupViewState owns native controls and model data for one Startup page.
// Inputs are Win32 messages; processing keeps enumeration and action dispatch
// local to this module and does not couple to Hardware or Window features.
struct StartupViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND enableButton = nullptr;
    HWND disableButton = nullptr;
    HWND deleteButton = nullptr;
    HWND openButton = nullptr;
    HWND entryList = nullptr;
    HWND detailList = nullptr;
    HIMAGELIST entryImageList = nullptr;
    StartupModel model;
    std::wstring statusText;
    std::unordered_map<std::wstring, int> iconCache;
};

// AddColumn inserts a report-view column. Inputs are list HWND, column index,
// title and width; processing sends LVM_INSERTCOLUMNW; no return value.
void AddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

// SetListText inserts or updates one report-view cell. Inputs are list HWND, row,
// column, text and optional lParam for first-column rows; no value is returned.
void SetListText(HWND list, int row, int column, const std::wstring& text, LPARAM data = 0, int imageIndex = -1) {
    if (column == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        if (imageIndex >= 0) {
            item.mask |= LVIF_IMAGE;
            item.iImage = imageIndex;
        }
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(text.c_str());
        item.lParam = data;
        ListView_InsertItem(list, &item);
        return;
    }
    ListView_SetItemText(list, row, column, const_cast<LPWSTR>(text.c_str()));
}

// AddDetailRow appends one startup property row. Inputs are detail list, row,
// property name and value; no value is returned.
void AddDetailRow(HWND list, int row, const std::wstring& name, const std::wstring& value) {
    SetListText(list, row, 0, name);
    SetListText(list, row, 1, value);
}

// ListText reads one ListView cell. Inputs are list HWND, row and column;
// processing uses a bounded stack-independent buffer; output is empty for blank
// or invalid cells.
std::wstring ListText(HWND list, int row, int column) {
    if (!list || row < 0 || column < 0) {
        return {};
    }
    std::vector<wchar_t> buffer(4096, L'\0');
    ListView_GetItemText(list, row, column, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data());
}

// CopyText writes Unicode text to the clipboard. Inputs are owner HWND and text;
// processing transfers CF_UNICODETEXT; output reports success.
bool CopyText(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
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
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

// TrimQuotes removes surrounding quotes and whitespace from a command token.
// Input is a command/path fragment; output is a clean path candidate.
std::wstring TrimQuotes(std::wstring text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.pop_back();
    }
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

// ExtractExecutableCandidate best-effort parses a startup command into an icon
// target. Inputs are full command line and explicit file path; processing
// prefers real file paths and handles quoted commands; output may be empty.
std::wstring ExtractExecutableCandidate(const StartupEntry& entry) {
    if (!entry.filePath.empty()) {
        return entry.filePath;
    }
    if (!entry.disabledFilePath.empty()) {
        return entry.disabledFilePath;
    }

    std::wstring command = TrimQuotes(entry.command);
    if (command.empty()) {
        return {};
    }
    if (command.front() == L'"') {
        const std::size_t endQuote = command.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            return command.substr(1, endQuote - 1);
        }
    }
    const std::size_t exe = command.find(L".exe");
    if (exe != std::wstring::npos) {
        return TrimQuotes(command.substr(0, exe + 4));
    }
    const std::size_t firstSpace = command.find(L' ');
    return TrimQuotes(firstSpace == std::wstring::npos ? command : command.substr(0, firstSpace));
}

// AddIconFromShell extracts one small icon for a startup target. Inputs are image
// list and candidate path; processing falls back to a generic executable icon;
// output is an image-list index or -1.
int AddIconFromShell(HIMAGELIST imageList, const std::wstring& path) {
    if (imageList == nullptr) {
        return -1;
    }

    SHFILEINFOW info{};
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    const bool fallback = path.empty();
    const wchar_t* queryPath = fallback ? L".exe" : path.c_str();
    if (fallback) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }
    if (!::SHGetFileInfoW(queryPath,
            fallback ? FILE_ATTRIBUTE_NORMAL : 0,
            &info,
            sizeof(info),
            flags) || info.hIcon == nullptr) {
        return -1;
    }

    const int index = ImageList_AddIcon(imageList, info.hIcon);
    ::DestroyIcon(info.hIcon);
    return index;
}

// StartupIconIndex resolves and caches the icon for one startup entry. Inputs
// are page state and entry; output is used by the first ListView column.
int StartupIconIndex(StartupViewState* state, const StartupEntry& entry) {
    if (!state || state->entryImageList == nullptr) {
        return -1;
    }
    const std::wstring target = ExtractExecutableCandidate(entry);
    const std::wstring key = target.empty() ? L"<generic-exe>" : target;
    const auto found = state->iconCache.find(key);
    if (found != state->iconCache.end()) {
        return found->second;
    }

    int index = AddIconFromShell(state->entryImageList, target);
    if (index < 0 && !target.empty()) {
        index = AddIconFromShell(state->entryImageList, {});
    }
    state->iconCache[key] = index;
    return index;
}

// SelectedModelIndex returns the selected startup model index. Input is module
// state; output is -1 when no visible list row is selected.
int SelectedModelIndex(StartupViewState* state) {
    if (!state || !state->entryList) {
        return -1;
    }
    const int selected = ListView_GetNextItem(state->entryList, -1, LVNI_SELECTED);
    if (selected < 0) {
        return -1;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(state->entryList, &item)) {
        return -1;
    }
    return static_cast<int>(item.lParam);
}

// SelectedEntry returns the selected startup entry. Input is state; output is
// nullptr when selection is absent or stale.
const StartupEntry* SelectedEntry(StartupViewState* state) {
    return state ? state->model.entryAt(SelectedModelIndex(state)) : nullptr;
}

// ShowDetail refreshes the detail pane for one startup row. Inputs are state and
// model index; processing uses StartupModel formatting only; no return.
void ShowDetail(StartupViewState* state, int modelIndex) {
    if (!state || !state->detailList) {
        return;
    }
    ListView_DeleteAllItems(state->detailList);
    const StartupEntry* entry = state->model.entryAt(modelIndex);
    if (!entry) {
        AddDetailRow(state->detailList, 0, L"Selection", L"No startup entry selected");
        return;
    }
    const std::vector<StartupProperty> properties = state->model.propertiesForEntry(*entry);
    int row = 0;
    for (const StartupProperty& property : properties) {
        AddDetailRow(state->detailList, row++, property.name, property.value);
    }
}

// PopulateList rebuilds the main Startup list from the model. Input is module
// state; processing stores model indexes in lParam; no value is returned.
void PopulateList(StartupViewState* state) {
    if (!state || !state->entryList) {
        return;
    }
    Ksword::Ui::ScopedListViewRedrawLock redrawLock(state->entryList);
    ListView_DeleteAllItems(state->entryList);
    const auto& entries = state->model.entries();
    for (int rowIndex = 0; rowIndex < static_cast<int>(entries.size()); ++rowIndex) {
        const StartupEntry& entry = entries[rowIndex];
        SetListText(state->entryList, rowIndex, 0, state->model.textForColumn(entry, 0), rowIndex, StartupIconIndex(state, entry));
        SetListText(state->entryList, rowIndex, 1, state->model.textForColumn(entry, 1));
        SetListText(state->entryList, rowIndex, 2, state->model.textForColumn(entry, 2));
        SetListText(state->entryList, rowIndex, 3, state->model.textForColumn(entry, 3));
        SetListText(state->entryList, rowIndex, 4, state->model.textForColumn(entry, 4));
        SetListText(state->entryList, rowIndex, 5, state->model.textForColumn(entry, 5));
    }
    if (!entries.empty()) {
        ListView_SetItemState(state->entryList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ShowDetail(state, 0);
    } else {
        ShowDetail(state, -1);
    }
}

// RefreshStartupEntries performs a full startup enumeration pass. Input is state;
// processing updates the model/list and preserves only local status text; no
// value is returned.
void RefreshStartupEntries(StartupViewState* state) {
    if (!state) {
        return;
    }
    StartupEnumerationResult result = EnumerateStartupEntries();
    if (result.success) {
        state->statusText = L"Startup entries: " + std::to_wstring(result.entries.size());
        state->model.setEntries(std::move(result.entries));
    } else {
        state->statusText = result.diagnosticText;
        state->model.setEntries({});
    }
    PopulateList(state);
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// RunAction dispatches one StartupActions operation for the selected entry.
// Inputs are state and command ID; processing mutates only through
// StartupActions.* and refreshes after successful changes; no value is returned.
void RunAction(StartupViewState* state, int commandId) {
    if (!state) {
        return;
    }
    const StartupEntry* entry = SelectedEntry(state);
    if (!entry) {
        state->statusText = L"No startup entry selected.";
        ::InvalidateRect(state->hwnd, nullptr, TRUE);
        return;
    }

    StartupActionResult result;
    switch (commandId) {
    case kEnableButtonId:
        result = EnableStartupEntry(*entry);
        break;
    case kDisableButtonId:
        result = DisableStartupEntry(*entry);
        break;
    case kDeleteButtonId:
        result = DeleteStartupEntry(*entry);
        break;
    case kOpenButtonId:
        result = OpenStartupEntryLocation(*entry);
        break;
    default:
        result = { false, L"Unknown startup action." };
        break;
    }
    state->statusText = result.message;
    if (result.success && commandId != kOpenButtonId) {
        RefreshStartupEntries(state);
        return;
    }
    ShowDetail(state, SelectedModelIndex(state));
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// CopyCurrentDetail copies the visible startup detail pane. Input is state;
// processing serializes property/value rows as TSV; no value is returned.
void CopyCurrentDetail(StartupViewState* state) {
    if (!state || !state->detailList) {
        return;
    }
    std::wstring text;
    const int rows = ListView_GetItemCount(state->detailList);
    for (int row = 0; row < rows; ++row) {
        text += ListText(state->detailList, row, 0);
        text += L'\t';
        text += ListText(state->detailList, row, 1);
        text += L"\r\n";
    }
    state->statusText = CopyText(state->hwnd, text) ? L"Startup detail copied." : L"Copy startup detail failed.";
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// ShowStartupContextMenu exposes the same startup actions as the toolbar from
// the selected row. Inputs are state and screen point; processing selects the
// hit row, groups refresh/state/location/copy actions, and dispatches the chosen
// command; no value is returned.
void ShowStartupContextMenu(StartupViewState* state, POINT screenPoint) {
    if (!state || !state->entryList) {
        return;
    }
    POINT clientPoint = screenPoint;
    ::ScreenToClient(state->entryList, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int hitRow = ListView_HitTest(state->entryList, &hit);
    if (hitRow >= 0 && (ListView_GetItemState(state->entryList, hitRow, LVIS_SELECTED) & LVIS_SELECTED) == 0) {
        ListView_SetItemState(state->entryList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(state->entryList, hitRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ShowDetail(state, SelectedModelIndex(state));
    }

    const bool hasEntry = SelectedEntry(state) != nullptr;
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING, kStartupMenuRefresh, L"刷新");
    HMENU stateMenu = ::CreatePopupMenu();
    if (stateMenu) {
        ::AppendMenuW(stateMenu, MF_STRING | (hasEntry ? 0U : MF_GRAYED), kStartupMenuEnable, L"启用");
        ::AppendMenuW(stateMenu, MF_STRING | (hasEntry ? 0U : MF_GRAYED), kStartupMenuDisable, L"禁用");
        ::AppendMenuW(stateMenu, MF_STRING | (hasEntry ? 0U : MF_GRAYED), kStartupMenuDelete, L"删除");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(stateMenu), L"启动项操作");
    }
    HMENU locateMenu = ::CreatePopupMenu();
    if (locateMenu) {
        ::AppendMenuW(locateMenu, MF_STRING | (hasEntry ? 0U : MF_GRAYED), kStartupMenuOpen, L"打开位置");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(locateMenu), L"位置");
    }
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | (hasEntry ? 0U : MF_GRAYED), kStartupMenuCopyDetail, L"复制详细信息");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        state->hwnd,
        nullptr);
    ::DestroyMenu(menu);

    switch (command) {
    case kStartupMenuRefresh:
        RefreshStartupEntries(state);
        break;
    case kStartupMenuEnable:
        RunAction(state, kEnableButtonId);
        break;
    case kStartupMenuDisable:
        RunAction(state, kDisableButtonId);
        break;
    case kStartupMenuDelete:
        RunAction(state, kDeleteButtonId);
        break;
    case kStartupMenuOpen:
        RunAction(state, kOpenButtonId);
        break;
    case kStartupMenuCopyDetail:
        CopyCurrentDetail(state);
        break;
    default:
        break;
    }
}

// LayoutView positions toolbar, list, and detail controls. Input is module state;
// processing computes child bounds from the current client rectangle; no return.
void LayoutView(StartupViewState* state) {
    if (!state || !state->hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state->hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    int x = kGap;
    ::MoveWindow(state->refreshButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->enableButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->disableButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->deleteButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->openButton, x, kGap, 94, 24, TRUE);

    const int detailHeight = height > 500 ? kDetailHeight : height / 3;
    const int listTop = kHeaderHeight + kGap;
    const int listHeight = height - listTop - detailHeight - (kGap * 2);
    ::MoveWindow(state->entryList, kGap, listTop, width - (kGap * 2), listHeight, TRUE);
    ::MoveWindow(state->detailList, kGap, listTop + listHeight + kGap, width - (kGap * 2), detailHeight, TRUE);
}

// CreateChildControls creates all native controls for the Startup page. Inputs
// are module state and parent HWND; output is true when required HWNDs exist.
bool CreateChildControls(StartupViewState* state, HWND hwnd) {
    state->refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"Refresh", 0, 0, 78, 24);
    state->enableButton = Ksword::Ui::CreateButton(hwnd, kEnableButtonId, L"Enable", 0, 0, 78, 24);
    state->disableButton = Ksword::Ui::CreateButton(hwnd, kDisableButtonId, L"Disable", 0, 0, 78, 24);
    state->deleteButton = Ksword::Ui::CreateButton(hwnd, kDeleteButtonId, L"Delete", 0, 0, 78, 24);
    state->openButton = Ksword::Ui::CreateButton(hwnd, kOpenButtonId, L"Open", 0, 0, 94, 24);
    state->entryList = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEntryListId)), ::GetModuleHandleW(nullptr), nullptr);
    state->detailList = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailListId)), ::GetModuleHandleW(nullptr), nullptr);
    if (!state->refreshButton || !state->enableButton || !state->disableButton || !state->deleteButton ||
        !state->openButton || !state->entryList || !state->detailList) {
        return false;
    }

    ::SendMessageW(state->entryList, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ::SendMessageW(state->detailList, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ListView_SetExtendedListViewStyle(state->entryList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    ListView_SetExtendedListViewStyle(state->detailList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    state->entryImageList = ImageList_Create(::GetSystemMetrics(SM_CXSMICON),
        ::GetSystemMetrics(SM_CYSMICON),
        ILC_COLOR32 | ILC_MASK,
        64,
        64);
    if (state->entryImageList != nullptr) {
        ListView_SetImageList(state->entryList, state->entryImageList, LVSIL_SMALL);
    }
    AddColumn(state->entryList, 0, L"Name", 230);
    AddColumn(state->entryList, 1, L"Kind", 130);
    AddColumn(state->entryList, 2, L"Scope", 120);
    AddColumn(state->entryList, 3, L"State", 90);
    AddColumn(state->entryList, 4, L"Command", 360);
    AddColumn(state->entryList, 5, L"Location", 360);
    AddColumn(state->detailList, 0, L"Property", 170);
    AddColumn(state->detailList, 1, L"Value", 760);
    return true;
}

// RegisterStartupViewClass registers the custom Startup feature page. There is
// no input beyond process module state; output is true when CreateWindowExW can
// instantiate the page.
bool RegisterStartupViewClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        StartupViewState* state = reinterpret_cast<StartupViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<StartupViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
            return TRUE;
        }
        case WM_CREATE:
            if (state && CreateChildControls(state, hwnd)) {
                LayoutView(state);
                RefreshStartupEntries(state);
            }
            return 0;
        case WM_SIZE:
            LayoutView(state);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kRefreshButtonId) {
                RefreshStartupEntries(state);
                return 0;
            }
            if (id == kEnableButtonId || id == kDisableButtonId || id == kDeleteButtonId || id == kOpenButtonId) {
                RunAction(state, id);
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            auto* notify = reinterpret_cast<NMHDR*>(lParam);
            if (state && notify && notify->idFrom == kEntryListId && notify->code == LVN_ITEMCHANGED) {
                auto* changed = reinterpret_cast<NMLISTVIEW*>(lParam);
                if ((changed->uNewState & LVIS_SELECTED) != 0) {
                    ShowDetail(state, SelectedModelIndex(state));
                }
                return 0;
            }
            if (state && notify && notify->idFrom == kEntryListId && notify->code == NM_RCLICK) {
                POINT point{};
                ::GetCursorPos(&point);
                ShowStartupContextMenu(state, point);
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->entryList) {
                POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (point.x == -1 && point.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->entryList, &rc);
                    point = { rc.left + 20, rc.top + 20 };
                }
                ShowStartupContextMenu(state, point);
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().panelBrush());
            RECT textRc{ 430, 7, rc.right - kGap, kHeaderHeight };
            const std::wstring title = state ? state->statusText : L"Startup";
            Ksword::Ui::DrawTextLine(dc, title, textRc, Ksword::Ui::AppTheme().mutedTextColor,
                Ksword::Ui::SystemUIFont(), DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (state && state->entryImageList != nullptr) {
                ImageList_Destroy(state->entryImageList);
                state->entryImageList = nullptr;
            }
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
    wc.lpszClassName = kStartupViewClass;
    if (::RegisterClassW(&wc)) {
        return true;
    }
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

HWND CreateStartupFeatureView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterStartupViewClass()) {
        return nullptr;
    }
    auto* state = new StartupViewState();
    HWND hwnd = ::CreateWindowExW(0, kStartupViewClass, L"Startup",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, Width(bounds), Height(bounds), parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::Startup
