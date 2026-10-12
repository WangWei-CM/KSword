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
#include <QThreadPool>
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
        // findMatchingRule 作用：
        // - 按"更具体优先"的顺序找规则：PID/映像名/映像路径三种具体规则任意命中就
        //   立即返回；都不命中时才退回到 targetKind=ALL 的全局规则（如果启用了）；
        //   一个进程被具体规则和全局规则同时覆盖时，具体规则的读/写/枚举动作生效，
        //   不会被全局规则的"仅记录"覆盖掉。
        const ClipboardGuardRule* findMatchingRule(const ks::process::ProcessRecord& processRecord) const;
        // findMatchingRuleIn：findMatchingRule 的无状态版本，供后台线程用一份
        // 规则快照做匹配，不直接触碰只能在 UI 线程读写的 m_rules。
        static const ClipboardGuardRule* findMatchingRuleIn(
            const std::vector<ClipboardGuardRule>& rules, const ks::process::ProcessRecord& processRecord);
        // ensureProcessProtected 现在可能在后台线程（QThreadPool）被调用，因此不再
        // 直接触碰 m_statusLabel 等 UI 控件；失败信息改为写进 errorTextOut，由调用方
        // 统一汇总后再排回 UI 线程展示。
        bool ensureProcessProtected(
            const ks::process::ProcessRecord& processRecord, const ClipboardGuardRule& matchedRule, QString* errorTextOut = nullptr);
        void teardownSession(std::uint32_t pid);
        void startPipeReadThreadForPid(std::uint32_t pid, const QString& sessionId, const ClipboardGuardRule& rule, ks::winapi_monitor::SessionLease lease);
        void enqueuePendingRow(ClipboardGuardEventRow rowValue);
        void flushPendingRows();
        void appendEventRow(const ClipboardGuardEventRow& rowValue);
        bool writeSessionConfigForPid(std::uint32_t pid, const ClipboardGuardRule& matchedRule,
            const QString& sessionId, QString* errorTextOut) const;
        QString resolveProcessNameForPid(quint32 pid);

    private:
        struct Session
        {
            ks::winapi_monitor::SessionLease lease;
            std::uint32_t pid = 0;
            QString sessionId;
            quint32 readAction = 0;
            quint32 writeAction = 0;
            quint32 enumAction = 0;
            std::unique_ptr<std::thread> pipeThread;
            std::atomic_bool stopFlag{ false };
            std::atomic_bool pipeThreadExited{ false };
            std::atomic_bool hooksInstalled{ false };
            std::atomic<std::uintptr_t> pipeHandleValue{ 0 };
        };

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

        // ---- 会话（每个受管进程一个命名管道读取线程）----
        std::vector<std::unique_ptr<Session>> m_sessions;
        std::mutex m_sessionsMutex;
        // 只由串行扫描线程访问；PID 必须连同创建时间核对，防止 PID 复用后
        // 把新进程误认为仍驻留着上一进程的 Agent。
        std::unordered_map<quint32, std::uint64_t> m_residentAgentCreationTimes;

        std::deque<ClipboardGuardEventRow> m_pendingRows;
        std::mutex m_pendingMutex;
        static constexpr std::size_t kPendingRowCapacity = 8000;
        static constexpr std::size_t kUiFlushRowLimit = 160;
        QTimer* m_uiFlushTimer = nullptr;
        QTimer* m_processPollTimer = nullptr;
        std::atomic_bool m_processScanInFlight{ false };
        // m_scanThreadPool：本页面私有的线程池，专门跑 refreshProcessListAndSessionsAsync
        // 的匹配/注入/会话回收任务。用私有池而不是 QThreadPool::globalInstance()，
        // 是因为析构函数需要 waitForDone() 确保没有任务还在访问 this 才能继续销毁——
        // 后台任务里会解引用 QPointer<ClipboardGuardPage>，QPointer 本身不是线程安全的，
        // 必须靠"任务存活期间对象绝不会开始析构"这个前提来保证它读到的值不会被并发修改。
        // 用全局线程池就无法只等待属于本页面的任务，会等到不相关的任务。
        QThreadPool m_scanThreadPool;

        // m_processNameCache 在后台扫描线程里整表重建、由 UI 线程（命名管道事件回调）
        // 读取，两侧不在同一线程，必须靠这把锁保护，否则并发读写 unordered_map 是未定义行为。
        std::unordered_map<quint32, QString> m_processNameCache;
        std::mutex m_processNameCacheMutex;

        ksword::ark::DriverClient m_driverClient;
    };
}
