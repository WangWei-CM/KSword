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
    , m_primarySeriesColor(67, 160, 255)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(68);
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
    if (m_primarySeriesFollowsAccentColor)
    {
        m_primarySeriesColor = accentColor;
    }
    if (!m_secondarySeriesVisible)
    {
        m_secondarySeriesColor = QColor();
    }
    update();
}

void PerformanceNavCard::setSeriesColors(
    const QColor& primarySeriesColor,
    const QColor& secondarySeriesColor)
{
    m_primarySeriesFollowsAccentColor = !primarySeriesColor.isValid();
    m_primarySeriesColor = primarySeriesColor.isValid() ? primarySeriesColor : m_accentColor;
    m_secondarySeriesColor = secondarySeriesColor;
    m_secondarySeriesVisible = secondarySeriesColor.isValid();
    if (!m_secondarySeriesVisible)
    {
        m_secondarySamples.clear();
    }
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
    m_primarySamples.push_back(clampedPercent);
    while (m_primarySamples.size() > m_maxSampleCount)
    {
        m_primarySamples.pop_front();
    }
    update();
}

void PerformanceNavCard::appendDualSample(
    const double primaryUsagePercent,
    const double secondaryUsagePercent)
{
    // primaryClampedPercent 用途：主序列采样值，限制在 0~100。
    const double primaryClampedPercent = std::clamp(primaryUsagePercent, 0.0, 100.0);
    // secondaryClampedPercent 用途：次序列采样值，限制在 0~100。
    const double secondaryClampedPercent = std::clamp(secondaryUsagePercent, 0.0, 100.0);
    m_primarySamples.push_back(primaryClampedPercent);
    m_secondarySamples.push_back(secondaryClampedPercent);
    while (m_primarySamples.size() > m_maxSampleCount)
    {
        m_primarySamples.pop_front();
    }
    while (m_secondarySamples.size() > m_maxSampleCount)
    {
        m_secondarySamples.pop_front();
    }
    update();
}

void PerformanceNavCard::setSampleSeries(
    const QVector<double>& primarySampleList,
    const QVector<double>& secondarySampleList)
{
    m_primarySamples = primarySampleList;
    while (m_primarySamples.size() > m_maxSampleCount)
    {
        m_primarySamples.pop_front();
    }

    if (m_secondarySeriesVisible)
    {
        m_secondarySamples = secondarySampleList;
        while (m_secondarySamples.size() > m_maxSampleCount)
        {
            m_secondarySamples.pop_front();
        }
    }
    else
    {
        m_secondarySamples.clear();
    }
    update();
}

void PerformanceNavCard::clearSamples()
{
    m_primarySamples.clear();
    m_secondarySamples.clear();
    update();
}

QSize PerformanceNavCard::sizeHint() const
{
    return QSize(252, 68);
}

int PerformanceNavCard::sampleCapacity() const
{
    return m_maxSampleCount;
}

void PerformanceNavCard::paintEvent(QPaintEvent* paintEventPointer)
{
    Q_UNUSED(paintEventPointer);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 卡片区域：改成透明底，仅保留边框高亮，避免计数器页左侧卡片遮住背景。
    const QRect cardRect = rect().adjusted(1, 1, -1, -1);
    // cardBorderColor 用途：当前卡片边框颜色；选中时更亮，不选中时仅保留弱轮廓。
    const QColor cardBorderColor = QColor(
        m_accentColor.red(),
        m_accentColor.green(),
        m_accentColor.blue(),
        m_selected ? 210 : 86);
    QPen cardBorderPen(cardBorderColor);
    cardBorderPen.setWidthF(m_selected ? 1.2 : 0.8);
    painter.setPen(cardBorderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(cardRect, 4.0, 4.0);

    // 缩略图区域：保留边框与曲线，内部背景保持透明。
    const QRect sparkRect(cardRect.left() + 6, cardRect.top() + 7, 64, cardRect.height() - 14);
    // sparkBorderColor 用途：缩略图边框颜色；选中时使用实色，未选中时降低透明度。
    const QColor sparkBorderColor = QColor(
        m_accentColor.red(),
        m_accentColor.green(),
        m_accentColor.blue(),
        m_selected ? 220 : 150);
    painter.setBrush(Qt::NoBrush);
    QPen sparkBorderPen(sparkBorderColor);
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

    // drawSeriesPath 作用：
    // - 把采样列表映射为缩略图曲线；
    // - 兼容单点与多点两种情况，供主/次序列复用。
    const auto drawSeriesPath =
        [&painter, &sparkRect](const QVector<double>& sampleList, const QColor& seriesColor)
        {
            if (sampleList.isEmpty())
            {
                return;
            }

            QPen trendPen(seriesColor);
            trendPen.setWidthF(1.6);
            painter.setPen(trendPen);
            painter.setBrush(Qt::NoBrush);

            if (sampleList.size() == 1)
            {
                const double yRatio = sampleList.at(0) / 100.0;
                const double yValue = sparkRect.bottom() - yRatio * static_cast<double>(sparkRect.height());
                painter.drawLine(
                    QPointF(sparkRect.left(), yValue),
                    QPointF(sparkRect.right(), yValue));
                return;
            }

            QPainterPath path;
            const int pointCount = sampleList.size();
            for (int indexValue = 0; indexValue < pointCount; ++indexValue)
            {
                const double xRatio = pointCount > 1
                    ? static_cast<double>(indexValue) / static_cast<double>(pointCount - 1)
                    : 0.0;
                const double yRatio = sampleList.at(indexValue) / 100.0;
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

            painter.drawPath(path);
        };

    // 双线卡片先画次序列再画主序列，确保主线不会被遮住。
    if (m_secondarySeriesVisible)
    {
        drawSeriesPath(m_secondarySamples, m_secondarySeriesColor);
    }
    drawSeriesPath(m_primarySamples, m_primarySeriesColor);

    // 文本区域：主标题加粗，副标题使用次级颜色。
    const QRect titleRect(sparkRect.right() + 8, cardRect.top() + 5, cardRect.width() - sparkRect.width() - 18, 30);
    const QRect subtitleRect(sparkRect.right() + 8, cardRect.top() + 32, cardRect.width() - sparkRect.width() - 18, 28);

    QFont titleFont = painter.font();
    titleFont.setPointSizeF(18.0);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(KswordTheme::IsDarkModeEnabled() ? QColor(240, 240, 240) : QColor(26, 32, 38));
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, m_titleText);

    QFont subtitleFont = painter.font();
    subtitleFont.setPointSizeF(11.5);
    subtitleFont.setBold(false);
    painter.setFont(subtitleFont);
    painter.setPen(KswordTheme::IsDarkModeEnabled() ? QColor(198, 212, 225) : QColor(63, 83, 102));
    painter.drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter, m_subtitleText);
}
