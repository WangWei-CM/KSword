#pragma once

// ============================================================
// NetworkDock.h
// 作用：
// 1) 构建“网络”Dock 的完整侧边栏 Tab UI；
// 2) 展示全部发送方向 TCP/UDP 报文表格；
// 3) 提供 PID 流量筛查、进程限速、报文详情查看能力。
// ============================================================

#include "../Framework.h"

#include <QHash>
#include <QIcon>
#include <QWidget>

#include <cstdint>      // std::uint32_t/std::uint64_t：PID、序号等。
#include <deque>        // std::deque：按时间顺序维护报文主键。
#include <memory>       // std::unique_ptr：管理后台监控服务对象。
#include <mutex>        // std::mutex：跨线程队列与缓存保护。
#include <optional>     // std::optional：PID 过滤状态。
#include <unordered_map>// 序号 -> 报文记录映射缓存。

// Qt 前置声明：减少头文件编译依赖。
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QVBoxLayout;
class QWidget;
class QTimer;

// ============================================================
// NetworkDock
// 说明：
// - 所有抓包/限速逻辑由 ks::network::TrafficMonitorService 承担；
// - 本类只负责 UI 呈现、用户交互和日志提示。
// ============================================================
class NetworkDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化 UI、绑定服务回调、连接交互逻辑。
    // - 参数 parent：Qt 父控件，可为空。
    explicit NetworkDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止后台抓包线程，避免窗口销毁后仍有回调。
    ~NetworkDock() override;

private:
    // PacketTableColumn：报文表格列索引定义。
    enum class PacketTableColumn : int
    {
        Time = 0,       // 抓包时间。
        Protocol,       // TCP/UDP。
        Direction,      // 方向（Outbound）。
        Pid,            // 归属进程 PID。
        ProcessName,    // 归属进程名。
        LocalEndpoint,  // 本地地址:端口。
        RemoteEndpoint, // 远端地址:端口。
        PacketSize,     // 报文总长度。
        PayloadSize,    // 负载长度。
        Preview,        // 内容预览。
        Count           // 列总数。
    };

    // RateLimitTableColumn：限速规则表格列索引定义。
    enum class RateLimitTableColumn : int
    {
        Pid = 0,            // 目标 PID。
        ProcessName,        // 目标进程名（快照查询时补充）。
        LimitKBps,          // 阈值（KB/s）。
        SuspendMs,          // 超限挂起时长（毫秒）。
        TriggerCount,       // 触发次数。
        CurrentWindowBytes, // 当前窗口累计字节。
        State,              // 当前状态（运行中/已挂起）。
        Count               // 列总数。
    };

private:
    // ========================= UI 初始化 =========================
    // initializeUi：
    // - 作用：初始化根布局与三个侧边栏页面。
    // - 返回：无。
    void initializeUi();

    // initializeTrafficMonitorTab：
    // - 作用：构建“流量监控”页的按钮和全量报文表。
    // - 返回：无。
    void initializeTrafficMonitorTab();

    // initializePidFilterTab：
    // - 作用：构建“PID 筛查”页的输入栏与筛查结果表。
    // - 返回：无。
    void initializePidFilterTab();

    // initializeRateLimitTab：
    // - 作用：构建“进程限速”页的规则编辑、规则表和动作日志区。
    // - 返回：无。
    void initializeRateLimitTab();

    // initializeConnections：
    // - 作用：统一连接按钮/表格交互信号。
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
    // - 作用：按 m_monitorRunning 刷新“开始/停止”按钮可用状态。
    // - 返回：无。
    void updateMonitorButtonState();

    // ========================= 报文数据处理 ======================
    // onPacketCaptured：
    // - 作用：处理后台推送的一条抓包记录并追加到表格缓存。
    // - 参数 packetRecord：后台服务解析出的报文记录。
    // - 返回：无。
    void onPacketCaptured(const ks::network::PacketRecord& packetRecord);

    // onStatusMessageArrived：
    // - 作用：显示后台状态文本并同步按钮状态。
    // - 参数 statusText：后台线程上报的状态说明。
    // - 返回：无。
    void onStatusMessageArrived(const std::string& statusText);

    // onRateLimitActionArrived：
    // - 作用：处理限速动作事件并输出到日志文本框。
    // - 参数 actionEvent：挂起/恢复动作信息。
    // - 返回：无。
    void onRateLimitActionArrived(const ks::network::RateLimitActionEvent& actionEvent);

    // appendPacketToMonitorTable：
    // - 作用：把一条报文追加到“流量监控”主表。
    // - 参数 packetRecord：待追加报文。
    // - 返回：无。
    void appendPacketToMonitorTable(const ks::network::PacketRecord& packetRecord);

    // appendPacketToPidFilterTableIfNeeded：
    // - 作用：当命中当前 PID 筛查条件时，追加到筛查结果表。
    // - 参数 packetRecord：待检查报文。
    // - 返回：无。
    void appendPacketToPidFilterTableIfNeeded(const ks::network::PacketRecord& packetRecord);

    // rebuildPidFilterTable：
    // - 作用：按当前 PID 筛查条件重建结果表。
    // - 返回：无。
    void rebuildPidFilterTable();

    // trimOldestPacketWhenNeeded：
    // - 作用：当缓存超上限时删除最老报文，控制内存增长。
    // - 返回：无。
    void trimOldestPacketWhenNeeded();

    // clearAllPacketRows：
    // - 作用：清空全部报文缓存与表格行。
    // - 返回：无。
    void clearAllPacketRows();

    // ========================= PID 筛查 =========================
    // applyPidFilter：
    // - 作用：读取输入 PID 并启用筛查模式。
    // - 返回：无。
    void applyPidFilter();

    // clearPidFilter：
    // - 作用：取消当前 PID 筛查状态。
    // - 返回：无。
    void clearPidFilter();

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
    // - 作用：清空全部限速规则（带确认）。
    // - 返回：无。
    void clearAllRateLimitRules();

    // refreshRateLimitTable：
    // - 作用：用后台快照数据重建限速规则表。
    // - 返回：无。
    void refreshRateLimitTable();

    // appendRateLimitActionLogLine：
    // - 作用：向限速动作日志窗口追加一行文本。
    // - 参数 logLine：日志正文。
    // - 返回：无。
    void appendRateLimitActionLogLine(const QString& logLine);

    // flushPendingPacketsToUi：
    // - 作用：批量把后台线程积压的报文刷新到 UI 表格；
    // - 说明：通过定时器节流，避免每包都触发 UI 更新导致卡顿。
    // - 返回：无。
    void flushPendingPacketsToUi();

    // ========================= 报文详情窗口 ======================
    // openPacketDetailWindowFromTableRow：
    // - 作用：根据表格行提取序号并打开详情窗口。
    // - 参数 tableWidget：源表格（主表或筛查表）。
    // - 参数 row：目标行索引。
    // - 返回：无。
    void openPacketDetailWindowFromTableRow(QTableWidget* tableWidget, int row);

    // openPacketDetailWindowBySequenceId：
    // - 作用：按报文序号从缓存查找并弹出详情窗口。
    // - 参数 sequenceId：报文序号主键。
    // - 返回：无。
    void openPacketDetailWindowBySequenceId(std::uint64_t sequenceId);

    // ========================= 通用辅助函数 ======================
    // toPacketColumn：
    // - 作用：把报文列枚举转为 int 列号。
    // - 参数 column：报文列枚举值。
    // - 返回：对应列号。
    static int toPacketColumn(PacketTableColumn column);

    // toRateLimitColumn：
    // - 作用：把限速列枚举转为 int 列号。
    // - 参数 column：限速列枚举值。
    // - 返回：对应列号。
    static int toRateLimitColumn(RateLimitTableColumn column);

    // tryParsePidText：
    // - 作用：解析十进制 PID 文本。
    // - 参数 pidText：输入文本。
    // - 参数 pidOut：解析成功时输出 PID。
    // - 返回：true 成功；false 失败。
    static bool tryParsePidText(const QString& pidText, std::uint32_t& pidOut);

    // resolveProcessIconByPid：
    // - 作用：按 PID 解析进程图标（带缓存）；
    // - 参数 processId：目标 PID；
    // - 参数 processName：进程名（兜底展示用）。
    // - 返回：可用于表格单元格的图标对象。
    QIcon resolveProcessIconByPid(std::uint32_t processId, const std::string& processName);

private:
    // ========================= 顶层布局 =========================
    QVBoxLayout* m_rootLayout = nullptr;      // 根布局，只承载侧边栏 Tab 容器。
    QTabWidget* m_sideTabWidget = nullptr;    // 侧边栏 Tab（West）。

    // ========================= Tab1：流量监控 ====================
    QWidget* m_trafficMonitorPage = nullptr;  // 流量监控页容器。
    QVBoxLayout* m_trafficMonitorLayout = nullptr; // 流量监控页主布局。
    QHBoxLayout* m_monitorControlLayout = nullptr; // 抓包控制栏布局。
    QPushButton* m_startMonitorButton = nullptr;   // 启动监控按钮。
    QPushButton* m_stopMonitorButton = nullptr;    // 停止监控按钮。
    QPushButton* m_clearPacketButton = nullptr;    // 清空报文按钮。
    QLabel* m_monitorStatusLabel = nullptr;        // 抓包状态标签。
    QTableWidget* m_packetTable = nullptr;         // 全量发送报文表格。

    // ========================= Tab2：PID 筛查 ====================
    QWidget* m_pidFilterPage = nullptr;      // PID 筛查页容器。
    QVBoxLayout* m_pidFilterLayout = nullptr;// PID 筛查页主布局。
    QHBoxLayout* m_pidFilterControlLayout = nullptr; // PID 筛查控制栏布局。
    QLineEdit* m_pidFilterEdit = nullptr;    // PID 输入框。
    QPushButton* m_applyPidFilterButton = nullptr;  // 应用筛查按钮。
    QPushButton* m_clearPidFilterButton = nullptr;  // 取消筛查按钮。
    QLabel* m_pidFilterStateLabel = nullptr;        // 当前筛查状态标签。
    QTableWidget* m_pidFilterTable = nullptr;       // 筛查结果表格。

    // ========================= Tab3：进程限速 ====================
    QWidget* m_rateLimitPage = nullptr;      // 进程限速页容器。
    QVBoxLayout* m_rateLimitLayout = nullptr;// 进程限速页主布局。
    QHBoxLayout* m_rateLimitControlLayout = nullptr; // 限速控制栏布局。
    QLineEdit* m_rateLimitPidEdit = nullptr; // 限速目标 PID 输入框。
    QSpinBox* m_rateLimitKBpsSpin = nullptr; // 限速阈值（KB/s）输入。
    QSpinBox* m_rateLimitSuspendMsSpin = nullptr; // 超限挂起时长输入。
    QPushButton* m_applyRateLimitButton = nullptr; // 新增/更新规则按钮。
    QPushButton* m_removeRateLimitButton = nullptr;// 删除选中规则按钮。
    QPushButton* m_clearRateLimitButton = nullptr; // 清空全部规则按钮。
    QTableWidget* m_rateLimitTable = nullptr;      // 限速规则表格。
    QPlainTextEdit* m_rateLimitLogOutput = nullptr;// 限速动作日志输出框。

    // ========================= 后台服务与缓存 ====================
    std::unique_ptr<ks::network::TrafficMonitorService> m_trafficService; // 后台抓包/限速服务。
    QTimer* m_rateLimitRefreshTimer = nullptr;       // 限速表轮询刷新定时器。
    QTimer* m_packetFlushTimer = nullptr;            // 报文批量刷新定时器（UI 节流关键）。
    bool m_monitorRunning = false;                   // 抓包运行状态缓存。
    std::optional<std::uint32_t> m_activePidFilter; // 当前 PID 筛查值。

    static constexpr std::size_t kMaxPacketCacheCount = 6000; // 最大缓存报文条数。
    static constexpr std::size_t kMaxPendingPacketQueueCount = 12000; // 后台待刷新队列上限。
    std::deque<std::uint64_t> m_packetSequenceOrder; // 按时间顺序记录报文序号。
    std::unordered_map<std::uint64_t, ks::network::PacketRecord> m_packetBySequence; // 序号到报文映射。

    // 后台线程 -> UI 线程的报文暂存队列（仅数据，不直接操作控件）。
    std::deque<ks::network::PacketRecord> m_pendingPacketQueue;
    mutable std::mutex m_pendingPacketMutex;
    std::uint64_t m_droppedPacketCount = 0; // 队列满时丢弃计数（用于状态提示与日志）。

    // PID 图标缓存：避免重复解析 exe 图标导致 UI 卡顿。
    QHash<quint32, QIcon> m_processIconCacheByPid;
};
