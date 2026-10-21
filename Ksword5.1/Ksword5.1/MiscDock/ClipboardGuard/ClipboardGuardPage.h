#pragma once

// ============================================================
// ClipboardGuardPage.h
// 作用：
// 1) "剪贴板保护"杂项子页——按 PID/进程名/路径 管理剪贴板读/写/枚举策略；
// 2) 策略权威存储在驱动（ArkDriverClient::setClipboardPolicy），实际拦截发生在
//    被注入到受管进程里的 APIMonitor_x64.dll（复用现有 hook 引擎，见
//    APIMonitor_x64/hook/ClipboardGuardHook.* / ClipboardGuardVTableHook.* /
//    ClipboardGuardWin32u.*）；
// 3) 本页只做：规则管理 UI、按规则匹配当前运行进程并注入/下发配置、
//    接收命名管道事件并展示、右键快捷操作、详情/调用栈弹窗。
// ============================================================

#include "../../Framework.h"
#include "../../MonitorDock/WinApiMonitorProtocol.h"
#include "../../ArkDriverClient/ArkDriverClient.h"

#include <QString>
#include <QElapsedTimer>
#include "../../UI/AsyncCleanupRegistry.h"
#include <condition_variable>
#include <optional>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPoint;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QVBoxLayout;

namespace ks::misc
{
    // ClipboardGuardColumnLayout：事件表 A/B 列组预设；Custom 表示用户已手动
    // 调整过列显隐，此时 A/B 按钮都不再着色（仓库 AGENTS.md 的 A/B 列组规范）。
    enum class ClipboardGuardColumnLayout
    {
        PresetA,
        PresetB,
        Custom
    };

    // ClipboardGuardRule：一条剪贴板策略规则。字段取值直接对应驱动协议
    // shared/driver/KswordArkClipboardPolicyIoctl.h 里的
    // KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_*/ACTION_*，不在 UI 层另建一套编码，
    // 避免下发/回读时来回转换出错。
    struct ClipboardGuardRule
    {
        quint32 ruleId = 0;
        bool enabled = true;
        quint32 targetKind = 0;       // NONE=0 / PID=1 / IMAGE_NAME=2 / IMAGE_PATH=3 / ALL=4（全局监控）
        quint32 targetProcessId = 0;
        QString targetImage;
        QString ruleName;
        quint32 readAction = 0;       // ALLOW=0 / BLOCK=1 / LOG_ONLY=2
        quint32 writeAction = 0;
        quint32 enumAction = 0;
    };

    // ClipboardGuardQuickAction：事件表右键菜单里针对"这一行所属进程"的快捷动作。
    // BlockAll 与 LogOnly 都是"一次性设置三个方向"的便捷入口，底层仍然落回同一套
    // 独立的 readAction/writeAction/enumAction 三个字段，不是第四种状态。
    enum class ClipboardGuardQuickAction
    {
        BlockRead,
        BlockWrite,
        BlockAll,
        LogOnly
    };

    // ClipboardGuardEventRow：事件表一行的展示数据，从 Agent 上报事件包的
    // detail 文本（ClipboardGuardCommon.cpp::ReportClipboardEvent 拼出来的
    // key=value 格式）解析得到。
    struct ClipboardGuardEventRow
    {
        QString timeText;
        quint32 pid = 0;
        quint32 tid = 0;
        QString processText;
        QString operationText;   // READ / WRITE / ENUM
        QString formatText;
        QString resultText;      // Allowed / Blocked / LoggedOnly
        QString sessionText;
        QString integrityText;
        QString ownerText;
        quint64 seq = 0;
    };

    // Value-only ingress survives the widget until pipe cleanup finishes.
    struct ClipboardGuardInbox
    {
        static constexpr std::size_t PendingCapacity = 8000;
        void push(ClipboardGuardEventRow row);
        std::vector<ClipboardGuardEventRow> take(std::size_t limit);
        QString processName(quint32 pid);
        void replaceNames(std::unordered_map<quint32, QString> names);
        void noteAgentDropped(quint64 session, quint64 count);
        void finishSessionDrops(quint64 session);
        struct Counts { std::uint64_t received = 0, dropped = 0, agentDropped = 0; std::size_t pending = 0; };
        Counts counts() const;
        mutable std::mutex mutex;
        std::deque<ClipboardGuardEventRow> pendingRows;
        std::unordered_map<quint32, QString> processNames;
        std::unordered_map<quint64, quint64> agentDropped;
        std::unordered_map<quint32, std::uint64_t> residentAgentCreationTimes;
        std::uint64_t received = 0;
        std::uint64_t dropped = 0;
        std::uint64_t completedAgentDropped = 0;
    };

    // Dedicated value owner; no QWidget/QPointer/GUI callback crosses its worker.
    class ClipboardGuardRuntime final : public std::enable_shared_from_this<ClipboardGuardRuntime>
    {
    public:
        explicit ClipboardGuardRuntime(std::shared_ptr<ClipboardGuardInbox> inbox);
        ~ClipboardGuardRuntime() = default;
        bool requestScan(std::vector<ClipboardGuardRule> rules, QString agentBasePath);
        void requestStop();
        bool hasFinished() const { return m_finished.load(); }
        bool hasStarted() const { return m_started.load(); }
        struct Summary { std::size_t ruleCount = 0, protectedCount = 0; int failureCount = 0; QString lastError; bool stopping = false; };
        Summary summary() const;
    private:
        struct Session
        {
            ks::winapi_monitor::SessionLease lease;
            std::uint32_t pid = 0;
            QString sessionId;
            std::uint64_t creationTime100ns = 0;
            quint32 readAction = 0;
            quint32 writeAction = 0;
            quint32 enumAction = 0;
            std::unique_ptr<std::thread> pipeThread;
            std::atomic_bool stopFlag{ false };
            std::atomic_bool pipeThreadExited{ false };
            std::atomic_bool hooksInstalled{ false };
            std::atomic<std::uintptr_t> pipeHandleValue{ 0 };
        };

        struct ScanRequest { std::vector<ClipboardGuardRule> rules; QString agentBasePath; };
        void run();
        void scan(const ScanRequest& request);
        static const ClipboardGuardRule* findMatchingRuleIn(const std::vector<ClipboardGuardRule>& rules,
            const ks::process::ProcessRecord& processRecord);
        bool ensureProcessProtected(const ks::process::ProcessRecord& processRecord,
            const ClipboardGuardRule& matchedRule, QString* errorTextOut);
        bool writeSessionConfigForPid(std::uint32_t pid, const ClipboardGuardRule& rule,
            const QString& sessionId, QString* errorTextOut) const;
        static void writeStopFlag(std::uint32_t pid);
        void teardownSession(std::uint32_t pid);
        void startPipeReadThreadForPid(std::uint32_t pid, std::uint64_t creationTime,
            const QString& sessionId, const ClipboardGuardRule& rule, ks::winapi_monitor::SessionLease lease);
        const std::shared_ptr<ClipboardGuardInbox> m_inbox;
        std::vector<std::unique_ptr<Session>> m_sessions;
        mutable std::mutex m_sessionsMutex;
        mutable std::mutex m_queueMutex;
        std::condition_variable m_wake;
        std::optional<ScanRequest> m_pending;
        Summary m_summary;
        QString m_agentBasePath;
        std::atomic_bool m_stopRequested{ false }, m_started{ false }, m_finished{ false };
        std::shared_ptr<ks::ui::AsyncCleanupRegistry::Lease> m_cleanupLease;
    };

    class ClipboardGuardPage final : public QWidget
    {
        Q_OBJECT

    public:
        explicit ClipboardGuardPage(QWidget* parent = nullptr);
        ~ClipboardGuardPage() override;

        // notifyPageActivated：页签首次真正显示时按需从驱动回读规则、启动轮询。
        void notifyPageActivated();

        // EventColumn：事件表列索引。前 6 列是 A 组"概览"，后 5 列是 B 组
        // "来源与诊断"；两组共用同一张表，只是显隐不同（AGENTS.md A/B 列组规范
        // 要求两组都是精简视图，不是"A 追加扩展列"）。
        enum EventColumn
        {
            ColumnTime = 0,
            ColumnProcess,
            ColumnPid,
            ColumnOperation,
            ColumnFormat,
            ColumnResult,
            ColumnTid,
            ColumnSession,
            ColumnIntegrity,
            ColumnOwner,
            ColumnSeq,
            ColumnCount
        };

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        // ---- ClipboardGuardPage.cpp：UI 骨架、A/B 列组 ----
        void initializeUi();
        void initializeConnections();
        void applyColumnLayout(ClipboardGuardColumnLayout layout);
        void clearColumnPresetSelection();
        void updateColumnPresetButtons();
        void showHeaderContextMenu(const QPoint& position);
        static QTableWidgetItem* createReadOnlyItem(const QString& textValue);

        // ---- ClipboardGuardPage.RuleEditor.cpp：右键菜单、规则对话框、详情弹窗 ----
        void showEventTableContextMenu(const QPoint& position);
        void openAddProcessRuleDialog();
        void removeSelectedRule();
        void applyQuickActionToSelectedRow(ClipboardGuardQuickAction action);
        void showClipboardContentsForSelectedRow();
        void showCallStackForSelectedRow();
        int selectedEventRow() const;

        // toggleGlobalMonitor 作用：
        // - 输入：enabled 为工具栏"全局监控"勾选框的新状态；
        // - 处理：勾选时新增/启用一条 targetKind=ALL 的规则（默认三个方向都是
        //   仅记录，不新增拦截面）；取消勾选时移除该条规则；随后统一走
        //   refreshRuleTable/syncRulesToDriver/refreshProcessListAndSessionsAsync
        //   三步，和手动编辑规则表的收尾完全一致；
        // - 返回：无返回值。
        void toggleGlobalMonitor(bool enabled);
        // hasEnabledGlobalRule 作用：查询 m_rules 里是否存在一条已启用的全局规则，
        // 供 refreshRuleTable 同步复选框勾选状态、避免两处状态各自维护而漂移。
        bool hasEnabledGlobalRule() const;

        // ---- ClipboardGuardPage.Session.cpp：驱动同步、注入编排、命名管道事件 ----
        void refreshRuleTable();
        void syncRulesToDriver();
        void loadRulesFromDriver();
        void refreshProcessListAndSessionsAsync();
        void flushPendingRows();
        void scheduleEventProjection();
        void trimRetainedEventRows();
        void appendEventRow(const ClipboardGuardEventRow& rowValue);
        void updateRuntimeStatus();

    private:
        // ---- 布局 ----
        QVBoxLayout* m_rootLayout = nullptr;
        QHBoxLayout* m_toolbarLayout = nullptr;
        QPushButton* m_addRuleButton = nullptr;
        QPushButton* m_removeRuleButton = nullptr;
        QPushButton* m_refreshButton = nullptr;
        QCheckBox* m_globalMonitorCheck = nullptr; // m_globalMonitorCheck：全局监控开关，勾选后监控系统上几乎全部进程。
        QWidget* m_columnPresetWidget = nullptr;
        QPushButton* m_columnPresetAButton = nullptr;
        QPushButton* m_columnPresetBButton = nullptr;
        QLabel* m_statusLabel = nullptr;
        QTableWidget* m_ruleTable = nullptr;
        QTableWidget* m_eventTable = nullptr;
        ClipboardGuardColumnLayout m_columnLayout = ClipboardGuardColumnLayout::PresetA;
        bool m_applyingColumnLayout = false;
        bool m_hasActivatedOnce = false;

        // ---- 规则 ----
        std::vector<ClipboardGuardRule> m_rules;
        quint32 m_nextLocalRuleId = 1;

        const std::shared_ptr<ClipboardGuardInbox> m_inbox;
        std::shared_ptr<ClipboardGuardRuntime> m_runtime;
        std::uint64_t m_evictedRows = 0;
        // One value slice survives budget yields; never capture accepted rows in
        // a latest-wins UI callback. Anchors belong to this existing table only.
        std::deque<ClipboardGuardEventRow> m_projectionRows;
        std::deque<QTableWidgetItem*> m_retainedEventAnchors;
        bool m_projectionScheduled = false;
        QElapsedTimer m_statusUpdateClock;
        static constexpr int kProjectionBudgetMs = 4;
        static constexpr int kRetainedEventRows = 6000;
        static constexpr std::size_t kUiFlushRowLimit = 160;
        QTimer* m_uiFlushTimer = nullptr;
        QTimer* m_processPollTimer = nullptr;

        ksword::ark::DriverClient m_driverClient;
    };
}
