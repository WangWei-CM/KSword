#include "SpectrumWidget.h"
#include <QPainter>
#include <QLinearGradient>
#include <QDebug>

SpectrumWidget::SpectrumWidget(QWidget* parent)
    : QWidget(parent)
    , m_currentSpectrum(NUM_BARS, 0.0f)
    , m_targetSpectrum(NUM_BARS, 0.0f)
{
    setupUI();
    createBars();

    m_animationTimer = new QTimer(this);
    m_animationTimer->setTimerType(Qt::PreciseTimer); // 高精度定时器
    connect(m_animationTimer, &QTimer::timeout, this, &SpectrumWidget::updateSpectrumDisplay);

    startAnimation(30);
}

SpectrumWidget::~SpectrumWidget()
{
    stopAnimation();
}

void SpectrumWidget::setupUI()
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(2,2,2,2);
    m_layout->setSpacing(m_barSpacing);
    m_layout->setAlignment(Qt::AlignBottom);

    // 设置组件样式
    setStyleSheet("background-color: #1a1a1a;"); // 深色背景
   
}

void SpectrumWidget::createBars()
{
    for (int i = 0; i < NUM_BARS; ++i) {
        QLabel* bar = new QLabel(this);

        // 设置初始样式
        bar->setFixedWidth(m_barWidth);
        bar->setMinimumHeight(1);
        bar->setStyleSheet(QString(
            "background: qlineargradient("
            "spread: pad, x1:0.5, y1:0, x1:0.5, y2:1,"
            "stop:0 #00ffff, stop:0.3 #00ccff, stop:0.7 #0099ff, stop:1 #0066ff);"
        ));

        bar->setAlignment(Qt::AlignBottom);
        m_bars.append(bar);
        m_layout->addWidget(bar);

        // 强制布局项垂直贴底
        QLayoutItem* barItem = m_layout->itemAt(i);
        if (barItem) {
            barItem->setAlignment(Qt::AlignBottom);
        }
    }
}

void SpectrumWidget::setSpectrumData(const QVector<float>& data) {
    if (data.size() >= NUM_BARS) {
        m_targetSpectrum = data;

        // 调试输出
        qDebug() << "Spectrum data received - Max value:" << *std::max_element(data.begin(), data.end());

        for (int i = 0; i < NUM_BARS; ++i) {
            m_targetSpectrum[i] = qBound(0.0f, m_targetSpectrum[i], 1.0f);
            if (i < 3) { // 只打印前3个值用于调试
                qDebug() << "Band" << i << ":" << m_targetSpectrum[i];
            }
        }
    }
    else {
        qWarning() << "Invalid spectrum data size:" << data.size() << ", expected:" << NUM_BARS;
    }
}

void SpectrumWidget::updateSpectrumDisplay()
{
    // 应用平滑过渡
    for (int i = 0; i < NUM_BARS; ++i) {
        float current = m_currentSpectrum[i];
        float target = m_targetSpectrum[i];

        // 指数平滑：current + factor * (target - current)
        m_currentSpectrum[i] = current + m_smoothingFactor * (target - current);

        // 避免过小的值导致闪烁
        if (m_currentSpectrum[i] < 0.01f) {
            m_currentSpectrum[i] = 0.0f;
        }
    }

    updateBarHeights();
}

void SpectrumWidget::updateBarHeights()
{
    for (int i = 0; i < NUM_BARS; ++i) {
        QLabel* bar = m_bars[i];
        float spectrumValue = m_currentSpectrum[i];

        // 应用非线性映射（平方根）使小值更明显
        float normalizedValue = std::sqrt(spectrumValue);

        int barHeight = static_cast<int>(normalizedValue * m_maxBarHeight);
        barHeight = qMax(2, barHeight); // 最小高度为2像素

        bar->setFixedHeight(barHeight);

        // 动态颜色效果：根据高度改变颜色强度
        if (barHeight > 0) {
            int intensity = qMin(255, 100 + static_cast<int>(normalizedValue * 155));
            QString style = QString(
                "background: qlineargradient("
                "spread: pad, x1:0.5, y1:0, x1:0.5, y2:1,"
                "stop:0 rgba(%1, 255, 255, 255), "
                "stop:0.3 rgba(%1, 204, 255, 255), "
                "stop:0.7 rgba(%1, 153, 255, 255), "
                "stop:1 rgba(%1, 102, 255, 255));"
            ).arg(intensity);

            bar->setStyleSheet(style);
        }
    }

    // 强制重绘
    update();
}

void SpectrumWidget::startAnimation(int fps)
{
    if (fps < 24) fps = 24; // 确保不低于24Hz
    if (fps > 120) fps = 120; // 限制最大刷新率

    m_updateInterval = 1000 / fps;
    stopAnimation(); // 先停止现有定时器

    m_animationTimer->start(m_updateInterval);
    qDebug() << "Spectrum animation started at" << fps << "Hz";
}

void SpectrumWidget::stopAnimation()
{
    if (m_animationTimer->isActive()) {
        m_animationTimer->stop();
    }
}

// 可选：添加绘制事件以增强视觉效果
//void SpectrumWidget::paintEvent(QPaintEvent* event)
//{
//    QWidget::paintEvent(event);
//
//    // 添加一些装饰性绘制
//    QPainter painter(this);
//    painter.setRenderHint(QPainter::Antialiasing);
//
//    // 绘制底部渐变遮罩
//    QLinearGradient gradient(0, height() - 30, 0, height());
//    gradient.setColorAt(0, QColor(0, 0, 0, 100));
//    gradient.setColorAt(1, QColor(0, 0, 0, 200));
//
//    painter.fillRect(0, height() - 30, width(), 30, gradient);
//}