#include "PlaceholderPage.h"

#include "Controls.h"
#include "Theme.h"

namespace Ksword::Ui {
namespace {
constexpr wchar_t kPlaceholderClass[] = L"KswordARKLight.PlaceholderPage";

struct PlaceholderState final {
    std::wstring title;
    std::wstring summary;
    std::wstring status;
    bool loading = false;
};

LRESULT CALLBACK PlaceholderPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PlaceholderState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<PlaceholderState*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT rect{};
        ::GetClientRect(hwnd, &rect);
        ::FillRect(dc, &rect, AppTheme().windowBrush());

        const std::wstring title = state ? state->title : L"KswordARKLight";
        const std::wstring status = state && !state->status.empty()
            ? state->status
            : (state && state->loading ? L"正在加载页面…" : L"选择此页面后开始加载。");
        const std::wstring summary = state ? state->summary : L"";
        RECT titleRect{ rect.left + 28, rect.top + 28, rect.right - 28, rect.top + 58 };
        RECT statusRect{ rect.left + 28, rect.top + 66, rect.right - 28, rect.top + 92 };
        RECT summaryRect{ rect.left + 28, rect.top + 104, rect.right - 28, rect.bottom - 28 };
        DrawTextLine(dc, title, titleRect, AppTheme().textColor, SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextLine(dc, status, statusRect, state && state->loading ? AppTheme().accentColor : AppTheme().mutedTextColor,
            SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextLine(dc, summary, summaryRect, AppTheme().mutedTextColor, SystemUIFont(), DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        ::EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnsureClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = PlaceholderPageProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = AppTheme().windowBrush();
    wc.lpszClassName = kPlaceholderClass;
    ::RegisterClassW(&wc);
    registered = true;
}
} // namespace

HWND CreatePlaceholderPage(HWND parent, const ModuleDescriptor& descriptor, const RECT& bounds) {
    EnsureClass();
    auto* state = new PlaceholderState{ descriptor.title, descriptor.summary, L"选择此页面后开始加载。", false };
    HWND page = ::CreateWindowExW(0, kPlaceholderClass, descriptor.title.c_str(), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!page) {
        delete state;
    }
    return page;
}

void UpdatePlaceholderPage(HWND page, const ModuleDescriptor& descriptor) {
    auto* state = page ? reinterpret_cast<PlaceholderState*>(::GetWindowLongPtrW(page, GWLP_USERDATA)) : nullptr;
    if (!page || !state) {
        return;
    }
    state->title = descriptor.title;
    state->summary = descriptor.summary;
    ::SetWindowTextW(page, descriptor.title.c_str());
    ::InvalidateRect(page, nullptr, TRUE);
}

void SetPlaceholderPageLoading(HWND page, const bool loading, const std::wstring& status) {
    auto* state = page ? reinterpret_cast<PlaceholderState*>(::GetWindowLongPtrW(page, GWLP_USERDATA)) : nullptr;
    if (!page || !state) {
        return;
    }
    state->loading = loading;
    state->status = status.empty() ? (loading ? L"正在加载页面…" : L"选择此页面后开始加载。") : status;
    ::InvalidateRect(page, nullptr, TRUE);
}

} // namespace Ksword::Ui
