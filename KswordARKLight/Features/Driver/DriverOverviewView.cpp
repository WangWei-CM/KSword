#include "DriverOverviewView.h"

#include "DriverActions.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace Ksword::Features::Driver {
namespace {

constexpr wchar_t kDriverOverviewClass[] = L"KswordARKLight.DriverOverviewView";
constexpr int kOverviewListId = 64001;
constexpr int kOverviewFilterId = 64002;
constexpr UINT kOverviewMenuDetail = 64101;
constexpr UINT kOverviewMenuCopyRow = 64103;
constexpr UINT kOverviewMenuCopyName = 64104;
constexpr UINT kOverviewMenuCopyPath = 64105;
constexpr wchar_t kOverviewDetailClass[] = L"KswordARKLight.DriverOverviewDetailDialog";
constexpr int kOverviewDetailEditId = 64121;
constexpr int kOverviewDetailCopyId = 64122;
constexpr int kOverviewDetailCloseId = 64123;

// DriverOverviewViewState owns the list control and the attached model pointer.
// Inputs arrive through Win32 messages; processing only reads the model and
// renders it into the child list; no ownership of the model is transferred.
struct DriverOverviewViewState {
    HWND hwnd = nullptr;
    HWND filterEdit = nullptr;
    HWND listView = nullptr;
    DriverModel* model = nullptr;
    std::vector<DriverOverviewRow> visibleRows;
};

// DetailDialogState owns one modeless detail window. Input is the generated
// detail text; processing creates an EDIT plus Copy/Close buttons; no driver
// state is modified by this helper.
struct DetailDialogState {
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    HWND copyButton = nullptr;
    HWND closeButton = nullptr;
    HWND owner = nullptr;
    std::wstring title;
    std::wstring text;
};

// StateFromWindow returns the view state previously stored on the HWND. Input
// is the child window; processing reads GWLP_USERDATA; output is null before
// WM_NCCREATE or after WM_NCDESTROY clears the pointer.
DriverOverviewViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<DriverOverviewViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// SetChildFont applies the system UI font to one child window. Input is a child
// HWND; processing sends WM_SETFONT; no return value is needed.
void SetChildFont(HWND hwnd) {
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
}

// GuessDriverObjectName builds the conventional \Driver\Name fallback from an
// overview row. Input is a kernel module row; processing strips common .sys/.exe
// suffixes and keeps existing object-manager names; output may be empty.
std::wstring GuessDriverObjectName(const DriverOverviewRow& row) {
    std::wstring name = row.driverName;
    if (name.empty()) {
        name = row.pathText;
        const std::size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash + 1 < name.size()) {
            name = name.substr(slash + 1);
        }
    }
    if (name.empty()) {
        return {};
    }
    if (name.rfind(L"\\Driver\\", 0) == 0) {
        return name;
    }
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        name.resize(dot);
    }
    return name.empty() ? std::wstring{} : (L"\\Driver\\" + name);
}

// SelectedOverviewRow copies the selected overview model row. Input is the view
// state and output pointer; processing maps the selected list index to the
// current model snapshot; output is true when a row is available.
bool SelectedOverviewRow(const DriverOverviewViewState& state, DriverOverviewRow* rowOut) {
    if (!state.listView || !state.model) {
        return false;
    }
    const int selected = ListView_GetNextItem(state.listView, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(state.visibleRows.size())) {
        return false;
    }
    if (rowOut) {
        *rowOut = state.visibleRows[static_cast<std::size_t>(selected)];
    }
    return true;
}

// GetEditText reads a single-line filter edit. Input is an edit HWND; output is
// the current UTF-16 text or an empty string when the control is unavailable.
std::wstring GetEditText(HWND edit) {
    if (!edit) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1U, L'\0');
    if (length > 0) {
        ::GetWindowTextW(edit, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(std::max(0, length)));
    return text;
}

// CopyOverviewText writes selected driver text to the clipboard. Inputs are view
// state and payload; processing delegates clipboard ownership to DriverActions;
// no value is returned because feedback is shown through a small message box.
void CopyOverviewText(const DriverOverviewViewState& state, const std::wstring& text, const wchar_t* title) {
    const bool ok = DriverActions::CopyTextToClipboard(state.hwnd, text);
    ::MessageBoxW(state.hwnd, ok ? L"已复制到剪贴板。" : L"复制失败。", title, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

// BuildOverviewRowText serializes one overview row. Input is the selected model
// row; output is a TSV-compatible row used by the right-click copy command.
std::wstring BuildOverviewRowText(const DriverOverviewRow& row) {
    return row.driverName + L"\t" +
        row.baseAddressText + L"\t" +
        row.memoryRangeText + L"\t" +
        row.sizeText + L"\t" +
        row.pathText + L"\t" +
        row.signatureText + L"\t" +
        row.statusText + L"\t" +
        row.anomalyText + L"\t" +
        row.capabilityHint;
}

// RegisterDetailDialogClass installs the modeless detail window class. There
// is no input; processing is idempotent; output is true when CreateWindowExW
// can use the class name.
bool RegisterDetailDialogClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        DetailDialogState* state = reinterpret_cast<DetailDialogState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<DetailDialogState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }

        switch (msg) {
        case WM_CREATE:
            if (state) {
                state->edit = ::CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    state->text.c_str(),
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
                    0,
                    0,
                    0,
                    0,
                    hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOverviewDetailEditId)),
                    ::GetModuleHandleW(nullptr),
                    nullptr);
                state->copyButton = Ksword::Ui::CreateButton(hwnd, kOverviewDetailCopyId, L"复制详情", 0, 0, 0, 0);
                state->closeButton = Ksword::Ui::CreateButton(hwnd, kOverviewDetailCloseId, L"关闭", 0, 0, 0, 0);
                Ksword::Ui::SetWindowFontRecursive(hwnd);
            }
            return 0;
        case WM_SIZE:
            if (state) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                const int width = rc.right - rc.left;
                const int height = rc.bottom - rc.top;
                const int margin = 8;
                const int buttonHeight = 26;
                ::MoveWindow(state->edit, margin, margin, std::max(80, width - margin * 2), std::max(80, height - buttonHeight - margin * 3), TRUE);
                ::MoveWindow(state->copyButton, margin, std::max(margin, height - buttonHeight - margin), 96, buttonHeight, TRUE);
                ::MoveWindow(state->closeButton, width - 88 - margin, std::max(margin, height - buttonHeight - margin), 88, buttonHeight, TRUE);
            }
            return 0;
        case WM_COMMAND:
            if (state && LOWORD(wParam) == kOverviewDetailCopyId) {
                DriverActions::CopyTextToClipboard(hwnd, state->text);
                return 0;
            }
            if (state && LOWORD(wParam) == kOverviewDetailCloseId) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
        }
        case WM_NCDESTROY:
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
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kOverviewDetailClass;
    registered = (::RegisterClassW(&wc) != 0) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

// ShowDetailWindow opens a scrollable, copyable detail popup. Inputs are owner,
// title and body text; processing creates a modeless top-level window; no value
// is returned.
void ShowDetailWindow(HWND owner, const std::wstring& title, const std::wstring& text) {
    if (!RegisterDetailDialogClass()) {
        ::MessageBoxW(owner, text.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }

    auto* state = new DetailDialogState();
    state->owner = owner;
    state->title = title;
    state->text = text;
    HWND hwnd = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kOverviewDetailClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        860,
        620,
        owner,
        nullptr,
        ::GetModuleHandleW(nullptr),
        state);
    if (!hwnd) {
        delete state;
        ::MessageBoxW(owner, text.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
    }
}

// BuildOverviewDetailText formats row-local details and, when possible, R0
// DriverObject details. Input is the selected row; output is a rich text body
// for the detail popup.
std::wstring BuildOverviewDetailText(const DriverOverviewRow& row) {
    std::wstring detail =
        L"DriverName: " + row.driverName + L"\r\n" +
        L"Base: " + row.baseAddressText + L"\r\n" +
        L"MemoryRange: " + row.memoryRangeText + L"\r\n" +
        L"Size: " + row.sizeText + L"\r\n" +
        L"Path: " + row.pathText + L"\r\n" +
        L"Signature: " + row.signatureText + L"\r\n" +
        L"Status: " + row.statusText + L"\r\n" +
        L"Anomaly: " + row.anomalyText + L"\r\n" +
        L"Hint: " + row.capabilityHint + L"\r\n";

    const std::wstring driverObjectName = GuessDriverObjectName(row);
    if (!driverObjectName.empty()) {
        const DriverActionResult r0Detail = DriverActions::BuildDriverObjectDetailText(driverObjectName);
        detail += L"\r\nR0 DriverObject query (" + driverObjectName + L"):\r\n";
        detail += r0Detail.statusText;
    }
    return detail;
}

// ShowOverviewDetail displays the generated detail body in a scrollable popup.
// Input is the view state; processing routes R0 access through DriverActions;
// no value is returned.
void ShowOverviewDetail(const DriverOverviewViewState& state) {
    DriverOverviewRow row;
    if (SelectedOverviewRow(state, &row)) {
        ShowDetailWindow(state.hwnd, L"驱动详细信息", BuildOverviewDetailText(row));
    }
}

// ShowOverviewContextMenu displays the overview right-click menu. Inputs are
// the view state and screen coordinate; processing selects the clicked row,
// groups read-only copy/detail actions, and returns no value.
void ShowOverviewContextMenu(DriverOverviewViewState& state, POINT screenPoint) {
    POINT clientPoint = screenPoint;
    ::ScreenToClient(state.listView, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int item = ListView_HitTest(state.listView, &hit);
    if (item >= 0) {
        ListView_SetItemState(state.listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(state.listView, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    const bool hasSelection = SelectedOverviewRow(state, nullptr);
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kOverviewMenuCopyRow, L"复制当前行");
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kOverviewMenuCopyName, L"复制驱动名称");
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kOverviewMenuCopyPath, L"复制驱动路径");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }
    HMENU detailMenu = ::CreatePopupMenu();
    if (detailMenu) {
        ::AppendMenuW(detailMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kOverviewMenuDetail, L"详细信息/R0 DriverObject");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(detailMenu), L"详细信息");
    }

    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == kOverviewMenuDetail) {
        ShowOverviewDetail(state);
    } else if (command == kOverviewMenuCopyRow ||
        command == kOverviewMenuCopyName ||
        command == kOverviewMenuCopyPath) {
        DriverOverviewRow row;
        if (!SelectedOverviewRow(state, &row)) {
            return;
        }
        if (command == kOverviewMenuCopyRow) {
            CopyOverviewText(state, BuildOverviewRowText(row), L"复制驱动行");
        } else if (command == kOverviewMenuCopyName) {
            CopyOverviewText(state, row.driverName, L"复制驱动名称");
        } else {
            CopyOverviewText(state, row.pathText, L"复制驱动路径");
        }
    }
}

// OverviewColumns returns the report columns shown by the overview page.
// There is no input; processing returns static descriptors; output is consumed
// by the ListView helper.
std::vector<Ksword::Ui::ListViewColumn> OverviewColumns() {
    return {
        { 0, 190, LVCFMT_LEFT, L"驱动名称" },
        { 1, 145, LVCFMT_LEFT, L"基址" },
        { 2, 260, LVCFMT_LEFT, L"内存区间" },
        { 3, 110, LVCFMT_RIGHT, L"大小" },
        { 4, 420, LVCFMT_LEFT, L"路径" },
        { 5, 160, LVCFMT_LEFT, L"签名/来源" },
        { 6, 180, LVCFMT_LEFT, L"状态" },
        { 7, 320, LVCFMT_LEFT, L"异常标记" },
        { 8, 320, LVCFMT_LEFT, L"能力提示" },
    };
}

// PopulateOverviewList rebuilds the list-view from the model snapshot. Input is
// the view state; processing copies each visible row into the report control;
// no value is returned.
void PopulateOverviewList(DriverOverviewViewState& state) {
    if (!state.listView) {
        return;
    }

    Ksword::Ui::ClearListViewRows(state.listView);
    state.visibleRows.clear();
    if (!state.model) {
        return;
    }

    state.visibleRows = state.model->filterOverviewRows(GetEditText(state.filterEdit));
    for (const DriverOverviewRow& row : state.visibleRows) {
        Ksword::Ui::InsertListViewTextRow(state.listView, {
            row.driverName,
            row.baseAddressText,
            row.memoryRangeText,
            row.sizeText,
            row.pathText,
            row.signatureText,
            row.statusText,
            row.anomalyText,
            row.capabilityHint
        });
    }
}

// BuildOverviewTsv converts the current overview rows into TSV text. Input is
// the view state; processing reads the model snapshot and serializes the same
// columns displayed in the report control; output is clipboard/file ready text.
std::wstring BuildOverviewTsv(const DriverOverviewViewState& state) {
    std::vector<std::vector<std::wstring>> rows;
    for (const DriverOverviewRow& row : state.visibleRows) {
        rows.push_back({
            row.driverName,
            row.baseAddressText,
            row.memoryRangeText,
            row.sizeText,
            row.pathText,
            row.signatureText,
            row.statusText,
            row.anomalyText,
            row.capabilityHint
        });
    }
    return DriverActions::BuildTsv({ L"驱动名称", L"基址", L"内存区间", L"大小", L"路径", L"签名/来源", L"状态", L"异常标记", L"能力提示" }, rows);
}

// RegisterDriverOverviewClass installs the child window class once. There is no
// input; processing is idempotent; output is true when CreateWindowExW can use
// the class name.
bool RegisterDriverOverviewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        DriverOverviewViewState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<DriverOverviewViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }

        switch (msg) {
        case WM_CREATE:
            if (state) {
                state->filterEdit = ::CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    0,
                    0,
                    0,
                    0,
                    hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOverviewFilterId)),
                    ::GetModuleHandleW(nullptr),
                    nullptr);
                state->listView = Ksword::Ui::CreateReportListView(hwnd, kOverviewListId, 0, 0, 0, 0);
                SetChildFont(state->filterEdit);
                SetChildFont(state->listView);
                Ksword::Ui::AddListViewColumns(state->listView, OverviewColumns());
                PopulateOverviewList(*state);
            }
            return 0;
        case WM_COMMAND:
            if (state && LOWORD(wParam) == kOverviewFilterId && HIWORD(wParam) == EN_CHANGE) {
                PopulateOverviewList(*state);
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->listView && header->code == NM_RCLICK) {
                    POINT pt{};
                    ::GetCursorPos(&pt);
                    ShowOverviewContextMenu(*state, pt);
                    return 0;
                }
            }
            break;
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->listView) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->listView, &rc);
                    pt = { rc.left + 24, rc.top + 24 };
                }
                ShowOverviewContextMenu(*state, pt);
                return 0;
            }
            break;
        case WM_SIZE:
            if (state && state->listView) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                const int width = rc.right - rc.left;
                const int height = rc.bottom - rc.top;
                const int margin = 6;
                const int filterHeight = 24;
                ::MoveWindow(state->filterEdit, margin, margin, std::max(80, width - margin * 2), filterHeight, TRUE);
                ::MoveWindow(state->listView, 0, filterHeight + margin * 2, width, std::max(40, height - filterHeight - margin * 2), TRUE);
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
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kDriverOverviewClass;
    if (::RegisterClassW(&wc)) {
        registered = true;
    } else if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateDriverOverviewView(HWND parent, const RECT& bounds, DriverModel* model) {
    if (!parent || !RegisterDriverOverviewClass()) {
        return nullptr;
    }

    auto* state = new DriverOverviewViewState();
    state->model = model;
    HWND hwnd = ::CreateWindowExW(
        0,
        kDriverOverviewClass,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

void ResizeDriverOverviewView(HWND view, const RECT& bounds) {
    if (view) {
        ::MoveWindow(view,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            TRUE);
    }
}

void RefreshDriverOverviewView(HWND view) {
    DriverOverviewViewState* state = StateFromWindow(view);
    if (state) {
        PopulateOverviewList(*state);
    }
}

std::wstring ExportDriverOverviewViewTsv(HWND view) {
    const DriverOverviewViewState* state = StateFromWindow(view);
    return state ? BuildOverviewTsv(*state) : std::wstring();
}

} // namespace Ksword::Features::Driver
