#pragma once

// ============================================================
// NetworkAuditPage.h
// 作用：
// 1) 提供只读网络审计页，集中展示 TCP/UDP cross-view、AFD、WFP、NDIS 和 NSI 摘要；
// 2) 仅做用户态审计与交叉验证，不提供任何 disable / detach / bypass 动作；
// 3) 所有刷新逻辑都保持为只读查询，便于 NetworkDock 侧边栏复用。
// ============================================================

#include "../Framework.h"

#include <QWidget>
#include <QJsonValue>

#include <atomic> // std::atomic_bool：防止并发刷新重入。
#include <memory> // std::unique_ptr：异步快照对象托管。
#include <vector> // std::vector：批量快照行缓存。

class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTableWidgetItem;
class QVBoxLayout;
class QHBoxLayout;

class NetworkAuditPage final : public QWidget
{
public:
    // 构造函数：
    // - 输入 parent：Qt 父控件，可为空；
    // - 处理：构建只读审计页 UI，并立即触发首轮异步刷新；
    // - 返回：无。
    explicit NetworkAuditPage(QWidget* parent = nullptr);

    // 析构函数：
    // - 处理：默认析构，Qt 自动释放子控件；
    // - 返回：无。
    ~NetworkAuditPage() override = default;

private:
    // CrossViewRow：TCP/UDP 交叉视图的一行聚合结果。
    struct CrossViewRow
    {
        std::uint32_t processId = 0;
        QString processName;
        std::uint32_t tcpCount = 0;
        std::uint32_t udpCount = 0;
        QString tcpSummary;
        QString udpSummary;
    };

    // TcpEndpointRow：TCP 明细表的一行，合并 R3 连接表与 R0 endpoint 审计行。
    struct TcpEndpointRow
    {
        std::uint32_t processId = 0; // processId：连接拥有者 PID，未知时为 0。
        QString processName;         // processName：R3 进程名或 R0 PID 提示。
        QString localEndpointText;   // localEndpointText：本地 IP:Port。
        QString remoteEndpointText;  // remoteEndpointText：远端 IP:Port。
        QString stateText;           // stateText：TCP 状态或协议状态。
        QString detailText;          // detailText：来源、对象地址、字段掩码等可复制明细。
    };

    // UdpEndpointRow：UDP 明细表的一行，展示本地端点与来源诊断。
    struct UdpEndpointRow
    {
        std::uint32_t processId = 0; // processId：端点拥有者 PID，未知时为 0。
        QString processName;         // processName：R3 进程名或 R0 PID 提示。
        QString localEndpointText;   // localEndpointText：本地 IP:Port。
        QString sourceText;          // sourceText：R3/R0 数据来源。
        QString detailText;          // detailText：对象地址、字段掩码等可复制明细。
    };

    // AfdHandleRow：AFD 关联句柄的一行展示结果。
    struct AfdHandleRow
    {
        std::uint32_t processId = 0;
        QString processName;
        QString handleValueText;
        QString typeName;
        QString objectName;
        QString sourceText;
        QString diffText;
        QString accessText;
        QString detailText;
    };

    // WfpProviderRow：WFP provider 一行展示结果。
    struct WfpProviderRow
    {
        QString nameText;
        QString descriptionText;
        QString guidText;
        QString flagsText;
        QString serviceNameText;
        QString dataSizeText;
    };

    // WfpSubLayerRow：WFP sublayer 一行展示结果。
    struct WfpSubLayerRow
    {
        QString nameText;
        QString descriptionText;
        QString guidText;
        QString flagsText;
        QString providerGuidText;
        QString weightText;
    };

    // WfpCalloutRow：WFP callout 一行展示结果。
    struct WfpCalloutRow
    {
        QString nameText;
        QString descriptionText;
        QString guidText;
        QString flagsText;
        QString providerGuidText;
        QString layerGuidText;
        QString calloutIdText;
    };

    // WfpFilterRow：WFP filter 一行展示结果。
    struct WfpFilterRow
    {
        QString nameText;
        QString descriptionText;
        QString guidText;
        QString flagsText;
        QString providerGuidText;
        QString layerGuidText;
        QString subLayerGuidText;
        QString weightText;
        QString actionText;
        QString conditionText;
        QString filterIdText;
    };

    // NdisAdapterRow：NDIS miniport/adapter 一行展示结果。
    struct NdisAdapterRow
    {
        QString nameText;
        QString descriptionText;
        QString ifIndexText;
        QString statusText;
        QString macText;
        QString linkSpeedText;
        QString connectionStateText;
    };

    // NdisBindingRow：NDIS binding 一行展示结果。
    struct NdisBindingRow
    {
        QString adapterNameText;
        QString displayNameText;
        QString componentIdText;
        QString enabledText;
        QString instanceIdText;
    };

    // NdisProtocolRow：NDIS protocol / interface 一行展示结果。
    struct NdisProtocolRow
    {
        QString interfaceAliasText;
        QString ifIndexText;
        QString addressFamilyText;
        QString connectionStateText;
        QString interfaceMetricText;
        QString mtuText;
    };

    // NsiSummaryRow：NSI 摘要的一行展示结果。
    struct NsiSummaryRow
    {
        QString metricText;
        QString valueText;
    };

    // R0NetworkSummaryRow：R0 网络审计 wrapper 的一行摘要。
    // 输入：由 buildAuditSnapshot 调用 ArkDriverClient 后填充。
    // 处理：UI 只展示 ok/unsupported/unavailable、计数、截断和 message。
    // 返回：结构体无函数返回，refreshNsiSummaryTable 会把它转换成表格行。
    struct R0NetworkSummaryRow
    {
        QString nameText;
        QString statusText;
        QString countText;
        QString truncatedText;
        QString messageText;
    };

    // AuditSnapshot：一次刷新需要的全部只读审计快照。
    struct AuditSnapshot
    {
        std::vector<TcpEndpointRow> tcpEndpointRows;
        std::vector<UdpEndpointRow> udpEndpointRows;
        std::vector<CrossViewRow> crossViewRows;
        std::vector<AfdHandleRow> afdRows;
        std::vector<WfpProviderRow> wfpProviderRows;
        std::vector<WfpSubLayerRow> wfpSubLayerRows;
        std::vector<WfpCalloutRow> wfpCalloutRows;
        std::vector<WfpFilterRow> wfpFilterRows;
        std::vector<NdisAdapterRow> ndisAdapterRows;
        std::vector<NdisBindingRow> ndisBindingRows;
        std::vector<NdisProtocolRow> ndisProtocolRows;
        std::vector<NsiSummaryRow> nsiSummaryRows;
        std::vector<R0NetworkSummaryRow> r0SummaryRows;
        QString statusText;
        QString detailText;
    };

    // initializeUi 作用：
    // - 构建顶部控制栏和四个审计分区；
    // - 输入：无；
    // - 返回：无。
    void initializeUi();

    // initializeConnections 作用：
    // - 连接刷新按钮和表格交互；
    // - 输入：无；
    // - 返回：无。
    void initializeConnections();

    // refreshAllSnapshotsAsync 作用：
    // - 后台一次性采集全部只读审计快照；
    // - forceRefresh 表示用户主动刷新；
    // - 返回：无，结果通过 UI 线程回投。
    void refreshAllSnapshotsAsync(bool forceRefresh);

    // applySnapshot 作用：
    // - 将后台采集完成的全部快照写回各个表格；
    // - 输入 snapshot：后台线程采集结果；
    // - 返回：无。
    void applySnapshot(const AuditSnapshot& snapshot);

    // refreshCrossViewTable 作用：
    // - 重建 TCP/UDP 明细表和进程维度交叉视图表格；
    // - 输入 snapshot：来自后台的完整网络快照；
    // - 返回：无。
    void refreshCrossViewTable(const AuditSnapshot& snapshot);

    // refreshAfdTable 作用：
    // - 重建 AFD 相关句柄表格；
    // - 输入 snapshot：来自后台的 AFD 句柄结果；
    // - 返回：无。
    void refreshAfdTable(const std::vector<AfdHandleRow>& snapshot);

    // refreshWfpTables 作用：
    // - 重建 WFP provider / sublayer / callout / filter 四张只读表；
    // - 输入 snapshot：来自后台的 WFP 目录结果；
    // - 返回：无。
    void refreshWfpTables(const AuditSnapshot& snapshot);

    // refreshNdisTables 作用：
    // - 重建 NDIS miniport / binding / protocol 表；
    // - 输入 snapshot：来自后台的 NDIS 结果；
    // - 返回：无。
    void refreshNdisTables(const AuditSnapshot& snapshot);

    // refreshNsiSummaryTable 作用：
    // - 重建 NSI 与 R0 网络审计摘要表；
    // - 输入 snapshot：来自后台的完整快照；
    // - 返回：无。
    void refreshNsiSummaryTable(const AuditSnapshot& snapshot);

    // buildAuditSnapshot 作用：
    // - 在后台线程中采集所有只读审计数据；
    // - 返回：完整快照与状态文本。
    static AuditSnapshot buildAuditSnapshot();

    // runPowerShellTextSync 作用：
    // - 同步执行 PowerShell 并返回 stdout 文本；
    // - scriptText 为要执行的脚本；
    // - timeoutMs 为等待超时时间；
    // - 返回：stdout；失败时返回诊断文本。
    static QString runPowerShellTextSync(const QString& scriptText, int timeoutMs, QString* errorTextOut = nullptr);

    // createCell 作用：
    // - 创建统一的只读表格单元格；
    // - 输入 cellText：单元格文本；
    // - 返回：可直接写入表格的 item。
    static QTableWidgetItem* createCell(const QString& cellText);

    // guidToText 作用：
    // - 把 GUID 格式化为可读字符串；
    // - 输入 guid：Windows GUID；
    // - 返回：字符串化 GUID。
    static QString guidToText(const GUID& guid);

    // bytesToHexText 作用：
    // - 把一段字节数格式化为十六进制显示；
    // - 输入 value：待格式化数值；
    // - 返回：十六进制字符串。
    static QString bytesToHexText(std::uint64_t value);

    // objectToText 作用：
    // - 把 PowerShell JSON 值转换为稳定文本；
    // - 输入 value：JSON 值；
    // - 返回：稳定展示文本。
    static QString objectToText(const QJsonValue& value);

    // compareContainsAfd 作用：
    // - 判断对象名称是否属于 AFD 相关对象；
    // - 输入 objectNameText：对象名；
    // - 返回：true=命中 AFD。
    static bool compareContainsAfd(const QString& objectNameText);

    // top-level tabs / controls.
    QVBoxLayout* m_rootLayout = nullptr;
    QHBoxLayout* m_headerLayout = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTabWidget* m_sectionTabWidget = nullptr;

    // TCP / UDP cross-view.
    QWidget* m_crossViewPage = nullptr;
    QSplitter* m_crossViewSplitter = nullptr;
    QSplitter* m_crossViewTopSplitter = nullptr;
    QTableWidget* m_tcpTable = nullptr;
    QTableWidget* m_udpTable = nullptr;
    QTableWidget* m_crossSummaryTable = nullptr;

    // AFD view.
    QWidget* m_afdPage = nullptr;
    QTableWidget* m_afdTable = nullptr;

    // WFP view.
    QWidget* m_wfpPage = nullptr;
    QTabWidget* m_wfpTabWidget = nullptr;
    QTableWidget* m_wfpProviderTable = nullptr;
    QTableWidget* m_wfpSubLayerTable = nullptr;
    QTableWidget* m_wfpCalloutTable = nullptr;
    QTableWidget* m_wfpFilterTable = nullptr;

    // NDIS view.
    QWidget* m_ndisPage = nullptr;
    QTabWidget* m_ndisTabWidget = nullptr;
    QTableWidget* m_ndisAdapterTable = nullptr;
    QTableWidget* m_ndisBindingTable = nullptr;
    QTableWidget* m_ndisProtocolTable = nullptr;

    // NSI summary.
    QWidget* m_nsiPage = nullptr;
    QTableWidget* m_nsiSummaryTable = nullptr;
    QLabel* m_kernelBadgeLabel = nullptr;

    std::atomic_bool m_refreshInProgress{ false };
};
