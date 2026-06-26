#include "DriverOverviewView.h"

#include "DriverActions.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"

#include <commctrl.h>
#include <windowsx.h>

#include <cstdint>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace Ksword::Features::Driver {
namespace {

constexpr wchar_t kDriverOverviewClass[] = L"KswordARKLight.DriverOverviewView";
constexpr int kOverviewListId = 64001;
constexpr UINT kOverviewMenuDetail = 64101;
constexpr UINT kOverviewMenuUnload = 64102;
constexpr UINT kOverviewMenuCopyRow = 64103;
constexpr UINT kOverviewMenuCopyName = 64104;
constexpr UINT kOverviewMenuCopyPath = 64105;

// DriverOverviewViewState owns the list control and the attached model pointer.
// Inputs arrive through Win32 messages; processing only reads the model and
// renders it into the child list; no ownership of the model is transferred.
struct DriverOverviewViewState {
    HWND hwnd = nullptr;
    HWND listView = nullptr;
    DriverModel* model = nullptr;
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

// ParseHexAddress converts the overview base-address text back into an integer
// for ArkDriverClient module-base operations. Input is a display string such as
// "0xFFFF..."; processing accepts optional whitespace and 0x prefix; output is
// zero when the text is not a valid hexadecimal address.
std::uint64_t ParseHexAddress(const std::wstring& text) {
    const wchar_t* begin = text.c_str();
    while (*begin == L' ' || *begin == L'\t') {
        ++begin;
    }
    if (begin[0] == L'0' && (begin[1] == L'x' || begin[1] == L'X')) {
        begin += 2;
    }
    wchar_t* end = nullptr;
    const unsigned long long value = ::wcstoull(begin, &end, 16);
    return end != begin ? static_cast<std::uint64_t>(value) : 0;
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
    const auto& rows = state.model->overviewRows();
    if (selected < 0 || selected >= static_cast<int>(rows.size())) {
        return false;
    }
    if (rowOut) {
        *rowOut = rows[static_cast<std::size_t>(selected)];
    }
    return true;
}

// ShowActionResult displays a DriverActions result and keeps the UI behavior
// explicit. Inputs are owner/title/result; processing uses MessageBoxW only; no
// value is returned.
void ShowActionResult(HWND owner, const wchar_t* title, const DriverActionResult& result) {
    ::MessageBoxW(
        owner,
        result.statusText.empty() ? L"<no details>" : result.statusText.c_str(),
        title,
        MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONWARNING));
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
        row.sizeText + L"\t" +
        row.pathText + L"\t" +
        row.statusText + L"\t" +
        row.capabilityHint;
}

// ShowOverviewDetail displays row-local details and, when possible, R0
// DriverObject details. Input is the view state; processing routes R0 access
// through DriverActions; no value is returned.
void ShowOverviewDetail(const DriverOverviewViewState& state) {
    DriverOverviewRow row;
    if (!SelectedOverviewRow(state, &row)) {
        return;
    }

    std::wstring detail =
        L"DriverName: " + row.driverName + L"\r\n" +
        L"Base: " + row.baseAddressText + L"\r\n" +
        L"Size: " + row.sizeText + L"\r\n" +
        L"Path: " + row.pathText + L"\r\n" +
        L"Status: " + row.statusText + L"\r\n" +
        L"Hint: " + row.capabilityHint + L"\r\n";

    const std::wstring driverObjectName = GuessDriverObjectName(row);
    if (!driverObjectName.empty()) {
        const DriverActionResult r0Detail = DriverActions::BuildDriverObjectDetailText(driverObjectName);
        detail += L"\r\nR0 DriverObject query (" + driverObjectName + L"):\r\n";
        detail += r0Detail.statusText;
    }

    ::MessageBoxW(state.hwnd, detail.c_str(), L"驱动详细信息", MB_OK | MB_ICONINFORMATION);
}

// ForceUnloadOverviewDriver confirms and sends a module-base unload request.
// Input is the view state; processing calls DriverActions/ArkDriverClient; no
// value is returned.
void ForceUnloadOverviewDriver(DriverOverviewViewState& state) {
    DriverOverviewRow row;
    if (!SelectedOverviewRow(state, &row)) {
        return;
    }

    const std::uint64_t moduleBase = ParseHexAddress(row.baseAddressText);
    const std::wstring fallback = GuessDriverObjectName(row);
    const std::wstring prompt =
        L"确认通过 R0 IOCTL 请求卸载此驱动？\r\n\r\n" +
        row.driverName + L"\r\n" +
        row.baseAddressText + L"\r\n" +
        row.pathText;
    if (::MessageBoxW(state.hwnd, prompt.c_str(), L"确认 R0 卸载驱动", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    const DriverActionResult result = DriverActions::ForceUnloadDriverByModuleBase(moduleBase, fallback, 0UL);
    ShowActionResult(state.hwnd, L"R0 卸载驱动结果", result);
}

// ShowOverviewContextMenu displays the overview right-click menu. Inputs are
// the view state and screen coordinate; processing selects the clicked row,
// groups copy/detail/R0 actions, and returns no value.
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
    HMENU r0Menu = ::CreatePopupMenu();
    if (r0Menu) {
        ::AppendMenuW(r0Menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kOverviewMenuUnload, L"R0 卸载驱动");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(r0Menu), L"R0 操作");
    }

    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == kOverviewMenuDetail) {
        ShowOverviewDetail(state);
    } else if (command == kOverviewMenuUnload) {
        ForceUnloadOverviewDriver(state);
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
        { 0, 210, LVCFMT_LEFT, L"驱动名称" },
        { 1, 150, LVCFMT_LEFT, L"基址" },
        { 2, 110, LVCFMT_RIGHT, L"大小" },
        { 3, 420, LVCFMT_LEFT, L"路径" },
        { 4, 180, LVCFMT_LEFT, L"状态" },
        { 5, 260, LVCFMT_LEFT, L"能力提示" },
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
    const std::vector<DriverOverviewRow>* rows = state.model ? &state.model->overviewRows() : nullptr;
    if (!rows) {
        return;
    }

    for (const DriverOverviewRow& row : *rows) {
        Ksword::Ui::InsertListViewTextRow(state.listView, {
            row.driverName,
            row.baseAddressText,
            row.sizeText,
            row.pathText,
            row.statusText,
            row.capabilityHint
        });
    }
}

// BuildOverviewTsv converts the current overview rows into TSV text. Input is
// the view state; processing reads the model snapshot and serializes the same
// columns displayed in the report control; output is clipboard/file ready text.
std::wstring BuildOverviewTsv(const DriverOverviewViewState& state) {
    std::vector<std::vector<std::wstring>> rows;
    if (state.model) {
        for (const DriverOverviewRow& row : state.model->overviewRows()) {
            rows.push_back({
                row.driverName,
                row.baseAddressText,
                row.sizeText,
                row.pathText,
                row.statusText,
                row.capabilityHint
            });
        }
    }
    return DriverActions::BuildTsv({ L"驱动名称", L"基址", L"大小", L"路径", L"状态", L"能力提示" }, rows);
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
                state->listView = Ksword::Ui::CreateReportListView(hwnd, kOverviewListId, 0, 0, 0, 0);
                SetChildFont(state->listView);
                Ksword::Ui::AddListViewColumns(state->listView, OverviewColumns());
                PopulateOverviewList(*state);
            }
            return 0;
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
                ::MoveWindow(state->listView, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
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
