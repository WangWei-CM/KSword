#include "StartupSplash.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Gdiplus.lib")

namespace
{
    constexpr wchar_t kWindowClass[] = L"KswordFrameworkSplashWindow";
    constexpr int kBaseWidth = 640;
    constexpr UINT kShowMessage = WM_APP + 1;
    constexpr UINT kHideMessage = WM_APP + 2;
    constexpr UINT kRedrawMessage = WM_APP + 3;
    constexpr UINT kResetAnimationMessage = WM_APP + 4;

    std::wstring utf8ToWide(const std::string& text)
    {
        if (text.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.c_str(), -1, nullptr, 0);
        if (count <= 0) return {};
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1,
            result.data(), count) == 0) return {};
        result.pop_back();
        return result;
    }

    std::wstring defaultImagePath()
    {
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;)
        {
            const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (copied == 0) return {};
            if (copied < buffer.size() - 1)
            {
                std::wstring path(buffer.data(), copied);
                const size_t slash = path.find_last_of(L"\\/");
                if (slash == std::wstring::npos) return {};
                return path.substr(0, slash + 1) + L"Style\\KswordHome_2.png";
            }
            if (buffer.size() >= 32768) return {};
            buffer.resize(buffer.size() * 2);
        }
    }

    void enableSplashThreadDpiAwareness()
    {
        // The splash precedes QApplication: set physical coordinates only on
        // its UI thread, leaving Qt's process/thread DPI setup to Qt.
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        using SetThreadDpiAwarenessContextFn = HANDLE(WINAPI*)(HANDLE);
        const auto setDpiContext = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (setDpiContext != nullptr)
        {
            if (setDpiContext(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4))) == nullptr)
                setDpiContext(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-3)));
        }
    }

    UINT windowDpi(HWND window)
    {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (window != nullptr && getDpi != nullptr)
        {
            const UINT dpi = getDpi(window);
            if (dpi != 0) return dpi;
        }
        HDC screen = GetDC(nullptr);
        const UINT dpi = screen != nullptr ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
        if (screen != nullptr) ReleaseDC(nullptr, screen);
        return std::max(96U, dpi);
    }

    void fillRoundedRectangle(Gdiplus::Graphics& graphics, const Gdiplus::Brush& brush,
        float x, float y, float width, float height)
    {
        if (width <= 0.0f || height <= 0.0f) return;
        const float diameter = std::min(width, height);
        Gdiplus::GraphicsPath outline;
        outline.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
        outline.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
        outline.AddArc(x + width - diameter, y + height - diameter,
            diameter, diameter, 0.0f, 90.0f);
        outline.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
        outline.CloseFigure();
        graphics.FillPath(&brush, &outline);
    }

    // Own selected DIBs for the splash UI thread; pixels stay premultiplied.
    struct DibSurface final
    {
        ~DibSurface() { reset(); }
        DibSurface() = default;
        DibSurface(const DibSurface&) = delete;
        DibSurface& operator=(const DibSurface&) = delete;

        void reset()
        {
            if (dc != nullptr && oldBitmap != nullptr) SelectObject(dc, oldBitmap);
            if (bitmap != nullptr) DeleteObject(bitmap);
            if (dc != nullptr) DeleteDC(dc);
            dc = nullptr; bitmap = nullptr; oldBitmap = nullptr; pixels = nullptr;
            width = height = 0;
        }
        bool ensure(int requestedWidth, int requestedHeight)
        {
            if (dc != nullptr && width == requestedWidth && height == requestedHeight) return true;
            HDC nextDc = CreateCompatibleDC(nullptr);
            if (nextDc == nullptr) return false;
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = requestedWidth;
            info.bmiHeader.biHeight = -requestedHeight;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* nextPixels = nullptr;
            HBITMAP nextBitmap = CreateDIBSection(nextDc, &info, DIB_RGB_COLORS, &nextPixels, nullptr, 0);
            if (nextBitmap == nullptr) { DeleteDC(nextDc); return false; }
            HGDIOBJ nextOldBitmap = SelectObject(nextDc, nextBitmap);
            if (nextOldBitmap == nullptr || nextOldBitmap == HGDI_ERROR)
            { DeleteObject(nextBitmap); DeleteDC(nextDc); return false; }
            reset();
            dc = nextDc; bitmap = nextBitmap; oldBitmap = nextOldBitmap; pixels = nextPixels;
            width = requestedWidth; height = requestedHeight;
            return true;
        }
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        void* pixels = nullptr;
        int width = 0, height = 0;
    };

    double cubic(double t, double p1, double p2)
    {
        const double inverse = 1.0 - t;
        return 3.0 * inverse * inverse * t * p1 +
            3.0 * inverse * t * t * p2 + t * t * t;
    }

    double winUiTravel(double elapsedFraction)
    {
        // WinUI's progress-bar spline is cubic-bezier(0.4, 0, 0.6, 1).
        double low = 0.0;
        double high = 1.0;
        for (int index = 0; index < 16; ++index)
        {
            const double middle = (low + high) * 0.5;
            if (cubic(middle, 0.4, 0.6) < elapsedFraction) low = middle;
            else high = middle;
        }
        return cubic((low + high) * 0.5, 0.0, 1.0);
    }
}

class kStartupSplash::Impl final
{
public:
    ~Impl()
    {
        const HWND window = m_window.load();
        if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
        if (m_thread.joinable()) m_thread.join();
    }

    void setBackgroundImagePath(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_thread.joinable()) m_imagePath = path;
    }

    void setFooterText(const std::wstring& email, const std::wstring& buildDate,
        const std::wstring& repository)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_email == email && m_buildDate == buildDate && m_repository == repository) return;
            m_email = email;
            m_buildDate = buildDate;
            m_repository = repository;
            ++m_stateRevision;
            ++m_staticRevision;
        }
        requestRedraw();
    }

    void setProgressMode(SplashProgressMode mode)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_mode == mode) return;
            m_mode = mode;
            ++m_stateRevision;
        }
        const HWND window = m_window.load();
        if (window != nullptr) PostMessageW(window, kResetAnimationMessage, 0, 0);
    }

    void setAccentColorRgb(unsigned int rgb)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const unsigned int accent = rgb & 0x00FFFFFFU;
            if (m_accentRgb == accent) return;
            m_accentRgb = accent;
            ++m_stateRevision;
            ++m_staticRevision;
        }
        requestRedraw();
    }

    bool show(const std::string& initialStatusText)
    {
        const std::wstring status = utf8ToWide(initialStatusText);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const std::wstring nextStatus = status.empty() ? L"Ksword" : status;
            if (m_status != nextStatus) ++m_staticRevision;
            m_status = nextStatus;
            m_percent = 0;
            ++m_stateRevision;
        }
        if (!m_thread.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_threadReady = false;
                m_threadSucceeded = false;
            }
            // Rescue launches select the caller's desktop before splash startup.
            m_desktop = GetThreadDesktop(GetCurrentThreadId());
            m_thread = std::thread(&Impl::run, this);
            std::unique_lock<std::mutex> lock(m_stateMutex);
            m_created.wait(lock, [this] { return m_threadReady; });
            if (!m_threadSucceeded)
            {
                lock.unlock();
                m_thread.join();
                return false;
            }
        }
        const HWND window = m_window.load();
        if (window == nullptr) return false;
        SendMessageW(window, kShowMessage, 0, 0);
        return IsWindowVisible(window) != FALSE;
    }

    void hide()
    {
        const HWND window = m_window.load();
        if (window != nullptr) SendMessageW(window, kHideMessage, 0, 0);
    }

    void progress(const std::string& operationName, int percent)
    {
        if (m_window.load() == nullptr) return;
        const std::wstring status = utf8ToWide(operationName);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const int clamped = std::clamp(percent, 0, 100);
            if ((status.empty() || m_status == status) && m_percent == clamped) return;
            if (!status.empty() && m_status != status)
            {
                m_status = status;
                ++m_staticRevision;
            }
            m_percent = clamped;
            ++m_stateRevision;
        }
        requestRedraw();
    }

    bool ready() const { return m_window.load() != nullptr; }

private:
    void signalCreated(bool succeeded)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_threadSucceeded = succeeded;
            m_threadReady = true;
        }
        m_created.notify_one();
    }

    void run()
    {
        if (m_desktop == nullptr || !SetThreadDesktop(m_desktop))
        {
            signalCreated(false);
            return;
        }
        enableSplashThreadDpiAwareness();
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &input, nullptr) != Gdiplus::Ok)
        {
            signalCreated(false);
            return;
        }
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            path = m_imagePath.empty() ? defaultImagePath() : m_imagePath;
        }
        m_image.reset(Gdiplus::Image::FromFile(path.c_str(), FALSE));
        if (!m_image || m_image->GetLastStatus() != Gdiplus::Ok ||
            m_image->GetWidth() == 0 || m_image->GetHeight() == 0)
        {
            m_image.reset();
            Gdiplus::GdiplusShutdown(m_gdiplusToken);
            m_gdiplusToken = 0;
            signalCreated(false);
            return;
        }
        m_imageWidth = static_cast<int>(m_image->GetWidth());
        m_imageHeight = static_cast<int>(m_image->GetHeight());

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kWindowClass;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        const bool registered = RegisterClassExW(&windowClass) != 0 ||
            GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (registered)
        {
            layout();
            const HWND window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                kWindowClass, L"Ksword", WS_POPUP, m_x, m_y, m_width, m_height,
                nullptr, nullptr, GetModuleHandleW(nullptr), this);
            m_window.store(window);
        }
        const HWND window = m_window.load();
        signalCreated(window != nullptr);
        if (window != nullptr)
        {
            MSG message = {};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            m_window.store(nullptr);
        }
        m_frame.reset();
        m_staticSurface.reset();
        m_image.reset();
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }

    void layout()
    {
        const HWND window = m_window.load();
        const HMONITOR monitor = window != nullptr
            ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY)
            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info = {sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) return;
        const int oldX = m_x, oldY = m_y, oldWidth = m_width, oldHeight = m_height;
        const UINT oldDpi = m_dpi;
        m_dpi = windowDpi(window);
        m_scale = static_cast<float>(m_dpi) / 96.0f;
        m_width = MulDiv(kBaseWidth, static_cast<int>(m_dpi), 96);
        m_height = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(m_width) * m_imageHeight / m_imageWidth)));
        m_canvasHeight = m_height / m_scale;
        m_x = info.rcMonitor.left + ((info.rcMonitor.right - info.rcMonitor.left) - m_width) / 2;
        m_y = info.rcMonitor.top + ((info.rcMonitor.bottom - info.rcMonitor.top) - m_height) / 2;
        if (oldX != m_x || oldY != m_y || oldWidth != m_width || oldHeight != m_height || oldDpi != m_dpi)
            ++m_geometryRevision;
        if (window != nullptr)
            SetWindowPos(window, HWND_TOPMOST, m_x, m_y, m_width, m_height,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    void requestRedraw()
    {
        const HWND window = m_window.load();
        if (window != nullptr && !m_redrawQueued.exchange(true))
        {
            if (!PostMessageW(window, kRedrawMessage, 0, 0)) m_redrawQueued.store(false);
        }
    }

    void updateAnimationTimer(bool visible)
    {
        const HWND window = m_window.load();
        BOOL animations = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) animations = TRUE;
        m_animationsEnabled = animations != FALSE;
        UINT refreshRate = 60;
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfoW(monitor, &info))
        {
            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency >= 24 && mode.dmDisplayFrequency <= 360)
                refreshRate = mode.dmDisplayFrequency;
        }
        // Win32 timers have a minimum interval; animation position uses elapsed
        // time, so a late/coalesced tick never slows the progress cycle.
        const UINT interval = std::max<UINT>(USER_TIMER_MINIMUM, (1000U + refreshRate / 2U) / refreshRate);
        SplashProgressMode mode;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            mode = m_mode;
        }
        if (window != nullptr)
        {
            if (visible && m_animationsEnabled && mode == SplashProgressMode::Indeterminate)
                SetTimer(window, 1, interval, nullptr);
            else
                KillTimer(window, 1);
        }
    }

    bool redraw(bool force = false)
    {
        const HWND window = m_window.load();
        return window != nullptr && paint(nullptr, force);
    }

    Gdiplus::Color backgroundTextColor(const void* pixels, const Gdiplus::RectF& area) const
    {
        // Sample only the resized background, before text or progress is drawn.
        // Any transparent sample uses white: the desktop behind it is unknown.
        const auto* bytes = static_cast<const BYTE*>(pixels);
        unsigned int brightness = 0;
        constexpr int columns = 5;
        constexpr int rows = 3;
        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                const int x = std::clamp(static_cast<int>(
                    (area.X + area.Width * (column + 0.5f) / columns) * m_scale), 0, m_width - 1);
                const int y = std::clamp(static_cast<int>(
                    (area.Y + area.Height * (row + 0.5f) / rows) * m_scale), 0, m_height - 1);
                const BYTE* pixel = bytes + (static_cast<size_t>(y) * m_width + x) * 4;
                if (pixel[3] < 255) return Gdiplus::Color(255, 255, 255, 255);
                brightness += 299U * pixel[2] + 587U * pixel[1] + 114U * pixel[0];
            }
        }
        return brightness >= 150U * 1000U * columns * rows
            ? Gdiplus::Color(255, 0, 0, 0) : Gdiplus::Color(255, 255, 255, 255);
    }

    bool paint(HDC target, bool force = false)
    {
        std::wstring status;
        std::wstring email;
        std::wstring buildDate;
        std::wstring repository;
        SplashProgressMode mode;
        unsigned int accentRgb;
        int percent;
        std::uint64_t revision, staticRevision;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            status = m_status;
            email = m_email;
            buildDate = m_buildDate;
            repository = m_repository;
            mode = m_mode;
            accentRgb = m_accentRgb;
            percent = m_percent;
            revision = m_stateRevision;
            staticRevision = m_staticRevision;
        }
        // Layered windows retain their submitted pixels. Repeated state messages
        // and WM_PAINT do not require resubmitting an unchanged determinate frame.
        if (!force && target == nullptr && (mode == SplashProgressMode::Determinate || !m_animationsEnabled) &&
            revision == m_paintedRevision && m_geometryRevision == m_paintedGeometryRevision)
            return true;

        const bool resized = m_staticSurface.width != m_width || m_staticSurface.height != m_height ||
            m_frame.width != m_width || m_frame.height != m_height;
        if (!m_staticSurface.ensure(m_width, m_height) || !m_frame.ensure(m_width, m_height)) return false;
        const bool rebuildStatic = resized || staticRevision != m_cachedStaticRevision ||
            m_geometryRevision != m_cachedStaticGeometryRevision;
        constexpr float trackX = 260.0f;
        constexpr float trackWidth = 354.0f;
        const float trackY = m_canvasHeight - 32.0f;
        constexpr float trackHeight = 8.0f;
        if (rebuildStatic)
        {
            // UpdateLayeredWindow requires premultiplied RGB and per-pixel alpha.
            // Drawing into this bitmap preserves alpha for both PNG and overlays.
            Gdiplus::Bitmap surface(m_width, m_height, m_width * 4,
                PixelFormat32bppPARGB, static_cast<BYTE*>(m_staticSurface.pixels));
            Gdiplus::Graphics graphics(&surface);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            graphics.ScaleTransform(m_scale, m_scale);

            // Scale directly from source pixels at the monitor's resolution.
            // No 640px intermediate bitmap is enlarged at high DPI.
            graphics.DrawImage(m_image.get(), Gdiplus::RectF(0.0f, 0.0f,
                static_cast<float>(kBaseWidth), m_canvasHeight),
                0.0f, 0.0f, static_cast<float>(m_imageWidth),
                static_cast<float>(m_imageHeight), Gdiplus::UnitPixel);

            graphics.Flush(Gdiplus::FlushIntentionSync);
            const Gdiplus::Color footerColor = backgroundTextColor(m_staticSurface.pixels,
                Gdiplus::RectF(28.0f, m_canvasHeight - 63.0f, 188.0f, 60.0f));
            const Gdiplus::Color statusColor = backgroundTextColor(m_staticSurface.pixels,
                Gdiplus::RectF(260.0f, m_canvasHeight - 60.0f, 354.0f, 24.0f));
            Gdiplus::SolidBrush footerForeground(footerColor);
            Gdiplus::SolidBrush statusForeground(statusColor);
            Gdiplus::Font footerFont(L"Microsoft YaHei UI", 14.0f,
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::StringFormat singleLine;
            singleLine.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            singleLine.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            const float footerX = 28.0f;
            constexpr float footerLineHeight = 20.0f;
            float footerY = m_canvasHeight - 63.0f;
            const std::wstring* footerLines[] = { &email, &buildDate, &repository };
            for (const std::wstring* line : footerLines)
            {
                graphics.DrawString(line->c_str(), -1, &footerFont,
                    Gdiplus::RectF(footerX, footerY, 188.0f, footerLineHeight),
                    &singleLine, &footerForeground);
                footerY += footerLineHeight;
            }

            // Inset the status/progress area from the divider, keeping the right margin.
            float statusSize = 12.0f;
            Gdiplus::RectF bounds;
            for (; statusSize > 9.0f; statusSize -= 1.0f)
            {
                Gdiplus::Font candidate(L"Microsoft YaHei UI", statusSize,
                    Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                graphics.MeasureString(status.c_str(), -1, &candidate,
                    Gdiplus::PointF(0.0f, 0.0f), &bounds);
                if (bounds.Width <= trackWidth) break;
            }
            Gdiplus::Font statusFont(L"Microsoft YaHei UI", statusSize,
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            graphics.DrawString(status.c_str(), -1, &statusFont,
                Gdiplus::RectF(trackX, m_canvasHeight - 60.0f, trackWidth, 24.0f),
                &singleLine, &statusForeground);

            Gdiplus::SolidBrush track(Gdiplus::Color(130,
                statusColor.GetR(), statusColor.GetG(), statusColor.GetB()));
            fillRoundedRectangle(graphics, track, trackX, trackY, trackWidth, trackHeight);
            graphics.Flush(Gdiplus::FlushIntentionSync);
            m_cachedStaticRevision = staticRevision;
            m_cachedStaticGeometryRevision = m_geometryRevision;
        }
        const size_t stride = static_cast<size_t>(m_width) * 4;
        if (rebuildStatic)
            std::memcpy(m_frame.pixels, m_staticSurface.pixels, stride * m_height);
        else
        {
            // Restore only the progress strip from the cached premultiplied
            // background. This replaces alpha as well, avoiding animation trails.
            const int top = std::clamp(static_cast<int>(std::floor(trackY * m_scale)) - 2, 0, m_height);
            const int bottom = std::clamp(static_cast<int>(std::ceil((trackY + trackHeight) * m_scale)) + 2, top, m_height);
            const size_t offset = static_cast<size_t>(top) * stride;
            std::memcpy(static_cast<BYTE*>(m_frame.pixels) + offset,
                static_cast<const BYTE*>(m_staticSurface.pixels) + offset,
                static_cast<size_t>(bottom - top) * stride);
        }
        {
            Gdiplus::Bitmap surface(m_width, m_height, m_width * 4,
                PixelFormat32bppPARGB, static_cast<BYTE*>(m_frame.pixels));
            Gdiplus::Graphics graphics(&surface);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.ScaleTransform(m_scale, m_scale);
            const int accentRed = static_cast<int>((accentRgb >> 16) & 0xFFU);
            const int accentGreen = static_cast<int>((accentRgb >> 8) & 0xFFU);
            const int accentBlue = static_cast<int>(accentRgb & 0xFFU);
            Gdiplus::SolidBrush indicator(Gdiplus::Color(240,
                static_cast<BYTE>(accentRed), static_cast<BYTE>(accentGreen),
                static_cast<BYTE>(accentBlue)));
            graphics.SetClip(Gdiplus::RectF(trackX, trackY, trackWidth, trackHeight));
            if (mode == SplashProgressMode::Indeterminate)
            {
                constexpr ULONGLONG kCycleMs = 1150;
                constexpr ULONGLONG kTravelMs = 1000;
                const ULONGLONG phase = m_animationsEnabled
                    ? (GetTickCount64() - m_animationStart) % kCycleMs : kTravelMs / 2;
                if (phase < kTravelMs)
                {
                    const float progress = static_cast<float>(winUiTravel(
                        static_cast<double>(phase) / kTravelMs));
                    const float segmentWidth = 30.0f +
                        70.0f * 4.0f * progress * (1.0f - progress);
                    const float segmentX = trackX +
                        (trackWidth - segmentWidth) * progress;
                    const float fadeIn = std::min(1.0f, phase / 90.0f);
                    const float fadeOut = std::min(1.0f, (kTravelMs - phase) / 90.0f);
                    Gdiplus::SolidBrush movingIndicator(Gdiplus::Color(
                        static_cast<BYTE>(240.0f * std::min(fadeIn, fadeOut)),
                        static_cast<BYTE>(accentRed), static_cast<BYTE>(accentGreen),
                        static_cast<BYTE>(accentBlue)));
                    fillRoundedRectangle(graphics, movingIndicator, segmentX, trackY,
                        segmentWidth, trackHeight);
                }
            }
            else if (percent > 0)
            {
                fillRoundedRectangle(graphics, indicator, trackX, trackY,
                    trackWidth * percent / 100.0f, trackHeight);
            }
            graphics.ResetClip();
            graphics.Flush(Gdiplus::FlushIntentionSync);
        }
        // Optional DC output is used by the offscreen rendering fixture.
        if (target != nullptr)
            BitBlt(target, 0, 0, m_width, m_height, m_frame.dc, 0, 0, SRCCOPY);
        const HWND window = m_window.load();
        POINT position = {m_x, m_y};
        POINT source = {};
        SIZE size = {m_width, m_height};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const bool submitted = window != nullptr && UpdateLayeredWindow(window, nullptr,
            &position, &size, m_frame.dc, &source, 0, &blend, ULW_ALPHA) != FALSE;
        if (submitted)
        {
            m_paintedRevision = revision;
            m_paintedGeometryRevision = m_geometryRevision;
        }
        return submitted;
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message)
        {
        case kShowMessage:
            if (self != nullptr)
            {
                self->m_animationStart = GetTickCount64();
                self->layout();
                self->updateAnimationTimer(false);
                if (self->redraw(true))
                {
                    ShowWindow(window, SW_SHOWNOACTIVATE);
                    self->updateAnimationTimer(true);
                }
            }
            return 0;
        case kHideMessage:
            KillTimer(window, 1);
            ShowWindow(window, SW_HIDE);
            return 0;
        case kResetAnimationMessage:
            if (self != nullptr)
            {
                self->m_animationStart = GetTickCount64();
                const bool visible = IsWindowVisible(window) != FALSE;
                self->updateAnimationTimer(visible);
                if (visible) self->redraw();
            }
            return 0;
        case kRedrawMessage:
            if (self != nullptr)
            {
                self->m_redrawQueued.store(false);
                if (IsWindowVisible(window)) self->redraw();
            }
            return 0;
        case WM_TIMER:
            if (wParam == 1 && self != nullptr && IsWindowVisible(window)) self->redraw();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (self != nullptr)
            {
                PAINTSTRUCT paint = {};
                BeginPaint(window, &paint);
                if (IsWindowVisible(window)) self->paint(nullptr);
                EndPaint(window, &paint);
                return 0;
            }
            break;
        case WM_SETTINGCHANGE:
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            if (self != nullptr)
            {
                self->layout();
                const bool visible = IsWindowVisible(window) != FALSE;
                self->updateAnimationTimer(visible);
                if (visible) self->redraw(true);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    std::thread m_thread;
    HDESK m_desktop = nullptr; // Borrowed from caller for UI thread startup.
    std::atomic<HWND> m_window{nullptr};
    std::atomic<bool> m_redrawQueued{false};
    mutable std::mutex m_stateMutex;
    std::condition_variable m_created;
    bool m_threadReady = false;
    bool m_threadSucceeded = false;
    std::wstring m_imagePath;
    std::wstring m_status = L"Ksword";
    std::wstring m_email;
    std::wstring m_buildDate;
    std::wstring m_repository;
    SplashProgressMode m_mode = SplashProgressMode::Determinate;
    unsigned int m_accentRgb = 0x43A0FFU;
    int m_percent = 0;
    std::uint64_t m_stateRevision = 1; // Protected by m_stateMutex.
    std::uint64_t m_staticRevision = 1; // Background/text inputs only, under the same mutex.
    std::uint64_t m_paintedRevision = 0; // Splash UI thread only.
    std::uint64_t m_geometryRevision = 1;
    std::uint64_t m_paintedGeometryRevision = 0;

    std::uint64_t m_cachedStaticRevision = 0;
    std::uint64_t m_cachedStaticGeometryRevision = 0;
    DibSurface m_staticSurface;
    DibSurface m_frame;
    bool m_animationsEnabled = true;
    ULONG_PTR m_gdiplusToken = 0;
    std::unique_ptr<Gdiplus::Image> m_image;
    int m_imageWidth = 0;
    int m_imageHeight = 0;
    float m_canvasHeight = 360.0f;
    int m_width = kBaseWidth;
    int m_height = 360;
    int m_x = 0;
    int m_y = 0;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    ULONGLONG m_animationStart = 0;
};

kStartupSplash::kStartupSplash() : m_impl(std::make_unique<Impl>()) {}
kStartupSplash::~kStartupSplash() = default;
void kStartupSplash::setBackgroundImagePath(const std::wstring& path) { m_impl->setBackgroundImagePath(path); }
void kStartupSplash::setFooterText(const std::wstring& email, const std::wstring& buildDate,
    const std::wstring& repository) { m_impl->setFooterText(email, buildDate, repository); }
void kStartupSplash::setProgressMode(SplashProgressMode mode) { m_impl->setProgressMode(mode); }
void kStartupSplash::setAccentColorRgb(unsigned int rgb) { m_impl->setAccentColorRgb(rgb); }
bool kStartupSplash::show(const std::string& initialStatusText) { return m_impl->show(initialStatusText); }
void kStartupSplash::hide() { m_impl->hide(); }
void kStartupSplash::progress(const std::string& operationName, int percent)
{ m_impl->progress(operationName, percent); }
bool kStartupSplash::ready() const { return m_impl->ready(); }

kStartupSplash kSplash;
