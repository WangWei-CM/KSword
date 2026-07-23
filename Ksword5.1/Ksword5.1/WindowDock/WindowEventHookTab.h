#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPoint;
class QPushButton;
class QShowEvent;
class QTableWidget;

// WindowEventHookTab：通过 ArkDriverClient 只读显示 WinEvent/tagEVENTHOOK 快照。
// 页面不提供 UnhookWinEvent 或任何链表修改操作。
class WindowEventHookTab final : public QWidget
{
public:
    explicit WindowEventHookTab(QWidget* parent = nullptr);
    ~WindowEventHookTab() override = default;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void initializeUi();
    void refreshAsync();
    void applySnapshot(QVector<QStringList> rows, const QString& statusText);
    void rebuildTable();
    void showCopyMenu(const QPoint& position);
    static QString rowClipboardText(QTableWidget* table, int row, bool includeHeader);

    QLineEdit* m_filterEdit = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;
    QVector<QStringList> m_rows;
    bool m_refreshing = false;
    bool m_firstRefreshStarted = false;
};
