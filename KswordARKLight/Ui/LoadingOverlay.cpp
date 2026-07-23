#include "LoadingOverlay.h"

#include "Controls.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace Ksword::Ui {
namespace {

constexpr wchar_t kLoadingOverlayClass[] = L"KswordARKLight.LoadingOverlay";
constexpr UINT_PTR kSpinnerTimer = 1;
constexpr UINT kSpinnerIntervalMilliseconds = 80;

struct LoadingOverlayState final {
    std::wstring message = L"正在加载…";
    unsigned int phase = 0;
};

LRESULT CALLBACK LoadingOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<LoadingOverlayState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<LoadingOverlayState*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_TIMER:
        if (state && wParam == kSpinnerTimer) {
            state->phase = (state->phase + 1U) % 12U;
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT rect{};
        ::GetClientRect(hwnd, &rect);
        ::FillRect(dc, &rect, AppTheme().panelBrush());

        HPEN border = ::CreatePen(PS_SOLID, 1, AppTheme().borderColor);
        HGDIOBJ previousPen = ::SelectObject(dc, border);
        HGDIOBJ previousBrush = ::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
        ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        ::SelectObject(dc, previousBrush);
        ::SelectObject(dc, previousPen);
        ::DeleteObject(border);

        const int centerX = (rect.left + rect.right) / 2;
        const int centerY = std::max(rect.top + 22, (rect.top + rect.bottom) / 2 - 14);
        for (unsigned int index = 0; index < 12U; ++index) {
            const unsigned int age = (index + 12U - (state ? state->phase : 0U)) % 12U;
            const int intensity = 70 + static_cast<int>((11U - age) * 14U);
            HPEN dot = ::CreatePen(PS_SOLID, 3, RGB(intensity / 2, intensity, std::min(255, intensity + 35)));
            const double angle = static_cast<double>(index) * 3.14159265358979323846 / 6.0;
            const int x = centerX + static_cast<int>(std::cos(angle) * 12.0);
            const int y = centerY + static_cast<int>(std::sin(angle) * 12.0);
            HGDIOBJ old = ::SelectObject(dc, dot);
            ::MoveToEx(dc, x, y, nullptr);
            ::LineTo(dc, x + 1, y + 1);
            ::SelectObject(dc, old);
            ::DeleteObject(dot);
        }

        const std::wstring text = state ? state->message : L"正在加载…";
        const int textBottom = std::min(static_cast<int>(rect.bottom - 8), centerY + 52);
        RECT textRect{ rect.left + 12, centerY + 24, rect.right - 12, textBottom };
        DrawTextLine(dc, text, textRect, AppTheme().mutedTextColor, SystemUIFont(), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        ::KillTimer(hwnd, kSpinnerTimer);
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureLoadingOverlayClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = LoadingOverlayProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_WAIT);
    windowClass.hbrBackground = AppTheme().panelBrush();
    windowClass.lpszClassName = kLoadingOverlayClass;
    registered = ::RegisterClassW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

} // namespace

HWND CreateLoadingOverlay(HWND parent, const int id, const RECT& bounds) {
    if (!parent || !EnsureLoadingOverlayClass()) {
        return nullptr;
    }
    auto* state = new LoadingOverlayState{};
    HWND overlay = ::CreateWindowExW(0, kLoadingOverlayClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, std::max(1L, bounds.right - bounds.left), std::max(1L, bounds.bottom - bounds.top),
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), state);
    if (!overlay) {
        delete state;
    }
    return overlay;
}

void SetLoadingOverlay(HWND overlay, const bool visible, const std::wstring& message) {
    auto* state = overlay ? reinterpret_cast<LoadingOverlayState*>(::GetWindowLongPtrW(overlay, GWLP_USERDATA)) : nullptr;
    if (!overlay || !state) {
        return;
    }
    if (!message.empty()) {
        state->message = message;
    }
    if (visible) {
        ::SetTimer(overlay, kSpinnerTimer, kSpinnerIntervalMilliseconds, nullptr);
        ::SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        ::InvalidateRect(overlay, nullptr, TRUE);
    } else {
        ::KillTimer(overlay, kSpinnerTimer);
        ::ShowWindow(overlay, SW_HIDE);
    }
}

} // namespace Ksword::Ui
