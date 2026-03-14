#pragma once

// ============================================================
// ProcessDetailWindow.h
// 作用：
// - 提供“进程详细信息”独立窗口（非 Dock、非阻塞）；
// - 每个进程可打开独立窗口，包含 详细信息 / 操作 / 模块 三个 Tab；
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
class QFormLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QPoint;

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

private:
    // ModuleRefreshResult：模块页后台刷新结果数据结构。
    struct ModuleRefreshResult
    {
        ks::process::ProcessModuleSnapshot moduleSnapshot; // 模块 + 线程快照。
        std::uint64_t elapsedMs = 0;                       // 后台刷新耗时（毫秒）。
        bool includeSignatureCheck = false;                // 本轮是否执行签名校验。
    };

private:
    // ======== UI 初始化 ========
    void initializeUi();
    void initializeDetailTab();
    void initializeActionTab();
    void initializeModuleTab();
    void initializeConnections();

    // ======== 详情页刷新 ========
    void refreshDetailTabTexts();
    void refreshParentProcessSection();
    void updateWindowTitle();

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
    bool readBinaryFile(const QString& filePath, std::vector<std::uint8_t>& bufferOut, std::string& errorTextOut) const;
    void showActionResultMessage(const QString& title, bool actionOk, const std::string& detailText);
    ks::process::ProcessModuleRecord* selectedModuleRecord();

private:
    // ======== 当前绑定进程基础数据 ========
    ks::process::ProcessRecord m_baseRecord;   // 当前窗口绑定进程快照。
    std::string m_identityKey;                 // PID+CreateTime 组成的 identity 字符串。

    // ======== 根布局与 Tabs ========
    QVBoxLayout* m_rootLayout = nullptr;       // 窗口根布局。
    QTabWidget* m_tabWidget = nullptr;         // 三个 Tab 的容器。
    QWidget* m_detailTab = nullptr;            // “详细信息”页。
    QWidget* m_actionTab = nullptr;            // “操作”页。
    QWidget* m_moduleTab = nullptr;            // “模块”页。

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

    // ======== 操作页控件 ========
    QVBoxLayout* m_actionLayout = nullptr;     // 操作页总布局。
    QPushButton* m_taskKillButton = nullptr;   // Taskkill。
    QPushButton* m_taskKillForceButton = nullptr; // Taskkill /f。
    QPushButton* m_terminateProcessButton = nullptr; // TerminateProcess。
    QPushButton* m_terminateThreadsButton = nullptr; // TerminateThread(全部)。
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

    // 图标缓存：路径 -> 图标，避免重复读取系统图标。
    QHash<QString, QIcon> m_iconCacheByPath;
};

