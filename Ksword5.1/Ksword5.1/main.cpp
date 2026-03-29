#include "MainWindow.h"

#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "Gdiplus.lib")

namespace
{
    // kNativeSplashClassName 作用：
    // - 启动画面的 Win32 窗口类名；
    // - 统一注册/创建时的类标识。
    constexpr wchar_t kNativeSplashClassName[] = L"KswordNativeSplashWindow";

    // clampPercent 作用：
    // - 把任意进度值限制在 0~100；
    // - 防止进度条长度越界。
    int clampPercent(const int rawValue)
    {
        return std::clamp(rawValue, 0, 100);
    }

    // initializeProcessDpiAwareness 作用：
    // - 在主函数最早阶段设置进程 DPI 感知模式；
    // - 避免 Qt 初始化后 DPI 上下文变化导致启动图缩放/偏移。
    void initializeProcessDpiAwareness()
    {
        HMODULE user32ModuleHandle = ::GetModuleHandleW(L"user32.dll");
        if (user32ModuleHandle != nullptr)
        {
            using SetDpiAwarenessContextFunction = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            SetDpiAwarenessContextFunction setContextFunction =
                reinterpret_cast<SetDpiAwarenessContextFunction>(
                    ::GetProcAddress(user32ModuleHandle, "SetProcessDpiAwarenessContext"));
            if (setContextFunction != nullptr)
            {
                if (setContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                {
                    return;
                }
                if (setContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
                {
                    return;
                }
            }
        }

        // 兼容旧系统的退化方案：至少启用系统 DPI 感知。
        ::SetProcessDPIAware();
    }

    // SplashWindowProc 作用：
    // - 处理启动窗口基础消息；
    // - 禁止背景擦除，避免出现白色闪烁底色。
    // 入参 hwnd/message/wParam/lParam：标准 Win32 回调参数。
    // 返回值：消息处理结果。
    LRESULT CALLBACK SplashWindowProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        default:
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    // pumpSplashMessages 作用：
    // - 在 Qt 事件循环启动前，手动泵一次 Win32 消息；
    // - 确保原生启动窗及时可见并响应重绘。
    void pumpSplashMessages()
    {
        MSG message = {};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    // getExecutableDirectoryPath 作用：
    // - 获取当前 EXE 所在目录；
    // - 用于定位 MainLogo.png 文件。
    std::filesystem::path getExecutableDirectoryPath()
    {
        wchar_t modulePathBuffer[MAX_PATH] = {};
        const DWORD copiedLength = ::GetModuleFileNameW(nullptr, modulePathBuffer, static_cast<DWORD>(std::size(modulePathBuffer)));
        if (copiedLength == 0 || copiedLength >= std::size(modulePathBuffer))
        {
            return std::filesystem::current_path();
        }

        std::filesystem::path modulePath(modulePathBuffer);
        if (!modulePath.has_parent_path())
        {
            return std::filesystem::current_path();
        }
        return modulePath.parent_path();
    }

    // resolveMainLogoPath 作用：
    // - 搜索 MainLogo.png 的绝对路径；
    // - 按 EXE 目录和当前目录向上回溯，兼容开发态与发布态。
    // 返回值：存在则返回完整路径，否则返回空字符串。
    std::wstring resolveMainLogoPath()
    {
        std::vector<std::filesystem::path> searchRoots;
        searchRoots.push_back(getExecutableDirectoryPath());
        searchRoots.push_back(std::filesystem::current_path());

        for (const std::filesystem::path& rootPath : searchRoots)
        {
            std::filesystem::path walkingPath = rootPath;
            for (int depth = 0; depth < 8; ++depth)
            {
                const std::filesystem::path directCandidate =
                    walkingPath / L"Resource" / L"Logo" / L"MainLogo.png";
                if (std::filesystem::exists(directCandidate))
                {
                    return directCandidate.wstring();
                }

                const std::filesystem::path nestedCandidate =
                    walkingPath / L"Ksword5.1" / L"Resource" / L"Logo" / L"MainLogo.png";
                if (std::filesystem::exists(nestedCandidate))
                {
                    return nestedCandidate.wstring();
                }

                if (!walkingPath.has_parent_path())
                {
                    break;
                }
                walkingPath = walkingPath.parent_path();
            }
        }

        return std::wstring();
    }

    // NativeSplashWindow 作用：
    // - 用 Win32 + GDI+ 绘制带 Alpha 透明的启动画面；
    // - 支持左下角状态文本和进度条。
    class NativeSplashWindow final
    {
    public:
        // initialize 作用：
        // - 启动 GDI+、加载 Logo、创建 Layered Window；
        // - 成功后可调用 show/updateProgress。
        // 入参 logoPathText：MainLogo.png 绝对路径。
        // 返回值：true=初始化成功；false=失败。
        bool initialize(const std::wstring& logoPathText)
        {
            if (m_initialized)
            {
                return true;
            }

            if (logoPathText.empty())
            {
                return false;
            }

            m_logoPathText = logoPathText;

            Gdiplus::GdiplusStartupInput startupInput;
            const Gdiplus::Status startupStatus = Gdiplus::GdiplusStartup(
                &m_gdiplusToken,
                &startupInput,
                nullptr);
            if (startupStatus != Gdiplus::Ok)
            {
                return false;
            }

            Gdiplus::Image* imageRaw = Gdiplus::Image::FromFile(m_logoPathText.c_str(), FALSE);
            m_logoImage.reset(imageRaw);
            if (!m_logoImage || m_logoImage->GetLastStatus() != Gdiplus::Ok)
            {
                m_logoImage.reset();
                return false;
            }

            if (!registerClassIfNeeded())
            {
                return false;
            }

            calculateLayoutMetrics();

            const int screenWidth = std::max(1, ::GetSystemMetrics(SM_CXSCREEN));
            const int screenHeight = std::max(1, ::GetSystemMetrics(SM_CYSCREEN));
            m_windowPosX = (screenWidth - m_windowWidth) / 2;
            m_windowPosY = (screenHeight - m_windowHeight) / 2;

            m_windowHandle = ::CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                kNativeSplashClassName,
                L"",
                WS_POPUP,
                m_windowPosX,
                m_windowPosY,
                m_windowWidth,
                m_windowHeight,
                nullptr,
                nullptr,
                ::GetModuleHandleW(nullptr),
                nullptr);
            if (m_windowHandle == nullptr)
            {
                return false;
            }

            m_initialized = true;
            return true;
        }

        // show 作用：
        // - 显示窗口并渲染第一帧。
        void show()
        {
            if (!m_initialized || m_windowHandle == nullptr)
            {
                return;
            }

            renderFrame();
            ::ShowWindow(m_windowHandle, SW_SHOWNOACTIVATE);
            ::UpdateWindow(m_windowHandle);
        }

        // updateProgress 作用：
        // - 更新底部状态文本和进度值；
        // - 每次调用立即重绘启动画面。
        // 入参 progressPercent：进度百分比（0~100）。
        // 入参 statusText：状态文本。
        void updateProgress(const int progressPercent, const std::wstring& statusText)
        {
            if (!m_initialized || m_windowHandle == nullptr)
            {
                return;
            }

            m_progressPercent = clampPercent(progressPercent);
            m_statusText = statusText;
            renderFrame();
        }

        // hide 作用：
        // - 隐藏并销毁启动窗口。
        void hide()
        {
            if (m_windowHandle != nullptr)
            {
                ::DestroyWindow(m_windowHandle);
                m_windowHandle = nullptr;
            }
        }

        // 析构函数作用：
        // - 释放窗口对象和 GDI+ 资源。
        ~NativeSplashWindow()
        {
            hide();
            m_logoImage.reset();
            if (m_gdiplusToken != 0)
            {
                Gdiplus::GdiplusShutdown(m_gdiplusToken);
                m_gdiplusToken = 0;
            }
        }

    private:
        // registerClassIfNeeded 作用：
        // - 仅首次注册启动窗口类；
        // - 防止重复注册失败。
        bool registerClassIfNeeded()
        {
            static bool classRegistered = false;
            if (classRegistered)
            {
                return true;
            }

            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = SplashWindowProc;
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.lpszClassName = kNativeSplashClassName;
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

        // calculateLayoutMetrics 作用：
        // - 按屏幕比例计算窗口尺寸；
        // - 保证 Logo 保持比例且居中。
        void calculateLayoutMetrics()
        {
            const int sourceLogoWidth = static_cast<int>(m_logoImage->GetWidth());
            const int sourceLogoHeight = static_cast<int>(m_logoImage->GetHeight());

            const int screenWidth = std::max(1, ::GetSystemMetrics(SM_CXSCREEN));
            const int screenHeight = std::max(1, ::GetSystemMetrics(SM_CYSCREEN));
            const int maxLogoWidth = std::max(320, (screenWidth * 40) / 100);
            const int maxLogoHeight = std::max(220, (screenHeight * 38) / 100);

            const double scaleByWidth = static_cast<double>(maxLogoWidth) / static_cast<double>(sourceLogoWidth);
            const double scaleByHeight = static_cast<double>(maxLogoHeight) / static_cast<double>(sourceLogoHeight);
            const double finalScale = std::min(1.0, std::min(scaleByWidth, scaleByHeight));

            m_logoDrawWidth = std::max(1, static_cast<int>(sourceLogoWidth * finalScale));
            m_logoDrawHeight = std::max(1, static_cast<int>(sourceLogoHeight * finalScale));

            // 启动图窗口尺寸只由 Logo 和边距决定：
            // - 进度条区域改为“底部叠加层”，不再挤占 Logo 空间；
            // - 修复“进度出现后 Logo 缩小并偏移”的视觉问题。
            m_windowWidth = m_logoDrawWidth + (m_padding * 2);
            m_windowHeight = m_logoDrawHeight + (m_padding * 2);
        }

        // renderFrame 作用：
        // - 构造 ARGB DIB 并使用 UpdateLayeredWindow 显示；
        // - 透明部分由 PNG alpha 保留。
        void renderFrame()
        {
            if (m_windowHandle == nullptr)
            {
                return;
            }

            HDC screenDc = ::GetDC(nullptr);
            HDC memoryDc = ::CreateCompatibleDC(screenDc);
            if (memoryDc == nullptr)
            {
                ::ReleaseDC(nullptr, screenDc);
                return;
            }

            BITMAPINFO bitmapInfo = {};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = m_windowWidth;
            bitmapInfo.bmiHeader.biHeight = -m_windowHeight;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            void* bitmapBits = nullptr;
            HBITMAP dibBitmap = ::CreateDIBSection(
                memoryDc,
                &bitmapInfo,
                DIB_RGB_COLORS,
                &bitmapBits,
                nullptr,
                0);
            if (dibBitmap == nullptr)
            {
                ::DeleteDC(memoryDc);
                ::ReleaseDC(nullptr, screenDc);
                return;
            }

            HGDIOBJ oldBitmap = ::SelectObject(memoryDc, dibBitmap);

            {
                Gdiplus::Graphics graphics(memoryDc);
                graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
                graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

                const int logoPosX = (m_windowWidth - m_logoDrawWidth) / 2;
                const int logoPosY = (m_windowHeight - m_logoDrawHeight) / 2;
                graphics.DrawImage(
                    m_logoImage.get(),
                    Gdiplus::Rect(logoPosX, logoPosY, m_logoDrawWidth, m_logoDrawHeight));

                // 底部叠加信息层：半透明面板，覆盖在 Logo 下方区域上。
                const int overlayTop = std::max(0, m_windowHeight - m_bottomAreaHeight);
                const Gdiplus::RectF overlayRect(
                    0.0f,
                    static_cast<Gdiplus::REAL>(overlayTop),
                    static_cast<Gdiplus::REAL>(m_windowWidth),
                    static_cast<Gdiplus::REAL>(m_bottomAreaHeight));
                Gdiplus::SolidBrush overlayBrush(Gdiplus::Color(138, 0, 0, 0));
                graphics.FillRectangle(&overlayBrush, overlayRect);

                const int textPosY = overlayTop + 10;
                const Gdiplus::RectF textRect(
                    static_cast<Gdiplus::REAL>(m_padding),
                    static_cast<Gdiplus::REAL>(textPosY),
                    static_cast<Gdiplus::REAL>(m_windowWidth - m_padding * 2),
                    20.0f);
                Gdiplus::Font textFont(L"Microsoft YaHei UI", 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush textBrush(Gdiplus::Color(235, 248, 248, 248));
                graphics.DrawString(m_statusText.c_str(), -1, &textFont, textRect, nullptr, &textBrush);

                const int trackTop = m_windowHeight - 14;
                const int trackWidth = m_windowWidth - m_padding * 2;
                const Gdiplus::RectF trackRect(
                    static_cast<Gdiplus::REAL>(m_padding),
                    static_cast<Gdiplus::REAL>(trackTop),
                    static_cast<Gdiplus::REAL>(trackWidth),
                    8.0f);
                Gdiplus::SolidBrush trackBrush(Gdiplus::Color(130, 25, 25, 25));
                graphics.FillRectangle(&trackBrush, trackRect);

                const int filledWidth = static_cast<int>(static_cast<double>(trackWidth) * (static_cast<double>(m_progressPercent) / 100.0));
                const Gdiplus::RectF filledRect(
                    static_cast<Gdiplus::REAL>(m_padding),
                    static_cast<Gdiplus::REAL>(trackTop),
                    static_cast<Gdiplus::REAL>(filledWidth),
                    8.0f);
                Gdiplus::SolidBrush valueBrush(Gdiplus::Color(240, 67, 160, 255));
                graphics.FillRectangle(&valueBrush, filledRect);
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
                screenDc,
                &targetPoint,
                &layerSize,
                memoryDc,
                &sourcePoint,
                0,
                &blendFunction,
                ULW_ALPHA);

            ::SelectObject(memoryDc, oldBitmap);
            ::DeleteObject(dibBitmap);
            ::DeleteDC(memoryDc);
            ::ReleaseDC(nullptr, screenDc);
        }

    private:
        HWND m_windowHandle = nullptr;                         // m_windowHandle：启动窗口句柄。
        ULONG_PTR m_gdiplusToken = 0;                         // m_gdiplusToken：GDI+ 令牌。
        std::unique_ptr<Gdiplus::Image> m_logoImage;          // m_logoImage：Logo 图像对象。
        std::wstring m_logoPathText;                          // m_logoPathText：Logo 文件路径。
        std::wstring m_statusText = L"正在启动...";             // m_statusText：底部状态文字。
        int m_progressPercent = 0;                            // m_progressPercent：进度百分比。
        int m_windowWidth = 480;                              // m_windowWidth：窗口宽度。
        int m_windowHeight = 320;                             // m_windowHeight：窗口高度。
        int m_logoDrawWidth = 360;                            // m_logoDrawWidth：Logo 绘制宽度。
        int m_logoDrawHeight = 220;                           // m_logoDrawHeight：Logo 绘制高度。
        int m_windowPosX = 0;                                 // m_windowPosX：窗口 X 坐标。
        int m_windowPosY = 0;                                 // m_windowPosY：窗口 Y 坐标。
        int m_padding = 24;                                   // m_padding：窗口内边距。
        int m_bottomAreaHeight = 56;                          // m_bottomAreaHeight：底部叠加区高度（不改窗口尺寸）。
        bool m_initialized = false;                           // m_initialized：初始化是否成功。
    };

    // FirstFrameSplashHider 作用：
    // - 监听主窗口首个 Paint 事件；
    // - 收到首帧后立即隐藏启动画面。
    class FirstFrameSplashHider final : public QObject
    {
    public:
        // 构造函数作用：
        // - 保存主窗口与启动画面实例引用。
        // 入参 targetWindow：目标主窗口。
        // 入参 splashWindow：启动画面对象。
        FirstFrameSplashHider(
            MainWindow* targetWindow,
            NativeSplashWindow* splashWindow)
            : QObject(targetWindow)
            , m_targetWindow(targetWindow)
            , m_splashWindow(splashWindow)
        {
        }

    protected:
        // eventFilter 作用：
        // - 捕获首个 Paint 后隐藏启动画面；
        // - 隐藏后取消过滤器，避免重复执行。
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (!m_hidden
                && watchedObject == m_targetWindow
                && eventObject != nullptr
                && eventObject->type() == QEvent::Paint)
            {
                m_hidden = true;
                if (m_splashWindow != nullptr)
                {
                    m_splashWindow->updateProgress(100, L"启动完成");
                    m_splashWindow->hide();
                }
                if (m_targetWindow != nullptr)
                {
                    m_targetWindow->removeEventFilter(this);
                }
            }

            return QObject::eventFilter(watchedObject, eventObject);
        }

    private:
        MainWindow* m_targetWindow = nullptr;         // m_targetWindow：主窗口指针。
        NativeSplashWindow* m_splashWindow = nullptr; // m_splashWindow：启动画面指针。
        bool m_hidden = false;                        // m_hidden：是否已执行隐藏。
    };
}

int main(int argc, char* argv[])
{
    // 启动流程：
    // 1) Qt 前显示原生透明启动画面；
    // 2) 分阶段更新启动文字和进度；
    // 3) 主窗口首帧绘制后立即隐藏。
    initializeProcessDpiAwareness();

    NativeSplashWindow splashWindow;
    const std::wstring logoPathText = resolveMainLogoPath();
    const bool splashReady = splashWindow.initialize(logoPathText);
    if (splashReady)
    {
        splashWindow.show();
        splashWindow.updateProgress(8, L"正在初始化 Qt 运行时...");
        pumpSplashMessages();
    }

    QApplication app(argc, argv);

    if (splashReady)
    {
        splashWindow.updateProgress(42, L"正在创建主窗口...");
        pumpSplashMessages();
    }

    MainWindow window;
    FirstFrameSplashHider firstFrameHider(&window, &splashWindow);
    window.installEventFilter(&firstFrameHider);

    if (splashReady)
    {
        splashWindow.updateProgress(78, L"正在加载 Dock 与主题...");
        pumpSplashMessages();
    }

    window.show();

    if (splashReady)
    {
        splashWindow.updateProgress(92, L"正在渲染第一帧...");
        pumpSplashMessages();

        // 兜底：若异常未收到 Paint 事件，4 秒后强制隐藏。
        QTimer::singleShot(4000, &window, [&splashWindow]() {
            splashWindow.hide();
            });
    }

    const int exitCode = app.exec();
    splashWindow.hide();
    return exitCode;
}
