#include "WindowView.h"

#include "WindowActions.h"
#include "WindowEnumerator.h"
#include "WindowModel.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <memory>
#include <string>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ksword::Features::Window {
namespace {
constexpr wchar_t kWindowViewClass[] = L"KswordARKLight.Window.FeatureView";
constexpr int kRefreshButtonId = 62001;
constexpr int kFrontButtonId = 62002;
constexpr int kRestoreButtonId = 62003;
constexpr int kMinimizeButtonId = 62004;
constexpr int kMaximizeButtonId = 62005;
constexpr int kCloseButtonId = 62006;
constexpr int kWindowListId = 62007;
constexpr int kDetailListId = 62008;
constexpr int kSortComboId = 62009;
constexpr int kHeaderHeight = 34;
constexpr int kGap = 6;
constexpr int kDetailHeight = 210;
constexpr UINT kWindowMenuRefreshDetail = 62601;
constexpr UINT kWindowMenuCopyDetail = 62602;
constexpr UINT kWindowMenuFront = 62603;
constexpr UINT kWindowMenuRestore = 62604;
constexpr UINT kWindowMenuMinimize = 62605;
constexpr UINT kWindowMenuMaximize = 62606;
constexpr UINT kWindowMenuClose = 62607;

// Width returns non-negative rectangle width. Input is a RECT; output is pixels
// available for child controls.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns non-negative rectangle height. Input is a RECT; output is pixels
// available for child controls.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// WindowViewState owns all HWNDs and the current model for one window page.
// Inputs are Win32 messages; processing is isolated to the Window module and no
// desktop-management data is stored here.
struct WindowViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND frontButton = nullptr;
    HWND restoreButton = nullptr;
    HWND minimizeButton = nullptr;
    HWND maximizeButton = nullptr;
    HWND closeButton = nullptr;
    HWND sortCombo = nullptr;
    HWND windowList = nullptr;
    HWND detailList = nullptr;
    HIMAGELIST processImageList = nullptr;
    WindowModel model;
    std::wstring statusText;
    std::unordered_map<std::wstring, int> processIconCache;
};

// AddColumn inserts one report-view column. Inputs are list HWND, column index,
// title and width; processing sends LVM_INSERTCOLUMNW; no value is returned.
void AddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

// SetListText inserts or updates a report-view cell. Inputs are list HWND, row,
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

// AddDetailRow appends one property/value row. Inputs are detail list HWND, row,
// property name and value; no value is returned.
void AddDetailRow(HWND list, int row, const std::wstring& name, const std::wstring& value) {
    SetListText(list, row, 0, name);
    SetListText(list, row, 1, value);
}

// ListText returns one ListView cell as UTF-16 text. Inputs are control, row and
// column indexes; processing uses a bounded local buffer; output is empty on
// invalid input or blank cells.
std::wstring ListText(HWND list, int row, int column) {
    if (!list || row < 0 || column < 0) {
        return {};
    }
    std::vector<wchar_t> buffer(4096, L'\0');
    LVITEMW item{};
    item.iSubItem = column;
    item.cchTextMax = static_cast<int>(buffer.size());
    item.pszText = buffer.data();
    ListView_GetItemText(list, row, column, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data());
}

// CopyText writes Unicode text to the clipboard. Inputs are owner HWND and text;
// processing transfers CF_UNICODETEXT; output reports success to callers that
// update the status line.
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

// AddIconFromShell extracts one small executable icon. Inputs are an image list
// and process image path; processing falls back to the generic .exe icon for
// inaccessible processes; output is the image-list index or -1 on failure.
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

// ProcessIconIndex resolves and caches the icon for a window row's owning
// process. Inputs are page state and a window row; output is the image-list index
// used by the PID/process column.
int ProcessIconIndex(WindowViewState* state, const WindowSnapshotRow& row) {
    if (!state || state->processImageList == nullptr) {
        return -1;
    }

    const std::wstring key = row.processImagePath.empty() ? L"<generic-exe>" : row.processImagePath;
    const auto found = state->processIconCache.find(key);
    if (found != state->processIconCache.end()) {
        return found->second;
    }

    int index = AddIconFromShell(state->processImageList, row.processImagePath);
    if (index < 0 && !row.processImagePath.empty()) {
        index = AddIconFromShell(state->processImageList, {});
    }
    state->processIconCache[key] = index;
    return index;
}

// SelectedModelIndex returns the currently selected model row. Input is module
// state; output is -1 when no visible list row is selected.
int SelectedModelIndex(WindowViewState* state) {
    if (!state || !state->windowList) {
        return -1;
    }
    const int selected = ListView_GetNextItem(state->windowList, -1, LVNI_SELECTED);
    if (selected < 0) {
        return -1;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(state->windowList, &item)) {
        return -1;
    }
    return static_cast<int>(item.lParam);
}

// SelectedWindow returns the selected HWND after model lookup. Input is module
// state; output is nullptr when no row is selected or the model index is stale.
HWND SelectedWindow(WindowViewState* state) {
    const WindowSnapshotRow* row = state ? state->model.rowAt(SelectedModelIndex(state)) : nullptr;
    return row ? row->hwnd : nullptr;
}

// ShowDetail refreshes the detail list for one model row. Inputs are state and
// model index; processing queries live HWND details with cached fallback; no
// value is returned.
void ShowDetail(WindowViewState* state, int modelIndex) {
    if (!state || !state->detailList) {
        return;
    }
    ListView_DeleteAllItems(state->detailList);
    const WindowSnapshotRow* row = state->model.rowAt(modelIndex);
    if (!row) {
        AddDetailRow(state->detailList, 0, L"Selection", L"No window selected");
        return;
    }

    WindowDetail detail = QueryWindowDetails(row->hwnd);
    if (!detail.found) {
        detail = state->model.detailFromRow(*row);
    }
    int detailRow = 0;
    AddDetailRow(state->detailList, detailRow++, L"Window", detail.title);
    for (const WindowProperty& property : detail.properties) {
        AddDetailRow(state->detailList, detailRow++, property.name, property.value);
    }
}

// PopulateList rebuilds the main window list from the model. Input is module
// state; processing keeps HWND data in item lParam only as model indexes; no
// value is returned.
void PopulateList(WindowViewState* state) {
    if (!state || !state->windowList) {
        return;
    }
    Ksword::Ui::ScopedListViewRedrawLock redrawLock(state->windowList);
    ListView_DeleteAllItems(state->windowList);
    const auto& rows = state->model.rows();
    for (int rowIndex = 0; rowIndex < static_cast<int>(rows.size()); ++rowIndex) {
        const WindowSnapshotRow& row = rows[rowIndex];
        SetListText(state->windowList, rowIndex, 0, state->model.textForColumn(row, 0), rowIndex, ProcessIconIndex(state, row));
        SetListText(state->windowList, rowIndex, 1, state->model.textForColumn(row, 1));
        SetListText(state->windowList, rowIndex, 2, state->model.textForColumn(row, 2));
        SetListText(state->windowList, rowIndex, 3, state->model.textForColumn(row, 3));
        SetListText(state->windowList, rowIndex, 4, state->model.textForColumn(row, 4));
    }
    if (!rows.empty()) {
        ListView_SetItemState(state->windowList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ShowDetail(state, 0);
    } else {
        ShowDetail(state, -1);
    }
}

// RefreshWindows performs one EnumWindows snapshot. Input is module state;
// processing updates the model and list; no value is returned.
void RefreshWindows(WindowViewState* state) {
    if (!state) {
        return;
    }
    WindowEnumerationResult result = EnumerateTopLevelWindows();
    if (result.success) {
        state->statusText = L"Windows: " + std::to_wstring(result.rows.size());
        state->model.setRows(std::move(result.rows));
    } else {
        state->statusText = result.diagnosticText;
        state->model.setRows({});
    }
    PopulateList(state);
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// UpdateSortModeFromCombo reads the toolbar combo-box and rebuilds the list
// without re-enumerating windows. Input is the page state; processing maps combo
// index 0 to stacking order and index 1 to process order; no value is returned.
void UpdateSortModeFromCombo(WindowViewState* state) {
    if (!state || !state->sortCombo) {
        return;
    }

    const LRESULT selected = ::SendMessageW(state->sortCombo, CB_GETCURSEL, 0, 0);
    const WindowSortMode mode = selected == 1 ? WindowSortMode::ProcessOrder : WindowSortMode::StackingOrder;
    state->model.setSortMode(mode);
    PopulateList(state);
    state->statusText = mode == WindowSortMode::ProcessOrder ? L"Windows: 按进程顺序排序" : L"Windows: 按堆叠顺序排序";
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// RunAction executes one centralized WindowActions operation against the current
// selection. Inputs are state and action command ID; processing delegates to
// WindowActions.* and refreshes status text; no value is returned.
void RunAction(WindowViewState* state, int commandId) {
    if (!state) {
        return;
    }
    HWND hwnd = SelectedWindow(state);
    WindowActionResult result;
    switch (commandId) {
    case kFrontButtonId:
        result = BringWindowToFront(hwnd);
        break;
    case kRestoreButtonId:
        result = RestoreWindow(hwnd);
        break;
    case kMinimizeButtonId:
        result = MinimizeWindow(hwnd);
        break;
    case kMaximizeButtonId:
        result = MaximizeWindow(hwnd);
        break;
    case kCloseButtonId:
        result = CloseWindowGracefully(hwnd);
        break;
    default:
        result = { false, L"Unknown action." };
        break;
    }
    state->statusText = result.message;
    ShowDetail(state, SelectedModelIndex(state));
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// CopyCurrentDetail serializes the current detail pane as tab-separated text.
// Input is the page state; processing reads the visible detail ListView rows;
// no value is returned because statusText stores the outcome for painting.
void CopyCurrentDetail(WindowViewState* state) {
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
    state->statusText = CopyText(state->hwnd, text) ? L"Window detail copied." : L"Copy window detail failed.";
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// ShowWindowContextMenu exposes the retained Window page actions from the row
// itself. Inputs are page state and screen coordinates; processing selects the
// hit row when needed, groups detail/window actions into submenus, then
// dispatches the chosen menu command; no value is returned.
void ShowWindowContextMenu(WindowViewState* state, POINT screenPoint) {
    if (!state || !state->windowList) {
        return;
    }

    POINT clientPoint = screenPoint;
    ::ScreenToClient(state->windowList, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int hitRow = ListView_HitTest(state->windowList, &hit);
    if (hitRow >= 0 && (ListView_GetItemState(state->windowList, hitRow, LVIS_SELECTED) & LVIS_SELECTED) == 0) {
        ListView_SetItemState(state->windowList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(state->windowList, hitRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ShowDetail(state, SelectedModelIndex(state));
    }

    const bool hasWindow = SelectedWindow(state) != nullptr;
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    HMENU detailMenu = ::CreatePopupMenu();
    if (detailMenu) {
        ::AppendMenuW(detailMenu, MF_STRING | (hasWindow ? 0U : MF_GRAYED), kWindowMenuRefreshDetail, L"刷新详细信息");
        ::AppendMenuW(detailMenu, MF_STRING | (hasWindow ? 0U : MF_GRAYED), kWindowMenuCopyDetail, L"复制详细信息");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(detailMenu), L"详细信息");
    }
    HMENU operationMenu = ::CreatePopupMenu();
    if (operationMenu) {
        ::AppendMenuW(operationMenu, MF_STRING | (hasWindow ? 0U : MF_GRAYED), kWindowMenuFront, L"置前");
        ::AppendMenuW(operationMenu, MF_STRING | (hasWindow ? 0U : MF_GRAYED), kWindowMenuRestore, L"恢复");
        ::AppendMenuW(operationMenu, MF_STRING | (hasWindow ? 0U : MF_GRAYED), kWindowMenuMinimize, L"最小化");
        ::AppendMenuW(operationMenu, MF_STRING | (hasWindow ? 0U : MF_GRAYED), kWindowMenuMaximize, L"最大化");
        ::AppendMenuW(operationMenu, MF_STRING | (hasWindow ? 0U : MF_GRAYED), kWindowMenuClose, L"关闭窗口");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(operationMenu), L"窗口操作");
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
    case kWindowMenuRefreshDetail:
        ShowDetail(state, SelectedModelIndex(state));
        state->statusText = L"Window detail refreshed.";
        ::InvalidateRect(state->hwnd, nullptr, TRUE);
        break;
    case kWindowMenuCopyDetail:
        CopyCurrentDetail(state);
        break;
    case kWindowMenuFront:
        RunAction(state, kFrontButtonId);
        break;
    case kWindowMenuRestore:
        RunAction(state, kRestoreButtonId);
        break;
    case kWindowMenuMinimize:
        RunAction(state, kMinimizeButtonId);
        break;
    case kWindowMenuMaximize:
        RunAction(state, kMaximizeButtonId);
        break;
    case kWindowMenuClose:
        RunAction(state, kCloseButtonId);
        break;
    default:
        break;
    }
}

// LayoutView positions toolbar, list, and detail panes. Input is state; no value
// is returned after MoveWindow calls are issued.
void LayoutView(WindowViewState* state) {
    if (!state || !state->hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state->hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    int x = kGap;
    ::MoveWindow(state->sortCombo, x, kGap, 150, 160, TRUE); x += 156;
    ::MoveWindow(state->refreshButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->frontButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->restoreButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->minimizeButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->maximizeButton, x, kGap, 78, 24, TRUE); x += 84;
    ::MoveWindow(state->closeButton, x, kGap, 78, 24, TRUE);

    const int detailHeight = height > 480 ? kDetailHeight : height / 3;
    const int listTop = kHeaderHeight + kGap;
    const int listHeight = height - listTop - detailHeight - (kGap * 2);
    ::MoveWindow(state->windowList, kGap, listTop, width - (kGap * 2), listHeight, TRUE);
    ::MoveWindow(state->detailList, kGap, listTop + listHeight + kGap, width - (kGap * 2), detailHeight, TRUE);
}

// CreateChildControls creates all native controls for the Window page. Inputs are
// state and parent HWND; output is true when every required HWND was created.
bool CreateChildControls(WindowViewState* state, HWND hwnd) {
    state->sortCombo = ::CreateWindowExW(0,
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0,
        0,
        150,
        160,
        hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSortComboId)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    state->refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"Refresh", 0, 0, 78, 24);
    state->frontButton = Ksword::Ui::CreateButton(hwnd, kFrontButtonId, L"Front", 0, 0, 78, 24);
    state->restoreButton = Ksword::Ui::CreateButton(hwnd, kRestoreButtonId, L"Restore", 0, 0, 78, 24);
    state->minimizeButton = Ksword::Ui::CreateButton(hwnd, kMinimizeButtonId, L"Minimize", 0, 0, 78, 24);
    state->maximizeButton = Ksword::Ui::CreateButton(hwnd, kMaximizeButtonId, L"Maximize", 0, 0, 78, 24);
    state->closeButton = Ksword::Ui::CreateButton(hwnd, kCloseButtonId, L"Close", 0, 0, 78, 24);
    state->windowList = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWindowListId)), ::GetModuleHandleW(nullptr), nullptr);
    state->detailList = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailListId)), ::GetModuleHandleW(nullptr), nullptr);
    if (!state->sortCombo || !state->refreshButton || !state->frontButton || !state->restoreButton || !state->minimizeButton ||
        !state->maximizeButton || !state->closeButton || !state->windowList || !state->detailList) {
        return false;
    }

    ::SendMessageW(state->sortCombo, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ::SendMessageW(state->sortCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"按堆叠顺序"));
    ::SendMessageW(state->sortCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"按进程顺序"));
    ::SendMessageW(state->sortCombo, CB_SETCURSEL, 0, 0);
    ::SendMessageW(state->windowList, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ::SendMessageW(state->detailList, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ListView_SetExtendedListViewStyle(state->windowList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    ListView_SetExtendedListViewStyle(state->detailList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    state->processImageList = ImageList_Create(::GetSystemMetrics(SM_CXSMICON),
        ::GetSystemMetrics(SM_CYSMICON),
        ILC_COLOR32 | ILC_MASK,
        64,
        64);
    if (state->processImageList != nullptr) {
        ListView_SetImageList(state->windowList, state->processImageList, LVSIL_SMALL);
    }
    AddColumn(state->windowList, 0, L"进程 / PID", 190);
    AddColumn(state->windowList, 1, L"HWND", 120);
    AddColumn(state->windowList, 2, L"Title", 300);
    AddColumn(state->windowList, 3, L"Class", 190);
    AddColumn(state->windowList, 4, L"State", 220);
    AddColumn(state->detailList, 0, L"Property", 170);
    AddColumn(state->detailList, 1, L"Value", 720);
    return true;
}

// RegisterWindowViewClass registers the custom Window feature page. There is no
// input beyond process module state; output is true when CreateWindowExW can use
// the class.
bool RegisterWindowViewClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        WindowViewState* state = reinterpret_cast<WindowViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<WindowViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
            return TRUE;
        }
        case WM_CREATE:
            if (state && CreateChildControls(state, hwnd)) {
                LayoutView(state);
                RefreshWindows(state);
            }
            return 0;
        case WM_SIZE:
            LayoutView(state);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kRefreshButtonId) {
                RefreshWindows(state);
                return 0;
            }
            if (id == kSortComboId && HIWORD(wParam) == CBN_SELCHANGE) {
                UpdateSortModeFromCombo(state);
                return 0;
            }
            if (id == kFrontButtonId || id == kRestoreButtonId || id == kMinimizeButtonId ||
                id == kMaximizeButtonId || id == kCloseButtonId) {
                RunAction(state, id);
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            auto* notify = reinterpret_cast<NMHDR*>(lParam);
            if (state && notify && notify->idFrom == kWindowListId && notify->code == LVN_ITEMCHANGED) {
                auto* changed = reinterpret_cast<NMLISTVIEW*>(lParam);
                if ((changed->uNewState & LVIS_SELECTED) != 0) {
                    ShowDetail(state, SelectedModelIndex(state));
                }
                return 0;
            }
            if (state && notify && notify->idFrom == kWindowListId && notify->code == NM_RCLICK) {
                POINT point{};
                ::GetCursorPos(&point);
                ShowWindowContextMenu(state, point);
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->windowList) {
                POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (point.x == -1 && point.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->windowList, &rc);
                    point = { rc.left + 20, rc.top + 20 };
                }
                ShowWindowContextMenu(state, point);
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().panelBrush());
            RECT textRc{ 668, 7, rc.right - kGap, kHeaderHeight };
            const std::wstring title = state ? state->statusText : L"Windows";
            Ksword::Ui::DrawTextLine(dc, title, textRc, Ksword::Ui::AppTheme().mutedTextColor,
                Ksword::Ui::SystemUIFont(), DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (state && state->processImageList != nullptr) {
                ImageList_Destroy(state->processImageList);
                state->processImageList = nullptr;
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
    wc.lpszClassName = kWindowViewClass;
    if (::RegisterClassW(&wc)) {
        return true;
    }
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

HWND CreateWindowFeatureView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterWindowViewClass()) {
        return nullptr;
    }
    auto* state = new WindowViewState();
    HWND hwnd = ::CreateWindowExW(0, kWindowViewClass, L"Windows",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, Width(bounds), Height(bounds), parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::Window
