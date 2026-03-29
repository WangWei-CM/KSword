#pragma once

// ============================================================
// HardwareDock.h
// 作用：
// 1) 新增硬件总览 Dock，提供“概览/利用率/CPU/显卡/内存”五个侧边 Tab；
// 2) 利用率页展示逻辑处理器折线图矩阵，风格贴近任务管理器性能页；
// 3) CPU/显卡/内存页提供详细硬件信息，便于运维排障。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>   // std::atomic_bool：异步探测任务互斥。
#include <cstdint>  // std::uint64_t：保存采样累计值与时间戳。
#include <vector>   // std::vector：保存每核图表与采样数据。

class CodeEditorWidget;
class QChartView;
class QGridLayout;
class QLabel;
class QLineSeries;
class QScrollArea;
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

    // CpuPowerSnapshot：
    // - 作用：保存 CallNtPowerInformation 返回的每核频率信息；
    // - 字段用于 CPU 详情表逐列展示。
    struct CpuPowerSnapshot
    {
        std::uint32_t coreIndex = 0; // coreIndex：逻辑处理器编号。
        std::uint32_t currentMhz = 0; // currentMhz：当前频率（MHz）。
        std::uint32_t maxMhz = 0;    // maxMhz：最大频率（MHz）。
        std::uint32_t limitMhz = 0;  // limitMhz：当前限频上限（MHz）。
    };

private:
    // ===================== UI 初始化 =====================
    void initializeUi();
    void initializeOverviewTab();
    void initializeUtilizationTab();
    void initializeCpuTab();
    void initializeGpuTab();
    void initializeMemoryTab();
    void initializeCoreCharts();
    void initializeConnections();

    // ===================== 采样与刷新 =====================
    void initializePerformanceCounters();
    void refreshAllViews();
    bool samplePerCoreUsage(
        std::vector<double>* coreUsageOut,
        double* totalUsageOut);
    bool sampleCpuPowerInfo(std::vector<CpuPowerSnapshot>* powerInfoOut);
    bool sampleMemoryUsage(double* memoryUsagePercentOut);
    void updateOverviewText(double cpuUsagePercent, double memoryUsagePercent);
    void updateUtilizationView(
        const std::vector<double>& coreUsageList,
        double memoryUsagePercent);
    void updateCpuDetailTable(
        const std::vector<double>& coreUsageList,
        const std::vector<CpuPowerSnapshot>& powerInfoList);
    void appendCoreSeriesPoint(
        CoreChartEntry& chartEntry,
        double usagePercent);
    void refreshStaticHardwareTexts(bool forceRefresh);
    void requestAsyncStaticInfoRefresh();
    void requestAsyncSensorRefresh();

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
    QWidget* m_overviewPage = nullptr;          // m_overviewPage：概览 Tab。
    QVBoxLayout* m_overviewLayout = nullptr;    // m_overviewLayout：概览布局。
    QLabel* m_overviewSummaryLabel = nullptr;   // m_overviewSummaryLabel：实时摘要标签。
    CodeEditorWidget* m_overviewEditor = nullptr; // m_overviewEditor：静态硬件清单文本。

    // 利用率页。
    QWidget* m_utilizationPage = nullptr;         // m_utilizationPage：利用率 Tab。
    QVBoxLayout* m_utilizationLayout = nullptr;   // m_utilizationLayout：利用率布局。
    QLabel* m_utilizationSummaryLabel = nullptr;  // m_utilizationSummaryLabel：总览标签。
    QScrollArea* m_coreChartScrollArea = nullptr; // m_coreChartScrollArea：核心图滚动容器。
    QWidget* m_coreChartHostWidget = nullptr;     // m_coreChartHostWidget：核心图宿主。
    QGridLayout* m_coreChartGridLayout = nullptr; // m_coreChartGridLayout：核心图矩阵布局。
    std::vector<CoreChartEntry> m_coreChartEntries; // m_coreChartEntries：每核图控件缓存。

    // CPU 详情页。
    QWidget* m_cpuPage = nullptr;             // m_cpuPage：CPU Tab。
    QVBoxLayout* m_cpuLayout = nullptr;       // m_cpuLayout：CPU 布局。
    QLabel* m_cpuDetailLabel = nullptr;       // m_cpuDetailLabel：温度/电压等摘要标签。
    QTableWidget* m_cpuDetailTable = nullptr; // m_cpuDetailTable：每核详情表。

    // 显卡与内存页。
    QWidget* m_gpuPage = nullptr;               // m_gpuPage：显卡 Tab。
    QVBoxLayout* m_gpuLayout = nullptr;         // m_gpuLayout：显卡布局。
    CodeEditorWidget* m_gpuEditor = nullptr;    // m_gpuEditor：显卡详情文本。
    QWidget* m_memoryPage = nullptr;            // m_memoryPage：内存 Tab。
    QVBoxLayout* m_memoryLayout = nullptr;      // m_memoryLayout：内存布局。
    CodeEditorWidget* m_memoryEditor = nullptr; // m_memoryEditor：内存详情文本。

    // 运行状态缓存。
    int m_historyLength = 60;                 // m_historyLength：单核曲线保留点数。
    int m_sampleCounter = 0;                  // m_sampleCounter：采样序号。
    QString m_cachedSensorText;               // m_cachedSensorText：CPU 温度/电压缓存。
    QString m_cachedOverviewStaticText;       // m_cachedOverviewStaticText：概览静态文本缓存。
    QString m_cachedGpuStaticText;            // m_cachedGpuStaticText：显卡静态文本缓存。
    QString m_cachedMemoryStaticText;         // m_cachedMemoryStaticText：内存静态文本缓存。
    std::atomic_bool m_staticInfoRefreshing{ false }; // m_staticInfoRefreshing：静态信息异步刷新锁。
    std::atomic_bool m_sensorRefreshing{ false };     // m_sensorRefreshing：传感器异步刷新锁。

    // PDH 性能计数器句柄（用 void* 规避头文件引入 Windows 细节）。
    void* m_cpuPerfQueryHandle = nullptr;           // m_cpuPerfQueryHandle：PDH 查询句柄。
    std::vector<void*> m_coreCounterHandles;        // m_coreCounterHandles：每核利用率计数器句柄。
};
