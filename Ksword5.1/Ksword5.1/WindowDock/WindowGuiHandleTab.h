#pragma once

#include <cstdint>
#include "../UI/ImmutableValueTable.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

namespace ks::ui { class AsyncOperation; }

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
    ~WindowGuiHandleTab() override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void initializeUi();
    void reconcileRefreshState();
    void refreshAsync();
    bool applySnapshot(ks::ui::ImmutableTableSource rows, const QString& statusText, std::uint64_t ticket);
    void rebuildTable();
    void showCopyMenu(const QPoint& position);
    static QString rowClipboardText(ks::ui::ImmutableValueTable* table, int row, bool includeHeader);

    QComboBox* m_typeFilterCombo = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    ks::ui::ImmutableValueTable* m_table = nullptr;
    ks::ui::ImmutableTableSource m_rows;
    ks::ui::AsyncOperation* m_refreshOperation = nullptr;
    bool m_refreshing = false;
    bool m_firstRefreshStarted = false;
};
