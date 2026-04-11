#include "MainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include "Framework.h"
#include "Framework/ThemedMessageBox.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace
{
    // kNativeAppIconResourceName 作用：
    // - 指向 AppIcon.rc 内声明的主图标资源名；
    // - 用于把原生图标同步到 Qt 主窗口标题栏和任务栏。
    constexpr wchar_t kNativeAppIconResourceName[] = L"IDI_APP_ICON";

    // isCurrentProcessElevated 作用：
    // - 判断当前进程是否已经运行在管理员权限下；
    // - 供“启动时自动请求管理员权限”逻辑复用。
    // 返回：true=已提权；false=普通权限。
    bool isCurrentProcessElevated()
    {
        HANDLE tokenHandle = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
        {
            return false;
        }

        // tokenElevation 作用：承接 TokenElevation 查询结果。
        TOKEN_ELEVATION tokenElevation{};
        DWORD returnLength = 0;
        const BOOL queryOk = ::GetTokenInformation(
            tokenHandle,
            TokenElevation,
            &tokenElevation,
            sizeof(tokenElevation),
            &returnLength);
        ::CloseHandle(tokenHandle);
        return queryOk != FALSE && tokenElevation.TokenIsElevated != 0;
    }

    // extractCurrentProcessParameterText 作用：
    // - 从当前命令行提取“exe 之后”的参数文本；
    // - runas 重启时原样透传，避免行为差异。
    // 返回：可传入 ShellExecuteW 的参数字符串。
    std::wstring extractCurrentProcessParameterText()
    {
        const wchar_t* commandLineText = ::GetCommandLineW();
        if (commandLineText == nullptr)
        {
            return std::wstring();
        }

        // cursorPointer 作用：遍历命令行并定位到第一个参数起始位置。
        const wchar_t* cursorPointer = commandLineText;
        bool insideQuotes = false;
        while (*cursorPointer != L'\0')
        {
            if (*cursorPointer == L'"')
            {
                insideQuotes = !insideQuotes;
            }
            else if (!insideQuotes && std::iswspace(static_cast<wint_t>(*cursorPointer)) != 0)
            {
                break;
            }
            ++cursorPointer;
        }

        while (std::iswspace(static_cast<wint_t>(*cursorPointer)) != 0)
        {
            ++cursorPointer;
        }

        return std::wstring(cursorPointer);
    }

    // tryLaunchElevatedSelfBeforeSplash 作用：
    // - 在启动页出现前尝试 runas 拉起管理员实例；
    // - 成功则当前普通权限实例直接退出，避免双开。
    // 返回：true=管理员实例已拉起；false=失败，继续普通启动。
    bool tryLaunchElevatedSelfBeforeSplash()
    {
        wchar_t executablePathBuffer[MAX_PATH] = {};
        const DWORD pathLength = ::GetModuleFileNameW(nullptr, executablePathBuffer, MAX_PATH);
        if (pathLength == 0 || pathLength >= MAX_PATH)
        {
            return false;
        }

        // parameterText 作用：保存当前命令行参数，传递给新启动实例。
        const std::wstring parameterText = extractCurrentProcessParameterText();
        HINSTANCE shellResult = ::ShellExecuteW(
            nullptr,
            L"runas",
            executablePathBuffer,
            parameterText.empty() ? nullptr : parameterText.c_str(),
            nullptr,
            SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(shellResult) > 32;
    }

    // initializeProcessDpiAwareness 作用：
    // - 在主函数最早阶段设置进程 DPI 感知；
    // - 保证启动页与 Qt 主窗口的缩放行为稳定。
    void initializeProcessDpiAwareness()
    {
        HMODULE user32ModuleHandle = ::GetModuleHandleW(L"user32.dll");
        if (user32ModuleHandle != nullptr)
        {
            using SetDpiAwarenessContextFunction = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            const SetDpiAwarenessContextFunction setContextFunction =
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

        // 旧系统回退方案：至少启用系统 DPI 感知。
        ::SetProcessDPIAware();
    }

    // querySystemScalePercent 作用：
    // - 读取当前系统缩放百分比（100/125/150...）；
    // - 新系统优先 GetDpiForSystem，旧系统回退 GDI 计算。
    // 返回：系统缩放百分比（最小 100）。
    int querySystemScalePercent()
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
                const UINT dpiValue = getDpiForSystemFunction();
                if (dpiValue >= 96U)
                {
                    const int scalePercent = static_cast<int>(
                        std::lround((static_cast<double>(dpiValue) * 100.0) / 96.0));
                    return std::max(100, scalePercent);
                }
            }
        }

        HDC screenDeviceContext = ::GetDC(nullptr);
        if (screenDeviceContext == nullptr)
        {
            return 100;
        }
        const int logPixelsX = ::GetDeviceCaps(screenDeviceContext, LOGPIXELSX);
        ::ReleaseDC(nullptr, screenDeviceContext);
        if (logPixelsX <= 0)
        {
            return 100;
        }
        const int scalePercent = static_cast<int>(
            std::lround((static_cast<double>(logPixelsX) * 100.0) / 96.0));
        return std::max(100, scalePercent);
    }

    // buildPercentText 作用：
    // - 把比例值转为百分比文本；
    // - 供启动前推荐缩放弹窗显示。
    // 入参 ratioValue：比例值（1.0=100%）。
    // 返回：宽字符串百分比文本。
    std::wstring buildPercentText(const double ratioValue)
    {
        const int percentValue = static_cast<int>(std::lround(ratioValue * 100.0));
        return std::to_wstring(percentValue) + L"%";
    }

    // showStartupScaleRecommendationDialog 作用：
    // - 在启动前提示是否应用推荐缩放；
    // - 弹窗内包含“不再提示”勾选框。
    // 入参 logicalClientWidth：按“物理像素/系统缩放”得到的可用宽度。
    // 入参 currentScaleFactor：当前配置缩放因子。
    // 入参 recommendedScaleFactor：推荐缩放因子。
    // 出参 applyRecommendedOut：是否应用推荐值。
    // 出参 disablePromptOut：是否勾选不再提示。
    // 返回：true=完成选择；false=弹窗失败或被关闭。
    bool showStartupScaleRecommendationDialog(
        const int logicalClientWidth,
        const double currentScaleFactor,
        const double recommendedScaleFactor,
        bool* applyRecommendedOut,
        bool* disablePromptOut)
    {
        if (applyRecommendedOut == nullptr || disablePromptOut == nullptr)
        {
            return false;
        }
        *applyRecommendedOut = false;
        *disablePromptOut = false;

        const std::wstring currentScaleText = buildPercentText(currentScaleFactor);
        const std::wstring recommendedScaleText = buildPercentText(recommendedScaleFactor);
        const std::wstring dialogContentText =
            L"当前可用宽度约 " + std::to_wstring(logicalClientWidth) + L"px，小于推荐的 1440px。\n"
            L"当前缩放：" + currentScaleText + L"\n"
            L"推荐缩放：" + recommendedScaleText + L"\n\n"
            L"是否应用推荐缩放并在下次启动生效？";

        // dialogButtons 作用：TaskDialog 自定义按钮集合。
        TASKDIALOG_BUTTON dialogButtons[] =
        {
            { IDYES, L"应用推荐缩放" },
            { IDNO, L"保持当前设置" }
        };

        TASKDIALOGCONFIG dialogConfig = {};
        dialogConfig.cbSize = sizeof(dialogConfig);
        dialogConfig.hInstance = ::GetModuleHandleW(nullptr);
        dialogConfig.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        dialogConfig.pszWindowTitle = L"Ksword5.1 启动缩放建议";
        dialogConfig.pszMainInstruction = L"检测到当前显示可用宽度偏小";
        dialogConfig.pszContent = dialogContentText.c_str();
        dialogConfig.pButtons = dialogButtons;
        dialogConfig.cButtons = ARRAYSIZE(dialogButtons);
        dialogConfig.nDefaultButton = IDYES;
        dialogConfig.pszVerificationText = L"不再提示";

        int pressedButtonId = 0;
        BOOL verificationChecked = FALSE;
        const HRESULT dialogResult = ::TaskDialogIndirect(
            &dialogConfig,
            &pressedButtonId,
            nullptr,
            &verificationChecked);
        if (FAILED(dialogResult))
        {
            return false;
        }

        *applyRecommendedOut = (pressedButtonId == IDYES);
        *disablePromptOut = (verificationChecked != FALSE);
        return true;
    }

    // maybeApplyStartupScaleRecommendation 作用：
    // - 当可用宽度小于 1440 时提示推荐缩放；
    // - 可写回“缩放因子 + 不再提示”到配置文件。
    // 调用方式：QApplication 创建前调用。
    // 入参 startupSettings：启动配置对象（按需被更新）。
    void maybeApplyStartupScaleRecommendation(ks::settings::AppearanceSettings* startupSettings)
    {
        if (startupSettings == nullptr)
        {
            return;
        }

        // scaleDecisionEvent 作用：串联启动缩放决策链路日志。
        kLogEvent scaleDecisionEvent;
        if (startupSettings->startupScaleRecommendPromptDisabled)
        {
            info << scaleDecisionEvent
                << "[main] 启动缩放推荐提示已禁用，跳过检测。"
                << eol;
            return;
        }

        const int physicalScreenWidth = std::max(1, ::GetSystemMetrics(SM_CXSCREEN));
        const int systemScalePercent = querySystemScalePercent();
        const int logicalClientWidth = static_cast<int>(
            std::lround((static_cast<double>(physicalScreenWidth) * 100.0) / static_cast<double>(systemScalePercent)));
        if (logicalClientWidth >= 1440)
        {
            info << scaleDecisionEvent
                << "[main] 可用宽度满足要求，跳过推荐缩放。 logicalWidth="
                << logicalClientWidth
                << ", systemScalePercent="
                << systemScalePercent
                << eol;
            return;
        }

        const double currentScaleFactor = ks::settings::normalizeWindowScaleFactor(
            startupSettings->startupWindowScaleFactor);
        const double recommendedScaleFactor = ks::settings::normalizeWindowScaleFactor(
            std::min(1.0, static_cast<double>(logicalClientWidth) / 1440.0));
        if (recommendedScaleFactor >= currentScaleFactor - 0.0001)
        {
            info << scaleDecisionEvent
                << "[main] 当前缩放已不大于推荐值，无需提示。 current="
                << currentScaleFactor
                << ", recommended="
                << recommendedScaleFactor
                << eol;
            return;
        }

        bool applyRecommendedScale = false;
        bool disablePrompt = false;
        const bool promptHandled = showStartupScaleRecommendationDialog(
            logicalClientWidth,
            currentScaleFactor,
            recommendedScaleFactor,
            &applyRecommendedScale,
            &disablePrompt);
        if (!promptHandled)
        {
            warn << scaleDecisionEvent
                << "[main] 启动缩放推荐弹窗失败或被关闭，保持当前设置。"
                << eol;
            return;
        }

        bool hasSettingsChanged = false;
        if (applyRecommendedScale)
        {
            startupSettings->startupWindowScaleFactor = recommendedScaleFactor;
            hasSettingsChanged = true;
        }
        if (disablePrompt)
        {
            startupSettings->startupScaleRecommendPromptDisabled = true;
            hasSettingsChanged = true;
        }

        if (!hasSettingsChanged)
        {
            info << scaleDecisionEvent
                << "[main] 用户未修改缩放建议配置。"
                << eol;
            return;
        }

        QString saveErrorText;
        const bool saveOk = ks::settings::saveAppearanceSettings(*startupSettings, &saveErrorText);
        if (!saveOk)
        {
            err << scaleDecisionEvent
                << "[main] 启动缩放推荐配置保存失败, error="
                << saveErrorText.toStdString()
                << eol;
            return;
        }

        info << scaleDecisionEvent
            << "[main] 启动缩放推荐配置已更新, logicalWidth="
            << logicalClientWidth
            << ", current="
            << currentScaleFactor
            << ", recommended="
            << recommendedScaleFactor
            << ", applied="
            << (applyRecommendedScale ? "true" : "false")
            << ", disablePrompt="
            << (startupSettings->startupScaleRecommendPromptDisabled ? "true" : "false")
            << eol;
    }

    // applyQtScaleFactorEnvironment 作用：
    // - 在 QApplication 创建前设置 QT_SCALE_FACTOR；
    // - 该值会参与整个 Qt 主窗口缩放计算。
    // 入参 scaleFactor：目标缩放因子（0.50~2.00）。
    void applyQtScaleFactorEnvironment(const double scaleFactor)
    {
        const double normalizedScaleFactor = ks::settings::normalizeWindowScaleFactor(scaleFactor);
        const QByteArray scaleFactorText = QByteArray::number(normalizedScaleFactor, 'f', 3);
        qputenv("QT_SCALE_FACTOR", scaleFactorText);

        kLogEvent scaleEnvEvent;
        info << scaleEnvEvent
            << "[main] 已设置 QT_SCALE_FACTOR="
            << scaleFactorText.toStdString()
            << eol;
    }

    // loadSharedIconFromResource 作用：
    // - 按指定尺寸从可执行文件资源加载共享图标；
    // - 供主窗口设置任务栏/标题栏图标。
    // 入参 widthValue/heightValue：目标图标尺寸。
    // 返回：图标句柄；失败返回 nullptr。
    HICON loadSharedIconFromResource(const int widthValue, const int heightValue)
    {
        return reinterpret_cast<HICON>(
            ::LoadImageW(
                ::GetModuleHandleW(nullptr),
                kNativeAppIconResourceName,
                IMAGE_ICON,
                widthValue,
                heightValue,
                LR_DEFAULTCOLOR | LR_SHARED));
    }

    // applyNativeAppIconToWidget 作用：
    // - 把 AppIcon.rc 图标显式同步到 Qt 顶层窗口；
    // - 修复“窗口图标未跟随 exe 图标”的问题。
    // 入参 targetWidget：目标顶层窗口。
    void applyNativeAppIconToWidget(QWidget* targetWidget)
    {
        if (targetWidget == nullptr)
        {
            return;
        }

        const HWND targetWindowHandle = reinterpret_cast<HWND>(targetWidget->winId());
        if (targetWindowHandle == nullptr)
        {
            return;
        }

        const int bigIconWidth = ::GetSystemMetrics(SM_CXICON);
        const int bigIconHeight = ::GetSystemMetrics(SM_CYICON);
        const int smallIconWidth = ::GetSystemMetrics(SM_CXSMICON);
        const int smallIconHeight = ::GetSystemMetrics(SM_CYSMICON);

        HICON bigIconHandle = loadSharedIconFromResource(bigIconWidth, bigIconHeight);
        HICON smallIconHandle = loadSharedIconFromResource(smallIconWidth, smallIconHeight);

        if (bigIconHandle != nullptr)
        {
            ::SendMessageW(
                targetWindowHandle,
                WM_SETICON,
                static_cast<WPARAM>(ICON_BIG),
                reinterpret_cast<LPARAM>(bigIconHandle));
        }
        if (smallIconHandle != nullptr)
        {
            ::SendMessageW(
                targetWindowHandle,
                WM_SETICON,
                static_cast<WPARAM>(ICON_SMALL),
                reinterpret_cast<LPARAM>(smallIconHandle));
            ::SendMessageW(
                targetWindowHandle,
                WM_SETICON,
                static_cast<WPARAM>(ICON_SMALL2),
                reinterpret_cast<LPARAM>(smallIconHandle));
        }
    }

    // FirstFrameSplashHider 作用：
    // - 监听主窗口首帧相关事件（Show/Paint/UpdateRequest）；
    // - 一旦确认主窗口已可见，立即隐藏启动页；
    // - 提供定时兜底，规避平台消息差异导致的漏判。
    class FirstFrameSplashHider final : public QObject
    {
    public:
        // 构造函数作用：
        // - 保存主窗口和启动页控制器指针；
        // - 后续由 eventFilter 与定时器统一执行隐藏。
        // 入参 targetWindow：主窗口对象。
        // 入参 splashWindow：启动页控制器对象。
        FirstFrameSplashHider(
            MainWindow* targetWindow,
            kStartupSplash* splashWindow)
            : QObject(targetWindow)
            , m_targetWindow(targetWindow)
            , m_targetCentralWidget(targetWindow != nullptr ? targetWindow->centralWidget() : nullptr)
            , m_splashWindow(splashWindow)
        {
            // 启动兜底：窗口进入可见状态后若事件链漏判，1.5 秒后也强制收起启动页。
            QTimer::singleShot(1500, this, [this]()
                {
                    if (m_hidden || m_targetWindow == nullptr)
                    {
                        return;
                    }
                    if (m_targetWindow->isVisible() && !m_targetWindow->isMinimized())
                    {
                        hideSplashAndDetach();
                    }
                });
        }

    protected:
        // eventFilter 作用：
        // - 捕获首帧相关事件后隐藏启动页；
        // - 同时监听主窗口与中央控件，避免仅监听主窗导致漏判。
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (m_hidden || eventObject == nullptr || m_targetWindow == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            // watchedTargetWindow 作用：判断本次事件是否来自主窗口。
            const bool watchedTargetWindow = (watchedObject == m_targetWindow);
            // watchedTargetCentral 作用：判断本次事件是否来自中央内容控件。
            const bool watchedTargetCentral = (watchedObject == m_targetCentralWidget);
            if (!watchedTargetWindow && !watchedTargetCentral)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            // eventType 作用：统一承接本次事件类型，便于扩展首帧判定条件。
            const QEvent::Type eventType = eventObject->type();
            // isFrameReadySignal 作用：判定是否是“窗口首帧已到”相关事件。
            const bool isFrameReadySignal =
                eventType == QEvent::Show ||
                eventType == QEvent::Paint ||
                eventType == QEvent::UpdateRequest;

            if (!m_hidden
                && isFrameReadySignal
                && m_targetWindow->isVisible()
                && !m_targetWindow->isMinimized())
            {
                hideSplashAndDetach();
            }
            return QObject::eventFilter(watchedObject, eventObject);
        }

    private:
        // hideSplashAndDetach 作用：
        // - 执行一次性隐藏动作并移除事件过滤器；
        // - 保证启动页只隐藏一次，避免重复调用。
        void hideSplashAndDetach()
        {
            m_hidden = true;
            if (m_splashWindow != nullptr)
            {
                m_splashWindow->progress("启动完成", 100);
                m_splashWindow->hide();
            }
            if (m_targetWindow != nullptr)
            {
                m_targetWindow->removeEventFilter(this);
            }
            if (m_targetCentralWidget != nullptr)
            {
                m_targetCentralWidget->removeEventFilter(this);
            }
        }

        MainWindow* m_targetWindow = nullptr;     // m_targetWindow：主窗口对象指针。
        QWidget* m_targetCentralWidget = nullptr; // m_targetCentralWidget：主窗口中央内容控件指针。
        kStartupSplash* m_splashWindow = nullptr; // m_splashWindow：启动页控制器指针。
        bool m_hidden = false;                    // m_hidden：是否已经执行过隐藏动作。
    };
}

int main(int argc, char* argv[])
{
    // 启动流程：
    // 1) 初始化 DPI 感知；
    // 2) 读取配置并处理推荐缩放；
    // 3) 按配置处理自动提权；
    // 4) 设置 QT_SCALE_FACTOR；
    // 5) 显示 Framework 启动页并创建主窗口。
    initializeProcessDpiAwareness();

    // startupSettings 作用：缓存本次启动所需配置快照。
    ks::settings::AppearanceSettings startupSettings = ks::settings::loadAppearanceSettings();
    maybeApplyStartupScaleRecommendation(&startupSettings);

    if (startupSettings.autoRequestAdminOnStartup && !isCurrentProcessElevated())
    {
        const bool elevatedLaunchStarted = tryLaunchElevatedSelfBeforeSplash();
        if (elevatedLaunchStarted)
        {
            return 0;
        }
    }

    applyQtScaleFactorEnvironment(startupSettings.startupWindowScaleFactor);

    const bool splashReady = kSplash.show();
    if (splashReady)
    {
        kSplash.progress("正在初始化 Qt 运行时...", 6);
    }

    QApplication app(argc, argv);
    ks::ui::InstallGlobalMessageBoxTheme(&app);

    // startupProgressCallback 作用：
    // - 接收 MainWindow 构造阶段回调；
    // - 持续回写启动页文案和进度百分比。
    const MainWindow::StartupProgressCallback startupProgressCallback =
        [splashReady](const int progressPercent, const QString& statusText)
        {
            if (!splashReady)
            {
                return;
            }
            kSplash.progress(statusText.toUtf8().toStdString(), progressPercent);
        };

    if (splashReady)
    {
        kSplash.progress("正在准备应用环境...", 18);
        kSplash.progress("正在创建主窗口...", 28);
    }

    MainWindow window(nullptr, startupProgressCallback);
    FirstFrameSplashHider firstFrameHider(&window, &kSplash);
    window.installEventFilter(&firstFrameHider);
    if (window.centralWidget() != nullptr)
    {
        window.centralWidget()->installEventFilter(&firstFrameHider);
    }

    if (splashReady)
    {
        kSplash.progress("正在显示主窗口...", 95);
    }

    if (startupSettings.launchMaximizedOnStartup)
    {
        window.showMaximized();
    }
    else
    {
        window.show();
    }
    applyNativeAppIconToWidget(&window);

    if (splashReady)
    {
        kSplash.progress("正在等待首帧渲染...", 98);

        // 兜底策略：
        // - 若首帧事件异常未触发；
        // - 4 秒后强制隐藏启动页。
        QTimer::singleShot(4000, &window, []()
            {
                kSplash.hide();
            });
    }

    const int exitCode = app.exec();
    kSplash.hide();
    return exitCode;
}
