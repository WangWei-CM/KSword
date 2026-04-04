#pragma once

// ============================================================
// FileHandleUsageWindow.h
// 作用：
// - 显示“文件/文件夹占用句柄扫描”结果；
// - 提供刷新、复制、转到进程详情交互；
// - 与 FileDock 解耦，便于独立维护窗口逻辑。
// ============================================================

#include "FileHandleUsageScanner.h"

#include <QDialog>

#include <cstdint>
#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QPoint;
class QPushButton;
class QResizeEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

class FileHandleUsageWindow final : public QDialog
{
public:
    // OpenProcessDetailCallback 作用：
    // - 把“打开进程详情”行为回调给外层（FileDock/MainWindow）；
    // - 参数为目标 PID。
    using OpenProcessDetailCallback = std::function<void(std::uint32_t)>;

public:
    // 构造函数作用：
    // - 接收目标路径集合并初始化窗口 UI；
    // - 默认启动一次异步扫描。
    // 参数 targetPaths：要扫描占用的目标路径列表。
    // 参数 parent：父窗口。
    explicit FileHandleUsageWindow(const std::vector<QString>& targetPaths, QWidget* parent = nullptr);

    // setOpenProcessDetailCallback 作用：
    // - 注入“转到进程详情”的外部处理逻辑；
    // - 在窗口内触发后回调该函数。
    // 参数 callback：回调函数对象。
    void setOpenProcessDetailCallback(OpenProcessDetailCallback callback);

protected:
    // resizeEvent 作用：
    // - 在窗口尺寸变化时重新分配结果表列宽；
    // - 让长文本列自动适应当前窗口空间。
    void resizeEvent(QResizeEvent* event) override;

private:
    // TableColumn 作用：统一表格列索引，避免魔法数字。
    enum class TableColumn : int
    {
        ProcessId = 0,   // ProcessId：PID。
        ProcessName,     // ProcessName：进程名。
        HandleValue,     // HandleValue：句柄值。
        TypeName,        // TypeName：对象类型。
        ObjectName,      // ObjectName：对象名称。
        AccessMask,      // AccessMask：访问掩码。
        MatchPath,       // MatchPath：命中目标路径。
        MatchRule,       // MatchRule：命中规则（目录/精确）。
        ProcessPath,     // ProcessPath：进程镜像路径。
        Count            // Count：列总数。
    };

private:
    // initializeUi 作用：构建窗口控件与表格列。
    void initializeUi();

    // initializeConnections 作用：绑定按钮和表格交互事件。
    void initializeConnections();

    // requestRefresh 作用：
    // - 启动异步扫描任务；
    // - forceRefresh=true 时可记录“扫描中再次刷新”请求。
    void requestRefresh(bool forceRefresh);

    // applyRefreshResult 作用：主线程应用扫描结果并刷新表格。
    void applyRefreshResult(std::uint64_t refreshTicket, const filedock::handleusage::HandleUsageScanResult& refreshResult);

    // rebuildTable 作用：按 entries 重建结果表。
    void rebuildTable(const std::vector<filedock::handleusage::HandleUsageEntry>& entries);

    // applyAdaptiveColumnWidths 作用：
    // - 根据窗口可视宽度重新分配结果表列宽；
    // - 让对象名/进程路径/命中目标三列优先占据剩余空间。
    void applyAdaptiveColumnWidths();

    // selectedEntry 作用：读取当前选中行对应的数据指针。
    const filedock::handleusage::HandleUsageEntry* selectedEntry() const;

    // copyCurrentRow 作用：复制当前行为 TAB 分隔文本。
    void copyCurrentRow();

    // openCurrentProcessDetail 作用：按当前行 PID 请求打开进程详情。
    void openCurrentProcessDetail();

    // showTableContextMenu 作用：弹出右键菜单（复制/转到进程详情）。
    void showTableContextMenu(const QPoint& localPosition);

private:
    std::vector<QString> m_targetPaths; // m_targetPaths：扫描目标路径集合。
    OpenProcessDetailCallback m_openProcessDetailCallback; // m_openProcessDetailCallback：进程详情回调。

    QVBoxLayout* m_rootLayout = nullptr;      // m_rootLayout：窗口根布局。
    QHBoxLayout* m_toolbarLayout = nullptr;   // m_toolbarLayout：顶部工具栏布局。
    QPushButton* m_refreshButton = nullptr;   // m_refreshButton：刷新按钮（图标）。
    QPushButton* m_openProcessButton = nullptr; // m_openProcessButton：转到进程详情按钮（图标）。
    QLabel* m_targetLabel = nullptr;          // m_targetLabel：目标路径摘要标签。
    QLabel* m_statusLabel = nullptr;          // m_statusLabel：扫描状态标签。
    QTreeWidget* m_resultTable = nullptr;     // m_resultTable：扫描结果表。

    bool m_refreshInProgress = false;         // m_refreshInProgress：扫描任务运行中标记。
    bool m_refreshPending = false;            // m_refreshPending：是否记录了待执行刷新请求。
    std::uint64_t m_refreshTicket = 0;        // m_refreshTicket：刷新序号，防乱序覆盖。
    int m_refreshProgressPid = 0;             // m_refreshProgressPid：kPro 任务 PID。
    std::vector<filedock::handleusage::HandleUsageEntry> m_entries; // m_entries：当前结果缓存。
};
