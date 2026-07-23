#include "DriverMemoryView.h"

#include "DriverMemoryClient.h"
#include "DriverMemoryModel.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <commctrl.h>
#include <windowsx.h>

namespace Ksword::Features::Memory {
namespace {

constexpr wchar_t kDriverMemoryViewClass[] = L"KswordARKLight.DriverMemoryView";
constexpr int kPidEditId = 51001;
constexpr int kAddressEditId = 51002;
constexpr int kLengthEditId = 51003;
constexpr int kReadButtonId = 51004;
constexpr int kWriteButtonId = 51005;
constexpr int kHexEditId = 51006;
constexpr int kStatusEditId = 51007;
constexpr int kHistoryFilterId = 51008;
constexpr int kHistoryListId = 51009;
constexpr UINT kMemoryMenuRead = 51501;
constexpr UINT kMemoryMenuWrite = 51502;
constexpr UINT kMemoryMenuCopyHex = 51503;
constexpr UINT kMemoryMenuPasteHex = 51504;
constexpr UINT kMemoryMenuClearHex = 51505;
constexpr UINT kMemoryMenuNormalizeHex = 51506;
constexpr UINT kMemoryMenuSelectAll = 51507;
constexpr UINT kMemoryMenuCopyStatus = 51508;
constexpr UINT kMemoryMenuCopyHistoryCell = 51509;
constexpr UINT kMemoryMenuCopyHistoryRow = 51510;
constexpr UINT kMemoryMenuCopyHistoryVisible = 51511;
constexpr UINT kMsgMemoryOperationCompleted = WM_APP + 598;
constexpr UINT kMsgMemoryHistoryFilterCompleted = WM_APP + 599;

struct MemoryOperationSnapshot {
    bool readOperation = false;
    DWORD processId = 0;
    std::uint64_t address = 0;
    std::size_t requestedBytes = 0;
    DriverMemoryReadResult readResult;
    DriverMemoryWriteResult writeResult;
};

struct MemoryHistoryEntry {
    std::uint64_t sequence = 0;
    bool readOperation = false;
    DWORD processId = 0;
    std::uint64_t address = 0;
    std::size_t requestedBytes = 0;
    std::size_t completedBytes = 0;
    bool success = false;
    std::wstring status;
};

struct MemoryHistoryFilterResult {
    std::uint64_t generation = 0;
    std::wstring query;
    std::vector<std::size_t> visibleIndexes;
};

// DriverMemoryViewState owns child HWNDs and the driver facade for one page.
// Inputs arrive through window messages; processing validates edit-control text
// and calls DriverMemoryClient; return values are produced by WndProc message
// handling rather than by this state object.
struct DriverMemoryViewState {
    HWND hwnd = nullptr;
    HWND pidEdit = nullptr;
    HWND addressEdit = nullptr;
    HWND lengthEdit = nullptr;
    HWND hexEdit = nullptr;
    HWND statusEdit = nullptr;
    HWND readButton = nullptr;
    HWND writeButton = nullptr;
    HWND historyFilter = nullptr;
    bool operationInProgress = false;
    std::vector<MemoryHistoryEntry> history;
    std::uint64_t nextHistorySequence = 1;
    std::uint64_t historyGeneration = 0;
    std::wstring historyFilterQuery;
    int historyContextColumn = 0;
    Ksword::Ui::VirtualListView historyList;
    std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>> historyFilterRows;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<MemoryOperationSnapshot>> operationTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<MemoryHistoryFilterResult>> historyFilterTask;
};

// GetWindowTextString copies text from a Win32 edit control. Input is the child
// HWND; processing queries length and copies the text; output is an empty string
// for null handles or controls without text.
std::wstring GetWindowTextString(HWND hwnd) {
    if (!hwnd) {
        return std::wstring();
    }
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    ::GetWindowTextW(hwnd, &text[0], length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

// SetStatus writes a human-readable status message. Inputs are page state and
// text; processing updates the multiline status edit; no value is returned.
void SetStatus(DriverMemoryViewState& state, const std::wstring& text) {
    if (state.statusEdit) {
        ::SetWindowTextW(state.statusEdit, text.c_str());
    }
}

std::wstring FormatHex64(const std::uint64_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::vector<Ksword::Ui::ListViewColumn> MemoryHistoryColumns() {
    return {
        { 0, 68, LVCFMT_RIGHT, L"序号" },
        { 1, 74, LVCFMT_LEFT, L"操作" },
        { 2, 74, LVCFMT_RIGHT, L"PID" },
        { 3, 150, LVCFMT_LEFT, L"地址" },
        { 4, 88, LVCFMT_RIGHT, L"请求字节" },
        { 5, 88, LVCFMT_RIGHT, L"完成字节" },
        { 6, 70, LVCFMT_LEFT, L"结果" },
        { 7, 520, LVCFMT_LEFT, L"状态" },
    };
}

void ApplyMemoryHistoryFilter(DriverMemoryViewState& state, MemoryHistoryFilterResult result) {
    if (result.generation != state.historyGeneration || result.query != state.historyFilterQuery) {
        return;
    }
    state.historyList.setVisibleIndexes(std::move(result.visibleIndexes));
}

void RequestMemoryHistoryFilter(DriverMemoryViewState& state, std::wstring query) {
    state.historyFilterQuery = std::move(query);
    const auto rows = state.historyFilterRows;
    const std::uint64_t generation = state.historyGeneration;
    if (!state.historyFilterTask || !rows) {
        return;
    }
    state.historyFilterTask->request(
        [rows, generation, query = state.historyFilterQuery]() mutable {
            MemoryHistoryFilterResult result{};
            result.generation = generation;
            result.query = std::move(query);
            result.visibleIndexes = Ksword::Ui::VirtualListView::FilterRowIndexes(*rows, result.query);
            return result;
        },
        [&state](std::uint64_t, std::optional<MemoryHistoryFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value()) {
                SetStatus(state, L"内存操作历史筛选异常结束，已保留当前可见结果。");
                return;
            }
            ApplyMemoryHistoryFilter(state, std::move(*result));
        });
}

void RebuildMemoryHistory(DriverMemoryViewState& state) {
    auto rows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>();
    rows->reserve(state.history.size());
    for (const MemoryHistoryEntry& entry : state.history) {
        Ksword::Ui::VirtualListRow row{};
        row.stableKey = std::to_wstring(entry.sequence);
        row.cells = {
            std::to_wstring(entry.sequence),
            entry.readOperation ? L"读取" : L"写入",
            std::to_wstring(entry.processId),
            FormatHex64(entry.address),
            std::to_wstring(entry.requestedBytes),
            std::to_wstring(entry.completedBytes),
            entry.success ? L"成功" : L"失败",
            entry.status,
        };
        // The state text is both visible and part of the immutable filtering
        // snapshot, so local filtering never performs another driver query.
        row.stableKey += L"|" + entry.status;
        rows->push_back(std::move(row));
    }
    state.historyList.setRows(*rows);
    state.historyFilterRows = std::move(rows);
    ++state.historyGeneration;
    RequestMemoryHistoryFilter(state, state.historyFilter ? Ksword::Ui::GetFilterBarText(state.historyFilter) : state.historyFilterQuery);
}

void AppendMemoryHistory(DriverMemoryViewState& state, const MemoryOperationSnapshot& snapshot) {
    MemoryHistoryEntry entry{};
    entry.sequence = state.nextHistorySequence++;
    entry.readOperation = snapshot.readOperation;
    entry.processId = snapshot.processId;
    entry.address = snapshot.address;
    entry.requestedBytes = snapshot.requestedBytes;
    entry.success = snapshot.readOperation ? snapshot.readResult.success : snapshot.writeResult.success;
    entry.completedBytes = snapshot.readOperation ? snapshot.readResult.bytes.size() : snapshot.writeResult.bytesWritten;
    entry.status = snapshot.readOperation ? snapshot.readResult.statusText : snapshot.writeResult.statusText;
    if (entry.status.empty()) {
        entry.status = entry.success ? L"操作完成。" : L"操作失败。";
    }
    constexpr std::size_t kMaxHistoryEntries = 512;
    state.history.push_back(std::move(entry));
    if (state.history.size() > kMaxHistoryEntries) {
        state.history.erase(state.history.begin(), state.history.begin() + static_cast<std::ptrdiff_t>(state.history.size() - kMaxHistoryEntries));
    }
    RebuildMemoryHistory(state);
}

// CopyTextToClipboard writes Unicode text from the memory page to the clipboard.
// Inputs are owner HWND and text; processing allocates CF_UNICODETEXT and hands
// it to Windows; output reports whether clipboard ownership succeeded.
bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!::OpenClipboard(owner)) {
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

// TextFromClipboard reads CF_UNICODETEXT for paste into the hex buffer. Input is
// an owner HWND; processing copies the global clipboard text before unlocking;
// output is empty when the clipboard does not contain Unicode text.
std::wstring TextFromClipboard(HWND owner) {
    if (!::OpenClipboard(owner)) {
        return {};
    }
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        ::CloseClipboard();
        return {};
    }
    const wchar_t* text = static_cast<const wchar_t*>(::GlobalLock(data));
    std::wstring output = text ? std::wstring(text) : std::wstring();
    if (text) {
        ::GlobalUnlock(data);
    }
    ::CloseClipboard();
    return output;
}

// ReplaceEditSelection inserts text into an edit control. Inputs are edit HWND
// and text; processing uses EM_REPLACESEL so paste respects the current
// selection/caret; no value is returned.
void ReplaceEditSelection(HWND edit, const std::wstring& text) {
    if (edit) {
        ::SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

// SelectAllEditText selects all text in a target edit control. Input is edit
// HWND; processing sends EM_SETSEL; no value is returned.
void SelectAllEditText(HWND edit) {
    if (edit) {
        ::SendMessageW(edit, EM_SETSEL, 0, -1);
        ::SetFocus(edit);
    }
}

// CreateEdit creates a single-line or multiline edit control with the project
// UI font. Inputs are parent/id/geometry/style flags; processing calls
// CreateWindowExW; output is the child HWND.
HWND CreateEdit(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, DWORD extraStyle) {
    HWND hwnd = ::CreateWindowExW(WS_EX_CLIENTEDGE,
        L"EDIT",
        text ? text : L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    return hwnd;
}

// LayoutChildren positions all controls inside the page. Input is the page
// state and current client rectangle; processing computes a simple two-panel
// layout; no value is returned.
void LayoutChildren(DriverMemoryViewState& state, const RECT& rc) {
    const int margin = 12;
    const int labelWidth = 58;
    const int editHeight = 24;
    const int buttonWidth = 88;
    const int gap = 8;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int rowTop = margin + 26;
    const int inputWidth = 130;

    ::MoveWindow(state.pidEdit, margin + labelWidth, rowTop, inputWidth, editHeight, TRUE);
    ::MoveWindow(state.addressEdit, margin + labelWidth + inputWidth + 72, rowTop, inputWidth + 70, editHeight, TRUE);
    ::MoveWindow(state.lengthEdit, margin + labelWidth + inputWidth + 72 + inputWidth + 70 + 72, rowTop, inputWidth, editHeight, TRUE);

    const int buttonTop = rowTop + editHeight + gap;
    ::MoveWindow(state.readButton, margin, buttonTop, buttonWidth, editHeight + 2, TRUE);
    ::MoveWindow(state.writeButton, margin + buttonWidth + gap, buttonTop, buttonWidth, editHeight + 2, TRUE);

    const int filterTop = buttonTop + editHeight + gap;
    const int hexTop = filterTop + editHeight + gap;
    const int statusHeight = 42;
    const int historyHeight = 112;
    const int hexHeight = std::max(64, height - hexTop - statusHeight - historyHeight - (margin * 2) - (gap * 3));
    const int contentWidth = std::max(100, width - margin * 2);
    ::MoveWindow(state.historyFilter, margin, filterTop, contentWidth, editHeight, TRUE);
    ::MoveWindow(state.hexEdit, margin, hexTop, contentWidth, hexHeight, TRUE);
    ::MoveWindow(state.statusEdit, margin, hexTop + hexHeight + gap, contentWidth, statusHeight, TRUE);
    const int historyTop = hexTop + hexHeight + gap + statusHeight + gap;
    ::MoveWindow(state.historyList.hwnd(), margin, historyTop, contentWidth, historyHeight, TRUE);
}

// PaintLabels draws static labels directly on the page to keep the child window
// count small. Input is page HWND and paint DC; processing draws title and field
// labels using the shared theme; no value is returned.
void PaintLabels(HWND hwnd, HDC dc) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());

    const COLORREF text = Ksword::Ui::AppTheme().textColor;
    const COLORREF muted = Ksword::Ui::AppTheme().mutedTextColor;
    RECT title{ 12, 8, rc.right - 12, 28 };
    Ksword::Ui::DrawTextLine(dc, L"Driver Memory Read / Write", title, text, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT pid{ 12, 38, 70, 62 };
    RECT address{ 200, 38, 272, 62 };
    RECT length{ 472, 38, 544, 62 };
    RECT filter{ 12, 96, rc.right - 12, 118 };
    Ksword::Ui::DrawTextLine(dc, L"PID", pid, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"Address", address, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"Length", length, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"操作历史筛选（匹配全部列和状态）", filter, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// StateFromWindow returns the state pointer stored on the page HWND. Input is a
// page HWND; processing reads GWLP_USERDATA; output is null before WM_NCCREATE
// finishes or after WM_NCDESTROY clears the pointer.
DriverMemoryViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<DriverMemoryViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// HandleRead validates read fields and invokes the driver facade. Input is page
// state; processing parses PID/address/length and calls DriverMemoryClient;
// output is reflected in the hex and status edit controls.
void HandleRead(DriverMemoryViewState& state) {
    DriverMemoryReadRequest request;
    std::wstring error;
    if (!ParseReadRequest(GetWindowTextString(state.pidEdit),
            GetWindowTextString(state.addressEdit),
            GetWindowTextString(state.lengthEdit),
            request,
            error)) {
        SetStatus(state, error);
        return;
    }

    if (state.operationInProgress || !state.operationTask) {
        SetStatus(state, L"内存操作正在执行。");
        return;
    }
    state.operationInProgress = true;
    ::EnableWindow(state.readButton, FALSE);
    ::EnableWindow(state.writeButton, FALSE);
    SetStatus(state, L"正在后台执行 R0 内存读取…");
    state.operationTask->request(
        [request] {
            MemoryOperationSnapshot snapshot{};
            snapshot.readOperation = true;
            snapshot.processId = request.processId;
            snapshot.address = request.address;
            snapshot.requestedBytes = request.length;
            DriverMemoryClient client;
            snapshot.readResult = client.ReadMemory(request);
            return snapshot;
        },
        [&state](std::uint64_t, std::optional<MemoryOperationSnapshot>&& snapshot, std::exception_ptr error) {
            state.operationInProgress = false;
            ::EnableWindow(state.readButton, TRUE);
            ::EnableWindow(state.writeButton, TRUE);
            if (error || !snapshot.has_value()) {
                SetStatus(state, L"R0 内存读取异常结束。");
                return;
            }
            if (snapshot->readResult.success) {
                ::SetWindowTextW(state.hexEdit, FormatHexBytesForDisplay(snapshot->readResult.bytes).c_str());
            }
            SetStatus(state, snapshot->readResult.statusText);
            AppendMemoryHistory(state, *snapshot);
        });
}

// HandleWrite validates write fields and invokes the driver facade. Input is
// page state; processing parses PID/address/hex bytes and calls
// DriverMemoryClient; output is reflected in the status edit control.
void HandleWrite(DriverMemoryViewState& state) {
    DriverMemoryWriteRequest request;
    std::wstring error;
    if (!ParseWriteRequest(GetWindowTextString(state.pidEdit),
            GetWindowTextString(state.addressEdit),
            GetWindowTextString(state.hexEdit),
            request,
            error)) {
        SetStatus(state, error);
        return;
    }

    if (state.operationInProgress || !state.operationTask) {
        SetStatus(state, L"内存操作正在执行。");
        return;
    }
    state.operationInProgress = true;
    ::EnableWindow(state.readButton, FALSE);
    ::EnableWindow(state.writeButton, FALSE);
    SetStatus(state, L"正在后台执行 R0 内存写入…");
    state.operationTask->request(
        [request = std::move(request)] {
            MemoryOperationSnapshot snapshot{};
            snapshot.processId = request.processId;
            snapshot.address = request.address;
            snapshot.requestedBytes = request.bytes.size();
            DriverMemoryClient client;
            snapshot.writeResult = client.WriteMemory(request);
            return snapshot;
        },
        [&state](std::uint64_t, std::optional<MemoryOperationSnapshot>&& snapshot, std::exception_ptr error) {
            state.operationInProgress = false;
            ::EnableWindow(state.readButton, TRUE);
            ::EnableWindow(state.writeButton, TRUE);
            if (error || !snapshot.has_value()) {
                SetStatus(state, L"R0 内存写入异常结束。");
                return;
            }
            SetStatus(state, snapshot->writeResult.statusText);
            AppendMemoryHistory(state, *snapshot);
        });
}

// NormalizeHexBuffer parses and rewrites the hex edit as canonical two-digit
// byte text. Input is page state; processing never performs driver I/O; output
// is reflected in the edit control and status line.
void NormalizeHexBuffer(DriverMemoryViewState& state) {
    std::vector<std::uint8_t> bytes;
    std::wstring error;
    if (!ParseHexBytes(GetWindowTextString(state.hexEdit), bytes, error)) {
        SetStatus(state, error);
        return;
    }
    ::SetWindowTextW(state.hexEdit, FormatHexBytesForDisplay(bytes).c_str());
    SetStatus(state, L"Hex buffer normalized.");
}

// ShowMemoryContextMenu displays compact driver-memory actions. Inputs are page
// state, target child window and screen point; processing groups read/write,
// Hex-buffer, and status commands into submenus before dispatching the selected
// command; no value is returned.
void ShowMemoryContextMenu(DriverMemoryViewState& state, HWND target, POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    const bool textTarget = target == state.hexEdit || target == state.statusEdit;
    const bool hexTarget = target == state.hexEdit || target == state.hwnd;
    HMENU driverMenu = ::CreatePopupMenu();
    if (driverMenu) {
        ::AppendMenuW(driverMenu, MF_STRING, kMemoryMenuRead, L"读取");
        ::AppendMenuW(driverMenu, MF_STRING, kMemoryMenuWrite, L"写入");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(driverMenu), L"驱动内存");
    }
    HMENU hexMenu = ::CreatePopupMenu();
    if (hexMenu) {
        ::AppendMenuW(hexMenu, MF_STRING | (textTarget ? 0U : MF_GRAYED), kMemoryMenuSelectAll, L"全选");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuCopyHex, L"复制 Hex");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuPasteHex, L"粘贴 Hex");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuClearHex, L"清空 Hex");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuNormalizeHex, L"格式化 Hex");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hexMenu), L"Hex");
    }
    HMENU statusMenu = ::CreatePopupMenu();
    if (statusMenu) {
        ::AppendMenuW(statusMenu, MF_STRING, kMemoryMenuCopyStatus, L"复制状态");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(statusMenu), L"状态");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        state.hwnd,
        nullptr);
    ::DestroyMenu(menu);

    switch (command) {
    case kMemoryMenuRead:
        HandleRead(state);
        break;
    case kMemoryMenuWrite:
        HandleWrite(state);
        break;
    case kMemoryMenuSelectAll:
        SelectAllEditText(textTarget ? target : state.hexEdit);
        break;
    case kMemoryMenuCopyHex:
        SetStatus(state, CopyTextToClipboard(state.hwnd, GetWindowTextString(state.hexEdit)) ? L"Hex copied." : L"Copy Hex failed.");
        break;
    case kMemoryMenuPasteHex:
        ReplaceEditSelection(state.hexEdit, TextFromClipboard(state.hwnd));
        SetStatus(state, L"Hex paste requested.");
        break;
    case kMemoryMenuClearHex:
        ::SetWindowTextW(state.hexEdit, L"");
        SetStatus(state, L"Hex buffer cleared.");
        break;
    case kMemoryMenuNormalizeHex:
        NormalizeHexBuffer(state);
        break;
    case kMemoryMenuCopyStatus:
        SetStatus(state, CopyTextToClipboard(state.hwnd, GetWindowTextString(state.statusEdit)) ? L"Status copied." : L"Copy status failed.");
        break;
    default:
        break;
    }
}

std::wstring SelectedHistoryCellText(const DriverMemoryViewState& state) {
    const HWND list = state.historyList.hwnd();
    const int selected = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    const auto& visible = state.historyList.visibleIndexes();
    const auto& rows = state.historyList.rows();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visible.size()) {
        return {};
    }
    const std::size_t source = visible[static_cast<std::size_t>(selected)];
    if (source >= rows.size() || state.historyContextColumn < 0 || static_cast<std::size_t>(state.historyContextColumn) >= rows[source].cells.size()) {
        return {};
    }
    return rows[source].cells[static_cast<std::size_t>(state.historyContextColumn)];
}

std::wstring SelectedHistoryRowsText(const DriverMemoryViewState& state, const bool allVisible) {
    const HWND list = state.historyList.hwnd();
    const auto& visible = state.historyList.visibleIndexes();
    const auto& rows = state.historyList.rows();
    std::wstring text;
    for (std::size_t item = 0; item < visible.size(); ++item) {
        if (!allVisible && (!list || (ListView_GetItemState(list, static_cast<int>(item), LVIS_SELECTED) & LVIS_SELECTED) == 0)) {
            continue;
        }
        const std::size_t source = visible[item];
        if (source >= rows.size()) {
            continue;
        }
        const auto& cells = rows[source].cells;
        for (std::size_t column = 0; column < cells.size(); ++column) {
            if (column != 0) {
                text.push_back(L'\t');
            }
            text += cells[column];
        }
        text += L"\r\n";
    }
    return text;
}

void ShowMemoryHistoryContextMenu(DriverMemoryViewState& state, POINT screenPoint) {
    const HWND list = state.historyList.hwnd();
    if (!list) {
        return;
    }
    POINT clientPoint = screenPoint;
    ::ScreenToClient(list, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int item = ListView_SubItemHitTest(list, &hit);
    if (item >= 0) {
        state.historyContextColumn = hit.iSubItem;
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    const bool hasSelection = ListView_GetNextItem(list, -1, LVNI_SELECTED) >= 0;
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMemoryMenuCopyHistoryCell, L"复制单元格");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMemoryMenuCopyHistoryRow, L"复制行");
    ::AppendMenuW(menu, MF_STRING | (!state.historyList.visibleIndexes().empty() ? 0U : MF_GRAYED), kMemoryMenuCopyHistoryVisible, L"复制可见结果");
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == kMemoryMenuCopyHistoryCell) {
        SetStatus(state, CopyTextToClipboard(state.hwnd, SelectedHistoryCellText(state)) ? L"已复制单元格。" : L"复制单元格失败。");
    } else if (command == kMemoryMenuCopyHistoryRow) {
        SetStatus(state, CopyTextToClipboard(state.hwnd, SelectedHistoryRowsText(state, false)) ? L"已复制行。" : L"复制行失败。");
    } else if (command == kMemoryMenuCopyHistoryVisible) {
        SetStatus(state, CopyTextToClipboard(state.hwnd, SelectedHistoryRowsText(state, true)) ? L"已复制可见结果。" : L"复制可见结果失败。");
    }
}

// CreateChildControls creates every input/output control for the page. Input is
// page state with hwnd set; processing creates edit controls and buttons; no
// value is returned because missing children are handled by normal HWND checks.
void CreateChildControls(DriverMemoryViewState& state) {
    state.pidEdit = CreateEdit(state.hwnd, kPidEditId, L"", 0, 0, 0, 0, 0);
    state.addressEdit = CreateEdit(state.hwnd, kAddressEditId, L"0x0", 0, 0, 0, 0, 0);
    state.lengthEdit = CreateEdit(state.hwnd, kLengthEditId, L"16", 0, 0, 0, 0, 0);
    state.readButton = Ksword::Ui::CreateButton(state.hwnd, kReadButtonId, L"Read", 0, 0, 0, 0);
    state.writeButton = Ksword::Ui::CreateButton(state.hwnd, kWriteButtonId, L"Write", 0, 0, 0, 0);
    state.hexEdit = CreateEdit(state.hwnd,
        kHexEditId,
        L"",
        0,
        0,
        0,
        0,
        ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
    state.historyFilter = Ksword::Ui::CreateFilterBar(state.hwnd, kHistoryFilterId, L"筛选操作、PID、地址、状态", 0, 0, 0, 0);
    state.historyList.create(state.hwnd, kHistoryListId, 0, 0, 0, 0, LVS_SHOWSELALWAYS | LVS_SINGLESEL);
    state.historyList.addColumns(MemoryHistoryColumns());
    if (state.historyList.hwnd()) {
        ListView_SetExtendedListViewStyle(state.historyList.hwnd(), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
        ::SendMessageW(state.historyList.hwnd(), WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    state.statusEdit = CreateEdit(state.hwnd,
        kStatusEditId,
        L"Driver-only memory read/write surface. Requests are sent through ArkDriverClient and the shared memory IOCTL protocol.",
        0,
        0,
        0,
        0,
        ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY);
}

// DriverMemoryViewWndProc dispatches page window messages. Inputs are standard
// Win32 message parameters; processing owns state lifetime, child layout and
// button clicks; output is an LRESULT for DefWindowProcW-compatible handling.
LRESULT CALLBACK DriverMemoryViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto state = std::make_unique<DriverMemoryViewState>();
        state->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.release()));
    }

    DriverMemoryViewState* state = StateFromWindow(hwnd);
    switch (msg) {
    case WM_CREATE:
        if (state) {
            CreateChildControls(*state);
            state->operationTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<MemoryOperationSnapshot>>(hwnd, kMsgMemoryOperationCompleted);
            state->historyFilterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<MemoryHistoryFilterResult>>(hwnd, kMsgMemoryHistoryFilterCompleted);
            RebuildMemoryHistory(*state);
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
        if (state && LOWORD(wParam) == kHistoryFilterId && HIWORD(wParam) == EN_CHANGE) {
            RequestMemoryHistoryFilter(*state, Ksword::Ui::GetFilterBarText(state->historyFilter));
            return 0;
        }
        if (state && HIWORD(wParam) == BN_CLICKED) {
            const int id = LOWORD(wParam);
            if (id == kReadButtonId) {
                HandleRead(*state);
                return 0;
            }
            if (id == kWriteButtonId) {
                HandleWrite(*state);
                return 0;
            }
        }
        break;
    case kMsgMemoryOperationCompleted:
        if (state && state->operationTask && state->operationTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case kMsgMemoryHistoryFilterCompleted:
        if (state && state->historyFilterTask && state->historyFilterTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case WM_NOTIFY: {
        const auto* notify = reinterpret_cast<const NMHDR*>(lParam);
        if (state && notify && notify->idFrom == kHistoryListId) {
            LRESULT result = 0;
            if (state->historyList.handleNotify(*notify, result)) {
                return result;
            }
        }
        break;
    }
    case WM_CONTEXTMENU:
        if (state) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                ::GetWindowRect(state->hexEdit ? state->hexEdit : hwnd, &rc);
                pt = { rc.left + 16, rc.top + 16 };
            }
            HWND target = reinterpret_cast<HWND>(wParam);
            if (target == state->historyList.hwnd()) {
                ShowMemoryHistoryContextMenu(*state, pt);
                return 0;
            }
            if (target != state->hexEdit && target != state->statusEdit) {
                target = state->hwnd;
            }
            ShowMemoryContextMenu(*state, target, pt);
            return 0;
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
        PaintLabels(hwnd, dc);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        if (state && state->operationTask) {
            state->operationTask->cancel();
        }
        if (state && state->historyFilterTask) {
            state->historyFilterTask->cancel();
        }
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// RegisterDriverMemoryViewClass installs the page WNDCLASS once. Input is none;
// processing calls RegisterClassW and accepts already-registered classes; output
// is true when CreateWindowExW can use the class.
bool RegisterDriverMemoryViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = DriverMemoryViewWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kDriverMemoryViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateDriverMemoryView(HWND parent, const RECT& bounds) {
    if (!RegisterDriverMemoryViewClass()) {
        return nullptr;
    }

    return ::CreateWindowExW(0,
        kDriverMemoryViewClass,
        L"Driver Memory",
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

} // namespace Ksword::Features::Memory
