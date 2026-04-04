#pragma once

// ============================================================
// MonitorDock.h
// 作用：
// 1) 实现监控页“WMI / ETW”双侧边栏 Tab；
// 2) 提供 WMI Provider 枚举、事件类选择、订阅控制与事件展示；
// 3) 提供 ETW Provider 枚举、参数配置、实时结果表与导出能力。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>      // std::atomic_bool：后台订阅状态控制。
#include <cstdint>     // std::uint32_t：PID 等固定宽度整数。
#include <memory>      // std::unique_ptr：线程对象托管。
#include <mutex>       // std::mutex：ETW 待刷新队列并发保护。
#include <string>      // std::string：日志输出与 COM 文本桥接。
#include <thread>      // std::thread：WMI 订阅后台线程。
#include <vector>      // std::vector：Provider/事件快照容器。

// Qt 前置声明：减少头文件编译依赖。
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QSpinBox;
class QStandardItemModel;
class QTableWidget;
class QTableWidgetItem;
class QTableView;
class QTabWidget;
class QTimer;
class QToolBox;
class QVBoxLayout;
class QGridLayout;
class QShowEvent;
class QBarSet;
class QChartView;
class QLineSeries;
class QValueAxis;

// COM 前置声明：避免在头文件引入大量 WMI 头。
struct IWbemClassObject;
struct IWbemLocator;
struct IWbemServices;
struct IEnumWbemClassObject;
struct _EVENT_RECORD;

class MonitorDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化 WMI/ETW 页面、连接信号槽并执行首轮枚举。
    // - 参数 parent：Qt 父控件。
    explicit MonitorDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止后台线程与定时器，确保资源释放。
    ~MonitorDock() override;

protected:
    // showEvent：
    // - 首次显示时再触发 WMI/ETW Provider 枚举；
    // - 避免主窗口启动阶段并发拉起多路后台发现任务。
    void showEvent(QShowEvent* event) override;

private:
    // WmiProviderEntry：
    // - 作用：保存单个 WMI Provider 表格行信息。
    struct WmiProviderEntry
    {
        QString providerName;      // Provider 名称。
        QString nameSpaceText;     // 命名空间。
        QString clsidText;         // CLSID 文本。
        int eventClassCount = 0;   // 支持事件类数量。
        bool subscribable = false; // 是否可订阅。
    };

    // EtwProviderEntry：
    // - 作用：保存 ETW Provider 基础信息。
    struct EtwProviderEntry
    {
        QString providerName;      // Provider 名称。
        QString providerGuidText;  // Provider GUID 字符串。
    };

private:
    // ========================= UI 初始化 =========================
    void initializeUi();
    void initializePerformancePanel();
    void initializeWmiTab();
    void initializeEtwTab();
    void initializeConnections();
    void refreshPerformanceCharts();
    bool sampleCpuUsage(double* cpuUsageOut);
    bool sampleDiskRate(double* readBytesPerSecOut, double* writeBytesPerSecOut);
    bool sampleNetworkRate(double* rxBytesPerSecOut, double* txBytesPerSecOut);
    void appendLineSample(
        QLineSeries* series,
        QValueAxis* axisX,
        QValueAxis* axisY,
        double value);

    // ========================= WMI 功能 ==========================
    void refreshWmiProvidersAsync();
    void refreshWmiEventClassesAsync();
    // updateWmiSubscribePanelCompactLayout：
    // - 作用：按当前事件类数量动态收敛“WMI订阅”右侧面板中的事件类表高度；
    // - 调用：初始化订阅UI后调用一次，事件类刷新完成后再次调用；
    // - 入参/出参：无（直接读取并更新成员控件尺寸）。
    void updateWmiSubscribePanelCompactLayout();
    void applyWmiProviderFilter();
    void startWmiSubscription();
    void stopWmiSubscription();
    void setWmiSubscriptionPaused(bool paused);
    void enqueueWmiEventRow(
        const QString& providerName,
        const QString& className,
        const QString& pidAndName,
        const QString& detailText);
    void applyWmiEventFilter();
    void clearWmiEventFilter();
    void flushWmiPendingRows();
    void appendWmiEventRow(
        const QString& providerName,
        const QString& className,
        const QString& pidAndName,
        const QString& detailText);
    void showWmiEventContextMenu(const QPoint& position);

    // ========================= ETW 功能 ==========================
    void refreshEtwProvidersAsync();
    void startEtwCapture();
    void stopEtwCapture();
    void setEtwCapturePaused(bool paused);
    static void WINAPI etwEventRecordCallback(struct _EVENT_RECORD* eventRecordPtr);
    void enqueueEtwEventFromRecord(const struct _EVENT_RECORD* eventRecordPtr);
    void appendEtwEventRow(
        const QString& providerName,
        int eventId,
        const QString& eventName,
        std::uint32_t pidValue,
        std::uint32_t tidValue,
        const QString& detailJson,
        const QString& activityIdText);
    void exportEtwRowsToTsv();
    void showEtwEventContextMenu(const QPoint& position);

    // ========================= 停止流程 ==========================
    // stopWmiSubscriptionInternal：
    // - 作用：停止 WMI 订阅，可选同步等待线程退出；
    // - 参数 waitForThread=true 时用于析构安全退出，false 时用于 UI 非阻塞停止。
    void stopWmiSubscriptionInternal(bool waitForThread);

    // stopEtwCaptureInternal：
    // - 作用：停止 ETW 捕获，可选同步等待线程退出；
    // - 参数 waitForThread=true 时用于析构安全退出，false 时用于 UI 非阻塞停止。
    void stopEtwCaptureInternal(bool waitForThread);

private:
    // ========================= 顶层布局 =========================
    QVBoxLayout* m_rootLayout = nullptr;    // 根布局。
    QWidget* m_perfPanel = nullptr;         // 顶部性能图面板。
    QGridLayout* m_perfPanelLayout = nullptr; // 顶部性能图布局。
    QTimer* m_perfUpdateTimer = nullptr;    // 性能图刷新定时器（默认1秒）。
    QTabWidget* m_sideTabWidget = nullptr;  // 侧边栏 Tab 容器。

    QChartView* m_cpuChartView = nullptr;      // CPU 条形图视图。
    QChartView* m_memoryChartView = nullptr;   // 内存条形图视图。
    QChartView* m_diskChartView = nullptr;     // 磁盘折线图视图。
    QChartView* m_networkChartView = nullptr;  // 网络折线图视图。
    QBarSet* m_cpuBarSet = nullptr;            // CPU 单柱数据集。
    QBarSet* m_memoryBarSet = nullptr;         // 内存单柱数据集。
    QLineSeries* m_diskReadSeries = nullptr;   // 磁盘读速率折线。
    QLineSeries* m_diskWriteSeries = nullptr;  // 磁盘写速率折线。
    QLineSeries* m_networkRxSeries = nullptr;  // 网络下载速率折线。
    QLineSeries* m_networkTxSeries = nullptr;  // 网络上传速率折线。
    QValueAxis* m_diskAxisX = nullptr;         // 磁盘图 X 轴。
    QValueAxis* m_diskAxisY = nullptr;         // 磁盘图 Y 轴。
    QValueAxis* m_networkAxisX = nullptr;      // 网络图 X 轴。
    QValueAxis* m_networkAxisY = nullptr;      // 网络图 Y 轴。
    int m_perfHistoryLength = 60;                        // 折线图保留点数。
    int m_perfSampleCounter = 0;                         // 当前采样序号。
    std::uint64_t m_lastCpuIdleTime = 0;                // 上次 CPU Idle 时间戳。
    std::uint64_t m_lastCpuKernelTime = 0;              // 上次 CPU Kernel 时间戳。
    std::uint64_t m_lastCpuUserTime = 0;                // 上次 CPU User 时间戳。
    bool m_cpuSampleValid = false;                      // CPU 采样是否已初始化。
    std::uint64_t m_lastNetworkRxBytes = 0;             // 上次网络累计接收字节。
    std::uint64_t m_lastNetworkTxBytes = 0;             // 上次网络累计发送字节。
    qint64 m_lastNetworkSampleMs = 0;                   // 上次网络采样时间（ms）。
    void* m_diskPerfQueryHandle = nullptr;              // PDH 查询句柄（磁盘性能）。
    void* m_diskReadCounterHandle = nullptr;            // PDH 磁盘读计数器句柄。
    void* m_diskWriteCounterHandle = nullptr;           // PDH 磁盘写计数器句柄。

    // ========================= WMI 页 ===========================
    QWidget* m_wmiPage = nullptr;                  // WMI 主页面。
    QVBoxLayout* m_wmiLayout = nullptr;            // WMI 页面布局。
    QWidget* m_wmiProviderPanel = nullptr;         // Provider 面板。
    QVBoxLayout* m_wmiProviderPanelLayout = nullptr; // Provider 面板布局。
    QHBoxLayout* m_wmiProviderControlLayout = nullptr; // Provider 控制栏布局。
    QLineEdit* m_wmiProviderFilterEdit = nullptr;  // Provider 过滤框。
    QPushButton* m_wmiProviderRefreshButton = nullptr; // Provider 刷新按钮。
    QLabel* m_wmiProviderStatusLabel = nullptr;    // Provider 状态文本。
    QTableView* m_wmiProviderTableView = nullptr;  // Provider 表格视图。
    QStandardItemModel* m_wmiProviderModel = nullptr; // Provider 数据模型。
    QSortFilterProxyModel* m_wmiProviderProxyModel = nullptr; // Provider 过滤模型。

    QWidget* m_wmiSubscribePanel = nullptr;        // WMI 订阅面板。
    QVBoxLayout* m_wmiSubscribeLayout = nullptr;   // WMI 订阅布局。
    QHBoxLayout* m_wmiEventClassControlLayout = nullptr; // 事件类控制栏。
    QPushButton* m_wmiSelectAllClassesButton = nullptr;  // 全选按钮。
    QPushButton* m_wmiSelectNoneClassesButton = nullptr; // 全不选按钮。
    QPushButton* m_wmiSelectWin32ClassesButton = nullptr; // 仅Win32按钮。
    QTableWidget* m_wmiEventClassTable = nullptr;  // 事件类选择表。
    QPlainTextEdit* m_wmiWhereEditor = nullptr;    // WHERE 条件编辑框。
    QComboBox* m_wmiWhereTemplateCombo = nullptr;  // WHERE 模板下拉框。
    QHBoxLayout* m_wmiSubscribeControlLayout = nullptr; // 订阅控制栏。
    QPushButton* m_wmiStartSubscribeButton = nullptr; // 开始订阅按钮。
    QPushButton* m_wmiStopSubscribeButton = nullptr;  // 停止订阅按钮。
    QPushButton* m_wmiPauseSubscribeButton = nullptr; // 暂停/继续按钮。
    QLabel* m_wmiSubscribeStatusLabel = nullptr;    // 订阅状态文本。
    QLineEdit* m_wmiEventGlobalFilterEdit = nullptr; // WMI 全字段筛选框。
    QLineEdit* m_wmiEventProviderFilterEdit = nullptr; // WMI Provider筛选框。
    QLineEdit* m_wmiEventClassFilterEdit = nullptr; // WMI 事件类筛选框。
    QLineEdit* m_wmiEventPidFilterEdit = nullptr; // WMI PID/进程筛选框。
    QLineEdit* m_wmiEventDetailFilterEdit = nullptr; // WMI 详情筛选框。
    QCheckBox* m_wmiEventRegexCheck = nullptr; // WMI 筛选是否启用正则。
    QCheckBox* m_wmiEventCaseCheck = nullptr; // WMI 筛选是否大小写敏感。
    QCheckBox* m_wmiEventInvertCheck = nullptr; // WMI 筛选是否反向匹配。
    QCheckBox* m_wmiEventKeepBottomCheck = nullptr; // WMI 表格是否保持贴底滚动。
    QPushButton* m_wmiEventFilterClearButton = nullptr; // WMI 筛选清空按钮。
    QLabel* m_wmiEventFilterStatusLabel = nullptr; // WMI 筛选结果状态文本。
    QTableWidget* m_wmiEventTable = nullptr;        // WMI 事件结果表。

    std::vector<WmiProviderEntry> m_wmiProviders; // Provider 缓存。
    std::atomic_bool m_wmiSubscribeRunning{ false }; // 订阅运行状态。
    std::atomic_bool m_wmiSubscribePaused{ false };  // 订阅暂停状态。
    std::atomic_bool m_wmiSubscribeStopFlag{ false }; // 订阅停止信号。
    std::unique_ptr<std::thread> m_wmiSubscribeThread; // WMI 后台订阅线程。
    int m_wmiProviderRefreshProgressPid = 0;          // WMI Provider 刷新进度 PID。
    int m_wmiSubscribeProgressPid = 0;                // WMI 订阅进度 PID。
    std::vector<QStringList> m_wmiPendingRows;        // WMI 待刷入 UI 的事件缓存。
    std::mutex m_wmiPendingMutex;                     // WMI 事件缓存互斥锁。
    QTimer* m_wmiUiUpdateTimer = nullptr;             // WMI UI 节流刷新定时器。

    // ========================= ETW 页 ===========================
    QWidget* m_etwPage = nullptr;                    // ETW 主页面。
    QVBoxLayout* m_etwLayout = nullptr;              // ETW 页面布局。
    QToolBox* m_etwSideToolBox = nullptr;            // ETW 侧边折叠菜单。
    QWidget* m_etwProviderPanel = nullptr;           // ETW Provider 面板。
    QVBoxLayout* m_etwProviderPanelLayout = nullptr; // ETW Provider 布局。
    QHBoxLayout* m_etwProviderControlLayout = nullptr; // ETW 控制栏。
    QPushButton* m_etwProviderRefreshButton = nullptr; // ETW 刷新按钮。
    QLabel* m_etwProviderStatusLabel = nullptr;      // ETW 状态标签。
    QComboBox* m_etwPresetCategoryCombo = nullptr;   // ETW 预置模板分类筛选下拉框。
    QListWidget* m_etwPresetProviderList = nullptr;  // ETW 预置常用 Provider 勾选列表。
    QListWidget* m_etwProviderList = nullptr;        // ETW Provider 复选列表。
    QLineEdit* m_etwManualProviderEdit = nullptr;    // 手动输入 Provider。
    QComboBox* m_etwLevelCombo = nullptr;            // 级别设置。
    QLineEdit* m_etwKeywordMaskEdit = nullptr;       // 关键字掩码输入。
    QSpinBox* m_etwBufferSizeSpin = nullptr;         // 缓冲区大小输入。
    QSpinBox* m_etwMinBufferSpin = nullptr;          // 最小缓冲区数输入。
    QSpinBox* m_etwMaxBufferSpin = nullptr;          // 最大缓冲区数输入。
    QHBoxLayout* m_etwCaptureControlLayout = nullptr; // ETW 控制栏布局。
    QPushButton* m_etwStartButton = nullptr;         // ETW 开始按钮。
    QPushButton* m_etwStopButton = nullptr;          // ETW 停止按钮。
    QPushButton* m_etwPauseButton = nullptr;         // ETW 暂停按钮。
    QPushButton* m_etwExportButton = nullptr;        // ETW 导出按钮。
    QLabel* m_etwCaptureStatusLabel = nullptr;       // ETW 状态文本。
    QTableWidget* m_etwEventTable = nullptr;         // ETW 事件表。
    QTimer* m_etwUiUpdateTimer = nullptr;            // 100ms 刷新节流定时器。

    std::vector<EtwProviderEntry> m_etwProviders;    // ETW Provider 缓存。
    std::vector<QStringList> m_etwPendingRows;       // ETW 待刷入 UI 的行缓存。
    std::mutex m_etwPendingMutex;                    // ETW 待刷入队列互斥锁。
    std::atomic_bool m_etwCaptureRunning{ false };   // ETW 捕获运行状态。
    std::atomic_bool m_etwCapturePaused{ false };    // ETW 捕获暂停状态。
    std::atomic_bool m_etwCaptureStopFlag{ false };  // ETW 捕获停止信号。
    std::unique_ptr<std::thread> m_etwCaptureThread; // ETW 后台线程。
    int m_etwCaptureProgressPid = 0;                 // ETW 捕获进度 PID。
    std::uint64_t m_etwSessionHandle = 0;            // ETW 会话句柄（TRACEHANDLE）。
    std::uint64_t m_etwTraceHandle = 0;              // ETW 消费句柄（TRACEHANDLE）。
    QString m_etwSessionName;                        // ETW 会话名（Stop/Query 复用）。
    bool m_initialDiscoveryDone = false;             // 首次显示时是否已触发 WMI/ETW Provider 枚举。
};
