#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>

#include "KTitleBar.h"

#include "KTheme.h"
#include "../resource.h"
#include "../KswordWinAPICore/ksword.h"

#include "Fl.H"
#include "Fl_ICO_Image.H"
#include "Fl_PNG_Image.H"
#include "fl_draw.H"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

// The bundled FLTK headers in this repository are included without an FL/
// prefix, while platform.H expects that layout. Declare only the Win32 handle
// bridge we need and keep the implementation linked from the existing FLTK lib.
extern HWND fl_win32_xid(const Fl_Window* window);

namespace {
constexpr int kTitleBarHeight = 42;
constexpr int kCaptionButtonWidth = 46;
constexpr int kCaptionIconSize = 24;
constexpr int kLogoTargetHeight = 26;
constexpr int kSnapEdgeMargin = 8;
constexpr int kMinimumRestoreWidth = 360;
constexpr int kMinimumRestoreHeight = 240;
constexpr int kMinimumResizeBorder = 6;
const wchar_t kChromeWndProcProp[] = L"KswordFrame3.CustomChrome.PreviousWndProc";
const wchar_t kChromeWindowProp[] = L"KswordFrame3.CustomChrome.FlWindow";

// EdgeSnap describes the basic Aero Snap target selected by a drag release.
// Input comes from root-screen cursor coordinates; None means keep normal bounds.
enum class EdgeSnap {
    None,
    Top,
    Left,
    Right
};

// ClampByte keeps manual color blending inside the displayable 8-bit range.
// Input is an integer channel candidate; output is a valid byte channel.
unsigned char ClampByte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<unsigned char>(value);
}

// ClampInt constrains integer geometry values to a safe range. Inputs are the
// candidate and inclusive bounds; output is the clamped value.
int ClampInt(int value, int minValue, int maxValue) {
    if (maxValue < minValue) {
        return minValue;
    }
    return std::max(minValue, std::min(value, maxValue));
}

// BlendColor approximates alpha composition for FLTK colors, which are drawn as
// opaque values. Inputs are foreground/background colors and foreground opacity;
// output is a new RGB FLTK color.
Fl_Color BlendColor(Fl_Color foreground, Fl_Color background, double opacity) {
    opacity = std::max(0.0, std::min(1.0, opacity));
    uchar fr = 0;
    uchar fg = 0;
    uchar fb = 0;
    uchar br = 0;
    uchar bg = 0;
    uchar bb = 0;
    Fl::get_color(foreground, fr, fg, fb);
    Fl::get_color(background, br, bg, bb);
    const double inv = 1.0 - opacity;
    return fl_rgb_color(
        ClampByte(static_cast<int>(fr * opacity + br * inv + 0.5)),
        ClampByte(static_cast<int>(fg * opacity + bg * inv + 0.5)),
        ClampByte(static_cast<int>(fb * opacity + bb * inv + 0.5)));
}

// FileExists checks whether a path points to a readable filesystem object.
// Input is a UTF-8/narrow path used by this Win32 project; output is true when present.
bool FileExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attrs = ::GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// DirectoryOf extracts the parent directory from a Windows path. Input is a path
// string; output is an empty string when no separator exists.
std::string DirectoryOf(const std::string& path) {
    const std::size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return "";
    }
    return path.substr(0, pos);
}

// JoinPath appends a relative path to a base directory. Inputs are narrow paths;
// output uses a backslash separator because this project targets Windows.
std::string JoinPath(const std::string& base, const std::string& child) {
    if (base.empty()) {
        return child;
    }
    const char last = base[base.size() - 1];
    if (last == '\\' || last == '/') {
        return base + child;
    }
    return base + "\\" + child;
}

// ExecutableDirectory returns the directory containing the running module. It is
// used only as a fallback when embedded RCDATA extraction is unavailable.
std::string ExecutableDirectory() {
    char modulePath[MAX_PATH] = {};
    const DWORD written = ::GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (written == 0 || written >= MAX_PATH) {
        return "";
    }
    return DirectoryOf(modulePath);
}

// ReadResourceBytes reads one RT_RCDATA resource from the current executable.
// Input is a numeric resource id; output is an empty vector when not found.
std::vector<unsigned char> ReadResourceBytes(int resourceId) {
    HMODULE module = ::GetModuleHandleW(nullptr);
    if (!module) {
        return {};
    }
    HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return {};
    }
    const DWORD size = ::SizeofResource(module, resource);
    HGLOBAL loaded = ::LoadResource(module, resource);
    if (!loaded || size == 0) {
        return {};
    }
    const void* data = ::LockResource(loaded);
    if (!data) {
        return {};
    }
    const unsigned char* begin = static_cast<const unsigned char*>(data);
    return std::vector<unsigned char>(begin, begin + size);
}

// ExtractResourceToTemp writes an embedded resource to a stable temp file for
// FLTK image loaders that accept paths rather than memory buffers. Inputs are a
// resource id and file suffix; output is the temp path or empty on failure.
std::string ExtractResourceToTemp(int resourceId, const char* fileName) {
    const std::vector<unsigned char> bytes = ReadResourceBytes(resourceId);
    if (bytes.empty() || !fileName || !*fileName) {
        return "";
    }

    char tempDir[MAX_PATH] = {};
    const DWORD count = ::GetTempPathA(MAX_PATH, tempDir);
    if (count == 0 || count >= MAX_PATH) {
        return "";
    }

    const std::string path = JoinPath(tempDir, std::string("KswordFrame3_0_") + fileName);
    // Always rewrite the temp copy so replacing the project asset and rebuilding
    // cannot accidentally display an older resource left by a previous run.
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) {
        return "";
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out ? path : "";
}

// ResolveProjectAsset finds a resource file when running from the source tree or
// from x64/Debug|Release. Input is the resource.h relative path; output may be empty.
std::string ResolveProjectAsset(const char* relativePath) {
    if (!relativePath || !*relativePath) {
        return "";
    }

    const std::string relative(relativePath);
    const std::string exeDir = ExecutableDirectory();
    std::vector<std::string> candidates;
    candidates.push_back(relative);
    candidates.push_back(JoinPath(exeDir, relative));
    candidates.push_back(JoinPath(JoinPath(exeDir, "..\\..\\KswordFrame3.0"), relative));
    candidates.push_back(JoinPath(JoinPath(exeDir, "..\\.."), relative));

    for (const std::string& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return "";
}

// WindowHandle maps an FLTK window to HWND only after the window has been shown.
// Input may be null; output is null when no platform handle exists yet.
HWND WindowHandle(Fl_Window* window) {
    if (!window) {
        return nullptr;
    }
    return fl_win32_xid(window);
}

// PointInRect tests absolute FLTK event coordinates against a simple rectangle.
// Inputs are point and rect; output is true only inside the rect bounds.
bool PointInRect(int px, int py, const KTitleBar::Rect& rect) {
    return rect.w > 0 && rect.h > 0 && px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h;
}

// DrawLineIcon resets the FLTK line style after custom caption button drawing.
// Input is the desired color and line width; there is no return value.
void SetCaptionLineStyle(Fl_Color color, int width) {
    fl_color(color);
    fl_line_style(FL_SOLID, width);
}

// ChromeSetWindowBounds applies outer window bounds in FLTK screen units. The
// optional frame refresh is size-neutral: FLTK may run with a logical-to-physical
// scale, so Win32 is asked only to recalc styles, never to resize with pixels.
void ChromeSetWindowBounds(Fl_Window* window, const KTitleBar::Rect& bounds, bool refreshFrame) {
    if (!window) {
        return;
    }

    const int safeWidth = std::max(1, bounds.w);
    const int safeHeight = std::max(1, bounds.h);
    window->resize(bounds.x, bounds.y, safeWidth, safeHeight);

    HWND hwnd = WindowHandle(window);
    if (!hwnd) {
        return;
    }

    ::SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// ChromeShowWindow is the only ShowWindow wrapper used by the custom chrome.
// Input is an FLTK window and Win32 show command; output is true if Win32 handled it.
bool ChromeShowWindow(Fl_Window* window, int command) {
    HWND hwnd = WindowHandle(window);
    if (!hwnd) {
        return false;
    }
    ::ShowWindow(hwnd, command);
    return true;
}

// WorkAreaForRootPoint returns the current monitor work area in FLTK logical
// screen units. Inputs are root-screen cursor/window-center coordinates; output
// is true only when FLTK reports a positive monitor work rectangle.
bool WorkAreaForRootPoint(int rootX, int rootY, KTitleBar::Rect& area) {
    int screenX = 0;
    int screenY = 0;
    int screenW = 0;
    int screenH = 0;
    Fl::screen_work_area(screenX, screenY, screenW, screenH, rootX, rootY);
    if (screenW <= 0 || screenH <= 0) {
        Fl::screen_work_area(screenX, screenY, screenW, screenH);
    }
    if (screenW <= 0 || screenH <= 0) {
        return false;
    }

    area.x = screenX;
    area.y = screenY;
    area.w = screenW;
    area.h = screenH;
    return true;
}

// WorkAreaForWindow returns the current monitor work area in the same FLTK
// coordinate space used by Fl_Window::resize(). This avoids mixing Win32
// physical pixels with FLTK logical units on scaled displays.
bool WorkAreaForWindow(Fl_Window* window, KTitleBar::Rect& area) {
    if (window) {
        const int centerX = window->x() + window->w() / 2;
        const int centerY = window->y() + window->h() / 2;
        if (WorkAreaForRootPoint(centerX, centerY, area)) {
            return true;
        }
    }
    return WorkAreaForRootPoint(Fl::event_x_root(), Fl::event_y_root(), area);
}

// SnapForCursor converts a root-screen drag point into a basic snap target.
// Inputs are monitor work area and cursor coordinates; output is the edge target.
EdgeSnap SnapForCursor(const KTitleBar::Rect& workArea, int rootX, int rootY) {
    if (rootY <= workArea.y + kSnapEdgeMargin) {
        return EdgeSnap::Top;
    }
    if (rootX <= workArea.x + kSnapEdgeMargin) {
        return EdgeSnap::Left;
    }
    if (rootX >= workArea.x + workArea.w - kSnapEdgeMargin - 1) {
        return EdgeSnap::Right;
    }
    return EdgeSnap::None;
}

// FindInstalledTitleBar detects a previously installed KTitleBar child. Input is
// a window; output is the child pointer or nullptr when not present.
KTitleBar* FindInstalledTitleBar(Fl_Window* window) {
    if (!window) {
        return nullptr;
    }
    for (int i = 0; i < window->children(); ++i) {
        KTitleBar* bar = dynamic_cast<KTitleBar*>(window->child(i));
        if (bar) {
            return bar;
        }
    }
    return nullptr;
}

// PreviousChromeWndProc returns the FLTK window procedure saved before the
// custom chrome subclass was installed. Input is an HWND; output may be null
// only if subclass installation failed or the window is being destroyed.
WNDPROC PreviousChromeWndProc(HWND hwnd) {
    return reinterpret_cast<WNDPROC>(::GetPropW(hwnd, kChromeWndProcProp));
}

// CallPreviousChromeWndProc forwards messages to FLTK so normal double-buffered
// client painting and control repaint behavior remain owned by FLTK. Inputs are
// the original Win32 message parameters; output is the previous procedure result.
LRESULT CallPreviousChromeWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    WNDPROC previous = PreviousChromeWndProc(hwnd);
    if (previous) {
        return ::CallWindowProcW(previous, hwnd, message, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// WindowFromChromeProp maps a subclassed HWND back to its FLTK owner. Input is
// the native window handle; output is null when no live FLTK window was recorded.
Fl_Window* WindowFromChromeProp(HWND hwnd) {
    return reinterpret_cast<Fl_Window*>(::GetPropW(hwnd, kChromeWindowProp));
}

// NativeResizeBorder returns the Win32 hit-test band in physical pixels. Input
// is none; output is never smaller than a practical mouse target for borderless
// resize on displays where Windows reports a very small padded frame.
int NativeResizeBorder() {
    const int frame = ::GetSystemMetrics(SM_CXSIZEFRAME);
    const int padded = ::GetSystemMetrics(SM_CXPADDEDBORDER);
    return std::max(kMinimumResizeBorder, frame + padded);
}

// ClientPointInCaptionButtonBand protects custom caption buttons from native
// resize hit-tests. Inputs are the FLTK owner, HWND, and screen point; output is
// true when the point falls over the minimize/maximize/close button strip.
bool ClientPointInCaptionButtonBand(Fl_Window* window, HWND hwnd, POINT screenPoint) {
    KTitleBar* bar = FindInstalledTitleBar(window);
    if (!bar || !window) {
        return false;
    }

    RECT clientRect = {};
    if (!::GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    POINT clientPoint = screenPoint;
    if (!::ScreenToClient(hwnd, &clientPoint)) {
        return false;
    }

    const int clientWidth = std::max<LONG>(1, clientRect.right - clientRect.left);
    const int clientHeight = std::max<LONG>(1, clientRect.bottom - clientRect.top);
    const double scaleX = static_cast<double>(clientWidth) / std::max(1, window->w());
    const double scaleY = static_cast<double>(clientHeight) / std::max(1, window->h());
    const int buttonSlots = bar->showMaximize() ? 3 : 2;
    const int buttonBand = static_cast<int>(kCaptionButtonWidth * buttonSlots * scaleX + 0.5);
    const int titleHeight = static_cast<int>(bar->h() * scaleY + 0.5);

    return clientPoint.y >= 0 &&
           clientPoint.y < titleHeight &&
           clientPoint.x >= clientWidth - buttonBand &&
           clientPoint.x < clientWidth;
}

// NativeResizeHitTest computes border and corner HT* codes for a borderless
// window. Inputs are the HWND, owner, and WM_NCHITTEST lParam; output is an HT
// code or HTNOWHERE when the point should remain normal FLTK client area.
LRESULT NativeResizeHitTest(HWND hwnd, Fl_Window* window, LPARAM lParam) {
    RECT windowRect = {};
    if (!::GetWindowRect(hwnd, &windowRect)) {
        return HTNOWHERE;
    }

    POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    const int border = NativeResizeBorder();
    bool onLeft = screenPoint.x >= windowRect.left && screenPoint.x < windowRect.left + border;
    bool onRight = screenPoint.x < windowRect.right && screenPoint.x >= windowRect.right - border;
    bool onTop = screenPoint.y >= windowRect.top && screenPoint.y < windowRect.top + border;
    bool onBottom = screenPoint.y < windowRect.bottom && screenPoint.y >= windowRect.bottom - border;

    // The top-right caption buttons must keep receiving FLTK mouse events, so
    // suppress native top/right resize hit-tests inside that button band.
    if ((onTop || onRight) && ClientPointInCaptionButtonBand(window, hwnd, screenPoint)) {
        onTop = false;
        onRight = false;
    }

    if (onTop && onLeft) {
        return HTTOPLEFT;
    }
    if (onTop && onRight) {
        return HTTOPRIGHT;
    }
    if (onBottom && onLeft) {
        return HTBOTTOMLEFT;
    }
    if (onBottom && onRight) {
        return HTBOTTOMRIGHT;
    }
    if (onLeft) {
        return HTLEFT;
    }
    if (onRight) {
        return HTRIGHT;
    }
    if (onTop) {
        return HTTOP;
    }
    if (onBottom) {
        return HTBOTTOM;
    }
    return HTNOWHERE;
}

// WindowMatchesMonitorWorkArea checks whether a manually resized borderless
// window already occupies the monitor work area. Input is HWND; output supports
// restoring the maximize icon correctly after minimize/taskbar restore.
bool WindowMatchesMonitorWorkArea(HWND hwnd) {
    RECT windowRect = {};
    if (!::GetWindowRect(hwnd, &windowRect)) {
        return false;
    }

    HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !::GetMonitorInfoW(monitor, &info)) {
        return false;
    }

    constexpr int kWorkAreaTolerance = 2;
    return std::abs(windowRect.left - info.rcWork.left) <= kWorkAreaTolerance &&
           std::abs(windowRect.top - info.rcWork.top) <= kWorkAreaTolerance &&
           std::abs(windowRect.right - info.rcWork.right) <= kWorkAreaTolerance &&
           std::abs(windowRect.bottom - info.rcWork.bottom) <= kWorkAreaTolerance;
}

// ApplyMonitorMaxInfo constrains native maximize operations to the taskbar-safe
// monitor work area. Inputs are HWND and WM_GETMINMAXINFO lParam; return is none.
void ApplyMonitorMaxInfo(HWND hwnd, LPARAM lParam) {
    MINMAXINFO* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
    if (!minMax) {
        return;
    }

    HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !::GetMonitorInfoW(monitor, &info)) {
        return;
    }

    const RECT& work = info.rcWork;
    const RECT& monitorRect = info.rcMonitor;
    minMax->ptMaxPosition.x = work.left - monitorRect.left;
    minMax->ptMaxPosition.y = work.top - monitorRect.top;
    minMax->ptMaxSize.x = work.right - work.left;
    minMax->ptMaxSize.y = work.bottom - work.top;
}

// SyncTitleBarFromNativeSize mirrors native WM_SIZE changes into the FLTK title
// bar only. Inputs are HWND, owner, and size code; output is none.
void SyncTitleBarFromNativeSize(HWND hwnd, Fl_Window* window, WPARAM sizeCode) {
    if (!window || sizeCode == SIZE_MINIMIZED) {
        return;
    }

    KTitleBar* bar = FindInstalledTitleBar(window);
    if (!bar) {
        return;
    }

    const bool maximized = sizeCode == SIZE_MAXIMIZED || ::IsZoomed(hwnd) || WindowMatchesMonitorWorkArea(hwnd);
    bar->syncFromNativeWindow(window->w(), maximized);
}

// ChromeWindowProc adds only native-window polish around FLTK: it blocks full
// background erase, exposes resize hit-tests, and forwards all normal messages
// back to FLTK without injecting redraws during move/window-position messages.
LRESULT CALLBACK ChromeWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) {
        // Returning non-zero tells Windows the client background is already
        // handled by FLTK's buffered paint path, preventing erase-then-redraw
        // flicker while the borderless window is moved.
        return 1;
    }

    if (message == WM_NCCALCSIZE && wParam == TRUE) {
        // Keep any resizable native style invisible; the full HWND remains FLTK
        // client area and KTitleBar draws the custom chrome.
        return 0;
    }

    if (message == WM_NCHITTEST) {
        Fl_Window* window = WindowFromChromeProp(hwnd);
        const LRESULT resizeHit = NativeResizeHitTest(hwnd, window, lParam);
        if (resizeHit != HTNOWHERE) {
            return resizeHit;
        }
        return CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
    }

    if (message == WM_GETMINMAXINFO) {
        const LRESULT result = CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
        ApplyMonitorMaxInfo(hwnd, lParam);
        return result;
    }

    if (message == WM_SIZE) {
        const LRESULT result = CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
        SyncTitleBarFromNativeSize(hwnd, WindowFromChromeProp(hwnd), wParam);
        return result;
    }

    if (message == WM_NCDESTROY) {
        WNDPROC previous = PreviousChromeWndProc(hwnd);
        const LRESULT result = CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
        if (previous) {
            ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        }
        ::RemovePropW(hwnd, kChromeWndProcProp);
        ::RemovePropW(hwnd, kChromeWindowProp);
        return result;
    }

    return CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
}

// InstallChromeSubclass attaches ChromeWindowProc once per HWND. Inputs are the
// FLTK owner and native handle; output is true when the subclass is present.
bool InstallChromeSubclass(Fl_Window* window, HWND hwnd) {
    if (!window || !hwnd) {
        return false;
    }

    ::SetPropW(hwnd, kChromeWindowProp, reinterpret_cast<HANDLE>(window));
    if (::GetPropW(hwnd, kChromeWndProcProp)) {
        return true;
    }

    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ChromeWindowProc));
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        ::RemovePropW(hwnd, kChromeWindowProp);
        return false;
    }

    if (!::SetPropW(hwnd, kChromeWndProcProp, reinterpret_cast<HANDLE>(previous))) {
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, previous);
        ::RemovePropW(hwnd, kChromeWindowProp);
        return false;
    }
    return true;
}

// ShouldExposeOnTaskbar identifies the main app window without changing dialog
// ownership. Input is an FLTK window; output is true only for the maximizable
// custom-chrome window used as the primary application surface.
bool ShouldExposeOnTaskbar(Fl_Window* window) {
    KTitleBar* bar = FindInstalledTitleBar(window);
    return bar && bar->showMaximize() && window && window->parent() == nullptr;
}

// ConfigureNativeAppWindow makes the primary borderless window a normal shell
// app window. Inputs are the FLTK owner and HWND; output is none.
void ConfigureNativeAppWindow(Fl_Window* window, HWND hwnd) {
    if (!ShouldExposeOnTaskbar(window) || !hwnd) {
        return;
    }

    KEnsureAppUserModelID();

    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR desiredStyle = style | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
    if (desiredStyle != style) {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, desiredStyle);
    }

    LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR desiredExStyle = (exStyle & ~(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) | WS_EX_APPWINDOW;
    if (desiredExStyle != exStyle) {
        ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, desiredExStyle);
    }

    if (::GetWindow(hwnd, GW_OWNER)) {
        ::SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
    }

    ::SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
}

KTitleBar::KTitleBar(int x, int y, int w, int h, const char* title, KTitleBarStyle style, bool showMaximize)
    : Fl_Group(x, y, w, h, title),
      style_(style),
      show_maximize_(showMaximize),
      dragging_(false),
      drag_started_from_normal_(false),
      restore_valid_(false),
      chrome_state_(ChromeState::Normal),
      drag_offset_x_(0),
      drag_offset_y_(0),
      drag_start_x_(0),
      drag_start_y_(0),
      drag_start_w_(0),
      drag_start_h_(0),
      restore_x_(0),
      restore_y_(0),
      restore_w_(0),
      restore_h_(0),
      hover_button_(Button::None),
      pressed_button_(Button::None),
      title_(title ? title : "KswordFrame3.0"),
      icon_image_(),
      icon_scaled_(),
      logo_image_(),
      logo_scaled_() {
    box(FL_NO_BOX);
    color(KThemeManager::instance().theme().windowBg);
}

KTitleBar::~KTitleBar() = default;

void KTitleBar::draw() {
    drawBackground();
    drawBrand();

    drawButton(Button::Minimize);
    if (show_maximize_) {
        drawButton(Button::Maximize);
    }
    drawButton(Button::Close);

    const KTheme& theme = KThemeManager::instance().theme();
    fl_color(theme.border);
    fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1);
}

int KTitleBar::handle(int event) {
    const int event_x = Fl::event_x();
    const int event_y = Fl::event_y();

    if (event == FL_ENTER || event == FL_MOVE) {
        Button next = hitButton(event_x, event_y);
        if (next != hover_button_) {
            hover_button_ = next;
            redraw();
        }
        return 1;
    }

    if (event == FL_LEAVE) {
        const bool changed = hover_button_ != Button::None || pressed_button_ != Button::None;
        hover_button_ = Button::None;
        pressed_button_ = Button::None;
        if (changed) {
            redraw();
        }
        return 1;
    }

    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        const Button hit = hitButton(event_x, event_y);
        if (hit != Button::None) {
            pressed_button_ = hit;
            redraw();
            return 1;
        }

        if (inDraggableArea(event_x, event_y)) {
            if (show_maximize_ && Fl::event_clicks() > 0) {
                toggleMaximize();
                return 1;
            }
            beginMoveDrag();
            return 1;
        }
    }

    if (event == FL_DRAG && dragging_) {
        moveWindowForDrag();
        return 1;
    }

    if (event == FL_DRAG && pressed_button_ != Button::None) {
        Button next = hitButton(event_x, event_y);
        if (next != hover_button_) {
            hover_button_ = next;
            redraw();
        }
        return 1;
    }

    if (event == FL_RELEASE) {
        const bool wasDragging = dragging_;
        const Button released = pressed_button_;
        pressed_button_ = Button::None;
        if (wasDragging && released == Button::None) {
            // finishMoveDrag() needs dragging_ to remain true long enough to
            // evaluate snap/maximize targets; it clears the flag before return.
            finishMoveDrag();
        }
        else {
            dragging_ = false;
        }
        if (released != Button::None && released == hitButton(event_x, event_y)) {
            triggerButton(released);
        }
        if (released != Button::None || wasDragging) {
            redraw();
        }
        return 1;
    }

    return Fl_Group::handle(event);
}

void KTitleBar::setStyle(KTitleBarStyle style) {
    if (style_ == style) {
        return;
    }
    style_ = style;
    redraw();
}

KTitleBarStyle KTitleBar::style() const {
    return style_;
}

void KTitleBar::setShowMaximize(bool showMaximize) {
    if (show_maximize_ == showMaximize) {
        return;
    }
    show_maximize_ = showMaximize;
    redraw();
}

bool KTitleBar::showMaximize() const {
    return show_maximize_;
}

void KTitleBar::syncFromNativeWindow(int ownerWidth, bool maximized) {
    bool changed = false;
    const int safeWidth = std::max(1, ownerWidth);
    if (safeWidth != w() || x() != 0 || y() != 0 || h() != kTitleBarHeight) {
        // WM_SIZE is the only native path that changes title-bar geometry; keep
        // repaint local to this widget so child controls retain FLTK repainting.
        Fl_Group::resize(0, 0, safeWidth, kTitleBarHeight);
        changed = true;
    }

    const ChromeState nextState = maximized ? ChromeState::Maximized : ChromeState::Normal;
    if (!dragging_ && chrome_state_ != nextState) {
        chrome_state_ = nextState;
        changed = true;
    }

    if (changed) {
        redraw();
    }
}

void KTitleBar::ensureImages() {
    if (!icon_image_) {
        std::string iconPath = ExtractResourceToTemp(IDR_KSWORD_APP_ICON_ICO, "app.ico");
        if (iconPath.empty()) {
            iconPath = ResolveProjectAsset(KSWORD_APP_ICON_FILE);
        }
        if (!iconPath.empty()) {
            Fl_ICO_Image* rawIcon = new Fl_ICO_Image(iconPath.c_str());
            if (rawIcon->fail()) {
                delete rawIcon;
            }
            else {
                icon_image_.reset(rawIcon);
                icon_scaled_.reset(icon_image_->copy(kCaptionIconSize, kCaptionIconSize));
            }
        }
    }

    if (!logo_image_) {
        std::string logoPath = ExtractResourceToTemp(IDR_KSWORD_APP_LOGO_PNG, "app_logo.png");
        if (logoPath.empty()) {
            logoPath = ResolveProjectAsset(KSWORD_APP_LOGO_FILE);
        }
        if (!logoPath.empty()) {
            Fl_PNG_Image* rawLogo = new Fl_PNG_Image(logoPath.c_str());
            if (rawLogo->fail()) {
                delete rawLogo;
            }
            else {
                logo_image_.reset(rawLogo);
                const int targetWidth = std::max(120, std::min(260, rawLogo->data_w() * kLogoTargetHeight / std::max(1, rawLogo->data_h())));
                logo_scaled_.reset(logo_image_->copy(targetWidth, kLogoTargetHeight));
            }
        }
    }
}

void KTitleBar::drawBackground() {
    const KTheme& theme = KThemeManager::instance().theme();
    const Fl_Color base = theme.windowBg;
    const Fl_Color fill = theme.primary;

    fl_color(base);
    fl_rectf(x(), y(), w(), h());

    if (style_ == KTitleBarStyle::Solid) {
        fl_color(fill);
        fl_rectf(x(), y(), w(), h());
        return;
    }

    if (style_ == KTitleBarStyle::Fade) {
        const int step = 4;
        for (int offset = 0; offset < w(); offset += step) {
            const double progress = static_cast<double>(offset) / std::max(1, w() - 1);
            const double opacity = std::max(0.0, 0.92 - progress * 0.86);
            fl_color(BlendColor(fill, base, opacity));
            fl_rectf(x() + offset, y(), std::min(step, w() - offset), h());
        }
        return;
    }

    if (style_ == KTitleBarStyle::Trapezoid) {
        const int brandWidth = std::min(360, std::max(240, w() / 3));
        const int slant = 34;
        fl_color(BlendColor(fill, base, 0.92));
        fl_begin_polygon();
        fl_vertex(x(), y());
        fl_vertex(x() + brandWidth, y());
        fl_vertex(x() + brandWidth - slant, y() + h());
        fl_vertex(x(), y() + h());
        fl_end_polygon();
    }
}

void KTitleBar::drawBrand() {
    ensureImages();

    const int iconX = x() + 14;
    const int iconY = y() + (h() - kCaptionIconSize) / 2;
    if (icon_scaled_) {
        icon_scaled_->draw(iconX, iconY);
    }
    else {
        drawFallbackIcon(iconX, iconY, kCaptionIconSize);
    }

    const int logoX = iconX + kCaptionIconSize + 12;
    if (logo_scaled_) {
        const int logoY = y() + (h() - logo_scaled_->h()) / 2;
        logo_scaled_->draw(logoX, logoY);
        return;
    }

    const KTheme& theme = KThemeManager::instance().theme();
    const bool onAccent = style_ == KTitleBarStyle::Solid || style_ == KTitleBarStyle::Trapezoid;
    fl_color(onAccent ? FL_WHITE : theme.text);
    fl_font(FL_HELVETICA_BOLD, 14);
    fl_draw(title_.c_str(), logoX, y(), std::max(120, w() / 3), h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

void KTitleBar::drawButton(Button button) {
    if (button == Button::None || (button == Button::Maximize && !show_maximize_)) {
        return;
    }

    const Rect rect = buttonRect(button);
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    const KTheme& theme = KThemeManager::instance().theme();
    const bool hovered = hover_button_ == button;
    const bool pressed = pressed_button_ == button;
    const bool accentText = style_ == KTitleBarStyle::Solid;

    if (hovered || pressed) {
        Fl_Color buttonFill = BlendColor(theme.primary, theme.windowBg, pressed ? 0.24 : 0.14);
        if (button == Button::Close) {
            buttonFill = pressed ? BlendColor(theme.danger, FL_BLACK, 0.92) : theme.danger;
        }
        fl_color(buttonFill);
        fl_rectf(rect.x, rect.y, rect.w, rect.h);
        fl_color(BlendColor(buttonFill, FL_BLACK, pressed ? 0.18 : 0.08));
        fl_line(rect.x, rect.y + rect.h - 1, rect.x + rect.w - 1, rect.y + rect.h - 1);
    }

    Fl_Color iconColor = accentText ? FL_WHITE : theme.text;
    if (button == Button::Close && hovered) {
        iconColor = FL_WHITE;
    }

    const int pressedOffset = pressed ? 1 : 0;
    const int cx = rect.x + rect.w / 2 + pressedOffset;
    const int cy = rect.y + rect.h / 2 + pressedOffset;
    SetCaptionLineStyle(iconColor, 2);

    if (button == Button::Minimize) {
        fl_line(cx - 6, cy + 5, cx + 6, cy + 5);
    }
    else if (button == Button::Maximize) {
        if (chrome_state_ == ChromeState::Maximized) {
            fl_rect(cx - 5, cy - 3, 10, 8);
            fl_line(cx - 2, cy - 6, cx + 7, cy - 6);
            fl_line(cx + 7, cy - 6, cx + 7, cy + 1);
        }
        else {
            fl_rect(cx - 6, cy - 6, 12, 12);
        }
    }
    else if (button == Button::Close) {
        fl_line(cx - 6, cy - 6, cx + 6, cy + 6);
        fl_line(cx + 6, cy - 6, cx - 6, cy + 6);
    }

    fl_line_style(0);
}

KTitleBar::Rect KTitleBar::buttonRect(Button button) const {
    const int right = x() + w();
    const int top = y();
    const int height = h();

    if (button == Button::Close) {
        return { right - kCaptionButtonWidth, top, kCaptionButtonWidth, height };
    }
    if (button == Button::Maximize && show_maximize_) {
        return { right - kCaptionButtonWidth * 2, top, kCaptionButtonWidth, height };
    }
    if (button == Button::Minimize) {
        const int slot = show_maximize_ ? 3 : 2;
        return { right - kCaptionButtonWidth * slot, top, kCaptionButtonWidth, height };
    }
    return { 0, 0, 0, 0 };
}

KTitleBar::Button KTitleBar::hitButton(int px, int py) const {
    if (PointInRect(px, py, buttonRect(Button::Close))) {
        return Button::Close;
    }
    if (show_maximize_ && PointInRect(px, py, buttonRect(Button::Maximize))) {
        return Button::Maximize;
    }
    if (PointInRect(px, py, buttonRect(Button::Minimize))) {
        return Button::Minimize;
    }
    return Button::None;
}

void KTitleBar::triggerButton(Button button) {
    if (button == Button::Minimize) {
        minimizeWindow();
    }
    else if (button == Button::Maximize) {
        toggleMaximize();
    }
    else if (button == Button::Close) {
        closeWindow();
    }
}

void KTitleBar::beginMoveDrag() {
    Fl_Window* owner = window();
    if (!owner) {
        return;
    }

    // Capture the starting bounds before the first move so Snap restore uses
    // the pre-drag normal geometry instead of the edge-aligned final geometry.
    dragging_ = true;
    drag_started_from_normal_ = chrome_state_ == ChromeState::Normal;
    drag_start_x_ = owner->x();
    drag_start_y_ = owner->y();
    drag_start_w_ = owner->w();
    drag_start_h_ = owner->h();
    drag_offset_x_ = Fl::event_x_root() - owner->x();
    drag_offset_y_ = Fl::event_y_root() - owner->y();
}

void KTitleBar::moveWindowForDrag() {
    Fl_Window* owner = window();
    if (!owner) {
        return;
    }

    // A maximized or snapped borderless window restores on first drag movement,
    // then the normal move path below keeps the restored window under the cursor.
    if (chrome_state_ != ChromeState::Normal) {
        restoreForInteractiveDrag();
    }

    const int nextX = Fl::event_x_root() - drag_offset_x_;
    const int nextY = Fl::event_y_root() - drag_offset_y_;
    applyWindowBounds({ nextX, nextY, owner->w(), owner->h() });
}

void KTitleBar::finishMoveDrag() {
    if (!dragging_) {
        return;
    }

    Fl_Window* owner = window();
    if (!owner) {
        dragging_ = false;
        return;
    }

    if (!show_maximize_) {
        dragging_ = false;
        return;
    }

    KTitleBar::Rect work = { 0, 0, 0, 0 };
    if (!WorkAreaForRootPoint(Fl::event_x_root(), Fl::event_y_root(), work)) {
        dragging_ = false;
        return;
    }

    // Match the basic Windows edge gestures on the monitor currently under the
    // cursor: top maximizes, left/right snap to the corresponding half.
    const EdgeSnap snap = SnapForCursor(work, Fl::event_x_root(), Fl::event_y_root());
    if (snap == EdgeSnap::Top) {
        maximizeWindow();
    }
    else if (snap == EdgeSnap::Left) {
        snapWindow(ChromeState::SnappedLeft);
    }
    else if (snap == EdgeSnap::Right) {
        snapWindow(ChromeState::SnappedRight);
    }

    dragging_ = false;
}

void KTitleBar::minimizeWindow() {
    Fl_Window* owner = window();
    if (!owner) {
        return;
    }
    if (ChromeShowWindow(owner, SW_MINIMIZE)) {
        return;
    }
    owner->iconize();
}

void KTitleBar::toggleMaximize() {
    if (!show_maximize_) {
        return;
    }
    if (chrome_state_ == ChromeState::Maximized) {
        restoreWindow();
    }
    else {
        maximizeWindow();
    }
}

void KTitleBar::maximizeWindow() {
    if (!show_maximize_) {
        return;
    }

    Fl_Window* owner = window();
    if (!owner) {
        return;
    }

    KTitleBar::Rect work = { 0, 0, 0, 0 };
    if (!WorkAreaForWindow(owner, work)) {
        return;
    }

    storeRestoreGeometry();
    applyWindowBounds(work);
    chrome_state_ = ChromeState::Maximized;
    redraw();
}

void KTitleBar::restoreWindow() {
    Fl_Window* owner = window();
    if (!owner) {
        return;
    }

    KTitleBar::Rect bounds = { restore_x_, restore_y_, restore_w_, restore_h_ };
    KTitleBar::Rect work = { 0, 0, 0, 0 };
    if (!restore_valid_ && WorkAreaForWindow(owner, work)) {
        bounds.w = std::max(1, work.w * 3 / 4);
        bounds.h = std::max(1, work.h * 3 / 4);
        bounds.x = work.x + (work.w - bounds.w) / 2;
        bounds.y = work.y + (work.h - bounds.h) / 2;
    }
    else if (!restore_valid_) {
        return;
    }

    if (WorkAreaForWindow(owner, work)) {
        const int minWidth = std::min(kMinimumRestoreWidth, std::max(1, work.w));
        const int minHeight = std::min(kMinimumRestoreHeight, std::max(1, work.h));
        bounds.w = ClampInt(bounds.w, minWidth, std::max(1, work.w));
        bounds.h = ClampInt(bounds.h, minHeight, std::max(1, work.h));
        bounds.x = ClampInt(bounds.x, work.x, work.x + std::max(0, work.w - bounds.w));
        bounds.y = ClampInt(bounds.y, work.y, work.y + std::max(0, work.h - bounds.h));
    }

    applyWindowBounds(bounds);
    chrome_state_ = ChromeState::Normal;
    redraw();
}

void KTitleBar::snapWindow(ChromeState snapState) {
    if (!show_maximize_) {
        return;
    }
    if (snapState != ChromeState::SnappedLeft && snapState != ChromeState::SnappedRight) {
        return;
    }
    Fl_Window* owner = window();
    if (!owner) {
        return;
    }

    KTitleBar::Rect work = { 0, 0, 0, 0 };
    if (!WorkAreaForWindow(owner, work)) {
        return;
    }

    storeRestoreGeometry();
    const int leftWidth = std::max(1, work.w / 2);
    if (snapState == ChromeState::SnappedLeft) {
        applyWindowBounds({ work.x, work.y, leftWidth, work.h });
    }
    else {
        applyWindowBounds({ work.x + leftWidth, work.y, std::max(1, work.w - leftWidth), work.h });
    }
    chrome_state_ = snapState;
    redraw();
}

void KTitleBar::restoreForInteractiveDrag() {
    Fl_Window* owner = window();
    if (!owner) {
        return;
    }

    if (!restore_valid_) {
        KTitleBar::Rect work = { 0, 0, 0, 0 };
        if (WorkAreaForWindow(owner, work)) {
            restore_w_ = std::max(1, work.w * 3 / 4);
            restore_h_ = std::max(1, work.h * 3 / 4);
            restore_x_ = work.x + (work.w - restore_w_) / 2;
            restore_y_ = work.y + (work.h - restore_h_) / 2;
        }
        else {
            restore_x_ = owner->x();
            restore_y_ = owner->y();
            restore_w_ = std::max(kMinimumRestoreWidth, owner->w() * 2 / 3);
            restore_h_ = std::max(kMinimumRestoreHeight, owner->h() * 2 / 3);
        }
        restore_valid_ = true;
    }

    const int rootX = Fl::event_x_root();
    const int rootY = Fl::event_y_root();
    const double rawRatio = static_cast<double>(rootX - owner->x()) / std::max(1, owner->w());
    const double ratio = std::max(0.15, std::min(0.85, rawRatio));
    const int localTitleY = ClampInt(Fl::event_y() - y(), 8, std::max(8, kTitleBarHeight - 8));
    const int nextX = rootX - static_cast<int>(restore_w_ * ratio);
    const int nextY = rootY - localTitleY;

    // Update drag offsets after restore so the same FL_DRAG event can continue
    // through the normal movement path without a visible jump.
    applyWindowBounds({ nextX, nextY, restore_w_, restore_h_ });
    chrome_state_ = ChromeState::Normal;
    drag_offset_x_ = rootX - nextX;
    drag_offset_y_ = rootY - nextY;
    redraw();
}

void KTitleBar::storeRestoreGeometry() {
    Fl_Window* owner = window();
    if (!owner) {
        return;
    }

    if (chrome_state_ == ChromeState::Normal && dragging_ && drag_started_from_normal_ && drag_start_w_ > 0 && drag_start_h_ > 0) {
        restore_x_ = drag_start_x_;
        restore_y_ = drag_start_y_;
        restore_w_ = drag_start_w_;
        restore_h_ = drag_start_h_;
    }
    else if (chrome_state_ == ChromeState::Normal || !restore_valid_) {
        restore_x_ = owner->x();
        restore_y_ = owner->y();
        restore_w_ = owner->w();
        restore_h_ = owner->h();
    }

    restore_valid_ = restore_w_ > 0 && restore_h_ > 0;
}

void KTitleBar::applyWindowBounds(const Rect& bounds) {
    Fl_Window* owner = window();
    if (!owner || bounds.w <= 0 || bounds.h <= 0) {
        return;
    }

    const bool sizeChanged = owner->w() != bounds.w || owner->h() != bounds.h;
    ChromeSetWindowBounds(owner, bounds, sizeChanged);
    if (sizeChanged) {
        // Movement-only drags must not invalidate the whole window. FLTK keeps
        // client controls painted; only actual size changes adjust this bar and
        // refresh the layout baseline.
        Fl_Group::resize(0, 0, bounds.w, kTitleBarHeight);
        owner->init_sizes();
        redraw();
    }
}

void KTitleBar::closeWindow() {
    Fl_Window* owner = window();
    if (owner) {
        owner->hide();
    }
}

void KTitleBar::drawFallbackIcon(int px, int py, int size) {
    const KTheme& theme = KThemeManager::instance().theme();
    fl_color(theme.primary);
    fl_rectf(px, py, size, size);
    fl_color(FL_WHITE);
    fl_font(FL_HELVETICA_BOLD, std::max(12, size - 8));
    fl_draw("K", px, py, size, size, FL_ALIGN_CENTER);
}

bool KTitleBar::inDraggableArea(int px, int py) const {
    if (py < y() || py >= y() + h()) {
        return false;
    }
    const Rect firstButton = buttonRect(Button::Minimize);
    const int dragRight = firstButton.w > 0 ? firstButton.x : x() + w();
    return px >= x() && px < dragRight;
}

int KTitleBarHeight() {
    return kTitleBarHeight;
}

bool KInstallTitleBar(Fl_Window* window, KTitleBarStyle style, bool showMaximize) {
    if (!window) {
        return false;
    }

    if (showMaximize) {
        // Set the shell identity before the main HWND is shown so taskbar
        // grouping is stable even though the icon/style is applied after show().
        KEnsureAppUserModelID();
    }

    KTitleBar* existing = FindInstalledTitleBar(window);
    if (existing) {
        existing->setStyle(style);
        existing->setShowMaximize(showMaximize);
        return true;
    }

    std::vector<Fl_Widget*> children;
    children.reserve(window->children());
    for (int i = 0; i < window->children(); ++i) {
        children.push_back(window->child(i));
    }

    window->border(0);
    const int oldWidth = window->w();
    const int oldHeight = window->h();
    window->size(oldWidth, oldHeight + kTitleBarHeight);

    for (Fl_Widget* child : children) {
        if (!child) {
            continue;
        }
        child->resize(child->x(), child->y() + kTitleBarHeight, child->w(), child->h());
    }

    Fl_Group* previous = Fl_Group::current();
    Fl_Group::current(nullptr);
    KTitleBar* bar = new KTitleBar(0, 0, oldWidth, kTitleBarHeight, window->label(), style, showMaximize);
    Fl_Group::current(previous);
    window->add(bar);
    window->init_sizes();
    KThemeManager::instance().ApplyTo(bar);
    window->redraw();
    return true;
}

void KApplyWindowIcon(Fl_Window* window) {
    HWND hwnd = WindowHandle(window);
    if (!hwnd) {
        return;
    }

    InstallChromeSubclass(window, hwnd);
    ConfigureNativeAppWindow(window, hwnd);

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    HICON bigIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_KSWORD_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));
    HICON smallIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_KSWORD_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    if (bigIcon) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    }
    if (smallIcon) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }
}
