#include "PerformanceNavCard.h"
#include "HudColors.h"

#include <QEasingCurve>
#include <QVariantAnimation>
#include <QPaintEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QPainter>
#include <QPainterPath>

PerformanceNavCard::PerformanceNavCard(QWidget* parent)
    : QWidget(parent)
    , m_accentColor(67, 160, 255)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(78);
    m_sampleAnimation = new QVariantAnimation(this);
    m_sampleAnimation->setDuration(260);
    m_sampleAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_sampleAnimation->setStartValue(0.0);
    m_sampleAnimation->setEndValue(1.0);
    connect(m_sampleAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_animationProgress = value.toDouble();
        update();
    });
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
    const double previousSample = m_samples.isEmpty()
        ? clampedPercent
        : m_samples.back();
    m_previousSampleCount = static_cast<int>(m_samples.size());
    m_historyWindowShifted = m_previousSampleCount >= m_maxSampleCount;
    m_samples.push_back(clampedPercent);
    while (m_samples.size() > m_maxSampleCount)
    {
        m_samples.pop_front();
    }
    startLatestSampleAnimation(previousSample);
}

void PerformanceNavCard::clearSamples()
{
    m_sampleAnimation->stop();
    m_animationProgress = 1.0;
    m_previousSampleCount = 0;
    m_historyWindowShifted = false;
    m_samples.clear();
    update();
}

void PerformanceNavCard::startLatestSampleAnimation(const double previousSample)
{
    m_previousSample = previousSample;
    m_sampleAnimation->stop();
    // Recording remains active while the HUD is hidden; presentation does not.
    if (!isVisible())
    {
        m_animationProgress = 1.0;
        return;
    }
    m_animationProgress = 0.0;
    m_sampleAnimation->start();
}

void PerformanceNavCard::hideEvent(QHideEvent* event)
{
    m_sampleAnimation->stop();
    m_animationProgress = 1.0;
    QWidget::hideEvent(event);
}

void PerformanceNavCard::showEvent(QShowEvent* event)
{
    m_sampleAnimation->stop();
    m_animationProgress = 1.0;
    QWidget::showEvent(event);
    update();
}

double PerformanceNavCard::animatedXRatio(const int sampleIndex, const int sampleCount) const
{
    if (sampleCount <= 1)
    {
        return 0.0;
    }

    const double targetRatio =
        static_cast<double>(sampleIndex) / static_cast<double>(sampleCount - 1);
    double startRatio = targetRatio;
    if (m_historyWindowShifted && m_previousSampleCount == sampleCount)
    {
        startRatio = sampleIndex + 1 < sampleCount
            ? static_cast<double>(sampleIndex + 1) / static_cast<double>(sampleCount - 1)
            : 1.0;
    }
    else if (m_previousSampleCount + 1 == sampleCount && m_previousSampleCount > 1)
    {
        startRatio = sampleIndex < m_previousSampleCount
            ? static_cast<double>(sampleIndex) / static_cast<double>(m_previousSampleCount - 1)
            : 1.0;
    }

    return startRatio + (targetRatio - startRatio) * m_animationProgress;
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
    const auto animatedValueAt = [this](const int indexValue) {
        const double targetValue = m_samples.at(indexValue);
        if (indexValue != m_samples.size() - 1 || m_animationProgress >= 1.0)
        {
            return targetValue;
        }
        return m_previousSample + (targetValue - m_previousSample) * m_animationProgress;
    };

    const QRect cardRect = rect().adjusted(2, 2, -2, -2);
    // 数据色相保留，只按当前面板有效底色校准线条；文字跟随父性能页 palette。
    const QColor accent = KswordHudColors::Readable(m_accentColor, palette().color(QPalette::Window), 3.0);
    const QColor cardBorderColor(
        accent.red(),
        accent.green(),
        accent.blue(),
        m_selected ? 210 : 92);
    QPen cardBorderPen(cardBorderColor);
    cardBorderPen.setWidthF(m_selected ? 1.6 : 1.0);
    painter.setPen(cardBorderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(cardRect, 4.0, 4.0);

    const QRect sparkRect(cardRect.left() + 10, cardRect.top() + 10, 62, cardRect.height() - 20);
    const QColor sparkBorderColor(
        accent.red(),
        accent.green(),
        accent.blue(),
        m_selected ? 220 : 150);
    QPen sparkBorderPen(sparkBorderColor);
    sparkBorderPen.setWidthF(1.2);
    painter.setPen(sparkBorderPen);
    painter.drawRect(sparkRect);

    QPen gridPen(QColor(
        accent.red(),
        accent.green(),
        accent.blue(),
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
        const double yRatio = animatedValueAt(0) / 100.0;
        const double yValue = sparkRect.bottom() - yRatio * static_cast<double>(sparkRect.height());
        QPen trendPen(accent);
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
            const double xRatio = animatedXRatio(indexValue, pointCount);
            const double yRatio = animatedValueAt(indexValue) / 100.0;
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

        QPen trendPen(accent);
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
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, m_titleText);

    QFont subtitleFont = painter.font();
    subtitleFont.setPointSizeF(11.0);
    subtitleFont.setBold(false);
    painter.setFont(subtitleFont);
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter, m_subtitleText);
}
