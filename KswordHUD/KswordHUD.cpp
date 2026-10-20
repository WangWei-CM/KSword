//#include "stdafx.h"

#include <windows.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QOpenGLPaintDevice>
#include <QPainter>

#include <QEasingCurve>
#include <QAbstractAnimation>
#include <QScreen>
#include <QTimer>
#include <QSurfaceFormat>

#include <algorithm>
// 硬件监控头文件
#include "hGet.h"
#include "HudProcessListPanel.h"
#include "HudPerformancePanel.h"
#include "KswordHUD.h"
#include "HudColors.h"
// 命名空间（简化代码）

namespace
{
    constexpr int kHudOuterMargin = 36;
    constexpr int kHudCenterGap = 24;
    constexpr int kDefaultLeftWidgetBackgroundOpacityPercent = 38;
    constexpr int kDefaultRightWidgetBackgroundOpacityPercent = 45;

    QColor parseConfigColor(const QString& colorText, const QColor& fallbackColor)
    {
        const QColor parsedColor(colorText.trimmed());
        return parsedColor.isValid() ? parsedColor : fallbackColor;
    }

    QString buildWidgetBackgroundStyle(const QColor& color, const int opacityPercent)
    {
        const int alphaValue = qBound(0, opacityPercent, 100) * 255 / 100;
        return QStringLiteral("background-color: rgba(%1, %2, %3, %4);")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(alphaValue);
    }
}

HHOOK KswordHUD::s_keyboardHook = nullptr;
KswordHUD* KswordHUD::s_instance = nullptr;
bool KswordHUD::s_rightCtrlDown = false;

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
    emit backdropChanged();
    update();
}

void OpenGLWidget::setOpacity(qreal opacity) {
    m_opacity = opacity;
    emit backdropChanged();
    update();
}

QColor OpenGLWidget::effectivePanelBackground(
    const QWidget* panel, const QColor& fillColor, const int opacityPercent) const
{
    // 与 paintGL 共用 KeepAspectRatioByExpanding、居中和图片 alpha；这里只做颜色采样。
    const QImage image = m_backgroundPixmap.toImage();
    const QRect area = panel != nullptr ? panel->geometry() : rect();
    const double scale = image.isNull() ? 1.0 : qMax(
        static_cast<double>(width()) / qMax(1, image.width()),
        static_cast<double>(height()) / qMax(1, image.height()));
    const double offsetX = (width() - image.width() * scale) * 0.5;
    const double offsetY = (height() - image.height() * scale) * 0.5;
    QColor fill = fillColor;
    fill.setAlpha(qBound(0, opacityPercent, 100) * 255 / 100);
    int red = 0;
    int green = 0;
    int blue = 0;
    constexpr int samples = 7;
    for (int row = 0; row < samples; ++row)
    {
        for (int column = 0; column < samples; ++column)
        {
            QColor pixel(0, 0, 0, 128);
            if (!image.isNull())
            {
                const double x = area.x() + area.width() * (column + 0.5) / samples;
                const double y = area.y() + area.height() * (row + 0.5) / samples;
                const int imageX = qBound(0, qRound((x - offsetX) / qMax(0.001, scale)), image.width() - 1);
                const int imageY = qBound(0, qRound((y - offsetY) / qMax(0.001, scale)), image.height() - 1);
                pixel = image.pixelColor(imageX, imageY);
            }
            pixel.setAlpha(qRound(pixel.alpha() * qBound(0.0, static_cast<double>(m_opacity), 1.0)));
            // 外部桌面不在 HUD 图片快照中，以合成层的透明黑为参考；不宣称任意桌面保证。
            const QColor background = KswordHudColors::Composite(pixel, QColor(Qt::black));
            const QColor visible = KswordHudColors::Composite(fill, background);
            red += visible.red();
            green += visible.green();
            blue += visible.blue();
        }
    }
    return QColor(red / (samples * samples), green / (samples * samples), blue / (samples * samples));
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

void OpenGLWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0, 0, 0, 0);
}

void OpenGLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    if (m_leftWidget && m_rightWidget) {
        const int contentWidth = std::max(0, w - 2 * kHudOuterMargin - kHudCenterGap);
        const int paneWidth = contentWidth / 2;
        const int paneHeight = std::max(0, h - 2 * kHudOuterMargin);
        const int leftX = kHudOuterMargin;
        const int rightX = leftX + paneWidth + kHudCenterGap;
        const int topY = kHudOuterMargin;
        m_leftWidget->setGeometry(leftX, topY, paneWidth, paneHeight);
        m_rightWidget->setGeometry(rightX, topY, contentWidth - paneWidth, paneHeight);
        emit backdropChanged();
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
    , m_windowOpacity(1.0)
    , m_isVisible(false)
    , m_isAnimating(false)
    , m_pendingToggleRequest(false)
    , m_leftWidget(nullptr)
    , m_rightWidget(nullptr)
    , m_tableWidget(nullptr)
    , m_hideAnimation(nullptr)
    , m_showAnimation(nullptr)
    , m_glWidget(nullptr)
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
    connect(m_glWidget, &OpenGLWidget::backdropChanged, this, [this]() { refreshPaneColors(); });
    setCentralWidget(m_glWidget);

    const HudConfig config = loadOrCreateConfig();

    // 左侧容器（50%宽度）
    m_leftWidget = new QWidget(m_glWidget);
    m_leftWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_leftWidget->setStyleSheet("background-color: transparent;");
    QVBoxLayout* leftLayout = new QVBoxLayout(m_leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    // 左侧挂平铺进程资源列表，先不做树状分组。
    m_processListPanel = new HudProcessListPanel(m_leftWidget);
    leftLayout->addWidget(m_processListPanel, 1);

    // 右侧容器（50%宽度，透明背景）
    m_rightWidget = new QWidget(m_glWidget);
    m_rightWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_rightWidget->setStyleSheet("background-color: transparent;");
    QVBoxLayout* rightLayout = new QVBoxLayout(m_rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // 右侧直接挂完整性能页，不再套外层Tab。
    m_performancePanel = new HudPerformancePanel(m_rightWidget);
    rightLayout->addWidget(m_performancePanel, 1);

    // 将左右部件交给OpenGL部件管理
    m_glWidget->setChildWidgets(m_leftWidget, m_rightWidget);
    applyHudConfig(config);

    // 初始化动画
    initializeAnimations();

    s_instance = this;
    if (s_keyboardHook == nullptr) {
        s_keyboardHook = SetWindowsHookExW(
            WH_KEYBOARD_LL,
            &KswordHUD::LowLevelKeyboardProc,
            GetModuleHandleW(nullptr),
            0);
        debugOutput(s_keyboardHook != nullptr
            ? QStringLiteral("右Ctrl键盘钩子安装成功")
            : QStringLiteral("右Ctrl键盘钩子安装失败"));
    }

    QTimer::singleShot(0, this, [this]() {
        hide();
        m_isVisible = false;
        debugOutput("窗口初始状态: 隐藏");
        });
}

KswordHUD::~KswordHUD()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (s_keyboardHook != nullptr) {
        UnhookWindowsHookEx(s_keyboardHook);
        s_keyboardHook = nullptr;
        s_rightCtrlDown = false;
    }
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
    debugOutput("KswordHUD 析构函数");
}

void KswordHUD::setWindowOpacity(qreal opacity)
{
    const qreal value = qBound(0.0, opacity, 1.0);
    if (qFuzzyCompare(m_windowOpacity, value)) return;
    m_windowOpacity = value;
    QMainWindow::setWindowOpacity(value);
}

KswordHUD::HudConfig KswordHUD::loadOrCreateConfig() const
{
    const QString styleDirPath = QCoreApplication::applicationDirPath() + "/Style";
    QDir styleDir(styleDirPath);
    if (!styleDir.exists()) {
        styleDir.mkpath(QStringLiteral("."));
    }

    const QString configPath = styleDir.filePath(QStringLiteral("KswordHudConfig.json"));
    QJsonObject configObject;
    bool shouldWriteConfig = false;

    QFile configFile(configPath);
    if (configFile.exists() && configFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument configDocument = QJsonDocument::fromJson(configFile.readAll());
        if (configDocument.isObject()) {
            configObject = configDocument.object();
        }
        else {
            shouldWriteConfig = true;
        }
        configFile.close();
    }
    else {
        shouldWriteConfig = true;
    }

    if (!configObject.contains(QStringLiteral("backgroundImagePath"))) {
        configObject.insert(QStringLiteral("backgroundImagePath"), QStringLiteral("HUD_background.png"));
        shouldWriteConfig = true;
    }
    if (!configObject.contains(QStringLiteral("leftWidgetOpacityPercent"))) {
        configObject.insert(QStringLiteral("leftWidgetOpacityPercent"), 100);
        shouldWriteConfig = true;
    }
    if (!configObject.contains(QStringLiteral("rightWidgetOpacityPercent"))) {
        configObject.insert(QStringLiteral("rightWidgetOpacityPercent"), 100);
        shouldWriteConfig = true;
    }
    if (!configObject.contains(QStringLiteral("leftWidgetBackgroundColor"))) {
        configObject.insert(QStringLiteral("leftWidgetBackgroundColor"), QStringLiteral("#0A0F16"));
        shouldWriteConfig = true;
    }
    if (!configObject.contains(QStringLiteral("leftWidgetBackgroundOpacityPercent"))) {
        configObject.insert(QStringLiteral("leftWidgetBackgroundOpacityPercent"), kDefaultLeftWidgetBackgroundOpacityPercent);
        shouldWriteConfig = true;
    }
    if (!configObject.contains(QStringLiteral("leftProcessTableFontColor"))) {
        configObject.insert(QStringLiteral("leftProcessTableFontColor"), QStringLiteral("#FFFFFF"));
        shouldWriteConfig = true;
    }
    if (!configObject.contains(QStringLiteral("rightWidgetBackgroundColor"))) {
        configObject.insert(QStringLiteral("rightWidgetBackgroundColor"), QStringLiteral("#0A0F16"));
        shouldWriteConfig = true;
    }
    if (!configObject.contains(QStringLiteral("rightWidgetBackgroundOpacityPercent"))) {
        configObject.insert(QStringLiteral("rightWidgetBackgroundOpacityPercent"), kDefaultRightWidgetBackgroundOpacityPercent);
        shouldWriteConfig = true;
    }
    if (shouldWriteConfig && configFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        configFile.write(QJsonDocument(configObject).toJson(QJsonDocument::Indented));
        configFile.close();
    }

    HudConfig config;
    config.backgroundImagePath = configObject.value(QStringLiteral("backgroundImagePath")).toString(QStringLiteral("HUD_background.png"));
    config.leftWidgetOpacityPercent = qBound(
        0,
        configObject.value(QStringLiteral("leftWidgetOpacityPercent")).toInt(100),
        100);
    config.rightWidgetOpacityPercent = qBound(
        0,
        configObject.value(QStringLiteral("rightWidgetOpacityPercent")).toInt(100),
        100);
    config.leftWidgetBackgroundColor =
        configObject.value(QStringLiteral("leftWidgetBackgroundColor")).toString(QStringLiteral("#0A0F16"));
    config.leftWidgetBackgroundOpacityPercent = qBound(
        0,
        configObject.value(QStringLiteral("leftWidgetBackgroundOpacityPercent")).toInt(kDefaultLeftWidgetBackgroundOpacityPercent),
        100);
    config.leftProcessTableFontColor =
        configObject.value(QStringLiteral("leftProcessTableFontColor")).toString(QStringLiteral("#FFFFFF"));
    config.rightWidgetBackgroundColor =
        configObject.value(QStringLiteral("rightWidgetBackgroundColor")).toString(QStringLiteral("#0A0F16"));
    config.rightWidgetBackgroundOpacityPercent = qBound(
        0,
        configObject.value(QStringLiteral("rightWidgetBackgroundOpacityPercent")).toInt(kDefaultRightWidgetBackgroundOpacityPercent),
        100);
    return config;
}

void KswordHUD::applyHudConfig(const HudConfig& config)
{
    m_hudConfig = config;
    const QString styleDirPath = QCoreApplication::applicationDirPath() + "/Style";
    QString backgroundPath = config.backgroundImagePath.trimmed();
    if (backgroundPath.isEmpty()) {
        backgroundPath = QStringLiteral("HUD_background.png");
    }
    if (QDir::isRelativePath(backgroundPath)) {
        backgroundPath = QDir(styleDirPath).filePath(backgroundPath);
    }

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

    if (m_leftOpacityEffect == nullptr && m_leftWidget != nullptr) {
        m_leftOpacityEffect = new QGraphicsOpacityEffect(m_leftWidget);
        m_leftWidget->setGraphicsEffect(m_leftOpacityEffect);
    }
    if (m_rightOpacityEffect == nullptr && m_rightWidget != nullptr) {
        m_rightOpacityEffect = new QGraphicsOpacityEffect(m_rightWidget);
        m_rightWidget->setGraphicsEffect(m_rightOpacityEffect);
    }

    if (m_leftOpacityEffect != nullptr) {
        m_leftOpacityEffect->setOpacity(static_cast<qreal>(config.leftWidgetOpacityPercent) / 100.0);
    }
    if (m_rightOpacityEffect != nullptr) {
        m_rightOpacityEffect->setOpacity(static_cast<qreal>(config.rightWidgetOpacityPercent) / 100.0);
    }

    const QColor defaultWidgetColor(0, 0, 0);
    const QColor leftBackgroundColor =
        parseConfigColor(config.leftWidgetBackgroundColor, defaultWidgetColor);
    const QColor leftProcessTableFontColor =
        parseConfigColor(config.leftProcessTableFontColor, QColor(255, 255, 255));
    const QColor rightBackgroundColor =
        parseConfigColor(config.rightWidgetBackgroundColor, defaultWidgetColor);
    if (m_leftWidget != nullptr) {
        m_leftWidget->setStyleSheet(buildWidgetBackgroundStyle(
            leftBackgroundColor,
            config.leftWidgetBackgroundOpacityPercent > 0
            ? config.leftWidgetBackgroundOpacityPercent
            : kDefaultLeftWidgetBackgroundOpacityPercent));
    }
    if (m_rightWidget != nullptr) {
        m_rightWidget->setStyleSheet(buildWidgetBackgroundStyle(
            rightBackgroundColor,
            config.rightWidgetBackgroundOpacityPercent > 0
            ? config.rightWidgetBackgroundOpacityPercent
            : kDefaultRightWidgetBackgroundOpacityPercent));
    }
    if (m_processListPanel != nullptr) {
        m_processListPanel->setTableTextColor(leftProcessTableFontColor);
    }
    refreshPaneColors();
}

void KswordHUD::refreshPaneColors()
{
    if (m_glWidget == nullptr)
    {
        return;
    }
    // 保留现有 0 值回退语义与所有背景配置，仅给前景模块提供实际配置合成后的参考底。
    const QColor left = parseConfigColor(m_hudConfig.leftWidgetBackgroundColor, QColor(Qt::black));
    const QColor right = parseConfigColor(m_hudConfig.rightWidgetBackgroundColor, QColor(Qt::black));
    const int leftOpacity = m_hudConfig.leftWidgetBackgroundOpacityPercent > 0
        ? m_hudConfig.leftWidgetBackgroundOpacityPercent : kDefaultLeftWidgetBackgroundOpacityPercent;
    const int rightOpacity = m_hudConfig.rightWidgetBackgroundOpacityPercent > 0
        ? m_hudConfig.rightWidgetBackgroundOpacityPercent : kDefaultRightWidgetBackgroundOpacityPercent;
    if (m_processListPanel != nullptr)
    {
        m_processListPanel->setEffectiveBackgroundColor(m_glWidget->effectivePanelBackground(m_leftWidget, left, leftOpacity));
    }
    if (m_performancePanel != nullptr)
    {
        m_performancePanel->setEffectiveBackgroundColor(m_glWidget->effectivePanelBackground(m_rightWidget, right, rightOpacity));
    }
}

bool KswordHUD::IsRightCtrlEvent(const WPARAM wParam, const KBDLLHOOKSTRUCT* keyboardInfo)
{
    if (keyboardInfo == nullptr) {
        return false;
    }

    const bool isKeyTransition =
        wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN || wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    if (!isKeyTransition) {
        return false;
    }

    if (keyboardInfo->vkCode == VK_RCONTROL) {
        return true;
    }

    return keyboardInfo->vkCode == VK_CONTROL
        && (keyboardInfo->flags & LLKHF_EXTENDED) != 0;
}

LRESULT CALLBACK KswordHUD::LowLevelKeyboardProc(const int nCode, const WPARAM wParam, const LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        const auto* keyboardInfo = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (IsRightCtrlEvent(wParam, keyboardInfo)) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (!s_rightCtrlDown && s_instance != nullptr) {
                    s_rightCtrlDown = true;
                    QMetaObject::invokeMethod(s_instance, [instance = s_instance]() {
                        if (instance != nullptr) {
                            instance->toggleVisibility();
                        }
                    }, Qt::QueuedConnection);
                }
            }
            else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                s_rightCtrlDown = false;
            }
        }
    }

    return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);
}

void KswordHUD::keyPressEvent(QKeyEvent* event)
{
    QMainWindow::keyPressEvent(event);
}

bool KswordHUD::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
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

void KswordHUD::debugOutput(const QString& message)
{
    if (!qEnvironmentVariableIsSet("KSWORDHUD_DEBUG")) {
        return;
    }
    qint64 elapsed = m_debugTimer.elapsed();
    qDebug() << QString("[%1 ms] %2").arg(elapsed, 6).arg(message);
}

void KswordHUD::initializeAnimations()
{
    BOOL animate = TRUE;
    ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    const int duration = animate ? 500 : 0;
    m_hideAnimation = new QPropertyAnimation(this, "windowOpacity", this);
    m_hideAnimation->setDuration(duration);
    m_hideAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_hideAnimation->setStartValue(1.0);
    m_hideAnimation->setEndValue(0.0);
    m_showAnimation = new QPropertyAnimation(this, "windowOpacity", this);
    m_showAnimation->setDuration(duration);
    m_showAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_showAnimation->setStartValue(0.0);
    m_showAnimation->setEndValue(1.0);
    connect(m_hideAnimation, &QPropertyAnimation::finished, this, [this]() {
        hide();
        m_isVisible = false;
        m_isAnimating = false;
        setWindowOpacity(1.0);
        if (m_pendingToggleRequest) {
            m_pendingToggleRequest = false;
            QTimer::singleShot(0, this, [this]() { toggleVisibility(); });
        }
    });
    connect(m_showAnimation, &QPropertyAnimation::finished, this, [this]() {
        m_isVisible = true;
        m_isAnimating = false;
        if (m_pendingToggleRequest) {
            m_pendingToggleRequest = false;
            QTimer::singleShot(0, this, [this]() { toggleVisibility(); });
        }
    });
}

void KswordHUD::toggleVisibility()
{
    if (m_isAnimating) {
        // Keep the parity of additional key presses without recapturing any content.
        m_pendingToggleRequest = !m_pendingToggleRequest;
        return;
    }
    m_pendingToggleRequest = false;
    m_isAnimating = true;
    if (m_isVisible) {
        // The system compositor fades the current surface; no child grab or CPU image scaling.
        m_hideAnimation->start();
    } else {
        setWindowOpacity(0.0);
        show();
        raise();
        activateWindow();
        m_showAnimation->start();
    }
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
