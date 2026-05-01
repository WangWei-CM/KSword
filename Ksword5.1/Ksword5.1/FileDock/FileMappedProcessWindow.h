#pragma once

// ============================================================
// FileMappedProcessWindow.h
// 作用：
// - 展示 Phase-7 文件 Section/ControlArea 反查映射进程结果；
// - 后台调用 ArkDriverClient，不在 UI 文件中直接 DeviceIoControl；
// - 支持跳转到进程详情与复制映射诊断行。
// ============================================================

#include "../Framework.h"
#include "../ArkDriverClient/ArkDriverTypes.h"

#include <QDialog>

#include <cstdint>
#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

class FileMappedProcessWindow final : public QDialog
{
public:
    using OpenProcessDetailCallback = std::function<void(std::uint32_t)>;

    // 构造函数作用：
    // - 接收待扫描文件路径；
    // - 初始化界面；
    // - 自动启动第一次 R0 映射反查。
    // 参数 targetPaths：文件路径列表。
    // 参数 parent：Qt 父对象。
    // 返回：无。
    explicit FileMappedProcessWindow(const std::vector<QString>& targetPaths, QWidget* parent = nullptr);

    // setOpenProcessDetailCallback 作用：
    // - 设置“转到进程详情”的外部桥接回调；
    // - FileDock 负责把回调转发给 MainWindow。
    // 参数 callback：PID 回调。
    // 返回：无。
    void setOpenProcessDetailCallback(OpenProcessDetailCallback callback);

private:
    struct MappedProcessRow
    {
        QString targetPath;                       // targetPath：命中文件路径。
        QString processName;                      // processName：R3 补齐的进程名。
        ksword::ark::FileSectionMappingEntry map; // map：R0 映射关系原始行。
    };

    struct RefreshResult
    {
        std::vector<MappedProcessRow> rows; // rows：展示行。
        QString diagnosticText;             // diagnosticText：状态栏诊断。
        std::uint64_t elapsedMs = 0;        // elapsedMs：后台耗时。
    };

    enum class TableColumn
    {
        TargetPath = 0,
        SectionKind,
        ProcessId,
        ProcessName,
        ViewMapType,
        BaseAddress,
        EndAddress,
        Size,
        ControlArea,
        Count
    };

private:
    // initializeUi 作用：创建工具栏、状态栏和结果表格。
    void initializeUi();
    // initializeConnections 作用：绑定刷新、跳转和右键菜单。
    void initializeConnections();
    // requestRefresh 作用：异步调用 R0 文件映射反查。
    void requestRefresh(bool forceRefresh);
    // applyRefreshResult 作用：主线程回填表格和状态栏。
    void applyRefreshResult(std::uint64_t refreshTicket, const RefreshResult& refreshResult);
    // rebuildTable 作用：根据 m_rows 重建 QTreeWidget。
    void rebuildTable();
    // selectedRow 作用：返回当前选中行缓存。
    const MappedProcessRow* selectedRow() const;
    // openCurrentProcessDetail 作用：跳转当前行 PID 的进程详情。
    void openCurrentProcessDetail();
    // copyCurrentRow 作用：复制当前行的可见列。
    void copyCurrentRow();
    // showTableContextMenu 作用：弹出结果表右键菜单。
    void showTableContextMenu(const QPoint& localPosition);
    // applyAdaptiveColumnWidths 作用：按窗口宽度设置列宽。
    void applyAdaptiveColumnWidths();
    // resizeEvent 作用：窗口尺寸变化后刷新列宽。
    void resizeEvent(QResizeEvent* event) override;

private:
    std::vector<QString> m_targetPaths;              // m_targetPaths：待扫描文件。
    std::vector<MappedProcessRow> m_rows;            // m_rows：结果缓存。
    OpenProcessDetailCallback m_openProcessDetailCallback; // m_openProcessDetailCallback：进程详情桥接。

    QVBoxLayout* m_rootLayout = nullptr;             // 根布局。
    QHBoxLayout* m_toolbarLayout = nullptr;          // 工具栏布局。
    QPushButton* m_refreshButton = nullptr;          // 刷新按钮。
    QPushButton* m_openProcessButton = nullptr;      // 跳转进程详情按钮。
    QLabel* m_targetLabel = nullptr;                 // 目标路径标签。
    QLabel* m_statusLabel = nullptr;                 // 状态标签。
    QTreeWidget* m_resultTable = nullptr;            // 结果表格。

    bool m_refreshInProgress = false;                // 是否正在刷新。
    bool m_refreshPending = false;                   // 是否有排队刷新。
    std::uint64_t m_refreshTicket = 0;               // 刷新序号。
    int m_refreshProgressPid = 0;                    // kPro 任务 ID。
};
