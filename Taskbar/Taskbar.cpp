#include "Taskbar.h"
#include "Function.h"
#include "Override.h"

#include <windows.h>
#include <shellapi.h>
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
    setFixedHeight(32);
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
        logoLabel->setFixedHeight(40);
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
    m_leftSpectrum->setFixedHeight(32);
    m_leftSpectrum->setMinimumWidth(450);

    m_rightSpectrum = new SpectrumWidget(SpectrumWidget::CenterToRight, this);
    m_rightSpectrum->setFixedHeight(32);
    m_rightSpectrum->setMinimumWidth(450);

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
    timeLabel->setMinimumWidth(60);

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
    cpuBarLayout->setContentsMargins(4, 4, 4, 4);
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
    networkSpeedContainer->setMinimumWidth(80);

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

void Taskbar::RegisterAsAppBar()
{
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

    QRect screenGeometry;
    if (m_targetScreenGeometry.isValid()) {
        screenGeometry = m_targetScreenGeometry;
    }
    else if (screen()) {
        screenGeometry = screen()->geometry();
    }
    else {
        screenGeometry = QRect(0, 0, 1920, height());
    }

    abd.rc.left = screenGeometry.left();
    abd.rc.top = screenGeometry.top();
    abd.rc.right = screenGeometry.right() + 1;
    abd.rc.bottom = screenGeometry.top() + height();

    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.bottom = abd.rc.top + height();
    SHAppBarMessage(ABM_SETPOS, &abd);

    setGeometry(
        abd.rc.left,
        abd.rc.top,
        abd.rc.right - abd.rc.left,
        abd.rc.bottom - abd.rc.top
    );
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
