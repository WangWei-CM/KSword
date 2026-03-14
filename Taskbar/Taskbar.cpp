#include "Taskbar.h"
#include "Function.h"
#include "Override.h"
#include <windows.h>
#include <shellapi.h>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>
#include <QPixmap>
#include <QStyle>
#include <qpushbutton.h>
#include <qtimer.h>
#include <QDateTime>
#include <QSizePolicy>
#include <Qscreen.h>
#include <thread>
#include <qthread.h>
extern std::vector<int> cpuUsage;

#pragma comment(lib, "shell32.lib")
bool Taskbar::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == appBarMessageId) {
        // 处理应用栏通知
        if (msg->wParam == ABN_POSCHANGED) {
            // 位置变化时重新调整
            RegisterAsAppBar();
        }
        *result = 0;
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
Taskbar::Taskbar(QWidget* parent) : QMainWindow(parent)
{
    // 1. 窗口基本属性设置
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::ToolTip);
    setFixedHeight(32); // 顶部预留区40px高度

    setAttribute(Qt::WA_TranslucentBackground, false);

    // 2. 创建中心部件及布局（原有代码，此处仅展示修改部分）
    centralWidget = new QWidget(this);
    centralWidget->setStyleSheet("background-color: #0d101b;");

    // 外层垂直布局（控制垂直居中）
    QVBoxLayout* vLayout = new QVBoxLayout(centralWidget);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);
    vLayout->setSizeConstraint(QLayout::SetNoConstraint);
    centralWidget->setLayout(vLayout); // 显式给centralWidget设置布局（避免遗漏）

    // 内层水平布局（控制水平居中）
    QHBoxLayout* hLayout = new QHBoxLayout();
    hLayout->setContentsMargins(2, 2, 2, 2);
    hLayout->setSpacing(5);
    // -------------------------- 1. 图片标签修改：固定高度40px，适配宽扁图片 --------------------------
    QLabel* logoLabel = new QLabel(centralWidget);
    QPixmap pixmap(":/Image/Resource/Image/MainLogo.png");
    if (!pixmap.isNull()) {
        // 核心修改：固定标签高度为40px，宽度自适应图片比例（解决“宽扁图片”显示问题）
        logoLabel->setFixedHeight(40); // 按需求设置高度40px
        logoLabel->setMinimumWidth(1); // 宽度最小为1，避免挤压，自适应图片比例

        // 缩放策略：以标签高度为基准，等比例缩放图片（确保高度填满40px，宽度自动匹配）
        QPixmap scaledPix = pixmap.scaled(
            QSize(QWIDGETSIZE_MAX, logoLabel->height()), // 宽度不限（QWIDGETSIZE_MAX），高度固定为标签高度
            Qt::KeepAspectRatio,                         // 保持图片原有宽高比，避免变形
            Qt::SmoothTransformation                     // 高质量缩放，图片更清晰
        );
        logoLabel->setPixmap(scaledPix);
        logoLabel->setAlignment(Qt::AlignCenter); // 图片在标签内居中显示（可选，优化视觉）
        logoLabel->setScaledContents(false);       // 禁用拉伸填充，确保缩放后图片不变形
        logoLabel->setStyleSheet("color: #00ffff;");
    }
    hLayout->addWidget(logoLabel); // 最左侧添加图标

    // -------------------------- 2. 纯文字显示框（无“框”样式，仅展示文字） --------------------------
    QLabel* contentLabel = new QLabel(centralWidget);
    // 1. 初始文字（可根据需求修改，比如空文本或默认提示）
    contentLabel->setText("· WangWei_CM [Dev].");
    //contentLabel->setText("· Crystal_awa.");
    //contentLabel->setText("· Extrella_Explore");

    // 2. 核心：无任何“框”样式，仅保留文字相关设置（去除边框、背景）
    contentLabel->setStyleSheet(R"(
        QLabel {
            /* 去掉所有边框、背景，仅保留文字 */
            border: none;           /* 无边框 */
            background: transparent;/* 透明背景，完全融入父部件 */
            padding: 5px 0;
            color: #00ffff; /* 字体颜色 #00ffff */
            font-size: 12px;        /* 文字大小（可选，匹配整体界面风格） */
        }
        )");
    // 3. 文字布局：左对齐+垂直居中（符合常规阅读习惯）
    contentLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // 4. 紧接图标右侧添加文本框
    hLayout->addWidget(contentLabel);
    hLayout->addStretch(1);
// -------------------------- 创建左右频谱组件 --------------------------
// 创建左侧频谱（从中心向左绘制）
m_leftSpectrum = new SpectrumWidget(SpectrumWidget::CenterToLeft, this);
m_leftSpectrum->setFixedHeight(32);
m_leftSpectrum->setMinimumWidth(450);

// 创建右侧频谱（从中心向右绘制）
m_rightSpectrum = new SpectrumWidget(SpectrumWidget::CenterToRight, this);
m_rightSpectrum->setFixedHeight(32);
m_rightSpectrum->setMinimumWidth(450);

// -------------------------- 初始化音频分析器 --------------------------
m_analyzer = new AudioSpectrumAnalyzer(this);

if (m_analyzer->initialize()) {
    // 将同一个信号连接到两个频谱组件
    bool connected1 = connect(m_analyzer, &AudioSpectrumAnalyzer::spectrumDataReady,
        m_leftSpectrum, &SpectrumWidget::setSpectrumData);
    bool connected2 = connect(m_analyzer, &AudioSpectrumAnalyzer::spectrumDataReady,
        m_rightSpectrum, &SpectrumWidget::setSpectrumData);

    qDebug() << "Left spectrum connection status:" << connected1;
    qDebug() << "Right spectrum connection status:" << connected2;
    qDebug() << "Analyzer object:" << m_analyzer;

    if (!connected1 || !connected2) {
        qWarning() << "FAILED to connect spectrumDataReady signal to one or both spectrum widgets!";

        // 尝试直接连接进行测试
        connect(m_analyzer, &AudioSpectrumAnalyzer::spectrumDataReady,
            this, [this](const QVector<float>& data) {
                qDebug() << "Lambda received spectrum data, size:" << data.size();
                m_leftSpectrum->setSpectrumData(data);
                m_rightSpectrum->setSpectrumData(data);
            });
    }

    m_analyzer->startCapture();
}

// -------------------------- 时间显示标签 --------------------------
timeLabel = new QLabel(centralWidget);
timeLabel->setStyleSheet(R"(
    QLabel {
        border: none;
        background: transparent;
        color: #00ffff;
        font-size: 12px;
        padding: 0px 0px;  /* 左右添加一些内边距，让时间显示更美观 */
    }
)");
timeLabel->setAlignment(Qt::AlignCenter);
timeLabel->setMinimumWidth(60);  // 确保时间标签有足够宽度

// -------------------------- 将频谱和时间标签添加到布局 --------------------------
// 注意：这里使用 addStretch() 来让频谱和时间标签居中显示
//hLayout->addStretch(2);  // 左侧弹性空间
// 设置频谱组件的尺寸策略为 Expanding，让它们占据可用空间
m_leftSpectrum->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
m_rightSpectrum->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
QHBoxLayout* spectrumTimeLayout = new QHBoxLayout();
spectrumTimeLayout->setSpacing(0);       // 组件之间间距为0，保证紧挨着
spectrumTimeLayout->setContentsMargins(0, 0, 0, 0); // 去除内边距
spectrumTimeLayout->addWidget(m_leftSpectrum);
spectrumTimeLayout->addWidget(timeLabel);
spectrumTimeLayout->addWidget(m_rightSpectrum);
hLayout->addLayout(spectrumTimeLayout);


hLayout->addStretch(1);  // 右侧弹性空间
    // -------------------------- 右上角按钮组 --------------------------
    rightBtnContainer = new QWidget(centralWidget);
    rightBtnContainer->setStyleSheet("background: transparent;");
    rightBtnContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    rightBtnLayout = new QHBoxLayout(rightBtnContainer);
    rightBtnLayout->setContentsMargins(0, 0, 0, 0);
    rightBtnLayout->setSpacing(1);

    // -------------------------- 新增：CPU核心占用率柱状图初始化（开始） --------------------------
// 1. 创建CPU柱状图总容器（背景透明，避免遮挡）
    cpuBarContainer = new QWidget(centralWidget);
    cpuBarContainer->setStyleSheet("background: transparent;");
    // 给容器设置水平布局（用于放所有柱子）
    QHBoxLayout* cpuBarLayout = new QHBoxLayout(cpuBarContainer);
    cpuBarLayout->setContentsMargins(4, 4, 4, 4); // 上下左右留边（避免柱子贴边）
    cpuBarLayout->setSpacing(2);                  // 柱子之间的间距（可调整）

    // 2. 根据CPU核心数创建柱子（每个核心1个柱子）
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    int coreCount = sysInfo.dwNumberOfProcessors; // 获取核心数
    cpuBars.resize(coreCount);                    // 初始化柱子数组大小

    for (int i = 0; i < coreCount; ++i) {
        QLabel* bar = new QLabel(cpuBarContainer);
        bar->setStyleSheet("background-color: #00ffff;"); // 柱子颜色#00FFFF
        bar->setAlignment(Qt::AlignBottom);              // 柱子从底部向上显示（0%时高度为0）

        cpuBars[i] = bar;                                // 加入柱子数组
        cpuBarLayout->addWidget(bar);                    // 加入柱状图布局

        QLayoutItem* barItem = cpuBarLayout->itemAt(i);  // 获取当前柱子的布局项
        barItem->setAlignment(Qt::AlignBottom);          // 强制布局项垂直贴底
    }

    // 3. 将CPU柱状图容器加入主水平布局（放在rightBtnContainer左边）
    hLayout->addWidget(cpuBarContainer);

        QSize iconSize(20, 20);

    // 1. 锁定按钮
    GlowIconButton *lockBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/lock_line.svg", 
        iconSize, 
        rightBtnContainer
    );
    rightBtnLayout->addWidget(lockBtn);
    QObject::connect(lockBtn, &QPushButton::clicked, lockWorkstation);

    // 2. 工具按钮（打开CMD）
    GlowIconButton *toolBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/tool_line.svg", 
        iconSize, 
        rightBtnContainer
    );
    rightBtnLayout->addWidget(toolBtn);
    QObject::connect(toolBtn, &QPushButton::clicked, openCmd);

    // 3. 用户按钮（自定义功能）
    GlowIconButton *userBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/user_2_line.svg", 
        iconSize, 
        rightBtnContainer
    );
    rightBtnLayout->addWidget(userBtn);
    QObject::connect(userBtn, &QPushButton::clicked, userCustomFunction);

    // 设置图标
    QIcon exitIcon(":/Icon/Resource/Icon/exit_fill.svg");
    GlowIconButton* exitBtn = new GlowIconButton(
        ":/Icon/Resource/Icon/exit_fill.svg", // 图标路径
        iconSize,
        rightBtnContainer
    );
    rightBtnLayout->addWidget(exitBtn);

    rightBtnLayout->setAlignment(Qt::AlignRight);
    connect(exitBtn, &QPushButton::clicked, this, &Taskbar::onExitClicked);


    hLayout->addWidget(rightBtnContainer);

    vLayout->addLayout(hLayout);
    setCentralWidget(centralWidget);

    // 2. 注册为应用桌面工具栏(AppBar) — 在 UI 构建完成后注册，保证 winId 与几何正确
    

    // -------------------------- 初始化计时器（每0.5秒更新一次） --------------------------
    timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &Taskbar::updateTime);
    timer->start();
    updateTime();
    RegisterAsAppBar();
    cpuUpdateTimer = new QTimer(this);
    cpuUpdateTimer->setInterval(200); // 1秒 = 1000毫秒
    // 2. 关联计时器到CPU更新函数
    connect(cpuUpdateTimer, &QTimer::timeout, this, &Taskbar::updateCPUUsage);
    // 3. 启动计时器，并首次触发更新（避免初始空白）
    cpuUpdateTimer->start();
    updateCPUUsage();
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
            // 兜底：若获取失败，用默认1920x1080的工作区
            return QRect(0, 0, 1920, 1080);
        }
        // 2. 获取屏幕的“工作区矩形”（x=0, y=0, width=屏幕宽, height=屏幕高-系统任务栏高）
        QRect workArea = screen->availableGeometry();
        return workArea;
        };
    setFixedWidth(GetWindowWeight().width());
}

Taskbar::~Taskbar()
{
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
    close();
}
void Taskbar::closeEvent(QCloseEvent* event)
{
    APPBARDATA abd = { 0 };
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = (HWND)winId();
    SHAppBarMessage(ABM_REMOVE, &abd);

    // 接受关闭事件（默认行为，可省略，但不要写event->ignore()）
    event->accept();

    // 调用基类处理（可选，基类默认也是accept）

    QMainWindow::closeEvent(event);
}
void Taskbar::updateTime()
{
    // 获取当前时间，格式化为24小时制（精确到分钟）：HH:mm
    QDateTime currentTime = {};currentTime = QDateTime::currentDateTime();
    QString timeStr = currentTime.toString("HH:mm");
    // 更新标签显示
    timeLabel->setText(timeStr);
}
void Taskbar::updateCPUUsage() {
    // 防止核心数不匹配（异常情况）
    static bool isFirstRun = true;
    if (isFirstRun) {
        std::thread([]()->void {
            while (1) {
                getCPUCoreUsage();
                QThread::msleep(200);
            }
            }).detach();
        isFirstRun = 0;
    }

    if (cpuUsage.size() != cpuBars.size()) return;

    // 2. 计算柱子最大可用高度（窗口高度32 - 上下边距4*2 = 24，可调整）
    int maxBarHeight = cpuBarContainer->height() - 8; // 上下各4px边距
    if (maxBarHeight <= 0) maxBarHeight = 24; // 兜底高度（避免负数）

    // 3. 遍历每个柱子，设置高度（占用率% → 像素高度）
    for (int i = 0; i < cpuBars.size(); ++i) {
        // 占用率换算成柱子高度（例：50% → 24 * 0.5 = 12px）
        int barHeight = (cpuUsage[i] * maxBarHeight) / 100;
        // 限制高度在0~maxBarHeight之间（避免异常值）
        barHeight = qBound(0, barHeight, maxBarHeight);
        // 设置柱子尺寸：宽度固定4px（可调整），高度为计算值
        cpuBars[i]->setFixedSize(4, barHeight);
    }
}
void Taskbar::onSpectrumDataReady(const QVector<float>& spectrumData)
{
    // 直接将频谱数据传递给显示组件
    m_spectrumWidget->setSpectrumData(spectrumData);
}