#include "Taskbar.h"
#include "Function.h"
#include "Override.h"
#include "TaskbarSettingsDialog.h"
#include "PageOverviewWindow.h"
#include <QPointer>

#include <windows.h>
#include <shellapi.h>
#include <cmath>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>
#include <QPixmap>
#include <QStyle>
#include <qpushbutton.h>
#include <qtimer.h>
#include <QDateTime>
#include <QCoreApplication>
#include <QAbstractAnimation>
#include <QGraphicsOpacityEffect>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QProcessEnvironment>
#include <QResizeEvent>
#include <Qscreen.h>
#include <QVariantAnimation>

#pragma comment(lib, "shell32.lib")

namespace {
constexpr int kTaskbarLogicalHeight = 32;
constexpr int kTaskbarOuterMargin = 2;
constexpr int kTaskbarContentHeight = kTaskbarLogicalHeight - (kTaskbarOuterMargin * 2);
constexpr int kMinimumSpectrumWidth = 80;
constexpr int kLargeScreenSpectrumMinimumWidth = 220;
constexpr int kMaximumSpectrumWidth = 320;
constexpr int kCompactScreenNonSpectrumBudget = 648;

struct MonitorSearchContext {
    QString targetName;
    QRect nativeGeometry;
    bool found;
};

// resolveCurrentUserNameText 作用：
// - 输入：无，读取当前进程环境变量和 Win32 用户名 API；
// - 处理：优先使用 USERNAME，失败时调用 GetUserNameW，最后给出稳定兜底；
// - 返回：用于任务栏左侧身份文本的当前登录用户名。
QString resolveCurrentUserNameText()
{
    const QString envUserNameText =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("USERNAME")).trimmed();
    if (!envUserNameText.isEmpty()) {
        return envUserNameText;
    }

    wchar_t userNameBuffer[256] = {};
    DWORD bufferLength = static_cast<DWORD>(std::size(userNameBuffer));
    if (::GetUserNameW(userNameBuffer, &bufferLength) != FALSE) {
        const QString apiUserNameText = QString::fromWCharArray(userNameBuffer).trimmed();
        if (!apiUserNameText.isEmpty()) {
            return apiUserNameText;
        }
    }

    return QStringLiteral("UnknownUser");
}

// Win32 monitor enumeration callback: input is an HMONITOR and caller context;
// processing compares MONITORINFOEX device names; return value controls enumeration continuation.
BOOL CALLBACK findMonitorByDisplayName(HMONITOR monitor, HDC, LPRECT, LPARAM userData)
{
    MonitorSearchContext* context = reinterpret_cast<MonitorSearchContext*>(userData);
    if (!context || context->targetName.isEmpty()) {
        return TRUE;
    }

    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return TRUE;
    }

    const QString monitorName = QString::fromWCharArray(monitorInfo.szDevice).trimmed().toUpper();
    if (monitorName != context->targetName) {
        return TRUE;
    }

    const RECT& nativeRect = monitorInfo.rcMonitor;
    context->nativeGeometry = QRect(
        nativeRect.left,
        nativeRect.top,
        nativeRect.right - nativeRect.left,
        nativeRect.bottom - nativeRect.top
    );
    context->found = true;
    return FALSE;
}

// Validate a device pixel ratio: input is a Qt DPR value; processing rejects invalid values;
// return value is a positive scale factor suitable for pixel conversion.
qreal safeDevicePixelRatio(qreal devicePixelRatio)
{
    if (!std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
        return 1.0;
    }
    return devicePixelRatio;
}

// Convert a logical length to native pixels: input is a Qt DIP length and DPR; processing rounds
// to the nearest physical pixel; return value is at least 1 pixel for non-empty AppBar reservation.
int logicalLengthToNativePixels(int logicalLength, qreal devicePixelRatio)
{
    return qMax(1, qRound(static_cast<qreal>(logicalLength) * safeDevicePixelRatio(devicePixelRatio)));
}

// Convert a Qt logical rectangle to native pixels: input is a DIP rectangle and DPR; processing
// scales origin and size; return value uses Win32-style physical pixel units.
QRect logicalRectToNativePixels(const QRect& logicalRect, qreal devicePixelRatio)
{
    const qreal dpr = safeDevicePixelRatio(devicePixelRatio);
    return QRect(
        qRound(static_cast<qreal>(logicalRect.x()) * dpr),
        qRound(static_cast<qreal>(logicalRect.y()) * dpr),
        qRound(static_cast<qreal>(logicalRect.width()) * dpr),
        qRound(static_cast<qreal>(logicalRect.height()) * dpr)
    );
}

// Normalize a Qt screen name to the Win32 MONITORINFOEX device form: input is either
// "DISPLAY1" or "\\.\DISPLAY1"; processing adds the missing prefix; return value is uppercase.
QString normalizeDisplayDeviceNameForWin32(const QString& displayName)
{
    QString normalizedName = displayName.trimmed().replace('/', '\\').toUpper();
    const QString win32Prefix = QStringLiteral("\\\\.\\");
    if (!normalizedName.isEmpty() && !normalizedName.startsWith(win32Prefix)) {
        normalizedName.prepend(win32Prefix);
    }
    return normalizedName;
}

// Resolve the monitor under a native point: input is a physical-pixel point; processing queries
// MonitorFromPoint and GetMonitorInfo; return value is the monitor rectangle or an invalid rect.
QRect nativeMonitorGeometryFromPoint(const QPoint& nativePoint)
{
    POINT point = { nativePoint.x(), nativePoint.y() };
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        return QRect();
    }

    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return QRect();
    }

    const RECT& nativeRect = monitorInfo.rcMonitor;
    return QRect(
        nativeRect.left,
        nativeRect.top,
        nativeRect.right - nativeRect.left,
        nativeRect.bottom - nativeRect.top
    );
}

// Resolve the monitor containing a window: input is an HWND; processing asks Win32 for the
// nearest monitor and reads MONITORINFOEX; return value is the monitor rectangle or invalid.
QRect nativeMonitorGeometryFromWindow(HWND windowHandle)
{
    if (!windowHandle) {
        return QRect();
    }

    HMONITOR monitor = MonitorFromWindow(windowHandle, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        return QRect();
    }

    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return QRect();
    }

    const RECT& nativeRect = monitorInfo.rcMonitor;
    return QRect(
        nativeRect.left,
        nativeRect.top,
        nativeRect.right - nativeRect.left,
        nativeRect.bottom - nativeRect.top
    );
}

// Map an AppBar rectangle from native monitor coordinates into Qt logical coordinates: input is
// the native AppBar rect plus native/logical monitor rects; processing converts only per-monitor
// offsets, not global desktop origin; return value is safe for mixed-DPI multi-monitor layouts.
QRect mapNativeAppBarRectToLogicalScreen(const QRect& nativeAppBarRect,
                                         const QRect& nativeScreenRect,
                                         const QRect& logicalScreenRect,
                                         qreal devicePixelRatio)
{
    const qreal dpr = safeDevicePixelRatio(devicePixelRatio);
    const int logicalLeft = logicalScreenRect.left()
        + qRound(static_cast<qreal>(nativeAppBarRect.left() - nativeScreenRect.left()) / dpr);
    const int logicalTop = logicalScreenRect.top()
        + qRound(static_cast<qreal>(nativeAppBarRect.top() - nativeScreenRect.top()) / dpr);
    const int logicalWidth = qRound(static_cast<qreal>(nativeAppBarRect.width()) / dpr);
    const int logicalHeight = qRound(static_cast<qreal>(nativeAppBarRect.height()) / dpr);

    return QRect(logicalLeft, logicalTop, logicalWidth, logicalHeight);
}

// 判断系统窗口管理命令：Taskbar 是固定的顶部 AppBar，不接受最小化、
// 最大化、还原、移动或调整大小等外部窗口状态变更。
bool isIgnoredTaskbarSystemCommand(WPARAM command)
{
    switch (static_cast<UINT_PTR>(command) & 0xFFF0U) {
    case SC_MINIMIZE:
    case SC_MAXIMIZE:
    case SC_RESTORE:
    case SC_MOVE:
    case SC_SIZE:
        return true;
    default:
        return false;
    }
}
}

bool Taskbar::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);

    MSG* msg = static_cast<MSG*>(message);
    if (msg == nullptr) {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    // Win+D 会通过 SC_MINIMIZE 让普通顶层窗口最小化。Taskbar 作为 AppBar
    // 必须保持可见，因此忽略该命令及其它会改变窗口状态的系统命令。
    if (msg->message == WM_SYSCOMMAND && isIgnoredTaskbarSystemCommand(msg->wParam)) {
        if (result != nullptr) {
            *result = 0;
        }
        return true;
    }

    // 与上面的系统命令配套处理，避免其它窗口管理路径将 AppBar 设为最小化。
    if (msg->message == WM_SIZE && msg->wParam == SIZE_MINIMIZED) {
        if (result != nullptr) {
            *result = 0;
        }
        return true;
    }

    if (msg->message == appBarMessageId) {
        // 处理应用栏通知。
        if (msg->wParam == ABN_POSCHANGED) {
            // 位置变化时重新调整。
            RegisterAsAppBar();
        }
        if (result != nullptr) {
            *result = 0;
        }
        return true;
    }

    return QMainWindow::nativeEvent(eventType, message, result);
}

Taskbar::Taskbar(QScreen* targetScreen, TaskbarSharedState* sharedState,
                 TaskbarNotificationService* notificationService, QWidget* parent)
    : QMainWindow(parent)
    , m_leftSpectrum(nullptr)
    , m_rightSpectrum(nullptr)
    , m_sharedState(sharedState)
    , m_notificationService(notificationService)
    , m_targetScreenGeometry(targetScreen ? targetScreen->geometry() : QRect())
    , m_targetScreenName(targetScreen ? targetScreen->name() : QString())
    , m_targetDevicePixelRatio(targetScreen ? targetScreen->devicePixelRatio() : 1.0)
    , cpuBarContainer(nullptr)
    , timer(nullptr)
    , timeLabel(nullptr)
    , contentLabel(nullptr)
    , logoLabel(nullptr)
    , networkSpeedContainer(nullptr)
    , uploadSpeedLabel(nullptr)
    , downloadSpeedLabel(nullptr)
    , networkUiTimer(nullptr)
    , isAppBarRegistered(false)
    , centralWidget(nullptr)
    , normalCenterWidget(nullptr)
    , notificationCenterWidget(nullptr)
    , centerStackLayout(nullptr)
    , normalCenterOpacity(nullptr)
    , notificationCenterOpacity(nullptr)
    , notificationSourceLabel(nullptr)
    , notificationTitleLabel(nullptr)
    , notificationBodyLabel(nullptr)
    , notificationVisible(false)
    , earthquakePresentation(false)
    , alertFlashAnimation(nullptr)
    , alertFlashBright(false)
    , notificationFlashWidget(nullptr)
    , notificationFlashOpacity(nullptr)
    , notificationFlashAnimation(nullptr)
    , rightBtnContainer(nullptr)
    , rightBtnLayout(nullptr)
    , exitBtn(nullptr)
    , lockBtn(nullptr)
    , toolBtn(nullptr)
    , settingsBtn(nullptr)
    , userBtn(nullptr)
    , settingsDialog(nullptr)
    , appBarMessageId(0)
    , cpuUpdateTimer(nullptr)
{
    // Taskbar 为每个 QScreen 建立独立 AppBar 窗口，但中央通知内容和警报状态由全局服务共享。
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::ToolTip);
    setFixedHeight(kTaskbarLogicalHeight);
    setAttribute(Qt::WA_TranslucentBackground, false);

    centralWidget = new QWidget(this);
    QVBoxLayout* vLayout = new QVBoxLayout(centralWidget);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);
    vLayout->setSizeConstraint(QLayout::SetNoConstraint);
    centralWidget->setLayout(vLayout);

    QHBoxLayout* hLayout = new QHBoxLayout();
    hLayout->setContentsMargins(2, 2, 2, 2);
    hLayout->setSpacing(5);

    // 左侧 Logo 在警报态由 applyTaskbarTheme 仅对非透明像素着色。
    logoLabel = new QLabel(centralWidget);
    logoPixmap = QPixmap(":/Image/Resource/Image/MainLogo.png");
    if (!logoPixmap.isNull()) {
        logoLabel->setFixedHeight(kTaskbarContentHeight);
        logoLabel->setMinimumWidth(1);
        logoLabel->setPixmap(logoPixmap.scaled(QSize(QWIDGETSIZE_MAX, logoLabel->height()),
            Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logoLabel->setAlignment(Qt::AlignCenter);
    }
    hLayout->addWidget(logoLabel);

    contentLabel = new QLabel(centralWidget);
    contentLabel->setText(QStringLiteral("· %1 [Dev].").arg(resolveCurrentUserNameText()));
    contentLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hLayout->addWidget(contentLabel);
    hLayout->addStretch(1);

    // 常态页继续使用原始双频谱和时钟；通知页与其完全叠放，不会改变任务栏宽度或 AppBar 几何。
    m_leftSpectrum = new SpectrumWidget(SpectrumWidget::CenterToLeft, this);
    m_rightSpectrum = new SpectrumWidget(SpectrumWidget::CenterToRight, this);
    m_leftSpectrum->setFixedHeight(kTaskbarContentHeight);
    m_rightSpectrum->setFixedHeight(kTaskbarContentHeight);
    m_leftSpectrum->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_rightSpectrum->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    timeLabel = new QLabel(centralWidget);
    timeLabel->setAlignment(Qt::AlignCenter);
    timeLabel->setMinimumWidth(52);

    QWidget* centerHost = new QWidget(centralWidget);
    centerHost->setMinimumWidth(spectrumMinimumWidthForScreen() * 2 + timeLabel->minimumWidth());
    centerHost->setMaximumWidth(spectrumMaximumWidthForScreen() * 2 + timeLabel->minimumWidth());
    centerHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    centerStackLayout = new QStackedLayout(centerHost);
    centerStackLayout->setContentsMargins(0, 0, 0, 0);
    centerStackLayout->setStackingMode(QStackedLayout::StackAll);

    normalCenterWidget = new QWidget(centerHost);
    QHBoxLayout* spectrumTimeLayout = new QHBoxLayout(normalCenterWidget);
    spectrumTimeLayout->setContentsMargins(0, 0, 0, 0);
    spectrumTimeLayout->setSpacing(0);
    spectrumTimeLayout->addWidget(m_leftSpectrum);
    spectrumTimeLayout->addWidget(timeLabel);
    spectrumTimeLayout->addWidget(m_rightSpectrum);

    notificationCenterWidget = new QWidget(centerHost);
    QHBoxLayout* notificationLayout = new QHBoxLayout(notificationCenterWidget);
    notificationLayout->setContentsMargins(4, 0, 4, 0);
    notificationLayout->setSpacing(5);
    notificationSourceLabel = new QLabel(notificationCenterWidget);
    notificationSourceLabel->setFixedWidth(52);
    notificationSourceLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    notificationTitleLabel = new QLabel(notificationCenterWidget);
    notificationTitleLabel->setMinimumWidth(84);
    notificationTitleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    notificationBodyLabel = new QLabel(notificationCenterWidget);
    notificationBodyLabel->setMinimumWidth(0);
    notificationBodyLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    notificationBodyLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    notificationBodyLabel->setTextFormat(Qt::PlainText);
    notificationLayout->addWidget(notificationSourceLabel);
    notificationLayout->addWidget(notificationTitleLabel);
    notificationLayout->addWidget(notificationBodyLabel, 1);

    normalCenterOpacity = new QGraphicsOpacityEffect(normalCenterWidget);
    normalCenterOpacity->setOpacity(1.0);
    normalCenterWidget->setGraphicsEffect(normalCenterOpacity);
    notificationCenterOpacity = new QGraphicsOpacityEffect(notificationCenterWidget);
    notificationCenterOpacity->setOpacity(0.0);
    notificationCenterWidget->setGraphicsEffect(notificationCenterOpacity);
    centerStackLayout->addWidget(normalCenterWidget);
    centerStackLayout->addWidget(notificationCenterWidget);
    hLayout->addWidget(centerHost);
    hLayout->addStretch(1);

    // CPU 与网络采样仍直接读取已有的共享状态对象，不因通知模块额外创建采样线程。
    cpuBarContainer = new QWidget(centralWidget);
    QHBoxLayout* cpuBarLayout = new QHBoxLayout(cpuBarContainer);
    cpuBarLayout->setContentsMargins(3, 3, 3, 3);
    cpuBarLayout->setSpacing(2);
    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    const int coreCount = static_cast<int>(sysInfo.dwNumberOfProcessors);
    cpuBars.resize(coreCount);
    for (int index = 0; index < coreCount; ++index) {
        QLabel* bar = new QLabel(cpuBarContainer);
        bar->setAlignment(Qt::AlignBottom);
        cpuBars[index] = bar;
        cpuBarLayout->addWidget(bar, 0, Qt::AlignBottom);
    }
    hLayout->addWidget(cpuBarContainer);

    networkSpeedContainer = new QWidget(centralWidget);
    networkSpeedContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    networkSpeedContainer->setMinimumWidth(74);
    QVBoxLayout* networkSpeedLayout = new QVBoxLayout(networkSpeedContainer);
    networkSpeedLayout->setContentsMargins(2, 2, 2, 2);
    networkSpeedLayout->setSpacing(0);
    uploadSpeedLabel = new QLabel(networkSpeedContainer);
    downloadSpeedLabel = new QLabel(networkSpeedContainer);
    uploadSpeedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    downloadSpeedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    networkSpeedLayout->addWidget(uploadSpeedLabel);
    networkSpeedLayout->addWidget(downloadSpeedLabel);
    hLayout->addWidget(networkSpeedContainer);

    // 右上角图标按钮均提供悬停说明；设置图标使用 Taskbar 已有 SVG 资源库而非 WindowsMarker 图标。
    const QSize iconSize(20, 20);
    rightBtnContainer = new QWidget(centralWidget);
    rightBtnContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    rightBtnLayout = new QHBoxLayout(rightBtnContainer);
    rightBtnLayout->setContentsMargins(0, 0, 0, 0);
    rightBtnLayout->setSpacing(1);
    lockBtn = new GlowIconButton(":/Icon/Resource/Icon/lock_line.svg", iconSize, rightBtnContainer);
    lockBtn->setToolTip(QStringLiteral("锁定工作站"));
    toolBtn = new GlowIconButton(":/Icon/Resource/Icon/tool_line.svg", iconSize, rightBtnContainer);
    toolBtn->setToolTip(QStringLiteral("打开命令提示符"));
    settingsBtn = new GlowIconButton(":/Icon/Resource/svg/system/settings_6_line.svg", iconSize, rightBtnContainer);
    settingsBtn->setToolTip(QStringLiteral("通知设置"));
    overviewBtn = new GlowIconButton(":/Icon/Resource/svg/system/grid_2_line.svg", iconSize, rightBtnContainer);
    overviewBtn->setToolTip(QStringLiteral("KSword 页面平铺：透明分列、搜索并直达各级页面"));
    userBtn = new GlowIconButton(":/Icon/Resource/Icon/user_2_line.svg", iconSize, rightBtnContainer);
    userBtn->setToolTip(QStringLiteral("用户自定义功能"));
    exitBtn = new GlowIconButton(":/Icon/Resource/Icon/exit_fill.svg", iconSize, rightBtnContainer);
    exitBtn->setToolTip(QStringLiteral("退出任务栏"));
    rightBtnLayout->addWidget(lockBtn);
    rightBtnLayout->addWidget(toolBtn);
    rightBtnLayout->addWidget(settingsBtn);
    rightBtnLayout->addWidget(overviewBtn);
    rightBtnLayout->addWidget(userBtn);
    rightBtnLayout->addWidget(exitBtn);
    rightBtnLayout->setAlignment(Qt::AlignRight);
    connect(lockBtn, &QPushButton::clicked, lockWorkstation);
    connect(toolBtn, &QPushButton::clicked, openCmd);
    connect(settingsBtn, &QPushButton::clicked, this, &Taskbar::showSettingsDialog);
    connect(overviewBtn, &QPushButton::clicked, this, [this]()
    {
        // 多显示器 Taskbar 共用一个导航窗，避免同时读取目录时互相覆盖路由代次。
        static QPointer<PageOverviewWindow> overview;
        if (!overview)
        {
            overview = new PageOverviewWindow(this);
        }
        if (overview->isVisible())
        {
            overview->hide();
        }
        else
        {
            overview->openOnScreen(screen());
        }
    });
    connect(userBtn, &QPushButton::clicked, userCustomFunction);
    connect(exitBtn, &QPushButton::clicked, this, &Taskbar::onExitClicked);
    hLayout->addWidget(rightBtnContainer);

    vLayout->addLayout(hLayout);
    setCentralWidget(centralWidget);

    // 新消息提亮层覆盖完整 Taskbar，但设置为鼠标穿透，避免遮挡右侧按钮交互。
    notificationFlashWidget = new QWidget(centralWidget);
    notificationFlashWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    notificationFlashWidget->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 32);"));
    notificationFlashWidget->setGeometry(centralWidget->rect());
    notificationFlashWidget->raise();
    notificationFlashOpacity = new QGraphicsOpacityEffect(notificationFlashWidget);
    notificationFlashOpacity->setOpacity(0.0);
    notificationFlashWidget->setGraphicsEffect(notificationFlashOpacity);
    notificationFlashAnimation = new QPropertyAnimation(notificationFlashOpacity, "opacity", this);
    notificationFlashAnimation->setDuration(500);
    notificationFlashAnimation->setEasingCurve(QEasingCurve::Linear);

    // 地震警报使用颜色值动画，亮红到暗红渐变，暗红回亮红只在动画周期边界瞬时切换。
    alertFlashAnimation = new QVariantAnimation(this);
    alertFlashAnimation->setDuration(500);
    alertFlashAnimation->setEasingCurve(QEasingCurve::Linear);
    connect(alertFlashAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (earthquakePresentation)
        {
            applyTaskbarTheme(true, value.value<QColor>());
        }
    });
    connect(alertFlashAnimation, &QVariantAnimation::finished, this, [this]() {
        if (!earthquakePresentation)
        {
            return;
        }
        alertFlashBright = false;
        applyTaskbarTheme(true, QColor(QStringLiteral("#480000")));
        alertFlashBright = true;
        applyTaskbarTheme(true, QColor(QStringLiteral("#D90000")));
        startAlertFlashCycle();
    });
    applyTaskbarTheme(false, QColor(QStringLiteral("#0A0F16")));

    // 保持现有的频谱、时间、CPU 和网络刷新节奏；通知服务只增加自己的轻量 UI 状态信号。
    if (m_sharedState) {
        connect(m_sharedState, &TaskbarSharedState::spectrumDataReady, this,
            &Taskbar::onSpectrumDataReady);
    }
    timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &Taskbar::updateTime);
    timer->start();
    updateTime();
    cpuUpdateTimer = new QTimer(this);
    cpuUpdateTimer->setInterval(200);
    connect(cpuUpdateTimer, &QTimer::timeout, this, &Taskbar::updateCPUUsage);
    cpuUpdateTimer->start();
    updateCPUUsage();
    networkUiTimer = new QTimer(this);
    networkUiTimer->setInterval(250);
    connect(networkUiTimer, &QTimer::timeout, this, &Taskbar::updateNetworkSpeedLabels);
    networkUiTimer->start();
    updateNetworkSpeedLabels();

    if (m_notificationService != nullptr) {
        connect(m_notificationService, &TaskbarNotificationService::presentationChanged, this,
            &Taskbar::onNotificationPresentationChanged);
        updateNotificationPresentation();
    }
    RegisterAsAppBar();
}

int Taskbar::appBarThicknessInNativePixels() const
{
    // Input: none. Processing: convert the visible Qt taskbar height from device-independent
    // pixels to Win32 native pixels. Return: AppBar reservation height in physical pixels.
    return logicalLengthToNativePixels(height(), m_targetDevicePixelRatio);
}

QRect Taskbar::targetScreenNativeGeometry() const
{
    // Input: none. Processing: prefer MONITORINFOEX because SHAppBarMessage consumes physical
    // monitor coordinates; fall back to Qt geometry scaled by DPR. Return: native monitor rect.
    const QString normalizedTargetName = normalizeDisplayDeviceNameForWin32(m_targetScreenName);
    if (!normalizedTargetName.isEmpty()) {
        MonitorSearchContext context = { normalizedTargetName, QRect(), false };
        EnumDisplayMonitors(nullptr, nullptr, findMonitorByDisplayName, reinterpret_cast<LPARAM>(&context));
        if (context.found && context.nativeGeometry.isValid()) {
            return context.nativeGeometry;
        }
    }

    if (m_targetScreenGeometry.isValid()) {
        const QRect windowMonitorGeometry = nativeMonitorGeometryFromWindow(reinterpret_cast<HWND>(winId()));
        if (windowMonitorGeometry.isValid()) {
            return windowMonitorGeometry;
        }

        const QPoint logicalCenter = m_targetScreenGeometry.center();
        const QPoint nativeCenter(
            qRound(static_cast<qreal>(logicalCenter.x()) * safeDevicePixelRatio(m_targetDevicePixelRatio)),
            qRound(static_cast<qreal>(logicalCenter.y()) * safeDevicePixelRatio(m_targetDevicePixelRatio))
        );
        const QRect nativeMonitorGeometry = nativeMonitorGeometryFromPoint(nativeCenter);
        if (nativeMonitorGeometry.isValid()) {
            return nativeMonitorGeometry;
        }

        return logicalRectToNativePixels(m_targetScreenGeometry, m_targetDevicePixelRatio);
    }

    if (screen()) {
        return logicalRectToNativePixels(screen()->geometry(), screen()->devicePixelRatio());
    }

    return QRect(0, 0, 1920, appBarThicknessInNativePixels());
}

QRect Taskbar::targetScreenLogicalGeometry() const
{
    // Input: none. Processing: use the constructor-bound QScreen geometry for stable multi-monitor
    // placement, with the live QWidget screen as a fallback. Return: Qt logical screen rectangle.
    if (m_targetScreenGeometry.isValid()) {
        return m_targetScreenGeometry;
    }

    if (screen()) {
        return screen()->geometry();
    }

    return QRect(0, 0, 1920, kTaskbarLogicalHeight);
}

int Taskbar::spectrumMinimumWidthForScreen() const
{
    // Input: none. Processing: reserve room for fixed logo/text/CPU/network/buttons first, then
    // allow both spectrum widgets to shrink on compact screens. Return: minimum spectrum width.
    const int logicalScreenWidth = m_targetScreenGeometry.isValid()
        ? m_targetScreenGeometry.width()
        : 1920;
    const int availableForEachSpectrum =
        (logicalScreenWidth - kCompactScreenNonSpectrumBudget) / 2;

    return qBound(
        kMinimumSpectrumWidth,
        availableForEachSpectrum,
        kLargeScreenSpectrumMinimumWidth
    );
}

int Taskbar::spectrumMaximumWidthForScreen() const
{
    // Input: none. Processing: keep the middle audio visualizer elastic but bounded, so the
    // surrounding spacers absorb wide-screen slack instead of pushing fixed content outward.
    // Return: maximum spectrum width.
    return qMax(spectrumMinimumWidthForScreen(), kMaximumSpectrumWidth);
}

void Taskbar::RegisterAsAppBar()
{
    // Input: none. Processing: register/update a top-edge AppBar using Win32 native-pixel
    // coordinates, then position the Qt window in the same native rectangle. Return: none.
    const QRect logicalScreenGeometry = targetScreenLogicalGeometry();
    setGeometry(
        logicalScreenGeometry.left(),
        logicalScreenGeometry.top(),
        logicalScreenGeometry.width(),
        height()
    );

    APPBARDATA abd = { 0 };
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = (HWND)winId();

    if (appBarMessageId == 0) {
        appBarMessageId = RegisterWindowMessageA("KswordTaskbarAppBarMessage");
    }
    abd.uCallbackMessage = appBarMessageId;

    if (!isAppBarRegistered) {
        SHAppBarMessage(ABM_NEW, &abd);
        isAppBarRegistered = true;
    }

    abd.uEdge = ABE_TOP;

    const QRect screenGeometry = targetScreenNativeGeometry();
    const int appBarThickness = appBarThicknessInNativePixels();

    abd.rc.left = screenGeometry.left();
    abd.rc.top = screenGeometry.top();
    abd.rc.right = screenGeometry.right() + 1;
    abd.rc.bottom = screenGeometry.top() + appBarThickness;

    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.bottom = abd.rc.top + appBarThickness;
    SHAppBarMessage(ABM_SETPOS, &abd);

    const QRect nativeAppBarRect(
        abd.rc.left,
        abd.rc.top,
        abd.rc.right - abd.rc.left,
        abd.rc.bottom - abd.rc.top
    );
    const QRect logicalAppBarRect = mapNativeAppBarRectToLogicalScreen(
        nativeAppBarRect,
        screenGeometry,
        logicalScreenGeometry,
        m_targetDevicePixelRatio
    );

    setGeometry(logicalAppBarRect.left(), logicalAppBarRect.top(), logicalAppBarRect.width(), height());
}

Taskbar::~Taskbar()
{
    // 析构只注销本窗口的 AppBar；共享采样由 TaskbarSharedState 管理。
    RemoveAppBar();
}

void Taskbar::onExitClicked()
{
    // 退出按钮的优先目标是结束进程：先触发关闭流程，再退出事件循环。
    close();
    QCoreApplication::quit();
}

void Taskbar::closeEvent(QCloseEvent* event)
{
    // 关闭窗口只注销本窗口 AppBar，避免影响其它显示器上的窗口。
    RemoveAppBar();

    event->accept();
    QMainWindow::closeEvent(event);
}

void Taskbar::RemoveAppBar()
{
    // 输入无；处理当前窗口 AppBar 注销；没有返回值。
    if (!isAppBarRegistered) {
        return;
    }

    APPBARDATA abd = { 0 };
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = (HWND)winId();
    SHAppBarMessage(ABM_REMOVE, &abd);
    isAppBarRegistered = false;
}

void Taskbar::updateTime()
{
    QDateTime currentTime = QDateTime::currentDateTime();
    QString timeStr = currentTime.toString("HH:mm");
    timeLabel->setText(timeStr);
}

void Taskbar::updateCPUUsage()
{
    if (!m_sharedState) {
        return;
    }

    const QVector<int> currentCpuUsage = m_sharedState->cpuUsageSnapshot();
    if (currentCpuUsage.size() != cpuBars.size()) {
        return;
    }

    int maxBarHeight = cpuBarContainer->height() - 8;
    if (maxBarHeight <= 0) {
        maxBarHeight = 24;
    }

    for (int i = 0; i < cpuBars.size(); ++i) {
        int barHeight = (currentCpuUsage[i] * maxBarHeight) / 100;
        barHeight = qBound(0, barHeight, maxBarHeight);
        cpuBars[i]->setFixedSize(4, barHeight);
    }
}

QString Taskbar::formatNetworkSpeed(std::uint64_t bytesPerSecond) const
{
    // 自动单位换算：B/s -> KB/s -> MB/s -> GB/s -> TB/s。
    static const char* units[] = { "B/s", "KB/s", "MB/s", "GB/s", "TB/s" };

    double speed = static_cast<double>(bytesPerSecond);
    int unitIndex = 0;
    while (speed >= 1024.0 && unitIndex < 4) {
        speed /= 1024.0;
        ++unitIndex;
    }

    // 与示例风格保持一致：如 1.2MB/s、240KB/s。
    int precision = 0;
    if (unitIndex > 0 && speed < 100.0) {
        precision = 1;
    }

    return QString("%1%2").arg(QString::number(speed, 'f', precision), units[unitIndex]);
}

void Taskbar::updateNetworkSpeedLabels()
{
    // 仅做 UI 文本刷新，不包含任何系统采样逻辑。
    if (!uploadSpeedLabel || !downloadSpeedLabel || !m_sharedState) {
        return;
    }

    const std::uint64_t up = m_sharedState->uploadSpeedBytesPerSecond();
    const std::uint64_t down = m_sharedState->downloadSpeedBytesPerSecond();

    uploadSpeedLabel->setText(QStringLiteral("\u2191%1").arg(formatNetworkSpeed(up)));
    downloadSpeedLabel->setText(QStringLiteral("\u2193%1").arg(formatNetworkSpeed(down)));
}

void Taskbar::onNotificationPresentationChanged()
{
    // 所有显示器由同一通知服务触发该槽，因此每个屏幕在同一事件循环内切换到相同内容。
    updateNotificationPresentation();
}

void Taskbar::updateNotificationPresentation()
{
    // 地震优先级最高且不淡入；普通通知与常态频谱/时钟严格通过半秒淡出再半秒淡入切换。
    if (m_notificationService == nullptr)
    {
        return;
    }

    const TaskbarNotificationView notification = m_notificationService->currentNotification();
    if (m_notificationService->earthquakeActive())
    {
        showEarthquakePresentation(notification);
        return;
    }

    if (m_notificationService->hasVisibleNotification())
    {
        transitionToNotification(notification);
        return;
    }

    transitionToNormalCenter();
}

void Taskbar::updateNotificationText(const TaskbarNotificationView& notification)
{
    // 所有文本使用单行省略策略，固定高度任务栏不会被长设备路径或剪贴板内容撑开。
    notificationSourceLabel->setText(notification.source);
    notificationTitleLabel->setText(notification.title);
    notificationBodyLabel->setText(notification.body);
    notificationBodyLabel->setToolTip(notification.body);
    displayedNotification = notification;
}

void Taskbar::transitionToNotification(const TaskbarNotificationView& notification)
{
    // 相同普通通知的状态刷新不重复启动动画，避免地震源多报次造成中央区域闪烁。
    if (!earthquakePresentation && notificationVisible &&
        displayedNotification.title == notification.title && displayedNotification.body == notification.body &&
        displayedNotification.source == notification.source)
    {
        return;
    }

    if (!earthquakePresentation)
    {
        // 普通消息刚产生时整条 Taskbar 立即提亮，随后由独立动画在 500ms 内淡回常态。
        flashNotificationBackground();
    }
    stopCentralAnimations();
    if (earthquakePresentation)
    {
        // 地震结束后先让当前警报正文半秒淡出，再淡入排队的普通通知；不让频谱短暂插入。
        earthquakePresentation = false;
        if (alertFlashAnimation != nullptr)
        {
            alertFlashAnimation->stop();
        }
        animateOpacity(notificationCenterOpacity, notificationCenterOpacity->opacity(), 0.0, [this, notification]() {
            updateNotificationText(notification);
            applyTaskbarTheme(false, QColor(QStringLiteral("#0A0F16")));
            animateOpacity(notificationCenterOpacity, 0.0, 1.0, []() {});
        });
        notificationVisible = true;
        return;
    }

    if (notificationVisible)
    {
        // 普通通知之间使用完整的 0.5s 淡出和 0.5s 淡入，内容替换只发生在完全透明后。
        animateOpacity(notificationCenterOpacity, notificationCenterOpacity->opacity(), 0.0, [this, notification]() {
            updateNotificationText(notification);
            animateOpacity(notificationCenterOpacity, 0.0, 1.0, []() {});
        });
        return;
    }

    // 常态频谱与时钟先半秒降低到隐藏，再显示并半秒淡入普通通知。
    animateOpacity(normalCenterOpacity, normalCenterOpacity->opacity(), 0.0, [this, notification]() {
        updateNotificationText(notification);
        animateOpacity(notificationCenterOpacity, 0.0, 1.0, []() {});
    });
    notificationVisible = true;
}

void Taskbar::transitionToNormalCenter()
{
    // 没有待显示通知时，反向执行半秒淡出通知和半秒淡入频谱/时钟。
    if (!notificationVisible)
    {
        return;
    }

    stopCentralAnimations();
    if (earthquakePresentation)
    {
        earthquakePresentation = false;
        if (alertFlashAnimation != nullptr)
        {
            alertFlashAnimation->stop();
        }
    }
    animateOpacity(notificationCenterOpacity, notificationCenterOpacity->opacity(), 0.0, [this]() {
        applyTaskbarTheme(false, QColor(QStringLiteral("#0A0F16")));
        animateOpacity(normalCenterOpacity, 0.0, 1.0, []() {});
    });
    notificationVisible = false;
    displayedNotification = TaskbarNotificationView();
}

void Taskbar::showEarthquakePresentation(const TaskbarNotificationView& notification)
{
    // 新地震到达时立即抛弃当前过渡和普通通知可见状态，不执行中央正文淡入效果。
    const bool sameWarning = earthquakePresentation && displayedNotification.title == notification.title &&
        displayedNotification.body == notification.body;
    stopCentralAnimations();
    updateNotificationText(notification);
    normalCenterOpacity->setOpacity(0.0);
    notificationCenterOpacity->setOpacity(1.0);
    notificationVisible = true;
    earthquakePresentation = true;
    if (!sameWarning || alertFlashAnimation == nullptr ||
        alertFlashAnimation->state() != QAbstractAnimation::Running)
    {
        alertFlashBright = true;
        applyTaskbarTheme(true, QColor(QStringLiteral("#D90000")));
        startAlertFlashCycle();
    }
    if (!sameWarning)
    {
        // 地震消息本身也触发整条 Taskbar 的瞬时提亮和 500ms 淡暗。
        flashNotificationBackground();
    }
}

void Taskbar::animateOpacity(QGraphicsOpacityEffect* effect, qreal startOpacity, qreal endOpacity,
                              const std::function<void()>& completed)
{
    // 每条动画严格固定为 500ms，形成用户要求的淡入/淡出节奏而不会改动任务栏布局尺寸。
    if (effect == nullptr)
    {
        if (completed)
        {
            completed();
        }
        return;
    }

    QPropertyAnimation* animation = new QPropertyAnimation(effect, "opacity", this);
    animation->setDuration(500);
    animation->setStartValue(startOpacity);
    animation->setEndValue(endOpacity);
    animation->setEasingCurve(QEasingCurve::InOutQuad);
    centralAnimations.push_back(animation);
    connect(animation, &QPropertyAnimation::finished, this, [this, animation, completed]() {
        centralAnimations.removeAll(animation);
        if (completed)
        {
            completed();
        }
        animation->deleteLater();
    });
    animation->start();
}

void Taskbar::stopCentralAnimations()
{
    // 地震抢占或新的状态切换会取消旧动画，确保不会有过期的 finished 回调覆盖新内容。
    const QList<QPropertyAnimation*> runningAnimations = centralAnimations;
    centralAnimations.clear();
    for (QPropertyAnimation* animation : runningAnimations)
    {
        if (animation != nullptr)
        {
            animation->stop();
            animation->deleteLater();
        }
    }
}

void Taskbar::showSettingsDialog()
{
    // 每个屏幕窗口最多保留一个非模态设置对话框，但它们连接到相同的全局通知服务和配置文件。
    if (settingsDialog == nullptr)
    {
        settingsDialog = new TaskbarSettingsDialog(m_notificationService, this);
    }
    settingsDialog->show();
    settingsDialog->raise();
    settingsDialog->activateWindow();
}

void Taskbar::onSpectrumDataReady(const QVector<float>& spectrumData)
{
    // 每个显示器窗口共享同一份数据，但各自刷新本窗口内的左右频谱组件。
    if (m_leftSpectrum) {
        m_leftSpectrum->setSpectrumData(spectrumData);
    }
    if (m_rightSpectrum) {
        m_rightSpectrum->setSpectrumData(spectrumData);
    }
}
