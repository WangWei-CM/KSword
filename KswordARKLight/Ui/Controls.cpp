#include "Controls.h"

#include "Theme.h"

#include <commctrl.h>

namespace Ksword::Ui {
namespace {
HFONT gSystemUIFont = nullptr;

// CreateSystemUIFont queries the active Windows non-client message font. Input is
// none; processing asks SystemParametersInfoW for NONCLIENTMETRICS; output is a
// newly created HFONT or DEFAULT_GUI_FONT fallback if the API fails.
HFONT CreateSystemUIFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0)) {
        HFONT font = ::CreateFontIndirectW(&metrics.lfMessageFont);
        if (font) {
            return font;
        }
    }
    return reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}
} // namespace

HFONT SystemUIFont() {
    if (!gSystemUIFont) {
        gSystemUIFont = CreateSystemUIFont();
    }
    return gSystemUIFont;
}

void RefreshSystemUIFont() {
    if (gSystemUIFont && reinterpret_cast<HGDIOBJ>(gSystemUIFont) != ::GetStockObject(DEFAULT_GUI_FONT)) {
        ::DeleteObject(gSystemUIFont);
    }
    gSystemUIFont = nullptr;
}

bool RegisterControlClasses(HINSTANCE instance) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&icc);
    (void)instance;
    return true;
}

HWND CreateText(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h) {
    HWND hwnd = ::CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
    }
    return hwnd;
}

HWND CreateButton(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h) {
    HWND hwnd = ::CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
    }
    return hwnd;
}

void SetWindowFontRecursive(HWND root) {
    if (!root) {
        return;
    }
    ::SendMessageW(root, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
    ::EnumChildWindows(root, [](HWND child, LPARAM) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
        return TRUE;
    }, 0);
}

void PaintPanel(HDC dc, const RECT& rect) {
    ::FillRect(dc, &rect, AppTheme().panelBrush());
    HPEN pen = ::CreatePen(PS_SOLID, 1, AppTheme().borderColor);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    const int oldMode = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = ::SetTextColor(dc, color);
    HFONT selectedFont = font ? font : SystemUIFont();
    HGDIOBJ oldFont = ::SelectObject(dc, reinterpret_cast<HGDIOBJ>(selectedFont));
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_END_ELLIPSIS);
    ::SelectObject(dc, oldFont);
    ::SetTextColor(dc, oldColor);
    ::SetBkMode(dc, oldMode);
}

} // namespace Ksword::Ui
