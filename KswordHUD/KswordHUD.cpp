//#include "stdafx.h"

#include <windows.h>
#include <QCoreApplication>
#include <QOpenGLPaintDevice>
#include <QPainter>

#include <QEasingCurve>
#include <QAbstractAnimation>
#include <QScreen>
#include <QApplication>
#include <QTimer>
#include <QSurfaceFormat>
// 硬件监控头文件
#include "hGet.h"
#include "KswordHUD.h"
// 命名空间（简化代码）
//using namespace QtCharts;

OpenGLWidget::OpenGLWidget(QWidget* parent)
    : QOpenGLWidget(parent), m_opacity(0.5) {
    // 设置垂直同步
    QSurfaceFormat format;
    format.setSwapInterval(1);
    setFormat(format);
}

OpenGLWidget::~OpenGLWidget() {
}

void OpenGLWidget::setBackgroundPixmap(const QPixmap& pixmap) {
    m_backgroundPixmap = pixmap;
    update();
}

void OpenGLWidget::setOpacity(qreal opacity) {
    m_opacity = opacity;
    update();
}

void OpenGLWidget::setChildWidgets(QWidget* left, QWidget* right) {
    m_leftWidget = left;
    m_rightWidget = right;
    if (left) {
        left->setParent(this);
        left->show();
    }
    if (right) {
        right->setParent(this);
        right->show();
    }
}

QPixmap OpenGLWidget::captureContent() {
    QPixmap pixmap(size());
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绘制背景（与paintGL完全一致）
    if (!m_backgroundPixmap.isNull()) {
        painter.setOpacity(m_opacity);
        QPixmap scaled = m_backgroundPixmap.scaled(
            size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
        );
        QRect rect = scaled.rect();
        rect.moveCenter(QRect(QPoint(), size()).center());
        painter.drawPixmap(rect, scaled);
    }
    else {
        painter.setOpacity(m_opacity);
        painter.fillRect(rect(), QColor(0, 0, 0, 128));
    }

    // 绘制子部件（与paintGL完全一致，直接渲染到painter）
    painter.setOpacity(1);
    if (m_leftWidget && m_leftWidget->isVisible()) {
        m_leftWidget->render(&painter, m_leftWidget->pos(), QRegion(),
            QWidget::DrawWindowBackground | QWidget::DrawChildren);
    }
    if (m_rightWidget && m_rightWidget->isVisible()) {
        m_rightWidget->render(&painter, m_rightWidget->pos(), QRegion(),
            QWidget::DrawWindowBackground | QWidget::DrawChildren);
    }
    // 移除setWindowOpacity(1.0)和setWindowOpacity(0.5)的波动操作
    return pixmap;
}
void OpenGLWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0, 0, 0, 0);
}

void OpenGLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    if (m_leftWidget && m_rightWidget) {
        int halfWidth = w / 2;
        m_leftWidget->setGeometry(0, 0, halfWidth, h);
        m_rightWidget->setGeometry(halfWidth, 0, halfWidth, h);
    }
}

void OpenGLWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绘制背景图（保持不变）
    if (!m_backgroundPixmap.isNull()) {
        painter.setOpacity(m_opacity);
        QPixmap scaled = m_backgroundPixmap.scaled(
            size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
        );
        QRect rect = scaled.rect();
        rect.moveCenter(QRect(QPoint(), size()).center());
        painter.drawPixmap(rect, scaled);
    }
    else {
        painter.setOpacity(m_opacity);
        painter.fillRect(rect(), QColor(0, 0, 0, 128));
    }

    // 关键修复：直接渲染子部件到当前painter，不使用临时QPixmap
    painter.setOpacity(0.99);
    //if (m_leftWidget && m_leftWidget->isVisible()) {
    //    m_leftWidget->render(&painter, m_leftWidget->pos(), QRegion(),
    //        QWidget::DrawWindowBackground | QWidget::DrawChildren);
    //}
    //if (m_rightWidget && m_rightWidget->isVisible()) {
    //    m_rightWidget->render(&painter, m_rightWidget->pos(), QRegion(),
    //        QWidget::DrawWindowBackground | QWidget::DrawChildren);
    //}
}
KswordHUD::KswordHUD(QWidget* parent)
    : QMainWindow(parent)
    , m_windowOpacity(0.5)
    , m_isVisible(false)
    , m_isAnimating(false)
    , m_leftWidget(nullptr)
    , m_rightWidget(nullptr)
    , m_tableWidget(nullptr)
    , m_hideAnimation(nullptr)
    , m_showAnimation(nullptr)
    , m_glWidget(nullptr)
    , m_contentScale(1.0)
    , m_paintFrameCount(0)
    , m_animationFrameCount(0)
{
    m_debugTimer.start();
    debugOutput("KswordHUD 构造函数开始");

    // 窗口基础设置
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);

    // 获取主屏幕并设置全屏
    QScreen* primaryScreen = QApplication::primaryScreen();
    if (primaryScreen) {
        setGeometry(primaryScreen->geometry());
        debugOutput("设置主屏幕几何:" + QString::number(primaryScreen->geometry().x()) + "," +
            QString::number(primaryScreen->geometry().y()) + " " +
            QString::number(primaryScreen->geometry().width()) + "x" +
            QString::number(primaryScreen->geometry().height()));
    }
    else {
        showFullScreen();
    }

    m_originalRect = geometry();
    debugOutput("窗口初始几何:" + QString::number(m_originalRect.x()) + "," +
        QString::number(m_originalRect.y()) + " " +
        QString::number(m_originalRect.width()) + "x" +
        QString::number(m_originalRect.height()));

    // 创建OpenGL加速部件
    m_glWidget = new OpenGLWidget(this);
    setCentralWidget(m_glWidget);

    // 加载背景图并设置到OpenGL部件
    const QString exePath = QCoreApplication::applicationDirPath();
    const QString backgroundPath = exePath + "/Style/HUD_background.png";
    QPixmap background;
    if (background.load(backgroundPath)) {
        m_glWidget->setBackgroundPixmap(background);
        debugOutput("背景图片加载成功:" + backgroundPath);
    }
    else {
        debugOutput("[警告] 背景图片加载失败！路径：" + backgroundPath);
        background = QPixmap(size());
        background.fill(QColor(30, 30, 30, 200));
        m_glWidget->setBackgroundPixmap(background);
    }

    // 左侧容器（50%宽度）
    m_leftWidget = new QWidget(m_glWidget);
    m_leftWidget->setStyleSheet("background-color: transparent;");
    QVBoxLayout* leftLayout = new QVBoxLayout(m_leftWidget);
    leftLayout->setContentsMargins(30, 0, 30, 0);
    leftLayout->setSpacing(0);

    // 左侧顶部间隔
    QSpacerItem* leftTopSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    leftLayout->addSpacerItem(leftTopSpacer);
    leftLayout->setStretch(0, 1);

    // 左侧表格 - 占3/4高度
    m_tableWidget = new QTableWidget;
    m_tableWidget->setStyleSheet(R"(
        QTableWidget {
            background-color: rgba(0, 0, 0, 0.3);
            color: #FFFFFF;
            gridline-color: rgba(255, 255, 255, 0.2);
            border: none;
            border-radius: 8px;
        }
        QHeaderView::section {
            background-color: rgba(0, 0, 0, 0.5);
            color: #FFFFFF;
            border: none;
            padding: 8px;
        }
        QTableWidget::item {
            padding: 8px;
            border-bottom: 1px solid rgba(255, 255, 255, 0.1);
        }
    )");
    m_tableWidget->setRowCount(10);
    m_tableWidget->setColumnCount(3);
    m_tableWidget->setHorizontalHeaderLabels({ "列1", "列2", "列3" });
    leftLayout->addWidget(m_tableWidget);
    leftLayout->setStretch(1, 3);

    // -------------------------- 关键修改1：右侧UI改造为TabWidget --------------------------
// 右侧容器（50%宽度，透明背景）
    m_rightWidget = new QWidget(m_glWidget);
    m_rightWidget->setStyleSheet("background-color: transparent;");
    QVBoxLayout* rightLayout = new QVBoxLayout(m_rightWidget);
    rightLayout->setContentsMargins(30, 0, 30, 0);
    rightLayout->setSpacing(0);

    // 右侧顶部间隔（保持原有布局比例）
    QSpacerItem* rightTopSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    rightLayout->addSpacerItem(rightTopSpacer);
    rightLayout->setStretch(0, 1);

    // 右侧TabWidget（核心：替换原有空白Widget）
    m_rightTabWidget = new QTabWidget;
    // TabWidget样式（透明背景+白色标签）
    m_rightTabWidget->setStyleSheet(R"(
        QTabWidget::pane { /* Tab容器背景 */
            border-top: 2px solid #C2C7CB;
            background-color: rgba(0, 0, 0, 0.3);
            border-radius: 8px;
        }
        QTabBar::tab { /* 未选中标签 */
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #E1E1E1, stop:1 #D3D3D3);
            border: 1px solid #C4C4C3;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            min-width: 80px;
            padding: 6px;
            margin-right: 2px;
        }
        QTabBar::tab:selected, QTabBar::tab:hover { /* 选中/hover标签 */
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #fafafa, stop:1 #e7e7e7);
        }
        QTabWidget::tab-bar { /* 标签栏居中 */
            alignment: Center;
        }
    )");
    rightLayout->addWidget(m_rightTabWidget);
    rightLayout->setStretch(1, 3);

    // -------------------------- 关键修改2：初始化3个硬件图表 --------------------------
    initCPUChart();   // CPU使用率图表
    initRAMChart();   // 内存使用率图表
    initDiskChart();  // 磁盘使用率图表

    // -------------------------- 关键修改3：初始化硬件监控与定时器 --------------------------
    // 1. 启动硬件监控（来自hGet.h）
    m_hwMonitor = new HardwareMonitor();
    if (m_hwMonitor->StartMonitoring()) {
        debugOutput("硬件监控模块启动成功");
    }
    else {
        debugOutput("[警告] 硬件监控模块启动失败！");
    }

    // 2. 启动1秒定时器（更新图表数据）
    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(1000); // 1000ms = 1秒
    connect(m_updateTimer, &QTimer::timeout, this, &KswordHUD::onUpdateTimerTimeout);
    m_updateTimer->start();

    //// 右侧顶部间隔
    //QSpacerItem* rightTopSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    //rightLayout->addSpacerItem(rightTopSpacer);
    //rightLayout->setStretch(0, 1);

    //// 右侧内容 - 占3/4高度
    //QWidget* rightContentWidget = new QWidget;
    //rightContentWidget->setStyleSheet(R"(
    //    background-color: rgba(0, 0, 0, 0.3);
    //    border-radius: 8px;
    //)");
    //rightLayout->addWidget(rightContentWidget);
    //rightLayout->setStretch(1, 3);

    // 将左右部件交给OpenGL部件管理
    m_glWidget->setChildWidgets(m_leftWidget, m_rightWidget);

    // 初始化动画
    initializeAnimations();

    // 延迟注册热键和显示窗口
    QTimer::singleShot(500, this, [this]() {
        if (!RegisterHotKey((HWND)winId(), HOTKEY_ID_CTRL, MOD_CONTROL | MOD_NOREPEAT, 0)) {
            DWORD error = GetLastError();
            debugOutput("注册热键失败，错误码:" + QString::number(error));
            if (error == 1409) {
                debugOutput("热键已被其他程序占用");
            }
        }
        else {
            debugOutput("Ctrl热键注册成功");
        }

        hide();
        m_isVisible = false;
        debugOutput("窗口初始状态: 隐藏");
        });
}

KswordHUD::~KswordHUD()
{
    UnregisterHotKey((HWND)winId(), HOTKEY_ID_CTRL);
        if (m_updateTimer) {
            m_updateTimer->stop();
            delete m_updateTimer;
        }

    // 2. 停止并释放硬件监控
    if (m_hwMonitor) {
        m_hwMonitor->StopMonitoring();
        delete m_hwMonitor;
    }

    // 3. 释放图表资源
    qDeleteAll(m_cpuBarSets);
    qDeleteAll(m_ramBarSets);
    qDeleteAll(m_diskBarSets);
    delete m_cpuChartView;
    delete m_ramChartView;
    delete m_diskChartView;
    delete m_rightTabWidget;
    debugOutput("KswordHUD 析构函数");
}

void KswordHUD::setWindowOpacity(qreal opacity)
{
    // 确保透明度在有效范围内
    m_windowOpacity = qBound(0.0, opacity, 1.0);
    debugOutput(QString("setWindowOpacity 被调用，新透明度: %1").arg(m_windowOpacity));

    // 只有在非动画状态下才立即更新
    if (!m_isAnimating) {
        update();
    }
    // 在动画状态下，update() 会在动画的valueChanged信号中调用
}
void KswordHUD::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Control) {
        debugOutput("Ctrl键按下，扫描码:" + QString::number(event->nativeScanCode()));
        toggleVisibility();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

bool KswordHUD::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY) {
        debugOutput("接收到热键消息，wParam:" + QString::number(msg->wParam) + " lParam:" + QString::number(msg->lParam));
        if (msg->wParam == HOTKEY_ID_CTRL) {
            debugOutput("全局热键触发，切换窗口显示状态");
            toggleVisibility();
            *result = 0;
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void KswordHUD::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    m_originalRect = geometry();
    debugOutput("窗口大小改变:" + QString::number(m_originalRect.x()) + "," +
        QString::number(m_originalRect.y()) + " " +
        QString::number(m_originalRect.width()) + "x" +
        QString::number(m_originalRect.height()));
}

void KswordHUD::paintEvent(QPaintEvent* event)
{
    m_paintFrameCount++;

    // 如果正在动画中且缓存内容有效，才绘制缓存内容
    if (m_isAnimating && !m_cachedContent.isNull()) {

        debugOutput(QString("paintEvent [动画帧 %1] - 透明度: %2, 缩放: %3")
            .arg(m_paintFrameCount)
            .arg(m_windowOpacity, 0, 'f', 3)
            .arg(m_contentScale, 0, 'f', 3));

        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        // 计算缩放后的尺寸和位置
        QSize scaledSize = m_cachedContent.size() * m_contentScale;
        QPoint centerPoint = rect().center() - QPoint(scaledSize.width() / 2, scaledSize.height() / 2);
        QRect drawRect(centerPoint, scaledSize);

        // 设置窗口透明度
        painter.setOpacity(m_windowOpacity);

        // 绘制缩放后的内容
        painter.drawPixmap(drawRect, m_cachedContent);
    }
    else {
        // 正常绘制 - 确保缓存已清除
        if (!m_cachedContent.isNull()) {
            clearCache(); // 额外安全检查
        }

        debugOutput(QString("paintEvent [正常帧 %1] - 透明度: %2")
            .arg(m_paintFrameCount)
            .arg(m_windowOpacity, 0, 'f', 3));
                //clearCache(); 
        QMainWindow::paintEvent(event);
    }
}
void KswordHUD::cacheWindowContent()
{
    if (m_glWidget) {
        m_cachedContent = m_glWidget->captureContent();
        debugOutput("已缓存窗口内容");
    }
}

void KswordHUD::debugOutput(const QString& message)
{
    qint64 elapsed = m_debugTimer.elapsed();
    qDebug() << QString("[%1 ms] %2").arg(elapsed, 6).arg(message);
}

void KswordHUD::initializeAnimations()
{
    debugOutput("初始化动画系统");
    // 隐藏动画：透明度从1.0→0.0（叠加后0.5→0.0）
    m_hideAnimation = new QPropertyAnimation(this, "windowOpacity");
    m_hideAnimation->setDuration(500);
    m_hideAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_hideAnimation->setStartValue(1.0);  // 初始透明度1.0（叠加后0.5）
    m_hideAnimation->setEndValue(0.0);    // 结束时完全透明

    connect(m_hideAnimation, &QPropertyAnimation::valueChanged, this, [this](const QVariant& value) {
        m_animationFrameCount++;
        qreal opacity = value.toReal();

        // 进度计算：1.0→0.0 对应 0→1 的进度
        qreal progress = 1.0 - opacity;  // 关键修改：进度 = 1 - 当前透明度
        m_contentScale = 1.0 + progress * 0.5;  // 缩放仍保持1.0→1.5

        debugOutput(QString("隐藏动画 [帧 %1] - 透明度: %2, 缩放: %3, 进度: %4")
            .arg(m_animationFrameCount)
            .arg(opacity, 0, 'f', 3)
            .arg(m_contentScale, 0, 'f', 3)
            .arg(progress, 0, 'f', 3));
        update();
        });
    // 其他代码保持不变...
    connect(m_hideAnimation, &QPropertyAnimation::finished, this, [this]() {
        hide();
        clearCache(); // 重要：清除缓存
        m_isVisible = false;
        m_isAnimating = false;
        m_glWidget->show();

        m_glWidget->update(); // 强制OpenGL部件重绘
        debugOutput("隐藏动画完成，窗口已隐藏");
        m_animationFrameCount = 0;
        });

    // 显示动画：透明度从0.0→1.0（叠加后0.0→0.5）

    m_showAnimation = new QPropertyAnimation(this, "windowOpacity");
    m_showAnimation->setDuration(500);
    m_showAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_showAnimation->setStartValue(0.0);  // 初始完全透明
    m_showAnimation->setEndValue(1.0);    // 结束时透明度1.0（叠加后0.5）
    setWindowOpacity(1.0);
    connect(m_showAnimation, &QPropertyAnimation::valueChanged, this, [this](const QVariant& value) {
        m_animationFrameCount++;
        qreal opacity = value.toReal();

        // 进度计算：0.0→1.0 对应 0→1 的进度
        qreal progress = opacity;  // 关键修改：进度 = 当前透明度
        m_contentScale = 1.5 - progress * 0.5;  // 缩放仍保持1.5→1.0

        debugOutput(QString("显示动画 [帧 %1] - 透明度: %2, 缩放: %3, 进度: %4")
            .arg(m_animationFrameCount)
            .arg(opacity, 0, 'f', 3)
            .arg(m_contentScale, 0, 'f', 3)
            .arg(progress, 0, 'f', 3));
        update();
        });
    connect(m_showAnimation, &QPropertyAnimation::finished, this, [this]() {
        m_isVisible = true;
        m_isAnimating = false;
        m_glWidget->show(); // 恢复显示OpenGL部件
        clearCache(); // 重要：清除缓存
        m_glWidget->update(); // 强制OpenGL部件重绘

        debugOutput("显示动画完成，窗口已显示");
        m_animationFrameCount = 0;
        });
    setWindowOpacity(0.5);
}

void KswordHUD::toggleVisibility()
{
    if (m_isAnimating) {
        debugOutput("动画正在进行中，忽略切换请求");
        return;
    }

    debugOutput("切换可见性，当前状态:" + QString(m_isVisible ? "显示" : "隐藏"));

    // 停止所有正在运行的动画
    if (m_hideAnimation->state() == QAbstractAnimation::Running) {
        m_hideAnimation->stop();
        debugOutput("停止正在运行的隐藏动画");
    }
    if (m_showAnimation->state() == QAbstractAnimation::Running) {
        m_showAnimation->stop();
        debugOutput("停止正在运行的显示动画");
    }

    if (m_isVisible) {
        // 隐藏窗口
        debugOutput("开始隐藏动画");

        m_isAnimating = true;
        m_animationFrameCount = 0;

        // 缓存当前窗口内容
        cacheWindowContent();

        // 隐藏OpenGL部件，只显示缓存内容
        m_glWidget->hide();

        m_hideAnimation->start();
    }
    else {
        // 显示窗口
        debugOutput("开始显示动画");

        m_isAnimating = true;
        m_animationFrameCount = 0;

        // 先显示窗口
        show();
        raise();
        activateWindow();

        // 缓存当前窗口内容
        cacheWindowContent();

        // 隐藏OpenGL部件，只显示缓存内容
        m_glWidget->hide();

        m_showAnimation->start();
    }
}
void KswordHUD::clearCache() {
    m_cachedContent = QPixmap();
    debugOutput("缓存已清除");
}

void KswordHUD::forceUpdateCache() {
    clearCache();
    if (m_glWidget) {
        m_glWidget->update(); // 强制OpenGLWidget重绘
    }
    update(); // 强制窗口重绘
    debugOutput("强制更新缓存");
}

// 1. 初始化CPU条形图（60秒历史数据，Y轴0-100%）
void KswordHUD::initCPUChart() {
    // 初始化60个数据集（对应60秒历史，初始值0）
    m_cpuBarSets.clear();
    for (int i = 0; i < HardwareMonitor::HISTORY_SIZE; ++i) {
        QBarSet* set = new QBarSet(QString("第%1秒").arg(i));
        *set << 0.0; // 初始使用率0%
        set->setBrush(QColor(255, 100, 100)); // 柱子颜色：浅红色
        set->setLabelBrush(QColor(255, 255, 255)); // 标签白色
        m_cpuBarSets.append(set);
    }

    // 创建条形图系列（整合60个数据集）
    m_cpuSeries = new QBarSeries();
    m_cpuSeries->append(m_cpuBarSets);
    m_cpuSeries->setName("CPU使用率(%)");

    // 创建图表核心对象
    QChart* cpuChart = new QChart();
    cpuChart->addSeries(m_cpuSeries);
    cpuChart->setTitle("CPU使用率历史（最近60秒）");
    cpuChart->setBackgroundBrush(QColor(0, 0, 0, 0)); // 透明背景
    cpuChart->setTitleBrush(QColor(255, 255, 255)); // 标题白色
    cpuChart->legend()->setLabelBrush(QColor(255, 255, 255)); // 图例白色

    // X轴：时间（0-59秒）
    m_cpuAxisX = new QValueAxis();
    m_cpuAxisX->setRange(0, HardwareMonitor::HISTORY_SIZE - 1);
    m_cpuAxisX->setLabelFormat("%d");
    m_cpuAxisX->setTitleText("时间（秒）");
    m_cpuAxisX->setTitleBrush(QColor(255, 255, 255));
    m_cpuAxisX->setLabelsBrush(QColor(255, 255, 255));

    // Y轴：使用率（0-100%，固定范围避免波动）
    m_cpuAxisY = new QValueAxis();
    m_cpuAxisY->setRange(0, 100);
    m_cpuAxisY->setLabelFormat("%.0f%%");
    m_cpuAxisY->setTitleText("使用率");
    m_cpuAxisY->setTitleBrush(QColor(255, 255, 255));
    m_cpuAxisY->setLabelsBrush(QColor(255, 255, 255));

    // 绑定坐标轴到图表
    cpuChart->addAxis(m_cpuAxisX, Qt::AlignBottom);
    cpuChart->addAxis(m_cpuAxisY, Qt::AlignLeft);
    m_cpuSeries->attachAxis(m_cpuAxisX);
    m_cpuSeries->attachAxis(m_cpuAxisY);

    // 创建图表视图（用于显示）
    m_cpuChartView = new QChartView(cpuChart);
    m_cpuChartView->setRenderHint(QPainter::Antialiasing); // 抗锯齿
    m_cpuChartView->setStyleSheet("background-color: transparent;"); // 透明背景

    // 添加到Tab页
    m_rightTabWidget->addTab(m_cpuChartView, "CPU");
}

// 2. 初始化RAM条形图（逻辑与CPU一致，仅数据字段和颜色不同）
void KswordHUD::initRAMChart() {
    m_ramBarSets.clear();
    for (int i = 0; i < HardwareMonitor::HISTORY_SIZE; ++i) {
        QBarSet* set = new QBarSet(QString("第%1秒").arg(i));
        *set << 0.0;
        set->setBrush(QColor(100, 255, 100)); // 柱子颜色：浅绿色
        set->setLabelBrush(QColor(255, 255, 255));
        m_ramBarSets.append(set);
    }

    m_ramSeries = new QBarSeries();
    m_ramSeries->append(m_ramBarSets);
    m_ramSeries->setName("内存使用率(%)");

    QChart* ramChart = new QChart();
    ramChart->addSeries(m_ramSeries);
    ramChart->setTitle("内存使用率历史（最近60秒）");
    ramChart->setBackgroundBrush(QColor(0, 0, 0, 0));
    ramChart->setTitleBrush(QColor(255, 255, 255));
    ramChart->legend()->setLabelBrush(QColor(255, 255, 255));

    m_ramAxisX = new QValueAxis();
    m_ramAxisX->setRange(0, HardwareMonitor::HISTORY_SIZE - 1);
    m_ramAxisX->setLabelFormat("%d");
    m_ramAxisX->setTitleText("时间（秒）");
    m_ramAxisX->setTitleBrush(QColor(255, 255, 255));
    m_ramAxisX->setLabelsBrush(QColor(255, 255, 255));

    m_ramAxisY = new QValueAxis();
    m_ramAxisY->setRange(0, 100);
    m_ramAxisY->setLabelFormat("%.0f%%");
    m_ramAxisY->setTitleText("使用率");
    m_ramAxisY->setTitleBrush(QColor(255, 255, 255));
    m_ramAxisY->setLabelsBrush(QColor(255, 255, 255));

    ramChart->addAxis(m_ramAxisX, Qt::AlignBottom);
    ramChart->addAxis(m_ramAxisY, Qt::AlignLeft);
    m_ramSeries->attachAxis(m_ramAxisX);
    m_ramSeries->attachAxis(m_ramAxisY);

    m_ramChartView = new QChartView(ramChart);
    m_ramChartView->setRenderHint(QPainter::Antialiasing);
    m_ramChartView->setStyleSheet("background-color: transparent;");

    m_rightTabWidget->addTab(m_ramChartView, "RAM");
}

// 3. 初始化Disk条形图（逻辑与CPU一致，仅数据字段和颜色不同）
void KswordHUD::initDiskChart() {
    m_diskBarSets.clear();
    for (int i = 0; i < HardwareMonitor::HISTORY_SIZE; ++i) {
        QBarSet* set = new QBarSet(QString("第%1秒").arg(i));
        *set << 0.0;
        set->setBrush(QColor(100, 100, 255)); // 柱子颜色：浅蓝色
        set->setLabelBrush(QColor(255, 255, 255));
        m_diskBarSets.append(set);
    }

    m_diskSeries = new QBarSeries();
    m_diskSeries->append(m_diskBarSets);
    m_diskSeries->setName("磁盘使用率(%)");

    QChart* diskChart = new QChart();
    diskChart->addSeries(m_diskSeries);
    diskChart->setTitle("磁盘使用率历史（最近60秒）");
    diskChart->setBackgroundBrush(QColor(0, 0, 0, 0));
    diskChart->setTitleBrush(QColor(255, 255, 255));
    diskChart->legend()->setLabelBrush(QColor(255, 255, 255));

    m_diskAxisX = new QValueAxis();
    m_diskAxisX->setRange(0, HardwareMonitor::HISTORY_SIZE - 1);
    m_diskAxisX->setLabelFormat("%d");
    m_diskAxisX->setTitleText("时间（秒）");
    m_diskAxisX->setTitleBrush(QColor(255, 255, 255));
    m_diskAxisX->setLabelsBrush(QColor(255, 255, 255));

    m_diskAxisY = new QValueAxis();
    m_diskAxisY->setRange(0, 100);
    m_diskAxisY->setLabelFormat("%.0f%%");
    m_diskAxisY->setTitleText("使用率");
    m_diskAxisY->setTitleBrush(QColor(255, 255, 255));
    m_diskAxisY->setLabelsBrush(QColor(255, 255, 255));

    diskChart->addAxis(m_diskAxisX, Qt::AlignBottom);
    diskChart->addAxis(m_diskAxisY, Qt::AlignLeft);
    m_diskSeries->attachAxis(m_diskAxisX);
    m_diskSeries->attachAxis(m_diskAxisY);

    m_diskChartView = new QChartView(diskChart);
    m_diskChartView->setRenderHint(QPainter::Antialiasing);
    m_diskChartView->setStyleSheet("background-color: transparent;");

    m_rightTabWidget->addTab(m_diskChartView, "Disk");
}

void KswordHUD::onUpdateTimerTimeout() {
    if (!m_hwMonitor) return;

    // -------------------------- 关键：获取硬件历史数据（线程安全） --------------------------
    // 注意：需先修改hGet.h的GetHistory()函数，添加互斥锁（见下文补充）
    std::array<HardwareSnapshot, HardwareMonitor::HISTORY_SIZE> hwHistory = m_hwMonitor->GetHistory();

    // -------------------------- 1. 更新CPU图表数据 --------------------------
    for (int i = 0; i < HardwareMonitor::HISTORY_SIZE; ++i) {
        double cpuUsage = hwHistory[i].cpuUsage;
        cpuUsage = qBound(0.0, cpuUsage, 100.0); // 确保数值在0-100之间
        m_cpuBarSets[i]->replace(0, cpuUsage);   // 更新第i秒的使用率
    }

    // -------------------------- 2. 更新RAM图表数据 --------------------------
    for (int i = 0; i < HardwareMonitor::HISTORY_SIZE; ++i) {
        double ramUsage = hwHistory[i].memoryUsagePercent;
        ramUsage = qBound(0.0, ramUsage, 100.0);
        m_ramBarSets[i]->replace(0, ramUsage);
    }

    // -------------------------- 3. 更新Disk图表数据 --------------------------
    for (int i = 0; i < HardwareMonitor::HISTORY_SIZE; ++i) {
        double diskUsage = hwHistory[i].diskUsagePercent;
        diskUsage = qBound(0.0, diskUsage, 100.0);
        m_diskBarSets[i]->replace(0, diskUsage);
    }

    // 强制刷新图表（确保UI即时更新）
    m_cpuChartView->chart()->update();
    m_ramChartView->chart()->update();
    m_diskChartView->chart()->update();

    debugOutput("硬件数据已更新，图表刷新完成");
}