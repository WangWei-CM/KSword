#include "MainWindow.h"
#include <QMenu>
#include <QAction>
#include <QEasingCurve>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QTextEdit>
#include <QTextStream>
#include <QTabWidget>
#include <QTabBar>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QList>
#include <QPainter>
#include <QPalette>
#include <QPointF>
#include <QPixmap>
#include <QPointer>
#include <QPropertyAnimation>
#include <QRectF>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QScrollBar>
#include <QImageReader>
#include <QDialog>
#include <QMessageBox>
#include <QProcess>
#include <QToolTip>
#include <QStyleHints>
#include <QUrl>
#include <QEvent>
#include <QEventLoop>
#include <QWheelEvent>
#pragma warning(disable: 4996)
#include "UI/UI.css/UI_css.h"
#include "Framework.h"
#include "Framework/LogDockWidget.h"
#include "Framework/ProgressDockWidget.h"
#include "Framework/CustomTitleBar.h"
#include "Framework/ThemedMessageBox.h"
#include "include/ads/DockWidgetTab.h"
#include "KernelDock/KernelDock.CallbackPromptManager.h"
#include "UI/CodeEditorWidget.h"
#include "theme.h"
#include "../../shared/KswordArkLogProtocol.h"
#include <windows.h>
// 菜单栏权限按钮涉及 Windows 令牌权限查询与提权动作。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <sddl.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstring>
#include <vector>
#include <TlHelp32.h>

#pragma comment(lib, "Dwmapi.lib")

namespace
{
    // GlobalContextMenuThemeFilter 作用：
    // - 在应用层拦截所有 QMenu 的显示/样式变化事件；
    // - 对“未显式设置样式”的菜单自动套用统一主题样式，避免遗漏单点 setStyleSheet。
    class GlobalContextMenuThemeFilter final : public QObject
    {
    public:
        explicit GlobalContextMenuThemeFilter(QObject* parent = nullptr)
            : QObject(parent)
        {
        }

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (watchedObject == nullptr || eventObject == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const QEvent::Type eventType = eventObject->type();
            if (eventType != QEvent::Show)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            QMenu* menuWidget = qobject_cast<QMenu*>(watchedObject);
            if (menuWidget == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            // 仅自动处理“未显式设置样式”的菜单；已手工定制的菜单保持原样。
            // 对自动处理过的菜单，每次显示都刷新一次，保证深浅色切换后立即生效。
            const bool autoThemedByKsword =
                menuWidget->property("ksword_auto_context_menu_themed").toBool();
            const bool noExplicitMenuStyle = menuWidget->styleSheet().trimmed().isEmpty();
            if (noExplicitMenuStyle || autoThemedByKsword)
            {
                menuWidget->setStyleSheet(KswordTheme::ContextMenuStyle());
                menuWidget->setProperty("ksword_auto_context_menu_themed", true);
            }

            return QObject::eventFilter(watchedObject, eventObject);
        }
    };

    QIcon contrastIconForSelectedTab(const QIcon& sourceIcon)
    {
        if (sourceIcon.isNull())
        {
            return sourceIcon;
        }

        // 原生 QTabBar 没有 QSS 图标着色能力，这里按当前图标蒙版生成白色版本。
        const QSize iconSize(16, 16);
        QPixmap sourcePixmap = sourceIcon.pixmap(iconSize);
        if (sourcePixmap.isNull())
        {
            return sourceIcon;
        }

        QPixmap contrastPixmap(iconSize);
        contrastPixmap.fill(Qt::transparent);
        QPainter painter(&contrastPixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.drawPixmap(0, 0, sourcePixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(contrastPixmap.rect(), QColor(255, 255, 255));
        painter.end();
        return QIcon(contrastPixmap);
    }

    class GlobalTabIconContrastFilter final : public QObject
    {
    public:
        explicit GlobalTabIconContrastFilter(QObject* parent = nullptr)
            : QObject(parent)
        {
        }

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (watchedObject == nullptr || eventObject == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            QTabBar* tabBar = qobject_cast<QTabBar*>(watchedObject);
            if (tabBar == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const QEvent::Type eventType = eventObject->type();
            if (eventType == QEvent::Destroy)
            {
                m_originalIconsByTabBar.erase(tabBar);
                m_displayIconsByTabBar.erase(tabBar);
                m_contrastIconsByTabBar.erase(tabBar);
                return QObject::eventFilter(watchedObject, eventObject);
            }
            if (eventType == QEvent::Show
                || eventType == QEvent::Polish
                || eventType == QEvent::StyleChange)
            {
                configureAdaptiveTabBar(tabBar);
            }
            if (eventType != QEvent::Show
                && eventType != QEvent::Paint
                && eventType != QEvent::Resize)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            refreshTabBarIcons(tabBar);
            return QObject::eventFilter(watchedObject, eventObject);
        }

    private:
        void configureAdaptiveTabBar(QTabBar* tabBar) const
        {
            if (tabBar == nullptr || tabBar->property("ksword_adaptive_tabbar_configured").toBool())
            {
                return;
            }

            tabBar->setProperty("ksword_adaptive_tabbar_configured", true);
            tabBar->setExpanding(false);
            tabBar->setUsesScrollButtons(true);
            tabBar->setElideMode(Qt::ElideNone);
        }

        void refreshTabBarIcons(QTabBar* tabBar)
        {
            if (tabBar == nullptr || tabBar->count() <= 0)
            {
                return;
            }

            QList<QIcon>& originalIconList = m_originalIconsByTabBar[tabBar];
            QList<QIcon>& displayIconList = m_displayIconsByTabBar[tabBar];
            QList<QIcon>& contrastIconList = m_contrastIconsByTabBar[tabBar];
            while (originalIconList.size() < tabBar->count())
            {
                originalIconList.push_back(QIcon());
                displayIconList.push_back(QIcon());
                contrastIconList.push_back(QIcon());
            }
            while (originalIconList.size() > tabBar->count())
            {
                originalIconList.removeLast();
                displayIconList.removeLast();
                contrastIconList.removeLast();
            }

            const int selectedIndex = tabBar->currentIndex();
            for (int tabIndex = 0; tabIndex < tabBar->count(); ++tabIndex)
            {
                const QIcon currentIcon = tabBar->tabIcon(tabIndex);
                const bool currentIconWasAppliedByFilter = !displayIconList[tabIndex].isNull()
                    && currentIcon.cacheKey() == displayIconList[tabIndex].cacheKey();
                if (originalIconList[tabIndex].isNull())
                {
                    originalIconList[tabIndex] = currentIcon;
                    contrastIconList[tabIndex] = QIcon();
                }
                else if (tabIndex != selectedIndex
                    && !currentIconWasAppliedByFilter
                    && !currentIcon.isNull()
                    && currentIcon.cacheKey() != originalIconList[tabIndex].cacheKey())
                {
                    originalIconList[tabIndex] = currentIcon;
                    contrastIconList[tabIndex] = QIcon();
                }

                const QIcon originalIcon = originalIconList[tabIndex];
                if (originalIcon.isNull())
                {
                    continue;
                }
                if (contrastIconList[tabIndex].isNull())
                {
                    contrastIconList[tabIndex] = contrastIconForSelectedTab(originalIcon);
                }

                const QIcon displayIcon = tabIndex == selectedIndex
                    ? contrastIconList[tabIndex]
                    : originalIcon;
                if (currentIcon.cacheKey() != displayIcon.cacheKey())
                {
                    tabBar->setTabIcon(tabIndex, displayIcon);
                }
                displayIconList[tabIndex] = displayIcon;
            }
        }

        std::unordered_map<QTabBar*, QList<QIcon>> m_originalIconsByTabBar;
        std::unordered_map<QTabBar*, QList<QIcon>> m_displayIconsByTabBar;
        std::unordered_map<QTabBar*, QList<QIcon>> m_contrastIconsByTabBar;
    };

    class GlobalSliderWheelFilter final : public QObject
    {
    public:
        explicit GlobalSliderWheelFilter(QObject* parent = nullptr)
            : QObject(parent)
        {
        }

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (watchedObject == nullptr || eventObject == nullptr || eventObject->type() != QEvent::Wheel)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(eventObject);
            if (trySmoothScroll(watchedObject, wheelEvent))
            {
                return true;
            }

            QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
            const bool sliderWheelAdjustEnabled = appInstance != nullptr
                && appInstance->property("ksword_slider_wheel_adjust_enabled").toBool();
            if (sliderWheelAdjustEnabled)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            // 只禁止 QSlider 这类“数值滑块”的滚轮调值，不能拦截 QScrollBar，否则页面滚动会失效。
            if (qobject_cast<QScrollBar*>(watchedObject) != nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            if (qobject_cast<QAbstractSlider*>(watchedObject) == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            eventObject->ignore();
            return true;
        }

    private:
        bool trySmoothScroll(QObject* watchedObject, QWheelEvent* wheelEvent)
        {
            if (wheelEvent == nullptr || wheelEvent->modifiers().testFlag(Qt::ControlModifier))
            {
                return false;
            }

            QScrollBar* targetScrollBar = findTargetScrollBar(watchedObject, wheelEvent);
            if (targetScrollBar == nullptr || targetScrollBar->minimum() == targetScrollBar->maximum())
            {
                return false;
            }

            const int wheelDelta = !wheelEvent->pixelDelta().isNull()
                ? wheelEvent->pixelDelta().y()
                : wheelEvent->angleDelta().y() / 8;
            if (wheelDelta == 0)
            {
                return false;
            }

            // 动画步长保持克制：让滚轮有平滑过渡，但不拖慢大量表格的快速浏览。
            const int lineStep = std::max(18, targetScrollBar->singleStep() * 3);
            const int targetValue = std::clamp(
                targetScrollBar->value() - wheelDelta * lineStep / 15,
                targetScrollBar->minimum(),
                targetScrollBar->maximum());
            animateScrollBar(targetScrollBar, targetValue);
            wheelEvent->accept();
            return true;
        }

        QScrollBar* findTargetScrollBar(QObject* watchedObject, QWheelEvent* wheelEvent) const
        {
            if (qobject_cast<QAbstractSlider*>(watchedObject) != nullptr
                && qobject_cast<QScrollBar*>(watchedObject) == nullptr)
            {
                return nullptr;
            }

            QScrollBar* directScrollBar = qobject_cast<QScrollBar*>(watchedObject);
            if (directScrollBar != nullptr)
            {
                return directScrollBar;
            }

            QWidget* sourceWidget = qobject_cast<QWidget*>(watchedObject);
            QWidget* currentWidget = sourceWidget;
            while (currentWidget != nullptr)
            {
                QAbstractScrollArea* scrollArea = qobject_cast<QAbstractScrollArea*>(currentWidget);
                if (scrollArea != nullptr)
                {
                    return chooseScrollBar(scrollArea, wheelEvent);
                }
                currentWidget = currentWidget->parentWidget();
            }
            return nullptr;
        }

        QScrollBar* chooseScrollBar(QAbstractScrollArea* scrollArea, QWheelEvent* wheelEvent) const
        {
            if (scrollArea == nullptr || wheelEvent == nullptr)
            {
                return nullptr;
            }

            if (std::abs(wheelEvent->angleDelta().x()) > std::abs(wheelEvent->angleDelta().y()))
            {
                QScrollBar* horizontalScrollBar = scrollArea->horizontalScrollBar();
                if (horizontalScrollBar != nullptr && horizontalScrollBar->minimum() != horizontalScrollBar->maximum())
                {
                    return horizontalScrollBar;
                }
            }
            return scrollArea->verticalScrollBar();
        }

        void animateScrollBar(QScrollBar* targetScrollBar, const int targetValue)
        {
            if (targetScrollBar == nullptr)
            {
                return;
            }

            QPointer<QPropertyAnimation>& animationRef = m_scrollAnimationByBar[targetScrollBar];
            if (animationRef == nullptr)
            {
                animationRef = new QPropertyAnimation(targetScrollBar, "value", targetScrollBar);
                animationRef->setDuration(110);
                animationRef->setEasingCurve(QEasingCurve::OutCubic);
            }

            animationRef->stop();
            animationRef->setStartValue(targetScrollBar->value());
            animationRef->setEndValue(targetValue);
            animationRef->start();
        }

        std::unordered_map<QScrollBar*, QPointer<QPropertyAnimation>> m_scrollAnimationByBar;
    };

    // ensureGlobalContextMenuThemeFilterInstalled 作用：
    // - 安装一次应用级 QMenu 主题过滤器；
    // - 让所有后续创建的右键菜单自动获得深浅色背景兜底。
    void ensureGlobalContextMenuThemeFilterInstalled()
    {
        QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (appInstance == nullptr)
        {
            return;
        }

        static GlobalContextMenuThemeFilter* contextMenuThemeFilter = nullptr;
        if (contextMenuThemeFilter == nullptr)
        {
            contextMenuThemeFilter = new GlobalContextMenuThemeFilter(appInstance);
            appInstance->installEventFilter(contextMenuThemeFilter);
        }
    }

    void ensureGlobalSliderWheelFilterInstalled()
    {
        QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (appInstance == nullptr)
        {
            return;
        }

        static GlobalTabIconContrastFilter* tabIconContrastFilter = nullptr;
        if (tabIconContrastFilter == nullptr)
        {
            tabIconContrastFilter = new GlobalTabIconContrastFilter(appInstance);
            appInstance->installEventFilter(tabIconContrastFilter);
        }

        static GlobalSliderWheelFilter* sliderWheelFilter = nullptr;
        if (sliderWheelFilter == nullptr)
        {
            sliderWheelFilter = new GlobalSliderWheelFilter(appInstance);
            appInstance->installEventFilter(sliderWheelFilter);
        }
    }

    // kTooltipStyleBeginMarker / kTooltipStyleEndMarker 作用：
    // - 在 QApplication 样式表中标记“Tooltip 主题片段”的起止位置；
    // - 便于主题切换时精准替换旧 Tooltip 样式，避免重复拼接。
    constexpr const char* kTooltipStyleBeginMarker = "/*KSWORD_TOOLTIP_STYLE_BEGIN*/";
    constexpr const char* kTooltipStyleEndMarker = "/*KSWORD_TOOLTIP_STYLE_END*/";
    // kContextMenuStyleBeginMarker / kContextMenuStyleEndMarker 作用：
    // - 在 QApplication 样式表中标记“右键菜单主题片段”的起止位置；
    // - 便于主题切换时替换旧菜单样式，避免重复拼接与样式污染。
    constexpr const char* kContextMenuStyleBeginMarker = "/*KSWORD_CONTEXT_MENU_STYLE_BEGIN*/";
    constexpr const char* kContextMenuStyleEndMarker = "/*KSWORD_CONTEXT_MENU_STYLE_END*/";
    // kDeferredDockLoadIntervalMs 作用：
    // - 控制“显示后补载”节流间隔；
    // - 避免 0ms 连续补载把 UI 线程再次打满。
    constexpr int kDeferredDockLoadIntervalMs = 60;
    // kDwmUseImmersiveDarkModeAttribute 作用：
    // - 兼容不同 SDK 头文件是否声明该枚举值；
    // - 用于告诉 DWM 当前窗口应按深色还是浅色边框策略处理。
    constexpr DWORD kDwmUseImmersiveDarkModeAttribute = 20;
    // kDwmBorderColorAttribute 作用：
    // - 指向 Win11 边框颜色属性编号；
    // - 用于关闭系统默认白色可见边框。
    constexpr DWORD kDwmBorderColorAttribute = 34;
    // kDwmColorNone 作用：
    // - 传给 DWMWA_BORDER_COLOR 后表示“不绘制可见边框”；
    // - 保留阴影与缩放能力，仅移除闪白边线。
    constexpr DWORD kDwmColorNone = 0xFFFFFFFE;
    constexpr wchar_t kR0DriverServiceName[] = L"KswordARK";
    constexpr wchar_t kR0DriverDisplayName[] = L"KswordARK Driver Service";
    constexpr DWORD kR0ServiceWaitTimeoutMs = 9000;
    constexpr int kR0LogConnectRetrySleepMs = 260;
    constexpr int kR0LogIdlePollSleepMs = 120;
    constexpr char kR0LogPrefixDebug[] = "[Debug]";
    constexpr char kR0LogPrefixInfo[] = "[Info]";
    constexpr char kR0LogPrefixWarn[] = "[Warn]";
    constexpr char kR0LogPrefixError[] = "[Error]";
    constexpr char kR0LogPrefixFatal[] = "[Fatal]";

    // sharedR0DriverLogEvent 作用：
    // - 统一承载 R3 进程内“驱动日志转发”链路的 GUID；
    // - 满足“所有驱动输出走同一个 kLogEvent”要求。
    kLogEvent& sharedR0DriverLogEvent()
    {
        static kLogEvent sharedEvent;
        return sharedEvent;
    }

    // startsWithLiteral 作用：
    // - 判断文本是否以固定前缀开头（区分大小写）；
    // - 仅用于日志等级标签解析。
    bool startsWithLiteral(const std::string& text, const char* prefixText)
    {
        if (prefixText == nullptr)
        {
            return false;
        }

        const std::size_t prefixLength = std::strlen(prefixText);
        if (text.size() < prefixLength)
        {
            return false;
        }
        return text.compare(0, prefixLength, prefixText) == 0;
    }

    class ScopedServiceHandle final
    {
    public:
        ScopedServiceHandle() = default;
        explicit ScopedServiceHandle(const SC_HANDLE handle)
            : m_handle(handle)
        {
        }

        ScopedServiceHandle(const ScopedServiceHandle&) = delete;
        ScopedServiceHandle& operator=(const ScopedServiceHandle&) = delete;

        ScopedServiceHandle(ScopedServiceHandle&& other) noexcept
            : m_handle(other.m_handle)
        {
            other.m_handle = nullptr;
        }

        ScopedServiceHandle& operator=(ScopedServiceHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = other.m_handle;
                other.m_handle = nullptr;
            }
            return *this;
        }

        ~ScopedServiceHandle()
        {
            reset();
        }

        void reset(const SC_HANDLE newHandle = nullptr)
        {
            if (m_handle != nullptr)
            {
                ::CloseServiceHandle(m_handle);
            }
            m_handle = newHandle;
        }

        SC_HANDLE get() const
        {
            return m_handle;
        }

        bool isValid() const
        {
            return m_handle != nullptr;
        }

    private:
        SC_HANDLE m_handle = nullptr;
    };

    QString formatWin32ErrorText(const DWORD errorCode)
    {
        if (errorCode == ERROR_SUCCESS)
        {
            return QStringLiteral("成功");
        }

        LPWSTR messageBuffer = nullptr;
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&messageBuffer),
            0,
            nullptr);

        QString messageText;
        if (messageLength > 0 && messageBuffer != nullptr)
        {
            messageText = QString::fromWCharArray(messageBuffer, static_cast<int>(messageLength)).trimmed();
        }
        if (messageBuffer != nullptr)
        {
            ::LocalFree(messageBuffer);
        }
        if (messageText.isEmpty())
        {
            messageText = QStringLiteral("未知系统错误");
        }
        return messageText;
    }

    QString serviceStateToText(const DWORD serviceState)
    {
        switch (serviceState)
        {
        case SERVICE_STOPPED: return QStringLiteral("STOPPED");
        case SERVICE_START_PENDING: return QStringLiteral("START_PENDING");
        case SERVICE_STOP_PENDING: return QStringLiteral("STOP_PENDING");
        case SERVICE_RUNNING: return QStringLiteral("RUNNING");
        case SERVICE_CONTINUE_PENDING: return QStringLiteral("CONTINUE_PENDING");
        case SERVICE_PAUSE_PENDING: return QStringLiteral("PAUSE_PENDING");
        case SERVICE_PAUSED: return QStringLiteral("PAUSED");
        default: return QStringLiteral("UNKNOWN");
        }
    }

    bool queryServiceStatus(const SC_HANDLE serviceHandle, SERVICE_STATUS_PROCESS& statusOut, DWORD& errorCodeOut)
    {
        errorCodeOut = ERROR_SUCCESS;
        DWORD bytesNeeded = 0;
        if (::QueryServiceStatusEx(
            serviceHandle,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&statusOut),
            sizeof(statusOut),
            &bytesNeeded) == FALSE)
        {
            errorCodeOut = ::GetLastError();
            return false;
        }
        return true;
    }

    bool waitServiceState(
        const SC_HANDLE serviceHandle,
        const DWORD targetState,
        const DWORD timeoutMs,
        SERVICE_STATUS_PROCESS& latestStatusOut,
        DWORD& errorCodeOut)
    {
        errorCodeOut = ERROR_SUCCESS;
        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        while (true)
        {
            if (!queryServiceStatus(serviceHandle, latestStatusOut, errorCodeOut))
            {
                return false;
            }
            if (latestStatusOut.dwCurrentState == targetState)
            {
                return true;
            }
            if (::GetTickCount64() >= deadline)
            {
                return false;
            }

            DWORD waitMs = latestStatusOut.dwWaitHint / 10;
            if (waitMs < 120)
            {
                waitMs = 120;
            }
            if (waitMs > 500)
            {
                waitMs = 500;
            }
            ::Sleep(waitMs);
        }
    }

    bool isRunningLikeServiceState(const DWORD serviceState)
    {
        return serviceState == SERVICE_RUNNING ||
            serviceState == SERVICE_START_PENDING ||
            serviceState == SERVICE_CONTINUE_PENDING;
    }

    // enableCurrentProcessPrivilege 作用：
    // - 尝试为当前进程启用指定权限（例如 SeLoadDriverPrivilege）；
    // - 用于 NtUnloadDriver 调用前的权限准备。
    bool enableCurrentProcessPrivilege(const wchar_t* const privilegeName, DWORD* const errorCodeOut)
    {
        if (errorCodeOut != nullptr)
        {
            *errorCodeOut = ERROR_SUCCESS;
        }
        if (privilegeName == nullptr || privilegeName[0] == L'\0')
        {
            if (errorCodeOut != nullptr)
            {
                *errorCodeOut = ERROR_INVALID_PARAMETER;
            }
            return false;
        }

        HANDLE tokenHandle = nullptr;
        if (::OpenProcessToken(
            ::GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &tokenHandle) == FALSE)
        {
            if (errorCodeOut != nullptr)
            {
                *errorCodeOut = ::GetLastError();
            }
            return false;
        }

        LUID privilegeLuid{};
        if (::LookupPrivilegeValueW(nullptr, privilegeName, &privilegeLuid) == FALSE)
        {
            if (errorCodeOut != nullptr)
            {
                *errorCodeOut = ::GetLastError();
            }
            ::CloseHandle(tokenHandle);
            return false;
        }

        TOKEN_PRIVILEGES tokenPrivileges{};
        tokenPrivileges.PrivilegeCount = 1;
        tokenPrivileges.Privileges[0].Luid = privilegeLuid;
        tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (::AdjustTokenPrivileges(
            tokenHandle,
            FALSE,
            &tokenPrivileges,
            0,
            nullptr,
            nullptr) == FALSE)
        {
            if (errorCodeOut != nullptr)
            {
                *errorCodeOut = ::GetLastError();
            }
            ::CloseHandle(tokenHandle);
            return false;
        }

        const DWORD adjustError = ::GetLastError();
        ::CloseHandle(tokenHandle);
        if (adjustError != ERROR_SUCCESS)
        {
            if (errorCodeOut != nullptr)
            {
                *errorCodeOut = adjustError;
            }
            return false;
        }

        return true;
    }

    // tryNtUnloadDriverByServiceName 作用：
    // - 通过 NtUnloadDriver 直接尝试卸载指定服务名对应的驱动；
    // - 返回 true 表示 NTSTATUS 成功（>=0）。
    bool tryNtUnloadDriverByServiceName(
        const wchar_t* const serviceName,
        long* const ntStatusOut)
    {
        if (ntStatusOut != nullptr)
        {
            *ntStatusOut = 0L;
        }
        if (serviceName == nullptr || serviceName[0] == L'\0')
        {
            return false;
        }

        const HMODULE ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdllModule == nullptr)
        {
            return false;
        }

        using NtUnloadDriverFn = long (NTAPI*)(PUNICODE_STRING);
        const NtUnloadDriverFn ntUnloadDriverFn =
            reinterpret_cast<NtUnloadDriverFn>(::GetProcAddress(ntdllModule, "NtUnloadDriver"));
        if (ntUnloadDriverFn == nullptr)
        {
            return false;
        }

        std::wstring registryServicePath = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
        registryServicePath += serviceName;

        UNICODE_STRING registryPathUnicode{};
        registryPathUnicode.Buffer = const_cast<PWSTR>(registryServicePath.c_str());
        registryPathUnicode.Length = static_cast<USHORT>(registryServicePath.size() * sizeof(wchar_t));
        registryPathUnicode.MaximumLength = registryPathUnicode.Length;

        const long ntStatus = ntUnloadDriverFn(&registryPathUnicode);
        if (ntStatusOut != nullptr)
        {
            *ntStatusOut = ntStatus;
        }
        return ntStatus >= 0;
    }

    // buildPrivilegeButtonStyle 作用：
    // - 按“当前是否具备权限”生成按钮样式；
    // - true  -> 蓝底白字；
    // - false -> 白底蓝字。
    QString buildPrivilegeButtonStyle(const bool activeState)
    {
        const QString backgroundColor = activeState
            ? KswordTheme::PrimaryBlueHex
            : KswordTheme::SurfaceHex();
        const QString textColor = activeState
            ? QStringLiteral("#FFFFFF")
            : (KswordTheme::IsDarkModeEnabled() ? KswordTheme::TextPrimaryHex() : KswordTheme::PrimaryBlueHex);
        const QString hoverColor = activeState
            ? KswordTheme::PrimaryBlueSolidHoverHex()
            : KswordTheme::PrimaryBlueSolidHoverHex();
        return QStringLiteral(
            "QPushButton {"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:3px;"
            "  padding:2px 8px;"
            "  font-weight:600;"
            "}"
            "QPushButton:hover {"
            "  background:%4;"
            "  color:#FFFFFF;"
            "  border:1px solid %4;"
            "}"
            "QPushButton:pressed {"
            "  background:%5;"
            "  color:#FFFFFF;"
            "}")
            .arg(backgroundColor)
            .arg(textColor)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(hoverColor)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // buildR0ButtonStyle 作用：
    // - R0 专用样式；
    // - true  -> 蓝底，字体按深浅主题自动黑/白；
    // - false -> 黑/白底蓝字。
    QString buildR0ButtonStyle(const bool activeState)
    {
        const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
        const QString adaptiveTextColor = QStringLiteral("#FFFFFF");
        const QString backgroundColor = activeState
            ? KswordTheme::PrimaryBlueHex
            : KswordTheme::SurfaceHex();
        const QString textColor = activeState
            ? adaptiveTextColor
            : KswordTheme::PrimaryBlueHex;
        const QString hoverColor = activeState
            ? KswordTheme::PrimaryBlueSolidHoverHex()
            : KswordTheme::PrimaryBlueSubtleHex();
        const QString pressedColor = activeState
            ? KswordTheme::PrimaryBluePressedHex
            : (darkModeEnabled ? QStringLiteral("#10283E") : QStringLiteral("#D6ECFF"));
        return QStringLiteral(
            "QPushButton {"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:3px;"
            "  padding:2px 8px;"
            "  font-weight:600;"
            "}"
            "QPushButton:hover {"
            "  background:%4;"
            "  color:%5;"
            "  border:1px solid %4;"
            "}"
            "QPushButton:pressed {"
            "  background:%6;"
            "  color:%5;"
            "}")
            .arg(backgroundColor)
            .arg(textColor)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(hoverColor)
            .arg(activeState ? adaptiveTextColor : KswordTheme::TextPrimaryColorHex())
            .arg(pressedColor);
    }

    // normalizeOpacityPercent 作用：
    // - 把透明度值限制在 0~100 范围，防止非法参数影响绘制。
    // 调用方式：生成背景画刷前调用。
    // 入参 rawOpacityPercent：原始透明度值。
    // 返回：合法透明度值。
    int normalizeOpacityPercent(const int rawOpacityPercent)
    {
        if (rawOpacityPercent < 0)
        {
            return 0;
        }
        if (rawOpacityPercent > 100)
        {
            return 100;
        }
        return rawOpacityPercent;
    }

    // buildBackgroundBrush 作用：
    // - 按“纯色底 + 可选背景图 + 透明度”组合一张画刷贴图；
    // - 用于主窗口与 Dock 管理器统一背景绘制。
    // 调用方式：MainWindow::rebuildWindowBackgroundBrush 调用。
    // 入参 windowSize：目标窗口尺寸；
    // 入参 baseColor：主题基底色（深色黑、浅色白）；
    // 入参 imagePath：背景图绝对路径；
    // 入参 imageOpacityPercent：背景图透明度（0~100）。
    // 返回：可直接设置到 QPalette::Window 的画刷。
    QBrush buildBackgroundBrush(
        const QSize& windowSize,
        const QColor& baseColor,
        const QString& imagePath,
        const int imageOpacityPercent)
    {
        const QSize safeSize = windowSize.isValid() ? windowSize : QSize(1, 1);
        QPixmap composedPixmap(safeSize);
        composedPixmap.fill(baseColor);

        if (!imagePath.trimmed().isEmpty())
        {
            QPixmap sourceImage(imagePath);
            if (!sourceImage.isNull())
            {
                QPainter painter(&composedPixmap);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.setOpacity(static_cast<double>(normalizeOpacityPercent(imageOpacityPercent)) / 100.0);

                // scaledImageSize 作用：按“覆盖整个窗口”策略计算缩放尺寸。
                QSizeF scaledImageSize = sourceImage.size();
                scaledImageSize.scale(safeSize, Qt::KeepAspectRatioByExpanding);

                const QRectF targetRect(
                    (static_cast<double>(safeSize.width()) - scaledImageSize.width()) / 2.0,
                    (static_cast<double>(safeSize.height()) - scaledImageSize.height()) / 2.0,
                    scaledImageSize.width(),
                    scaledImageSize.height());

                painter.drawPixmap(targetRect, sourceImage, QRectF(QPointF(0, 0), sourceImage.size()));
            }
        }

        return QBrush(composedPixmap);
    }

    // buildGlobalTooltipStyleBlock 作用：
    // - 生成全局 Tooltip 样式片段；
    // - 片段带起止标记，供 applyGlobalTooltipStyleBlock 做替换更新。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 darkModeEnabled：当前是否深色模式。
    // 返回：可直接拼接到 QApplication 样式表的 Tooltip 片段。
    QString buildGlobalTooltipStyleBlock(const bool darkModeEnabled)
    {
        // tooltipRule 作用：QToolTip 规则本体，统一深浅色背景与文字。
        const QString tooltipRule = QStringLiteral(
            "QToolTip{"
            "  background-color:%1 !important;"
            "  color:%2 !important;"
            "  border:1px solid %3 !important;"
            "  padding:4px 6px;"
            "  border-radius:3px;"
            "}")
            .arg(darkModeEnabled ? QStringLiteral("#101923") : QStringLiteral("#FFFFFF"))
            .arg(darkModeEnabled ? QStringLiteral("#EDF6FF") : QStringLiteral("#102336"))
            .arg(KswordTheme::BorderColorHex());

        return QStringLiteral("\n%1\n%2\n%3\n")
            .arg(QString::fromLatin1(kTooltipStyleBeginMarker))
            .arg(tooltipRule)
            .arg(QString::fromLatin1(kTooltipStyleEndMarker));
    }

    // buildGlobalContextMenuStyleBlock 作用：
    // - 生成全局右键菜单样式片段，兜底所有标准输入控件右键菜单；
    // - 修复独立顶层窗口中输入框右键菜单在深浅色切换后背景不一致的问题。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 darkModeEnabled：当前是否深色模式。
    // 返回：可直接拼接到 QApplication 样式表的 QMenu 片段。
    QString buildGlobalContextMenuStyleBlock(const bool darkModeEnabled)
    {
        const QString menuBackgroundColor = darkModeEnabled
            ? QStringLiteral("#111924")
            : QStringLiteral("#FFFFFF");
        const QString menuTextColor = darkModeEnabled
            ? QStringLiteral("#EDF6FF")
            : QStringLiteral("#102336");
        const QString menuBorderColor = darkModeEnabled
            ? QStringLiteral("#37506A")
            : QStringLiteral("#BED3E9");
        const QString disabledTextColor = darkModeEnabled
            ? QStringLiteral("#7C92A9")
            : QStringLiteral("#7E8EA0");

        const QString contextMenuRule = QStringLiteral(
            "QMenu{"
            "  background-color:%1 !important;"
            "  color:%2 !important;"
            "  border:1px solid %3 !important;"
            "  padding:3px;"
            "}"
            "QMenu::item{"
            "  color:%2 !important;"
            "  padding:5px 18px 5px 14px;"
            "  background-color:transparent !important;"
            "}"
            "QMenu::item:selected{"
            "  background-color:%4 !important;"
            "  color:#FFFFFF !important;"
            "}"
            "QMenu::item:disabled{"
            "  color:%5 !important;"
            "  background-color:transparent !important;"
            "}"
            "QMenu::separator{"
            "  height:1px;"
            "  background-color:%3;"
            "  margin:2px 6px;"
            "}")
            .arg(menuBackgroundColor)
            .arg(menuTextColor)
            .arg(menuBorderColor)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(disabledTextColor);

        return QStringLiteral("\n%1\n%2\n%3\n")
            .arg(QString::fromLatin1(kContextMenuStyleBeginMarker))
            .arg(contextMenuRule)
            .arg(QString::fromLatin1(kContextMenuStyleEndMarker));
    }

    // applyGlobalTooltipStyleBlock 作用：
    // - 把 Tooltip 样式片段写入 QApplication 样式表；
    // - 先删除旧标记片段，再追加新片段，确保切换主题后 Tooltip 立即生效。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 tooltipStyleBlock：buildGlobalTooltipStyleBlock 生成的样式片段。
    void applyGlobalTooltipStyleBlock(const QString& tooltipStyleBlock)
    {
        QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (appInstance == nullptr)
        {
            return;
        }

        // appStyleSheetText 作用：读取并更新 QApplication 当前样式表文本。
        QString appStyleSheetText = appInstance->styleSheet();
        const QString beginMarkerText = QString::fromLatin1(kTooltipStyleBeginMarker);
        const QString endMarkerText = QString::fromLatin1(kTooltipStyleEndMarker);
        const int beginMarkerIndex = appStyleSheetText.indexOf(beginMarkerText);

        if (beginMarkerIndex >= 0)
        {
            const int endMarkerIndex = appStyleSheetText.indexOf(endMarkerText, beginMarkerIndex);
            if (endMarkerIndex >= 0)
            {
                const int removeLength = (endMarkerIndex - beginMarkerIndex) + endMarkerText.length();
                appStyleSheetText.remove(beginMarkerIndex, removeLength);
            }
        }

        appStyleSheetText += tooltipStyleBlock;
        appInstance->setStyleSheet(appStyleSheetText);
    }

    // applyGlobalContextMenuStyleBlock 作用：
    // - 把 QMenu 样式片段写入 QApplication 样式表；
    // - 先删除旧标记片段，再追加新片段，确保主题切换后右键菜单立即生效。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 contextMenuStyleBlock：buildGlobalContextMenuStyleBlock 生成的样式片段。
    void applyGlobalContextMenuStyleBlock(const QString& contextMenuStyleBlock)
    {
        QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (appInstance == nullptr)
        {
            return;
        }

        QString appStyleSheetText = appInstance->styleSheet();
        const QString beginMarkerText = QString::fromLatin1(kContextMenuStyleBeginMarker);
        const QString endMarkerText = QString::fromLatin1(kContextMenuStyleEndMarker);
        const int beginMarkerIndex = appStyleSheetText.indexOf(beginMarkerText);

        if (beginMarkerIndex >= 0)
        {
            const int endMarkerIndex = appStyleSheetText.indexOf(endMarkerText, beginMarkerIndex);
            if (endMarkerIndex >= 0)
            {
                const int removeLength = (endMarkerIndex - beginMarkerIndex) + endMarkerText.length();
                appStyleSheetText.remove(beginMarkerIndex, removeLength);
            }
        }

        appStyleSheetText += contextMenuStyleBlock;
        appInstance->setStyleSheet(appStyleSheetText);
    }

    // isBackgroundImageReady 作用：
    // - 判断背景图路径是否可用（存在且可读）；
    // - 供样式层决定是否开启 Dock 全透明策略。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 rawImagePath：配置中的背景图路径（相对或绝对）。
    // 返回：true=背景图可加载；false=背景图不可用。
    bool isBackgroundImageReady(const QString& rawImagePath)
    {
        const QString resolvedImagePath = ks::settings::resolveBackgroundImagePathForLoad(rawImagePath);
        if (resolvedImagePath.trimmed().isEmpty())
        {
            return false;
        }

        const QFileInfo imageFileInfo(resolvedImagePath);
        return imageFileInfo.exists() && imageFileInfo.isFile() && imageFileInfo.isReadable();
    }
}

MainWindow::MainWindow(
    QWidget* parent,
    StartupProgressCallback startupProgressCallback)
    : QMainWindow(parent)
    , m_startupProgressCallback(startupProgressCallback)
{
    // 安装全局 QMenu 主题过滤器：
    // - 统一兜底所有右键菜单背景；
    // - 避免后续新增菜单遗漏 setStyleSheet 导致浅色模式黑底。
    ensureGlobalContextMenuThemeFilterInstalled();
    ensureGlobalSliderWheelFilterInstalled();

    // 记录主窗口启动日志，便于验证日志系统与 UI 联动是否生效。
    // 注意：使用 kLogEvent，避免与 QObject::event 命名冲突。
    kLogEvent startupEvent;
    info << startupEvent << "MainWindow 构造开始，准备初始化 Dock 系统。" << eol;

    // 启动阶段细分：
    // - 主窗口外壳；
    // - 菜单；
    // - 权限按钮；
    // - Dock 内容；
    // - 外观系统。
    // 提前读取一次外观配置，便于在 initDockWidgets 阶段确定启动默认页签的预加载策略。
    m_currentAppearanceSettings = ks::settings::loadAppearanceSettings();
    reportStartupProgress(32, QStringLiteral("正在初始化主窗口框架..."));

    // 启用无边框模式：
    // - 由自绘标题栏接管最小化/最大化/关闭/置顶操作；
    // - 保留系统菜单与最小化/最大化能力，便于原生行为兼容。
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowFlag(Qt::WindowSystemMenuHint, true);
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);

    // Dock 全局配置：
    // - 关闭所有可关闭按钮与标题栏三按钮（标签菜单/浮动/关闭）；
    // - 与用户“所有 Dock Tab 不可关闭、去掉右上角按钮”的要求一致。
    ads::CDockManager::setConfigFlag(ads::CDockManager::ActiveTabHasCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::AllTabsHaveCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, false);

    // 创建主窗口根容器：
    // - 第 0 行放自绘标题栏；
    // - 第 1 行放 ADS Dock 管理器；
    // - 统一规避 setMenuWidget 在无边框场景下的可见性不稳定问题。
    m_mainRootContainer = new QWidget(this);
    m_mainRootContainer->setObjectName(QStringLiteral("ksMainRootContainer"));
    m_mainRootContainer->setAutoFillBackground(false);
    m_mainRootContainer->setAttribute(Qt::WA_StyledBackground, false);

    m_mainRootLayout = new QVBoxLayout(m_mainRootContainer);
    m_mainRootLayout->setContentsMargins(0, 0, 0, 0);
    m_mainRootLayout->setSpacing(0);

    m_pDockManager = new ads::CDockManager(m_mainRootContainer);
    m_mainRootLayout->addWidget(m_pDockManager, 1);
    setCentralWidget(m_mainRootContainer);

    // m_backgroundRebuildTimer 用途：
    // - 合并窗口 resize 期间的背景重建请求；
    // - 避免最大化恢复为窗口化时立即同步重建整窗背景造成卡顿。
    m_backgroundRebuildTimer = new QTimer(this);
    m_backgroundRebuildTimer->setSingleShot(true);
    m_backgroundRebuildTimer->setInterval(24);
    connect(m_backgroundRebuildTimer, &QTimer::timeout, this, [this]()
        {
            rebuildWindowBackgroundBrush();
        });

    // 浮动窗口创建后立即挂接事件过滤器：
    // - 让脱离主窗口的 Dock 容器也能同步背景图与纯色底；
    // - 后续 resize/show 时统一走 MainWindow 的外观同步逻辑。
    connect(
        m_pDockManager,
        &ads::CDockManager::floatingWidgetCreated,
        this,
        [this](ads::CFloatingDockContainer* floatingWidget)
        {
            if (floatingWidget == nullptr)
            {
                return;
            }
            floatingWidget->installEventFilter(this);
            applyFloatingDockContainerAppearance(floatingWidget);
        });

    // 显式要求“最后一个窗口关闭后退出应用”。
    // 说明：用户要求主窗口关闭时进程必须结束，这里先设置 Qt 全局退出策略。
    QApplication::setQuitOnLastWindowClosed(true);

    // 设置窗口标题和大小
    setWindowTitle("Ksword5.1");
    resize(1024, 768);

    // 初始化菜单
    reportStartupProgress(38, QStringLiteral("正在初始化菜单..."));
    initMenus();

    // 初始化自绘标题栏：
    // - 替代系统标题栏；
    // - 承载置顶按钮、命令输入框和窗口控制按钮。
    reportStartupProgress(44, QStringLiteral("正在初始化自绘标题栏..."));
    initCustomTitleBar();

    // 初始化权限状态按钮：
    // - Admin / Debug / System / TI / R0；
    // - 挂载到自绘标题栏右侧，不再依赖原生菜单栏。
    reportStartupProgress(46, QStringLiteral("正在初始化权限状态按钮..."));
    initPrivilegeStatusButtons();
    startR0DriverLogPoller();

    if (CallbackPromptManager* callbackPromptManager = CallbackPromptManager::ensureGlobalManager(this))
    {
        callbackPromptManager->setHostWindow(this);
        callbackPromptManager->start();
    }

    // 初始化Dock Widgets
    reportStartupProgress(48, QStringLiteral("正在创建页面组件..."));
    initDockWidgets();

    // 设置Dock布局
    reportStartupProgress(74, QStringLiteral("正在整理 Dock 布局..."));
    setupDockLayout();

    // 初始化外观设置：
    // - 读取 JSON；
    // - 绑定系统深浅色变化；
    // - 应用窗口背景色/背景图/文本颜色。
    reportStartupProgress(84, QStringLiteral("正在初始化主题与外观..."));
    initAppearanceSettings();

    // 记录初始化完成日志，方便用户在“日志输出”面板直接看到结果。
    // 注意：使用 kLogEvent，避免与 QObject::event 命名冲突。
    kLogEvent readyEvent;
    info << readyEvent << "MainWindow 初始化完成，日志面板已加载。" << eol;
    reportStartupProgress(93, QStringLiteral("主窗口初始化完成，准备显示..."));
}

MainWindow::~MainWindow()
{
    CallbackPromptManager::shutdownGlobalManager();
    stopR0DriverLogPoller();
    // ADS会自动管理内存，无需手动删除
}

void MainWindow::reportStartupProgress(
    const int progressPercent,
    const QString& statusText) const
{
    // 安全回调策略：
    // - 未传入回调则静默跳过；
    // - 有回调时把阶段百分比与文字原样转发给主函数。
    if (!m_startupProgressCallback)
    {
        return;
    }

    m_startupProgressCallback(progressPercent, statusText);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 关闭事件日志：用于排查“窗口关闭但进程仍驻留”的问题。
    kLogEvent closeEventLog;
    info << closeEventLog << "[MainWindow] 收到关闭事件，准备退出进程。" << eol;

    // 停止权限状态定时器，避免退出阶段继续触发 UI 更新。
    if (m_privilegeStatusTimer != nullptr)
    {
        m_privilegeStatusTimer->stop();
    }
    stopR0DriverLogPoller();
    CallbackPromptManager::shutdownGlobalManager();

    // 关闭窗口时自动停止 R0 驱动：
    // - 先静默查询服务状态；
    // - 若在运行则执行“静默停驱”（仅记录日志，不弹错误框）。
    bool r0RunningBeforeExit = false;
    if (queryR0DriverServiceRunning(r0RunningBeforeExit, false) && r0RunningBeforeExit)
    {
        const bool stopOk = stopR0DriverService(true);
        kLogEvent autoStopEvent;
        if (stopOk)
        {
            info << autoStopEvent << "[MainWindow][R0] 关闭窗口时已自动停止并删除驱动服务。" << eol;
        }
        else
        {
            warn << autoStopEvent << "[MainWindow][R0] 关闭窗口时自动停驱失败（已静默处理）。" << eol;
        }
    }

    // 接受关闭事件并主动触发应用退出。
    // 这里同时调用 quit 与 exit(0)，确保事件循环尽快结束。
    if (event != nullptr)
    {
        event->accept();
    }
    QApplication::quit();
    QCoreApplication::exit(0);

    kLogEvent closeFinishLog;
    info << closeFinishLog << "[MainWindow] 已提交退出请求 (exit code=0)。" << eol;
}

bool MainWindow::eventFilter(QObject* watchedObject, QEvent* event)
{
    ads::CFloatingDockContainer* floatingWidget =
        qobject_cast<ads::CFloatingDockContainer*>(watchedObject);
    if (floatingWidget != nullptr && event != nullptr)
    {
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize)
        {
            applyFloatingDockContainerAppearance(floatingWidget);
        }
    }

    return QMainWindow::eventFilter(watchedObject, event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    scheduleWindowBackgroundBrushRebuild();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    ensureNativeFramelessWindowStyle();
    applyNativeWindowFrameVisualStyle();
    syncCustomTitleBarMaximizedState();

    if (m_deferredDockInitializationStarted)
    {
        return;
    }

    m_deferredDockInitializationStarted = true;

    // 懒加载策略修正：
    // - 旧逻辑会在主窗口 show 后继续把所有未初始化 Dock 逐个补载；
    // - 这会让“懒加载”退化成“延后但仍全量加载”，启动后首屏持续卡顿；
    // - 现在改为仅在用户真正切到对应 Dock，或代码显式跳转该 Dock 时，再初始化内容。
    // 说明：visibilityChanged / focusXXXDock / raiseStartupDockByKey 已经覆盖按需初始化入口。
    Q_UNUSED(kDeferredDockLoadIntervalMs);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);

    if (event != nullptr
        && event->type() == QEvent::WindowStateChange)
    {
        syncCustomTitleBarMaximizedState();
    }
}

void MainWindow::syncCustomTitleBarMaximizedState()
{
    if (m_customTitleBar == nullptr)
    {
        return;
    }

    // maximizedState 用途：统一记录当前窗口是否处于最大化态，供标题栏图标刷新。
    const bool maximizedState = isWindowActuallyMaximized();
    m_customTitleBar->setMaximizedState(maximizedState);
}

void MainWindow::ensureNativeFramelessWindowStyle()
{
#ifdef Q_OS_WIN
    // mainWindowHandle 用途：获取主窗口原生句柄，用于补齐 Win32 样式位。
    const HWND mainWindowHandle = reinterpret_cast<HWND>(winId());
    if (mainWindowHandle == nullptr || ::IsWindow(mainWindowHandle) == FALSE)
    {
        return;
    }

    // currentStyleValue 用途：保存当前窗口样式，供增量合并必需样式位。
    const LONG_PTR currentStyleValue = ::GetWindowLongPtrW(mainWindowHandle, GWL_STYLE);
    // removeStyleMask 用途：显式移除系统标题栏样式，防止顶部出现白色非客户区。
    const LONG_PTR removeStyleMask = WS_CAPTION;
    // requiredStyleMask 用途：无边框窗口仍需具备的系统交互能力掩码。
    const LONG_PTR requiredStyleMask = WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    // updatedStyleValue 用途：合并后的目标样式值。
    const LONG_PTR updatedStyleValue = ((currentStyleValue & ~removeStyleMask) | requiredStyleMask);

    if (updatedStyleValue != currentStyleValue)
    {
        ::SetWindowLongPtrW(mainWindowHandle, GWL_STYLE, updatedStyleValue);
        ::SetWindowPos(
            mainWindowHandle,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#endif
}

void MainWindow::applyNativeWindowFrameVisualStyle()
{
#ifdef Q_OS_WIN
    // 未创建原生窗口句柄时不主动触发 winId，避免在构造早期引入额外重入。
    if (!testAttribute(Qt::WA_WState_Created))
    {
        return;
    }

    // mainWindowHandle 用途：向 DWM 写入主窗口无边框视觉属性。
    const HWND mainWindowHandle = reinterpret_cast<HWND>(winId());
    if (mainWindowHandle == nullptr || ::IsWindow(mainWindowHandle) == FALSE)
    {
        return;
    }

    // darkModeEnabled 用途：根据当前外观配置决定 DWM 是否启用沉浸式深色边框策略。
    const bool darkModeEnabled = isDarkModeEffective(m_currentAppearanceSettings);
    // immersiveDarkModeValue 用途：DwmSetWindowAttribute 所需 BOOL 入参。
    const BOOL immersiveDarkModeValue = darkModeEnabled ? TRUE : FALSE;
    // borderColorValue 用途：要求 DWM 不再绘制系统默认白色边框。
    const DWORD borderColorValue = kDwmColorNone;

    const HRESULT darkModeResult = ::DwmSetWindowAttribute(
        mainWindowHandle,
        kDwmUseImmersiveDarkModeAttribute,
        &immersiveDarkModeValue,
        sizeof(immersiveDarkModeValue));
    const HRESULT borderColorResult = ::DwmSetWindowAttribute(
        mainWindowHandle,
        kDwmBorderColorAttribute,
        &borderColorValue,
        sizeof(borderColorValue));

    // darkModeUnsupported / borderColorUnsupported 用途：
    // - 标记当前系统仅仅是不支持新属性，而不是发生异常；
    // - 避免旧系统在每次启动时输出无意义告警。
    const bool darkModeUnsupported = (darkModeResult == E_INVALIDARG);
    const bool borderColorUnsupported = (borderColorResult == E_INVALIDARG);
    if ((!SUCCEEDED(darkModeResult) && !darkModeUnsupported)
        || (!SUCCEEDED(borderColorResult) && !borderColorUnsupported))
    {
        kLogEvent frameVisualEvent;
        warn << frameVisualEvent
            << "[MainWindow] DWM 边框样式同步失败, dark_hr=0x"
            << std::hex
            << static_cast<unsigned long>(darkModeResult)
            << ", border_hr=0x"
            << static_cast<unsigned long>(borderColorResult)
            << std::dec
            << eol;
    }
#endif
}

bool MainWindow::isWindowActuallyMaximized() const
{
    // maximizedState 用途：先基于 Qt 状态判断当前是否最大化。
    bool maximizedState = (windowState() & Qt::WindowMaximized) != 0 || isMaximized();
#ifdef Q_OS_WIN
    // 仅在窗口句柄已创建后再读取 Win32 状态，避免初始化阶段触发 winId() 重入。
    if (!testAttribute(Qt::WA_WState_Created))
    {
        return maximizedState;
    }

    // mainWindowHandle 用途：读取 Win32 Zoomed 状态，补齐 Qt 状态判定盲区。
    const HWND mainWindowHandle = reinterpret_cast<HWND>(const_cast<MainWindow*>(this)->winId());
    if (mainWindowHandle != nullptr && ::IsWindow(mainWindowHandle) != FALSE)
    {
        maximizedState = maximizedState || (::IsZoomed(mainWindowHandle) != FALSE);
    }
#endif

    return maximizedState;
}

void MainWindow::setWindowMaximizedBySystemCommand(const bool targetMaximizedState)
{
#ifdef Q_OS_WIN
    // mainWindowHandle 用途：执行原生最大化/还原时使用的窗口句柄。
    const HWND mainWindowHandle = reinterpret_cast<HWND>(winId());
    if (mainWindowHandle != nullptr && ::IsWindow(mainWindowHandle) != FALSE)
    {
        // currentMaximizedState 用途：记录当前真实最大化态，避免重复发送相同命令。
        const bool currentMaximizedState = (::IsZoomed(mainWindowHandle) != FALSE);
        if (targetMaximizedState != currentMaximizedState)
        {
            // 使用 ShowWindow 切换窗口状态：
            // - 避免在标题栏鼠标消息处理期间同步 SendMessage(WM_SYSCOMMAND) 造成重入；
            // - 修复“先跳到左上角再闪回”和双击后标题栏拖动链路失效的问题。
            ::ShowWindow(
                mainWindowHandle,
                targetMaximizedState ? SW_MAXIMIZE : SW_RESTORE);
        }
    }
    else
    {
        if (targetMaximizedState)
        {
            showMaximized();
        }
        else
        {
            showNormal();
        }
    }
#else
    if (targetMaximizedState)
    {
        showMaximized();
    }
    else
    {
        showNormal();
    }
#endif

    syncCustomTitleBarMaximizedState();
    // 二次同步只保留 0ms 一次：
    // - 覆盖“系统命令异步切换窗口态”的下一轮事件循环；
    // - 避免多次延迟同步导致体感卡顿或图标抖动。
    QTimer::singleShot(0, this, [this]()
        {
            syncCustomTitleBarMaximizedState();
        });
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);

#ifdef Q_OS_WIN
    if (message == nullptr || result == nullptr)
    {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    MSG* nativeMessage = reinterpret_cast<MSG*>(message);
    if (nativeMessage == nullptr)
    {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    if (nativeMessage->message == WM_NCCALCSIZE)
    {
        // WM_NCCALCSIZE 双分支统一返回 0：
        // - wParam=TRUE：窗口尺寸/状态变化时，让整块窗口都成为客户区；
        // - wParam=FALSE：窗口初始创建阶段同样移除默认非客户区，修复 Win11 左/上透明残留带。
        // 说明：这里不做额外矩形改写，保持现有可缩放与最大化行为不变。
        *result = 0;
        return true;
    }

    if (nativeMessage->message == WM_NCLBUTTONDOWN)
    {
        // 对 HTCAPTION 显式转发给 DefWindowProc：
        // - Qt 无边框窗口不会总是替我们走完原生标题栏拖动语义；
        // - 这里显式调用原生默认过程，保证单击按住后可以拖动。
        if (nativeMessage->wParam == HTCAPTION)
        {
            *result = ::DefWindowProcW(
                nativeMessage->hwnd,
                nativeMessage->message,
                nativeMessage->wParam,
                nativeMessage->lParam);
            return true;
        }
    }

    if (nativeMessage->message == WM_NCLBUTTONDBLCLK)
    {
        // HTCAPTION 双击手动切换最大化/还原：
        // - 保留系统标题栏语义；
        // - 同时避免 Qt/无边框窗口把双击吞掉导致无反应。
        if (nativeMessage->wParam == HTCAPTION)
        {
            const bool targetMaximizedState =
                (nativeMessage->hwnd != nullptr && ::IsWindow(nativeMessage->hwnd) != FALSE)
                ? (::IsZoomed(nativeMessage->hwnd) == FALSE)
                : !isWindowActuallyMaximized();
            setWindowMaximizedBySystemCommand(targetMaximizedState);
            *result = 0;
            return true;
        }
    }

    if (nativeMessage->message == WM_NCACTIVATE)
    {
        // 焦点切换时要求系统跳过默认非客户区重绘，避免瞬间刷出白色边框。
        *result = ::DefWindowProcW(
            nativeMessage->hwnd,
            nativeMessage->message,
            nativeMessage->wParam,
            static_cast<LPARAM>(-1));
        return true;
    }

    if (nativeMessage->message == WM_NCHITTEST)
    {
        // maximizedInNativeMessage 用途：使用当前消息窗口句柄直接判定最大化状态，避免重入。
        const bool maximizedInNativeMessage =
            (nativeMessage->hwnd != nullptr && ::IsWindow(nativeMessage->hwnd) != FALSE)
            ? (::IsZoomed(nativeMessage->hwnd) != FALSE)
            : isWindowActuallyMaximized();

        const LPARAM pointData = nativeMessage->lParam;
        const POINT screenPoint = {
            static_cast<LONG>(static_cast<short>(LOWORD(pointData))),
            static_cast<LONG>(static_cast<short>(HIWORD(pointData)))
        };
        const QPoint localPoint = mapFromGlobal(QPoint(screenPoint.x, screenPoint.y));
        // windowRectValue 用途：读取顶层窗口的屏幕坐标矩形，供边框缩放命中使用。
        RECT windowRectValue = {};
        ::GetWindowRect(nativeMessage->hwnd, &windowRectValue);
        // frameLocalPoint 用途：把屏幕坐标转换为相对整个顶层窗口左上角的坐标。
        const QPoint frameLocalPoint(
            screenPoint.x - windowRectValue.left,
            screenPoint.y - windowRectValue.top);
        // frameWidthValue/frameHeightValue 用途：保存顶层窗口当前尺寸，供右边/下边缩放命中判断。
        const int frameWidthValue = windowRectValue.right - windowRectValue.left;
        const int frameHeightValue = windowRectValue.bottom - windowRectValue.top;
        const int borderWidth = std::max(
            8,
            static_cast<int>(
                ::GetSystemMetrics(SM_CXSIZEFRAME)
                + ::GetSystemMetrics(SM_CXPADDEDBORDER)));

        // 边缘缩放命中优先：
        // - 必须先于标题栏拖动命中，否则标题栏会吞掉上边/左右边缩放区域；
        // - 这是“窗口无法调整大小”的直接原因。
        if (!maximizedInNativeMessage)
        {
            const bool hitLeft = frameLocalPoint.x() >= 0 && frameLocalPoint.x() < borderWidth;
            const bool hitRight =
                frameLocalPoint.x() <= frameWidthValue
                && frameLocalPoint.x() > (frameWidthValue - borderWidth);
            const bool hitTop = frameLocalPoint.y() >= 0 && frameLocalPoint.y() < borderWidth;
            const bool hitBottom =
                frameLocalPoint.y() <= frameHeightValue
                && frameLocalPoint.y() > (frameHeightValue - borderWidth);

            if (hitTop && hitLeft)
            {
                *result = HTTOPLEFT;
                return true;
            }
            if (hitTop && hitRight)
            {
                *result = HTTOPRIGHT;
                return true;
            }
            if (hitBottom && hitLeft)
            {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (hitBottom && hitRight)
            {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (hitLeft)
            {
                *result = HTLEFT;
                return true;
            }
            if (hitRight)
            {
                *result = HTRIGHT;
                return true;
            }
            if (hitTop)
            {
                *result = HTTOP;
                return true;
            }
            if (hitBottom)
            {
                *result = HTBOTTOM;
                return true;
            }
        }

        // 兼容处理：若仍存在顶端负坐标带（非客户区残留），直接按可拖动标题栏返回。
        if (localPoint.y() < 0 && localPoint.y() >= -borderWidth)
        {
            *result = HTCAPTION;
            return true;
        }

    }
#endif

    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::initCustomTitleBar()
{
    if (m_customTitleBar != nullptr)
    {
        return;
    }

    m_customTitleBar = new ks::ui::CustomTitleBar(this);
    m_customTitleBar->setPinnedState(m_windowPinned);
    syncCustomTitleBarMaximizedState();
    m_customTitleBar->setDarkModeEnabled(KswordTheme::IsDarkModeEnabled());
    if (menuBar() != nullptr)
    {
        // menuBarVisibleState 用途：隐藏原生菜单栏，避免出现在自绘标题栏上方。
        const bool menuBarVisibleState = false;
        menuBar()->setNativeMenuBar(false);
        menuBar()->setVisible(menuBarVisibleState);
    }
    if (m_mainRootLayout != nullptr && m_mainRootContainer != nullptr)
    {
        m_customTitleBar->setParent(m_mainRootContainer);
        m_mainRootLayout->insertWidget(0, m_customTitleBar, 0);
    }
    else
    {
        // 兜底：若根容器尚未就绪，退回 QMainWindow 的 menuWidget 挂载方式。
        setMenuWidget(m_customTitleBar);
    }
    m_customTitleBar->show();

    connect(m_customTitleBar, &ks::ui::CustomTitleBar::requestTogglePinned, this, [this]() {
        togglePinnedWindowState();
    });
    connect(m_customTitleBar, &ks::ui::CustomTitleBar::requestMinimizeWindow, this, [this]() {
        showMinimized();
    });
    connect(m_customTitleBar, &ks::ui::CustomTitleBar::requestToggleMaximizeWindow, this, [this]() {
        // targetMaximizedState 用途：根据真实状态计算下一目标状态（最大化或还原）。
        const bool targetMaximizedState = !isWindowActuallyMaximized();
        setWindowMaximizedBySystemCommand(targetMaximizedState);
    });
    connect(m_customTitleBar, &ks::ui::CustomTitleBar::requestCloseWindow, this, [this]() {
        close();
    });
    connect(m_customTitleBar, &ks::ui::CustomTitleBar::commandSubmitted, this, [this](const QString& commandText) {
        executeCommandInNewConsole(commandText);
    });

    kLogEvent initTitleBarEvent;
    info << initTitleBarEvent << "[MainWindow] 自绘标题栏初始化完成。" << eol;

    // 首帧自检：
    // - setMenuWidget 后下一轮事件循环检查标题栏是否可见；
    // - 同步记录挂载状态与尺寸，便于排查“标题栏未显示”问题。
    QTimer::singleShot(0, this, [this]()
        {
            if (m_customTitleBar == nullptr)
            {
                return;
            }

            // mountedAsMenuWidget 作用：确认自绘标题栏是否挂到 QMainWindow 菜单位。
            const bool mountedAsMenuWidget = (menuWidget() == m_customTitleBar);
            // mountedInRootLayout 作用：确认自绘标题栏是否挂到根容器纵向布局第 0 行。
            const bool mountedInRootLayout =
                (m_mainRootLayout != nullptr && m_mainRootLayout->indexOf(m_customTitleBar) >= 0);
            if (!m_customTitleBar->isVisible())
            {
                m_customTitleBar->show();
            }

            kLogEvent titleBarCheckEvent;
            info << titleBarCheckEvent
                << "[MainWindow] 自绘标题栏挂载检查, mounted="
                << (mountedAsMenuWidget ? "true" : "false")
                << ", mounted_layout="
                << (mountedInRootLayout ? "true" : "false")
                << ", visible="
                << (m_customTitleBar->isVisible() ? "true" : "false")
                << ", size="
                << m_customTitleBar->width()
                << "x"
                << m_customTitleBar->height()
                << eol;
        });
}

void MainWindow::setPinnedWindowState(const bool pinnedState, const bool emitLog)
{
    if (m_windowPinned == pinnedState)
    {
        if (m_customTitleBar != nullptr)
        {
            m_customTitleBar->setPinnedState(m_windowPinned);
        }
        return;
    }

    const HWND mainWindowHandle = reinterpret_cast<HWND>(winId());
    if (mainWindowHandle == nullptr || ::IsWindow(mainWindowHandle) == FALSE)
    {
        kLogEvent failedEvent;
        err << failedEvent << "[MainWindow] 置顶切换失败：主窗口句柄无效。" << eol;
        return;
    }

    const BOOL setTopMostResult = ::SetWindowPos(
        mainWindowHandle,
        pinnedState ? HWND_TOPMOST : HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (setTopMostResult == FALSE)
    {
        const DWORD errorCode = ::GetLastError();
        kLogEvent failedEvent;
        err << failedEvent
            << "[MainWindow] 置顶切换失败, targetPinned="
            << (pinnedState ? "true" : "false")
            << ", errorCode="
            << errorCode
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("窗口置顶"),
            QStringLiteral("置顶状态切换失败，错误码：%1").arg(errorCode));
        return;
    }

    m_windowPinned = pinnedState;
    if (m_customTitleBar != nullptr)
    {
        m_customTitleBar->setPinnedState(m_windowPinned);
    }

    if (emitLog)
    {
        kLogEvent pinEvent;
        info << pinEvent
            << "[MainWindow] 置顶状态已切换, pinned="
            << (m_windowPinned ? "true" : "false")
            << eol;
    }
}

void MainWindow::togglePinnedWindowState()
{
    setPinnedWindowState(!m_windowPinned, true);
}

void MainWindow::executeCommandInNewConsole(const QString& commandText)
{
    const QString trimmedCommandText = commandText.trimmed();
    if (trimmedCommandText.isEmpty())
    {
        return;
    }

    kLogEvent commandEvent;
    info << commandEvent
        << "[MainWindow] 标题栏命令执行请求, command="
        << trimmedCommandText.toStdString()
        << eol;

    QString commandInterpreterPath = qEnvironmentVariable("ComSpec").trimmed();
    if (commandInterpreterPath.isEmpty())
    {
        commandInterpreterPath = QStringLiteral("C:\\Windows\\System32\\cmd.exe");
    }
    commandInterpreterPath = QDir::toNativeSeparators(commandInterpreterPath);

    const QString commandLineText = QStringLiteral("\"%1\" /K %2")
        .arg(commandInterpreterPath, trimmedCommandText);
    std::wstring commandInterpreterPathWide = commandInterpreterPath.toStdWString();
    std::wstring commandLineWide = commandLineText.toStdWString();
    std::vector<wchar_t> commandLineBuffer(commandLineWide.begin(), commandLineWide.end());
    commandLineBuffer.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION processInfo{};
    const BOOL createProcessResult = ::CreateProcessW(
        commandInterpreterPathWide.c_str(),
        commandLineBuffer.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    if (createProcessResult == FALSE)
    {
        const DWORD errorCode = ::GetLastError();
        err << commandEvent
            << "[MainWindow] 标题栏命令执行失败, errorCode="
            << errorCode
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("执行命令"),
            QStringLiteral("启动 cmd 失败，错误码：%1").arg(errorCode));
        return;
    }

    ::CloseHandle(processInfo.hThread);
    ::CloseHandle(processInfo.hProcess);

    info << commandEvent
        << "[MainWindow] 标题栏命令执行已启动, pid="
        << processInfo.dwProcessId
        << eol;
}

void MainWindow::initMenus()
{
    if (menuBar() != nullptr)
    {
        // 隐藏 QMainWindow 原生菜单栏，避免出现在自绘标题栏上方。
        menuBar()->setNativeMenuBar(false);
        menuBar()->setVisible(false);
    }

    if (m_topActionRowWidget != nullptr)
    {
        return;
    }

    // 功能条：位于自绘标题栏下方，左侧“文件”，右侧权限按钮。
    m_topActionRowWidget = new QWidget(m_mainRootContainer);
    m_topActionRowWidget->setObjectName(QStringLiteral("ksTopActionRow"));
    m_topActionRowWidget->setFixedHeight(28);
    m_topActionRowLayout = new QHBoxLayout(m_topActionRowWidget);
    m_topActionRowLayout->setContentsMargins(8, 1, 8, 1);
    m_topActionRowLayout->setSpacing(8);

    const bool topMenuDarkModeEnabled = KswordTheme::IsDarkModeEnabled();
    const QString topMenuHoverColor = topMenuDarkModeEnabled
        ? QStringLiteral("rgba(63,143,232,0.16)")
        : QStringLiteral("#DCEBFB");
    const QString topMenuPressedColor = topMenuDarkModeEnabled
        ? QStringLiteral("rgba(63,143,232,0.24)")
        : QStringLiteral("#C7DFF8");
    const QString topMenuTextColor = topMenuDarkModeEnabled
        ? QStringLiteral("#EAF2FF")
        : QStringLiteral("#173554");
    const QString topMenuButtonStyle = QStringLiteral(
        "QToolButton{"
        "  background:transparent !important;"
        "  color:%1 !important;"
        "  border:1px solid transparent !important;"
        "  border-radius:4px;"
        "  margin:0;"
        "  padding:2px 8px 2px 6px;"
        "  font-size:12px;"
        "  font-weight:500;"
        "  text-align:left;"
        "}"
        "QToolButton:hover{"
        "  background:%2 !important;"
        "  color:%1 !important;"
        "  border-color:%3 !important;"
        "}"
        "QToolButton:pressed{"
        "  background:%4 !important;"
        "  color:%1 !important;"
        "  border-color:%3 !important;"
        "}"
        "QToolButton::menu-indicator{"
        "  image:none;"
        "  width:0;"
        "  height:0;"
        "}")
        .arg(topMenuTextColor)
        .arg(topMenuHoverColor)
        .arg(topMenuDarkModeEnabled
            ? QStringLiteral("rgba(83,167,255,0.38)")
            : QStringLiteral("#A8C9EA"))
        .arg(topMenuPressedColor);

    m_fileMenuButton = new QToolButton(m_topActionRowWidget);
    m_fileMenuButton->setObjectName(QStringLiteral("ksFileMenuButton"));
    m_fileMenuButton->setText(QStringLiteral("文件"));
    m_fileMenuButton->setToolTip(QStringLiteral("打开文件菜单"));
    m_fileMenuButton->setPopupMode(QToolButton::InstantPopup);
    m_fileMenuButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_fileMenuButton->setAutoRaise(true);
    m_fileMenuButton->setFixedHeight(22);
    m_fileMenuButton->setStyleSheet(topMenuButtonStyle);

    // fileMenu 用途：承载“文件”下拉菜单动作（当前保留退出）。
    QMenu* fileMenu = new QMenu(m_fileMenuButton);
    QAction* updateAction = new QAction(QStringLiteral("检查更新(&U)"), fileMenu);
    connect(updateAction, &QAction::triggered, this, &MainWindow::openReleasePageFromMenu);
    fileMenu->addAction(updateAction);

    QAction* licenseAction = new QAction(QStringLiteral("许可证(&L)"), fileMenu);
    connect(licenseAction, &QAction::triggered, this, &MainWindow::showLicenseFromMenu);
    fileMenu->addAction(licenseAction);
    fileMenu->addSeparator();

    QAction* exitAction = new QAction(QStringLiteral("退出(&X)"), fileMenu);
    exitAction->setShortcut(Qt::CTRL | Qt::Key_Q);
    connect(exitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);
    fileMenu->addAction(exitAction);
    m_fileMenuButton->setMenu(fileMenu);

    m_settingsMenuButton = new QToolButton(m_topActionRowWidget);
    m_settingsMenuButton->setObjectName(QStringLiteral("ksSettingsMenuButton"));
    m_settingsMenuButton->setText(QStringLiteral("设置"));
    m_settingsMenuButton->setToolTip(QStringLiteral("打开界面与启动设置"));
    m_settingsMenuButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_settingsMenuButton->setAutoRaise(true);
    m_settingsMenuButton->setFixedHeight(22);
    m_settingsMenuButton->setStyleSheet(topMenuButtonStyle);
    connect(m_settingsMenuButton, &QToolButton::clicked, this, [this]() {
        showSettingsPanelFromMenu();
    });

    m_topActionRowLayout->addWidget(m_fileMenuButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
    m_topActionRowLayout->addWidget(m_settingsMenuButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
    m_topActionRowLayout->addStretch(1);

    if (m_mainRootLayout != nullptr)
    {
        // 插入到根布局顶部；后续 initCustomTitleBar 会插到 index=0，因此标题栏仍在最上方。
        m_mainRootLayout->insertWidget(0, m_topActionRowWidget, 0);
    }
}

void MainWindow::openReleasePageFromMenu()
{
    const QUrl releaseUrl(QStringLiteral("https://github.com/WangWei-CM/KSword/releases"));
    if (!QDesktopServices::openUrl(releaseUrl))
    {
        QMessageBox::warning(
            this,
            QStringLiteral("检查更新"),
            QStringLiteral("无法打开更新页面：%1").arg(releaseUrl.toString()));
    }
}

void MainWindow::showLicenseFromMenu()
{
    const QString licensePath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("license"));
    QFile licenseFile(licensePath);
    QString licenseText;
    if (licenseFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QTextStream licenseStream(&licenseFile);
        licenseStream.setEncoding(QStringConverter::Utf8);
        licenseText = licenseStream.readAll();
    }
    else
    {
        licenseText = QStringLiteral("未找到同目录 license 文件：\n%1").arg(QDir::toNativeSeparators(licensePath));
    }

    QDialog licenseDialog(this);
    licenseDialog.setWindowTitle(QStringLiteral("许可证"));
    licenseDialog.resize(760, 560);
    licenseDialog.setStyleSheet(QStringLiteral(
        "QDialog{background:%1;color:%2;}"
        "QTextEdit{background:%1;color:%2;border:1px solid %3;}"
        "QPushButton{padding:4px 14px;}" )
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::TextPrimaryHex())
        .arg(KswordTheme::BorderHex()));

    QVBoxLayout dialogLayout(&licenseDialog);
    dialogLayout.setContentsMargins(8, 8, 8, 8);
    dialogLayout.setSpacing(8);

    QTextEdit licenseEditor(&licenseDialog);
    licenseEditor.setReadOnly(true);
    licenseEditor.setPlainText(licenseText.trimmed().isEmpty()
        ? QStringLiteral("license 文件为空。")
        : licenseText);
    dialogLayout.addWidget(&licenseEditor, 1);

    QPushButton closeButton(QStringLiteral("关闭"), &licenseDialog);
    connect(&closeButton, &QPushButton::clicked, &licenseDialog, &QDialog::accept);
    dialogLayout.addWidget(&closeButton, 0, Qt::AlignRight);

    licenseDialog.exec();
}
void MainWindow::showSettingsPanelFromMenu()
{
    QDialog settingsDialog(this);
    settingsDialog.setWindowTitle(QStringLiteral("设置"));
    settingsDialog.setModal(false);
    settingsDialog.resize(760, 640);
    const bool settingsDialogDarkModeEnabled = KswordTheme::IsDarkModeEnabled();
    const QString settingsComboBackgroundColor = settingsDialogDarkModeEnabled
        ? QStringLiteral("#182334")
        : QStringLiteral("#FFFFFF");
    const QString settingsComboTextColor = settingsDialogDarkModeEnabled
        ? QStringLiteral("#F3F7FF")
        : QStringLiteral("#162A42");
    const QString settingsComboBorderColor = settingsDialogDarkModeEnabled
        ? QStringLiteral("#3D5775")
        : QStringLiteral("#9CB8D8");
    const QString settingsComboPopupBackgroundColor = settingsDialogDarkModeEnabled
        ? QStringLiteral("#142032")
        : QStringLiteral("#FFFFFF");
    const QString settingsComboSelectionBackgroundColor = settingsDialogDarkModeEnabled
        ? QStringLiteral("#27466A")
        : QStringLiteral("#E6F2FF");
    settingsDialog.setStyleSheet(QStringLiteral(
        "QDialog{background:%1;color:%2;}"
        "QDialog QComboBox{"
        "  background-color:%3 !important;"
        "  color:%4 !important;"
        "  border:1px solid %5 !important;"
        "  border-radius:3px;"
        "  padding:2px 20px 2px 6px;"
        "  min-height:22px;"
        "}"
        "QDialog QComboBox::drop-down{"
        "  border:none !important;"
        "  width:18px;"
        "}"
        "QDialog QComboBox QAbstractItemView{"
        "  background-color:%6 !important;"
        "  color:%4 !important;"
        "  border:1px solid %5 !important;"
        "  selection-background-color:%7 !important;"
        "  selection-color:#FFFFFF !important;"
        "  outline:0;"
        "}"
        "QDialog QComboBox QAbstractItemView::item{"
        "  background-color:%6 !important;"
        "  color:%4 !important;"
        "}"
        "QDialog QComboBox QAbstractItemView::item:hover{"
        "  background-color:%7 !important;"
        "  color:#FFFFFF !important;"
        "}"
        "QDialog QComboBox QAbstractItemView::item:selected{"
        "  background-color:%7 !important;"
        "  color:#FFFFFF !important;"
        "}")
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::TextPrimaryHex())
        .arg(settingsComboBackgroundColor)
        .arg(settingsComboTextColor)
        .arg(settingsComboBorderColor)
        .arg(settingsComboPopupBackgroundColor)
        .arg(settingsComboSelectionBackgroundColor));

    QVBoxLayout dialogLayout(&settingsDialog);
    dialogLayout.setContentsMargins(8, 8, 8, 8);
    dialogLayout.setSpacing(6);

    // 设置面板改为顶部菜单即时对话框，每次打开读取当前 JSON，避免占用主 Tab 栏空间。
    SettingsDock settingsPanel(&settingsDialog);
    connect(
        &settingsPanel,
        &SettingsDock::appearanceSettingsChanged,
        this,
        [this](const ks::settings::AppearanceSettings& settings) {
            applyAppearanceSettings(settings, QStringLiteral("顶部菜单设置变更"));
        });
    dialogLayout.addWidget(&settingsPanel, 1);

    settingsDialog.exec();
}

void MainWindow::initPrivilegeStatusButtons()
{
    // 防重复初始化：若已创建容器则只刷新一次状态。
    if (m_privilegeButtonContainer != nullptr)
    {
        refreshPrivilegeStatusButtons();
        return;
    }

    // 在功能条右侧放置一个水平容器，承载权限状态按钮。
    QWidget* privilegeButtonParent = m_topActionRowWidget != nullptr
        ? m_topActionRowWidget
        : this;
    m_privilegeButtonContainer = new QWidget(privilegeButtonParent);
    QHBoxLayout* buttonLayout = new QHBoxLayout(m_privilegeButtonContainer);
    buttonLayout->setContentsMargins(0, 0, 4, 0);
    buttonLayout->setSpacing(6);

    // 按钮文本采用纯文字，满足用户要求。
    m_adminStatusButton = new QPushButton("Admin", m_privilegeButtonContainer);
    m_debugStatusButton = new QPushButton("Debug", m_privilegeButtonContainer);
    m_systemStatusButton = new QPushButton("System", m_privilegeButtonContainer);
    m_tiStatusButton = new QPushButton("TI", m_privilegeButtonContainer);
    m_r0StatusButton = new QPushButton("R0", m_privilegeButtonContainer);

    // 统一按钮尺寸，保证右上角布局整齐。
    const std::array<QPushButton*, 5> statusButtons{
        m_adminStatusButton,
        m_debugStatusButton,
        m_systemStatusButton,
        m_tiStatusButton,
        m_r0StatusButton
    };
    for (QPushButton* statusButton : statusButtons)
    {
        if (statusButton == nullptr)
        {
            continue;
        }
        statusButton->setFixedHeight(22);
        statusButton->setMinimumWidth(56);
        buttonLayout->addWidget(statusButton);
    }

    m_tiStatusButton->setToolTip("TrustedInstaller 状态位");
    m_r0StatusButton->setToolTip("R0：KswordARK 驱动服务快捷开关");

    // 把容器挂到功能条右侧。
    if (m_topActionRowLayout != nullptr)
    {
        m_topActionRowLayout->addWidget(m_privilegeButtonContainer, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    // Admin 按钮：
    // - 已是管理员：仅提示当前状态；
    // - 非管理员：立刻触发 runas 重启提权。
    connect(m_adminStatusButton, &QPushButton::clicked, this, [this]() {
        if (hasAdminPrivilege())
        {
            QMessageBox::information(this, "Admin", "当前已是管理员权限。");
            refreshPrivilegeStatusButtons();
            return;
        }

        kLogEvent logEvent;
        warn << logEvent << "[MainWindow] Admin 按钮触发提权重启。" << eol;
        requestAdminElevationRestart();
    });

    // Debug 按钮：
    // - 非管理员：按需求先执行 Admin 提权动作；
    // - 已管理员：申请 SeDebugPrivilege。
    connect(m_debugStatusButton, &QPushButton::clicked, this, [this]() {
        if (!hasAdminPrivilege())
        {
            kLogEvent logEvent;
            warn << logEvent << "[MainWindow] Debug 按钮检测到非管理员，转为执行 Admin 提权。" << eol;
            requestAdminElevationRestart();
            return;
        }

        std::string errorText;
        const bool enableOk = enableSeDebugPrivilege(errorText);
        if (enableOk)
        {
            kLogEvent logEvent;
            info << logEvent << "[MainWindow] SeDebugPrivilege 申请成功。" << eol;
            QMessageBox::information(this, "Debug", "SeDebugPrivilege 已启用。");
        }
        else
        {
            kLogEvent logEvent;
            err << logEvent << "[MainWindow] SeDebugPrivilege 申请失败: " << errorText << eol;
            QMessageBox::warning(this, "Debug", QString("SeDebugPrivilege 启用失败。\n%1").arg(QString::fromStdString(errorText)));
        }
        refreshPrivilegeStatusButtons();
    });

    // System 按钮仅展示状态，点击给出提示（普通用户态无法直接“切到 SYSTEM”）。
    connect(m_systemStatusButton, &QPushButton::clicked, this, [this]() {
        if (hasSystemPrivilege())
        {
            QMessageBox::information(this, "System", "当前进程已经是 LocalSystem 身份。");
        }
        else
        {
            HANDLE hToken = NULL;          // 当前进程的令牌句柄
            LUID Luid;                     // 特权局部唯一标识符
            TOKEN_PRIVILEGES tp;           // 令牌特权结构体

            // 打开当前进程的令牌，要求调整特权与查询权限
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken failed, error: " << GetLastError() << eol;
                return 1;
            }

            // 查找 SE_DEBUG_NAME 特权的 LUID
            if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Luid))
            {
                kLogEvent logEvent;
                err << logEvent << "LookupPrivilegeValue failed, error: " << GetLastError() << eol;
                CloseHandle(hToken);
                return 1;
            }

            // 设置特权属性为启用
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = Luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

            // 调整令牌特权
            if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL))
            {
                kLogEvent logEvent;
                err << logEvent << "AdjustTokenPrivileges failed, error: " << GetLastError() << eol;
                CloseHandle(hToken);
                return 1;
            }
            // 检查特权是否真正被启用（AdjustTokenPrivileges 可能成功但未全部分配）
            if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
            {
                kLogEvent logEvent;
                err << logEvent << "AdjustTokenPrivileges: SeDebugPrivilege not assigned" << eol;
                CloseHandle(hToken);
                return 1;
            }
            CloseHandle(hToken);  // 特权已启用，关闭临时句柄

            // ========== 第二步：枚举进程，获取 lsass.exe 和 winlogon.exe 的 PID ==========
            DWORD idL = 0;                     // 存放 lsass.exe 的进程ID
            DWORD idW = 0;                     // 存放 winlogon.exe 的进程ID
            PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };  // 进程快照条目（宽字符）
            HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);  // 进程快照句柄
            if (hSnapshot == INVALID_HANDLE_VALUE)
            {
                kLogEvent logEvent;
                err << logEvent << "CreateToolhelp32Snapshot failed, error: " << GetLastError() << eol;
                return 1;
            }

            // 遍历进程快照，匹配目标进程名
            if (Process32FirstW(hSnapshot, &pe))
            {
                do
                {
                    if (_wcsicmp(pe.szExeFile, L"lsass.exe") == 0)
                    {
                        idL = pe.th32ProcessID;
                        kLogEvent logEvent;
                        info << logEvent << "Found lsass.exe with PID: " << idL << eol;
                    }
                    else if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0)
                    {
                        idW = pe.th32ProcessID;
                        kLogEvent logEvent;
                        info << logEvent << "Found winlogon.exe with PID: " << idW << eol;
                    }
                } while (Process32NextW(hSnapshot, &pe));
            }
            CloseHandle(hSnapshot);

            // ========== 第三步：打开目标进程（优先 lsass，其次 winlogon） ==========
            HANDLE hProcess = NULL;          // 目标进程句柄
            if (idL != 0)
                hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, idL);
            if (!hProcess && idW != 0)
                hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, idW);
            if (!hProcess)
            {
                kLogEvent logEvent;
                err << logEvent << "Failed to open target process (lsass/winlogon), error: " << GetLastError() << eol;
                return 1;
            }
            {
                kLogEvent logEvent;
                info << logEvent << "Opened target process" << eol;
            }

            // ========== 第四步：打开目标进程的令牌 ==========
            HANDLE hTokenx = NULL;           // 目标进程的令牌句柄
            if (!OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hTokenx))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken on target process failed, error: " << GetLastError() << eol;
                CloseHandle(hProcess);
                return 1;
            }

            // ========== 第五步：复制令牌，获得可用的主令牌 ==========
            HANDLE hNewToken = NULL;         // 复制得到的新令牌句柄
            if (!DuplicateTokenEx(hTokenx, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hNewToken))
            {
                kLogEvent logEvent;
                err << logEvent << "DuplicateTokenEx failed, error: " << GetLastError() << eol;
                CloseHandle(hTokenx);
                CloseHandle(hProcess);
                return 1;
            }
            CloseHandle(hTokenx);
            CloseHandle(hProcess);

            // ========== 第六步：获取当前程序自身的路径 ==========
            std::wstring selfPath = ks::process::GetCurrentProcessPath();
            if (selfPath.empty())
            {
                kLogEvent logEvent;
                err << logEvent <<"Failed to get current process path" << eol;
                CloseHandle(hNewToken);
                return 1;
            }
            {
                kLogEvent logEvent;
                // pathUtf8 用途：把 UTF-16 路径转换为 UTF-8，避免日志流(std::ostringstream)直接输出 std::wstring 导致编译错误。
                const std::string pathUtf8 = ks::str::Utf16ToUtf8(selfPath);
                info << logEvent << "Current process path: " << pathUtf8 << eol;
            }

            // ========== 第七步：使用复制得到的令牌启动自身 ==========
            STARTUPINFOW si = { sizeof(STARTUPINFOW) };
            PROCESS_INFORMATION pi = { 0 };
            // 为 lpDesktop 使用可写缓冲区（避免 const wchar_t* 赋值给 LPWSTR 的编译错误）
            wchar_t desktop[] = L"winsta0\\default";
            si.lpDesktop = desktop;          // 显示在交互式桌面

            if (!CreateProcessWithTokenW(hNewToken, LOGON_NETCREDENTIALS_ONLY, selfPath.c_str(), NULL,
                NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi))
            {
                kLogEvent logEvent;
                err << logEvent << "CreateProcessWithTokenW failed, error: " << GetLastError() << eol;
                CloseHandle(hNewToken);
                return 1;
            }

            {
                kLogEvent logEvent;
                info << logEvent << "Successfully started new instance of the program. New PID: " << pi.dwProcessId << eol;
            }

            // 清理资源
            CloseHandle(hNewToken);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    });

    // TI 按钮：展示 TrustedInstaller 身份状态。
    // 说明：仅做状态检查，不提供自动令牌窃取/注入类提权逻辑。
    connect(m_tiStatusButton, &QPushButton::clicked, this, [this]() {
        if (hasTrustedInstallerPrivilege())
        {
            QMessageBox::information(this, "TI", "当前进程已经是 TrustedInstaller 身份。");
        }
        else
        {
            {
                kLogEvent logEvent;
                info << logEvent << "Starting self with TrustedInstaller privilege..." << eol;
            }

            // ========== 第 2 步：模拟 SYSTEM 账户（通过 winlogon.exe） ==========
            // 2.1 获取 winlogon.exe 的进程 ID
            DWORD winlogonPid = 0;
            HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnapshot == INVALID_HANDLE_VALUE)
            {
                kLogEvent logEvent;
                err << logEvent << "CreateToolhelp32Snapshot failed, error: " << GetLastError() << eol;
                return 1;
            }
            PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
            if (Process32FirstW(hSnapshot, &pe))
            {
                do
                {
                    if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0)
                    {
                        winlogonPid = pe.th32ProcessID;
                        break;
                    }
                } while (Process32NextW(hSnapshot, &pe));
            }
            CloseHandle(hSnapshot);
            if (winlogonPid == 0)
            {
                kLogEvent logEvent;
                err << logEvent << "winlogon.exe not found" << eol;
                return 1;
            }

            // 2.2 打开 winlogon.exe 进程
            HANDLE hWinlogon = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, winlogonPid);
            if (!hWinlogon)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcess(winlogon.exe) failed, error: " << GetLastError() << eol;
                return 1;
            }

            // 2.3 打开 winlogon 进程的令牌
            HANDLE hWinlogonToken = NULL;
            if (!OpenProcessToken(hWinlogon, MAXIMUM_ALLOWED, &hWinlogonToken))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken(winlogon.exe) failed, error: " << GetLastError() << eol;
                CloseHandle(hWinlogon);
                return 1;
            }

            // 2.4 复制 winlogon 令牌为模拟令牌
            HANDLE hSystemImpersonationToken = NULL;
            SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };
            if (!DuplicateTokenEx(hWinlogonToken, MAXIMUM_ALLOWED, &sa, SecurityImpersonation, TokenImpersonation, &hSystemImpersonationToken))
            {
                kLogEvent logEvent;
                err << logEvent << "DuplicateTokenEx(winlogon) failed, error: " << GetLastError() << eol;
                CloseHandle(hWinlogonToken);
                CloseHandle(hWinlogon);
                return 1;
            }

            // 2.5 模拟 SYSTEM 账户（当前线程获得 SYSTEM 上下文）
            if (!ImpersonateLoggedOnUser(hSystemImpersonationToken))
            {
                kLogEvent logEvent;
                err << logEvent << "ImpersonateLoggedOnUser failed, error: " << GetLastError() << eol;
                CloseHandle(hSystemImpersonationToken);
                CloseHandle(hWinlogonToken);
                CloseHandle(hWinlogon);
                return 1;
            }
            {
                kLogEvent logEvent;
                info << logEvent << "Successfully impersonated SYSTEM via winlogon.exe" << eol;
            }

            // 清理 winlogon 相关句柄（模拟令牌已生效，句柄可关闭）
            CloseHandle(hSystemImpersonationToken);
            CloseHandle(hWinlogonToken);
            CloseHandle(hWinlogon);

            // ========== 第 3 步：启动 TrustedInstaller 服务并获取其进程 ID ==========
            SC_HANDLE hSCM = OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE, GENERIC_EXECUTE);
            if (!hSCM)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenSCManagerW failed, error: " << GetLastError() << eol;
                return 1;
            }
            SC_HANDLE hService = OpenServiceW(hSCM, L"TrustedInstaller", GENERIC_READ | GENERIC_EXECUTE);
            if (!hService)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenServiceW(TrustedInstaller) failed, error: " << GetLastError() << eol;
                CloseServiceHandle(hSCM);
                return 1;
            }

            DWORD tiPid = 0;
            SERVICE_STATUS_PROCESS status = { 0 };
            DWORD bytesNeeded = 0;
            // 循环等待服务进入运行状态
            while (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded))
            {
                if (status.dwCurrentState == SERVICE_RUNNING)
                {
                    tiPid = status.dwProcessId;
                    {
                        kLogEvent logEvent;
                        info << logEvent << "TrustedInstaller service is running, PID: " << tiPid << eol;
                    }
                    break;
                }
                else if (status.dwCurrentState == SERVICE_STOPPED)
                {
                    if (!StartServiceW(hService, 0, nullptr))
                    {
                        kLogEvent logEvent;
                        err << logEvent << "StartServiceW(TrustedInstaller) failed, error: " << GetLastError() << eol;
                        CloseServiceHandle(hService);
                        CloseServiceHandle(hSCM);
                        return 1;
                    }
                    {
                        kLogEvent logEvent;
                        info << logEvent << "Started TrustedInstaller service, waiting..." << eol;
                    }
                    // 继续循环等待服务启动完成
                    continue;
                }
                else if (status.dwCurrentState == SERVICE_START_PENDING || status.dwCurrentState == SERVICE_STOP_PENDING)
                {
                    Sleep(status.dwWaitHint);
                    continue;
                }
                else
                {
                    // 其他状态（异常）
                    break;
                }
            }
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            if (tiPid == 0)
            {
                kLogEvent logEvent;
                err << logEvent << "Failed to get TrustedInstaller PID" << eol;
                return 1;
            }

            // ========== 第 4 步：打开 TrustedInstaller 进程并获取其令牌 ==========
            HANDLE hTiProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, tiPid);
            if (!hTiProcess)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcess(TrustedInstaller.exe) failed, error: " << GetLastError() << eol;
                return 1;
            }
            HANDLE hTiToken = NULL;
            if (!OpenProcessToken(hTiProcess, MAXIMUM_ALLOWED, &hTiToken))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken(TrustedInstaller.exe) failed, error: " << GetLastError() << eol;
                CloseHandle(hTiProcess);
                return 1;
            }

            // 复制 TrustedInstaller 令牌为主令牌（TokenPrimary）
            HANDLE hNewToken = NULL;
            if (!DuplicateTokenEx(hTiToken, MAXIMUM_ALLOWED, &sa, SecurityImpersonation, TokenPrimary, &hNewToken))
            {
                kLogEvent logEvent;
                err << logEvent << "DuplicateTokenEx(TrustedInstaller) failed, error: " << GetLastError() << eol;
                CloseHandle(hTiToken);
                CloseHandle(hTiProcess);
                return 1;
            }
            CloseHandle(hTiToken);
            CloseHandle(hTiProcess);

            // ========== 第 5 步：获取当前进程自身路径 ==========
            wchar_t selfPath[MAX_PATH] = { 0 };
            if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH) == 0)
            {
                kLogEvent logEvent;
                err << logEvent << "GetModuleFileNameW failed, error: " << GetLastError() << eol;
                CloseHandle(hNewToken);
                return 1;
            }

            // ========== 第 6 步：使用 TrustedInstaller 令牌启动自身 ==========
            STARTUPINFOW si = { sizeof(STARTUPINFOW) };
            wchar_t desktop[] = L"winsta0\\default";   // 可写缓冲区
            si.lpDesktop = desktop;
            PROCESS_INFORMATION pi = { 0 };
            if (!CreateProcessWithTokenW(hNewToken, LOGON_WITH_PROFILE, selfPath, nullptr,
                CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi))
            {
                kLogEvent logEvent;
                err << logEvent << "CreateProcessWithTokenW failed, error: " << GetLastError() << eol;
                CloseHandle(hNewToken);
                return 1;
            }

            // ========== 第 7 步：成功，记录日志并清理 ==========
            {
                kLogEvent logEvent;
                info << logEvent << "Successfully started self with TrustedInstaller token. New PID: " << pi.dwProcessId << eol;
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(hNewToken);
        }
    });

    // R0 按钮：
    // - 启动前先查询服务状态；
    // - 运行中则停止并卸载服务；
    // - 未运行则从当前 exe 目录加载 KswordARK.sys。
    connect(m_r0StatusButton, &QPushButton::clicked, this, [this]() {
        handleR0StatusButtonClicked();
    });

    // 定时刷新权限状态，保证按钮颜色与实际权限一致。
    m_privilegeStatusTimer = new QTimer(this);
    m_privilegeStatusTimer->setInterval(1500);
    connect(m_privilegeStatusTimer, &QTimer::timeout, this, [this]() {
        refreshPrivilegeStatusButtons();
    });
    m_privilegeStatusTimer->start();

    // 启动时执行一次 R0 服务状态查询，作为按钮初始态来源。
    queryR0DriverServiceRunning(m_r0DriverServiceRunning, true);
    refreshPrivilegeStatusButtons();
}

void MainWindow::refreshPrivilegeStatusButtons()
{
    // 读取当前权限状态。
    const bool adminEnabled = hasAdminPrivilege();
    const bool debugEnabled = hasDebugPrivilege();
    const bool systemEnabled = hasSystemPrivilege();

    // TI/R0 状态位：
    // - TI：检查当前令牌是否为 TrustedInstaller 服务 SID；
    // - R0：检查 KswordARK 驱动服务是否处于运行态。
    const bool trustedInstallerEnabled = hasTrustedInstallerPrivilege();
    const bool r0Enabled = m_r0DriverServiceRunning;

    // 按状态更新按钮样式与提示文本。
    applyPrivilegeButtonStyle(m_adminStatusButton, adminEnabled);
    applyPrivilegeButtonStyle(m_debugStatusButton, debugEnabled);
    applyPrivilegeButtonStyle(m_systemStatusButton, systemEnabled);
    applyPrivilegeButtonStyle(m_tiStatusButton, trustedInstallerEnabled);
    if (m_r0StatusButton != nullptr)
    {
        m_r0StatusButton->setStyleSheet(buildR0ButtonStyle(r0Enabled));
    }

    if (m_adminStatusButton != nullptr)
    {
        m_adminStatusButton->setToolTip(adminEnabled ? "管理员权限已启用" : "点击提权到管理员（重启当前程序）");
    }
    if (m_debugStatusButton != nullptr)
    {
        m_debugStatusButton->setToolTip(debugEnabled ? "SeDebugPrivilege 已启用" : "点击启用 SeDebugPrivilege");
    }
    if (m_systemStatusButton != nullptr)
    {
        m_systemStatusButton->setToolTip(systemEnabled ? "当前运行身份：LocalSystem" : "当前运行身份：非 LocalSystem");
    }
    if (m_tiStatusButton != nullptr)
    {
        m_tiStatusButton->setToolTip(trustedInstallerEnabled ? "当前运行身份：TrustedInstaller" : "当前运行身份：非 TrustedInstaller");
    }
    if (m_r0StatusButton != nullptr)
    {
        m_r0StatusButton->setToolTip(r0Enabled
            ? "R0 已启用：KswordARK 驱动服务正在运行（点击卸载）"
            : "R0 未启用：点击创建并启动 KswordARK 驱动服务");
    }

    // 仅在状态变化时写日志，避免定时器造成日志刷屏。
    static bool hasPreviousState = false;
    static bool previousAdmin = false;
    static bool previousDebug = false;
    static bool previousSystem = false;
    static bool previousTi = false;
    static bool previousR0 = false;
    if (!hasPreviousState ||
        previousAdmin != adminEnabled ||
        previousDebug != debugEnabled ||
        previousSystem != systemEnabled ||
        previousTi != trustedInstallerEnabled ||
        previousR0 != r0Enabled)
    {
        hasPreviousState = true;
        previousAdmin = adminEnabled;
        previousDebug = debugEnabled;
        previousSystem = systemEnabled;
        previousTi = trustedInstallerEnabled;
        previousR0 = r0Enabled;

        kLogEvent logEvent;
        info << logEvent
            << "[MainWindow] 权限状态刷新, admin=" << (adminEnabled ? "true" : "false")
            << ", debug=" << (debugEnabled ? "true" : "false")
            << ", system=" << (systemEnabled ? "true" : "false")
            << ", ti=" << (trustedInstallerEnabled ? "true" : "false")
            << ", r0=" << (r0Enabled ? "true" : "false")
            << eol;
    }
}

void MainWindow::applyPrivilegeButtonStyle(QPushButton* button, const bool activeState)
{
    if (button == nullptr)
    {
        return;
    }
    button->setStyleSheet(buildPrivilegeButtonStyle(activeState));
}

void MainWindow::startR0DriverLogPoller()
{
    if (m_r0DriverLogPollerRunning.exchange(true))
    {
        return;
    }

    try
    {
        m_r0DriverLogPollerThread = std::make_unique<std::thread>([this]()
            {
                runR0DriverLogPollerLoop();
            });
    }
    catch (...)
    {
        m_r0DriverLogPollerRunning.store(false);
        m_r0DriverLogPollerThread.reset();

        kLogEvent& logEvent = sharedR0DriverLogEvent();
        err << logEvent << "[MainWindow][R0Log] 轮询线程创建失败。" << eol;
    }
}

void MainWindow::stopR0DriverLogPoller()
{
    m_r0DriverLogPollerRunning.store(false);
    if (m_r0DriverLogPollerThread != nullptr && m_r0DriverLogPollerThread->joinable())
    {
        m_r0DriverLogPollerThread->join();
    }
    m_r0DriverLogPollerThread.reset();
}

void MainWindow::runR0DriverLogPollerLoop()
{
    HANDLE logDeviceHandle = INVALID_HANDLE_VALUE;
    std::string pendingPayloadText;
    pendingPayloadText.reserve(2048);
    bool waitingDeviceLogged = false;
    static const std::string endMarkerText = KSWORD_ARK_LOG_END_MARKER;

    kLogEvent& logEvent = sharedR0DriverLogEvent();
    info << logEvent << "[MainWindow][R0Log] 轮询线程已启动。" << eol;

    while (m_r0DriverLogPollerRunning.load())
    {
        if (logDeviceHandle == INVALID_HANDLE_VALUE)
        {
            logDeviceHandle = ::CreateFileW(
                KSWORD_ARK_LOG_WIN32_PATH,
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (logDeviceHandle == INVALID_HANDLE_VALUE)
            {
                if (!waitingDeviceLogged)
                {
                    waitingDeviceLogged = true;
                    dbg << logEvent << "[MainWindow][R0Log] 等待日志设备上线：" << "path=\\\\.\\KswordARKLog" << eol;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kR0LogConnectRetrySleepMs));
                continue;
            }

            waitingDeviceLogged = false;
            info << logEvent << "[MainWindow][R0Log] 已连接驱动日志设备。" << eol;
        }

        char readBuffer[1024] = { 0 };
        DWORD bytesRead = 0;
        if (::ReadFile(logDeviceHandle, readBuffer, static_cast<DWORD>(sizeof(readBuffer)), &bytesRead, nullptr) != FALSE)
        {
            if (bytesRead == 0U)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(kR0LogIdlePollSleepMs));
                continue;
            }

            pendingPayloadText.append(readBuffer, bytesRead);
            while (true)
            {
                const std::size_t markerPosition = pendingPayloadText.find(endMarkerText);
                if (markerPosition == std::string::npos)
                {
                    break;
                }

                const std::string recordText = pendingPayloadText.substr(0, markerPosition);
                pendingPayloadText.erase(0, markerPosition + endMarkerText.size());
                dispatchR0DriverLogRecord(recordText);
            }
            continue;
        }

        const DWORD readError = ::GetLastError();
        if (readError == ERROR_NO_MORE_ITEMS ||
            readError == ERROR_NO_DATA ||
            readError == ERROR_HANDLE_EOF)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kR0LogIdlePollSleepMs));
            continue;
        }

        warn << logEvent
            << "[MainWindow][R0Log] 读取日志设备失败, error="
            << readError
            << ", 将重新连接。"
            << eol;
        ::CloseHandle(logDeviceHandle);
        logDeviceHandle = INVALID_HANDLE_VALUE;
        std::this_thread::sleep_for(std::chrono::milliseconds(kR0LogConnectRetrySleepMs));
    }

    if (logDeviceHandle != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(logDeviceHandle);
        logDeviceHandle = INVALID_HANDLE_VALUE;
    }

    info << logEvent << "[MainWindow][R0Log] 轮询线程已退出。" << eol;
}

void MainWindow::dispatchR0DriverLogRecord(const std::string& logRecordText)
{
    if (logRecordText.empty())
    {
        return;
    }

    std::string payloadText = logRecordText;
    LogStream* outputStream = &info;
    std::size_t prefixLength = 0U;

    if (startsWithLiteral(payloadText, kR0LogPrefixDebug))
    {
        outputStream = &dbg;
        prefixLength = std::strlen(kR0LogPrefixDebug);
    }
    else if (startsWithLiteral(payloadText, kR0LogPrefixInfo))
    {
        outputStream = &info;
        prefixLength = std::strlen(kR0LogPrefixInfo);
    }
    else if (startsWithLiteral(payloadText, kR0LogPrefixWarn))
    {
        outputStream = &warn;
        prefixLength = std::strlen(kR0LogPrefixWarn);
    }
    else if (startsWithLiteral(payloadText, kR0LogPrefixError))
    {
        outputStream = &err;
        prefixLength = std::strlen(kR0LogPrefixError);
    }
    else if (startsWithLiteral(payloadText, kR0LogPrefixFatal))
    {
        outputStream = &fatal;
        prefixLength = std::strlen(kR0LogPrefixFatal);
    }

    if (prefixLength > 0U && payloadText.size() >= prefixLength)
    {
        payloadText.erase(0, prefixLength);
    }

    kLogEvent& logEvent = sharedR0DriverLogEvent();
    (*outputStream) << logEvent << "[R0] " << payloadText << eol;
}

void MainWindow::showR0FatalError(
    const QString& stageText,
    const unsigned long errorCode,
    const QString& detailText)
{
    const DWORD win32ErrorCode = static_cast<DWORD>(errorCode);
    QString messageText = stageText.trimmed();
    if (win32ErrorCode != ERROR_SUCCESS)
    {
        messageText += QStringLiteral("\n\n错误码：%1").arg(win32ErrorCode);
        messageText += QStringLiteral("\n系统信息：%1").arg(formatWin32ErrorText(win32ErrorCode));
    }
    if (!detailText.trimmed().isEmpty())
    {
        messageText += QStringLiteral("\n\n详细信息：\n%1").arg(detailText.trimmed());
    }

    kLogEvent logEvent;
    fatal << logEvent
        << "[MainWindow][R0][Fatal] stage=" << stageText.toStdString()
        << ", error=" << win32ErrorCode
        << ", detail=" << detailText.toStdString()
        << eol;

    QMessageBox::critical(this, QStringLiteral("R0 操作失败"), messageText);
}

bool MainWindow::isR0DriverSignatureFailure(const unsigned long errorCode) const
{
    return errorCode == ERROR_INVALID_IMAGE_HASH ||
        errorCode == ERROR_DRIVER_BLOCKED;
}

bool MainWindow::queryR0DriverServiceRunning(bool& runningOut, const bool fatalOnError)
{
    runningOut = false;

    ScopedServiceHandle scmHandle(::OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
    if (!scmHandle.isValid())
    {
        if (fatalOnError)
        {
            showR0FatalError(
                QStringLiteral("查询 KswordARK 驱动服务状态失败：无法连接服务控制管理器。"),
                ::GetLastError());
        }
        return false;
    }

    ScopedServiceHandle serviceHandle(::OpenServiceW(
        scmHandle.get(),
        kR0DriverServiceName,
        SERVICE_QUERY_STATUS));
    if (!serviceHandle.isValid())
    {
        const DWORD openError = ::GetLastError();
        if (openError == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            runningOut = false;
            return true;
        }
        if (fatalOnError)
        {
            showR0FatalError(
                QStringLiteral("查询 KswordARK 驱动服务状态失败：无法打开服务。"),
                openError,
                QStringLiteral("目标服务名：%1").arg(QString::fromWCharArray(kR0DriverServiceName)));
        }
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD queryError = ERROR_SUCCESS;
    if (!queryServiceStatus(serviceHandle.get(), status, queryError))
    {
        if (fatalOnError)
        {
            showR0FatalError(
                QStringLiteral("查询 KswordARK 驱动服务状态失败：读取服务状态失败。"),
                queryError);
        }
        return false;
    }

    runningOut = isRunningLikeServiceState(status.dwCurrentState);
    return true;
}

bool MainWindow::stopR0DriverService(const bool suppressErrorDialog)
{
    bool usedDirectNtUnloadFallback = false;

    const auto reportStopFailure =
        [this, suppressErrorDialog](
            const QString& stageText,
            const unsigned long errorCode,
            const QString& detailText = QString())
        {
            if (!suppressErrorDialog)
            {
                showR0FatalError(stageText, errorCode, detailText);
                return;
            }

            kLogEvent logEvent;
            err << logEvent
                << "[MainWindow][R0][AutoStop] stage="
                << stageText.toStdString()
                << ", error="
                << errorCode
                << ", detail="
                << detailText.toStdString()
                << eol;
        };

    ScopedServiceHandle scmHandle(::OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
    if (!scmHandle.isValid())
    {
        reportStopFailure(
            QStringLiteral("R0 卸载失败：无法连接服务控制管理器。"),
            ::GetLastError());
        return false;
    }

    ScopedServiceHandle serviceHandle(::OpenServiceW(
        scmHandle.get(),
        kR0DriverServiceName,
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
    if (!serviceHandle.isValid())
    {
        const DWORD openError = ::GetLastError();
        if (openError == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            return true;
        }
        reportStopFailure(
            QStringLiteral("R0 卸载失败：无法打开驱动服务。"),
            openError);
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD queryError = ERROR_SUCCESS;
    if (!queryServiceStatus(serviceHandle.get(), status, queryError))
    {
        reportStopFailure(
            QStringLiteral("R0 卸载失败：读取驱动服务状态失败。"),
            queryError);
        return false;
    }

    if (status.dwCurrentState != SERVICE_STOPPED)
    {
        if (status.dwCurrentState != SERVICE_STOP_PENDING)
        {
            SERVICE_STATUS ignoredStatus{};
            if (::ControlService(serviceHandle.get(), SERVICE_CONTROL_STOP, &ignoredStatus) == FALSE)
            {
                const DWORD stopError = ::GetLastError();
                if (stopError != ERROR_SERVICE_NOT_ACTIVE)
                {
                    // 命中 1052（不接受 STOP 控制）时，回退到 NtUnloadDriver 直卸路径。
                    if (stopError == ERROR_INVALID_SERVICE_CONTROL)
                    {
                        DWORD privilegeError = ERROR_SUCCESS;
                        const bool privilegeOk =
                            enableCurrentProcessPrivilege(SE_LOAD_DRIVER_NAME, &privilegeError);

                        long ntUnloadStatus = 0;
                        const bool unloadOk = tryNtUnloadDriverByServiceName(
                            kR0DriverServiceName,
                            &ntUnloadStatus);
                        if (!unloadOk)
                        {
                            reportStopFailure(
                                QStringLiteral("R0 卸载失败：ControlService 返回 1052，且 NtUnloadDriver 回退失败。"),
                                stopError,
                                QStringLiteral("enablePrivilegeOk=%1, privilegeError=%2, ntUnloadStatus=0x%3")
                                .arg(privilegeOk ? QStringLiteral("true") : QStringLiteral("false"))
                                .arg(privilegeError)
                                .arg(static_cast<qulonglong>(static_cast<unsigned long>(ntUnloadStatus)), 8, 16, QChar('0')));
                            return false;
                        }

                        usedDirectNtUnloadFallback = true;
                        status.dwCurrentState = SERVICE_STOPPED;
                    }
                    else
                    {
                        reportStopFailure(
                            QStringLiteral("R0 卸载失败：停止驱动服务失败。"),
                            stopError);
                        return false;
                    }
                }
            }
        }

        if (!usedDirectNtUnloadFallback)
        {
            SERVICE_STATUS_PROCESS latestStatus{};
            DWORD waitError = ERROR_SUCCESS;
            if (!waitServiceState(
                serviceHandle.get(),
                SERVICE_STOPPED,
                kR0ServiceWaitTimeoutMs,
                latestStatus,
                waitError))
            {
                if (waitError != ERROR_SUCCESS)
                {
                    reportStopFailure(
                        QStringLiteral("R0 卸载失败：等待服务停止时查询状态失败。"),
                        waitError);
                }
                else
                {
                    reportStopFailure(
                        QStringLiteral("R0 卸载失败：等待服务停止超时。"),
                        ERROR_TIMEOUT,
                        QStringLiteral("当前状态：%1").arg(serviceStateToText(latestStatus.dwCurrentState)));
                }
                return false;
            }
        }
    }

    if (::DeleteService(serviceHandle.get()) == FALSE)
    {
        const DWORD deleteError = ::GetLastError();
        if (deleteError != ERROR_SERVICE_MARKED_FOR_DELETE &&
            deleteError != ERROR_SERVICE_DOES_NOT_EXIST)
        {
            reportStopFailure(
                QStringLiteral("R0 卸载失败：删除驱动服务失败。"),
                deleteError);
            return false;
        }
    }

    kLogEvent logEvent;
    info << logEvent << "[MainWindow][R0] 已停止并删除 KswordARK 驱动服务。" << eol;
    return true;
}

bool MainWindow::showUnsignedDriverFailureDialog(
    const unsigned long errorCode,
    const QString& operationText)
{
    const DWORD win32ErrorCode = static_cast<DWORD>(errorCode);
    const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
    const QString adaptiveTextColor = QStringLiteral("#FFFFFF");

    QDialog decisionDialog(this);
    decisionDialog.setModal(true);
    decisionDialog.setWindowTitle(QStringLiteral("KswordARK 驱动签名校验失败"));
    decisionDialog.setObjectName(QStringLiteral("ksUnsignedDriverFailureDialog"));
    decisionDialog.setMinimumWidth(680);
    decisionDialog.setStyleSheet(KswordTheme::OpaqueDialogStyle(decisionDialog.objectName()));

    QVBoxLayout* rootLayout = new QVBoxLayout(&decisionDialog);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(10);

    QLabel* failureReasonLabel = new QLabel(
        QStringLiteral(
            "驱动加载失败，失败原因是系统拒绝了未通过数字签名校验的内核驱动。\n\n"
            "操作阶段：%1\n错误码：%2\n系统信息：%3")
        .arg(operationText)
        .arg(win32ErrorCode)
        .arg(formatWin32ErrorText(win32ErrorCode)),
        &decisionDialog);
    failureReasonLabel->setWordWrap(true);
    failureReasonLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(failureReasonLabel);

    QLabel* signatureMechanismLabel = new QLabel(
        QStringLiteral(
            "Windows 内核驱动默认启用“强制数字签名”机制：系统在加载 .sys 时会校验证书链与镜像完整性。"
            "当驱动未签名、签名损坏或证书不被信任时，系统会阻止加载。"),
        &decisionDialog);
    signatureMechanismLabel->setWordWrap(true);
    rootLayout->addWidget(signatureMechanismLabel);

    QLabel* actionTitleLabel = new QLabel(QStringLiteral("我可以做什么？"), &decisionDialog);
    actionTitleLabel->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;"));
    rootLayout->addWidget(actionTitleLabel);

    QLabel* testModeDescriptionLabel = new QLabel(
        QStringLiteral(
            "测试模式（Test Signing）会放宽驱动签名限制，便于开发调试。\n"
            "风险说明：\n"
            "1. 未经签名的恶意驱动同样可以加载。\n"
            "2. 反作弊程序会阻止所有游戏启动。\n"
            "3. 开启和关闭测试模式都需要重启电脑。"),
        &decisionDialog);
    testModeDescriptionLabel->setWordWrap(true);
    rootLayout->addWidget(testModeDescriptionLabel);

    QPushButton* continueR3Button = new QPushButton(QStringLiteral("退出并继续使用R3功能"), &decisionDialog);
    continueR3Button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    continueR3Button->setMinimumHeight(42);
    continueR3Button->setStyleSheet(QStringLiteral(
        "QPushButton{"
        "  background:%1;"
        "  color:%2;"
        "  border:1px solid %1;"
        "  border-radius:4px;"
        "  font-weight:700;"
        "}"
        "QPushButton:hover{"
        "  background:%4;"
        "}"
        "QPushButton:pressed{"
        "  background:%3;"
        "}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(adaptiveTextColor)
        .arg(KswordTheme::PrimaryBluePressedHex)
        .arg(KswordTheme::PrimaryBlueSolidHoverHex()));
    rootLayout->addWidget(continueR3Button);

    QPushButton* enableTestModeButton = new QPushButton(QStringLiteral("开启测试模式"), &decisionDialog);
    enableTestModeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    enableTestModeButton->setMinimumHeight(42);
    enableTestModeButton->setStyleSheet(QStringLiteral(
        "QPushButton{"
        "  background:%1;"
        "  color:%2;"
        "  border:1px solid %2;"
        "  border-radius:4px;"
        "  font-weight:700;"
        "}"
        "QPushButton:hover{"
        "  background:%3;"
        "}"
        "QPushButton:pressed{"
        "  background:%4;"
        "}")
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::PrimaryBlueSubtleHex())
        .arg(darkModeEnabled ? QStringLiteral("#10283E") : QStringLiteral("#D6ECFF")));
    rootLayout->addWidget(enableTestModeButton);

    bool enableTestMode = false;
    connect(continueR3Button, &QPushButton::clicked, &decisionDialog, [&decisionDialog]() {
        decisionDialog.done(QDialog::Rejected);
    });
    connect(enableTestModeButton, &QPushButton::clicked, &decisionDialog, [&decisionDialog, &enableTestMode]() {
        enableTestMode = true;
        decisionDialog.done(QDialog::Accepted);
    });

    decisionDialog.exec();
    if (!enableTestMode)
    {
        return true;
    }
    return enableWindowsTestModeAndPromptReboot();
}

bool MainWindow::enableWindowsTestModeAndPromptReboot()
{
    if (!hasAdminPrivilege())
    {
        showR0FatalError(
            QStringLiteral("开启测试模式失败：当前进程没有管理员权限。"),
            ERROR_ACCESS_DENIED,
            QStringLiteral("请先以管理员身份运行 Ksword5.1。"));
        return false;
    }

    QProcess bcdeditProcess;
    bcdeditProcess.start(QStringLiteral("bcdedit"), { QStringLiteral("/set"), QStringLiteral("testsigning"), QStringLiteral("on") });
    if (!bcdeditProcess.waitForStarted(5000))
    {
        showR0FatalError(
            QStringLiteral("开启测试模式失败：无法启动 bcdedit。"),
            ERROR_GEN_FAILURE,
            bcdeditProcess.errorString());
        return false;
    }
    if (!bcdeditProcess.waitForFinished(15000))
    {
        bcdeditProcess.kill();
        bcdeditProcess.waitForFinished(3000);
        showR0FatalError(
            QStringLiteral("开启测试模式失败：bcdedit 执行超时。"),
            ERROR_TIMEOUT);
        return false;
    }

    const QString standardOutput = QString::fromLocal8Bit(bcdeditProcess.readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromLocal8Bit(bcdeditProcess.readAllStandardError()).trimmed();
    if (bcdeditProcess.exitStatus() != QProcess::NormalExit || bcdeditProcess.exitCode() != 0)
    {
        QString detailText = QStringLiteral("退出码：%1").arg(bcdeditProcess.exitCode());
        if (!standardOutput.isEmpty())
        {
            detailText += QStringLiteral("\nstdout：%1").arg(standardOutput);
        }
        if (!standardError.isEmpty())
        {
            detailText += QStringLiteral("\nstderr：%1").arg(standardError);
        }
        showR0FatalError(
            QStringLiteral("开启测试模式失败：bcdedit 返回错误。"),
            ERROR_GEN_FAILURE,
            detailText);
        return false;
    }

    QMessageBox rebootDialog(this);
    rebootDialog.setIcon(QMessageBox::Question);
    rebootDialog.setWindowTitle(QStringLiteral("测试模式已设置"));
    rebootDialog.setText(QStringLiteral("已执行 bcdedit /set testsigning on。"));
    rebootDialog.setInformativeText(QStringLiteral("需要重启电脑后才会生效。你可以选择稍后重启或现在重启。"));
    QPushButton* rebootLaterButton = rebootDialog.addButton(QStringLiteral("稍后重启"), QMessageBox::RejectRole);
    QPushButton* rebootNowButton = rebootDialog.addButton(QStringLiteral("现在重启"), QMessageBox::AcceptRole);
    rebootDialog.exec();

    if (rebootDialog.clickedButton() == rebootNowButton)
    {
        if (!QProcess::startDetached(QStringLiteral("shutdown"), { QStringLiteral("/r"), QStringLiteral("/t"), QStringLiteral("0") }))
        {
            showR0FatalError(
                QStringLiteral("立即重启失败：无法调用 shutdown 命令。"),
                ERROR_GEN_FAILURE);
            return false;
        }
    }
    else if (rebootDialog.clickedButton() == rebootLaterButton)
    {
        kLogEvent logEvent;
        info << logEvent << "[MainWindow][R0] 用户选择稍后重启，测试模式将在下次重启后生效。" << eol;
    }

    return true;
}

bool MainWindow::startR0DriverService()
{
    const QString driverPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("KswordARK.sys"));
    const QString nativeDriverPath = QDir::toNativeSeparators(driverPath);
    const QFileInfo driverFileInfo(driverPath);
    if (!driverFileInfo.exists() || !driverFileInfo.isFile())
    {
        showR0FatalError(
            QStringLiteral("R0 启动失败：当前程序目录下不存在 KswordARK.sys。"),
            ERROR_FILE_NOT_FOUND,
            QStringLiteral("期望路径：%1").arg(nativeDriverPath));
        return false;
    }

    ScopedServiceHandle scmHandle(::OpenSCManagerW(
        nullptr,
        SERVICES_ACTIVE_DATABASE,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scmHandle.isValid())
    {
        showR0FatalError(
            QStringLiteral("R0 启动失败：无法连接服务控制管理器。"),
            ::GetLastError());
        return false;
    }

    ScopedServiceHandle serviceHandle(::OpenServiceW(
        scmHandle.get(),
        kR0DriverServiceName,
        SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | SERVICE_CHANGE_CONFIG | DELETE));
    if (!serviceHandle.isValid())
    {
        const DWORD openError = ::GetLastError();
        if (openError != ERROR_SERVICE_DOES_NOT_EXIST)
        {
            showR0FatalError(
                QStringLiteral("R0 启动失败：无法打开已有驱动服务。"),
                openError);
            return false;
        }

        const std::wstring driverPathWide = nativeDriverPath.toStdWString();
        serviceHandle.reset(::CreateServiceW(
            scmHandle.get(),
            kR0DriverServiceName,
            kR0DriverDisplayName,
            SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | SERVICE_CHANGE_CONFIG | DELETE,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            driverPathWide.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr));
        if (!serviceHandle.isValid())
        {
            showR0FatalError(
                QStringLiteral("R0 启动失败：创建驱动服务失败。"),
                ::GetLastError(),
                QStringLiteral("驱动路径：%1").arg(nativeDriverPath));
            return false;
        }
    }
    else
    {
        const std::wstring driverPathWide = nativeDriverPath.toStdWString();
        if (::ChangeServiceConfigW(
            serviceHandle.get(),
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            driverPathWide.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            kR0DriverDisplayName) == FALSE)
        {
            showR0FatalError(
                QStringLiteral("R0 启动失败：更新驱动服务配置失败。"),
                ::GetLastError());
            return false;
        }
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD queryError = ERROR_SUCCESS;
    if (!queryServiceStatus(serviceHandle.get(), status, queryError))
    {
        showR0FatalError(
            QStringLiteral("R0 启动失败：读取服务状态失败。"),
            queryError);
        return false;
    }

    if (isRunningLikeServiceState(status.dwCurrentState))
    {
        m_r0DriverServiceRunning = true;
        return true;
    }

    if (::StartServiceW(serviceHandle.get(), 0, nullptr) == FALSE)
    {
        const DWORD startError = ::GetLastError();
        if (startError == ERROR_SERVICE_ALREADY_RUNNING)
        {
            m_r0DriverServiceRunning = true;
            return true;
        }

        if (isR0DriverSignatureFailure(startError))
        {
            kLogEvent logEvent;
            fatal << logEvent
                << "[MainWindow][R0][Fatal] 驱动签名校验失败, error="
                << startError
                << eol;
            showUnsignedDriverFailureDialog(
                startError,
                QStringLiteral("启动 KswordARK 驱动服务"));
            return false;
        }

        showR0FatalError(
            QStringLiteral("R0 启动失败：驱动服务启动失败。"),
            startError,
            QStringLiteral("驱动路径：%1").arg(nativeDriverPath));
        return false;
    }

    SERVICE_STATUS_PROCESS latestStatus{};
    DWORD waitError = ERROR_SUCCESS;
    if (!waitServiceState(
        serviceHandle.get(),
        SERVICE_RUNNING,
        kR0ServiceWaitTimeoutMs,
        latestStatus,
        waitError))
    {
        if (waitError != ERROR_SUCCESS)
        {
            showR0FatalError(
                QStringLiteral("R0 启动失败：等待服务运行时查询状态失败。"),
                waitError);
        }
        else
        {
            showR0FatalError(
                QStringLiteral("R0 启动失败：等待驱动进入运行态超时。"),
                ERROR_TIMEOUT,
                QStringLiteral("当前状态：%1").arg(serviceStateToText(latestStatus.dwCurrentState)));
        }
        return false;
    }

    m_r0DriverServiceRunning = true;
    kLogEvent logEvent;
    info << logEvent << "[MainWindow][R0] 已创建并启动 KswordARK 驱动服务。" << eol;
    return true;
}

void MainWindow::handleR0StatusButtonClicked()
{
    bool runningNow = false;
    if (!queryR0DriverServiceRunning(runningNow, true))
    {
        return;
    }
    m_r0DriverServiceRunning = runningNow;

    if (runningNow)
    {
        if (!stopR0DriverService())
        {
            refreshPrivilegeStatusButtons();
            return;
        }
        m_r0DriverServiceRunning = false;
        refreshPrivilegeStatusButtons();
        return;
    }

    if (!startR0DriverService())
    {
        m_r0DriverServiceRunning = false;
        refreshPrivilegeStatusButtons();
        return;
    }

    bool finalRunningState = false;
    if (!queryR0DriverServiceRunning(finalRunningState, true))
    {
        refreshPrivilegeStatusButtons();
        return;
    }
    m_r0DriverServiceRunning = finalRunningState;
    refreshPrivilegeStatusButtons();
}

void MainWindow::requestAdminElevationRestart()
{
    // 获取当前可执行文件路径，作为 runas 重启目标。
    wchar_t exePathBuffer[MAX_PATH] = {};
    const DWORD pathLength = ::GetModuleFileNameW(nullptr, exePathBuffer, static_cast<DWORD>(std::size(exePathBuffer)));
    if (pathLength == 0 || pathLength >= std::size(exePathBuffer))
    {
        const DWORD lastError = ::GetLastError();
        QMessageBox::warning(this, "Admin", QString("读取当前程序路径失败，错误码: %1").arg(lastError));
        return;
    }

    // 使用 ShellExecute("runas") 触发 UAC 提权。
    HINSTANCE shellResult = ::ShellExecuteW(
        nullptr,
        L"runas",
        exePathBuffer,
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(shellResult) <= 32)
    {
        QMessageBox::warning(this, "Admin", "提权启动失败，可能被用户取消或系统策略阻止。");
        return;
    }

    // 新进程启动成功后，当前实例退出。
    kLogEvent logEvent;
    info << logEvent << "[MainWindow] 已触发管理员重启，当前实例即将退出。" << eol;
    QApplication::quit();
}

bool MainWindow::hasAdminPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

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

bool MainWindow::hasDebugPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

    DWORD requiredLength = 0;
    ::GetTokenInformation(tokenHandle, TokenPrivileges, nullptr, 0, &requiredLength);
    if (requiredLength == 0)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    std::vector<BYTE> privilegeBuffer(requiredLength, 0);
    if (::GetTokenInformation(
        tokenHandle,
        TokenPrivileges,
        privilegeBuffer.data(),
        requiredLength,
        &requiredLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    LUID debugLuid{};
    if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debugLuid) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    const TOKEN_PRIVILEGES* tokenPrivileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(privilegeBuffer.data());
    for (DWORD privilegeIndex = 0; privilegeIndex < tokenPrivileges->PrivilegeCount; ++privilegeIndex)
    {
        const LUID_AND_ATTRIBUTES& privilegeItem = tokenPrivileges->Privileges[privilegeIndex];
        if (privilegeItem.Luid.LowPart == debugLuid.LowPart &&
            privilegeItem.Luid.HighPart == debugLuid.HighPart)
        {
            const bool enabled = (privilegeItem.Attributes & SE_PRIVILEGE_ENABLED) != 0;
            ::CloseHandle(tokenHandle);
            return enabled;
        }
    }

    ::CloseHandle(tokenHandle);
    return false;
}

bool MainWindow::hasSystemPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

    DWORD requiredLength = 0;
    ::GetTokenInformation(tokenHandle, TokenUser, nullptr, 0, &requiredLength);
    if (requiredLength == 0)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    std::vector<BYTE> userBuffer(requiredLength, 0);
    if (::GetTokenInformation(
        tokenHandle,
        TokenUser,
        userBuffer.data(),
        requiredLength,
        &requiredLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    BYTE systemSidBuffer[SECURITY_MAX_SID_SIZE] = {};
    DWORD systemSidLength = static_cast<DWORD>(std::size(systemSidBuffer));
    if (::CreateWellKnownSid(
        WinLocalSystemSid,
        nullptr,
        systemSidBuffer,
        &systemSidLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
    const bool isSystem = (::EqualSid(tokenUser->User.Sid, systemSidBuffer) != FALSE);
    ::CloseHandle(tokenHandle);
    return isSystem;
}

bool MainWindow::hasTrustedInstallerPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

    DWORD requiredLength = 0;
    ::GetTokenInformation(tokenHandle, TokenUser, nullptr, 0, &requiredLength);
    if (requiredLength == 0)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    std::vector<BYTE> userBuffer(requiredLength, 0);
    if (::GetTokenInformation(
        tokenHandle,
        TokenUser,
        userBuffer.data(),
        requiredLength,
        &requiredLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    PSID trustedInstallerSid = nullptr;
    const BOOL sidOk = ::ConvertStringSidToSidW(
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
        &trustedInstallerSid);
    if (sidOk == FALSE || trustedInstallerSid == nullptr)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
    const bool isTrustedInstaller = (::EqualSid(tokenUser->User.Sid, trustedInstallerSid) != FALSE);

    ::LocalFree(trustedInstallerSid);
    ::CloseHandle(tokenHandle);
    return isTrustedInstaller;
}

bool MainWindow::enableSeDebugPrivilege(std::string& errorTextOut) const
{
    errorTextOut.clear();

    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(
        ::GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        &tokenHandle) == FALSE)
    {
        errorTextOut = "OpenProcessToken failed, error=" + std::to_string(::GetLastError());
        return false;
    }

    LUID debugLuid{};
    if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debugLuid) == FALSE)
    {
        errorTextOut = "LookupPrivilegeValue(SE_DEBUG_NAME) failed, error=" + std::to_string(::GetLastError());
        ::CloseHandle(tokenHandle);
        return false;
    }

    TOKEN_PRIVILEGES tokenPrivileges{};
    tokenPrivileges.PrivilegeCount = 1;
    tokenPrivileges.Privileges[0].Luid = debugLuid;
    tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (::AdjustTokenPrivileges(
        tokenHandle,
        FALSE,
        &tokenPrivileges,
        sizeof(tokenPrivileges),
        nullptr,
        nullptr) == FALSE)
    {
        errorTextOut = "AdjustTokenPrivileges failed, error=" + std::to_string(::GetLastError());
        ::CloseHandle(tokenHandle);
        return false;
    }

    const DWORD adjustError = ::GetLastError();
    ::CloseHandle(tokenHandle);
    if (adjustError != ERROR_SUCCESS)
    {
        errorTextOut = "AdjustTokenPrivileges returned error=" + std::to_string(adjustError);
        return false;
    }
    return true;
}

QWidget* MainWindow::createDockPlaceholderWidget(const QString& titleText) const
{
    QWidget* placeholderWidget = new QWidget();
    placeholderWidget->setObjectName(QStringLiteral("ksLazyDockPlaceholder_%1").arg(titleText));
    placeholderWidget->setAutoFillBackground(false);
    placeholderWidget->setAttribute(Qt::WA_StyledBackground, false);
    placeholderWidget->setStyleSheet(
        QStringLiteral(
            "QWidget{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "}"
            "QLabel{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "}"));

    auto* placeholderLayout = new QVBoxLayout(placeholderWidget);
    placeholderLayout->setContentsMargins(24, 24, 24, 24);
    placeholderLayout->setSpacing(8);

    QLabel* titleLabel = new QLabel(QStringLiteral("%1 页面正在延迟初始化...").arg(titleText), placeholderWidget);
    titleLabel->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;"));
    titleLabel->setAlignment(Qt::AlignCenter);
    placeholderLayout->addStretch(1);
    placeholderLayout->addWidget(titleLabel);

    QLabel* hintLabel = new QLabel(QStringLiteral("主窗口已优先完成首屏加载，页面内容将在首次打开时加载。"), placeholderWidget);
    hintLabel->setWordWrap(true);
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setStyleSheet(QStringLiteral("font-size:12px;color:%1;").arg(KswordTheme::TextSecondaryHex()));
    placeholderLayout->addWidget(hintLabel);
    placeholderLayout->addStretch(1);
    return placeholderWidget;
}

void MainWindow::ensureDockContentInitialized(ads::CDockWidget* dockWidget)
{
    if (dockWidget == nullptr)
    {
        return;
    }
    if (dockWidget->property("ks_lazy_initialized").toBool())
    {
        return;
    }

    const QString dockKey = dockWidget->property("ks_lazy_key").toString().trimmed().toLower();
    const QString dockTitleText = dockWidget->windowTitle().trimmed().isEmpty()
        ? dockKey
        : dockWidget->windowTitle().trimmed();
    const int progressPid = kPro.add("页面", QStringLiteral("打开%1页").arg(dockTitleText).toStdString());
    kPro.set(progressPid, QStringLiteral("准备加载%1页").arg(dockTitleText).toStdString(), 0, 8.0f);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const bool isNetworkDock = (dockKey == QStringLiteral("network"));
    QWidget* realWidget = nullptr;

    if (dockKey == QStringLiteral("process"))
    {
        if (m_processWidget == nullptr) { m_processWidget = new ProcessDock(this); }
        realWidget = m_processWidget;
    }
    else if (dockKey == QStringLiteral("network"))
    {
        if (m_networkWidget == nullptr) { m_networkWidget = new NetworkDock(this); }
        realWidget = m_networkWidget;
    }
    else if (dockKey == QStringLiteral("memory"))
    {
        if (m_memoryWidget == nullptr) { m_memoryWidget = new MemoryDock(this); }
        realWidget = m_memoryWidget;
    }
    else if (dockKey == QStringLiteral("file"))
    {
        if (m_fileWidget == nullptr) { m_fileWidget = new FileDock(this); }
        realWidget = m_fileWidget;
    }
    else if (dockKey == QStringLiteral("driver"))
    {
        if (m_driverWidget == nullptr) { m_driverWidget = new DriverDock(this); }
        realWidget = m_driverWidget;
    }
    else if (dockKey == QStringLiteral("kernel"))
    {
        if (m_kernelWidget == nullptr) { m_kernelWidget = new KernelDock(this); }
        realWidget = m_kernelWidget;
    }
    else if (dockKey == QStringLiteral("monitor"))
    {
        if (m_monitorWidget == nullptr) { m_monitorWidget = new MonitorDock(this); }
        realWidget = m_monitorWidget;
    }
    else if (dockKey == QStringLiteral("hardware"))
    {
        if (m_hardwareWidget == nullptr) { m_hardwareWidget = new HardwareDock(this); }
        realWidget = m_hardwareWidget;
    }
    else if (dockKey == QStringLiteral("privilege"))
    {
        if (m_privilegeWidget == nullptr) { m_privilegeWidget = new PrivilegeDock(this); }
        realWidget = m_privilegeWidget;
    }
    else if (dockKey == QStringLiteral("window"))
    {
        if (m_windowWidget == nullptr) { m_windowWidget = new WindowDock(this); }
        realWidget = m_windowWidget;
    }
    else if (dockKey == QStringLiteral("registry"))
    {
        if (m_registryWidget == nullptr) { m_registryWidget = new RegistryDock(this); }
        realWidget = m_registryWidget;
    }
    else if (dockKey == QStringLiteral("handle"))
    {
        if (m_handleWidget == nullptr) { m_handleWidget = new HandleDock(this); }
        realWidget = m_handleWidget;
    }
    else if (dockKey == QStringLiteral("startup"))
    {
        if (m_startupWidget == nullptr) { m_startupWidget = new StartupDock(this); }
        realWidget = m_startupWidget;
    }
    else if (dockKey == QStringLiteral("service"))
    {
        if (m_serviceWidget == nullptr) { m_serviceWidget = new ServiceDock(this); }
        realWidget = m_serviceWidget;
    }
    else if (dockKey == QStringLiteral("misc"))
    {
        if (m_miscWidget == nullptr) { m_miscWidget = new MiscDock(this); }
        realWidget = m_miscWidget;
    }
    if (realWidget == nullptr)
    {
        kPro.set(progressPid, QStringLiteral("%1页无需加载").arg(dockTitleText).toStdString(), 0, 100.0f);
        return;
    }

    kPro.set(progressPid, QStringLiteral("正在创建%1页内容").arg(dockTitleText).toStdString(), 0, 45.0f);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    realWidget->setAutoFillBackground(false);
    realWidget->setAttribute(Qt::WA_StyledBackground, false);
    realWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    realWidget->setStyleSheet(
        realWidget->styleSheet()
        + QStringLiteral(
            "QWidget{"
            "  background:transparent;"
            "  background-color:transparent;"
            "}"));

    if (isNetworkDock)
    {
        // 网络页额外要求：
        // 1) 不再让 ADS 自动包一层外部 QScrollArea；
        // 2) 把整个网络 Dock 的最小高度抬到 300，便于验证问题是否位于最外层 Dock 容器。
        realWidget->setMinimumHeight(300);
        dockWidget->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidgetMinimumSize);
        dockWidget->setMinimumHeight(300);
        dockWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    kPro.set(progressPid, QStringLiteral("正在挂载%1页").arg(dockTitleText).toStdString(), 0, 72.0f);
    QWidget* oldWidget = dockWidget->takeWidget();
    dockWidget->setWidget(
        realWidget,
        isNetworkDock ? ads::CDockWidget::ForceNoScrollArea : ads::CDockWidget::AutoScrollArea);
    dockWidget->setProperty("ks_lazy_initialized", true);
    if (oldWidget != nullptr)
    {
        oldWidget->deleteLater();
    }

    if (realWidget == m_processWidget)
    {
        m_processWidget->refreshThemeVisuals();
    }

    // 延迟补载时只做局部刷新，不再全局重算外观，避免每个 Dock 都触发一次大范围重绘。
    realWidget->setPalette(palette());
    realWidget->update();
    dockWidget->update();
    kPro.set(progressPid, QStringLiteral("%1页加载完成").arg(dockTitleText).toStdString(), 0, 100.0f);
}

void MainWindow::initializeNextDeferredDock()
{
    while (m_nextDeferredDockIndex < m_deferredDockLoadQueue.size())
    {
        ads::CDockWidget* dockWidget = m_deferredDockLoadQueue[m_nextDeferredDockIndex++];
        if (dockWidget == nullptr || dockWidget->property("ks_lazy_initialized").toBool())
        {
            continue;
        }

        ensureDockContentInitialized(dockWidget);
        QTimer::singleShot(kDeferredDockLoadIntervalMs, this, [this]()
            {
                initializeNextDeferredDock();
            });
        return;
    }

    reportStartupProgress(98, QStringLiteral("剩余页面补载完成。"));
}

void MainWindow::initDockWidgets()
{
    const QString startupDockKey = m_currentAppearanceSettings.startupDefaultTabKey.trimmed().toLower();
    const auto shouldEagerLoad = [&startupDockKey](const QString& dockKey) -> bool
        {
            return dockKey == QStringLiteral("welcome") ||
                (startupDockKey == QStringLiteral("winapi") && dockKey == QStringLiteral("monitor")) ||
                dockKey == startupDockKey;
        };

    // 首屏优先：欢迎页、启动默认页签，以及右侧/底部辅助组件；设置改由顶部菜单即时打开。
    reportStartupProgress(50, QStringLiteral("正在创建首屏页面..."));
    m_welcomeWidget = new WelcomeDock(this);
    if (shouldEagerLoad(QStringLiteral("process"))) { m_processWidget = new ProcessDock(this); }
    if (shouldEagerLoad(QStringLiteral("network"))) { m_networkWidget = new NetworkDock(this); }
    if (shouldEagerLoad(QStringLiteral("memory"))) { m_memoryWidget = new MemoryDock(this); }
    if (shouldEagerLoad(QStringLiteral("file"))) { m_fileWidget = new FileDock(this); }
    if (shouldEagerLoad(QStringLiteral("driver"))) { m_driverWidget = new DriverDock(this); }
    if (shouldEagerLoad(QStringLiteral("kernel"))) { m_kernelWidget = new KernelDock(this); }
    if (shouldEagerLoad(QStringLiteral("monitor"))) { m_monitorWidget = new MonitorDock(this); }
    if (shouldEagerLoad(QStringLiteral("hardware"))) { m_hardwareWidget = new HardwareDock(this); }
    if (shouldEagerLoad(QStringLiteral("privilege"))) { m_privilegeWidget = new PrivilegeDock(this); }
    if (shouldEagerLoad(QStringLiteral("window"))) { m_windowWidget = new WindowDock(this); }
    if (shouldEagerLoad(QStringLiteral("registry"))) { m_registryWidget = new RegistryDock(this); }
    if (shouldEagerLoad(QStringLiteral("handle"))) { m_handleWidget = new HandleDock(this); }
    if (shouldEagerLoad(QStringLiteral("startup"))) { m_startupWidget = new StartupDock(this); }
    if (shouldEagerLoad(QStringLiteral("service"))) { m_serviceWidget = new ServiceDock(this); }
    if (shouldEagerLoad(QStringLiteral("misc"))) { m_miscWidget = new MiscDock(this); }

    reportStartupProgress(60, QStringLiteral("正在创建辅助组件..."));
    m_monitorPanelWidget = new MonitorPanelWidget(this);
    m_logWidget = new LogDockWidget(this);
    m_progressWidget = new ProgressDockWidget(this);
    m_immediateEditorWidget = new CodeEditorWidget(this);

    // 创建 Dock 容器前再推进一次启动进度，避免长时间停留在单一文案。
    reportStartupProgress(68, QStringLiteral("正在封装 Dock 容器..."));

    // 使用辅助函数创建Dock Widgets。
    auto createDockWidget = [this](
        QWidget* widget,
        const QString& title,
        const ads::CDockWidget::eInsertMode insertMode = ads::CDockWidget::AutoScrollArea) -> ads::CDockWidget* {
        ads::CDockWidget* dock = new ads::CDockWidget(title);
        dock->setWidget(widget, insertMode);
        // DockWidgetClosable 禁用：统一去掉每个 Dock 标签旁边的关闭叉。
        dock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
        dock->setFeature(ads::CDockWidget::DockWidgetMovable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetFloatable, true);

        // Dock 背景属性初始化：
        // - 默认关闭 Dock 与内容根控件的自动背景填充；
        // - 避免背景图模式下被黑/白纯色底覆盖。
        dock->setAutoFillBackground(false);
        dock->setAttribute(Qt::WA_StyledBackground, false);
        if (widget != nullptr)
        {
            widget->setAutoFillBackground(false);
            widget->setAttribute(Qt::WA_StyledBackground, false);
        }
        return dock;
        };

    auto createLazyDockWidget = [this, &createDockWidget](
        ads::CDockWidget*& dockOut,
        QWidget* eagerWidget,
        const QString& title,
        const QString& dockKey)
        {
            const bool isNetworkDock = (dockKey == QStringLiteral("network"));
            QWidget* dockContentWidget = eagerWidget;
            if (dockContentWidget == nullptr)
            {
                dockContentWidget = createDockPlaceholderWidget(title);
            }

            dockContentWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            dockOut = createDockWidget(
                dockContentWidget,
                title,
                isNetworkDock ? ads::CDockWidget::ForceNoScrollArea : ads::CDockWidget::AutoScrollArea);
            dockOut->setProperty("ks_lazy_key", dockKey);
            dockOut->setProperty("ks_lazy_initialized", eagerWidget != nullptr);
            if (isNetworkDock)
            {
                dockContentWidget->setMinimumHeight(300);
                dockOut->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidgetMinimumSize);
                dockOut->setMinimumHeight(300);
                dockOut->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            }
            connect(dockOut, &ads::CDockWidget::visibilityChanged, this, [this, dockOut](const bool visible)
                {
                    if (visible)
                    {
                        QTimer::singleShot(0, this, [this, dockOut]() {
                            ensureDockContentInitialized(dockOut);
                            });
                    }
                });

            if (eagerWidget == nullptr)
            {
                m_deferredDockLoadQueue.push_back(dockOut);
            }
        };

    // setupDockTabText 作用：统一主 Dock Tab 的文本省略策略，并允许按内容自适应宽度。
    const auto setupDockTabText = [](ads::CDockWidget* dockWidget) {
        if (dockWidget == nullptr || dockWidget->tabWidget() == nullptr)
        {
            return;
        }

        ads::CDockWidgetTab* tabWidget = dockWidget->tabWidget();
        tabWidget->setElideMode(Qt::ElideNone);
        tabWidget->setMinimumWidth(0);
        tabWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    };

    // 创建所有 Dock 壳；重页面若未预加载，则先挂占位页并排入显示后补载队列。
    m_dockWelcome = createDockWidget(m_welcomeWidget, "欢迎");
    createLazyDockWidget(m_dockProcess, m_processWidget, "进程", QStringLiteral("process"));
    createLazyDockWidget(m_dockNetwork, m_networkWidget, "网络", QStringLiteral("network"));
    createLazyDockWidget(m_dockMemory, m_memoryWidget, "内存", QStringLiteral("memory"));
    createLazyDockWidget(m_dockFile, m_fileWidget, "文件", QStringLiteral("file"));
    createLazyDockWidget(m_dockDriver, m_driverWidget, "驱动", QStringLiteral("driver"));
    createLazyDockWidget(m_dockKernel, m_kernelWidget, "内核", QStringLiteral("kernel"));
    createLazyDockWidget(m_dockMonitorTab, m_monitorWidget, "监控", QStringLiteral("monitor"));
    createLazyDockWidget(m_dockHardware, m_hardwareWidget, "硬件", QStringLiteral("hardware"));
    createLazyDockWidget(m_dockPrivilege, m_privilegeWidget, "权限", QStringLiteral("privilege"));
    createLazyDockWidget(m_dockWindow, m_windowWidget, "窗口", QStringLiteral("window"));
    createLazyDockWidget(m_dockRegistry, m_registryWidget, "注册表", QStringLiteral("registry"));
    createLazyDockWidget(m_dockHandle, m_handleWidget, "句柄", QStringLiteral("handle"));
    createLazyDockWidget(m_dockStartup, m_startupWidget, "启动项", QStringLiteral("startup"));
    createLazyDockWidget(m_dockService, m_serviceWidget, "服务", QStringLiteral("service"));
    createLazyDockWidget(m_dockMisc, m_miscWidget, "杂项", QStringLiteral("misc"));

    // 创建右侧和底部的基本Widgets
    m_dockCurrentOp = createDockWidget(m_progressWidget, "当前操作");
    m_dockLog = createDockWidget(m_logWidget, "日志输出");
    m_dockImmediate = createDockWidget(m_immediateEditorWidget, "即时窗口");
    // 左下角“监视面板”接入独立性能图组件（CPU每核/内存/磁盘/网络）。
    m_dockMonitor = createDockWidget(m_monitorPanelWidget, "监视面板");

    // 当前操作 Dock 专项透明策略：
    // - Dock 自身背景透明；
    // - 内容容器背景透明；
    // - 避免 ADS 默认底色盖住壁纸或主题底图。
    if (m_dockCurrentOp != nullptr)
    {
        m_dockCurrentOp->setObjectName(QStringLiteral("ksCurrentOperationDock"));
        m_dockCurrentOp->setStyleSheet(
            QStringLiteral(
            "#ksCurrentOperationDock,"
            "#ksCurrentOperationDock > QWidget,"
            "#ksCurrentOperationDock QScrollArea,"
            "#ksCurrentOperationDock QAbstractScrollArea,"
            "#ksCurrentOperationDock QAbstractScrollArea::viewport{"
            "  background:transparent;"
            "  background-color:transparent;"
            "  border:none;"
            "}"));
    }

    QList<ads::CDockWidget*> mainDockTabList{
        m_dockWelcome,
        m_dockProcess,
        m_dockNetwork,
        m_dockMemory,
        m_dockFile,
        m_dockDriver,
        m_dockKernel,
        m_dockMonitorTab,
        m_dockHardware,
        m_dockPrivilege,
        m_dockWindow,
        m_dockRegistry,
        m_dockHandle,
        m_dockStartup,
        m_dockService,
        m_dockMisc,
        m_dockCurrentOp,
        m_dockLog,
        m_dockImmediate,
        m_dockMonitor
    };
    for (ads::CDockWidget* dockWidget : mainDockTabList)
    {
        setupDockTabText(dockWidget);
    }

    // 顶部菜单栏已精简，不再注册 Dock 视图切换菜单。
    reportStartupProgress(72, QStringLiteral("跳过视图菜单注册"));
}
#define ADS_TABIFY_DOCK_WIDGET_AVAILABLE
void MainWindow::setupDockLayout()
{
    // 1. 初始化DockManager（若未在构造函数中初始化）
    if (!m_pDockManager) {
        QWidget* dockParentWidget = (m_mainRootContainer != nullptr)
            ? m_mainRootContainer
            : this;
        m_pDockManager = new ads::CDockManager(dockParentWidget);
        if (m_mainRootLayout != nullptr && m_mainRootContainer != nullptr)
        {
            m_mainRootLayout->addWidget(m_pDockManager, 1);
            if (centralWidget() != m_mainRootContainer)
            {
                setCentralWidget(m_mainRootContainer);
            }
        }
        else
        {
            setCentralWidget(m_pDockManager);
        }
    }

    // 2. 左侧区域：先添加第一个DockWidget，获取其所在的DockArea
    auto leftDockArea = m_pDockManager->addDockWidget(ads::LeftDockWidgetArea, m_dockWelcome);

    // 3. 使用正确的方法将其他DockWidget添加到同一个DockArea形成标签页
    // 方法1: 使用addDockWidgetTabToArea（推荐）
    m_pDockManager->addDockWidgetTabToArea(m_dockProcess, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockNetwork, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockMemory, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockFile, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockDriver, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockKernel, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockMonitorTab, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockHardware, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockPrivilege, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockWindow, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockRegistry, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockHandle, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockStartup, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockService, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockMisc, leftDockArea);

    // 方法2: 或者使用addDockWidget并指定CenterDockWidgetArea
    // m_pDockManager->addDockWidget(ads::CenterDockWidgetArea, m_dockProcess, leftDockArea);

    // 4. 右侧区域：同理
    auto rightDockArea = m_pDockManager->addDockWidget(ads::RightDockWidgetArea, m_dockCurrentOp);

    // 右侧仅保留“当前操作 + 即时窗口”。
    m_pDockManager->addDockWidgetTabToArea(m_dockImmediate, rightDockArea);

    // 5. 底部区域：按“监视面板 + 日志输出”左右分栏并占满底部。
    auto bottomDockArea = m_pDockManager->addDockWidget(ads::BottomDockWidgetArea, m_dockMonitor);
    m_pDockManager->addDockWidget(ads::RightDockWidgetArea, m_dockLog, bottomDockArea);

    // 6. 设置默认显示的标签页
    m_dockWelcome->raise();
    m_dockCurrentOp->raise();
    m_dockMonitor->raise();
}

void MainWindow::focusHandleDockByPid(const quint32 pid)
{
    // 跳转入口日志：记录来源请求 PID，便于串联调用链审计。
    kLogEvent focusHandleEvent;
    info << focusHandleEvent
        << "[MainWindow] focusHandleDockByPid: pid="
        << pid
        << eol;

    if (m_dockHandle != nullptr)
    {
        ensureDockContentInitialized(m_dockHandle);
    }
    if (m_handleWidget != nullptr)
    {
        m_handleWidget->focusProcessId(static_cast<std::uint32_t>(pid), true);
    }
    if (m_dockHandle != nullptr)
    {
        m_dockHandle->raise();
        m_dockHandle->setVisible(true);
    }
}

void MainWindow::focusMemoryDockByPid(const quint32 pid)
{
    // 跳转入口日志：记录目标 PID，并确保内存 Dock 惰性加载完成。
    kLogEvent focusMemoryEvent;
    info << focusMemoryEvent
        << "[MainWindow] focusMemoryDockByPid: pid="
        << pid
        << eol;

    if (m_dockMemory != nullptr)
    {
        ensureDockContentInitialized(m_dockMemory);
    }
    if (m_memoryWidget != nullptr)
    {
        m_memoryWidget->focusProcessForOperations(static_cast<std::uint32_t>(pid), false);
    }
    if (m_dockMemory != nullptr)
    {
        m_dockMemory->raise();
        m_dockMemory->setVisible(true);
    }
}

void MainWindow::openProcessDetailByPid(const quint32 pid)
{
    // 跳转入口日志：记录来自外部模块的 PID 跳转请求。
    kLogEvent openProcessDetailEvent;
    info << openProcessDetailEvent
        << "[MainWindow] openProcessDetailByPid: pid="
        << pid
        << eol;

    if (m_dockProcess != nullptr)
    {
        ensureDockContentInitialized(m_dockProcess);
    }
    if (m_processWidget != nullptr)
    {
        m_processWidget->requestOpenProcessDetailByPid(static_cast<std::uint32_t>(pid));
    }
    if (m_dockProcess != nullptr)
    {
        m_dockProcess->raise();
        m_dockProcess->setVisible(true);
    }
}

void MainWindow::focusServiceDockByName(const QString& serviceNameText)
{
    const QString normalizedServiceName = serviceNameText.trimmed();
    kLogEvent focusServiceEvent;
    info << focusServiceEvent
        << "[MainWindow] focusServiceDockByName: service="
        << normalizedServiceName.toStdString()
        << eol;

    if (m_dockService != nullptr)
    {
        ensureDockContentInitialized(m_dockService);
    }
    if (m_serviceWidget != nullptr && !normalizedServiceName.isEmpty())
    {
        m_serviceWidget->focusServiceByName(normalizedServiceName);
    }
    if (m_dockService != nullptr)
    {
        m_dockService->raise();
        m_dockService->setVisible(true);
    }
}

void MainWindow::openFileDetailDockByPath(const QString& filePath)
{
    const QString normalizedFilePath = QDir::toNativeSeparators(filePath.trimmed());
    if (normalizedFilePath.isEmpty())
    {
        return;
    }

    kLogEvent openFileDetailEvent;
    info << openFileDetailEvent
        << "[MainWindow] openFileDetailDockByPath: file="
        << normalizedFilePath.toStdString()
        << eol;

    if (m_dockFile != nullptr)
    {
        ensureDockContentInitialized(m_dockFile);
    }
    if (m_fileWidget != nullptr)
    {
        m_fileWidget->openFileDetailByPath(normalizedFilePath);
    }
    if (m_dockFile != nullptr)
    {
        m_dockFile->raise();
        m_dockFile->setVisible(true);
    }
}

void MainWindow::openFileUnlockerDockByPath(const QString& filePath)
{
    const QString normalizedFilePath = QDir::toNativeSeparators(filePath.trimmed());
    if (normalizedFilePath.isEmpty())
    {
        return;
    }

    kLogEvent unlockFileEvent;
    info << unlockFileEvent
        << "[MainWindow] openFileUnlockerDockByPath: path="
        << normalizedFilePath.toStdString()
        << eol;

    if (m_dockFile != nullptr)
    {
        ensureDockContentInitialized(m_dockFile);
        m_dockFile->raise();
        m_dockFile->setVisible(true);
    }
    if (m_fileWidget != nullptr)
    {
        m_fileWidget->unlockFileByPath(normalizedFilePath);
    }
}

void MainWindow::initAppearanceSettings()
{
    // appearanceInitEvent 作用：统一追踪外观系统初始化流程日志。
    kLogEvent appearanceInitEvent;
    info << appearanceInitEvent << "[MainWindow] 开始初始化外观设置系统。" << eol;

    // raiseStartupDockByKey 作用：
    // - 按配置 key 激活启动默认主页签；
    // - key 非法或目标 Dock 不可用时自动回退到欢迎页。
    const auto raiseStartupDockByKey = [this](const QString& startupDockKey, const QString& triggerReason) {
        const QString normalizedKey = startupDockKey.trimmed().toLower();
        ads::CDockWidget* targetDock = nullptr;
        QString targetName = QStringLiteral("欢迎");

        if (normalizedKey == QStringLiteral("process"))
        {
            targetDock = m_dockProcess;
            targetName = QStringLiteral("进程");
        }
        else if (normalizedKey == QStringLiteral("network"))
        {
            targetDock = m_dockNetwork;
            targetName = QStringLiteral("网络");
        }
        else if (normalizedKey == QStringLiteral("memory"))
        {
            targetDock = m_dockMemory;
            targetName = QStringLiteral("内存");
        }
        else if (normalizedKey == QStringLiteral("file"))
        {
            targetDock = m_dockFile;
            targetName = QStringLiteral("文件");
        }
        else if (normalizedKey == QStringLiteral("driver"))
        {
            targetDock = m_dockDriver;
            targetName = QStringLiteral("驱动");
        }
        else if (normalizedKey == QStringLiteral("kernel"))
        {
            targetDock = m_dockKernel;
            targetName = QStringLiteral("内核");
        }
        else if (normalizedKey == QStringLiteral("monitor"))
        {
            targetDock = m_dockMonitorTab;
            targetName = QStringLiteral("监控");
        }
        else if (normalizedKey == QStringLiteral("hardware"))
        {
            targetDock = m_dockHardware;
            targetName = QStringLiteral("硬件");
        }
        else if (normalizedKey == QStringLiteral("privilege"))
        {
            targetDock = m_dockPrivilege;
            targetName = QStringLiteral("权限");
        }
        else if (normalizedKey == QStringLiteral("settings"))
        {
            // 旧配置可能仍保存 settings；设置已移动到顶部菜单，启动时回退欢迎页避免自动弹窗。
            targetDock = m_dockWelcome;
            targetName = QStringLiteral("欢迎");
        }
        else if (normalizedKey == QStringLiteral("window"))
        {
            targetDock = m_dockWindow;
            targetName = QStringLiteral("窗口");
        }
        else if (normalizedKey == QStringLiteral("registry"))
        {
            targetDock = m_dockRegistry;
            targetName = QStringLiteral("注册表");
        }
        else if (normalizedKey == QStringLiteral("handle"))
        {
            targetDock = m_dockHandle;
            targetName = QStringLiteral("句柄");
        }
        else if (normalizedKey == QStringLiteral("startup"))
        {
            targetDock = m_dockStartup;
            targetName = QStringLiteral("启动项");
        }
        else if (normalizedKey == QStringLiteral("service"))
        {
            targetDock = m_dockService;
            targetName = QStringLiteral("服务");
        }
        else if (normalizedKey == QStringLiteral("misc"))
        {
            targetDock = m_dockMisc;
            targetName = QStringLiteral("杂项");
        }
        else if (normalizedKey == QStringLiteral("winapi"))
        {
            targetDock = m_dockMonitorTab;
            targetName = QStringLiteral("监控/WinAPI");
        }
        else
        {
            targetDock = m_dockWelcome;
            targetName = QStringLiteral("欢迎");
        }

        if (targetDock == nullptr)
        {
            targetDock = m_dockWelcome;
            targetName = QStringLiteral("欢迎");
        }

        if (targetDock != nullptr)
        {
            ensureDockContentInitialized(targetDock);
            if (normalizedKey == QStringLiteral("winapi") && m_monitorWidget != nullptr)
            {
                m_monitorWidget->activateMonitorTab(QStringLiteral("winapi"));
            }
            targetDock->raise();
            kLogEvent startupDockEvent;
            info << startupDockEvent
                << "[MainWindow] 已激活启动默认页签, trigger="
                << triggerReason.toStdString()
                << ", key="
                << normalizedKey.toStdString()
                << ", tab="
                << targetName.toStdString()
                << eol;
        }
    };

    // 启动画面细分：
    // - 设置页改为顶部菜单按需创建；
    // - 初始化时直接从 JSON 读取外观配置，再应用首轮主题样式。
    reportStartupProgress(86, QStringLiteral("正在读取外观设置..."));
    m_currentAppearanceSettings = ks::settings::loadAppearanceSettings();

    QStyleHints* styleHints = QGuiApplication::styleHints();
    if (styleHints != nullptr)
    {
        connect(styleHints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme /*newScheme*/) {
            if (m_currentAppearanceSettings.themeMode == ks::settings::ThemeMode::FollowSystem)
            {
                applyAppearanceSettings(m_currentAppearanceSettings, QStringLiteral("系统颜色方案变化"));
            }
            });
    }

    reportStartupProgress(90, QStringLiteral("正在应用主界面主题..."));
    applyAppearanceSettings(m_currentAppearanceSettings, QStringLiteral("初始化加载"));
    raiseStartupDockByKey(m_currentAppearanceSettings.startupDefaultTabKey, QStringLiteral("初始化加载"));
    info << appearanceInitEvent << "[MainWindow] 外观设置系统初始化完成。" << eol;
}

void MainWindow::applyAppearanceSettings(
    const ks::settings::AppearanceSettings& settings,
    const QString& triggerReason)
{
    // appearanceApplyEvent 作用：本次“外观应用”过程统一日志事件对象。
    kLogEvent appearanceApplyEvent;

    m_currentAppearanceSettings = settings;

    const bool darkModeEnabled = isDarkModeEffective(settings);
    KswordTheme::SetDarkModeEnabled(darkModeEnabled);
    const QColor windowBackgroundColor = KswordTheme::WindowColor();
    const QColor textColor = KswordTheme::TextPrimaryColor();
    const QColor baseColor = KswordTheme::SurfaceColor();
    const QColor alternateBaseColor = KswordTheme::SurfaceAltColor();
    const QColor midColor = KswordTheme::BorderColor();

    // enableDockTransparencyForBackgroundImage 作用：
    // - 当背景图可用时，把 Dock 系统背景整体切换为透明；
    // - 满足“加载背景图后 Dock 黑白底必须全部透明”的需求。
    const bool enableDockTransparencyForBackgroundImage =
        isBackgroundImageReady(settings.backgroundImagePath);

    // 把深浅色状态写入全局属性，供各 Dock 在绘制/着色时读取。
    if (QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance()))
    {
        appInstance->setProperty("ksword_slider_wheel_adjust_enabled", settings.sliderWheelAdjustEnabled);
    }

    // Windows 11 背景控制要求：
    // 即使是纯黑/纯白，也必须显式设置 Window 颜色，避免系统接管背景。
    QPalette mainPalette = palette();
    mainPalette.setColor(QPalette::Window, windowBackgroundColor);
    mainPalette.setColor(QPalette::WindowText, textColor);
    mainPalette.setColor(QPalette::Base, baseColor);
    mainPalette.setColor(QPalette::AlternateBase, alternateBaseColor);
    mainPalette.setColor(QPalette::Mid, midColor);
    mainPalette.setColor(QPalette::Midlight, KswordTheme::BorderStrongColor());
    mainPalette.setColor(QPalette::Dark, darkModeEnabled ? QColor(20, 30, 42) : QColor(144, 165, 188));
    mainPalette.setColor(QPalette::Text, textColor);
    mainPalette.setColor(QPalette::Button, alternateBaseColor);
    mainPalette.setColor(QPalette::ButtonText, textColor);
    mainPalette.setColor(QPalette::ToolTipBase, baseColor);
    mainPalette.setColor(QPalette::ToolTipText, textColor);
    mainPalette.setColor(QPalette::Highlight, KswordTheme::PrimaryBlueColor);
    mainPalette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    QApplication::setPalette(mainPalette);
    setPalette(mainPalette);
    setAutoFillBackground(true);

    // 全局提示框样式：
    // - 通过 QToolTip 静态调色板强制应用深浅色提示框；
    // - 修复深色模式下 Tooltip 仍是白底的问题。
    QPalette toolTipPalette = mainPalette;
    toolTipPalette.setColor(QPalette::ToolTipBase, baseColor);
    toolTipPalette.setColor(QPalette::ToolTipText, textColor);
    QToolTip::setPalette(toolTipPalette);
    QToolTip::setFont(QApplication::font());

    // 同步写入 QApplication 样式表中的 QToolTip 规则：
    // - 覆盖所有顶层窗口（含浮动 Dock 与后续新建窗口）；
    // - 修复“部分按钮 Tooltip 仍白底”的残留问题。
    applyGlobalTooltipStyleBlock(buildGlobalTooltipStyleBlock(darkModeEnabled));
    // 同步写入 QApplication 样式表中的 QMenu 规则：
    // - 兜底标准输入控件（QLineEdit/QTextEdit/QPlainTextEdit）右键菜单；
    // - 覆盖独立顶层窗口中未手动设置菜单样式的场景。
    applyGlobalContextMenuStyleBlock(buildGlobalContextMenuStyleBlock(darkModeEnabled));

    // 全局 QMessageBox 主题刷新：
    // - 深浅色切换后，已打开的消息框也同步改为当前主题；
    // - 修复深色模式下消息框仍残留白底的问题。
    ks::ui::RefreshGlobalMessageBoxTheme();

    if (menuBar() != nullptr)
    {
        QPalette menuPalette = menuBar()->palette();
        menuPalette.setColor(QPalette::Window, windowBackgroundColor);
        menuPalette.setColor(QPalette::WindowText, textColor);
        menuPalette.setColor(QPalette::Text, textColor);
        menuBar()->setPalette(menuPalette);
        menuBar()->setAutoFillBackground(true);
    }

    if (m_pDockManager != nullptr)
    {
        QPalette dockPalette = m_pDockManager->palette();
        dockPalette.setColor(QPalette::Window, windowBackgroundColor);
        dockPalette.setColor(QPalette::WindowText, textColor);
        dockPalette.setColor(QPalette::Text, textColor);
        m_pDockManager->setPalette(dockPalette);
        m_pDockManager->setAutoFillBackground(true);
    }

    // appearanceStyleSheet 作用：
    // - 聚合基础样式与深浅色覆盖样式；
    // - 同时应用到 MainWindow 与 ADS DockManager，覆盖其默认白色样式。
    const QString appearanceStyleSheet =
        QSS_MainWindow_TabWidget
        + QSS_MainWindow_dockStyle
        + buildAppearanceOverlayStyleSheet(
            m_currentAppearanceSettings,
            darkModeEnabled,
            enableDockTransparencyForBackgroundImage);

    setStyleSheet(appearanceStyleSheet);
    if (m_pDockManager != nullptr)
    {
        // DockManager 清空局部样式表，改为继承 MainWindow 的统一样式；
        // 避免局部样式覆盖背景画刷导致背景图不可见。
        m_pDockManager->setStyleSheet(QString());
        m_pDockManager->setAttribute(Qt::WA_StyledBackground, false);
    }

    // 主题切换后主动刷新“按主题着色的表格行”，避免墨绿色残留到浅色模式。
    if (m_processWidget != nullptr)
    {
        m_processWidget->refreshThemeVisuals();
    }

    if (m_customTitleBar != nullptr)
    {
        m_customTitleBar->setDarkModeEnabled(darkModeEnabled);
        m_customTitleBar->setPinnedState(m_windowPinned);
        syncCustomTitleBarMaximizedState();
    }

    applyNativeWindowFrameVisualStyle();
    rebuildWindowBackgroundBrush();
    refreshPrivilegeStatusButtons();

    const QString effectiveModeText = darkModeEnabled ? QStringLiteral("dark") : QStringLiteral("light");
    info << appearanceApplyEvent
        << "[MainWindow] 已应用外观设置，触发来源="
        << triggerReason.toStdString()
        << "，theme_mode="
        << ks::settings::themeModeToJsonText(settings.themeMode).toStdString()
        << "，effective_mode="
        << effectiveModeText.toStdString()
        << "，dock_transparent="
        << (enableDockTransparencyForBackgroundImage ? "true" : "false")
        << "，background_path="
        << settings.backgroundImagePath.toStdString()
        << "，opacity="
        << settings.backgroundOpacityPercent
        << "%"
        << eol;
}

bool MainWindow::isDarkModeEffective(const ks::settings::AppearanceSettings& settings) const
{
    if (settings.themeMode == ks::settings::ThemeMode::Dark)
    {
        return true;
    }
    if (settings.themeMode == ks::settings::ThemeMode::Light)
    {
        return false;
    }

    QStyleHints* styleHints = QGuiApplication::styleHints();
    if (styleHints == nullptr)
    {
        return false;
    }
    return styleHints->colorScheme() == Qt::ColorScheme::Dark;
}

void MainWindow::rebuildWindowBackgroundBrush()
{
    const bool darkModeEnabled = isDarkModeEffective(m_currentAppearanceSettings);
    const QColor baseColor = darkModeEnabled ? QColor(0, 0, 0) : QColor(255, 255, 255);

    // resolvedImagePath 作用：把配置路径解析成可加载的绝对路径。
    const QString resolvedImagePath = ks::settings::resolveBackgroundImagePathForLoad(
        m_currentAppearanceSettings.backgroundImagePath);

    const QBrush backgroundBrush = buildBackgroundBrush(
        size(),
        baseColor,
        resolvedImagePath,
        m_currentAppearanceSettings.backgroundOpacityPercent);

    QPalette mainPalette = palette();
    mainPalette.setColor(QPalette::Window, baseColor);
    mainPalette.setBrush(QPalette::Window, backgroundBrush);
    setPalette(mainPalette);
    setAutoFillBackground(true);

    if (m_pDockManager != nullptr)
    {
        QPalette dockPalette = m_pDockManager->palette();
        dockPalette.setColor(QPalette::Window, baseColor);
        dockPalette.setBrush(QPalette::Window, backgroundBrush);
        m_pDockManager->setPalette(dockPalette);
        m_pDockManager->setAutoFillBackground(true);
    }
}

void MainWindow::scheduleWindowBackgroundBrushRebuild()
{
    if (m_backgroundRebuildTimer == nullptr)
    {
        rebuildWindowBackgroundBrush();
        return;
    }

    // 连续 resize 时只保留最后一次重建：
    // - 最大化拖下恢复为窗口化时，先让系统完成状态切换；
    // - 再重建背景，减少窗口化瞬间的卡顿感。
    m_backgroundRebuildTimer->start();
}

void MainWindow::applyFloatingDockContainerAppearance(ads::CFloatingDockContainer* floatingWidget) const
{
    if (floatingWidget == nullptr)
    {
        return;
    }

    const bool darkModeEnabled = isDarkModeEffective(m_currentAppearanceSettings);
    const QColor baseColor = darkModeEnabled ? QColor(0, 0, 0) : QColor(255, 255, 255);
    const QString resolvedImagePath = ks::settings::resolveBackgroundImagePathForLoad(
        m_currentAppearanceSettings.backgroundImagePath);
    const bool enableDockTransparencyForBackgroundImage =
        isBackgroundImageReady(m_currentAppearanceSettings.backgroundImagePath);

    // floatingSize 用途：浮动容器当前尺寸；无效时至少给 1x1，避免画刷构造失败。
    const QSize floatingSize = floatingWidget->size().isValid() ? floatingWidget->size() : QSize(1, 1);
    const QBrush backgroundBrush = buildBackgroundBrush(
        floatingSize,
        baseColor,
        resolvedImagePath,
        m_currentAppearanceSettings.backgroundOpacityPercent);

    QPalette floatingPalette = floatingWidget->palette();
    floatingPalette.setColor(QPalette::Window, baseColor);
    floatingPalette.setBrush(QPalette::Window, backgroundBrush);
    floatingPalette.setColor(QPalette::WindowText, darkModeEnabled ? QColor(255, 255, 255) : QColor(0, 0, 0));
    floatingWidget->setPalette(floatingPalette);
    floatingWidget->setAutoFillBackground(true);
    floatingWidget->setAttribute(Qt::WA_StyledBackground, false);

    // 浮动窗口是独立顶层窗口，不继承 MainWindow 的局部样式表；
    // 这里显式复用同一套外观样式，确保 Tab/TitleBar/内容区规则一致。
    const QString appearanceStyleSheet =
        QSS_MainWindow_TabWidget
        + QSS_MainWindow_dockStyle
        + buildAppearanceOverlayStyleSheet(
            m_currentAppearanceSettings,
            darkModeEnabled,
            enableDockTransparencyForBackgroundImage);
    floatingWidget->setStyleSheet(appearanceStyleSheet);

    ads::CDockContainerWidget* dockContainer = floatingWidget->dockContainer();
    if (dockContainer != nullptr)
    {
        QPalette dockContainerPalette = dockContainer->palette();
        dockContainerPalette.setColor(QPalette::Window, baseColor);
        dockContainerPalette.setBrush(QPalette::Window, backgroundBrush);
        dockContainer->setPalette(dockContainerPalette);
        dockContainer->setAutoFillBackground(true);
        dockContainer->setAttribute(Qt::WA_StyledBackground, false);
    }
}

QString MainWindow::buildAppearanceOverlayStyleSheet(
    const ks::settings::AppearanceSettings& settings,
    const bool darkModeEnabled,
    const bool enableDockTransparencyForBackgroundImage) const
{
    // tooltipStyle 作用：
    // - 强制全局提示框采用主题化背景和文字；
    // - 修复深色模式下 Tooltip 仍为白底的问题。
    const int scrollBarHoverExtentPx = settings.useWideScrollBars ? 12 : 7;
    const int scrollBarExtentPx = settings.scrollBarAutoHideEnabled ? 3 : scrollBarHoverExtentPx;
    const int scrollBarRadiusPx = 0;
    const QString windowBackgroundText = KswordTheme::WindowColorHex();
    const QString surfaceBackgroundText = KswordTheme::SurfaceColorHex();
    const QString surfaceAltBackgroundText = KswordTheme::SurfaceAltColorHex();
    const QString surfaceMutedBackgroundText = KswordTheme::SurfaceMutedColorHex();
    const QString borderColorText = KswordTheme::BorderColorHex();
    const QString borderStrongColorText = KswordTheme::BorderStrongColorHex();
    const QString primaryTextColor = KswordTheme::TextPrimaryColorHex();
    const QString disabledTextColor = KswordTheme::TextDisabledColorHex();
    const QString selectedTextColor = QStringLiteral("#FFFFFF");
    const QString activeThemeColor = KswordTheme::PrimaryBlueHex;
    const QString activeThemeHoverColor = KswordTheme::PrimaryBlueSolidHoverHex();
    const QString activeThemePressedColor = KswordTheme::PrimaryBluePressedHex;
    const QString subtleThemeColor = KswordTheme::PrimaryBlueSubtleHex();
    const QString scrollBarHandleColor = settings.scrollBarAutoHideEnabled
        ? QStringLiteral("rgba(67,160,255,0.42)")
        : QStringLiteral("rgba(67,160,255,0.78)");
    const QString scrollBarHandleHoverColor = QStringLiteral("rgba(67,160,255,0.92)");
    const QString panelBackgroundColor = darkModeEnabled
        ? QStringLiteral("rgba(17,25,36,0.94)")
        : QStringLiteral("rgba(255,255,255,0.95)");
    const QString panelBorderColor = borderColorText;
    const QString inactiveTabColor = surfaceAltBackgroundText;
    const QString inactiveTabTextColor = primaryTextColor;
    const QString activeTabColor = activeThemeColor;
    const QString activeTabTextColor = selectedTextColor;
    const QString tabHoverColor = darkModeEnabled
        ? QStringLiteral("rgba(67,160,255,0.16)")
        : subtleThemeColor;
    const QString comboBackgroundColor = surfaceBackgroundText;
    const QString comboTextColor = primaryTextColor;
    const QString comboBorderColor = borderColorText;
    const QString comboPopupBackgroundColor = surfaceBackgroundText;
    const QString comboPopupHoverColor = tabHoverColor;

    const QString tooltipStyle = QStringLiteral(
        "QToolTip{"
        "  background-color:%1 !important;"
        "  color:%2 !important;"
        "  border:1px solid %3 !important;"
        "  padding:4px 6px;"
        "  border-radius:3px;"
        "}")
        .arg(surfaceBackgroundText)
        .arg(primaryTextColor)
        .arg(borderStrongColorText);

    // dockBackgroundPolicyStyle 作用：
    // - 背景图可用时：Dock 相关容器全部透明，让底图完整透出；
    // - 背景图不可用时：保持 DockManager 使用 palette(window) 作为背景底色。
    const QString dockBackgroundPolicyStyle = enableDockTransparencyForBackgroundImage
        ? QStringLiteral(
            "QDockWidget,"
            "QDockWidget::title,"
            "QDockWidget > QWidget,"
            "ads--CDockManager,"
            "ads--CDockContainerWidget,"
            "ads--CDockAreaWidget,"
            "ads--CDockAreaTitleBar,"
            "ads--CFloatingDockContainer,"
            "ads--CDockAreaTabBar{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "  color:%1 !important;"
            "}")
        : QStringLiteral(
            "ads--CDockManager{"
            "  background-color:palette(window) !important;"
            "  color:%1 !important;"
            "}"
            "ads--CDockContainerWidget,"
            "ads--CDockAreaWidget,"
            "ads--CFloatingDockContainer{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "}"
            "ads--CDockAreaTitleBar{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "  color:%1 !important;"
            "}");

    // rootStyle 作用：
    // - 主窗口继续使用 palette(window)（含背景图画刷）作为底图来源；
    // - 再叠加 Dock 背景策略样式。
    const QString rootStyle = QStringLiteral(
        "QMainWindow{"
        "  background-color:palette(window) !important;"
        "  color:%1;"
        "}")
        .arg(primaryTextColor)
        + dockBackgroundPolicyStyle.arg(
            primaryTextColor);

    // depthOverlayStyle 作用：
    // - 为 Dock 面板、分组、表格和 Tab 增加边界/圆角/轻阴影感；
    // - 让当前 Tab 使用与图标错开的深浅对比色，避免蓝色图标在蓝底上不可见。
    const QString depthOverlayStyle = QStringLiteral(
        "ads--CDockAreaWidget{"
        "  border:1px solid %1 !important;"
        "  border-radius:8px;"
        "  background:%2 !important;"
        "}"
        "ads--CDockAreaTitleBar{"
        "  border-bottom:none !important;"
        "  padding:0px;"
        "}"
        "QGroupBox,QFrame#card,QWidget#card{"
        "  border:1px solid %1;"
        "  border-radius:8px;"
        "  background:%2;"
        "  margin-top:12px;"
        "}"
        "QGroupBox{"
        "  padding-top:6px;"
        "}"
        "QGroupBox::title{"
        "  subcontrol-origin:margin;"
        "  subcontrol-position:top left;"
        "  left:10px;"
        "  padding:0px 4px;"
        "  background:%2;"
        "  color:%3;"
        "}"
        "QTabWidget::pane{"
        "  border:none !important;"
        "  border-radius:0px;"
        "  background:%2 !important;"
        "  top:0px;"
        "}"
        "QHeaderView::section{"
        "  font-weight:600;"
        "  min-height:24px;"
        "}")
        .arg(panelBorderColor)
        .arg(panelBackgroundColor)
        .arg(primaryTextColor);

    // scrollBarOverlayStyle 作用：
    // - 全局改为透明轨道，减少遮挡；
    // - 根据设置切换窄/宽滚动条，并支持默认弱显示、悬停增强。
    const QString scrollBarOverlayStyle = QStringLiteral(
        "QScrollBar:vertical{"
        "  background:transparent !important;"
        "  border:none !important;"
        "  width:%1px !important;"
        "  margin:0px;"
        "}"
        "QScrollBar:horizontal{"
        "  background:transparent !important;"
        "  border:none !important;"
        "  height:%1px !important;"
        "  margin:0px;"
        "}"
        "QScrollBar:vertical:hover{"
        "  width:%5px !important;"
        "}"
        "QScrollBar:horizontal:hover{"
        "  height:%5px !important;"
        "}"
        "QScrollBar::handle:vertical{"
        "  background-color:%3 !important;"
        "  min-height:24px;"
        "  border-radius:%2px;"
        "}"
        "QScrollBar::handle:horizontal{"
        "  background-color:%3 !important;"
        "  min-width:24px;"
        "  border-radius:%2px;"
        "}"
        "QScrollBar::handle:vertical:hover,QScrollBar::handle:horizontal:hover{"
        "  background-color:%4 !important;"
        "}"
        "QScrollBar::add-line,QScrollBar::sub-line,QScrollBar::add-page,QScrollBar::sub-page{"
        "  background:transparent !important;"
        "  border:none !important;"
        "  width:0px;"
        "  height:0px;"
        "}")
        .arg(scrollBarExtentPx)
        .arg(scrollBarRadiusPx)
        .arg(scrollBarHandleColor)
        .arg(scrollBarHandleHoverColor)
        .arg(scrollBarHoverExtentPx);

    // sharedOverlayStyle 作用：
    // - 统一 hover/pressed 与 Tab 高亮；
    // - 当前 Tab 采用反差色，避免图标与选中底色混在一起。
    const QString buttonInteractionStyle = QStringLiteral(
        "QPushButton,QToolButton{"
        "  background-color:%4 !important;"
        "  color:%5 !important;"
        "  border:1px solid %6 !important;"
        "}"
        "QPushButton:hover,QToolButton:hover{"
        "  background-color:%1 !important;"
        "  color:%3 !important;"
        "  border-color:%1 !important;"
        "}"
        "QPushButton:pressed,QToolButton:pressed{"
        "  background-color:%2 !important;"
        "  color:%3 !important;"
        "  border-color:%2 !important;"
        "}"
        "QPushButton:disabled,QToolButton:disabled{"
        "  background-color:%7 !important;"
        "  color:%8 !important;"
        "  border-color:%6 !important;"
        "}")
        .arg(activeThemeHoverColor)
        .arg(activeThemePressedColor)
        .arg(selectedTextColor)
        .arg(darkModeEnabled ? surfaceAltBackgroundText : subtleThemeColor)
        .arg(darkModeEnabled ? primaryTextColor : QStringLiteral("#174A79"))
        .arg(darkModeEnabled ? borderStrongColorText : QStringLiteral("#9BC7F2"))
        .arg(darkModeEnabled ? surfaceMutedBackgroundText : QStringLiteral("#EDF4FC"))
        .arg(disabledTextColor);

    const QString comboBoxStyle = QStringLiteral(
        "QComboBox{"
        "  background-color:%1 !important;"
        "  color:%2 !important;"
        "  border:1px solid %3 !important;"
        "  border-radius:3px;"
        "  padding:2px 20px 2px 6px;"
        "  min-height:22px;"
        "  selection-background-color:%4 !important;"
        "  selection-color:%7 !important;"
        "}"
        "QComboBox:hover{"
        "  background-color:%6 !important;"
        "  color:%2 !important;"
        "  border-color:%4 !important;"
        "}"
        "QComboBox:focus,QComboBox:on{"
        "  background-color:%1 !important;"
        "  color:%2 !important;"
        "  border-color:%4 !important;"
        "}"
        "QComboBox:disabled{"
        "  background-color:%6 !important;"
        "  color:%8 !important;"
        "  border-color:%3 !important;"
        "}"
        "QComboBox::drop-down{"
        "  border:none !important;"
        "  background:transparent !important;"
        "  width:18px;"
        "}"
        "QComboBox::down-arrow{"
        "  image:none;"
        "  border-left:4px solid transparent;"
        "  border-right:4px solid transparent;"
        "  border-top:5px solid %2;"
        "  width:0px;"
        "  height:0px;"
        "}"
        "QComboBox QAbstractItemView{"
        "  background-color:%5 !important;"
        "  alternate-background-color:%5 !important;"
        "  color:%2 !important;"
        "  border:1px solid %3 !important;"
        "  selection-background-color:%4 !important;"
        "  selection-color:%7 !important;"
        "  outline:0;"
        "}"
        "QComboBox QAbstractItemView::item{"
        "  background-color:%5 !important;"
        "  color:%2 !important;"
        "}"
        "QComboBox QAbstractItemView::item:hover{"
        "  background-color:%6 !important;"
        "  color:%2 !important;"
        "}"
        "QComboBox QAbstractItemView::item:selected{"
        "  background-color:%4 !important;"
        "  color:%7 !important;"
        "}")
        .arg(comboBackgroundColor)
        .arg(comboTextColor)
        .arg(comboBorderColor)
        .arg(activeTabColor)
        .arg(comboPopupBackgroundColor)
        .arg(comboPopupHoverColor)
        .arg(selectedTextColor)
        .arg(disabledTextColor);

    const QString tabStyle = QStringLiteral(
        "QTabBar{"
        "  border:none !important;"
        "}"
        "QTabBar::tab{"
        "  background-color:%1 !important;"
        "  color:%2 !important;"
        "  border:none !important;"
        "  border-radius:0px !important;"
        "  padding:3px 12px;"
        "  min-height:22px;"
        "  margin:0px;"
        "  font-size:15px;"
        "}"
        "QTabBar::tab:left,QTabBar::tab:right{"
        "  padding:6px 10px;"
        "  font-size:15px;"
        "}"
        "QTabBar::tab:selected{"
        "  background-color:%4 !important;"
        "  color:%5 !important;"
        "  font-size:17px;"
        "  font-weight:700;"
        "}"
        "QTabBar::tab:left:selected,QTabBar::tab:right:selected{"
        "  font-size:18px;"
        "}"
        "QTabBar::tab:hover:!selected{"
        "  background-color:%6 !important;"
        "  color:%2 !important;"
        "}"
        "ads--CDockAreaTabBar{"
        "  background:transparent !important;"
        "  border:none !important;"
        "  padding:0px;"
        "}"
        "ads--CDockWidgetTab,ads--CAutoHideTab{"
        "  background-color:%1 !important;"
        "  color:%2 !important;"
        "  border:none !important;"
        "  border-radius:0px !important;"
        "  padding:3px 12px;"
        "  margin:0px;"
        "  min-height:22px;"
        "  font-size:15px;"
        "}"
        "ads--CDockWidgetTab QLabel,ads--CAutoHideTab QLabel{"
        "  color:%2 !important;"
        "  font-size:15px;"
        "}"
        "ads--CDockWidgetTab[activeTab=\"true\"],ads--CAutoHideTab[activeTab=\"true\"]{"
        "  background-color:%4 !important;"
        "  color:%5 !important;"
        "}"
        "ads--CDockWidgetTab[activeTab=\"true\"] QLabel,ads--CAutoHideTab[activeTab=\"true\"] QLabel{"
        "  color:%5 !important;"
        "  font-size:17px;"
        "  font-weight:700;"
        "}"
        "ads--CDockWidgetTab:hover,ads--CAutoHideTab:hover{"
        "  background-color:%6 !important;"
        "  color:%2 !important;"
        "}"
        "ads--CDockAreaTitleBar QToolButton,ads--CDockAreaTitleBar QPushButton{"
        "  border:none !important;"
        "  background:transparent !important;"
        "}")
        .arg(inactiveTabColor)
        .arg(inactiveTabTextColor)
        .arg(panelBorderColor)
        .arg(activeTabColor)
        .arg(activeTabTextColor)
        .arg(tabHoverColor);

    const QString sharedOverlayStyle = depthOverlayStyle
        + scrollBarOverlayStyle
        + buttonInteractionStyle
        + comboBoxStyle
        + tabStyle;    // dockContentTransparentStyle 作用：
    // - 背景图可用时，把 Dock 内容区域常见容器背景全部改为透明；
    // - 修复“Dock 面板整体仍是黑底/白底，背景图只能从缝隙看到”的问题。
    // 注意：该片段只作用于 ads--CDockWidget 后代，不影响菜单栏等全局区域。
    const QString dockContentTransparentStyle = enableDockTransparencyForBackgroundImage
        ? QStringLiteral(
            "ads--CDockWidget,"
            "ads--CDockWidget > QWidget,"
            "ads--CDockWidget QFrame,"
            "ads--CDockWidget QTabWidget::pane,"
            "ads--CDockWidget QStackedWidget,"
            "ads--CDockWidget QStackedWidget > QWidget,"
            "ads--CDockWidget QSplitter,"
            "ads--CDockWidget QSplitter::handle,"
            "ads--CDockWidget QScrollArea,"
            "ads--CDockWidget QAbstractScrollArea,"
            "ads--CDockWidget QAbstractScrollArea::viewport,"
            "ads--CDockWidget QTableView,"
            "ads--CDockWidget QTableWidget,"
            "ads--CDockWidget QTreeView,"
            "ads--CDockWidget QTreeWidget,"
            "ads--CDockWidget QListView,"
            "ads--CDockWidget QListWidget,"
            "ads--CDockWidget QTextEdit,"
            "ads--CDockWidget QPlainTextEdit,"
            "ads--CDockWidget QGroupBox{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "}"
            "ads--CDockWidget QTableView,"
            "ads--CDockWidget QTableWidget,"
            "ads--CDockWidget QTreeView,"
            "ads--CDockWidget QTreeWidget,"
            "ads--CDockWidget QListView,"
            "ads--CDockWidget QListWidget{"
            "  alternate-background-color:transparent !important;"
            "}")
        : QString();

    if (!darkModeEnabled)
    {
        return rootStyle
            + QStringLiteral(
                "QMenuBar{background-color:%1;color:%3;}"
                "QMenuBar::item{background:transparent;color:%3;padding:2px 7px;}"
                "QMenuBar::item:selected{background:%2;color:%3;}"
                "QMenuBar::item:pressed{background:__LIGHT_MENUBAR_PRESSED__;color:%3;}"
                "QStatusBar{background-color:%1;color:%3;}"
                "QLineEdit,QTextEdit,QPlainTextEdit,QTableWidget,QTreeWidget,QListWidget,QSpinBox,QDoubleSpinBox{"
                "  background-color:%1 !important;"
                "  color:%3 !important;"
                "  border:1px solid %4;"
                "}"
                "QPushButton,QToolButton{"
                "  background-color:%2 !important;"
                "  color:#174A79 !important;"
                "  border:1px solid %5 !important;"
                "}"
                "QTableView,QTableWidget,QTreeView,QTreeWidget,QListView,QListWidget{"
                "  background:%1 !important;"
                "  alternate-background-color:%6 !important;"
                "  color:%3 !important;"
                "  gridline-color:%4;"
                "}"
                "QTableView::item:selected,QTableWidget::item:selected,QTreeView::item:selected,QTreeWidget::item:selected{"
                "  background:%7 !important;"
                "  color:%8 !important;"
                "}"
                "QHeaderView::section{"
                "  background:%6 !important;"
                "  color:%3 !important;"
                "  border:1px solid %4;"
                "}"
                "QTableCornerButton::section{"
                "  background:%6 !important;"
                "  border:none !important;"
                "}"
                "QScrollBar:vertical,QScrollBar:horizontal{"
                "  background:%9 !important;"
                "  border:none !important;"
                "}")
                .arg(surfaceBackgroundText)
                .arg(subtleThemeColor)
                .arg(primaryTextColor)
                .arg(borderColorText)
                .arg(borderStrongColorText)
                .arg(surfaceAltBackgroundText)
                .arg(activeThemeColor)
                .arg(selectedTextColor)
                .arg(windowBackgroundText)
                .replace(QStringLiteral("__LIGHT_MENUBAR_PRESSED__"), surfaceMutedBackgroundText)
            + sharedOverlayStyle
            + tooltipStyle
            + dockContentTransparentStyle;
    }

    return rootStyle
        + QStringLiteral(
            "QMenuBar{background-color:%1;color:%3;}"
            "QMenuBar::item{background:transparent;color:%3;padding:2px 7px;}"
            "QMenuBar::item:selected{background:rgba(67,160,255,0.28);color:%3;}"
            "QMenuBar::item:pressed{background:rgba(67,160,255,0.38);color:%8;}"
            "QStatusBar{background-color:%1;color:%3;}"
            "QLineEdit,QTextEdit,QPlainTextEdit,QTableWidget,QTreeWidget,QListWidget,QSpinBox,QDoubleSpinBox{"
            "  background-color:%2 !important;"
            "  color:%3 !important;"
            "  border:1px solid %4;"
            "}"
            "QPushButton,QToolButton{"
            "  background-color:%6 !important;"
            "  color:%3 !important;"
            "  border:1px solid %5 !important;"
            "}"
            "QTableView,QTableWidget,QTreeView,QTreeWidget,QListView,QListWidget{"
            "  background:%2 !important;"
            "  alternate-background-color:%6 !important;"
            "  color:%3 !important;"
            "  gridline-color:%4;"
            "}"
            "QTableView::item:selected,QTableWidget::item:selected,QTreeView::item:selected,QTreeWidget::item:selected{"
            "  background:%7 !important;"
            "  color:%8 !important;"
            "}"
            "QHeaderView::section{"
            "  background:%6 !important;"
            "  color:%3 !important;"
            "  border:1px solid %4;"
            "}"
            "QTableCornerButton::section{"
            "  background:%6 !important;"
            "  border:none !important;"
            "}"
            "QScrollBar:vertical,QScrollBar:horizontal{"
            "  background:%1 !important;"
            "  border:none !important;"
            "}")
            .arg(windowBackgroundText)
            .arg(surfaceBackgroundText)
            .arg(primaryTextColor)
            .arg(borderColorText)
            .arg(borderStrongColorText)
            .arg(surfaceAltBackgroundText)
            .arg(activeThemeColor)
            .arg(selectedTextColor)
        + sharedOverlayStyle
        + tooltipStyle
        + dockContentTransparentStyle;
}
