#include "ProcessTraceTimelineWidget.h"

// ============================================================
// ProcessTraceTimelineWidget.cpp
// 作用：
// 1) 绘制紧凑的 ETW 瀑布流时间轴与分类行；
// 2) 维护内部绝对时间选区，作为事件表时间筛选条件；
// 3) 把鼠标操作转换成时间戳更新，不从图形事件点反向推导事件集合。
// ============================================================

#include "../theme.h"

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QSizePolicy>
#include <QtGlobal>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace
{
    // kTimelineHeight：
    // - ETW 瀑布流时间轴的固定可视高度；
    // - 构造函数会同步把控件高度锁定到该值。
    constexpr int kTimelineHeight = 40;

    // kHorizontalPadding：
    // - 为边框和左右标签保留极小横向边距；
    // - 其余宽度全部作为时间轴可用空间。
    constexpr int kHorizontalPadding = 4;

    // kVerticalPadding：
    // - 选区矩形需要占满时间轴高度，因此垂直方向不预留额外空白；
    // - 线条抗锯齿产生的半像素裁剪可接受，优先满足交互语义。
    constexpr int kVerticalPadding = 0;

    // kEdgeHitWidth：
    // - 选区左右边缘可触发拉伸的逻辑像素宽度；
    // - 命中区域故意宽于可见线条，便于鼠标操作。
    constexpr int kEdgeHitWidth = 6;

    // kMinimumSelection100ns：
    // - 防止选区坍缩成零宽时间点；
    // - 10ms 足够细，同时仍然可拖动。
    constexpr std::uint64_t kMinimumSelection100ns = 10ULL * 1000ULL * 10ULL;

    // kDefaultRange100ns：
    // - 第一条 ETW 事件到达前提供非零绘制范围；
    // - 1 秒默认跨度能让初始坐标计算保持稳定。
    constexpr std::uint64_t kDefaultRange100ns = 1ULL * 1000ULL * 1000ULL * 10ULL;

    // kLaneCount：
    // - 为当前 ETW 类型下拉中的主要类别预留独立行；
    // - 40px 高度较窄，因此点半径会相应减小。
    constexpr int kLaneCount = 13;

    // themeColorFromText：
    // - 将主题返回的调色板字符串安全转换为 QColor；
    // - 当主题文本不是具体 #RRGGBB 时，使用 fallbackColor 保持绘制稳定。
    QColor themeColorFromText(const QString& colorText, const QColor& fallbackColor)
    {
        QColor colorValue(colorText);
        return colorValue.isValid() ? colorValue : fallbackColor;
    }

    // effectiveWheelDelta：
    // - 从 Qt 滚轮事件中读取最可靠的滚动方向；
    // - 正数表示向上滚动，负数表示向下滚动。
    int effectiveWheelDelta(const QWheelEvent* eventPointer)
    {
        if (eventPointer == nullptr)
        {
            return 0;
        }

        const QPoint angleDelta = eventPointer->angleDelta();
        if (angleDelta.y() != 0)
        {
            return angleDelta.y();
        }

        const QPoint pixelDelta = eventPointer->pixelDelta();
        return pixelDelta.y();
    }

    // currentMousePosition：
    // - 返回兼容不同 Qt 版本的本地鼠标坐标；
    // - Qt 6 使用 position()，旧接口使用 pos()。
    QPoint currentMousePosition(const QMouseEvent* eventPointer)
    {
        if (eventPointer == nullptr)
        {
            return QPoint();
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return eventPointer->position().toPoint();
#else
        return eventPointer->pos();
#endif
    }

    // currentWheelPosition：
    // - 返回兼容不同 Qt 版本的本地滚轮坐标；
    // - 返回点仅用作缩放锚点。
    QPoint currentWheelPosition(const QWheelEvent* eventPointer)
    {
        if (eventPointer == nullptr)
        {
            return QPoint();
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return eventPointer->position().toPoint();
#else
        return eventPointer->pos();
#endif
    }
}

ProcessTraceTimelineWidget::ProcessTraceTimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    // 本控件是窄条时间轴，应该占用父布局提供的全部横向空间。
    setFixedHeight(kTimelineHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

void ProcessTraceTimelineWidget::setCaptureRange(
    const std::uint64_t start100ns,
    const std::uint64_t end100ns)
{
    // normalizedEnd100ns 用途：即使采集刚开始或调用方传入相同起止值，也保证时间轴范围非零。
    const std::uint64_t normalizedEnd100ns = end100ns > start100ns
        ? end100ns
        : start100ns + kDefaultRange100ns;

    const bool hadRange = m_rangeEnd100ns > m_rangeStart100ns;
    m_rangeStart100ns = start100ns;
    m_rangeEnd100ns = normalizedEnd100ns;

    // 用户未手动调整选区前，选区持续覆盖完整捕获范围，避免默认产生隐式时间过滤。
    if (!hadRange || !m_userAdjustedSelection)
    {
        m_selectionStart100ns = m_rangeStart100ns;
        m_selectionEnd100ns = m_rangeEnd100ns;
    }
    else
    {
        clampSelectionToRange();
    }

    update();
}

void ProcessTraceTimelineWidget::resetTimeline(const std::uint64_t start100ns)
{
    m_eventPointList.clear();
    m_ratePointList.clear();
    m_rangeStart100ns = start100ns;
    m_rangeEnd100ns = start100ns + kDefaultRange100ns;
    m_selectionStart100ns = m_rangeStart100ns;
    m_selectionEnd100ns = m_rangeEnd100ns;
    m_dragPressTime100ns = 0;
    m_dragOriginalStart100ns = 0;
    m_dragOriginalEnd100ns = 0;
    m_dragMode = DragMode::None;
    m_userAdjustedSelection = false;
    unsetCursor();
    update();
}

void ProcessTraceTimelineWidget::resetSelectionToFullRange()
{
    // 清空筛选时使用全范围选区，并重新允许后续采集时间右端自动跟随。
    m_selectionStart100ns = m_rangeStart100ns;
    m_selectionEnd100ns = m_rangeEnd100ns;
    m_userAdjustedSelection = false;
    m_dragMode = DragMode::None;
    unsetCursor();
    update();
}

void ProcessTraceTimelineWidget::setEventPoints(
    const std::vector<ProcessTraceTimelineEventPoint>& eventPointList)
{
    m_eventPointList = eventPointList;
    update();
}

void ProcessTraceTimelineWidget::setRateOverlayPoints(
    const std::vector<ProcessTraceTimelineRatePoint>& ratePointList)
{
    // 速率折线是可选叠加层：
    // - ETW 页不会设置该列表，因此原有事件瀑布流绘制不受影响；
    // - 网络页传入按秒聚合后的上传/下载速率，控件只负责映射到当前时间范围。
    m_ratePointList = ratePointList;
    update();
}

void ProcessTraceTimelineWidget::setSelectionChangedCallback(
    std::function<void(std::uint64_t, std::uint64_t)> callbackValue)
{
    m_selectionChangedCallback = std::move(callbackValue);
}

std::uint64_t ProcessTraceTimelineWidget::selectionStart100ns() const
{
    return m_selectionStart100ns;
}

std::uint64_t ProcessTraceTimelineWidget::selectionEnd100ns() const
{
    return m_selectionEnd100ns;
}

void ProcessTraceTimelineWidget::paintEvent(QPaintEvent* eventPointer)
{
    (void)eventPointer;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF axisRect = timelineRect();
    // themeColorFromText 用途：KswordTheme 可能返回 palette(mid) 这类样式表文本，
    // 绘图 API 需要真实 QColor，因此这里统一提供深浅色兜底。
    const QColor borderColor = themeColorFromText(
        KswordTheme::BorderHex(),
        KswordTheme::IsDarkModeEnabled() ? QColor(78, 86, 96) : QColor(210, 216, 224));
    const QColor surfaceColor = themeColorFromText(
        KswordTheme::SurfaceHex(),
        KswordTheme::IsDarkModeEnabled() ? QColor(28, 32, 38) : QColor(250, 252, 255));
    const QColor textColor = themeColorFromText(
        KswordTheme::TextSecondaryHex(),
        KswordTheme::IsDarkModeEnabled() ? QColor(172, 184, 198) : QColor(96, 105, 116));

    // 背景与边框：
    // - 这个矩形本身代表完整时间范围；
    // - 即使没有事件，也要保留清晰边界。
    painter.setPen(QPen(borderColor, 1.0));
    painter.setBrush(surfaceColor);
    painter.drawRect(axisRect);

    // 行分隔线只做弱提示，主要类别信息由事件点颜色表达。
    painter.setPen(QPen(borderColor, 0.5));
    for (int laneIndex = 1; laneIndex < kLaneCount; ++laneIndex)
    {
        const double yValue = axisRect.top()
            + axisRect.height() * static_cast<double>(laneIndex)
            / static_cast<double>(kLaneCount);
        painter.drawLine(QPointF(axisRect.left(), yValue), QPointF(axisRect.right(), yValue));
    }

    // 分类绘制事件点：
    // - 每类事件落在稳定纵向行；
    // - 点透明度为 20%，高密度事件会自然叠加强度。
    for (const ProcessTraceTimelineEventPoint& pointValue : m_eventPointList)
    {
        if (pointValue.time100ns < m_rangeStart100ns || pointValue.time100ns > m_rangeEnd100ns)
        {
            continue;
        }

        const int laneIndex = laneForType(pointValue.typeText);
        const double laneHeight = axisRect.height() / static_cast<double>(kLaneCount);
        const double yValue = axisRect.top() + laneHeight * (static_cast<double>(laneIndex) + 0.5);
        const double xValue = timeToX(pointValue.time100ns);

        painter.setPen(Qt::NoPen);
        painter.setBrush(colorForType(pointValue.typeText));
        painter.drawEllipse(QPointF(xValue, yValue), 1.7, 1.7);
    }

    // 速率折线叠加：
    // - 折线使用同一条 X 轴，Y 轴按当前可见范围内的峰值自适应；
    // - 绿色表示上传/出站，蓝色表示下载/入站；
    // - 折线绘制在选区框之前，保证框选区域仍然位于最上层。
    if (!m_ratePointList.empty() && m_rangeEnd100ns > m_rangeStart100ns)
    {
        double maxVisibleRate = 0.0;
        for (const ProcessTraceTimelineRatePoint& ratePoint : m_ratePointList)
        {
            if (ratePoint.time100ns < m_rangeStart100ns || ratePoint.time100ns > m_rangeEnd100ns)
            {
                continue;
            }
            maxVisibleRate = std::max(maxVisibleRate, ratePoint.uploadBytesPerSecond);
            maxVisibleRate = std::max(maxVisibleRate, ratePoint.downloadBytesPerSecond);
        }

        if (maxVisibleRate > 0.0)
        {
            QPolygonF uploadPolygon;
            QPolygonF downloadPolygon;
            const QRectF rateRect = axisRect.adjusted(0.0, 3.0, 0.0, -4.0);

            // appendRatePoint 用途：把“某秒 B/s”映射成折线坐标点。
            const auto appendRatePoint = [this, &rateRect, maxVisibleRate](
                QPolygonF& polygon,
                const std::uint64_t time100ns,
                const double bytesPerSecond)
                {
                    const double clampedRate = std::clamp(bytesPerSecond, 0.0, maxVisibleRate);
                    const double ratio = maxVisibleRate <= 0.0 ? 0.0 : clampedRate / maxVisibleRate;
                    const double xValue = timeToX(time100ns);
                    const double yValue = rateRect.bottom() - rateRect.height() * ratio;
                    polygon << QPointF(xValue, yValue);
                };

            for (const ProcessTraceTimelineRatePoint& ratePoint : m_ratePointList)
            {
                if (ratePoint.time100ns < m_rangeStart100ns || ratePoint.time100ns > m_rangeEnd100ns)
                {
                    continue;
                }
                appendRatePoint(uploadPolygon, ratePoint.time100ns, ratePoint.uploadBytesPerSecond);
                appendRatePoint(downloadPolygon, ratePoint.time100ns, ratePoint.downloadBytesPerSecond);
            }

            QColor uploadLineColor(76, 175, 80, 220);
            QColor downloadLineColor(33, 150, 243, 220);
            painter.setBrush(Qt::NoBrush);

            // drawPolyline 需要至少两个点；单秒只有一个采样时退化成圆点，避免折线不可见。
            painter.setPen(QPen(downloadLineColor, 1.5));
            if (downloadPolygon.size() > 1)
            {
                painter.drawPolyline(downloadPolygon);
            }
            else if (downloadPolygon.size() == 1)
            {
                painter.setBrush(downloadLineColor);
                painter.drawEllipse(downloadPolygon.first(), 2.0, 2.0);
                painter.setBrush(Qt::NoBrush);
            }

            painter.setPen(QPen(uploadLineColor, 1.5));
            if (uploadPolygon.size() > 1)
            {
                painter.drawPolyline(uploadPolygon);
            }
            else if (uploadPolygon.size() == 1)
            {
                painter.setBrush(uploadLineColor);
                painter.drawEllipse(uploadPolygon.first(), 2.0, 2.0);
                painter.setBrush(Qt::NoBrush);
            }

            // 简短图例直接绘制在轴内，避免新增控件占用网络 Dock 垂直空间。
            const QFont originalFont = painter.font();
            QFont legendFont = originalFont;
            legendFont.setPointSizeF(std::max(7.0, originalFont.pointSizeF() - 1.0));
            painter.setFont(legendFont);
            painter.setPen(uploadLineColor);
            painter.drawText(axisRect.adjusted(54.0, 1.0, -54.0, 0.0), Qt::AlignTop | Qt::AlignHCenter, QStringLiteral("上行"));
            painter.setPen(downloadLineColor);
            painter.drawText(axisRect.adjusted(96.0, 1.0, -12.0, 0.0), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("下行"));
            painter.setFont(originalFont);
        }
    }

    // 选区框放在事件点之后绘制，保证拖拽框始终可见。
    const QRectF selectedRect = selectionRect();
    if (!selectedRect.isEmpty())
    {
        QColor fillColor(KswordTheme::PrimaryBlueColor);
        fillColor.setAlpha(36);
        QColor edgeColor(KswordTheme::PrimaryBlueColor);
        edgeColor.setAlpha(220);

        painter.setBrush(fillColor);
        painter.setPen(QPen(edgeColor, 1.5));
        painter.drawRect(selectedRect);

        // 左右把手是两条竖线，不额外创建子控件。
        painter.setPen(QPen(edgeColor, 2.0));
        painter.drawLine(selectedRect.topLeft(), selectedRect.bottomLeft());
        painter.drawLine(selectedRect.topRight(), selectedRect.bottomRight());
    }

    // 标签最后绘制：
    // - 左侧固定相对时间 00:00；
    // - 右侧显示当前或停止后的总耗时。
    painter.setPen(textColor);
    const QString leftText = QStringLiteral("00:00");
    const QString rightText = formatDurationText(m_rangeEnd100ns > m_rangeStart100ns
        ? (m_rangeEnd100ns - m_rangeStart100ns)
        : 0);
    painter.drawText(axisRect.adjusted(5, 0, -5, 0), Qt::AlignLeft | Qt::AlignVCenter, leftText);
    painter.drawText(axisRect.adjusted(5, 0, -5, 0), Qt::AlignRight | Qt::AlignVCenter, rightText);
}

void ProcessTraceTimelineWidget::mousePressEvent(QMouseEvent* eventPointer)
{
    // 只响应左键拖拽，避免右键或中键误触发时间窗口变化。
    if (eventPointer == nullptr || eventPointer->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(eventPointer);
        return;
    }

    // hitMode 决定后续鼠标移动是整体平移还是单侧拉伸。
    const QPoint position = currentMousePosition(eventPointer);
    const DragMode hitMode = hitTestSelection(position);
    if (hitMode == DragMode::None)
    {
        QWidget::mousePressEvent(eventPointer);
        return;
    }

    // 记录拖动起点的时间和原始选区，后续移动均基于这组稳定基准计算。
    m_dragMode = hitMode;
    m_dragPressTime100ns = xToTime(position.x());
    m_dragOriginalStart100ns = m_selectionStart100ns;
    m_dragOriginalEnd100ns = m_selectionEnd100ns;
    eventPointer->accept();
}

void ProcessTraceTimelineWidget::mouseMoveEvent(QMouseEvent* eventPointer)
{
    // 空事件直接交回 Qt 默认处理，保持 QWidget 行为一致。
    if (eventPointer == nullptr)
    {
        QWidget::mouseMoveEvent(eventPointer);
        return;
    }

    const QPoint position = currentMousePosition(eventPointer);
    if (m_dragMode == DragMode::None)
    {
        // 未拖动时只更新光标，不改变内部时间选区。
        updateHoverCursor(position);
        QWidget::mouseMoveEvent(eventPointer);
        return;
    }

    // currentTime100ns 是鼠标当前位置对应的真实时间，不依赖事件点绘制结果。
    const std::uint64_t currentTime100ns = xToTime(position.x());
    const std::uint64_t originalWidth100ns =
        m_dragOriginalEnd100ns > m_dragOriginalStart100ns
        ? (m_dragOriginalEnd100ns - m_dragOriginalStart100ns)
        : kMinimumSelection100ns;

    if (m_dragMode == DragMode::Move)
    {
        // 整体移动时保持原选区宽度，只改变左右边界的绝对时间。
        const qint64 delta100ns = static_cast<qint64>(currentTime100ns)
            - static_cast<qint64>(m_dragPressTime100ns);
        qint64 newStart100ns = static_cast<qint64>(m_dragOriginalStart100ns) + delta100ns;
        qint64 newEnd100ns = static_cast<qint64>(m_dragOriginalEnd100ns) + delta100ns;

        if (newStart100ns < static_cast<qint64>(m_rangeStart100ns))
        {
            // 左侧越界时贴住时间轴起点，并保持原始宽度。
            newStart100ns = static_cast<qint64>(m_rangeStart100ns);
            newEnd100ns = newStart100ns + static_cast<qint64>(originalWidth100ns);
        }
        if (newEnd100ns > static_cast<qint64>(m_rangeEnd100ns))
        {
            // 右侧越界时贴住时间轴终点，并保持原始宽度。
            newEnd100ns = static_cast<qint64>(m_rangeEnd100ns);
            newStart100ns = newEnd100ns - static_cast<qint64>(originalWidth100ns);
        }

        m_selectionStart100ns = static_cast<std::uint64_t>(std::max<qint64>(newStart100ns, 0));
        m_selectionEnd100ns = static_cast<std::uint64_t>(std::max<qint64>(newEnd100ns, 0));
    }
    else if (m_dragMode == DragMode::ResizeLeft)
    {
        // 左边缘拉伸只修改起点，终点保持按下时的原值。
        m_selectionStart100ns = currentTime100ns;
        m_selectionEnd100ns = m_dragOriginalEnd100ns;
    }
    else if (m_dragMode == DragMode::ResizeRight)
    {
        // 右边缘拉伸只修改终点，起点保持按下时的原值。
        m_selectionStart100ns = m_dragOriginalStart100ns;
        m_selectionEnd100ns = currentTime100ns;
    }

    // 任何拖动都会把时间轴切换到用户选区模式，并立即通知事件表筛选。
    m_userAdjustedSelection = true;
    clampSelectionToRange();
    update();
    notifySelectionChanged();
    eventPointer->accept();
}

void ProcessTraceTimelineWidget::mouseReleaseEvent(QMouseEvent* eventPointer)
{
    // 左键释放即结束拖动；其他按钮继续走 QWidget 默认逻辑。
    if (eventPointer != nullptr && eventPointer->button() == Qt::LeftButton)
    {
        m_dragMode = DragMode::None;
        updateHoverCursor(currentMousePosition(eventPointer));
        eventPointer->accept();
        return;
    }

    QWidget::mouseReleaseEvent(eventPointer);
}

void ProcessTraceTimelineWidget::leaveEvent(QEvent* eventPointer)
{
    // 未拖动时离开控件应恢复普通光标，避免父界面继续显示拉伸光标。
    if (m_dragMode == DragMode::None)
    {
        unsetCursor();
    }

    QWidget::leaveEvent(eventPointer);
}

void ProcessTraceTimelineWidget::wheelEvent(QWheelEvent* eventPointer)
{
    // 捕获范围无效时不消费滚轮，交给父级滚动区域处理。
    if (eventPointer == nullptr || m_rangeEnd100ns <= m_rangeStart100ns)
    {
        QWidget::wheelEvent(eventPointer);
        return;
    }

    const int wheelDelta = effectiveWheelDelta(eventPointer);
    if (wheelDelta == 0)
    {
        // 某些触控板事件可能没有方向信息，此时不改变时间选区。
        QWidget::wheelEvent(eventPointer);
        return;
    }

    // selectionWidth100ns 是本次缩放前的选区宽度，用于计算缩放锚点比例。
    const std::uint64_t rangeWidth100ns = m_rangeEnd100ns - m_rangeStart100ns;
    const std::uint64_t selectionWidth100ns =
        m_selectionEnd100ns > m_selectionStart100ns
        ? (m_selectionEnd100ns - m_selectionStart100ns)
        : rangeWidth100ns;

    // 滚轮方向规则：
    // - 向上扩大选区；
    // - 向下缩小选区。
    const double scaleFactor = wheelDelta > 0 ? 1.20 : 0.80;
    std::uint64_t newWidth100ns = static_cast<std::uint64_t>(
        std::max<double>(
            static_cast<double>(kMinimumSelection100ns),
            static_cast<double>(selectionWidth100ns) * scaleFactor));
    newWidth100ns = std::min(newWidth100ns, rangeWidth100ns);

    // 缩放以鼠标所在时间点为锚点；鼠标不在选区内时比例会被夹到边界。
    const std::uint64_t anchorTime100ns = xToTime(currentWheelPosition(eventPointer).x());
    const double anchorRatio = selectionWidth100ns == 0
        ? 0.5
        : std::clamp(
            static_cast<double>(anchorTime100ns > m_selectionStart100ns
                ? anchorTime100ns - m_selectionStart100ns
                : 0)
            / static_cast<double>(selectionWidth100ns),
            0.0,
            1.0);

    const std::uint64_t leftPart100ns = static_cast<std::uint64_t>(
        static_cast<double>(newWidth100ns) * anchorRatio);
    m_selectionStart100ns = anchorTime100ns > leftPart100ns
        ? anchorTime100ns - leftPart100ns
        : m_rangeStart100ns;
    m_selectionEnd100ns = m_selectionStart100ns + newWidth100ns;

    // 滚轮缩放同样是用户主动选择时间窗口，需要立即叠加到事件表。
    m_userAdjustedSelection = true;
    clampSelectionToRange();
    update();
    notifySelectionChanged();
    eventPointer->accept();
}

QRectF ProcessTraceTimelineWidget::timelineRect() const
{
    // 高度和宽度最小夹到 1，防止极端布局阶段出现除零或空 QRectF。
    return QRectF(
        static_cast<double>(kHorizontalPadding),
        static_cast<double>(kVerticalPadding),
        static_cast<double>(std::max(1, width() - kHorizontalPadding * 2)),
        static_cast<double>(std::max(1, height() - kVerticalPadding * 2 - 1)));
}

QRectF ProcessTraceTimelineWidget::selectionRect() const
{
    // 无有效捕获范围或选区为空时，绘制层和命中测试都应视为没有选区。
    if (m_rangeEnd100ns <= m_rangeStart100ns || m_selectionEnd100ns <= m_selectionStart100ns)
    {
        return QRectF();
    }

    const QRectF axisRect = timelineRect();
    const double leftX = timeToX(m_selectionStart100ns);
    const double rightX = timeToX(m_selectionEnd100ns);
    return QRectF(
        QPointF(std::min(leftX, rightX), axisRect.top()),
        QPointF(std::max(leftX, rightX), axisRect.bottom()));
}

ProcessTraceTimelineWidget::DragMode ProcessTraceTimelineWidget::hitTestSelection(const QPoint& position) const
{
    // 命中测试只关注当前选区矩形，不读取事件点，因此不会从图形反推事件。
    const QRectF selectedRect = selectionRect();
    if (selectedRect.isEmpty())
    {
        return DragMode::None;
    }

    const QRectF expandedRect = selectedRect.adjusted(
        -static_cast<double>(kEdgeHitWidth),
        0.0,
        static_cast<double>(kEdgeHitWidth),
        0.0);
    // expandedRect 用于扩大可点击区域，但真正移动模式仍要求点在选区内部。
    if (!expandedRect.contains(position))
    {
        return DragMode::None;
    }

    if (std::abs(position.x() - selectedRect.left()) <= kEdgeHitWidth)
    {
        return DragMode::ResizeLeft;
    }
    if (std::abs(position.x() - selectedRect.right()) <= kEdgeHitWidth)
    {
        return DragMode::ResizeRight;
    }
    return selectedRect.contains(position) ? DragMode::Move : DragMode::None;
}

void ProcessTraceTimelineWidget::updateHoverCursor(const QPoint& position)
{
    // 光标只反映当前可执行操作，不改变任何内部状态。
    const DragMode hitMode = hitTestSelection(position);
    if (hitMode == DragMode::ResizeLeft || hitMode == DragMode::ResizeRight)
    {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    if (hitMode == DragMode::Move)
    {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    unsetCursor();
}

double ProcessTraceTimelineWidget::timeToX(const std::uint64_t time100ns) const
{
    // 无有效时间范围时返回左边界，保证调用方仍能完成绘制。
    const QRectF axisRect = timelineRect();
    if (m_rangeEnd100ns <= m_rangeStart100ns)
    {
        return axisRect.left();
    }

    const std::uint64_t clampedTime100ns = std::clamp(
        time100ns,
        m_rangeStart100ns,
        m_rangeEnd100ns);
    // ratio 是时间在完整捕获范围中的相对位置。
    const double ratio = static_cast<double>(clampedTime100ns - m_rangeStart100ns)
        / static_cast<double>(m_rangeEnd100ns - m_rangeStart100ns);
    return axisRect.left() + axisRect.width() * ratio;
}

std::uint64_t ProcessTraceTimelineWidget::xToTime(const double xValue) const
{
    // 坐标转时间仅服务于选区交互，返回值会被夹在捕获范围内。
    const QRectF axisRect = timelineRect();
    if (m_rangeEnd100ns <= m_rangeStart100ns || axisRect.width() <= 0.0)
    {
        return m_rangeStart100ns;
    }

    const double ratio = std::clamp(
        (xValue - axisRect.left()) / axisRect.width(),
        0.0,
        1.0);
    // offset100ns 是相对起点的时间偏移。
    const double offset100ns = static_cast<double>(m_rangeEnd100ns - m_rangeStart100ns) * ratio;
    return m_rangeStart100ns + static_cast<std::uint64_t>(offset100ns);
}

int ProcessTraceTimelineWidget::laneForType(const QString& typeText) const
{
    // 行号与事件类型固定绑定，保证同类事件在不同刷新周期中不会跳行。
    const QString normalizedText = typeText.trimmed();
    if (normalizedText == QStringLiteral("进程"))
    {
        return 0;
    }
    if (normalizedText == QStringLiteral("线程") || normalizedText == QStringLiteral("镜像"))
    {
        return normalizedText == QStringLiteral("线程") ? 1 : 2;
    }
    if (normalizedText == QStringLiteral("文件"))
    {
        return 3;
    }
    if (normalizedText == QStringLiteral("注册表"))
    {
        return 4;
    }
    if (normalizedText == QStringLiteral("网络") || normalizedText == QStringLiteral("DNS"))
    {
        return normalizedText == QStringLiteral("网络") ? 5 : 6;
    }
    if (normalizedText == QStringLiteral("PowerShell") || normalizedText == QStringLiteral("WMI"))
    {
        return normalizedText == QStringLiteral("PowerShell") ? 7 : 8;
    }
    if (normalizedText == QStringLiteral("计划任务") || normalizedText == QStringLiteral("安全审计"))
    {
        return normalizedText == QStringLiteral("计划任务") ? 9 : 10;
    }
    if (normalizedText == QStringLiteral("Defender"))
    {
        return 11;
    }
    return 12;
}

QColor ProcessTraceTimelineWidget::colorForType(const QString& typeText) const
{
    // 颜色只编码事件大类；透明度统一在函数末尾设置为 20%。
    QColor colorValue;
    const QString normalizedText = typeText.trimmed();
    if (normalizedText == QStringLiteral("进程"))
    {
        colorValue = QColor(76, 175, 80);
    }
    else if (normalizedText == QStringLiteral("线程"))
    {
        colorValue = QColor(139, 195, 74);
    }
    else if (normalizedText == QStringLiteral("镜像"))
    {
        colorValue = QColor(0, 188, 212);
    }
    else if (normalizedText == QStringLiteral("文件"))
    {
        colorValue = QColor(33, 150, 243);
    }
    else if (normalizedText == QStringLiteral("注册表"))
    {
        colorValue = QColor(156, 39, 176);
    }
    else if (normalizedText == QStringLiteral("网络"))
    {
        colorValue = QColor(255, 152, 0);
    }
    else if (normalizedText == QStringLiteral("DNS"))
    {
        colorValue = QColor(255, 193, 7);
    }
    else if (normalizedText == QStringLiteral("PowerShell"))
    {
        colorValue = QColor(63, 81, 181);
    }
    else if (normalizedText == QStringLiteral("WMI"))
    {
        colorValue = QColor(0, 150, 136);
    }
    else if (normalizedText == QStringLiteral("安全审计"))
    {
        colorValue = QColor(244, 67, 54);
    }
    else if (normalizedText == QStringLiteral("Defender"))
    {
        colorValue = QColor(121, 85, 72);
    }
    else
    {
        colorValue = QColor(96, 125, 139);
    }

    colorValue.setAlpha(51);
    return colorValue;
}

QString ProcessTraceTimelineWidget::formatDurationText(const std::uint64_t duration100ns) const
{
    // ETW 时间戳单位为 100ns，这里先折算到秒再格式化。
    const std::uint64_t totalSeconds = duration100ns / (1000ULL * 1000ULL * 10ULL);
    const std::uint64_t hours = totalSeconds / 3600ULL;
    const std::uint64_t minutes = (totalSeconds % 3600ULL) / 60ULL;
    const std::uint64_t seconds = totalSeconds % 60ULL;

    if (hours > 0)
    {
        // 超过一小时时展示 hh:mm:ss，避免右侧标签丢失小时信息。
        return QStringLiteral("%1:%2:%3")
            .arg(static_cast<qulonglong>(hours), 2, 10, QChar(u'0'))
            .arg(static_cast<qulonglong>(minutes), 2, 10, QChar(u'0'))
            .arg(static_cast<qulonglong>(seconds), 2, 10, QChar(u'0'));
    }

    // 一小时内展示 mm:ss，符合左侧固定 00:00 的短时间阅读习惯。
    return QStringLiteral("%1:%2")
        .arg(static_cast<qulonglong>(minutes), 2, 10, QChar(u'0'))
        .arg(static_cast<qulonglong>(seconds), 2, 10, QChar(u'0'));
}

void ProcessTraceTimelineWidget::clampSelectionToRange()
{
    // 捕获范围无效时直接同步到范围端点，避免保留陈旧选区。
    if (m_rangeEnd100ns <= m_rangeStart100ns)
    {
        m_selectionStart100ns = m_rangeStart100ns;
        m_selectionEnd100ns = m_rangeEnd100ns;
        return;
    }

    if (m_selectionStart100ns > m_selectionEnd100ns)
    {
        // 左右边缘拖过头时允许交换，随后再按最小宽度修正。
        std::swap(m_selectionStart100ns, m_selectionEnd100ns);
    }

    // 最小宽度不能超过整个捕获范围。
    const std::uint64_t rangeWidth100ns = m_rangeEnd100ns - m_rangeStart100ns;
    const std::uint64_t minimumWidth100ns = std::min(kMinimumSelection100ns, rangeWidth100ns);

    m_selectionStart100ns = std::clamp(
        m_selectionStart100ns,
        m_rangeStart100ns,
        m_rangeEnd100ns);
    m_selectionEnd100ns = std::clamp(
        m_selectionEnd100ns,
        m_rangeStart100ns,
        m_rangeEnd100ns);

    if (m_selectionEnd100ns >= m_selectionStart100ns
        && (m_selectionEnd100ns - m_selectionStart100ns) >= minimumWidth100ns)
    {
        // 选区已经合法时无需额外移动边界。
        return;
    }

    if (m_dragMode == DragMode::ResizeLeft)
    {
        // 左边缘拉伸过窄时优先保持右边缘不动。
        m_selectionStart100ns = m_selectionEnd100ns > minimumWidth100ns
            ? m_selectionEnd100ns - minimumWidth100ns
            : m_rangeStart100ns;
    }
    else
    {
        // 其他场景优先保持左边缘不动并扩展右边缘。
        m_selectionEnd100ns = m_selectionStart100ns + minimumWidth100ns;
    }

    if (m_selectionEnd100ns > m_rangeEnd100ns)
    {
        // 修正右侧越界后，反向移动左侧以继续满足最小宽度。
        m_selectionEnd100ns = m_rangeEnd100ns;
        m_selectionStart100ns = m_selectionEnd100ns > minimumWidth100ns
            ? m_selectionEnd100ns - minimumWidth100ns
            : m_rangeStart100ns;
    }
}

void ProcessTraceTimelineWidget::notifySelectionChanged()
{
    // 回调为空时只更新自身绘制状态；宿主可选择不绑定筛选逻辑。
    if (m_selectionChangedCallback)
    {
        m_selectionChangedCallback(m_selectionStart100ns, m_selectionEnd100ns);
    }
}
