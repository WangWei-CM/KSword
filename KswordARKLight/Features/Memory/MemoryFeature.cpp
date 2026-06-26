#include "MemoryFeature.h"

#include "DriverMemoryView.h"
#include "../Kernel/KernelFeature.h"
#include "../../Ui/Controls.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>

#include <algorithm>

namespace Ksword::Features::Memory {
namespace {
constexpr wchar_t kMemoryHostClass[] = L"KswordARKLight.MemoryFeaturePage";
constexpr int kTabId = 52101;
constexpr int kDriverMemoryTabIndex = 0;
constexpr int kPhysicalMemoryTabIndex = 1;
constexpr int kKernelEvidenceTabIndex = 2;

// MemoryFeaturePageState owns the tab host and all retained child HWNDs. Input
// arrives through Win32 messages; processing never destroys a child page on tab
// switch, it only hides and shows existing HWNDs; no external ownership exists.
struct MemoryFeaturePageState {
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HWND driverMemoryView = nullptr;
    HWND physicalMemoryView = nullptr;
    HWND kernelEvidenceView = nullptr;
    int currentTab = kDriverMemoryTabIndex;
};

// StateFromWindow returns the page state stored on the HWND. Input is the page
// window; processing reads GWLP_USERDATA; output is null before creation or
// after destruction.
MemoryFeaturePageState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<MemoryFeaturePageState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// Width returns a non-negative client width. Input is a RECT; output is the
// usable width for layout.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative client height. Input is a RECT; output is the
// usable height for layout.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// ShowPages keeps all child HWNDs alive and only toggles visibility. Input is
// page state; processing shows the active child and hides inactive children; no
// value is returned.
void ShowPages(MemoryFeaturePageState& state) {
    const bool driverVisible = state.currentTab == kDriverMemoryTabIndex;
    const bool physicalVisible = state.currentTab == kPhysicalMemoryTabIndex;
    const bool kernelVisible = state.currentTab == kKernelEvidenceTabIndex;
    if (state.driverMemoryView) {
        ::ShowWindow(state.driverMemoryView, driverVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.physicalMemoryView) {
        ::ShowWindow(state.physicalMemoryView, physicalVisible ? SW_SHOW : SW_HIDE);
    }
    if (state.kernelEvidenceView) {
        ::ShowWindow(state.kernelEvidenceView, kernelVisible ? SW_SHOW : SW_HIDE);
    }
}

// LayoutChildren sizes the tab control and the cached child pages. Input is the
// page state; processing uses the current client rect and the tab display area;
// no return value is produced.
void LayoutChildren(MemoryFeaturePageState& state) {
    if (!state.hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    ::MoveWindow(state.tab, 0, 0, width, height, TRUE);
    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    const int pageWidth = Width(display);
    const int pageHeight = Height(display);
    if (state.driverMemoryView) {
        ::MoveWindow(state.driverMemoryView, display.left, display.top, pageWidth, pageHeight, TRUE);
    }
    if (state.physicalMemoryView) {
        ::MoveWindow(state.physicalMemoryView, display.left, display.top, pageWidth, pageHeight, TRUE);
    }
    if (state.kernelEvidenceView) {
        ::MoveWindow(state.kernelEvidenceView, display.left, display.top, pageWidth, pageHeight, TRUE);
    }
    ShowPages(state);
}

// CreateChildControls builds the tab host and the three retained child pages.
// Input is the page state with hwnd already set; processing creates each child
// once and hides the non-active ones; output is true when all children exist.
bool CreateChildControls(MemoryFeaturePageState& state) {
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kTabId, 0, 0, 0, 0);
    if (!state.tab) {
        return false;
    }
    Ksword::Ui::AddTabPage(state.tab, kDriverMemoryTabIndex, { L"驱动内存读写" });
    Ksword::Ui::AddTabPage(state.tab, kPhysicalMemoryTabIndex, { L"物理内存布局" });
    Ksword::Ui::AddTabPage(state.tab, kKernelEvidenceTabIndex, { L"内核内存证据" });
    ::SendMessageW(state.tab, TCM_SETCURSEL, static_cast<WPARAM>(kDriverMemoryTabIndex), 0);

    RECT pageRect{};
    ::GetClientRect(state.tab, &pageRect);
    pageRect = Ksword::Ui::GetTabDisplayRect(state.tab);
    const RECT childBounds{ 0, 0, std::max(1, Width(pageRect)), std::max(1, Height(pageRect)) };

    state.driverMemoryView = CreateDriverMemoryView(state.tab, childBounds);
    state.physicalMemoryView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        52102,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::PhysicalMemoryLayout);
    state.kernelEvidenceView = Ksword::Features::Kernel::CreateKernelSingleFeaturePage(
        state.tab,
        52103,
        childBounds,
        Ksword::Features::Kernel::KernelFeatureId::KernelMemoryEvidence);
    if (!state.driverMemoryView || !state.physicalMemoryView || !state.kernelEvidenceView) {
        return false;
    }
    return true;
}

// RegisterMemoryFeatureClass registers the tab-host window class once. There is
// no input; processing is idempotent; output is true when the class is usable.
bool RegisterMemoryFeatureClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        MemoryFeaturePageState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<MemoryFeaturePageState*>(create->lpCreateParams) : nullptr;
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
    wc.lpszClassName = kMemoryHostClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateMemoryFeaturePage(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterMemoryFeatureClass()) {
        return nullptr;
    }

    auto* state = new MemoryFeaturePageState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kMemoryHostClass,
        L"Memory",
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

} // namespace Ksword::Features::Memory
