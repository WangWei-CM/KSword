#include "PerformanceNavCard.h"

// ============================================================
// PerformanceNavCard.cpp
// 作用：
// 1) 按任务管理器样式绘制左侧性能导航卡片；
// 2) 深浅色主题下统一处理背景和文字可读性；
// 3) 维护缩略折线历史并在每次采样后重绘。
// ============================================================

#include "../theme.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

PerformanceNavCard::PerformanceNavCard(QWidget* parent)
    : QWidget(parent)
    , m_accentColor(67, 160, 255)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(78);
}

void PerformanceNavCard::setTitleText(const QString& titleText)
{
    m_titleText = titleText;
    update();
}

void PerformanceNavCard::setSubtitleText(const QString& subtitleText)
{
    m_subtitleText = subtitleText;
    update();
}

void PerformanceNavCard::setAccentColor(const QColor& accentColor)
{
    m_accentColor = accentColor;
    update();
}

void PerformanceNavCard::setSelectedState(const bool selected)
{
    m_selected = selected;
    update();
}

void PerformanceNavCard::appendSample(const double usagePercent)
{
    // clampedPercent 用途：把外部传入采样限制在 0~100，避免越界绘制。
    const double clampedPercent = std::clamp(usagePercent, 0.0, 100.0);
    m_samples.push_back(clampedPercent);
    while (m_samples.size() > m_maxSampleCount)
    {
        m_samples.pop_front();
    }
    update();
}

void PerformanceNavCard::clearSamples()
{
    m_samples.clear();
    update();
}

QSize PerformanceNavCard::sizeHint() const
{
    return QSize(264, 78);
}

void PerformanceNavCard::paintEvent(QPaintEvent* paintEventPointer)
{
    Q_UNUSED(paintEventPointer);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 卡片区域：统一给圆角背景，选中态按主题使用蓝色高亮底。
    const QRect cardRect = rect().adjusted(2, 2, -2, -2);
    QColor backgroundColor = KswordTheme::IsDarkModeEnabled()
        ? QColor(34, 40, 48)
        : QColor(244, 247, 251);
    if (m_selected)
    {
        backgroundColor = KswordTheme::IsDarkModeEnabled()
            ? QColor(47, 79, 109)
            : QColor(189, 216, 241);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(backgroundColor);
    painter.drawRoundedRect(cardRect, 4.0, 4.0);

    // 缩略图区域：绘制边框和背景，保持与任务管理器左栏接近。
    const QRect sparkRect(cardRect.left() + 10, cardRect.top() + 10, 62, cardRect.height() - 20);
    QColor sparkBackgroundColor = KswordTheme::IsDarkModeEnabled()
        ? QColor(28, 28, 28)
        : QColor(250, 252, 255);
    painter.setBrush(sparkBackgroundColor);
    QPen sparkBorderPen(m_accentColor);
    sparkBorderPen.setWidthF(1.2);
    painter.setPen(sparkBorderPen);
    painter.drawRect(sparkRect);

    // 网格线：浅色辅助线，提升趋势可读性但不喧宾夺主。
    QPen gridPen(m_accentColor);
    gridPen.setWidthF(0.8);
    gridPen.setColor(QColor(m_accentColor.red(), m_accentColor.green(), m_accentColor.blue(), 45));
    painter.setPen(gridPen);
    for (int rowIndex = 1; rowIndex < 4; ++rowIndex)
    {
        const int yValue = sparkRect.top() + (sparkRect.height() * rowIndex / 4);
        painter.drawLine(sparkRect.left(), yValue, sparkRect.right(), yValue);
    }

    // 折线路径：把采样映射到缩略图区域，最大点数不足时也正常绘制。
    if (m_samples.size() == 1)
    {
        const double yRatio = m_samples.at(0) / 100.0;
        const double yValue = sparkRect.bottom() - yRatio * static_cast<double>(sparkRect.height());
        QPen trendPen(m_accentColor);
        trendPen.setWidthF(1.6);
        painter.setPen(trendPen);
        painter.drawLine(
            QPointF(sparkRect.left(), yValue),
            QPointF(sparkRect.right(), yValue));
    }
    else if (m_samples.size() >= 2)
    {
        QPainterPath path;
        const int pointCount = m_samples.size();
        for (int indexValue = 0; indexValue < pointCount; ++indexValue)
        {
            const double xRatio = pointCount > 1
                ? static_cast<double>(indexValue) / static_cast<double>(pointCount - 1)
                : 0.0;
            const double yRatio = m_samples.at(indexValue) / 100.0;
            const double xValue = sparkRect.left() + xRatio * static_cast<double>(sparkRect.width());
            const double yValue = sparkRect.bottom() - yRatio * static_cast<double>(sparkRect.height());
            if (indexValue == 0)
            {
                path.moveTo(xValue, yValue);
            }
            else
            {
                path.lineTo(xValue, yValue);
            }
        }

        QPen trendPen(m_accentColor);
        trendPen.setWidthF(1.6);
        painter.setPen(trendPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    // 文本区域：主标题加粗，副标题使用次级颜色。
    const QRect titleRect(sparkRect.right() + 10, cardRect.top() + 8, cardRect.width() - sparkRect.width() - 24, 28);
    const QRect subtitleRect(sparkRect.right() + 10, cardRect.top() + 34, cardRect.width() - sparkRect.width() - 24, 30);

    QFont titleFont = painter.font();
    titleFont.setPointSizeF(16.0);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(KswordTheme::IsDarkModeEnabled() ? QColor(240, 240, 240) : QColor(26, 32, 38));
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, m_titleText);

    QFont subtitleFont = painter.font();
    subtitleFont.setPointSizeF(11.0);
    subtitleFont.setBold(false);
    painter.setFont(subtitleFont);
    painter.setPen(KswordTheme::IsDarkModeEnabled() ? QColor(198, 212, 225) : QColor(63, 83, 102));
    painter.drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter, m_subtitleText);
}
