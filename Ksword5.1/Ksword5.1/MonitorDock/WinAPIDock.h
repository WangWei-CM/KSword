#pragma once

// ============================================================
// WinAPIDock.h
// 作用：
// 1) 提供 “WinAPI” 独立 Dock 页面；
// 2) 负责选择目标进程、配置 Agent DLL 路径并启动 API 监控；
// 3) 通过命名管道接收 APIMonitor_x64 DLL 回传的 API 事件。
// ============================================================

#include "../Framework.h"
#include "WinApiMonitorProtocol.h"

#include <QString>  // QString：事件文本、样式文本与会话路径缓存。
#include <QWidget>  // QWidget：WinAPIDock 的直接基类。

#include <atomic>   // std::atomic_bool/std::atomic_uintptr_t：会话线程与管道句柄状态控制。
#include <cstdint>  // std::uint32_t/std::uintptr_t：PID 与句柄数值桥接。
#include <memory>   // std::unique_ptr：后台线程对象托管。
#include <mutex>    // std::mutex：保护待刷入事件队列。
#include <thread>   // std::thread：进程刷新与命名管道读取线程。
#include <vector>   // std::vector：进程快照与待刷新事件缓存。

class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QVBoxLayout;
class QWidget;
class QPoint;

class WinAPIDock final : public QWidget
{
public:
    // 构造函数：
    // - 作用：初始化 WinAPI 监控页 UI、连接和后台刷新定时器；
    // - 参数 parent：Qt 父控件。
    explicit WinAPIDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止命名管道线程，确保对象释放后无后台回调继续访问成员。
    ~WinAPIDock() override;

    // notifyPageActivated：
    // - 作用：在页签真正切换到 WinAPI 页面时触发轻量激活逻辑；
    // - 调用：MonitorDock 切到 WinAPI 子页时调用，用于按需刷新进程快照。
    void notifyPageActivated();

public:
    // ProcessColumn：
    // - 作用：定义目标进程表列索引；
    // - 调用：刷新进程列表和读取选中 PID 时统一复用。
    enum ProcessColumn
    {
        ProcessColumnPid = 0,
        ProcessColumnName,
        ProcessColumnPath,
        ProcessColumnUser,
        ProcessColumnCount
    };

    // EventColumn：
    // - 作用：定义 API 事件表列索引；
    // - 调用：插入事件、导出和右键菜单时统一按列访问。
    enum EventColumn
    {
        EventColumnTime100ns = 0,
        EventColumnCategory,
        EventColumnApi,
        EventColumnResult,
        EventColumnPidTid,
        EventColumnDetail,
        EventColumnCount
    };

    // EventRow：
    // - 作用：缓存一行待刷入 UI 的 API 事件；
    // - internalEvent：是否是 UI/Agent 自己生成的内部状态事件。
    struct EventRow
    {
        QString time100nsText;
        QString categoryText;
        QString apiText;
        QString resultText;
        QString pidTidText;
        QString detailText;
        bool internalEvent = false;
    };

private:
    void initializeUi();
    void initializeConnections();
    void updateActionState();
    void updateStatusLabel();

    void refreshProcessListAsync();
    void populateProcessTable(const std::vector<ks::process::ProcessRecord>& processList);
    void applyProcessFilter();
    bool currentSelectedPid(std::uint32_t* pidOut) const;
    void browseAgentDllPath();

    void startMonitoring();
    void stopMonitoring();
    void stopMonitoringInternal(bool waitForThread);
    void terminateHooksForSelectedProcess();
    bool prepareSessionArtifacts(std::uint32_t pidValue, QString* errorTextOut);
    bool writeSessionConfigFile(QString* errorTextOut) const;
    void appendInternalEvent(const QString& categoryText, const QString& apiText, const QString& detailText);

    void startPipeReadThread();
    void startChildPipeReadThread(std::uint32_t childPidValue);
    void closeChildPipeHandles();
    void joinChildPipeThreads();
    void writeChildStopFlags();
    void flushPendingRows();
    void appendEventRow(const EventRow& rowValue);

    void applyEventFilter();
    void clearEventFilter();
    void exportVisibleRowsToTsv();
    void showEventContextMenu(const QPoint& position);

    static QString blueButtonStyle();
    static QString blueInputStyle();
    static QString blueHeaderStyle();
    static QString buildStatusStyle(const QString& colorHex);
    static QString monitorInfoColorHex();
    static QString monitorSuccessColorHex();
    static QString monitorWarningColorHex();
    static QString monitorErrorColorHex();
    static QString monitorIdleColorHex();
    static QString eventCategoryText(std::uint32_t categoryValue);
    static QString defaultDllPathHint();
    static QString resultCodeText(std::int32_t resultCodeValue);
    static QString now100nsText();
    static bool tryParseUint32Text(const QString& textValue, std::uint32_t* valueOut);
    static QTableWidgetItem* createReadOnlyItem(const QString& textValue);

private:
    QVBoxLayout* m_rootLayout = nullptr;               // m_rootLayout：根布局。
    QSplitter* m_topSplitter = nullptr;                // m_topSplitter：上方左右分栏。
    QWidget* m_processPanel = nullptr;                 // m_processPanel：进程列表面板。
    QWidget* m_sessionPanel = nullptr;                 // m_sessionPanel：会话配置面板。
    QWidget* m_filterPanel = nullptr;                  // m_filterPanel：事件筛选面板。

    QLineEdit* m_processFilterEdit = nullptr;          // m_processFilterEdit：进程过滤框。
    QPushButton* m_processRefreshButton = nullptr;     // m_processRefreshButton：刷新进程列表按钮。
    QLabel* m_processStatusLabel = nullptr;            // m_processStatusLabel：进程列表状态文本。
    QTableWidget* m_processTable = nullptr;            // m_processTable：系统进程表。

    QLineEdit* m_manualPidEdit = nullptr;              // m_manualPidEdit：手动输入 PID 文本框。
    QLineEdit* m_agentDllPathEdit = nullptr;           // m_agentDllPathEdit：Agent DLL 路径文本框。
    QPushButton* m_browseAgentDllButton = nullptr;     // m_browseAgentDllButton：浏览 DLL 按钮。
    QCheckBox* m_hookFileCheck = nullptr;              // m_hookFileCheck：是否启用文件 API 监控。
    QCheckBox* m_hookRegistryCheck = nullptr;          // m_hookRegistryCheck：是否启用注册表 API 监控。
    QCheckBox* m_hookNetworkCheck = nullptr;           // m_hookNetworkCheck：是否启用网络 API 监控。
    QCheckBox* m_hookProcessCheck = nullptr;           // m_hookProcessCheck：是否启用进程 API 监控。
    QCheckBox* m_hookLoaderCheck = nullptr;            // m_hookLoaderCheck：是否启用加载器 API 监控。
    QCheckBox* m_autoInjectChildCheck = nullptr;       // m_autoInjectChildCheck：是否在 CreateProcessW 成功后自动注入子进程。
    QPushButton* m_startButton = nullptr;              // m_startButton：启动 WinAPI 监控按钮。
    QPushButton* m_stopButton = nullptr;               // m_stopButton：停止 WinAPI 监控按钮。
    QPushButton* m_terminateHookButton = nullptr;      // m_terminateHookButton：手动终止目标进程 Hook 按钮。
    QPushButton* m_exportButton = nullptr;             // m_exportButton：导出当前可见事件按钮。
    QPushButton* m_clearEventButton = nullptr;         // m_clearEventButton：清空事件表按钮。
    QLabel* m_sessionStatusLabel = nullptr;            // m_sessionStatusLabel：会话状态文本。

    QLineEdit* m_eventFilterEdit = nullptr;            // m_eventFilterEdit：全字段事件过滤框。
    QPushButton* m_eventFilterClearButton = nullptr;   // m_eventFilterClearButton：清空事件过滤按钮。
    QCheckBox* m_eventKeepBottomCheck = nullptr;       // m_eventKeepBottomCheck：是否自动滚动到底部。
    QLabel* m_eventFilterStatusLabel = nullptr;        // m_eventFilterStatusLabel：过滤结果状态文本。
    QTableWidget* m_eventTable = nullptr;              // m_eventTable：API 事件结果表。

    std::vector<ks::process::ProcessRecord> m_processList; // m_processList：当前系统进程快照。
    std::vector<EventRow> m_pendingRows;                   // m_pendingRows：后台线程待刷入的事件行。
    std::mutex m_pendingMutex;                             // m_pendingMutex：保护待刷入事件队列。
    std::unique_ptr<std::thread> m_pipeThread;             // m_pipeThread：命名管道读取线程。
    std::vector<std::unique_ptr<std::thread>> m_childPipeThreads; // m_childPipeThreads：自动注入子进程后的子管道读取线程。
    std::vector<std::uintptr_t> m_childPipeHandleValues;   // m_childPipeHandleValues：子管道句柄快照，用于停止时打断阻塞 ReadFile。
    std::vector<std::uint32_t> m_childSessionPids;          // m_childSessionPids：已发现的自动注入子进程 PID，用于写停止标记。
    std::mutex m_childPipeMutex;                           // m_childPipeMutex：保护子管道线程/句柄/PID 容器。
    std::atomic_bool m_processRefreshPending{ false };     // m_processRefreshPending：进程刷新是否进行中。
    std::atomic_bool m_pipeRunning{ false };               // m_pipeRunning：当前监控会话是否在运行。
    std::atomic_bool m_pipeConnected{ false };             // m_pipeConnected：是否已成功连上 Agent 管道。
    std::atomic_bool m_pipeStopFlag{ false };              // m_pipeStopFlag：命名管道线程停止信号。
    std::atomic_uintptr_t m_pipeHandleValue{ 0 };          // m_pipeHandleValue：后台线程持有的管道句柄数值。
    QTimer* m_uiFlushTimer = nullptr;                      // m_uiFlushTimer：批量刷新事件表的定时器。
    int m_sessionProgressPid = 0;                          // m_sessionProgressPid：kPro 任务卡片 ID。
    std::uint32_t m_currentSessionPid = 0;                 // m_currentSessionPid：当前监控中的目标 PID。
    QString m_currentPipeName;                             // m_currentPipeName：当前命名管道名。
    QString m_currentConfigPath;                           // m_currentConfigPath：当前会话配置文件路径。
    QString m_currentStopFlagPath;                         // m_currentStopFlagPath：当前停止标记文件路径。
    qint64 m_lastProcessRefreshMs = 0;                     // m_lastProcessRefreshMs：上次完成进程快照的时间戳（ms）。
    bool m_hasActivatedOnce = false;                       // m_hasActivatedOnce：是否已经至少激活过一次页面。
};
