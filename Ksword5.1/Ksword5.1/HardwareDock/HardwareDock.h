#pragma once

// ============================================================
// HardwareDock.h
// 作用：
// 1) 提供“利用率/概览/CPU/显卡/内存/硬盘监控”等侧边 Tab，并让利用率拥有最高优先级；
// 2) 利用率页按任务管理器风格实现“左侧缩略卡片 + 右侧详情页”；
// 3) CPU/显卡/内存页保留文本化详情，便于排障与审计。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>   // std::atomic_bool：异步探测任务互斥。
#include <cstdint>  // std::uint64_t：保存采样累计值与时间戳。
#include <vector>   // std::vector：保存每核图表与采样数据。

class CodeEditorWidget;
class DiskMonitorPage;
class MemoryCompositionHistoryWidget;
class HardwareOtherDevicesPage;
class PerformanceNavCard;
class QChartView;
class QAreaSeries;
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
        QLineSeries* baselineSeries = nullptr; // baselineSeries：沿 X 轴的 0% 基准线，用于填充曲线下方面积。
        QAreaSeries* areaSeries = nullptr;   // areaSeries：折线与 X 轴围成的填充区域。
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
        QLineSeries* baselineSeries = nullptr; // baselineSeries：引擎图 X 轴基准线，用于面积填充闭合。
        QAreaSeries* areaSeries = nullptr; // areaSeries：引擎利用率折线下方填充区域。
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

    // UtilizationDeviceKind：
    // - 作用：标识“利用率”左侧卡片对应的详情页类型；
    // - 处理逻辑：CPU/内存固定存在，磁盘/网卡/GPU按设备动态扩展；
    // - 返回行为：枚举本身无返回值，仅用于导航状态同步。
    enum class UtilizationDeviceKind
    {
        Cpu,     // Cpu：CPU固定详情页。
        Memory,  // Memory：内存固定详情页。
        Disk,    // Disk：单个物理磁盘详情页。
        Network, // Network：单个网络接口详情页。
        Gpu      // Gpu：单个DXGI显卡适配器详情页。
    };

    // UtilizationNavEntry：
    // - 作用：绑定左侧 PerformanceNavCard 与右侧 QStackedWidget 页面；
    // - 输入来源：initializeUtilizationSidebarCards 和动态设备发现逻辑；
    // - 返回行为：结构体仅保存指针和索引，不负责释放 Qt 对象。
    struct UtilizationNavEntry
    {
        PerformanceNavCard* navCard = nullptr;       // navCard：左侧导航卡片控件。
        QWidget* detailPage = nullptr;               // detailPage：右侧详情页控件。
        UtilizationDeviceKind kind = UtilizationDeviceKind::Cpu; // kind：设备类型。
        int deviceIndex = -1;                        // deviceIndex：同类设备数组索引。
    };

    // DiskRateSample：
    // - 作用：保存一次单磁盘 PDH 读写速率采样；
    // - 调用方式：sampleDiskRates 填充列表，刷新阶段按 instanceNameText 匹配页面；
    // - 返回行为：结构体无方法，字段均为输出数据。
    struct DiskRateSample
    {
        QString instanceNameText;       // instanceNameText：PDH PhysicalDisk 实例名。
        QString displayNameText;        // displayNameText：界面展示名，例如“磁盘 0 (C:)”。
        double readBytesPerSec = 0.0;   // readBytesPerSec：读取字节每秒。
        double writeBytesPerSec = 0.0;  // writeBytesPerSec：写入字节每秒。
    };

    // NetworkRateSample：
    // - 作用：保存一次单网卡收发速率采样；
    // - 调用方式：sampleNetworkRates 读取 GetIfTable2 并按接口 LUID 计算增量；
    // - 返回行为：字段直接供 UI 卡片和详情页刷新。
    struct NetworkRateSample
    {
        std::uint64_t interfaceKey = 0;             // interfaceKey：网络接口 LUID 打包键。
        QString displayNameText;                   // displayNameText：网卡别名或描述。
        std::uint64_t linkBitsPerSecond = 0;        // linkBitsPerSecond：链路速率 bit/s。
        double rxBytesPerSec = 0.0;                 // rxBytesPerSec：接收字节每秒。
        double txBytesPerSec = 0.0;                 // txBytesPerSec：发送字节每秒。
        std::uint64_t totalRxBytes = 0;             // totalRxBytes：系统累计接收字节。
        std::uint64_t totalTxBytes = 0;             // totalTxBytes：系统累计发送字节。
    };

    // GpuUsageSample：
    // - 作用：保存一次单 GPU 的引擎利用率和显存采样；
    // - 调用方式：sampleGpuUsages 先枚举 DXGI 适配器，再合并 PDH GPU Engine 数据；
    // - 返回行为：刷新阶段按 adapterKey 更新对应 GPU 页面。
    struct GpuUsageSample
    {
        std::uint64_t adapterKey = 0;              // adapterKey：DXGI LUID 打包键。
        int adapterIndex = 0;                      // adapterIndex：DXGI 枚举序号。
        QString displayNameText;                  // displayNameText：显卡名称。
        double overallUsagePercent = 0.0;         // overallUsagePercent：总体近似利用率。
        double usage3DPercent = 0.0;              // usage3DPercent：3D 引擎利用率。
        double usageCopyPercent = 0.0;            // usageCopyPercent：Copy 引擎利用率。
        double usageVideoEncodePercent = 0.0;     // usageVideoEncodePercent：视频编码利用率。
        double usageVideoDecodePercent = 0.0;     // usageVideoDecodePercent：视频解码利用率。
        double dedicatedMemoryGiB = 0.0;          // dedicatedMemoryGiB：专用显存总量 GiB。
        double dedicatedUsedGiB = 0.0;            // dedicatedUsedGiB：专用显存已用 GiB。
        double dedicatedBudgetGiB = 0.0;          // dedicatedBudgetGiB：专用显存预算 GiB。
        double sharedUsedGiB = 0.0;               // sharedUsedGiB：共享显存已用 GiB。
        double sharedBudgetGiB = 0.0;             // sharedBudgetGiB：共享显存预算 GiB。
    };

    // DiskUtilizationDevice：
    // - 作用：保存一个磁盘卡片、详情页和历史缩略图状态；
    // - 调用方式：ensureDiskUtilizationDevice 在发现新 PDH 实例时创建；
    // - 返回行为：结构体由 HardwareDock 持有，Qt 对象仍走父子树释放。
    struct DiskUtilizationDevice
    {
        QString instanceNameText;                 // instanceNameText：PDH PhysicalDisk 实例名。
        QString displayNameText;                  // displayNameText：左侧卡片标题。
        QWidget* pageWidget = nullptr;            // pageWidget：右侧详情页。
        QLabel* summaryLabel = nullptr;           // summaryLabel：读写摘要。
        QChartView* chartView = nullptr;          // chartView：读写趋势图。
        QLineSeries* readLineSeries = nullptr;    // readLineSeries：读取速率折线。
        QLineSeries* readBaselineSeries = nullptr; // readBaselineSeries：读取折线的 0 轴基准线。
        QAreaSeries* readAreaSeries = nullptr;    // readAreaSeries：读取折线下方填充区域。
        QLineSeries* writeLineSeries = nullptr;   // writeLineSeries：写入速率折线。
        QLineSeries* writeBaselineSeries = nullptr; // writeBaselineSeries：写入折线的 0 轴基准线。
        QAreaSeries* writeAreaSeries = nullptr;   // writeAreaSeries：写入折线下方填充区域。
        QValueAxis* axisX = nullptr;              // axisX：趋势图 X 轴。
        QValueAxis* axisY = nullptr;              // axisY：趋势图 Y 轴。
        QLabel* detailLabel = nullptr;            // detailLabel：参数详情。
        PerformanceNavCard* navCard = nullptr;    // navCard：左侧导航卡片。
        double navAutoScaleBytesPerSec = 1024.0 * 1024.0; // navAutoScaleBytesPerSec：缩略图动态上限。
        std::vector<double> readHistoryBytesPerSec;  // readHistoryBytesPerSec：缩略图读取历史。
        std::vector<double> writeHistoryBytesPerSec; // writeHistoryBytesPerSec：缩略图写入历史。
    };

    // NetworkUtilizationDevice：
    // - 作用：保存一个网卡页面、速率历史和上次累计计数；
    // - 调用方式：sampleNetworkRates 会在 UI 线程内按 LUID 找到或创建；
    // - 返回行为：结构体不返回数据，刷新函数读取字段更新 UI。
    struct NetworkUtilizationDevice
    {
        std::uint64_t interfaceKey = 0;           // interfaceKey：网络接口 LUID 打包键。
        QString displayNameText;                 // displayNameText：网卡展示名。
        std::uint64_t linkBitsPerSecond = 0;      // linkBitsPerSecond：链路速率 bit/s。
        std::uint64_t lastRxBytes = 0;            // lastRxBytes：上次累计接收字节。
        std::uint64_t lastTxBytes = 0;            // lastTxBytes：上次累计发送字节。
        qint64 lastSampleMs = 0;                  // lastSampleMs：上次采样时间戳。
        bool hasPreviousSample = false;           // hasPreviousSample：是否已有增量基线。
        QWidget* pageWidget = nullptr;            // pageWidget：右侧详情页。
        QLabel* summaryLabel = nullptr;           // summaryLabel：收发摘要。
        QChartView* chartView = nullptr;          // chartView：收发趋势图。
        QLineSeries* rxLineSeries = nullptr;      // rxLineSeries：接收速率折线。
        QLineSeries* rxBaselineSeries = nullptr;  // rxBaselineSeries：接收折线的 0 轴基准线。
        QAreaSeries* rxAreaSeries = nullptr;      // rxAreaSeries：接收折线下方填充区域。
        QLineSeries* txLineSeries = nullptr;      // txLineSeries：发送速率折线。
        QLineSeries* txBaselineSeries = nullptr;  // txBaselineSeries：发送折线的 0 轴基准线。
        QAreaSeries* txAreaSeries = nullptr;      // txAreaSeries：发送折线下方填充区域。
        QValueAxis* axisX = nullptr;              // axisX：趋势图 X 轴。
        QValueAxis* axisY = nullptr;              // axisY：趋势图 Y 轴。
        QLabel* detailLabel = nullptr;            // detailLabel：参数详情。
        PerformanceNavCard* navCard = nullptr;    // navCard：左侧导航卡片。
        double navAutoScaleBytesPerSec = 1024.0 * 1024.0; // navAutoScaleBytesPerSec：缩略图动态上限。
        std::vector<double> rxHistoryBytesPerSec; // rxHistoryBytesPerSec：缩略图接收历史。
        std::vector<double> txHistoryBytesPerSec; // txHistoryBytesPerSec：缩略图发送历史。
    };

    // GpuUtilizationDevice：
    // - 作用：保存一个 GPU 适配器的详情页、引擎图和显存图控件；
    // - 调用方式：ensureGpuUtilizationDevice 在 DXGI 发现新适配器时创建；
    // - 返回行为：字段由 updateGpuUtilizationDevice 读取并刷新。
    struct GpuUtilizationDevice
    {
        std::uint64_t adapterKey = 0;             // adapterKey：DXGI LUID 打包键。
        bool adapterKeyAssigned = false;          // adapterKeyAssigned：adapterKey 是否已经绑定真实设备。
        int adapterIndex = 0;                     // adapterIndex：DXGI 枚举序号。
        QString displayNameText;                 // displayNameText：显卡名称。
        QWidget* pageWidget = nullptr;            // pageWidget：右侧详情页。
        QLabel* adapterTitleLabel = nullptr;      // adapterTitleLabel：标题区适配器名。
        QLabel* summaryLabel = nullptr;           // summaryLabel：利用率摘要。
        QWidget* engineHostWidget = nullptr;      // engineHostWidget：引擎小图宿主。
        QGridLayout* engineGridLayout = nullptr;  // engineGridLayout：引擎小图网格。
        std::vector<GpuEngineChartEntry> engineCharts; // engineCharts：四类引擎图。
        QChartView* dedicatedMemoryChartView = nullptr; // dedicatedMemoryChartView：专用显存图。
        QLineSeries* dedicatedMemoryLineSeries = nullptr; // dedicatedMemoryLineSeries：专用显存折线。
        QLineSeries* dedicatedMemoryBaselineSeries = nullptr; // dedicatedMemoryBaselineSeries：专用显存 0 轴基准线。
        QAreaSeries* dedicatedMemoryAreaSeries = nullptr; // dedicatedMemoryAreaSeries：专用显存折线填充区域。
        QValueAxis* dedicatedMemoryAxisX = nullptr; // dedicatedMemoryAxisX：专用显存 X 轴。
        QValueAxis* dedicatedMemoryAxisY = nullptr; // dedicatedMemoryAxisY：专用显存 Y 轴。
        QChartView* sharedMemoryChartView = nullptr; // sharedMemoryChartView：共享显存图。
        QLineSeries* sharedMemoryLineSeries = nullptr; // sharedMemoryLineSeries：共享显存折线。
        QLineSeries* sharedMemoryBaselineSeries = nullptr; // sharedMemoryBaselineSeries：共享显存 0 轴基准线。
        QAreaSeries* sharedMemoryAreaSeries = nullptr; // sharedMemoryAreaSeries：共享显存折线填充区域。
        QValueAxis* sharedMemoryAxisX = nullptr;  // sharedMemoryAxisX：共享显存 X 轴。
        QValueAxis* sharedMemoryAxisY = nullptr;  // sharedMemoryAxisY：共享显存 Y 轴。
        QLabel* detailLabel = nullptr;            // detailLabel：参数详情。
        PerformanceNavCard* navCard = nullptr;    // navCard：左侧导航卡片。
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
    void initializeDiskMonitorTab();
    void initializeOtherDevicesTab();
    void initializeCoreCharts();
    void initializeConnections();
    void scheduleUtilizationLayoutRefresh();
    void syncUtilizationSidebarSelection(int selectedRowIndex);
    void adjustUtilizationChartHeights();
    PerformanceNavCard* addUtilizationSidebarCard(
        QWidget* detailPage,
        const QString& titleText,
        const QColor& accentColor,
        UtilizationDeviceKind kind,
        int deviceIndex);
    int findDiskUtilizationDeviceIndexByInstance(const QString& instanceNameText) const;
    int ensureDiskUtilizationDevice(const DiskRateSample& sample, int ordinalIndex);
    int findNetworkUtilizationDeviceIndexByKey(std::uint64_t interfaceKey) const;
    int ensureNetworkUtilizationDevice(const NetworkRateSample& sample, int ordinalIndex);
    int findGpuUtilizationDeviceIndexByKey(std::uint64_t adapterKey) const;
    int ensureGpuUtilizationDevice(const GpuUsageSample& sample, int ordinalIndex);
    void createDiskUtilizationDevicePage(DiskUtilizationDevice* devicePointer);
    void createNetworkUtilizationDevicePage(NetworkUtilizationDevice* devicePointer);
    void createGpuUtilizationDevicePage(GpuUtilizationDevice* devicePointer);

    // ===================== 采样与刷新 =====================
    void initializePerformanceCounters();
    void refreshAllViews();
    bool samplePerCoreUsage(
        std::vector<double>* coreUsageOut,
        double* totalUsageOut);
    bool sampleCpuPowerInfo(std::vector<CpuPowerSnapshot>* powerInfoOut);
    bool sampleMemoryUsage(double* memoryUsagePercentOut);
    bool sampleDiskRates(std::vector<DiskRateSample>* sampleListOut);
    bool sampleDiskRate(double* readBytesPerSecOut, double* writeBytesPerSecOut);
    bool sampleNetworkRates(std::vector<NetworkRateSample>* sampleListOut);
    bool sampleNetworkRate(double* rxBytesPerSecOut, double* txBytesPerSecOut);
    bool sampleGpuUsages(std::vector<GpuUsageSample>* sampleListOut);
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
    void updateAdditionalDiskUtilizationDevices(const std::vector<DiskRateSample>& sampleList);
    void updateAdditionalNetworkUtilizationDevices(const std::vector<NetworkRateSample>& sampleList);
    void updateAdditionalGpuUtilizationDevices(const std::vector<GpuUsageSample>& sampleList);
    void updateDiskUtilizationDevice(DiskUtilizationDevice& device, const DiskRateSample& sample);
    void updateNetworkUtilizationDevice(NetworkUtilizationDevice& device, const NetworkRateSample& sample);
    void updateGpuUtilizationDevice(GpuUtilizationDevice& device, const GpuUsageSample& sample);
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
    // appendFilledSeriesPoint 作用：
    // - 同步追加折线采样和 0 轴基准线采样；
    // - 保证 QAreaSeries 的填充区域能随历史窗口一起平移。
    void appendFilledSeriesPoint(
        QLineSeries* lineSeries,
        QLineSeries* baselineSeries,
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
    std::vector<UtilizationNavEntry> m_utilizationNavEntries; // m_utilizationNavEntries：左侧卡片到右侧页的映射。

    // 左侧性能卡片。
    PerformanceNavCard* m_cpuNavCard = nullptr;      // m_cpuNavCard：CPU 导航卡片。
    PerformanceNavCard* m_memoryNavCard = nullptr;   // m_memoryNavCard：内存导航卡片。
    PerformanceNavCard* m_diskNavCard = nullptr;     // m_diskNavCard：兼容旧聚合磁盘卡片，当前动态设备模式下为空。
    PerformanceNavCard* m_networkNavCard = nullptr;  // m_networkNavCard：兼容旧聚合网络卡片，当前动态设备模式下为空。
    PerformanceNavCard* m_gpuNavCard = nullptr;      // m_gpuNavCard：兼容旧聚合GPU卡片，当前动态设备模式下为空。

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
    QLineSeries* m_diskReadBaselineSeries = nullptr; // m_diskReadBaselineSeries：磁盘读速率 0 轴基准线。
    QAreaSeries* m_diskReadAreaSeries = nullptr;   // m_diskReadAreaSeries：磁盘读速率填充区域。
    QLineSeries* m_diskWriteLineSeries = nullptr;  // m_diskWriteLineSeries：磁盘写速率折线。
    QLineSeries* m_diskWriteBaselineSeries = nullptr; // m_diskWriteBaselineSeries：磁盘写速率 0 轴基准线。
    QAreaSeries* m_diskWriteAreaSeries = nullptr;  // m_diskWriteAreaSeries：磁盘写速率填充区域。
    QValueAxis* m_diskUtilAxisX = nullptr;         // m_diskUtilAxisX：磁盘图 X 轴。
    QValueAxis* m_diskUtilAxisY = nullptr;         // m_diskUtilAxisY：磁盘图 Y 轴。
    QLabel* m_diskUtilDetailLabel = nullptr;       // m_diskUtilDetailLabel：磁盘参数文本。

    // 利用率详情页：网络。
    QWidget* m_utilizationNetworkSubPage = nullptr; // m_utilizationNetworkSubPage：网络详情页。
    QLabel* m_networkUtilSummaryLabel = nullptr;    // m_networkUtilSummaryLabel：网络摘要文本。
    QChartView* m_networkUtilChartView = nullptr;   // m_networkUtilChartView：网络趋势图视图。
    QLineSeries* m_networkRxLineSeries = nullptr;   // m_networkRxLineSeries：网络下行折线。
    QLineSeries* m_networkRxBaselineSeries = nullptr; // m_networkRxBaselineSeries：网络下行 0 轴基准线。
    QAreaSeries* m_networkRxAreaSeries = nullptr;   // m_networkRxAreaSeries：网络下行填充区域。
    QLineSeries* m_networkTxLineSeries = nullptr;   // m_networkTxLineSeries：网络上行折线。
    QLineSeries* m_networkTxBaselineSeries = nullptr; // m_networkTxBaselineSeries：网络上行 0 轴基准线。
    QAreaSeries* m_networkTxAreaSeries = nullptr;   // m_networkTxAreaSeries：网络上行填充区域。
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
    QLineSeries* m_gpuDedicatedMemoryBaselineSeries = nullptr; // m_gpuDedicatedMemoryBaselineSeries：专用显存 0 轴基准线。
    QAreaSeries* m_gpuDedicatedMemoryAreaSeries = nullptr; // m_gpuDedicatedMemoryAreaSeries：专用显存填充区域。
    QValueAxis* m_gpuDedicatedMemoryAxisX = nullptr; // m_gpuDedicatedMemoryAxisX：专用显存图 X 轴。
    QValueAxis* m_gpuDedicatedMemoryAxisY = nullptr; // m_gpuDedicatedMemoryAxisY：专用显存图 Y 轴。
    QChartView* m_gpuSharedMemoryChartView = nullptr; // m_gpuSharedMemoryChartView：共享显存曲线图。
    QLineSeries* m_gpuSharedMemoryLineSeries = nullptr; // m_gpuSharedMemoryLineSeries：共享显存使用曲线。
    QLineSeries* m_gpuSharedMemoryBaselineSeries = nullptr; // m_gpuSharedMemoryBaselineSeries：共享显存 0 轴基准线。
    QAreaSeries* m_gpuSharedMemoryAreaSeries = nullptr; // m_gpuSharedMemoryAreaSeries：共享显存填充区域。
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
    DiskMonitorPage* m_diskMonitorPage = nullptr; // m_diskMonitorPage：硬盘监控 Tab。
    HardwareOtherDevicesPage* m_otherDevicesPage = nullptr; // m_otherDevicesPage：其他硬件设备内侧边 Tab。

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
    std::uint64_t m_lastNetworkRxBytes = 0;           // m_lastNetworkRxBytes：兼容旧聚合网络采样的累计接收字节。
    std::uint64_t m_lastNetworkTxBytes = 0;           // m_lastNetworkTxBytes：兼容旧聚合网络采样的累计发送字节。
    qint64 m_lastNetworkSampleMs = 0;                 // m_lastNetworkSampleMs：兼容旧聚合网络采样时间戳(ms)。
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

    double m_diskNavAutoScaleBytesPerSec = 1024.0 * 1024.0; // m_diskNavAutoScaleBytesPerSec：兼容旧聚合磁盘页的动态缩放上限。
    double m_networkNavAutoScaleBytesPerSec = 1024.0 * 1024.0; // m_networkNavAutoScaleBytesPerSec：兼容旧聚合网络页的动态缩放上限。
    std::vector<double> m_memoryNavUsedHistoryPercent; // m_memoryNavUsedHistoryPercent：内存已用缩略图历史。
    std::vector<double> m_memoryNavCachedHistoryPercent; // m_memoryNavCachedHistoryPercent：内存缓存/池缩略图历史。
    std::vector<double> m_diskNavReadHistoryBytesPerSec; // m_diskNavReadHistoryBytesPerSec：兼容旧聚合磁盘卡片读取历史。
    std::vector<double> m_diskNavWriteHistoryBytesPerSec; // m_diskNavWriteHistoryBytesPerSec：兼容旧聚合磁盘卡片写入历史。
    std::vector<double> m_networkNavRxHistoryBytesPerSec; // m_networkNavRxHistoryBytesPerSec：兼容旧聚合网络卡片下行历史。
    std::vector<double> m_networkNavTxHistoryBytesPerSec; // m_networkNavTxHistoryBytesPerSec：兼容旧聚合网络卡片上行历史。
    std::vector<DiskUtilizationDevice> m_diskUtilDevices; // m_diskUtilDevices：磁盘利用率多设备页。
    std::vector<NetworkUtilizationDevice> m_networkUtilDevices; // m_networkUtilDevices：网卡利用率多设备页。
    std::vector<GpuUtilizationDevice> m_gpuUtilDevices; // m_gpuUtilDevices：GPU利用率多设备页。

    // PDH 性能计数器句柄（用 void* 规避头文件引入 Windows 细节）。
    void* m_cpuPerfQueryHandle = nullptr;     // m_cpuPerfQueryHandle：PDH 查询句柄。
    std::vector<void*> m_coreCounterHandles;  // m_coreCounterHandles：每核利用率计数器句柄。
    void* m_diskPerfQueryHandle = nullptr;    // m_diskPerfQueryHandle：磁盘速率查询句柄。
    void* m_diskReadCounterHandle = nullptr;  // m_diskReadCounterHandle：磁盘读速率计数器句柄。
    void* m_diskWriteCounterHandle = nullptr; // m_diskWriteCounterHandle：磁盘写速率计数器句柄。
    void* m_gpuPerfQueryHandle = nullptr;     // m_gpuPerfQueryHandle：GPU 利用率查询句柄。
    void* m_gpuCounterHandle = nullptr;       // m_gpuCounterHandle：GPU 引擎利用率计数器句柄。
};
