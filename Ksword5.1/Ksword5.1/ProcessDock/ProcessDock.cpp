#include "ProcessDock.h"

#include "../theme.h"
#include "ProcessDetailWindow.h"
#include "../ArkDriverClient/ArkDriverClient.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDoubleValidator>
#include <QEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointF>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>
#include <QRectF>
#include <QRunnable>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyledItemDelegate>
#include <QSlider>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextEdit>
#include <QThreadPool>
#include <QTimer>
#include <QToolTip>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

namespace
{
    // 列标题文本常量，索引与 ProcessDock::TableColumn 一一对应。
    const QStringList ProcessTableHeaders{
        "进程名",
        "PID",
        "CPU",
        "RAM",
        "DISK",
        "GPU",
        "Net",
        "数字签名",
        "路径",
        "父进程",
        "启动参数",
        "用户",
        "启动时间",
        "管理员",
        "PPL保护级别",
        "保护状态",
        "PPL",
        "句柄数",
        "HandleTable",
        "SectionObject",
        "R0状态"
    };

    // 常用图标路径常量（全部来自 qrc 的 /Icon 前缀资源）。
    constexpr const char* IconProcessMain = ":/Icon/process_main.svg";
    constexpr const char* IconRefresh = ":/Icon/process_refresh.svg";
    constexpr const char* IconTree = ":/Icon/process_tree.svg";
    constexpr const char* IconList = ":/Icon/process_list.svg";
    constexpr const char* IconStart = ":/Icon/process_start.svg";
    constexpr const char* IconPause = ":/Icon/process_pause.svg";
    constexpr const char* IconThreadTab = ":/Icon/process_threads.svg";
    constexpr const char* IconWindowPickerTarget = ":/Icon/window_picker_target.svg";
    // Kernel 标识图路径：
    // - 使用 qrc 资源路径，避免把开发机绝对路径写入源码；
    // - 实际文件仍由 Ksword5.qrc 映射到 Resource/Kernel.png。
    const QString KernelBadgeImagePath = QStringLiteral(
        ":/Image/kernel_badge.png");

    // 默认按钮图标尺寸。
    constexpr QSize DefaultIconSize(18, 18);
    constexpr QSize SideTabIconSize(22, 22);
    constexpr QSize CompactIconButtonSize(28, 28);
    constexpr int ProcessSideTabMinHeightPx = 66;
    constexpr int ProcessNumericSortRole = Qt::UserRole + 200;
    constexpr int ProcessEfficiencyModeRole = Qt::UserRole + 201;
    constexpr int ProcessEfficiencyModeKnownRole = Qt::UserRole + 202;
    constexpr int ActivityMinimumIntervalMilliseconds = 50;
    constexpr int ActivityMaximumIntervalMilliseconds = 60000;
    constexpr int ProcessTableMinimumIntervalMilliseconds = 500;
    constexpr int ProcessTableMaximumIntervalMilliseconds = 60000;
    constexpr std::size_t ActivityMaximumSampleCount = 1800;

    // clampPercentValue 作用：
    // - 把所有图表输入值统一夹到 0~100；
    // - 折线图只绘制百分比，避免不同单位直接叠加造成误读。
    double clampPercentValue(const double percentValue)
    {
        if (!std::isfinite(percentValue))
        {
            return 0.0;
        }
        return std::clamp(percentValue, 0.0, 100.0);
    }

    // totalPhysicalMemoryMB 作用：
    // - 读取系统物理内存总量；
    // - 用于把进程工作集 MB 转换成百分比。
    double totalPhysicalMemoryMB()
    {
        MEMORYSTATUSEX memoryStatus{};
        memoryStatus.dwLength = sizeof(memoryStatus);
        if (::GlobalMemoryStatusEx(&memoryStatus) == FALSE || memoryStatus.ullTotalPhys == 0ULL)
        {
            return 0.0;
        }
        return static_cast<double>(memoryStatus.ullTotalPhys) / (1024.0 * 1024.0);
    }

    // processActivityMetricText 作用：
    // - 将活动图内部指标枚举映射为 UI 显示名称；
    // - 返回值用于按钮、图例和悬停快照。
    QString processActivityMetricText(const ProcessDock::ProcessActivityMetric metric)
    {
        switch (metric)
        {
        case ProcessDock::ProcessActivityMetric::Cpu:
            return QStringLiteral("CPU");
        case ProcessDock::ProcessActivityMetric::Memory:
            return QStringLiteral("内存");
        case ProcessDock::ProcessActivityMetric::Disk:
            return QStringLiteral("磁盘");
        case ProcessDock::ProcessActivityMetric::Network:
            return QStringLiteral("网络");
        case ProcessDock::ProcessActivityMetric::Gpu:
            return QStringLiteral("GPU");
        default:
            return QStringLiteral("未知");
        }
    }

    // processActivityMetricColor 作用：
    // - 固定每个指标的主题色，避免用户切换按钮后颜色漂移；
    // - alpha 由调用方根据柱形/图例场景再做调整。
    QColor processActivityMetricColor(const ProcessDock::ProcessActivityMetric metric)
    {
        switch (metric)
        {
        case ProcessDock::ProcessActivityMetric::Cpu:
            return QColor(56, 132, 255);
        case ProcessDock::ProcessActivityMetric::Memory:
            return QColor(104, 196, 112);
        case ProcessDock::ProcessActivityMetric::Disk:
            return QColor(255, 172, 64);
        case ProcessDock::ProcessActivityMetric::Network:
            return QColor(92, 204, 230);
        case ProcessDock::ProcessActivityMetric::Gpu:
            return QColor(172, 112, 255);
        default:
            return QColor(120, 132, 148);
        }
    }

    // processActivityMetricUnit 作用：
    // - 返回指标单位文本；
    // - 悬停快照和图表标题复用同一套单位。
    QString processActivityMetricUnit(const ProcessDock::ProcessActivityMetric metric)
    {
        switch (metric)
        {
        case ProcessDock::ProcessActivityMetric::Cpu:
        case ProcessDock::ProcessActivityMetric::Memory:
        case ProcessDock::ProcessActivityMetric::Disk:
        case ProcessDock::ProcessActivityMetric::Network:
        case ProcessDock::ProcessActivityMetric::Gpu:
            return QStringLiteral("%");
        default:
            return QString();
        }
    }

    // formatActivityElapsedText 作用：
    // - 将记录相对毫秒转换为紧凑时间轴标签；
    // - 小于 1 秒时保留 0.1s 精度，便于 0.1s 打点场景定位。
    QString formatActivityElapsedText(const std::uint64_t elapsedMs)
    {
        if (elapsedMs < 1000U)
        {
            return QStringLiteral("%1s").arg(static_cast<double>(elapsedMs) / 1000.0, 0, 'f', 1);
        }

        const std::uint64_t totalSeconds = elapsedMs / 1000U;
        const std::uint64_t hours = totalSeconds / 3600U;
        const std::uint64_t minutes = (totalSeconds / 60U) % 60U;
        const std::uint64_t seconds = totalSeconds % 60U;
        if (hours > 0U)
        {
            return QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QChar('0'))
                .arg(seconds, 2, 10, QChar('0'));
        }
        return QStringLiteral("%1:%2")
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }

    // activityMousePosition 作用：
    // - 兼容 Qt5/Qt6 的鼠标坐标 API；
    // - 返回值是控件本地坐标。
    QPoint activityMousePosition(const QMouseEvent* eventPointer)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return eventPointer != nullptr ? eventPointer->position().toPoint() : QPoint();
#else
        return eventPointer != nullptr ? eventPointer->pos() : QPoint();
#endif
    }

    // themeColorFromText 作用：
    // - KswordTheme 的部分颜色可能来自 palette(...) 表达式；
    // - 绘图只能使用 QColor，所以无法解析时返回调用方给出的兜底色。
    QColor themeColorFromText(const QString& colorText, const QColor& fallbackColor)
    {
        const QColor parsedColor(colorText);
        return parsedColor.isValid() ? parsedColor : fallbackColor;
    }

    class ProcessSortTreeWidgetItem final : public QTreeWidgetItem
    {
    public:
        // operator< 作用：让进程表按隐藏数值键排序，而不是按展示字符串排序。
        bool operator<(const QTreeWidgetItem& otherItem) const override
        {
            const QTreeWidget* ownerTree = treeWidget();
            const int sortColumn = ownerTree != nullptr ? ownerTree->sortColumn() : 0;
            bool leftOk = false;
            bool rightOk = false;
            const double leftValue = data(sortColumn, ProcessNumericSortRole).toDouble(&leftOk);
            const double rightValue = otherItem.data(sortColumn, ProcessNumericSortRole).toDouble(&rightOk);
            if (leftOk && rightOk && leftValue != rightValue)
            {
                return leftValue < rightValue;
            }
            if (leftOk && rightOk)
            {
                return text(sortColumn).localeAwareCompare(otherItem.text(sortColumn)) < 0;
            }
            return QTreeWidgetItem::operator<(otherItem);
        }
    };

    // ProcessWindowPickerDragButton：
    // - 作用：复用窗口页“准星拖拽拾取”的输入模型；
    // - 按住按钮拖到任意窗口后松开，向宿主回调全局屏幕坐标；
    // - 只负责输入捕获，不直接读取 HWND/PID，避免 UI 控件承担业务逻辑。
    class ProcessWindowPickerDragButton final : public QPushButton
    {
    public:
        using ReleaseCallback = std::function<void(const QPoint&)>;

        // 构造函数：
        // - parent：Qt 父控件；
        // - 处理：启用鼠标追踪，允许拖拽过程中持续计算距离；
        // - 返回：无返回值。
        explicit ProcessWindowPickerDragButton(QWidget* parent = nullptr)
            : QPushButton(parent)
        {
            setMouseTracking(true);
        }

        // setReleaseCallback：
        // - callback：鼠标释放时接收全局坐标的函数；
        // - 处理：保存到成员变量，供 mouseReleaseEvent 调用；
        // - 返回：无返回值。
        void setReleaseCallback(ReleaseCallback callback)
        {
            m_releaseCallback = std::move(callback);
        }

    protected:
        // mousePressEvent：
        // - 输入：Qt 鼠标按下事件；
        // - 处理：记录左键起点并抓取鼠标，光标切换成准星；
        // - 返回：无返回值。
        void mousePressEvent(QMouseEvent* eventPointer) override
        {
            if (eventPointer != nullptr && eventPointer->button() == Qt::LeftButton)
            {
                m_dragTracking = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                m_pressGlobalPos = eventPointer->globalPosition().toPoint();
#else
                m_pressGlobalPos = eventPointer->globalPos();
#endif
                m_hasReachedDragThreshold = false;
                grabMouse(QCursor(Qt::CrossCursor));
            }
            QPushButton::mousePressEvent(eventPointer);
        }

        // mouseMoveEvent：
        // - 输入：Qt 鼠标移动事件；
        // - 处理：超过系统拖拽阈值后才允许释放触发，避免普通点击误拾取；
        // - 返回：无返回值。
        void mouseMoveEvent(QMouseEvent* eventPointer) override
        {
            if (m_dragTracking && eventPointer != nullptr)
            {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const QPoint currentGlobalPos = eventPointer->globalPosition().toPoint();
#else
                const QPoint currentGlobalPos = eventPointer->globalPos();
#endif
                const int moveDistance = (currentGlobalPos - m_pressGlobalPos).manhattanLength();
                if (moveDistance >= QApplication::startDragDistance())
                {
                    m_hasReachedDragThreshold = true;
                }
            }
            QPushButton::mouseMoveEvent(eventPointer);
        }

        // mouseReleaseEvent：
        // - 输入：Qt 鼠标释放事件；
        // - 处理：释放鼠标抓取，并在有效拖拽时回调释放坐标；
        // - 返回：无返回值。
        void mouseReleaseEvent(QMouseEvent* eventPointer) override
        {
            const bool shouldDispatch =
                m_dragTracking &&
                m_hasReachedDragThreshold &&
                eventPointer != nullptr &&
                eventPointer->button() == Qt::LeftButton;

            if (m_dragTracking)
            {
                releaseMouse();
                m_dragTracking = false;
            }
            m_hasReachedDragThreshold = false;

            QPoint releaseGlobalPos;
            if (eventPointer != nullptr)
            {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                releaseGlobalPos = eventPointer->globalPosition().toPoint();
#else
                releaseGlobalPos = eventPointer->globalPos();
#endif
            }

            QPushButton::mouseReleaseEvent(eventPointer);

            if (shouldDispatch && m_releaseCallback)
            {
                m_releaseCallback(releaseGlobalPos);
            }
        }

    private:
        bool m_dragTracking = false;             // m_dragTracking：当前是否处于准星拖拽链路。
        bool m_hasReachedDragThreshold = false;  // m_hasReachedDragThreshold：是否达到系统拖拽阈值。
        QPoint m_pressGlobalPos;                 // m_pressGlobalPos：左键按下时的全局坐标。
        ReleaseCallback m_releaseCallback;       // m_releaseCallback：释放后通知宿主处理 PID 过滤。
    };

}

class ProcessActivityChartWidget final : public QWidget
{
public:
    // 构造函数：
    // - ownerDock：提供样本、选择和指标开关；
    // - parent：Qt 父控件；
    // - 无返回值，初始化鼠标跟踪以支持悬停快照。
    explicit ProcessActivityChartWidget(ProcessDock* ownerDock, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_ownerDock(ownerDock)
    {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
        // 折线图不再自行铺底色，父级 Dock 背景图需要从图表空白区透出。
        setAutoFillBackground(false);
        setAttribute(Qt::WA_StyledBackground, false);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    // setFocusedSampleIndex：
    // - sampleIndex：时间轴当前定位的样本下标，-1 表示无焦点；
    // - 函数只更新绘制光标，不修改宿主滑块。
    void setFocusedSampleIndex(const int sampleIndex)
    {
        if (m_focusedSampleIndex == sampleIndex)
        {
            return;
        }
        m_focusedSampleIndex = sampleIndex;
        update();
    }

protected:
    // event：
    // - 输入：Qt 通用事件，重点处理 ToolTip 事件；
    // - 处理：在提示即将显示时重新按当前鼠标位置计算最近样本；
    // - 返回：true 表示 tooltip 已由控件接管，false 表示交给 QWidget 默认处理。
    bool event(QEvent* eventPointer) override
    {
        if (eventPointer != nullptr && eventPointer->type() == QEvent::ToolTip)
        {
            QHelpEvent* helpEvent = static_cast<QHelpEvent*>(eventPointer);
            showSnapshotToolTipAtPosition(helpEvent->pos(), helpEvent->globalPos());
            eventPointer->accept();
            return true;
        }
        return QWidget::event(eventPointer);
    }

    // paintEvent：
    // - 以时间为横轴绘制多指标百分比折线图；
    // - 无返回值，所有数据都从宿主的有界样本缓存读取。
    void paintEvent(QPaintEvent* eventPointer) override
    {
        (void)eventPointer;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // 背景保持透明：只绘制网格、边框和曲线，不用 Surface 色块覆盖 Dock 背景图。

        const QRectF plotRect = chartRect();
        const QColor borderColor = themeColorFromText(
            KswordTheme::BorderHex(),
            KswordTheme::IsDarkModeEnabled() ? QColor(76, 84, 96) : QColor(210, 216, 224));
        const QColor textColor = themeColorFromText(
            KswordTheme::TextSecondaryHex(),
            KswordTheme::IsDarkModeEnabled() ? QColor(180, 190, 205) : QColor(88, 96, 108));

        painter.setPen(QPen(borderColor, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plotRect);

        if (m_ownerDock == nullptr || m_ownerDock->m_activitySamples.empty())
        {
            painter.setPen(textColor);
            painter.drawText(plotRect, Qt::AlignCenter, QStringLiteral("未开始刷新进程列表活动"));
            return;
        }

        std::vector<ProcessDock::ProcessActivityMetric> enabledMetrics = enabledMetricList();
        if (enabledMetrics.empty())
        {
            painter.setPen(textColor);
            painter.drawText(plotRect, Qt::AlignCenter, QStringLiteral("未选择任何指标，请勾选 CPU / 内存 / 磁盘 / 网络 / GPU"));
            return;
        }

        std::vector<std::string> selectionKeys = m_ownerDock->currentProcessActivitySelectionKeys();
        const MetricScale metricScale = calculateMetricScale(enabledMetrics, selectionKeys);

        drawGrid(painter, plotRect, borderColor, textColor);
        drawLines(painter, plotRect, enabledMetrics, selectionKeys, metricScale);
        drawLegend(painter, enabledMetrics, textColor);
        drawFocusLine(painter, plotRect);
    }

    // mouseMoveEvent：
    // - 将鼠标 X 坐标映射为最近样本；
    // - 通知宿主更新时间轴滑块与快照标签。
    void mouseMoveEvent(QMouseEvent* eventPointer) override
    {
        if (eventPointer == nullptr || m_ownerDock == nullptr || m_ownerDock->m_activitySamples.empty())
        {
            QWidget::mouseMoveEvent(eventPointer);
            return;
        }

        const int sampleIndex = sampleIndexAtX(activityMousePosition(eventPointer).x());
        if (sampleIndex >= 0)
        {
            const bool oldPinnedToLatest = m_ownerDock->m_activityTimelinePinnedToLatest;
            m_ownerDock->previewProcessActivitySnapshotForIndex(sampleIndex);
            m_ownerDock->m_activityTimelinePinnedToLatest = oldPinnedToLatest;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint globalPosition = eventPointer->globalPosition().toPoint();
#else
            const QPoint globalPosition = eventPointer->globalPos();
#endif
            showSnapshotToolTipAtPosition(activityMousePosition(eventPointer), globalPosition);
        }
        eventPointer->accept();
    }

    // mousePressEvent：
    // - 单击图表时将时间轴固定到对应历史样本；
    // - 若点击最右侧样本，则恢复“吸附最新”模式。
    void mousePressEvent(QMouseEvent* eventPointer) override
    {
        if (eventPointer == nullptr ||
            eventPointer->button() != Qt::LeftButton ||
            m_ownerDock == nullptr ||
            m_ownerDock->m_activitySamples.empty())
        {
            QWidget::mousePressEvent(eventPointer);
            return;
        }

        const int sampleIndex = sampleIndexAtX(activityMousePosition(eventPointer).x());
        if (sampleIndex >= 0)
        {
            // 图表本身现在就是唯一时间轴：
            // - 左键点击提交历史时刻；
            // - 提交会同步更新下方进程表到对应快照。
            m_ownerDock->commitProcessActivityTimelineIndex(sampleIndex);
            eventPointer->accept();
            return;
        }
        QWidget::mousePressEvent(eventPointer);
    }

    // leaveEvent：
    // - 鼠标离开图表后保留当前时间轴位置；
    // - 仅隐藏 tooltip，避免用户读下方快照时被清空。
    void leaveEvent(QEvent* eventPointer) override
    {
        QToolTip::hideText();
        QWidget::leaveEvent(eventPointer);
    }

private:
    // showSnapshotToolTipAtPosition：
    // - localPosition：图表本地坐标，用于映射最近样本；
    // - globalPosition：屏幕坐标，用于放置 tooltip；
    // - 返回：无；没有样本时主动隐藏，避免显示控件静态 tooltip。
    void showSnapshotToolTipAtPosition(const QPoint& localPosition, const QPoint& globalPosition)
    {
        if (m_ownerDock == nullptr || m_ownerDock->m_activitySamples.empty())
        {
            QToolTip::hideText();
            return;
        }

        const int sampleIndex = sampleIndexAtX(localPosition.x());
        if (sampleIndex < 0)
        {
            QToolTip::hideText();
            return;
        }

        const QRect toolTipRect(localPosition - QPoint(6, 6), QSize(12, 12));
        QToolTip::showText(
            globalPosition,
            m_ownerDock->buildProcessActivitySnapshotText(sampleIndex),
            this,
            toolTipRect);
    }

    // chartRect：
    // - 计算图表实际绘制区域；
    // - 给左侧刻度和底部时间标签预留固定空间。
    QRectF chartRect() const
    {
        return QRectF(rect()).adjusted(42.0, 8.0, -10.0, -24.0);
    }

    // enabledMetricList：
    // - 从宿主按钮状态读取当前可见指标；
    // - 返回顺序即折线绘制顺序。
    std::vector<ProcessDock::ProcessActivityMetric> enabledMetricList() const
    {
        std::vector<ProcessDock::ProcessActivityMetric> metricList;
        if (m_ownerDock == nullptr)
        {
            return metricList;
        }
        const ProcessDock::ProcessActivityMetric allMetrics[] = {
            ProcessDock::ProcessActivityMetric::Cpu,
            ProcessDock::ProcessActivityMetric::Memory,
            ProcessDock::ProcessActivityMetric::Disk,
            ProcessDock::ProcessActivityMetric::Network,
            ProcessDock::ProcessActivityMetric::Gpu
        };
        for (const ProcessDock::ProcessActivityMetric metric : allMetrics)
        {
            if (m_ownerDock->isProcessActivityMetricEnabled(metric))
            {
                metricList.push_back(metric);
            }
        }
        return metricList;
    }

    // MetricScale 作用：
    // - 保存折线图百分比归一化分母；
    // - 磁盘/网络使用当前历史窗口最大值作为 100%。
    struct MetricScale
    {
        double memoryDenominatorMB = 1.0;     // memoryDenominatorMB：物理内存总量或历史最大内存。
        double diskDenominatorMBps = 1.0;     // diskDenominatorMBps：历史最大磁盘吞吐。
        double networkDenominatorKBps = 1.0;  // networkDenominatorKBps：历史最大网络吞吐。
    };

    // sampleRawMetricValue：
    // - 读取某个采样点的原始单项指标；
    // - selectionKeys 为空时返回总体聚合，否则返回选中进程之和。
    double sampleRawMetricValue(
        const ProcessDock::ProcessActivitySample& sample,
        const ProcessDock::ProcessActivityMetric metric,
        const std::vector<std::string>& selectionKeys) const
    {
        if (selectionKeys.empty())
        {
            switch (metric)
            {
            case ProcessDock::ProcessActivityMetric::Cpu:
                return sample.totalCpuPercent;
            case ProcessDock::ProcessActivityMetric::Memory:
                return sample.totalMemoryMB;
            case ProcessDock::ProcessActivityMetric::Disk:
                return sample.totalDiskMBps;
            case ProcessDock::ProcessActivityMetric::Network:
                return sample.totalNetKBps;
            case ProcessDock::ProcessActivityMetric::Gpu:
                return sample.totalGpuPercent;
            default:
                return 0.0;
            }
        }

        double value = 0.0;
        for (const ProcessDock::ProcessActivityProcessPoint& processPoint : sample.processes)
        {
            if (std::find(selectionKeys.begin(), selectionKeys.end(), processPoint.identityKey) == selectionKeys.end())
            {
                continue;
            }
            switch (metric)
            {
            case ProcessDock::ProcessActivityMetric::Cpu:
                value += processPoint.cpuPercent;
                break;
            case ProcessDock::ProcessActivityMetric::Memory:
                value += processPoint.memoryMB;
                break;
            case ProcessDock::ProcessActivityMetric::Disk:
                value += processPoint.diskMBps;
                break;
            case ProcessDock::ProcessActivityMetric::Network:
                value += processPoint.netKBps;
                break;
            case ProcessDock::ProcessActivityMetric::Gpu:
                value += processPoint.gpuPercent;
                break;
            default:
                break;
            }
        }
        return value;
    }

    // calculateMetricScale：
    // - 每次绘制都重新扫描历史最大值；
    // - 当磁盘/网络出现新峰值时，本轮立刻重算百分比并重绘。
    MetricScale calculateMetricScale(
        const std::vector<ProcessDock::ProcessActivityMetric>& metricList,
        const std::vector<std::string>& selectionKeys) const
    {
        MetricScale scale{};
        if (m_ownerDock == nullptr)
        {
            return scale;
        }

        scale.memoryDenominatorMB = std::max(1.0, m_ownerDock->m_activityTotalPhysicalMemoryMB);
        double maxMemoryMB = 0.0;
        double maxDiskMBps = 0.0;
        double maxNetworkKBps = 0.0;
        for (const ProcessDock::ProcessActivitySample& sample : m_ownerDock->m_activitySamples)
        {
            maxMemoryMB = std::max(maxMemoryMB, sampleRawMetricValue(sample, ProcessDock::ProcessActivityMetric::Memory, selectionKeys));
            maxDiskMBps = std::max(maxDiskMBps, sampleRawMetricValue(sample, ProcessDock::ProcessActivityMetric::Disk, selectionKeys));
            maxNetworkKBps = std::max(maxNetworkKBps, sampleRawMetricValue(sample, ProcessDock::ProcessActivityMetric::Network, selectionKeys));
        }
        if (scale.memoryDenominatorMB <= 1.0 && maxMemoryMB > 0.0)
        {
            scale.memoryDenominatorMB = maxMemoryMB;
        }
        scale.diskDenominatorMBps = std::max(1.0, maxDiskMBps);
        scale.networkDenominatorKBps = std::max(1.0, maxNetworkKBps);
        (void)metricList;
        return scale;
    }

    // samplePercentMetricValue：
    // - 把原始指标统一转换为百分比；
    // - 磁盘/网络按历史最大值归一化，CPU/GPU 天然是百分比。
    double samplePercentMetricValue(
        const ProcessDock::ProcessActivitySample& sample,
        const ProcessDock::ProcessActivityMetric metric,
        const std::vector<std::string>& selectionKeys,
        const MetricScale& scale) const
    {
        const double rawValue = sampleRawMetricValue(sample, metric, selectionKeys);
        switch (metric)
        {
        case ProcessDock::ProcessActivityMetric::Cpu:
        case ProcessDock::ProcessActivityMetric::Gpu:
            return clampPercentValue(rawValue);
        case ProcessDock::ProcessActivityMetric::Memory:
            return clampPercentValue((rawValue / std::max(1.0, scale.memoryDenominatorMB)) * 100.0);
        case ProcessDock::ProcessActivityMetric::Disk:
            return clampPercentValue((rawValue / std::max(1.0, scale.diskDenominatorMBps)) * 100.0);
        case ProcessDock::ProcessActivityMetric::Network:
            return clampPercentValue((rawValue / std::max(1.0, scale.networkDenominatorKBps)) * 100.0);
        default:
            return 0.0;
        }
    }

    // sampleIndexAtX：
    // - 将鼠标横坐标映射为最近样本下标；
    // - 越界坐标会夹到首尾样本。
    int sampleIndexAtX(const int xValue) const
    {
        if (m_ownerDock == nullptr || m_ownerDock->m_activitySamples.empty())
        {
            return -1;
        }
        const QRectF plotRect = chartRect();
        if (plotRect.width() <= 1.0)
        {
            return -1;
        }
        const double ratio = std::clamp(
            (static_cast<double>(xValue) - plotRect.left()) / plotRect.width(),
            0.0,
            1.0);
        const std::size_t sampleCount = m_ownerDock->m_activitySamples.size();
        const int sampleIndex = static_cast<int>(std::llround(ratio * static_cast<double>(sampleCount - 1U)));
        return std::clamp(sampleIndex, 0, static_cast<int>(sampleCount) - 1);
    }

    // sampleIndexToX：
    // - 将样本下标映射为折线点 X；
    // - 绘制焦点线和时间标签复用该函数。
    double sampleIndexToX(const int sampleIndex, const QRectF& plotRect) const
    {
        if (m_ownerDock == nullptr || m_ownerDock->m_activitySamples.size() <= 1U)
        {
            return plotRect.left() + plotRect.width() * 0.5;
        }
        const double ratio = static_cast<double>(sampleIndex)
            / static_cast<double>(m_ownerDock->m_activitySamples.size() - 1U);
        return plotRect.left() + ratio * plotRect.width();
    }

    // drawGrid：
    // - 绘制弱网格、最大值标签和首尾时间；
    // - 不创建额外轴控件，降低 UI 成本。
    void drawGrid(
        QPainter& painter,
        const QRectF& plotRect,
        const QColor& borderColor,
        const QColor& textColor) const
    {
        painter.setPen(QPen(borderColor, 0.5));
        for (int i = 1; i <= 3; ++i)
        {
            const double yValue = plotRect.bottom() - plotRect.height() * static_cast<double>(i) / 4.0;
            painter.drawLine(QPointF(plotRect.left(), yValue), QPointF(plotRect.right(), yValue));
        }

        painter.setPen(textColor);
        painter.drawText(QRectF(2.0, plotRect.top() - 2.0, 38.0, 18.0), Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("100%"));
        painter.drawText(QRectF(2.0, plotRect.center().y() - 9.0, 38.0, 18.0), Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("50%"));
        painter.drawText(QRectF(2.0, plotRect.bottom() - 16.0, 38.0, 18.0), Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("0%"));

        if (m_ownerDock != nullptr && !m_ownerDock->m_activitySamples.empty())
        {
            const ProcessDock::ProcessActivitySample& firstSample = m_ownerDock->m_activitySamples.front();
            const ProcessDock::ProcessActivitySample& lastSample = m_ownerDock->m_activitySamples.back();
            painter.drawText(QRectF(plotRect.left(), plotRect.bottom() + 3.0, 80.0, 18.0),
                Qt::AlignLeft | Qt::AlignVCenter,
                formatActivityElapsedText(firstSample.elapsedMs));
            painter.drawText(QRectF(plotRect.right() - 90.0, plotRect.bottom() + 3.0, 90.0, 18.0),
                Qt::AlignRight | Qt::AlignVCenter,
                formatActivityElapsedText(lastSample.elapsedMs));
        }
    }

    // drawLines：
    // - 绘制按时间排列的多指标折线；
    // - 所有指标已转为 0~100%，不同单位可以共用同一 Y 轴。
    void drawLines(
        QPainter& painter,
        const QRectF& plotRect,
        const std::vector<ProcessDock::ProcessActivityMetric>& metricList,
        const std::vector<std::string>& selectionKeys,
        const MetricScale& metricScale) const
    {
        if (m_ownerDock == nullptr || m_ownerDock->m_activitySamples.empty())
        {
            return;
        }

        const std::size_t sampleCount = m_ownerDock->m_activitySamples.size();
        for (const ProcessDock::ProcessActivityMetric metric : metricList)
        {
            QPainterPath metricPath;
            bool hasPoint = false;
            std::vector<QPointF> pointList;
            pointList.reserve(sampleCount);
            for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
            {
                const ProcessDock::ProcessActivitySample& sample = m_ownerDock->m_activitySamples[sampleIndex];
                const double percentValue = samplePercentMetricValue(sample, metric, selectionKeys, metricScale);
                const double xValue = sampleIndexToX(static_cast<int>(sampleIndex), plotRect);
                const double yValue = plotRect.bottom() - (percentValue / 100.0) * plotRect.height();
                const QPointF point(xValue, yValue);
                pointList.push_back(point);
                if (!hasPoint)
                {
                    metricPath.moveTo(point);
                    hasPoint = true;
                }
                else
                {
                    metricPath.lineTo(point);
                }
            }

            QColor lineColor = processActivityMetricColor(metric);
            lineColor.setAlpha(230);
            // 折线只允许描边，不允许沿用上一条指标采样点留下的 brush。
            // Qt 的 drawPath 会同时 stroke 和 fill；如果 brush 未清空，开放折线路径会被隐式闭合填充。
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(lineColor, 2.0));
            painter.drawPath(metricPath);

            QColor pointColor = lineColor;
            pointColor.setAlpha(245);
            painter.setBrush(pointColor);
            painter.setPen(Qt::NoPen);
            const int pointStride = static_cast<int>(std::max<std::size_t>(1U, sampleCount / 80U));
            for (std::size_t pointIndex = 0; pointIndex < pointList.size(); pointIndex += static_cast<std::size_t>(pointStride))
            {
                painter.drawEllipse(pointList[pointIndex], 2.2, 2.2);
            }
            // 采样点绘制会设置实心 brush，循环下一条折线前必须恢复为空画刷。
            painter.setBrush(Qt::NoBrush);
        }
    }

    // drawLegend：
    // - 在图表上沿绘制当前启用指标图例；
    // - 用户可据此确认按钮筛选后的折线颜色。
    void drawLegend(
        QPainter& painter,
        const std::vector<ProcessDock::ProcessActivityMetric>& metricList,
        const QColor& textColor) const
    {
        int xOffset = 48;
        const int yOffset = 4;
        painter.setPen(textColor);
        for (const ProcessDock::ProcessActivityMetric metric : metricList)
        {
            QColor color = processActivityMetricColor(metric);
            color.setAlpha(220);
            painter.fillRect(QRect(xOffset, yOffset + 4, 9, 9), color);
            painter.drawText(QRect(xOffset + 13, yOffset, 54, 18),
                Qt::AlignLeft | Qt::AlignVCenter,
                processActivityMetricText(metric));
            xOffset += 62;
        }
    }

    // drawFocusLine：
    // - 绘制时间轴当前定位样本的竖线；
    // - 该线由滑块或图表悬停共同驱动。
    void drawFocusLine(QPainter& painter, const QRectF& plotRect) const
    {
        if (m_ownerDock == nullptr || m_ownerDock->m_activitySamples.empty())
        {
            return;
        }
        const int safeIndex = std::clamp(
            m_focusedSampleIndex,
            0,
            static_cast<int>(m_ownerDock->m_activitySamples.size()) - 1);
        const double xValue = sampleIndexToX(safeIndex, plotRect);
        QColor lineColor = KswordTheme::PrimaryBlueColor;
        lineColor.setAlpha(230);
        painter.setPen(QPen(lineColor, 1.4));
        painter.drawLine(QPointF(xValue, plotRect.top()), QPointF(xValue, plotRect.bottom()));
    }

private:
    ProcessDock* m_ownerDock = nullptr; // m_ownerDock：宿主 ProcessDock，不拥有。
    int m_focusedSampleIndex = -1;      // m_focusedSampleIndex：当前时间轴定位样本。
};

class ProcessActivityTimelineSlider final : public QSlider
{
public:
    // 构造函数：
    // - ownerDock：用于在悬停时读取历史快照；
    // - parent：Qt 父控件；
    // - 无返回值，启用鼠标追踪实现“移到那里展示快照”。
    explicit ProcessActivityTimelineSlider(ProcessDock* ownerDock, QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
        , m_ownerDock(ownerDock)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    // mouseMoveEvent：
    // - 鼠标悬停到时间轴任意位置时映射到对应样本；
    // - 不要求按下拖动也能展示快照。
    void mouseMoveEvent(QMouseEvent* eventPointer) override
    {
        if (eventPointer != nullptr && m_ownerDock != nullptr && !m_ownerDock->m_activitySamples.empty())
        {
            const int sampleIndex = valueAtPosition(activityMousePosition(eventPointer).x());
            // 悬停只更新快照提示，不改变滑块值，也不重绘下方进程表。
            const bool oldPinnedToLatest = m_ownerDock->m_activityTimelinePinnedToLatest;
            m_ownerDock->previewProcessActivitySnapshotForIndex(sampleIndex);
            m_ownerDock->m_activityTimelinePinnedToLatest = oldPinnedToLatest;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint globalPosition = eventPointer->globalPosition().toPoint();
#else
            const QPoint globalPosition = eventPointer->globalPos();
#endif
            QToolTip::showText(globalPosition, m_ownerDock->buildProcessActivitySnapshotText(sampleIndex), this);
        }
        QSlider::mouseMoveEvent(eventPointer);
    }

    // mousePressEvent：
    // - 单击时间轴即跳转到对应样本；
    // - 拖到最右侧后宿主会重新进入吸附最新模式。
    void mousePressEvent(QMouseEvent* eventPointer) override
    {
        if (eventPointer != nullptr && eventPointer->button() == Qt::LeftButton && maximum() >= minimum())
        {
            const int sampleIndex = valueAtPosition(activityMousePosition(eventPointer).x());
            setValue(sampleIndex);
            if (m_ownerDock != nullptr)
            {
                m_ownerDock->commitProcessActivityTimelineIndex(sampleIndex);
            }
            eventPointer->accept();
            return;
        }
        QSlider::mousePressEvent(eventPointer);
    }

    // leaveEvent：
    // - 鼠标离开后隐藏 tooltip；
    // - 当前选中的历史样本仍保留在下方快照标签中。
    void leaveEvent(QEvent* eventPointer) override
    {
        QToolTip::hideText();
        QSlider::leaveEvent(eventPointer);
    }

private:
    // valueAtPosition：
    // - 将本地 X 坐标映射到滑块范围；
    // - 返回值自动夹在 minimum/maximum 内。
    int valueAtPosition(const int xValue) const
    {
        const int rangeValue = maximum() - minimum();
        if (rangeValue <= 0 || width() <= 1)
        {
            return minimum();
        }
        const QStyleOptionSlider option = sliderOption();
        const QRect grooveRect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const QRect handleRect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const int sliderMin = grooveRect.left();
        const int sliderMax = grooveRect.right() - handleRect.width() + 1;
        if (sliderMax <= sliderMin)
        {
            const double fallbackRatio = std::clamp(
                static_cast<double>(xValue) / static_cast<double>(std::max(1, width() - 1)),
                0.0,
                1.0);
            return minimum() + static_cast<int>(std::llround(fallbackRatio * static_cast<double>(rangeValue)));
        }
        const double denominator = static_cast<double>(std::max(1, sliderMax - sliderMin));
        const double ratio = std::clamp(
            (static_cast<double>(xValue) - static_cast<double>(sliderMin)) / denominator,
            0.0,
            1.0);
        return minimum() + static_cast<int>(std::llround(ratio * static_cast<double>(rangeValue)));
    }

    // sliderOption：
    // - 构造当前 QSlider 样式选项；
    // - 用于准确获取 groove/handle 几何范围。
    QStyleOptionSlider sliderOption() const
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        return option;
    }

private:
    ProcessDock* m_ownerDock = nullptr; // m_ownerDock：宿主 ProcessDock，不拥有。
};

namespace
{
    // ProcessNameDelegate 作用：在“进程名”列右侧绘制效率模式绿叶，不新增表格列。
    class ProcessNameDelegate final : public QStyledItemDelegate
    {
    public:
        explicit ProcessNameDelegate(QObject* parent = nullptr)
            : QStyledItemDelegate(parent)
        {
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            QStyleOptionViewItem itemOption(option);
            if (index.column() == 0 && index.data(ProcessEfficiencyModeRole).toBool())
            {
                itemOption.rect.adjust(0, 0, -22, 0);
            }
            QStyledItemDelegate::paint(painter, itemOption, index);

            if (index.column() != 0 || !index.data(ProcessEfficiencyModeRole).toBool())
            {
                return;
            }
            if (painter == nullptr)
            {
                return;
            }

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            const int iconSize = std::min(16, std::max(10, option.rect.height() - 6));
            const QRect leafRect(
                option.rect.right() - iconSize - 5,
                option.rect.center().y() - iconSize / 2,
                iconSize,
                iconSize);
            const QColor leafColor = KswordTheme::IsDarkModeEnabled()
                ? QColor(101, 216, 120)
                : QColor(32, 166, 72);
            QPainterPath leafPath;
            leafPath.moveTo(leafRect.left() + leafRect.width() * 0.18, leafRect.center().y());
            leafPath.cubicTo(
                leafRect.left() + leafRect.width() * 0.32,
                leafRect.top() + leafRect.height() * 0.08,
                leafRect.right() - leafRect.width() * 0.10,
                leafRect.top() + leafRect.height() * 0.06,
                leafRect.right() - leafRect.width() * 0.08,
                leafRect.center().y());
            leafPath.cubicTo(
                leafRect.right() - leafRect.width() * 0.10,
                leafRect.bottom() - leafRect.height() * 0.08,
                leafRect.left() + leafRect.width() * 0.30,
                leafRect.bottom() - leafRect.height() * 0.05,
                leafRect.left() + leafRect.width() * 0.18,
                leafRect.center().y());
            painter->fillPath(leafPath, leafColor);
            painter->setPen(QPen(QColor(255, 255, 255, 210), 1.2));
            painter->drawLine(
                QPointF(leafRect.left() + leafRect.width() * 0.28, leafRect.bottom() - leafRect.height() * 0.25),
                QPointF(leafRect.right() - leafRect.width() * 0.20, leafRect.top() + leafRect.height() * 0.22));
            painter->restore();
        }
    };

    // 当前 steady_clock 时间转 100ns（与 ks::process 差值计算规则保持一致）。
    std::uint64_t steadyNow100ns()
    {
        const auto nowDuration = std::chrono::steady_clock::now().time_since_epoch();
        const auto nowNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(nowDuration).count();
        return static_cast<std::uint64_t>(nowNanoseconds / 100);
    }

    // 从策略下拉索引映射到 ks::process 策略枚举。
    ks::process::ProcessEnumStrategy toStrategy(const int strategyIndex)
    {
        switch (strategyIndex)
        {
        case 0:
            return ks::process::ProcessEnumStrategy::SnapshotProcess32;
        case 1:
            return ks::process::ProcessEnumStrategy::NtQuerySystemInfo;
        default:
            return ks::process::ProcessEnumStrategy::NtQuerySystemInfo;
        }
    }

    // 策略枚举转可读文本：用于刷新状态标签与详细日志输出。
    const char* strategyToText(const ks::process::ProcessEnumStrategy strategy)
    {
        switch (strategy)
        {
        case ks::process::ProcessEnumStrategy::SnapshotProcess32:
            return "Toolhelp Snapshot + Process32First/Next";
        case ks::process::ProcessEnumStrategy::NtQuerySystemInfo:
            return "NtQuerySystemInformation";
        case ks::process::ProcessEnumStrategy::Auto:
            return "Auto (NtQuery 优先, 失败回退 Toolhelp)";
        default:
            return "Unknown";
        }
    }

    // isProcessR0ExtensionVisible 作用：
    // - 判断一行是否真正携带 R0 扩展字段；
    // - 全部 Unavailable 时 UI 会自动隐藏内核专属列，避免误导用户以为 R3 字段异常。
    bool isProcessR0ExtensionVisible(const ks::process::ProcessRecord& processRecord)
    {
        if (processRecord.r0Status != KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE)
        {
            return true;
        }

        return (processRecord.r0FieldFlags &
            (KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT |
                KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_AVAILABLE |
                KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_VALUE_PRESENT |
                KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_AVAILABLE |
                KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_VALUE_PRESENT)) != 0U;
    }

    // terminateProcessByR0Driver 作用：
    // - 通过 ArkDriverClient 发送“结束进程”IOCTL；
    // - Dock 不再直接打开 KswordARK 设备或调用 DeviceIoControl。
    bool terminateProcessByR0Driver(const std::uint32_t targetPid, std::string* const detailTextOut)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        if (targetPid == 0U || targetPid <= 4U)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::IoResult result = driverClient.terminateProcess(
            targetPid,
            static_cast<long>(0xC0000005u));
        if (detailTextOut != nullptr)
        {
            *detailTextOut = result.message;
        }
        return result.ok;
    }

    // suspendProcessByR0Driver 作用：
    // - 通过 ArkDriverClient 发送“挂起进程”IOCTL；
    // - 保持旧 detailText 输出格式，降低 UI 行为变化。
    bool suspendProcessByR0Driver(const std::uint32_t targetPid, std::string* const detailTextOut)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        if (targetPid == 0U || targetPid <= 4U)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::IoResult result = driverClient.suspendProcess(targetPid);
        if (detailTextOut != nullptr)
        {
            *detailTextOut = result.message;
        }
        return result.ok;
    }

    // setPplProtectionLevelByR0Driver 作用：
    // - 通过 ArkDriverClient 发送“设置 PPL 保护层级”IOCTL；
    // - protectionLevel 与 ProcessProtectionInformation 的单字节层级编码保持一致。
    bool setPplProtectionLevelByR0Driver(
        const std::uint32_t targetPid,
        const std::uint8_t protectionLevel,
        std::string* const detailTextOut)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        if (targetPid == 0U || targetPid <= 4U)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::IoResult result = driverClient.setProcessProtection(targetPid, protectionLevel);
        if (detailTextOut != nullptr)
        {
            *detailTextOut = result.message;
        }
        return result.ok;
    }

    bool setProcessVisibilityByR0Driver(
        const std::uint32_t targetPid,
        const unsigned long action,
        std::string* const detailTextOut)
    {
        // 作用：调用 ArkDriverClient 更新 R0 可恢复隐藏标记。
        // 返回：true 表示驱动接受并更新；false 表示 IOCTL 或 R0 状态失败。
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        if (action != KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL &&
            (targetPid == 0U || targetPid <= 4U))
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::ProcessVisibilityResult result =
            driverClient.setProcessVisibility(targetPid, action);
        if (detailTextOut != nullptr)
        {
            *detailTextOut = result.io.message;
        }
        return result.io.ok &&
            result.lastStatus >= 0 &&
            (result.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN ||
                result.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_VISIBLE ||
                result.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED);
    }

    bool setProcessSpecialFlagsByR0Driver(
        const std::uint32_t targetPid,
        const unsigned long action,
        std::string* const detailTextOut)
    {
        // 作用：封装 BreakOnTermination/APC 插入控制 IOCTL。
        // 返回：true 表示 R0 完成动作；false 表示 IOCTL 或 R0 状态失败。
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }
        if (targetPid == 0U || targetPid <= 4U)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::ProcessSpecialFlagsResult result =
            driverClient.setProcessSpecialFlags(targetPid, action);
        if (detailTextOut != nullptr)
        {
            *detailTextOut = result.io.message;
        }
        return result.io.ok &&
            result.lastStatus >= 0 &&
            result.status == KSWORD_ARK_PROCESS_SPECIAL_STATUS_APPLIED;
    }

    bool dkomProcessByR0Driver(
        const std::uint32_t targetPid,
        const unsigned long action,
        std::string* const detailTextOut)
    {
        // 作用：封装 PspCidTable DKOM 删除 IOCTL。
        // 返回：true 表示 R0 至少删除一个 CID entry。
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }
        if (targetPid == 0U || targetPid <= 4U)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::ProcessDkomResult result =
            driverClient.dkomProcess(targetPid, action);
        if (detailTextOut != nullptr)
        {
            *detailTextOut = result.io.message;
        }
        return result.io.ok &&
            result.lastStatus >= 0 &&
            result.status == KSWORD_ARK_PROCESS_DKOM_STATUS_REMOVED &&
            result.removedEntries > 0U;
    }

    // KernelProcessSnapshotEntry 作用：
    // - 承载 R0 枚举返回的一条进程快照；
    // - 仅保留 UI 对比所需字段（PID/父 PID/标志/短进程名）。
    struct KernelProcessSnapshotEntry
    {
        std::uint32_t processId = 0;
        std::uint32_t parentProcessId = 0;
        std::uint32_t flags = 0;
        std::uint32_t sessionId = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t r0Status = KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE;
        std::uint32_t sessionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint8_t protection = 0;
        std::uint8_t signatureLevel = 0;
        std::uint8_t sectionSignatureLevel = 0;
        std::uint32_t protectionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t signatureLevelSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t sectionSignatureLevelSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t objectTableSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t sectionObjectSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t imagePathSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t protectionOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t signatureLevelOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t sectionSignatureLevelOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t objectTableOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t sectionObjectOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint64_t objectTableAddress = 0;
        std::uint64_t sectionObjectAddress = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::string imageName;
        std::string imagePath;
    };

    // 内核专属记录使用固定创建时间种子，避免与 R3 常规 identity 发生冲突。
    constexpr std::uint64_t KernelOnlyCreationTimeSeed = 0xFFFFFFFF00000000ULL;

    QString processFieldSourceText(const std::uint32_t sourceValue)
    {
        // sourceValue 用途：共享协议中的字段来源枚举。
        // 返回值：面向 UI 的稳定可读文本。
        switch (sourceValue)
        {
        case KSWORD_ARK_PROCESS_FIELD_SOURCE_PUBLIC_API:
            return QStringLiteral("Public API");
        case KSWORD_ARK_PROCESS_FIELD_SOURCE_SYSTEM_INFORMER_DYNDATA:
            return QStringLiteral("System Informer DynData");
        case KSWORD_ARK_PROCESS_FIELD_SOURCE_RUNTIME_PATTERN:
            return QStringLiteral("Runtime pattern");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    QString processR0StatusText(const std::uint32_t statusValue)
    {
        // statusValue 用途：R0 每行扩展信息的整体完成状态。
        // 返回值：短文本，直接用于表格列与详情页。
        switch (statusValue)
        {
        case KSWORD_ARK_PROCESS_R0_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_PROCESS_R0_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_PROCESS_R0_STATUS_DYNDATA_MISSING:
            return QStringLiteral("DynData missing");
        case KSWORD_ARK_PROCESS_R0_STATUS_READ_FAILED:
            return QStringLiteral("Read failed");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    QString byteHexText(const std::uint8_t byteValue)
    {
        // byteValue 用途：保护/签名等级原始单字节值。
        // 返回值：0xNN 格式，便于和内核原始字段对照。
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(byteValue), 2, 16, QChar('0'))
            .toUpper();
    }

    bool resolvePplSignatureLevelsForUi(
        const std::uint8_t protectionLevel,
        std::uint8_t* const signatureLevelOut,
        std::uint8_t* const sectionSignatureLevelOut)
    {
        // protectionLevel 用途：菜单传入的 PS_PROTECTION 原始字节。
        // 返回值：true 表示 UI 能预测驱动侧同步写入的签名级别。
        if (signatureLevelOut == nullptr || sectionSignatureLevelOut == nullptr)
        {
            return false;
        }

        const std::uint8_t signerType = (protectionLevel == 0U)
            ? static_cast<std::uint8_t>(0U)
            : static_cast<std::uint8_t>((protectionLevel & 0xF0U) >> 4U);
        switch (signerType)
        {
        case 0:
            *signatureLevelOut = 0x00U;
            *sectionSignatureLevelOut = 0x00U;
            return true;
        case 1:
            *signatureLevelOut = 0x04U;
            *sectionSignatureLevelOut = 0x04U;
            return true;
        case 2:
            *signatureLevelOut = 0x0BU;
            *sectionSignatureLevelOut = 0x06U;
            return true;
        case 3:
            *signatureLevelOut = 0x07U;
            *sectionSignatureLevelOut = 0x07U;
            return true;
        case 4:
            *signatureLevelOut = 0x0CU;
            *sectionSignatureLevelOut = 0x08U;
            return true;
        case 5:
            *signatureLevelOut = 0x0CU;
            *sectionSignatureLevelOut = 0x0CU;
            return true;
        case 6:
            *signatureLevelOut = 0x0EU;
            *sectionSignatureLevelOut = 0x0CU;
            return true;
        default:
            return false;
        }
    }

    QString pplMutationCapabilityText(const ks::process::ProcessRecord& processRecord)
    {
        // processRecord 用途：读取当前行的 DynData capability 与字段来源。
        // 返回值：二次确认框中的 capability/source 摘要。
        const bool capabilityPresent =
            (processRecord.r0DynDataCapabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) != 0U;
        const bool protectionOffsetPresent =
            processRecord.r0ProtectionOffset != KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE &&
            processRecord.r0SignatureLevelOffset != KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE &&
            processRecord.r0SectionSignatureLevelOffset != KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        return QStringLiteral("Capability: %1 | Offsets: %2 | Sources: Protection=%3, Signature=%4, SectionSignature=%5")
            .arg(capabilityPresent ? QStringLiteral("KSW_CAP_PROCESS_PROTECTION_PATCH present") : QStringLiteral("missing/unknown"))
            .arg(protectionOffsetPresent ? QStringLiteral("present") : QStringLiteral("missing/unknown"))
            .arg(processFieldSourceText(processRecord.r0ProtectionSource))
            .arg(processFieldSourceText(processRecord.r0SignatureLevelSource))
            .arg(processFieldSourceText(processRecord.r0SectionSignatureLevelSource));
    }

    QString pointerAvailabilityText(
        const bool available,
        const std::uint64_t addressValue,
        const std::uint32_t sourceValue)
    {
        // available 表示 offset/capability 是否可用；addressValue 是当前字段值。
        // 返回值含来源，方便用户判断 DynData 是否命中。
        if (!available)
        {
            return QStringLiteral("Unavailable (%1)").arg(processFieldSourceText(sourceValue));
        }
        if (addressValue == 0U)
        {
            return QStringLiteral("Available: null (%1)").arg(processFieldSourceText(sourceValue));
        }
        const QString addressText = QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(addressValue), 0, 16)
            .toUpper();
        return QStringLiteral("Available: 0x%1 (%2)")
            .arg(addressText.mid(2))
            .arg(processFieldSourceText(sourceValue));
    }

    // enumerateProcessesByR0Driver 作用：
    // - 通过 ArkDriverClient 获取内核侧进程列表；
    // - 输出可用于“R3 列表 vs R0 列表”差异比对的数据。
    bool enumerateProcessesByR0Driver(
        std::vector<KernelProcessSnapshotEntry>* const processListOut,
        std::string* const detailTextOut)
    {
        if (processListOut == nullptr)
        {
            return false;
        }
        processListOut->clear();
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::ProcessEnumResult enumResult = driverClient.enumerateProcesses(
            KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE);
        if (!enumResult.io.ok)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = enumResult.io.message;
            }
            return false;
        }

        processListOut->reserve(enumResult.entries.size());
        for (const ksword::ark::ProcessEntry& entry : enumResult.entries)
        {
            KernelProcessSnapshotEntry processEntry{};
            processEntry.processId = entry.processId;
            processEntry.parentProcessId = entry.parentProcessId;
            processEntry.flags = entry.flags;
            processEntry.sessionId = entry.sessionId;
            processEntry.fieldFlags = entry.fieldFlags;
            processEntry.r0Status = entry.r0Status;
            processEntry.sessionSource = entry.sessionSource;
            processEntry.protection = entry.protection;
            processEntry.signatureLevel = entry.signatureLevel;
            processEntry.sectionSignatureLevel = entry.sectionSignatureLevel;
            processEntry.protectionSource = entry.protectionSource;
            processEntry.signatureLevelSource = entry.signatureLevelSource;
            processEntry.sectionSignatureLevelSource = entry.sectionSignatureLevelSource;
            processEntry.objectTableSource = entry.objectTableSource;
            processEntry.sectionObjectSource = entry.sectionObjectSource;
            processEntry.imagePathSource = entry.imagePathSource;
            processEntry.protectionOffset = entry.protectionOffset;
            processEntry.signatureLevelOffset = entry.signatureLevelOffset;
            processEntry.sectionSignatureLevelOffset = entry.sectionSignatureLevelOffset;
            processEntry.objectTableOffset = entry.objectTableOffset;
            processEntry.sectionObjectOffset = entry.sectionObjectOffset;
            processEntry.objectTableAddress = entry.objectTableAddress;
            processEntry.sectionObjectAddress = entry.sectionObjectAddress;
            processEntry.dynDataCapabilityMask = entry.dynDataCapabilityMask;
            processEntry.imageName = entry.imageName;
            processEntry.imagePath = entry.imagePath;
            processListOut->push_back(std::move(processEntry));
        }

        if (detailTextOut != nullptr)
        {
            *detailTextOut = enumResult.io.message;
        }
        return true;
    }

    // mergeKernelProcessExtension 作用：
    // - 把 R0 枚举得到的 Phase-2 EPROCESS 扩展字段合并到 R3 进程记录；
    // - 基础路径/命令行仍以用户态公开 API 为主，R0 路径仅作为补充诊断字段。
    void mergeKernelProcessExtension(
        ks::process::ProcessRecord& processRecord,
        const KernelProcessSnapshotEntry& kernelProcess)
    {
        processRecord.r0Flags = kernelProcess.flags;
        processRecord.r0FieldFlags = kernelProcess.fieldFlags;
        processRecord.r0Status = kernelProcess.r0Status;
        processRecord.r0DynDataCapabilityMask = kernelProcess.dynDataCapabilityMask;
        processRecord.r0ImagePath = kernelProcess.imagePath;

        if ((kernelProcess.fieldFlags & KSWORD_ARK_PROCESS_FIELD_SESSION_PRESENT) != 0U)
        {
            processRecord.sessionId = kernelProcess.sessionId;
        }

        processRecord.r0Protection = kernelProcess.protection;
        processRecord.r0SignatureLevel = kernelProcess.signatureLevel;
        processRecord.r0SectionSignatureLevel = kernelProcess.sectionSignatureLevel;
        processRecord.r0SessionSource = kernelProcess.sessionSource;
        processRecord.r0ImagePathSource = kernelProcess.imagePathSource;
        processRecord.r0ProtectionSource = kernelProcess.protectionSource;
        processRecord.r0SignatureLevelSource = kernelProcess.signatureLevelSource;
        processRecord.r0SectionSignatureLevelSource = kernelProcess.sectionSignatureLevelSource;
        processRecord.r0ObjectTableSource = kernelProcess.objectTableSource;
        processRecord.r0SectionObjectSource = kernelProcess.sectionObjectSource;
        processRecord.r0ProtectionOffset = kernelProcess.protectionOffset;
        processRecord.r0SignatureLevelOffset = kernelProcess.signatureLevelOffset;
        processRecord.r0SectionSignatureLevelOffset = kernelProcess.sectionSignatureLevelOffset;
        processRecord.r0ObjectTableOffset = kernelProcess.objectTableOffset;
        processRecord.r0SectionObjectOffset = kernelProcess.sectionObjectOffset;
        processRecord.r0ObjectTableAddress = kernelProcess.objectTableAddress;
        processRecord.r0SectionObjectAddress = kernelProcess.sectionObjectAddress;
    }

    bool enrichProcessRecordWithR0ExtensionByPid(
        ks::process::ProcessRecord& processRecord,
        std::string* const detailTextOut)
    {
        // processRecord 用途：调用方当前选中或即将打开详情的 R3 进程记录。
        // 返回值：true 表示已从 R0 枚举中找到同 PID 并合并 Phase-2 扩展字段。
        std::vector<KernelProcessSnapshotEntry> kernelProcessList;
        std::string queryDetailText;
        const bool queryOk = enumerateProcessesByR0Driver(&kernelProcessList, &queryDetailText);
        if (!queryOk)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = queryDetailText.empty()
                    ? std::string("query kernel process list failed")
                    : queryDetailText;
            }
            return false;
        }

        for (const KernelProcessSnapshotEntry& kernelProcess : kernelProcessList)
        {
            if (kernelProcess.processId != processRecord.pid)
            {
                continue;
            }

            mergeKernelProcessExtension(processRecord, kernelProcess);
            if (processRecord.processName.empty() && !kernelProcess.imageName.empty())
            {
                processRecord.processName = kernelProcess.imageName;
            }
            if (detailTextOut != nullptr)
            {
                *detailTextOut = queryDetailText;
            }
            return true;
        }

        if (detailTextOut != nullptr)
        {
            *detailTextOut = "target pid not returned by R0 process enumeration";
        }
        return false;
    }

    // isProcessPresentBySnapshot 作用：
    // - 通过 Toolhelp 进程快照判断目标 PID 当前是否仍存在；
    // - 用于“结束进程组合动作”每一步后的真实存活判定。
    // 调用方式：ProcessDock::executeTerminateProcessAction 内部循环调用。
    // 参数 targetPid：目标进程 PID。
    // 参数 queryOkOut：快照查询是否成功（可空）。
    // 返回值：true=进程仍存在（或查询失败时保守视为存在）；false=进程不存在。
    bool isProcessPresentBySnapshot(const std::uint32_t targetPid, bool* const queryOkOut)
    {
        if (queryOkOut != nullptr)
        {
            *queryOkOut = false;
        }

        // snapshotHandle 用途：承接系统进程快照句柄，供后续枚举 PID。
        const HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return true;
        }

        // processEntry 用途：逐条读取进程快照记录并比较 PID。
        PROCESSENTRY32W processEntry{};
        processEntry.dwSize = sizeof(processEntry);
        if (::Process32FirstW(snapshotHandle, &processEntry) == FALSE)
        {
            ::CloseHandle(snapshotHandle);
            return true;
        }

        // processPresent 用途：记录目标 PID 是否在当前快照里被命中。
        bool processPresent = false;
        do
        {
            if (processEntry.th32ProcessID == targetPid)
            {
                processPresent = true;
                break;
            }
        } while (::Process32NextW(snapshotHandle, &processEntry) != FALSE);

        ::CloseHandle(snapshotHandle);
        if (queryOkOut != nullptr)
        {
            *queryOkOut = true;
        }
        return processPresent;
    }

    // usageRatioToHighlightColor 作用：
    // - 按占用比例（0~1）返回主题蓝色透明高亮；
    // - 占用越高，alpha 越大，视觉上更“深”。
    QColor usageRatioToHighlightColor(double usageRatio)
    {
        usageRatio = std::clamp(usageRatio, 0.0, 1.0);
        const int alphaValue = static_cast<int>(24.0 + usageRatio * 146.0);
        QColor highlightColor = KswordTheme::PrimaryBlueColor;
        highlightColor.setAlpha(alphaValue);
        return highlightColor;
    }

    // hasDetailWindowSignificantChange 作用：
    // - 判断两轮进程记录是否存在“需要立刻同步到详情窗口”的显著变化；
    // - 通过阈值过滤掉轻微抖动，减少刷新期间 UI 卡顿。
    bool hasDetailWindowSignificantChange(
        const ks::process::ProcessRecord& oldRecord,
        const ks::process::ProcessRecord& newRecord)
    {
        if (oldRecord.pid != newRecord.pid ||
            oldRecord.creationTime100ns != newRecord.creationTime100ns)
        {
            return true;
        }

        if (std::fabs(oldRecord.cpuPercent - newRecord.cpuPercent) >= 4.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.ramMB - newRecord.ramMB) >= 16.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.diskMBps - newRecord.diskMBps) >= 1.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.netKBps - newRecord.netKBps) >= 8.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.gpuPercent - newRecord.gpuPercent) >= 5.0)
        {
            return true;
        }
        if (oldRecord.protectionLevelKnown != newRecord.protectionLevelKnown ||
            oldRecord.protectionLevel != newRecord.protectionLevel ||
            oldRecord.protectionLevelText != newRecord.protectionLevelText)
        {
            return true;
        }

        if (oldRecord.threadCount != newRecord.threadCount ||
            oldRecord.handleCount != newRecord.handleCount ||
            oldRecord.parentPid != newRecord.parentPid ||
            oldRecord.isAdmin != newRecord.isAdmin ||
            oldRecord.signatureTrusted != newRecord.signatureTrusted)
        {
            return true;
        }

        if (oldRecord.imagePath != newRecord.imagePath ||
            oldRecord.commandLine != newRecord.commandLine ||
            oldRecord.userName != newRecord.userName ||
            oldRecord.signatureState != newRecord.signatureState ||
            oldRecord.signaturePublisher != newRecord.signaturePublisher ||
            oldRecord.startTimeText != newRecord.startTimeText)
        {
            return true;
        }

        return false;
    }

    // 统一按钮蓝色样式，和现有主题风格保持一致。
    QString buildBlueButtonStyle(const bool iconOnlyButton)
    {
        // 图标按钮采用更紧凑 padding，避免出现多余空白。
        const QString paddingText = iconOnlyButton ? QStringLiteral("4px") : QStringLiteral("4px 10px");
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: %6;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: %5;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "  color: #FFFFFF;"
            "  border: 1px solid %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueSolidHoverHex())
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(paddingText)
            .arg(KswordTheme::SurfaceHex());
    }

    // 下拉框主题描边样式，保持与按钮同色系。
    QString buildBlueComboBoxStyle()
    {
        const QString comboBackgroundColor = KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#172232")
            : QStringLiteral("#FFFFFF");
        const QString comboTextColor = KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#F4F8FF")
            : QStringLiteral("#172B43");
        const QString comboBorderColor = KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#34506D")
            : QStringLiteral("#AFC4DC");

        return QStringLiteral(
            "QComboBox{"
            "  background-color:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:3px;"
            "  padding:2px 20px 2px 6px;"
            "}"
            "QComboBox::drop-down{"
            "  border:none;"
            "  width:18px;"
            "}"
            "QComboBox QAbstractItemView{"
            "  background-color:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  selection-background-color:%4;"
            "  selection-color:%2;"
            "}")
            .arg(comboBackgroundColor)
            .arg(comboTextColor)
            .arg(comboBorderColor)
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // 统一“普通输入框”主题边框。
    QString buildBlueLineEditStyle()
    {
        return QStringLiteral(
            "QLineEdit, QPlainTextEdit, QTextEdit {"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  background: %3;"
            "  color: %4;"
            "  padding: 3px 5px;"
            "}"
            "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus {"
            "  border: 1px solid %1;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // applyTransparentContainerStyle 作用：
    // - 仅把“创建进程”页中的容器类控件背景改成透明；
    // - 不影响输入框、按钮等已有主题样式。
    void applyTransparentContainerStyle(QWidget* widgetPointer)
    {
        if (widgetPointer == nullptr)
        {
            return;
        }

        widgetPointer->setAttribute(Qt::WA_StyledBackground, true);
        widgetPointer->setAutoFillBackground(false);
        widgetPointer->setStyleSheet(
            widgetPointer->styleSheet()
            + QStringLiteral("background:transparent;background-color:transparent;"));

        QAbstractScrollArea* scrollAreaPointer = qobject_cast<QAbstractScrollArea*>(widgetPointer);
        if (scrollAreaPointer == nullptr || scrollAreaPointer->viewport() == nullptr)
        {
            return;
        }

        scrollAreaPointer->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        scrollAreaPointer->viewport()->setAutoFillBackground(false);
        scrollAreaPointer->viewport()->setStyleSheet(
            scrollAreaPointer->viewport()->styleSheet()
            + QStringLiteral("background:transparent;background-color:transparent;"));
    }

    // 常见令牌特权列表：用于“可视化调权”表格。
    const QStringList CommonPrivilegeNames{
        "SeDebugPrivilege",
        "SeImpersonatePrivilege",
        "SeAssignPrimaryTokenPrivilege",
        "SeIncreaseQuotaPrivilege",
        "SeTcbPrivilege",
        "SeBackupPrivilege",
        "SeRestorePrivilege",
        "SeLoadDriverPrivilege",
        "SeSecurityPrivilege",
        "SeTakeOwnershipPrivilege"
    };

    // BitmaskFlagDefinition 作用：
    // - 统一描述“复选框可勾选的位标志定义”；
    // - nameText：显示名称；
    // - value：该位标志对应的掩码值；
    // - descriptionText：鼠标悬停说明，帮助用户理解语义。
    struct BitmaskFlagDefinition
    {
        const char* nameText = "";            // 标志名（例如 CREATE_SUSPENDED）。
        std::uint32_t value = 0;              // 标志位掩码（DWORD）。
        const char* descriptionText = "";     // 标志用途说明文本。
    };

    // CreateProcess.dwCreationFlags 常用且可组合的位标志全集。
    const std::vector<BitmaskFlagDefinition> CreateProcessFlagDefinitions{
        { "DEBUG_PROCESS", 0x00000001U, "调试子进程和其后代进程。" },
        { "DEBUG_ONLY_THIS_PROCESS", 0x00000002U, "仅调试当前创建的子进程。" },
        { "CREATE_SUSPENDED", 0x00000004U, "主线程创建后先挂起。" },
        { "DETACHED_PROCESS", 0x00000008U, "控制台进程脱离父控制台。" },
        { "CREATE_NEW_CONSOLE", 0x00000010U, "为新进程分配新控制台窗口。" },
        { "NORMAL_PRIORITY_CLASS", 0x00000020U, "普通优先级类。" },
        { "IDLE_PRIORITY_CLASS", 0x00000040U, "空闲优先级类。" },
        { "HIGH_PRIORITY_CLASS", 0x00000080U, "高优先级类。" },
        { "REALTIME_PRIORITY_CLASS", 0x00000100U, "实时优先级类（高风险）。" },
        { "CREATE_NEW_PROCESS_GROUP", 0x00000200U, "创建新的进程组。" },
        { "CREATE_UNICODE_ENVIRONMENT", 0x00000400U, "环境块按 Unicode 传递。" },
        { "CREATE_SEPARATE_WOW_VDM", 0x00000800U, "16 位应用使用独立 WOW VDM。" },
        { "CREATE_SHARED_WOW_VDM", 0x00001000U, "16 位应用共享 WOW VDM。" },
        { "CREATE_FORCEDOS", 0x00002000U, "强制 DOS 兼容模式（历史选项）。" },
        { "BELOW_NORMAL_PRIORITY_CLASS", 0x00004000U, "低于普通优先级类。" },
        { "ABOVE_NORMAL_PRIORITY_CLASS", 0x00008000U, "高于普通优先级类。" },
        { "INHERIT_PARENT_AFFINITY", 0x00010000U, "继承父进程 CPU 亲和性。" },
        { "CREATE_PROTECTED_PROCESS", 0x00040000U, "创建受保护进程（受系统限制）。" },
        { "EXTENDED_STARTUPINFO_PRESENT", 0x00080000U, "启用 STARTUPINFOEX 扩展结构。" },
        { "PROCESS_MODE_BACKGROUND_BEGIN", 0x00100000U, "进入后台模式（I/O/CPU 降优先级）。" },
        { "PROCESS_MODE_BACKGROUND_END", 0x00200000U, "退出后台模式。" },
        { "CREATE_SECURE_PROCESS", 0x00400000U, "创建安全进程（受系统策略限制）。" },
        { "CREATE_BREAKAWAY_FROM_JOB", 0x01000000U, "允许脱离 Job 对象。" },
        { "CREATE_PRESERVE_CODE_AUTHZ_LEVEL", 0x02000000U, "保持代码授权级别。" },
        { "CREATE_DEFAULT_ERROR_MODE", 0x04000000U, "使用默认错误模式。" },
        { "CREATE_NO_WINDOW", 0x08000000U, "控制台进程不创建窗口。" },
        { "PROFILE_USER", 0x10000000U, "启用用户模式性能统计。" },
        { "PROFILE_KERNEL", 0x20000000U, "启用内核模式性能统计。" },
        { "PROFILE_SERVER", 0x40000000U, "启用服务器性能统计。" },
        { "CREATE_IGNORE_SYSTEM_DEFAULT", 0x80000000U, "忽略系统默认设置（较少使用）。" }
    };

    // STARTUPINFO.dwFlags 位标志全集。
    const std::vector<BitmaskFlagDefinition> StartupInfoFlagDefinitions{
        { "STARTF_USESHOWWINDOW", 0x00000001U, "启用 wShowWindow 字段。" },
        { "STARTF_USESIZE", 0x00000002U, "启用 dwXSize/dwYSize 字段。" },
        { "STARTF_USEPOSITION", 0x00000004U, "启用 dwX/dwY 字段。" },
        { "STARTF_USECOUNTCHARS", 0x00000008U, "启用控制台字符网格大小字段。" },
        { "STARTF_USEFILLATTRIBUTE", 0x00000010U, "启用 dwFillAttribute 字段。" },
        { "STARTF_RUNFULLSCREEN", 0x00000020U, "全屏模式启动（主要针对旧控制台）。" },
        { "STARTF_FORCEONFEEDBACK", 0x00000040U, "强制显示忙碌光标反馈。" },
        { "STARTF_FORCEOFFFEEDBACK", 0x00000080U, "关闭启动忙碌光标反馈。" },
        { "STARTF_USESTDHANDLES", 0x00000100U, "启用标准输入/输出/错误句柄字段。" },
        { "STARTF_USEHOTKEY", 0x00000200U, "启用热键字段（hStdInput 解释为 hotkey）。" },
        { "STARTF_TITLEISLINKNAME", 0x00000800U, "标题解释为 Shell 链接名。" },
        { "STARTF_TITLEISAPPID", 0x00001000U, "标题解释为 AppUserModelID。" },
        { "STARTF_PREVENTPINNING", 0x00002000U, "阻止任务栏固定（需 AppID）。" },
        { "STARTF_UNTRUSTEDSOURCE", 0x00008000U, "标记命令来源不可信。" },
        { "STARTF_HOLOGRAPHIC", 0x00040000U, "全息场景启动标记（特定平台）。" }
    };

    // STARTUPINFO.dwFillAttribute 控制台颜色/样式标志全集。
    const std::vector<BitmaskFlagDefinition> ConsoleFillAttributeDefinitions{
        { "FOREGROUND_BLUE", 0x0001U, "前景色：蓝。" },
        { "FOREGROUND_GREEN", 0x0002U, "前景色：绿。" },
        { "FOREGROUND_RED", 0x0004U, "前景色：红。" },
        { "FOREGROUND_INTENSITY", 0x0008U, "前景色高亮。" },
        { "BACKGROUND_BLUE", 0x0010U, "背景色：蓝。" },
        { "BACKGROUND_GREEN", 0x0020U, "背景色：绿。" },
        { "BACKGROUND_RED", 0x0040U, "背景色：红。" },
        { "BACKGROUND_INTENSITY", 0x0080U, "背景色高亮。" },
        { "COMMON_LVB_LEADING_BYTE", 0x0100U, "双字节字符前导字节标记。" },
        { "COMMON_LVB_TRAILING_BYTE", 0x0200U, "双字节字符后继字节标记。" },
        { "COMMON_LVB_GRID_HORIZONTAL", 0x0400U, "水平网格线。" },
        { "COMMON_LVB_GRID_LVERTICAL", 0x0800U, "左垂直网格线。" },
        { "COMMON_LVB_GRID_RVERTICAL", 0x1000U, "右垂直网格线。" },
        { "COMMON_LVB_REVERSE_VIDEO", 0x4000U, "反色显示。" },
        { "COMMON_LVB_UNDERSCORE", 0x8000U, "下划线显示。" }
    };

    // Token DesiredAccess 常用位标志全集（OpenProcessToken / DuplicateTokenEx 路径）。
    const std::vector<BitmaskFlagDefinition> TokenDesiredAccessDefinitions{
        { "TOKEN_ASSIGN_PRIMARY", 0x00000001U, "可把令牌分配给新进程主令牌。" },
        { "TOKEN_DUPLICATE", 0x00000002U, "可复制令牌。" },
        { "TOKEN_IMPERSONATE", 0x00000004U, "可模拟令牌。" },
        { "TOKEN_QUERY", 0x00000008U, "可查询令牌信息。" },
        { "TOKEN_QUERY_SOURCE", 0x00000010U, "可查询令牌来源。" },
        { "TOKEN_ADJUST_PRIVILEGES", 0x00000020U, "可调整令牌特权。" },
        { "TOKEN_ADJUST_GROUPS", 0x00000040U, "可调整令牌组。" },
        { "TOKEN_ADJUST_DEFAULT", 0x00000080U, "可调整默认 DACL/Owner 等。" },
        { "TOKEN_ADJUST_SESSIONID", 0x00000100U, "可调整会话 ID。" },
        { "DELETE", 0x00010000U, "标准删除权限。" },
        { "READ_CONTROL", 0x00020000U, "标准读取安全描述符权限。" },
        { "WRITE_DAC", 0x00040000U, "标准写 DACL 权限。" },
        { "WRITE_OWNER", 0x00080000U, "标准写 Owner 权限。" },
        { "ACCESS_SYSTEM_SECURITY", 0x01000000U, "访问 SACL 权限（高权限）。" },
        { "MAXIMUM_ALLOWED", 0x02000000U, "请求对象允许的最大权限。" },
        { "GENERIC_ALL", 0x10000000U, "通用全部权限映射。" },
        { "GENERIC_EXECUTE", 0x20000000U, "通用执行权限映射。" },
        { "GENERIC_WRITE", 0x40000000U, "通用写权限映射。" },
        { "GENERIC_READ", 0x80000000U, "通用读权限映射。" }
    };
}

ProcessDock::ProcessDock(QWidget* parent)
    : QWidget(parent)
{
    // 初始化硬件并发参数：至少按 1 核处理，避免除零。
    m_logicalCpuCount = std::max<std::uint32_t>(1, std::thread::hardware_concurrency());

    // 构造阶段按“UI -> 连接 -> 定时器 -> 首次刷新”顺序执行。
    m_activityTotalPhysicalMemoryMB = totalPhysicalMemoryMB();
    initializeUi();
    initializeConnections();
    initializeTimer();
    m_monitoringEnabled = false;
}

void ProcessDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // 首次显示或重新回到可见状态时，把搜索焦点交给进程搜索框：
    // - 用户切到该页面后可以直接键入搜索词；
    // - 不要求先手动点击搜索框。
    if (m_sideTabWidget != nullptr && m_sideTabWidget->currentWidget() == m_processListPage)
    {
        focusProcessSearchBox(true);
    }

    if (m_initialRefreshScheduled)
    {
        return;
    }

    m_initialRefreshScheduled = true;
    m_monitoringEnabled = true;
    m_activityRecordingEnabled = true;
    if (m_activitySamples.empty())
    {
        m_activityRecordingStartTick100ns = steadyNow100ns();
        m_activityNextSequence = 0;
    }
    if (m_tableRefreshIntervalEdit != nullptr)
    {
        m_tableRefreshIntervalEdit->setEnabled(true);
    }
    if (m_refreshIntervalEdit != nullptr)
    {
        m_refreshIntervalEdit->setEnabled(true);
    }
    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->start();
    }

    // 首次显示后再触发首轮刷新：
    // - 避免主窗口启动阶段提前拉起进程枚举；
    // - 用户真正切到“进程”页时再加载数据。
    QTimer::singleShot(0, this, [this]()
        {
            requestAsyncRefresh(true);
        });
}

void ProcessDock::refreshThemeVisuals()
{
    // 仅重建当前表格可视层，不触发新的后台枚举任务。
    // 用途：深浅色切换后，立即刷新“新增/退出”行的主题高亮色。
    rebuildTable();
    rebuildThreadTable();
}

void ProcessDock::initializeUi()
{
    // 根布局只容纳一个侧边栏 tab 控件。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    m_sideTabWidget = new QTabWidget(this);
    m_sideTabWidget->setTabPosition(QTabWidget::West);
    m_sideTabWidget->setDocumentMode(true);
    m_sideTabWidget->setIconSize(SideTabIconSize);

    // 左侧页签使用内容自适应宽度，避免固定宽度导致不同字号/语言下被截断。
    // 页签字号不在局部 QSS 中设置，统一继承 Qt 默认应用字号。
    if (m_sideTabWidget->tabBar() != nullptr)
    {
        m_sideTabWidget->tabBar()->setExpanding(false);
        m_sideTabWidget->tabBar()->setUsesScrollButtons(true);
        m_sideTabWidget->tabBar()->setStyleSheet(QStringLiteral(
            "QTabBar{background:transparent;border:none;}"
            "QTabBar::tab{min-height:%1px;padding:5px 5px;margin:0px;border:none;border-radius:0px;}"
            "QTabBar::tab:selected{background-color:%2;color:#FFFFFF;font-weight:700;}"
            "QTabBar::tab:hover:!selected{background-color:%3;color:%4;}" )
            .arg(ProcessSideTabMinHeightPx)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceAltHex())
            .arg(KswordTheme::TextPrimaryHex()));
    }

    // “进程列表”页是本模块核心页面。
    m_processListPage = new QWidget(this);
    m_processPageLayout = new QVBoxLayout(m_processListPage);
    m_processPageLayout->setContentsMargins(6, 6, 6, 6);
    m_processPageLayout->setSpacing(6);

    // 初始化上方控制栏和下方表格。
    initializeTopControls();
    initializeProcessActivityPanel();
    initializeProcessTable();
    initializeThreadPage();
    initializeCreateProcessPage();

    m_rootLayout->addWidget(m_sideTabWidget);
}

void ProcessDock::initializeTopControls()
{
    // 控制区改为“两行布局”：第一行放操作按钮，第二行单独放监控状态。
    QVBoxLayout* controlContainerLayout = new QVBoxLayout();
    controlContainerLayout->setContentsMargins(0, 0, 0, 0);
    controlContainerLayout->setSpacing(4);

    m_controlLayout = new QHBoxLayout();
    m_controlLayout->setContentsMargins(0, 0, 0, 0);
    m_controlLayout->setSpacing(8);
    m_statusLayout = new QHBoxLayout();
    m_statusLayout->setContentsMargins(0, 0, 0, 0);
    m_statusLayout->setSpacing(8);

    // 遍历策略下拉框：
    // 1) Toolhelp（CreateToolhelp32Snapshot + Process32First/Next）
    // 2) NtQuerySystemInformation
    // 说明：不再默认 Auto，直接明确展示当前使用的方法。
    m_strategyCombo = new QComboBox(this);
    m_strategyCombo->addItem(QIcon(IconRefresh), "Toolhelp Snapshot / Process32First / Process32Next");
    m_strategyCombo->addItem(QIcon(IconRefresh), "NtQuerySystemInformation");
    m_strategyCombo->setCurrentIndex(1);
    m_strategyCombo->setToolTip("指定进程遍历方案");
    // 自适应宽度策略：避免长文本把 Dock 顶出横向滚动条。
    m_strategyCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_strategyCombo->setMinimumContentsLength(18);
    m_strategyCombo->setMaximumWidth(260);

    // 树/列表切换按钮：按需求仅显示图标。
    m_treeToggleButton = new QPushButton(QIcon(IconTree), "", this);
    m_treeToggleButton->setIconSize(DefaultIconSize);
    m_treeToggleButton->setFixedSize(CompactIconButtonSize);
    m_treeToggleButton->setCheckable(true);
    m_treeToggleButton->setChecked(false);
    m_treeToggleButton->setToolTip("切换为树状视图");

    // 视图模式下拉框：默认监视视图。
    m_viewModeCombo = new QComboBox(this);
    m_viewModeCombo->addItem(QIcon(IconList), "监视视图");
    m_viewModeCombo->addItem(QIcon(IconProcessMain), "详细信息视图");
    m_viewModeCombo->setCurrentIndex(static_cast<int>(ViewMode::Monitor));
    m_viewModeCombo->setToolTip("切换监视视图 / 详细信息视图");
    m_viewModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_viewModeCombo->setMinimumContentsLength(8);
    m_viewModeCombo->setMaximumWidth(180);

    const QString comboStyle = buildBlueComboBoxStyle();
    m_strategyCombo->setStyleSheet(comboStyle);
    m_viewModeCombo->setStyleSheet(comboStyle);

    // 开始/暂停按钮：按需求仅显示图标。
    m_startButton = new QPushButton(QIcon(IconStart), "", this);
    m_pauseButton = new QPushButton(QIcon(IconPause), "", this);
    m_startButton->setIconSize(DefaultIconSize);
    m_pauseButton->setIconSize(DefaultIconSize);
    m_startButton->setFixedSize(CompactIconButtonSize);
    m_pauseButton->setFixedSize(CompactIconButtonSize);
    m_startButton->setToolTip("开始周期性刷新进程列表，并同步记录进程活动");
    m_pauseButton->setToolTip("暂停周期性刷新进程列表，并同步停止记录");

    // 进程表刷新间隔：
    // - 该间隔只控制下方进程表格重绘频率，默认 2 秒；
    // - 后台监视和活动打点仍走 0.1 秒采样，避免表格渲染成本影响记录精度。
    m_refreshLabel = new QLabel("列表刷新(s):", this);
    m_tableRefreshIntervalEdit = new QLineEdit(this);
    m_tableRefreshIntervalEdit->setText(QStringLiteral("2.0"));
    m_tableRefreshIntervalEdit->setFixedWidth(64);
    m_tableRefreshIntervalEdit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_tableRefreshIntervalEdit->setValidator(new QDoubleValidator(0.5, 60.0, 2, m_tableRefreshIntervalEdit));
    m_tableRefreshIntervalEdit->setToolTip("只控制下方进程表格刷新频率，默认 2 秒。");
    m_tableRefreshIntervalEdit->setStyleSheet(buildBlueLineEditStyle());
    m_tableRefreshIntervalEdit->setEnabled(false);

    // 记录/打点间隔输入框：
    // - 允许小数秒，默认 0.1s；
    // - 该间隔驱动后台监视刷新和活动记录采样。
    m_sampleIntervalLabel = new QLabel("记录打点(s):", this);
    m_refreshIntervalEdit = new QLineEdit(this);
    m_refreshIntervalEdit->setText(QStringLiteral("0.1"));
    m_refreshIntervalEdit->setFixedWidth(64);
    m_refreshIntervalEdit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_refreshIntervalEdit->setValidator(new QDoubleValidator(0.05, 60.0, 3, m_refreshIntervalEdit));
    m_refreshIntervalEdit->setToolTip("记录打点间隔，允许输入小数秒，默认 0.1；过小间隔会提高系统枚举开销。");
    m_refreshIntervalEdit->setStyleSheet(buildBlueLineEditStyle());
    m_refreshIntervalEdit->setEnabled(false);

    // 刷新状态标签：明确告诉用户当前是否在刷新，以及最后耗时。
    m_refreshStateLabel = new QLabel("● 空闲", this);
    m_refreshStateLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    m_refreshStateLabel->setToolTip("当前刷新状态");

    // 进程搜索框：
    // - 直接基于当前缓存做本地过滤，不额外触发系统查询；
    // - 切到“进程列表”页后会自动聚焦，支持直接键入搜索词。
    m_processSearchLineEdit = new QLineEdit(this);
    m_processSearchLineEdit->setClearButtonEnabled(true);
    m_processSearchLineEdit->setPlaceholderText("搜索 PID / 名称 / 路径 / 命令行 / 用户");
    m_processSearchLineEdit->setToolTip("切到进程列表页后可直接输入搜索词");
    m_processSearchLineEdit->setStyleSheet(buildBlueLineEditStyle());
    m_processSearchLineEdit->setMaximumWidth(320);

    // 内核对比开关：
    // - 勾选后每轮刷新额外请求 R0 进程列表；
    // - UI 会把“R0 有、R3 无”的进程按红色高亮显示。
    m_kernelCompareCheck = new QCheckBox("刷新时对比内核进程（查隐藏）", this);
    m_kernelCompareCheck->setChecked(false);
    m_kernelCompareCheck->setToolTip("勾选后刷新会额外请求驱动进程列表，并高亮仅内核可见的进程。");

    // Ksword 可恢复隐藏显示开关：
    // - 默认不显示 R0 标记隐藏的进程；
    // - 勾选后仍展示这些行，便于右键“取消隐藏”。
    m_showKswordHiddenProcessCheck = new QCheckBox(QStringLiteral("显示Ksword隐藏项"), this);
    m_showKswordHiddenProcessCheck->setChecked(false);
    m_showKswordHiddenProcessCheck->setToolTip(QStringLiteral("显示通过 R0 可恢复隐藏标记过滤的进程，用于取消隐藏或核对状态。"));

    // 按钮统一蓝色风格（图标按钮版本）。
    const QString buttonStyle = buildBlueButtonStyle(true);
    m_treeToggleButton->setStyleSheet(buttonStyle);
    m_startButton->setStyleSheet(buttonStyle);
    m_pauseButton->setStyleSheet(buttonStyle);

    // 第一行放“操作按钮 + 刷新间隔”。
    m_controlLayout->addWidget(m_strategyCombo);
    m_controlLayout->addWidget(m_treeToggleButton);
    m_controlLayout->addWidget(m_viewModeCombo);
    m_controlLayout->addWidget(m_startButton);
    m_controlLayout->addWidget(m_pauseButton);
    m_controlLayout->addWidget(m_processSearchLineEdit);
    m_controlLayout->addWidget(m_kernelCompareCheck);
    m_controlLayout->addWidget(m_showKswordHiddenProcessCheck);
    m_controlLayout->addStretch(1);
    m_controlLayout->addWidget(m_refreshLabel);
    m_controlLayout->addWidget(m_tableRefreshIntervalEdit);
    m_controlLayout->addWidget(m_sampleIntervalLabel);
    m_controlLayout->addWidget(m_refreshIntervalEdit);
    // 第二行只放“监控状态”，避免与操作按钮挤在同一行导致横向滚动。
    m_statusLayout->addWidget(m_refreshStateLabel, 1);
    m_statusLayout->addStretch(1);

    controlContainerLayout->addLayout(m_controlLayout);
    controlContainerLayout->addLayout(m_statusLayout);
    m_processPageLayout->addLayout(controlContainerLayout);
}

void ProcessDock::initializeProcessActivityPanel()
{
    // 活动面板放在进程表上方：
    // - 不额外枚举进程，只消费每轮刷新后的 m_cacheByIdentity；
    // - 图表、时间轴和快照共享同一份有界样本缓存。
    m_activityPanelWidget = new QWidget(m_processListPage);
    m_activityPanelWidget->setObjectName(QStringLiteral("processActivityPanelWidget"));
    m_activityPanelWidget->setAutoFillBackground(false);
    m_activityPanelWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_activityPanelWidget->setStyleSheet(QStringLiteral(
        "QWidget#processActivityPanelWidget {"
        "  background:transparent;"
        "  background-color:transparent;"
        "  border:1px solid %1;"
        "  border-radius:4px;"
        "}")
        .arg(KswordTheme::BorderHex()));

    QVBoxLayout* panelLayout = new QVBoxLayout(m_activityPanelWidget);
    panelLayout->setContentsMargins(6, 6, 6, 6);
    panelLayout->setSpacing(5);

    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);

    m_activityClearButton = new QPushButton(QStringLiteral("清空"), m_activityPanelWidget);
    m_activityClearButton->setToolTip(QStringLiteral("清空当前刷新同步记录的进程活动样本。"));
    m_activityClearButton->setStyleSheet(buildBlueButtonStyle(false));

    m_activityBackgroundRecordCheck = new QCheckBox(QStringLiteral("后台保持刷新/记录"), m_activityPanelWidget);
    m_activityBackgroundRecordCheck->setToolTip(QStringLiteral("默认仅进程列表 Tab 显示时刷新和记录；勾选后切到其它 Tab 仍继续刷新并记录。"));

    m_activityListOnlyRefreshCheck = new QCheckBox(QStringLiteral("只刷新列表"), m_activityPanelWidget);
    m_activityListOnlyRefreshCheck->setToolTip(QStringLiteral("勾选后周期刷新仍会更新进程列表，但不会向上方时间轴写入新的活动记录。"));

    const QString metricButtonStyle = QStringLiteral(
        "QPushButton {"
        "  color:%1;"
        "  background:%2;"
        "  border:1px solid %3;"
        "  border-radius:3px;"
        "  padding:3px 8px;"
        "}"
        "QPushButton:checked {"
        "  color:#FFFFFF;"
        "  background:%4;"
        "  border:1px solid %4;"
        "}"
        "QPushButton:hover {"
        "  border:1px solid %4;"
        "}")
        .arg(KswordTheme::TextPrimaryHex())
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::BorderHex())
        .arg(KswordTheme::PrimaryBlueHex);

    // 指标按钮必须可独立开关：
    // - 默认全部点亮，用户打开页面即可看到 CPU/内存/磁盘/网络/GPU 全部曲线；
    // - 后续可以按需关闭单项指标，避免曲线过密。
    auto createMetricButton =
        [this, &metricButtonStyle](const QString& text, const bool checkedByDefault)
        {
            QPushButton* button = new QPushButton(text, m_activityPanelWidget);
            button->setCheckable(true);
            button->setChecked(checkedByDefault);
            button->setStyleSheet(metricButtonStyle);
            button->setToolTip(QStringLiteral("切换该指标是否绘制在上方百分比折线图中。"));
            return button;
        };
    m_activityCpuButton = createMetricButton(QStringLiteral("CPU"), true);
    m_activityMemoryButton = createMetricButton(QStringLiteral("内存"), true);
    m_activityDiskButton = createMetricButton(QStringLiteral("磁盘"), true);
    m_activityNetworkButton = createMetricButton(QStringLiteral("网络"), true);
    m_activityGpuButton = createMetricButton(QStringLiteral("GPU"), true);

    // 准星按钮放在“网络/GPU”等时间轴指标按钮右侧：
    // - 交互与窗口页拾取按钮一致，必须按住拖到目标窗口后松开；
    // - 释放后不打开窗口详情，而是按目标窗口所属 PID 过滤进程列表并打开进程详情。
    ProcessWindowPickerDragButton* processPickerButton = new ProcessWindowPickerDragButton(m_activityPanelWidget);
    processPickerButton->setIcon(QIcon(IconWindowPickerTarget));
    processPickerButton->setIconSize(QSize(16, 16));
    processPickerButton->setFixedSize(CompactIconButtonSize);
    processPickerButton->setStyleSheet(buildBlueButtonStyle(true));
    processPickerButton->setToolTip(QStringLiteral("按住并拖拽准星到目标窗口，松开后按该窗口 PID 筛选进程并打开进程详细信息"));
    processPickerButton->setReleaseCallback([this](const QPoint& globalPos) {
        handleProcessWindowPickerRelease(globalPos);
    });
    m_activityProcessPickerButton = processPickerButton;

    m_activityStatusLabel = new QLabel(QStringLiteral("进程活动：未开始刷新 | 样本 0"), m_activityPanelWidget);
    m_activityStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    toolbarLayout->addWidget(m_activityClearButton);
    toolbarLayout->addWidget(m_activityBackgroundRecordCheck);
    toolbarLayout->addWidget(m_activityListOnlyRefreshCheck);
    toolbarLayout->addSpacing(8);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("显示:"), m_activityPanelWidget));
    toolbarLayout->addWidget(m_activityCpuButton);
    toolbarLayout->addWidget(m_activityMemoryButton);
    toolbarLayout->addWidget(m_activityDiskButton);
    toolbarLayout->addWidget(m_activityNetworkButton);
    toolbarLayout->addWidget(m_activityGpuButton);
    toolbarLayout->addWidget(m_activityProcessPickerButton);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(m_activityStatusLabel);

    m_activityChartWidget = new ProcessActivityChartWidget(this, m_activityPanelWidget);
    m_activityChartWidget->setToolTip(QString());

    m_activityTimelineSlider = new ProcessActivityTimelineSlider(this, m_activityPanelWidget);
    m_activityTimelineSlider->setRange(0, 0);
    m_activityTimelineSlider->setValue(0);
    m_activityTimelineSlider->setVisible(false);
    m_activityTimelineSlider->setToolTip(QStringLiteral("隐藏时间轴：内部仅保存当前样本索引，用户通过折线图点击切换历史时刻。"));

    m_activitySnapshotLabel = new QLabel(QStringLiteral("时间轴快照：暂无样本"), m_activityPanelWidget);
    m_activitySnapshotLabel->setWordWrap(true);
    m_activitySnapshotLabel->setMinimumHeight(36);
    m_activitySnapshotLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_activitySnapshotLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  color:%1;"
        "  background:transparent;"
        "  background-color:transparent;"
        "  border:1px solid %2;"
        "  border-radius:3px;"
        "  padding:4px;"
        "}")
        .arg(KswordTheme::TextSecondaryHex())
        .arg(KswordTheme::BorderHex()));

    panelLayout->addLayout(toolbarLayout);
    panelLayout->addWidget(m_activityChartWidget);
    panelLayout->addWidget(m_activitySnapshotLabel);
    m_processPageLayout->addWidget(m_activityPanelWidget, 0);
}

void ProcessDock::handleProcessWindowPickerRelease(const QPoint& globalPos)
{
    // 该函数是进程页准星拾取的业务入口：
    // - 命中逻辑与窗口页保持一致，先拿鼠标下窗口，再回退到根窗口；
    // - 结果不进入窗口详情，而是转成 PID 过滤和进程详情。
    kLogEvent pickEvent;
    info << pickEvent
        << "[ProcessDock] 进程准星拾取释放, x="
        << globalPos.x()
        << ", y="
        << globalPos.y()
        << eol;

    POINT nativePoint{};
    nativePoint.x = globalPos.x();
    nativePoint.y = globalPos.y();

    // rawWindowHandle 是鼠标下最细粒度窗口；rootWindowHandle 用于顶级窗口回退。
    HWND rawWindowHandle = ::WindowFromPoint(nativePoint);
    HWND rootWindowHandle = rawWindowHandle != nullptr ? ::GetAncestor(rawWindowHandle, GA_ROOT) : nullptr;
    HWND targetWindowHandle = rawWindowHandle != nullptr ? rawWindowHandle : rootWindowHandle;
    if (targetWindowHandle == nullptr || ::IsWindow(targetWindowHandle) == FALSE)
    {
        warn << pickEvent
            << "[ProcessDock] 进程准星拾取失败：WindowFromPoint 未命中有效窗口。"
            << eol;
        QMessageBox::information(
            this,
            QStringLiteral("进程拾取"),
            QStringLiteral("未命中可用窗口，请重试。"));
        return;
    }

    // 优先解析原始窗口 PID；异常时再用根窗口 PID 兜底，贴近窗口页拾取体验。
    DWORD targetPid = 0;
    DWORD targetTid = ::GetWindowThreadProcessId(targetWindowHandle, &targetPid);
    if ((targetPid == 0 || targetTid == 0) && rootWindowHandle != nullptr && rootWindowHandle != targetWindowHandle)
    {
        targetWindowHandle = rootWindowHandle;
        targetPid = 0;
        targetTid = ::GetWindowThreadProcessId(targetWindowHandle, &targetPid);
    }

    const quint64 targetHwndValue = static_cast<quint64>(reinterpret_cast<quintptr>(targetWindowHandle));
    if (targetPid == 0)
    {
        warn << pickEvent
            << "[ProcessDock] 进程准星拾取失败：无法解析目标窗口 PID, hwnd=0x"
            << std::hex
            << targetHwndValue
            << std::dec
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("进程拾取"),
            QStringLiteral("无法读取目标窗口所属进程 PID。"));
        return;
    }

    // 切回实时表格：用户拖窗口定位的是当前系统进程，历史快照表格不应继续覆盖实时列表。
    m_activityTimelinePinnedToLatest = true;
    m_activityTableSnapshotIndex = -1;
    m_activityTableSnapshotRecords.clear();
    if (m_activityTimelineSlider != nullptr && !m_activitySamples.empty())
    {
        const bool oldUpdating = m_activityTimelineSliderUpdating;
        m_activityTimelineSliderUpdating = true;
        m_activityTimelineSlider->setValue(static_cast<int>(m_activitySamples.size()) - 1);
        m_activityTimelineSliderUpdating = oldUpdating;
    }

    // 设置进程列表筛选器为 PID：
    // - 直接写入顶部搜索框，复用现有过滤逻辑和 UI 可见状态；
    // - blockSignals 后手动 rebuild，避免历史状态切换时重复重建。
    if (m_processSearchLineEdit != nullptr)
    {
        QSignalBlocker blocker(m_processSearchLineEdit);
        m_processSearchLineEdit->setText(QStringLiteral("pid:%1").arg(static_cast<qulonglong>(targetPid)));
    }
    // 优先把目标 PID 对应行设为重建后的当前行，筛选后用户能直接看到定位结果。
    m_trackedSelectedIdentityKey.clear();
    m_trackedSelectedIdentityKeys.clear();
    for (const auto& cachePair : m_cacheByIdentity)
    {
        if (cachePair.second.record.pid == targetPid)
        {
            m_trackedSelectedIdentityKey = cachePair.first;
            m_trackedSelectedIdentityKeys.push_back(cachePair.first);
            break;
        }
    }
    rebuildTable();
    updateProcessActivityStatusLabel();

    info << pickEvent
        << "[ProcessDock] 进程准星拾取成功, hwnd=0x"
        << std::hex
        << targetHwndValue
        << std::dec
        << ", pid="
        << targetPid
        << ", tid="
        << targetTid
        << "，已设置进程列表筛选器并打开进程详情。"
        << eol;
    openProcessDetailWindowByPid(static_cast<std::uint32_t>(targetPid));
}

void ProcessDock::initializeProcessTable()
{
    m_processTable = new QTreeWidget(this);
    m_processTable->setColumnCount(static_cast<int>(TableColumn::Count));
    m_processTable->setHeaderLabels(ProcessTableHeaders);
    m_processTable->setRootIsDecorated(false);
    m_processTable->setItemsExpandable(false);
    m_processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 选择模式：
    // - 普通左键仍保持单行焦点；
    // - 按住 Ctrl 左键点击时由 Qt 进入复选式多选/取消选择；
    // - 右键菜单会读取所有已选行并批量执行动作。
    m_processTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processTable->setUniformRowHeights(true);
    m_processTable->setSortingEnabled(true);
    m_processTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTable->setAlternatingRowColors(true);
    m_processTable->setItemDelegateForColumn(
        toColumnIndex(TableColumn::Name),
        new ProcessNameDelegate(m_processTable));
    // 列宽由自适应逻辑统一控制，强制关闭内部横向滚动条。
    m_processTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 表头支持拖动、右键显示/隐藏列。
    QHeaderView* headerView = m_processTable->header();
    headerView->setSectionsMovable(true);
    headerView->setStretchLastSection(false);
    headerView->setContextMenuPolicy(Qt::CustomContextMenu);
    headerView->setStyleSheet(QStringLiteral(
        "QHeaderView::section {"
        "  color: %1;"
        "  background: %2;"
        "  border: 1px solid %3;"
        "  padding: 4px;"
        "  font-weight: 600;"
        "}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::BorderHex()));

    applyDefaultColumnWidths();
    applyViewMode(ViewMode::Monitor);
    applyAdaptiveColumnWidths();
    m_processPageLayout->addWidget(m_processTable, 1);

    // R0 功能标识：
    // - 进程页包含“R0结束进程”动作，因此在右下角固定放置内核标识图；
    // - 路径按项目规范使用固定 Kernel.png 资源文件。
    QLabel* r0KernelBadgeLabel = new QLabel(m_processListPage);
    r0KernelBadgeLabel->setObjectName(QStringLiteral("r0KernelBadgeLabel"));
    r0KernelBadgeLabel->setToolTip(QStringLiteral("R0 功能标识"));
    const QPixmap kernelBadgePixmap(KernelBadgeImagePath);
    if (!kernelBadgePixmap.isNull())
    {
        r0KernelBadgeLabel->setPixmap(kernelBadgePixmap.scaled(
            52,
            52,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    }
    m_processPageLayout->addWidget(
        r0KernelBadgeLabel,
        0,
        Qt::AlignRight | Qt::AlignBottom);

    // 满足需求 3.1：侧边栏 Tab 中包含“进程列表”页签。
    m_sideTabWidget->addTab(m_processListPage, blueTintedIcon(IconProcessMain), "进程列表");
    m_sideTabWidget->setCurrentIndex(0);
    refreshSideTabIconContrast();
}

void ProcessDock::initializeCreateProcessPage()
{
    m_createProcessPage = new QWidget(this);
    applyTransparentContainerStyle(m_createProcessPage);
    m_createProcessPageLayout = new QVBoxLayout(m_createProcessPage);
    m_createProcessPageLayout->setContentsMargins(6, 6, 6, 6);
    m_createProcessPageLayout->setSpacing(6);

    QScrollArea* scrollArea = new QScrollArea(m_createProcessPage);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    applyTransparentContainerStyle(scrollArea);
    m_createProcessPageLayout->addWidget(scrollArea, 1);

    QWidget* contentWidget = new QWidget(scrollArea);
    applyTransparentContainerStyle(contentWidget);
    QVBoxLayout* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(2, 2, 2, 2);
    contentLayout->setSpacing(8);
    scrollArea->setWidget(contentWidget);

    const QString inputStyle = buildBlueLineEditStyle();
    const QString comboStyle = buildBlueComboBoxStyle();
    const QString buttonStyle = buildBlueButtonStyle(false);

    // 每次重建页面前先清空位标志复选框缓存，避免重复初始化导致悬空指针。
    m_creationFlagChecks.clear();
    m_startupFlagChecks.clear();
    m_startupFillAttributeChecks.clear();
    m_tokenDesiredAccessChecks.clear();

    // buildBitmaskCheckGroup 作用：
    // - 根据“标志定义列表”自动生成复选框分组；
    // - 每个复选框通过 Qt Property 保存 flagValue/flagName，后续可统一做组合计算。
    const auto buildBitmaskCheckGroup =
        [](
            QWidget* parentWidget,
            const QString& groupTitle,
            const std::vector<BitmaskFlagDefinition>& definitionList,
            std::vector<QCheckBox*>* outputCheckBoxList) -> QGroupBox*
    {
        QGroupBox* groupBox = new QGroupBox(groupTitle, parentWidget);
        applyTransparentContainerStyle(groupBox);
        QGridLayout* groupLayout = new QGridLayout(groupBox);
        groupLayout->setContentsMargins(6, 6, 6, 6);
        groupLayout->setHorizontalSpacing(10);
        groupLayout->setVerticalSpacing(4);

        const int columnCount = 3;
        for (std::size_t index = 0; index < definitionList.size(); ++index)
        {
            const BitmaskFlagDefinition& definition = definitionList[index];
            QCheckBox* flagCheck = new QCheckBox(QString::fromUtf8(definition.nameText), groupBox);
            flagCheck->setProperty("flagValue", static_cast<qulonglong>(definition.value));
            flagCheck->setProperty("flagName", QString::fromUtf8(definition.nameText));
            flagCheck->setToolTip(
                QStringLiteral("%1\n值: 0x%2\n说明: %3")
                .arg(QString::fromUtf8(definition.nameText))
                .arg(QString::number(static_cast<qulonglong>(definition.value), 16).toUpper())
                .arg(QString::fromUtf8(definition.descriptionText)));

            const int row = static_cast<int>(index / static_cast<std::size_t>(columnCount));
            const int col = static_cast<int>(index % static_cast<std::size_t>(columnCount));
            groupLayout->addWidget(flagCheck, row, col);

            if (outputCheckBoxList != nullptr)
            {
                outputCheckBoxList->push_back(flagCheck);
            }
        }
        return groupBox;
    };

    // 1) 创建方式 + 令牌来源配置。
    QGroupBox* methodGroup = new QGroupBox("创建方式 / 令牌来源", contentWidget);
    applyTransparentContainerStyle(methodGroup);
    QGridLayout* methodLayout = new QGridLayout(methodGroup);
    methodLayout->setHorizontalSpacing(8);
    methodLayout->setVerticalSpacing(6);
    m_createMethodCombo = new QComboBox(methodGroup);
    m_createMethodCombo->addItem("CreateProcessW");
    m_createMethodCombo->addItem("CreateProcessAsTokenW (内部使用 CreateProcessAsUserW + fallback)");
    m_createMethodCombo->setStyleSheet(comboStyle);
    m_createMethodCombo->setCurrentIndex(0);
    m_createMethodCombo->setToolTip("默认直接调用 CreateProcessW；切换到 Token 模式时会按 PID 打开并调整令牌。");

    m_tokenSourcePidEdit = new QLineEdit("0", methodGroup);
    m_tokenDesiredAccessEdit = new QLineEdit("0x00000FAB", methodGroup);
    m_tokenDuplicatePrimaryCheck = new QCheckBox("DuplicateTokenEx 成 PrimaryToken", methodGroup);
    m_tokenDuplicatePrimaryCheck->setChecked(true);
    m_tokenSourcePidEdit->setStyleSheet(inputStyle);
    m_tokenDesiredAccessEdit->setStyleSheet(inputStyle);

    // 方法选择区域补充中文语义，避免只看英文 API 名不易理解。
    methodLayout->addWidget(new QLabel("API（创建方式）:", methodGroup), 0, 0);
    methodLayout->addWidget(m_createMethodCombo, 0, 1, 1, 3);
    methodLayout->addWidget(new QLabel("源 PID（令牌来源进程）:", methodGroup), 1, 0);
    methodLayout->addWidget(m_tokenSourcePidEdit, 1, 1);
    methodLayout->addWidget(new QLabel("令牌访问掩码（DesiredAccess）:", methodGroup), 1, 2);
    methodLayout->addWidget(m_tokenDesiredAccessEdit, 1, 3);
    methodLayout->addWidget(m_tokenDuplicatePrimaryCheck, 2, 0, 1, 4);

    // Token DesiredAccess 位标志勾选区：
    // - 覆盖最常见 TOKEN_* / 标准权限 / GENERIC_*；
    // - 用户可通过勾选组合，自动拼出访问掩码。
    QGroupBox* tokenAccessGroup = buildBitmaskCheckGroup(
        methodGroup,
        "Token DesiredAccess 位标志组合",
        TokenDesiredAccessDefinitions,
        &m_tokenDesiredAccessChecks);
    methodLayout->addWidget(tokenAccessGroup, 3, 0, 1, 4);
    contentLayout->addWidget(methodGroup);

    // 2) CreateProcessW 基础参数。
    QGroupBox* basicGroup = new QGroupBox("CreateProcessW 参数（全部可选 Null）", contentWidget);
    applyTransparentContainerStyle(basicGroup);
    QGridLayout* basicLayout = new QGridLayout(basicGroup);
    basicLayout->setHorizontalSpacing(8);
    basicLayout->setVerticalSpacing(6);

    m_useApplicationNameCheck = new QCheckBox("启用 lpApplicationName（应用程序路径）", basicGroup);
    m_useCommandLineCheck = new QCheckBox("启用 lpCommandLine（命令行参数）", basicGroup);
    m_useCurrentDirectoryCheck = new QCheckBox("启用 lpCurrentDirectory（当前工作目录）", basicGroup);
    m_useEnvironmentCheck = new QCheckBox("启用 lpEnvironment（环境变量块）", basicGroup);
    m_environmentUnicodeCheck = new QCheckBox("环境块按 Unicode 传递（CREATE_UNICODE_ENVIRONMENT）", basicGroup);
    m_inheritHandleCheck = new QCheckBox("bInheritHandles（是否继承句柄）=TRUE", basicGroup);

    m_applicationNameEdit = new QLineEdit(basicGroup);
    m_applicationBrowseButton = new QPushButton("浏览...", basicGroup);
    m_commandLineEdit = new QLineEdit(basicGroup);
    m_currentDirectoryEdit = new QLineEdit(basicGroup);
    m_currentDirectoryBrowseButton = new QPushButton("浏览...", basicGroup);
    m_environmentEditor = new QPlainTextEdit(basicGroup);
    m_creationFlagsEdit = new QLineEdit("0x00000000", basicGroup);
    m_environmentEditor->setPlaceholderText("每行一个 KEY=VALUE，留空则为 null。");
    m_environmentEditor->setFixedHeight(72);

    m_applicationNameEdit->setStyleSheet(inputStyle);
    m_commandLineEdit->setStyleSheet(inputStyle);
    m_currentDirectoryEdit->setStyleSheet(inputStyle);
    m_environmentEditor->setStyleSheet(inputStyle);
    m_creationFlagsEdit->setStyleSheet(inputStyle);
    m_applicationBrowseButton->setStyleSheet(buttonStyle);
    m_currentDirectoryBrowseButton->setStyleSheet(buttonStyle);

    m_useApplicationNameCheck->setChecked(false);
    m_useCommandLineCheck->setChecked(false);
    m_useCurrentDirectoryCheck->setChecked(false);
    m_useEnvironmentCheck->setChecked(false);
    m_environmentUnicodeCheck->setChecked(true);

    basicLayout->addWidget(m_useApplicationNameCheck, 0, 0);
    basicLayout->addWidget(m_applicationNameEdit, 0, 1, 1, 2);
    basicLayout->addWidget(m_applicationBrowseButton, 0, 3);
    basicLayout->addWidget(m_useCommandLineCheck, 1, 0);
    basicLayout->addWidget(m_commandLineEdit, 1, 1, 1, 3);
    basicLayout->addWidget(m_useCurrentDirectoryCheck, 2, 0);
    basicLayout->addWidget(m_currentDirectoryEdit, 2, 1, 1, 2);
    basicLayout->addWidget(m_currentDirectoryBrowseButton, 2, 3);
    basicLayout->addWidget(m_useEnvironmentCheck, 3, 0);
    basicLayout->addWidget(m_environmentEditor, 3, 1, 2, 3);
    basicLayout->addWidget(m_environmentUnicodeCheck, 5, 1, 1, 3);
    basicLayout->addWidget(new QLabel("dwCreationFlags（创建标志）:", basicGroup), 6, 0);
    basicLayout->addWidget(m_creationFlagsEdit, 6, 1);
    basicLayout->addWidget(m_inheritHandleCheck, 6, 2, 1, 2);

    // dwCreationFlags 位标志勾选区：
    // - 列出 CreateProcess 常见全部标志；
    // - 用户勾选后自动组合成掩码写回 dwCreationFlags 输入框。
    QGroupBox* creationFlagsGroup = buildBitmaskCheckGroup(
        basicGroup,
        "dwCreationFlags 位标志组合",
        CreateProcessFlagDefinitions,
        &m_creationFlagChecks);
    basicLayout->addWidget(creationFlagsGroup, 7, 0, 1, 4);
    contentLayout->addWidget(basicGroup);

    // 3) PROCESS / THREAD SECURITY_ATTRIBUTES。
    QGroupBox* securityGroup = new QGroupBox("SECURITY_ATTRIBUTES（Process / Thread）", contentWidget);
    applyTransparentContainerStyle(securityGroup);
    QGridLayout* securityLayout = new QGridLayout(securityGroup);
    securityLayout->setHorizontalSpacing(8);
    securityLayout->setVerticalSpacing(6);

    m_useProcessSecurityCheck = new QCheckBox("启用 lpProcessAttributes（进程安全属性）", securityGroup);
    m_processSecurityLengthEdit = new QLineEdit("0", securityGroup);
    m_processSecurityDescriptorEdit = new QLineEdit("0", securityGroup);
    m_processSecurityInheritCheck = new QCheckBox("bInheritHandle（进程 SA）", securityGroup);

    m_useThreadSecurityCheck = new QCheckBox("启用 lpThreadAttributes（线程安全属性）", securityGroup);
    m_threadSecurityLengthEdit = new QLineEdit("0", securityGroup);
    m_threadSecurityDescriptorEdit = new QLineEdit("0", securityGroup);
    m_threadSecurityInheritCheck = new QCheckBox("bInheritHandle（线程 SA）", securityGroup);

    m_processSecurityLengthEdit->setStyleSheet(inputStyle);
    m_processSecurityDescriptorEdit->setStyleSheet(inputStyle);
    m_threadSecurityLengthEdit->setStyleSheet(inputStyle);
    m_threadSecurityDescriptorEdit->setStyleSheet(inputStyle);

    securityLayout->addWidget(m_useProcessSecurityCheck, 0, 0);
    securityLayout->addWidget(new QLabel("nLength（结构体长度）", securityGroup), 0, 1);
    securityLayout->addWidget(m_processSecurityLengthEdit, 0, 2);
    securityLayout->addWidget(new QLabel("lpSecurityDescriptor（安全描述符指针）", securityGroup), 0, 3);
    securityLayout->addWidget(m_processSecurityDescriptorEdit, 0, 4);
    securityLayout->addWidget(m_processSecurityInheritCheck, 0, 5);
    securityLayout->addWidget(m_useThreadSecurityCheck, 1, 0);
    securityLayout->addWidget(new QLabel("nLength（结构体长度）", securityGroup), 1, 1);
    securityLayout->addWidget(m_threadSecurityLengthEdit, 1, 2);
    securityLayout->addWidget(new QLabel("lpSecurityDescriptor（安全描述符指针）", securityGroup), 1, 3);
    securityLayout->addWidget(m_threadSecurityDescriptorEdit, 1, 4);
    securityLayout->addWidget(m_threadSecurityInheritCheck, 1, 5);
    contentLayout->addWidget(securityGroup);

    // 4) STARTUPINFOW 全字段。
    QGroupBox* startupGroup = new QGroupBox("STARTUPINFOW（全部字段）", contentWidget);
    applyTransparentContainerStyle(startupGroup);
    QGridLayout* startupLayout = new QGridLayout(startupGroup);
    startupLayout->setHorizontalSpacing(8);
    startupLayout->setVerticalSpacing(6);
    m_useStartupInfoCheck = new QCheckBox("启用 lpStartupInfo（启动信息结构体，取消则传 NULL）", startupGroup);
    m_useStartupInfoCheck->setChecked(false);
    m_useStartupInfoCheck->setToolTip("注意：Win32 通常要求该参数非空，传 NULL 主要用于测试极限场景。");

    m_siCbEdit = new QLineEdit("0", startupGroup);
    m_siReservedEdit = new QLineEdit(startupGroup);
    m_siDesktopEdit = new QLineEdit(startupGroup);
    m_siTitleEdit = new QLineEdit(startupGroup);
    m_siXEdit = new QLineEdit("0", startupGroup);
    m_siYEdit = new QLineEdit("0", startupGroup);
    m_siXSizeEdit = new QLineEdit("0", startupGroup);
    m_siYSizeEdit = new QLineEdit("0", startupGroup);
    m_siXCountCharsEdit = new QLineEdit("0", startupGroup);
    m_siYCountCharsEdit = new QLineEdit("0", startupGroup);
    m_siFillAttributeEdit = new QLineEdit("0x00000000", startupGroup);
    m_siFlagsEdit = new QLineEdit("0x00000000", startupGroup);
    m_siShowWindowEdit = new QLineEdit("0", startupGroup);
    m_siCbReserved2Edit = new QLineEdit("0", startupGroup);
    m_siReserved2PtrEdit = new QLineEdit("0", startupGroup);
    m_siStdInputEdit = new QLineEdit("0", startupGroup);
    m_siStdOutputEdit = new QLineEdit("0", startupGroup);
    m_siStdErrorEdit = new QLineEdit("0", startupGroup);

    const QList<QLineEdit*> startupEdits{
        m_siCbEdit, m_siReservedEdit, m_siDesktopEdit, m_siTitleEdit,
        m_siXEdit, m_siYEdit, m_siXSizeEdit, m_siYSizeEdit,
        m_siXCountCharsEdit, m_siYCountCharsEdit, m_siFillAttributeEdit,
        m_siFlagsEdit, m_siShowWindowEdit, m_siCbReserved2Edit,
        m_siReserved2PtrEdit, m_siStdInputEdit, m_siStdOutputEdit, m_siStdErrorEdit
    };
    for (QLineEdit* startupEdit : startupEdits)
    {
        startupEdit->setStyleSheet(inputStyle);
    }

    int startupRow = 0;
    startupLayout->addWidget(m_useStartupInfoCheck, startupRow++, 0, 1, 6);
    const auto addStartupField = [&startupLayout, &startupRow](const QString& labelText, QWidget* editorWidget, const int colOffset)
        {
            startupLayout->addWidget(new QLabel(labelText), startupRow, colOffset);
            startupLayout->addWidget(editorWidget, startupRow, colOffset + 1);
        };

    addStartupField("cb（结构体大小）", m_siCbEdit, 0);
    addStartupField("lpReserved（保留字符串）", m_siReservedEdit, 2);
    addStartupField("lpDesktop（目标桌面）", m_siDesktopEdit, 4);
    ++startupRow;
    addStartupField("lpTitle（窗口标题）", m_siTitleEdit, 0);
    addStartupField("dwX（窗口X坐标）", m_siXEdit, 2);
    addStartupField("dwY（窗口Y坐标）", m_siYEdit, 4);
    ++startupRow;
    addStartupField("dwXSize（窗口宽）", m_siXSizeEdit, 0);
    addStartupField("dwYSize（窗口高）", m_siYSizeEdit, 2);
    addStartupField("dwXCountChars（控制台宽）", m_siXCountCharsEdit, 4);
    ++startupRow;
    addStartupField("dwYCountChars（控制台高）", m_siYCountCharsEdit, 0);
    addStartupField("dwFillAttribute（控制台属性）", m_siFillAttributeEdit, 2);
    addStartupField("dwFlags（启动标志）", m_siFlagsEdit, 4);
    ++startupRow;
    addStartupField("wShowWindow（显示方式）", m_siShowWindowEdit, 0);
    addStartupField("cbReserved2（保留2长度）", m_siCbReserved2Edit, 2);
    addStartupField("lpReserved2（保留2指针）", m_siReserved2PtrEdit, 4);
    ++startupRow;
    addStartupField("hStdInput（标准输入句柄）", m_siStdInputEdit, 0);
    addStartupField("hStdOutput（标准输出句柄）", m_siStdOutputEdit, 2);
    addStartupField("hStdError（标准错误句柄）", m_siStdErrorEdit, 4);
    ++startupRow;

    // STARTUPINFO.dwFillAttribute 位标志勾选区：
    // - 提供前景/背景颜色与样式位的可视化组合。
    QGroupBox* startupFillAttrGroup = buildBitmaskCheckGroup(
        startupGroup,
        "STARTUPINFO.dwFillAttribute 位标志组合",
        ConsoleFillAttributeDefinitions,
        &m_startupFillAttributeChecks);
    startupLayout->addWidget(startupFillAttrGroup, startupRow++, 0, 1, 6);

    // STARTUPINFO.dwFlags 位标志勾选区：
    // - 列出 STARTF_* 标志；
    // - 通过复选框直接组合，并自动回填到 dwFlags 输入框。
    QGroupBox* startupFlagsGroup = buildBitmaskCheckGroup(
        startupGroup,
        "STARTUPINFO.dwFlags 位标志组合",
        StartupInfoFlagDefinitions,
        &m_startupFlagChecks);
    startupLayout->addWidget(startupFlagsGroup, startupRow++, 0, 1, 6);

    contentLayout->addWidget(startupGroup);

    // 5) PROCESS_INFORMATION 全字段。
    QGroupBox* processInfoGroup = new QGroupBox("PROCESS_INFORMATION（输出结构体，支持自定义初值）", contentWidget);
    applyTransparentContainerStyle(processInfoGroup);
    QGridLayout* processInfoLayout = new QGridLayout(processInfoGroup);
    processInfoLayout->setHorizontalSpacing(8);
    processInfoLayout->setVerticalSpacing(6);

    m_useProcessInfoCheck = new QCheckBox("启用 lpProcessInformation（进程信息输出结构，取消则传 NULL）", processInfoGroup);
    m_useProcessInfoCheck->setChecked(false);
    m_useProcessInfoCheck->setToolTip("注意：Win32 通常要求该参数非空，传 NULL 会导致调用失败。");
    m_piProcessHandleEdit = new QLineEdit("0", processInfoGroup);
    m_piThreadHandleEdit = new QLineEdit("0", processInfoGroup);
    m_piPidEdit = new QLineEdit("0", processInfoGroup);
    m_piTidEdit = new QLineEdit("0", processInfoGroup);
    m_piProcessHandleEdit->setStyleSheet(inputStyle);
    m_piThreadHandleEdit->setStyleSheet(inputStyle);
    m_piPidEdit->setStyleSheet(inputStyle);
    m_piTidEdit->setStyleSheet(inputStyle);

    processInfoLayout->addWidget(m_useProcessInfoCheck, 0, 0, 1, 4);
    processInfoLayout->addWidget(new QLabel("hProcess（输出进程句柄）", processInfoGroup), 1, 0);
    processInfoLayout->addWidget(m_piProcessHandleEdit, 1, 1);
    processInfoLayout->addWidget(new QLabel("hThread（输出线程句柄）", processInfoGroup), 1, 2);
    processInfoLayout->addWidget(m_piThreadHandleEdit, 1, 3);
    processInfoLayout->addWidget(new QLabel("dwProcessId（输出PID）", processInfoGroup), 2, 0);
    processInfoLayout->addWidget(m_piPidEdit, 2, 1);
    processInfoLayout->addWidget(new QLabel("dwThreadId（输出TID）", processInfoGroup), 2, 2);
    processInfoLayout->addWidget(m_piTidEdit, 2, 3);
    contentLayout->addWidget(processInfoGroup);

    // 6) Token 特权编辑器。
    QGroupBox* tokenPrivilegeGroup = new QGroupBox("Token 特权调整（AdjustTokenPrivileges）", contentWidget);
    applyTransparentContainerStyle(tokenPrivilegeGroup);
    QVBoxLayout* tokenPrivilegeLayout = new QVBoxLayout(tokenPrivilegeGroup);
    m_tokenPrivilegeTable = new QTableWidget(CommonPrivilegeNames.size(), 2, tokenPrivilegeGroup);
    m_tokenPrivilegeTable->setHorizontalHeaderLabels(QStringList{ "Privilege", "Action" });
    m_tokenPrivilegeTable->horizontalHeader()->setStretchLastSection(true);
    m_tokenPrivilegeTable->verticalHeader()->setVisible(false);
    m_tokenPrivilegeTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_tokenPrivilegeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tokenPrivilegeTable->setAlternatingRowColors(true);

    for (int row = 0; row < CommonPrivilegeNames.size(); ++row)
    {
        QTableWidgetItem* nameItem = new QTableWidgetItem(CommonPrivilegeNames.at(row));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        m_tokenPrivilegeTable->setItem(row, 0, nameItem);

        QComboBox* actionCombo = new QComboBox(m_tokenPrivilegeTable);
        actionCombo->addItem("保持", static_cast<int>(ks::process::TokenPrivilegeAction::Keep));
        actionCombo->addItem("启用", static_cast<int>(ks::process::TokenPrivilegeAction::Enable));
        actionCombo->addItem("禁用", static_cast<int>(ks::process::TokenPrivilegeAction::Disable));
        actionCombo->addItem("移除", static_cast<int>(ks::process::TokenPrivilegeAction::Remove));
        actionCombo->setCurrentIndex(0);
        actionCombo->setStyleSheet(comboStyle);
        m_tokenPrivilegeTable->setCellWidget(row, 1, actionCombo);
    }

    QHBoxLayout* tokenActionLayout = new QHBoxLayout();
    m_applyTokenPrivilegeButton = new QPushButton("仅应用令牌调整（不创建）", tokenPrivilegeGroup);
    m_resetTokenPrivilegeButton = new QPushButton("重置全部特权动作为保持", tokenPrivilegeGroup);
    m_applyTokenPrivilegeButton->setStyleSheet(buttonStyle);
    m_resetTokenPrivilegeButton->setStyleSheet(buttonStyle);
    tokenActionLayout->addWidget(m_applyTokenPrivilegeButton);
    tokenActionLayout->addWidget(m_resetTokenPrivilegeButton);
    tokenActionLayout->addStretch(1);

    tokenPrivilegeLayout->addWidget(m_tokenPrivilegeTable, 1);
    tokenPrivilegeLayout->addLayout(tokenActionLayout);
    contentLayout->addWidget(tokenPrivilegeGroup);

    // 7) 操作按钮 + 输出日志。
    QGroupBox* actionGroup = new QGroupBox("执行与结果", contentWidget);
    applyTransparentContainerStyle(actionGroup);
    QVBoxLayout* actionLayout = new QVBoxLayout(actionGroup);
    QHBoxLayout* actionButtonLayout = new QHBoxLayout();
    m_launchProcessButton = new QPushButton("执行创建进程", actionGroup);
    m_resetCreateFormButton = new QPushButton("恢复默认配置", actionGroup);
    m_launchProcessButton->setStyleSheet(buttonStyle);
    m_resetCreateFormButton->setStyleSheet(buttonStyle);
    actionButtonLayout->addWidget(m_launchProcessButton);
    actionButtonLayout->addWidget(m_resetCreateFormButton);
    actionButtonLayout->addStretch(1);

    m_createResultOutput = new QTextEdit(actionGroup);
    m_createResultOutput->setReadOnly(true);
    m_createResultOutput->setMinimumHeight(140);
    m_createResultOutput->setStyleSheet(inputStyle);
    m_createResultOutput->setPlaceholderText("这里显示请求参数摘要、API 返回结果和失败错误码。");

    actionLayout->addLayout(actionButtonLayout);
    actionLayout->addWidget(m_createResultOutput, 1);
    contentLayout->addWidget(actionGroup, 1);

    // 默认值补充：命令行默认跟随 applicationName。
    m_commandLineEdit->setPlaceholderText("例如: \"C:\\Windows\\System32\\notepad.exe\" C:\\test.txt");
    m_applicationNameEdit->setPlaceholderText("可执行文件路径（可为空并传 null）");
    m_currentDirectoryEdit->setPlaceholderText("工作目录（可为空并传 null）");

    // 为关键 CreateProcess 参数补充中文解释 Tooltip，便于用户理解每个字段的语义。
    m_applicationNameEdit->setToolTip("lpApplicationName：应用程序路径。可为 null，由命令行首段决定可执行文件。");
    m_commandLineEdit->setToolTip("lpCommandLine：完整命令行。可执行路径 + 参数，传入后可能被 API 就地修改。");
    m_currentDirectoryEdit->setToolTip("lpCurrentDirectory：子进程初始工作目录。");
    m_environmentEditor->setToolTip("lpEnvironment：环境变量块。每行 KEY=VALUE；禁用时传 null。");
    m_inheritHandleCheck->setToolTip("bInheritHandles：是否继承父进程可继承句柄。");
    m_creationFlagsEdit->setToolTip("dwCreationFlags：创建标志位掩码；可在下方复选框中逐位勾选组合。");
    m_useStartupInfoCheck->setToolTip("lpStartupInfo：启动信息结构体，控制窗口/标准句柄等行为。");
    m_useProcessInfoCheck->setToolTip("lpProcessInformation：接收新进程与主线程句柄/PID/TID 的输出结构。");
    m_useProcessSecurityCheck->setToolTip("lpProcessAttributes：进程对象安全属性。");
    m_useThreadSecurityCheck->setToolTip("lpThreadAttributes：主线程对象安全属性。");
    m_siFlagsEdit->setToolTip("STARTUPINFO.dwFlags：启动标志位掩码；可在下方 STARTF 复选框中组合。");
    m_siFillAttributeEdit->setToolTip("STARTUPINFO.dwFillAttribute：控制台颜色/样式位；可在下方复选框组合。");
    m_tokenDesiredAccessEdit->setToolTip("Token DesiredAccess：令牌访问掩码；可在下方复选框组合。");

    m_sideTabWidget->addTab(m_createProcessPage, blueTintedIcon(IconStart), "创建进程");
    refreshSideTabIconContrast();
    initializeCreateProcessConnections();
}

void ProcessDock::initializeConnections()
{
    // 策略切换后立即强制刷新。
    connect(m_strategyCombo, &QComboBox::currentIndexChanged, this, [this]() {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 进程枚举策略切换为: "
            << strategyToText(toStrategy(m_strategyCombo->currentIndex()))
            << eol;
        requestAsyncRefresh(true);
    });

    // 内核对比勾选变更后立即刷新，确保列表高亮状态与开关一致。
    connect(m_kernelCompareCheck, &QCheckBox::toggled, this, [this](const bool checked) {
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 内核进程对比开关变更, enabled="
            << (checked ? "true" : "false")
            << eol;
        requestAsyncRefresh(true);
    });

    // 显示隐藏项开关只影响本地过滤；若需要重新拉 R0 标记，用户可手动刷新。
    connect(m_showKswordHiddenProcessCheck, &QCheckBox::toggled, this, [this](const bool checked) {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] Ksword隐藏项显示开关变更, visible="
            << (checked ? "true" : "false")
            << eol;
        rebuildTable();
    });

    // 树/列表切换：仅图标切换（不显示文字），并立即重建表格。
    connect(m_treeToggleButton, &QPushButton::toggled, this, [this](const bool checked) {
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
        if (checked)
        {
            m_treeToggleButton->setIcon(QIcon(IconList));
            m_treeToggleButton->setToolTip("切换为列表视图");
        }
        else
        {
            m_treeToggleButton->setIcon(QIcon(IconTree));
            m_treeToggleButton->setToolTip("切换为树状视图");
        }
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 树状显示开关变更, treeMode=" << (checked ? "true" : "false")
            << eol;
        rebuildTable();
    });

    // 视图模式切换：重置默认可见列。
    connect(m_viewModeCombo, &QComboBox::currentIndexChanged, this, [this](const int modeIndex) {
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 视图模式切换, modeIndex=" << modeIndex
            << ", detailMode=" << (modeIndex == static_cast<int>(ViewMode::Detail) ? "true" : "false")
            << eol;
        applyViewMode(static_cast<ViewMode>(modeIndex));
        rebuildTable();
        requestAsyncRefresh(true);
    });

    // 开始/暂停监视：仅切换标记和定时器，不阻塞 UI。
    connect(m_startButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        info << logEvent << "[ProcessDock] 用户点击开始刷新，恢复周期刷新并同步记录进程活动。" << eol;
        m_monitoringEnabled = true;
        m_activityRecordingEnabled = true;
        if (m_activitySamples.empty())
        {
            m_activityRecordingStartTick100ns = steadyNow100ns();
            m_activityNextSequence = 0;
        }
        m_activityTimelinePinnedToLatest = true;
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
        if (m_tableRefreshIntervalEdit != nullptr)
        {
            m_tableRefreshIntervalEdit->setEnabled(true);
        }
        if (m_refreshIntervalEdit != nullptr)
        {
            m_refreshIntervalEdit->setEnabled(true);
        }
        if (m_refreshTimer != nullptr)
        {
            if (isProcessActivityRefreshAllowedNow())
            {
                m_refreshTimer->start(refreshIntervalMillisecondsFromInput());
            }
            else
            {
                m_refreshTimer->stop();
            }
        }
        requestAsyncRefresh(true);
    });
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] 用户点击暂停刷新，周期刷新与进程活动记录一起暂停。" << eol;
        m_monitoringEnabled = false;
        m_activityRecordingEnabled = false;
        if (m_tableRefreshIntervalEdit != nullptr)
        {
            m_tableRefreshIntervalEdit->setEnabled(false);
        }
        if (m_refreshIntervalEdit != nullptr)
        {
            m_refreshIntervalEdit->setEnabled(false);
        }
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->stop();
        }
        updateRefreshStateUi(false, "● 已暂停刷新/记录");
        updateProcessActivityStatusLabel();
    });

    // 记录打点间隔输入：
    // - 支持 0.1 这类小数秒；
    // - 编辑完成后立即应用到后台监视定时器。
    connect(m_refreshIntervalEdit, &QLineEdit::editingFinished, this, [this]() {
        applyRefreshIntervalInput();
    });
    connect(m_tableRefreshIntervalEdit, &QLineEdit::editingFinished, this, [this]() {
        applyTableRefreshIntervalInput();
    });

    // 清空记录缓存：重置序号、时间轴和吸附状态。
    connect(m_activityClearButton, &QPushButton::clicked, this, [this]() {
        m_activitySamples.clear();
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
        m_activityNextSequence = 0;
        m_activityRecordingStartTick100ns = steadyNow100ns();
        m_activityTimelinePinnedToLatest = true;
        refreshProcessActivityTimeline();
        refreshProcessActivityChart();
        rebuildTable();
        updateProcessActivityStatusLabel();
        if (m_activitySnapshotLabel != nullptr)
        {
            m_activitySnapshotLabel->setText(QStringLiteral("时间轴快照：暂无样本"));
        }
    });

    // 指标按钮：切换后只重绘图表，不改变样本缓存。
    const auto connectMetricButton = [this](QPushButton* button) {
        if (button == nullptr)
        {
            return;
        }
        connect(button, &QPushButton::toggled, this, [this]() {
            refreshProcessActivityChart();
            const int sampleIndex = (m_activityTimelineSlider != nullptr) ? m_activityTimelineSlider->value() : -1;
            if (sampleIndex >= 0)
            {
                previewProcessActivitySnapshotForIndex(sampleIndex);
            }
        });
    };
    connectMetricButton(m_activityCpuButton);
    connectMetricButton(m_activityMemoryButton);
    connectMetricButton(m_activityDiskButton);
    connectMetricButton(m_activityNetworkButton);
    connectMetricButton(m_activityGpuButton);

    // 时间轴滑块：用户拖到最右侧后进入“吸附最新”模式，否则停留历史样本。
    connect(m_activityTimelineSlider, &QSlider::valueChanged, this, [this](const int sampleIndex) {
        if (m_activityTimelineSlider == nullptr)
        {
            return;
        }
        if (!m_activityTimelineSliderUpdating)
        {
            // 用户要求：只有点击时间轴才切换表格时刻。
            // 普通 valueChanged 可能来自程序同步或键盘操作，这里只更新快照提示。
            previewProcessActivitySnapshotForIndex(sampleIndex);
            return;
        }
        previewProcessActivitySnapshotForIndex(sampleIndex);
    });

    // 后台保持刷新/记录：
    // - 未勾选时，进程页隐藏后周期刷新和记录都会自动暂停；
    // - 勾选后允许后台继续刷新，除非“只刷新列表”主动禁止写记录。
    connect(m_activityBackgroundRecordCheck, &QCheckBox::toggled, this, [this]() {
        updateProcessActivityStatusLabel();
        if (m_refreshTimer != nullptr && m_monitoringEnabled)
        {
            if (isProcessActivityRefreshAllowedNow())
            {
                m_refreshTimer->start(refreshIntervalMillisecondsFromInput());
            }
            else
            {
                m_refreshTimer->stop();
            }
        }
        if (m_monitoringEnabled && isProcessActivityRefreshAllowedNow())
        {
            requestAsyncRefresh(true);
        }
    });

    // 只刷新列表：
    // - 勾选时不清空历史样本，只暂停后续 append；
    // - 取消后继续沿用同一条记录时间轴，方便对比前后变化。
    connect(m_activityListOnlyRefreshCheck, &QCheckBox::toggled, this, [this](const bool checked) {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 只刷新列表开关变更, listOnly="
            << (checked ? "true" : "false")
            << eol;
        updateProcessActivityStatusLabel();
        refreshProcessActivityChart();
    });

    // 表格右键菜单。
    connect(m_processTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showTableContextMenu(localPosition);
    });

    // 表头右键菜单（列显示/隐藏）。
    connect(m_processTable->header(), &QHeaderView::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showHeaderContextMenu(localPosition);
    });

    // 搜索框输入变更后直接重建表格：
    // - 仅过滤当前缓存，不等待下一轮刷新；
    // - 这样键入 notepad 等关键词时结果会立即收敛。
    connect(m_processSearchLineEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        rebuildTable();
    });

    // 切回“进程列表”页时自动聚焦搜索框：
    // - 用户无需先点一下输入框；
    // - 可以直接开始输入 notepad 等搜索词。
    connect(m_sideTabWidget, &QTabWidget::currentChanged, this, [this](const int currentIndex) {
        if (m_sideTabWidget == nullptr || currentIndex < 0)
        {
            return;
        }
        refreshSideTabIconContrast();

        QWidget* currentPage = m_sideTabWidget->widget(currentIndex);
        if (currentPage == m_processListPage)
        {
            if (m_monitoringEnabled && m_refreshTimer != nullptr && !m_refreshTimer->isActive())
            {
                m_refreshTimer->start(refreshIntervalMillisecondsFromInput());
            }
            focusProcessSearchBox(true);
            updateProcessActivityStatusLabel();
            if (m_monitoringEnabled)
            {
                requestAsyncRefresh(true);
            }
            return;
        }
        if (m_monitoringEnabled &&
            m_activityBackgroundRecordCheck != nullptr &&
            !m_activityBackgroundRecordCheck->isChecked() &&
            m_refreshTimer != nullptr)
        {
            m_refreshTimer->stop();
            updateRefreshStateUi(false, "● 进程页隐藏，刷新/记录自动暂停");
        }
        if (currentPage == m_threadPage)
        {
            updateProcessActivityStatusLabel();
            requestAsyncThreadRefresh(true);
            return;
        }
        updateProcessActivityStatusLabel();
    });

    // currentItemChanged 作用：
    // - 记录当前被用户选中的进程 identityKey；
    // - 周期刷新后 rebuildTable 会按这个 key 恢复高亮。
    connect(m_processTable, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* currentItem, QTreeWidgetItem*) {
        if (currentItem == nullptr)
        {
            if (!m_contextMenuVisible)
            {
                m_trackedSelectedIdentityKey.clear();
                m_trackedSelectedIdentityKeys.clear();
                m_trackedSelectedColumn = 0;
            }
            return;
        }

        const int currentColumn = m_processTable->currentColumn();
        if (currentColumn >= 0 && currentColumn < static_cast<int>(TableColumn::Count))
        {
            m_trackedSelectedColumn = currentColumn;
        }
        syncTrackedSelectionFromTable();
    });

    // itemSelectionChanged 作用：
    // - 记录 Ctrl 复选后的完整行集合；
    // - 周期刷新 rebuildTable 后按 identityKey 恢复多选状态。
    connect(m_processTable, &QTreeWidget::itemSelectionChanged, this, [this]() {
        syncTrackedSelectionFromTable();
        refreshProcessActivityChart();
        if (m_activityTimelineSlider != nullptr && !m_activitySamples.empty())
        {
            previewProcessActivitySnapshotForIndex(m_activityTimelineSlider->value());
        }
    });

    // itemPressed 作用：
    // - 左键点选某一行时同步记录点击列；
    // - 刷新恢复时尽量回到用户原来的焦点列。
    connect(m_processTable, &QTreeWidget::itemPressed, this, [this](QTreeWidgetItem* item, const int column) {
        if (item == nullptr)
        {
            return;
        }

        m_trackedSelectedIdentityKey = item->data(0, Qt::UserRole).toString().toStdString();
        if (column >= 0 && column < static_cast<int>(TableColumn::Count))
        {
            m_trackedSelectedColumn = column;
        }
        QTimer::singleShot(0, this, [this]()
        {
            syncTrackedSelectionFromTable();
        });
    });

    // Alt+E 作用：
    // - 提供与任务管理器类似的快速结束进程快捷键；
    // - 仅在“进程列表”页且存在选中行时执行“结束进程组合动作”。
    QShortcut* terminateShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_E), this);
    terminateShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(terminateShortcut, &QShortcut::activated, this, [this]() {
        if (m_sideTabWidget == nullptr || m_sideTabWidget->currentWidget() != m_processListPage)
        {
            return;
        }

        if (selectedActionTargets().empty())
        {
            kLogEvent logEvent;
            warn << logEvent
                << "[ProcessDock] Alt+E 被忽略：当前没有选中可结束的进程。"
                << eol;
            return;
        }

        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 触发快捷键 Alt+E，执行结束进程组合动作。"
            << eol;
        executeTerminateProcessAction();
    });

    // 线程页专属连接：集中在独立函数中，避免主连接函数继续膨胀。
    initializeThreadPageConnections();
}

void ProcessDock::initializeCreateProcessConnections()
{
    if (m_applicationBrowseButton != nullptr)
    {
        connect(m_applicationBrowseButton, &QPushButton::clicked, this, [this]() {
            browseCreateProcessApplicationPath();
            });
    }
    if (m_currentDirectoryBrowseButton != nullptr)
    {
        connect(m_currentDirectoryBrowseButton, &QPushButton::clicked, this, [this]() {
            browseCreateProcessCurrentDirectory();
            });
    }
    if (m_launchProcessButton != nullptr)
    {
        connect(m_launchProcessButton, &QPushButton::clicked, this, [this]() {
            executeCreateProcessRequest();
            });
    }
    if (m_resetCreateFormButton != nullptr)
    {
        connect(m_resetCreateFormButton, &QPushButton::clicked, this, [this]() {
            resetCreateProcessForm();
            });
    }
    if (m_applyTokenPrivilegeButton != nullptr)
    {
        connect(m_applyTokenPrivilegeButton, &QPushButton::clicked, this, [this]() {
            executeApplyTokenPrivilegeEditsOnly();
            });
    }
    if (m_resetTokenPrivilegeButton != nullptr && m_tokenPrivilegeTable != nullptr)
    {
        connect(m_resetTokenPrivilegeButton, &QPushButton::clicked, this, [this]() {
            for (int row = 0; row < m_tokenPrivilegeTable->rowCount(); ++row)
            {
                QComboBox* actionCombo = qobject_cast<QComboBox*>(m_tokenPrivilegeTable->cellWidget(row, 1));
                if (actionCombo != nullptr)
                {
                    actionCombo->setCurrentIndex(0);
                }
            }
            appendCreateResultLine("已重置全部特权动作到“保持”。");
            });
    }

    // 选择应用程序路径后，若 lpCommandLine 为空则自动填充便于快速执行。
    if (m_applicationNameEdit != nullptr && m_commandLineEdit != nullptr)
    {
        connect(m_applicationNameEdit, &QLineEdit::textChanged, this, [this](const QString& textValue) {
            if (textValue.trimmed().isEmpty())
            {
                return;
            }
            if (m_commandLineEdit->text().trimmed().isEmpty())
            {
                m_commandLineEdit->setText(QStringLiteral("\"%1\"").arg(textValue.trimmed()));
            }
            });
    }

    if (m_createMethodCombo != nullptr && m_tokenSourcePidEdit != nullptr)
    {
        connect(m_createMethodCombo, &QComboBox::currentIndexChanged, this, [this](const int indexValue) {
            const bool tokenMode = (indexValue == 1);
            m_tokenSourcePidEdit->setEnabled(tokenMode);
            m_tokenDesiredAccessEdit->setEnabled(tokenMode);
            m_tokenDuplicatePrimaryCheck->setEnabled(tokenMode);
            m_tokenPrivilegeTable->setEnabled(tokenMode);
            m_applyTokenPrivilegeButton->setEnabled(tokenMode);
            m_resetTokenPrivilegeButton->setEnabled(tokenMode);
            appendCreateResultLine(tokenMode
                ? "已切换到 Token 创建模式。"
                : "已切换到普通 CreateProcessW 模式。");
            });
        m_createMethodCombo->setCurrentIndex(0);
    }

    // 位标志编辑联动：
    // - 勾选复选框自动组合掩码写回输入框；
    // - 手工修改输入框会反向刷新复选框状态。
    bindBitmaskEditor(m_creationFlagsEdit, &m_creationFlagChecks, "dwCreationFlags");
    bindBitmaskEditor(m_siFlagsEdit, &m_startupFlagChecks, "STARTUPINFO.dwFlags");
    bindBitmaskEditor(m_siFillAttributeEdit, &m_startupFillAttributeChecks, "STARTUPINFO.dwFillAttribute");
    bindBitmaskEditor(m_tokenDesiredAccessEdit, &m_tokenDesiredAccessChecks, "Token DesiredAccess");

    const bool tokenMode = (m_createMethodCombo != nullptr && m_createMethodCombo->currentIndex() == 1);
    if (m_tokenSourcePidEdit != nullptr) m_tokenSourcePidEdit->setEnabled(tokenMode);
    if (m_tokenDesiredAccessEdit != nullptr) m_tokenDesiredAccessEdit->setEnabled(tokenMode);
    if (m_tokenDuplicatePrimaryCheck != nullptr) m_tokenDuplicatePrimaryCheck->setEnabled(tokenMode);
    if (m_tokenPrivilegeTable != nullptr) m_tokenPrivilegeTable->setEnabled(tokenMode);
    if (m_applyTokenPrivilegeButton != nullptr) m_applyTokenPrivilegeButton->setEnabled(tokenMode);
    if (m_resetTokenPrivilegeButton != nullptr) m_resetTokenPrivilegeButton->setEnabled(tokenMode);
}

void ProcessDock::initializeTimer()
{
    // 周期监视定时器：
    // - 默认 0.1 秒执行后台进程刷新和活动打点；
    // - 下方进程表格是否重绘由独立“列表刷新(s)”输入框节流。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(refreshIntervalMillisecondsFromInput());
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        requestAsyncRefresh(false);
    });
    // 首次显示前不启动，避免主窗口启动阶段后台偷跑刷新。
}

int ProcessDock::refreshIntervalMillisecondsFromInput() const
{
    // 输入为空或非法时回退默认 0.1s；
    // clamp 避免极小间隔造成后台枚举连续堆积。
    bool parseOk = false;
    double secondsValue = (m_refreshIntervalEdit != nullptr)
        ? m_refreshIntervalEdit->text().trimmed().toDouble(&parseOk)
        : 0.1;
    if (parseOk && !std::isfinite(secondsValue))
    {
        parseOk = false;
        secondsValue = 0.1;
    }
    const double safeSeconds = parseOk ? secondsValue : 0.1;
    const double clampedSeconds = std::clamp(
        safeSeconds,
        static_cast<double>(ActivityMinimumIntervalMilliseconds) / 1000.0,
        static_cast<double>(ActivityMaximumIntervalMilliseconds) / 1000.0);
    const int milliseconds = static_cast<int>(std::llround(clampedSeconds * 1000.0));
    return std::clamp(
        milliseconds,
        ActivityMinimumIntervalMilliseconds,
        ActivityMaximumIntervalMilliseconds);
}

void ProcessDock::applyRefreshIntervalInput()
{
    const int intervalMs = refreshIntervalMillisecondsFromInput();
    const double normalizedSeconds = static_cast<double>(intervalMs) / 1000.0;

    // 规范化显示，避免用户输入 0 或非法文本后仍看到误导值。
    if (m_refreshIntervalEdit != nullptr)
    {
        QSignalBlocker blocker(m_refreshIntervalEdit);
        m_refreshIntervalEdit->setText(QString::number(normalizedSeconds, 'f', normalizedSeconds < 1.0 ? 2 : 1));
    }

    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->setInterval(intervalMs);
        if (m_monitoringEnabled && isProcessActivityRefreshAllowedNow())
        {
            m_refreshTimer->start(intervalMs);
        }
    }
    updateProcessActivityStatusLabel();

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 记录打点/后台监视间隔变更为 "
        << intervalMs
        << " ms。"
        << eol;
}

int ProcessDock::tableRefreshIntervalMillisecondsFromInput() const
{
    // 表格刷新默认 2 秒；该值只影响 UI 重绘，不影响 0.1 秒后台采样。
    bool parseOk = false;
    double secondsValue = (m_tableRefreshIntervalEdit != nullptr)
        ? m_tableRefreshIntervalEdit->text().trimmed().toDouble(&parseOk)
        : 2.0;
    if (parseOk && !std::isfinite(secondsValue))
    {
        parseOk = false;
        secondsValue = 2.0;
    }
    const double safeSeconds = parseOk ? secondsValue : 2.0;
    const double clampedSeconds = std::clamp(
        safeSeconds,
        static_cast<double>(ProcessTableMinimumIntervalMilliseconds) / 1000.0,
        static_cast<double>(ProcessTableMaximumIntervalMilliseconds) / 1000.0);
    const int milliseconds = static_cast<int>(std::llround(clampedSeconds * 1000.0));
    return std::clamp(
        milliseconds,
        ProcessTableMinimumIntervalMilliseconds,
        ProcessTableMaximumIntervalMilliseconds);
}

void ProcessDock::applyTableRefreshIntervalInput()
{
    const int intervalMs = tableRefreshIntervalMillisecondsFromInput();
    const double normalizedSeconds = static_cast<double>(intervalMs) / 1000.0;

    // 规范化显示，避免输入非法值后 UI 留下误导文本。
    if (m_tableRefreshIntervalEdit != nullptr)
    {
        QSignalBlocker blocker(m_tableRefreshIntervalEdit);
        m_tableRefreshIntervalEdit->setText(QString::number(normalizedSeconds, 'f', normalizedSeconds < 1.0 ? 2 : 1));
    }

    updateProcessActivityStatusLabel();

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 进程表格刷新间隔变更为 "
        << intervalMs
        << " ms。"
        << eol;
}

void ProcessDock::focusProcessSearchBox(const bool selectAllText)
{
    if (m_processSearchLineEdit == nullptr)
    {
        return;
    }

    // 使用 singleShot(0) 把聚焦动作延后到当前事件循环尾部：
    // - 避免与页签切换、显示事件内部的焦点竞争；
    // - 保证用户切页后第一时间就能直接输入搜索词。
    QPointer<QLineEdit> guardSearchLineEdit(m_processSearchLineEdit);
    QTimer::singleShot(0, this, [guardSearchLineEdit, selectAllText]() {
        if (guardSearchLineEdit == nullptr)
        {
            return;
        }

        guardSearchLineEdit->setFocus(Qt::ShortcutFocusReason);
        if (selectAllText)
        {
            guardSearchLineEdit->selectAll();
        }
    });
}

QString ProcessDock::currentProcessSearchText() const
{
    if (m_processSearchLineEdit == nullptr)
    {
        return QString();
    }

    return m_processSearchLineEdit->text().trimmed();
}

bool ProcessDock::processRecordMatchesSearch(const ks::process::ProcessRecord& processRecord) const
{
    const QString searchText = currentProcessSearchText();
    if (searchText.isEmpty())
    {
        return true;
    }

    // 显式 PID 过滤语法：
    // - 准星拾取会写入 pid:<数字>，保证只收敛到目标窗口所属进程；
    // - 普通纯数字输入仍保留原来的模糊搜索行为，不破坏用户既有习惯。
    const QString lowerSearchText = searchText.toLower();
    if (lowerSearchText.startsWith(QStringLiteral("pid:")) ||
        lowerSearchText.startsWith(QStringLiteral("pid=")))
    {
        bool pidParseOk = false;
        const std::uint32_t filterPid = searchText.mid(4).trimmed().toUInt(&pidParseOk);
        if (pidParseOk)
        {
            return processRecord.pid == filterPid;
        }
    }

    // 搜索字段覆盖：
    // - 进程名 / PID / 路径 / 命令行 / 用户 / 签名 / 启动时间 / 父 PID；
    // - 统一使用 contains(忽略大小写) 做模糊匹配，方便快速定位。
    const QStringList searchableFields{
        QString::fromStdString(processRecord.processName),
        QString::number(processRecord.pid),
        QString::fromStdString(processRecord.imagePath),
        QString::fromStdString(processRecord.r0ImagePath),
        QString::fromStdString(processRecord.commandLine),
        QString::fromStdString(processRecord.userName),
        QString::fromStdString(processRecord.signatureState),
        processR0StatusText(processRecord.r0Status),
        processFieldSourceText(processRecord.r0ProtectionSource),
        QString::fromStdString(processRecord.startTimeText),
        QString::number(processRecord.parentPid)
    };
    for (const QString& fieldText : searchableFields)
    {
        if (fieldText.contains(searchText, Qt::CaseInsensitive))
        {
            return true;
        }
    }

    return false;
}

void ProcessDock::updateRefreshStateUi(const bool refreshing, const QString& stateText)
{
    // 统一刷新状态标签更新逻辑，确保“刷新中/空闲”展示一致。
    if (m_refreshStateLabel == nullptr)
    {
        return;
    }

    if (refreshing)
    {
        m_refreshStateLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }
    else
    {
        const QString idleColor = KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#6ECF7A")
            : QStringLiteral("#2F7D32");
        m_refreshStateLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:600;").arg(idleColor));
    }
    m_refreshStateLabel->setText(stateText);
}

void ProcessDock::applyDefaultColumnWidths()
{
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Name), 280);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Pid), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Cpu), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Ram), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Disk), 95);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Gpu), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Net), 95);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Signature), 260);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Path), 280);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::ParentPid), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::CommandLine), 320);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::User), 180);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::StartTime), 160);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::IsAdmin), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::PplLevel), 220);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Protection), 130);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Ppl), 120);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::HandleCount), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::HandleTable), 180);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::SectionObject), 180);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::R0Status), 130);
}

void ProcessDock::applyViewMode(const ViewMode viewMode)
{
    const bool hideR0OnlyColumns = m_autoHideUnavailableR0Columns;

    // 先全部隐藏，再按视图打开目标列，保证状态可预测。
    for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
    {
        m_processTable->setColumnHidden(column, true);
    }

    // 监视视图：进程名 + PID + 性能计数器。
    if (viewMode == ViewMode::Monitor)
    {
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Name), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Pid), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Cpu), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Ram), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Disk), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Gpu), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Net), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::HandleCount), false);
        if (!hideR0OnlyColumns)
        {
            m_processTable->setColumnHidden(toColumnIndex(TableColumn::Protection), false);
            m_processTable->setColumnHidden(toColumnIndex(TableColumn::Ppl), false);
            m_processTable->setColumnHidden(toColumnIndex(TableColumn::HandleTable), false);
            m_processTable->setColumnHidden(toColumnIndex(TableColumn::SectionObject), false);
            m_processTable->setColumnHidden(toColumnIndex(TableColumn::R0Status), false);
        }
        applyAdaptiveColumnWidths();
        return;
    }

    // 详细信息视图：按用户要求“不要性能计数器列”。
    // 仅展示静态/管理相关信息列。
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Name), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Pid), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Signature), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Path), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::ParentPid), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::CommandLine), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::User), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::StartTime), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::IsAdmin), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::PplLevel), false);
    if (!hideR0OnlyColumns)
    {
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Protection), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Ppl), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::HandleTable), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::SectionObject), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::R0Status), false);
    }
    applyAdaptiveColumnWidths();
}

void ProcessDock::applyAdaptiveColumnWidths()
{
    // 该函数作用：按当前可见列数量，均分可用宽度，彻底避免横向滚动条出现。
    if (m_processTable == nullptr)
    {
        return;
    }

    QHeaderView* headerView = m_processTable->header();
    if (headerView == nullptr)
    {
        return;
    }

    // 先统计可见列，隐藏列保留 ResizeToContents，防止切换视图时状态错乱。
    int visibleColumnCount = 0;
    for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
    {
        if (!m_processTable->isColumnHidden(column))
        {
            ++visibleColumnCount;
            headerView->setSectionResizeMode(column, QHeaderView::Stretch);
        }
        else
        {
            headerView->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        }
    }

    if (visibleColumnCount <= 0)
    {
        return;
    }

    // 兜底：宽度分配有时会因延迟布局未立刻生效，主动触发一次 viewport 更新。
    m_processTable->viewport()->update();
}

void ProcessDock::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    // Dock 尺寸变化后立即重新分配列宽，保证任何宽度下都不出现内部横向滚动条。
    applyAdaptiveColumnWidths();
}

void ProcessDock::requestAsyncRefresh(const bool forceRefresh)
{
    // 需求：每次刷新前都检测 Ctrl，按下则跳过本轮（无论是否强制刷新）。
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDock] 检测到 Ctrl 按下，本轮刷新跳过。" << eol;
        updateRefreshStateUi(false, "● Ctrl 按下，跳过本轮刷新");
        return;
    }

    // 非强制刷新时，暂停监视或正在刷新则直接跳过。
    if (!forceRefresh)
    {
        // 右键菜单弹出期间冻结周期刷新，防止菜单绑定项被重建后失效。
        if (m_contextMenuVisible)
        {
            kLogEvent logEvent;
            dbg << logEvent << "[ProcessDock] 右键菜单处于打开状态，本轮刷新跳过。" << eol;
            updateRefreshStateUi(false, "● 菜单打开中，跳过本轮刷新");
            return;
        }

        if (!m_monitoringEnabled || m_refreshInProgress)
        {
            kLogEvent logEvent;
            dbg << logEvent
                << "[ProcessDock] 跳过非强制刷新, monitoringEnabled=" << (m_monitoringEnabled ? "true" : "false")
                << ", refreshInProgress=" << (m_refreshInProgress ? "true" : "false")
                << eol;
            return;
        }

        // 后台保持刷新/记录约束“是否继续刷新”：
        // - 默认离开进程列表页就不再继续刷新；
        // - 勾选后允许后台继续枚举；
        // - “只刷新列表”只影响是否写记录，不应阻断列表刷新。
        if (!isProcessActivityRefreshAllowedNow())
        {
            kLogEvent logEvent;
            dbg << logEvent << "[ProcessDock] 跳过非强制刷新：进程页不可见且未启用后台保持刷新/记录。" << eol;
            updateRefreshStateUi(false, "● 进程页隐藏，刷新/记录自动暂停");
            updateProcessActivityStatusLabel();
            return;
        }
    }

    // 强制刷新也要避免并发任务叠加。
    if (m_refreshInProgress)
    {
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDock] 跳过刷新：当前已有后台刷新任务在执行。" << eol;
        return;
    }
    m_refreshInProgress = true;

    // 创建并复用“进程刷新”进度任务，避免每轮刷新都新增新卡片。
    if (m_refreshProgressTaskPid <= 0)
    {
        m_refreshProgressTaskPid = kPro.add("进程列表刷新", "初始化刷新任务");
    }
    kPro.set(m_refreshProgressTaskPid, forceRefresh ? "准备刷新列表参数..." : "准备监视采样参数...", 0, 0.02f);

    // 复制当前缓存快照给后台线程，避免跨线程读写冲突。
    const int strategyIndex = m_strategyCombo->currentIndex();
    const bool detailModeEnabled = (currentViewMode() == ViewMode::Detail);
    const bool queryKernelProcessList =
        (m_kernelCompareCheck != nullptr && m_kernelCompareCheck->isChecked());
    const bool isFirstRefresh = m_cacheByIdentity.empty();
    const int staticDetailFillBudget =
        detailModeEnabled
        ? (isFirstRefresh ? 96 : 48)   // 详细视图也做预算控制，避免首轮全量静态查询导致 UI 抖动。
        : (isFirstRefresh ? 8 : 4);    // 监视视图优先速度，预算更小。
    const std::uint32_t cpuCount = m_logicalCpuCount;
    const auto previousCache = m_cacheByIdentity;
    const auto previousCounters = m_counterSampleByIdentity;

    // ticket 用于丢弃过期结果（防止乱序覆盖）。
    const std::uint64_t localTicket = ++m_refreshTicket;
    m_lastRefreshStartTime = std::chrono::steady_clock::now();
    QPointer<ProcessDock> guard(this);

    // 刷新前先更新 UI 状态与日志，给出明显“刷新中”提示。
    const QString forceReasonText = (!forceRefresh && m_monitoringEnabled && !isProcessActivityRefreshAllowedNow())
        ? QStringLiteral(" | 刷新/记录=自动暂停")
        : QString();
    updateRefreshStateUi(
        true,
        QString("● 正在刷新... 策略=%1%2")
        .arg(QString::fromUtf8(strategyToText(toStrategy(strategyIndex))))
        .arg(queryKernelProcessList ? " | 内核对比=ON" : "")
        + forceReasonText);

    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 进程监视刷新开始, ticket=" << localTicket
            << ", force=" << (forceRefresh ? "true" : "false")
            << ", strategy=" << strategyToText(toStrategy(strategyIndex))
            << ", detailMode=" << (detailModeEnabled ? "true" : "false")
            << ", kernelCompare=" << (queryKernelProcessList ? "true" : "false")
            << ", staticBudget=" << staticDetailFillBudget
            << ", cacheSize=" << previousCache.size()
            << ", uiTableIntervalMs=" << tableRefreshIntervalMillisecondsFromInput()
            << ", sampleIntervalMs=" << refreshIntervalMillisecondsFromInput()
            << eol;
    }

    // QRunnable + 线程池：满足“异步刷新，不阻塞 GUI”。
    // 注意：forceUiRefresh 必须在进入后台 lambda 前保存成局部值；
    // 否则内层 QueuedConnection lambda 在工作线程里无法再引用 requestAsyncRefresh 的参数。
    const bool forceUiRefresh = forceRefresh;
    const int progressPid = m_refreshProgressTaskPid;
    QRunnable* backgroundTask = QRunnable::create([
        guard,
        localTicket,
        strategyIndex,
        detailModeEnabled,
        queryKernelProcessList,
        staticDetailFillBudget,
        cpuCount,
        progressPid,
        forceUiRefresh,
        previousCache,
        previousCounters]() mutable {
        const ProcessDock::RefreshResult refreshResult = ProcessDock::buildRefreshResult(
            strategyIndex,
            detailModeEnabled,
            queryKernelProcessList,
            staticDetailFillBudget,
            localTicket,
            progressPid,
            previousCache,
            previousCounters,
            cpuCount);

        if (guard == nullptr)
        {
            return;
        }

        // 结果通过队列连接回主线程更新 UI。
        QMetaObject::invokeMethod(guard, [guard, localTicket, refreshResult, forceUiRefresh]() {
            if (guard == nullptr)
            {
                return;
            }

            // 只接受最新 ticket 的结果，旧结果直接丢弃。
            if (localTicket < guard->m_refreshTicket)
            {
                guard->m_refreshInProgress = false;
                return;
            }

            guard->applyRefreshResult(refreshResult, forceUiRefresh);
            guard->m_refreshInProgress = false;
        }, Qt::QueuedConnection);
    });

    backgroundTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(backgroundTask);
}

void ProcessDock::applyRefreshResult(const RefreshResult& refreshResult, const bool forceUiRefresh)
{
    // 计算主线程观测耗时，用于“刷新状态标签”和日志输出。
    const auto nowTime = std::chrono::steady_clock::now();
    const auto elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - m_lastRefreshStartTime).count());

    // 把最新进程数据同步到已打开的详情窗口（若对应进程仍存在）。
    // 性能策略：
    // 1) 仅同步“可见且未最小化”的详情窗口；
    // 2) 轻微变化由节流器吸收，避免每轮刷新都触发重型解析链路。
    const std::chrono::milliseconds detailWindowSyncInterval(1500);
    for (auto windowIt = m_detailWindowByIdentity.begin(); windowIt != m_detailWindowByIdentity.end();)
    {
        if (windowIt->second == nullptr)
        {
            m_detailWindowLastSyncTimeByIdentity.erase(windowIt->first);
            windowIt = m_detailWindowByIdentity.erase(windowIt);
            continue;
        }

        const QPointer<ProcessDetailWindow>& detailWindow = windowIt->second;
        if (!detailWindow->isVisible() || detailWindow->isMinimized())
        {
            ++windowIt;
            continue;
        }

        const auto nextCacheIt = refreshResult.nextCache.find(windowIt->first);
        if (nextCacheIt == refreshResult.nextCache.end())
        {
            ++windowIt;
            continue;
        }

        const auto previousCacheIt = m_cacheByIdentity.find(windowIt->first);
        const bool hasSignificantChange =
            (previousCacheIt == m_cacheByIdentity.end()) ||
            hasDetailWindowSignificantChange(previousCacheIt->second.record, nextCacheIt->second.record);

        const auto lastSyncIt = m_detailWindowLastSyncTimeByIdentity.find(windowIt->first);
        const bool reachPeriodicSyncTime =
            (lastSyncIt == m_detailWindowLastSyncTimeByIdentity.end()) ||
            (std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - lastSyncIt->second) >= detailWindowSyncInterval);

        if (hasSignificantChange || reachPeriodicSyncTime)
        {
            detailWindow->updateBaseRecord(nextCacheIt->second.record);
            m_detailWindowLastSyncTimeByIdentity[windowIt->first] = nowTime;
        }
        ++windowIt;
    }

    // 用新结果替换缓存；表格重绘由独立“列表刷新(s)”节流，避免 0.1s 打点拖垮 UI。
    m_cacheByIdentity = refreshResult.nextCache;
    m_counterSampleByIdentity = refreshResult.nextCounters;

    appendProcessActivitySample();
    if (isProcessActivityTableSnapshotActive())
    {
        rebuildProcessActivityTableSnapshotRecords();
    }
    if (shouldRebuildProcessTableForRefresh(forceUiRefresh))
    {
        rebuildTable();
        m_lastProcessTableRebuildTime = nowTime;
    }
    if (m_sideTabWidget != nullptr && m_sideTabWidget->currentWidget() == m_threadPage)
    {
        requestAsyncThreadRefresh(false);
    }

    // 更新进度任务：本轮刷新完成后自动隐藏卡片。
    if (m_refreshProgressTaskPid > 0)
    {
        kPro.set(m_refreshProgressTaskPid, "刷新完成", 100, 1.0f);
    }

    // 刷新状态标签展示详细统计，明确告诉用户“刷新已完成”。
    QString refreshStatusText = QString("● 刷新完成 %1 ms | 方法:%2 | 枚举:%3 新增:%4 退出:%5")
        .arg(elapsedMs)
        .arg(QString::fromUtf8(strategyToText(refreshResult.actualStrategy)))
        .arg(refreshResult.enumeratedCount)
        .arg(refreshResult.newProcessCount)
        .arg(refreshResult.exitedProcessCount);
    if (refreshResult.kernelCompareEnabled)
    {
        if (refreshResult.kernelQuerySucceeded)
        {
            refreshStatusText += QString(" | 内核:%1 隐藏:%2")
                .arg(refreshResult.kernelEnumeratedCount)
                .arg(refreshResult.kernelOnlyCount);
        }
        else
        {
            refreshStatusText += " | 内核查询失败";
        }
    }
    updateRefreshStateUi(false, refreshStatusText);

    // 输出详细刷新日志，便于后续性能与正确性排查。
    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 刷新完成, elapsedMs(main)=" << elapsedMs
            << ", elapsedMs(worker)=" << refreshResult.workerElapsedMs
            << ", strategySelected=" << strategyToText(refreshResult.selectedStrategy)
            << ", strategyActual=" << strategyToText(refreshResult.actualStrategy)
            << ", enumerated=" << refreshResult.enumeratedCount
            << ", reused=" << refreshResult.reusedProcessCount
            << ", new=" << refreshResult.newProcessCount
            << ", exitedHold=" << refreshResult.exitedProcessCount
            << ", staticFilled=" << refreshResult.staticFilledCount
            << ", staticDeferred=" << refreshResult.staticDeferredCount
            << ", imagePathFilled=" << refreshResult.imagePathFilledCount
            << ", kernelCompareEnabled=" << (refreshResult.kernelCompareEnabled ? "true" : "false")
            << ", kernelQuerySucceeded=" << (refreshResult.kernelQuerySucceeded ? "true" : "false")
            << ", kernelEnumerated=" << refreshResult.kernelEnumeratedCount
            << ", kernelOnly=" << refreshResult.kernelOnlyCount
            << ", kernelDetail=" << refreshResult.kernelQueryDetailText
            << ", cacheNow=" << m_cacheByIdentity.size()
            << ", uiRebuildForced=" << (forceUiRefresh ? "true" : "false")
            << eol;
    }
}

bool ProcessDock::shouldRebuildProcessTableForRefresh(const bool forceUiRefresh) const
{
    // 强制刷新、历史快照表格、首轮显示必须立刻重绘。
    if (forceUiRefresh || isProcessActivityTableSnapshotActive() || m_lastProcessTableRebuildTime.time_since_epoch().count() == 0)
    {
        return true;
    }

    // 非强制后台监视按独立 UI 间隔节流，避免每 0.1s 重建整张进程表。
    const int intervalMs = tableRefreshIntervalMillisecondsFromInput();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_lastProcessTableRebuildTime).count();
    return elapsedMs >= intervalMs;
}

ProcessDock::RefreshResult ProcessDock::buildRefreshResult(
    const int strategyIndex,
    const bool detailModeEnabled,
    const bool queryKernelProcessList,
    const int staticDetailFillBudget,
    const std::uint64_t refreshTicket,
    const int progressTaskPid,
    const std::unordered_map<std::string, CacheEntry>& previousCache,
    const std::unordered_map<std::string, ks::process::CounterSample>& previousCounters,
    const std::uint32_t logicalCpuCount)
{
    const auto workerStartTime = std::chrono::steady_clock::now();

    RefreshResult refreshResult;
    refreshResult.nextCache.clear();
    refreshResult.nextCounters.clear();
    refreshResult.selectedStrategyIndex = strategyIndex;
    refreshResult.selectedStrategy = toStrategy(strategyIndex);
    refreshResult.actualStrategy = refreshResult.selectedStrategy;
    refreshResult.detailModeEnabled = detailModeEnabled;
    refreshResult.kernelCompareEnabled = queryKernelProcessList;

    // 进度条阶段 1：开始枚举。
    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在枚举进程列表...", 10, 0.10f);
    }

    const ks::process::ProcessEnumStrategy strategy = toStrategy(strategyIndex);
    std::vector<ks::process::ProcessRecord> latestProcessList = ks::process::EnumerateProcesses(
        strategy,
        &refreshResult.actualStrategy);
    const std::uint64_t sampleTick = steadyNow100ns();
    std::unordered_set<std::uint32_t> kernelOnlyPidSet;
    std::unordered_map<std::uint32_t, KernelProcessSnapshotEntry> kernelProcessByPid;

    // 可选阶段：向 R0 请求内核进程列表，并追加“仅内核可见”的记录。
    if (queryKernelProcessList)
    {
        if (progressTaskPid > 0)
        {
            kPro.set(progressTaskPid, "正在请求内核进程列表...", 18, 0.18f);
        }

        std::vector<KernelProcessSnapshotEntry> kernelProcessList;
        std::string kernelQueryDetailText;
        const bool queryKernelOk = enumerateProcessesByR0Driver(&kernelProcessList, &kernelQueryDetailText);
        refreshResult.kernelQuerySucceeded = queryKernelOk;
        refreshResult.kernelQueryDetailText = kernelQueryDetailText;
        refreshResult.kernelEnumeratedCount = kernelProcessList.size();

        if (queryKernelOk)
        {
            std::unordered_set<std::uint32_t> userPidSet;
            userPidSet.reserve(latestProcessList.size() * 2 + 1);
            kernelProcessByPid.reserve(kernelProcessList.size() * 2 + 1);
            for (const KernelProcessSnapshotEntry& kernelProcess : kernelProcessList)
            {
                kernelProcessByPid[kernelProcess.processId] = kernelProcess;
            }
            for (const ks::process::ProcessRecord& processRecord : latestProcessList)
            {
                userPidSet.insert(processRecord.pid);
            }

            for (const KernelProcessSnapshotEntry& kernelProcess : kernelProcessList)
            {
                if (userPidSet.find(kernelProcess.processId) != userPidSet.end())
                {
                    continue;
                }
                userPidSet.insert(kernelProcess.processId);

                ks::process::ProcessRecord kernelOnlyRecord{};
                kernelOnlyRecord.pid = kernelProcess.processId;
                kernelOnlyRecord.parentPid = kernelProcess.parentProcessId;
                mergeKernelProcessExtension(kernelOnlyRecord, kernelProcess);
                kernelOnlyRecord.creationTime100ns =
                    KernelOnlyCreationTimeSeed + static_cast<std::uint64_t>(kernelProcess.processId);
                kernelOnlyRecord.processName = kernelProcess.imageName.empty()
                    ? std::string("[R0] Unknown")
                    : std::string("[R0] ") + kernelProcess.imageName;
                kernelOnlyRecord.imagePath = "[仅内核枚举可见]";
                kernelOnlyRecord.commandLine = "[仅内核枚举可见]";
                kernelOnlyRecord.userName = "-";
                kernelOnlyRecord.signatureState = "KernelOnly(Hidden?)";
                kernelOnlyRecord.signaturePublisher.clear();
                kernelOnlyRecord.signatureTrusted = false;
                kernelOnlyRecord.startTimeText = "-";
                kernelOnlyRecord.architectureText = "Unknown";
                kernelOnlyRecord.priorityText = "-";
                kernelOnlyRecord.isAdmin = false;
                kernelOnlyRecord.dynamicCountersReady = true;
                kernelOnlyRecord.staticDetailsReady = true;

                latestProcessList.push_back(std::move(kernelOnlyRecord));
                kernelOnlyPidSet.insert(kernelProcess.processId);
                ++refreshResult.kernelOnlyCount;
            }
        }
        else if (refreshResult.kernelQueryDetailText.empty())
        {
            refreshResult.kernelQueryDetailText = "query kernel process list failed";
        }
    }

    refreshResult.enumeratedCount = latestProcessList.size();

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在复用缓存并计算性能计数...", 25, 0.25f);
    }

    // 静态详情预算控制：
    // - 预算用于限制“路径/命令行/用户/签名”等慢操作，避免首轮刷新过慢；
    // - 监视模式预算较低，详细模式预算较高。
    const std::size_t staticFillBudget = static_cast<std::size_t>(std::max(0, staticDetailFillBudget));

    // 第一阶段：预处理 identity、复用旧字段，并筛选“需要补静态详情”的 PID 列表。
    std::vector<std::string> identityKeys(latestProcessList.size());
    std::vector<bool> isNewProcess(latestProcessList.size(), false);
    std::vector<bool> shouldFillStatic(latestProcessList.size(), false);
    std::vector<bool> includeSignatureList(latestProcessList.size(), false);
    std::vector<bool> isStaticFillCandidate(latestProcessList.size(), false);
    std::vector<char> staticFillSucceeded(latestProcessList.size(), 0);

    for (std::size_t recordIndex = 0; recordIndex < latestProcessList.size(); ++recordIndex)
    {
        ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];

        // 若 creationTime 未取到，仍可用 0 参与 key（稳定但区分度降低）。
        const std::string identityKey = ks::process::BuildProcessIdentityKey(
            processRecord.pid,
            processRecord.creationTime100ns);
        identityKeys[recordIndex] = identityKey;

        const auto oldCacheIt = previousCache.find(identityKey);
        const bool isKernelOnlyRecord =
            (kernelOnlyPidSet.find(processRecord.pid) != kernelOnlyPidSet.end());
        bool needsStaticFill = false;
        bool includeSignatureCheck = false;
        if (oldCacheIt != previousCache.end())
        {
            // 复用规则：PID + 创建时间相同则复用旧静态字段（性能计数器除外）。
            const ks::process::ProcessRecord& oldRecord = oldCacheIt->second.record;
            if (processRecord.imagePath.empty()) processRecord.imagePath = oldRecord.imagePath;
            if (processRecord.commandLine.empty()) processRecord.commandLine = oldRecord.commandLine;
            if (processRecord.userName.empty()) processRecord.userName = oldRecord.userName;
            if (processRecord.signatureState.empty()) processRecord.signatureState = oldRecord.signatureState;
            if (processRecord.signaturePublisher.empty()) processRecord.signaturePublisher = oldRecord.signaturePublisher;
            if (processRecord.r0FieldFlags == 0U) processRecord.r0FieldFlags = oldRecord.r0FieldFlags;
            if (processRecord.r0ImagePath.empty()) processRecord.r0ImagePath = oldRecord.r0ImagePath;
            if (processRecord.r0Status == KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE) processRecord.r0Status = oldRecord.r0Status;
            // PPL 保护级别枚举是手动刷新字段，不能从上一轮缓存继承。
            processRecord.protectionLevelKnown = false;
            processRecord.protectionLevel = 0;
            processRecord.protectionLevelText.clear();
            processRecord.r0Flags = oldRecord.r0Flags;
            processRecord.r0DynDataCapabilityMask = oldRecord.r0DynDataCapabilityMask;
            processRecord.r0Protection = oldRecord.r0Protection;
            processRecord.r0SignatureLevel = oldRecord.r0SignatureLevel;
            processRecord.r0SectionSignatureLevel = oldRecord.r0SectionSignatureLevel;
            processRecord.r0SessionSource = oldRecord.r0SessionSource;
            processRecord.r0ImagePathSource = oldRecord.r0ImagePathSource;
            processRecord.r0ProtectionSource = oldRecord.r0ProtectionSource;
            processRecord.r0SignatureLevelSource = oldRecord.r0SignatureLevelSource;
            processRecord.r0SectionSignatureLevelSource = oldRecord.r0SectionSignatureLevelSource;
            processRecord.r0ObjectTableSource = oldRecord.r0ObjectTableSource;
            processRecord.r0SectionObjectSource = oldRecord.r0SectionObjectSource;
            processRecord.r0ProtectionOffset = oldRecord.r0ProtectionOffset;
            processRecord.r0SignatureLevelOffset = oldRecord.r0SignatureLevelOffset;
            processRecord.r0SectionSignatureLevelOffset = oldRecord.r0SectionSignatureLevelOffset;
            processRecord.r0ObjectTableOffset = oldRecord.r0ObjectTableOffset;
            processRecord.r0SectionObjectOffset = oldRecord.r0SectionObjectOffset;
            processRecord.r0ObjectTableAddress = oldRecord.r0ObjectTableAddress;
            processRecord.r0SectionObjectAddress = oldRecord.r0SectionObjectAddress;
            processRecord.signatureTrusted = oldRecord.signatureTrusted;
            if (processRecord.startTimeText.empty()) processRecord.startTimeText = oldRecord.startTimeText;
            processRecord.isAdmin = oldRecord.isAdmin;
            processRecord.staticDetailsReady = oldRecord.staticDetailsReady;
            ++refreshResult.reusedProcessCount;

            // 旧进程若静态字段还不完整，或签名仍 Pending，则进入“待补齐候选”。
            // 对持续失败的进程做退避，避免反复占满预算导致其它进程长期 Pending。
            const bool signaturePending = (processRecord.signatureState.empty() || processRecord.signatureState == "Pending");
            const bool baseNeedsStaticFill = !processRecord.staticDetailsReady || (detailModeEnabled && signaturePending);
            const std::uint32_t oldFailureCount = oldCacheIt->second.staticFillFailureCount;
            if (baseNeedsStaticFill && oldFailureCount >= 3)
            {
                // 失败次数高时采用“稀疏重试”：降低对主预算的持续占用。
                // 公式引入 PID 偏移，避免同一轮集中重试同一批进程。
                constexpr std::uint64_t retryBackoffPeriod = 8;
                const bool shouldRetryThisRound =
                    ((refreshTicket + static_cast<std::uint64_t>(processRecord.pid)) % retryBackoffPeriod) == 0;
                needsStaticFill = shouldRetryThisRound;
            }
            else
            {
                needsStaticFill = baseNeedsStaticFill;
            }
            includeSignatureCheck = detailModeEnabled;
        }
        else
        {
            if (isKernelOnlyRecord)
            {
                // 仅内核可见记录默认不走“新增绿色”路径，避免与红色隐藏高亮冲突。
                isNewProcess[recordIndex] = false;
                needsStaticFill = false;
                includeSignatureCheck = false;
            }
            else
            {
                // 新出现进程：计数 + 依据预算决定是否补齐静态详情。
                ++refreshResult.newProcessCount;
                isNewProcess[recordIndex] = true;
                needsStaticFill = true;
                includeSignatureCheck = detailModeEnabled;
            }
        }
        const auto kernelProcessIt = kernelProcessByPid.find(processRecord.pid);
        if (kernelProcessIt != kernelProcessByPid.end())
        {
            mergeKernelProcessExtension(processRecord, kernelProcessIt->second);
        }

        if (isKernelOnlyRecord)
        {
            processRecord.staticDetailsReady = true;
            processRecord.dynamicCountersReady = true;
            if (processRecord.signatureState.empty())
            {
                processRecord.signatureState = "KernelOnly(Hidden?)";
            }
        }

        if (needsStaticFill)
        {
            isStaticFillCandidate[recordIndex] = true;
            includeSignatureList[recordIndex] = includeSignatureCheck;
        }
    }

    // 预算选择策略：
    // - 先收集全部候选，再做“轮转挑选”；
    // - 避免固定从头挑选导致尾部进程长期 Pending。
    std::vector<std::size_t> staticFillCandidateIndices;
    staticFillCandidateIndices.reserve(latestProcessList.size());
    for (std::size_t recordIndex = 0; recordIndex < isStaticFillCandidate.size(); ++recordIndex)
    {
        if (isStaticFillCandidate[recordIndex])
        {
            staticFillCandidateIndices.push_back(recordIndex);
        }
    }

    if (!staticFillCandidateIndices.empty() && staticFillBudget > 0)
    {
        const std::size_t candidateCount = staticFillCandidateIndices.size();
        const std::size_t allowCount = std::min(staticFillBudget, candidateCount);
        const std::size_t rotationOffset =
            static_cast<std::size_t>(
                (refreshTicket * static_cast<std::uint64_t>(std::max(1, staticDetailFillBudget)))
                % static_cast<std::uint64_t>(candidateCount));
        for (std::size_t offset = 0; offset < allowCount; ++offset)
        {
            const std::size_t candidateOrder = (rotationOffset + offset) % candidateCount;
            const std::size_t selectedIndex = staticFillCandidateIndices[candidateOrder];
            shouldFillStatic[selectedIndex] = true;
        }
    }

    // 未命中预算的候选统一保持 Pending，等待后续轮次补齐。
    for (const std::size_t recordIndex : staticFillCandidateIndices)
    {
        if (shouldFillStatic[recordIndex])
        {
            continue;
        }
        ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];
        if (processRecord.signatureState.empty())
        {
            processRecord.signatureState = "Pending";
        }
        processRecord.signaturePublisher.clear();
        processRecord.signatureTrusted = false;
        ++refreshResult.staticDeferredCount;
    }

    // 第二阶段：把“路径/签名/参数”等慢静态操作并行化，减少详细视图卡顿。
    std::vector<std::size_t> staticFillIndices;
    staticFillIndices.reserve(latestProcessList.size());
    for (std::size_t recordIndex = 0; recordIndex < shouldFillStatic.size(); ++recordIndex)
    {
        if (shouldFillStatic[recordIndex])
        {
            staticFillIndices.push_back(recordIndex);
        }
    }

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在并行补齐路径/签名/参数...", 40, 0.40f);
    }

    if (!staticFillIndices.empty())
    {
        // 线程数量策略：
        // - 详细视图：使用更多并行度，加速签名校验；
        // - 监视视图：仅小并发，避免过度占用 CPU。
        const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        const unsigned int wantedThreads = detailModeEnabled
            ? std::max(4u, std::min(12u, hardwareThreads))
            : 2u;
        const unsigned int workerCount = std::max(
            1u,
            std::min<unsigned int>(wantedThreads, static_cast<unsigned int>(staticFillIndices.size())));

        std::atomic<std::size_t> nextTaskIndex{ 0 };
        std::vector<std::thread> workerThreads;
        workerThreads.reserve(workerCount);

        // 每个线程循环领取 PID 任务并调用 FillProcessStaticDetails。
        for (unsigned int workerId = 0; workerId < workerCount; ++workerId)
        {
            workerThreads.emplace_back([&]() {
                for (;;)
                {
                    const std::size_t taskOrder = nextTaskIndex.fetch_add(1);
                    if (taskOrder >= staticFillIndices.size())
                    {
                        break;
                    }

                    const std::size_t recordIndex = staticFillIndices[taskOrder];
                    const bool fillOk = ks::process::FillProcessStaticDetails(
                        latestProcessList[recordIndex],
                        includeSignatureList[recordIndex]);
                    staticFillSucceeded[recordIndex] = fillOk ? 1 : 0;

                    // 失败降级策略：
                    // - 避免签名列长期保持 Pending；
                    // - 对权限受限场景直接标注 No Access。
                    if (!fillOk && includeSignatureList[recordIndex])
                    {
                        ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];
                        if (processRecord.signatureState.empty() || processRecord.signatureState == "Pending")
                        {
                            processRecord.signatureState = "No Access";
                            processRecord.signaturePublisher.clear();
                            processRecord.signatureTrusted = false;
                        }
                    }
                }
                });
        }

        for (std::thread& workerThread : workerThreads)
        {
            if (workerThread.joinable())
            {
                workerThread.join();
            }
        }
        refreshResult.staticFilledCount += staticFillIndices.size();
    }

    // 第二阶段补充：单独为 imagePath 做更高预算的快速补齐。
    // 说明：
    // 1) 图标展示只依赖 imagePath，且路径查询比“命令行/签名”更轻；
    // 2) 因此即使静态详情预算较低，也要额外补齐路径，避免整表图标都退化成占位图。
    const bool isFirstRound = previousCache.empty();
    int remainingImagePathBudget = detailModeEnabled
        ? (isFirstRound ? 640 : 320)
        : (isFirstRound ? 320 : 160);
    std::vector<std::size_t> imagePathFillIndices;
    imagePathFillIndices.reserve(latestProcessList.size());
    for (std::size_t recordIndex = 0; recordIndex < latestProcessList.size(); ++recordIndex)
    {
        const ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];
        if (processRecord.pid == 0 || !processRecord.imagePath.empty())
        {
            continue;
        }
        if (remainingImagePathBudget <= 0)
        {
            break;
        }
        imagePathFillIndices.push_back(recordIndex);
        --remainingImagePathBudget;
    }

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在补齐进程图标路径...", 48, 0.48f);
    }

    if (!imagePathFillIndices.empty())
    {
        // 独立路径补齐线程池：仅执行 QueryProcessPathByPid，避免 UI 线程兜底查询造成卡顿。
        const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        const unsigned int wantedThreads = detailModeEnabled ? std::min(8u, hardwareThreads) : std::min(4u, hardwareThreads);
        const unsigned int workerCount = std::max(
            1u,
            std::min<unsigned int>(wantedThreads, static_cast<unsigned int>(imagePathFillIndices.size())));
        std::atomic<std::size_t> nextTaskIndex{ 0 };
        std::atomic<std::size_t> filledCount{ 0 };
        std::vector<std::thread> workerThreads;
        workerThreads.reserve(workerCount);
        for (unsigned int workerId = 0; workerId < workerCount; ++workerId)
        {
            workerThreads.emplace_back([&]() {
                for (;;)
                {
                    const std::size_t taskOrder = nextTaskIndex.fetch_add(1);
                    if (taskOrder >= imagePathFillIndices.size())
                    {
                        break;
                    }

                    const std::size_t recordIndex = imagePathFillIndices[taskOrder];
                    ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];
                    const std::string pathText = ks::process::QueryProcessPathByPid(processRecord.pid);
                    if (!pathText.empty())
                    {
                        processRecord.imagePath = pathText;
                        filledCount.fetch_add(1);
                    }
                }
                });
        }
        for (std::thread& workerThread : workerThreads)
        {
            if (workerThread.joinable())
            {
                workerThread.join();
            }
        }
        refreshResult.imagePathFilledCount = filledCount.load();
    }

    // 第三阶段：计算性能差值并写回缓存（该阶段仍串行，保证逻辑简单稳定）。
    std::size_t processIndex = 0;
    for (std::size_t recordIndex = 0; recordIndex < latestProcessList.size(); ++recordIndex)
    {
        ++processIndex;
        ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];
        const std::string& identityKey = identityKeys[recordIndex];

        // 若当前策略未填动态计数器，则显式刷新一次。
        if (!processRecord.dynamicCountersReady)
        {
            ks::process::RefreshProcessDynamicCounters(processRecord);
        }

        // 计算 CPU/DISK 衍生计数，并写入下一轮样本。
        ks::process::CounterSample nextSample{};
        const auto oldCounterIt = previousCounters.find(identityKey);
        const ks::process::CounterSample* oldSample =
            (oldCounterIt == previousCounters.end()) ? nullptr : &oldCounterIt->second;
        ks::process::UpdateDerivedCounters(
            processRecord,
            oldSample,
            nextSample,
            logicalCpuCount,
            sampleTick);
        refreshResult.nextCounters[identityKey] = nextSample;

        CacheEntry cacheEntry{};
        cacheEntry.record = std::move(processRecord);
        cacheEntry.missingRounds = 0;
        cacheEntry.isNewInLatestRound = isNewProcess[recordIndex];
        cacheEntry.isExitedInLatestRound = false;
        cacheEntry.isKernelOnlyInLatestRound =
            (kernelOnlyPidSet.find(cacheEntry.record.pid) != kernelOnlyPidSet.end());
        {
            // staticFillAttemptCount/staticFillFailureCount 用途：
            // - 记录当前 identity 的补齐尝试历史；
            // - 下一轮用于“连续失败退避”，降低失败进程对预算的长期占用。
            const auto oldCacheIt = previousCache.find(identityKey);
            const std::uint32_t oldAttemptCount =
                (oldCacheIt == previousCache.end()) ? 0U : oldCacheIt->second.staticFillAttemptCount;
            const std::uint32_t oldFailureCount =
                (oldCacheIt == previousCache.end()) ? 0U : oldCacheIt->second.staticFillFailureCount;
            const bool attemptedThisRound = shouldFillStatic[recordIndex];
            const bool fillOkThisRound = (staticFillSucceeded[recordIndex] != 0);

            cacheEntry.staticFillAttemptCount = attemptedThisRound
                ? (oldAttemptCount + 1U)
                : oldAttemptCount;
            cacheEntry.staticFillFailureCount = attemptedThisRound
                ? (fillOkThisRound ? 0U : (oldFailureCount + 1U))
                : oldFailureCount;

            // 若当前记录已经具备可用静态详情，则主动清零失败计数。
            if (cacheEntry.record.staticDetailsReady && cacheEntry.record.signatureState != "Pending")
            {
                cacheEntry.staticFillFailureCount = 0;
            }
        }
        refreshResult.nextCache.emplace(identityKey, std::move(cacheEntry));

        // 进度条阶段 3：按处理进度更新（频率做了抽样，避免过度抖动）。
        if (progressTaskPid > 0 && (processIndex % 48 == 0 || processIndex == latestProcessList.size()))
        {
            const double ratio = latestProcessList.empty()
                ? 1.0
                : (static_cast<double>(processIndex) / static_cast<double>(latestProcessList.size()));
            const float progressValue = static_cast<float>(0.50 + ratio * 0.35); // 50% -> 85%
            kPro.set(progressTaskPid, "正在处理缓存与性能差值...", 55, progressValue);
        }
    }

    // 再处理退出进程：上一轮存在、本轮不存在，则保留显示 1 轮灰底。
    for (const auto& oldPair : previousCache)
    {
        if (refreshResult.nextCache.find(oldPair.first) != refreshResult.nextCache.end())
        {
            continue;
        }

        const CacheEntry& oldEntry = oldPair.second;
        if (oldEntry.isKernelOnlyInLatestRound)
        {
            // 仅内核可见记录不做“退出保留”，避免在关闭内核对比后残留一轮灰底。
            continue;
        }
        if (oldEntry.missingRounds >= 1)
        {
            // 已经保留过一轮，本次彻底移除。
            continue;
        }

        CacheEntry exitedEntry = oldEntry;
        exitedEntry.missingRounds = oldEntry.missingRounds + 1;
        exitedEntry.isNewInLatestRound = false;
        exitedEntry.isExitedInLatestRound = true;
        // 退出保留行也不能携带上一轮手动 PPL 枚举，避免灰色残留行显示过期保护级别。
        exitedEntry.record.protectionLevelKnown = false;
        exitedEntry.record.protectionLevel = 0;
        exitedEntry.record.protectionLevelText.clear();
        refreshResult.nextCache.emplace(oldPair.first, std::move(exitedEntry));
        ++refreshResult.exitedProcessCount;

        const auto oldCounterIt = previousCounters.find(oldPair.first);
        if (oldCounterIt != previousCounters.end())
        {
            refreshResult.nextCounters.emplace(oldPair.first, oldCounterIt->second);
        }
    }

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "后台刷新结果构建完成，等待主线程应用...", 90, 0.90f);
    }

    refreshResult.workerElapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - workerStartTime).count());

    return refreshResult;
}

void ProcessDock::rebuildTable()
{
    if (m_processTable == nullptr)
    {
        return;
    }

    if (isProcessActivityTableSnapshotActive())
    {
        rebuildProcessActivityTableSnapshotRecords();
    }

    // 记录用户当前排序列与顺序，解决“刷新后被重置为 PID 排序”的问题。
    const int previousSortColumn = m_processTable->header()->sortIndicatorSection();
    const Qt::SortOrder previousSortOrder = m_processTable->header()->sortIndicatorOrder();
    const std::string trackedIdentityKeyBeforeRebuild =
        !m_trackedSelectedIdentityKey.empty()
        ? m_trackedSelectedIdentityKey
        : selectedIdentityKey();
    std::unordered_set<std::string> trackedIdentityKeysBeforeRebuild;
    for (const std::string& identityKey : m_trackedSelectedIdentityKeys)
    {
        if (!identityKey.empty())
        {
            trackedIdentityKeysBeforeRebuild.insert(identityKey);
        }
    }
    if (!trackedIdentityKeyBeforeRebuild.empty())
    {
        trackedIdentityKeysBeforeRebuild.insert(trackedIdentityKeyBeforeRebuild);
    }
    const int trackedColumnBeforeRebuild = std::clamp(
        m_trackedSelectedColumn,
        0,
        static_cast<int>(TableColumn::Count) - 1);
    // 滚动位置快照：
    // - 保存刷新前用户视口位置；
    // - 刷新后恢复，避免每轮重建跳回顶部。
    QScrollBar* verticalScrollBar = m_processTable->verticalScrollBar();
    QScrollBar* horizontalScrollBar = m_processTable->horizontalScrollBar();
    const int verticalScrollValueBeforeRebuild = (verticalScrollBar != nullptr) ? verticalScrollBar->value() : 0;
    const int horizontalScrollValueBeforeRebuild = (horizontalScrollBar != nullptr) ? horizontalScrollBar->value() : 0;

    // 刷新期间临时冻结视图更新，减少大量 addTopLevelItem 时的重绘抖动。
    QSignalBlocker tableSignalBlocker(m_processTable);
    m_processTable->setUpdatesEnabled(false);
    m_processTable->clear();

    // 树状模式下保持父子顺序，禁用自动排序。
    const bool activitySnapshotActive = isProcessActivityTableSnapshotActive();
    const bool enableSorting = activitySnapshotActive || !isTreeModeEnabled();
    m_processTable->setSortingEnabled(false);

    const std::vector<DisplayRow> displayRows = buildDisplayOrder();

    // 先预计算 RAM/DISK/NET/句柄数的本轮最大值，用于把绝对值映射成“占用比例高亮”。
    double maxRamMB = 0.0;
    double maxDiskMBps = 0.0;
    double maxNetKBps = 0.0;
    std::uint32_t maxHandleCount = 0;
    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr)
        {
            continue;
        }
        maxRamMB = std::max(maxRamMB, displayRow.record->workingSetMB);
        maxDiskMBps = std::max(maxDiskMBps, displayRow.record->diskMBps);
        maxNetKBps = std::max(maxNetKBps, displayRow.record->netKBps);
        maxHandleCount = std::max(maxHandleCount, displayRow.record->handleCount);
    }

    // applyUsageHighlight 作用：
    // - 对目标单元格按占用比例绘制主题蓝背景；
    // - 占用越高，背景越明显。
    const auto applyUsageHighlight = [this](QTreeWidgetItem* item, const int columnIndex, double usageRatio)
        {
            if (item == nullptr)
            {
                return;
            }
            usageRatio = std::clamp(usageRatio, 0.0, 1.0);
            const QColor usageColor = usageRatioToHighlightColor(usageRatio);
            item->setBackground(columnIndex, usageColor);

            // 高占用时将文本置白，保证可读性。
            if (usageRatio >= 0.70)
            {
                item->setForeground(columnIndex, QColor(255, 255, 255));
            }
        };

    // trackedRowItemToRestore 作用：
    // - 记录当前这轮重建中是否找到了“用户之前选中的进程行”；
    // - 这样刷新完成后可以按 identityKey 恢复选中高亮。
    QTreeWidgetItem* trackedRowItemToRestore = nullptr;
    std::vector<QTreeWidgetItem*> trackedRowItemsToRestore;
    trackedRowItemsToRestore.reserve(trackedIdentityKeysBeforeRebuild.size());

    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr)
        {
            continue;
        }

        QTreeWidgetItem* rowItem = new ProcessSortTreeWidgetItem();
        const ks::process::ProcessRecord& processRecord = *displayRow.record;
        const std::string identityKey = ks::process::BuildProcessIdentityKey(
            processRecord.pid,
            processRecord.creationTime100ns);
        rowItem->setData(0, Qt::UserRole, QString::fromStdString(identityKey));
        rowItem->setToolTip(
            toColumnIndex(TableColumn::Name),
            activitySnapshotActive
                ? QStringLiteral("历史快照行：该行来自时间轴样本，不代表当前实时进程状态。")
                : QString::fromStdString(processRecord.processName));

        // 每一列文本填充，Name 列附加图标。
        for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
        {
            const TableColumn column = static_cast<TableColumn>(columnIndex);
            rowItem->setText(columnIndex, formatColumnText(processRecord, column, displayRow.depth));
        }

        // 排序键使用原始数值：展示文本可以带单位，但排序必须按真实大小比较。
        rowItem->setData(toColumnIndex(TableColumn::Pid), ProcessNumericSortRole, static_cast<double>(processRecord.pid));
        rowItem->setData(toColumnIndex(TableColumn::Cpu), ProcessNumericSortRole, processRecord.cpuPercent);
        rowItem->setData(toColumnIndex(TableColumn::Ram), ProcessNumericSortRole, processRecord.workingSetMB);
        rowItem->setData(toColumnIndex(TableColumn::Disk), ProcessNumericSortRole, processRecord.diskMBps);
        rowItem->setData(toColumnIndex(TableColumn::Gpu), ProcessNumericSortRole, processRecord.gpuPercent);
        rowItem->setData(toColumnIndex(TableColumn::Net), ProcessNumericSortRole, processRecord.netKBps);
        rowItem->setData(toColumnIndex(TableColumn::ParentPid), ProcessNumericSortRole, static_cast<double>(processRecord.parentPid));
        rowItem->setData(toColumnIndex(TableColumn::StartTime), ProcessNumericSortRole, static_cast<double>(processRecord.creationTime100ns));
        rowItem->setData(toColumnIndex(TableColumn::IsAdmin), ProcessNumericSortRole, processRecord.isAdmin ? 1.0 : 0.0);
        rowItem->setData(
            toColumnIndex(TableColumn::PplLevel),
            ProcessNumericSortRole,
            processRecord.protectionLevelKnown ? static_cast<double>(processRecord.protectionLevel) : -1.0);
        rowItem->setData(toColumnIndex(TableColumn::Protection), ProcessNumericSortRole, static_cast<double>(processRecord.r0Protection));
        rowItem->setData(toColumnIndex(TableColumn::Ppl), ProcessNumericSortRole, static_cast<double>(processRecord.r0Protection));
        rowItem->setData(toColumnIndex(TableColumn::HandleCount), ProcessNumericSortRole, static_cast<double>(processRecord.handleCount));
        rowItem->setData(
            toColumnIndex(TableColumn::HandleTable),
            ProcessNumericSortRole,
            ((processRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_AVAILABLE) != 0U) ? 1.0 : 0.0);
        rowItem->setData(
            toColumnIndex(TableColumn::SectionObject),
            ProcessNumericSortRole,
            ((processRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_AVAILABLE) != 0U) ? 1.0 : 0.0);
        rowItem->setData(toColumnIndex(TableColumn::R0Status), ProcessNumericSortRole, static_cast<double>(processRecord.r0Status));
        rowItem->setData(toColumnIndex(TableColumn::Name), ProcessEfficiencyModeKnownRole, processRecord.efficiencyModeSupported);
        rowItem->setData(toColumnIndex(TableColumn::Name), ProcessEfficiencyModeRole, processRecord.efficiencyModeEnabled);
        if (processRecord.efficiencyModeEnabled)
        {
            rowItem->setToolTip(
                toColumnIndex(TableColumn::Name),
                QStringLiteral("%1\n效率模式已启用")
                    .arg(QString::fromStdString(processRecord.processName)));
        }

        // 进程名列固定显示目标 EXE 图标（命中缓存后开销可控）。
        rowItem->setIcon(toColumnIndex(TableColumn::Name), resolveProcessIcon(processRecord));

        // 管理员列：按要求使用“绿色/红色方块”直观显示状态。
        rowItem->setTextAlignment(toColumnIndex(TableColumn::IsAdmin), Qt::AlignCenter);
        const QColor adminYesColor = KswordTheme::IsDarkModeEnabled()
            ? QColor(130, 210, 140)
            : QColor(34, 139, 34);
        const QColor adminNoColor = KswordTheme::IsDarkModeEnabled()
            ? QColor(255, 140, 140)
            : QColor(220, 50, 47);
        rowItem->setForeground(
            toColumnIndex(TableColumn::IsAdmin),
            processRecord.isAdmin ? adminYesColor : adminNoColor);

        // 数字签名列：非受信任时标红，方便快速识别风险进程。
        if (!processRecord.signatureTrusted && processRecord.signatureState != "Pending")
        {
            rowItem->setForeground(toColumnIndex(TableColumn::Signature), adminNoColor);
        }
        else if (processRecord.signatureTrusted)
        {
            rowItem->setForeground(toColumnIndex(TableColumn::Signature), adminYesColor);
        }

        // 退出保留进程灰色高亮；仅内核可见进程红色高亮；普通新增进程绿色高亮。
        if (displayRow.isExited)
        {
            for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
            {
                rowItem->setBackground(columnIndex, KswordTheme::ExitedRowBackgroundColor());
                rowItem->setForeground(columnIndex, KswordTheme::ExitedRowForegroundColor());
            }
        }
        else if (displayRow.isKernelOnly)
        {
            const QColor kernelOnlyForeground = KswordTheme::IsDarkModeEnabled()
                ? QColor(255, 140, 140)
                : QColor(200, 32, 32);
            const QColor kernelOnlyBackground = KswordTheme::IsDarkModeEnabled()
                ? QColor(110, 28, 28, 140)
                : QColor(255, 224, 224);
            for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
            {
                rowItem->setBackground(columnIndex, kernelOnlyBackground);
                rowItem->setForeground(columnIndex, kernelOnlyForeground);
            }
        }
        else if (displayRow.isNew)
        {
            for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
            {
                rowItem->setBackground(columnIndex, KswordTheme::NewRowBackgroundColor());
            }
        }
        else
        {
            // 常规行按占用比例做主题色高亮（CPU/RAM/DISK/GPU/NET）。
            const double cpuUsageRatio = std::clamp(processRecord.cpuPercent / 100.0, 0.0, 1.0);
            const double ramUsageRatio = (maxRamMB > 0.0)
                ? std::clamp(processRecord.workingSetMB / maxRamMB, 0.0, 1.0)
                : 0.0;
            const double diskUsageRatio = (maxDiskMBps > 0.0)
                ? std::clamp(processRecord.diskMBps / maxDiskMBps, 0.0, 1.0)
                : 0.0;
            const double gpuUsageRatio = std::clamp(processRecord.gpuPercent / 100.0, 0.0, 1.0);
            const double netUsageRatio = (maxNetKBps > 0.0)
                ? std::clamp(processRecord.netKBps / maxNetKBps, 0.0, 1.0)
                : 0.0;
            const double handleUsageRatio = (maxHandleCount > 0U)
                ? std::clamp(
                    static_cast<double>(processRecord.handleCount) / static_cast<double>(maxHandleCount),
                    0.0,
                    1.0)
                : 0.0;

            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Cpu), cpuUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Ram), ramUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Disk), diskUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Gpu), gpuUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Net), netUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::HandleCount), handleUsageRatio);
        }

        if (!trackedIdentityKeyBeforeRebuild.empty() && identityKey == trackedIdentityKeyBeforeRebuild)
        {
            trackedRowItemToRestore = rowItem;
        }
        if (trackedIdentityKeysBeforeRebuild.find(identityKey) != trackedIdentityKeysBeforeRebuild.end())
        {
            trackedRowItemsToRestore.push_back(rowItem);
        }

        m_processTable->addTopLevelItem(rowItem);
    }

    m_processTable->setSortingEnabled(enableSorting);
    if (enableSorting)
    {
        // 恢复用户上一次排序选择，而不是强制回到 PID 升序。
        const int safeSortColumn =
            (previousSortColumn >= 0 && previousSortColumn < static_cast<int>(TableColumn::Count))
            ? previousSortColumn
            : toColumnIndex(TableColumn::Pid);
        m_processTable->header()->setSortIndicator(safeSortColumn, previousSortOrder);
        m_processTable->sortItems(safeSortColumn, previousSortOrder);
    }

    // 按 identityKey 恢复用户之前选中的进程：
    // - 左键点中的那一行在刷新后继续保持高亮；
    // - 若该进程已不存在，则清空追踪状态，避免错误高亮。
    if (!trackedRowItemsToRestore.empty())
    {
        for (QTreeWidgetItem* rowItem : trackedRowItemsToRestore)
        {
            if (rowItem != nullptr)
            {
                rowItem->setSelected(true);
            }
        }
        QTreeWidgetItem* currentRowItemToRestore =
            trackedRowItemToRestore != nullptr ? trackedRowItemToRestore : trackedRowItemsToRestore.front();
        m_processTable->setCurrentItem(currentRowItemToRestore, trackedColumnBeforeRebuild);
        syncTrackedSelectionFromTable();
        m_trackedSelectedColumn = trackedColumnBeforeRebuild;
    }
    else if (!trackedIdentityKeysBeforeRebuild.empty() && currentProcessSearchText().isEmpty())
    {
        m_trackedSelectedIdentityKey.clear();
        m_trackedSelectedIdentityKeys.clear();
        m_trackedSelectedColumn = 0;
    }

    // 根据本轮数据刷新标题栏“占用总和”。
    updateUsageSummaryInHeader(displayRows);
    applyR0ColumnAvailability(displayRows);

    // 恢复滚动位置：保持用户当前视图位置不被刷新打断。
    if (verticalScrollBar != nullptr)
    {
        verticalScrollBar->setValue(std::clamp(
            verticalScrollValueBeforeRebuild,
            verticalScrollBar->minimum(),
            verticalScrollBar->maximum()));
    }
    if (horizontalScrollBar != nullptr)
    {
        horizontalScrollBar->setValue(std::clamp(
            horizontalScrollValueBeforeRebuild,
            horizontalScrollBar->minimum(),
            horizontalScrollBar->maximum()));
    }

    // 表格重建完成后恢复刷新绘制。
    m_processTable->setUpdatesEnabled(true);
    m_processTable->viewport()->update();

    // 重建完成后输出一条细粒度日志，便于分析 UI 刷新开销与排序状态。
    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] rebuildTable 完成, rows=" << displayRows.size()
        << ", treeMode=" << (isTreeModeEnabled() ? "true" : "false")
        << ", sortingEnabled=" << (enableSorting ? "true" : "false")
        << ", sortColumn=" << (enableSorting ? m_processTable->header()->sortIndicatorSection() : -1)
        << eol;
}

void ProcessDock::updateUsageSummaryInHeader(const std::vector<DisplayRow>& displayRows)
{
    // 标题栏展示占用总和：对当前可见数据（排除退出保留行）做聚合。
    if (m_processTable == nullptr || m_processTable->headerItem() == nullptr)
    {
        return;
    }

    double totalCpuPercent = 0.0;
    double totalRamMB = 0.0;
    double totalDiskMBps = 0.0;
    double totalGpuPercent = 0.0;
    double totalNetKBps = 0.0;
    std::uint64_t totalHandleCount = 0;
    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr || displayRow.isExited)
        {
            continue;
        }

        // CPU 汇总按用户要求排除“System Idle Process”(PID=0) 的空闲占比。
        const bool isSystemIdleProcess =
            (displayRow.record->pid == 0) ||
            (QString::fromStdString(displayRow.record->processName).compare("System Idle Process", Qt::CaseInsensitive) == 0);
        if (!isSystemIdleProcess)
        {
            totalCpuPercent += displayRow.record->cpuPercent;
        }

        totalRamMB += displayRow.record->ramMB;
        totalDiskMBps += displayRow.record->diskMBps;
        totalGpuPercent += displayRow.record->gpuPercent;
        totalNetKBps += displayRow.record->netKBps;
        totalHandleCount += displayRow.record->handleCount;
    }

    // 非占用列保持原始列名；占用列追加“总和”文本（不使用 Σ 符号）。
    QTreeWidgetItem* headerItem = m_processTable->headerItem();
    headerItem->setText(toColumnIndex(TableColumn::Name), ProcessTableHeaders.at(toColumnIndex(TableColumn::Name)));
    headerItem->setText(toColumnIndex(TableColumn::Pid), ProcessTableHeaders.at(toColumnIndex(TableColumn::Pid)));
    headerItem->setText(
        toColumnIndex(TableColumn::Cpu),
        QString("CPU %1%").arg(totalCpuPercent, 0, 'f', 2));
    headerItem->setText(
        toColumnIndex(TableColumn::Ram),
        QString("RAM %1 MB").arg(totalRamMB, 0, 'f', 1));
    headerItem->setText(
        toColumnIndex(TableColumn::Disk),
        QString("DISK %1 MB/s").arg(totalDiskMBps, 0, 'f', 2));
    headerItem->setText(
        toColumnIndex(TableColumn::Gpu),
        QString("GPU %1%").arg(totalGpuPercent, 0, 'f', 1));
    headerItem->setText(
        toColumnIndex(TableColumn::Net),
        QString("Net %1 KB/s").arg(totalNetKBps, 0, 'f', 2));
    headerItem->setText(toColumnIndex(TableColumn::Signature), ProcessTableHeaders.at(toColumnIndex(TableColumn::Signature)));
    headerItem->setText(toColumnIndex(TableColumn::Path), ProcessTableHeaders.at(toColumnIndex(TableColumn::Path)));
    headerItem->setText(toColumnIndex(TableColumn::ParentPid), ProcessTableHeaders.at(toColumnIndex(TableColumn::ParentPid)));
    headerItem->setText(toColumnIndex(TableColumn::CommandLine), ProcessTableHeaders.at(toColumnIndex(TableColumn::CommandLine)));
    headerItem->setText(toColumnIndex(TableColumn::User), ProcessTableHeaders.at(toColumnIndex(TableColumn::User)));
    headerItem->setText(toColumnIndex(TableColumn::StartTime), ProcessTableHeaders.at(toColumnIndex(TableColumn::StartTime)));
    headerItem->setText(toColumnIndex(TableColumn::IsAdmin), ProcessTableHeaders.at(toColumnIndex(TableColumn::IsAdmin)));
    headerItem->setText(toColumnIndex(TableColumn::PplLevel), ProcessTableHeaders.at(toColumnIndex(TableColumn::PplLevel)));
    headerItem->setText(toColumnIndex(TableColumn::Protection), ProcessTableHeaders.at(toColumnIndex(TableColumn::Protection)));
    headerItem->setText(toColumnIndex(TableColumn::Ppl), ProcessTableHeaders.at(toColumnIndex(TableColumn::Ppl)));
    headerItem->setText(
        toColumnIndex(TableColumn::HandleCount),
        QString("句柄数 %1").arg(static_cast<qulonglong>(totalHandleCount)));
    headerItem->setText(toColumnIndex(TableColumn::HandleTable), ProcessTableHeaders.at(toColumnIndex(TableColumn::HandleTable)));
    headerItem->setText(toColumnIndex(TableColumn::SectionObject), ProcessTableHeaders.at(toColumnIndex(TableColumn::SectionObject)));
    headerItem->setText(toColumnIndex(TableColumn::R0Status), ProcessTableHeaders.at(toColumnIndex(TableColumn::R0Status)));
}

void ProcessDock::applyR0ColumnAvailability(const std::vector<DisplayRow>& displayRows)
{
    if (m_processTable == nullptr)
    {
        return;
    }

    bool hasVisibleR0Extension = false;
    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr || displayRow.isExited)
        {
            continue;
        }

        if (isProcessR0ExtensionVisible(*displayRow.record))
        {
            hasVisibleR0Extension = true;
            break;
        }
    }

    const bool shouldAutoHide = !hasVisibleR0Extension;
    const bool stateChanged = (m_autoHideUnavailableR0Columns != shouldAutoHide);
    m_autoHideUnavailableR0Columns = shouldAutoHide;
    if (!stateChanged && !shouldAutoHide)
    {
        return;
    }

    const int r0OnlyColumns[] = {
        toColumnIndex(TableColumn::Protection),
        toColumnIndex(TableColumn::Ppl),
        toColumnIndex(TableColumn::HandleTable),
        toColumnIndex(TableColumn::SectionObject),
        toColumnIndex(TableColumn::R0Status)
    };

    for (const int columnIndex : r0OnlyColumns)
    {
        m_processTable->setColumnHidden(columnIndex, shouldAutoHide);
    }

    if (!shouldAutoHide)
    {
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Protection), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Ppl), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::HandleTable), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::SectionObject), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::R0Status), false);
    }

    applyAdaptiveColumnWidths();

    if (!stateChanged)
    {
        return;
    }

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] R0-only 列自动"
        << (shouldAutoHide ? "隐藏" : "显示")
        << ", reason="
        << (shouldAutoHide ? "所有可见行 R0 扩展均为 Unavailable" : "检测到可用 R0 扩展字段")
        << eol;
}

bool ProcessDock::isProcessActivityMetricEnabled(const ProcessActivityMetric metric) const
{
    // 按钮为空时采用默认全显示：
    // - 与初始化后的按钮状态保持一致；
    // - 这样首帧绘制不会因为控件尚未绑定而漏掉磁盘/网络/GPU。
    // 这保证面板初始化前调用也不会产生空指针。
    switch (metric)
    {
    case ProcessActivityMetric::Cpu:
        return m_activityCpuButton == nullptr || m_activityCpuButton->isChecked();
    case ProcessActivityMetric::Memory:
        return m_activityMemoryButton == nullptr || m_activityMemoryButton->isChecked();
    case ProcessActivityMetric::Disk:
        return m_activityDiskButton == nullptr || m_activityDiskButton->isChecked();
    case ProcessActivityMetric::Network:
        return m_activityNetworkButton == nullptr || m_activityNetworkButton->isChecked();
    case ProcessActivityMetric::Gpu:
        return m_activityGpuButton == nullptr || m_activityGpuButton->isChecked();
    default:
        return false;
    }
}

namespace
{
    // processActivitySampleMetricValue 作用：
    // - 根据当前选择范围提取一个样本的单项数值；
    // - selectionKeys 为空时取总体，非空时汇总对应进程。
    double processActivitySampleMetricValue(
        const ProcessDock::ProcessActivitySample& sample,
        const ProcessDock::ProcessActivityMetric metric,
        const std::vector<std::string>& selectionKeys)
    {
        if (selectionKeys.empty())
        {
            switch (metric)
            {
            case ProcessDock::ProcessActivityMetric::Cpu:
                return sample.totalCpuPercent;
            case ProcessDock::ProcessActivityMetric::Memory:
                return sample.totalMemoryMB;
            case ProcessDock::ProcessActivityMetric::Disk:
                return sample.totalDiskMBps;
            case ProcessDock::ProcessActivityMetric::Network:
                return sample.totalNetKBps;
            case ProcessDock::ProcessActivityMetric::Gpu:
                return sample.totalGpuPercent;
            default:
                return 0.0;
            }
        }

        double value = 0.0;
        for (const ProcessDock::ProcessActivityProcessPoint& processPoint : sample.processes)
        {
            if (std::find(selectionKeys.begin(), selectionKeys.end(), processPoint.identityKey) == selectionKeys.end())
            {
                continue;
            }
            switch (metric)
            {
            case ProcessDock::ProcessActivityMetric::Cpu:
                value += processPoint.cpuPercent;
                break;
            case ProcessDock::ProcessActivityMetric::Memory:
                value += processPoint.memoryMB;
                break;
            case ProcessDock::ProcessActivityMetric::Disk:
                value += processPoint.diskMBps;
                break;
            case ProcessDock::ProcessActivityMetric::Network:
                value += processPoint.netKBps;
                break;
            case ProcessDock::ProcessActivityMetric::Gpu:
                value += processPoint.gpuPercent;
                break;
            default:
                break;
            }
        }
        return value;
    }
}

bool ProcessDock::isProcessListPageVisibleForRecording() const
{
    // 默认只有进程列表页处于当前页时才刷新/记录；
    // 用户勾选“后台保持刷新/记录”后才允许切到其它子页继续刷新/记录。
    return m_sideTabWidget != nullptr &&
        m_processListPage != nullptr &&
        m_sideTabWidget->currentWidget() == m_processListPage &&
        m_processListPage->isVisible();
}

bool ProcessDock::isProcessActivityRefreshAllowedNow() const
{
    // 刷新允许逻辑只关心“是否应该继续枚举列表”：
    // - 暂停按钮关闭后不再刷新；
    // - 当前在进程列表页时允许刷新；
    // - 后台保持开关允许离开进程列表页后继续刷新。
    if (!m_monitoringEnabled)
    {
        return false;
    }

    if (m_activityBackgroundRecordCheck != nullptr && m_activityBackgroundRecordCheck->isChecked())
    {
        return true;
    }

    return isProcessListPageVisibleForRecording();
}

bool ProcessDock::isProcessActivityRecordingAllowedNow() const
{
    // 记录允许逻辑在刷新允许之上额外叠加“只刷新列表”：
    // - 这样可以继续更新下方列表；
    // - 同时不污染上方时间轴样本。
    if (!isProcessActivityRefreshAllowedNow())
    {
        return false;
    }

    if (m_activityListOnlyRefreshCheck != nullptr && m_activityListOnlyRefreshCheck->isChecked())
    {
        return false;
    }

    return true;
}

void ProcessDock::appendProcessActivitySample()
{
    if (!isProcessActivityRecordingAllowedNow())
    {
        updateProcessActivityStatusLabel();
        refreshProcessActivityChart();
        return;
    }

    if (m_activityRecordingStartTick100ns == 0 || m_activitySamples.empty())
    {
        m_activityRecordingStartTick100ns = steadyNow100ns();
    }

    ProcessActivitySample sample{};
    const std::uint64_t nowTick100ns = steadyNow100ns();
    sample.sequence = m_activityNextSequence++;
    sample.elapsedMs = (nowTick100ns >= m_activityRecordingStartTick100ns)
        ? ((nowTick100ns - m_activityRecordingStartTick100ns) / 10000ULL)
        : 0ULL;
    sample.unixMilliseconds = QDateTime::currentMSecsSinceEpoch();
    sample.processes.reserve(m_cacheByIdentity.size());

    for (const auto& cachePair : m_cacheByIdentity)
    {
        const CacheEntry& cacheEntry = cachePair.second;
        if (cacheEntry.isExitedInLatestRound)
        {
            continue;
        }

        const ks::process::ProcessRecord& processRecord = cacheEntry.record;
        ProcessActivityProcessPoint processPoint{};
        processPoint.identityKey = cachePair.first;
        processPoint.processName = processRecord.processName;
        processPoint.imagePath = processRecord.imagePath;
        processPoint.iconCacheKey = processRecord.processName + "|" + processRecord.imagePath;
        processPoint.creationTime100ns = processRecord.creationTime100ns;
        processPoint.pid = processRecord.pid;
        processPoint.cpuPercent = processRecord.cpuPercent;
        processPoint.memoryMB = processRecord.workingSetMB;
        processPoint.diskMBps = processRecord.diskMBps;
        processPoint.netKBps = processRecord.netKBps;
        processPoint.gpuPercent = processRecord.gpuPercent;

        const bool isSystemIdleProcess =
            (processRecord.pid == 0) ||
            (QString::fromStdString(processRecord.processName).compare("System Idle Process", Qt::CaseInsensitive) == 0);
        if (!isSystemIdleProcess)
        {
            sample.totalCpuPercent += processRecord.cpuPercent;
        }
        sample.totalMemoryMB += processRecord.workingSetMB;
        sample.totalDiskMBps += processRecord.diskMBps;
        sample.totalNetKBps += processRecord.netKBps;
        sample.totalGpuPercent += processRecord.gpuPercent;

        // 历史快照只保存轻量数据，但图标必须能按“进程名+路径”恢复：
        // - 这里复用实时列表的 resolveProcessIcon；
        // - 缓存的是 QIcon 副本，后续查看历史表格时不会再走 PID 查询。
        if (!processPoint.iconCacheKey.empty())
        {
            const QString iconKey = QString::fromStdString(processPoint.iconCacheKey);
            if (!m_activityIconCacheByProcessKey.contains(iconKey))
            {
                m_activityIconCacheByProcessKey.insert(iconKey, resolveProcessIcon(processRecord));
            }
        }
        sample.processes.push_back(std::move(processPoint));
    }

    m_activitySamples.push_back(std::move(sample));
    const bool sampleIndexShiftedLeft = trimProcessActivitySamples();
    if (m_activityTableSnapshotIndex >= static_cast<int>(m_activitySamples.size()))
    {
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
    }
    refreshProcessActivityTimeline(sampleIndexShiftedLeft);
    refreshProcessActivityChart();
    updateProcessActivityStatusLabel();
}

bool ProcessDock::trimProcessActivitySamples()
{
    // 样本缓存固定上限，避免 0.1s 长时间记录造成内存无限增长。
    if (m_activitySamples.size() <= ActivityMaximumSampleCount)
    {
        return false;
    }

    const std::size_t removeCount = m_activitySamples.size() - ActivityMaximumSampleCount;
    m_activitySamples.erase(m_activitySamples.begin(), m_activitySamples.begin() + static_cast<std::ptrdiff_t>(removeCount));
    if (m_activityTableSnapshotIndex >= 0)
    {
        m_activityTableSnapshotIndex -= static_cast<int>(removeCount);
        if (m_activityTableSnapshotIndex < 0)
        {
            m_activityTableSnapshotIndex = -1;
            m_activityTableSnapshotRecords.clear();
        }
    }
    return removeCount > 0;
}

void ProcessDock::refreshProcessActivityTimeline(const bool indexShiftedLeft)
{
    if (m_activityTimelineSlider == nullptr)
    {
        return;
    }

    const bool oldUpdating = m_activityTimelineSliderUpdating;
    const int sampleCount = static_cast<int>(m_activitySamples.size());
    const int previousValue = (indexShiftedLeft && !m_activityTimelinePinnedToLatest)
        ? std::max(0, m_activityTimelineSlider->value())
        : m_activityTimelineSlider->value();
    const int previousMaximum = m_activityTimelineSlider->maximum();
    const bool shouldPinToLatest = m_activityTimelinePinnedToLatest || previousValue >= previousMaximum;

    m_activityTimelineSliderUpdating = true;
    m_activityTimelineSlider->setRange(0, std::max(0, sampleCount - 1));
    if (sampleCount == 0)
    {
        m_activityTimelineSlider->setValue(0);
    }
    else if (shouldPinToLatest)
    {
        m_activityTimelineSlider->setValue(sampleCount - 1);
        m_activityTimelinePinnedToLatest = true;
    }
    else
    {
        const int restoredValue = std::clamp(previousValue, 0, sampleCount - 1);
        m_activityTimelineSlider->setValue(restoredValue);
        m_activityTimelinePinnedToLatest = (restoredValue >= sampleCount - 1);
    }
    m_activityTimelineSliderUpdating = oldUpdating;

    if (sampleCount > 0)
    {
        if (m_activityTimelinePinnedToLatest)
        {
            m_activityTableSnapshotIndex = -1;
            m_activityTableSnapshotRecords.clear();
        }
        else if (isProcessActivityTableSnapshotActive())
        {
            rebuildProcessActivityTableSnapshotRecords();
        }
        previewProcessActivitySnapshotForIndex(m_activityTimelineSlider->value());
    }
    else if (m_activityChartWidget != nullptr)
    {
        m_activityChartWidget->setFocusedSampleIndex(-1);
    }
}

void ProcessDock::refreshProcessActivityChart()
{
    if (m_activityChartWidget != nullptr)
    {
        const int sampleIndex = (m_activityTimelineSlider != nullptr) ? m_activityTimelineSlider->value() : -1;
        m_activityChartWidget->setFocusedSampleIndex(sampleIndex);
        m_activityChartWidget->update();
    }
}

void ProcessDock::updateProcessActivityStatusLabel()
{
    if (m_activityStatusLabel == nullptr)
    {
        return;
    }

    QString stateText;
    const bool refreshAllowed = isProcessActivityRefreshAllowedNow();
    const bool recordingAllowed = isProcessActivityRecordingAllowedNow();
    m_activityRecordingEnabled = recordingAllowed;
    if (!m_monitoringEnabled)
    {
        stateText = QStringLiteral("已暂停刷新/记录");
    }
    else if (!refreshAllowed)
    {
        stateText = QStringLiteral("进程页隐藏，刷新/记录自动暂停");
    }
    else if (!recordingAllowed)
    {
        stateText = QStringLiteral("刷新中，不写记录");
    }
    else if (m_activityBackgroundRecordCheck != nullptr && m_activityBackgroundRecordCheck->isChecked())
    {
        stateText = QStringLiteral("后台刷新，同步记录");
    }
    else
    {
        stateText = QStringLiteral("刷新中，同步记录");
    }

    const int intervalMs = refreshIntervalMillisecondsFromInput();
    const int tableIntervalMs = tableRefreshIntervalMillisecondsFromInput();
    m_activityStatusLabel->setText(QStringLiteral("进程活动：%1 | 样本 %2 | 打点 %3s | 列表 %4s%5%6%7")
        .arg(stateText)
        .arg(static_cast<qulonglong>(m_activitySamples.size()))
        .arg(static_cast<double>(intervalMs) / 1000.0, 0, 'f', intervalMs < 1000 ? 2 : 1)
        .arg(static_cast<double>(tableIntervalMs) / 1000.0, 0, 'f', tableIntervalMs < 1000 ? 2 : 1)
        .arg((m_activityBackgroundRecordCheck != nullptr && m_activityBackgroundRecordCheck->isChecked())
            ? QStringLiteral(" | 后台")
            : QString())
        .arg((m_activityListOnlyRefreshCheck != nullptr && m_activityListOnlyRefreshCheck->isChecked())
            ? QStringLiteral(" | 不记录")
            : QString())
        .arg(isProcessActivityTableSnapshotActive() ? QStringLiteral(" | 表格=历史快照") : QStringLiteral(" | 表格=实时")));
}

void ProcessDock::previewProcessActivitySnapshotForIndex(const int sampleIndex)
{
    if (m_activitySamples.empty())
    {
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
        if (m_activitySnapshotLabel != nullptr)
        {
            m_activitySnapshotLabel->setText(QStringLiteral("时间轴快照：暂无样本"));
        }
        if (m_activityChartWidget != nullptr)
        {
            m_activityChartWidget->setFocusedSampleIndex(-1);
        }
        return;
    }

    const int safeIndex = std::clamp(sampleIndex, 0, static_cast<int>(m_activitySamples.size()) - 1);
    if (m_activityChartWidget != nullptr)
    {
        m_activityChartWidget->setFocusedSampleIndex(safeIndex);
    }
    if (m_activitySnapshotLabel != nullptr)
    {
        m_activitySnapshotLabel->setText(buildProcessActivitySnapshotText(safeIndex));
    }
}

void ProcessDock::showProcessActivitySnapshotForIndex(const int sampleIndex)
{
    previewProcessActivitySnapshotForIndex(sampleIndex);
}

void ProcessDock::commitProcessActivityTimelineIndex(const int sampleIndex)
{
    if (m_activitySamples.empty())
    {
        m_activityTableSnapshotIndex = -1;
        m_activityTableSnapshotRecords.clear();
        previewProcessActivitySnapshotForIndex(-1);
        rebuildTable();
        updateProcessActivityStatusLabel();
        return;
    }

    const int safeIndex = std::clamp(sampleIndex, 0, static_cast<int>(m_activitySamples.size()) - 1);
    const bool latestSelected =
        (m_activityTimelineSlider != nullptr && safeIndex >= m_activityTimelineSlider->maximum()) ||
        (safeIndex >= static_cast<int>(m_activitySamples.size()) - 1);

    m_activityTimelinePinnedToLatest = latestSelected;
    m_activityTableSnapshotIndex = latestSelected ? -1 : safeIndex;
    if (m_activityTimelineSlider != nullptr && m_activityTimelineSlider->value() != safeIndex)
    {
        const bool oldUpdating = m_activityTimelineSliderUpdating;
        m_activityTimelineSliderUpdating = true;
        m_activityTimelineSlider->setValue(safeIndex);
        m_activityTimelineSliderUpdating = oldUpdating;
    }

    previewProcessActivitySnapshotForIndex(safeIndex);
    rebuildProcessActivityTableSnapshotRecords();
    rebuildTable();
    updateProcessActivityStatusLabel();
}

bool ProcessDock::isProcessActivityTableSnapshotActive() const
{
    return m_activityTableSnapshotIndex >= 0 &&
        m_activityTableSnapshotIndex < static_cast<int>(m_activitySamples.size());
}

void ProcessDock::rebuildProcessActivityTableSnapshotRecords()
{
    m_activityTableSnapshotRecords.clear();
    if (!isProcessActivityTableSnapshotActive())
    {
        return;
    }

    const ProcessActivitySample& sample =
        m_activitySamples[static_cast<std::size_t>(m_activityTableSnapshotIndex)];
    m_activityTableSnapshotRecords.reserve(sample.processes.size());
    for (const ProcessActivityProcessPoint& processPoint : sample.processes)
    {
        ks::process::ProcessRecord record{};
        record.pid = processPoint.pid;
        record.parentPid = 0;
        record.threadCount = 0;
        record.handleCount = 0;
        record.creationTime100ns = processPoint.creationTime100ns;
        if (record.creationTime100ns == 0)
        {
            record.creationTime100ns = (sample.sequence + 1U) * 100000ULL + processPoint.pid;
        }
        record.processName = processPoint.processName.empty() ? std::string("PID ") + std::to_string(processPoint.pid) : processPoint.processName;
        record.imagePath = processPoint.imagePath.empty() ? std::string("历史快照") : processPoint.imagePath;
        record.commandLine = "时间轴历史样本";
        record.userName = "-";
        record.signatureState = "历史快照";
        record.startTimeText = QDateTime::fromMSecsSinceEpoch(sample.unixMilliseconds)
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
            .toStdString();
        record.cpuPercent = processPoint.cpuPercent;
        record.ramMB = processPoint.memoryMB;
        record.workingSetMB = processPoint.memoryMB;
        record.diskMBps = processPoint.diskMBps;
        record.netKBps = processPoint.netKBps;
        record.gpuPercent = processPoint.gpuPercent;
        record.staticDetailsReady = true;
        record.dynamicCountersReady = true;
        m_activityTableSnapshotRecords.push_back(std::move(record));
    }
}

QString ProcessDock::buildProcessActivitySnapshotText(const int sampleIndex) const
{
    if (m_activitySamples.empty())
    {
        return QStringLiteral("时间轴快照：暂无样本");
    }

    const int safeIndex = std::clamp(sampleIndex, 0, static_cast<int>(m_activitySamples.size()) - 1);
    const ProcessActivitySample& sample = m_activitySamples[static_cast<std::size_t>(safeIndex)];
    const std::vector<std::string> selectionKeys = currentProcessActivitySelectionKeys();

    const double cpuValue = processActivitySampleMetricValue(sample, ProcessActivityMetric::Cpu, selectionKeys);
    const double memoryValue = processActivitySampleMetricValue(sample, ProcessActivityMetric::Memory, selectionKeys);
    const double diskValue = processActivitySampleMetricValue(sample, ProcessActivityMetric::Disk, selectionKeys);
    const double netValue = processActivitySampleMetricValue(sample, ProcessActivityMetric::Network, selectionKeys);
    const double gpuValue = processActivitySampleMetricValue(sample, ProcessActivityMetric::Gpu, selectionKeys);
    double maxDiskValue = 0.0;
    double maxNetValue = 0.0;
    for (const ProcessActivitySample& historySample : m_activitySamples)
    {
        maxDiskValue = std::max(maxDiskValue, processActivitySampleMetricValue(historySample, ProcessActivityMetric::Disk, selectionKeys));
        maxNetValue = std::max(maxNetValue, processActivitySampleMetricValue(historySample, ProcessActivityMetric::Network, selectionKeys));
    }
    const double memoryPercent = clampPercentValue(
        (memoryValue / std::max(1.0, m_activityTotalPhysicalMemoryMB)) * 100.0);
    const double diskPercent = clampPercentValue((diskValue / std::max(1.0, maxDiskValue)) * 100.0);
    const double netPercent = clampPercentValue((netValue / std::max(1.0, maxNetValue)) * 100.0);
    std::vector<ProcessActivityProcessPoint> matchedProcesses;
    if (!selectionKeys.empty())
    {
        for (const ProcessActivityProcessPoint& processPoint : sample.processes)
        {
            if (std::find(selectionKeys.begin(), selectionKeys.end(), processPoint.identityKey) == selectionKeys.end())
            {
                continue;
            }
            matchedProcesses.push_back(processPoint);
        }
    }

    const QDateTime sampleDateTime = QDateTime::fromMSecsSinceEpoch(sample.unixMilliseconds);
    QString text = QStringLiteral("时间轴快照：%1 / +%2 | 范围:%3 | CPU %4% | 内存 %5% (%6 MB) | 磁盘 %7% (%8 MB/s) | 网络 %9% (%10 KB/s) | GPU %11%")
        .arg(sampleDateTime.toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(formatActivityElapsedText(sample.elapsedMs))
        .arg(selectionKeys.empty()
            ? QStringLiteral("总体")
            : QStringLiteral("选中%1个进程").arg(static_cast<qulonglong>(selectionKeys.size())))
        .arg(clampPercentValue(cpuValue), 0, 'f', 2)
        .arg(memoryPercent, 0, 'f', 2)
        .arg(memoryValue, 0, 'f', 1)
        .arg(diskPercent, 0, 'f', 2)
        .arg(diskValue, 0, 'f', 2)
        .arg(netPercent, 0, 'f', 2)
        .arg(netValue, 0, 'f', 2)
        .arg(gpuValue, 0, 'f', 1);

    QStringList enabledMetricTextList;
    const ProcessActivityMetric allMetrics[] = {
        ProcessActivityMetric::Cpu,
        ProcessActivityMetric::Memory,
        ProcessActivityMetric::Disk,
        ProcessActivityMetric::Network,
        ProcessActivityMetric::Gpu
    };
    for (const ProcessActivityMetric metric : allMetrics)
    {
        if (isProcessActivityMetricEnabled(metric))
        {
            const double metricValue = processActivitySampleMetricValue(sample, metric, selectionKeys);
            double percentValue = 0.0;
            switch (metric)
            {
            case ProcessActivityMetric::Cpu:
            case ProcessActivityMetric::Gpu:
                percentValue = clampPercentValue(metricValue);
                break;
            case ProcessActivityMetric::Memory:
                percentValue = memoryPercent;
                break;
            case ProcessActivityMetric::Disk:
                percentValue = diskPercent;
                break;
            case ProcessActivityMetric::Network:
                percentValue = netPercent;
                break;
            default:
                percentValue = 0.0;
                break;
            }
            enabledMetricTextList << QStringLiteral("%1 %2%3")
                .arg(processActivityMetricText(metric))
                .arg(percentValue, 0, 'f', percentValue >= 100.0 ? 1 : 2)
                .arg(processActivityMetricUnit(metric));
        }
    }
    if (!enabledMetricTextList.isEmpty())
    {
        text += QStringLiteral(" | 当前显示: %1").arg(enabledMetricTextList.join(QStringLiteral(", ")));
    }

    if (!selectionKeys.empty())
    {
        QStringList processTextList;
        const int maxProcessPreviewCount = 4;
        for (int i = 0; i < static_cast<int>(matchedProcesses.size()) && i < maxProcessPreviewCount; ++i)
        {
            const ProcessActivityProcessPoint& processPoint = matchedProcesses[static_cast<std::size_t>(i)];
            processTextList << QStringLiteral("%1(%2)")
                .arg(QString::fromStdString(processPoint.processName.empty() ? std::string("PID") : processPoint.processName))
                .arg(processPoint.pid);
        }
        if (matchedProcesses.size() > static_cast<std::size_t>(maxProcessPreviewCount))
        {
            processTextList << QStringLiteral("...");
        }
        text += QStringLiteral(" | 进程: %1").arg(processTextList.isEmpty() ? QStringLiteral("该时刻未出现") : processTextList.join(QStringLiteral(", ")));
    }
    else
    {
        text += QStringLiteral(" | 进程数: %1").arg(static_cast<qulonglong>(sample.processes.size()));
    }

    return text;
}

std::vector<std::string> ProcessDock::currentProcessActivitySelectionKeys() const
{
    // 使用已追踪的多选集合，避免读右键菜单冻结副本；
    // 图表只关心用户当前表格选择，不应被上下文菜单动作影响。
    std::vector<std::string> selectionKeys;
    std::unordered_set<std::string> visitedSet;
    if (m_processTable != nullptr)
    {
        const QList<QTreeWidgetItem*> selectedItems = m_processTable->selectedItems();
        selectionKeys.reserve(static_cast<std::size_t>(selectedItems.size()));
        for (QTreeWidgetItem* itemPointer : selectedItems)
        {
            if (itemPointer == nullptr)
            {
                continue;
            }
            const std::string identityKey = itemPointer->data(0, Qt::UserRole).toString().toStdString();
            if (!identityKey.empty() && visitedSet.insert(identityKey).second)
            {
                selectionKeys.push_back(identityKey);
            }
        }
    }
    if (isProcessActivityTableSnapshotActive() && selectionKeys.empty())
    {
        return selectionKeys;
    }
    if (selectionKeys.empty())
    {
        for (const std::string& identityKey : m_trackedSelectedIdentityKeys)
        {
            if (!identityKey.empty() &&
                m_cacheByIdentity.find(identityKey) != m_cacheByIdentity.end() &&
                visitedSet.insert(identityKey).second)
            {
                selectionKeys.push_back(identityKey);
            }
        }
    }
    return selectionKeys;
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildDisplayOrder() const
{
    if (isProcessActivityTableSnapshotActive())
    {
        return buildActivitySnapshotDisplayOrder();
    }

    // 搜索激活时统一返回扁平结果：
    // - 用户搜索的目标是“快速定位进程”，不需要树结构干扰；
    // - 扁平结果还能避免父节点未命中时留下孤立缩进。
    if (!currentProcessSearchText().isEmpty())
    {
        return buildListDisplayOrder();
    }

    return isTreeModeEnabled() ? buildTreeDisplayOrder() : buildListDisplayOrder();
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildActivitySnapshotDisplayOrder() const
{
    // 历史时间轴模式：
    // - 下方进程表直接展示当时样本中的进程快照；
    // - 不走实时缓存和树形父子排序，避免把历史时刻与当前系统状态混合。
    std::vector<DisplayRow> displayRows;
    displayRows.reserve(m_activityTableSnapshotRecords.size());
    for (const ks::process::ProcessRecord& processRecord : m_activityTableSnapshotRecords)
    {
        if (!processRecordMatchesSearch(processRecord))
        {
            continue;
        }

        DisplayRow displayRow{};
        displayRow.record = const_cast<ks::process::ProcessRecord*>(&processRecord);
        displayRow.depth = 0;
        displayRow.isNew = false;
        displayRow.isExited = false;
        displayRow.isKernelOnly = false;
        displayRows.push_back(displayRow);
    }

    std::sort(displayRows.begin(), displayRows.end(), [](const DisplayRow& leftRow, const DisplayRow& rightRow) {
        if (leftRow.record == nullptr || rightRow.record == nullptr)
        {
            return false;
        }
        return leftRow.record->pid < rightRow.record->pid;
    });
    return displayRows;
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildListDisplayOrder() const
{
    std::vector<DisplayRow> displayRows;
    displayRows.reserve(m_cacheByIdentity.size());

    for (const auto& cachePair : m_cacheByIdentity)
    {
        const bool isKswordHidden =
            ((cachePair.second.record.r0Flags & KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI) != 0U) ||
            (m_hiddenProcessPidSet.find(cachePair.second.record.pid) != m_hiddenProcessPidSet.end());
        const bool showKswordHidden =
            (m_showKswordHiddenProcessCheck != nullptr && m_showKswordHiddenProcessCheck->isChecked());
        if (isKswordHidden && !showKswordHidden)
        {
            continue;
        }
        if (!processRecordMatchesSearch(cachePair.second.record))
        {
            continue;
        }

        DisplayRow displayRow{};
        displayRow.record = const_cast<ks::process::ProcessRecord*>(&cachePair.second.record);
        displayRow.depth = 0;
        displayRow.isNew = cachePair.second.isNewInLatestRound;
        displayRow.isExited = cachePair.second.isExitedInLatestRound;
        displayRow.isKernelOnly = cachePair.second.isKernelOnlyInLatestRound;
        displayRows.push_back(displayRow);
    }

    std::sort(displayRows.begin(), displayRows.end(), [](const DisplayRow& leftRow, const DisplayRow& rightRow) {
        if (leftRow.record == nullptr || rightRow.record == nullptr)
        {
            return false;
        }
        return leftRow.record->pid < rightRow.record->pid;
    });
    return displayRows;
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildTreeDisplayOrder() const
{
    // Step1：把缓存转换成便于处理的指针数组。
    struct Node
    {
        const std::string* identityKey = nullptr;
        const CacheEntry* cacheEntry = nullptr;
    };
    std::vector<Node> nodes;
    nodes.reserve(m_cacheByIdentity.size());
    for (const auto& cachePair : m_cacheByIdentity)
    {
        const bool isKswordHidden =
            ((cachePair.second.record.r0Flags & KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI) != 0U) ||
            (m_hiddenProcessPidSet.find(cachePair.second.record.pid) != m_hiddenProcessPidSet.end());
        const bool showKswordHidden =
            (m_showKswordHiddenProcessCheck != nullptr && m_showKswordHiddenProcessCheck->isChecked());
        if (isKswordHidden && !showKswordHidden)
        {
            continue;
        }
        nodes.push_back(Node{ &cachePair.first, &cachePair.second });
    }

    // Step2：构建 parentPid -> child 节点列表。
    std::unordered_map<std::uint32_t, std::vector<Node>> childrenByParentPid;
    std::unordered_set<std::uint32_t> existingPidSet;
    for (const Node& node : nodes)
    {
        if (node.cacheEntry == nullptr)
        {
            continue;
        }
        existingPidSet.insert(node.cacheEntry->record.pid);
        childrenByParentPid[node.cacheEntry->record.parentPid].push_back(node);
    }

    // 子列表按 PID 排序，保证同层稳定顺序。
    for (auto& pair : childrenByParentPid)
    {
        auto& childNodes = pair.second;
        std::sort(childNodes.begin(), childNodes.end(), [](const Node& leftNode, const Node& rightNode) {
            return leftNode.cacheEntry->record.pid < rightNode.cacheEntry->record.pid;
        });
    }

    // Step3：找到根节点（父 PID 不存在或为 0）。
    std::vector<Node> rootNodes;
    for (const Node& node : nodes)
    {
        const std::uint32_t parentPid = node.cacheEntry->record.parentPid;
        if (parentPid == 0 || existingPidSet.find(parentPid) == existingPidSet.end())
        {
            rootNodes.push_back(node);
        }
    }
    std::sort(rootNodes.begin(), rootNodes.end(), [](const Node& leftNode, const Node& rightNode) {
        return leftNode.cacheEntry->record.pid < rightNode.cacheEntry->record.pid;
    });

    // Step4：DFS 生成“树状列表顺序 + 缩进深度”。
    std::vector<DisplayRow> displayRows;
    std::unordered_set<std::string> visitedIdentitySet;

    std::function<void(const Node&, int)> appendNode;
    appendNode = [&](const Node& node, const int depth)
        {
            if (node.identityKey == nullptr || node.cacheEntry == nullptr)
            {
                return;
            }
            if (visitedIdentitySet.find(*node.identityKey) != visitedIdentitySet.end())
            {
                return;
            }
            visitedIdentitySet.insert(*node.identityKey);

            DisplayRow displayRow{};
            displayRow.record = const_cast<ks::process::ProcessRecord*>(&node.cacheEntry->record);
            displayRow.depth = depth;
            displayRow.isNew = node.cacheEntry->isNewInLatestRound;
            displayRow.isExited = node.cacheEntry->isExitedInLatestRound;
            displayRow.isKernelOnly = node.cacheEntry->isKernelOnlyInLatestRound;
            displayRows.push_back(displayRow);

            const auto childIt = childrenByParentPid.find(node.cacheEntry->record.pid);
            if (childIt == childrenByParentPid.end())
            {
                return;
            }
            for (const Node& childNode : childIt->second)
            {
                appendNode(childNode, depth + 1);
            }
        };

    for (const Node& rootNode : rootNodes)
    {
        appendNode(rootNode, 0);
    }

    // 兜底：若仍有未访问节点（极端 parent 环），直接平铺补入。
    for (const Node& node : nodes)
    {
        if (node.identityKey == nullptr || node.cacheEntry == nullptr)
        {
            continue;
        }
        if (visitedIdentitySet.find(*node.identityKey) != visitedIdentitySet.end())
        {
            continue;
        }
        DisplayRow fallbackRow{};
        fallbackRow.record = const_cast<ks::process::ProcessRecord*>(&node.cacheEntry->record);
        fallbackRow.depth = 0;
        fallbackRow.isNew = node.cacheEntry->isNewInLatestRound;
        fallbackRow.isExited = node.cacheEntry->isExitedInLatestRound;
        fallbackRow.isKernelOnly = node.cacheEntry->isKernelOnlyInLatestRound;
        displayRows.push_back(fallbackRow);
    }

    return displayRows;
}

void ProcessDock::showTableContextMenu(const QPoint& localPosition)
{
    QTreeWidgetItem* clickedItem = m_processTable->itemAt(localPosition);
    if (clickedItem == nullptr)
    {
        clearContextActionBinding();
        return;
    }
    const int clickedColumn = m_processTable->columnAt(localPosition.x());

    // 右键行为：
    // - 若右键点在已有多选集合内，则保持集合不变，菜单动作对所有选中行生效；
    // - 若右键点在未选中行上，则切换为该单行，保持传统右键体验。
    if (!clickedItem->isSelected())
    {
        m_processTable->clearSelection();
        if (clickedColumn >= 0)
        {
            m_processTable->setCurrentItem(clickedItem, clickedColumn);
        }
        else
        {
            m_processTable->setCurrentItem(clickedItem);
        }
        clickedItem->setSelected(true);
    }
    else if (clickedColumn >= 0)
    {
        if (QItemSelectionModel* selectionModel = m_processTable->selectionModel())
        {
            selectionModel->setCurrentIndex(
                m_processTable->indexFromItem(clickedItem, clickedColumn),
                QItemSelectionModel::NoUpdate);
        }
    }
    else
    {
        if (QItemSelectionModel* selectionModel = m_processTable->selectionModel())
        {
            selectionModel->setCurrentIndex(
                m_processTable->indexFromItem(clickedItem, 0),
                QItemSelectionModel::NoUpdate);
        }
    }
    bindContextActionToItem(clickedItem);
    const std::vector<ProcessActionTarget> contextActionTargets = selectedActionTargets();
    const bool hasBatchSelection = contextActionTargets.size() > 1;
    const ks::process::ProcessRecord* contextProcessRecord =
        contextActionTargets.empty() ? nullptr : &contextActionTargets.front().record;

    QMenu contextMenu(this);
    // 右键菜单显式样式：避免浅色模式在透明父控件下出现黑底黑字。
    contextMenu.setStyleSheet(buildThreadContextMenuStyle());

    // R0 动作图标构造器：
    // - 基础图标仍沿用主题蓝；
    // - 在图标右下角叠加 Kernel.png，作为 R0 入口统一视觉标识。
    const auto buildR0ActionIcon = [this](const char* iconPath) -> QIcon
    {
        QPixmap iconPixmap = blueTintedIcon(iconPath).pixmap(DefaultIconSize);
        if (iconPixmap.isNull())
        {
            iconPixmap = QPixmap(DefaultIconSize);
            iconPixmap.fill(Qt::transparent);
        }

        const QPixmap kernelPixmap(KernelBadgeImagePath);
        if (!kernelPixmap.isNull())
        {
            const int badgeSide = std::max(8, std::min(iconPixmap.width(), iconPixmap.height()) / 2);
            const QPixmap scaledBadge = kernelPixmap.scaled(
                badgeSide,
                badgeSide,
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation);

            QPainter painter(&iconPixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.drawPixmap(
                iconPixmap.width() - scaledBadge.width(),
                iconPixmap.height() - scaledBadge.height(),
                scaledBadge);
            painter.end();
        }

        return QIcon(iconPixmap);
    };

    QAction* copyCellAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_copy_cell.svg"), "复制单元格");
    QAction* copyRowAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_copy_row.svg"), "复制行");
    contextMenu.addSeparator();
    if (hasBatchSelection)
    {
        QAction* batchHintAction = contextMenu.addAction(
            blueTintedIcon(":/Icon/process_list.svg"),
            QStringLiteral("已选择 %1 个进程，支持批量动作").arg(contextActionTargets.size()));
        batchHintAction->setEnabled(false);
        contextMenu.addSeparator();
    }

    // 结束动作区：
    // - 取消“结束进程”二级菜单，改为一级动作；
    // - “结束进程”会按顺序执行 TerminateProcess + TerminateThread(全部线程)。
    QAction* terminateProcessAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_terminate.svg"),
        "结束进程");
    QAction* r0TerminateAction = contextMenu.addAction(
        buildR0ActionIcon(":/Icon/process_terminate.svg"),
        "R0结束进程");
    QAction* r0SuspendAction = contextMenu.addAction(
        buildR0ActionIcon(":/Icon/process_suspend.svg"),
        "R0挂起进程");
    QAction* refreshPplLevelAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_refresh.svg"),
        "手动刷新PPL保护级别");
    refreshPplLevelAction->setToolTip(QStringLiteral("查询 ProcessProtectionLevelInfo；结果只更新当前列表快照，不写入跨轮缓存。"));
    QMenu* r0PplLevelSubMenu = contextMenu.addMenu(
        buildR0ActionIcon(":/Icon/process_critical.svg"),
        "R0设置PPL层级");
    QAction* r0PplNoneAction = r0PplLevelSubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_critical.svg"),
        "关闭PPL保护 (0x00)");
    r0PplNoneAction->setData(0x00U);
    r0PplLevelSubMenu->addSeparator();
    // PPL 预设签名器列表：
    // - 与 PPLcontrol 的 signer 命名/数值对齐（PsProtectedSigner*）；
    // - protectionLevel = (Signer << 4) | Type，其中 Type 固定为 1（PPL）。
    struct PplSignerPreset
    {
        int signerValue;       // signerValue：PPL Signer 数值（1~7）。
        const char* signerName; // signerName：菜单展示名称。
        const char* meaningText; // meaningText：菜单展示释义。
        bool supportedByDriver; // supportedByDriver：当前驱动是否支持 signer 对应签名级别联动。
    };
    const PplSignerPreset presetList[] =
    {
        { 1, "Authenticode", "签名代码（Authenticode）", true },
        { 2, "CodeGen", "动态代码生成", true },
        { 3, "Antimalware", "反恶意软件", true },
        { 4, "Lsa", "本地安全机构", true },
        { 5, "Windows", "Windows 组件", true },
        { 6, "WinTcb", "可信计算基础（最高）", true },
        { 7, "WinSystem", "系统 signer（当前驱动未启用）", false }
    };
    for (const PplSignerPreset& presetEntry : presetList)
    {
        const unsigned int protectionLevel = (static_cast<unsigned int>(presetEntry.signerValue) << 4U) | 0x01U;
        const QString protectionLevelHexText = QStringLiteral("0x%1")
            .arg(protectionLevel, 2, 16, QChar('0'))
            .toUpper();
        QAction* presetAction = r0PplLevelSubMenu->addAction(
            buildR0ActionIcon(":/Icon/process_critical.svg"),
            QStringLiteral("%1 (%2) → %3 [%4]")
            .arg(QString::fromLatin1(presetEntry.signerName))
            .arg(presetEntry.signerValue)
            .arg(QString::fromUtf8(presetEntry.meaningText))
            .arg(protectionLevelHexText));
        presetAction->setData(protectionLevel);
        if (!presetEntry.supportedByDriver)
        {
            presetAction->setEnabled(false);
            presetAction->setToolTip(QStringLiteral("该 Signer 在当前驱动下暂无签名级别联动映射。"));
        }
    }
    QMenu* r0VisibilitySubMenu = contextMenu.addMenu(
        buildR0ActionIcon(":/Icon/process_details.svg"),
        QStringLiteral("R0进程隐藏(可恢复)"));
    QAction* r0HideProcessAction = r0VisibilitySubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_suspend.svg"),
        QStringLiteral("隐藏选中进程"));
    QAction* r0UnhideProcessAction = r0VisibilitySubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_resume.svg"),
        QStringLiteral("取消隐藏选中进程"));
    QAction* r0ClearHiddenProcessAction = r0VisibilitySubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_refresh.svg"),
        QStringLiteral("清空全部隐藏标记"));
    r0HideProcessAction->setToolTip(QStringLiteral("只更新 Ksword 驱动内可恢复隐藏表，不执行 DKOM 断链。"));
    r0UnhideProcessAction->setToolTip(QStringLiteral("从 Ksword 驱动隐藏表移除选中 PID。"));
    r0ClearHiddenProcessAction->setToolTip(QStringLiteral("清空驱动内所有隐藏 PID 标记。"));
    QMenu* r0DangerSubMenu = contextMenu.addMenu(
        buildR0ActionIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("R0危险进程标志/DKOM"));
    QAction* r0EnableBreakAction = r0DangerSubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_critical.svg"),
        QStringLiteral("启用 BreakOnTermination"));
    QAction* r0DisableBreakAction = r0DangerSubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("关闭 BreakOnTermination"));
    QAction* r0DisableApcAction = r0DangerSubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_suspend.svg"),
        QStringLiteral("禁止APC插入(现有线程)"));
    r0DangerSubMenu->addSeparator();
    QAction* r0DkomCidRemoveAction = r0DangerSubMenu->addAction(
        buildR0ActionIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("DKOM从PspCidTable删除"));
    r0EnableBreakAction->setToolTip(QStringLiteral("调用 ZwSetInformationProcess(ProcessBreakOnTermination=1)。"));
    r0DisableBreakAction->setToolTip(QStringLiteral("调用 ZwSetInformationProcess(ProcessBreakOnTermination=0)。"));
    r0DisableApcAction->setToolTip(QStringLiteral("清除目标进程现有线程 ETHREAD ApcQueueable 位；新建线程不自动继承。"));
    r0DkomCidRemoveAction->setToolTip(QStringLiteral("从 PspCidTable 清零目标 EPROCESS 的 CID 表项；高风险且不可通过本菜单恢复。"));
    contextMenu.addSeparator();

    QAction* suspendAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_suspend.svg"), "挂起进程");
    QAction* resumeAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_resume.svg"), "恢复进程");
    QAction* enableEfficiencyAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_resume.svg"),
        "开启效率模式（绿叶）");
    QAction* disableEfficiencyAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_suspend.svg"),
        "关闭效率模式");
    if (contextProcessRecord != nullptr && contextProcessRecord->efficiencyModeSupported)
    {
        enableEfficiencyAction->setEnabled(!contextProcessRecord->efficiencyModeEnabled);
        disableEfficiencyAction->setEnabled(contextProcessRecord->efficiencyModeEnabled);
    }
    QAction* setCriticalAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_critical.svg"), "设为关键进程");
    QAction* clearCriticalAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_uncritical.svg"), "取消关键进程");
    QAction* openFolderAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_open_folder.svg"), "打开所在目录");
    QAction* openMemoryAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_details.svg"),
        "跳转到内存操作（可在此处转储）");

    // 优先级二级菜单。
    QMenu* prioritySubMenu = contextMenu.addMenu(blueTintedIcon(":/Icon/process_priority.svg"), "设置进程优先级");
    QAction* idlePriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Idle");
    QAction* belowNormalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Below Normal");
    QAction* normalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Normal");
    QAction* aboveNormalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Above Normal");
    QAction* highPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "High");
    QAction* realtimePriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Realtime");
    idlePriority->setData(0);
    belowNormalPriority->setData(1);
    normalPriority->setData(2);
    aboveNormalPriority->setData(3);
    highPriority->setData(4);
    realtimePriority->setData(5);

    QAction* detailsAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_details.svg"), "进程详细信息");
    detailsAction->setEnabled(!hasBatchSelection);

    m_contextMenuVisible = true;
    QAction* selectedAction = contextMenu.exec(m_processTable->viewport()->mapToGlobal(localPosition));
    m_contextMenuVisible = false;
    {
        if (selectedAction == nullptr)
        {
            clearContextActionBinding();
            return;
        }

        {
            kLogEvent logEvent;
            info << logEvent
                << "[ProcessDock] 右键菜单执行动作: " << selectedAction->text().toStdString()
                << eol;
        }

        if (selectedAction == copyCellAction) { copyCurrentCell(); }
        else if (selectedAction == copyRowAction) { copyCurrentRow(); }
        else if (selectedAction == terminateProcessAction) { executeTerminateProcessAction(); }
        else if (selectedAction == r0TerminateAction) { executeR0TerminateProcessAction(); }
        else if (selectedAction == r0SuspendAction) { executeR0SuspendProcessAction(); }
        else if (selectedAction == r0HideProcessAction) { executeR0SetProcessHiddenAction(true); }
        else if (selectedAction == r0UnhideProcessAction) { executeR0SetProcessHiddenAction(false); }
        else if (selectedAction == r0ClearHiddenProcessAction) { executeR0ClearProcessHiddenAction(); }
        else if (selectedAction == r0EnableBreakAction) { executeR0SetBreakOnTerminationAction(true); }
        else if (selectedAction == r0DisableBreakAction) { executeR0SetBreakOnTerminationAction(false); }
        else if (selectedAction == r0DisableApcAction) { executeR0DisableApcInsertionAction(); }
        else if (selectedAction == r0DkomCidRemoveAction) { executeR0DkomRemoveFromCidTableAction(); }
        else if (selectedAction == refreshPplLevelAction) { executeRefreshPplProtectionLevelAction(); }
        else if (selectedAction == suspendAction) { executeSuspendAction(); }
        else if (selectedAction == resumeAction) { executeResumeAction(); }
        else if (selectedAction == enableEfficiencyAction) { executeSetEfficiencyModeAction(true); }
        else if (selectedAction == disableEfficiencyAction) { executeSetEfficiencyModeAction(false); }
        else if (selectedAction == setCriticalAction) { executeSetCriticalAction(true); }
        else if (selectedAction == clearCriticalAction) { executeSetCriticalAction(false); }
        else if (selectedAction == openFolderAction) { executeOpenFolderAction(); }
        else if (selectedAction == openMemoryAction) { executeOpenMemoryOperationAction(); }
        else if (selectedAction == detailsAction) { openProcessDetailsPlaceholder(); }
        else if (selectedAction->parent() == prioritySubMenu)
        {
            executeSetPriorityAction(selectedAction->data().toInt());
        }
        else if (selectedAction->parent() == r0PplLevelSubMenu)
        {
            const unsigned int levelValue = selectedAction->data().toUInt();
            if (levelValue > 0xFFU)
            {
                kLogEvent actionEvent;
                warn << actionEvent
                    << "[ProcessDock] R0 PPL 层级无效: levelValue="
                    << levelValue
                    << eol;
                showActionResultMessage(
                    QStringLiteral("R0设置PPL层级"),
                    false,
                    std::string("invalid PPL level value"),
                    actionEvent);
                clearContextActionBinding();
                return;
            }

            executeR0SetPplProtectionAction(
                static_cast<std::uint8_t>(levelValue),
                selectedAction->text());
        }
    }
    clearContextActionBinding();
}

void ProcessDock::showHeaderContextMenu(const QPoint& localPosition)
{
    Q_UNUSED(localPosition);

    // 每列一个勾选动作，允许用户动态显示/隐藏。
    QMenu columnMenu(this);
    for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
    {
        QAction* toggleAction = columnMenu.addAction(ProcessTableHeaders.at(columnIndex));
        toggleAction->setCheckable(true);
        toggleAction->setChecked(!m_processTable->isColumnHidden(columnIndex));
        toggleAction->setData(columnIndex);
    }

    QAction* selectedAction = columnMenu.exec(QCursor::pos());
    if (selectedAction == nullptr)
    {
        return;
    }

    const int columnIndex = selectedAction->data().toInt();
    const bool shouldShow = selectedAction->isChecked();
    if (shouldShow && m_autoHideUnavailableR0Columns)
    {
        const TableColumn selectedColumn = static_cast<TableColumn>(columnIndex);
        if (selectedColumn == TableColumn::Protection ||
            selectedColumn == TableColumn::Ppl ||
            selectedColumn == TableColumn::HandleTable ||
            selectedColumn == TableColumn::SectionObject ||
            selectedColumn == TableColumn::R0Status)
        {
            kLogEvent logEvent;
            info << logEvent
                << "[ProcessDock] 忽略 R0-only 列手动显示请求：当前所有可见行 R0 扩展均为 Unavailable, column="
                << columnIndex
                << eol;
            return;
        }
    }

    m_processTable->setColumnHidden(columnIndex, !shouldShow);
    applyAdaptiveColumnWidths();

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 列显示状态变更, column=" << columnIndex
        << ", header=" << ProcessTableHeaders.value(columnIndex).toStdString()
        << ", visible=" << (shouldShow ? "true" : "false")
        << eol;
}

void ProcessDock::copyCurrentCell()
{
    if (m_processTable == nullptr)
    {
        return;
    }

    const int currentColumn = m_processTable->currentColumn();
    if (currentColumn < 0)
    {
        return;
    }

    QList<QTreeWidgetItem*> selectedItemList = m_processTable->selectedItems();
    if (selectedItemList.isEmpty() && m_processTable->currentItem() != nullptr)
    {
        selectedItemList.push_back(m_processTable->currentItem());
    }

    QStringList cellTexts;
    cellTexts.reserve(selectedItemList.size());
    std::unordered_set<std::string> visitedIdentitySet;
    for (QTreeWidgetItem* itemPointer : selectedItemList)
    {
        if (itemPointer == nullptr)
        {
            continue;
        }

        const std::string identityKey = itemPointer->data(0, Qt::UserRole).toString().toStdString();
        if (!identityKey.empty() && visitedIdentitySet.find(identityKey) != visitedIdentitySet.end())
        {
            continue;
        }
        if (!identityKey.empty())
        {
            visitedIdentitySet.insert(identityKey);
        }
        cellTexts.push_back(itemPointer->text(currentColumn));
    }

    QApplication::clipboard()->setText(cellTexts.join("\n"));

    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] 复制单元格, column=" << currentColumn
        << ", rowCount=" << cellTexts.size()
        << ", text=" << cellTexts.join("\\n").toStdString()
        << eol;
}

void ProcessDock::copyCurrentRow()
{
    if (m_processTable == nullptr)
    {
        return;
    }

    QList<QTreeWidgetItem*> selectedItemList = m_processTable->selectedItems();
    if (selectedItemList.isEmpty() && m_processTable->currentItem() != nullptr)
    {
        selectedItemList.push_back(m_processTable->currentItem());
    }

    QStringList rowTexts;
    rowTexts.reserve(selectedItemList.size());
    std::unordered_set<std::string> visitedIdentitySet;
    for (QTreeWidgetItem* itemPointer : selectedItemList)
    {
        if (itemPointer == nullptr)
        {
            continue;
        }

        const std::string identityKey = itemPointer->data(0, Qt::UserRole).toString().toStdString();
        if (!identityKey.empty() && visitedIdentitySet.find(identityKey) != visitedIdentitySet.end())
        {
            continue;
        }
        if (!identityKey.empty())
        {
            visitedIdentitySet.insert(identityKey);
        }

        QStringList rowFields;
        rowFields.reserve(static_cast<int>(TableColumn::Count));
        for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
        {
            rowFields.push_back(itemPointer->text(columnIndex));
        }
        rowTexts.push_back(rowFields.join("\t"));
    }
    QApplication::clipboard()->setText(rowTexts.join("\n"));

    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] 复制整行, rowCount=" << rowTexts.size()
        << ", text=" << rowTexts.join("\\n").toStdString()
        << eol;
}

void ProcessDock::bindContextActionToItem(QTreeWidgetItem* clickedItem)
{
    clearContextActionBinding();
    if (m_processTable == nullptr)
    {
        return;
    }

    // 右键动作绑定：
    // - 优先绑定当前所有选中行，让菜单动作天然支持 Ctrl 复选批量操作；
    // - 如果当前没有选中行，则退化为右键点中的单行。
    QList<QTreeWidgetItem*> selectedItemList = m_processTable->selectedItems();
    if (selectedItemList.isEmpty() && clickedItem != nullptr)
    {
        selectedItemList.push_back(clickedItem);
    }

    std::unordered_set<std::string> visitedIdentitySet;
    for (QTreeWidgetItem* itemPointer : selectedItemList)
    {
        if (itemPointer == nullptr)
        {
            continue;
        }

        const std::string identityKey = itemPointer->data(0, Qt::UserRole).toString().toStdString();
        if (identityKey.empty() || visitedIdentitySet.find(identityKey) != visitedIdentitySet.end())
        {
            continue;
        }
        visitedIdentitySet.insert(identityKey);

        ProcessActionTarget actionTarget{};
        actionTarget.identityKey = identityKey;

        const auto cacheIt = m_cacheByIdentity.find(identityKey);
        if (cacheIt != m_cacheByIdentity.end())
        {
            actionTarget.record = cacheIt->second.record;
            m_contextActionRecords.push_back(std::move(actionTarget));
            continue;
        }

        // 若刷新刚好重建导致 identity 缓存缺失，尝试用当前行 PID 兜底。
        bool pidParseOk = false;
        const std::uint32_t pidValue = itemPointer->text(toColumnIndex(TableColumn::Pid)).toUInt(&pidParseOk);
        if (pidParseOk)
        {
            actionTarget.record = {};
            actionTarget.record.pid = pidValue;
            actionTarget.record.processName = itemPointer->text(toColumnIndex(TableColumn::Name)).toStdString();
            m_contextActionRecords.push_back(std::move(actionTarget));
        }
    }

    if (m_contextActionRecords.empty())
    {
        return;
    }

    m_contextActionIdentityKey = m_contextActionRecords.front().identityKey;
    m_contextActionRecord = m_contextActionRecords.front().record;
    m_hasContextActionRecord = true;
}

void ProcessDock::clearContextActionBinding()
{
    m_contextActionIdentityKey.clear();
    m_contextActionRecords.clear();
    m_hasContextActionRecord = false;
    m_contextMenuVisible = false;
}

std::string ProcessDock::selectedIdentityKey() const
{
    if (!m_contextActionIdentityKey.empty())
    {
        return m_contextActionIdentityKey;
    }

    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem == nullptr)
    {
        return std::string();
    }
    return currentItem->data(0, Qt::UserRole).toString().toStdString();
}

ks::process::ProcessRecord* ProcessDock::selectedRecord()
{
    const std::string identityKey = selectedIdentityKey();
    if (identityKey.empty())
    {
        return nullptr;
    }

    auto cacheIt = m_cacheByIdentity.find(identityKey);
    if (cacheIt == m_cacheByIdentity.end())
    {
        if (m_hasContextActionRecord)
        {
            return &m_contextActionRecord;
        }
        return nullptr;
    }
    return &cacheIt->second.record;
}

std::vector<ProcessDock::ProcessActionTarget> ProcessDock::selectedActionTargets() const
{
    // 右键菜单弹出期间优先使用冻结的动作绑定，避免刷新或选择变化影响执行对象。
    if (!m_contextActionRecords.empty())
    {
        return m_contextActionRecords;
    }

    std::vector<ProcessActionTarget> actionTargets;
    std::unordered_set<std::string> visitedIdentitySet;

    // collectItemTarget 作用：
    // - 从表格行解析 identityKey；
    // - 优先从缓存取完整记录，缓存缺失时用行文本中的 PID 构造兜底记录。
    const auto collectItemTarget =
        [this, &actionTargets, &visitedIdentitySet](QTreeWidgetItem* itemPointer)
        {
            if (itemPointer == nullptr)
            {
                return;
            }

            const std::string identityKey = itemPointer->data(0, Qt::UserRole).toString().toStdString();
            if (identityKey.empty() || visitedIdentitySet.find(identityKey) != visitedIdentitySet.end())
            {
                return;
            }
            visitedIdentitySet.insert(identityKey);

            ProcessActionTarget actionTarget{};
            actionTarget.identityKey = identityKey;

            const auto cacheIt = m_cacheByIdentity.find(identityKey);
            if (cacheIt != m_cacheByIdentity.end())
            {
                actionTarget.record = cacheIt->second.record;
                actionTargets.push_back(std::move(actionTarget));
                return;
            }

            bool pidParseOk = false;
            const std::uint32_t pidValue = itemPointer->text(toColumnIndex(TableColumn::Pid)).toUInt(&pidParseOk);
            if (pidParseOk)
            {
                actionTarget.record = {};
                actionTarget.record.pid = pidValue;
                actionTarget.record.processName = itemPointer->text(toColumnIndex(TableColumn::Name)).toStdString();
                actionTargets.push_back(std::move(actionTarget));
            }
        };

    if (m_processTable != nullptr)
    {
        const QList<QTreeWidgetItem*> selectedItemList = m_processTable->selectedItems();
        for (QTreeWidgetItem* itemPointer : selectedItemList)
        {
            collectItemTarget(itemPointer);
        }
        if (actionTargets.empty())
        {
            collectItemTarget(m_processTable->currentItem());
        }
    }

    return actionTargets;
}

void ProcessDock::syncTrackedSelectionFromTable()
{
    if (m_processTable == nullptr || m_contextMenuVisible)
    {
        return;
    }

    std::vector<std::string> selectedIdentityKeys;
    std::unordered_set<std::string> visitedIdentitySet;
    const QList<QTreeWidgetItem*> selectedItemList = m_processTable->selectedItems();
    selectedIdentityKeys.reserve(static_cast<std::size_t>(selectedItemList.size()));

    for (QTreeWidgetItem* itemPointer : selectedItemList)
    {
        if (itemPointer == nullptr)
        {
            continue;
        }

        const std::string identityKey = itemPointer->data(0, Qt::UserRole).toString().toStdString();
        if (identityKey.empty() || visitedIdentitySet.find(identityKey) != visitedIdentitySet.end())
        {
            continue;
        }
        visitedIdentitySet.insert(identityKey);
        selectedIdentityKeys.push_back(identityKey);
    }

    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem != nullptr)
    {
        const std::string currentIdentityKey = currentItem->data(0, Qt::UserRole).toString().toStdString();
        if (!currentIdentityKey.empty())
        {
            m_trackedSelectedIdentityKey = currentIdentityKey;
            if (visitedIdentitySet.find(currentIdentityKey) == visitedIdentitySet.end())
            {
                selectedIdentityKeys.push_back(currentIdentityKey);
            }
        }
    }
    else
    {
        m_trackedSelectedIdentityKey.clear();
    }

    m_trackedSelectedIdentityKeys = std::move(selectedIdentityKeys);
}

void ProcessDock::dispatchProcessActionTargetsInParallel(
    const QString& actionTitle,
    const std::vector<ProcessActionTarget>& actionTargets,
    const std::function<bool(const ProcessActionTarget&, std::string*)>& actionInvoker,
    const bool refreshWhenAnySucceeded)
{
    // 参数检查：没有目标或没有执行体时直接记录并返回。
    if (actionTargets.empty() || !actionInvoker)
    {
        kLogEvent logEvent;
        warn << logEvent
            << "[ProcessDock] 批量动作被忽略：目标为空或执行体为空, title="
            << actionTitle.toStdString()
            << eol;
        return;
    }

    // 单目标沿用同步执行路径，避免简单动作产生不必要线程切换。
    if (actionTargets.size() == 1U)
    {
        std::string detailText;
        const bool actionOk = actionInvoker(actionTargets.front(), &detailText);
        kLogEvent actionEvent;
        (actionOk ? info : err) << actionEvent
            << "[ProcessDock] 单进程动作完成, title=" << actionTitle.toStdString()
            << ", pid=" << actionTargets.front().record.pid
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << (detailText.empty() ? "无附加信息" : detailText)
            << eol;
        showActionResultMessage(actionTitle, actionOk, detailText, actionEvent);
        if (actionOk && refreshWhenAnySucceeded)
        {
            requestAsyncRefresh(true);
        }
        clearContextActionBinding();
        return;
    }

    // 批量动作：每个 PID 独立线程执行，避免一个目标卡住后阻塞后续目标。
    QPointer<ProcessDock> guard(this);
    const QString localActionTitle = actionTitle;
    const std::size_t targetCount = actionTargets.size();
    const auto finishedCounter = std::make_shared<std::atomic_size_t>(0U);
    const auto anySucceeded = std::make_shared<std::atomic_bool>(false);
    kLogEvent startEvent;
    info << startEvent
        << "[ProcessDock] 启动批量动作, title=" << localActionTitle.toStdString()
        << ", targetCount=" << targetCount
        << eol;

    for (const ProcessActionTarget& actionTarget : actionTargets)
    {
        std::thread([
            guard,
            localActionTitle,
            actionTarget,
            actionInvoker,
            refreshWhenAnySucceeded,
            targetCount,
            finishedCounter,
            anySucceeded]()
        {
            std::string detailText;
            const bool actionOk = actionInvoker(actionTarget, &detailText);
            const std::string normalizedDetailText = detailText.empty() ? "无附加信息" : detailText;
            if (actionOk)
            {
                anySucceeded->store(true, std::memory_order_relaxed);
            }
            const std::size_t finishedCount =
                finishedCounter->fetch_add(1U, std::memory_order_acq_rel) + 1U;
            const bool allTargetsFinished = (finishedCount >= targetCount);

            kLogEvent threadEvent;
            (actionOk ? info : err) << threadEvent
                << "[ProcessDock] 批量动作单目标完成, title=" << localActionTitle.toStdString()
                << ", pid=" << actionTarget.record.pid
                << ", identity=" << actionTarget.identityKey
                << ", ok=" << (actionOk ? "true" : "false")
                << ", detail=" << normalizedDetailText
                << eol;

            if (guard == nullptr)
            {
                return;
            }

            QMetaObject::invokeMethod(guard, [guard, localActionTitle, actionOk, detailText, refreshWhenAnySucceeded]()
            {
                if (guard == nullptr)
                {
                    return;
                }

                kLogEvent uiEvent;
                guard->showActionResultMessage(localActionTitle, actionOk, detailText, uiEvent);
            }, Qt::QueuedConnection);

            if (!allTargetsFinished || guard == nullptr)
            {
                return;
            }

            QMetaObject::invokeMethod(guard, [guard, localActionTitle, refreshWhenAnySucceeded, anySucceeded]()
            {
                if (guard == nullptr)
                {
                    return;
                }

                if (refreshWhenAnySucceeded && anySucceeded->load(std::memory_order_relaxed))
                {
                    kLogEvent refreshEvent;
                    info << refreshEvent
                        << "[ProcessDock] 批量动作全部完成，触发一次刷新, title="
                        << localActionTitle.toStdString()
                        << eol;
                    guard->requestAsyncRefresh(true);
                }
            }, Qt::QueuedConnection);
        }).detach();
    }
}

QString ProcessDock::formatColumnText(const ks::process::ProcessRecord& processRecord, const TableColumn column, const int depth) const
{
    switch (column)
    {
    case TableColumn::Name:
        return QString::fromStdString(buildRulerPrefix(depth) + processRecord.processName);
    case TableColumn::Pid:
        return QString::number(processRecord.pid);
    case TableColumn::Cpu:
        // CPU 改为两位小数，避免低占用进程全部显示 0.0 的视觉误差。
        return QString::number(processRecord.cpuPercent, 'f', 2) + "%";
    case TableColumn::Ram:
        return QStringLiteral("使用 %1 MB / 申请 %2 MB")
            .arg(processRecord.workingSetMB, 0, 'f', 1)
            .arg(processRecord.ramMB, 0, 'f', 1);
    case TableColumn::Disk:
        return QString::number(processRecord.diskMBps, 'f', 2) + " MB/s";
    case TableColumn::Gpu:
        return QString::number(processRecord.gpuPercent, 'f', 1) + "%";
    case TableColumn::Net:
        return QString::number(processRecord.netKBps, 'f', 2) + " KB/s";
    case TableColumn::Signature:
        // 显示“厂家 + 可信状态”文本，未填充时显示 Unknown。
        return QString::fromStdString(processRecord.signatureState.empty() ? "Unknown" : processRecord.signatureState);
    case TableColumn::Path:
        return QString::fromStdString(processRecord.imagePath.empty() ? "-" : processRecord.imagePath);
    case TableColumn::ParentPid:
        return QString::number(processRecord.parentPid);
    case TableColumn::CommandLine:
        return QString::fromStdString(processRecord.commandLine.empty() ? "-" : processRecord.commandLine);
    case TableColumn::User:
        return QString::fromStdString(processRecord.userName.empty() ? "-" : processRecord.userName);
    case TableColumn::StartTime:
        return QString::fromStdString(processRecord.startTimeText);
    case TableColumn::IsAdmin:
        // 用方块 + 文本表示管理员状态（颜色在重建表格时设置）。
        return processRecord.isAdmin ? "■ 是" : "■ 否";
    case TableColumn::PplLevel:
        // PPL 保护级别枚举只由用户手动刷新，不从缓存继承。
        if (!processRecord.protectionLevelKnown)
        {
            return QStringLiteral("未手动刷新");
        }
        return QString::fromStdString(processRecord.protectionLevelText.empty()
            ? "Unknown"
            : processRecord.protectionLevelText);
    case TableColumn::Protection:
        if ((processRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable (%1)").arg(processFieldSourceText(processRecord.r0ProtectionSource));
        }
        return QStringLiteral("%1 (%2)")
            .arg(byteHexText(processRecord.r0Protection))
            .arg(processFieldSourceText(processRecord.r0ProtectionSource));
    case TableColumn::Ppl:
        if ((processRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return (processRecord.r0Protection == 0U)
            ? QStringLiteral("None (0x00)")
            : QStringLiteral("PPL %1").arg(byteHexText(processRecord.r0Protection));
    case TableColumn::HandleCount:
        return QString::number(processRecord.handleCount);
    case TableColumn::HandleTable:
        return pointerAvailabilityText(
            (processRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_AVAILABLE) != 0U,
            processRecord.r0ObjectTableAddress,
            processRecord.r0ObjectTableSource);
    case TableColumn::SectionObject:
        return pointerAvailabilityText(
            (processRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_AVAILABLE) != 0U,
            processRecord.r0SectionObjectAddress,
            processRecord.r0SectionObjectSource);
    case TableColumn::R0Status:
        return processR0StatusText(processRecord.r0Status);
    default:
        return QString();
    }
}

QIcon ProcessDock::resolveProcessIcon(const ks::process::ProcessRecord& processRecord)
{
    const QString processNameText = QString::fromStdString(processRecord.processName).trimmed();
    // 仅使用后台缓存中的 imagePath：
    // - 禁止在 UI 线程里按 PID 再查路径，避免刷新阶段出现卡顿。
    QString pathText = QString::fromStdString(processRecord.imagePath);
    pathText = pathText.trimmed();

    // 历史快照表格没有实时 PID 查询能力：
    // - 采样时已维护“进程名+路径 -> 图标”的稳定映射；
    // - 查看旧时间点时优先命中该映射，避免所有行退化成同一个占位图标。
    const QString activityIconKey = processNameText + QStringLiteral("|") + pathText;
    const auto activityIconIt = m_activityIconCacheByProcessKey.find(activityIconKey);
    if (activityIconIt != m_activityIconCacheByProcessKey.end())
    {
        return activityIconIt.value();
    }

    if (pathText.isEmpty() || pathText.startsWith('[') || pathText == QStringLiteral("历史快照"))
    {
        return QIcon(":/Icon/process_main.svg");
    }

    auto iconIt = m_iconCacheByPath.find(pathText);
    if (iconIt != m_iconCacheByPath.end())
    {
        return iconIt.value();
    }

    // 先尝试直接从 EXE 路径构造图标（期望拿到真实程序图标）。
    QIcon processIcon(pathText);
    if (processIcon.isNull())
    {
        QFileIconProvider iconProvider;
        processIcon = iconProvider.icon(QFileInfo(pathText));
    }
    if (processIcon.isNull())
    {
        processIcon = QIcon(":/Icon/process_main.svg");
    }
    m_iconCacheByPath.insert(pathText, processIcon);
    if (!activityIconKey.trimmed().isEmpty())
    {
        m_activityIconCacheByProcessKey.insert(activityIconKey, processIcon);
    }
    return processIcon;
}

QIcon ProcessDock::blueTintedIcon(const char* iconPath, const QSize& iconSize) const
{
    return tintedProcessTabIcon(iconPath, KswordTheme::PrimaryBlueColor, iconSize);
}

QIcon ProcessDock::tintedProcessTabIcon(
    const char* iconPath,
    const QColor& tintColor,
    const QSize& iconSize) const
{
    // SVG 着色流程：先按原路径渲染透明图，再用 SourceIn 覆盖指定颜色。
    QSvgRenderer renderer(QString::fromUtf8(iconPath));
    if (!renderer.isValid())
    {
        return QIcon(QString::fromUtf8(iconPath));
    }

    QPixmap iconPixmap(iconSize);
    iconPixmap.fill(Qt::transparent);

    QPainter painter(&iconPixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(iconPixmap.rect(), tintColor);
    painter.end();
    return QIcon(iconPixmap);
}

void ProcessDock::refreshSideTabIconContrast()
{
    if (m_sideTabWidget == nullptr)
    {
        return;
    }

    // 左侧 Tab 选中态是深蓝背景，当前页图标改为白色以避免融入背景。
    const int currentIndex = m_sideTabWidget->currentIndex();
    const QColor selectedIconColor(255, 255, 255);
    const QIcon processIcon = currentIndex == m_sideTabWidget->indexOf(m_processListPage)
        ? tintedProcessTabIcon(IconProcessMain, selectedIconColor)
        : blueTintedIcon(IconProcessMain);
    const QIcon threadIcon = currentIndex == m_sideTabWidget->indexOf(m_threadPage)
        ? tintedProcessTabIcon(IconThreadTab, selectedIconColor)
        : blueTintedIcon(IconThreadTab);
    const QIcon createIcon = currentIndex == m_sideTabWidget->indexOf(m_createProcessPage)
        ? tintedProcessTabIcon(IconStart, selectedIconColor)
        : blueTintedIcon(IconStart);

    if (m_processListPage != nullptr)
    {
        m_sideTabWidget->setTabIcon(m_sideTabWidget->indexOf(m_processListPage), processIcon);
    }
    if (m_threadPage != nullptr)
    {
        m_sideTabWidget->setTabIcon(m_sideTabWidget->indexOf(m_threadPage), threadIcon);
    }
    if (m_createProcessPage != nullptr)
    {
        m_sideTabWidget->setTabIcon(m_sideTabWidget->indexOf(m_createProcessPage), createIcon);
    }
}

void ProcessDock::showActionResultMessage(
    const QString& title,
    const bool actionOk,
    const std::string& detailText,
    const kLogEvent& actionEvent)
{
    // 统一动作结果日志：按照规范不再弹窗，避免频繁打断用户流程。
    const std::string normalizedDetailText = detailText.empty() ? "无附加信息" : detailText;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] 动作结果, title=" << title.toStdString()
        << ", actionOk=" << (actionOk ? "true" : "false")
        << ", detail=" << normalizedDetailText
        << eol;
}

std::string ProcessDock::buildRulerPrefix(const int depth)
{
    if (depth <= 0)
    {
        return std::string();
    }

    std::string prefixText;
    for (int index = 0; index < depth; ++index)
    {
        prefixText += (index + 1 == depth) ? "└─ " : "│  ";
    }
    return prefixText;
}

int ProcessDock::toColumnIndex(const TableColumn column)
{
    return static_cast<int>(column);
}

void ProcessDock::syncEditValueFromBitmaskChecks(
    QLineEdit* const valueEdit,
    const std::vector<QCheckBox*>* const checkBoxList)
{
    // 参数合法性检查：任一为空直接返回，避免空指针访问。
    if (valueEdit == nullptr || checkBoxList == nullptr)
    {
        return;
    }

    // 先计算“所有已知位掩码 + 已勾选位掩码”，
    // 以便在回写时保留用户手工输入但列表中未覆盖的未知位。
    std::uint32_t knownMask = 0;
    std::uint32_t checkedMask = 0;
    for (QCheckBox* checkBox : *checkBoxList)
    {
        if (checkBox == nullptr)
        {
            continue;
        }
        bool convertOk = false;
        const std::uint32_t flagValue = static_cast<std::uint32_t>(
            checkBox->property("flagValue").toULongLong(&convertOk));
        if (!convertOk)
        {
            continue;
        }

        knownMask |= flagValue;
        if (checkBox->isChecked())
        {
            checkedMask |= flagValue;
        }
    }

    // 保留未知位：避免勾选一个已知位时误清空手工填入的其他位。
    bool parseOk = false;
    const std::uint32_t originalValue = parseUInt32WithDefault(valueEdit->text(), 0, &parseOk);
    const std::uint32_t unknownMask = parseOk ? (originalValue & ~knownMask) : 0;
    const std::uint32_t mergedValue = (checkedMask | unknownMask);

    const QString mergedText = QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(mergedValue), 8, 16, QChar('0'))
        .toUpper();
    if (valueEdit->text().compare(mergedText, Qt::CaseInsensitive) == 0)
    {
        return;
    }

    // 阻断 textChanged 信号，防止“回写文本 -> 再次反向同步”递归触发。
    const QSignalBlocker blocker(valueEdit);
    valueEdit->setText(mergedText);
}

void ProcessDock::syncBitmaskChecksFromEditValue(
    QLineEdit* const valueEdit,
    const std::vector<QCheckBox*>* const checkBoxList,
    const QString& fieldDisplayName)
{
    // 参数合法性检查：任一为空直接返回。
    if (valueEdit == nullptr || checkBoxList == nullptr)
    {
        return;
    }

    // 解析失败时仅跳过勾选同步，不主动覆写用户输入内容。
    bool parseOk = false;
    const std::uint32_t editValue = parseUInt32WithDefault(valueEdit->text(), 0, &parseOk);
    if (!parseOk)
    {
        Q_UNUSED(fieldDisplayName);
        return;
    }

    // 按输入值逐项更新勾选状态；使用 QSignalBlocker 防止触发 toggled 回调。
    for (QCheckBox* checkBox : *checkBoxList)
    {
        if (checkBox == nullptr)
        {
            continue;
        }

        bool convertOk = false;
        const std::uint32_t flagValue = static_cast<std::uint32_t>(
            checkBox->property("flagValue").toULongLong(&convertOk));
        if (!convertOk || flagValue == 0)
        {
            continue;
        }

        const bool shouldChecked = ((editValue & flagValue) == flagValue);
        if (checkBox->isChecked() == shouldChecked)
        {
            continue;
        }

        const QSignalBlocker blocker(checkBox);
        checkBox->setChecked(shouldChecked);
    }
}

void ProcessDock::bindBitmaskEditor(
    QLineEdit* const valueEdit,
    std::vector<QCheckBox*>* const checkBoxList,
    const QString& fieldDisplayName)
{
    // 参数校验：没有输入框或没有复选框列表则不绑定。
    if (valueEdit == nullptr || checkBoxList == nullptr)
    {
        return;
    }

    // 复选框 -> 文本框：每次勾选变化都回算位掩码。
    for (QCheckBox* checkBox : *checkBoxList)
    {
        if (checkBox == nullptr)
        {
            continue;
        }

        connect(checkBox, &QCheckBox::toggled, this, [this, valueEdit, checkBoxList, fieldDisplayName](bool) {
            syncEditValueFromBitmaskChecks(valueEdit, checkBoxList);

            kLogEvent logEvent;
            dbg << logEvent
                << "[ProcessDock] 位标志勾选变更, field="
                << fieldDisplayName.toStdString()
                << ", value="
                << valueEdit->text().toStdString()
                << eol;
            });
    }

    // 文本框 -> 复选框：支持用户手工输入十进制或 0x 十六进制。
    connect(valueEdit, &QLineEdit::textChanged, this, [this, valueEdit, checkBoxList, fieldDisplayName](const QString&) {
        syncBitmaskChecksFromEditValue(valueEdit, checkBoxList, fieldDisplayName);
        });

    // 初始同步：页面打开时让默认值和复选框状态一致。
    syncBitmaskChecksFromEditValue(valueEdit, checkBoxList, fieldDisplayName);
}

bool ProcessDock::parseUnsignedText(const QString& text, std::uint64_t& valueOut)
{
    QString normalizedText = text.trimmed();
    if (normalizedText.isEmpty())
    {
        valueOut = 0;
        return true;
    }

    int numberBase = 10;
    if (normalizedText.startsWith("0x", Qt::CaseInsensitive))
    {
        normalizedText = normalizedText.mid(2);
        numberBase = 16;
    }
    else if (normalizedText.endsWith(QStringLiteral("h"), Qt::CaseInsensitive))
    {
        normalizedText.chop(1);
        numberBase = 16;
    }

    bool parseOk = false;
    const std::uint64_t parsedValue = normalizedText.toULongLong(&parseOk, numberBase);
    if (!parseOk)
    {
        valueOut = 0;
        return false;
    }
    valueOut = parsedValue;
    return true;
}

std::uint32_t ProcessDock::parseUInt32WithDefault(
    const QString& text,
    const std::uint32_t defaultValue,
    bool* const parseOkOut)
{
    std::uint64_t parsedValue = 0;
    if (!parseUnsignedText(text, parsedValue))
    {
        if (parseOkOut != nullptr)
        {
            *parseOkOut = false;
        }
        return defaultValue;
    }
    if (parsedValue > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        if (parseOkOut != nullptr)
        {
            *parseOkOut = false;
        }
        return defaultValue;
    }
    if (parseOkOut != nullptr)
    {
        *parseOkOut = true;
    }
    return static_cast<std::uint32_t>(parsedValue);
}

std::uint64_t ProcessDock::parseUInt64WithDefault(
    const QString& text,
    const std::uint64_t defaultValue,
    bool* const parseOkOut)
{
    std::uint64_t parsedValue = 0;
    if (!parseUnsignedText(text, parsedValue))
    {
        if (parseOkOut != nullptr)
        {
            *parseOkOut = false;
        }
        return defaultValue;
    }
    if (parseOkOut != nullptr)
    {
        *parseOkOut = true;
    }
    return parsedValue;
}

void ProcessDock::appendCreateResultLine(const QString& lineText)
{
    if (m_createResultOutput == nullptr)
    {
        return;
    }
    const QString timeText = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_createResultOutput->append(QString("[%1] %2").arg(timeText, lineText));
}

void ProcessDock::browseCreateProcessApplicationPath()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        "选择可执行文件",
        m_applicationNameEdit != nullptr ? m_applicationNameEdit->text().trimmed() : QString(),
        "Executable (*.exe);;All Files (*.*)");
    if (filePath.isEmpty())
    {
        return;
    }

    if (m_applicationNameEdit != nullptr)
    {
        m_applicationNameEdit->setText(filePath);
    }
    if (m_useApplicationNameCheck != nullptr)
    {
        m_useApplicationNameCheck->setChecked(true);
    }
}

void ProcessDock::browseCreateProcessCurrentDirectory()
{
    const QString startPath = m_currentDirectoryEdit != nullptr
        ? m_currentDirectoryEdit->text().trimmed()
        : QString();
    const QString directoryPath = QFileDialog::getExistingDirectory(
        this,
        "选择工作目录",
        startPath);
    if (directoryPath.isEmpty())
    {
        return;
    }

    if (m_currentDirectoryEdit != nullptr)
    {
        m_currentDirectoryEdit->setText(directoryPath);
    }
    if (m_useCurrentDirectoryCheck != nullptr)
    {
        m_useCurrentDirectoryCheck->setChecked(true);
    }
}

void ProcessDock::resetCreateProcessForm()
{
    if (m_createMethodCombo != nullptr) m_createMethodCombo->setCurrentIndex(0);
    if (m_useApplicationNameCheck != nullptr) m_useApplicationNameCheck->setChecked(false);
    if (m_applicationNameEdit != nullptr) m_applicationNameEdit->clear();
    if (m_useCommandLineCheck != nullptr) m_useCommandLineCheck->setChecked(false);
    if (m_commandLineEdit != nullptr) m_commandLineEdit->clear();
    if (m_useCurrentDirectoryCheck != nullptr) m_useCurrentDirectoryCheck->setChecked(false);
    if (m_currentDirectoryEdit != nullptr) m_currentDirectoryEdit->clear();
    if (m_useEnvironmentCheck != nullptr) m_useEnvironmentCheck->setChecked(false);
    if (m_environmentUnicodeCheck != nullptr) m_environmentUnicodeCheck->setChecked(true);
    if (m_environmentEditor != nullptr) m_environmentEditor->clear();
    if (m_inheritHandleCheck != nullptr) m_inheritHandleCheck->setChecked(false);
    if (m_creationFlagsEdit != nullptr) m_creationFlagsEdit->setText("0x00000000");

    if (m_useProcessSecurityCheck != nullptr) m_useProcessSecurityCheck->setChecked(false);
    if (m_processSecurityLengthEdit != nullptr) m_processSecurityLengthEdit->setText("0");
    if (m_processSecurityDescriptorEdit != nullptr) m_processSecurityDescriptorEdit->setText("0");
    if (m_processSecurityInheritCheck != nullptr) m_processSecurityInheritCheck->setChecked(false);
    if (m_useThreadSecurityCheck != nullptr) m_useThreadSecurityCheck->setChecked(false);
    if (m_threadSecurityLengthEdit != nullptr) m_threadSecurityLengthEdit->setText("0");
    if (m_threadSecurityDescriptorEdit != nullptr) m_threadSecurityDescriptorEdit->setText("0");
    if (m_threadSecurityInheritCheck != nullptr) m_threadSecurityInheritCheck->setChecked(false);

    if (m_useStartupInfoCheck != nullptr) m_useStartupInfoCheck->setChecked(false);
    if (m_siCbEdit != nullptr) m_siCbEdit->setText("0");
    if (m_siReservedEdit != nullptr) m_siReservedEdit->clear();
    if (m_siDesktopEdit != nullptr) m_siDesktopEdit->clear();
    if (m_siTitleEdit != nullptr) m_siTitleEdit->clear();
    if (m_siXEdit != nullptr) m_siXEdit->setText("0");
    if (m_siYEdit != nullptr) m_siYEdit->setText("0");
    if (m_siXSizeEdit != nullptr) m_siXSizeEdit->setText("0");
    if (m_siYSizeEdit != nullptr) m_siYSizeEdit->setText("0");
    if (m_siXCountCharsEdit != nullptr) m_siXCountCharsEdit->setText("0");
    if (m_siYCountCharsEdit != nullptr) m_siYCountCharsEdit->setText("0");
    if (m_siFillAttributeEdit != nullptr) m_siFillAttributeEdit->setText("0x00000000");
    if (m_siFlagsEdit != nullptr) m_siFlagsEdit->setText("0x00000000");
    if (m_siShowWindowEdit != nullptr) m_siShowWindowEdit->setText("0");
    if (m_siCbReserved2Edit != nullptr) m_siCbReserved2Edit->setText("0");
    if (m_siReserved2PtrEdit != nullptr) m_siReserved2PtrEdit->setText("0");
    if (m_siStdInputEdit != nullptr) m_siStdInputEdit->setText("0");
    if (m_siStdOutputEdit != nullptr) m_siStdOutputEdit->setText("0");
    if (m_siStdErrorEdit != nullptr) m_siStdErrorEdit->setText("0");

    if (m_useProcessInfoCheck != nullptr) m_useProcessInfoCheck->setChecked(false);
    if (m_piProcessHandleEdit != nullptr) m_piProcessHandleEdit->setText("0");
    if (m_piThreadHandleEdit != nullptr) m_piThreadHandleEdit->setText("0");
    if (m_piPidEdit != nullptr) m_piPidEdit->setText("0");
    if (m_piTidEdit != nullptr) m_piTidEdit->setText("0");

    if (m_tokenSourcePidEdit != nullptr) m_tokenSourcePidEdit->setText("0");
    if (m_tokenDesiredAccessEdit != nullptr) m_tokenDesiredAccessEdit->setText("0x00000FAB");
    if (m_tokenDuplicatePrimaryCheck != nullptr) m_tokenDuplicatePrimaryCheck->setChecked(true);

    if (m_tokenPrivilegeTable != nullptr)
    {
        for (int row = 0; row < m_tokenPrivilegeTable->rowCount(); ++row)
        {
            QComboBox* actionCombo = qobject_cast<QComboBox*>(m_tokenPrivilegeTable->cellWidget(row, 1));
            if (actionCombo != nullptr)
            {
                actionCombo->setCurrentIndex(0);
            }
        }
    }

    if (m_createResultOutput != nullptr)
    {
        m_createResultOutput->clear();
    }
    appendCreateResultLine("已恢复创建进程表单默认值。");
}

ks::process::CreateProcessRequest ProcessDock::buildCreateProcessRequestFromUi(
    bool* const buildOk,
    QString* const errorTextOut) const
{
    ks::process::CreateProcessRequest request;
    if (buildOk != nullptr)
    {
        *buildOk = false;
    }

    const auto failBuild = [errorTextOut](const QString& textValue) {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = textValue;
        }
        };

    request.useApplicationName = (m_useApplicationNameCheck != nullptr && m_useApplicationNameCheck->isChecked());
    request.applicationName = (m_applicationNameEdit != nullptr) ? m_applicationNameEdit->text().trimmed().toStdString() : std::string();

    request.useCommandLine = (m_useCommandLineCheck != nullptr && m_useCommandLineCheck->isChecked());
    request.commandLine = (m_commandLineEdit != nullptr) ? m_commandLineEdit->text().trimmed().toStdString() : std::string();

    request.useCurrentDirectory = (m_useCurrentDirectoryCheck != nullptr && m_useCurrentDirectoryCheck->isChecked());
    request.currentDirectory = (m_currentDirectoryEdit != nullptr) ? m_currentDirectoryEdit->text().trimmed().toStdString() : std::string();

    request.useEnvironment = (m_useEnvironmentCheck != nullptr && m_useEnvironmentCheck->isChecked());
    request.environmentUnicode = (m_environmentUnicodeCheck != nullptr && m_environmentUnicodeCheck->isChecked());
    if (request.useEnvironment && m_environmentEditor != nullptr)
    {
        const QStringList envLines = m_environmentEditor->toPlainText().split('\n');
        for (const QString& lineText : envLines)
        {
            const QString trimmedText = lineText.trimmed();
            if (!trimmedText.isEmpty())
            {
                request.environmentEntries.push_back(trimmedText.toStdString());
            }
        }
    }

    request.inheritHandles = (m_inheritHandleCheck != nullptr && m_inheritHandleCheck->isChecked());

    bool parseOk = false;
    request.creationFlags = parseUInt32WithDefault(
        m_creationFlagsEdit != nullptr ? m_creationFlagsEdit->text() : QString(),
        0,
        &parseOk);
    if (!parseOk)
    {
        failBuild("dwCreationFlags 解析失败，请输入十进制或 0x 十六进制。");
        return request;
    }

    request.processAttributes.useValue = (m_useProcessSecurityCheck != nullptr && m_useProcessSecurityCheck->isChecked());
    if (request.processAttributes.useValue)
    {
        request.processAttributes.nLength = parseUInt32WithDefault(
            m_processSecurityLengthEdit != nullptr ? m_processSecurityLengthEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Process SECURITY_ATTRIBUTES.nLength 解析失败。");
            return request;
        }
        request.processAttributes.securityDescriptor = parseUInt64WithDefault(
            m_processSecurityDescriptorEdit != nullptr ? m_processSecurityDescriptorEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Process SECURITY_ATTRIBUTES.lpSecurityDescriptor 解析失败。");
            return request;
        }
        request.processAttributes.inheritHandle = (m_processSecurityInheritCheck != nullptr && m_processSecurityInheritCheck->isChecked());
    }

    request.threadAttributes.useValue = (m_useThreadSecurityCheck != nullptr && m_useThreadSecurityCheck->isChecked());
    if (request.threadAttributes.useValue)
    {
        request.threadAttributes.nLength = parseUInt32WithDefault(
            m_threadSecurityLengthEdit != nullptr ? m_threadSecurityLengthEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Thread SECURITY_ATTRIBUTES.nLength 解析失败。");
            return request;
        }
        request.threadAttributes.securityDescriptor = parseUInt64WithDefault(
            m_threadSecurityDescriptorEdit != nullptr ? m_threadSecurityDescriptorEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Thread SECURITY_ATTRIBUTES.lpSecurityDescriptor 解析失败。");
            return request;
        }
        request.threadAttributes.inheritHandle = (m_threadSecurityInheritCheck != nullptr && m_threadSecurityInheritCheck->isChecked());
    }

    request.startupInfo.useValue = (m_useStartupInfoCheck != nullptr && m_useStartupInfoCheck->isChecked());
    if (request.startupInfo.useValue)
    {
        request.startupInfo.cb = parseUInt32WithDefault(m_siCbEdit != nullptr ? m_siCbEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.cb 解析失败。"); return request; }
        request.startupInfo.lpReserved = (m_siReservedEdit != nullptr ? m_siReservedEdit->text() : QString()).toStdString();
        request.startupInfo.lpDesktop = (m_siDesktopEdit != nullptr ? m_siDesktopEdit->text() : QString()).toStdString();
        request.startupInfo.lpTitle = (m_siTitleEdit != nullptr ? m_siTitleEdit->text() : QString()).toStdString();
        request.startupInfo.dwX = parseUInt32WithDefault(m_siXEdit != nullptr ? m_siXEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwX 解析失败。"); return request; }
        request.startupInfo.dwY = parseUInt32WithDefault(m_siYEdit != nullptr ? m_siYEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwY 解析失败。"); return request; }
        request.startupInfo.dwXSize = parseUInt32WithDefault(m_siXSizeEdit != nullptr ? m_siXSizeEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwXSize 解析失败。"); return request; }
        request.startupInfo.dwYSize = parseUInt32WithDefault(m_siYSizeEdit != nullptr ? m_siYSizeEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwYSize 解析失败。"); return request; }
        request.startupInfo.dwXCountChars = parseUInt32WithDefault(m_siXCountCharsEdit != nullptr ? m_siXCountCharsEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwXCountChars 解析失败。"); return request; }
        request.startupInfo.dwYCountChars = parseUInt32WithDefault(m_siYCountCharsEdit != nullptr ? m_siYCountCharsEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwYCountChars 解析失败。"); return request; }
        request.startupInfo.dwFillAttribute = parseUInt32WithDefault(m_siFillAttributeEdit != nullptr ? m_siFillAttributeEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwFillAttribute 解析失败。"); return request; }
        request.startupInfo.dwFlags = parseUInt32WithDefault(m_siFlagsEdit != nullptr ? m_siFlagsEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwFlags 解析失败。"); return request; }
        request.startupInfo.wShowWindow = static_cast<std::uint16_t>(
            parseUInt32WithDefault(m_siShowWindowEdit != nullptr ? m_siShowWindowEdit->text() : QString(), 0, &parseOk));
        if (!parseOk) { failBuild("STARTUPINFO.wShowWindow 解析失败。"); return request; }
        request.startupInfo.cbReserved2 = static_cast<std::uint16_t>(
            parseUInt32WithDefault(m_siCbReserved2Edit != nullptr ? m_siCbReserved2Edit->text() : QString(), 0, &parseOk));
        if (!parseOk) { failBuild("STARTUPINFO.cbReserved2 解析失败。"); return request; }
        request.startupInfo.lpReserved2 = parseUInt64WithDefault(m_siReserved2PtrEdit != nullptr ? m_siReserved2PtrEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.lpReserved2 解析失败。"); return request; }
        request.startupInfo.hStdInput = parseUInt64WithDefault(m_siStdInputEdit != nullptr ? m_siStdInputEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.hStdInput 解析失败。"); return request; }
        request.startupInfo.hStdOutput = parseUInt64WithDefault(m_siStdOutputEdit != nullptr ? m_siStdOutputEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.hStdOutput 解析失败。"); return request; }
        request.startupInfo.hStdError = parseUInt64WithDefault(m_siStdErrorEdit != nullptr ? m_siStdErrorEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.hStdError 解析失败。"); return request; }
    }

    request.processInfo.useValue = (m_useProcessInfoCheck != nullptr && m_useProcessInfoCheck->isChecked());
    if (request.processInfo.useValue)
    {
        request.processInfo.hProcess = parseUInt64WithDefault(m_piProcessHandleEdit != nullptr ? m_piProcessHandleEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.hProcess 解析失败。"); return request; }
        request.processInfo.hThread = parseUInt64WithDefault(m_piThreadHandleEdit != nullptr ? m_piThreadHandleEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.hThread 解析失败。"); return request; }
        request.processInfo.dwProcessId = parseUInt32WithDefault(m_piPidEdit != nullptr ? m_piPidEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.dwProcessId 解析失败。"); return request; }
        request.processInfo.dwThreadId = parseUInt32WithDefault(m_piTidEdit != nullptr ? m_piTidEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.dwThreadId 解析失败。"); return request; }
    }

    request.tokenModeEnabled = (m_createMethodCombo != nullptr && m_createMethodCombo->currentIndex() == 1);
    if (request.tokenModeEnabled)
    {
        request.tokenSourcePid = parseUInt32WithDefault(
            m_tokenSourcePidEdit != nullptr ? m_tokenSourcePidEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Token 模式 source PID 解析失败。");
            return request;
        }
        request.tokenDesiredAccess = parseUInt32WithDefault(
            m_tokenDesiredAccessEdit != nullptr ? m_tokenDesiredAccessEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Token 模式 desired access 解析失败。");
            return request;
        }
        request.duplicatePrimaryToken = (m_tokenDuplicatePrimaryCheck != nullptr && m_tokenDuplicatePrimaryCheck->isChecked());

        if (m_tokenPrivilegeTable != nullptr)
        {
            for (int row = 0; row < m_tokenPrivilegeTable->rowCount(); ++row)
            {
                QTableWidgetItem* privilegeItem = m_tokenPrivilegeTable->item(row, 0);
                QComboBox* actionCombo = qobject_cast<QComboBox*>(m_tokenPrivilegeTable->cellWidget(row, 1));
                if (privilegeItem == nullptr || actionCombo == nullptr)
                {
                    continue;
                }

                const auto actionValue = static_cast<ks::process::TokenPrivilegeAction>(
                    actionCombo->currentData().toInt());
                if (actionValue == ks::process::TokenPrivilegeAction::Keep)
                {
                    continue;
                }

                ks::process::TokenPrivilegeEdit editItem{};
                editItem.privilegeName = privilegeItem->text().trimmed().toStdString();
                editItem.action = actionValue;
                request.tokenPrivilegeEdits.push_back(std::move(editItem));
            }
        }
    }

    if (buildOk != nullptr)
    {
        *buildOk = true;
    }
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }
    return request;
}

void ProcessDock::executeApplyTokenPrivilegeEditsOnly()
{
    // 令牌调整动作日志：整段流程复用同一个 kLogEvent，避免离散调用链。
    kLogEvent actionEvent;
    bool buildOk = false;
    QString errorText;
    const ks::process::CreateProcessRequest request = buildCreateProcessRequestFromUi(&buildOk, &errorText);
    if (!buildOk)
    {
        appendCreateResultLine("参数解析失败: " + errorText);
        err << actionEvent
            << "[ProcessDock] 令牌调整参数解析失败, error="
            << errorText.toStdString()
            << eol;
        return;
    }
    if (!request.tokenModeEnabled)
    {
        appendCreateResultLine("当前不是 Token 模式，无法仅应用令牌调整。");
        warn << actionEvent
            << "[ProcessDock] 令牌调整被拒绝：当前非 Token 模式。"
            << eol;
        return;
    }

    std::string detailText;
    const bool adjustOk = ks::process::ApplyTokenPrivilegeEditsByPid(
        request.tokenSourcePid,
        request.tokenDesiredAccess,
        request.duplicatePrimaryToken,
        request.tokenPrivilegeEdits,
        &detailText);
    std::ostringstream desiredAccessStream;
    desiredAccessStream << "0x" << std::uppercase << std::hex << request.tokenDesiredAccess;

    appendCreateResultLine(QString("令牌调整结果: %1").arg(adjustOk ? "成功" : "失败"));
    appendCreateResultLine(QString::fromStdString(detailText.empty() ? "无附加信息" : detailText));
    (adjustOk ? info : err) << actionEvent
        << "[ProcessDock] 令牌调整完成, ok=" << (adjustOk ? "true" : "false")
        << ", sourcePid=" << request.tokenSourcePid
        << ", desiredAccess=" << desiredAccessStream.str()
        << ", duplicatePrimary=" << (request.duplicatePrimaryToken ? "true" : "false")
        << ", editCount=" << request.tokenPrivilegeEdits.size()
        << ", detail=" << (detailText.empty() ? "无附加信息" : detailText)
        << eol;
}

void ProcessDock::executeCreateProcessRequest()
{
    // 创建进程动作日志：整段流程复用同一个 kLogEvent，避免离散调用链。
    kLogEvent createProcessEvent;
    bool buildOk = false;
    QString errorText;
    const ks::process::CreateProcessRequest request = buildCreateProcessRequestFromUi(&buildOk, &errorText);
    if (!buildOk)
    {
        appendCreateResultLine("参数解析失败: " + errorText);
        err << createProcessEvent
            << "[ProcessDock] CreateProcess 参数解析失败, error="
            << errorText.toStdString()
            << eol;
        return;
    }

    ks::process::CreateProcessResult createResult{};
    const bool launchOk = ks::process::LaunchProcess(request, &createResult);
    appendCreateResultLine(QString("调用结果: %1").arg(launchOk ? "成功" : "失败"));
    appendCreateResultLine(QString("路径模式: %1").arg(createResult.usedTokenPath ? "Token" : "CreateProcessW"));
    appendCreateResultLine(QString("错误码: %1").arg(createResult.win32Error));
    appendCreateResultLine(QString::fromStdString(createResult.detailText));
    if (createResult.processInfoAvailable)
    {
        appendCreateResultLine(
            QString("输出 PI: pid=%1 tid=%2 hProcess=0x%3 hThread=0x%4")
            .arg(createResult.dwProcessId)
            .arg(createResult.dwThreadId)
            .arg(QString::number(createResult.hProcess, 16))
            .arg(QString::number(createResult.hThread, 16)));
    }

    (launchOk ? info : err) << createProcessEvent
        << "[ProcessDock] CreateProcess 请求完成, ok=" << (launchOk ? "true" : "false")
        << ", tokenMode=" << (request.tokenModeEnabled ? "true" : "false")
        << ", error=" << createResult.win32Error
        << ", detail=" << createResult.detailText
        << eol;

    if (launchOk)
    {
        requestAsyncRefresh(true);
    }
}

bool ProcessDock::isTreeModeEnabled() const
{
    return m_treeToggleButton != nullptr && m_treeToggleButton->isChecked();
}

ProcessDock::ViewMode ProcessDock::currentViewMode() const
{
    return static_cast<ViewMode>(m_viewModeCombo->currentIndex());
}

void ProcessDock::executeR0TerminateProcessAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeR0TerminateProcessAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("R0结束进程"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return terminateProcessByR0Driver(actionTarget.record.pid, detailTextOut);
        },
        true);
}

void ProcessDock::executeR0SuspendProcessAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeR0SuspendProcessAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("R0挂起进程"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return suspendProcessByR0Driver(actionTarget.record.pid, detailTextOut);
        },
        false);
}

void ProcessDock::executeR0SetProcessHiddenAction(const bool hidden)
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeR0SetProcessHiddenAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    if (hidden)
    {
        const QMessageBox::StandardButton choice = QMessageBox::warning(
            this,
            QStringLiteral("R0进程隐藏(可恢复)"),
            QStringLiteral(
                "将把选中的 %1 个进程写入 Ksword 驱动隐藏表。\n\n"
                "说明：当前实现只影响 Ksword 进程列表过滤和 R0 枚举标记，不做 DKOM 断链；"
                "可通过“显示Ksword隐藏项”或“清空全部隐藏标记”恢复。")
                .arg(actionTargets.size()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes)
        {
            return;
        }
    }

    std::size_t successCount = 0U;
    std::size_t failureCount = 0U;
    QStringList detailLines;
    const unsigned long action = hidden
        ? KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE
        : KSWORD_ARK_PROCESS_VISIBILITY_ACTION_UNHIDE;

    for (const ProcessActionTarget& actionTarget : actionTargets)
    {
        std::string detailText;
        const bool actionOk = setProcessVisibilityByR0Driver(
            actionTarget.record.pid,
            action,
            &detailText);
        if (actionOk)
        {
            ++successCount;
            if (hidden)
            {
                m_hiddenProcessPidSet.insert(actionTarget.record.pid);
            }
            else
            {
                m_hiddenProcessPidSet.erase(actionTarget.record.pid);
            }
        }
        else
        {
            ++failureCount;
        }
        detailLines.push_back(QStringLiteral("PID=%1 %2 | %3")
            .arg(actionTarget.record.pid)
            .arg(actionOk ? QStringLiteral("OK") : QStringLiteral("FAIL"))
            .arg(QString::fromStdString(detailText.empty() ? std::string("无附加信息") : detailText)));
    }

    const QString titleText = hidden
        ? QStringLiteral("R0隐藏进程")
        : QStringLiteral("R0取消隐藏进程");
    const QString summaryText = QStringLiteral("%1 完成：成功 %2，失败 %3\n\n%4")
        .arg(titleText)
        .arg(successCount)
        .arg(failureCount)
        .arg(detailLines.join(QLatin1Char('\n')));

    kLogEvent logEvent;
    (failureCount == 0U ? info : warn) << logEvent
        << "[ProcessDock] " << titleText.toStdString()
        << " completed, success=" << successCount
        << ", failure=" << failureCount
        << eol;
    showActionResultMessage(titleText, failureCount == 0U, summaryText.toStdString(), logEvent);
    requestAsyncRefresh(true);
}

void ProcessDock::executeR0ClearProcessHiddenAction()
{
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this,
        QStringLiteral("清空R0隐藏标记"),
        QStringLiteral("确定清空 Ksword 驱动内全部可恢复进程隐藏标记吗？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes)
    {
        return;
    }

    std::string detailText;
    const bool actionOk = setProcessVisibilityByR0Driver(
        0U,
        KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL,
        &detailText);
    if (actionOk)
    {
        m_hiddenProcessPidSet.clear();
    }

    kLogEvent logEvent;
    (actionOk ? info : warn) << logEvent
        << "[ProcessDock] 清空R0隐藏标记完成, ok="
        << (actionOk ? "true" : "false")
        << ", detail=" << (detailText.empty() ? "无附加信息" : detailText)
        << eol;
    showActionResultMessage(
        QStringLiteral("清空R0隐藏标记"),
        actionOk,
        detailText.empty() ? std::string("无附加信息") : detailText,
        logEvent);
    requestAsyncRefresh(true);
}

void ProcessDock::executeR0SetBreakOnTerminationAction(const bool enabled)
{
    // BreakOnTermination：
    // - R0 侧使用 ZwSetInformationProcess，不直接硬编码 EPROCESS.Flags；
    // - 批量目标逐个分发，失败详情写入统一动作日志。
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeR0SetBreakOnTerminationAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this,
        enabled ? QStringLiteral("启用 BreakOnTermination") : QStringLiteral("关闭 BreakOnTermination"),
        enabled
        ? QStringLiteral("将把选中的 %1 个进程设为关键进程。目标退出可能触发系统崩溃保护。是否继续？").arg(actionTargets.size())
        : QStringLiteral("将清除选中 %1 个进程的 BreakOnTermination。是否继续？").arg(actionTargets.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes)
    {
        return;
    }

    const unsigned long action = enabled
        ? KSWORD_ARK_PROCESS_SPECIAL_ACTION_ENABLE_BREAK_ON_TERMINATION
        : KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_BREAK_ON_TERMINATION;
    dispatchProcessActionTargetsInParallel(
        enabled ? QStringLiteral("R0启用BreakOnTermination") : QStringLiteral("R0关闭BreakOnTermination"),
        actionTargets,
        [action](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return setProcessSpecialFlagsByR0Driver(actionTarget.record.pid, action, detailTextOut);
        },
        false);
}

void ProcessDock::executeR0DisableApcInsertionAction()
{
    // 禁 APC 插入：
    // - R0 侧只处理当前已有线程的 ApcQueueable 位；
    // - 新创建线程不在本次结果范围内，因此提示中明确边界。
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeR0DisableApcInsertionAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this,
        QStringLiteral("禁止APC插入"),
        QStringLiteral(
            "将清除选中 %1 个进程当前线程的 ApcQueueable 位。\n\n"
            "说明：该动作影响现有线程；目标后续新建线程不自动覆盖。错误线程偏移可能导致系统不稳定。是否继续？")
            .arg(actionTargets.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes)
    {
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("R0禁止APC插入"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return setProcessSpecialFlagsByR0Driver(
                actionTarget.record.pid,
                KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_APC_INSERTION,
                detailTextOut);
        },
        false);
}

void ProcessDock::executeR0DkomRemoveFromCidTableAction()
{
    // PspCidTable DKOM：
    // - 目标对象由 R0 根据 PID 引用；
    // - UI 不传 EPROCESS 地址；
    // - 删除后 PsLookupProcessByProcessId 可能无法再找到目标，需刷新列表。
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeR0DkomRemoveFromCidTableAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    const QMessageBox::StandardButton choice = QMessageBox::critical(
        this,
        QStringLiteral("DKOM从PspCidTable删除"),
        QStringLiteral(
            "将从 PspCidTable 删除选中 %1 个进程的 CID 表项。\n\n"
            "风险：该动作不可通过当前菜单恢复，可能破坏句柄/PID 查询语义，错误系统版本或竞态会导致蓝屏。是否继续？")
            .arg(actionTargets.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes)
    {
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("R0 DKOM PspCidTable删除"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return dkomProcessByR0Driver(
                actionTarget.record.pid,
                KSWORD_ARK_PROCESS_DKOM_ACTION_REMOVE_FROM_PSP_CID_TABLE,
                detailTextOut);
        },
        false);
    requestAsyncRefresh(true);
}

void ProcessDock::executeR0SetPplProtectionAction(
    const std::uint8_t protectionLevel,
    const QString& levelDisplayText)
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeR0SetPplProtectionAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    const ks::process::ProcessRecord& primaryRecord = actionTargets.front().record;
    const std::uint32_t targetPid = primaryRecord.pid;
    std::uint8_t targetSignatureLevel = 0U;
    std::uint8_t targetSectionSignatureLevel = 0U;
    const bool signaturePredictionOk = resolvePplSignatureLevelsForUi(
        protectionLevel,
        &targetSignatureLevel,
        &targetSectionSignatureLevel);
    const QString currentProtectionText =
        ((primaryRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT) != 0U)
        ? byteHexText(primaryRecord.r0Protection)
        : QStringLiteral("Unavailable");
    const QString currentSignatureText =
        ((primaryRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_SIGNATURE_LEVEL_PRESENT) != 0U)
        ? byteHexText(primaryRecord.r0SignatureLevel)
        : QStringLiteral("Unavailable");
    const QString currentSectionSignatureText =
        ((primaryRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_SECTION_SIGNATURE_LEVEL_PRESENT) != 0U)
        ? byteHexText(primaryRecord.r0SectionSignatureLevel)
        : QStringLiteral("Unavailable");
    const QString targetSignatureText = signaturePredictionOk
        ? byteHexText(targetSignatureLevel)
        : QStringLiteral("Unknown");
    const QString targetSectionSignatureText = signaturePredictionOk
        ? byteHexText(targetSectionSignatureLevel)
        : QStringLiteral("Unknown");
    const QString confirmationText = QStringLiteral(
        "将通过 R0 驱动修改目标进程 PPL/Protection 字段。\n\n"
        "进程: %1 (PID %2)%12\n"
        "当前 Protection: %3  来源: %4\n"
        "目标 Protection: %5  菜单: %6\n\n"
        "SignatureLevel 影响:\n"
        "  当前 SignatureLevel: %7 -> 目标: %8\n"
        "  当前 SectionSignatureLevel: %9 -> 目标: %10\n\n"
        "%11\n\n"
        "风险: 该动作会直接写 EPROCESS.Protection/SignatureLevel/SectionSignatureLevel。"
        "错误的 DynData 偏移、系统版本差异或目标进程状态变化可能导致回滚失败、访问异常或系统不稳定。"
        "继续前请确认已保存当前字段值用于手工回滚。")
        .arg(QString::fromStdString(primaryRecord.processName.empty() ? std::string("Unknown") : primaryRecord.processName))
        .arg(targetPid)
        .arg(currentProtectionText)
        .arg(processFieldSourceText(primaryRecord.r0ProtectionSource))
        .arg(byteHexText(protectionLevel))
        .arg(levelDisplayText)
        .arg(currentSignatureText)
        .arg(targetSignatureText)
        .arg(currentSectionSignatureText)
        .arg(targetSectionSignatureText)
        .arg(pplMutationCapabilityText(primaryRecord))
        .arg(actionTargets.size() > 1U ? QStringLiteral("\n批量目标数: %1，确认后每个进程会独立线程执行。").arg(actionTargets.size()) : QString());

    const QMessageBox::StandardButton confirmationButton = QMessageBox::warning(
        this,
        QStringLiteral("确认 R0 设置 PPL 层级"),
        confirmationText,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmationButton != QMessageBox::Yes)
    {
        kLogEvent cancelEvent;
        warn << cancelEvent
            << "[ProcessDock] R0 set PPL action cancelled by user, pid="
            << targetPid
            << ", protectionLevel=0x"
            << std::hex
            << std::uppercase
            << static_cast<unsigned int>(protectionLevel)
            << std::dec
            << eol;
        return;
    }

    const QString actionTitle = QStringLiteral("R0设置PPL层级(%1)").arg(levelDisplayText);
    dispatchProcessActionTargetsInParallel(
        actionTitle,
        actionTargets,
        [protectionLevel](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return setPplProtectionLevelByR0Driver(actionTarget.record.pid, protectionLevel, detailTextOut);
        },
        false);
}

void ProcessDock::executeRefreshPplProtectionLevelAction()
{
    // 该动作只刷新用户选中的当前快照字段，不触发完整进程枚举。
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] PPL 保护级别刷新被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::size_t successCount = 0;
    std::size_t failureCount = 0;
    QStringList resultLineList;
    resultLineList.reserve(static_cast<qsizetype>(actionTargets.size()));

    // 每个目标独立调用 GetProcessInformation，避免 PPL 列依赖旧缓存。
    for (const ProcessActionTarget& actionTarget : actionTargets)
    {
        std::uint32_t protectionLevelValue = 0;
        std::string protectionLevelText;
        std::string errorText;
        const bool queryOk = ks::process::QueryProcessProtectionLevelByPid(
            actionTarget.record.pid,
            &protectionLevelValue,
            &protectionLevelText,
            &errorText);

        auto cacheIt = m_cacheByIdentity.find(actionTarget.identityKey);
        if (cacheIt == m_cacheByIdentity.end())
        {
            ++failureCount;
            resultLineList.push_back(QStringLiteral("PID %1: cache missing")
                .arg(actionTarget.record.pid));
            continue;
        }

        if (queryOk)
        {
            cacheIt->second.record.protectionLevelKnown = true;
            cacheIt->second.record.protectionLevel = protectionLevelValue;
            cacheIt->second.record.protectionLevelText = protectionLevelText;
            ++successCount;
            resultLineList.push_back(QStringLiteral("PID %1: %2")
                .arg(actionTarget.record.pid)
                .arg(QString::fromStdString(protectionLevelText)));
        }
        else
        {
            cacheIt->second.record.protectionLevelKnown = true;
            cacheIt->second.record.protectionLevel = 0;
            cacheIt->second.record.protectionLevelText = errorText.empty()
                ? std::string("Query failed")
                : std::string("Query failed: ") + errorText;
            ++failureCount;
            resultLineList.push_back(QStringLiteral("PID %1: %2")
                .arg(actionTarget.record.pid)
                .arg(QString::fromStdString(cacheIt->second.record.protectionLevelText)));
        }
    }

    rebuildTable();
    updateRefreshStateUi(
        false,
        QStringLiteral("● PPL保护级别手动刷新完成 成功:%1 失败:%2")
            .arg(successCount)
            .arg(failureCount));

    kLogEvent actionEvent;
    (failureCount == 0 ? info : warn) << actionEvent
        << "[ProcessDock] PPL 保护级别手动刷新完成, targets=" << actionTargets.size()
        << ", success=" << successCount
        << ", failure=" << failureCount
        << ", detail=" << resultLineList.join(" | ").toStdString()
        << eol;
}

void ProcessDock::executeTerminateProcessAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTerminateProcessAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("结束进程"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            // targetPid 用途：固定本次动作的目标 PID，避免中途选中行变化影响执行对象。
            const std::uint32_t targetPid = actionTarget.record.pid;
            // 单目标动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
            kLogEvent actionEvent;
            info << actionEvent
                << "[ProcessDock] 开始执行结束进程组合动作, pid=" << targetPid
                << eol;

            // 先做一次进程存在性检查，避免对已退出 PID 执行无意义操作。
            bool initialQueryOk = false;
            bool processStillPresent = isProcessPresentBySnapshot(targetPid, &initialQueryOk);
            if (!initialQueryOk)
            {
                warn << actionEvent
                    << "[ProcessDock] 结束进程前置存在性检查失败，将按“进程仍存在”继续处理, pid="
                    << targetPid
                    << eol;
            }
            if (!processStillPresent)
            {
                info << actionEvent
                    << "[ProcessDock] 目标进程已不存在，结束动作直接判定成功, pid="
                    << targetPid
                    << eol;
                if (detailTextOut != nullptr)
                {
                    *detailTextOut = "目标进程已不存在，无需执行结束动作。";
                }
                return true;
            }

            // kTerminateRoundLimit 用途：限制“全方法链”轮次，避免异常目标导致无限阻塞 UI。
            constexpr int kTerminateRoundLimit = 2;
            // processExited 用途：记录组合动作最终是否确认目标进程已退出。
            bool processExited = false;
            // actionDetailStream 用途：汇总每一轮、每一方法的细节，供最终统一输出。
            std::ostringstream actionDetailStream;
            actionDetailStream << "pid=" << targetPid;

            // TerminateMethodEntry 作用：描述一个可执行的“结束进程原理方法”。
            struct TerminateMethodEntry
            {
                const char* methodName = nullptr; // methodName：日志中显示的方法名。
                std::function<bool(std::string*)> invokeMethod; // invokeMethod：方法调用体。
            };

            // terminateMethodList 作用：
            // - 维护“结束进程原理”的顺序清单；
            // - 每个进程的内部方法链保持顺序，但多个进程之间并行执行。
            const std::vector<TerminateMethodEntry> terminateMethodList =
            {
                { "TerminateProcess(Kernel32)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByWin32(targetPid, detailOut); } },
                { "NtTerminateProcess/ZwTerminateProcess", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByNtNative(targetPid, detailOut); } },
                { "WTSTerminateProcess(WTS API)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByWtsApi(targetPid, detailOut); } },
                { "WinStationTerminateProcess(winsta)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByWinStationApi(targetPid, detailOut); } },
                { "TerminateJobObject(Job)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByJobObject(targetPid, detailOut); } },
                { "NtTerminateJobObject/ZwTerminateJobObject", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByNtJobObject(targetPid, detailOut); } },
                { "RmShutdown(Restart Manager)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByRestartManager(targetPid, false, detailOut); } },
                { "RmShutdown(Restart Manager, force)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByRestartManager(targetPid, true, detailOut); } },
                { "DuplicateHandle(-1)+TerminateProcess", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByDuplicateHandlePseudo(targetPid, detailOut); } },
                { "TerminateThread(全部线程)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateAllThreadsByPid(targetPid, detailOut); } },
                { "NtTerminateThread/ZwTerminateThread(全部线程)", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateAllThreadsByPidNtNative(targetPid, detailOut); } },
                { "DebugActiveProcess 调试附加", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByDebugAttach(targetPid, detailOut); } },
                { "ntsd -c q -p <pid>", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByNtsdCommand(targetPid, detailOut); } },
                { "NtUnmapViewOfSection 卸载 ntdll.dll", [targetPid](std::string* detailOut)
                    { return ks::process::TerminateProcessByNtUnmapNtdll(targetPid, detailOut); } }
            };

            for (int roundIndex = 0; roundIndex < kTerminateRoundLimit && !processExited; ++roundIndex)
            {
                // roundNumber 用途：日志中的轮次编号（从 1 开始，便于人工排查）。
                const int roundNumber = roundIndex + 1;

                for (std::size_t methodIndex = 0;
                    methodIndex < terminateMethodList.size() && !processExited;
                    ++methodIndex)
                {
                    const TerminateMethodEntry& methodEntry = terminateMethodList[methodIndex];
                    if (methodEntry.methodName == nullptr || !methodEntry.invokeMethod)
                    {
                        continue;
                    }

                    std::string methodDetailText;
                    const bool methodOk = methodEntry.invokeMethod(&methodDetailText);
                    const std::string normalizedMethodDetailText =
                        methodDetailText.empty() ? "无附加信息" : methodDetailText;
                    (methodOk ? info : err) << actionEvent
                        << "[ProcessDock] 结束进程组合动作-方法执行, pid="
                        << targetPid
                        << ", round="
                        << roundNumber
                        << ", method="
                        << methodEntry.methodName
                        << ", ok="
                        << (methodOk ? "true" : "false")
                        << ", detail="
                        << normalizedMethodDetailText
                        << eol;
                    if (!methodOk)
                    {
                        warn << actionEvent
                            << "[ProcessDock] 当前方法执行失败，继续尝试下一方法, pid="
                            << targetPid
                            << ", round="
                            << roundNumber
                            << ", method="
                            << methodEntry.methodName
                            << eol;
                    }

                    actionDetailStream
                        << " | round"
                        << roundNumber
                        << ":"
                        << methodEntry.methodName
                        << "="
                        << (methodOk ? "ok" : "fail")
                        << "("
                        << normalizedMethodDetailText
                        << ")";

                    bool queryProcessPresentOk = false;
                    processStillPresent = isProcessPresentBySnapshot(targetPid, &queryProcessPresentOk);
                    if (!queryProcessPresentOk)
                    {
                        warn << actionEvent
                            << "[ProcessDock] 方法执行后存在性检查失败，按“仍存活”继续尝试, pid="
                            << targetPid
                            << ", round="
                            << roundNumber
                            << ", method="
                            << methodEntry.methodName
                            << eol;
                    }
                    if (!processStillPresent)
                    {
                        processExited = true;
                        info << actionEvent
                            << "[ProcessDock] 目标进程已退出, pid="
                            << targetPid
                            << ", round="
                            << roundNumber
                            << ", method="
                            << methodEntry.methodName
                            << eol;
                        break;
                    }
                }

                if (!processExited)
                {
                    warn << actionEvent
                        << "[ProcessDock] 本轮全方法链执行后目标仍存活，将进入下一轮, pid="
                        << targetPid
                        << ", round="
                        << roundNumber
                        << eol;
                }
            }

            if (!processExited)
            {
                err << actionEvent
                    << "[ProcessDock] 结束进程组合动作达到上限后目标仍存活, pid="
                    << targetPid
                    << ", roundLimit="
                    << kTerminateRoundLimit
                    << eol;
            }

            if (detailTextOut != nullptr)
            {
                *detailTextOut = actionDetailStream.str();
            }
            return processExited;
        },
        true);
}

void ProcessDock::executeTerminateThreadsAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTerminateThreadsAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("TerminateThread(全部线程)"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return ks::process::TerminateAllThreadsByPid(actionTarget.record.pid, detailTextOut);
        },
        true);
}

void ProcessDock::executeSuspendAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSuspendAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("挂起进程"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return ks::process::SuspendProcess(actionTarget.record.pid, detailTextOut);
        },
        false);
}

void ProcessDock::executeResumeAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeResumeAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("恢复进程"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return ks::process::ResumeProcess(actionTarget.record.pid, detailTextOut);
        },
        false);
}

void ProcessDock::executeSetCriticalAction(const bool enableCritical)
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSetCriticalAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        enableCritical ? QStringLiteral("设为关键进程") : QStringLiteral("取消关键进程"),
        actionTargets,
        [enableCritical](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return ks::process::SetProcessCriticalFlag(actionTarget.record.pid, enableCritical, detailTextOut);
        },
        false);
}

void ProcessDock::executeSetPriorityAction(const int priorityActionId)
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSetPriorityAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    ks::process::ProcessPriorityLevel priorityLevel = ks::process::ProcessPriorityLevel::Normal;
    switch (priorityActionId)
    {
    case 0: priorityLevel = ks::process::ProcessPriorityLevel::Idle; break;
    case 1: priorityLevel = ks::process::ProcessPriorityLevel::BelowNormal; break;
    case 2: priorityLevel = ks::process::ProcessPriorityLevel::Normal; break;
    case 3: priorityLevel = ks::process::ProcessPriorityLevel::AboveNormal; break;
    case 4: priorityLevel = ks::process::ProcessPriorityLevel::High; break;
    case 5: priorityLevel = ks::process::ProcessPriorityLevel::Realtime; break;
    default: break;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("设置进程优先级"),
        actionTargets,
        [priorityLevel](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return ks::process::SetProcessPriority(actionTarget.record.pid, priorityLevel, detailTextOut);
        },
        false);
}

void ProcessDock::executeSetEfficiencyModeAction(const bool enableEfficiencyMode)
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSetEfficiencyModeAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    const QString actionTitle =
        enableEfficiencyMode ? QStringLiteral("开启效率模式") : QStringLiteral("关闭效率模式");
    dispatchProcessActionTargetsInParallel(
        actionTitle,
        actionTargets,
        [enableEfficiencyMode](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return ks::process::SetProcessEfficiencyMode(
                actionTarget.record.pid,
                enableEfficiencyMode,
                detailTextOut);
        },
        false);

    // UI 缓存即时更新：线程执行结果仍会独立记录；这里仅让已选行视觉状态快速响应。
    for (const ProcessActionTarget& actionTarget : actionTargets)
    {
        const auto cacheIt = m_cacheByIdentity.find(actionTarget.identityKey);
        if (cacheIt != m_cacheByIdentity.end())
        {
            cacheIt->second.record.efficiencyModeSupported = true;
            cacheIt->second.record.efficiencyModeEnabled = enableEfficiencyMode;
        }
    }
    if (m_processTable != nullptr)
    {
        m_processTable->viewport()->update();
    }
}

void ProcessDock::executeOpenFolderAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeOpenFolderAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    dispatchProcessActionTargetsInParallel(
        QStringLiteral("打开所在目录"),
        actionTargets,
        [](const ProcessActionTarget& actionTarget, std::string* detailTextOut)
        {
            return ks::process::OpenProcessFolder(actionTarget.record.pid, detailTextOut);
        },
        false);
}

void ProcessDock::executeOpenMemoryOperationAction()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeOpenMemoryOperationAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    for (const ProcessActionTarget& actionTarget : actionTargets)
    {
        const bool invokeOk = QMetaObject::invokeMethod(
            this->parent(),
            "focusMemoryDockByPid",
            Qt::QueuedConnection,
            Q_ARG(quint32, static_cast<quint32>(actionTarget.record.pid)));
        kLogEvent actionEvent;
        (invokeOk ? info : warn) << actionEvent
            << "[ProcessDock] 跳转到内存操作, pid=" << actionTarget.record.pid
            << ", invokeOk=" << (invokeOk ? "true" : "false")
            << eol;
        if (!invokeOk)
        {
            showActionResultMessage(
                QStringLiteral("跳转到内存操作"),
                false,
                std::string("focusMemoryDockByPid invoke failed"),
                actionEvent);
        }
    }
}

void ProcessDock::requestOpenProcessDetailByPid(const std::uint32_t pid)
{
    // 对外统一入口：便于 FileDock/主窗口按 PID 打开进程详情。
    kLogEvent requestDetailEvent;
    info << requestDetailEvent
        << "[ProcessDock] requestOpenProcessDetailByPid: pid="
        << pid
        << eol;
    openProcessDetailWindowByPid(pid);
}

void ProcessDock::openProcessDetailsPlaceholder()
{
    const std::vector<ProcessActionTarget> actionTargets = selectedActionTargets();
    if (actionTargets.empty())
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] 打开进程详细信息失败：当前没有选中进程。" << eol;
        QMessageBox::warning(this, "进程详细信息", "请先在表格中选中一个进程。");
        return;
    }
    if (actionTargets.size() > 1U)
    {
        kLogEvent logEvent;
        warn << logEvent
            << "[ProcessDock] 打开进程详细信息失败：详情窗口仅支持单进程, selectedCount="
            << actionTargets.size()
            << eol;
        QMessageBox::information(this, "进程详细信息", "请只选中一个进程再打开详情窗口。");
        return;
    }

    // 详情窗口展示前不再同步补齐静态字段：
    // - 旧逻辑会在 UI 线程读取命令行、令牌和数字签名；
    // - 签名校验与权限受限进程会让“打开进程详细信息”明显卡顿；
    // - 现在先用列表缓存开窗，ProcessDetailWindow 内部后台补齐缺失字段。
    ks::process::ProcessRecord detailRecord = actionTargets.front().record;

    // identityKey 用于“一进程一窗口”复用逻辑。
    const std::string identityKey = ks::process::BuildProcessIdentityKey(
        detailRecord.pid,
        detailRecord.creationTime100ns);

    auto existingWindowIt = m_detailWindowByIdentity.find(identityKey);
    if (existingWindowIt != m_detailWindowByIdentity.end() && existingWindowIt->second != nullptr)
    {
        existingWindowIt->second->updateBaseRecord(detailRecord);
        m_detailWindowLastSyncTimeByIdentity[identityKey] = std::chrono::steady_clock::now();
        existingWindowIt->second->show();
        existingWindowIt->second->raise();
        existingWindowIt->second->activateWindow();

        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 复用已存在进程详情窗口, pid=" << detailRecord.pid
            << ", identity=" << identityKey
            << eol;
        return;
    }

    // 创建新的独立窗口（不属于 Docking System，可并行打开多个）。
    ProcessDetailWindow* detailWindow = new ProcessDetailWindow(detailRecord, nullptr);
    detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    m_detailWindowByIdentity[identityKey] = detailWindow;
    m_detailWindowLastSyncTimeByIdentity[identityKey] = std::chrono::steady_clock::now();

    // 详情窗口销毁后，从缓存移除，防止悬空指针。
    connect(detailWindow, &QObject::destroyed, this, [this, identityKey]() {
        m_detailWindowByIdentity.erase(identityKey);
        m_detailWindowLastSyncTimeByIdentity.erase(identityKey);
    });

    // “转到父进程”由详情窗口发信号到这里统一处理。
    connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
        openProcessDetailWindowByPid(parentPid);
    });
    connect(detailWindow, &ProcessDetailWindow::requestOpenHandleDockByPid, this, [this](const std::uint32_t targetPid) {
        const bool invokeOk = QMetaObject::invokeMethod(
            this->parent(),
            "focusHandleDockByPid",
            Qt::QueuedConnection,
            Q_ARG(quint32, static_cast<quint32>(targetPid)));
        if (!invokeOk)
        {
            kLogEvent logEvent;
            warn << logEvent
                << "[ProcessDock] requestOpenHandleDockByPid 转发失败, pid="
                << targetPid
                << eol;
        }
    });

    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 创建新的进程详情窗口, pid=" << detailRecord.pid
        << ", identity=" << identityKey
        << eol;
}

void ProcessDock::openProcessDetailWindowByPid(const std::uint32_t pid)
{
    // 优先从当前缓存中查找对应 PID（可避免额外系统调用）。
    for (const auto& cachePair : m_cacheByIdentity)
    {
        const ks::process::ProcessRecord& cacheRecord = cachePair.second.record;
        if (cacheRecord.pid != pid)
        {
            continue;
        }

        auto existingWindowIt = m_detailWindowByIdentity.find(cachePair.first);
        if (existingWindowIt != m_detailWindowByIdentity.end() && existingWindowIt->second != nullptr)
        {
            existingWindowIt->second->updateBaseRecord(cacheRecord);
            m_detailWindowLastSyncTimeByIdentity[cachePair.first] = std::chrono::steady_clock::now();
            existingWindowIt->second->show();
            existingWindowIt->second->raise();
            existingWindowIt->second->activateWindow();
            return;
        }

        // 与当前选中逻辑一致：直接使用缓存开窗。
        // 缺失的静态字段由详情窗口后台补齐，避免这里阻塞 UI 线程。
        ks::process::ProcessRecord detailRecord = cacheRecord;

        ProcessDetailWindow* detailWindow = new ProcessDetailWindow(detailRecord, nullptr);
        detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        m_detailWindowByIdentity[cachePair.first] = detailWindow;
        m_detailWindowLastSyncTimeByIdentity[cachePair.first] = std::chrono::steady_clock::now();
        connect(detailWindow, &QObject::destroyed, this, [this, identityKey = cachePair.first]() {
            m_detailWindowByIdentity.erase(identityKey);
            m_detailWindowLastSyncTimeByIdentity.erase(identityKey);
        });
        connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
            openProcessDetailWindowByPid(parentPid);
        });
        connect(detailWindow, &ProcessDetailWindow::requestOpenHandleDockByPid, this, [this](const std::uint32_t targetPid) {
            const bool invokeOk = QMetaObject::invokeMethod(
                this->parent(),
                "focusHandleDockByPid",
                Qt::QueuedConnection,
                Q_ARG(quint32, static_cast<quint32>(targetPid)));
            if (!invokeOk)
            {
                kLogEvent logEvent;
                warn << logEvent
                    << "[ProcessDock] requestOpenHandleDockByPid 转发失败, pid="
                    << targetPid
                    << eol;
            }
        });
        detailWindow->show();
        detailWindow->raise();
        detailWindow->activateWindow();
        return;
    }

    // 缓存不存在时只读取轻量名称并打开窗口：
    // - QueryProcessStaticDetailByPid 默认包含签名校验，不能放在 UI 开窗路径；
    // - 详情窗口会在后台补齐路径、命令行、用户和签名。
    ks::process::ProcessRecord queriedRecord{};
    queriedRecord.pid = pid;
    queriedRecord.processName = ks::process::GetProcessNameByPID(pid);
    if (queriedRecord.processName.empty())
    {
        queriedRecord.processName = "PID_" + std::to_string(pid);
    }

    const std::string identityKey = ks::process::BuildProcessIdentityKey(
        queriedRecord.pid,
        queriedRecord.creationTime100ns);

    ProcessDetailWindow* detailWindow = new ProcessDetailWindow(queriedRecord, nullptr);
    detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    m_detailWindowByIdentity[identityKey] = detailWindow;
    m_detailWindowLastSyncTimeByIdentity[identityKey] = std::chrono::steady_clock::now();
    connect(detailWindow, &QObject::destroyed, this, [this, identityKey]() {
        m_detailWindowByIdentity.erase(identityKey);
        m_detailWindowLastSyncTimeByIdentity.erase(identityKey);
    });
    connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
        openProcessDetailWindowByPid(parentPid);
    });
    connect(detailWindow, &ProcessDetailWindow::requestOpenHandleDockByPid, this, [this](const std::uint32_t targetPid) {
        const bool invokeOk = QMetaObject::invokeMethod(
            this->parent(),
            "focusHandleDockByPid",
            Qt::QueuedConnection,
            Q_ARG(quint32, static_cast<quint32>(targetPid)));
        if (!invokeOk)
        {
            kLogEvent logEvent;
            warn << logEvent
                << "[ProcessDock] requestOpenHandleDockByPid 转发失败, pid="
                << targetPid
                << eol;
        }
    });
    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();
}
