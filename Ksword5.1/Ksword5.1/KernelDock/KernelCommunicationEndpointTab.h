#pragma once

// ============================================================
// KernelCommunicationEndpointTab.h
// 作用说明：
// 1) 提供通信端点对象命名空间聚合视图；
// 2) 复用目录递归 Worker 只读枚举 ALPC/RPC 相关命名对象；
// 3) 不使用句柄枚举，不新增 R0 协议。
// ============================================================

#include "KernelObjectDirectoryDeepWorker.h"

#include <QWidget>

#include <atomic>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QPoint;
class QTableWidget;
class QTableWidgetItem;

class KernelCommunicationEndpointTab final : public QWidget
{
public:
    explicit KernelCommunicationEndpointTab(QWidget* parent = nullptr);
    ~KernelCommunicationEndpointTab() override = default;

private:
    void initializeUi();
    void initializeConnections();
    void refreshAsync();
    void applyRefreshResult(std::vector<KernelObjectDirectoryDeepEntry> rows, const QString& errorText, bool success);
    void rebuildTable();
    void showContextMenu(const QPoint& localPosition);
    void copyCurrentRow() const;

    bool rowMatchesFilter(const KernelObjectDirectoryDeepEntry& entry) const;
    static bool isCommunicationEndpoint(const KernelObjectDirectoryDeepEntry& entry);
    static QTableWidgetItem* readOnlyItem(const QString& text);

private:
    QPushButton* m_refreshButton = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;

    std::atomic_bool m_refreshing{ false };
    std::vector<KernelObjectDirectoryDeepEntry> m_rows;
};
