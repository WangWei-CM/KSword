#include "DriverFeature.h"

#include "DriverActions.h"
#include "DriverModel.h"
#include "DriverObjectView.h"
#include "DriverOverviewView.h"
#include "../Kernel/KernelFeature.h"
#include "../../Ui/Controls.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace Ksword::Features::Driver {
namespace {

constexpr wchar_t kDriverFeatureClass[] = L"KswordARKLight.DriverFeaturePage";
constexpr int kRefreshButtonId = 65001;
constexpr int kExportButtonId = 65002;
constexpr int kTabControlId = 65003;
constexpr int kStatusTextId = 65004;
constexpr int kOverviewTabIndex = 0;
constexpr int kObjectTabIndex = 1;
constexpr int kIntegrityTabIndex = 2;
constexpr int kDynDataCapabilitiesTabIndex = 3;
constexpr int kMinifilterBypassPidsTabIndex = 4;
constexpr int kDriverStatusTabIndex = 5;
constexpr int kDynDataTabIndex = 6;

// DriverFeaturePageState owns the root page HWND, local driver views, and
// retained KernelPage-backed migration tabs. Inputs arrive through Win32
// messages; processing coordinates refresh/export for local tables and keeps
// every migrated page HWND alive for the lifetime of this dock page.
struct DriverFeaturePageState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND exportButton = nullptr;
    HWND statusText = nullptr;
    HWND tab = nullptr;
    HWND overviewView = nullptr;
    HWND objectView = nullptr;
    HWND integrityView = nullptr;
    HWND dynDataCapabilitiesView = nullptr;
    HWND minifilterBypassPidsView = nullptr;
    HWND driverStatusView = nullptr;
    HWND dynDataView = nullptr;
    DriverModel model;
    int currentTab = kOverviewTabIndex;
};

// StateFromWindow returns the page state stored on the HWND. Input is the page
// window; processing reads GWLP_USERDATA; output is null during creation or
// after destruction.
DriverFeaturePageState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<DriverFeaturePageState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// Width returns a non-negative rectangle width. Input is a RECT; output is the
// usable client width in pixels.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative rectangle height. Input is a RECT; output is
// the usable client height in pixels.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// ShowChildPages toggles the currently selected subview while retaining every
// child HWND. Input is page state; processing compares the selected tab index
// against each cached page and only changes visibility; no child page is
// destroyed or recreated, and no value is returned.
void ShowChildPages(DriverFeaturePageState& state) {
    const bool overviewVisible = state.currentTab == kOverviewTabIndex;
    const bool objectVisible = state.currentTab == kObjectTabIndex;
    const bool integrityVisible = state.currentTab == kIntegrityTabIndex;
    const bool dynDataCapabilitiesVisible = state.currentTab == kDynDataCapabilitiesTabIndex;
    const bool minifilterBypassPidsVisible = state.currentTab == kMinifilterBypassPidsTabIndex;
    const bool driverStatusVisible = state.currentTab == kDriverStatusTabIndex;
    const bool dynDataVisible = state.currentTab == kDynDataTabIndex;
    if (state.overviewView) {
        ::ShowWindow(state.overviewView, overviewVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.objectView) {
        ::ShowWindow(state.objectView, objectVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.integrityView) {
        ::ShowWindow(state.integrityView, integrityVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.dynDataCapabilitiesView) {
        ::ShowWindow(state.dynDataCapabilitiesView, dynDataCapabilitiesVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.minifilterBypassPidsView) {
        ::ShowWindow(state.minifilterBypassPidsView, minifilterBypassPidsVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.driverStatusView) {
        ::ShowWindow(state.driverStatusView, driverStatusVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.dynDataView) {
        ::ShowWindow(state.dynDataView, dynDataVisible ? SW_SHOW : SW_HIDE);
    }
}

// LayoutChildren positions the toolbar, tab control, and active subview.
// Input is page state; processing uses the current client rect; no value is
// returned.
void LayoutChildren(DriverFeaturePageState& state) {
    if (!state.hwnd) {
        return;
    }

    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    const int margin = 8;
    const int toolbarHeight = 30;
    const int buttonWidth = 82;
    const int buttonGap = 6;

    ::MoveWindow(state.refreshButton, margin, margin, buttonWidth, 24, TRUE);
    ::MoveWindow(state.exportButton, margin + buttonWidth + buttonGap, margin, buttonWidth, 24, TRUE);
    ::MoveWindow(state.statusText, margin + (buttonWidth + buttonGap) * 2 + 16, margin + 2, std::max(80, width - 240), 20, TRUE);

    const int tabTop = margin + toolbarHeight;
    ::MoveWindow(state.tab, margin, tabTop, std::max(100, width - margin * 2), std::max(100, height - tabTop - margin), TRUE);

    RECT tabRc{};
    ::GetClientRect(state.tab, &tabRc);
    TabCtrl_AdjustRect(state.tab, FALSE, &tabRc);

    const HWND childViews[] = {
        state.overviewView,
        state.objectView,
        state.integrityView,
        state.dynDataCapabilitiesView,
        state.minifilterBypassPidsView,
        state.driverStatusView,
        state.dynDataView
    };
    for (HWND child : childViews) {
        if (child) {
            ::MoveWindow(child, tabRc.left, tabRc.top, Width(tabRc), Height(tabRc), TRUE);
        }
    }
    ShowChildPages(state);
}

// SetStatus writes a short footer message. Inputs are page state and text;
// processing updates the read-only static control; no value is returned.
void SetStatus(DriverFeaturePageState& state, const std::wstring& text) {
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, text.c_str());
    }
}

// RefreshFeatureModel pulls the latest snapshot and redraws the local driver
// overview/object subviews. Input is page state; processing uses DriverActions
// and the local model only; KernelPage-backed migration tabs keep their own
// refresh controls and state; no value is returned.
void RefreshFeatureModel(DriverFeaturePageState& state) {
    SetStatus(state, L"正在刷新驱动概览和对象信息...");
    const DriverActionResult result = DriverActions::RefreshModel(state.model);
    RefreshDriverOverviewView(state.overviewView);
    RefreshDriverObjectView(state.objectView);
    SetStatus(state, result.statusText.empty() ? L"驱动数据已刷新。" : result.statusText);
}

// CopyCurrentTabTsv exports the active table to the clipboard. Input is page
// state; processing renders the current visible tab into TSV and copies it to
// the clipboard; no value is returned.
void CopyCurrentTabTsv(DriverFeaturePageState& state) {
    std::wstring tsv;
    if (state.currentTab == kObjectTabIndex) {
        tsv = ExportDriverObjectViewTsv(state.objectView);
    } else if (state.currentTab == kIntegrityTabIndex ||
        state.currentTab == kDynDataCapabilitiesTabIndex ||
        state.currentTab == kMinifilterBypassPidsTabIndex ||
        state.currentTab == kDriverStatusTabIndex ||
        state.currentTab == kDynDataTabIndex) {
        SetStatus(state, L"当前页由内核模块渲染，请在该页内使用其右键菜单复制/导出。");
        return;
    } else {
        tsv = ExportDriverOverviewViewTsv(state.overviewView);
    }
    if (DriverActions::CopyTextToClipboard(state.hwnd, tsv)) {
        SetStatus(state, L"已将当前表格导出为 TSV 并复制到剪贴板。");
    } else {
        SetStatus(state, L"TSV 导出失败：剪贴板不可用或当前没有可导出的内容。");
    }
}

// CreateChildControls builds the page toolbar, local driver tabs, and retained
// KernelPage-backed migration tabs. Input is page state with hwnd already
// stored; processing creates each child page exactly once; output is true when
// all required child HWNDs exist.
bool CreateChildControls(DriverFeaturePageState& state) {
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.exportButton = Ksword::Ui::CreateButton(state.hwnd, kExportButtonId, L"导出 TSV", 0, 0, 0, 0);
    state.statusText = Ksword::Ui::CreateText(state.hwnd, kStatusTextId, L"准备刷新驱动数据。", 0, 0, 0, 0);
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kTabControlId, 0, 0, 0, 0);
    if (!state.refreshButton || !state.exportButton || !state.statusText || !state.tab) {
        return false;
    }

    Ksword::Ui::AddTabPage(state.tab, kOverviewTabIndex, { L"驱动概览" });
    Ksword::Ui::AddTabPage(state.tab, kObjectTabIndex, { L"对象信息" });
    Ksword::Ui::AddTabPage(state.tab, kIntegrityTabIndex, { L"驱动完整性" });
    Ksword::Ui::AddTabPage(state.tab, kDynDataCapabilitiesTabIndex, { L"DynData能力" });
    Ksword::Ui::AddTabPage(state.tab, kMinifilterBypassPidsTabIndex, { L"Minifilter放行PID" });
    Ksword::Ui::AddTabPage(state.tab, kDriverStatusTabIndex, { L"驱动状态" });
    Ksword::Ui::AddTabPage(state.tab, kDynDataTabIndex, { L"动态偏移 / DynData" });
    ::SendMessageW(state.tab, TCM_SETCURSEL, static_cast<WPARAM>(kOverviewTabIndex), 0);
    state.currentTab = kOverviewTabIndex;

    RECT pageRect{ 0, 0, 100, 100 };
    ::GetClientRect(state.tab, &pageRect);
    TabCtrl_AdjustRect(state.tab, FALSE, &pageRect);
    const int pageWidth = std::max(1, static_cast<int>(pageRect.right - pageRect.left));
    const int pageHeight = std::max(1, static_cast<int>(pageRect.bottom - pageRect.top));
    RECT childBounds{ 0, 0, pageWidth, pageHeight };

    state.overviewView = CreateDriverOverviewView(state.tab, childBounds, &state.model);
    state.objectView = CreateDriverObjectView(state.tab, childBounds, &state.model);
    state.integrityView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        65005,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::DriverIntegrity);
    state.dynDataCapabilitiesView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        65006,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::DynDataCapabilities);
    state.minifilterBypassPidsView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        65007,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::MinifilterBypassPids);
    state.driverStatusView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        65008,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::DriverStatus);
    state.dynDataView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        65009,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::DynData);
    if (!state.overviewView ||
        !state.objectView ||
        !state.integrityView ||
        !state.dynDataCapabilitiesView ||
        !state.minifilterBypassPidsView ||
        !state.driverStatusView ||
        !state.dynDataView) {
        return false;
    }

    Ksword::Ui::SetWindowFontRecursive(state.hwnd);
    RefreshFeatureModel(state);
    return true;
}

// RegisterDriverFeatureClass installs the root page class once. There is no
// input; processing is idempotent; output is true when the class can be used.
bool RegisterDriverFeatureClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        DriverFeaturePageState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<DriverFeaturePageState*>(create->lpCreateParams) : nullptr;
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
                if (header && header->idFrom == kTabControlId && header->code == TCN_SELCHANGE) {
                    const LRESULT selected = ::SendMessageW(state->tab, TCM_GETCURSEL, 0, 0);
                    if (selected >= 0) {
                        state->currentTab = static_cast<int>(selected);
                    }
                    ShowChildPages(*state);
                    return 0;
                }
            }
            break;
        case WM_COMMAND:
            if (state && HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                case kRefreshButtonId:
                    RefreshFeatureModel(*state);
                    return 0;
                case kExportButtonId:
                    CopyCurrentTabTsv(*state);
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
    wc.lpszClassName = kDriverFeatureClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateDriverFeaturePage(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterDriverFeatureClass()) {
        return nullptr;
    }

    auto* state = new DriverFeaturePageState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kDriverFeatureClass,
        L"驱动",
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

void ResizeDriverFeaturePage(HWND page, const RECT& bounds) {
    if (page) {
        ::MoveWindow(page,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            TRUE);
    }
}

} // namespace Ksword::Features::Driver
