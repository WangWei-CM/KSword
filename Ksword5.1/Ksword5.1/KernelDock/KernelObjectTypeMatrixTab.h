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

    bool rowMatchesFilter(const KernelObjectTypeEntry& entry) const;
    static QTableWidgetItem* readOnlyItem(const QString& text);

private:
    QPushButton* m_refreshButton = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;

    std::atomic_bool m_refreshing{ false };
    std::vector<KernelObjectTypeEntry> m_rows;
};
