#pragma once

// ============================================================
// ProcessDock.h
// 作用：
// - 构建“进程”Dock 内的完整 R3 任务管理器界面；
// - 提供异步刷新、树/列表视图、列管理、右键操作等能力；
// - 与 ks::process Win32 封装层解耦，UI 层只做展示与交互。
// ============================================================

#include "../Framework.h"

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QSize>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

// 前置声明：减少头文件编译开销。
class QComboBox;
class QCheckBox;
class QFormLayout;
class QGroupBox;
class QHeaderView;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QSlider;
class QTableWidget;
class QTabWidget;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QHBoxLayout;
class QPoint;
class QWidget;
class ProcessDetailWindow;

namespace ks::process
{
    struct CounterSample;
    struct ProcessRecord;
    struct SystemThreadRecord;
}

class ProcessDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数作用：
    // - 初始化侧边栏 Tab；
    // - 初始化进程列表与控制栏；
    // - 启动默认监视（性能计数器视图）。
    explicit ProcessDock(QWidget* parent = nullptr);

    // refreshThemeVisuals 作用：
    // - 在深浅色切换后，重绘当前列表行着色；
    // - 修复“新增进程高亮色在主题切换后残留”的问题。
    // 调用方式：MainWindow::applyAppearanceSettings 在主题更新后调用。
    // 入参：无。
    // 返回：无。
    void refreshThemeVisuals();

    // requestOpenProcessDetailByPid 作用：
    // - 外部模块按 PID 打开进程详情窗口；
    // - 若已存在对应窗口则复用，不重复创建。
    // 调用方式：MainWindow/FileDock 通过此入口跳转。
    // 参数 pid：目标进程 PID。
    // 返回：无。
    void requestOpenProcessDetailByPid(std::uint32_t pid);

protected:
    // showEvent 作用：
    // - 在 Dock 首次真正显示时再启动首轮刷新与周期监视；
    // - 避免主窗口启动阶段被进程枚举拖慢。
    void showEvent(QShowEvent* event) override;

    // resizeEvent 作用：
    // - 在 Dock 尺寸变化时重新分配可见列宽；
    // - 避免出现内部横向滚动条，保持“自适应宽度”体验。
    // 调用方式：Qt 在窗口尺寸变化时自动触发。
    // 参数 event：Qt 提供的尺寸变化事件对象（只读使用）。
    // 返回值：无。
    void resizeEvent(QResizeEvent* event) override;

private:
    // TableColumn：统一定义表格列索引，避免硬编码魔法数。
    enum class TableColumn : int
    {
        Name = 0,      // 进程名（含图标）。
        Pid,           // PID。
        Cpu,           // CPU 百分比。
        Ram,           // RAM（MB）。
        Disk,          // DISK（MB/s）。
        Gpu,           // GPU（预留）。
        Net,           // Net（预留）。
        Signature,     // 数字签名状态。
        Path,          // 可执行路径。
        ParentPid,     // 父进程 PID。
        CommandLine,   // 启动参数。
        User,          // 用户名。
        StartTime,     // 启动时间。
        IsAdmin,       // 是否管理员。
        Count          // 列总数。
    };

    // ThreadTableColumn：线程页表格列索引定义。
    enum class ThreadTableColumn : int
    {
        ThreadId = 0,      // 线程 ID。
        OwnerPid,          // 所属进程 PID。
        ProcessName,       // 所属进程名（含图标）。
        StartAddress,      // 线程启动地址。
        Priority,          // 动态优先级。
        BasePriority,      // 基础优先级。
        ThreadState,       // 线程状态码（含文本）。
        WaitReason,        // 等待原因码（含文本）。
        KernelTimeMs,      // 内核态累计时间（毫秒）。
        UserTimeMs,        // 用户态累计时间（毫秒）。
        CpuTimeMs,         // CPU 累计时间（毫秒）。
        WaitTimeTick,      // 等待时长计数。
        ContextSwitches,   // 上下文切换次数。
        CreateTime,        // 创建时间。
        ProcessPath,       // 所属进程路径（可为空）。
        Count              // 列总数。
    };

    // ViewMode：两种视图模式。
    enum class ViewMode : int
    {
        Monitor = 0, // 监视视图（性能计数器列）。
        Detail = 1   // 详细信息视图（展示所有补充字段）。
    };

    // DisplayRow：列表渲染层的数据结构（带状态标记）。
    struct DisplayRow
    {
        ks::process::ProcessRecord* record = nullptr; // 指向缓存中的实体数据。
        int depth = 0;                                // 树状列表下的缩进深度。
        bool isNew = false;                           // 本轮新增进程（绿色高亮）。
        bool isExited = false;                        // 本轮退出但保留一轮（灰色高亮）。
        bool isKernelOnly = false;                    // 仅内核枚举可见（疑似隐藏进程，红色高亮）。
    };

    // CacheEntry：进程缓存条目（用于复用静态信息和退出保留）。
    struct CacheEntry
    {
        ks::process::ProcessRecord record; // 完整进程记录。
        int missingRounds = 0;             // 连续缺失轮次（1 表示“刚退出，保留显示”）。
        bool isNewInLatestRound = false;   // 最新刷新中是否为新增。
        bool isExitedInLatestRound = false;// 最新刷新中是否为退出保留。
        bool isKernelOnlyInLatestRound = false; // 最新刷新中是否仅内核枚举可见。
        std::uint32_t staticFillAttemptCount = 0; // 静态详情补齐尝试次数（含成功/失败）。
        std::uint32_t staticFillFailureCount = 0; // 静态详情连续失败次数（用于退避重试）。
    };

    // RefreshResult：后台线程刷新结果对象。
    struct RefreshResult
    {
        std::unordered_map<std::string, CacheEntry> nextCache;                    // 下一轮缓存。
        std::unordered_map<std::string, ks::process::CounterSample> nextCounters; // 下一轮计数器样本。

        // ======== 统计字段（用于 UI 状态提示 + 详细日志） ========
        std::size_t enumeratedCount = 0;        // 本轮枚举到的“当前存活”进程数。
        std::size_t newProcessCount = 0;        // 本轮新增进程数量。
        std::size_t exitedProcessCount = 0;     // 本轮退出保留数量（灰底保留一轮）。
        std::size_t reusedProcessCount = 0;     // 本轮复用旧缓存的数量。
        std::size_t staticFilledCount = 0;      // 本轮实际补齐静态详情的数量。
        std::size_t staticDeferredCount = 0;    // 本轮因预算或模式延后的静态详情数量。
        std::size_t imagePathFilledCount = 0;   // 本轮额外补齐 imagePath 的数量（用于图标显示）。
        std::uint64_t workerElapsedMs = 0;      // 后台线程本轮构建结果耗时（毫秒）。
        int selectedStrategyIndex = 0;          // UI 选择的策略下标（0/1）。
        ks::process::ProcessEnumStrategy selectedStrategy{}; // 由下标映射的策略枚举。
        ks::process::ProcessEnumStrategy actualStrategy{};   // 实际执行策略（Auto 下可能回退）。
        bool detailModeEnabled = false;         // 本轮是否处于“详细信息视图”。
        bool kernelCompareEnabled = false;      // 本轮是否启用内核进程对比。
        bool kernelQuerySucceeded = false;      // 内核进程查询是否成功。
        std::size_t kernelEnumeratedCount = 0;  // 内核枚举得到的进程数量。
        std::size_t kernelOnlyCount = 0;        // 仅内核可见（疑似隐藏）进程数量。
        std::string kernelQueryDetailText;      // 内核查询诊断文本（错误或统计）。
    };

private:
    // ======== UI 初始化相关 ========
    void initializeUi();
    void initializeTopControls();
    void initializeProcessTable();
    void initializeCreateProcessPage();
    void initializeThreadPage();
    void initializeConnections();
    void initializeThreadPageConnections();
    void initializeTimer();
    void updateRefreshStateUi(bool refreshing, const QString& stateText);
    void applyAdaptiveColumnWidths();
    void initializeCreateProcessConnections();
    void focusProcessSearchBox(bool selectAllText);
    QString currentProcessSearchText() const;
    bool processRecordMatchesSearch(const ks::process::ProcessRecord& processRecord) const;

    // ======== 刷新与渲染 ========
    void requestAsyncRefresh(bool forceRefresh);
    void applyRefreshResult(const RefreshResult& refreshResult);
    void rebuildTable();
    void requestAsyncThreadRefresh(bool forceRefresh);
    void rebuildThreadTable();
    void applyThreadStatusUi(bool refreshing, const QString& stateText);
    std::vector<DisplayRow> buildDisplayOrder() const;
    std::vector<DisplayRow> buildTreeDisplayOrder() const;
    std::vector<DisplayRow> buildListDisplayOrder() const;

    // ======== 视图控制 ========
    void applyViewMode(ViewMode viewMode);
    void applyDefaultColumnWidths();
    bool isTreeModeEnabled() const;
    ViewMode currentViewMode() const;

    // ======== 表格交互 ========
    void showTableContextMenu(const QPoint& localPosition);
    void showThreadTableContextMenu(const QPoint& localPosition);
    void showHeaderContextMenu(const QPoint& localPosition);
    void copyCurrentCell();
    void copyCurrentRow();
    void copyCurrentThreadCell();
    void copyCurrentThreadRow();
    void openProcessDetailsPlaceholder();
    void openProcessDetailWindowByPid(std::uint32_t pid);
    void openThreadOwnerProcessDetails();
    void executeSuspendThreadAction();
    void executeResumeThreadAction();
    void executeTerminateThreadAction();
    void updateUsageSummaryInHeader(const std::vector<DisplayRow>& displayRows);

    // ======== 进程控制动作 ========
    // executeTerminateProcessAction 作用：
    // - 执行“结束进程组合动作”；
    // - 固定顺序执行多种结束原理（TerminateProcess/Nt/WTS/Job/RestartManager/线程终止/调试器/Unmap 等）；
    // - 使用同一个 kLogEvent 串联整次调用链日志并判定目标是否真正退出。
    void executeTerminateProcessAction();
    // executeR0TerminateProcessAction 作用：
    // - 通过 R0 驱动 IOCTL 请求内核态结束目标进程；
    // - 成功/失败细节统一写入日志面板。
    void executeR0TerminateProcessAction();
    // executeR0SuspendProcessAction 作用：
    // - 通过 R0 驱动 IOCTL 请求内核态挂起目标进程；
    // - 成功/失败细节统一写入日志面板。
    void executeR0SuspendProcessAction();
    // executeR0SetPplProtectionAction 作用：
    // - 通过 R0 驱动 IOCTL 请求设置目标进程 PPL 保护层级；
    // - protectionLevel 使用单字节原生层级编码（Signer<<4 | Type）。
    void executeR0SetPplProtectionAction(std::uint8_t protectionLevel, const QString& levelDisplayText);
    // executeTerminateThreadsAction 作用：
    // - 单独执行 TerminateThread(全部线程)（保留给其他入口复用）；
    // - 与“结束进程组合动作”不同，该函数不包含 TerminateProcess 步骤。
    void executeTerminateThreadsAction();
    void executeSuspendAction();
    void executeResumeAction();
    void executeSetCriticalAction(bool enableCritical);
    void executeSetPriorityAction(int priorityActionId);
    // executeSetEfficiencyModeAction 作用：开启/关闭 Windows 进程效率模式。
    void executeSetEfficiencyModeAction(bool enableEfficiencyMode);
    void executeOpenFolderAction();
    // executeOpenMemoryOperationAction 作用：跳转到内存页并附加当前进程，便于继续转储/查看区域。
    void executeOpenMemoryOperationAction();

    // ======== 工具函数 ========
    std::string selectedIdentityKey() const;
    ks::process::ProcessRecord* selectedRecord();
    const ks::process::SystemThreadRecord* selectedThreadRecord() const;
    void bindContextActionToItem(QTreeWidgetItem* clickedItem);
    void clearContextActionBinding();
    void bindThreadContextActionToItem(QTreeWidgetItem* clickedItem);
    void clearThreadContextActionBinding();
    bool threadRecordMatchesSearch(const ks::process::SystemThreadRecord& threadRecord) const;
    QString formatColumnText(const ks::process::ProcessRecord& processRecord, TableColumn column, int depth) const;
    QString formatThreadColumnText(const ks::process::SystemThreadRecord& threadRecord, ThreadTableColumn column) const;
    QString threadStateText(std::uint32_t stateValue) const;
    QString threadWaitReasonText(std::uint32_t waitReasonValue) const;
    QIcon resolveProcessIcon(const ks::process::ProcessRecord& processRecord);
    QIcon blueTintedIcon(const char* iconPath, const QSize& iconSize = QSize(16, 16)) const;
    // tintedProcessTabIcon 作用：按指定颜色重绘进程页侧栏图标，避免选中态蓝底蓝图标。
    QIcon tintedProcessTabIcon(const char* iconPath, const QColor& tintColor, const QSize& iconSize = QSize(16, 16)) const;
    // refreshSideTabIconContrast 作用：刷新左侧 Tab 选中态图标颜色，提升当前页识别度。
    void refreshSideTabIconContrast();
    QString buildThreadContextMenuStyle() const;
    // showActionResultMessage 作用：统一记录进程动作结果日志（不弹窗），复用同一 kLogEvent 保持调用链连续。
    // 调用方式：在动作函数中创建 kLogEvent 后，将同一个事件对象传入本函数。
    // 参数 title：动作标题；actionOk：动作是否成功；detailText：动作详情；actionEvent：本次动作全链路事件对象。
    // 返回值：无。
    void showActionResultMessage(const QString& title, bool actionOk, const std::string& detailText, const kLogEvent& actionEvent);
    ks::process::CreateProcessRequest buildCreateProcessRequestFromUi(bool* buildOk, QString* errorTextOut) const;
    void executeCreateProcessRequest();
    void executeApplyTokenPrivilegeEditsOnly();
    void appendCreateResultLine(const QString& lineText);
    void browseCreateProcessApplicationPath();
    void browseCreateProcessCurrentDirectory();
    void resetCreateProcessForm();
    void bindBitmaskEditor(QLineEdit* valueEdit, std::vector<QCheckBox*>* checkBoxList, const QString& fieldDisplayName);
    void syncEditValueFromBitmaskChecks(QLineEdit* valueEdit, const std::vector<QCheckBox*>* checkBoxList);
    void syncBitmaskChecksFromEditValue(QLineEdit* valueEdit, const std::vector<QCheckBox*>* checkBoxList, const QString& fieldDisplayName);
    static std::string buildRulerPrefix(int depth);
    static int toColumnIndex(TableColumn column);
    static int toThreadColumnIndex(ThreadTableColumn column);
    static bool parseUnsignedText(const QString& text, std::uint64_t& valueOut);
    static std::uint32_t parseUInt32WithDefault(const QString& text, std::uint32_t defaultValue, bool* parseOkOut = nullptr);
    static std::uint64_t parseUInt64WithDefault(const QString& text, std::uint64_t defaultValue, bool* parseOkOut = nullptr);

    // ======== 后台线程核心函数（静态） ========
    static RefreshResult buildRefreshResult(
        int strategyIndex,
        bool detailModeEnabled,
        bool queryKernelProcessList,
        int staticDetailFillBudget,
        std::uint64_t refreshTicket,
        int progressTaskPid,
        const std::unordered_map<std::string, CacheEntry>& previousCache,
        const std::unordered_map<std::string, ks::process::CounterSample>& previousCounters,
        std::uint32_t logicalCpuCount);

private:
    // ======== 顶层布局 ========
    QVBoxLayout* m_rootLayout = nullptr;      // 根布局：只包含侧边栏 Tab。
    QTabWidget* m_sideTabWidget = nullptr;    // 左侧 tab 栏（West），包含“进程列表”页。
    QWidget* m_processListPage = nullptr;     // “进程列表”页容器。
    QVBoxLayout* m_processPageLayout = nullptr; // 进程页主布局。
    QWidget* m_createProcessPage = nullptr;   // “创建进程”页容器。
    QVBoxLayout* m_createProcessPageLayout = nullptr; // 创建页主布局。
    QWidget* m_threadPage = nullptr;          // “线程列表”页容器。
    QVBoxLayout* m_threadPageLayout = nullptr; // 线程页主布局。

    // ======== 控制栏 ========
    QHBoxLayout* m_controlLayout = nullptr;   // 上方“操作按钮”行布局。
    QHBoxLayout* m_statusLayout = nullptr;    // 下方“监控状态”行布局。
    QComboBox* m_strategyCombo = nullptr;     // 进程遍历方案下拉框。
    QPushButton* m_treeToggleButton = nullptr;// 树/列表切换按钮。
    QComboBox* m_viewModeCombo = nullptr;     // 监视视图/详细视图下拉框。
    QPushButton* m_startButton = nullptr;     // 开始监视按钮。
    QPushButton* m_pauseButton = nullptr;     // 暂停监视按钮。
    QCheckBox* m_kernelCompareCheck = nullptr;// 刷新时是否额外请求内核进程列表并做差异对比。
    QLineEdit* m_processSearchLineEdit = nullptr; // 进程搜索框；用于按名称/PID/路径等关键词过滤当前列表。
    QLabel* m_refreshLabel = nullptr;         // 刷新间隔标签。
    QSlider* m_refreshSlider = nullptr;       // 刷新间隔滑块（秒）。
    QLabel* m_refreshStateLabel = nullptr;    // 刷新状态标签（明显展示“刷新中/空闲+耗时”）。

    // ======== 进程表格 ========
    QTreeWidget* m_processTable = nullptr;    // 进程列表表格（支持列拖动/排序/右键）。

    // ======== 线程页控件 ========
    QHBoxLayout* m_threadTopLayout = nullptr; // 线程页顶部操作栏。
    QPushButton* m_threadRefreshButton = nullptr; // 线程页刷新按钮（图标按钮）。
    QLineEdit* m_threadSearchLineEdit = nullptr; // 线程页搜索框（按 TID/PID/名称过滤）。
    QLabel* m_threadStatusLabel = nullptr;    // 线程页刷新状态标签。
    QTreeWidget* m_threadTable = nullptr;     // 线程列表表格（支持右键动作）。

    // ======== 创建进程页 - 通用参数 ========
    QComboBox* m_createMethodCombo = nullptr; // CreateProcessW / Token 路径。
    QLineEdit* m_applicationNameEdit = nullptr;
    QPushButton* m_applicationBrowseButton = nullptr;
    QCheckBox* m_useApplicationNameCheck = nullptr;
    QLineEdit* m_commandLineEdit = nullptr;
    QCheckBox* m_useCommandLineCheck = nullptr;
    QLineEdit* m_currentDirectoryEdit = nullptr;
    QPushButton* m_currentDirectoryBrowseButton = nullptr;
    QCheckBox* m_useCurrentDirectoryCheck = nullptr;
    QPlainTextEdit* m_environmentEditor = nullptr;
    QCheckBox* m_useEnvironmentCheck = nullptr;
    QCheckBox* m_environmentUnicodeCheck = nullptr;
    QCheckBox* m_inheritHandleCheck = nullptr;
    QLineEdit* m_creationFlagsEdit = nullptr;
    std::vector<QCheckBox*> m_creationFlagChecks; // dwCreationFlags 勾选集合。

    // ======== 创建进程页 - SECURITY_ATTRIBUTES ========
    QCheckBox* m_useProcessSecurityCheck = nullptr;
    QLineEdit* m_processSecurityLengthEdit = nullptr;
    QLineEdit* m_processSecurityDescriptorEdit = nullptr;
    QCheckBox* m_processSecurityInheritCheck = nullptr;
    QCheckBox* m_useThreadSecurityCheck = nullptr;
    QLineEdit* m_threadSecurityLengthEdit = nullptr;
    QLineEdit* m_threadSecurityDescriptorEdit = nullptr;
    QCheckBox* m_threadSecurityInheritCheck = nullptr;

    // ======== 创建进程页 - STARTUPINFOW ========
    QCheckBox* m_useStartupInfoCheck = nullptr;
    QLineEdit* m_siCbEdit = nullptr;
    QLineEdit* m_siReservedEdit = nullptr;
    QLineEdit* m_siDesktopEdit = nullptr;
    QLineEdit* m_siTitleEdit = nullptr;
    QLineEdit* m_siXEdit = nullptr;
    QLineEdit* m_siYEdit = nullptr;
    QLineEdit* m_siXSizeEdit = nullptr;
    QLineEdit* m_siYSizeEdit = nullptr;
    QLineEdit* m_siXCountCharsEdit = nullptr;
    QLineEdit* m_siYCountCharsEdit = nullptr;
    QLineEdit* m_siFillAttributeEdit = nullptr;
    QLineEdit* m_siFlagsEdit = nullptr;
    std::vector<QCheckBox*> m_startupFillAttributeChecks; // STARTUPINFO.dwFillAttribute 勾选集合。
    std::vector<QCheckBox*> m_startupFlagChecks; // STARTUPINFO.dwFlags 勾选集合。
    QLineEdit* m_siShowWindowEdit = nullptr;
    QLineEdit* m_siCbReserved2Edit = nullptr;
    QLineEdit* m_siReserved2PtrEdit = nullptr;
    QLineEdit* m_siStdInputEdit = nullptr;
    QLineEdit* m_siStdOutputEdit = nullptr;
    QLineEdit* m_siStdErrorEdit = nullptr;

    // ======== 创建进程页 - PROCESS_INFORMATION ========
    QCheckBox* m_useProcessInfoCheck = nullptr;
    QLineEdit* m_piProcessHandleEdit = nullptr;
    QLineEdit* m_piThreadHandleEdit = nullptr;
    QLineEdit* m_piPidEdit = nullptr;
    QLineEdit* m_piTidEdit = nullptr;

    // ======== 创建进程页 - Token 路径 ========
    QLineEdit* m_tokenSourcePidEdit = nullptr;
    QLineEdit* m_tokenDesiredAccessEdit = nullptr;
    QCheckBox* m_tokenDuplicatePrimaryCheck = nullptr;
    std::vector<QCheckBox*> m_tokenDesiredAccessChecks; // Token DesiredAccess 勾选集合。
    QTableWidget* m_tokenPrivilegeTable = nullptr;
    QPushButton* m_applyTokenPrivilegeButton = nullptr;
    QPushButton* m_resetTokenPrivilegeButton = nullptr;

    // ======== 创建进程页 - 操作与输出 ========
    QPushButton* m_launchProcessButton = nullptr;
    QPushButton* m_resetCreateFormButton = nullptr;
    QTextEdit* m_createResultOutput = nullptr;

    // ======== 刷新调度 ========
    QTimer* m_refreshTimer = nullptr;         // 周期刷新定时器。
    bool m_monitoringEnabled = true;          // 当前是否处于监视状态。
    bool m_refreshInProgress = false;         // 防止并发刷新的互斥标记。
    std::uint64_t m_refreshTicket = 0;        // 刷新请求序号（防乱序）。
    std::uint32_t m_logicalCpuCount = 1;      // CPU 核心数（CPU 百分比换算）。
    std::chrono::steady_clock::time_point m_lastRefreshStartTime{}; // 主线程记录的刷新开始时刻。
    int m_refreshProgressTaskPid = 0;         // 绑定到 kPro 的“进程刷新任务”PID。

    // ======== 数据缓存 ========
    std::unordered_map<std::string, CacheEntry> m_cacheByIdentity; // 进程缓存（PID+CreateTime）。
    std::unordered_map<std::string, ks::process::CounterSample> m_counterSampleByIdentity; // 差值样本。
    QHash<QString, QIcon> m_iconCacheByPath;  // 进程图标缓存，避免重复提取。
    std::unordered_map<std::string, QPointer<ProcessDetailWindow>> m_detailWindowByIdentity; // 详情窗口缓存（同进程复用窗口）。
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_detailWindowLastSyncTimeByIdentity; // 详情窗口最近一次同步时间，避免每轮刷新都触发重型解析。
    std::string m_trackedSelectedIdentityKey; // 当前选中进程 identityKey；表格刷新重建后用于恢复高亮。
    int m_trackedSelectedColumn = 0;          // 当前选中列索引；恢复 currentItem 时尽量保持用户焦点列。
    std::vector<ks::process::SystemThreadRecord> m_threadRecordList; // 线程页最近一次刷新结果缓存。
    std::string m_threadDiagnosticText;       // 线程页最近一次刷新诊断文本。

    // ======== 右键菜单绑定状态 ========
    std::string m_contextActionIdentityKey;      // 当前菜单动作绑定的 identityKey。
    ks::process::ProcessRecord m_contextActionRecord{}; // identity 在刷新中失效时的兜底副本。
    bool m_hasContextActionRecord = false;
    bool m_contextMenuVisible = false;           // 菜单弹出期间用于冻结周期刷新。
    std::uint32_t m_threadContextActionTid = 0;  // 线程右键菜单绑定的 TID。
    std::uint32_t m_threadContextActionPid = 0;  // 线程右键菜单绑定的 PID。
    bool m_threadContextMenuVisible = false;     // 线程菜单弹出期间冻结线程刷新。

    // ======== 线程刷新调度 ========
    bool m_threadRefreshInProgress = false;      // 线程页是否存在进行中的后台刷新任务。
    std::uint64_t m_threadRefreshTicket = 0;     // 线程页刷新序号（用于丢弃过期结果）。
    bool m_initialRefreshScheduled = false;      // 首次显示时是否已安排首轮刷新。
};
