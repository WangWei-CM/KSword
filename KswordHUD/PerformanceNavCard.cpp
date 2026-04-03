#include "PerformanceNavCard.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

namespace
{
    QColor textPrimaryColor()
    {
        return QColor(242, 246, 252);
    }

    QColor textSecondaryColor()
    {
        return QColor(190, 206, 226);
    }
}

PerformanceNavCard::PerformanceNavCard(QWidget* parent)
    : QWidget(parent)
    , m_accentColor(67, 160, 255)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
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
    const double clampedPercent = qBound(0.0, usagePercent, 100.0);
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

    const QRect cardRect = rect().adjusted(2, 2, -2, -2);
    const QColor cardBorderColor(
        m_accentColor.red(),
        m_accentColor.green(),
        m_accentColor.blue(),
        m_selected ? 210 : 92);
    QPen cardBorderPen(cardBorderColor);
    cardBorderPen.setWidthF(m_selected ? 1.6 : 1.0);
    painter.setPen(cardBorderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(cardRect, 4.0, 4.0);

    const QRect sparkRect(cardRect.left() + 10, cardRect.top() + 10, 62, cardRect.height() - 20);
    const QColor sparkBorderColor(
        m_accentColor.red(),
        m_accentColor.green(),
        m_accentColor.blue(),
        m_selected ? 220 : 150);
    QPen sparkBorderPen(sparkBorderColor);
    sparkBorderPen.setWidthF(1.2);
    painter.setPen(sparkBorderPen);
    painter.drawRect(sparkRect);

    QPen gridPen(QColor(
        m_accentColor.red(),
        m_accentColor.green(),
        m_accentColor.blue(),
        45));
    gridPen.setWidthF(0.8);
    painter.setPen(gridPen);
    for (int rowIndex = 1; rowIndex < 4; ++rowIndex)
    {
        const int yValue = sparkRect.top() + (sparkRect.height() * rowIndex / 4);
        painter.drawLine(sparkRect.left(), yValue, sparkRect.right(), yValue);
    }

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

    const QRect titleRect(
        sparkRect.right() + 10,
        cardRect.top() + 8,
        cardRect.width() - sparkRect.width() - 24,
        28);
    const QRect subtitleRect(
        sparkRect.right() + 10,
        cardRect.top() + 34,
        cardRect.width() - sparkRect.width() - 24,
        30);

    QFont titleFont = painter.font();
    titleFont.setPointSizeF(16.0);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(textPrimaryColor());
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, m_titleText);

    QFont subtitleFont = painter.font();
    subtitleFont.setPointSizeF(11.0);
    subtitleFont.setBold(false);
    painter.setFont(subtitleFont);
    painter.setPen(textSecondaryColor());
    painter.drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter, m_subtitleText);
}
