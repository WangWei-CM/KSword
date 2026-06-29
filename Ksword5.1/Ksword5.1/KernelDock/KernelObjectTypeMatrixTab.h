#pragma once

// ============================================================
// KernelObjectTypeMatrixTab.h
// 作用说明：
// 1) 提供对象类型统计与可枚举策略矩阵；
// 2) 复用 KernelDockQueryWorker 的 ObjectTypesInformation 解析；
// 3) 只做 R3 只读展示，不枚举进程句柄、不访问 R0。
// ============================================================

#include "KernelDock.h"

#include <QWidget>

#include <atomic>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QPoint;
class QTableWidget;
class QTableWidgetItem;
class CodeEditorWidget;

class KernelObjectTypeMatrixTab final : public QWidget
{
public:
    explicit KernelObjectTypeMatrixTab(QWidget* parent = nullptr);
    ~KernelObjectTypeMatrixTab() override = default;

    static QString strategyForType(const QString& typeNameText);
    static QString formatAccessMask(std::uint32_t accessMask);

private:
    void initializeUi();
    void initializeConnections();
    void refreshAsync();
    void applyRefreshResult(std::vector<KernelObjectTypeEntry> rows, const QString& errorText, bool success);
    void rebuildTable();
    void showContextMenu(const QPoint& localPosition);
    void copyCurrentRow() const;
    void updateDetailForRow(int tableRow);
    QString buildDetailText(const KernelObjectTypeEntry& entry) const;

    // buildDiagnosticDetailText：
    // - 输入：刷新失败、无数据或筛选无结果的原因；
    // - 处理：展开当前筛选、数据来源和下一步排查方向；
    // - 返回：可直接写入详情编辑器的只读文本。
    QString buildDiagnosticDetailText(const QString& reasonText) const;

    // insertDiagnosticRow：
    // - 输入：诊断行标题和详情文本；
    // - 处理：向表格写入一行可复制的占位记录；
    // - 返回：无返回值，详情文本保存在 UserRole + 2。
    void insertDiagnosticRow(const QString& titleText, const QString& detailText);

    bool rowMatchesFilter(const KernelObjectTypeEntry& entry) const;
    std::size_t sourceIndexForTableRow(int tableRow) const;
    static QTableWidgetItem* readOnlyItem(const QString& text);

private:
    QPushButton* m_refreshButton = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;
    CodeEditorWidget* m_detailEditor = nullptr;

    std::atomic_bool m_refreshing{ false };
    std::vector<KernelObjectTypeEntry> m_rows;
};
