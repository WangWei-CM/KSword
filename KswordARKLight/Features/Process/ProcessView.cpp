#include "ProcessView.h"

#include "ProcessActions.h"
#include "ProcessEnumerator.h"
#include "ProcessModel.h"
#include "../ProcessDetail/ProcessDetailFeature.h"
#include "../../Ui/Controls.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/LoadingOverlay.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
constexpr int kFilterBarId = 52010;
constexpr UINT kContextMenuBaseId = 53000;
constexpr UINT_PTR kRefreshTimerId = 52008;
constexpr UINT kMsgInitialRefresh = WM_APP + 520;
constexpr UINT kMsgRequestRefresh = WM_APP + 521;
constexpr UINT kMsgRefreshCompleted = WM_APP + 522;
constexpr UINT kMsgFilterCompleted = WM_APP + 523;
constexpr int kTreeIndentPixels = 18;
constexpr int kTreeIconGap = 4;
constexpr int kTreeTextGap = 4;

struct NotifyResult {
    bool handled = false;
    LRESULT result = 0;
};

// ProcessPresentationRow is the immutable UI snapshot for one process row.
// Keeping display text, identity and icon input together means owner-data
// callbacks never enumerate processes or format every row during a repaint.
struct ProcessPresentationRow {
    ProcessDisplayRow display;
    std::wstring stableKey;
    std::wstring iconPath;
    std::vector<std::wstring> cells;
    DWORD processId = 0;
    bool kernelOnly = false;
};

// ProcessFilterResult is produced from a copied presentation snapshot. The UI
// accepts it only when its display generation still matches the current list.
struct ProcessFilterResult {
    std::uint64_t displayGeneration = 0;
    std::wstring query;
    std::vector<std::size_t> visibleIndexes;
    std::vector<DWORD> selectedPids;
    std::wstring topStableKey;
};

enum class ProcessRowVisualState {
    Normal,
    KernelOnly,
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
    int maximumWidth = 0;
};

struct DetailHostCreateParams {
    DWORD processId = 0;
    int maximumWidth = 0;
};

constexpr wchar_t kProcessDetailHostClass[] = L"KswordARKLight.ProcessDetailHost";

bool EnsureDetailChild(HWND hwnd, DetailHostState& state) {
    if (state.child) {
        return true;
    }

    RECT client{};
    ::GetClientRect(hwnd, &client);
    state.child = Ksword::Features::ProcessDetail::CreateProcessDetailPage(hwnd, state.processId, client);
    if (!state.child) {
        return false;
    }
    ::ShowWindow(state.child, SW_SHOW);
    ::MoveWindow(state.child, 0, 0, client.right - client.left, client.bottom - client.top, TRUE);
    return true;
}

// DetailHostProc is the top-level process-detail host window procedure. Inputs
// are ordinary Win32 messages; processing creates/resizes the child detail page;
// output is the message LRESULT expected by DefWindowProcW callers.
LRESULT CALLBACK DetailHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DetailHostState* state = reinterpret_cast<DetailHostState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        const auto* parameters = create ? static_cast<const DetailHostCreateParams*>(create->lpCreateParams) : nullptr;
        auto* owned = new DetailHostState();
        if (parameters) {
            owned->processId = parameters->processId;
            owned->maximumWidth = parameters->maximumWidth;
        }
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<DetailHostState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        // Delay child-page creation until the first size/show pass. At WM_CREATE
        // the overlapped frame may not yet have its final client rectangle.
        return 0;
    case WM_SIZE:
        if (state && EnsureDetailChild(hwnd, *state)) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::MoveWindow(state->child, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        }
        return 0;
    case WM_SHOWWINDOW:
        if (state && wParam != FALSE) {
            EnsureDetailChild(hwnd, *state);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (state && state->maximumWidth > 0) {
            auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
            minMax->ptMaxTrackSize.x = state->maximumWidth;
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

    const HWND rootOwner = owner ? ::GetAncestor(owner, GA_ROOT) : nullptr;
    RECT available{};
    if (rootOwner) {
        ::GetClientRect(rootOwner, &available);
    }
    int availableWidth = available.right - available.left;
    if (availableWidth <= 0) {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &available, 0);
        availableWidth = available.right - available.left;
    }
    const int maximumWidth = std::max(720, availableWidth * 3 / 4);
    const int initialWidth = std::min(1160, maximumWidth);

    std::wstring processName = LeafName(QueryProcessImagePath(processId));
    if (processName.empty()) {
        processName = L"<unknown>";
    }
    const std::wstring title = L"进程详细信息 - " + processName + L" (PID " + std::to_wstring(processId) + L")";
    const DetailHostCreateParams parameters{ processId, maximumWidth };
    HWND host = ::CreateWindowExW(WS_EX_APPWINDOW,
        kProcessDetailHostClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        initialWidth,
        760,
        rootOwner,
        nullptr,
        ::GetModuleHandleW(nullptr),
        const_cast<DetailHostCreateParams*>(&parameters));
    if (!host) {
        return false;
    }

    ::ShowWindow(host, SW_SHOWNORMAL);
    ::UpdateWindow(host);
    auto* state = reinterpret_cast<DetailHostState*>(::GetWindowLongPtrW(host, GWLP_USERDATA));
    if (!state || !EnsureDetailChild(host, *state)) {
        ::DestroyWindow(host);
        return false;
    }
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
    HWND filterBar = nullptr;
    HWND listView = nullptr;
    HWND loadingOverlay = nullptr;
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
    std::vector<ProcessPresentationRow> presentationRows;
    std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>> filterRows;
    std::vector<std::size_t> visibleRowIndexes;
    std::wstring displayTextScratch;
    std::wstring filterQuery;
    std::uint64_t displayGeneration = 0;
    bool hasLastActiveSnapshot = false;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<struct ProcessRefreshSnapshot>> refreshTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ProcessFilterResult>> filterTask;
};

// KernelProcessSnapshotEntry 用途：保存 ArkDriverClient R0 枚举返回的一行进程证据。
// 调用方式：EnumerateProcessesByR0Driver 填充，ApplyDefaultHiddenProcessAudit 合并到 R3 行。
struct KernelProcessSnapshotEntry {
    std::uint32_t processId = 0;
    std::uint32_t parentProcessId = 0;
    std::uint32_t flags = 0;
    std::uint32_t sessionId = 0;
    std::uint32_t fieldFlags = 0;
    std::uint32_t r0Status = KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE;
    std::string imageName;
    std::string imagePath;
};

// HiddenProcessAuditResult 用途：汇总默认 R0 查隐藏进程结果，最终追加到状态栏。
// 返回值语义：querySucceeded=false 表示驱动不可用或 IOCTL 失败，但普通 R3 刷新继续执行。
struct HiddenProcessAuditResult {
    bool querySucceeded = false;
    std::size_t kernelEnumeratedCount = 0;
    std::size_t kernelOnlyCount = 0;
    std::wstring detailText;
};

// ProcessRefreshSnapshot is produced entirely on a worker thread. It keeps R3
// enumeration and both R0 evidence queries out of timer and button handlers.
struct ProcessRefreshSnapshot {
    ProcessEnumerationResult enumeration;
    HiddenProcessAuditResult hiddenAudit;
    std::wstring crossViewStatusSuffix;
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
            { 9, 150, LVCFMT_RIGHT, L"EPROCESS" },
            { 10, 150, LVCFMT_LEFT, L"R0来源" },
            { 11, 180, LVCFMT_LEFT, L"R0异常" },
            { 12, 420, LVCFMT_LEFT, L"映像路径" },
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
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
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
    x += 110;

    const int statusTop = buttonTop + buttonHeight;
    const int width = std::max(100, static_cast<int>(rc.right - rc.left));
    const int height = std::max(100, static_cast<int>(rc.bottom - rc.top));
    if (state.filterBar) {
        ::MoveWindow(state.filterBar,
            x + 8,
            buttonTop,
            std::max(1, width - x - 8),
            buttonHeight,
            TRUE);
    }
    ::MoveWindow(state.statusText, 0, statusTop, width, 20, TRUE);

    const int listTop = statusTop + 20;
    ::MoveWindow(state.listView,
        0,
        listTop,
        std::max(80, width),
        std::max(80, height - listTop),
        TRUE);
    if (state.loadingOverlay) {
        ::MoveWindow(state.loadingOverlay,
            0,
            listTop,
            std::max(80, width),
            std::max(80, height - listTop),
            TRUE);
    }
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

// SelectedPids and the filtering scheduler are declared here because installing
// a new immutable presentation must save selection and viewport before replacing
// the owner-data row count.
std::vector<DWORD> SelectedPids(ProcessViewState& state);
std::wstring StableKeyFromListItem(const ProcessViewState& state, int item);
void RequestProcessFilter(ProcessViewState& state,
    const std::wstring& query,
    std::vector<DWORD> selectedPids,
    std::wstring topStableKey);

// BuildPresentationRows converts the model into immutable owner-data rows once
// per completed snapshot. Formatting is deliberately not performed by
// LVN_GETDISPINFO or custom draw, keeping scrolling independent of table size.
void BuildPresentationRows(ProcessViewState& state,
    std::vector<ProcessPresentationRow>& presentationRows,
    std::vector<Ksword::Ui::VirtualListRow>& filterRows) {
    const auto& sourceRows = state.model.displayRows(state.mode);
    const auto activeColumns = ColumnSet(state.mode);
    const auto detailColumns = ColumnSet(ProcessViewMode::Detail);
    presentationRows.clear();
    filterRows.clear();
    presentationRows.reserve(sourceRows.size());
    filterRows.reserve(sourceRows.size());

    for (const ProcessDisplayRow& sourceRow : sourceRows) {
        ProcessPresentationRow row{};
        row.display = sourceRow;
        row.cells.reserve(activeColumns.size());
        for (const auto& column : activeColumns) {
            row.cells.push_back(state.model.textForColumn(sourceRow, column.index, state.mode));
        }

        if (const ProcessSnapshotRow* process = state.model.rowForDisplayRow(sourceRow)) {
            row.processId = process->processId;
            row.kernelOnly = process->r0KernelOnly;
            row.iconPath = process->imagePath;
            row.stableKey = L"pid:" + std::to_wstring(process->processId);
        } else {
            row.stableKey = L"group:" + std::to_wstring(static_cast<int>(sourceRow.group));
        }

        Ksword::Ui::VirtualListRow filterRow{};
        filterRow.stableKey = row.stableKey;
        filterRow.cells = row.cells;
        // Always include detail-view text, even when the compact utilization
        // layout is active, so filtering covers hidden details and R0 evidence.
        for (const auto& column : detailColumns) {
            filterRow.cells.push_back(state.model.textForColumn(sourceRow, column.index, ProcessViewMode::Detail));
        }
        if (!row.iconPath.empty()) {
            filterRow.cells.push_back(row.iconPath);
        }

        presentationRows.push_back(std::move(row));
        filterRows.push_back(std::move(filterRow));
    }
}

// RebuildRows replaces the backing presentation snapshot, then schedules a
// background match against it. LVS_OWNERDATA receives only an item count here;
// no per-row ListView_InsertItem or ListView_SetItemText messages are sent.
void RebuildRows(ProcessViewState& state) {
    if (!state.listView) {
        return;
    }

    const std::vector<DWORD> selectedPids = SelectedPids(state);
    const std::wstring topStableKey = StableKeyFromListItem(state, ListView_GetTopIndex(state.listView));
    auto filterRows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>();
    BuildPresentationRows(state, state.presentationRows, *filterRows);
    state.filterRows = std::move(filterRows);
    ++state.displayGeneration;

    // The old row mapping belongs to the previous immutable snapshot. Clear it
    // until the matching result arrives rather than letting stale indexes point
    // at newly enumerated process data.
    state.visibleRowIndexes.clear();
    {
        Ksword::Ui::ScopedListViewRedrawLock redrawLock(state.listView);
        ListView_SetItemCountEx(state.listView, 0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    }
    ::InvalidateRect(state.listView, nullptr, FALSE);

    const std::wstring query = state.filterBar ? Ksword::Ui::GetFilterBarText(state.filterBar) : state.filterQuery;
    RequestProcessFilter(state, query, selectedPids, topStableKey);
}

// DisplayIndexFromListItem maps a visible owner-data row to its immutable
// presentation index. It does not call ListView_GetItem because owner-data
// controls do not retain one LVITEM per row.
int DisplayIndexFromListItem(const ProcessViewState& state, int item) {
    if (item < 0 || static_cast<std::size_t>(item) >= state.visibleRowIndexes.size()) {
        return -1;
    }
    const std::size_t displayIndex = state.visibleRowIndexes[static_cast<std::size_t>(item)];
    return displayIndex <= static_cast<std::size_t>(INT_MAX) ? static_cast<int>(displayIndex) : -1;
}

// SelectedDisplayIndexes reads the current multi-selection and translates the
// visible owner-data indexes through the immutable result mapping.
std::vector<int> SelectedDisplayIndexes(ProcessViewState& state) {
    std::vector<int> indexes;
    if (!state.listView) {
        return indexes;
    }

    int item = -1;
    while ((item = ListView_GetNextItem(state.listView, item, LVNI_SELECTED)) != -1) {
        const int displayIndex = DisplayIndexFromListItem(state, item);
        if (displayIndex >= 0) {
            indexes.push_back(displayIndex);
        }
    }
    return indexes;
}

// SelectedPids converts selected immutable presentation rows to process ids.
// Group headers remain selectable for copy operations but never become actions.
std::vector<DWORD> SelectedPids(ProcessViewState& state) {
    std::vector<DWORD> pids;
    for (const int index : SelectedDisplayIndexes(state)) {
        if (index < 0 || static_cast<std::size_t>(index) >= state.presentationRows.size()) {
            continue;
        }
        const DWORD processId = state.presentationRows[static_cast<std::size_t>(index)].processId;
        if (processId != 0) {
            pids.push_back(processId);
        }
    }
    return pids;
}

std::wstring StableKeyFromListItem(const ProcessViewState& state, const int item) {
    const int displayIndex = DisplayIndexFromListItem(state, item);
    if (displayIndex < 0 || static_cast<std::size_t>(displayIndex) >= state.presentationRows.size()) {
        return {};
    }
    return state.presentationRows[static_cast<std::size_t>(displayIndex)].stableKey;
}

// GroupHeaderAtListItem returns the clicked friendly group header from the
// current immutable presentation snapshot.
const ProcessPresentationRow* GroupHeaderAtListItem(ProcessViewState& state, int item) {
    const int displayIndex = DisplayIndexFromListItem(state, item);
    if (displayIndex < 0 || static_cast<std::size_t>(displayIndex) >= state.presentationRows.size()) {
        return nullptr;
    }
    const ProcessPresentationRow& row = state.presentationRows[static_cast<std::size_t>(displayIndex)];
    return row.display.groupHeader ? &row : nullptr;
}

// DisplayRowFromListItem resolves a visible owner-data row without touching the
// model. Custom drawing therefore remains O(visible cells), even for long lists.
const ProcessPresentationRow* DisplayRowFromListItem(ProcessViewState& state, int item) {
    const int displayIndex = DisplayIndexFromListItem(state, item);
    if (displayIndex < 0 || static_cast<std::size_t>(displayIndex) >= state.presentationRows.size()) {
        return nullptr;
    }
    return &state.presentationRows[static_cast<std::size_t>(displayIndex)];
}

// VisualStateForDisplayRow resolves green/gray lifecycle highlighting for a
// visible process row. Inputs are page state and display row; processing maps
// process PID into the latest refresh-diff table; output is Normal for groups
// and unchanged process rows.
ProcessRowVisualState VisualStateForDisplayRow(ProcessViewState& state, const ProcessPresentationRow& displayRow) {
    if (displayRow.processId == 0) {
        return ProcessRowVisualState::Normal;
    }
    if (displayRow.display.groupHeader) {
        return ProcessRowVisualState::Normal;
    }
    if (displayRow.kernelOnly) {
        return ProcessRowVisualState::KernelOnly;
    }
    const auto source = state.visualStateByPid.find(displayRow.processId);
    return source != state.visualStateByPid.end() ? source->second : ProcessRowVisualState::Normal;
}

// RowBackgroundColor returns the fill color used by custom draw. Inputs are
// selection and lifecycle state; processing gives selected rows system colors,
// new rows light green and deleted rows light gray; output is a COLORREF.
COLORREF RowBackgroundColor(bool selected, ProcessRowVisualState visualState) {
    if (selected) {
        return ::GetSysColor(COLOR_HIGHLIGHT);
    }
    if (visualState == ProcessRowVisualState::KernelOnly) {
        return RGB(255, 226, 226);
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
    if (visualState == ProcessRowVisualState::KernelOnly) {
        return RGB(150, 0, 0);
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
    const ProcessPresentationRow* displayRow = DisplayRowFromListItem(state, item);
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

    if (subItem == 0 && !displayRow->display.groupHeader) {
        DrawDottedTreeGuides(dc, cell, displayRow->display.depth);
        const int iconSize = ::GetSystemMetrics(SM_CXSMICON);
        const int iconLeft = cell.left + displayRow->display.depth * kTreeIndentPixels;
        const int rowHeight = static_cast<int>(cell.bottom - cell.top);
        const int iconTop = static_cast<int>(cell.top) + std::max(0, (rowHeight - iconSize) / 2);
        const int iconIndex = IconIndexForPath(state, displayRow->iconPath);
        if (state.imageList && iconIndex >= 0) {
            ImageList_Draw(state.imageList, iconIndex, dc, iconLeft, iconTop, ILD_TRANSPARENT);
        }

        RECT textRect = cell;
        textRect.left = iconLeft + iconSize + kTreeIconGap + kTreeTextGap;
        const std::wstring empty;
        const std::wstring& text = displayRow->cells.empty() ? empty : displayRow->cells.front();
        ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT textRect = cell;
        textRect.left += 4;
        textRect.right -= 4;
        const std::wstring empty;
        const std::wstring& text = subItem >= 0 && static_cast<std::size_t>(subItem) < displayRow->cells.size()
            ? displayRow->cells[static_cast<std::size_t>(subItem)]
            : empty;
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

// HandleVirtualListDisplayInfo supplies only the text and lParam requested for
// visible owner-data rows. The backing ProcessPresentationRow snapshot owns the
// text for the entire notification, so no temporary process enumeration occurs.
LRESULT HandleVirtualListDisplayInfo(ProcessViewState& state, NMLVDISPINFOW* displayInfo) {
    if (!displayInfo) {
        return 0;
    }

    const int displayIndex = DisplayIndexFromListItem(state, displayInfo->item.iItem);
    if (displayIndex < 0 || static_cast<std::size_t>(displayIndex) >= state.presentationRows.size()) {
        return 0;
    }

    const ProcessPresentationRow& row = state.presentationRows[static_cast<std::size_t>(displayIndex)];
    if ((displayInfo->item.mask & LVIF_TEXT) != 0) {
        const int subItem = displayInfo->item.iSubItem;
        state.displayTextScratch = subItem >= 0 && static_cast<std::size_t>(subItem) < row.cells.size()
            ? row.cells[static_cast<std::size_t>(subItem)]
            : std::wstring{};
        displayInfo->item.pszText = state.displayTextScratch.data();
    }
    if ((displayInfo->item.mask & LVIF_PARAM) != 0) {
        displayInfo->item.lParam = static_cast<LPARAM>(displayIndex);
    }
    return 0;
}

// ApplyProcessFilter installs a background-filtered index map while preserving
// selection and the top visible logical row whenever they remain present.
void ApplyProcessFilter(ProcessViewState& state, ProcessFilterResult result) {
    if (!state.listView || result.displayGeneration != state.displayGeneration || result.query != state.filterQuery) {
        return;
    }

    state.visibleRowIndexes = std::move(result.visibleIndexes);
    int topItem = -1;
    int firstSelectedItem = -1;
    {
        Ksword::Ui::ScopedListViewRedrawLock redrawLock(state.listView);
        ListView_SetItemCountEx(state.listView,
            static_cast<int>(std::min<std::size_t>(state.visibleRowIndexes.size(), static_cast<std::size_t>(INT_MAX))),
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        ListView_SetItemState(state.listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);

        std::unordered_set<DWORD> selectedSet(result.selectedPids.begin(), result.selectedPids.end());
        for (std::size_t item = 0; item < state.visibleRowIndexes.size(); ++item) {
            const std::size_t displayIndex = state.visibleRowIndexes[item];
            if (displayIndex >= state.presentationRows.size()) {
                continue;
            }
            const ProcessPresentationRow& row = state.presentationRows[displayIndex];
            if (topItem < 0 && !result.topStableKey.empty() && row.stableKey == result.topStableKey) {
                topItem = static_cast<int>(item);
            }
            if (row.processId != 0 && selectedSet.find(row.processId) != selectedSet.end()) {
                const int selectedItem = static_cast<int>(item);
                ListView_SetItemState(state.listView, selectedItem, LVIS_SELECTED, LVIS_SELECTED);
                if (firstSelectedItem < 0) {
                    firstSelectedItem = selectedItem;
                }
            }
        }
        if (firstSelectedItem >= 0) {
            ListView_SetItemState(state.listView, firstSelectedItem, LVIS_FOCUSED, LVIS_FOCUSED);
        }
        if (topItem >= 0) {
            ListView_EnsureVisible(state.listView, topItem, FALSE);
        } else if (firstSelectedItem >= 0) {
            ListView_EnsureVisible(state.listView, firstSelectedItem, FALSE);
        }
    }
    ::InvalidateRect(state.listView, nullptr, FALSE);
    if (!result.query.empty()) {
        SetStatus(state,
            L"筛选结果 " + std::to_wstring(state.visibleRowIndexes.size()) +
            L" / " + std::to_wstring(state.presentationRows.size()) + L" 行。");
    }
}

// RequestProcessFilter captures a shared, immutable text snapshot. Repeated
// keystrokes coalesce in AsyncSnapshotTask and its generation check prevents a
// stale background match from overwriting a newer query or process refresh.
void RequestProcessFilter(ProcessViewState& state,
    const std::wstring& query,
    std::vector<DWORD> selectedPids,
    std::wstring topStableKey) {
    state.filterQuery = query;
    const std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>> rows = state.filterRows;
    const std::uint64_t displayGeneration = state.displayGeneration;
    if (!state.filterTask || !rows) {
        ProcessFilterResult result{};
        result.displayGeneration = displayGeneration;
        result.query = query;
        result.selectedPids = std::move(selectedPids);
        result.topStableKey = std::move(topStableKey);
        result.visibleIndexes.resize(state.presentationRows.size());
        for (std::size_t index = 0; index < result.visibleIndexes.size(); ++index) {
            result.visibleIndexes[index] = index;
        }
        ApplyProcessFilter(state, std::move(result));
        return;
    }

    state.filterTask->request(
        [rows, displayGeneration, query, selectedPids = std::move(selectedPids), topStableKey = std::move(topStableKey)]() mutable {
            ProcessFilterResult result{};
            result.displayGeneration = displayGeneration;
            result.query = std::move(query);
            result.selectedPids = std::move(selectedPids);
            result.topStableKey = std::move(topStableKey);
            result.visibleIndexes = Ksword::Ui::VirtualListView::FilterRowIndexes(*rows, result.query);
            return result;
        },
        [&state](std::uint64_t, std::optional<ProcessFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value()) {
                SetStatus(state, L"进程筛选任务异常结束。已保留当前可见结果。");
                return;
            }
            ApplyProcessFilter(state, std::move(*result));
        });
}

// SelectedRowsAsText builds tab-separated text for selected visible rows. Inputs
// are page state and whether all columns should be copied. Processing reads the
// current model display rows; output is suitable for the clipboard.
std::wstring SelectedRowsAsText(ProcessViewState& state, bool allColumns) {
    const auto indexes = SelectedDisplayIndexes(state);
    std::wstring text;

    for (int index : indexes) {
        if (index < 0 || static_cast<std::size_t>(index) >= state.presentationRows.size()) {
            continue;
        }
        const ProcessPresentationRow& row = state.presentationRows[static_cast<std::size_t>(index)];
        const int maxColumn = allColumns ? static_cast<int>(row.cells.size()) : std::min<int>(1, static_cast<int>(row.cells.size()));
        for (int column = 0; column < maxColumn; ++column) {
            if (column != 0) {
                text += L'\t';
            }
            text += row.cells[static_cast<std::size_t>(column)];
        }
        text += L"\r\n";
    }
    return text;
}

// VisibleRowsAsText exports the current owner-data result mapping, allowing the
// context menu to copy every filtered row without selecting or repainting them.
std::wstring VisibleRowsAsText(const ProcessViewState& state) {
    std::wstring text;
    for (const std::size_t displayIndex : state.visibleRowIndexes) {
        if (displayIndex >= state.presentationRows.size()) {
            continue;
        }
        const ProcessPresentationRow& row = state.presentationRows[displayIndex];
        for (std::size_t column = 0; column < row.cells.size(); ++column) {
            if (column != 0) {
                text += L'\t';
            }
            text += row.cells[column];
        }
        text += L"\r\n";
    }
    return text;
}

// NarrowToWide converts ArkDriverClient diagnostic strings to the native UI
// encoding. Input is UTF-8/ASCII text from the shared wrapper; output is a
// best-effort wide string suitable for status bars and R0 evidence cells.
std::wstring NarrowToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    int chars = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (chars <= 0) {
        codePage = CP_ACP;
        chars = ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (chars <= 0) {
        return L"<decode failed>";
    }

    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), wide.data(), chars);
    return wide;
}

// FlagHexText formats raw source/anomaly masks without interpreting unsupported
// future bits. Input is a protocol mask; output is stable uppercase hex.
std::wstring FlagHexText(ULONG value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

// ProcessR0StatusText 用途：把 R0 枚举行状态转换成状态栏/表格可读文本。
// 输入 statusValue 为 KSWORD_ARK_PROCESS_R0_STATUS_*；返回中文短语，未知值保留数字。
std::wstring ProcessR0StatusText(const std::uint32_t statusValue) {
    switch (statusValue) {
    case KSWORD_ARK_PROCESS_R0_STATUS_OK:
        return L"OK";
    case KSWORD_ARK_PROCESS_R0_STATUS_PARTIAL:
        return L"Partial";
    case KSWORD_ARK_PROCESS_R0_STATUS_DYNDATA_MISSING:
        return L"DynData missing";
    case KSWORD_ARK_PROCESS_R0_STATUS_READ_FAILED:
        return L"Read failed";
    default:
        return L"Unavailable";
    }
}

// EnumerateProcessesByR0Driver 用途：复用 Ksword5.1 的 R0 进程枚举方式查隐藏进程。
// 参数 processListOut 接收 R0 行；detailTextOut 接收 ArkDriverClient 诊断文本；返回 true 表示可对比。
bool EnumerateProcessesByR0Driver(
    std::vector<KernelProcessSnapshotEntry>* const processListOut,
    std::wstring* const detailTextOut) {
    if (processListOut == nullptr) {
        return false;
    }
    processListOut->clear();
    if (detailTextOut != nullptr) {
        detailTextOut->clear();
    }

    const ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessEnumResult enumResult = driverClient.enumerateProcesses(
        KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE);
    if (!enumResult.io.ok) {
        if (detailTextOut != nullptr) {
            *detailTextOut = NarrowToWide(enumResult.io.message);
        }
        return false;
    }

    processListOut->reserve(enumResult.entries.size());
    for (const ksword::ark::ProcessEntry& entry : enumResult.entries) {
        KernelProcessSnapshotEntry processEntry{};
        processEntry.processId = entry.processId;
        processEntry.parentProcessId = entry.parentProcessId;
        processEntry.flags = entry.flags;
        processEntry.sessionId = entry.sessionId;
        processEntry.fieldFlags = entry.fieldFlags;
        processEntry.r0Status = entry.r0Status;
        processEntry.imageName = entry.imageName;
        processEntry.imagePath = entry.imagePath;
        processListOut->push_back(std::move(processEntry));
    }

    if (detailTextOut != nullptr) {
        *detailTextOut = NarrowToWide(enumResult.io.message);
    }
    return true;
}

// PromptOpenPayloadFile shows a common open-file dialog for R0 injection payloads.
// Inputs are owner/filter/title; processing never changes the current directory;
// output is empty when the user cancels.
std::wstring PromptOpenPayloadFile(HWND owner, const wchar_t* filter, const wchar_t* title) {
    wchar_t path[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(std::size(path));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return ::GetOpenFileNameW(&ofn) ? std::wstring(path) : std::wstring();
}

// ConfirmR0Injection asks for the explicit UI confirmation expected by the
// shared R0 injection protocol. Inputs are owner/action/pid/path; output is true
// only when the user accepts the high-risk operation.
bool ConfirmR0Injection(HWND owner, const wchar_t* action, DWORD pid, const std::wstring& path) {
    std::wstring message = L"将通过 KswordARK R0 进程注入协议执行操作：";
    message += action ? action : L"注入";
    message += L"\r\n\r\n目标 PID: " + std::to_wstring(pid);
    message += L"\r\nPayload: " + path;
    message += L"\r\n\r\n该操作会在目标进程创建远程线程，可能导致目标崩溃或系统不稳定。是否继续？";
    return ::MessageBoxW(owner, message.c_str(), L"确认 R0 注入", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES;
}

// MergeKernelProcessExtension 用途：把 R0 枚举字段合并到 Light 进程行，不覆盖 R3 公开 API 的基础信息。
// 参数 row 是目标 UI 行，kernelProcess 是 R0 行；无返回值，调用方继续决定是否合成隐藏行。
void MergeKernelProcessExtension(
    ProcessSnapshotRow& row,
    const KernelProcessSnapshotEntry& kernelProcess) {
    row.r0EnumFlags = kernelProcess.flags;
    row.r0EnumStatus = kernelProcess.r0Status;
    row.r0EnumImagePath = NarrowToWide(kernelProcess.imagePath);
    if ((kernelProcess.fieldFlags & KSWORD_ARK_PROCESS_FIELD_SESSION_PRESENT) != 0U) {
        row.sessionId = kernelProcess.sessionId;
    }
    if (!row.r0EnumImagePath.empty() && row.imagePath.empty()) {
        row.imagePath = row.r0EnumImagePath;
    }
}

// BuildKernelOnlyRow 用途：构造 R0 有、R3 无的合成行，和 Ksword5.1 的隐藏进程文案保持一致。
// 输入 kernelProcess 为 R0 枚举行；返回可直接追加到 ProcessModel 的 ProcessSnapshotRow。
ProcessSnapshotRow BuildKernelOnlyRow(const KernelProcessSnapshotEntry& kernelProcess) {
    ProcessSnapshotRow row{};
    row.processId = static_cast<DWORD>(kernelProcess.processId);
    row.parentProcessId = static_cast<DWORD>(kernelProcess.parentProcessId);
    row.r0KernelOnly = true;
    MergeKernelProcessExtension(row, kernelProcess);

    const std::wstring imageName = NarrowToWide(kernelProcess.imageName);
    row.imageName = imageName.empty() ? L"[R0] Unknown" : (L"[R0] " + imageName);
    const bool cidTableWeakEvidence =
        (kernelProcess.flags &
            (KSWORD_ARK_PROCESS_FLAG_CID_TABLE_REFERENCE_FAILED |
                KSWORD_ARK_PROCESS_FLAG_TERMINATING_OR_EXITED)) != 0U;
    row.imagePath = cidTableWeakEvidence
        ? L"[CID Table命中：对象引用失败或已退出]"
        : L"[仅内核枚举可见]";
    row.r0AuditSummary = L"KernelOnly(Hidden?)";
    row.r0AuditDetail = cidTableWeakEvidence
        ? L"CID Table命中：保留显示，可尝试R0结束"
        : L"仅内核枚举可见";
    row.r0AuditDetail += L"; flags=" + FlagHexText(kernelProcess.flags);
    row.r0AuditDetail += L"; status=" + ProcessR0StatusText(kernelProcess.r0Status);
    return row;
}

// ApplyDefaultHiddenProcessAudit 用途：默认执行 R0/R3 进程列表差异对比，找出疑似隐藏进程。
// 返回值包含状态栏摘要；驱动不可用时只返回失败摘要，不影响 rows 中的 R3 枚举结果。
HiddenProcessAuditResult ApplyDefaultHiddenProcessAudit(std::vector<ProcessSnapshotRow>& rows) {
    HiddenProcessAuditResult result{};
    std::vector<KernelProcessSnapshotEntry> kernelRows;
    std::wstring queryDetail;
    if (!EnumerateProcessesByR0Driver(&kernelRows, &queryDetail)) {
        result.detailText = L"R0隐藏检查: 待驱动装载/查询失败";
        if (!queryDetail.empty()) {
            result.detailText += L" " + queryDetail;
        }
        return result;
    }

    result.querySucceeded = true;
    result.kernelEnumeratedCount = kernelRows.size();

    std::unordered_map<DWORD, const KernelProcessSnapshotEntry*> kernelByPid;
    kernelByPid.reserve(kernelRows.size() * 2U + 1U);
    for (const KernelProcessSnapshotEntry& kernelProcess : kernelRows) {
        kernelByPid[static_cast<DWORD>(kernelProcess.processId)] = &kernelProcess;
    }

    std::unordered_set<DWORD> userPidSet;
    userPidSet.reserve(rows.size() * 2U + 1U);
    for (ProcessSnapshotRow& row : rows) {
        userPidSet.insert(row.processId);
        const auto found = kernelByPid.find(row.processId);
        if (found != kernelByPid.end()) {
            MergeKernelProcessExtension(row, *found->second);
        }
    }

    for (const KernelProcessSnapshotEntry& kernelProcess : kernelRows) {
        const DWORD processId = static_cast<DWORD>(kernelProcess.processId);
        if (userPidSet.find(processId) != userPidSet.end()) {
            continue;
        }
        rows.push_back(BuildKernelOnlyRow(kernelProcess));
        userPidSet.insert(processId);
        ++result.kernelOnlyCount;
    }

    result.detailText = L"R0隐藏检查: 内核返回 " +
        std::to_wstring(result.kernelEnumeratedCount) +
        L"，疑似隐藏 " +
        std::to_wstring(result.kernelOnlyCount);
    return result;
}

// SourceMaskText renders the process cross-view source matrix. Input is the raw
// sourceMask from R0; processing keeps names aligned with the shared protocol.
std::wstring SourceMaskText(ULONG sourceMask) {
    std::vector<std::wstring> parts;
    if ((sourceMask & KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK) != 0) {
        parts.push_back(L"Public");
    }
    if ((sourceMask & KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST) != 0) {
        parts.push_back(L"ActiveProcessLinks");
    }
    if ((sourceMask & KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE) != 0) {
        parts.push_back(L"CID");
    }
    if (parts.empty()) {
        return L"无来源";
    }

    std::wstring text;
    for (const std::wstring& part : parts) {
        if (!text.empty()) {
            text += L"+";
        }
        text += part;
    }
    return text;
}

// AnomalyMaskText renders known process/thread anomaly bits as evidence labels.
// Input is the raw anomalyFlags mask; output keeps unknown future bits visible.
std::wstring AnomalyMaskText(ULONG anomalyFlags) {
    if (anomalyFlags == 0) {
        return L"未见异常";
    }

    std::vector<std::wstring> parts;
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) != 0) {
        parts.push_back(L"CID-only");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) != 0) {
        parts.push_back(L"Active-only");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) != 0) {
        parts.push_back(L"缺ActiveProcessLinks");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) != 0) {
        parts.push_back(L"缺CID");
    }
    if ((anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_PID_FIELD_MISMATCH) != 0) {
        parts.push_back(L"PID不一致");
    }

    std::wstring text;
    for (const std::wstring& part : parts) {
        if (!text.empty()) {
            text += L"; ";
        }
        text += part;
    }
    if (text.empty()) {
        text = L"未知异常位 " + FlagHexText(anomalyFlags);
    }
    return text;
}

// ApplyR0ProcessAuditRows annotates R3 process rows with process cross-view
// evidence. Inputs are mutable rows and a status suffix; processing calls only
// ArkDriverClient wrappers and never issues raw DeviceIoControl from the UI.
void ApplyR0ProcessAuditRows(std::vector<ProcessSnapshotRow>& rows, std::wstring& statusSuffix) {
    const ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessCrossViewResult audit = driverClient.queryProcessCrossView(
        KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_ALL,
        0,
        0,
        KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES);
    if (!audit.io.ok) {
        statusSuffix = L"；R0审计不可用: " + NarrowToWide(audit.io.message);
        for (ProcessSnapshotRow& row : rows) {
            if (row.r0KernelOnly) {
                row.r0AuditSummary = row.r0AuditSummary.empty() ? L"KernelOnly(Hidden?)" : row.r0AuditSummary;
                if (!row.r0AuditDetail.empty()) {
                    row.r0AuditDetail += L"; CrossView不可用";
                } else {
                    row.r0AuditDetail = audit.unsupported ? L"驱动不支持CrossView" : L"CrossView查询失败";
                }
                continue;
            }
            row.r0AuditSummary = L"不可用";
            row.r0AuditDetail = audit.unsupported ? L"驱动不支持" : L"查询失败";
        }
        return;
    }

    std::unordered_map<DWORD, const ksword::ark::ProcessCrossViewEntry*> auditByPid;
    auditByPid.reserve(audit.entries.size());
    for (const ksword::ark::ProcessCrossViewEntry& entry : audit.entries) {
        auditByPid[static_cast<DWORD>(entry.processId)] = &entry;
    }

    std::size_t matched = 0;
    std::size_t anomalous = 0;
    for (ProcessSnapshotRow& row : rows) {
        const auto found = auditByPid.find(row.processId);
        if (found == auditByPid.end()) {
            if (row.r0KernelOnly) {
                row.r0AuditSummary = row.r0AuditSummary.empty() ? L"KernelOnly(Hidden?)" : row.r0AuditSummary;
                if (!row.r0AuditDetail.empty()) {
                    row.r0AuditDetail += L"; CrossView未返回";
                } else {
                    row.r0AuditDetail = L"仅 R0 枚举返回，CrossView未返回";
                }
                continue;
            }
            row.r0AuditSummary = L"R3-only";
            row.r0AuditDetail = L"R0未返回";
            continue;
        }

        const ksword::ark::ProcessCrossViewEntry& entry = *found->second;
        row.r0ProcessObjectAddress = static_cast<std::uintptr_t>(entry.objectAddress);
        row.r0SourceMask = entry.sourceMask;
        row.r0AnomalyFlags = entry.anomalyFlags;
        row.r0Confidence = entry.confidence;
        row.r0AuditSummary = SourceMaskText(row.r0SourceMask);
        row.r0AuditDetail = AnomalyMaskText(row.r0AnomalyFlags);
        row.r0AuditDetail += L"; conf=" + std::to_wstring(row.r0Confidence);
        if (!entry.detail.empty()) {
            row.r0AuditDetail += L"; " + NarrowToWide(entry.detail);
        }
        ++matched;
        if (row.r0AnomalyFlags != 0) {
            ++anomalous;
        }
    }

    statusSuffix = L"；R0审计匹配 " + std::to_wstring(matched) +
        L"/" + std::to_wstring(rows.size()) +
        L"，异常 " + std::to_wstring(anomalous) +
        L"，返回 " + std::to_wstring(audit.returnedCount) +
        L"/" + std::to_wstring(audit.totalCount);
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

// CollectProcessRefreshSnapshot performs every potentially blocking query for a
// process refresh. It never accesses HWNDs or ProcessViewState and is safe for
// AsyncSnapshotTask's worker thread.
ProcessRefreshSnapshot CollectProcessRefreshSnapshot() {
    ProcessRefreshSnapshot snapshot{};
    snapshot.enumeration = EnumerateProcessesByNtQuerySystemInformation();
    if (!snapshot.enumeration.success) {
        return snapshot;
    }
    snapshot.hiddenAudit = ApplyDefaultHiddenProcessAudit(snapshot.enumeration.rows);
    ApplyR0ProcessAuditRows(snapshot.enumeration.rows, snapshot.crossViewStatusSuffix);
    return snapshot;
}

// ApplyProcessRefresh installs a completed immutable snapshot on the UI thread.
// It performs lightweight diff bookkeeping and preserves the existing
// incremental ListView update path until the virtual-list migration lands.
void ApplyProcessRefresh(ProcessViewState& state, ProcessRefreshSnapshot snapshot) {
    if (!snapshot.enumeration.success) {
        state.model.setRows({});
        RebuildRows(state);
        SetStatus(state, L"进程枚举失败: " + snapshot.enumeration.diagnosticText);
        return;
    }

    std::vector<ProcessSnapshotRow> rows = std::move(snapshot.enumeration.rows);
    const HiddenProcessAuditResult& hiddenAudit = snapshot.hiddenAudit;
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
        L"s；" + hiddenAudit.detailText + snapshot.crossViewStatusSuffix +
        L"；左 Ctrl 按下时跳过自动刷新。");
}

// BeginProcessRefresh only schedules work and updates lightweight feedback. A
// running request is coalesced by AsyncSnapshotTask so fast timers and manual
// refreshes cannot start concurrent R0/R3 enumeration passes.
void BeginProcessRefresh(ProcessViewState& state) {
    if (!state.refreshTask) {
        return;
    }
    SetStatus(state, state.refreshTask->running() ? L"进程刷新已排队，等待当前快照完成…" : L"正在后台枚举进程与 R0 证据…");
    if (state.refreshButton) {
        ::EnableWindow(state.refreshButton, FALSE);
    }
    Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, true, L"正在刷新进程列表…");
    state.refreshTask->request(
        []() { return CollectProcessRefreshSnapshot(); },
        [&state](std::uint64_t, std::optional<ProcessRefreshSnapshot>&& snapshot, std::exception_ptr error) {
            if (state.refreshButton) {
                ::EnableWindow(state.refreshButton, TRUE);
            }
            Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, false);
            if (error || !snapshot.has_value()) {
                SetStatus(state, L"进程刷新任务异常结束。请检查驱动状态和访问权限。");
                return;
            }
            ApplyProcessRefresh(state, std::move(*snapshot));
        });
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
    bool found = false;
    for (int item = 0; item < ListView_GetItemCount(state.listView); ++item) {
        const int displayIndex = DisplayIndexFromListItem(state, item);
        if (displayIndex < 0 || static_cast<std::size_t>(displayIndex) >= state.presentationRows.size()) {
            continue;
        }
        if (state.presentationRows[static_cast<std::size_t>(displayIndex)].processId == pid) {
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
    if (actionId == ProcessActionId::CopyCell || actionId == ProcessActionId::CopyRow || actionId == ProcessActionId::CopyVisibleResults) {
        if (actionId == ProcessActionId::CopyVisibleResults) {
            const bool copied = WriteClipboardText(state.hwnd, VisibleRowsAsText(state));
            SetStatus(state, copied ? L"已复制全部可见进程结果。" : L"复制失败：当前没有可见结果或剪贴板不可用。");
            return;
        }
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

    if (actionId == ProcessActionId::R0InjectDll || actionId == ProcessActionId::R0InjectShellcode) {
        const std::vector<DWORD> pids = SelectedPids(state);
        if (pids.size() != 1) {
            SetStatus(state, L"R0 注入需要单选一个进程。");
            return;
        }

        const bool dllMode = actionId == ProcessActionId::R0InjectDll;
        const std::wstring payloadPath = PromptOpenPayloadFile(
            state.hwnd,
            dllMode ? L"DLL 文件 (*.dll)\0*.dll\0所有文件 (*.*)\0*.*\0" : L"Shellcode/二进制文件 (*.*)\0*.*\0",
            dllMode ? L"选择要注入的 DLL" : L"选择要注入的 Shellcode 二进制文件");
        if (payloadPath.empty()) {
            SetStatus(state, L"已取消 R0 注入。");
            return;
        }
        if (!ConfirmR0Injection(state.hwnd, dllMode ? L"DLL 注入" : L"Shellcode 注入", pids.front(), payloadPath)) {
            SetStatus(state, L"用户取消 R0 注入。");
            return;
        }

        const ProcessActionResult result = dllMode
            ? ExecuteR0ProcessDllInjection(pids, payloadPath)
            : ExecuteR0ProcessShellcodeInjection(pids, payloadPath);
        SetStatus(state, result.title + L": " + result.detail);
        if (!result.success) {
            ::MessageBoxW(state.hwnd, result.detail.c_str(), result.title.c_str(), MB_OK | MB_ICONINFORMATION);
        }
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
    appendAction(copyMenu, ProcessActionId::CopyVisibleResults, L"复制可见结果", !state.visibleRowIndexes.empty());
    appendPopup(menu, copyMenu, L"复制");

    HMENU processMenu = ::CreatePopupMenu();
    appendAction(processMenu, ProcessActionId::OpenDetails, L"进程详细信息", singleProcess);
    appendAction(processMenu, ProcessActionId::TerminateProcessMultiMethod, L"结束进程(组合方法链)", hasProcessSelection);
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

    HMENU integrityMenu = ::CreatePopupMenu();
    appendAction(integrityMenu, ProcessActionId::R0SetIntegrityUntrusted, L"Untrusted (S-1-16-0)", hasProcessSelection);
    appendAction(integrityMenu, ProcessActionId::R0SetIntegrityLow, L"Low (S-1-16-4096)", hasProcessSelection);
    appendAction(integrityMenu, ProcessActionId::R0SetIntegrityMedium, L"Medium (S-1-16-8192)", hasProcessSelection);
    appendAction(integrityMenu, ProcessActionId::R0SetIntegrityMediumPlus, L"Medium Plus (S-1-16-8448)", hasProcessSelection);
    appendAction(integrityMenu, ProcessActionId::R0SetIntegrityHigh, L"High (S-1-16-12288)", hasProcessSelection);
    appendAction(integrityMenu, ProcessActionId::R0SetIntegritySystem, L"System (S-1-16-16384)", hasProcessSelection);
    appendPopup(r0Menu, integrityMenu, L"设置完整性");

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

    HMENU injectMenu = ::CreatePopupMenu();
    appendAction(injectMenu, ProcessActionId::R0InjectDll, L"DLL 注入...", singleProcess);
    appendAction(injectMenu, ProcessActionId::R0InjectShellcode, L"Shellcode 注入...", singleProcess);
    appendPopup(r0Menu, injectMenu, L"注入(需确认)");
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
    state.filterBar = Ksword::Ui::CreateFilterBar(state.hwnd, kFilterBarId, L"筛选进程、PID、路径和 R0 证据", 0, 0, 0, 0);
    state.listView = CreateProcessListView(state.hwnd, kProcessListId);
    state.imageList = CreateIconList();
    if (state.listView && state.imageList) {
        ListView_SetImageList(state.listView, state.imageList, LVSIL_SMALL);
    }
    state.loadingOverlay = Ksword::Ui::CreateLoadingOverlay(state.hwnd, 52009, { 0, 0, 1, 1 });
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

    if (header->code == LVN_GETDISPINFOW) {
        return { true, HandleVirtualListDisplayInfo(state, reinterpret_cast<NMLVDISPINFOW*>(header)) };
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
        const ProcessPresentationRow* group = GroupHeaderAtListItem(state, activate->iItem);
        if (group) {
            const ProcessFriendlyGroup groupId = group->display.group;
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
            state->refreshTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ProcessRefreshSnapshot>>(hwnd, kMsgRefreshCompleted);
            state->filterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ProcessFilterResult>>(hwnd, kMsgFilterCompleted);
            SetStatus(*state, L"进程页已创建，等待首次异步刷新。");
            ::PostMessageW(hwnd, kMsgInitialRefresh, 0, 0);
        }
        return 0;
    case kMsgInitialRefresh:
    case kMsgRequestRefresh:
        if (state) {
            BeginProcessRefresh(*state);
            RestartRefreshTimer(*state);
        }
        return 0;
    case kMsgRefreshCompleted:
        if (state && state->refreshTask && state->refreshTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case kMsgFilterCompleted:
        if (state && state->filterTask && state->filterTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case WM_SIZE:
        if (state) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            LayoutChildren(*state, rc);
        }
        return 0;
    case WM_COMMAND:
        if (state && LOWORD(wParam) == kFilterBarId && HIWORD(wParam) == EN_CHANGE) {
            RequestProcessFilter(*state,
                Ksword::Ui::GetFilterBarText(state->filterBar),
                SelectedPids(*state),
                StableKeyFromListItem(*state, ListView_GetTopIndex(state->listView)));
            return 0;
        }
        if (state && HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case kRefreshButtonId:
                BeginProcessRefresh(*state);
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
            BeginProcessRefresh(*state);
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
            if (state->refreshTask) {
                state->refreshTask->cancel();
            }
            if (state->filterTask) {
                state->filterTask->cancel();
            }
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

void RequestProcessViewRefresh(HWND view) {
    if (view) {
        // view 用途：已创建的进程页窗口；PostMessage 避免驱动状态回调同步阻塞主窗口。
        ::PostMessageW(view, kMsgRequestRefresh, 0, 0);
    }
}

} // namespace Ksword::Features::Process
