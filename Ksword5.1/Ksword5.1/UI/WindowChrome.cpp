#include "WindowChrome.h"

#include "../theme.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QPointer>
#include <QVariant>
#include <QWidget>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")
#endif

namespace
{
#ifdef Q_OS_WIN
    // kDwmUseImmersiveDarkModeAttribute 作用：
    // - DWMWA_USE_IMMERSIVE_DARK_MODE（Win10 20H1+）；
    // - 深色模式下让原生标题栏使用系统深色绘制策略。
    constexpr DWORD kDwmUseImmersiveDarkModeAttribute = 20;
    // kDwmBorderColorAttribute 作用：
    // - DWMWA_BORDER_COLOR（Win11+），控制窗口细边框颜色。
    constexpr DWORD kDwmBorderColorAttribute = 34;
    // 系统默认圆角策略：让最大化、贴靠、辅助功能及系统版本共同决定窗口外形。
    constexpr DWORD kDwmCornerPreferenceAttribute = 33;
    // kDwmCaptionColorAttribute 作用：
    // - DWMWA_CAPTION_COLOR（Win11+），直接把标题栏染成指定颜色；
    // - 这是子窗口标题栏与主题背景融为一体的关键属性。
    constexpr DWORD kDwmCaptionColorAttribute = 35;
    // kDwmTextColorAttribute 作用：
    // - DWMWA_CAPTION_TEXT_COLOR（Win11+），控制标题栏文字颜色。
    constexpr DWORD kDwmTextColorAttribute = 36;

    // kChromeAppliedKeyPropertyName 作用：
    // - 缓存上次应用到窗口的主题指纹（深浅色+标题栏色+文字色）；
    // - 主题未变化时跳过重复的 DWM 调用。
    constexpr const char* kChromeAppliedKeyPropertyName = "ksword_window_chrome_applied_key";

    // toColorRef 作用：QColor -> DWM 需要的 0x00BBGGRR COLORREF。
    COLORREF toColorRef(const QColor& colorValue)
    {
        return RGB(colorValue.red(), colorValue.green(), colorValue.blue());
    }
#endif

    // shouldApplyChromeToWidget 作用：
    // - 判断某个窗口是否应由本模块染色标题栏；
    // - 无边框窗口（主窗口、启动页、通知卡片）没有原生标题栏，跳过；
    // - Popup/ToolTip 等瞬态表面同样没有标题栏，跳过。
    bool shouldApplyChromeToWidget(const QWidget* widgetPointer)
    {
        if (widgetPointer == nullptr || !widgetPointer->isWindow())
        {
            return false;
        }
        const Qt::WindowFlags windowFlags = widgetPointer->windowFlags();
        if ((windowFlags & Qt::FramelessWindowHint) != 0)
        {
            return false;
        }
        const Qt::WindowType windowType =
            static_cast<Qt::WindowType>(static_cast<int>(windowFlags & Qt::WindowType_Mask));
        switch (windowType)
        {
        case Qt::Popup:
        case Qt::ToolTip:
        case Qt::SplashScreen:
        case Qt::Desktop:
            return false;
        default:
            break;
        }
        return true;
    }

    // WindowChromeStyler 作用：
    // - QApplication 级事件过滤器；
    // - 顶层窗口首次显示时把主题标题栏颜色写入 DWM。
    class WindowChromeStyler final : public QObject
    {
    public:
        explicit WindowChromeStyler(QObject* parentObject)
            : QObject(parentObject)
        {
        }

        // eventFilter 作用：
        // - 监听顶层窗口 Show 事件（此时原生句柄必然已创建）；
        // - WinIdChange 兜底句柄重建场景（如切换窗口标志后）。
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (eventObject == nullptr
                || (eventObject->type() != QEvent::Show
                    && eventObject->type() != QEvent::WinIdChange))
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            QWidget* widgetPointer = qobject_cast<QWidget*>(watchedObject);
            if (widgetPointer != nullptr && shouldApplyChromeToWidget(widgetPointer))
            {
                ks::ui::ApplyWindowChrome(widgetPointer);
            }
            return QObject::eventFilter(watchedObject, eventObject);
        }
    };

    // windowChromeStylerInstance 作用：
    // - 返回标题栏主题器单例；
    // - 单例父对象绑定 QApplication，避免手动释放。
    WindowChromeStyler* windowChromeStylerInstance()
    {
        static QPointer<WindowChromeStyler> stylerInstance;
        if (stylerInstance == nullptr && qApp != nullptr)
        {
            stylerInstance = new WindowChromeStyler(qApp);
        }
        return stylerInstance.data();
    }
}

namespace ks::ui
{
    void ApplyWindowChrome(QWidget* topLevelWidget)
    {
#ifdef Q_OS_WIN
        if (!shouldApplyChromeToWidget(topLevelWidget))
        {
            return;
        }
        // 未创建原生句柄时不主动触发 winId，避免在窗口构造早期引入重入。
        if (!topLevelWidget->testAttribute(Qt::WA_WState_Created))
        {
            return;
        }
        const HWND windowHandle = reinterpret_cast<HWND>(topLevelWidget->winId());
        if (windowHandle == nullptr || ::IsWindow(windowHandle) == FALSE)
        {
            return;
        }

        const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
        // captionColor 用途：标题栏与窗口背景使用同一角色，视觉上融为一体。
        const QColor captionColor = KswordTheme::WindowColor();
        const QColor captionTextColor = KswordTheme::MainBackgroundTextColor();
        const QColor borderColor = KswordTheme::BorderColor();

        // appliedKey 用途：主题指纹，未变化时跳过重复 DWM 写入。
        const QString appliedKey = QStringLiteral("%1|%2|%3|%4")
            .arg(darkModeEnabled ? 1 : 0)
            .arg(captionColor.rgb())
            .arg(captionTextColor.rgb())
            .arg(borderColor.rgb());
        if (topLevelWidget->property(kChromeAppliedKeyPropertyName).toString() == appliedKey)
        {
            return;
        }
        topLevelWidget->setProperty(kChromeAppliedKeyPropertyName, appliedKey);

        // Win10 20H1+：深色模式绘制策略（Win11 染色属性不可用时的最低保障）。
        const BOOL immersiveDarkModeValue = darkModeEnabled ? TRUE : FALSE;
        (void)::DwmSetWindowAttribute(
            windowHandle,
            kDwmUseImmersiveDarkModeAttribute,
            &immersiveDarkModeValue,
            sizeof(immersiveDarkModeValue));

        const DWORD cornerPreference = 0; // DWMWCP_DEFAULT，不强制方角或圆角。
        (void)::DwmSetWindowAttribute(windowHandle, kDwmCornerPreferenceAttribute,
            &cornerPreference, sizeof(cornerPreference));

        // Win11+：直接染色标题栏/文字/边框；旧系统返回 E_INVALIDARG，静默忽略。
        const COLORREF captionColorValue = toColorRef(captionColor);
        (void)::DwmSetWindowAttribute(
            windowHandle,
            kDwmCaptionColorAttribute,
            &captionColorValue,
            sizeof(captionColorValue));

        const COLORREF captionTextColorValue = toColorRef(captionTextColor);
        (void)::DwmSetWindowAttribute(
            windowHandle,
            kDwmTextColorAttribute,
            &captionTextColorValue,
            sizeof(captionTextColorValue));

        const COLORREF borderColorValue = toColorRef(borderColor);
        (void)::DwmSetWindowAttribute(
            windowHandle,
            kDwmBorderColorAttribute,
            &borderColorValue,
            sizeof(borderColorValue));
#else
        Q_UNUSED(topLevelWidget);
#endif
    }

    void InstallWindowChrome(QApplication* appInstance)
    {
        if (appInstance == nullptr)
        {
            return;
        }

        WindowChromeStyler* stylerInstance = windowChromeStylerInstance();
        if (stylerInstance == nullptr)
        {
            return;
        }

        appInstance->installEventFilter(stylerInstance);
        RefreshAllWindowChrome();
    }

    void RefreshAllWindowChrome()
    {
        if (qApp == nullptr)
        {
            return;
        }

        const QWidgetList topLevelWidgetList = qApp->topLevelWidgets();
        for (QWidget* topLevelWidget : topLevelWidgetList)
        {
            ApplyWindowChrome(topLevelWidget);
        }
    }
}
