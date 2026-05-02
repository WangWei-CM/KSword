#include "DiskMapWidget.h"

// ============================================================
// DiskMapWidget.cpp
// 作用：
// 1) 绘制横向磁盘分区柱形图；
// 2) 保持分区起始偏移、长度和未分配区域的视觉比例；
// 3) 为上层 DiskEditorTab 提供点击联动和悬停提示。
// ============================================================

#include "../../theme.h"

#include <QBrush>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>
#include <QToolTip>

#include <algorithm>

namespace
{
    // kOuterMargin：柱形图外边距，预留标题和阴影空间。
    constexpr int kOuterMargin = 14;

    // kHeaderHeight：顶部磁盘摘要区域高度。
    constexpr int kHeaderHeight = 28;

    // kMapHeight：横向分区条主体高度。
    constexpr int kMapHeight = 72;

    // kMinVisibleSegmentWidth：极小分区仍保留的最小点击宽度。
    constexpr int kMinVisibleSegmentWidth = 7;

    // formatBytesForDiskMap：
    // - 把字节数转为简短容量文本；
    // - bytes 为原始字节数；
    // - 返回类似 512.00 GB 的字符串。
    QString formatBytesForDiskMap(const std::uint64_t bytes)
    {
        const double value = static_cast<double>(bytes);
        if (bytes >= 1024ULL * 1024ULL * 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 TB").arg(value / 1099511627776.0, 0, 'f', 2);
        }
        if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 GB").arg(value / 1073741824.0, 0, 'f', 2);
        }
        if (bytes >= 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 MB").arg(value / 1048576.0, 0, 'f', 2);
        }
        if (bytes >= 1024ULL)
        {
            return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 2);
        }
        return QStringLiteral("%1 B").arg(static_cast<qulonglong>(bytes));
    }

    // darkerColor：
    // - 生成分区边框和渐变下缘颜色；
    // - color 为基础颜色；
    // - 返回略暗的 QColor。
    QColor darkerColor(const QColor& color)
    {
        QColor result = color.darker(KswordTheme::IsDarkModeEnabled() ? 130 : 112);
        result.setAlpha(255);
        return result;
    }
}

namespace ks::misc
{
    DiskMapWidget::DiskMapWidget(QWidget* parent)
        : QWidget(parent)
    {
        // 鼠标跟踪用于实时 tooltip 和悬停描边。
        setMouseTracking(true);
        setMinimumHeight(kOuterMargin * 2 + kHeaderHeight + kMapHeight + 22);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void DiskMapWidget::setDisk(const DiskDeviceInfo& diskInfo)
    {
        // 保存快照，避免外部 vector 生命周期影响绘制。
        m_diskInfo = diskInfo;
        m_hasDisk = true;
        m_selectedPartitionIndex = -1;
        m_hoverPartitionIndex = -1;
        update();
    }

    void DiskMapWidget::clearDisk()
    {
        // 清空状态后保持占位绘制，避免页面突然塌陷。
        m_diskInfo = DiskDeviceInfo{};
        m_hasDisk = false;
        m_selectedPartitionIndex = -1;
        m_hoverPartitionIndex = -1;
        update();
    }

    void DiskMapWidget::setSelectedPartitionIndex(const int partitionIndex)
    {
        if (m_selectedPartitionIndex == partitionIndex)
        {
            return;
        }
        m_selectedPartitionIndex = partitionIndex;
        update();
    }

    void DiskMapWidget::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), Qt::transparent);

        const QRect cardRect = rect().adjusted(6, 4, -6, -4);
        painter.setPen(QPen(KswordTheme::BorderColor(), 1));
        painter.setBrush(KswordTheme::SurfaceColor());
        painter.drawRoundedRect(cardRect, 10, 10);

        QFont titleFont = painter.font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(KswordTheme::TextPrimaryColor());

        const QString titleText = m_hasDisk
            ? QStringLiteral("%1  ·  %2  ·  %3")
                .arg(m_diskInfo.displayName.isEmpty() ? m_diskInfo.devicePath : m_diskInfo.displayName)
                .arg(formatBytesForDiskMap(m_diskInfo.sizeBytes))
                .arg(m_diskInfo.busType.isEmpty() ? QStringLiteral("未知总线") : m_diskInfo.busType)
            : QStringLiteral("尚未选择磁盘");
        painter.drawText(
            cardRect.adjusted(kOuterMargin, 8, -kOuterMargin, 0),
            Qt::AlignLeft | Qt::AlignTop,
            titleText);

        const QRect mapFrame(
            cardRect.left() + kOuterMargin,
            cardRect.top() + kHeaderHeight + 10,
            cardRect.width() - kOuterMargin * 2,
            kMapHeight);

        painter.setPen(QPen(KswordTheme::BorderStrongColor(), 1));
        painter.setBrush(KswordTheme::SurfaceAltColor());
        painter.drawRoundedRect(mapFrame, 8, 8);

        if (!m_hasDisk || m_diskInfo.sizeBytes == 0)
        {
            painter.setPen(KswordTheme::TextSecondaryColor());
            painter.drawText(mapFrame, Qt::AlignCenter, QStringLiteral("无法读取磁盘布局，请刷新或以管理员权限运行"));
            return;
        }

        const std::vector<PaintSegment> segments = rebuildPaintSegments();
        for (const PaintSegment& segment : segments)
        {
            const bool selected = segment.partition.tableIndex == m_selectedPartitionIndex;
            const bool hovered = segment.partition.tableIndex == m_hoverPartitionIndex;
            const QColor baseColor = segment.partition.color.isValid()
                ? segment.partition.color
                : KswordTheme::PrimaryBlueColor;

            QLinearGradient gradient(segment.rect.topLeft(), segment.rect.bottomLeft());
            gradient.setColorAt(0.0, baseColor.lighter(KswordTheme::IsDarkModeEnabled() ? 115 : 130));
            gradient.setColorAt(0.58, baseColor);
            gradient.setColorAt(1.0, darkerColor(baseColor));

            painter.setPen(QPen(selected || hovered ? KswordTheme::PrimaryBlueColor : darkerColor(baseColor),
                selected ? 3 : (hovered ? 2 : 1)));
            painter.setBrush(QBrush(gradient));
            painter.drawRoundedRect(segment.rect, 6, 6);

            const QString labelText = segment.partition.name.isEmpty()
                ? segment.partition.typeText
                : segment.partition.name;
            const QString sizeText = formatBytesForDiskMap(segment.partition.lengthBytes);
            const QString visibleText = labelText.isEmpty()
                ? sizeText
                : QStringLiteral("%1\n%2").arg(labelText, sizeText);

            painter.setPen(Qt::white);
            QFont segmentFont = painter.font();
            segmentFont.setBold(true);
            painter.setFont(segmentFont);

            const QFontMetrics metrics(segmentFont);
            if (segment.rect.width() >= 68)
            {
                painter.drawText(segment.rect.adjusted(5, 4, -5, -4), Qt::AlignCenter, visibleText);
            }
            else if (segment.rect.width() >= 28)
            {
                const QString shortText = metrics.elidedText(labelText, Qt::ElideRight, segment.rect.width() - 4);
                painter.drawText(segment.rect.adjusted(2, 0, -2, 0), Qt::AlignCenter, shortText);
            }
        }

        painter.setFont(QWidget::font());
        painter.setPen(KswordTheme::TextSecondaryColor());
        const QString footerText = QStringLiteral("0  ·  扇区 %1 B  ·  共 %2 个分区块  ·  末尾 %3")
            .arg(m_diskInfo.bytesPerSector)
            .arg(static_cast<int>(segments.size()))
            .arg(formatBytesForDiskMap(m_diskInfo.sizeBytes));
        painter.drawText(
            QRect(mapFrame.left(), mapFrame.bottom() + 7, mapFrame.width(), 18),
            Qt::AlignLeft | Qt::AlignVCenter,
            footerText);
    }

    void DiskMapWidget::mousePressEvent(QMouseEvent* event)
    {
        if (event == nullptr || event->button() != Qt::LeftButton)
        {
            QWidget::mousePressEvent(event);
            return;
        }

        const int partitionIndex = hitTest(event->pos());
        if (partitionIndex >= 0)
        {
            m_selectedPartitionIndex = partitionIndex;
            update();
            emit partitionActivated(partitionIndex);
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void DiskMapWidget::mouseMoveEvent(QMouseEvent* event)
    {
        if (event == nullptr)
        {
            return;
        }

        const std::vector<PaintSegment> segments = rebuildPaintSegments();
        int nextHoverIndex = -1;
        QString tooltipText;
        for (const PaintSegment& segment : segments)
        {
            if (segment.rect.contains(event->pos()))
            {
                nextHoverIndex = segment.partition.tableIndex;
                tooltipText = segmentTooltipText(segment);
                break;
            }
        }

        if (m_hoverPartitionIndex != nextHoverIndex)
        {
            m_hoverPartitionIndex = nextHoverIndex;
            update();
        }

        if (!tooltipText.isEmpty())
        {
            // Qt6 已弃用 globalPos()，这里使用 globalPosition() 并转回 QPoint 供 QToolTip 使用。
            QToolTip::showText(event->globalPosition().toPoint(), tooltipText, this);
        }
        else
        {
            QToolTip::hideText();
        }
    }

    void DiskMapWidget::leaveEvent(QEvent* event)
    {
        Q_UNUSED(event);
        m_hoverPartitionIndex = -1;
        QToolTip::hideText();
        update();
    }

    std::vector<DiskMapWidget::PaintSegment> DiskMapWidget::rebuildPaintSegments() const
    {
        std::vector<PaintSegment> segments;
        if (!m_hasDisk || m_diskInfo.sizeBytes == 0)
        {
            return segments;
        }

        QRect mapRect(
            6 + kOuterMargin,
            4 + kHeaderHeight + 10,
            width() - 12 - kOuterMargin * 2,
            kMapHeight);
        mapRect = mapRect.adjusted(5, 5, -5, -5);
        if (mapRect.width() <= 0 || mapRect.height() <= 0)
        {
            return segments;
        }

        for (const DiskPartitionInfo& partition : m_diskInfo.partitions)
        {
            if (partition.lengthBytes == 0 || partition.offsetBytes >= m_diskInfo.sizeBytes)
            {
                continue;
            }

            const std::uint64_t clampedEnd =
                std::min<std::uint64_t>(m_diskInfo.sizeBytes, partition.offsetBytes + partition.lengthBytes);
            const double startRatio =
                static_cast<double>(partition.offsetBytes) / static_cast<double>(m_diskInfo.sizeBytes);
            const double endRatio =
                static_cast<double>(clampedEnd) / static_cast<double>(m_diskInfo.sizeBytes);

            int left = mapRect.left() + static_cast<int>(startRatio * static_cast<double>(mapRect.width()));
            int right = mapRect.left() + static_cast<int>(endRatio * static_cast<double>(mapRect.width()));
            if (right <= left)
            {
                right = left + kMinVisibleSegmentWidth;
            }
            if (right > mapRect.right())
            {
                right = mapRect.right();
            }

            QRect segmentRect(left, mapRect.top(), std::max(kMinVisibleSegmentWidth, right - left), mapRect.height());
            segmentRect = segmentRect.adjusted(1, 1, -1, -1);
            if (segmentRect.right() > mapRect.right())
            {
                segmentRect.setRight(mapRect.right() - 1);
            }

            segments.push_back(PaintSegment{ segmentRect, partition });
        }

        return segments;
    }

    int DiskMapWidget::hitTest(const QPoint& point) const
    {
        const std::vector<PaintSegment> segments = rebuildPaintSegments();
        for (const PaintSegment& segment : segments)
        {
            if (segment.rect.contains(point))
            {
                return segment.partition.tableIndex;
            }
        }
        return -1;
    }

    QString DiskMapWidget::segmentTooltipText(const PaintSegment& segment)
    {
        const DiskPartitionInfo& partition = segment.partition;
        return QStringLiteral("%1\n类型: %2\n容量: %3\n偏移: 0x%4\n长度: 0x%5\n标记: %6")
            .arg(partition.name.isEmpty() ? QStringLiteral("未命名分区") : partition.name)
            .arg(partition.typeText.isEmpty() ? QStringLiteral("未知") : partition.typeText)
            .arg(formatBytesForDiskMap(partition.lengthBytes))
            .arg(static_cast<qulonglong>(partition.offsetBytes), 16, 16, QChar('0'))
            .arg(static_cast<qulonglong>(partition.lengthBytes), 16, 16, QChar('0'))
            .arg(partition.flagsText.isEmpty() ? QStringLiteral("-") : partition.flagsText)
            .toUpper();
    }
}
