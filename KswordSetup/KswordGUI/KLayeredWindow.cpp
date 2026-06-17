#include "KLayeredWindow.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <mutex>
#include <vector>

extern HWND fl_win32_xid(const Fl_Window* window);

namespace {
constexpr wchar_t kLayeredImageWindowClass[] = L"Ksword.LayeredImageWindow";

// LayeredWindowProc handles destruction minimally and delegates all other
// messages to DefWindowProc. Inputs are the native Win32 message parameters.
LRESULT CALLBACK LayeredWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

// EnsureGdiplus starts GDI+ once for all layered image windows.
// Input is none; output is true when a usable GDI+ token is available.
bool EnsureGdiplus() {
    static std::once_flag once;
    static bool ok = false;
    static ULONG_PTR token = 0;
    std::call_once(once, []() {
        Gdiplus::GdiplusStartupInput input;
        ok = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    });
    return ok;
}

// EnsureWindowClass registers the native layered helper class exactly once.
// Input is none; output is true when registration is available to CreateWindowEx.
bool EnsureWindowClass() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, []() {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LayeredWindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kLayeredImageWindowClass;
        const ATOM atom = ::RegisterClassExW(&wc);
        ok = atom != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });
    return ok;
}

// Utf8ToWide converts a UTF-8 path to UTF-16 for Win32/GDI+ APIs.
// Input is UTF-8 text; output is empty only when conversion fails or input is empty.
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int count = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (count <= 1) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(count - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], count);
    return wide;
}

// ClampByte narrows an integer channel into the byte range used by BGRA pixels.
unsigned char ClampByte(int value) {
    return static_cast<unsigned char>(std::max(0, std::min(255, value)));
}

// LoadPngAsPremultipliedBgra scales a PNG into a top-down premultiplied BGRA buffer.
// Input is a UTF-16 file path and target size; output pixels are suitable for AC_SRC_ALPHA.
bool LoadPngAsPremultipliedBgra(const std::wstring& path, int width, int height, std::vector<unsigned char>& pixels) {
    if (!EnsureGdiplus() || path.empty() || width <= 0 || height <= 0) {
        return false;
    }

    Gdiplus::Bitmap source(path.c_str(), FALSE);
    if (source.GetLastStatus() != Gdiplus::Ok || source.GetWidth() == 0 || source.GetHeight() == 0) {
        return false;
    }

    Gdiplus::Bitmap target(width, height, PixelFormat32bppPARGB);
    if (target.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }

    Gdiplus::Graphics graphics(&target);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    if (graphics.DrawImage(&source, 0, 0, width, height) != Gdiplus::Ok) {
        return false;
    }

    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData data = {};
    if (target.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) {
        return false;
    }

    pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    const unsigned char* sourceRow = static_cast<const unsigned char*>(data.Scan0);
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = sourceRow + static_cast<std::ptrdiff_t>(y) * data.Stride;
        unsigned char* out = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const unsigned char* p = row + x * 4;
            out[x * 4 + 0] = p[0];
            out[x * 4 + 1] = p[1];
            out[x * 4 + 2] = p[2];
            out[x * 4 + 3] = p[3];
        }
    }

    target.UnlockBits(&data);
    return true;
}

// UpdateLayeredPixels sends one premultiplied BGRA buffer to an HWND.
// Inputs are target HWND, screen position, size, and pixels; output is Win32 success.
bool UpdateLayeredPixels(HWND hwnd, int screenX, int screenY, int width, int height, const std::vector<unsigned char>& pixels) {
    if (!hwnd || width <= 0 || height <= 0 || pixels.size() < static_cast<std::size_t>(width) * height * 4) {
        return false;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = ::GetDC(nullptr);
    HDC memoryDc = ::CreateCompatibleDC(screenDc);
    HBITMAP bitmap = ::CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!memoryDc || !bitmap || !bits) {
        if (bitmap) {
            ::DeleteObject(bitmap);
        }
        if (memoryDc) {
            ::DeleteDC(memoryDc);
        }
        if (screenDc) {
            ::ReleaseDC(nullptr, screenDc);
        }
        return false;
    }

    std::copy(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(width) * height * 4, static_cast<unsigned char*>(bits));
    HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);

    POINT destination = { screenX, screenY };
    SIZE size = { width, height };
    POINT source = { 0, 0 };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL ok = ::UpdateLayeredWindow(hwnd, screenDc, &destination, &size, memoryDc, &source, 0, &blend, ULW_ALPHA);

    ::SelectObject(memoryDc, oldBitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(memoryDc);
    ::ReleaseDC(nullptr, screenDc);
    return ok != FALSE;
}
} // namespace

KLayeredImageWindow::KLayeredImageWindow()
    : hwnd_(nullptr),
      owner_(nullptr),
      width_(0),
      height_(0),
      click_through_(true) {
}

KLayeredImageWindow::~KLayeredImageWindow() {
    destroy();
}

bool KLayeredImageWindow::showPngAt(const std::string& pngPathUtf8, int screenX, int screenY, int width, int height, HWND owner) {
    return showPngAt(Utf8ToWide(pngPathUtf8), screenX, screenY, width, height, owner);
}

bool KLayeredImageWindow::showPngAt(const std::wstring& pngPath, int screenX, int screenY, int width, int height, HWND owner) {
    if (width <= 0 || height <= 0 || !ensureWindow(owner)) {
        return false;
    }

    std::vector<unsigned char> pixels;
    if (!LoadPngAsPremultipliedBgra(pngPath, width, height, pixels)) {
        return false;
    }

    width_ = width;
    height_ = height;
    owner_ = owner;
    updateExtendedStyle();
    if (!UpdateLayeredPixels(hwnd_, screenX, screenY, width, height, pixels)) {
        return false;
    }

    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
}

bool KLayeredImageWindow::showPngForWindow(Fl_Window* owner, const std::string& pngPathUtf8, int offsetX, int offsetY, int width, int height) {
    if (!owner) {
        return false;
    }
    HWND ownerHwnd = fl_win32_xid(owner);
    if (!ownerHwnd) {
        return false;
    }

    // FLTK window geometry is expressed in FLTK logical units, while native
    // layered windows are positioned in Win32 physical screen pixels. Anchor to
    // the real owner HWND rectangle and scale caller offsets/sizes by the
    // screen scale that FLTK applies to that owner. This keeps the image aligned
    // on 125%, 150%, and mixed-DPI displays.
    RECT ownerRect = {};
    if (!::GetWindowRect(ownerHwnd, &ownerRect)) {
        return false;
    }

    const int screenNumber = owner->screen_num();
    float scale = Fl::screen_scale(screenNumber);
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    const int pixelOffsetX = static_cast<int>(std::lround(static_cast<double>(offsetX) * scale));
    const int pixelOffsetY = static_cast<int>(std::lround(static_cast<double>(offsetY) * scale));
    const int pixelWidth = std::max(1, static_cast<int>(std::lround(static_cast<double>(width) * scale)));
    const int pixelHeight = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * scale)));
    return showPngAt(pngPathUtf8, ownerRect.left + pixelOffsetX, ownerRect.top + pixelOffsetY, pixelWidth, pixelHeight, ownerHwnd);
}

bool KLayeredImageWindow::moveTo(int screenX, int screenY) {
    if (!hwnd_) {
        return false;
    }
    return ::SetWindowPos(hwnd_, nullptr, screenX, screenY, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER) != FALSE;
}

void KLayeredImageWindow::hide() {
    if (hwnd_) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
}

void KLayeredImageWindow::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    owner_ = nullptr;
    width_ = 0;
    height_ = 0;
}

void KLayeredImageWindow::setClickThrough(bool enabled) {
    click_through_ = enabled;
    updateExtendedStyle();
}
bool KLayeredImageWindow::valid() const {
    return hwnd_ != nullptr && ::IsWindow(hwnd_) != FALSE;
}

HWND KLayeredImageWindow::hwnd() const {
    return hwnd_;
}

int KLayeredImageWindow::width() const {
    return width_;
}

int KLayeredImageWindow::height() const {
    return height_;
}

bool KLayeredImageWindow::ensureWindow(HWND owner) {
    if (valid()) {
        owner_ = owner;
        return true;
    }
    if (!EnsureWindowClass()) {
        return false;
    }

    owner_ = owner;
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (click_through_ ? WS_EX_TRANSPARENT : 0);
    hwnd_ = ::CreateWindowExW(
        exStyle,
        kLayeredImageWindowClass,
        L"KswordSetup Character",
        WS_POPUP,
        0,
        0,
        1,
        1,
        owner_,
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);
    return hwnd_ != nullptr;
}

void KLayeredImageWindow::updateExtendedStyle() {
    if (!hwnd_) {
        return;
    }
    LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    exStyle &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    exStyle |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (click_through_) {
        exStyle |= WS_EX_TRANSPARENT;
    }
    else {
        exStyle &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    }
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
