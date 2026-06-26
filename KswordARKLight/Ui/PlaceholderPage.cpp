#include "PlaceholderPage.h"

namespace Ksword::Ui {
namespace {
constexpr wchar_t kPlaceholderClass[] = L"KswordARKLight.PlaceholderPage";

void EnsureClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPlaceholderClass;
    ::RegisterClassW(&wc);
    registered = true;
}
} // namespace

HWND CreatePlaceholderPage(HWND parent, const ModuleDescriptor& descriptor, const RECT& bounds) {
    EnsureClass();
    return ::CreateWindowExW(0, kPlaceholderClass, descriptor.title.c_str(), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

void UpdatePlaceholderPage(HWND page, const ModuleDescriptor& descriptor) {
    if (!page) {
        return;
    }
    ::SetWindowTextW(page, descriptor.title.c_str());
    ::InvalidateRect(page, nullptr, TRUE);
}

} // namespace Ksword::Ui
