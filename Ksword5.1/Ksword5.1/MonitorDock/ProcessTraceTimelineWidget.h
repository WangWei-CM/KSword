#pragma once

// ============================================================
// ProcessTraceTimelineWidget.h
// 作用：
// 1) 在 ETW 事件表上方提供紧凑的瀑布流时间轴；
// 2) 用真实 100ns 时间戳维护时间框选状态，避免从图形点位反推事件；
// 3) 暴露轻量 QWidget API，让 ProcessTraceMonitorWidget 把时间筛选与现有文本/类型筛选叠加。
// ============================================================

#include <QWidget>
#include <QColor>
#include <QPoint>
#include <QRectF>
#include <QString>

#include <cstdint>     // std::uint64_t：ETW FILETIME 兼容的 100ns 时间戳。
#include <functional>  // std::function：不依赖 Qt moc signal 的轻量回调。
#include <vector>      // std::vector：从事件表缓存复制而来的轻量点列表。

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

// ProcessTraceTimelineEventPoint：
// - time100ns：ETW 绝对时间戳，单位为 100ns；
// - typeText：已经归类后的事件类型，用于决定颜色与行；
// - 控件只读取该轻量结构，不依赖表格行号或屏幕坐标。
struct ProcessTraceTimelineEventPoint
{
    std::uint64_t time100ns = 0;
    QString typeText;
};

// ProcessTraceTimelineRatePoint：
// - time100ns：折线点对应的时间轴位置，单位仍为 100ns；
// - uploadBytesPerSecond：该秒出站/上传速率，单位 B/s；
// - downloadBytesPerSecond：该秒入站/下载速率，单位 B/s；
// - 该结构是可选叠加层，ETW 页面不设置时不会绘制任何速率折线。
struct ProcessTraceTimelineRatePoint
{
    std::uint64_t time100ns = 0;
    double uploadBytesPerSecond = 0.0;
    double downloadBytesPerSecond = 0.0;
};

class ProcessTraceTimelineWidget final : public QWidget
{
public:
    // 构造函数：
    // - parent：Qt 父控件；
    // - 控件高度固定为 40 逻辑像素，宽度由布局横向拉伸。
    explicit ProcessTraceTimelineWidget(QWidget* parent = nullptr);

    // setCaptureRange：
    // - start100ns：时间轴最左侧对应的绝对 100ns 时间戳；
    // - end100ns：时间轴最右侧对应的当前或最终 100ns 时间戳；
    // - 用户未手动调整前，选区自动覆盖完整范围。
    void setCaptureRange(std::uint64_t start100ns, std::uint64_t end100ns);

    // resetTimeline：
    // - 清空全部事件点，并从 start100ns 开始建立新的全范围选区；
    // - 无返回值，需要时调用 selectionStart100ns/selectionEnd100ns 读取状态。
    void resetTimeline(std::uint64_t start100ns);

    // resetSelectionToFullRange：
    // - 把当前选区恢复为完整捕获范围；
    // - 无返回值，调用方负责重新执行事件表筛选。
    void resetSelectionToFullRange();

    // setEventPoints：
    // - 替换用于绘制的轻量事件点缓存；
    // - 调用方负责事件生命周期与表格筛选，本控件只负责绘制。
    void setEventPoints(const std::vector<ProcessTraceTimelineEventPoint>& eventPointList);

    // setRateOverlayPoints：
    // - 替换用于绘制的上传/下载速率折线缓存；
    // - 调用方负责按秒聚合，本控件只做缩放映射与叠加绘制；
    // - 传入空列表表示关闭速率折线叠加层。
    void setRateOverlayPoints(const std::vector<ProcessTraceTimelineRatePoint>& ratePointList);

    // setSelectionChangedCallback：
    // - 注册用户交互导致选区变化时的回调；
    // - 回调参数是绝对起止时间戳，单位为 100ns。
    void setSelectionChangedCallback(
        std::function<void(std::uint64_t, std::uint64_t)> callbackValue);

    // selectionStart100ns：
    // - 返回当前选区起点的绝对 100ns 时间戳；
    // - 捕获范围尚未初始化时返回 0。
    std::uint64_t selectionStart100ns() const;

    // selectionEnd100ns：
    // - 返回当前选区终点的绝对 100ns 时间戳；
    // - 捕获范围尚未初始化时返回 0。
    std::uint64_t selectionEnd100ns() const;

protected:
    // paintEvent：
    // - 绘制时间轴背景、事件点、持续时间标签和选区矩形；
    // - 无返回值，Qt 通过虚函数分发消费绘制事件。
    void paintEvent(QPaintEvent* eventPointer) override;

    // mousePressEvent：
    // - 开始拖动选区本体或左右边缘；
    // - 无返回值，命中可拖动区域时接管事件。
    void mousePressEvent(QMouseEvent* eventPointer) override;

    // mouseMoveEvent：
    // - 更新悬停光标，或执行当前拖动操作；
    // - 无返回值，选区变化时通知宿主回调。
    void mouseMoveEvent(QMouseEvent* eventPointer) override;

    // mouseReleaseEvent：
    // - 结束当前拖动操作并恢复普通命中测试；
    // - 无返回值。
    void mouseReleaseEvent(QMouseEvent* eventPointer) override;

    // leaveEvent：
    // - 鼠标离开且没有拖动时恢复默认光标；
    // - 无返回值。
    void leaveEvent(QEvent* eventPointer) override;

    // wheelEvent：
    // - 滚轮向上扩大选区，向下缩小选区；
    // - 操作修改内部时间戳，不从表格行或图形点反推。
    void wheelEvent(QWheelEvent* eventPointer) override;

private:
    // DragMode：
    // - None：普通悬停；
    // - Move：整体平移选区；
    // - ResizeLeft/ResizeRight：单独调整左侧或右侧边界。
    enum class DragMode
    {
        None,
        Move,
        ResizeLeft,
        ResizeRight
    };

    // timelineRect：
    // - 返回 40px 时间轴中的实际可绘制矩形；
    // - 矩形尽量占满可用宽度，仅保留极小内边距。
    QRectF timelineRect() const;

    // selectionRect：
    // - 把内部选区时间戳转换为绘制和命中测试用矩形；
    // - 捕获范围无效时返回空矩形。
    QRectF selectionRect() const;

    // hitTestSelection：
    // - 返回鼠标命中选区框的哪一部分；
    // - 边缘命中区域比可见边框更宽，便于拖拽。
    DragMode hitTestSelection(const QPoint& position) const;

    // updateHoverCursor：
    // - 根据当前悬停位置选择合适光标；
    // - 无返回值。
    void updateHoverCursor(const QPoint& position);

    // timeToX：
    // - 把绝对 100ns 时间戳映射为 X 坐标；
    // - 超出捕获范围的值会被夹到时间轴矩形内。
    double timeToX(std::uint64_t time100ns) const;

    // xToTime：
    // - 把 X 坐标映射回绝对 100ns 时间戳；
    // - 仅用于把鼠标输入转换为内部选区时间。
    std::uint64_t xToTime(double xValue) const;

    // laneForType：
    // - 把事件类型映射到稳定的纵向行；
    // - 40px 高度不足以容纳所有子类型时，相近事件族共享行。
    int laneForType(const QString& typeText) const;

    // colorForType：
    // - 返回某事件类型对应的 20% 透明度颜色；
    // - 不同类别使用有意区分的颜色。
    QColor colorForType(const QString& typeText) const;

    // formatDurationText：
    // - 把 100ns 持续时间格式化为 mm:ss 或 hh:mm:ss；
    // - 用于右侧标签，左侧标签固定为 00:00。
    QString formatDurationText(std::uint64_t duration100ns) const;

    // clampSelectionToRange：
    // - 保证选区位于捕获范围内，并强制最小宽度；
    // - 无返回值，直接修正成员时间戳。
    void clampSelectionToRange();

    // notifySelectionChanged：
    // - 如果宿主注册了回调，则通知选区变化；
    // - 无返回值。
    void notifySelectionChanged();

private:
    std::vector<ProcessTraceTimelineEventPoint> m_eventPointList; // m_eventPointList：仅用于绘制的轻量事件缓存。
    std::vector<ProcessTraceTimelineRatePoint> m_ratePointList; // m_ratePointList：上传/下载速率折线点缓存，空时不绘制。
    std::function<void(std::uint64_t, std::uint64_t)> m_selectionChangedCallback; // m_selectionChangedCallback：宿主筛选回调。
    std::uint64_t m_rangeStart100ns = 0;             // m_rangeStart100ns：时间轴左边缘绝对时间。
    std::uint64_t m_rangeEnd100ns = 0;               // m_rangeEnd100ns：时间轴右边缘绝对时间。
    std::uint64_t m_selectionStart100ns = 0;         // m_selectionStart100ns：选区起点绝对时间。
    std::uint64_t m_selectionEnd100ns = 0;           // m_selectionEnd100ns：选区终点绝对时间。
    std::uint64_t m_dragPressTime100ns = 0;          // m_dragPressTime100ns：拖动起点对应的时间。
    std::uint64_t m_dragOriginalStart100ns = 0;      // m_dragOriginalStart100ns：拖动前选区起点。
    std::uint64_t m_dragOriginalEnd100ns = 0;        // m_dragOriginalEnd100ns：拖动前选区终点。
    DragMode m_dragMode = DragMode::None;            // m_dragMode：当前鼠标操作模式。
    bool m_userAdjustedSelection = false;            // m_userAdjustedSelection：用户是否已手动调整选区。
};
