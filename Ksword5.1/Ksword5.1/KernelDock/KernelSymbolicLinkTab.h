#pragma once

// ============================================================
// KernelSymbolicLinkTab.h
// 作用说明：
// 1) 提供符号链接专项 QWidget 页面；
// 2) 支持刷新、关键字过滤、目标路径过滤和复制目标路径；
// 3) UI 只消费 R3 Worker 结果，不直接访问驱动或新增 IOCTL。
// ============================================================

#include "KernelSymbolicLinkWorker.h"

#include <QWidget>

#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QPoint;

class KernelSymbolicLinkTab final : public QWidget
{
public:
    // KernelSymbolicLinkTab：
    // - 输入 parent：Qt 父对象，可为空。
    // - 处理逻辑：创建工具栏、说明文本、结果表并连接交互信号。
    // - 返回结果：构造后的页面对象。
    explicit KernelSymbolicLinkTab(QWidget* parent = nullptr);

private:
    enum class Column : int
    {
        SourceDirectory = 0,
        LinkName,
        FullPath,
        TargetPath,
        DosCandidate,
        StatusText,
        Count
    };

    void initializeUi();
    void initializeConnections();
    void refreshAsync();
    void applySnapshotResult(
        std::vector<KernelSymbolicLinkEntry> rows,
        const QString& errorText,
        bool ok);
    void applyFilters();
    void rebuildTable();
    void copyCurrentTarget() const;
    void copyCurrentRow() const;
    void showContextMenu(const QPoint& position);

    static QTableWidgetItem* createReadOnlyItem(const QString& text);
    static QString rowToTsv(const KernelSymbolicLinkEntry& row);

private:
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_copyTargetButton = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QLineEdit* m_targetFilterEdit = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_noteLabel = nullptr;
    QTableWidget* m_table = nullptr;

    std::vector<KernelSymbolicLinkEntry> m_allRows;
    std::vector<KernelSymbolicLinkEntry> m_visibleRows;
    bool m_refreshing = false;
};
