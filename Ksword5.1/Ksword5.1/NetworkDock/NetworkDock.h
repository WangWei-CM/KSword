#pragma once

// ============================================================
// NetworkDock.h
// 作用：
// 1) 构建“网络”Dock 的完整侧边栏 Tab UI；
// 2) 展示全部发送方向 TCP/UDP 报文表格；
// 3) 提供组合过滤（PID/IP段/端口/包长）、进程限速、报文详情查看能力；
// 4) 提供 TCP/UDP 连接监控与 TCP 连接终止；
// 5) 提供“手动构造网络请求”可视化参数执行页。
// ============================================================

#include "../Framework.h"

#include <QHash>
#include <QIcon>
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
class QVBoxLayout;
class QWidget;

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
    // - 作用：解析 UI 输入并应用组合过滤（PID/IP段/端口/包长）。
    // - 返回：无。
    void applyMonitorFilters();

    // clearMonitorFilters：
    // - 作用：清除全部过滤条件并恢复显示全量报文。
    // - 返回：无。
    void clearMonitorFilters();

    // updateMonitorFilterStateLabel：
    // - 作用：把当前已启用过滤条件汇总为状态文本展示在 UI。
    // - 返回：无。
    void updateMonitorFilterStateLabel();

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

    // resetManualRequestForm：
    // - 作用：恢复请求构造页默认参数。
    // - 返回：无。
    void resetManualRequestForm();

    // appendManualRequestLogLine：
    // - 作用：向请求构造结果框追加一行带时间前缀的文本。
    // - 参数 logLine：输出正文。
    // - 返回：无。
    void appendManualRequestLogLine(const QString& logLine);

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
    QHBoxLayout* m_monitorFilterLayoutLine1 = nullptr; // 过滤栏第 1 行（PID/IP）。
    QHBoxLayout* m_monitorFilterLayoutLine2 = nullptr; // 过滤栏第 2 行（端口/包长）。

    QPushButton* m_startMonitorButton = nullptr; // 启动监控按钮。
    QPushButton* m_stopMonitorButton = nullptr;  // 停止监控按钮。
    QPushButton* m_clearPacketButton = nullptr;  // 清空报文按钮。
    QLabel* m_monitorStatusLabel = nullptr;      // 抓包状态标签。

    QLineEdit* m_pidFilterEdit = nullptr;      // PID 过滤输入框（可为空表示禁用）。
    QLineEdit* m_localIpFilterEdit = nullptr;  // 本地 IP 段过滤输入框（支持 CIDR/范围/单 IP）。
    QLineEdit* m_remoteIpFilterEdit = nullptr; // 远端 IP 段过滤输入框（支持 CIDR/范围/单 IP）。

    QLineEdit* m_localPortFilterEdit = nullptr;  // 本地端口过滤输入框（单值或范围）。
    QLineEdit* m_remotePortFilterEdit = nullptr; // 远端端口过滤输入框（单值或范围）。
    QSpinBox* m_packetSizeMinSpin = nullptr;     // 报文大小最小值（0 表示不限）。
    QSpinBox* m_packetSizeMaxSpin = nullptr;     // 报文大小最大值（0 表示不限）。

    QPushButton* m_applyMonitorFilterButton = nullptr; // 应用组合过滤按钮。
    QPushButton* m_clearMonitorFilterButton = nullptr; // 清空组合过滤按钮。
    QLabel* m_monitorFilterStateLabel = nullptr;       // 当前过滤状态汇总标签。
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

    // ========================= Tab5：ARP 缓存 ====================
    QWidget* m_arpCachePage = nullptr;            // ARP缓存页容器。
    QVBoxLayout* m_arpCacheLayout = nullptr;      // ARP缓存页布局。
    QHBoxLayout* m_arpCacheControlLayout = nullptr; // ARP控制栏布局。
    QPushButton* m_refreshArpButton = nullptr;    // 刷新ARP按钮。
    QPushButton* m_addArpButton = nullptr;        // 新增ARP按钮。
    QPushButton* m_removeArpButton = nullptr;     // 删除选中ARP按钮。
    QPushButton* m_flushArpButton = nullptr;      // 清空ARP按钮。
    QLabel* m_arpStatusLabel = nullptr;           // ARP状态标签。
    QTableWidget* m_arpTable = nullptr;           // ARP缓存表格。

    // ========================= Tab6：DNS 缓存 ====================
    QWidget* m_dnsCachePage = nullptr;            // DNS缓存页容器。
    QVBoxLayout* m_dnsCacheLayout = nullptr;      // DNS缓存页布局。
    QHBoxLayout* m_dnsCacheControlLayout = nullptr; // DNS控制栏布局。
    QPushButton* m_refreshDnsButton = nullptr;    // 刷新DNS按钮。
    QPushButton* m_removeDnsButton = nullptr;     // 删除DNS按钮。
    QPushButton* m_flushDnsButton = nullptr;      // 清空DNS按钮。
    QLineEdit* m_dnsEntryEdit = nullptr;          // DNS删除项输入框。
    QLabel* m_dnsStatusLabel = nullptr;           // DNS状态标签。
    QTableWidget* m_dnsTable = nullptr;           // DNS缓存表格。

    // ========================= Tab7：存活主机发现 ====================
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

    // ========================= 后台服务与缓存 ====================
    std::unique_ptr<ks::network::TrafficMonitorService> m_trafficService; // 抓包/限速后台服务对象。
    QTimer* m_rateLimitRefreshTimer = nullptr; // 限速规则轮询刷新定时器。
    QTimer* m_packetFlushTimer = nullptr;      // 报文批量刷新定时器（UI 节流关键）。
    QTimer* m_connectionRefreshTimer = nullptr; // 连接快照轮询刷新定时器（TCP/UDP）。
    bool m_monitorRunning = false;             // 抓包运行状态缓存。
    std::atomic_bool m_monitorStopInProgress{ false }; // 停止流程进行中，避免重复 stop 导致 UI 抖动。
    std::unique_ptr<std::thread> m_monitorStopThread;  // 异步 stop 的 join 线程，防止主线程等待卡顿。

    // 当前启用的组合过滤条件：
    // - 任一 optional 为空表示该条件“未启用”；
    // - 全部为空时等价于“无过滤”。
    std::optional<std::uint32_t> m_activePidFilter;          // PID 精确匹配过滤。
    std::optional<UInt32Range> m_activeLocalIpv4RangeFilter; // 本地 IP 范围过滤（主机序）。
    std::optional<UInt32Range> m_activeRemoteIpv4RangeFilter;// 远端 IP 范围过滤（主机序）。
    std::optional<UInt16Range> m_activeLocalPortRangeFilter; // 本地端口范围过滤。
    std::optional<UInt16Range> m_activeRemotePortRangeFilter;// 远端端口范围过滤。
    std::optional<UInt32Range> m_activePacketSizeRangeFilter; // 报文总长度范围过滤（字节）。

    static constexpr std::size_t kMaxPacketCacheCount = 6000;          // 报文缓存上限。
    static constexpr std::size_t kMaxPendingPacketQueueCount = 12000;  // 后台待刷新队列上限。
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
};
