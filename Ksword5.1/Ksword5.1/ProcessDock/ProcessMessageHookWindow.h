#pragma once

// ============================================================
// ProcessMessageHookWindow.h
// 作用：
// - 在独立非模态窗口中展示目标侧、安装者侧或两侧关联的消息 Hook；
// - 查询复用 ArkDriverClient 的 Win32k PDB 只读快照接口；
// - R0 按所选 owner/target 范围筛选，R3 再按同一语义复核；不修改 Hook 或驱动状态。
// ============================================================

#include <QDialog>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>
#include <memory>
namespace ks::ui { class ImmutableValueTable; }

class QComboBox;
class QLabel;
class QPoint;
class QPushButton;
class QTableWidget;

struct ProcessMessageHookTarget
{
    std::uint32_t processId = 0; // processId：目标进程 PID。
    std::uint32_t sessionId = 0; // sessionId：目标进程所在登录会话。
    std::uint64_t creationTime100ns = 0; // creationTime100ns：防止窗口刷新时 PID 已复用。
    QString processName;         // processName：用于窗口摘要的进程显示名。
};

class ProcessMessageHookWindow final : public QDialog
{
public:
    explicit ProcessMessageHookWindow(
        const ProcessMessageHookTarget& target,
        QWidget* parent = nullptr);

public:
    enum class QueryScope
    {
        TargetThreads = 0,
        InstalledByProcess,
        RelatedToProcess
    };

    enum class Column
    {
        TargetProcessId = 0,
        TargetThreadId,
        HookType,
        OwnerProcessId,
        OwnerThreadId,
        CallbackAddress,
        Module,
        Flags,
        Status,
        SessionId,
        HookHandle,
        HookObject,
        ModuleBase,
        ProcedureOffset,
        Source,
        LastStatus,
        Diagnostic,
        Count
    };

private:
    struct QueryResult
    {
        bool ioOk = false;
        bool unsupported = false;
        QueryScope queryScope = QueryScope::TargetThreads;
        std::uint32_t status = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t matchedCount = 0;
        std::uint32_t discoveredChainCount = 0;
        std::uint32_t visitedNodeCount = 0;
        std::uint32_t readFailureCount = 0;
        std::uint32_t corruptLinkCount = 0;
        std::uint32_t duplicateCount = 0;
        long lastStatus = 0;
        QString ioMessage;
        QString detail;
        std::vector<QStringList> rows;
    };

    // initializeUi：创建顶部目标摘要、刷新按钮、A/B 列组和结果表格。
    void initializeUi();
    // initializeConnections：连接刷新、列组、表头菜单和表格复制菜单。
    void initializeConnections();
    // requestRefresh：在线程池调用 ArkDriverClient，避免阻塞主窗口。
    void requestRefresh();
    // currentQueryScope：返回当前 owner/target 查询范围，非法值回退到目标线程。
    QueryScope currentQueryScope() const;
    // applyQueryResult：在 UI 线程应用一次查询结果。
    void applyQueryResult(std::uint64_t ticket, const std::shared_ptr<const QueryResult>& result);
    // rebuildTable：使用已过滤行重建表格，并为空结果保留可复制诊断行。
    void rebuildTable(const std::shared_ptr<const QueryResult>& result);
    void finishQueryPresentation(std::uint64_t ticket, const std::shared_ptr<const QueryResult>& result);
    // applyColumnPreset：应用互补的 A/B 精简列组。
    void applyColumnPreset(const QString& presetName);
    // updateColumnPresetButtons：同步 A/B 按钮的选中样式。
    void updateColumnPresetButtons();
    // showHeaderContextMenu：表头右键逐列显隐。
    void showHeaderContextMenu(const QPoint& localPosition);
    // showTableContextMenu：表格右键复制单元格、当前行或全部行。
    void showTableContextMenu(const QPoint& localPosition);
    // copyCurrentCell：复制当前单元格。
    void copyCurrentCell() const;
    // copyCurrentRow：复制当前行全部字段。
    void copyCurrentRow() const;
    // copyAllRows：复制表头和全部结果行。
    void copyAllRows() const;
    // tableRowText：把指定行按 TSV 序列化。
    QString tableRowText(int row) const;
    // tableHeaderText：把完整表头按 TSV 序列化。
    QString tableHeaderText() const;
    // visibleColumnCount：统计当前可见列，防止用户隐藏最后一列。
    int visibleColumnCount() const;

private:
    ProcessMessageHookTarget m_target; // m_target：窗口绑定的稳定进程快照。
    QLabel* m_targetLabel = nullptr;   // m_targetLabel：PID/Session/进程名摘要。
    QLabel* m_statusLabel = nullptr;   // m_statusLabel：异步查询状态与诊断摘要。
    QPushButton* m_refreshButton = nullptr; // m_refreshButton：手动重新查询。
    QComboBox* m_scopeCombo = nullptr; // m_scopeCombo：目标侧、所有者侧或两侧查询范围。
    QPushButton* m_columnAButton = nullptr; // m_columnAButton：常用定位列组。
    QPushButton* m_columnBButton = nullptr; // m_columnBButton：底层证据列组。
    ks::ui::ImmutableValueTable* m_table = nullptr;   // m_table：消息 Hook 结果表格。
    bool m_refreshInProgress = false;  // m_refreshInProgress：防止并发重复查询。
    bool m_refreshPending = false;     // m_refreshPending：查询中再次刷新时排队一次。
    std::uint64_t m_refreshTicket = 0; // m_refreshTicket：丢弃过期查询结果。
};
