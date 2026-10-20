#include "TableRowFilterIdentityState.h"
#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QStringList>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>

static int checks = 0;
static void check(bool value, const char* message)
{ ++checks; if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
class Model final : public QAbstractTableModel
{
public:
    struct Row { int id; QStringList values; };
    QVector<Row> rows;
    int nextId = 0;
    mutable int reads = 0;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(rows.size()); }
    int columnCount(const QModelIndex& = {}) const override { return 2; }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    { if (!index.isValid() || role != Qt::DisplayRole) return {}; ++reads; return rows.at(index.row()).values.value(index.column()); }
    int insertAt(int at, QStringList values)
    { beginInsertRows({}, at, at); const int id = ++nextId; rows.insert(at, {id, values}); endInsertRows(); return id; }
    void removeAt(int at) { beginRemoveRows({}, at, at); rows.removeAt(at); endRemoveRows(); }
    void change(int row, QString value, int role)
    { if (role == Qt::DisplayRole) rows[row].values[0] = value; emit dataChanged(index(row, 0), index(row, 0), {role}); }
    void reverse()
    {
        emit layoutAboutToBeChanged();
        const auto old = persistentIndexList(); QVector<int> ids;
        for (const auto& index : old) ids.push_back(rows.at(index.row()).id);
        std::reverse(rows.begin(), rows.end());
        QModelIndexList changed;
        for (int i = 0; i < old.size(); ++i)
        { int row = 0; while (rows.at(row).id != ids.at(i)) ++row; changed.push_back(index(row, old.at(i).column())); }
        changePersistentIndexList(old, changed); emit layoutChanged();
    }
    void moveRowAt(int first, int destination)
    {
        if (first == destination || first + 1 == destination) return;
        beginMoveRows({}, first, first, {}, destination);
        Row value = rows.takeAt(first); rows.insert(destination > first ? destination - 1 : destination, value);
        endMoveRows();
    }
    void reset()
    { beginResetModel(); rows.clear(); for (int i = 0; i < 15; ++i) rows.push_back({++nextId, {QStringLiteral("alpha"), QStringLiteral("beta")}}); endResetModel(); }
};
class FakeTable final : public QObject
{
public:
    explicit FakeTable(Model* model) : m_model(model)
    {
        hidden.resize(model->rowCount());
        connect(model, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex&, int first, int last) {
            for (int row = first; row <= last; ++row) hidden.insert(row, false);
        });
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex&, int first, int last) {
            hidden.remove(first, last - first + 1);
        });
        connect(model, &QAbstractItemModel::modelReset, this, [this]() { hidden.fill(false, m_model->rowCount()); });
        connect(model, &QAbstractItemModel::rowsMoved, this, [this](const QModelIndex&, int first, int, const QModelIndex&, int destination) {
            bool value = hidden.takeAt(first); hidden.insert(destination > first ? destination - 1 : destination, value);
        });
    }
    Model* m_model;
    QVector<bool> hidden;
    QSet<int> hiddenColumns;
    bool visible = true;
    QPointer<ks::ui::TableRowFilterIdentityState> rowState;
    QSet<QPersistentModelIndex> business, frozen;
    int visibilityWrites = 0;
    Model* model() const { return m_model; }
    QModelIndex rootIndex() const { return {}; }
    bool isVisible() const { return visible; }
    bool isRowHidden(int row) const { return hidden.value(row); }
    void setRowHidden(int row, bool value) { check(row >= 0 && row < hidden.size(), "out of range visibility write"); hidden[row] = value; ++visibilityWrites; }
    bool isColumnHidden(int column) const { return hiddenColumns.contains(column); }
};
namespace ks::ui
{
    static TableRowFilterIdentityState* stateFor(FakeTable* table)
    {
        if (!table->rowState)
            table->rowState = new TableRowFilterIdentityState(table->model(),{},
                [table](int row) { return table->isRowHidden(row); },
                [table](int row) { return table->frozen.contains(table->model()->index(row,0)); },
                [table](int row,bool value) { table->setRowHidden(row,value); },[]() {},
                [table]() { QSet<int> rows; for (const auto& id : table->frozen) if (id.isValid()) rows.insert(id.row()); return rows; },table);
        return table->rowState.data();
    }
    static bool IsTableRowHiddenByLegacyFilter(FakeTable* table,int row)
    { return stateFor(table)->legacyExcludes(row); }
    static void SetTableSearchRowExcluded(FakeTable* table,int row,bool value)
    { stateFor(table)->setSearch(row,value); }
    static void ClearTableSearchRowFilter(FakeTable* table)
    { stateFor(table)->clearSearch(); }
    static bool isColumnHiddenByFreeze(FakeTable*,int) { return false; }
}
static bool tableNeedsSearchAccess(FakeTable*) { return true; }
class FilterHarness final : public QObject
{
public:
    explicit FilterHarness(FakeTable* table) : m_tableView(table) {}
    bool isGenericSearchEligible() const { return true; }
    void scheduleRefresh() {}
    void resume() { refreshSearchColumns(); scheduleFilterReapply(false); }
    bool pending() const { return m_filterRefreshPending || m_allRowsDirty || !m_dirtyRows.isEmpty(); }
    QString query() const { return m_resultFilterQuery; }
    bool active() const { return m_resultFilterActive; }
#include "table_filter_production.inc"
private:
    struct FilterRowState { quint64 key = 0; QPersistentModelIndex identity; bool baselineHidden = false; bool baselinePending = true; bool matches = true; };
    QPointer<FakeTable> m_tableView;
    QPointer<QAbstractItemModel> m_filterModel;
    QVector<FilterRowState> m_filterRows;
    QPersistentModelIndex m_filterRoot;
    QSet<quint64> m_dirtyRows;
    QHash<quint64, int> m_rowLookup;
    quint64 m_nextRowKey = 0;
    QVector<int> m_searchColumns;
    bool m_allRowsDirty = false;
    int m_filterCursor = 0;
    QString m_resultFilterQuery;
    bool m_resultFilterActive = false;
    bool m_modelMutationPrepared = false;
    bool m_keepSearchAccessVisible = false;
    bool m_filterRefreshPending = false;
    quint64 m_filterGeneration = 0;
};
static void drain(FilterHarness& filter)
{ for (int i = 0; i < 10000 && filter.pending(); ++i) QCoreApplication::processEvents(); check(!filter.pending(), "filter did not drain"); QCoreApplication::processEvents(); }
static void equivalent(Model& model, FakeTable& table, FilterHarness& filter, const QSet<int>& baseline)
{
    for (int row = 0; row < model.rows.size(); ++row)
    {
        bool matches = false;
        for (int col = 0; col < 2; ++col)
            if (!table.isColumnHidden(col) && model.rows.at(row).values.value(col).contains(filter.query(), Qt::CaseInsensitive)) matches = true;
        const bool expected = baseline.contains(model.rows.at(row).id) || (filter.active() && !matches)
            || table.business.contains(model.index(row,0)) || table.frozen.contains(model.index(row,0));
        check(table.isRowHidden(row) == expected, "incremental visibility differs from full reference/business baseline");
    }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Model model; for (int i = 0; i < 80; ++i) model.insertAt(i, {i % 2 ? QStringLiteral("alpha") : QStringLiteral("beta"), QStringLiteral("second")});
    FakeTable table(&model); FilterHarness filter(&table); QSet<int> baseline;
    for (int row = 0; row < 80; row += 7) { baseline.insert(model.rows.at(row).id); table.setRowHidden(row, true); }
    filter.applyResultFilter(QStringLiteral("alpha")); drain(filter); equivalent(model, table, filter, baseline);
    model.reads = table.visibilityWrites = 0; model.change(1, {}, Qt::UserRole); drain(filter);
    check(model.reads == 0 && table.visibilityWrites == 0, "non-display role caused filtering work");
    model.reads = 0; model.change(1, QStringLiteral("beta"), Qt::DisplayRole); drain(filter);
    check(model.reads <= 2, "single-row data change rescanned full table"); equivalent(model, table, filter, baseline);
    std::mt19937 random(10917);
    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        const int op = random() % 9;
        if (op == 0 || model.rows.isEmpty())
        {
            int row = random() % (model.rows.size() + 1);
            int id = model.insertAt(row, {random()%2 ? QStringLiteral("alpha") : QStringLiteral("beta"), QStringLiteral("tail")});
            if (random()%3 == 0) { baseline.insert(id); table.setRowHidden(row, true); } // after endInsertRows
        }
        else if (op == 1) { int row = random()%model.rows.size(); baseline.remove(model.rows.at(row).id); model.removeAt(row); }
        else if (op == 2) model.reverse();
        else if (op == 3) model.moveRowAt(random()%model.rows.size(), random()%(model.rows.size()+1));
        else if (op == 4) filter.applyResultFilter(random()%2 ? QStringLiteral("alpha") : QStringLiteral("missing"));
        else if (op == 5) model.change(random()%model.rows.size(), random()%2 ? QStringLiteral("alpha") : QStringLiteral("beta"), Qt::DisplayRole);
        else if (op == 6) { if (table.hiddenColumns.contains(0)) table.hiddenColumns.remove(0); else table.hiddenColumns.insert(0); filter.resume(); }
        else if (op == 7) { filter.clearResultFilter(); equivalent(model, table, filter, baseline); filter.applyResultFilter(QStringLiteral("alpha")); }
        else
        {
            model.reset(); baseline.clear();
            for (int row = 0; row < model.rows.size(); row += 3) { baseline.insert(model.rows.at(row).id); table.setRowHidden(row, true); }
        }
        drain(filter); equivalent(model, table, filter, baseline);
    }
    table.visible = false; model.change(1, QStringLiteral("hidden alpha"), Qt::DisplayRole);
    QCoreApplication::processEvents(); check(filter.pending(), "hidden table discarded dirty projection");
    table.visible = true; filter.resume(); drain(filter); equivalent(model, table, filter, baseline);
    const int id = model.insertAt(0, {QStringLiteral("alpha"), {}}); baseline.insert(id); table.setRowHidden(0, true);
    filter.clearResultFilter(); QCoreApplication::processEvents(); equivalent(model, table, filter, baseline);
    // Business filters use a separate exclusion layer even when the source row
    // is already hidden by a different query/freeze. Clearing query must not undo it.
    table.business.insert(model.index(1,0));
    ks::ui::stateFor(&table)->setBusiness(table.business);
    filter.applyResultFilter(QStringLiteral("alpha"));drain(filter);
    filter.clearResultFilter();equivalent(model,table,filter,baseline);
    check(table.isRowHidden(1),"clear query preserves explicit business filter");
    model.reverse();equivalent(model,table,filter,baseline);
    table.business.clear();ks::ui::stateFor(&table)->setBusiness({});
    equivalent(model,table,filter,baseline);
    filter.applyResultFilter(QStringLiteral("missing"));
    model.reset();baseline.clear();table.business.clear();
    baseline.insert(model.rows.at(2).id);table.setRowHidden(2,true);
    table.business.insert(model.index(4,0));ks::ui::stateFor(&table)->setBusiness(table.business);
    filter.clearResultFilter();QCoreApplication::processEvents();
    equivalent(model,table,filter,baseline);
    check(table.isRowHidden(2)&&table.isRowHidden(4),"immediate reset/clear preserves post-reset legacy and explicit business filters");
    std::printf("PASS: %d assertions; extracted production methods; 1000 randomized state transitions; no Widgets\n", checks);
}
