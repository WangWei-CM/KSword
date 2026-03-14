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
    connect(m_animationTimer, &QTimer::timeout, this, &SpectrumWidget::updateSpectrumDisplay);

    // 默认60Hz刷新率
    startAnimation(60);
}

SpectrumWidget::~SpectrumWidget()
{
    stopAnimation();
}

void SpectrumWidget::setupUI()
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(10, 10, 10, 10);
    m_layout->setSpacing(m_barSpacing);
    m_layout->setAlignment(Qt::AlignBottom);

    // 设置组件样式
    setStyleSheet("background-color: #1a1a1a;"); // 深色背景
    setMinimumHeight(250);
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

void SpectrumWidget::setSpectrumData(const QVector<float>& data)
{
    qDebug() << "SpectrumWidget::setSpectrumData called on" << this << "size=" << data.size();
    if (data.size() >= NUM_BARS) {
        m_targetSpectrum = data;

        // 限制数据范围在0-1之间
        for (int i = 0; i < NUM_BARS; ++i) {
            m_targetSpectrum[i] = qBound(0.0f, m_targetSpectrum[i], 1.0f);
        }
    }
}

void SpectrumWidget::updateSpectrumDisplay()
{
    qDebug() << "updateSpectrumDisplay on" << this;
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

    update();
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

    qDebug() << "SpectrumWidget::startAnimation on" << this << "fps=" << fps;
    m_animationTimer->start(m_updateInterval);
    qDebug() << "Animation timer active:" << m_animationTimer->isActive();
}

void SpectrumWidget::stopAnimation()
{
    if (m_animationTimer->isActive()) {
        m_animationTimer->stop();
    }
}
// 重写paintEvent，一次性绘制所有频谱条
void SpectrumWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing); // 按需开启抗锯齿

    // 计算绘制区域（减去边距）
    const int margin = 10;
    const int totalWidth = width() - 2 * margin;
    const int totalHeight = height() - 2 * margin;
    m_maxBarHeight = totalHeight; // 缓存最大高度

    // 计算每个条的宽度和间距（避免布局管理器开销）
    const int availableWidth = totalWidth - (NUM_BARS - 1) * m_barSpacing;
    const int barWidth = qMax(1, availableWidth / NUM_BARS); // 确保至少1px

    // 预缓存渐变（避免每次绘制创建）
    if (m_gradient.stops().isEmpty()) {
        m_gradient = QLinearGradient(0, 0, 0, totalHeight);
        m_gradient.setColorAt(0, QColor(0, 255, 255));
        m_gradient.setColorAt(0.3, QColor(0, 204, 255));
        m_gradient.setColorAt(0.7, QColor(0, 153, 255));
        m_gradient.setColorAt(1, QColor(0, 102, 255));
    }

    // 绘制所有频谱条
    painter.translate(margin, margin + totalHeight); // 原点移至底部
    for (int i = 0; i < NUM_BARS; ++i) {
        const float value = m_currentSpectrum[i];
        if (value <= 0.01f) continue; // 跳过极小值

        // 计算高度（非线性映射）
        const float normalized = std::sqrt(value);
        const int barHeight = static_cast<int>(normalized * totalHeight);

        // 绘制位置（x偏移：条宽+间距的累积）
        const int x = i * (barWidth + m_barSpacing);
        // 绘制矩形（y轴向下为正，所以高度取负值）
        painter.fillRect(x, -barHeight, barWidth, barHeight, m_gradient);
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