#include "HardwareFeature.h"

#include "HardwareView.h"
#include "../Kernel/KernelFeature.h"
#include "../../Ui/Controls.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"

#include <algorithm>
#include <commctrl.h>

namespace Ksword::Features::Hardware {
namespace {
constexpr wchar_t kHardwareHostClass[] = L"KswordARKLight.HardwareFeaturePage";
constexpr int kTabId = 61101;
constexpr int kDeviceManagerTabIndex = 0;
constexpr int kCpuIntegrityTabIndex = 1;
constexpr int kCpuSnapshotTabIndex = 2;
constexpr int kEmbeddedKernelPrimaryTabId = 51001;
constexpr int kEmbeddedKernelSecondaryTabId = 51002;

// HardwareFeaturePageState owns the device-manager page plus both CPU-related
// KernelPage embeds. Inputs arrive through Win32 messages; processing preserves
// child HWND state across tab switches by hiding instead of destroying pages.
struct HardwareFeaturePageState {
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HWND deviceManagerView = nullptr;
    HWND cpuIntegrityView = nullptr;
    HWND cpuSnapshotView = nullptr;
    int currentTab = kDeviceManagerTabIndex;
};

// StateFromWindow retrieves the state pointer from a host HWND. Input is the
// host HWND; output is null before WM_NCCREATE or after WM_NCDESTROY.
HardwareFeaturePageState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<HardwareFeaturePageState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// Width returns a non-negative rectangle width. Input is a RECT; output is used
// by child layout.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative rectangle height. Input is a RECT; output is
// used by child layout.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// ShowPages toggles only visibility, preserving each page's internal controls.
// Input is page state; no return value is produced.
void ShowPages(HardwareFeaturePageState& state) {
    const bool deviceVisible = state.currentTab == kDeviceManagerTabIndex;
    const bool cpuIntegrityVisible = state.currentTab == kCpuIntegrityTabIndex;
    const bool cpuVisible = state.currentTab == kCpuSnapshotTabIndex;
    if (state.deviceManagerView) {
        ::ShowWindow(state.deviceManagerView, deviceVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.cpuIntegrityView) {
        ::ShowWindow(state.cpuIntegrityView, cpuIntegrityVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.cpuSnapshotView) {
        ::ShowWindow(state.cpuSnapshotView, cpuVisible ? SW_SHOW : SW_HIDE);
    }
}

// HideEmbeddedKernelNavigationTabs removes only the Kernel page navigation tabs
// from a Hardware-owned KernelPage embed. Input is the embedded Kernel HWND; the
// processing hides the primary/secondary Kernel tab controls whose stable child
// ids are defined by the referenced Kernel page implementation. It intentionally
// leaves the selected feature content and all collection logic alive, and it
// returns no value because missing child tabs are non-fatal.
void HideEmbeddedKernelNavigationTabs(HWND embeddedKernelPage) {
    if (!embeddedKernelPage) {
        return;
    }

    HWND primaryTab = ::GetDlgItem(embeddedKernelPage, kEmbeddedKernelPrimaryTabId);
    if (primaryTab) {
        ::ShowWindow(primaryTab, SW_HIDE);
    }

    HWND secondaryTab = ::GetDlgItem(embeddedKernelPage, kEmbeddedKernelSecondaryTabId);
    if (secondaryTab) {
        ::ShowWindow(secondaryTab, SW_HIDE);
    }
}

// LayoutChildren sizes the tab control and retained child pages. Input is
// state; processing uses TabCtrl_AdjustRect through the UI helper; no value is
// returned.
void LayoutChildren(HardwareFeaturePageState& state) {
    if (!state.hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    ::MoveWindow(state.tab, 0, 0, Width(rc), Height(rc), TRUE);
    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    const int pageWidth = Width(display);
    const int pageHeight = Height(display);
    if (state.deviceManagerView) {
        ::MoveWindow(state.deviceManagerView, display.left, display.top, pageWidth, pageHeight, TRUE);
    }
    if (state.cpuIntegrityView) {
        ::MoveWindow(state.cpuIntegrityView, display.left, display.top, pageWidth, pageHeight, TRUE);
        HideEmbeddedKernelNavigationTabs(state.cpuIntegrityView);
    }
    if (state.cpuSnapshotView) {
        ::MoveWindow(state.cpuSnapshotView, display.left, display.top, pageWidth, pageHeight, TRUE);
        HideEmbeddedKernelNavigationTabs(state.cpuSnapshotView);
    }
    ShowPages(state);
}

// CreateChildControls creates the tab host plus retained device and CPU pages.
// Input is state with hwnd set; processing embeds existing KernelPage features
// for CPU/IDT integrity and CPU hardware snapshot; output is true when all
// required HWNDs exist.
bool CreateChildControls(HardwareFeaturePageState& state) {
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kTabId, 0, 0, 0, 0);
    if (!state.tab) {
        return false;
    }
    Ksword::Ui::AddTabPage(state.tab, kDeviceManagerTabIndex, { L"设备管理器" });
    Ksword::Ui::AddTabPage(state.tab, kCpuIntegrityTabIndex, { L"CPU/IDT 完整性" });
    Ksword::Ui::AddTabPage(state.tab, kCpuSnapshotTabIndex, { L"CPU 硬件快照" });
    ::SendMessageW(state.tab, TCM_SETCURSEL, static_cast<WPARAM>(kDeviceManagerTabIndex), 0);

    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    const RECT childBounds{ 0, 0, std::max(1, Width(display)), std::max(1, Height(display)) };
    state.deviceManagerView = CreateHardwareDeviceManagerView(state.tab, childBounds);
    state.cpuIntegrityView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        61102,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::KernelCpuIntegrity);
    state.cpuSnapshotView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        61103,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::CpuHardwareSnapshot);
    HideEmbeddedKernelNavigationTabs(state.cpuIntegrityView);
    HideEmbeddedKernelNavigationTabs(state.cpuSnapshotView);
    return state.deviceManagerView != nullptr &&
        state.cpuIntegrityView != nullptr &&
        state.cpuSnapshotView != nullptr;
}

// RegisterHardwareFeatureClass registers the host window class once. There is
// no input; output is true when CreateWindowExW can use the class.
bool RegisterHardwareFeatureClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        HardwareFeaturePageState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<HardwareFeaturePageState*>(create->lpCreateParams) : nullptr;
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
                    ShowPages(*state);
                    return 0;
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
    wc.lpszClassName = kHardwareHostClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateHardwareFeaturePage(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterHardwareFeatureClass()) {
        return nullptr;
    }
    auto* state = new HardwareFeaturePageState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kHardwareHostClass,
        L"Hardware",
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

} // namespace Ksword::Features::Hardware
