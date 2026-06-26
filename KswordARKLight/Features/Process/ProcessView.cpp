#include "ProcessView.h"

#include "ProcessActions.h"
#include "ProcessEnumerator.h"
#include "ProcessModel.h"
#include "../ProcessDetail/ProcessDetailFeature.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Ksword::Features::Process {
namespace {

constexpr wchar_t kProcessViewClass[] = L"KswordARKLight.ProcessView";
constexpr int kRefreshButtonId = 52001;
constexpr int kModeButtonId = 52002;
constexpr int kPauseButtonId = 52003;
constexpr int kPickerButtonId = 52004;
constexpr int kStatusTextId = 52005;
constexpr int kProcessListId = 52006;
constexpr int kRefreshSliderId = 52007;
constexpr UINT kContextMenuBaseId = 53000;
constexpr UINT_PTR kRefreshTimerId = 52008;
constexpr UINT kMsgInitialRefresh = WM_APP + 520;
constexpr int kTreeIndentPixels = 18;
constexpr int kTreeIconGap = 4;
constexpr int kTreeTextGap = 4;

struct NotifyResult {
    bool handled = false;
    LRESULT result = 0;
};

enum class ProcessRowVisualState {
    Normal,
    Added,
    Removed
};


// DetailHostState owns one top-level process detail window. Input values are
// supplied at creation time through CREATESTRUCTW; processing hosts the existing
// WS_CHILD ProcessDetailPage and resizes it with the frame; no value is returned
// directly because lifetime is tied to WM_NCDESTROY.
struct DetailHostState {
    DWORD processId = 0;
    HWND child = nullptr;
};

constexpr wchar_t kProcessDetailHostClass[] = L"KswordARKLight.ProcessDetailHost";

// DetailHostProc is the top-level process-detail host window procedure. Inputs
// are ordinary Win32 messages; processing creates/resizes the child detail page;
// output is the message LRESULT expected by DefWindowProcW callers.
LRESULT CALLBACK DetailHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DetailHostState* state = reinterpret_cast<DetailHostState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* owned = new DetailHostState();
        owned->processId = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(create->lpCreateParams));
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<DetailHostState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (state) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            state->child = Ksword::Features::ProcessDetail::CreateProcessDetailPage(hwnd, state->processId, rc);
        }
        return 0;
    case WM_SIZE:
        if (state && state->child) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::MoveWindow(state->child, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        }
        return 0;
    case WM_NCDESTROY:
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// RegisterDetailHostClass installs the standalone detail host class once. There
// is no input; processing registers a standard overlapped window; output reports
// whether CreateWindowExW may use the class name.
bool RegisterDetailHostClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = DetailHostProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kProcessDetailHostClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

// OpenProcessDetailWindow creates one top-level detail window for a single PID.
// Inputs are owner and pid; processing reuses the ProcessDetail page exactly as
// used in docks; output reports whether the host window was created and shown.
bool OpenProcessDetailWindow(HWND owner, DWORD processId) {
    if (processId == 0 || !RegisterDetailHostClass()) {
        return false;
    }

    const std::wstring title = L"进程详细信息 - PID " + std::to_wstring(processId);
    HWND host = ::CreateWindowExW(WS_EX_APPWINDOW,
        kProcessDetailHostClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        680,
        owner,
        nullptr,
        ::GetModuleHandleW(nullptr),
        reinterpret_cast<void*>(static_cast<ULONG_PTR>(processId)));
    if (!host) {
        return false;
    }

    ::ShowWindow(host, SW_SHOWNORMAL);
    ::UpdateWindow(host);
    return true;
}

// ProcessViewState owns the controls and model for one process-list page.
// Inputs are Win32 messages routed through ProcessViewWndProc. Processing keeps
// the current snapshot, visible mode, icon cache, picker state, and selected
// rows in sync. Return values are produced by WndProc message handling.
struct ProcessViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND modeButton = nullptr;
    HWND pauseButton = nullptr;
    HWND pickerButton = nullptr;
    HWND refreshSlider = nullptr;
    HWND statusText = nullptr;
    HWND listView = nullptr;
    HIMAGELIST imageList = nullptr;
    ProcessModel model;
    ProcessViewMode mode = ProcessViewMode::UtilizationFriendly;
    bool pickingWindow = false;
    bool refreshPaused = false;
    UINT refreshIntervalSeconds = 2;
    ULONGLONG previousSampleTickMs = 0;
    std::vector<ProcessActionMenuItem> activeMenuItems;
    std::unordered_map<std::wstring, int> iconCache;
    std::unordered_map<DWORD, ULONGLONG> previousCpuTime100ns;
    std::unordered_map<DWORD, ProcessSnapshotRow> lastActiveRowsByPid;
    std::unordered_map<DWORD, ProcessRowVisualState> visualStateByPid;
    bool hasLastActiveSnapshot = false;
};

// ColumnSet returns the report columns for a process view mode. Input is the
// selected view mode; processing emits static descriptors; output is consumed by
// the ListView header rebuild path.
std::vector<Ksword::Ui::ListViewColumn> ColumnSet(ProcessViewMode mode) {
    if (mode == ProcessViewMode::Detail) {
        return {
            { 0, 240, LVCFMT_LEFT, L"名称" },
            { 1, 78, LVCFMT_RIGHT, L"PID" },
            { 2, 78, LVCFMT_RIGHT, L"父 PID" },
            { 3, 78, LVCFMT_RIGHT, L"线程" },
            { 4, 110, LVCFMT_RIGHT, L"工作集" },
            { 5, 118, LVCFMT_RIGHT, L"私有字节" },
            { 6, 118, LVCFMT_RIGHT, L"虚拟内存" },
            { 7, 86, LVCFMT_RIGHT, L"基础优先级" },
            { 8, 70, LVCFMT_RIGHT, L"会话" },
            { 9, 420, LVCFMT_LEFT, L"映像路径" },
        };
    }

    return {
        { 0, 250, LVCFMT_LEFT, L"进程" },
        { 1, 78, LVCFMT_RIGHT, L"PID" },
        { 2, 74, LVCFMT_RIGHT, L"CPU" },
        { 3, 110, LVCFMT_RIGHT, L"工作集" },
        { 4, 110, LVCFMT_RIGHT, L"私有" },
        { 5, 110, LVCFMT_RIGHT, L"虚拟" },
        { 6, 70, LVCFMT_RIGHT, L"线程" },
        { 7, 70, LVCFMT_RIGHT, L"会话" },
        { 8, 90, LVCFMT_RIGHT, L"页错误" },
    };
}

// StateFromWindow returns the state pointer stored on the page HWND. Input is a
// window handle; processing reads GWLP_USERDATA; output is null before creation
// finishes or after destruction clears the pointer.
ProcessViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<ProcessViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// SetStatus writes a short human-readable status line. Inputs are page state and
// text; processing updates the STATIC control; no value is returned.
void SetStatus(ProcessViewState& state, const std::wstring& text) {
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, text.c_str());
    }
}

// RefreshIntervalMs returns the active refresh period in milliseconds. Input is
// state.refreshIntervalSeconds from the compact slider; processing clamps the
// value to the supported slider range; output is passed to SetTimer.
UINT RefreshIntervalMs(const ProcessViewState& state) {
    const UINT seconds = std::clamp<UINT>(state.refreshIntervalSeconds, 1, 10);
    return seconds * 1000U;
}

// RestartRefreshTimer applies the current slider interval to the page timer.
// Input is the process page state; processing kills the old timer and creates a
// new one on the page HWND; no value is returned.
void RestartRefreshTimer(ProcessViewState& state) {
    if (!state.hwnd) {
        return;
    }
    ::KillTimer(state.hwnd, kRefreshTimerId);
    ::SetTimer(state.hwnd, kRefreshTimerId, RefreshIntervalMs(state), nullptr);
}

// UpdateToolbarTexts keeps compact button labels in sync with runtime state.
// Input is the page state; processing updates only existing HWNDs; no value is
// returned because controls may be absent during partial creation.
void UpdateToolbarTexts(ProcessViewState& state) {
    if (state.modeButton) {
        ::SetWindowTextW(state.modeButton,
            state.mode == ProcessViewMode::Detail ? L"详细" : L"利用率");
    }
    if (state.pauseButton) {
        ::SetWindowTextW(state.pauseButton, state.refreshPaused ? L"恢复" : L"暂停");
    }
}

// CreateProcessListView creates a multi-select report ListView. Inputs are the
// parent HWND and child id. Processing intentionally avoids the shared helper
// because that helper enables LVS_SINGLESEL; output is the child HWND or null.
HWND CreateProcessListView(HWND parent, int id) {
    HWND hwnd = ::CreateWindowExW(WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS,
        0,
        0,
        0,
        0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (!hwnd) {
        return nullptr;
    }

    ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ListView_SetExtendedListViewStyleEx(hwnd,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    return hwnd;
}

// CreateIconList creates the small-image list used by the process list. There
// are no inputs; processing asks comctl32 for a 16x16 image list; output is the
// HIMAGELIST handle or null. The caller owns and destroys it.
HIMAGELIST CreateIconList() {
    return ImageList_Create(::GetSystemMetrics(SM_CXSMICON),
        ::GetSystemMetrics(SM_CYSMICON),
        ILC_COLOR32 | ILC_MASK,
        64,
        64);
}

// AddIconFromShell extracts a small icon for a path or returns a generic icon.
// Inputs are the image list, an executable path and whether fallback is allowed.
// Processing uses SHGetFileInfoW and copies the icon into the image list. Return
// value is the image index or -1 when no icon could be obtained.
int AddIconFromShell(HIMAGELIST imageList, const std::wstring& path, bool fallback) {
    if (!imageList) {
        return -1;
    }

    SHFILEINFOW info{};
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    const wchar_t* queryPath = path.empty() ? L".exe" : path.c_str();
    if (path.empty() && fallback) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }

    if (!::SHGetFileInfoW(queryPath,
            fallback ? FILE_ATTRIBUTE_NORMAL : 0,
            &info,
            sizeof(info),
            flags) || !info.hIcon) {
        return -1;
    }

    const int index = ImageList_AddIcon(imageList, info.hIcon);
    ::DestroyIcon(info.hIcon);
    return index;
}

// IconIndexForPath resolves and caches a process icon. Inputs are page state and
// an executable path. Processing extracts an icon with Shell APIs and caches by
// path; output is a ListView image index, falling back to a generic executable.
int IconIndexForPath(ProcessViewState& state, const std::wstring& path) {
    const std::wstring key = path.empty() ? L"<generic-exe>" : path;
    const auto found = state.iconCache.find(key);
    if (found != state.iconCache.end()) {
        return found->second;
    }

    int index = AddIconFromShell(state.imageList, path, path.empty());
    if (index < 0 && !path.empty()) {
        index = IconIndexForPath(state, std::wstring());
    }
    if (index >= 0) {
        state.iconCache.emplace(key, index);
    }
    return index;
}

// LayoutChildren positions the toolbar, status text and ListView. Inputs are the
// page state and client rectangle; processing uses fixed toolbar heights and the
// remaining area for process rows; no value is returned.
void LayoutChildren(ProcessViewState& state, const RECT& rc) {
    const int buttonHeight = 24;
    const int buttonTop = 0;
    int x = 0;

    ::MoveWindow(state.refreshButton, x, buttonTop, 56, buttonHeight, TRUE);
    x += 56;
    ::MoveWindow(state.pauseButton, x, buttonTop, 56, buttonHeight, TRUE);
    x += 56;
    ::MoveWindow(state.modeButton, x, buttonTop, 62, buttonHeight, TRUE);
    x += 62;
    ::MoveWindow(state.pickerButton, x, buttonTop, 104, buttonHeight, TRUE);
    x += 104;
    ::MoveWindow(state.refreshSlider, x, buttonTop + 2, 110, buttonHeight - 4, TRUE);

    const int statusTop = buttonTop + buttonHeight;
    const int width = std::max(100, static_cast<int>(rc.right - rc.left));
    const int height = std::max(100, static_cast<int>(rc.bottom - rc.top));
    ::MoveWindow(state.statusText, 0, statusTop, width, 20, TRUE);

    const int listTop = statusTop + 20;
    ::MoveWindow(state.listView,
        0,
        listTop,
        std::max(80, width),
        std::max(80, height - listTop),
        TRUE);
}

// PaintBackground clears the page background only. Inputs are HWND and paint DC;
// processing intentionally draws no title text or decorative padding; no value
// is returned.
void PaintBackground(HWND hwnd, HDC dc) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());
}

// RebuildColumns replaces the ListView columns for the current mode. Input is
// page state; processing deletes old columns and inserts mode-specific headers;
// no value is returned.
void RebuildColumns(ProcessViewState& state) {
    if (!state.listView) {
        return;
    }

    HWND header = ListView_GetHeader(state.listView);
    const int count = header ? Header_GetItemCount(header) : 0;
    for (int index = count - 1; index >= 0; --index) {
        ListView_DeleteColumn(state.listView, index);
    }

    for (const auto& column : ColumnSet(state.mode)) {
        LVCOLUMNW nativeColumn{};
        nativeColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        nativeColumn.fmt = column.format;
        nativeColumn.cx = column.width;
        nativeColumn.pszText = const_cast<LPWSTR>(column.title.c_str());
        ListView_InsertColumn(state.listView, column.index, &nativeColumn);
    }
}

// SetListSubItemText writes one text cell to the report control. Inputs are the
// list handle, item/subitem indexes and text; processing sends the common
// control update message; no value is returned.
void SetListSubItemText(HWND listView, int item, int subItem, const std::wstring& text) {
    ListView_SetItemText(listView, item, subItem, const_cast<LPWSTR>(text.c_str()));
}

// RebuildRows synchronizes visible rows without deleting the whole table. Input
// is the page state; processing adjusts the item count, rewrites lParam/text in
// place, and leaves the control/window alive so refreshes avoid full-table
// flicker. No value is returned.
void RebuildRows(ProcessViewState& state) {
    if (!state.listView) {
        return;
    }

    Ksword::Ui::ScopedListViewRedrawLock redrawLock(state.listView);
    const auto& rows = state.model.displayRows(state.mode);
    const int columnCount = static_cast<int>(ColumnSet(state.mode).size());
    while (ListView_GetItemCount(state.listView) > static_cast<int>(rows.size())) {
        ListView_DeleteItem(state.listView, ListView_GetItemCount(state.listView) - 1);
    }

    for (std::size_t displayIndex = 0; displayIndex < rows.size(); ++displayIndex) {
        const ProcessDisplayRow& displayRow = rows[displayIndex];
        std::vector<std::wstring> cells;
        cells.reserve(static_cast<std::size_t>(columnCount));
        for (int column = 0; column < columnCount; ++column) {
            cells.push_back(state.model.textForColumn(displayRow, column, state.mode));
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(displayIndex);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(cells.empty() ? L"" : cells[0].c_str());
        item.lParam = static_cast<LPARAM>(displayIndex);

        const int currentCount = ListView_GetItemCount(state.listView);
        int row = item.iItem;
        if (row >= currentCount) {
            row = ListView_InsertItem(state.listView, &item);
            if (row < 0) {
                continue;
            }
        } else {
            ListView_SetItem(state.listView, &item);
        }
        for (int column = 1; column < static_cast<int>(cells.size()); ++column) {
            SetListSubItemText(state.listView, row, column, cells[static_cast<std::size_t>(column)]);
        }
        for (int column = static_cast<int>(cells.size()); column < columnCount; ++column) {
            SetListSubItemText(state.listView, row, column, L"");
        }
    }
    if (!rows.empty()) {
        ListView_RedrawItems(state.listView, 0, static_cast<int>(rows.size() - 1));
    }
}

// SelectedDisplayIndexes reads the current multi-selection. Input is page state;
// processing walks ListView selected rows and returns model display indexes from
// lParam values. Output excludes rows with invalid lParam values.
std::vector<int> SelectedDisplayIndexes(ProcessViewState& state) {
    std::vector<int> indexes;
    if (!state.listView) {
        return indexes;
    }

    int item = -1;
    while ((item = ListView_GetNextItem(state.listView, item, LVNI_SELECTED)) != -1) {
        LVITEMW lv{};
        lv.mask = LVIF_PARAM;
        lv.iItem = item;
        if (ListView_GetItem(state.listView, &lv)) {
            indexes.push_back(static_cast<int>(lv.lParam));
        }
    }
    return indexes;
}

// SelectedPids converts the current ListView selection to process ids. Input is
// page state; processing delegates group filtering to ProcessModel; output may
// be empty when only group rows are selected.
std::vector<DWORD> SelectedPids(ProcessViewState& state) {
    return state.model.selectedPids(SelectedDisplayIndexes(state), state.mode);
}

// DisplayIndexFromListItem reads the model display-row index attached to a
// ListView row. Inputs are the page state and item index; processing reads
// LVIF_PARAM; output is -1 for invalid list rows.
int DisplayIndexFromListItem(ProcessViewState& state, int item) {
    if (!state.listView || item < 0) {
        return -1;
    }

    LVITEMW lv{};
    lv.mask = LVIF_PARAM;
    lv.iItem = item;
    if (!ListView_GetItem(state.listView, &lv)) {
        return -1;
    }
    return static_cast<int>(lv.lParam);
}

// GroupHeaderAtListItem returns the clicked group header row when the item is a
// friendly group header. Inputs are page state and a list item index; output is
// nullptr for process rows, invalid rows, or stale display indexes.
const ProcessDisplayRow* GroupHeaderAtListItem(ProcessViewState& state, int item) {
    const int displayIndex = DisplayIndexFromListItem(state, item);
    const auto& rows = state.model.displayRows(state.mode);
    if (displayIndex < 0 || displayIndex >= static_cast<int>(rows.size())) {
        return nullptr;
    }
    const ProcessDisplayRow& row = rows[static_cast<std::size_t>(displayIndex)];
    return row.groupHeader ? &row : nullptr;
}

// DisplayRowFromListItem resolves a ListView row back to the visible model row.
// Inputs are page state and item index; output is null when the row is stale or
// invalid. Drawing and hit handling use this instead of trusting row order.
const ProcessDisplayRow* DisplayRowFromListItem(ProcessViewState& state, int item) {
    const int displayIndex = DisplayIndexFromListItem(state, item);
    const auto& rows = state.model.displayRows(state.mode);
    if (displayIndex < 0 || displayIndex >= static_cast<int>(rows.size())) {
        return nullptr;
    }
    return &rows[static_cast<std::size_t>(displayIndex)];
}

// VisualStateForDisplayRow resolves green/gray lifecycle highlighting for a
// visible process row. Inputs are page state and display row; processing maps
// process PID into the latest refresh-diff table; output is Normal for groups
// and unchanged process rows.
ProcessRowVisualState VisualStateForDisplayRow(ProcessViewState& state, const ProcessDisplayRow& displayRow) {
    const ProcessSnapshotRow* row = state.model.rowForDisplayRow(displayRow);
    if (!row) {
        return ProcessRowVisualState::Normal;
    }
    const auto found = state.visualStateByPid.find(row->processId);
    return found != state.visualStateByPid.end() ? found->second : ProcessRowVisualState::Normal;
}

// RowBackgroundColor returns the fill color used by custom draw. Inputs are
// selection and lifecycle state; processing gives selected rows system colors,
// new rows light green and deleted rows light gray; output is a COLORREF.
COLORREF RowBackgroundColor(bool selected, ProcessRowVisualState visualState) {
    if (selected) {
        return ::GetSysColor(COLOR_HIGHLIGHT);
    }
    if (visualState == ProcessRowVisualState::Added) {
        return RGB(216, 248, 216);
    }
    if (visualState == ProcessRowVisualState::Removed) {
        return RGB(232, 232, 232);
    }
    return ::GetSysColor(COLOR_WINDOW);
}

// RowTextColor returns the text color used by custom draw. Inputs are selection
// and lifecycle state; processing keeps deleted rows muted while selected rows
// use system highlight text; output is a COLORREF.
COLORREF RowTextColor(bool selected, ProcessRowVisualState visualState) {
    if (selected) {
        return ::GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    if (visualState == ProcessRowVisualState::Removed) {
        return ::GetSysColor(COLOR_GRAYTEXT);
    }
    return ::GetSysColor(COLOR_WINDOWTEXT);
}

// SubItemBounds returns a bounded rectangle for one report-cell. Inputs are the
// list view, item and subitem; processing avoids the common-control quirk where
// subitem zero can report the whole row by clamping to column zero width; output
// is true when a rectangle was obtained.
bool SubItemBounds(HWND listView, int item, int subItem, RECT& bounds) {
    bounds = {};
    if (subItem == 0) {
        bounds.left = LVIR_BOUNDS;
        if (!ListView_GetSubItemRect(listView, item, 0, LVIR_BOUNDS, &bounds)) {
            return false;
        }
        bounds.right = bounds.left + ListView_GetColumnWidth(listView, 0);
        return true;
    }
    return ListView_GetSubItemRect(listView, item, subItem, LVIR_BOUNDS, &bounds) != FALSE;
}

// TextFormatForColumn maps ListView column alignment into DrawText flags.
// Inputs are the active mode and subitem index; output uses vertical centering,
// ellipsis and the column's left/right alignment.
UINT TextFormatForColumn(ProcessViewMode mode, int subItem) {
    UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    const auto columns = ColumnSet(mode);
    for (const auto& column : columns) {
        if (column.index == subItem && column.format == LVCFMT_RIGHT) {
            return format | DT_RIGHT;
        }
    }
    return format | DT_LEFT;
}

// DrawDottedTreeGuides paints lightweight dotted indentation guides. Inputs are
// the draw DC, first-column bounds and process depth; processing uses the
// current system text color family with a dotted pen; no value is returned.
void DrawDottedTreeGuides(HDC dc, const RECT& bounds, int depth) {
    if (depth <= 0) {
        return;
    }

    HPEN pen = ::CreatePen(PS_DOT, 1, ::GetSysColor(COLOR_3DSHADOW));
    if (!pen) {
        return;
    }
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const int rowMidY = bounds.top + (bounds.bottom - bounds.top) / 2;
    for (int level = 1; level <= depth; ++level) {
        const int x = bounds.left + (level - 1) * kTreeIndentPixels + kTreeIndentPixels / 2;
        ::MoveToEx(dc, x, bounds.top, nullptr);
        ::LineTo(dc, x, bounds.bottom);
    }
    const int branchX = bounds.left + (depth - 1) * kTreeIndentPixels + kTreeIndentPixels / 2;
    const int iconLeft = bounds.left + depth * kTreeIndentPixels;
    ::MoveToEx(dc, branchX, rowMidY, nullptr);
    ::LineTo(dc, iconLeft, rowMidY);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// DrawProcessSubItem custom-draws every visible process cell. Inputs are the
// page state and NMLVCUSTOMDRAW payload; processing draws a stable background,
// optional tree guides/icon in column zero, and text for all columns so hover
// repaint never erases row text; output reports whether default drawing should
// be skipped.
LRESULT DrawProcessSubItem(ProcessViewState& state, NMLVCUSTOMDRAW* draw) {
    if (!draw) {
        return CDRF_DODEFAULT;
    }

    const int item = static_cast<int>(draw->nmcd.dwItemSpec);
    const int subItem = draw->iSubItem;
    const ProcessDisplayRow* displayRow = DisplayRowFromListItem(state, item);
    if (!displayRow) {
        return CDRF_DODEFAULT;
    }

    RECT cell{};
    if (!SubItemBounds(state.listView, item, subItem, cell)) {
        return CDRF_DODEFAULT;
    }

    HDC dc = draw->nmcd.hdc;
    const bool selected = (ListView_GetItemState(state.listView, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const bool focused = (ListView_GetItemState(state.listView, item, LVIS_FOCUSED) & LVIS_FOCUSED) != 0;
    const ProcessRowVisualState visualState = VisualStateForDisplayRow(state, *displayRow);
    const COLORREF background = RowBackgroundColor(selected, visualState);
    const COLORREF textColor = RowTextColor(selected, visualState);
    HBRUSH brush = ::CreateSolidBrush(background);
    if (brush) {
        ::FillRect(dc, &cell, brush);
        ::DeleteObject(brush);
    }

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, textColor);
    HFONT font = Ksword::Ui::SystemUIFont();
    HGDIOBJ oldFont = font ? ::SelectObject(dc, font) : nullptr;

    if (subItem == 0 && !displayRow->groupHeader) {
        DrawDottedTreeGuides(dc, cell, displayRow->depth);
        const int iconSize = ::GetSystemMetrics(SM_CXSMICON);
        const int iconLeft = cell.left + displayRow->depth * kTreeIndentPixels;
        const int rowHeight = static_cast<int>(cell.bottom - cell.top);
        const int iconTop = static_cast<int>(cell.top) + std::max(0, (rowHeight - iconSize) / 2);
        const int iconIndex = IconIndexForPath(state, state.model.iconPathForRow(*displayRow));
        if (state.imageList && iconIndex >= 0) {
            ImageList_Draw(state.imageList, iconIndex, dc, iconLeft, iconTop, ILD_TRANSPARENT);
        }

        RECT textRect = cell;
        textRect.left = iconLeft + iconSize + kTreeIconGap + kTreeTextGap;
        const std::wstring text = state.model.textForColumn(*displayRow, 0, state.mode);
        ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT textRect = cell;
        textRect.left += 4;
        textRect.right -= 4;
        const std::wstring text = state.model.textForColumn(*displayRow, subItem, state.mode);
        ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect, TextFormatForColumn(state.mode, subItem));
    }

    if (focused && subItem == 0) {
        RECT focusRect = cell;
        focusRect.right = cell.left;
        HWND header = ListView_GetHeader(state.listView);
        const int count = header ? Header_GetItemCount(header) : 0;
        for (int column = 0; column < count; ++column) {
            focusRect.right += ListView_GetColumnWidth(state.listView, column);
        }
        ::DrawFocusRect(dc, &focusRect);
    }
    if (oldFont) {
        ::SelectObject(dc, oldFont);
    }
    return CDRF_SKIPDEFAULT;
}

// HandleListCustomDraw owns first-column tree drawing while letting comctl32
// render all other cells normally. Inputs are page state and custom-draw data;
// output is the required custom-draw return code for WM_NOTIFY.
LRESULT HandleListCustomDraw(ProcessViewState& state, NMLVCUSTOMDRAW* draw) {
    if (!draw) {
        return CDRF_DODEFAULT;
    }
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
        return DrawProcessSubItem(state, draw);
    default:
        return CDRF_DODEFAULT;
    }
}

// SelectedRowsAsText builds tab-separated text for selected visible rows. Inputs
// are page state and whether all columns should be copied. Processing reads the
// current model display rows; output is suitable for the clipboard.
std::wstring SelectedRowsAsText(ProcessViewState& state, bool allColumns) {
    const auto indexes = SelectedDisplayIndexes(state);
    const auto& visibleRows = state.model.displayRows(state.mode);
    const int columnCount = static_cast<int>(ColumnSet(state.mode).size());
    std::wstring text;

    for (int index : indexes) {
        if (index < 0 || index >= static_cast<int>(visibleRows.size())) {
            continue;
        }
        const ProcessDisplayRow& row = visibleRows[static_cast<std::size_t>(index)];
        const int maxColumn = allColumns ? columnCount : 1;
        for (int column = 0; column < maxColumn; ++column) {
            if (column != 0) {
                text += L'\t';
            }
            text += state.model.textForColumn(row, column, state.mode);
        }
        text += L"\r\n";
    }
    return text;
}

// WriteClipboardText copies Unicode text to the Windows clipboard. Inputs are
// the owner HWND and text. Processing allocates CF_UNICODETEXT global memory and
// transfers ownership to the clipboard. Return value reports success.
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

// RefreshProcesses performs one process enumeration and redraws the list. Input
// is page state. Processing calls NtQuerySystemInformation through the process
// enumerator and updates the model. No return value is needed; status text shows
// success or failure.
void RefreshProcesses(ProcessViewState& state) {
    SetStatus(state, L"正在通过 NtQuerySystemInformation 枚举进程...");
    const ProcessEnumerationResult result = EnumerateProcessesByNtQuerySystemInformation();
    if (!result.success) {
        state.model.setRows({});
        RebuildRows(state);
        SetStatus(state, L"进程枚举失败: " + result.diagnosticText);
        return;
    }

    std::vector<ProcessSnapshotRow> rows = result.rows;
    const ULONGLONG nowMs = ::GetTickCount64();
    const ULONGLONG elapsedMs = state.previousSampleTickMs == 0 ? 0 : nowMs - state.previousSampleTickMs;
    const DWORD processorCount = std::max<DWORD>(1, ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    std::unordered_map<DWORD, ULONGLONG> newCpuTimes;
    std::unordered_map<DWORD, ProcessSnapshotRow> activeRowsByPid;
    newCpuTimes.reserve(rows.size());
    activeRowsByPid.reserve(rows.size());
    state.visualStateByPid.clear();
    for (ProcessSnapshotRow& row : rows) {
        const ULONGLONG total100ns = row.kernelTime100ns + row.userTime100ns;
        newCpuTimes[row.processId] = total100ns;
        activeRowsByPid[row.processId] = row;
        if (state.hasLastActiveSnapshot && state.lastActiveRowsByPid.find(row.processId) == state.lastActiveRowsByPid.end()) {
            state.visualStateByPid[row.processId] = ProcessRowVisualState::Added;
        }
        row.cpuUsagePercent = 0.0;
        const auto previous = state.previousCpuTime100ns.find(row.processId);
        if (elapsedMs > 0 && previous != state.previousCpuTime100ns.end() && total100ns >= previous->second) {
            const ULONGLONG delta100ns = total100ns - previous->second;
            const double capacity100ns = static_cast<double>(elapsedMs) * 10000.0 * static_cast<double>(processorCount);
            if (capacity100ns > 0.0) {
                row.cpuUsagePercent = std::clamp((static_cast<double>(delta100ns) * 100.0) / capacity100ns, 0.0, 999.9);
            }
        }
    }
    if (state.hasLastActiveSnapshot) {
        for (const auto& oldRow : state.lastActiveRowsByPid) {
            if (activeRowsByPid.find(oldRow.first) == activeRowsByPid.end()) {
                ProcessSnapshotRow removed = oldRow.second;
                removed.cpuUsagePercent = 0.0;
                rows.push_back(removed);
                state.visualStateByPid[oldRow.first] = ProcessRowVisualState::Removed;
            }
        }
    }

    const std::size_t activeCount = activeRowsByPid.size();
    const std::size_t addedCount = std::count_if(state.visualStateByPid.begin(), state.visualStateByPid.end(), [](const auto& entry) {
        return entry.second == ProcessRowVisualState::Added;
    });
    const std::size_t removedCount = std::count_if(state.visualStateByPid.begin(), state.visualStateByPid.end(), [](const auto& entry) {
        return entry.second == ProcessRowVisualState::Removed;
    });

    state.previousCpuTime100ns = std::move(newCpuTimes);
    state.previousSampleTickMs = nowMs;
    state.lastActiveRowsByPid = std::move(activeRowsByPid);
    state.hasLastActiveSnapshot = true;

    state.model.setRows(std::move(rows));
    RebuildRows(state);
    SetStatus(state,
        L"已同步 " + std::to_wstring(activeCount) +
        L" 个进程；新增 " + std::to_wstring(addedCount) +
        L"，退出 " + std::to_wstring(removedCount) +
        L" 个进程；刷新 " + std::to_wstring(state.refreshIntervalSeconds) +
        L"s；左 Ctrl 按下时跳过自动刷新。");
}

// SwitchMode changes the visible process layout. Inputs are page state and new
// mode. Processing rebuilds columns and rows; no value is returned.
void SwitchMode(ProcessViewState& state, ProcessViewMode mode) {
    if (state.mode == mode) {
        return;
    }
    state.mode = mode;
    RebuildColumns(state);
    RebuildRows(state);
    UpdateToolbarTexts(state);
    SetStatus(state, mode == ProcessViewMode::Detail ? L"已切换到详细信息视图。" : L"已切换到利用率展示视图（无折线图）。");
}

// ToggleMode switches the single mode button between utilization and detail.
// Input is the page state; processing calls SwitchMode; no value is returned.
void ToggleMode(ProcessViewState& state) {
    SwitchMode(state,
        state.mode == ProcessViewMode::Detail ? ProcessViewMode::UtilizationFriendly : ProcessViewMode::Detail);
}

// ToggleRefreshPaused flips automatic refresh. Input is the page state;
// processing updates the toolbar and status; no value is returned.
void ToggleRefreshPaused(ProcessViewState& state) {
    state.refreshPaused = !state.refreshPaused;
    UpdateToolbarTexts(state);
    SetStatus(state, state.refreshPaused ? L"自动刷新已暂停。" : L"自动刷新已恢复。");
}

// SelectPid highlights all visible rows matching one PID. Inputs are page state
// and PID. Processing clears existing selection then selects/focuses the first
// matching row. Return value reports whether a visible row was found.
bool SelectPid(ProcessViewState& state, DWORD pid) {
    if (!state.listView) {
        return false;
    }

    ListView_SetItemState(state.listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    const auto& rows = state.model.displayRows(state.mode);
    bool found = false;
    for (int item = 0; item < ListView_GetItemCount(state.listView); ++item) {
        LVITEMW lv{};
        lv.mask = LVIF_PARAM;
        lv.iItem = item;
        if (!ListView_GetItem(state.listView, &lv)) {
            continue;
        }
        const int displayIndex = static_cast<int>(lv.lParam);
        if (displayIndex < 0 || displayIndex >= static_cast<int>(rows.size())) {
            continue;
        }
        const ProcessSnapshotRow* row = state.model.rowForDisplayRow(rows[static_cast<std::size_t>(displayIndex)]);
        if (row && row->processId == pid) {
            ListView_SetItemState(state.listView, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(state.listView, item, FALSE);
            found = true;
        }
    }
    return found;
}

// BeginPicker starts the drag-select interaction. Input is page state; processing
// captures the mouse to the page and changes the cursor prompt. The selected PID
// is resolved later from WindowFromPoint on mouse release; no value is returned.
void BeginPicker(ProcessViewState& state) {
    state.pickingWindow = true;
    ::SetCapture(state.hwnd);
    ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
    SetStatus(state, L"拖动到目标窗口后释放鼠标，将根据窗口 HWND 选中对应进程。");
}

// CompletePicker ends drag-select. Input is page state. Processing reads the
// current cursor position, resolves the window PID with GetWindowThreadProcessId,
// selects a matching row, and updates status. No value is returned.
void CompletePicker(ProcessViewState& state) {
    state.pickingWindow = false;
    if (::GetCapture() == state.hwnd) {
        ::ReleaseCapture();
    }

    POINT point{};
    ::GetCursorPos(&point);
    HWND target = ::WindowFromPoint(point);
    DWORD pid = 0;
    if (target) {
        ::GetWindowThreadProcessId(target, &pid);
    }

    if (pid != 0 && SelectPid(state, pid)) {
        SetStatus(state, L"已通过窗口拖选定位 PID " + std::to_wstring(pid) + L"。");
    } else if (pid != 0) {
        SetStatus(state, L"窗口属于 PID " + std::to_wstring(pid) + L"，但当前列表中未找到可见行。请刷新后重试。");
    } else {
        SetStatus(state, L"未能从释放位置解析窗口进程。");
    }
}

// ExecuteMenuItem handles one context-menu command. Inputs are page state and an
// action id. Processing keeps UI-local copy commands visible, then delegates the
// Win32/R3 action contract to ProcessActions. No value is returned.
void ExecuteMenuItem(ProcessViewState& state, ProcessActionId actionId) {
    if (actionId == ProcessActionId::CopyCell || actionId == ProcessActionId::CopyRow) {
        const bool allColumns = actionId == ProcessActionId::CopyRow;
        const std::wstring text = SelectedRowsAsText(state, allColumns);
        const bool copied = WriteClipboardText(state.hwnd, text);
        SetStatus(state, copied ? L"已复制选中进程行文本。" : L"复制失败：没有可复制的选中行或剪贴板不可用。");
        return;
    }

    if (actionId == ProcessActionId::OpenDetails) {
        const std::vector<DWORD> pids = SelectedPids(state);
        if (pids.size() != 1) {
            SetStatus(state, L"进程详细信息需要单选一个进程。");
            return;
        }
        const bool opened = OpenProcessDetailWindow(state.hwnd, pids.front());
        SetStatus(state, opened ? L"已打开进程详细信息窗口。" : L"进程详细信息窗口创建失败。");
        return;
    }

    const ProcessActionResult result = ExecuteProcessAction(actionId, SelectedPids(state), state.model.rows());
    SetStatus(state, result.title + L": " + result.detail);
    if (!result.success) {
        ::MessageBoxW(state.hwnd, result.detail.c_str(), result.title.c_str(), MB_OK | MB_ICONINFORMATION);
    }
}

// ShowContextMenu builds and displays the grouped process context menu. Inputs are
// page state and screen coordinates. Processing folds related operations into
// submenus so the full retained ProcessDock action surface stays usable without
// a long root menu; selected command ids are mapped back to ProcessActionId.
void ShowContextMenu(ProcessViewState& state, POINT screenPoint) {
    const std::vector<DWORD> pids = SelectedPids(state);
    const bool hasVisibleSelection = !SelectedDisplayIndexes(state).empty();
    const bool hasProcessSelection = !pids.empty();
    const bool singleProcess = pids.size() == 1;
    state.activeMenuItems.clear();

    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }

    auto appendAction = [&](HMENU parent, ProcessActionId id, const wchar_t* text, bool enabled) {
        state.activeMenuItems.push_back(ProcessActionMenuItem{ id, text });
        const UINT command = kContextMenuBaseId + static_cast<UINT>(state.activeMenuItems.size() - 1);
        ::AppendMenuW(parent, MF_STRING | (enabled ? 0U : MF_GRAYED), command, text);
    };
    auto appendPopup = [](HMENU parent, HMENU child, const wchar_t* text) {
        ::AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(child), text);
    };

    HMENU copyMenu = ::CreatePopupMenu();
    appendAction(copyMenu, ProcessActionId::CopyCell, L"复制单元格", hasVisibleSelection);
    appendAction(copyMenu, ProcessActionId::CopyRow, L"复制行", hasVisibleSelection);
    appendPopup(menu, copyMenu, L"复制");

    HMENU processMenu = ::CreatePopupMenu();
    appendAction(processMenu, ProcessActionId::OpenDetails, L"进程详细信息", singleProcess);
    appendAction(processMenu, ProcessActionId::TerminateProcess, L"结束进程", hasProcessSelection);
    appendAction(processMenu, ProcessActionId::SuspendProcess, L"挂起进程", hasProcessSelection);
    appendAction(processMenu, ProcessActionId::ResumeProcess, L"恢复进程", hasProcessSelection);
    appendAction(processMenu, ProcessActionId::OpenFolder, L"打开所在目录", singleProcess);
    appendAction(processMenu, ProcessActionId::OpenMemoryOperation, L"跳转到内存操作", hasProcessSelection);
    appendAction(processMenu, ProcessActionId::ScanHotkeys, L"扫描进程热键", singleProcess);

    HMENU efficiencyMenu = ::CreatePopupMenu();
    appendAction(efficiencyMenu, ProcessActionId::EnableEfficiencyMode, L"开启效率模式", hasProcessSelection);
    appendAction(efficiencyMenu, ProcessActionId::DisableEfficiencyMode, L"关闭效率模式", hasProcessSelection);
    appendPopup(processMenu, efficiencyMenu, L"效率模式");

    HMENU criticalMenu = ::CreatePopupMenu();
    appendAction(criticalMenu, ProcessActionId::SetCriticalProcess, L"设为关键进程", hasProcessSelection);
    appendAction(criticalMenu, ProcessActionId::ClearCriticalProcess, L"取消关键进程", hasProcessSelection);
    appendPopup(processMenu, criticalMenu, L"关键进程");

    HMENU priorityMenu = ::CreatePopupMenu();
    appendAction(priorityMenu, ProcessActionId::SetPriorityIdle, L"Idle", hasProcessSelection);
    appendAction(priorityMenu, ProcessActionId::SetPriorityBelowNormal, L"Below Normal", hasProcessSelection);
    appendAction(priorityMenu, ProcessActionId::SetPriorityNormal, L"Normal", hasProcessSelection);
    appendAction(priorityMenu, ProcessActionId::SetPriorityAboveNormal, L"Above Normal", hasProcessSelection);
    appendAction(priorityMenu, ProcessActionId::SetPriorityHigh, L"High", hasProcessSelection);
    appendAction(priorityMenu, ProcessActionId::SetPriorityRealtime, L"Realtime", hasProcessSelection);
    appendPopup(processMenu, priorityMenu, L"优先级");
    appendPopup(menu, processMenu, L"进程");

    HMENU r0Menu = ::CreatePopupMenu();
    appendAction(r0Menu, ProcessActionId::R0TerminateProcess, L"R0结束进程", hasProcessSelection);
    appendAction(r0Menu, ProcessActionId::R0SuspendProcess, L"R0挂起进程", hasProcessSelection);
    appendAction(r0Menu, ProcessActionId::RefreshPplProtectionLevel, L"刷新PPL保护级别", hasProcessSelection);

    HMENU pplMenu = ::CreatePopupMenu();
    appendAction(pplMenu, ProcessActionId::R0SetPplNone, L"关闭PPL保护 (0x00)", hasProcessSelection);
    appendAction(pplMenu, ProcessActionId::R0SetPplAuthenticode, L"Authenticode (0x11)", hasProcessSelection);
    appendAction(pplMenu, ProcessActionId::R0SetPplCodeGen, L"CodeGen (0x21)", hasProcessSelection);
    appendAction(pplMenu, ProcessActionId::R0SetPplAntimalware, L"Antimalware (0x31)", hasProcessSelection);
    appendAction(pplMenu, ProcessActionId::R0SetPplLsa, L"Lsa (0x41)", hasProcessSelection);
    appendAction(pplMenu, ProcessActionId::R0SetPplWindows, L"Windows (0x51)", hasProcessSelection);
    appendAction(pplMenu, ProcessActionId::R0SetPplWinTcb, L"WinTcb (0x61)", hasProcessSelection);
    appendPopup(r0Menu, pplMenu, L"设置PPL");

    HMENU hideMenu = ::CreatePopupMenu();
    appendAction(hideMenu, ProcessActionId::R0HideUnlinkOnly, L"只断链", hasProcessSelection);
    appendAction(hideMenu, ProcessActionId::R0HidePatchPidOnly, L"只改PID", hasProcessSelection);
    appendAction(hideMenu, ProcessActionId::R0HideLegacyBoth, L"改PID+断链(旧版高风险)", hasProcessSelection);
    appendAction(hideMenu, ProcessActionId::R0UnhideProcess, L"取消隐藏选中进程", hasProcessSelection);
    appendAction(hideMenu, ProcessActionId::R0ClearHiddenMarks, L"清空全部隐藏标记", true);
    appendPopup(r0Menu, hideMenu, L"进程隐藏");

    HMENU specialMenu = ::CreatePopupMenu();
    appendAction(specialMenu, ProcessActionId::R0EnableBreakOnTermination, L"启用 BreakOnTermination", hasProcessSelection);
    appendAction(specialMenu, ProcessActionId::R0DisableBreakOnTermination, L"关闭 BreakOnTermination", hasProcessSelection);
    appendAction(specialMenu, ProcessActionId::R0DisableApcInsertion, L"禁止APC插入(现有线程)", hasProcessSelection);
    appendAction(specialMenu, ProcessActionId::R0DkomRemoveFromCidTable, L"DKOM从PspCidTable删除", hasProcessSelection);
    appendPopup(r0Menu, specialMenu, L"特殊/DKOM");
    appendPopup(menu, r0Menu, L"R0");

    if (!hasVisibleSelection && !hasProcessSelection) {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_GRAYED, 0, L"未选择进程");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        state.hwnd,
        nullptr);
    ::DestroyMenu(menu);

    if (command >= kContextMenuBaseId) {
        const std::size_t index = static_cast<std::size_t>(command - kContextMenuBaseId);
        if (index < state.activeMenuItems.size()) {
            ExecuteMenuItem(state, state.activeMenuItems[index].id);
        }
    }
}

// CreateChildControls creates toolbar buttons, status text and the ListView.
// Input is state with hwnd set. Processing also attaches the image list and
// inserts initial columns. No value is returned; missing HWNDs are tolerated by
// later null checks.
void CreateChildControls(ProcessViewState& state) {
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.pauseButton = Ksword::Ui::CreateButton(state.hwnd, kPauseButtonId, L"暂停", 0, 0, 0, 0);
    state.modeButton = Ksword::Ui::CreateButton(state.hwnd, kModeButtonId, L"利用率", 0, 0, 0, 0);
    state.pickerButton = Ksword::Ui::CreateButton(state.hwnd, kPickerButtonId, L"拖动选中进程", 0, 0, 0, 0);
    state.refreshSlider = ::CreateWindowExW(0,
        TRACKBAR_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_TOOLTIPS,
        0,
        0,
        0,
        0,
        state.hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshSliderId)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (state.refreshSlider) {
        ::SendMessageW(state.refreshSlider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
        ::SendMessageW(state.refreshSlider, TBM_SETTICFREQ, 1, 0);
        ::SendMessageW(state.refreshSlider, TBM_SETPOS, TRUE, state.refreshIntervalSeconds);
    }
    state.statusText = Ksword::Ui::CreateText(state.hwnd, kStatusTextId, L"准备枚举进程。", 0, 0, 0, 0);
    state.listView = CreateProcessListView(state.hwnd, kProcessListId);
    state.imageList = CreateIconList();
    if (state.listView && state.imageList) {
        ListView_SetImageList(state.listView, state.imageList, LVSIL_SMALL);
    }
    UpdateToolbarTexts(state);
    RebuildColumns(state);
}

// HandleListNotify processes ListView notifications. Inputs are page state and
// NMHDR. Processing supports custom first-column drawing, right-click selection
// correction and context menus; output carries both handled state and LRESULT.
NotifyResult HandleListNotify(ProcessViewState& state, NMHDR* header) {
    if (!header || header->hwndFrom != state.listView) {
        return {};
    }

    if (header->code == NM_CUSTOMDRAW) {
        return { true, HandleListCustomDraw(state, reinterpret_cast<NMLVCUSTOMDRAW*>(header)) };
    }

    if (header->code == NM_RCLICK) {
        POINT screenPoint{};
        ::GetCursorPos(&screenPoint);
        POINT clientPoint = screenPoint;
        ::ScreenToClient(state.listView, &clientPoint);

        LVHITTESTINFO hit{};
        hit.pt = clientPoint;
        const int item = ListView_HitTest(state.listView, &hit);
        if (GroupHeaderAtListItem(state, item)) {
            return { true, 0 };
        }
        if (item >= 0 && (ListView_GetItemState(state.listView, item, LVIS_SELECTED) & LVIS_SELECTED) == 0) {
            ListView_SetItemState(state.listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(state.listView, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        ShowContextMenu(state, screenPoint);
        return { true, 0 };
    }

    if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
        const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(header);
        if (!activate || activate->iItem < 0) {
            return {};
        }
        const ProcessDisplayRow* group = GroupHeaderAtListItem(state, activate->iItem);
        if (group) {
            const ProcessFriendlyGroup groupId = group->group;
            state.model.toggleGroupCollapsed(groupId);
            RebuildRows(state);
            SetStatus(state, state.model.isGroupCollapsed(groupId) ? L"已折叠进程分组。" : L"已展开进程分组。");
            return { true, 0 };
        }
    }

    return {};
}

// ProcessViewWndProc dispatches page messages. Inputs are standard Win32 window
// procedure parameters. Processing owns state lifetime, child layout, refresh,
// view switching, drag-picker completion and context menus. Output is an LRESULT
// compatible with DefWindowProcW.
LRESULT CALLBACK ProcessViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto state = std::make_unique<ProcessViewState>();
        state->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.release()));
    }

    ProcessViewState* state = StateFromWindow(hwnd);
    switch (msg) {
    case WM_CREATE:
        if (state) {
            CreateChildControls(*state);
            SetStatus(*state, L"进程页已创建，等待首次异步刷新。");
            ::PostMessageW(hwnd, kMsgInitialRefresh, 0, 0);
        }
        return 0;
    case kMsgInitialRefresh:
        if (state) {
            RefreshProcesses(*state);
            RestartRefreshTimer(*state);
        }
        return 0;
    case WM_SIZE:
        if (state) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            LayoutChildren(*state, rc);
        }
        return 0;
    case WM_COMMAND:
        if (state && HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case kRefreshButtonId:
                RefreshProcesses(*state);
                return 0;
            case kModeButtonId:
                ToggleMode(*state);
                return 0;
            case kPauseButtonId:
                ToggleRefreshPaused(*state);
                return 0;
            case kPickerButtonId:
                BeginPicker(*state);
                return 0;
            default:
                break;
            }
        }
        break;
    case WM_TIMER:
        if (state && wParam == kRefreshTimerId) {
            if (state->refreshPaused) {
                return 0;
            }
            if ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) {
                SetStatus(*state, L"左 Ctrl 按下，跳过本次自动刷新。");
                return 0;
            }
            RefreshProcesses(*state);
            return 0;
        }
        break;
    case WM_HSCROLL:
        if (state && reinterpret_cast<HWND>(lParam) == state->refreshSlider) {
            const LRESULT pos = ::SendMessageW(state->refreshSlider, TBM_GETPOS, 0, 0);
            state->refreshIntervalSeconds = static_cast<UINT>(std::clamp<LRESULT>(pos, 1, 10));
            RestartRefreshTimer(*state);
            SetStatus(*state, L"自动刷新频率: " + std::to_wstring(state->refreshIntervalSeconds) + L"s。");
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (state) {
            const NotifyResult notify = HandleListNotify(*state, reinterpret_cast<NMHDR*>(lParam));
            if (notify.handled) {
                return notify.result;
            }
        }
        break;
    case WM_CONTEXTMENU:
        if (state && reinterpret_cast<HWND>(wParam) == state->listView) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x != -1 || pt.y != -1) {
                return 0;
            }
            RECT rc{};
            ::GetWindowRect(state->listView, &rc);
            pt.x = rc.left + 24;
            pt.y = rc.top + 24;
            ShowContextMenu(*state, pt);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (state && state->pickingWindow) {
            CompletePicker(*state);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (state && state->pickingWindow && wParam == VK_ESCAPE) {
            state->pickingWindow = false;
            if (::GetCapture() == hwnd) {
                ::ReleaseCapture();
            }
            SetStatus(*state, L"拖动选中进程已取消。");
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (state && state->pickingWindow) {
            ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_CAPTURECHANGED:
        if (state && state->pickingWindow && reinterpret_cast<HWND>(lParam) != hwnd) {
            state->pickingWindow = false;
            SetStatus(*state, L"拖动选中进程已取消。");
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        PaintBackground(hwnd, dc);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        if (state) {
            ::KillTimer(hwnd, kRefreshTimerId);
            if (state->imageList) {
                ImageList_Destroy(state->imageList);
                state->imageList = nullptr;
            }
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// RegisterProcessViewClass installs the custom process page class once. There
// are no inputs; processing registers WNDCLASSW and accepts an existing class;
// output is true when CreateWindowExW can use the class name.
bool RegisterProcessViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = ProcessViewWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kProcessViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateProcessView(HWND parent, const RECT& bounds) {
    if (!RegisterProcessViewClass()) {
        return nullptr;
    }

    return ::CreateWindowExW(0,
        kProcessViewClass,
        L"Process List",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);
}

void ResizeProcessView(HWND view, const RECT& bounds) {
    if (view) {
        ::MoveWindow(view,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            TRUE);
    }
}

} // namespace Ksword::Features::Process
