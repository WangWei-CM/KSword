#pragma once

// ============================================================
// MonitorDock.h
// 作用：
// 1) 实现监控页“WMI / ETW”双侧边栏 Tab；
// 2) 提供 WMI Provider 枚举、事件类选择、订阅控制与事件展示；
// 3) 提供 ETW Provider 枚举、参数配置、实时结果表与导出能力。
// ============================================================

#include "../Framework.h"

#include <QRegularExpression>
#include <QWidget>

#include <atomic>      // std::atomic_bool：后台订阅状态控制。
#include <cstdint>     // std::uint32_t：PID 等固定宽度整数。
#include <functional>  // std::function：筛选匹配回调。
#include <memory>      // std::unique_ptr：线程对象托管。
#include <mutex>       // std::mutex：ETW 待刷新队列并发保护。
#include <string>      // std::string：日志输出与 COM 文本桥接。
#include <thread>      // std::thread：WMI 订阅后台线程。
#include <unordered_map> // std::unordered_map：字段映射缓存。
#include <utility>     // std::pair：筛选字段键值映射。
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
class QScrollArea;
class QVBoxLayout;
class QGridLayout;
class QShowEvent;
class QBarSet;
class QChartView;
class QLineSeries;
class QValueAxis;
class WinAPIDock;
class ProcessTraceMonitorWidget;

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

    // activateMonitorTab：
    // - 作用：把监控页内部子页切到指定 tab；
    // - 调用：MainWindow 需要把启动默认页或跳转目标定位到“WinAPI / WMI / ETW / 进程定向”时调用。
    void activateMonitorTab(const QString& tabKey);

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

    // EtwSessionEntry：
    // - 作用：保存系统中一个活动 ETW 会话的快照信息；
    // - 调用：ETW 会话栏枚举、展示与停止指定会话时复用。
    struct EtwSessionEntry
    {
        QString sessionName;
        QString modeText;
        QString bufferText;
        quint32 eventsLost = 0;
        QString logFilePath;
    };

public:
    // ========================= ETW 双筛选器 ==========================
    // EtwFilterStage：
    // - 作用：区分“前置筛选（不捕获）”与“后置筛选（仅隐藏显示）”。
    enum class EtwFilterStage : int
    {
        Pre = 0,
        Post = 1
    };

    // EtwStringMatchMode：
    // - 作用：字符串匹配模式；底层统一编译为正则表达式执行。
    enum class EtwStringMatchMode : int
    {
        Regex = 0,
        Exact = 1,
        Contains = 2,
        Prefix = 3,
        Suffix = 4
    };

    // EtwFilterFieldType：
    // - 作用：标记筛选字段值类型，驱动编译与匹配策略。
    enum class EtwFilterFieldType : int
    {
        Text = 0,
        Number = 1,
        NumberOrText = 2,
        Ip = 3,
        Port = 4,
        TimeRange = 5
    };

    // EtwFilterFieldId：
    // - 作用：统一标识 ETW 筛选字段，便于规则编译后快速匹配。
    enum class EtwFilterFieldId : int
    {
        ProviderName = 0,
        ProviderGuid,
        ProviderCategory,
        EventId,
        EventName,
        Task,
        Opcode,
        Level,
        KeywordMask,
        HeaderPid,
        HeaderTid,
        ActivityId,
        TimestampRange,
        ResourceType,
        Action,
        Target,
        Status,
        DetailKeyword,
        TargetPid,
        ParentPid,
        TargetTid,
        ProcessName,
        ImagePath,
        CommandLine,
        FilePath,
        FileOldPath,
        FileNewPath,
        FileOperation,
        FileStatusCode,
        FileAccessMask,
        RegistryKeyPath,
        RegistryValueName,
        RegistryHive,
        RegistryOperation,
        RegistryStatus,
        SourceIp,
        SourcePort,
        DestinationIp,
        DestinationPort,
        Protocol,
        Direction,
        Domain,
        Host,
        AuditResult,
        UserText,
        SidText,
        SecurityPid,
        SecurityTid,
        SecurityLevel,
        ScriptHostProcess,
        ScriptKeyword,
        ScriptTaskName,
        WmiClassName,
        WmiNamespace
    };

    struct EtwFilterNumericRange
    {
        std::uint64_t minValue = 0;
        std::uint64_t maxValue = 0;
    };

    struct EtwFilterIpRange
    {
        std::uint32_t minValue = 0;
        std::uint32_t maxValue = 0;
    };

    struct EtwFilterPortRange
    {
        std::uint16_t minValue = 0;
        std::uint16_t maxValue = 0;
    };

    struct EtwFilterFieldUiState
    {
        EtwFilterFieldId fieldId = EtwFilterFieldId::ProviderName;
        QString fieldKey;
        QString fieldLabel;
        QLineEdit* inputEdit = nullptr;
    };

    struct EtwFilterCategoryCheckUiState
    {
        QString categoryText;
        QCheckBox* checkBox = nullptr;
    };

    struct EtwFilterRuleGroupUiState
    {
        int groupId = 0;
        QWidget* containerWidget = nullptr;
        QLabel* titleLabel = nullptr;
        QCheckBox* enabledCheck = nullptr;
        QPushButton* removeGroupButton = nullptr;

        QComboBox* stringModeCombo = nullptr;
        QCheckBox* caseSensitiveCheck = nullptr;
        QCheckBox* invertCheck = nullptr;
        QCheckBox* detailVisibleColumnsCheck = nullptr;
        QCheckBox* detailMatchAllFieldsCheck = nullptr;

        std::vector<EtwFilterFieldUiState> fieldList;
        std::vector<EtwFilterCategoryCheckUiState> categoryCheckList;
    };

    struct EtwFilterRuleFieldCompiled
    {
        EtwFilterFieldId fieldId = EtwFilterFieldId::ProviderName;
        QString fieldKey;
        QString fieldLabel;
        EtwFilterFieldType fieldType = EtwFilterFieldType::Text;
        bool requiresDecodedPayload = false;
        std::vector<QRegularExpression> regexRuleList;
        std::vector<EtwFilterNumericRange> numericRangeList;
        std::vector<EtwFilterIpRange> ipRangeList;
        std::vector<EtwFilterPortRange> portRangeList;
    };

    struct EtwFilterRuleGroupCompiled
    {
        int groupId = 0;
        int displayIndex = 0;
        bool enabled = true;
        EtwStringMatchMode stringMode = EtwStringMatchMode::Regex;
        bool caseSensitive = false;
        bool invertMatch = false;
        bool detailVisibleColumnsOnly = false;
        bool detailMatchAllFields = true;
        bool requiresDecodedPayload = false;
        std::vector<EtwFilterRuleFieldCompiled> fieldList;

        bool hasAnyCondition() const
        {
            return !fieldList.empty();
        }
    };

    struct EtwCapturedEventRow
    {
        bool decodedReady = false;
        QString timestampText;
        std::uint64_t timestampValue = 0;
        QString providerName;
        QString providerGuid;
        QString providerCategory;
        int eventId = 0;
        QString eventName;
        int task = 0;
        QString taskName;
        int opcode = 0;
        QString opcodeName;
        int level = 0;
        QString levelText;
        std::uint64_t keywordMaskValue = 0;
        QString keywordMaskText;
        std::uint32_t headerPid = 0;
        std::uint32_t headerTid = 0;
        QString activityId;
        QString pidTidText;
        QString detailSummary;
        QString detailJson;
        QString detailVisibleText;
        QString detailAllText;

        QString resourceTypeText;
        QString actionText;
        QString targetText;
        QString statusText;

        std::uint32_t targetPid = 0;
        bool targetPidValid = false;
        std::uint32_t parentPid = 0;
        bool parentPidValid = false;
        std::uint32_t targetTid = 0;
        bool targetTidValid = false;

        QString processNameText;
        QString imagePathText;
        QString commandLineText;

        QString filePathText;
        QString fileOldPathText;
        QString fileNewPathText;
        QString fileOperationText;
        QString fileStatusCodeText;
        QString fileAccessMaskText;

        QString registryKeyPathText;
        QString registryValueNameText;
        QString registryHiveText;
        QString registryOperationText;
        QString registryStatusText;

        QString sourceIpText;
        std::uint32_t sourceIpValue = 0;
        bool sourceIpValid = false;
        std::uint16_t sourcePort = 0;
        bool sourcePortValid = false;
        QString destinationIpText;
        std::uint32_t destinationIpValue = 0;
        bool destinationIpValid = false;
        std::uint16_t destinationPort = 0;
        bool destinationPortValid = false;
        QString protocolText;
        QString directionText;
        QString domainText;
        QString hostText;

        QString auditResultText;
        QString userText;
        QString sidText;
        std::uint32_t securityPid = 0;
        bool securityPidValid = false;
        std::uint32_t securityTid = 0;
        bool securityTidValid = false;
        QString securityLevelText;

        QString scriptHostProcessText;
        QString scriptKeywordText;
        QString scriptTaskNameText;
        QString wmiClassNameText;
        QString wmiNamespaceText;
    };

private:
    // ========================= UI 初始化 =========================
    void initializeUi();
    void initializePerformancePanel();
    void initializeWmiTab();
    void initializeEtwTab();
    void ensureWinApiTabInitialized();
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
    void exportWmiRowsToTsv();
    void openWmiEventDetailViewerForRow(int row) const;
    void showWmiEventContextMenu(const QPoint& position);

    // ========================= ETW 功能 ==========================
    void refreshEtwProvidersAsync();
    void refreshEtwSessionsAsync();
    void stopSelectedEtwSessions();
    void startEtwCapture();
    void stopEtwCapture();
    void setEtwCapturePaused(bool paused);
    void updateEtwCaptureActionState();
    // updateEtwCollapseHeight：
    // - 作用：按当前展开项内容重算 ETW 折叠区高度，避免压缩折叠页内部控件；
    // - 说明：高度不足时交给外层滚动区域处理，不在折叠页内部压缩。
    void updateEtwCollapseHeight();
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
    void exportEtwRowsToTsv(bool visibleOnly = true);
    void openEtwEventDetailViewerForRow(int row) const;
    void showEtwEventContextMenu(const QPoint& position);

    // ========================= ETW 双筛选 ========================
    void initializeEtwFilterPanels();
    void addEtwFilterRuleGroup(EtwFilterStage stage);
    void removeEtwFilterRuleGroup(EtwFilterStage stage, int groupId);
    void rebuildEtwFilterRuleGroupUi(EtwFilterStage stage);
    void clearEtwFilterGroups(EtwFilterStage stage);
    void applyEtwFilterRules(EtwFilterStage stage);
    void applyEtwPostFilterToTable();
    void updateEtwFilterStateLabel(EtwFilterStage stage);
    bool tryCompileEtwFilterGroups(
        EtwFilterStage stage,
        std::vector<EtwFilterRuleGroupCompiled>& compiledGroupsOut,
        QString& errorTextOut) const;
    EtwFilterRuleGroupUiState* findEtwFilterRuleGroupById(EtwFilterStage stage, int groupId);
    const EtwFilterRuleGroupUiState* findEtwFilterRuleGroupById(EtwFilterStage stage, int groupId) const;
    QString etwFilterConfigPath() const;
    bool saveEtwFilterConfigToPath(const QString& filePath, bool showErrorDialog) const;
    bool loadEtwFilterConfigFromPath(const QString& filePath, bool showErrorDialog);
    void saveEtwFilterConfigToDefaultPath(bool showDialog) const;
    void loadEtwFilterConfigFromDefaultPath(bool showDialog);
    void importEtwFilterConfigFromUserSelectedPath();
    void exportEtwFilterConfigToUserSelectedPath() const;

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
    ProcessTraceMonitorWidget* m_processTraceWidget = nullptr; // m_processTraceWidget：进程定向监控子页。
    QWidget* m_winApiPage = nullptr;        // m_winApiPage：WinAPI 子页宿主容器。
    WinAPIDock* m_winApiWidget = nullptr;   // m_winApiWidget：真正的 WinAPI 监控控件。

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
    QPushButton* m_wmiExportButton = nullptr;         // 导出结果按钮。
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
    QWidget* m_etwSessionPanel = nullptr;            // ETW 会话面板。
    QVBoxLayout* m_etwSessionPanelLayout = nullptr;  // ETW 会话布局。
    QHBoxLayout* m_etwSessionControlLayout = nullptr; // ETW 会话控制栏。
    QPushButton* m_etwSessionRefreshButton = nullptr; // ETW 会话刷新按钮。
    QPushButton* m_etwSessionStopButton = nullptr;   // ETW 会话停止按钮。
    QLabel* m_etwSessionStatusLabel = nullptr;       // ETW 会话状态标签。
    QTableWidget* m_etwSessionTable = nullptr;       // ETW 会话表。
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
    QWidget* m_etwPreFilterPanel = nullptr;          // 前置筛选面板。
    QWidget* m_etwPostFilterPanel = nullptr;         // 后置筛选面板。
    QVBoxLayout* m_etwPreFilterPanelLayout = nullptr; // 前置筛选布局。
    QVBoxLayout* m_etwPostFilterPanelLayout = nullptr; // 后置筛选布局。
    QPushButton* m_etwPreFilterAddGroupButton = nullptr; // 前置筛选新增组按钮。
    QPushButton* m_etwPreFilterApplyButton = nullptr; // 前置筛选应用按钮。
    QPushButton* m_etwPreFilterClearButton = nullptr; // 前置筛选清空按钮。
    QPushButton* m_etwPreFilterLoadDefaultButton = nullptr; // 前置筛选加载默认配置按钮。
    QPushButton* m_etwPreFilterSaveDefaultButton = nullptr; // 前置筛选保存默认配置按钮。
    QPushButton* m_etwPreFilterImportButton = nullptr; // 前置筛选导入配置按钮。
    QPushButton* m_etwPreFilterExportButton = nullptr; // 前置筛选导出配置按钮。
    QLabel* m_etwPreFilterStateLabel = nullptr;      // 前置筛选状态汇总标签。
    QScrollArea* m_etwPreFilterScrollArea = nullptr; // 前置筛选滚动容器。
    QWidget* m_etwPreFilterGroupHostWidget = nullptr; // 前置筛选规则组宿主。
    QVBoxLayout* m_etwPreFilterGroupHostLayout = nullptr; // 前置筛选规则组布局。
    QPushButton* m_etwPostFilterAddGroupButton = nullptr; // 后置筛选新增组按钮。
    QPushButton* m_etwPostFilterApplyButton = nullptr; // 后置筛选应用按钮。
    QPushButton* m_etwPostFilterClearButton = nullptr; // 后置筛选清空按钮。
    QPushButton* m_etwPostFilterLoadDefaultButton = nullptr; // 后置筛选加载默认配置按钮。
    QPushButton* m_etwPostFilterSaveDefaultButton = nullptr; // 后置筛选保存默认配置按钮。
    QPushButton* m_etwPostFilterImportButton = nullptr; // 后置筛选导入配置按钮。
    QPushButton* m_etwPostFilterExportButton = nullptr; // 后置筛选导出配置按钮。
    QLabel* m_etwPostFilterStateLabel = nullptr;      // 后置筛选状态汇总标签。
    QScrollArea* m_etwPostFilterScrollArea = nullptr; // 后置筛选滚动容器。
    QWidget* m_etwPostFilterGroupHostWidget = nullptr; // 后置筛选规则组宿主。
    QVBoxLayout* m_etwPostFilterGroupHostLayout = nullptr; // 后置筛选规则组布局。
    QTableWidget* m_etwEventTable = nullptr;         // ETW 事件表。
    QTimer* m_etwUiUpdateTimer = nullptr;            // 100ms 刷新节流定时器。

    std::vector<EtwProviderEntry> m_etwProviders;    // ETW Provider 缓存。
    std::vector<EtwSessionEntry> m_etwSessions;      // ETW 会话缓存。
    int m_etwPreFilterNextGroupId = 1;               // 前置筛选规则组递增ID。
    int m_etwPostFilterNextGroupId = 1;              // 后置筛选规则组递增ID。
    std::vector<std::unique_ptr<EtwFilterRuleGroupUiState>> m_etwPreFilterRuleGroupUiList; // 前置筛选UI原始规则组。
    std::vector<std::unique_ptr<EtwFilterRuleGroupUiState>> m_etwPostFilterRuleGroupUiList; // 后置筛选UI原始规则组。
    std::vector<EtwFilterRuleGroupCompiled> m_etwPreFilterCompiledGroupList; // 前置筛选编译规则组。
    std::vector<EtwFilterRuleGroupCompiled> m_etwPostFilterCompiledGroupList; // 后置筛选编译规则组。
    std::shared_ptr<const std::vector<EtwFilterRuleGroupCompiled>> m_etwPreFilterCompiledSnapshot; // ETW回调使用的前置筛选快照。
    std::mutex m_etwPreFilterSnapshotMutex;          // 前置筛选快照互斥锁。
    std::vector<EtwCapturedEventRow> m_etwPendingRows; // ETW 待刷入 UI 的事件缓存。
    std::vector<EtwCapturedEventRow> m_etwCapturedRows; // ETW 已捕获事件缓存（后置筛选仅隐藏）。
    std::mutex m_etwPendingMutex;                    // ETW 待刷入队列互斥锁。
    std::atomic_bool m_etwCaptureRunning{ false };   // ETW 捕获运行状态。
    std::atomic_bool m_etwCapturePaused{ false };    // ETW 捕获暂停状态。
    std::atomic_bool m_etwCaptureStopFlag{ false };  // ETW 捕获停止信号。
    std::unique_ptr<std::thread> m_etwCaptureThread; // ETW 后台线程。
    int m_etwCaptureProgressPid = 0;                 // ETW 捕获进度 PID。
    int m_etwSessionRefreshProgressPid = 0;          // ETW 会话刷新/结束进度 PID。
    std::atomic<std::uint64_t> m_etwSessionHandle{ 0 }; // ETW 会话句柄（TRACEHANDLE）。
    std::atomic<std::uint64_t> m_etwTraceHandle{ 0 };   // ETW 消费句柄（TRACEHANDLE）。
    QString m_etwSessionName;                        // ETW 会话名（Stop/Query 复用）。
    bool m_initialDiscoveryDone = false;             // 首次显示时是否已触发 WMI/ETW Provider 枚举。
};
