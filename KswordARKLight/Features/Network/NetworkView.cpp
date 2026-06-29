#include "NetworkView.h"

#include "NetworkModel.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"

#include <algorithm>
#include <commctrl.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace Ksword::Features::Network {
namespace {

constexpr wchar_t kNetworkViewClass[] = L"KswordARKLight.NetworkFeatureView";
constexpr int kRefreshButtonId = 69001;
constexpr int kCopyButtonId = 69002;
constexpr int kStatusTextId = 69003;
constexpr int kSummaryTextId = 69004;
constexpr int kTabControlId = 69005;
constexpr int kListViewId = 69006;

// NetworkViewState owns the root page HWND and all child controls for this
// self-contained ARKLight Network module. Inputs arrive through Win32 messages;
// processing only renders local model descriptors and never calls
// DeviceIoControl or any mutating network API.
struct NetworkViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND copyButton = nullptr;
    HWND statusText = nullptr;
    HWND summaryText = nullptr;
    HWND tab = nullptr;
    HWND listView = nullptr;
    NetworkAuditModel model;
    int currentTab = 0;
};

// StateFromWindow returns the state pointer stored on the root HWND. Input is a
// window handle; processing reads GWLP_USERDATA; output is nullptr before
// WM_NCCREATE completes or after WM_NCDESTROY.
NetworkViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<NetworkViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// Width returns a clamped rectangle width. Input is any RECT; output is a
// non-negative pixel count used by MoveWindow calls.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a clamped rectangle height. Input is any RECT; output is a
// non-negative pixel count used by MoveWindow calls.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// SetStatus writes one footer/status line. Inputs are page state and text;
// processing updates the STATIC control only; no value is returned.
void SetStatus(NetworkViewState& state, const std::wstring& text) {
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, text.c_str());
    }
}

// SetSummary writes the selected tab summary. Inputs are page state and text;
// processing updates the STATIC control only; no value is returned.
void SetSummary(NetworkViewState& state, const std::wstring& text) {
    if (state.summaryText) {
        ::SetWindowTextW(state.summaryText, text.c_str());
    }
}

// CopyTextToClipboard writes a Unicode payload to the shell clipboard. Inputs
// are the owner window and text; processing allocates a movable global block and
// transfers ownership to SetClipboardData; output is true when accepted.
bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
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

// SanitizeTsvCell removes tab/newline separators from one display cell. Input
// is raw UI text; processing replaces TSV-breaking characters with spaces;
// output is safe for clipboard export.
std::wstring SanitizeTsvCell(std::wstring cell) {
    for (wchar_t& ch : cell) {
        if (ch == L'\t' || ch == L'\r' || ch == L'\n') {
            ch = L' ';
        }
    }
    return cell;
}

// AppendTsvRow appends one TSV row. Inputs are an output buffer and text cells;
// processing sanitizes cells and joins them with tabs; no value is returned.
void AppendTsvRow(std::wstring& output, const std::vector<std::wstring>& cells) {
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (index != 0) {
            output.push_back(L'\t');
        }
        output += SanitizeTsvCell(cells[index]);
    }
    output += L"\r\n";
}

// BuildCurrentPageTsv serializes the selected Network page. Input is page
// state; processing reads only the local model descriptor; output is ready for
// clipboard/export use.
std::wstring BuildCurrentPageTsv(const NetworkViewState& state) {
    const NetworkAuditPage* page = state.model.pageAt(state.currentTab);
    if (!page) {
        return {};
    }

    std::wstring output;
    std::vector<std::wstring> headers;
    for (const NetworkAuditColumn& column : page->columns) {
        headers.push_back(column.title);
    }
    AppendTsvRow(output, headers);
    for (const NetworkAuditRow& row : page->rows) {
        AppendTsvRow(output, row.cells);
    }
    return output;
}

// ApplyColumns rebuilds the report columns for one Network page. Inputs are the
// ListView and page descriptor; processing clears old headers and inserts the
// page-specific schema; no value is returned.
void ApplyColumns(HWND listView, const NetworkAuditPage& page) {
    if (!listView) {
        return;
    }

    Ksword::Ui::ClearListViewColumns(listView);
    std::vector<Ksword::Ui::ListViewColumn> columns;
    int index = 0;
    for (const NetworkAuditColumn& column : page.columns) {
        columns.push_back({ index, column.width, column.format, column.title });
        ++index;
    }
    Ksword::Ui::AddListViewColumns(listView, columns);
}

// PopulateRows rebuilds the report rows for one Network page. Inputs are the
// ListView and page descriptor; processing inserts display cells only; no value
// is returned.
void PopulateRows(HWND listView, const NetworkAuditPage& page) {
    if (!listView) {
        return;
    }

    Ksword::Ui::ClearListViewRows(listView);
    Ksword::Ui::ScopedListViewRedrawLock redrawLock(listView);
    for (const NetworkAuditRow& row : page.rows) {
        Ksword::Ui::InsertListViewTextRow(listView, row.cells);
    }
}

// RenderCurrentPage applies the selected tab descriptor to the summary and
// report ListView. Input is page state; processing is fully local/read-only; no
// value is returned.
void RenderCurrentPage(NetworkViewState& state) {
    const NetworkAuditPage* page = state.model.pageAt(state.currentTab);
    if (!page) {
        SetSummary(state, L"Network audit page unavailable.");
        SetStatus(state, L"Network 页索引无效。");
        return;
    }

    ApplyColumns(state.listView, *page);
    PopulateRows(state.listView, *page);
    SetSummary(state, page->summary);
    SetStatus(state, L"Network R0 审计已刷新：TCP/UDP/WFP/NDIS 通过 ArkDriverClient wrapper 查询。");
}

// LayoutChildren positions toolbar, summary, tab host and shared report list.
// Input is page state; processing uses the current client rectangle; no value
// is returned.
void LayoutChildren(NetworkViewState& state) {
    if (!state.hwnd) {
        return;
    }

    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    const int margin = 8;
    const int buttonWidth = 82;
    const int buttonGap = 6;
    const int toolbarHeight = 30;
    const int summaryHeight = 38;

    ::MoveWindow(state.refreshButton, margin, margin, buttonWidth, 24, TRUE);
    ::MoveWindow(state.copyButton, margin + buttonWidth + buttonGap, margin, buttonWidth, 24, TRUE);
    ::MoveWindow(
        state.statusText,
        margin + (buttonWidth + buttonGap) * 2 + 16,
        margin + 2,
        std::max(100, width - 240),
        20,
        TRUE);

    const int summaryTop = margin + toolbarHeight;
    ::MoveWindow(
        state.summaryText,
        margin,
        summaryTop,
        std::max(100, width - margin * 2),
        summaryHeight,
        TRUE);

    const int tabTop = summaryTop + summaryHeight + 4;
    ::MoveWindow(
        state.tab,
        margin,
        tabTop,
        std::max(100, width - margin * 2),
        std::max(100, height - tabTop - margin),
        TRUE);

    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    if (state.listView) {
        ::MoveWindow(
            state.listView,
            display.left,
            display.top,
            Width(display),
            Height(display),
            TRUE);
    }
}

// CreateChildControls builds the Network toolbar, tab control and shared table.
// Input is state with hwnd assigned; processing creates all controls once;
// output is true when required HWNDs exist.
bool CreateChildControls(NetworkViewState& state) {
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.copyButton = Ksword::Ui::CreateButton(state.hwnd, kCopyButtonId, L"复制 TSV", 0, 0, 0, 0);
    state.statusText = Ksword::Ui::CreateText(state.hwnd, kStatusTextId, L"Network 只读审计页面准备就绪。", 0, 0, 0, 0);
    state.summaryText = Ksword::Ui::CreateText(state.hwnd, kSummaryTextId, L"", 0, 0, 0, 0);
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kTabControlId, 0, 0, 0, 0);
    if (!state.refreshButton || !state.copyButton || !state.statusText || !state.summaryText || !state.tab) {
        return false;
    }

    int index = 0;
    for (const NetworkAuditPage& page : state.model.pages()) {
        Ksword::Ui::AddTabPage(state.tab, index, { page.title, static_cast<LPARAM>(index) });
        ++index;
    }
    ::SendMessageW(state.tab, TCM_SETCURSEL, 0, 0);
    state.currentTab = 0;

    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    state.listView = Ksword::Ui::CreateReportListView(
        state.tab,
        kListViewId,
        display.left,
        display.top,
        std::max(1, Width(display)),
        std::max(1, Height(display)));
    if (!state.listView) {
        return false;
    }

    Ksword::Ui::SetWindowFontRecursive(state.hwnd);
    RenderCurrentPage(state);
    return true;
}

// OnCopyCurrentPage copies the selected table to the clipboard. Input is page
// state; processing serializes the local descriptor rows; no value is returned.
void OnCopyCurrentPage(NetworkViewState& state) {
    const std::wstring tsv = BuildCurrentPageTsv(state);
    if (CopyTextToClipboard(state.hwnd, tsv)) {
        SetStatus(state, L"已复制当前 Network 审计页 TSV。");
    } else {
        SetStatus(state, L"复制失败：当前页无内容或剪贴板不可用。");
    }
}

// RegisterNetworkViewClass registers the root Network view class once. There is
// no input; processing installs a small WNDCLASSW; output is true when
// CreateWindowExW can use it.
bool RegisterNetworkViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        NetworkViewState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<NetworkViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }

        switch (msg) {
        case WM_CREATE:
            if (state) {
                if (!CreateChildControls(*state)) {
                    delete state;
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    return -1;
                }
                LayoutChildren(*state);
            }
            return 0;
        case WM_SIZE:
            if (state) {
                LayoutChildren(*state);
            }
            return 0;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
                    const LRESULT selected = ::SendMessageW(state->tab, TCM_GETCURSEL, 0, 0);
                    if (selected >= 0) {
                        state->currentTab = static_cast<int>(selected);
                    }
                    RenderCurrentPage(*state);
                    LayoutChildren(*state);
                    return 0;
                }
            }
            break;
        case WM_COMMAND:
            if (state && HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                case kRefreshButtonId:
                    state->model.refresh();
                    RenderCurrentPage(*state);
                    return 0;
                case kCopyButtonId:
                    OnCopyCurrentPage(*state);
                    return 0;
                default:
                    break;
                }
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
    wc.lpszClassName = kNetworkViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateNetworkFeatureView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterNetworkViewClass()) {
        return nullptr;
    }

    auto* state = new NetworkViewState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kNetworkViewClass,
        L"Network",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
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

void ResizeNetworkFeatureView(HWND view, const RECT& bounds) {
    if (view) {
        ::MoveWindow(
            view,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            TRUE);
    }
}

} // namespace Ksword::Features::Network
