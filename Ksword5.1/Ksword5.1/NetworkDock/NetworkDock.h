#pragma once

// ============================================================
// NetworkDock.h
// 作用：
// 1) 构建“网络”Dock 的完整侧边栏 Tab UI；
// 2) 展示全部收发方向 TCP/UDP 报文表格；
// 3) 提供组合过滤（PID/IP段/端口/包长）、进程限速、报文详情查看能力；
// 4) 提供 TCP/UDP 连接监控与 TCP 连接终止；
// 5) 提供“手动构造网络请求”可视化参数执行页。
// ============================================================

#include "../Framework.h"

#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <atomic>        // std::atomic_bool：存活主机扫描的并发状态控制。
#include <cstdint>       // std::uint32_t/std::uint64_t：PID、序号与长度字段。
#include <deque>         // std::deque：报文序号顺序缓存与后台待刷新队列。
#include <memory>        // std::unique_ptr：后台监控服务对象托管。
#include <mutex>         // std::mutex：后台线程与 UI 线程共享队列保护。
#include <optional>      // std::optional：可选过滤条件启停状态。
#include <unordered_map> // 序号 -> 报文实体缓存，支持详情回查。
#include <utility>       // std::pair：范围过滤（min/max）表达。
#include <vector>        // std::vector：连接快照缓存与渲染临时数组。

// Qt 前置声明：减少头文件耦合与编译开销。
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTimer;
class QUrl;
class QVBoxLayout;
class QWidget;
class QShowEvent;
class QDialog;
class QCompleter;
class QScrollArea;
class QStandardItemModel;
class CodeEditorWidget;
class MultiThreadDownloadSegmentBarWidget;

namespace ks::network
{
    struct HttpsProxyParsedEntry;
    class HttpsMitmProxyService;
}

// ============================================================
// NetworkDock
// 说明：
// - 抓包/限速底层能力由 ks::network::TrafficMonitorService 提供；
// - 连接快照与手动请求执行由 ks::network 工具函数提供；
// - 本类只负责 UI 呈现、过滤条件管理、用户交互与日志输出。
// ============================================================
class NetworkDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化 UI、绑定后台服务回调、连接交互信号。
    // - 参数 parent：Qt 父控件，可为空。
    explicit NetworkDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止后台抓包线程，避免窗口释放后仍有异步回调。
    ~NetworkDock() override;

protected:
    // showEvent：
    // - 首次显示时再初始化 HTTPS 代理服务实例；
    // - 避免主窗口启动阶段提前构造代理相关对象。
    void showEvent(QShowEvent* event) override;

private:
    // PacketTableColumn：流量监控主表列索引定义。
    enum class PacketTableColumn : int
    {
        Time = 0,       // 抓包时间（毫秒精度）。
        Protocol,       // 协议（TCP/UDP）。
        Direction,      // 方向（Outbound/Inbound）。
        Pid,            // 归属进程 PID。
        ProcessName,    // 进程名（带图标）。
        LocalEndpoint,  // 本地地址:端口。
        RemoteEndpoint, // 远端地址:端口。
        PacketSize,     // 总包长（字节）。
        PayloadSize,    // 负载长度（字节）。
        Preview,        // 负载预览（十六进制）。
        Count           // 列总数。
    };

    // RateLimitTableColumn：限速规则表格列索引定义。
    enum class RateLimitTableColumn : int
    {
        Pid = 0,            // 目标 PID。
        ProcessName,        // 目标进程名（快照查询补齐）。
        LimitKBps,          // 限速阈值（KB/s）。
        SuspendMs,          // 超限挂起时长（毫秒）。
        TriggerCount,       // 触发次数。
        CurrentWindowBytes, // 当前窗口累计字节。
        State,              // 状态（运行中/已挂起）。
        Count               // 列总数。
    };

    // TcpConnectionTableColumn：TCP 连接监控表列定义。
    enum class TcpConnectionTableColumn : int
    {
        State = 0,      // TCP 状态文本。
        Pid,            // 所属 PID。
        ProcessName,    // 进程名（带图标）。
        LocalEndpoint,  // 本地地址:端口。
        RemoteEndpoint, // 远端地址:端口。
        Count           // 列总数。
    };

    // UdpEndpointTableColumn：UDP 端点监控表列定义。
    enum class UdpEndpointTableColumn : int
    {
        Pid = 0,        // 所属 PID。
        ProcessName,    // 进程名（带图标）。
        LocalEndpoint,  // 本地地址:端口。
        Count           // 列总数。
    };

    // 范围类型别名：
    // - UInt32Range 用于 IPv4 主机序范围与包长范围；
    // - UInt16Range 用于端口范围。
    using UInt32Range = std::pair<std::uint32_t, std::uint32_t>;
    using UInt16Range = std::pair<std::uint16_t, std::uint16_t>;

    // MonitorTextRuleFieldKind：规则组中“文本列表类型”枚举。
    enum class MonitorTextRuleFieldKind : int
    {
        LocalAddress = 0, // 本地地址列表（支持 CIDR/范围/单 IP）。
        RemoteAddress,    // 远程地址列表（支持 CIDR/范围/单 IP）。
        LocalPort,        // 本地端口列表（支持单端口/范围）。
        RemotePort,       // 远程端口列表（支持单端口/范围）。
        PacketSize        // 包长列表（支持单值/范围，逗号分隔）。
    };

    // MonitorProcessTarget：进程过滤条目（PID + 名称 + 图标）。
    struct MonitorProcessTarget
    {
        std::uint32_t pid = 0;
        QString processName;
        QIcon processIcon;
    };

    // MonitorProcessCandidate：PID 输入补全候选项缓存。
    struct MonitorProcessCandidate
    {
        std::uint32_t pid = 0;
        QString processName;
        QString displayText;
        QString searchText;
        QIcon processIcon;
    };

    // MonitorTextRuleFieldUiState：地址/端口/包长列表字段的 UI + 数据状态。
    struct MonitorTextRuleFieldUiState
    {
        QString labelText;
        QLineEdit* inputEdit = nullptr;
        QPushButton* addButton = nullptr;
        QPushButton* clearButton = nullptr;
        QTableWidget* tableWidget = nullptr;
        QStringList valueList;
    };

    // MonitorFilterRuleGroupCompiled：单规则组编译后的可匹配过滤条件（组内 AND，组间 OR）。
    struct MonitorFilterRuleGroupCompiled
    {
        int groupId = 0;
        bool enabled = true;
        std::vector<std::uint32_t> processIdList;
        std::vector<UInt32Range> localAddressRangeList;
        std::vector<UInt32Range> remoteAddressRangeList;
        std::vector<UInt16Range> localPortRangeList;
        std::vector<UInt16Range> remotePortRangeList;
        std::vector<UInt32Range> packetSizeRangeList;

        bool hasAnyCondition() const
        {
            return !processIdList.empty() ||
                !localAddressRangeList.empty() ||
                !remoteAddressRangeList.empty() ||
                !localPortRangeList.empty() ||
                !remotePortRangeList.empty() ||
                !packetSizeRangeList.empty();
        }
    };

    // MonitorFilterRuleGroupUiState：单规则组的 UI 组件与原始输入状态。
    struct MonitorFilterRuleGroupUiState
    {
        int groupId = 0;
        QWidget* containerWidget = nullptr;
        QLabel* titleLabel = nullptr;
        QCheckBox* enabledCheck = nullptr;
        QPushButton* removeGroupButton = nullptr;

        QLineEdit* processInputEdit = nullptr;
        QCompleter* processCompleter = nullptr;
        QStandardItemModel* processSuggestionModel = nullptr;
        QPushButton* addProcessButton = nullptr;
        QPushButton* removeInvalidProcessButton = nullptr;
        QPushButton* clearProcessButton = nullptr;
        QTableWidget* processTable = nullptr;
        std::vector<MonitorProcessTarget> processTargetList;

        MonitorTextRuleFieldUiState localAddressField;
        MonitorTextRuleFieldUiState remoteAddressField;
        MonitorTextRuleFieldUiState localPortField;
        MonitorTextRuleFieldUiState remotePortField;
        MonitorTextRuleFieldUiState packetSizeField;
    };

private:
    // MultiThreadDownloadSegmentState：
    // - 作用：保存单分段下载进度状态；
    // - 仅供“多线程下载”页面内部逻辑使用。
    struct MultiThreadDownloadSegmentState
    {
        std::uint64_t rangeBeginByte = 0; // rangeBeginByte：分段起始字节（含）。
        std::uint64_t rangeEndByte = 0; // rangeEndByte：分段结束字节（含）。
        std::atomic<std::uint64_t> downloadedBytes{ 0 }; // downloadedBytes：该分段已下载字节数。
        std::atomic_bool finished{ false }; // finished：该分段是否已下载完成。
        QString statusText = QStringLiteral("等待中"); // statusText：该分段状态文本。
        mutable std::mutex statusMutex; // statusMutex：分段状态文本并发读写保护锁。
    };

    // MultiThreadDownloadTaskState：
    // - 作用：保存单任务下载状态、分段集合和后台线程状态；
    // - 仅供“多线程下载”页面内部逻辑使用。
    struct MultiThreadDownloadTaskState
    {
        int taskId = 0; // taskId：任务编号。
        QString urlText; // urlText：任务原始下载URL。
        QString savePathText; // savePathText：任务最终输出文件路径。
        QString fileNameText; // fileNameText：任务文件名。
        int requestedThreadCount = 1; // requestedThreadCount：用户配置线程数。
        int actualThreadCount = 1; // actualThreadCount：实际执行线程数（按服务端能力调整）。
        bool supportsRange = false; // supportsRange：服务端是否支持 Range 分段下载。
        std::uint64_t totalBytes = 0; // totalBytes：目标文件总字节数。
        std::atomic<std::uint64_t> downloadedBytes{ 0 }; // downloadedBytes：任务累计已下载字节数。
        std::atomic_int runningWorkerCount{ 0 }; // runningWorkerCount：仍在运行的下载线程数量。
        std::atomic_bool finished{ false }; // finished：任务是否已完成（成功或失败结束）。
        std::atomic_bool failed{ false }; // failed：任务是否失败结束。
        std::atomic_bool cancelRequested{ false }; // cancelRequested：任务是否收到取消请求。
        QString statusText = QStringLiteral("等待启动"); // statusText：任务状态文本。
        QString errorReasonText; // errorReasonText：任务失败原因文本。
        mutable std::mutex statusMutex; // statusMutex：任务状态与错误文本并发保护锁。
        std::vector<std::shared_ptr<MultiThreadDownloadSegmentState>> segmentStateList; // segmentStateList：任务分段状态列表。
    };

private:
    // ========================= UI 初始化 =========================
    // initializeUi：
    // - 作用：初始化根布局与侧边栏 Tab 容器。
    // - 返回：无。
    void initializeUi();

    // initializeTrafficMonitorTab：
    // - 作用：构建“流量监控”页（控制栏 + 组合过滤栏 + 报文表）。
    // - 返回：无。
    void initializeTrafficMonitorTab();

    // initializeRateLimitTab：
    // - 作用：构建“进程限速”页（规则输入 + 规则表 + 动作日志）。
    // - 返回：无。
    void initializeRateLimitTab();

    // initializeConnectionManageTab：
    // - 作用：构建“连接管理”页（TCP 表/UDP 表 + 连接控制按钮）。
    // - 返回：无。
    void initializeConnectionManageTab();

    // initializeManualRequestTab：
    // - 作用：构建“请求构造”页（手动 API 参数编辑 + 执行结果输出）。
    // - 返回：无。
    void initializeManualRequestTab();

    // initializeMultiThreadDownloadTab：
    // - 作用：构建“多线程下载”页（URL/路径/线程数 + 任务与分段进度）。
    // - 返回：无。
    void initializeMultiThreadDownloadTab();

    // initializeArpCacheTab：
    // - 作用：构建“ARP缓存”页（展示 + 编辑）。
    // - 返回：无。
    void initializeArpCacheTab();

    // initializeDnsCacheTab：
    // - 作用：构建“DNS缓存”页（展示 + 编辑）。
    // - 返回：无。
    void initializeDnsCacheTab();

    // initializeAliveHostScanTab：
    // - 作用：构建“存活主机发现”页（IP段 ICMP 扫描）。
    // - 返回：无。
    void initializeAliveHostScanTab();

    // initializeHostsFileEditorTab：
    // - 作用：构建“hosts文件编辑”页（全屏文本编辑器）。
    // - 返回：无。
    void initializeHostsFileEditorTab();

    // initializeHttpsAnalyzeTab：
    // - 作用：构建“HTTPS解析”页（代理控制 + 证书信任 + 解析结果）。
    // - 返回：无。
    void initializeHttpsAnalyzeTab();

    // initializeConnections：
    // - 作用：统一连接按钮、输入框、表格的信号槽。
    // - 返回：无。
    void initializeConnections();

    // ========================= 抓包控制 =========================
    // startTrafficMonitor：
    // - 作用：请求后台服务启动抓包线程。
    // - 返回：无。
    void startTrafficMonitor();

    // stopTrafficMonitor：
    // - 作用：请求后台服务停止抓包线程。
    // - 返回：无。
    void stopTrafficMonitor();

    // updateMonitorButtonState：
    // - 作用：按 m_monitorRunning 刷新开始/停止按钮状态。
    // - 返回：无。
    void updateMonitorButtonState();

    // ========================= 报文数据处理 ======================
    // onPacketCaptured：
    // - 作用：处理一条抓包记录，写入缓存并按过滤状态决定是否显示。
    // - 参数 packetRecord：后台服务解析出的报文记录。
    // - 返回：无。
    void onPacketCaptured(const ks::network::PacketRecord& packetRecord);

    // onStatusMessageArrived：
    // - 作用：显示后台状态文本并同步按钮状态。
    // - 参数 statusText：后台线程上报的状态说明。
    // - 返回：无。
    void onStatusMessageArrived(const std::string& statusText);

    // onRateLimitActionArrived：
    // - 作用：处理限速动作事件并追加到限速日志区。
    // - 参数 actionEvent：挂起/恢复动作信息。
    // - 返回：无。
    void onRateLimitActionArrived(const ks::network::RateLimitActionEvent& actionEvent);

    // appendPacketToMonitorTable：
    // - 作用：把一条报文追加到“流量监控”主表末尾。
    // - 参数 packetRecord：待追加报文。
    // - 返回：无。
    void appendPacketToMonitorTable(const ks::network::PacketRecord& packetRecord);

    // rebuildMonitorTableByFilter：
    // - 作用：按当前组合过滤条件重建主流量表。
    // - 返回：无。
    void rebuildMonitorTableByFilter();

    // trimOldestPacketWhenNeeded：
    // - 作用：当缓存超上限时删除最老报文，限制内存占用。
    // - 返回：无。
    void trimOldestPacketWhenNeeded();

    // clearAllPacketRows：
    // - 作用：清空缓存与表格行，同时清空后台待刷新队列。
    // - 返回：无。
    void clearAllPacketRows();

    // ========================= 流量过滤 ==========================
    // applyMonitorFilters：
    // - 作用：把规则组配置编译为可匹配条件并应用到流量表。
    // - 返回：无。
    void applyMonitorFilters();

    // clearMonitorFilters：
    // - 作用：清空全部规则组配置并恢复显示全量报文。
    // - 返回：无。
    void clearMonitorFilters();

    // updateMonitorFilterStateLabel：
    // - 作用：把当前规则组过滤状态汇总到状态标签。
    // - 返回：无。
    void updateMonitorFilterStateLabel();

    // addMonitorFilterRuleGroup：
    // - 作用：新增一个规则组（组间 OR）。
    // - 返回：无。
    void addMonitorFilterRuleGroup();

    // removeMonitorFilterRuleGroup：
    // - 作用：删除指定规则组。
    // - 参数 groupId：规则组唯一 ID。
    // - 返回：无。
    void removeMonitorFilterRuleGroup(int groupId);

    // rebuildMonitorFilterRuleGroupUi：
    // - 作用：按当前规则组状态重建规则组区域 UI。
    // - 返回：无。
    void rebuildMonitorFilterRuleGroupUi();

    // refreshProcessSuggestionModelForGroup：
    // - 作用：按输入关键词刷新指定规则组的进程候选下拉。
    // - 参数 groupId：规则组唯一 ID。
    // - 参数 keywordText：当前输入文本。
    // - 返回：无。
    void refreshProcessSuggestionModelForGroup(int groupId, const QString& keywordText);

    // refreshMonitorProcessCandidateList：
    // - 作用：刷新“系统进程候选缓存”（PID/名称/图标）。
    // - 参数 forceRefresh：true 时忽略最小刷新间隔。
    // - 返回：无。
    void refreshMonitorProcessCandidateList(bool forceRefresh);

    // addProcessTargetByInput：
    // - 作用：把进程输入框当前值解析并加入规则组进程列表。
    // - 参数 groupId：规则组唯一 ID。
    // - 返回：无。
    void addProcessTargetByInput(int groupId);

    // removeProcessTarget：
    // - 作用：按 PID 删除规则组中的单个进程条目。
    // - 参数 groupId：规则组唯一 ID。
    // - 参数 pidValue：目标 PID。
    // - 返回：无。
    void removeProcessTarget(int groupId, std::uint32_t pidValue);

    // clearProcessTargetList：
    // - 作用：清空规则组全部进程条目。
    // - 参数 groupId：规则组唯一 ID。
    // - 返回：无。
    void clearProcessTargetList(int groupId);

    // removeInvalidProcessTargets：
    // - 作用：移除“PID 不存在或 PID 与进程名不匹配”的条目。
    // - 参数 groupId：规则组唯一 ID。
    // - 返回：无。
    void removeInvalidProcessTargets(int groupId);

    // addTextFilterItemsByInput：
    // - 作用：把输入框文本解析为一个或多个条目并加入字段列表。
    // - 参数 groupId：规则组唯一 ID。
    // - 参数 fieldKind：字段类型（地址/端口/包长）。
    // - 返回：无。
    void addTextFilterItemsByInput(int groupId, MonitorTextRuleFieldKind fieldKind);

    // removeTextFilterItem：
    // - 作用：删除字段列表中的单个条目。
    // - 参数 groupId：规则组唯一 ID。
    // - 参数 fieldKind：字段类型。
    // - 参数 itemIndex：条目索引。
    // - 返回：无。
    void removeTextFilterItem(int groupId, MonitorTextRuleFieldKind fieldKind, int itemIndex);

    // clearTextFilterItems：
    // - 作用：清空字段列表全部条目。
    // - 参数 groupId：规则组唯一 ID。
    // - 参数 fieldKind：字段类型。
    // - 返回：无。
    void clearTextFilterItems(int groupId, MonitorTextRuleFieldKind fieldKind);

    // refreshProcessTableForGroup：
    // - 作用：刷新指定规则组“进程列表”表格显示。
    // - 参数 groupId：规则组唯一 ID。
    // - 返回：无。
    void refreshProcessTableForGroup(int groupId);

    // refreshTextTableForGroup：
    // - 作用：刷新指定规则组字段表格（地址/端口/包长）。
    // - 参数 groupId：规则组唯一 ID。
    // - 参数 fieldKind：字段类型。
    // - 返回：无。
    void refreshTextTableForGroup(int groupId, MonitorTextRuleFieldKind fieldKind);

    // clearAllMonitorFilterConfigurations：
    // - 作用：一键清空所有规则组及其全部条目，并保留一个空白规则组。
    // - 返回：无。
    void clearAllMonitorFilterConfigurations();

    // importMonitorFilterConfigFromUserSelectedPath：
    // - 作用：从用户选择的配置文件导入规则组。
    // - 返回：无。
    void importMonitorFilterConfigFromUserSelectedPath();

    // exportMonitorFilterConfigToUserSelectedPath：
    // - 作用：把当前规则组导出到用户选择路径。
    // - 返回：无。
    void exportMonitorFilterConfigToUserSelectedPath() const;

    // loadMonitorFilterConfigFromDefaultPath：
    // - 作用：从默认路径 `config/wireshark.cfg` 读取规则组。
    // - 返回：无。
    void loadMonitorFilterConfigFromDefaultPath();

    // saveMonitorFilterConfigToDefaultPath：
    // - 作用：保存当前规则组到默认路径 `config/wireshark.cfg`。
    // - 返回：无。
    void saveMonitorFilterConfigToDefaultPath() const;

    // saveMonitorFilterConfigToPath：
    // - 作用：保存当前规则组到指定路径。
    // - 参数 filePath：目标配置文件路径。
    // - 参数 showErrorDialog：是否在失败时弹框。
    // - 返回：true=保存成功；false=保存失败。
    bool saveMonitorFilterConfigToPath(const QString& filePath, bool showErrorDialog) const;

    // loadMonitorFilterConfigFromPath：
    // - 作用：从指定路径加载规则组并刷新 UI。
    // - 参数 filePath：来源配置文件路径。
    // - 参数 showErrorDialog：是否在失败时弹框。
    // - 返回：true=加载成功；false=加载失败。
    bool loadMonitorFilterConfigFromPath(const QString& filePath, bool showErrorDialog);

    // monitorFilterConfigPath：
    // - 作用：获取默认规则配置文件绝对路径。
    // - 返回：`<exe目录>/config/wireshark.cfg`。
    QString monitorFilterConfigPath() const;

    // tryCompileMonitorFilterGroups：
    // - 作用：把 UI 原始条目编译为可匹配条件。
    // - 参数 compiledGroupsOut：成功时输出编译结果。
    // - 参数 errorTextOut：失败时输出错误文本。
    // - 返回：true=编译成功；false=存在非法条目。
    bool tryCompileMonitorFilterGroups(
        std::vector<MonitorFilterRuleGroupCompiled>& compiledGroupsOut,
        QString& errorTextOut) const;

    // packetMatchesMonitorFilterGroup：
    // - 作用：判断报文是否命中单规则组（组内 AND）。
    // - 参数 packetRecord：待匹配报文。
    // - 参数 groupFilter：编译后的规则组。
    // - 返回：true=命中；false=不命中。
    bool packetMatchesMonitorFilterGroup(
        const ks::network::PacketRecord& packetRecord,
        const MonitorFilterRuleGroupCompiled& groupFilter) const;

    // findMonitorFilterRuleGroupById：
    // - 作用：按规则组 ID 查找 UI 状态对象。
    // - 参数 groupId：规则组唯一 ID。
    // - 返回：找到返回指针，未找到返回空。
    MonitorFilterRuleGroupUiState* findMonitorFilterRuleGroupById(int groupId);
    const MonitorFilterRuleGroupUiState* findMonitorFilterRuleGroupById(int groupId) const;

    // findTextRuleField：
    // - 作用：从规则组中按字段类型定位字段状态对象。
    // - 参数 groupState：目标规则组。
    // - 参数 fieldKind：字段类型。
    // - 返回：找到返回指针，未找到返回空。
    MonitorTextRuleFieldUiState* findTextRuleField(
        MonitorFilterRuleGroupUiState& groupState,
        MonitorTextRuleFieldKind fieldKind);
    const MonitorTextRuleFieldUiState* findTextRuleField(
        const MonitorFilterRuleGroupUiState& groupState,
        MonitorTextRuleFieldKind fieldKind) const;

    // addOrTrackProcessPid：
    // - 作用：把某 PID 注入首个规则组并立即应用过滤。
    // - 参数 pidValue：目标 PID。
    // - 返回：无。
    void addOrTrackProcessPid(std::uint32_t pidValue);

    // splitMonitorFilterTokens：
    // - 作用：按逗号/分号/空白拆分输入文本。
    // - 参数 inputText：原始输入文本。
    // - 返回：分词结果（去空项）。
    static QStringList splitMonitorFilterTokens(const QString& inputText);

    // tryParsePacketSizeToken：
    // - 作用：解析单个包长条件（如 "40" 或 "60-80"）。
    // - 参数 tokenText：输入词元。
    // - 参数 rangeOut：解析成功时输出范围。
    // - 参数 normalizeTextOut：解析成功时输出规范化文本。
    // - 返回：true=成功；false=失败。
    static bool tryParsePacketSizeToken(
        const QString& tokenText,
        UInt32Range& rangeOut,
        QString& normalizeTextOut);

    // trackProcessByTableRow：
    // - 作用：右键“跟踪此进程”时把该行 PID 写入过滤并立即应用。
    // - 参数 row：当前选中行索引。
    // - 返回：无。
    void trackProcessByTableRow(int row);

    // gotoProcessDetailByTableRow：
    // - 作用：右键“转到进程详细信息”时打开独立进程详情窗口。
    // - 参数 row：当前选中行索引。
    // - 返回：无。
    void gotoProcessDetailByTableRow(int row);

    // ========================= 进程限速 =========================
    // applyOrUpdateRateLimitRule：
    // - 作用：新增或更新指定 PID 的限速规则。
    // - 返回：无。
    void applyOrUpdateRateLimitRule();

    // removeSelectedRateLimitRule：
    // - 作用：删除限速规则表当前选中行对应的规则。
    // - 返回：无。
    void removeSelectedRateLimitRule();

    // clearAllRateLimitRules：
    // - 作用：清空全部限速规则（包含确认对话框）。
    // - 返回：无。
    void clearAllRateLimitRules();

    // refreshRateLimitTable：
    // - 作用：用后台快照重建限速规则表。
    // - 返回：无。
    void refreshRateLimitTable();

    // appendRateLimitActionLogLine：
    // - 作用：向限速动作日志窗口追加一行文本。
    // - 参数 logLine：日志正文。
    // - 返回：无。
    void appendRateLimitActionLogLine(const QString& logLine);

    // ========================= 连接管理 =========================
    // refreshConnectionTables：
    // - 作用：刷新 TCP/UDP 两张连接监控表。
    // - 返回：无。
    void refreshConnectionTables();

    // refreshTcpConnectionTable：
    // - 作用：拉取 TCP 连接快照并重建 TCP 表。
    // - 返回：无。
    void refreshTcpConnectionTable();

    // refreshUdpEndpointTable：
    // - 作用：拉取 UDP 端点快照并重建 UDP 表。
    // - 返回：无。
    void refreshUdpEndpointTable();

    // terminateSelectedTcpConnection：
    // - 作用：终止 TCP 表当前选中连接（DELETE_TCB）。
    // - 返回：无。
    void terminateSelectedTcpConnection();

    // copySelectedConnectionRowToClipboard：
    // - 作用：复制指定连接表当前选中整行到剪贴板。
    // - 参数 tableWidget：连接表对象。
    // - 返回：无。
    void copySelectedConnectionRowToClipboard(QTableWidget* tableWidget);

    // ========================= 请求构造 =========================
    // executeManualRequest：
    // - 作用：读取 UI 构造参数，执行一次手动网络请求并输出结果。
    // - 返回：无。
    void executeManualRequest();

    // replayPacketToManualRequestByTableRow：
    // - 作用：把抓包表指定行的报文快速回填到“请求构造”页，形成可编辑重放草稿；
    // - 参数 row：抓包列表中的目标行号；
    // - 返回：无。
    void replayPacketToManualRequestByTableRow(int row);

    // resetManualRequestForm：
    // - 作用：恢复请求构造页默认参数。
    // - 返回：无。
    void resetManualRequestForm();

    // appendManualRequestLogLine：
    // - 作用：向请求构造结果框追加一行带时间前缀的文本。
    // - 参数 logLine：输出正文。
    // - 返回：无。
    void appendManualRequestLogLine(const QString& logLine);

    // ========================= 多线程下载 ======================
    // startMultiThreadDownloadTask：
    // - 作用：按当前 URL、保存目录、线程数创建并启动下载任务。
    // - 返回：无。
    void startMultiThreadDownloadTask();

    // browseMultiThreadDownloadDirectory：
    // - 作用：选择下载输出目录并回填到目录输入框。
    // - 返回：无。
    void browseMultiThreadDownloadDirectory();

    // loadMultiThreadDownloadCaptureSettings：
    // - 作用：读取“剪贴板自动捕获下载链接”设置并同步到多线程下载页控件；
    // - 调用方式：多线程下载页 UI 构建完成后调用。
    // - 返回：无。
    void loadMultiThreadDownloadCaptureSettings();

    // saveMultiThreadDownloadCaptureSettings：
    // - 作用：把“剪贴板自动捕获下载链接”设置写入 JSON 文件；
    // - 调用方式：设置控件发生变更后调用。
    // - 返回：无。
    void saveMultiThreadDownloadCaptureSettings();

    // onMultiThreadDownloadClipboardChanged：
    // - 作用：处理系统剪贴板文本变化并尝试识别可下载链接；
    // - 调用方式：QClipboard::changed(Clipboard) 信号触发时调用。
    // - 返回：无。
    void onMultiThreadDownloadClipboardChanged();

    // showMultiThreadDownloadClipboardPrompt：
    // - 作用：以非阻塞对话框形式确认下载 URL 与保存目录；
    // - 调用方式：剪贴板识别到匹配后缀的 URL 时调用。
    // - 入参 urlText：候选下载 URL 文本。
    // - 返回：无。
    void showMultiThreadDownloadClipboardPrompt(const QString& urlText);

    // startMultiThreadDownloadTaskFromInput：
    // - 作用：把外部输入的 URL/目录注入 UI 并复用现有启动逻辑；
    // - 调用方式：剪贴板询问框“开始下载”按钮点击后调用。
    // - 入参 urlText：确认后的下载 URL；
    // - 入参 saveDirectoryText：确认后的保存目录。
    // - 返回：true=已成功创建新任务；false=启动失败或未创建任务。
    bool startMultiThreadDownloadTaskFromInput(
        const QString& urlText,
        const QString& saveDirectoryText);

    // isMultiThreadDownloadClipboardUrlSupported：
    // - 作用：按当前后缀规则判断 URL 是否可触发自动下载询问；
    // - 调用方式：剪贴板检测流程内部调用。
    // - 入参 urlObject：已解析的 URL 对象。
    // - 返回：true=匹配后缀；false=不匹配。
    bool isMultiThreadDownloadClipboardUrlSupported(const QUrl& urlObject) const;

    // refreshMultiThreadDownloadUi：
    // - 作用：刷新下载任务表、分段表和总进度条显示。
    // - 返回：无。
    void refreshMultiThreadDownloadUi();

    // findMultiThreadDownloadTaskById：
    // - 作用：按任务编号查找下载任务状态对象。
    // - 参数 taskId：任务编号。
    // - 返回：找到时返回共享指针，否则返回空。
    std::shared_ptr<MultiThreadDownloadTaskState> findMultiThreadDownloadTaskById(int taskId) const;

    // ========================= ARP/DNS/存活主机 ======================
    // refreshArpCacheTable：
    // - 作用：刷新 ARP 缓存表格。
    // - 返回：无。
    void refreshArpCacheTable();

    // addArpCacheEntry：
    // - 作用：新增静态 ARP 映射行。
    // - 返回：无。
    void addArpCacheEntry();

    // removeSelectedArpCacheEntry：
    // - 作用：删除 ARP 表当前选中行。
    // - 返回：无。
    void removeSelectedArpCacheEntry();

    // flushArpCache：
    // - 作用：清空系统 ARP 缓存。
    // - 返回：无。
    void flushArpCache();

    // refreshDnsCacheTable：
    // - 作用：刷新 DNS 解析缓存列表。
    // - 返回：无。
    void refreshDnsCacheTable();

    // removeDnsCacheEntry：
    // - 作用：按名称删除 DNS 缓存条目。
    // - 返回：无。
    void removeDnsCacheEntry();

    // flushDnsCache：
    // - 作用：清空 DNS 缓存。
    // - 返回：无。
    void flushDnsCache();

    // startAliveHostScan：
    // - 作用：启动指定 IP 段存活主机扫描。
    // - 返回：无。
    void startAliveHostScan();

    // stopAliveHostScan：
    // - 作用：停止当前存活主机扫描。
    // - 返回：无。
    void stopAliveHostScan();

    // appendAliveHostRow：
    // - 作用：向扫描结果表追加一行。
    // - 返回：无。
    void appendAliveHostRow(
        const QString& ipText,
        bool alive,
        std::uint32_t rttMs,
        const QString& detailText);

    // ========================= HTTPS 解析 ======================
    // startHttpsProxyService：
    // - 作用：按当前监听参数启动本地 HTTPS 代理。
    // - 返回：无。
    void startHttpsProxyService();

    // stopHttpsProxyService：
    // - 作用：停止本地 HTTPS 代理并更新状态。
    // - 返回：无。
    void stopHttpsProxyService();

    // ensureHttpsRootCertificateTrusted：
    // - 作用：一键生成并安装信任根证书（当前用户证书存储）。
    // - 返回：无。
    void ensureHttpsRootCertificateTrusted();

    // applyHttpsSystemProxy：
    // - 作用：把系统代理切到本地 HTTPS 代理端口。
    // - 返回：无。
    void applyHttpsSystemProxy();

    // clearHttpsSystemProxy：
    // - 作用：清除系统代理配置并恢复直连。
    // - 返回：无。
    void clearHttpsSystemProxy();

    // onHttpsProxyParsedEntryArrived：
    // - 作用：接收代理层解析结果并写入表格。
    // - 参数 parsedEntry：解析出的单条记录。
    // - 返回：无。
    void onHttpsProxyParsedEntryArrived(const ks::network::HttpsProxyParsedEntry& parsedEntry);

    // openHttpsParsedDetailByRow：
    // - 作用：按 HTTPS 解析表行号打开详情窗口。
    // - 参数 row：HTTPS 解析表行索引。
    // - 返回：无。
    void openHttpsParsedDetailByRow(int row);

    // appendHttpsProxyLogLine：
    // - 作用：向 HTTPS 页日志输出框追加一行时间戳文本。
    // - 参数 logLine：日志正文。
    // - 返回：无。
    void appendHttpsProxyLogLine(const QString& logLine);

    // updateHttpsProxyStatusLabel：
    // - 作用：统一刷新 HTTPS 代理状态标签文本。
    // - 参数 statusText：状态说明文本。
    // - 返回：无。
    void updateHttpsProxyStatusLabel(const QString& statusText);

    // flushPendingPacketsToUi：
    // - 作用：批量消费后台积压报文并刷新 UI。
    // - 说明：通过定时器节流，避免高频逐包更新导致卡顿。
    // - 返回：无。
    void flushPendingPacketsToUi();

    // ========================= 报文详情窗口 ======================
    // openPacketDetailWindowFromTableRow：
    // - 作用：按表格行提取报文序号并打开详情窗口。
    // - 参数 tableWidget：来源表格指针。
    // - 参数 row：目标行索引。
    // - 返回：无。
    void openPacketDetailWindowFromTableRow(QTableWidget* tableWidget, int row);

    // openPacketDetailWindowBySequenceId：
    // - 作用：按报文序号从缓存查找并弹出独立详情窗口。
    // - 参数 sequenceId：报文主键序号。
    // - 返回：无。
    void openPacketDetailWindowBySequenceId(std::uint64_t sequenceId);

    // ========================= 通用辅助函数 ======================
    // toPacketColumn：
    // - 作用：把报文列枚举转成 int 列号。
    // - 参数 column：报文列枚举值。
    // - 返回：对应列号。
    static int toPacketColumn(PacketTableColumn column);

    // toRateLimitColumn：
    // - 作用：把限速列枚举转成 int 列号。
    // - 参数 column：限速列枚举值。
    // - 返回：对应列号。
    static int toRateLimitColumn(RateLimitTableColumn column);

    // toTcpConnectionColumn：
    // - 作用：把 TCP 连接列枚举转成 int 列号。
    // - 参数 column：TCP 连接列枚举值。
    // - 返回：对应列号。
    static int toTcpConnectionColumn(TcpConnectionTableColumn column);

    // toUdpEndpointColumn：
    // - 作用：把 UDP 端点列枚举转成 int 列号。
    // - 参数 column：UDP 端点列枚举值。
    // - 返回：对应列号。
    static int toUdpEndpointColumn(UdpEndpointTableColumn column);

    // tryParsePidText：
    // - 作用：解析十进制 PID 文本。
    // - 参数 pidText：输入文本。
    // - 参数 pidOut：解析成功时输出 PID。
    // - 返回：true=成功；false=失败。
    static bool tryParsePidText(const QString& pidText, std::uint32_t& pidOut);

    // tryParseUnsignedIntegerText：
    // - 作用：解析“十进制或 0x 十六进制”无符号整数文本。
    // - 参数 integerText：输入文本。
    // - 参数 valueOut：解析成功时输出数值。
    // - 返回：true=成功；false=失败。
    static bool tryParseUnsignedIntegerText(const QString& integerText, std::uint32_t& valueOut);

    // resolveProcessIconByPid：
    // - 作用：按 PID 解析并缓存进程图标。
    // - 参数 processId：目标进程 PID。
    // - 参数 processName：进程名（日志兜底提示用）。
    // - 返回：可用于表格单元格的图标。
    QIcon resolveProcessIconByPid(std::uint32_t processId, const std::string& processName);

    // packetPassesMonitorFilter：
    // - 作用：判断报文是否满足当前所有已启用过滤条件。
    // - 参数 packetRecord：待判断报文。
    // - 返回：true=通过；false=被过滤。
    bool packetPassesMonitorFilter(const ks::network::PacketRecord& packetRecord) const;

private:
    // ========================= 顶层布局 =========================
    QVBoxLayout* m_rootLayout = nullptr;   // 根布局，只承载侧边栏 Tab 容器。
    QTabWidget* m_sideTabWidget = nullptr; // 侧边栏 Tab（West）。

    // ========================= Tab1：流量监控 ====================
    QWidget* m_trafficMonitorPage = nullptr;      // 流量监控页容器。
    QVBoxLayout* m_trafficMonitorLayout = nullptr;// 流量监控页主布局。
    QHBoxLayout* m_monitorControlLayout = nullptr;// 抓包控制栏布局。
    QHBoxLayout* m_monitorFilterHeaderLayout = nullptr; // 过滤面板标题栏（漏斗/按钮组）。
    QWidget* m_monitorFilterPanel = nullptr;            // 过滤规则组折叠面板容器。
    QVBoxLayout* m_monitorFilterPanelLayout = nullptr;  // 过滤规则组面板主布局。
    QScrollArea* m_monitorFilterScrollArea = nullptr;   // 规则组滚动容器。
    QWidget* m_monitorFilterGroupHostWidget = nullptr;  // 规则组承载容器。
    QVBoxLayout* m_monitorFilterGroupHostLayout = nullptr; // 规则组承载布局。

    QPushButton* m_startMonitorButton = nullptr; // 启动监控按钮。
    QPushButton* m_stopMonitorButton = nullptr;  // 停止监控按钮。
    QPushButton* m_clearPacketButton = nullptr;  // 清空报文按钮。
    QLabel* m_monitorStatusLabel = nullptr;      // 抓包状态标签。

    QPushButton* m_monitorFilterToggleButton = nullptr;     // 漏斗按钮（展开/收起过滤配置）。
    QPushButton* m_addMonitorFilterGroupButton = nullptr;   // 新增规则组按钮。
    QPushButton* m_applyMonitorFilterButton = nullptr;      // 应用过滤按钮。
    QPushButton* m_clearMonitorFilterButton = nullptr;      // 一键清空全部配置按钮。
    QPushButton* m_saveMonitorFilterButton = nullptr;       // 保存到默认 cfg 按钮。
    QPushButton* m_importMonitorFilterButton = nullptr;     // 导入配置按钮。
    QPushButton* m_exportMonitorFilterButton = nullptr;     // 导出配置按钮。
    QLabel* m_monitorFilterStateLabel = nullptr;            // 当前过滤状态汇总标签。
    QTableWidget* m_packetTable = nullptr;             // 全量发送报文表格。

    // ========================= Tab2：进程限速 ====================
    QWidget* m_rateLimitPage = nullptr;           // 进程限速页容器。
    QVBoxLayout* m_rateLimitLayout = nullptr;     // 进程限速页主布局。
    QHBoxLayout* m_rateLimitControlLayout = nullptr; // 限速控制栏布局。
    QLineEdit* m_rateLimitPidEdit = nullptr;      // 限速目标 PID 输入框。
    QSpinBox* m_rateLimitKBpsSpin = nullptr;      // 限速阈值（KB/s）输入。
    QSpinBox* m_rateLimitSuspendMsSpin = nullptr; // 超限挂起时长输入。
    QPushButton* m_applyRateLimitButton = nullptr;// 新增/更新规则按钮。
    QPushButton* m_removeRateLimitButton = nullptr;// 删除选中规则按钮。
    QPushButton* m_clearRateLimitButton = nullptr; // 清空全部规则按钮。
    QTableWidget* m_rateLimitTable = nullptr;      // 限速规则表格。
    QPlainTextEdit* m_rateLimitLogOutput = nullptr;// 限速动作日志输出框。

    // ========================= Tab3：连接管理 ====================
    QWidget* m_connectionManagePage = nullptr;      // 连接管理页容器。
    QVBoxLayout* m_connectionManageLayout = nullptr;// 连接管理页主布局。
    QHBoxLayout* m_connectionControlLayout = nullptr;// 连接管理控制栏布局。
    QPushButton* m_refreshConnectionButton = nullptr; // 手动刷新连接快照按钮。
    QPushButton* m_autoRefreshConnectionButton = nullptr; // 自动刷新开关按钮（checkable）。
    QPushButton* m_terminateTcpButton = nullptr;     // 终止选中 TCP 连接按钮。
    QLabel* m_connectionStatusLabel = nullptr;       // 连接管理状态标签。
    QTabWidget* m_connectionSubTabWidget = nullptr;  // 连接子页签（TCP/UDP）。
    QTableWidget* m_tcpConnectionTable = nullptr;    // TCP 连接监控表。
    QTableWidget* m_udpEndpointTable = nullptr;      // UDP 端点监控表。

    // ========================= Tab4：请求构造 ====================
    QWidget* m_manualRequestPage = nullptr;          // 请求构造页容器。
    QVBoxLayout* m_manualRequestLayout = nullptr;    // 请求构造页主布局。
    QComboBox* m_manualApiCombo = nullptr;           // API 方式选择（TCP/UDP）。
    QCheckBox* m_manualOverrideSocketParameterCheck = nullptr; // 是否手动覆盖 socket 参数。
    QLineEdit* m_manualAddressFamilyEdit = nullptr;  // address family 输入（AF_* 数值）。
    QLineEdit* m_manualSocketTypeEdit = nullptr;     // socket type 输入（SOCK_* 数值）。
    QLineEdit* m_manualProtocolEdit = nullptr;       // protocol 输入（IPPROTO_* 数值）。
    QLineEdit* m_manualSocketFlagsEdit = nullptr;    // WSASocket flags 输入。
    QCheckBox* m_manualEnableBindCheck = nullptr;    // 是否先 bind 本地端点。
    QLineEdit* m_manualLocalAddressEdit = nullptr;   // 本地地址输入。
    QSpinBox* m_manualLocalPortSpin = nullptr;       // 本地端口输入。
    QLineEdit* m_manualRemoteAddressEdit = nullptr;  // 远端地址输入。
    QSpinBox* m_manualRemotePortSpin = nullptr;      // 远端端口输入。
    QCheckBox* m_manualConnectBeforeSendCheck = nullptr; // 发送前是否 connect。
    QCheckBox* m_manualReuseAddressCheck = nullptr;  // SO_REUSEADDR 开关。
    QCheckBox* m_manualNoDelayCheck = nullptr;       // TCP_NODELAY 开关。
    QSpinBox* m_manualSendTimeoutSpin = nullptr;     // 发送超时（ms）。
    QSpinBox* m_manualRecvTimeoutSpin = nullptr;     // 接收超时（ms）。
    QComboBox* m_manualPayloadFormatCombo = nullptr; // 载荷格式（文本/十六进制）。
    QPlainTextEdit* m_manualPayloadEditor = nullptr; // 请求载荷输入。
    QLineEdit* m_manualSendFlagsEdit = nullptr;      // send flags 输入。
    QLineEdit* m_manualReceiveFlagsEdit = nullptr;   // recv flags 输入。
    QCheckBox* m_manualReceiveAfterSendCheck = nullptr; // 发送后是否立即接收。
    QSpinBox* m_manualReceiveMaxBytesSpin = nullptr; // 接收字节上限。
    QCheckBox* m_manualShutdownSendCheck = nullptr;  // 发送后是否 shutdown(SD_SEND)。
    QPushButton* m_manualExecuteButton = nullptr;    // 执行请求按钮。
    QPushButton* m_manualResetButton = nullptr;      // 重置参数按钮。
    QPlainTextEdit* m_manualResultOutput = nullptr;  // 请求执行结果输出框。

    // ========================= Tab5：多线程下载 ====================
    QWidget* m_multiThreadDownloadPage = nullptr;      // 多线程下载页容器。
    QVBoxLayout* m_multiThreadDownloadLayout = nullptr; // 多线程下载页主布局。
    QHBoxLayout* m_multiThreadDownloadControlLayout = nullptr; // 下载参数控制栏布局。
    QLineEdit* m_multiDownloadUrlEdit = nullptr;       // 下载URL输入框。
    QLineEdit* m_multiDownloadSaveDirEdit = nullptr;   // 下载目录输入框。
    QSpinBox* m_multiDownloadThreadCountSpin = nullptr; // 下载线程数输入框。
    QCheckBox* m_multiDownloadAutoCaptureClipboardCheck = nullptr; // 自动捕获剪贴板下载链接开关。
    QLineEdit* m_multiDownloadCaptureSuffixEdit = nullptr; // 自动识别后缀输入框（支持 ; , 空格 分隔）。
    QPushButton* m_multiDownloadSaveCaptureSettingsButton = nullptr; // 保存捕获设置到 JSON 按钮。
    QPushButton* m_multiDownloadBrowseDirButton = nullptr; // 选择下载目录按钮。
    QPushButton* m_multiDownloadStartButton = nullptr; // 启动下载按钮。
    QLabel* m_multiDownloadStatusLabel = nullptr;      // 下载状态标签。
    QTableWidget* m_multiDownloadTaskTable = nullptr;  // 下载任务总览表。
    QTableWidget* m_multiDownloadSegmentTable = nullptr; // 当前任务分段进度表。
    MultiThreadDownloadSegmentBarWidget* m_multiDownloadSegmentBar = nullptr; // 断节式总进度条控件。
    QLabel* m_multiDownloadTotalProgressLabel = nullptr; // 总进度百分比标签。

    // ========================= Tab6：ARP 缓存 ====================
    QWidget* m_arpCachePage = nullptr;            // ARP缓存页容器。
    QVBoxLayout* m_arpCacheLayout = nullptr;      // ARP缓存页布局。
    QHBoxLayout* m_arpCacheControlLayout = nullptr; // ARP控制栏布局。
    QPushButton* m_refreshArpButton = nullptr;    // 刷新ARP按钮。
    QPushButton* m_addArpButton = nullptr;        // 新增ARP按钮。
    QPushButton* m_removeArpButton = nullptr;     // 删除选中ARP按钮。
    QPushButton* m_flushArpButton = nullptr;      // 清空ARP按钮。
    QLabel* m_arpStatusLabel = nullptr;           // ARP状态标签。
    QTableWidget* m_arpTable = nullptr;           // ARP缓存表格。

    // ========================= Tab7：DNS 缓存 ====================
    QWidget* m_dnsCachePage = nullptr;            // DNS缓存页容器。
    QVBoxLayout* m_dnsCacheLayout = nullptr;      // DNS缓存页布局。
    QHBoxLayout* m_dnsCacheControlLayout = nullptr; // DNS控制栏布局。
    QPushButton* m_refreshDnsButton = nullptr;    // 刷新DNS按钮。
    QPushButton* m_removeDnsButton = nullptr;     // 删除DNS按钮。
    QPushButton* m_flushDnsButton = nullptr;      // 清空DNS按钮。
    QLineEdit* m_dnsEntryEdit = nullptr;          // DNS删除项输入框。
    QLabel* m_dnsStatusLabel = nullptr;           // DNS状态标签。
    QTableWidget* m_dnsTable = nullptr;           // DNS缓存表格。

    // ========================= Tab8：存活主机发现 ====================
    QWidget* m_aliveScanPage = nullptr;           // 存活主机扫描页容器。
    QVBoxLayout* m_aliveScanLayout = nullptr;     // 存活主机扫描页布局。
    QHBoxLayout* m_aliveScanControlLayout = nullptr; // 扫描控制栏布局。
    QLineEdit* m_aliveScanStartIpEdit = nullptr;  // 扫描起始IP输入框。
    QLineEdit* m_aliveScanEndIpEdit = nullptr;    // 扫描结束IP输入框。
    QSpinBox* m_aliveScanTimeoutSpin = nullptr;   // ICMP超时输入框。
    QPushButton* m_startAliveScanButton = nullptr; // 启动扫描按钮。
    QPushButton* m_stopAliveScanButton = nullptr;  // 停止扫描按钮。
    QProgressBar* m_aliveScanProgressBar = nullptr; // 扫描进度条。
    QLabel* m_aliveScanStatusLabel = nullptr;     // 扫描状态标签。
    QTableWidget* m_aliveScanTable = nullptr;     // 存活主机结果表。

    // ========================= Tab9：HTTPS解析 ====================
    QWidget* m_httpsAnalyzePage = nullptr;          // HTTPS解析页容器。
    QVBoxLayout* m_httpsAnalyzeLayout = nullptr;    // HTTPS解析页主布局。
    QHBoxLayout* m_httpsAnalyzeControlLayout = nullptr; // HTTPS控制栏布局。
    QLineEdit* m_httpsListenAddressEdit = nullptr;  // 代理监听地址输入框。
    QSpinBox* m_httpsListenPortSpin = nullptr;      // 代理监听端口输入框。
    QPushButton* m_httpsStartProxyButton = nullptr; // 启动HTTPS代理按钮。
    QPushButton* m_httpsStopProxyButton = nullptr;  // 停止HTTPS代理按钮。
    QPushButton* m_httpsTrustCertButton = nullptr;  // 一键信任证书按钮。
    QPushButton* m_httpsApplyProxyButton = nullptr; // 应用系统代理按钮。
    QPushButton* m_httpsClearProxyButton = nullptr; // 清除系统代理按钮。
    QLabel* m_httpsProxyStatusLabel = nullptr;      // HTTPS代理状态标签。
    QTableWidget* m_httpsParsedTable = nullptr;     // HTTPS解析结果表格。
    QPlainTextEdit* m_httpsProxyLogOutput = nullptr;// HTTPS代理日志输出框。
    std::vector<ks::network::HttpsProxyParsedEntry> m_httpsParsedEntryCache; // HTTPS解析结果缓存，行号与表格一一对应。

    // ========================= Tab10：hosts文件编辑 ====================
    QWidget* m_hostsFileEditorPage = nullptr;       // hosts文件编辑页容器。
    QVBoxLayout* m_hostsFileEditorLayout = nullptr; // hosts文件编辑页布局。
    CodeEditorWidget* m_hostsFileEditor = nullptr;  // hosts文件编辑器（全屏复用项目文本编辑组件）。

    // ========================= 后台服务与缓存 ====================
    std::unique_ptr<ks::network::TrafficMonitorService> m_trafficService; // 抓包/限速后台服务对象。
    QTimer* m_rateLimitRefreshTimer = nullptr; // 限速规则轮询刷新定时器。
    QTimer* m_packetFlushTimer = nullptr;      // 报文批量刷新定时器（UI 节流关键）。
    QTimer* m_connectionRefreshTimer = nullptr; // 连接快照轮询刷新定时器（TCP/UDP）。
    QTimer* m_multiDownloadRefreshTimer = nullptr; // 多线程下载页面刷新定时器（进度UI节流）。
    std::unique_ptr<ks::network::HttpsMitmProxyService> m_httpsProxyService; // HTTPS MITM 代理服务对象。
    bool m_monitorRunning = false;             // 抓包运行状态缓存。
    bool m_httpsProxyRunning = false;          // HTTPS代理运行状态缓存。
    bool m_httpsProxyServiceInitialized = false; // HTTPS代理服务是否已延后初始化。
    std::atomic_bool m_monitorStopInProgress{ false }; // 停止流程进行中，避免重复 stop 导致 UI 抖动。
    std::unique_ptr<std::thread> m_monitorStopThread;  // 异步 stop 的 join 线程，防止主线程等待卡顿。

    // 当前已应用过滤状态：
    // - m_monitorFilterRuleGroupUiList 保存“原始 UI 配置”；
    // - m_activeMonitorFilterGroupList 保存“编译后可匹配条件”；
    // - m_monitorProcessCandidateList 是 PID 输入补全缓存。
    std::vector<std::unique_ptr<MonitorFilterRuleGroupUiState>> m_monitorFilterRuleGroupUiList;
    std::vector<MonitorFilterRuleGroupCompiled> m_activeMonitorFilterGroupList;
    std::vector<MonitorProcessCandidate> m_monitorProcessCandidateList;
    int m_monitorFilterNextGroupId = 1;
    qint64 m_monitorProcessCandidateLastRefreshMs = 0;

    static constexpr std::size_t kMaxPacketCacheCount = 6000;          // 报文缓存上限。
    // 后台待刷新队列上限：
    // - 适当放大到 80000，降低突发流量时“UI 来不及刷导致队列溢出”的丢包概率；
    // - 仍保留硬上限，避免极端场景内存失控。
    static constexpr std::size_t kMaxPendingPacketQueueCount = 80000;
    std::deque<std::uint64_t> m_packetSequenceOrder; // 报文序号按时间顺序缓存。
    std::unordered_map<std::uint64_t, ks::network::PacketRecord> m_packetBySequence; // 序号 -> 报文映射。

    // 后台线程 -> UI 线程的报文暂存队列（只存数据，不碰 UI 控件）。
    std::deque<ks::network::PacketRecord> m_pendingPacketQueue;
    mutable std::mutex m_pendingPacketMutex;
    std::uint64_t m_droppedPacketCount = 0; // 队列满时丢弃计数（用于状态提示）。

    // 连接快照缓存：
    // - UI 表格行与快照向量按同索引对应；
    // - 终止连接、复制行等操作直接通过当前行索引回查。
    std::vector<ks::network::TcpConnectionRecord> m_tcpConnectionCache;
    std::vector<ks::network::UdpEndpointRecord> m_udpEndpointCache;

    // PID 图标缓存：避免重复解析 EXE 图标导致 UI 卡顿。
    QHash<quint32, QIcon> m_processIconCacheByPid;

    // 存活主机扫描状态：防止重复启动并支持用户中断。
    std::atomic_bool m_aliveScanRunning{ false };
    std::atomic_bool m_aliveScanCancel{ false };
    int m_aliveScanProgressPid = 0;

    // 多线程下载任务运行状态：支持多任务并行与界面周期刷新。
    mutable std::mutex m_multiDownloadTaskMutex; // m_multiDownloadTaskMutex：下载任务列表并发访问锁。
    std::vector<std::shared_ptr<MultiThreadDownloadTaskState>> m_multiDownloadTaskList; // m_multiDownloadTaskList：下载任务状态集合。
    int m_multiDownloadNextTaskId = 1; // m_multiDownloadNextTaskId：新建任务编号递增计数器。
    int m_multiDownloadSelectedTaskId = 0; // m_multiDownloadSelectedTaskId：当前分段详情面板绑定的任务编号。
    bool m_multiDownloadAutoCaptureClipboardEnabled = true; // m_multiDownloadAutoCaptureClipboardEnabled：是否启用剪贴板自动捕获。
    QStringList m_multiDownloadCaptureSuffixList; // m_multiDownloadCaptureSuffixList：当前用于识别下载链接的后缀集合。
    QString m_multiDownloadLastClipboardText; // m_multiDownloadLastClipboardText：最近一次处理过的剪贴板文本（去重）。
    QPointer<QDialog> m_multiDownloadClipboardPromptDialog; // m_multiDownloadClipboardPromptDialog：当前非阻塞下载询问框弱引用。
};
