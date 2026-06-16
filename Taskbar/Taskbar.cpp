#include "Taskbar.h"
#include "Function.h"
#include "Override.h"

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
#include <QSizePolicy>
#include <Qscreen.h>

#pragma comment(lib, "shell32.lib")

namespace {
constexpr int kTaskbarLogicalHeight = 32;
constexpr int kTaskbarOuterMargin = 2;
constexpr int kTaskbarContentHeight = kTaskbarLogicalHeight - (kTaskbarOuterMargin * 2);
constexpr int kMinimumSpectrumWidth = 80;
constexpr int kLargeScreenSpectrumMinimumWidth = 220;
constexpr int kMaximumSpectrumWidth = 320;
constexpr int kCompactScreenNonSpectrumBudget = 620;

struct MonitorSearchContext {
    QString targetName;
    QRect nativeGeometry;
    bool found;
};

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
}

bool Taskbar::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == appBarMessageId) {
        // 处理应用栏通知。
        if (msg->wParam == ABN_POSCHANGED) {
            // 位置变化时重新调整。
            RegisterAsAppBar();
        }
        *result = 0;
        return true;
    }

    return QMainWindow::nativeEvent(eventType, message, result);
}

Taskbar::Taskbar(QScreen* targetScreen, TaskbarSharedState* sharedState, QWidget* parent)
    : QMainWindow(parent),
      m_leftSpectrum(nullptr),
      m_rightSpectrum(nullptr),
      m_sharedState(sharedState),
      m_targetScreenGeometry(targetScreen ? targetScreen->geometry() : QRect()),
      m_targetScreenName(targetScreen ? targetScreen->name() : QString()),
      m_targetDevicePixelRatio(targetScreen ? targetScreen->devicePixelRatio() : 1.0),
      cpuBarContainer(nullptr),
      timer(nullptr),
      timeLabel(nullptr),
      networkSpeedContainer(nullptr),
      uploadSpeedLabel(nullptr),
      downloadSpeedLabel(nullptr),
      networkUiTimer(nullptr),
      isAppBarRegistered(false),
      centralWidget(nullptr),
      rightBtnContainer(nullptr),
      rightBtnLayout(nullptr),
      exitBtn(nullptr),
      appBarMessageId(0),
      cpuUpdateTimer(nullptr)
{
    // 1. 窗口基本属性设置。
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::ToolTip);
    setFixedHeight(kTaskbarLogicalHeight);
    setAttribute(Qt::WA_TranslucentBackground, false);

    // 2. 创建中心容器与外层布局。
    centralWidget = new QWidget(this);
    centralWidget->setStyleSheet("background-color: #0A0F16;");

    QVBoxLayout* vLayout = new QVBoxLayout(centralWidget);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);
    vLayout->setSizeConstraint(QLayout::SetNoConstraint);
    centralWidget->setLayout(vLayout);

    QHBoxLayout* hLayout = new QHBoxLayout();
    hLayout->setContentsMargins(2, 2, 2, 2);
    hLayout->setSpacing(5);

    // -------------------------- 左侧 Logo --------------------------
    QLabel* logoLabel = new QLabel(centralWidget);
    QPixmap pixmap(":/Image/Resource/Image/MainLogo.png");
    if (!pixmap.isNull()) {
        logoLabel->setFixedHeight(kTaskbarContentHeight);
        logoLabel->setMinimumWidth(1);

        QPixmap scaledPix = pixmap.scaled(
            QSize(QWIDGETSIZE_MAX, logoLabel->height()),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
        logoLabel->setPixmap(scaledPix);
        logoLabel->setAlignment(Qt::AlignCenter);
        logoLabel->setScaledContents(false);
        logoLabel->setStyleSheet("color: #00FFFF;");
    }
    hLayout->addWidget(logoLabel);

    // -------------------------- 左侧文字 --------------------------
    QLabel* contentLabel = new QLabel(centralWidget);
    contentLabel->setText("· WangWei_CM [Dev].");

    contentLabel->setStyleSheet(R"(
        QLabel {
            border: none;
            background: transparent;
            padding: 5px 0;
            color: #00FFFF;
            font-size: 12px;
        }
        )");
    contentLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hLayout->addWidget(contentLabel);
    hLayout->addStretch(1);

    // -------------------------- 创建左右频谱组件 --------------------------
    m_leftSpectrum = new SpectrumWidget(SpectrumWidget::CenterToLeft, this);
    m_leftSpectrum->setFixedHeight(kTaskbarContentHeight);
    m_leftSpectrum->setMinimumWidth(spectrumMinimumWidthForScreen());
    m_leftSpectrum->setMaximumWidth(spectrumMaximumWidthForScreen());

    m_rightSpectrum = new SpectrumWidget(SpectrumWidget::CenterToRight, this);
    m_rightSpectrum->setFixedHeight(kTaskbarContentHeight);
    m_rightSpectrum->setMinimumWidth(spectrumMinimumWidthForScreen());
    m_rightSpectrum->setMaximumWidth(spectrumMaximumWidthForScreen());

    // -------------------------- 连接共享频谱数据 --------------------------
    if (m_sharedState) {
        connect(
            m_sharedState,
            &TaskbarSharedState::spectrumDataReady,
            this,
            &Taskbar::onSpectrumDataReady,
            Qt::QueuedConnection
        );
    }

    // -------------------------- 时间标签 --------------------------
    timeLabel = new QLabel(centralWidget);
    timeLabel->setStyleSheet(R"(
        QLabel {
            border: none;
            background: transparent;
            color: #00FFFF;
            font-size: 12px;
            padding: 0px 0px;
        }
    )");
    timeLabel->setAlignment(Qt::AlignCenter);
    timeLabel->setMinimumWidth(52);

    // -------------------------- 中部频谱+时间 --------------------------
    m_leftSpectrum->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_rightSpectrum->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QHBoxLayout* spectrumTimeLayout = new QHBoxLayout();
    spectrumTimeLayout->setSpacing(0);
    spectrumTimeLayout->setContentsMargins(0, 0, 0, 0);
    spectrumTimeLayout->addWidget(m_leftSpectrum);
    spectrumTimeLayout->addWidget(timeLabel);
    spectrumTimeLayout->addWidget(m_rightSpectrum);
    hLayout->addLayout(spectrumTimeLayout);

    hLayout->addStretch(1);

    // -------------------------- 右上角按钮组容器 --------------------------
    rightBtnContainer = new QWidget(centralWidget);
    rightBtnContainer->setStyleSheet("background: transparent;");
    rightBtnContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    rightBtnLayout = new QHBoxLayout(rightBtnContainer);
    rightBtnLayout->setContentsMargins(0, 0, 0, 0);
    rightBtnLayout->setSpacing(1);

    // -------------------------- CPU 核心占用率柱状图 --------------------------
    cpuBarContainer = new QWidget(centralWidget);
    cpuBarContainer->setStyleSheet("background: transparent;");

    QHBoxLayout* cpuBarLayout = new QHBoxLayout(cpuBarContainer);
    cpuBarLayout->setContentsMargins(3, 3, 3, 3);
    cpuBarLayout->setSpacing(2);

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    int coreCount = static_cast<int>(sysInfo.dwNumberOfProcessors);
    cpuBars.resize(coreCount);

    for (int i = 0; i < coreCount; ++i) {
        QLabel* bar = new QLabel(cpuBarContainer);
        bar->setStyleSheet("background-color: #00FFFF;");
        bar->setAlignment(Qt::AlignBottom);

        cpuBars[i] = bar;
        cpuBarLayout->addWidget(bar);

        QLayoutItem* barItem = cpuBarLayout->itemAt(i);
        barItem->setAlignment(Qt::AlignBottom);
    }

    // 将 CPU 柱图加入右侧区域。
    hLayout->addWidget(cpuBarContainer);

    // -------------------------- CPU 旁边新增网络速率区（两行） --------------------------
    networkSpeedContainer = new QWidget(centralWidget);
    networkSpeedContainer->setStyleSheet("background: transparent;");
    networkSpeedContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    networkSpeedContainer->setMinimumWidth(74);

    QVBoxLayout* networkSpeedLayout = new QVBoxLayout(networkSpeedContainer);
    networkSpeedLayout->setContentsMargins(2, 2, 2, 2);
    networkSpeedLayout->setSpacing(0);

    uploadSpeedLabel = new QLabel(networkSpeedContainer);
    uploadSpeedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    uploadSpeedLabel->setStyleSheet(
        "border: none; background: transparent; color: #00FFFF; font-size: 10px;"
    );
    uploadSpeedLabel->setText(QStringLiteral("\u21910B/s"));

    downloadSpeedLabel = new QLabel(networkSpeedContainer);
    downloadSpeedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    downloadSpeedLabel->setStyleSheet(
        "border: none; background: transparent; color: #00FFFF; font-size: 10px;"
    );
    downloadSpeedLabel->setText(QStringLiteral("\u21930B/s"));

    networkSpeedLayout->addWidget(uploadSpeedLabel);
    networkSpeedLayout->addWidget(downloadSpeedLabel);
    hLayout->addWidget(networkSpeedContainer);

    // -------------------------- 右侧功能按钮 --------------------------
    QSize iconSize(20, 20);

    GlowIconButton* lockBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/lock_line.svg",
        iconSize,
        rightBtnContainer
    );
    rightBtnLayout->addWidget(lockBtn);
    QObject::connect(lockBtn, &QPushButton::clicked, lockWorkstation);

    GlowIconButton* toolBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/tool_line.svg",
        iconSize,
        rightBtnContainer
    );
    rightBtnLayout->addWidget(toolBtn);
    QObject::connect(toolBtn, &QPushButton::clicked, openCmd);

    GlowIconButton* userBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/user_2_line.svg",
        iconSize,
        rightBtnContainer
    );
    rightBtnLayout->addWidget(userBtn);
    QObject::connect(userBtn, &QPushButton::clicked, userCustomFunction);

    exitBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/exit_fill.svg",
        iconSize,
        rightBtnContainer
    );
    rightBtnLayout->addWidget(exitBtn);

    rightBtnLayout->setAlignment(Qt::AlignRight);
    connect(exitBtn, &QPushButton::clicked, this, &Taskbar::onExitClicked);

    hLayout->addWidget(rightBtnContainer);

    vLayout->addLayout(hLayout);
    setCentralWidget(centralWidget);

    // -------------------------- 启动时间与 CPU 刷新 --------------------------
    timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &Taskbar::updateTime);
    timer->start();
    updateTime();

    RegisterAsAppBar();

    cpuUpdateTimer = new QTimer(this);
    cpuUpdateTimer->setInterval(200);
    connect(cpuUpdateTimer, &QTimer::timeout, this, &Taskbar::updateCPUUsage);
    cpuUpdateTimer->start();
    updateCPUUsage();

    // -------------------------- 启动网络采样线程与文本刷新 --------------------------
    networkUiTimer = new QTimer(this);
    networkUiTimer->setInterval(250);
    connect(networkUiTimer, &QTimer::timeout, this, &Taskbar::updateNetworkSpeedLabels);
    networkUiTimer->start();

    updateNetworkSpeedLabels();
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
