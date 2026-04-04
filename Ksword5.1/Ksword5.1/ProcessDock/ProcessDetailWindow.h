#pragma once

// ============================================================
// ProcessDetailWindow.h
// 作用：
// - 提供“进程详细信息”独立窗口（非 Dock、非阻塞）；
// - 每个进程可打开独立窗口，包含 详细信息 / 线程 / 操作 / 模块 / 令牌 / PEB 六个 Tab；
// - 支持模块刷新、右键操作、DLL 注入、Shellcode 注入等能力。
// ============================================================

#include "../Framework.h"

#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

// 前置声明：减少头文件依赖，提升编译速度。
class QCheckBox;
class QComboBox;
class QEvent;
class QFormLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QPoint;
class CodeEditorWidget;

class ProcessDetailWindow final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数作用：
    // - 接收进程基础快照作为窗口初始数据；
    // - 初始化所有 UI 与交互连接；
    // - 启动模块页首次异步刷新。
    explicit ProcessDetailWindow(const ks::process::ProcessRecord& baseRecord, QWidget* parent = nullptr);

    // updateBaseRecord 作用：
    // - 用外部最新进程快照更新窗口展示；
    // - 不会销毁窗口，只刷新显示文本与状态。
    void updateBaseRecord(const ks::process::ProcessRecord& baseRecord);

    // pid 作用：返回当前窗口绑定进程 PID。
    std::uint32_t pid() const;

signals:
    // requestOpenProcessByPid 作用：
    // - 在“转到父进程”按钮点击时发出；
    // - 由 ProcessDock 统一接收并打开对应进程详情窗口。
    void requestOpenProcessByPid(std::uint32_t pid);

    // requestOpenHandleDockByPid 作用：
    // - 在“跳转句柄”按钮点击时发出；
    // - 由 ProcessDock 转发给 MainWindow 打开句柄 Dock 并按 PID 过滤。
    void requestOpenHandleDockByPid(std::uint32_t pid);

private:
    // ModuleRefreshResult：模块页后台刷新结果数据结构。
    struct ModuleRefreshResult
    {
        ks::process::ProcessModuleSnapshot moduleSnapshot; // 模块 + 线程快照。
        std::uint64_t elapsedMs = 0;                       // 后台刷新耗时（毫秒）。
        bool includeSignatureCheck = false;                // 本轮是否执行签名校验。
    };

    // ThreadInspectItem：线程细节页单行数据。
    struct ThreadInspectItem
    {
        std::uint32_t threadId = 0;        // 线程 ID。
        QString stateText;                 // 状态文本（运行/结束/未知）。
        int priorityValue = 0;             // 线程优先级值。
        quint64 switchCount = 0;           // 上下文切换计数（当前实现可能为 0）。
        QString startAddressText;          // 线程起始地址（十六进制）。
        QString tebAddressText;            // 线程 TEB 地址（十六进制）。
        QString affinityText;              // 线程亲和性文本。
        QString registerSummaryText;       // 寄存器摘要文本。
    };

    // ThreadInspectRefreshResult：线程细节异步刷新结果。
    struct ThreadInspectRefreshResult
    {
        std::vector<ThreadInspectItem> rows; // 线程行数据。
        QString diagnosticText;              // 诊断文本。
        std::uint64_t elapsedMs = 0;         // 刷新耗时（毫秒）。
    };

    // TextRefreshResult：令牌页/PEB 页文本刷新结果。
    struct TextRefreshResult
    {
        QString detailText;               // 展示文本内容。
        QString diagnosticText;           // 诊断文本。
        std::uint64_t elapsedMs = 0;      // 刷新耗时（毫秒）。
    };

private:
    // ======== UI 初始化 ========
    // changeEvent 作用：
    // - 监听调色板/样式变化；
    // - 在深浅色切换后重建内部样式，避免进程详情页残留白底。
    // 调用方式：Qt 自动触发。
    // 参数 event：变更事件对象。
    // 返回：无。
    void changeEvent(QEvent* event) override;

    void initializeUi();
    // applyThemeStyle 作用：
    // - 给详情窗口内部控件统一应用深浅色样式；
    // - 显式强制 Window/Base 颜色，规避 Win11 自动背景接管问题。
    // 调用方式：initializeUi 完成控件创建后调用；changeEvent 中再次调用。
    // 参数：无。
    // 返回：无。
    void applyThemeStyle();
    void initializeDetailTab();
    void initializeThreadTab();
    void initializeActionTab();
    void initializeModuleTab();
    void initializeTokenTab();
    void initializePebTab();
    void initializeConnections();

    // ======== 详情页刷新 ========
    void refreshDetailTabTexts();
    void refreshParentProcessSection();
    void updateWindowTitle();
    void requestAsyncThreadInspectRefresh();
    void applyThreadInspectResult(const ThreadInspectRefreshResult& refreshResult);
    void updateThreadInspectStatusLabel(const QString& statusText, bool refreshing);

    // ======== 令牌页/PEB页刷新 ========
    void requestAsyncTokenRefresh();
    void requestAsyncPebRefresh();
    void applyTokenRefreshResult(const TextRefreshResult& refreshResult);
    void applyPebRefreshResult(const TextRefreshResult& refreshResult);

    // ======== 模块页刷新 ========
    void requestAsyncModuleRefresh(bool forceRefresh);
    void applyModuleRefreshResult(const ModuleRefreshResult& refreshResult);
    void rebuildModuleTable();
    void updateModuleStatusLabel(const QString& statusText, bool refreshing);

    // ======== 模块表右键 ========
    void showModuleContextMenu(const QPoint& localPosition);
    void copyCurrentModuleCell();
    void copyCurrentModuleRow();
    void openCurrentModuleFolder();
    void unloadCurrentModule();
    void suspendCurrentModuleThread();
    void resumeCurrentModuleThread();
    void terminateCurrentModuleThread();

    // ======== 操作页动作 ========
    void executeTaskKillAction(bool forceKill);
    void executeTerminateProcessAction();
    void executeTerminateThreadsAction();
    void executeSelectedTerminateAction();
    void executeSuspendProcessAction();
    void executeResumeProcessAction();
    void executeSetCriticalAction(bool enableCritical);
    void executeSetPriorityAction();
    void executeInjectInvalidShellcodeAction();
    void executeInjectDllAction();
    void executeInjectShellcodeAction();

    // ======== 工具函数 ========
    QIcon resolveProcessIcon(const std::string& processPath, int iconPixelSize);
    QString formatModuleSizeText(std::uint32_t moduleSizeBytes) const;
    QString formatHexText(std::uint64_t value) const;
    // readBinaryFile 作用：读取二进制文件到缓冲区，并沿用调用方传入的同一 kLogEvent 输出过程日志。
    // 调用方式：由注入动作函数传入 actionEvent，以保证“动作+文件读取”日志链路一致。
    // 参数 filePath：文件路径；bufferOut：输出缓冲区；errorTextOut：错误信息；actionEvent：同链路日志事件对象。
    // 返回值：读取成功返回 true，失败返回 false。
    bool readBinaryFile(
        const QString& filePath,
        std::vector<std::uint8_t>& bufferOut,
        std::string& errorTextOut,
        const kLogEvent& actionEvent) const;
    // showActionResultMessage 作用：统一记录动作结果（不弹框），并复用外层传入的同一 kLogEvent 维持调用链。
    // 调用方式：动作函数先创建 kLogEvent，再将该事件对象传入本函数。
    // 参数 title：动作标题；actionOk：动作是否成功；detailText：动作详情；actionEvent：同链路日志事件对象。
    // 返回值：无。
    void showActionResultMessage(const QString& title, bool actionOk, const std::string& detailText, const kLogEvent& actionEvent);
    ks::process::ProcessModuleRecord* selectedModuleRecord();

private:
    // ======== 当前绑定进程基础数据 ========
    ks::process::ProcessRecord m_baseRecord;   // 当前窗口绑定进程快照。
    std::string m_identityKey;                 // PID+CreateTime 组成的 identity 字符串。

    // ======== 根布局与 Tabs ========
    QVBoxLayout* m_rootLayout = nullptr;       // 窗口根布局。
    QTabWidget* m_tabWidget = nullptr;         // 详情窗口全部 Tab 的容器。
    QWidget* m_detailTab = nullptr;            // “详细信息”页。
    QWidget* m_threadTab = nullptr;            // “线程”页。
    QWidget* m_actionTab = nullptr;            // “操作”页。
    QWidget* m_moduleTab = nullptr;            // “模块”页。
    QWidget* m_tokenTab = nullptr;             // “令牌”页。
    QWidget* m_pebTab = nullptr;               // “PEB”页。

    // ======== 详细信息页控件 ========
    QVBoxLayout* m_detailLayout = nullptr;     // 详细页总布局。
    QLabel* m_processIconLabel = nullptr;      // 顶部进程图标（40px）。
    QLabel* m_processTitleLabel = nullptr;     // 顶部标题（进程名 + PID）。
    QLineEdit* m_pathLineEdit = nullptr;       // 程序路径（只读）。
    QPushButton* m_copyPathButton = nullptr;   // 复制路径按钮。
    QPushButton* m_openPathFolderButton = nullptr; // 打开路径按钮。
    QLineEdit* m_commandLineEdit = nullptr;    // 启动命令行（只读）。
    QPushButton* m_copyCommandButton = nullptr; // 复制命令行按钮。
    QLabel* m_parentIconLabel = nullptr;       // 父进程图标（20px）。
    QLabel* m_parentInfoLabel = nullptr;       // 父进程名 + PID。
    QPushButton* m_openHandleDockButton = nullptr; // 跳转到句柄 Dock 按钮（按当前 PID 过滤）。
    QPushButton* m_gotoParentButton = nullptr; // 转到父进程按钮。

    QLabel* m_detailStartTimeValue = nullptr;  // 启动时间值。
    QLabel* m_detailUserValue = nullptr;       // 用户值。
    QLabel* m_detailAdminValue = nullptr;      // 是否管理员值。
    QLabel* m_detailArchitectureValue = nullptr; // 架构值。
    QLabel* m_detailPriorityValue = nullptr;   // 优先级值。
    QLabel* m_detailSessionValue = nullptr;    // 会话 ID 值。
    QLabel* m_detailThreadCountValue = nullptr; // 线程数值。
    QLabel* m_detailHandleCountValue = nullptr; // 句柄数值。
    QLabel* m_detailCpuValue = nullptr;        // CPU 当前占用值。
    QLabel* m_detailRamValue = nullptr;        // RAM 当前占用值。
    QLabel* m_detailDiskValue = nullptr;       // DISK 当前占用值。
    QLabel* m_detailSignatureValue = nullptr;  // 数字签名状态值。

    // ======== 线程页控件 ========
    QVBoxLayout* m_threadLayout = nullptr;     // 线程页总布局。
    QPushButton* m_refreshThreadInspectButton = nullptr; // 刷新线程细节按钮。
    QLabel* m_threadInspectStatusLabel = nullptr; // 线程细节刷新状态。
    QTableWidget* m_threadInspectTable = nullptr; // 线程细节表格。

    // ======== 操作页控件 ========
    QVBoxLayout* m_actionLayout = nullptr;     // 操作页总布局。
    QComboBox* m_terminateActionCombo = nullptr; // 结束方案下拉框。
    QPushButton* m_executeTerminateActionButton = nullptr; // 执行当前结束方案按钮。
    QPushButton* m_suspendProcessButton = nullptr; // 挂起进程。
    QPushButton* m_resumeProcessButton = nullptr; // 恢复进程。
    QPushButton* m_setCriticalButton = nullptr; // 设为关键进程。
    QPushButton* m_clearCriticalButton = nullptr; // 取消关键进程。
    QPushButton* m_injectInvalidShellcodeButton = nullptr; // 注入无效 shellcode。

    QComboBox* m_priorityCombo = nullptr;      // 优先级选择框。
    QPushButton* m_applyPriorityButton = nullptr; // 应用优先级按钮。

    QLineEdit* m_dllPathLineEdit = nullptr;    // DLL 路径输入框。
    QPushButton* m_browseDllButton = nullptr;  // 浏览 DLL 按钮。
    QPushButton* m_injectDllButton = nullptr;  // 执行 DLL 注入按钮。

    QLineEdit* m_shellcodePathLineEdit = nullptr; // shellcode 文件路径输入框。
    QPushButton* m_browseShellcodeButton = nullptr; // 浏览 shellcode 文件按钮。
    QPushButton* m_injectShellcodeButton = nullptr; // 执行 shellcode 注入按钮。

    // ======== 模块页控件 ========
    QVBoxLayout* m_moduleLayout = nullptr;     // 模块页总布局。
    QHBoxLayout* m_moduleTopBarLayout = nullptr; // 模块页顶部工具栏布局。
    QPushButton* m_refreshModuleButton = nullptr; // 模块刷新按钮。
    QCheckBox* m_signatureCheckBox = nullptr;  // 是否刷新时做签名校验。
    QLabel* m_moduleStatusLabel = nullptr;     // 模块刷新状态标签。
    QTreeWidget* m_moduleTable = nullptr;      // 模块表格。

    std::vector<ks::process::ProcessModuleRecord> m_moduleRecords; // 当前模块数据缓存。

    bool m_moduleRefreshing = false;           // 模块刷新进行中标记。
    bool m_firstModuleRefreshDone = false;     // 首轮模块刷新是否已完成。
    std::uint64_t m_moduleRefreshTicket = 0;   // 模块刷新序号（防乱序）。
    int m_moduleRefreshProgressPid = 0;        // 首轮模块刷新对应的 kPro 任务 PID。

    // ======== 线程细节刷新状态 ========
    bool m_threadInspectRefreshing = false;        // 线程细节是否正在刷新。
    std::uint64_t m_threadInspectRefreshTicket = 0;// 线程细节刷新序号。
    int m_threadInspectRefreshProgressPid = 0;     // 线程细节刷新对应进度 PID。

    // ======== 令牌页控件与状态 ========
    QVBoxLayout* m_tokenLayout = nullptr;          // 令牌页布局。
    QPushButton* m_refreshTokenButton = nullptr;   // 刷新令牌信息按钮。
    QLabel* m_tokenStatusLabel = nullptr;          // 令牌页状态文本。
    CodeEditorWidget* m_tokenDetailOutput = nullptr; // 令牌信息输出框（统一文本编辑器组件，只读）。
    bool m_tokenRefreshing = false;                // 令牌页刷新状态。
    std::uint64_t m_tokenRefreshTicket = 0;        // 令牌页刷新序号。
    int m_tokenRefreshProgressPid = 0;             // 令牌页刷新进度 PID。

    // ======== PEB页控件与状态 ========
    QVBoxLayout* m_pebLayout = nullptr;            // PEB 页布局。
    QPushButton* m_refreshPebButton = nullptr;     // 刷新 PEB 信息按钮。
    QLabel* m_pebStatusLabel = nullptr;            // PEB 页状态文本。
    CodeEditorWidget* m_pebDetailOutput = nullptr;   // PEB 信息输出框（统一文本编辑器组件，只读）。
    bool m_pebRefreshing = false;                  // PEB 页刷新状态。
    std::uint64_t m_pebRefreshTicket = 0;          // PEB 页刷新序号。
    int m_pebRefreshProgressPid = 0;               // PEB 页刷新进度 PID。
    bool m_themeStyleApplying = false;             // 主题样式重建防重入标记，避免 changeEvent 循环触发。

    // 图标缓存：路径 -> 图标，避免重复读取系统图标。
    QHash<QString, QIcon> m_iconCacheByPath;
};
