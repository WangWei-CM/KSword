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
#include <thread>
#include <chrono>

extern std::vector<int> cpuUsage;

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

Taskbar::Taskbar(QWidget* parent)
    : QMainWindow(parent),
      m_leftSpectrum(nullptr),
      m_rightSpectrum(nullptr),
      m_analyzer(nullptr),
      m_spectrumWidget(nullptr),
      cpuBarContainer(nullptr),
      timer(nullptr),
      timeLabel(nullptr),
      networkSpeedContainer(nullptr),
      uploadSpeedLabel(nullptr),
      downloadSpeedLabel(nullptr),
      networkUiTimer(nullptr),
      cpuWorkerRunning(false),
      networkWorkerRunning(false),
      uploadBytesPerSecond(0),
      downloadBytesPerSecond(0),
      centralWidget(nullptr),
      rightBtnContainer(nullptr),
      rightBtnLayout(nullptr),
      exitBtn(nullptr),
      appBarMessageId(0),
      hWnd(nullptr),
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
        logoLabel->setStyleSheet("color: #43A0FF;");
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
            color: #43A0FF;
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

    // -------------------------- 初始化音频分析器 --------------------------
    m_analyzer = new AudioSpectrumAnalyzer(this);
    if (m_analyzer->initialize()) {
        bool connected1 = connect(
            m_analyzer,
            &AudioSpectrumAnalyzer::spectrumDataReady,
            m_leftSpectrum,
            &SpectrumWidget::setSpectrumData
        );
        bool connected2 = connect(
            m_analyzer,
            &AudioSpectrumAnalyzer::spectrumDataReady,
            m_rightSpectrum,
            &SpectrumWidget::setSpectrumData
        );

        qDebug() << "Left spectrum connection status:" << connected1;
        qDebug() << "Right spectrum connection status:" << connected2;
        qDebug() << "Analyzer object:" << m_analyzer;

        if (!connected1 || !connected2) {
            qWarning() << "FAILED to connect spectrumDataReady signal to one or both spectrum widgets!";

            connect(m_analyzer, &AudioSpectrumAnalyzer::spectrumDataReady,
                this, [this](const QVector<float>& data) {
                    qDebug() << "Lambda received spectrum data, size:" << data.size();
                    m_leftSpectrum->setSpectrumData(data);
                    m_rightSpectrum->setSpectrumData(data);
                });
        }

        m_analyzer->startCapture();
    }

    // -------------------------- 时间标签 --------------------------
    timeLabel = new QLabel(centralWidget);
    timeLabel->setStyleSheet(R"(
        QLabel {
            border: none;
            background: transparent;
            color: #43A0FF;
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
        bar->setStyleSheet("background-color: #43A0FF;");
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
        "border: none; background: transparent; color: #ffd54a; font-size: 10px;"
    );
    uploadSpeedLabel->setText(QStringLiteral("\u21910B/s"));

    downloadSpeedLabel = new QLabel(networkSpeedContainer);
    downloadSpeedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    downloadSpeedLabel->setStyleSheet(
        "border: none; background: transparent; color: #61d969; font-size: 10px;"
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
    startCpuWorker();
    updateCPUUsage();

    // -------------------------- 启动网络采样线程与文本刷新 --------------------------
    networkUiTimer = new QTimer(this);
    networkUiTimer->setInterval(250);
    connect(networkUiTimer, &QTimer::timeout, this, &Taskbar::updateNetworkSpeedLabels);
    networkUiTimer->start();

    startNetworkWorker();
    updateNetworkSpeedLabels();
}

void Taskbar::RegisterAsAppBar()
{
    APPBARDATA abd = { 0 };
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = (HWND)winId();

    abd.uCallbackMessage = RegisterWindowMessageA("AppBarMessage");
    SHAppBarMessage(ABM_NEW, &abd);

    abd.uEdge = ABE_TOP;
    SHAppBarMessage(ABM_SETAUTOHIDEBAR, &abd);

    RECT rc;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
    abd.rc = rc;
    abd.rc.bottom = abd.rc.top + 40;

    SHAppBarMessage(ABM_QUERYPOS, &abd);
    SHAppBarMessage(ABM_SETPOS, &abd);

    move(abd.rc.left, abd.rc.top);

    auto GetWindowWeight = [=]()->QRect {
        QScreen* screen = this->screen();
        if (!screen) {
            return QRect(0, 0, 1920, 1080);
        }
        QRect workArea = screen->availableGeometry();
        return workArea;
    };

    setFixedWidth(GetWindowWeight().width());
}

Taskbar::~Taskbar()
{
    // 析构前先停止后台线程，避免线程继续运行导致进程无法退出。
    stopCpuWorker();
    stopNetworkWorker();

    if (m_analyzer) {
        m_analyzer->stopCapture();
    }

    APPBARDATA abd = { 0 };
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = (HWND)winId();
    SHAppBarMessage(ABM_REMOVE, &abd);
}

void Taskbar::onExitClicked()
{
    // 退出按钮的优先目标是结束进程：先触发关闭流程，再退出事件循环。
    close();
    QCoreApplication::quit();
}

void Taskbar::closeEvent(QCloseEvent* event)
{
    // 关闭窗口时先停止后台线程，确保析构阶段无并发访问。
    stopCpuWorker();
    stopNetworkWorker();

    APPBARDATA abd = { 0 };
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = (HWND)winId();
    SHAppBarMessage(ABM_REMOVE, &abd);

    event->accept();
    QMainWindow::closeEvent(event);
}

void Taskbar::updateTime()
{
    QDateTime currentTime = QDateTime::currentDateTime();
    QString timeStr = currentTime.toString("HH:mm");
    timeLabel->setText(timeStr);
}

void Taskbar::updateCPUUsage()
{
    if (cpuUsage.size() != cpuBars.size()) {
        return;
    }

    int maxBarHeight = cpuBarContainer->height() - 8;
    if (maxBarHeight <= 0) {
        maxBarHeight = 24;
    }

    for (int i = 0; i < cpuBars.size(); ++i) {
        int barHeight = (cpuUsage[i] * maxBarHeight) / 100;
        barHeight = qBound(0, barHeight, maxBarHeight);
        cpuBars[i]->setFixedSize(4, barHeight);
    }
}

void Taskbar::startCpuWorker()
{
    // 防止重复启动线程。
    if (cpuWorkerRunning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    cpuWorkerThread = std::thread([this]() {
        while (cpuWorkerRunning.load(std::memory_order_acquire)) {
            getCPUCoreUsage();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });
}

void Taskbar::stopCpuWorker()
{
    // 先发停止信号，再在主线程 join，保证线程安全退出。
    cpuWorkerRunning.store(false, std::memory_order_release);
    if (cpuWorkerThread.joinable()) {
        cpuWorkerThread.join();
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

void Taskbar::startNetworkWorker()
{
    // 防止重复启动线程。
    if (networkWorkerRunning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    networkWorkerThread = std::thread([this]() {
        while (networkWorkerRunning.load(std::memory_order_acquire)) {
            const NetworkSpeedRate rate = getNetworkSpeedRate();

            uploadBytesPerSecond.store(rate.uploadBytesPerSecond, std::memory_order_release);
            downloadBytesPerSecond.store(rate.downloadBytesPerSecond, std::memory_order_release);

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
}

void Taskbar::stopNetworkWorker()
{
    // 先发停止信号，再在主线程 join，保证线程安全退出。
    networkWorkerRunning.store(false, std::memory_order_release);
    if (networkWorkerThread.joinable()) {
        networkWorkerThread.join();
    }
}

void Taskbar::updateNetworkSpeedLabels()
{
    // 仅做 UI 文本刷新，不包含任何系统采样逻辑。
    if (!uploadSpeedLabel || !downloadSpeedLabel) {
        return;
    }

    const std::uint64_t up = uploadBytesPerSecond.load(std::memory_order_acquire);
    const std::uint64_t down = downloadBytesPerSecond.load(std::memory_order_acquire);

    uploadSpeedLabel->setText(QStringLiteral("\u2191%1").arg(formatNetworkSpeed(up)));
    downloadSpeedLabel->setText(QStringLiteral("\u2193%1").arg(formatNetworkSpeed(down)));
}

void Taskbar::onSpectrumDataReady(const QVector<float>& spectrumData)
{
    // 直接将频谱数据传递给显示组件。
    m_spectrumWidget->setSpectrumData(spectrumData);
}
