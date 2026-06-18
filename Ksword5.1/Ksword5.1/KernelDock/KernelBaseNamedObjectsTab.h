#pragma once

// ============================================================
// KernelBaseNamedObjectsTab.h
// 作用：
// 1) 声明 BaseNamedObjects 专项聚合视图；
// 2) 提供 Session、对象类型、关键字过滤；
// 3) 仅依赖 R3 worker，不访问 KswordARK 驱动。
// ============================================================

#include "../Framework.h"
#include "KernelBaseNamedObjectsWorker.h"

#include <QWidget>

#include <atomic>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

// KernelBaseNamedObjectsTab 说明：
// - 输入：Qt parent；
// - 处理逻辑：初始化只读表格和过滤控件，后台执行 BaseNamedObjects 快照；
// - 返回行为：无业务返回值，结果显示在表格中。
class KernelBaseNamedObjectsTab final : public QWidget
{
public:
    explicit KernelBaseNamedObjectsTab(QWidget* parent = nullptr);
    ~KernelBaseNamedObjectsTab() override = default;

private:
    // initializeUi：
    // - 输入：无；
    // - 处理：创建刷新按钮、过滤器和结果表；
    // - 返回：无。
    void initializeUi();

    // initializeConnections：
    // - 输入：无；
    // - 处理：连接刷新按钮和过滤控件；
    // - 返回：无。
    void initializeConnections();

    // refreshSnapshotAsync：
    // - 输入 forceRefresh：用户主动刷新时为 true；
    // - 处理：后台调用 runBaseNamedObjectsSnapshotTask；
    // - 返回：无，结果通过 Qt queued invocation 回到 UI 线程。
    void refreshSnapshotAsync(bool forceRefresh);

    // populateTable：
    // - 输入 rows：worker 返回的完整快照；
    // - 处理：缓存并重建表格和过滤下拉项；
    // - 返回：无。
    void populateTable(const std::vector<KernelBaseNamedObjectEntry>& rows);

    // rebuildFilterOptions：
    // - 输入：无，读取 m_rows；
    // - 处理：刷新 Session/类型下拉框；
    // - 返回：无。
    void rebuildFilterOptions();

    // applyFilters：
    // - 输入：无，读取当前过滤控件；
    // - 处理：隐藏不匹配表格行并刷新状态；
    // - 返回：无。
    void applyFilters();

    // rowMatchesFilters：
    // - 输入 entry：一条 BaseNamedObjects 记录；
    // - 处理：匹配 Session、类型和关键字；
    // - 返回：true 表示该行应显示。
    bool rowMatchesFilters(const KernelBaseNamedObjectEntry& entry) const;

    // setStatusText：
    // - 输入 statusText：展示文本；
    // - 处理：更新状态标签；
    // - 返回：无。
    void setStatusText(const QString& statusText);

private:
    QVBoxLayout* m_rootLayout = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QComboBox* m_sessionFilterCombo = nullptr;
    QComboBox* m_typeFilterCombo = nullptr;
    QLineEdit* m_keywordFilterEdit = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;

    std::vector<KernelBaseNamedObjectEntry> m_rows;
    std::atomic_bool m_refreshing{ false };
};

