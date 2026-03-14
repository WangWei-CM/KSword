#include "SpectrumWidget.h"
#include <QPainter>
#include <QDebug>
#include <cmath>

SpectrumWidget::SpectrumWidget(Direction direction, QWidget* parent)
    : QWidget(parent)
    , m_direction(direction)
    , m_spectrumData(NUM_BARS, 0.0f)
{
    setupUI();
    m_paintTimer.start();
}

SpectrumWidget::~SpectrumWidget()
{
}

void SpectrumWidget::setupUI()
{
    // 设置背景透明
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("background: transparent;");

    // 设置尺寸策略
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(150);
}

void SpectrumWidget::setSpectrumData(const QVector<float>& data)
{
    if (data.size() >= NUM_BARS) {
        // 限制数据范围在0-1之间
        for (int i = 0; i < NUM_BARS; ++i) {
            m_spectrumData[i] = qBound(0.0f, data[i], 1.0f);
        }

        // 限制重绘频率，避免过度绘制
        if (m_paintTimer.hasExpired(m_minPaintInterval)) {
            update();
            m_paintTimer.restart();
        }
    }
}

void SpectrumWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // 不再需要复杂的绘制区域计算
}
void SpectrumWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // 实际可用的最大高度
    int maxAvailableHeight = static_cast<int>(height() * m_maxHeightRatio);

    // 设置统一的颜色
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_barColor);

    // 根据方向绘制频谱条
    switch (m_direction) {
    case LeftToRight:
        // 从左到右正常绘制
        for (int i = 0; i < NUM_BARS; ++i) {
            float spectrumValue = m_spectrumData[i];
            if (spectrumValue <= 0.001f) continue;

            float normalizedValue = std::sqrt(spectrumValue);
            int barHeight = static_cast<int>(normalizedValue * maxAvailableHeight);
            barHeight = qMax(4, barHeight);

            int x = i * (m_barWidth + m_barSpacing);
            int y = height() - barHeight;

            QRect barRect(x, y, m_barWidth, barHeight);
            painter.drawRect(barRect);
        }
        break;

    case RightToLeft:
        // 从右到左镜像绘制
        for (int i = 0; i < NUM_BARS; ++i) {
            float spectrumValue = m_spectrumData[i];
            if (spectrumValue <= 0.001f) continue;

            float normalizedValue = std::sqrt(spectrumValue);
            int barHeight = static_cast<int>(normalizedValue * maxAvailableHeight);
            barHeight = qMax(4, barHeight);

            int x = width() - (i + 1) * (m_barWidth + m_barSpacing);
            int y = height() - barHeight;

            QRect barRect(x, y, m_barWidth, barHeight);
            painter.drawRect(barRect);
        }
        break;

    case CenterToLeft:
        // 从右侧边缘向左绘制（用于左侧频谱）
        for (int i = 0; i < NUM_BARS; ++i) {
            float spectrumValue = m_spectrumData[i];
            if (spectrumValue <= 0.001f) continue;

            float normalizedValue = std::sqrt(spectrumValue);
            int barHeight = static_cast<int>(normalizedValue * maxAvailableHeight);
            barHeight = qMax(4, barHeight);

            // 从右侧边缘开始向左绘制
            int x = width() - (i + 1) * (m_barWidth + m_barSpacing);
            int y = height() - barHeight;

            QRect barRect(x, y, m_barWidth, barHeight);
            painter.drawRect(barRect);
        }
        break;

    case CenterToRight:
        // 从左侧边缘向右绘制（用于右侧频谱）
        for (int i = 0; i < NUM_BARS; ++i) {
            float spectrumValue = m_spectrumData[i];
            if (spectrumValue <= 0.001f) continue;

            float normalizedValue = std::sqrt(spectrumValue);
            int barHeight = static_cast<int>(normalizedValue * maxAvailableHeight);
            barHeight = qMax(4, barHeight);

            // 从左侧边缘开始向右绘制
            int x = i * (m_barWidth + m_barSpacing);
            int y = height() - barHeight;

            QRect barRect(x, y, m_barWidth, barHeight);
            painter.drawRect(barRect);
        }
        break;
    }
}