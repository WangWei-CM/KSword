#pragma once

// ============================================================
// MonitorPanelWidget.h
// 作用：
// 1) 提供“监视面板”Dock的四宫格性能图（CPU/内存/磁盘/网络）；
// 2) CPU 图按“每个逻辑核心”展示柱状条；
// 3) 采样逻辑独立于 MonitorDock，避免与 WMI/ETW 页面耦合。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <memory>

namespace ks::ui { class AsyncUiDispatcher; }

#include <cstdint> // std::uint64_t：保存时间戳与累计字节计数。
#include <vector>  // std::vector：保存每核计数器句柄与采样结果。

class QBarSet;
class QChartView;
class QGridLayout;
class QLabel;
class QLineSeries;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QEvent;
class QTimer;
class QValueAxis;
class QVBoxLayout;

class MonitorPanelWidget final : public QWidget
{
public:
    // 构造函数作用：
    // - 初始化四宫格图表；
    // - 启动 1 秒刷新定时器。
    // 参数 parent：Qt 父控件。
    explicit MonitorPanelWidget(QWidget* parent = nullptr);

    // 析构函数作用：
    // - 停止刷新定时器；
    // - 请求后台原生状态停止；句柄在退出 lease 内收尾，界面不等待。
    ~MonitorPanelWidget() override;

protected:
    // resizeEvent 作用：
    // - Dock 高度变化时重算四宫格图高度；
    // - 尽量避免监视面板出现滚动条。
    void resizeEvent(QResizeEvent* resizeEventPointer) override;

    // showEvent 作用：
    // - 首次展示后触发一次延迟重排；
    // - 解决初始 geometry 尚未稳定时的高度估算偏差。
    void showEvent(QShowEvent* showEventPointer) override;
    void hideEvent(QHideEvent* hideEventPointer) override;

    // changeEvent 作用：主题、样式或应用调色板变化后立即重刷图表文字颜色。
    void changeEvent(QEvent* eventPointer) override;

private:
    // initializeUi 作用：
    // - 创建 CPU/内存/磁盘/网络图表控件与布局。
    void initializeUi();

    // One accepted native collection at a time; a physical receipt also recovers lost UI delivery.
    void refreshMetrics();
    void reconcileMetrics();
    void refreshVisiblePresentation(bool snapToLatest = false);

    struct MemoryCompositionSample
    {
        double usedPercent = 0.0;       // usedPercent：物理内存使用百分比。
        double standbyPercent = 0.0;    // standbyPercent：近似缓存/备用百分比。
        double availablePercent = 0.0;  // availablePercent：可用物理内存百分比。
        double commitPercent = 0.0;     // commitPercent：提交量占提交上限百分比。
    };

    struct MetricPresentationSample
    {
        std::vector<double> coreUsages;
        double totalCpuUsage = 0.0;
        MemoryCompositionSample memory;
        double diskReadBytesPerSec = 0.0;
        double diskWriteBytesPerSec = 0.0;
        double networkRxBytesPerSec = 0.0;
        double networkTxBytesPerSec = 0.0;
        qint64 sampleTimeMs = 0;
        bool memoryValid = false;
        bool diskValid = false;
        bool networkValid = false;
    };

    struct SamplingState;
    struct SamplingReceipt;
    void applyMetricSample(MetricPresentationSample sample);
    std::shared_ptr<SamplingState> m_samplingState;
    std::shared_ptr<SamplingReceipt> m_samplingReceipt;
    std::shared_ptr<ks::ui::AsyncUiDispatcher> m_samplingDispatcher;

    // appendLineSample 作用：
    // - 给折线图追加一个采样点；
    // - 自动维护历史长度与轴范围。
    // 参数 series：目标折线序列。
    // 参数 axisX：X 轴对象。
    // 参数 axisY：Y 轴对象。
    // 参数 value：本次采样值。
    void appendLineSample(
        QLineSeries* series,
        QValueAxis* axisX,
        QValueAxis* axisY,
        double value);

    // adjustChartCellHeights 作用：
    // - 按当前 Dock 可用高度压缩四张图；
    // - 统一设置最小/最大高度，防止外层布局因最小高度触发滚动条。
    void adjustChartCellHeights();

    // applyChartTextTheme 作用：
    // - 统一刷新图表标题、图例、坐标轴文字颜色；
    // - 修复深色模式下左下角监视器文字对比度过低。
    void applyChartTextTheme();

    // updateCompactVisibility 作用：
    // - 当 Dock 高度过低时隐藏四张图，仅显示文字摘要；
    // - 避免自绘图表在极小高度下反复布局造成闪烁；
    // - 返回：无返回值，直接更新控件可见性。
    void updateCompactVisibility();

private:
    // 布局控件。
    QVBoxLayout* m_rootLayout = nullptr;   // m_rootLayout：根布局。
    QGridLayout* m_chartGridLayout = nullptr; // m_chartGridLayout：四宫格布局。
    QLabel* m_compactSummaryLabel = nullptr; // m_compactSummaryLabel：低高度时显示的文字摘要。
    QTimer* m_refreshTimer = nullptr;      // m_refreshTimer：性能采样定时器。

    // CPU/内存图控件。
    QChartView* m_cpuChartView = nullptr;     // m_cpuChartView：CPU 每核柱状图。
    QChartView* m_memoryTrendChartView = nullptr; // m_memoryTrendChartView：内存占用与组成合并趋势图。
    QBarSet* m_cpuCoreBarSet = nullptr;       // m_cpuCoreBarSet：CPU 每核当前柱条数据集。
    QBarSet* m_cpuCoreHistoryBarSet = nullptr; // m_cpuCoreHistoryBarSet：CPU 每核历史半透明柱条数据集。
    QLineSeries* m_memoryUsedSeries = nullptr; // m_memoryUsedSeries：已用组成面积上边界。
    QLineSeries* m_memoryStandbyTopSeries = nullptr; // m_memoryStandbyTopSeries：提交/缓存面积上边界。
    QLineSeries* m_memoryStandbyBaseSeries = nullptr; // m_memoryStandbyBaseSeries：提交/缓存面积下边界。
    QLineSeries* m_memoryAvailableTopSeries = nullptr; // m_memoryAvailableTopSeries：可用面积上边界。
    QLineSeries* m_memoryAvailableBaseSeries = nullptr; // m_memoryAvailableBaseSeries：可用面积下边界。
    QLineSeries* m_memoryUsageSeries = nullptr; // m_memoryUsageSeries：内存占用历史折线。
    QValueAxis* m_memoryTrendAxisX = nullptr; // m_memoryTrendAxisX：内存合并趋势 X 轴。
    QValueAxis* m_memoryTrendAxisY = nullptr; // m_memoryTrendAxisY：内存合并趋势 Y 轴。
    std::vector<double> m_cpuCoreHistoryPercentList; // m_cpuCoreHistoryPercentList：每核历史平均占用率。

    // 磁盘/网络图控件。
    QChartView* m_diskChartView = nullptr;    // m_diskChartView：磁盘读写折线图。
    QChartView* m_networkChartView = nullptr; // m_networkChartView：网络上下行折线图。
    QLineSeries* m_diskReadSeries = nullptr;  // m_diskReadSeries：磁盘读速率序列。
    QLineSeries* m_diskWriteSeries = nullptr; // m_diskWriteSeries：磁盘写速率序列。
    QLineSeries* m_networkRxSeries = nullptr; // m_networkRxSeries：网络下行速率序列。
    QLineSeries* m_networkTxSeries = nullptr; // m_networkTxSeries：网络上行速率序列。
    QValueAxis* m_diskAxisX = nullptr;        // m_diskAxisX：磁盘图 X 轴。
    QValueAxis* m_diskAxisY = nullptr;        // m_diskAxisY：磁盘图 Y 轴。
    QValueAxis* m_networkAxisX = nullptr;     // m_networkAxisX：网络图 X 轴。
    QValueAxis* m_networkAxisY = nullptr;     // m_networkAxisY：网络图 Y 轴。

    // 隐藏/最小化与紧凑摘要状态只门控展示，采样和规范历史继续。
    MetricPresentationSample m_latestPresentation;
    bool m_hasMetricSample = false;
    bool m_chartPresentationDirty = false;
    bool m_chartThemeDirty = true;
    bool m_presentationExposed = false;
    bool m_compactTextOnly = false;

    // 历史采样状态。
    qint64 m_metricSampleTimeMs = 0; // 本帧共同采样时间，毫秒。
    bool m_metricDiskValid = true; // 磁盘采样有效性，失败保留断点。
    bool m_metricNetworkValid = true; // 网络采样有效性，失败保留断点。
    bool m_metricMemoryValid = true; // 内存组成查询有效位。
    int m_historyLength = 60;        // m_historyLength：折线图保留点数。
    int m_sampleCounter = 0;         // m_sampleCounter：当前采样序号。
    QString m_lastCompactSummaryText; // m_lastCompactSummaryText：最近一次采样摘要文本。
};
