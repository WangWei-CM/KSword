#include "MemoryCompositionHistoryWidget.h"

#include "../theme.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QSizePolicy>

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    // CompositionColor 作用：保存图例名称与对应填充色。
    struct CompositionColor
    {
        const char* labelText = ""; // labelText：图例显示文本。
        QColor color;               // color：该内存构成层的填充颜色。
    };

    // buildCompositionColorList 作用：按绘制顺序返回内存构成配色。
    std::array<CompositionColor, 4> buildCompositionColorList()
    {
        return {
            CompositionColor{ "活跃", QColor(184, 99, 255, 145) },
            CompositionColor{ "缓存", QColor(79, 195, 247, 120) },
            CompositionColor{ "分页池", QColor(255, 193, 7, 120) },
            CompositionColor{ "非分页池", QColor(255, 112, 67, 130) },
        };
    }
}

MemoryCompositionHistoryWidget::MemoryCompositionHistoryWidget(QWidget* parent)
    : QWidget(parent)
{
    // 利用率页要求宽高不足时图表自动压缩，不能用固定最小高度撑出外层滚动条。
    setMinimumSize(0, 0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void MemoryCompositionHistoryWidget::setHistoryLength(const int historyLength)
{
    m_historyLength = std::max(2, historyLength);
    while (static_cast<int>(m_sampleList.size()) > m_historyLength)
    {
        m_sampleList.erase(m_sampleList.begin());
    }
    update();
}

void MemoryCompositionHistoryWidget::appendSample(const CompositionSample& sample)
{
    CompositionSample safeSample;
    safeSample.usedPercent = boundedPercent(sample.usedPercent);
    safeSample.cachedPercent = boundedPercent(sample.cachedPercent);
    safeSample.pagedPoolPercent = boundedPercent(sample.pagedPoolPercent);
    safeSample.nonPagedPoolPercent = boundedPercent(sample.nonPagedPoolPercent);

    const double poolPercentSum = safeSample.pagedPoolPercent + safeSample.nonPagedPoolPercent;
    safeSample.cachedPercent = std::min(safeSample.cachedPercent, safeSample.usedPercent);
    if (safeSample.cachedPercent + poolPercentSum > safeSample.usedPercent)
    {
        const double scaleValue = safeSample.usedPercent / std::max(1.0, safeSample.cachedPercent + poolPercentSum);
        safeSample.cachedPercent *= scaleValue;
        safeSample.pagedPoolPercent *= scaleValue;
        safeSample.nonPagedPoolPercent *= scaleValue;
    }
    safeSample.activePercent = std::max(
        0.0,
        safeSample.usedPercent
            - safeSample.cachedPercent
            - safeSample.pagedPoolPercent
            - safeSample.nonPagedPoolPercent);

    m_sampleList.push_back(safeSample);
    while (static_cast<int>(m_sampleList.size()) > m_historyLength)
    {
        m_sampleList.erase(m_sampleList.begin());
    }
    update();
}

void MemoryCompositionHistoryWidget::clearSamples()
{
    m_sampleList.clear();
    update();
}

void MemoryCompositionHistoryWidget::paintEvent(QPaintEvent* paintEventPointer)
{
    Q_UNUSED(paintEventPointer);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor textColor = KswordTheme::IsDarkModeEnabled()
        ? QColor(235, 242, 250)
        : QColor(25, 42, 60);
    const QColor borderColor = KswordTheme::IsDarkModeEnabled()
        ? QColor(95, 120, 148, 130)
        : QColor(145, 175, 210, 150);
    const QColor gridColor = KswordTheme::IsDarkModeEnabled()
        ? QColor(184, 99, 255, 38)
        : QColor(67, 160, 255, 45);

    painter.fillRect(rect(), Qt::transparent);
    // 图例空间随高度缩放；低高度时舍弃下方图例，优先保留折线和构成填充。
    const bool compactHeight = height() < 70;
    const QRectF plotRect = compactHeight
        ? rect().adjusted(4.0, 4.0, -4.0, -4.0)
        : rect().adjusted(8.0, 10.0, -8.0, -24.0);
    if (plotRect.width() <= 4.0 || plotRect.height() <= 4.0)
    {
        return;
    }

    painter.setPen(QPen(borderColor, 1.0));
    painter.drawRect(plotRect);
    painter.setPen(QPen(gridColor, 1.0));
    for (int gridIndex = 1; gridIndex < 4; ++gridIndex)
    {
        const double yValue = plotRect.top() + plotRect.height() * static_cast<double>(gridIndex) / 4.0;
        painter.drawLine(QPointF(plotRect.left(), yValue), QPointF(plotRect.right(), yValue));
    }

    drawStackedComposition(painter, plotRect);
    drawUsageLine(painter, plotRect);

    if (!compactHeight)
    {
        painter.setPen(textColor);
        painter.setFont(QFont(painter.font().family(), 9));
        painter.drawText(
            plotRect.adjusted(6.0, 4.0, -6.0, -4.0),
            Qt::AlignTop | Qt::AlignLeft,
            QStringLiteral("内存占用历史 / 构成填充"));
        drawLegend(painter, plotRect);
    }
}

double MemoryCompositionHistoryWidget::boundedPercent(const double percentValue)
{
    if (!std::isfinite(percentValue))
    {
        return 0.0;
    }
    return std::clamp(percentValue, 0.0, 100.0);
}

double MemoryCompositionHistoryWidget::sampleX(const int sampleIndex, const QRectF& plotRect) const
{
    if (m_sampleList.size() <= 1)
    {
        return plotRect.left();
    }
    const double denominatorValue = static_cast<double>(m_sampleList.size() - 1);
    return plotRect.left() + plotRect.width() * static_cast<double>(sampleIndex) / denominatorValue;
}

double MemoryCompositionHistoryWidget::percentY(const double percentValue, const QRectF& plotRect)
{
    return plotRect.bottom() - plotRect.height() * boundedPercent(percentValue) / 100.0;
}

void MemoryCompositionHistoryWidget::drawStackedComposition(QPainter& painter, const QRectF& plotRect) const
{
    if (m_sampleList.empty())
    {
        return;
    }

    const std::array<CompositionColor, 4> colorList = buildCompositionColorList();
    for (int componentIndex = 0; componentIndex < static_cast<int>(colorList.size()); ++componentIndex)
    {
        QPainterPath componentPath;
        componentPath.moveTo(sampleX(0, plotRect), plotRect.bottom());

        for (int sampleIndex = 0; sampleIndex < static_cast<int>(m_sampleList.size()); ++sampleIndex)
        {
            const CompositionSample& sample = m_sampleList[static_cast<std::size_t>(sampleIndex)];
            const double componentValues[4] = {
                sample.activePercent,
                sample.cachedPercent,
                sample.pagedPoolPercent,
                sample.nonPagedPoolPercent,
            };

            double stackedPercent = 0.0;
            for (int stackedIndex = 0; stackedIndex <= componentIndex; ++stackedIndex)
            {
                stackedPercent += componentValues[stackedIndex];
            }
            componentPath.lineTo(sampleX(sampleIndex, plotRect), percentY(stackedPercent, plotRect));
        }

        for (int sampleIndex = static_cast<int>(m_sampleList.size()) - 1; sampleIndex >= 0; --sampleIndex)
        {
            const CompositionSample& sample = m_sampleList[static_cast<std::size_t>(sampleIndex)];
            const double componentValues[4] = {
                sample.activePercent,
                sample.cachedPercent,
                sample.pagedPoolPercent,
                sample.nonPagedPoolPercent,
            };

            double lowerStackedPercent = 0.0;
            for (int stackedIndex = 0; stackedIndex < componentIndex; ++stackedIndex)
            {
                lowerStackedPercent += componentValues[stackedIndex];
            }
            componentPath.lineTo(sampleX(sampleIndex, plotRect), percentY(lowerStackedPercent, plotRect));
        }
        componentPath.closeSubpath();

        painter.fillPath(componentPath, colorList[static_cast<std::size_t>(componentIndex)].color);
    }
}

void MemoryCompositionHistoryWidget::drawUsageLine(QPainter& painter, const QRectF& plotRect) const
{
    if (m_sampleList.empty())
    {
        return;
    }

    QPainterPath linePath;
    for (int sampleIndex = 0; sampleIndex < static_cast<int>(m_sampleList.size()); ++sampleIndex)
    {
        const QPointF pointValue(
            sampleX(sampleIndex, plotRect),
            percentY(m_sampleList[static_cast<std::size_t>(sampleIndex)].usedPercent, plotRect));
        if (sampleIndex == 0)
        {
            linePath.moveTo(pointValue);
        }
        else
        {
            linePath.lineTo(pointValue);
        }
    }

    painter.setPen(QPen(QColor(184, 99, 255), 2.0));
    painter.drawPath(linePath);
}

void MemoryCompositionHistoryWidget::drawLegend(QPainter& painter, const QRectF& plotRect) const
{
    const std::array<CompositionColor, 4> colorList = buildCompositionColorList();
    const QColor textColor = KswordTheme::IsDarkModeEnabled()
        ? QColor(235, 242, 250)
        : QColor(25, 42, 60);

    painter.setFont(QFont(painter.font().family(), 8));
    painter.setPen(textColor);

    double xValue = plotRect.left();
    const double yValue = plotRect.bottom() + 9.0;
    for (const CompositionColor& colorEntry : colorList)
    {
        const QRectF colorRect(xValue, yValue, 10.0, 7.0);
        painter.fillRect(colorRect, colorEntry.color);
        painter.drawText(
            QRectF(xValue + 13.0, yValue - 4.0, 58.0, 16.0),
            Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromUtf8(colorEntry.labelText));
        xValue += 68.0;
    }
}
