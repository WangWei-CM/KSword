#pragma once

// ============================================================
// KernelDeviceDriverObjectsTab.h
// 作用说明：
// 1) 提供“设备与驱动”专项视图的独立 QWidget；
// 2) 仅做 R3 只读展示、筛选、复制与 TSV 导出；
// 3) 后续可被 KernelDock 挂载为子页，但本轮不修改集成文件；
// 4) TODO(集成)：后续由 KernelDock.cpp / KernelDock.h 负责挂载到现有对象命名空间页。
// ============================================================

#include "KernelDeviceDriverObjectsWorker.h"

#include <QWidget>

#include <atomic>  // std::atomic_bool：控制后台刷新互斥。
#include <vector>  // std::vector：缓存枚举结果与过滤结果。

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QPoint;
class QTableWidget;
class QTableWidgetItem;
class QVBoxLayout;

// ============================================================
// KernelDeviceDriverObjectsTab
// 作用：
// - 输入：Qt 父控件；
// - 处理：异步枚举对象管理器中的设备/驱动/文件系统对象，并在 UI 中按目录、
//   类型和关键字过滤展示；
// - 输出：只读表格、状态标签和可导出的 TSV 文本。
// ============================================================
class KernelDeviceDriverObjectsTab final : public QWidget
{
public:
    // 构造函数：
    // - 输入 parent：Qt 父控件；
    // - 处理：创建 UI，发起首次后台刷新；
    // - 返回：无返回值。
    explicit KernelDeviceDriverObjectsTab(QWidget* parent = nullptr);

    // 析构函数：
    // - 处理：对象生命周期交给 Qt 父子树管理；
    // - 返回：无返回值。
    ~KernelDeviceDriverObjectsTab() override = default;

private:
    // initializeUi：
    // - 处理：创建顶部工具栏、过滤器和结果表格；
    // - 返回：无返回值。
    void initializeUi();

    // initializeConnections：
    // - 处理：连接按钮、过滤器和表格右键菜单；
    // - 返回：无返回值。
    void initializeConnections();

    // refreshAsync：
    // - 处理：在后台线程执行 R3 枚举任务；
    // - 返回：无返回值，结果会回投到 UI 线程。
    void refreshAsync();

    // applyRefreshResult：
    // - 输入 rows：后台枚举结果；input errorText：致命错误文本；
    // - 处理：回到 UI 线程后重建过滤器和表格；
    // - 返回：无返回值。
    void applyRefreshResult(
        std::vector<KernelDeviceDriverObjectEntry> rows,
        const QString& errorText);

    // rebuildVisibleRows：
    // - 处理：根据当前过滤器筛选可见行并重建表格；
    // - 返回：无返回值。
    void rebuildVisibleRows();

    // populateFilterCombos：
    // - 输入 rows：当前全量结果；
    // - 处理：刷新目录/类型下拉框；
    // - 返回：无返回值。
    void populateFilterCombos(const std::vector<KernelDeviceDriverObjectEntry>& rows);

    // matchesCurrentFilters：
    // - 输入 entry：待判断行；
    // - 处理：按目录、对象类型和关键字做只读筛选；
    // - 返回：true 表示该行应显示。
    bool matchesCurrentFilters(const KernelDeviceDriverObjectEntry& entry) const;

    // rebuildTableWidget：
    // - 处理：把当前可见行写入表格控件；
    // - 返回：无返回值。
    void rebuildTableWidget();

    // updateStatusText：
    // - 输入 errorText：可选错误文本；
    // - 处理：根据当前加载与过滤状态刷新状态标签颜色和文案；
    // - 返回：无返回值。
    void updateStatusText(const QString& errorText = QString());

    // showTableContextMenu：
    // - 输入 localPosition：表格视口中的点击位置；
    // - 处理：弹出复制与导出菜单；
    // - 返回：无返回值。
    void showTableContextMenu(const QPoint& localPosition);

    // copyCellAt：
    // - 输入 row / column：目标单元格位置；
    // - 处理：把单元格文本写入剪贴板；
    // - 返回：无返回值。
    void copyCellAt(int row, int column) const;

    // copyRowAt：
    // - 输入 row：目标行；
    // - 处理：把整行导出为 TSV 风格文本并写入剪贴板；
    // - 返回：无返回值。
    void copyRowAt(int row) const;

    // copyVisibleRowsAsTsv：
    // - 处理：把当前可见结果写入剪贴板；
    // - 返回：无返回值。
    void copyVisibleRowsAsTsv() const;

    // exportVisibleRowsAsTsv：
    // - 处理：把当前可见结果另存为 TSV 文件；
    // - 返回：无返回值。
    void exportVisibleRowsAsTsv();

    // rowsToTsv：
    // - 输入 includeHeader：true 时输出表头；
    // - 处理：把当前可见结果序列化为 TSV；
    // - 返回：TSV 文本。
    QString rowsToTsv(bool includeHeader) const;

    // sanitizeTsvField：
    // - 输入 text：原始字段文本；
    // - 处理：把制表符与换行压平，避免 TSV 结构错位；
    // - 返回：适合写入 TSV 的单字段文本。
    static QString sanitizeTsvField(const QString& text);

    // makeReadOnlyItem：
    // - 输入 text：单元格内容；
    // - 处理：创建不可编辑的 QTableWidgetItem；
    // - 返回：可直接塞入表格的 item 指针。
    static QTableWidgetItem* makeReadOnlyItem(const QString& text);

private:
    QVBoxLayout* m_rootLayout = nullptr;          // m_rootLayout：页面根布局。
    QWidget* m_toolbarWidget = nullptr;           // m_toolbarWidget：顶部工具栏容器。
    QWidget* m_filterWidget = nullptr;            // m_filterWidget：过滤器行容器。
    QPushButton* m_refreshButton = nullptr;       // m_refreshButton：刷新按钮。
    QPushButton* m_exportButton = nullptr;        // m_exportButton：导出按钮。
    QLabel* m_statusLabel = nullptr;              // m_statusLabel：状态标签。
    QComboBox* m_directoryFilterCombo = nullptr;   // m_directoryFilterCombo：目录过滤器。
    QComboBox* m_typeFilterCombo = nullptr;        // m_typeFilterCombo：对象类型过滤器。
    QLineEdit* m_keywordEdit = nullptr;           // m_keywordEdit：关键字过滤框。
    QTableWidget* m_tableWidget = nullptr;        // m_tableWidget：结果表格。

    std::vector<KernelDeviceDriverObjectEntry> m_allRows;     // m_allRows：后台枚举得到的全量结果。
    std::vector<KernelDeviceDriverObjectEntry> m_visibleRows;  // m_visibleRows：当前过滤后可见结果。
    std::atomic_bool m_refreshRunning = false;                 // m_refreshRunning：刷新互斥标记。
};
