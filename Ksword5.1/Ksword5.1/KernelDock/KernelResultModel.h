#pragma once

#include "../UI/VisibleTableWidget.h"
#include "../UI/CompleteExport.h"
#include "../Internationalization/LanguageManager.h"
#include <QAbstractTableModel>
#include <QTreeView>
#include <QTableWidgetItem>
#include <QSet>
#include <QColor>
#include <functional>
#include <memory>
#include <vector>

class QLineEdit;

// KernelDock-local immutable ownership. The retirement alias is the existing
// complete-export value lane, including superseded requests and page close.
template<class T> class KernelSnapshotRows
{
    std::shared_ptr<const std::vector<T>> m_rows;
public:
    KernelSnapshotRows() : m_rows(std::make_shared<const std::vector<T>>()) {}
    KernelSnapshotRows& operator=(std::vector<T>&& rows)
    {
        m_rows = ks::ui::RetainCompleteExportSnapshot<std::vector<T>>(
            std::make_shared<const std::vector<T>>(std::move(rows)));
        return *this;
    }
    std::size_t size() const { return m_rows->size(); }
    bool empty() const { return m_rows->empty(); }
    const T& operator[](std::size_t index) const { return (*m_rows)[index]; }
    auto begin() const { return m_rows->begin(); }
    auto end() const { return m_rows->end(); }
    operator const std::vector<T>&() const { return *m_rows; }
};

namespace ksword::kernel_dock_internal
{
    struct ResultColors
    {
        QColor success, warning, error, secondary, muted, primary, warningAccent;
        static ResultColors capture();
        static ResultColors current();
        QColor SuccessColor() const { return success; }
        QColor WarningColor() const { return warning; }
        QColor ErrorColor() const { return error; }
        QColor TextSecondaryColor() const { return secondary; }
        QColor SurfaceMutedColor() const { return muted; }
        QColor TextPrimaryColor() const { return primary; }
        QColor WarningAccentColor() const { return warningAccent; }
    };
    inline thread_local const ks::i18n::SourceTextSnapshot* resultTexts = nullptr;
    inline thread_local const ResultColors* resultColors = nullptr;
    struct ResultTextScope
    {
        const ks::i18n::SourceTextSnapshot* previous;
        explicit ResultTextScope(const ks::i18n::SourceTextSnapshot& texts) : previous(resultTexts) { resultTexts = &texts; }
        ~ResultTextScope() { resultTexts = previous; }
    };
}

// QTableWidgetItem is used only as Qt's detached, non-QObject role value and
// comparator. It is never inserted into a QTableWidget or mutated after publish.
// This retains native QVariant numeric/string ordering and every original role.
struct KernelResultValues
{
    using Row = std::vector<std::unique_ptr<QTableWidgetItem>>;
    std::vector<Row> rows;
    int columns = 0, separator = -1, separatorColumn = -1;
    explicit KernelResultValues(std::size_t count = 0, int columnCount = 0) : rows(count), columns(columnCount)
    { for (auto& row : rows) row.resize(columnCount); }
    void setItem(int row, int column, QTableWidgetItem* item) { rows.at(row).at(column).reset(item); }
    int columnCount() const { return columns; }
    void setSpan(int row, int column, int, int) { separator = row; separatorColumn = column; }
};
struct KernelResultProjection
{
    std::shared_ptr<const KernelResultValues> source;
    QVector<int> order, inverse;
    QSet<int> excluded;
    QString language;
};

class KernelResultTable final : public ks::ui::TableActionTableView
{
public:
    using Factory = std::function<void(KernelResultValues&)>;
    explicit KernelResultTable(QWidget* parent = nullptr);
    ~KernelResultTable() override;
    void setColumnCount(int count);
    void setHorizontalHeaderLabels(const QStringList& labels);
    QTableWidgetItem* horizontalHeaderItem(int column) const;
    int rowCount() const;
    int columnCount() const;
    int currentRow() const { return currentIndex().row(); }
    int currentColumn() const { return currentIndex().column(); }
    QTableWidgetItem* item(int row, int column) const;
    QTableWidgetItem* itemAt(const QPoint& point) const { const auto index = indexAt(point); return item(index.row(), index.column()); }
    void setCurrentCell(int row, int column) { setCurrentIndex(model()->index(row, column)); }
    void prepare(std::size_t rows, Factory factory, QString query, int searchRole,
        std::function<void()> ready, bool replaceSource = true);
    void filter(QString query);
    void setFilterEdit(QLineEdit* edit);
    std::function<void()> preparationFailed;
    int separatorRow() const;
private:
    class Model;
    Model* m_values;
    struct State;
    std::unique_ptr<State> m_state;
    void request();
    void publish(std::shared_ptr<const KernelResultProjection> result, quint64 revision);
};

// Two-level Minifilter value tree. Child parent IDs are resolved on the worker;
// duplicate registration keys deliberately route to the last original parent.
struct KernelMinifilterValues
{
    struct Node
    {
        QStringList cells;
        QHash<int, QHash<int, QVariant>> roles;
        QVector<int> children;
        int parent = -1, position = -1;
        bool accepted = true;
    };
    QVector<Node> nodes;
    QVector<int> roots, expandRoots;
};
class KernelMinifilterTree final : public QTreeView
{
public:
    explicit KernelMinifilterTree(QWidget* parent = nullptr);
    ~KernelMinifilterTree() override;
    void setColumnCount(int count);
    void setHeaderLabels(const QStringList& labels);
    void prepare(std::function<std::shared_ptr<KernelMinifilterValues>()> factory,
        std::function<void()> ready, quint64 sourceVersion, std::function<bool()> accepts);
private:
    class Model;
    Model* m_values;
    struct State;
    std::unique_ptr<State> m_state;
};
