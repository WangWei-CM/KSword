#pragma once

// ============================================================
// HardwareDock.h
// 作用：
// 1) 提供“概览/利用率/CPU/显卡/内存”五个侧边 Tab；
// 2) 利用率页按任务管理器风格实现“左侧缩略卡片 + 右侧详情页”；
// 3) CPU/显卡/内存页保留文本化详情，便于排障与审计。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>   // std::atomic_bool：异步探测任务互斥。
#include <cstdint>  // std::uint64_t：保存采样累计值与时间戳。
#include <vector>   // std::vector：保存每核图表与采样数据。

class CodeEditorWidget;
class MemoryCompositionHistoryWidget;
class PerformanceNavCard;
class QChartView;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineSeries;
class QListWidget;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTimer;
class QValueAxis;
class QVBoxLayout;
class QWidget;

class HardwareDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - parent：Qt 父控件；
    // - 作用：初始化全部硬件页签并启动周期采样。
    explicit HardwareDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止采样定时器并释放 PDH 查询句柄。
    ~HardwareDock() override;

protected:
    // resizeEvent 作用：
    // - 窗口尺寸变化时动态重排“利用率”页图表高度；
    // - 保证任务管理器风格页面尽量不出现滚动条。
    void resizeEvent(QResizeEvent* resizeEventPointer) override;

    // showEvent 作用：
    // - Dock 首次显示后再次触发布局重排；
    // - 修复“利用率->CPU 首次进入高度尚未就绪”导致核心图挤压的问题。
    void showEvent(QShowEvent* showEventPointer) override;

private:
    // CoreChartEntry：
    // - 作用：保存单个逻辑处理器迷你图表所需控件；
    // - 后续刷新时按索引快速更新对应曲线。
    struct CoreChartEntry
    {
        QWidget* containerWidget = nullptr; // containerWidget：单核图容器。
        QLabel* titleLabel = nullptr;       // titleLabel：显示“CPU n”标题。
        QChartView* chartView = nullptr;    // chartView：折线图视图。
        QLineSeries* lineSeries = nullptr;  // lineSeries：该核心利用率曲线。
        QValueAxis* axisX = nullptr;        // axisX：时间轴（隐藏标签）。
        QValueAxis* axisY = nullptr;        // axisY：百分比轴（隐藏标签）。
    };

    // GpuEngineChartEntry：
    // - 作用：保存 GPU 引擎小图所需控件和引擎键名；
    // - 用于在刷新阶段按“3D/Copy/Video Encode/Video Decode”更新。
    struct GpuEngineChartEntry
    {
        QString engineKeyText;          // engineKeyText：引擎内部键名（小写匹配用）。
        QString displayNameText;        // displayNameText：界面标题文本。
        QLabel* titleLabel = nullptr;   // titleLabel：每个引擎小图标题标签。
        QChartView* chartView = nullptr; // chartView：每个引擎小图视图。
        QLineSeries* lineSeries = nullptr; // lineSeries：引擎利用率曲线。
        QValueAxis* axisX = nullptr;    // axisX：引擎图 X 轴。
        QValueAxis* axisY = nullptr;    // axisY：引擎图 Y 轴。
    };

    // CpuPowerSnapshot：
    // - 作用：保存 CallNtPowerInformation 返回的每核频率信息；
    // - 字段用于 CPU 详情表逐列展示。
    struct CpuPowerSnapshot
    {
        std::uint32_t coreIndex = 0;  // coreIndex：逻辑处理器编号。
        std::uint32_t currentMhz = 0; // currentMhz：当前频率（MHz）。
        std::uint32_t maxMhz = 0;     // maxMhz：最大频率（MHz）。
        std::uint32_t limitMhz = 0;   // limitMhz：当前限频上限（MHz）。
    };

    // SystemPerformanceSnapshot：
    // - 作用：收敛系统全局性能统计，避免重复调用 Win32 API；
    // - 字段用于 CPU/内存详情页下方参数展示。
    struct SystemPerformanceSnapshot
    {
        std::uint32_t processCount = 0;        // processCount：系统进程总数。
        std::uint32_t threadCount = 0;         // threadCount：系统线程总数。
        std::uint32_t handleCount = 0;         // handleCount：系统句柄总数。
        std::uint64_t commitTotalBytes = 0;    // commitTotalBytes：已提交内存字节。
        std::uint64_t commitLimitBytes = 0;    // commitLimitBytes：可提交上限字节。
        std::uint64_t cachedBytes = 0;         // cachedBytes：系统缓存字节。
        std::uint64_t pagedPoolBytes = 0;      // pagedPoolBytes：分页池字节。
        std::uint64_t nonPagedPoolBytes = 0;   // nonPagedPoolBytes：非分页池字节。
    };

private:
    // ===================== UI 初始化 =====================
    void initializeUi();
    void initializeOverviewTab();
    void initializeUtilizationTab();
    void initializeUtilizationSidebarCards();
    void initializeUtilizationCpuSubTab();
    void initializeUtilizationMemorySubTab();
    void initializeUtilizationDiskSubTab();
    void initializeUtilizationNetworkSubTab();
    void initializeUtilizationGpuSubTab();
    void initializeCpuTab();
    void initializeGpuTab();
    void initializeMemoryTab();
    void initializeCoreCharts();
    void initializeConnections();
    void syncUtilizationSidebarSelection(int selectedRowIndex);
    void adjustUtilizationChartHeights();

    // ===================== 采样与刷新 =====================
    void initializePerformanceCounters();
    void refreshAllViews();
    bool samplePerCoreUsage(
        std::vector<double>* coreUsageOut,
        double* totalUsageOut);
    bool sampleCpuPowerInfo(std::vector<CpuPowerSnapshot>* powerInfoOut);
    bool sampleMemoryUsage(double* memoryUsagePercentOut);
    bool sampleDiskRate(double* readBytesPerSecOut, double* writeBytesPerSecOut);
    bool sampleNetworkRate(double* rxBytesPerSecOut, double* txBytesPerSecOut);
    bool sampleGpuUsage(double* gpuUsagePercentOut);
    bool sampleGpuMemoryInfoByDxgi();
    bool sampleSystemPerformanceSnapshot(SystemPerformanceSnapshot* snapshotOut) const;
    void updateOverviewText(double cpuUsagePercent, double memoryUsagePercent);
    void updateUtilizationView(
        const std::vector<double>& coreUsageList,
        double memoryUsagePercent,
        double diskReadBytesPerSec,
        double diskWriteBytesPerSec,
        double networkRxBytesPerSec,
        double networkTxBytesPerSec,
        double gpuUsagePercent);
    void updateUtilizationSidebarCards(
        double cpuUsagePercent,
        double memoryUsagePercent,
        double diskReadBytesPerSec,
        double diskWriteBytesPerSec,
        double networkRxBytesPerSec,
        double networkTxBytesPerSec,
        double gpuUsagePercent);
    void updateTaskManagerDetailLabels(
        const std::vector<double>& coreUsageList,
        const std::vector<CpuPowerSnapshot>& powerInfoList,
        double memoryUsagePercent,
        double diskReadBytesPerSec,
        double diskWriteBytesPerSec,
        double networkRxBytesPerSec,
        double networkTxBytesPerSec,
        double gpuUsagePercent);
    void updateCpuDetailTable(
        const std::vector<double>& coreUsageList,
        const std::vector<CpuPowerSnapshot>& powerInfoList);
    void appendCoreSeriesPoint(
        CoreChartEntry& chartEntry,
        double usagePercent);
    void appendGeneralSeriesPoint(
        QLineSeries* lineSeries,
        QValueAxis* axisX,
        QValueAxis* axisY,
        double sampleValue,
        double minAxisYValue = 0.0);
    // updateSharedSeriesAxisRange 作用：
    // - 给共用同一坐标轴的两条折线统一计算 X/Y 可见范围；
    // - 用两条曲线的可见历史峰值同步刷新纵向比例。
    void updateSharedSeriesAxisRange(
        QLineSeries* primaryLineSeries,
        QLineSeries* secondaryLineSeries,
        QValueAxis* axisX,
        QValueAxis* axisY,
        double minAxisYValue = 0.0);
    // rebuildDualRateNavCard 作用：
    // - 记录磁盘/网络双速率原始历史；
    // - 按当前可见历史峰值整体重算缩略图比例并整段重绘。
    void rebuildDualRateNavCard(
        PerformanceNavCard* navCard,
        std::vector<double>* primaryHistoryOut,
        std::vector<double>* secondaryHistoryOut,
        double primaryBytesPerSecond,
        double secondaryBytesPerSecond,
        double* upperBoundBytesPerSecondOut,
        const QString& subtitleText);
    QString formatRateText(double bytesPerSecondValue) const;
    void refreshStaticHardwareTexts(bool forceRefresh);
    void requestAsyncStaticInfoRefresh();
    void requestAsyncSensorRefresh();
    void refreshCpuTopologyStaticInfo();
    void refreshSystemVolumeInfo();

    // ===================== 文本采集 =====================
    QString buildOverviewStaticText() const;
    QString buildGpuStaticText() const;
    QString buildMemoryStaticText() const;
    QString buildCpuSensorText(bool forceRefresh);

private:
    // 顶层结构。
    QVBoxLayout* m_rootLayout = nullptr;    // m_rootLayout：根布局。
    QTabWidget* m_sideTabWidget = nullptr;  // m_sideTabWidget：侧边页签容器。
    QTimer* m_refreshTimer = nullptr;       // m_refreshTimer：1 秒采样定时器。

    // 概览页。
    QWidget* m_overviewPage = nullptr;           // m_overviewPage：概览 Tab。
    QVBoxLayout* m_overviewLayout = nullptr;     // m_overviewLayout：概览布局。
    QLabel* m_overviewSummaryLabel = nullptr;    // m_overviewSummaryLabel：实时摘要标签。
    CodeEditorWidget* m_overviewEditor = nullptr; // m_overviewEditor：静态硬件清单文本。

    // 利用率页（任务管理器风格）。
    QWidget* m_utilizationPage = nullptr;          // m_utilizationPage：利用率 Tab。
    QVBoxLayout* m_utilizationLayout = nullptr;    // m_utilizationLayout：利用率外层布局。
    QHBoxLayout* m_utilizationBodyLayout = nullptr; // m_utilizationBodyLayout：左右分栏布局。
    QListWidget* m_utilizationSidebarList = nullptr; // m_utilizationSidebarList：左侧性能卡片列表。
    QStackedWidget* m_utilizationDetailStack = nullptr; // m_utilizationDetailStack：右侧详情页栈。

    // 左侧性能卡片。
    PerformanceNavCard* m_cpuNavCard = nullptr;      // m_cpuNavCard：CPU 导航卡片。
    PerformanceNavCard* m_memoryNavCard = nullptr;   // m_memoryNavCard：内存导航卡片。
    PerformanceNavCard* m_diskNavCard = nullptr;     // m_diskNavCard：磁盘导航卡片。
    PerformanceNavCard* m_networkNavCard = nullptr;  // m_networkNavCard：网络导航卡片。
    PerformanceNavCard* m_gpuNavCard = nullptr;      // m_gpuNavCard：GPU 导航卡片。

    // 利用率详情页：CPU。
    QWidget* m_utilizationCpuSubPage = nullptr;    // m_utilizationCpuSubPage：CPU 详情页。
    QLabel* m_cpuModelLabel = nullptr;             // m_cpuModelLabel：CPU 型号标签。
    QLabel* m_utilizationSummaryLabel = nullptr;   // m_utilizationSummaryLabel：CPU 图表说明文本。
    QScrollArea* m_coreChartScrollArea = nullptr;  // m_coreChartScrollArea：CPU 核心图滚动容器。
    QWidget* m_coreChartHostWidget = nullptr;      // m_coreChartHostWidget：CPU 核心图宿主。
    QGridLayout* m_coreChartGridLayout = nullptr;  // m_coreChartGridLayout：CPU 核心图矩阵布局。
    QLabel* m_cpuUtilPrimaryDetailLabel = nullptr; // m_cpuUtilPrimaryDetailLabel：CPU 左侧参数文本。
    QLabel* m_cpuUtilSecondaryDetailLabel = nullptr; // m_cpuUtilSecondaryDetailLabel：CPU 右侧参数文本。

    // 利用率详情页：内存。
    QWidget* m_utilizationMemorySubPage = nullptr;   // m_utilizationMemorySubPage：内存详情页。
    QLabel* m_memoryCapacityLabel = nullptr;         // m_memoryCapacityLabel：内存总量标签。
    QLabel* m_memoryUtilSummaryLabel = nullptr;      // m_memoryUtilSummaryLabel：内存摘要文本。
    MemoryCompositionHistoryWidget* m_memoryCompositionHistoryWidget = nullptr; // m_memoryCompositionHistoryWidget：内存历史与构成合并图。
    QLabel* m_memoryUtilPrimaryDetailLabel = nullptr; // m_memoryUtilPrimaryDetailLabel：内存左侧参数文本。
    QLabel* m_memoryUtilSecondaryDetailLabel = nullptr; // m_memoryUtilSecondaryDetailLabel：内存右侧参数文本。

    // 利用率详情页：磁盘。
    QWidget* m_utilizationDiskSubPage = nullptr;   // m_utilizationDiskSubPage：磁盘详情页。
    QLabel* m_diskUtilSummaryLabel = nullptr;      // m_diskUtilSummaryLabel：磁盘摘要文本。
    QChartView* m_diskUtilChartView = nullptr;     // m_diskUtilChartView：磁盘趋势图视图。
    QLineSeries* m_diskReadLineSeries = nullptr;   // m_diskReadLineSeries：磁盘读速率折线。
    QLineSeries* m_diskWriteLineSeries = nullptr;  // m_diskWriteLineSeries：磁盘写速率折线。
    QValueAxis* m_diskUtilAxisX = nullptr;         // m_diskUtilAxisX：磁盘图 X 轴。
    QValueAxis* m_diskUtilAxisY = nullptr;         // m_diskUtilAxisY：磁盘图 Y 轴。
    QLabel* m_diskUtilDetailLabel = nullptr;       // m_diskUtilDetailLabel：磁盘参数文本。

    // 利用率详情页：网络。
    QWidget* m_utilizationNetworkSubPage = nullptr; // m_utilizationNetworkSubPage：网络详情页。
    QLabel* m_networkUtilSummaryLabel = nullptr;    // m_networkUtilSummaryLabel：网络摘要文本。
    QChartView* m_networkUtilChartView = nullptr;   // m_networkUtilChartView：网络趋势图视图。
    QLineSeries* m_networkRxLineSeries = nullptr;   // m_networkRxLineSeries：网络下行折线。
    QLineSeries* m_networkTxLineSeries = nullptr;   // m_networkTxLineSeries：网络上行折线。
    QValueAxis* m_networkUtilAxisX = nullptr;       // m_networkUtilAxisX：网络图 X 轴。
    QValueAxis* m_networkUtilAxisY = nullptr;       // m_networkUtilAxisY：网络图 Y 轴。
    QLabel* m_networkUtilDetailLabel = nullptr;     // m_networkUtilDetailLabel：网络参数文本。

    // 利用率详情页：GPU。
    QWidget* m_utilizationGpuSubPage = nullptr;    // m_utilizationGpuSubPage：GPU 详情页。
    QLabel* m_gpuAdapterTitleLabel = nullptr;      // m_gpuAdapterTitleLabel：GPU 标题区适配器文本。
    QLabel* m_gpuUtilSummaryLabel = nullptr;       // m_gpuUtilSummaryLabel：GPU 摘要文本。
    QWidget* m_gpuEngineHostWidget = nullptr;      // m_gpuEngineHostWidget：GPU 引擎小图宿主。
    QGridLayout* m_gpuEngineGridLayout = nullptr;  // m_gpuEngineGridLayout：GPU 引擎小图网格。
    std::vector<GpuEngineChartEntry> m_gpuEngineCharts; // m_gpuEngineCharts：GPU 引擎图列表。
    QChartView* m_gpuDedicatedMemoryChartView = nullptr; // m_gpuDedicatedMemoryChartView：专用显存曲线图。
    QLineSeries* m_gpuDedicatedMemoryLineSeries = nullptr; // m_gpuDedicatedMemoryLineSeries：专用显存使用曲线。
    QValueAxis* m_gpuDedicatedMemoryAxisX = nullptr; // m_gpuDedicatedMemoryAxisX：专用显存图 X 轴。
    QValueAxis* m_gpuDedicatedMemoryAxisY = nullptr; // m_gpuDedicatedMemoryAxisY：专用显存图 Y 轴。
    QChartView* m_gpuSharedMemoryChartView = nullptr; // m_gpuSharedMemoryChartView：共享显存曲线图。
    QLineSeries* m_gpuSharedMemoryLineSeries = nullptr; // m_gpuSharedMemoryLineSeries：共享显存使用曲线。
    QValueAxis* m_gpuSharedMemoryAxisX = nullptr;   // m_gpuSharedMemoryAxisX：共享显存图 X 轴。
    QValueAxis* m_gpuSharedMemoryAxisY = nullptr;   // m_gpuSharedMemoryAxisY：共享显存图 Y 轴。
    QLabel* m_gpuUtilDetailLabel = nullptr;        // m_gpuUtilDetailLabel：GPU 参数文本。

    // 每核图缓存。
    std::vector<CoreChartEntry> m_coreChartEntries; // m_coreChartEntries：每核图控件缓存。
    int m_cpuCoreGridColumnCount = 1;             // m_cpuCoreGridColumnCount：CPU 小图网格列数。
    int m_cpuCoreGridRowCount = 1;                // m_cpuCoreGridRowCount：CPU 小图网格行数。

    // CPU 详情页（原有文本/表格页）。
    QWidget* m_cpuPage = nullptr;             // m_cpuPage：CPU Tab。
    QVBoxLayout* m_cpuLayout = nullptr;       // m_cpuLayout：CPU 布局。
    QLabel* m_cpuDetailLabel = nullptr;       // m_cpuDetailLabel：温度/电压等摘要标签。
    QTableWidget* m_cpuDetailTable = nullptr; // m_cpuDetailTable：每核详情表。

    // 显卡与内存页（原有文本页）。
    QWidget* m_gpuPage = nullptr;               // m_gpuPage：显卡 Tab。
    QVBoxLayout* m_gpuLayout = nullptr;         // m_gpuLayout：显卡布局。
    CodeEditorWidget* m_gpuEditor = nullptr;    // m_gpuEditor：显卡详情文本。
    QWidget* m_memoryPage = nullptr;            // m_memoryPage：内存 Tab。
    QVBoxLayout* m_memoryLayout = nullptr;      // m_memoryLayout：内存布局。
    CodeEditorWidget* m_memoryEditor = nullptr; // m_memoryEditor：内存详情文本。

    // 运行状态缓存。
    int m_historyLength = 60;                 // m_historyLength：曲线保留点数。
    int m_sampleCounter = 60;                 // m_sampleCounter：采样序号（从历史长度起步避免首段无图）。
    QString m_cachedSensorText;               // m_cachedSensorText：CPU 温度/电压缓存。
    QString m_lastSensorLogSignatureText;     // m_lastSensorLogSignatureText：最近一次传感器日志去重签名。
    QString m_cachedOverviewStaticText;       // m_cachedOverviewStaticText：概览静态文本缓存。
    QString m_cachedGpuStaticText;            // m_cachedGpuStaticText：显卡静态文本缓存。
    QString m_cachedMemoryStaticText;         // m_cachedMemoryStaticText：内存静态文本缓存。
    std::atomic_bool m_staticInfoRefreshing{ false }; // m_staticInfoRefreshing：静态信息异步刷新锁。
    std::atomic_bool m_sensorRefreshing{ false };     // m_sensorRefreshing：传感器异步刷新锁。
    bool m_initialSamplingStarted = false;            // m_initialSamplingStarted：首次显示时是否已启动首轮采样。
    std::uint64_t m_lastNetworkRxBytes = 0;           // m_lastNetworkRxBytes：上次网络接收累计字节。
    std::uint64_t m_lastNetworkTxBytes = 0;           // m_lastNetworkTxBytes：上次网络发送累计字节。
    qint64 m_lastNetworkSampleMs = 0;                 // m_lastNetworkSampleMs：上次网络采样时间戳(ms)。
    QString m_primaryNetworkAdapterName;              // m_primaryNetworkAdapterName：当前主活跃网卡名称。
    std::uint64_t m_primaryNetworkLinkBitsPerSecond = 0; // m_primaryNetworkLinkBitsPerSecond：主网卡链路速率。

    // 任务管理器详情态缓存。
    QString m_cpuModelText;                  // m_cpuModelText：CPU 型号文本。
    int m_cpuPackageCount = 0;               // m_cpuPackageCount：CPU 插槽数量。
    int m_cpuPhysicalCoreCount = 0;          // m_cpuPhysicalCoreCount：物理核心数量。
    int m_cpuLogicalCoreCount = 0;           // m_cpuLogicalCoreCount：逻辑核心数量。
    std::uint64_t m_cpuL1CacheBytes = 0;     // m_cpuL1CacheBytes：L1 缓存总字节。
    std::uint64_t m_cpuL2CacheBytes = 0;     // m_cpuL2CacheBytes：L2 缓存总字节。
    std::uint64_t m_cpuL3CacheBytes = 0;     // m_cpuL3CacheBytes：L3 缓存总字节。
    double m_lastCpuSpeedGhz = 0.0;          // m_lastCpuSpeedGhz：最近一次 CPU 速度（GHz）。

    int m_memorySpeedMhz = 0;                // m_memorySpeedMhz：内存主频（MHz）。
    int m_memorySlotUsed = 0;                // m_memorySlotUsed：已用内存插槽数量。
    int m_memorySlotTotal = 0;               // m_memorySlotTotal：总内存插槽数量。
    QString m_memoryFormFactorText;          // m_memoryFormFactorText：内存外形规格文本。

    QString m_gpuAdapterNameText;            // m_gpuAdapterNameText：GPU 适配器名称。
    QString m_gpuDriverVersionText;          // m_gpuDriverVersionText：GPU 驱动版本。
    QString m_gpuDriverDateText;             // m_gpuDriverDateText：GPU 驱动日期文本。
    QString m_gpuPnpDeviceIdText;            // m_gpuPnpDeviceIdText：GPU PNP 设备ID。
    double m_gpuDedicatedMemoryGiB = 0.0;    // m_gpuDedicatedMemoryGiB：GPU 专用显存（GiB）。
    double m_gpuUsage3DPercent = 0.0;        // m_gpuUsage3DPercent：3D 引擎利用率。
    double m_gpuUsageCopyPercent = 0.0;      // m_gpuUsageCopyPercent：Copy 引擎利用率。
    double m_gpuUsageVideoEncodePercent = 0.0; // m_gpuUsageVideoEncodePercent：视频编码引擎利用率。
    double m_gpuUsageVideoDecodePercent = 0.0; // m_gpuUsageVideoDecodePercent：视频解码引擎利用率。
    double m_gpuDedicatedUsedGiB = 0.0;      // m_gpuDedicatedUsedGiB：专用显存当前使用量。
    double m_gpuDedicatedBudgetGiB = 0.0;    // m_gpuDedicatedBudgetGiB：专用显存预算上限。
    double m_gpuSharedUsedGiB = 0.0;         // m_gpuSharedUsedGiB：共享显存当前使用量。
    double m_gpuSharedBudgetGiB = 0.0;       // m_gpuSharedBudgetGiB：共享显存预算上限。

    QString m_systemVolumeText;              // m_systemVolumeText：系统盘卷标文本。
    std::uint64_t m_systemVolumeTotalBytes = 0; // m_systemVolumeTotalBytes：系统盘总容量字节。
    std::uint64_t m_systemVolumeFreeBytes = 0;  // m_systemVolumeFreeBytes：系统盘剩余容量字节。

    double m_diskNavAutoScaleBytesPerSec = 1024.0 * 1024.0; // m_diskNavAutoScaleBytesPerSec：磁盘卡片动态缩放上限。
    double m_networkNavAutoScaleBytesPerSec = 1024.0 * 1024.0; // m_networkNavAutoScaleBytesPerSec：网络卡片动态缩放上限。
    std::vector<double> m_memoryNavUsedHistoryPercent; // m_memoryNavUsedHistoryPercent：内存已用缩略图历史。
    std::vector<double> m_memoryNavCachedHistoryPercent; // m_memoryNavCachedHistoryPercent：内存缓存/池缩略图历史。
    std::vector<double> m_diskNavReadHistoryBytesPerSec; // m_diskNavReadHistoryBytesPerSec：磁盘读取缩略图原始速率历史。
    std::vector<double> m_diskNavWriteHistoryBytesPerSec; // m_diskNavWriteHistoryBytesPerSec：磁盘写入缩略图原始速率历史。
    std::vector<double> m_networkNavRxHistoryBytesPerSec; // m_networkNavRxHistoryBytesPerSec：网络下行缩略图原始速率历史。
    std::vector<double> m_networkNavTxHistoryBytesPerSec; // m_networkNavTxHistoryBytesPerSec：网络上行缩略图原始速率历史。

    // PDH 性能计数器句柄（用 void* 规避头文件引入 Windows 细节）。
    void* m_cpuPerfQueryHandle = nullptr;     // m_cpuPerfQueryHandle：PDH 查询句柄。
    std::vector<void*> m_coreCounterHandles;  // m_coreCounterHandles：每核利用率计数器句柄。
    void* m_diskPerfQueryHandle = nullptr;    // m_diskPerfQueryHandle：磁盘速率查询句柄。
    void* m_diskReadCounterHandle = nullptr;  // m_diskReadCounterHandle：磁盘读速率计数器句柄。
    void* m_diskWriteCounterHandle = nullptr; // m_diskWriteCounterHandle：磁盘写速率计数器句柄。
    void* m_gpuPerfQueryHandle = nullptr;     // m_gpuPerfQueryHandle：GPU 利用率查询句柄。
    void* m_gpuCounterHandle = nullptr;       // m_gpuCounterHandle：GPU 引擎利用率计数器句柄。
};
