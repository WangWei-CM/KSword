#include "StartupSplash.h"

#include <QtCore/QFile>
#include <QtCore/QString>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gdiplus.h>
#include <objidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Ole32.lib")

namespace
{
    // kFrameworkSplashClassName 作用：
    // - Framework 启动画面的原生窗口类名；
    // - 用于注册和创建 Win32 Layered Window。
    constexpr wchar_t kFrameworkSplashClassName[] = L"KswordFrameworkStartupSplashWindow";

    // kFrameworkSplashLogoResourcePath 作用：
    // - 启动画面 Logo 的 qrc 资源路径；
    // - 启动图直接从程序资源读取，不依赖磁盘目录。
    constexpr auto kFrameworkSplashLogoResourcePath = ":/Image/Resource/Logo/MainLogo.png";

    // clampProgressPercent 作用：
    // - 把进度值约束在 0~100；
    // - 避免进度条宽度出现负值或溢出。
    int clampProgressPercent(const int rawPercentValue)
    {
        return std::clamp(rawPercentValue, 0, 100);
    }

    // utf8ToWideText 作用：
    // - 把 UTF-8 文本转换为 UTF-16 宽字符串；
    // - 供 GDI+ DrawString 绘制中文状态文本。
    // 调用方式：progress 调用时转换业务文案。
    // 入参 utf8Text：UTF-8 编码文本。
    // 返回：UTF-16 宽字符串；失败时返回空字符串。
    std::wstring utf8ToWideText(const std::string& utf8Text)
    {
        if (utf8Text.empty())
        {
            return std::wstring();
        }

        const int requiredLength = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8Text.c_str(),
            -1,
            nullptr,
            0);
        if (requiredLength <= 1)
        {
            return std::wstring();
        }

        std::wstring wideTextBuffer(static_cast<std::size_t>(requiredLength - 1), L'\0');
        const int convertResult = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8Text.c_str(),
            -1,
            wideTextBuffer.data(),
            requiredLength);
        if (convertResult <= 1)
        {
            return std::wstring();
        }
        return wideTextBuffer;
    }

    // querySystemDpiValue 作用：
    // - 获取当前系统 DPI 值；
    // - 新系统优先调用 GetDpiForSystem，旧系统回退 96 DPI。
    // 返回：系统 DPI（>=96）。
    UINT querySystemDpiValue()
    {
        HMODULE user32ModuleHandle = ::GetModuleHandleW(L"user32.dll");
        if (user32ModuleHandle != nullptr)
        {
            using GetDpiForSystemFunction = UINT(WINAPI*)();
            const GetDpiForSystemFunction getDpiForSystemFunction =
                reinterpret_cast<GetDpiForSystemFunction>(
                    ::GetProcAddress(user32ModuleHandle, "GetDpiForSystem"));
            if (getDpiForSystemFunction != nullptr)
            {
                const UINT queriedDpi = getDpiForSystemFunction();
                if (queriedDpi >= 96U)
                {
                    return queriedDpi;
                }
            }
        }
        return 96U;
    }

    // pumpNativeSplashMessages 作用：
    // - 手动泵一次启动窗自己的 Win32 消息循环；
    // - 避免把 Qt 主窗口消息提前派发，打乱首帧与事件时序。
    // 入参 splashWindowHandle：启动窗句柄。
    void pumpNativeSplashMessages(const HWND splashWindowHandle)
    {
        if (splashWindowHandle == nullptr)
        {
            return;
        }

        MSG pendingMessage = {};
        while (::PeekMessageW(&pendingMessage, splashWindowHandle, 0, 0, PM_REMOVE) != FALSE)
        {
            ::TranslateMessage(&pendingMessage);
            ::DispatchMessageW(&pendingMessage);
        }
    }
}

// kStartupSplash::Impl 作用：
// - 承载启动窗口全部 Win32/GDI+ 资源与绘制逻辑；
// - 由外层 kStartupSplash 通过 PImpl 统一调度。
class kStartupSplash::Impl final
{
public:
    // 构造函数作用：
    // - 初始化默认状态文本和绘制参数；
    // - 不做重量级资源创建。
    Impl() = default;

    // 析构函数作用：
    // - 统一回收窗口句柄与 GDI+ 资源；
    // - 避免进程退出时残留对象。
    ~Impl()
    {
        shutdown();
    }

    // showWindow 作用：
    // - 确保初始化成功后显示启动窗口；
    // - 首次显示时同步渲染首帧。
    // 返回：true=显示成功；false=初始化失败。
    bool showWindow()
    {
        if (!initialize())
        {
            return false;
        }
        if (m_windowHandle == nullptr)
        {
            return false;
        }

        recalculateLayoutMetrics();
        renderFrame();
        ::ShowWindow(m_windowHandle, SW_SHOWNOACTIVATE);
        ::UpdateWindow(m_windowHandle);
        pumpNativeSplashMessages(m_windowHandle);
        return true;
    }

    // hideWindow 作用：
    // - 隐藏启动窗口但保留句柄，支持后续再次显示；
    // - 不销毁 GDI+ 与图像资源。
    void hideWindow()
    {
        if (m_windowHandle != nullptr)
        {
            ::ShowWindow(m_windowHandle, SW_HIDE);
            pumpNativeSplashMessages(m_windowHandle);
        }
    }

    // setProgressState 作用：
    // - 更新状态文案与进度百分比；
    // - 立即重绘并泵消息，保证视觉实时反馈。
    // 入参 operationNameUtf8：当前操作名称（UTF-8）。
    // 入参 progressPercent：进度百分比（0~100）。
    void setProgressState(const std::string& operationNameUtf8, const int progressPercent)
    {
        if (m_windowHandle == nullptr || !m_initialized)
        {
            return;
        }

        const std::wstring nextStatusText = utf8ToWideText(operationNameUtf8);
        if (!nextStatusText.empty())
        {
            m_statusText = nextStatusText;
        }
        m_progressPercent = clampProgressPercent(progressPercent);
        renderFrame();
        pumpNativeSplashMessages(m_windowHandle);
    }

    // isReady 作用：
    // - 查询当前实现对象是否已准备好显示。
    // 返回：true=初始化完成且窗口句柄有效。
    bool isReady() const
    {
        return m_initialized && m_windowHandle != nullptr;
    }

private:
    // StreamReleaser 作用：
    // - IStream 智能指针删除器；
    // - 用于安全释放 COM 内存流对象。
    struct StreamReleaser
    {
        void operator()(IStream* streamPointer) const
        {
            if (streamPointer != nullptr)
            {
                streamPointer->Release();
            }
        }
    };

    // initialize 作用：
    // - 初始化 GDI+、加载 Logo、注册窗口类并创建窗口；
    // - 初始化成功后可执行 show/progress。
    bool initialize()
    {
        if (m_initialized)
        {
            return true;
        }

        // startupInput 作用：GDI+ 启动配置输入结构。
        Gdiplus::GdiplusStartupInput startupInput;
        const Gdiplus::Status startupResult = Gdiplus::GdiplusStartup(
            &m_gdiplusToken,
            &startupInput,
            nullptr);
        if (startupResult != Gdiplus::Ok)
        {
            return false;
        }

        // logoFile 作用：读取 qrc 内嵌 Logo 的二进制数据。
        QFile logoFile(QString::fromLatin1(kFrameworkSplashLogoResourcePath));
        if (!logoFile.open(QIODevice::ReadOnly))
        {
            shutdown();
            return false;
        }

        // logoBytes 作用：承载 Logo PNG 的原始字节流。
        const QByteArray logoBytes = logoFile.readAll();
        if (logoBytes.isEmpty())
        {
            shutdown();
            return false;
        }

        // memoryHandle 作用：为 COM IStream 分配可移动全局内存块。
        HGLOBAL memoryHandle = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(logoBytes.size()));
        if (memoryHandle == nullptr)
        {
            shutdown();
            return false;
        }

        void* memoryPointer = ::GlobalLock(memoryHandle);
        if (memoryPointer == nullptr)
        {
            ::GlobalFree(memoryHandle);
            shutdown();
            return false;
        }
        std::memcpy(memoryPointer, logoBytes.constData(), static_cast<std::size_t>(logoBytes.size()));
        ::GlobalUnlock(memoryHandle);

        IStream* streamRawPointer = nullptr;
        const HRESULT createStreamResult = ::CreateStreamOnHGlobal(memoryHandle, TRUE, &streamRawPointer);
        if (FAILED(createStreamResult) || streamRawPointer == nullptr)
        {
            ::GlobalFree(memoryHandle);
            shutdown();
            return false;
        }
        m_logoStream.reset(streamRawPointer);

        // imageRawPointer 作用：GDI+ 图像对象原始指针，后续转为 unique_ptr 托管。
        Gdiplus::Image* imageRawPointer = Gdiplus::Image::FromStream(m_logoStream.get(), FALSE);
        m_logoImage.reset(imageRawPointer);
        if (!m_logoImage || m_logoImage->GetLastStatus() != Gdiplus::Ok)
        {
            shutdown();
            return false;
        }

        if (!registerWindowClassIfNeeded())
        {
            shutdown();
            return false;
        }

        recalculateLayoutMetrics();

        // windowStyleEx 作用：组合透明层、置顶、无激活启动窗样式。
        const DWORD windowStyleEx = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        m_windowHandle = ::CreateWindowExW(
            windowStyleEx,
            kFrameworkSplashClassName,
            L"",
            WS_POPUP,
            m_windowPosX,
            m_windowPosY,
            m_windowWidth,
            m_windowHeight,
            nullptr,
            nullptr,
            ::GetModuleHandleW(nullptr),
            this);
        if (m_windowHandle == nullptr)
        {
            shutdown();
            return false;
        }

        m_initialized = true;
        return true;
    }

    // shutdown 作用：
    // - 回收原生窗口、图像流与 GDI+；
    // - 在析构和初始化失败回滚场景复用。
    void shutdown()
    {
        if (m_windowHandle != nullptr)
        {
            ::DestroyWindow(m_windowHandle);
            m_windowHandle = nullptr;
        }
        m_logoImage.reset();
        m_logoStream.reset();
        if (m_gdiplusToken != 0)
        {
            Gdiplus::GdiplusShutdown(m_gdiplusToken);
            m_gdiplusToken = 0;
        }
        m_initialized = false;
    }

    // registerWindowClassIfNeeded 作用：
    // - 仅首次注册启动窗类；
    // - 重复调用安全。
    bool registerWindowClassIfNeeded()
    {
        static bool classRegistered = false;
        if (classRegistered)
        {
            return true;
        }

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::windowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kFrameworkSplashClassName;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;

        const ATOM registerResult = ::RegisterClassExW(&windowClass);
        if (registerResult == 0)
        {
            const DWORD errorCode = ::GetLastError();
            if (errorCode != ERROR_CLASS_ALREADY_EXISTS)
            {
                return false;
            }
        }

        classRegistered = true;
        return true;
    }

    // recalculateLayoutMetrics 作用：
    // - 依据系统 DPI 和屏幕尺寸重算 Logo 绘制尺寸与窗口尺寸；
    // - 保证启动图在不同缩放下保持一致视觉比例。
    void recalculateLayoutMetrics()
    {
        if (!m_logoImage)
        {
            return;
        }

        m_currentDpi = std::max(96U, querySystemDpiValue());
        m_dpiScaleFactor = static_cast<double>(m_currentDpi) / 96.0;

        // 根据 DPI 缩放固定像素参数，修复高缩放下控件偏小问题。
        m_padding = std::max(8, static_cast<int>(std::lround(m_basePadding * m_dpiScaleFactor)));
        m_bottomAreaHeight = std::max(28, static_cast<int>(std::lround(m_baseBottomAreaHeight * m_dpiScaleFactor)));
        m_logoInfoSpacing = std::max(6, static_cast<int>(std::lround(m_baseLogoInfoSpacing * m_dpiScaleFactor)));
        m_statusFontPixelSize = std::max(9.0f, static_cast<float>(m_baseStatusFontPixelSize * m_dpiScaleFactor));
        m_trackHeight = std::max(4, static_cast<int>(std::lround(m_baseTrackHeight * m_dpiScaleFactor)));

        // sourceLogoWidth/sourceLogoHeight 作用：Logo 原始像素尺寸。
        const int sourceLogoWidth = std::max(1, static_cast<int>(m_logoImage->GetWidth()));
        const int sourceLogoHeight = std::max(1, static_cast<int>(m_logoImage->GetHeight()));

        // screenWidth/screenHeight 作用：当前主屏幕像素宽高。
        const int screenWidth = std::max(1, ::GetSystemMetrics(SM_CXSCREEN));
        const int screenHeight = std::max(1, ::GetSystemMetrics(SM_CYSCREEN));

        // 按 DPI 缩放最小显示尺寸，确保高缩放下 Logo 仍清晰且占比合理。
        const int minLogoWidth = std::max(1, static_cast<int>(std::lround(320.0 * m_dpiScaleFactor)));
        const int minLogoHeight = std::max(1, static_cast<int>(std::lround(220.0 * m_dpiScaleFactor)));
        const int maxLogoWidth = std::max(minLogoWidth, (screenWidth * 40) / 100);
        const int maxLogoHeight = std::max(minLogoHeight, (screenHeight * 38) / 100);

        const double scaleByWidth = static_cast<double>(maxLogoWidth) / static_cast<double>(sourceLogoWidth);
        const double scaleByHeight = static_cast<double>(maxLogoHeight) / static_cast<double>(sourceLogoHeight);
        const double finalScale = std::min(1.0, std::min(scaleByWidth, scaleByHeight));

        m_logoDrawWidth = std::max(1, static_cast<int>(std::lround(sourceLogoWidth * finalScale)));
        m_logoDrawHeight = std::max(1, static_cast<int>(std::lround(sourceLogoHeight * finalScale)));

        m_windowWidth = m_logoDrawWidth + (m_padding * 2);
        m_windowHeight = m_logoDrawHeight + (m_padding * 2) + m_logoInfoSpacing + m_bottomAreaHeight;
        m_windowPosX = (screenWidth - m_windowWidth) / 2;
        m_windowPosY = (screenHeight - m_windowHeight) / 2;

        if (m_windowHandle != nullptr)
        {
            ::SetWindowPos(
                m_windowHandle,
                HWND_TOPMOST,
                m_windowPosX,
                m_windowPosY,
                m_windowWidth,
                m_windowHeight,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    // renderFrame 作用：
    // - 使用 32 位 ARGB DIB 绘制透明启动窗；
    // - 通过 UpdateLayeredWindow 提交到系统桌面。
    void renderFrame()
    {
        if (m_windowHandle == nullptr || !m_logoImage)
        {
            return;
        }

        HDC screenDeviceContext = ::GetDC(nullptr);
        if (screenDeviceContext == nullptr)
        {
            return;
        }

        HDC memoryDeviceContext = ::CreateCompatibleDC(screenDeviceContext);
        if (memoryDeviceContext == nullptr)
        {
            ::ReleaseDC(nullptr, screenDeviceContext);
            return;
        }

        BITMAPINFO bitmapInfo = {};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = m_windowWidth;
        bitmapInfo.bmiHeader.biHeight = -m_windowHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* dibBitsPointer = nullptr;
        HBITMAP dibBitmapHandle = ::CreateDIBSection(
            memoryDeviceContext,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &dibBitsPointer,
            nullptr,
            0);
        if (dibBitmapHandle == nullptr)
        {
            ::DeleteDC(memoryDeviceContext);
            ::ReleaseDC(nullptr, screenDeviceContext);
            return;
        }

        HGDIOBJ oldBitmapHandle = ::SelectObject(memoryDeviceContext, dibBitmapHandle);

        {
            Gdiplus::Graphics graphics(memoryDeviceContext);
            graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
            graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

            // 居中绘制 Logo 主图层。
            const int logoPosX = (m_windowWidth - m_logoDrawWidth) / 2;
            const int logoPosY = m_padding;
            graphics.DrawImage(
                m_logoImage.get(),
                Gdiplus::Rect(logoPosX, logoPosY, m_logoDrawWidth, m_logoDrawHeight));

            // 底部叠加信息层：状态文案 + 进度条（整体位于 Logo 下方，不遮挡主图）。
            const int overlayTop = logoPosY + m_logoDrawHeight + m_logoInfoSpacing;
            const Gdiplus::RectF overlayRect(
                0.0f,
                static_cast<Gdiplus::REAL>(overlayTop),
                static_cast<Gdiplus::REAL>(m_windowWidth),
                static_cast<Gdiplus::REAL>(m_bottomAreaHeight));
            Gdiplus::SolidBrush overlayBrush(Gdiplus::Color(138, 0, 0, 0));
            graphics.FillRectangle(&overlayBrush, overlayRect);

            const int textPosY = overlayTop + std::max(6, static_cast<int>(std::lround(8.0 * m_dpiScaleFactor)));
            const Gdiplus::RectF textRect(
                static_cast<Gdiplus::REAL>(m_padding),
                static_cast<Gdiplus::REAL>(textPosY),
                static_cast<Gdiplus::REAL>(m_windowWidth - m_padding * 2),
                static_cast<Gdiplus::REAL>(std::max(16, static_cast<int>(std::lround(22.0 * m_dpiScaleFactor)))));
            Gdiplus::Font textFont(
                L"Microsoft YaHei UI",
                m_statusFontPixelSize,
                Gdiplus::FontStyleRegular,
                Gdiplus::UnitPixel);
            Gdiplus::SolidBrush textBrush(Gdiplus::Color(235, 248, 248, 248));
            graphics.DrawString(m_statusText.c_str(), -1, &textFont, textRect, nullptr, &textBrush);

            const int trackMarginBottom = std::max(4, static_cast<int>(std::lround(6.0 * m_dpiScaleFactor)));
            const int trackTop = overlayTop + m_bottomAreaHeight - m_trackHeight - trackMarginBottom;
            const int trackWidth = std::max(1, m_windowWidth - m_padding * 2);
            const Gdiplus::RectF trackRect(
                static_cast<Gdiplus::REAL>(m_padding),
                static_cast<Gdiplus::REAL>(trackTop),
                static_cast<Gdiplus::REAL>(trackWidth),
                static_cast<Gdiplus::REAL>(m_trackHeight));
            Gdiplus::SolidBrush trackBrush(Gdiplus::Color(130, 25, 25, 25));
            graphics.FillRectangle(&trackBrush, trackRect);

            const int filledWidth = static_cast<int>(
                std::lround(static_cast<double>(trackWidth) * (static_cast<double>(m_progressPercent) / 100.0)));
            const Gdiplus::RectF valueRect(
                static_cast<Gdiplus::REAL>(m_padding),
                static_cast<Gdiplus::REAL>(trackTop),
                static_cast<Gdiplus::REAL>(std::max(0, filledWidth)),
                static_cast<Gdiplus::REAL>(m_trackHeight));
            Gdiplus::SolidBrush valueBrush(Gdiplus::Color(240, 67, 160, 255));
            graphics.FillRectangle(&valueBrush, valueRect);
        }

        POINT sourcePoint = { 0, 0 };
        SIZE layerSize = { m_windowWidth, m_windowHeight };
        POINT targetPoint = { m_windowPosX, m_windowPosY };
        BLENDFUNCTION blendFunction = {};
        blendFunction.BlendOp = AC_SRC_OVER;
        blendFunction.SourceConstantAlpha = 255;
        blendFunction.AlphaFormat = AC_SRC_ALPHA;

        ::UpdateLayeredWindow(
            m_windowHandle,
            screenDeviceContext,
            &targetPoint,
            &layerSize,
            memoryDeviceContext,
            &sourcePoint,
            0,
            &blendFunction,
            ULW_ALPHA);

        ::SelectObject(memoryDeviceContext, oldBitmapHandle);
        ::DeleteObject(dibBitmapHandle);
        ::DeleteDC(memoryDeviceContext);
        ::ReleaseDC(nullptr, screenDeviceContext);
    }

    // windowProc 作用：
    // - 处理启动窗口基础消息；
    // - 关闭背景擦除，避免透明层闪白。
    // 入参 hwnd/message/wParam/lParam：Win32 标准消息参数。
    // 返回：消息处理结果。
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const CREATESTRUCTW* createStructPointer = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            if (createStructPointer != nullptr)
            {
                Impl* selfPointer = reinterpret_cast<Impl*>(createStructPointer->lpCreateParams);
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(selfPointer));
            }
        }

        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        default:
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

private:
    HWND m_windowHandle = nullptr;                              // m_windowHandle：启动窗口句柄。
    ULONG_PTR m_gdiplusToken = 0;                              // m_gdiplusToken：GDI+ 运行令牌。
    std::unique_ptr<Gdiplus::Image> m_logoImage;               // m_logoImage：Logo 图像对象。
    std::unique_ptr<IStream, StreamReleaser> m_logoStream;     // m_logoStream：Logo 内存流对象。
    std::wstring m_statusText = L"正在启动...";                  // m_statusText：当前状态文本。
    int m_progressPercent = 0;                                 // m_progressPercent：进度百分比。
    int m_windowWidth = 480;                                   // m_windowWidth：窗口宽度像素。
    int m_windowHeight = 320;                                  // m_windowHeight：窗口高度像素。
    int m_logoDrawWidth = 360;                                 // m_logoDrawWidth：Logo 绘制宽度像素。
    int m_logoDrawHeight = 220;                                // m_logoDrawHeight：Logo 绘制高度像素。
    int m_windowPosX = 0;                                      // m_windowPosX：窗口左上角 X 坐标。
    int m_windowPosY = 0;                                      // m_windowPosY：窗口左上角 Y 坐标。
    int m_padding = 24;                                        // m_padding：窗口内边距（DPI 缩放后值）。
    int m_bottomAreaHeight = 56;                               // m_bottomAreaHeight：底部信息区高度（DPI 缩放后值）。
    int m_logoInfoSpacing = 12;                                // m_logoInfoSpacing：Logo 与信息区垂直间距。
    float m_statusFontPixelSize = 12.0f;                       // m_statusFontPixelSize：状态文字像素字号。
    int m_trackHeight = 8;                                     // m_trackHeight：进度条轨道高度。
    bool m_initialized = false;                                // m_initialized：是否初始化完成。
    UINT m_currentDpi = 96U;                                   // m_currentDpi：当前系统 DPI。
    double m_dpiScaleFactor = 1.0;                             // m_dpiScaleFactor：DPI 缩放倍率。
    int m_basePadding = 24;                                    // m_basePadding：96DPI 基准内边距。
    int m_baseBottomAreaHeight = 56;                           // m_baseBottomAreaHeight：96DPI 基准底部区高度。
    int m_baseLogoInfoSpacing = 12;                            // m_baseLogoInfoSpacing：96DPI 基准 Logo 与信息区间距。
    float m_baseStatusFontPixelSize = 12.0f;                   // m_baseStatusFontPixelSize：96DPI 基准文字字号。
    int m_baseTrackHeight = 8;                                 // m_baseTrackHeight：96DPI 基准进度条高度。
};

kStartupSplash::kStartupSplash()
    : m_impl(std::make_unique<Impl>())
{
}

kStartupSplash::~kStartupSplash() = default;

bool kStartupSplash::show()
{
    if (!m_impl)
    {
        return false;
    }
    return m_impl->showWindow();
}

void kStartupSplash::hide()
{
    if (m_impl)
    {
        m_impl->hideWindow();
    }
}

void kStartupSplash::progress(const std::string& operationName, const int progressPercent)
{
    if (m_impl)
    {
        m_impl->setProgressState(operationName, progressPercent);
    }
}

bool kStartupSplash::ready() const
{
    if (!m_impl)
    {
        return false;
    }
    return m_impl->isReady();
}

// kSplash 作用：
// - Framework 全局启动画面控制对象定义；
// - 与 Framework.h 中 extern 声明对应。
kStartupSplash kSplash;
