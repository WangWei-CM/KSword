#pragma once

// ============================================================
// NetworkFirewallPage.h
// 作用：
// 1) 提供 System Informer 风格的 WFP 防火墙事件页；
// 2) 提供 Windows Firewall 规则管理（枚举 / 新增 / 编辑 / 启停 / 删除）；
// 3) 动态加载 fwpuclnt.dll，避免项目链接环境强依赖 Fwpuclnt.lib。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic> // std::atomic_bool：防止历史刷新并发。
#include <mutex>  // std::mutex：实时回调队列保护。
#include <vector> // std::vector：事件批量回投。

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTimer;
class QVBoxLayout;

// NetworkFirewallPage 说明：
// - 输入：Qt 父控件；
// - 处理：初始化防火墙事件与规则管理页，并按需启动 WFP 历史枚举/实时订阅；
// - 返回行为：无业务返回值，事件直接渲染到表格。
class NetworkFirewallPage final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - parent：Qt 父控件；
    // - 处理：创建 UI，并尝试启动历史刷新；
    // - 返回：无。
    explicit NetworkFirewallPage(QWidget* parent = nullptr);

    // 析构函数：
    // - 处理：停止实时订阅、关闭 BFE engine、释放 fwpuclnt.dll；
    // - 返回：无。
    ~NetworkFirewallPage() override;

    // FirewallEventEntry：
    // - 作用：保存一次 WFP net event 的展示字段；
    // - 处理逻辑：WFP 线程填充，UI 线程插入表格；
    // - 返回行为：纯数据结构，无函数返回。
    struct FirewallEventEntry
    {
        QString nameText;          // nameText：进程/应用名或事件名。
        QString actionText;        // actionText：DROP/Allowed 等动作。
        QString directionText;     // directionText：In/Out/FWD/BI。
        QString ruleText;          // ruleText：过滤器/规则名。
        QString descriptionText;   // descriptionText：过滤器描述或事件描述。
        QString localAddressText;  // localAddressText：本地地址。
        QString localPortText;     // localPortText：本地端口。
        QString localHostText;     // localHostText：本地主机名。
        QString remoteAddressText; // remoteAddressText：远端地址。
        QString remotePortText;    // remotePortText：远端端口。
        QString remoteHostText;    // remoteHostText：远端主机名。
        QString protocolText;      // protocolText：TCP/UDP/ICMP 或协议号。
        QString timestampText;     // timestampText：事件时间。
        bool isDrop = false;       // isDrop：是否用红色突出。
    };

    // FirewallRuleEntry：
    // - 作用：保存一条 Windows Firewall 规则的展示字段和原始控制字段；
    // - 处理逻辑：后台 COM 枚举填充，UI 线程写入规则表或编辑对话框；
    // - 返回行为：纯数据结构，无函数返回。
    struct FirewallRuleEntry
    {
        QString fingerprintText;      // fingerprintText：当前快照的匹配键，用于更新/启停定位。
        QString nameText;             // nameText：规则名称。
        QString descriptionText;      // descriptionText：规则描述。
        QString applicationText;      // applicationText：程序路径。
        QString serviceText;          // serviceText：服务名。
        QString localPortsText;       // localPortsText：本地端口。
        QString remotePortsText;      // remotePortsText：远端端口。
        QString localAddressesText;   // localAddressesText：本地地址。
        QString remoteAddressesText;  // remoteAddressesText：远端地址。
        QString groupingText;         // groupingText：规则分组。
        QString actionText;           // actionText：Allow/Block 文本。
        QString directionText;        // directionText：In/Out 文本。
        QString profilesText;         // profilesText：Domain/Private/Public 文本。
        QString protocolText;         // protocolText：TCP/UDP/Any 文本。
        bool enabled = false;         // enabled：规则启用状态。
        long actionValue = 0;         // actionValue：NET_FW_ACTION 原始值。
        long directionValue = 0;      // directionValue：NET_FW_RULE_DIRECTION 原始值。
        long profilesValue = 0;       // profilesValue：NET_FW_PROFILE_TYPE2 位掩码。
        long protocolValue = 0;       // protocolValue：协议原始值。
    };

private:

    // initializeUi 作用：
    // - 创建状态栏、事件页和规则页；
    // - 无输入参数；
    // - 无返回值。
    void initializeUi();

    // initializeConnections 作用：
    // - 连接事件刷新、规则刷新、编辑动作和定时消费队列；
    // - 无输入参数；
    // - 无返回值。
    void initializeConnections();

    // initializeEventMonitorUi 作用：
    // - 构建“事件监控”子页；
    // - 无输入参数；
    // - 无返回值。
    void initializeEventMonitorUi();

    // initializeRuleManagerUi 作用：
    // - 构建“规则管理”子页；
    // - 无输入参数；
    // - 无返回值。
    void initializeRuleManagerUi();

    // refreshHistoryAsync 作用：
    // - 后台枚举 WFP 历史事件；
    // - forceRefresh 表示用户主动刷新；
    // - 无返回值，结果回投 UI。
    void refreshHistoryAsync(bool forceRefresh);

    // startLiveMonitor 作用：
    // - 启动 BFE 会话、开启事件收集并订阅实时 WFP net event；
    // - 无输入参数；
    // - 无返回值，状态显示到 UI。
    void startLiveMonitor();

    // stopLiveMonitor 作用：
    // - 取消实时订阅并关闭当前 engine；
    // - 无输入参数；
    // - 无返回值。
    void stopLiveMonitor();

    // appendEventsToTable 作用：
    // - 输入：批量事件、是否清空旧内容；
    // - 处理：写入表格并应用 DROP 高亮；
    // - 无返回值。
    void appendEventsToTable(const std::vector<FirewallEventEntry>& eventList, bool clearBeforeAppend);

    // applyFilterToRows 作用：
    // - 根据搜索文本和“仅 DROP”状态隐藏表格行；
    // - 无输入参数；
    // - 无返回值。
    void applyFilterToRows();

    // flushLiveEventsToUi 作用：
    // - 周期性从实时队列取出事件并追加到 UI；
    // - 无输入参数；
    // - 无返回值。
    void flushLiveEventsToUi();

    // setStatusText 作用：
    // - 输入：状态文本；
    // - 处理：线程安全地回投状态标签；
    // - 无返回值。
    void setStatusText(const QString& statusText);

    // refreshRulesAsync 作用：
    // - 后台枚举 Windows Firewall 规则；
    // - forceRefresh 表示用户主动刷新；
    // - 无返回值，结果回投 UI。
    void refreshRulesAsync(bool forceRefresh);

    // appendRulesToTable 作用：
    // - 输入：批量规则、是否清空旧内容；
    // - 处理：写入规则表格；
    // - 无返回值。
    void appendRulesToTable(const std::vector<FirewallRuleEntry>& ruleList, bool clearBeforeAppend);

    // applyRuleFilterToRows 作用：
    // - 根据搜索文本和“仅启用”状态隐藏规则行；
    // - 无输入参数；
    // - 无返回值。
    void applyRuleFilterToRows();

    // updateRuleActionButtons 作用：
    // - 根据当前规则选择状态更新编辑/启停/删除按钮；
    // - 无输入参数；
    // - 无返回值。
    void updateRuleActionButtons();

    // addFirewallRule 作用：
    // - 打开新增规则对话框，并将规则加入系统防火墙；
    // - 无输入参数；
    // - 无返回值。
    void addFirewallRule();

    // editSelectedFirewallRule 作用：
    // - 编辑当前选中的一条规则；
    // - 无输入参数；
    // - 无返回值。
    void editSelectedFirewallRule();

    // toggleSelectedFirewallRuleEnabled 作用：
    // - 切换当前选中规则的启用状态；
    // - 无输入参数；
    // - 无返回值。
    void toggleSelectedFirewallRuleEnabled();

    // deleteSelectedFirewallRules 作用：
    // - 删除当前选中的一条或多条规则；
    // - 无输入参数；
    // - 无返回值。
    void deleteSelectedFirewallRules();

    // selectedRuleEntry 作用：
    // - 返回当前规则表选中的首条规则快照；
    // - 返回：找到时返回 true，并输出规则。
    bool selectedRuleEntry(FirewallRuleEntry* ruleEntryOut) const;

    // ruleNameDuplicateCount 作用：
    // - 统计当前快照中同名规则数量；
    // - 返回：同名规则计数。
    int ruleNameDuplicateCount(const QString& ruleNameText) const;

    // enumerateFirewallRulesSnapshot 作用：
    // - 后台枚举当前系统防火墙规则快照；
    // - 返回：规则列表，失败时通过 errorTextOut 返回错误。
    std::vector<FirewallRuleEntry> enumerateFirewallRulesSnapshot(QString* errorTextOut) const;

    // addFirewallRuleEntryToSystem 作用：
    // - 将一条新规则写入系统防火墙；
    // - 返回：成功时 true。
    bool addFirewallRuleEntryToSystem(const FirewallRuleEntry& ruleEntry, QString* errorTextOut) const;

    // updateFirewallRuleEntryInSystem 作用：
    // - 根据原始 fingerprint 定位并修改规则；
    // - 返回：成功时 true。
    bool updateFirewallRuleEntryInSystem(
        const QString& originalFingerprintText,
        const FirewallRuleEntry& updatedRuleEntry,
        QString* errorTextOut) const;

    // setFirewallRuleEnabledInSystem 作用：
    // - 根据 fingerprint 定位规则并切换启用状态；
    // - 返回：成功时 true。
    bool setFirewallRuleEnabledInSystem(
        const QString& fingerprintText,
        bool enabled,
        QString* errorTextOut) const;

    // deleteFirewallRuleFromSystem 作用：
    // - 根据规则名称删除系统防火墙规则；
    // - 返回：成功时 true。
    bool deleteFirewallRuleFromSystem(const QString& ruleNameText, QString* errorTextOut) const;

    // ensureWfpApiLoaded 作用：
    // - 动态加载 fwpuclnt.dll 并解析需要的 WFP 函数；
    // - 返回：全部关键函数可用时 true。
    bool ensureWfpApiLoaded(QString* errorTextOut);

    // openWfpEngine 作用：
    // - 输入：是否启用事件收集；
    // - 处理：打开 BFE engine，必要时设置 WFP net event collection；
    // - 返回：成功时 true，并在 engineHandleOut 输出句柄。
    bool openWfpEngine(bool enableCollection, void** engineHandleOut, QString* errorTextOut);

    // closeWfpEngine 作用：
    // - 输入：engine 句柄、是否关闭事件收集；
    // - 处理：可选关闭事件收集并关闭 BFE engine；
    // - 无返回值。
    void closeWfpEngine(void* engineHandle, bool disableCollection);

    // enumerateHistoryWithEngine 作用：
    // - 输入：已打开 BFE engine；
    // - 处理：调用 FwpmNetEventEnum* 获取历史事件；
    // - 返回：事件列表。
    std::vector<FirewallEventEntry> enumerateHistoryWithEngine(void* engineHandle, QString* errorTextOut);

    // convertWfpEventToEntry 作用：
    // - 输入：FWPM_NET_EVENT 指针；
    // - 处理：提取动作、方向、地址、端口、协议、规则名等字段；
    // - 返回：可显示事件。
    FirewallEventEntry convertWfpEventToEntry(const void* wfpEventPointer, void* engineHandle);

    // enqueueLiveEvent 作用：
    // - 输入：WFP 实时回调事件；
    // - 处理：转换后放入线程安全队列；
    // - 无返回值。
    void enqueueLiveEvent(const void* wfpEventPointer);

    // liveEventCallback 作用：
    // - 输入：WFP 事件回调上下文和事件指针；
    // - 处理：转发给当前页面实例；
    // - 无返回值。
    static void __stdcall liveEventCallback(void* context, const void* eventPointer);

private:
    QVBoxLayout* m_rootLayout = nullptr;       // m_rootLayout：页面根布局。
    QTabWidget* m_innerTabWidget = nullptr;    // m_innerTabWidget：事件监控/规则管理子页。
    QLabel* m_statusLabel = nullptr;           // m_statusLabel：状态和错误文本。
    QWidget* m_eventMonitorPage = nullptr;     // m_eventMonitorPage：事件监控子页。
    QPushButton* m_refreshHistoryButton = nullptr; // m_refreshHistoryButton：历史刷新按钮。
    QPushButton* m_startLiveButton = nullptr;  // m_startLiveButton：启动实时订阅。
    QPushButton* m_stopLiveButton = nullptr;   // m_stopLiveButton：停止实时订阅。
    QPushButton* m_clearButton = nullptr;      // m_clearButton：清空表格。
    QLineEdit* m_searchEdit = nullptr;         // m_searchEdit：搜索输入框。
    QCheckBox* m_dropOnlyCheck = nullptr;      // m_dropOnlyCheck：仅显示 DROP。
    QTableWidget* m_eventTable = nullptr;      // m_eventTable：防火墙事件表。
    QTimer* m_liveFlushTimer = nullptr;        // m_liveFlushTimer：实时队列消费定时器。
    std::atomic_bool m_refreshingHistory{ false }; // m_refreshingHistory：历史刷新互斥。
    std::atomic_bool m_liveRunning{ false };   // m_liveRunning：实时监控状态。
    std::mutex m_liveEventMutex;               // m_liveEventMutex：实时队列锁。
    std::vector<FirewallEventEntry> m_liveEventQueue; // m_liveEventQueue：实时事件队列。
    QWidget* m_ruleManagerPage = nullptr;      // m_ruleManagerPage：规则管理子页。
    QPushButton* m_refreshRulesButton = nullptr; // m_refreshRulesButton：规则刷新按钮。
    QPushButton* m_addRuleButton = nullptr;    // m_addRuleButton：新增规则按钮。
    QPushButton* m_editRuleButton = nullptr;   // m_editRuleButton：编辑规则按钮。
    QPushButton* m_toggleRuleButton = nullptr; // m_toggleRuleButton：启停规则按钮。
    QPushButton* m_deleteRuleButton = nullptr; // m_deleteRuleButton：删除规则按钮。
    QLineEdit* m_ruleSearchEdit = nullptr;     // m_ruleSearchEdit：规则搜索输入框。
    QCheckBox* m_ruleEnabledOnlyCheck = nullptr; // m_ruleEnabledOnlyCheck：仅显示启用规则。
    QTableWidget* m_ruleTable = nullptr;       // m_ruleTable：防火墙规则表。
    std::atomic_bool m_refreshingRules{ false }; // m_refreshingRules：规则刷新互斥。
    std::vector<FirewallRuleEntry> m_ruleEntryList; // m_ruleEntryList：规则快照缓存。
    void* m_fwpuclntModule = nullptr;          // m_fwpuclntModule：fwpuclnt.dll 模块句柄。
    void* m_liveEngineHandle = nullptr;        // m_liveEngineHandle：实时监控 BFE engine。
    void* m_liveSubscriptionHandle = nullptr;  // m_liveSubscriptionHandle：实时订阅句柄。
};
