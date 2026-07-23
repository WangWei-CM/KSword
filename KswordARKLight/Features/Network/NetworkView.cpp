#include "NetworkView.h"

#include "NetworkModel.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/LoadingOverlay.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"

#include <algorithm>
#include <commctrl.h>
#include <windowsx.h>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
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
constexpr int kFilterBarId = 69007;
constexpr int kLoadingOverlayId = 69008;
constexpr UINT kMenuCopyCell = 69101;
constexpr UINT kMenuCopyRow = 69102;
constexpr UINT kMenuCopyVisible = 69103;
constexpr UINT kMsgRefreshCompleted = WM_APP + 595;
constexpr UINT kMsgFilterCompleted = WM_APP + 596;

struct NetworkFilterResult {
    std::uint64_t generation = 0;
    std::wstring query;
    std::vector<std::size_t> visibleIndexes;
};

struct NetworkViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND copyButton = nullptr;
    HWND statusText = nullptr;
    HWND summaryText = nullptr;
    HWND tab = nullptr;
    HWND filterBar = nullptr;
    HWND loadingOverlay = nullptr;
    Ksword::Ui::VirtualListView list;
    NetworkAuditModel model;
    std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>> filterRows;
    int currentTab = 0;
    std::wstring filterQuery;
    std::uint64_t displayGeneration = 0;
    int contextColumn = 0;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<std::vector<NetworkAuditPage>>> refreshTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<NetworkFilterResult>> filterTask;
};

NetworkViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<NetworkViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

void SetStatus(NetworkViewState& state, const std::wstring& text) {
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, text.c_str());
    }
}

void SetSummary(NetworkViewState& state, const std::wstring& text) {
    if (state.summaryText) {
        ::SetWindowTextW(state.summaryText, text.c_str());
    }
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
        return false;
    }
    ::EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
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

std::wstring SanitizeTsvCell(std::wstring cell) {
    for (wchar_t& ch : cell) {
        if (ch == L'\t' || ch == L'\r' || ch == L'\n') {
            ch = L' ';
        }
    }
    return cell;
}

void AppendTsvRow(std::wstring& output, const std::vector<std::wstring>& cells) {
    for (std::size_t column = 0; column < cells.size(); ++column) {
        if (column != 0) {
            output.push_back(L'\t');
        }
        output += SanitizeTsvCell(cells[column]);
    }
    output += L"\r\n";
}

std::wstring BuildVisiblePageTsv(const NetworkViewState& state) {
    const NetworkAuditPage* page = state.model.pageAt(state.currentTab);
    if (!page) {
        return {};
    }
    std::vector<std::wstring> headers;
    headers.reserve(page->columns.size());
    for (const NetworkAuditColumn& column : page->columns) {
        headers.push_back(column.title);
    }
    std::wstring output;
    AppendTsvRow(output, headers);
    const auto& rows = state.list.rows();
    for (const std::size_t index : state.list.visibleIndexes()) {
        if (index < rows.size()) {
            AppendTsvRow(output, rows[index].cells);
        }
    }
    return output;
}

void ApplyColumns(NetworkViewState& state, const NetworkAuditPage& page) {
    HWND list = state.list.hwnd();
    Ksword::Ui::ClearListViewColumns(list);
    std::vector<Ksword::Ui::ListViewColumn> columns;
    columns.reserve(page.columns.size());
    for (std::size_t index = 0; index < page.columns.size(); ++index) {
        columns.push_back({ static_cast<int>(index), page.columns[index].width, page.columns[index].format, page.columns[index].title });
    }
    state.list.addColumns(columns);
}

void ApplyNetworkFilter(NetworkViewState& state, NetworkFilterResult result) {
    if (result.generation != state.displayGeneration || result.query != state.filterQuery) {
        return;
    }
    state.list.setVisibleIndexes(std::move(result.visibleIndexes));
    if (!result.query.empty()) {
        SetStatus(state, L"Network 筛选结果 " + std::to_wstring(state.list.rowCount()) + L" 项。");
    }
}

void RequestNetworkFilter(NetworkViewState& state, std::wstring query) {
    state.filterQuery = std::move(query);
    const auto rows = state.filterRows;
    const std::uint64_t generation = state.displayGeneration;
    if (!state.filterTask || !rows) {
        return;
    }
    state.filterTask->request(
        [rows, generation, query = state.filterQuery]() mutable {
            NetworkFilterResult result{};
            result.generation = generation;
            result.query = std::move(query);
            result.visibleIndexes = Ksword::Ui::VirtualListView::FilterRowIndexes(*rows, result.query);
            return result;
        },
        [&state](std::uint64_t, std::optional<NetworkFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value()) {
                SetStatus(state, L"Network 筛选任务异常结束，已保留当前可见结果。");
                return;
            }
            ApplyNetworkFilter(state, std::move(*result));
        });
}

void RenderCurrentPage(NetworkViewState& state) {
    const NetworkAuditPage* page = state.model.pageAt(state.currentTab);
    if (!page) {
        SetSummary(state, L"正在等待后台 Network 审计快照…");
        state.list.setRows({});
        return;
    }
    ApplyColumns(state, *page);
    auto rows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>();
    rows->reserve(page->rows.size());
    for (std::size_t index = 0; index < page->rows.size(); ++index) {
        Ksword::Ui::VirtualListRow row{};
        row.stableKey = std::to_wstring(index);
        row.cells = page->rows[index].cells;
        for (const std::wstring& cell : row.cells) {
            row.stableKey += L"|" + cell;
        }
        rows->push_back(std::move(row));
    }
    state.list.setRows(*rows);
    state.list.setVisibleIndexes({});
    state.filterRows = std::move(rows);
    ++state.displayGeneration;
    SetSummary(state, page->summary);
    RequestNetworkFilter(state, state.filterBar ? Ksword::Ui::GetFilterBarText(state.filterBar) : state.filterQuery);
}

void BeginNetworkRefresh(NetworkViewState& state) {
    if (!state.refreshTask) {
        return;
    }
    const bool firstLoad = state.model.pages().empty();
    SetStatus(state, state.refreshTask->running() ? L"Network 刷新已排队，等待当前快照完成…" : L"正在后台采集 TCP、UDP、WFP、NDIS、AFD 和 NSI 审计…");
    ::EnableWindow(state.refreshButton, FALSE);
    if (firstLoad) {
        Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, true, L"正在加载 Network 审计…");
    }
    state.refreshTask->request(
        [] { return BuildNetworkAuditPages(); },
        [&state](std::uint64_t, std::optional<std::vector<NetworkAuditPage>>&& pages, std::exception_ptr error) {
            ::EnableWindow(state.refreshButton, TRUE);
            Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, false);
            if (error || !pages.has_value()) {
                SetStatus(state, L"Network 后台审计异常结束。请检查驱动状态与访问权限。");
                return;
            }
            state.model.replacePages(std::move(*pages));
            RenderCurrentPage(state);
            SetStatus(state, L"Network R0 与 R3 审计快照已刷新。");
        });
}

std::wstring SelectedRowsText(const NetworkViewState& state, bool allVisible) {
    const HWND list = state.list.hwnd();
    const auto& rows = state.list.rows();
    std::wstring text;
    const auto& visible = state.list.visibleIndexes();
    for (std::size_t item = 0; item < visible.size(); ++item) {
        if (!allVisible && (ListView_GetItemState(list, static_cast<int>(item), LVIS_SELECTED) & LVIS_SELECTED) == 0) {
            continue;
        }
        const std::size_t source = visible[item];
        if (source < rows.size()) {
            AppendTsvRow(text, rows[source].cells);
        }
    }
    return text;
}

std::wstring SelectedCellText(const NetworkViewState& state) {
    const HWND list = state.list.hwnd();
    const int selected = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    const auto& visible = state.list.visibleIndexes();
    const auto& rows = state.list.rows();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visible.size()) {
        return {};
    }
    const std::size_t source = visible[static_cast<std::size_t>(selected)];
    if (source >= rows.size() || state.contextColumn < 0 || static_cast<std::size_t>(state.contextColumn) >= rows[source].cells.size()) {
        return {};
    }
    return rows[source].cells[static_cast<std::size_t>(state.contextColumn)];
}

void ShowListContextMenu(NetworkViewState& state, POINT point) {
    POINT client = point;
    ::ScreenToClient(state.list.hwnd(), &client);
    LVHITTESTINFO hit{};
    hit.pt = client;
    const int hitItem = ListView_SubItemHitTest(state.list.hwnd(), &hit);
    if (hitItem >= 0) {
        state.contextColumn = hit.iSubItem;
        ListView_SetItemState(state.list.hwnd(), -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(state.list.hwnd(), hitItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    const bool hasSelection = ListView_GetNextItem(state.list.hwnd(), -1, LVNI_SELECTED) >= 0;
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyCell, L"复制单元格");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyRow, L"复制行");
    ::AppendMenuW(menu, MF_STRING | (!state.list.visibleIndexes().empty() ? 0U : MF_GRAYED), kMenuCopyVisible, L"复制可见结果");
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == kMenuCopyCell) {
        SetStatus(state, CopyTextToClipboard(state.hwnd, SelectedCellText(state)) ? L"已复制单元格。" : L"复制单元格失败。");
    } else if (command == kMenuCopyRow) {
        SetStatus(state, CopyTextToClipboard(state.hwnd, SelectedRowsText(state, false)) ? L"已复制行。" : L"复制行失败。");
    } else if (command == kMenuCopyVisible) {
        SetStatus(state, CopyTextToClipboard(state.hwnd, SelectedRowsText(state, true)) ? L"已复制可见结果。" : L"复制可见结果失败。");
    }
}

void LayoutChildren(NetworkViewState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    const int margin = 8;
    const int buttonWidth = 82;
    const int buttonGap = 6;
    ::MoveWindow(state.refreshButton, margin, margin, buttonWidth, 24, TRUE);
    ::MoveWindow(state.copyButton, margin + buttonWidth + buttonGap, margin, buttonWidth, 24, TRUE);
    ::MoveWindow(state.statusText, margin + (buttonWidth + buttonGap) * 2 + 16, margin + 2, std::max(100, width - 240), 20, TRUE);
    const int summaryTop = margin + 30;
    ::MoveWindow(state.summaryText, margin, summaryTop, std::max(100, width - margin * 2), 38, TRUE);
    const int filterTop = summaryTop + 42;
    ::MoveWindow(state.filterBar, margin, filterTop, std::max(100, width - margin * 2), 24, TRUE);
    const int tabTop = filterTop + 28;
    ::MoveWindow(state.tab, margin, tabTop, std::max(100, width - margin * 2), std::max(100, height - tabTop - margin), TRUE);
    const RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    ::MoveWindow(state.list.hwnd(), display.left, display.top, Width(display), Height(display), TRUE);
    ::MoveWindow(state.loadingOverlay, display.left, display.top, std::max(1, Width(display)), std::max(1, Height(display)), TRUE);
}

bool CreateChildControls(NetworkViewState& state) {
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.copyButton = Ksword::Ui::CreateButton(state.hwnd, kCopyButtonId, L"复制 TSV", 0, 0, 0, 0);
    state.statusText = Ksword::Ui::CreateText(state.hwnd, kStatusTextId, L"Network 审计准备就绪。", 0, 0, 0, 0);
    state.summaryText = Ksword::Ui::CreateText(state.hwnd, kSummaryTextId, L"", 0, 0, 0, 0);
    state.filterBar = Ksword::Ui::CreateFilterBar(state.hwnd, kFilterBarId, L"筛选当前页所有列和详情文本", 0, 0, 0, 0);
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kTabControlId, 0, 0, 0, 0);
    if (!state.refreshButton || !state.copyButton || !state.statusText || !state.summaryText || !state.filterBar || !state.tab ||
        !state.list.create(state.tab, kListViewId, 0, 0, 1, 1, LVS_SHOWSELALWAYS)) {
        return false;
    }
    constexpr const wchar_t* kTabTitles[] = {
        L"TCP/UDP R0 cross-view", L"AFD endpoint", L"WFP callout/filter/provider", L"NDIS protocol/filter", L"NSI / interfaces / routes"
    };
    for (int index = 0; index < static_cast<int>(std::size(kTabTitles)); ++index) {
        Ksword::Ui::AddTabPage(state.tab, index, { kTabTitles[index], static_cast<LPARAM>(index) });
    }
    ::SendMessageW(state.tab, TCM_SETCURSEL, 0, 0);
    ListView_SetExtendedListViewStyle(state.list.hwnd(), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    state.loadingOverlay = Ksword::Ui::CreateLoadingOverlay(state.tab, kLoadingOverlayId, { 0, 0, 1, 1 });
    if (!state.loadingOverlay) {
        return false;
    }
    Ksword::Ui::SetWindowFontRecursive(state.hwnd);
    return true;
}

void OnCopyCurrentPage(NetworkViewState& state) {
    const std::wstring tsv = BuildVisiblePageTsv(state);
    SetStatus(state, CopyTextToClipboard(state.hwnd, tsv) ? L"已复制当前可见 Network 审计结果。" : L"复制失败：当前页无内容或剪贴板不可用。");
}

bool RegisterNetworkViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        NetworkViewState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            state = create ? static_cast<NetworkViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }
        switch (msg) {
        case WM_CREATE:
            if (!state || !CreateChildControls(*state)) {
                delete state;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return -1;
            }
            state->refreshTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<std::vector<NetworkAuditPage>>>(hwnd, kMsgRefreshCompleted);
            state->filterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<NetworkFilterResult>>(hwnd, kMsgFilterCompleted);
            LayoutChildren(*state);
            BeginNetworkRefresh(*state);
            return 0;
        case WM_SIZE:
            if (state) LayoutChildren(*state);
            return 0;
        case WM_COMMAND:
            if (state && LOWORD(wParam) == kFilterBarId && HIWORD(wParam) == EN_CHANGE) {
                RequestNetworkFilter(*state, Ksword::Ui::GetFilterBarText(state->filterBar));
                return 0;
            }
            if (state && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kRefreshButtonId) {
                BeginNetworkRefresh(*state);
                return 0;
            }
            if (state && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kCopyButtonId) {
                OnCopyCurrentPage(*state);
                return 0;
            }
            break;
        case kMsgRefreshCompleted:
            if (state && state->refreshTask && state->refreshTask->consume(hwnd, wParam, lParam)) return 0;
            break;
        case kMsgFilterCompleted:
            if (state && state->filterTask && state->filterTask->consume(hwnd, wParam, lParam)) return 0;
            break;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (state && header && header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
                const LRESULT selected = ::SendMessageW(state->tab, TCM_GETCURSEL, 0, 0);
                if (selected >= 0) state->currentTab = static_cast<int>(selected);
                RenderCurrentPage(*state);
                LayoutChildren(*state);
                return 0;
            }
            if (state && header && header->hwndFrom == state->list.hwnd()) {
                LRESULT result = 0;
                if (state->list.handleNotify(*header, result)) return result;
                if (header->code == NM_RCLICK) {
                    POINT point{};
                    ::GetCursorPos(&point);
                    ShowListContextMenu(*state, point);
                    return 0;
                }
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->list.hwnd()) {
                POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (point.x == -1 && point.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->list.hwnd(), &rc);
                    point = { rc.left + 20, rc.top + 20 };
                }
                ShowListContextMenu(*state, point);
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
            if (state) {
                if (state->refreshTask) state->refreshTask->cancel();
                if (state->filterTask) state->filterTask->cancel();
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
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kNetworkViewClass;
    registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

} // namespace

HWND CreateNetworkFeatureView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterNetworkViewClass()) {
        return nullptr;
    }
    auto* state = new NetworkViewState();
    HWND hwnd = ::CreateWindowExW(0, kNetworkViewClass, L"Network", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, Width(bounds), Height(bounds), parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::Network
