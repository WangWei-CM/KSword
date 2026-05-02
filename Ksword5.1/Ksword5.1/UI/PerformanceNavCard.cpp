#include "PerformanceNavCard.h"

// ============================================================
// PerformanceNavCard.cpp
// 作用：
// 1) 按任务管理器样式绘制左侧性能导航卡片；
// 2) 深浅色主题下统一处理背景和文字可读性；
// 3) 维护缩略折线历史并在每次采样后重绘。
// ============================================================

#include "../theme.h"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>

#include <algorithm>

PerformanceNavCard::PerformanceNavCard(QWidget* parent)
    : QWidget(parent)
    , m_accentColor(67, 160, 255)
    , m_primarySeriesColor(67, 160, 255)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    // 左侧设备列表会按 Dock 可用高度动态压缩卡片，因此这里不能设置固定最小高度。
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumSize(0, 0);
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
    // 默认高度压到 52px，实际高度由 HardwareDock 按列表可见高度继续动态下调。
    return QSize(208, 52);
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
    // compactMode 用途：窄宽度/低高度下收缩缩略图和文字字号，避免左侧列表触发滚动条。
    const bool compactMode = cardRect.width() < 176 || cardRect.height() < 48;
    const int sparkInset = compactMode ? 4 : 5;
    const int sparkWidth = std::clamp(cardRect.width() / 3, 8, compactMode ? 44 : 56);
    const QRect sparkRect(
        cardRect.left() + sparkInset,
        cardRect.top() + sparkInset,
        sparkWidth,
        std::max(1, cardRect.height() - sparkInset * 2));
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
    // - 先绘制折线与 X 轴围成的透明填充，再绘制趋势线本体。
    const auto drawSeriesPath =
        [&painter, &sparkRect](const QVector<double>& sampleList, const QColor& seriesColor)
        {
            if (sampleList.isEmpty())
            {
                return;
            }

            QPen trendPen(seriesColor);
            trendPen.setWidthF(1.6);

            if (sampleList.size() == 1)
            {
                const double yRatio = sampleList.at(0) / 100.0;
                const double yValue = sparkRect.bottom() - yRatio * static_cast<double>(sparkRect.height());
                QColor fillColor(
                    seriesColor.red(),
                    seriesColor.green(),
                    seriesColor.blue(),
                    34);
                painter.fillRect(
                    QRectF(
                        QPointF(sparkRect.left(), yValue),
                        QPointF(sparkRect.right(), sparkRect.bottom())),
                    fillColor);
                painter.setPen(trendPen);
                painter.setBrush(Qt::NoBrush);
                painter.drawLine(
                    QPointF(sparkRect.left(), yValue),
                    QPointF(sparkRect.right(), yValue));
                return;
            }

            QPainterPath path;
            QPainterPath fillPath;
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
                    fillPath.moveTo(xValue, sparkRect.bottom());
                    fillPath.lineTo(xValue, yValue);
                }
                else
                {
                    path.lineTo(xValue, yValue);
                    fillPath.lineTo(xValue, yValue);
                }
            }

            fillPath.lineTo(sparkRect.right(), sparkRect.bottom());
            fillPath.closeSubpath();
            painter.fillPath(
                fillPath,
                QColor(seriesColor.red(), seriesColor.green(), seriesColor.blue(), 34));
            painter.setPen(trendPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        };

    // 双线卡片先画次序列再画主序列，确保主线不会被遮住。
    if (m_secondarySeriesVisible)
    {
        drawSeriesPath(m_secondarySamples, m_secondarySeriesColor);
    }
    drawSeriesPath(m_primarySamples, m_primarySeriesColor);

    // 文本区域：主标题加粗，副标题使用次级颜色。
    const int textLeft = sparkRect.right() + (compactMode ? 5 : 7);
    const int textWidth = std::max(0, cardRect.right() - textLeft - 4);
    const int titleHeight = std::max(1, cardRect.height() / 2);
    const QRect titleRect(textLeft, cardRect.top() + 2, textWidth, titleHeight);
    const QRect subtitleRect(
        textLeft,
        titleRect.bottom() - 1,
        textWidth,
        std::max(1, cardRect.bottom() - titleRect.bottom()));

    QFont titleFont = painter.font();
    // 设备名主标题按需求改小，并在紧凑模式下继续压缩。
    titleFont.setPointSizeF(compactMode ? 11.0 : 12.5);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(KswordTheme::IsDarkModeEnabled() ? QColor(240, 240, 240) : QColor(26, 32, 38));
    const QString elidedTitleText =
        QFontMetrics(titleFont).elidedText(m_titleText, Qt::ElideRight, titleRect.width());
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, elidedTitleText);

    QFont subtitleFont = painter.font();
    subtitleFont.setPointSizeF(compactMode ? 8.5 : 9.5);
    subtitleFont.setBold(false);
    painter.setFont(subtitleFont);
    painter.setPen(KswordTheme::IsDarkModeEnabled() ? QColor(198, 212, 225) : QColor(63, 83, 102));
    const QString elidedSubtitleText =
        QFontMetrics(subtitleFont).elidedText(m_subtitleText, Qt::ElideRight, subtitleRect.width());
    painter.drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter, elidedSubtitleText);
}
