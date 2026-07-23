#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QComboBox;
class QLineEdit;
class QPoint;
class QPushButton;
class QShowEvent;
class QTableWidget;

// WindowGuiHandleTab：只读枚举当前 Session 的 USER Handle 共享表。
// 页面不会修改、关闭或释放任何 GUI 对象。
class WindowGuiHandleTab final : public QWidget
{
public:
    explicit WindowGuiHandleTab(QWidget* parent = nullptr);
    ~WindowGuiHandleTab() override = default;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void initializeUi();
    void refreshAsync();
    void applySnapshot(QVector<QStringList> rows, const QString& statusText);
    void rebuildTable();
    void showCopyMenu(const QPoint& position);
    static QString rowClipboardText(QTableWidget* table, int row, bool includeHeader);

    QComboBox* m_typeFilterCombo = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;
    QVector<QStringList> m_rows;
    bool m_refreshing = false;
    bool m_firstRefreshStarted = false;
};
