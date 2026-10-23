#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QString>
#include <QVector>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "ModelTextSearchCursor.h"

// This QObject host deliberately has no Widgets dependency. The runner extracts
// the production table cursor wrapper bodies verbatim into the included file.
class FakeTable final : public QObject
{
public:
    QPointer<QAbstractItemModel> currentModel;
    QAbstractItemModel* model() const { return currentModel.data(); }
    QModelIndex rootIndex() const { return {}; }
    bool isColumnHidden(int) const { return false; }
};
using QTableView = FakeTable;

class ValueModel final : public QAbstractTableModel
{
public:
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 5000; }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 1; }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    { return index.isValid() && role == Qt::DisplayRole ? QVariant(QStringLiteral("needle")) : QVariant(); }
};

namespace ks::i18n { QString sourceText(const QString& value) { return value; } }
namespace ks::ui
{
    struct TableCellSearchMatch
    {
        QPointer<QTableView> tableView;
        QPersistentModelIndex modelIndex;
        QString matchedText;
        QString locationText;
        int matchRank = 0;
    };
    QString ResolveTableSearchDisplayName(const QTableView*) { return QStringLiteral("test"); }
    bool isColumnHiddenByFreeze(const QTableView*, int) { return false; }
    bool IsTableCellExcludedByBusinessFilter(const QTableView*, const QModelIndex&) { return false; }
    quint64 TableRowFilterIdentityRevision(const QTableView*) { return 0; }
    int normalizedMatchRank(const QString&, const QString&) { return 0; }
#include "table_search_wrapper_actual.inc"
}

namespace
{
    int checks = 0;
    void check(bool condition, const char* message)
    {
        ++checks;
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    }
    void finish(const std::shared_ptr<ks::ui::TableCellSearchCursor>& scan)
    {
        for (int attempt = 0; attempt < 10000; ++attempt)
            if (ks::ui::ContinueTableCellSearch(scan)) return;
        check(false, "wrapper search did not terminate");
    }
    void empty(const std::shared_ptr<ks::ui::TableCellSearchCursor>& scan)
    { check(ks::ui::CompletedTableCellSearchMatches(scan).isEmpty(), "obsolete wrapper returned results"); }
    void stale(const std::shared_ptr<ks::ui::TableCellSearchCursor>& scan)
    {
#ifdef HAS_TABLE_SEARCH_CURRENT
        check(!ks::ui::IsTableCellSearchCurrent(scan), "obsolete wrapper remained current");
#endif
        check(ks::ui::ContinueTableCellSearch(scan), "obsolete wrapper remained pending");
        check(scan->cancelled, "obsolete pending wrapper was not cancelled");
        empty(scan);
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace ks::ui;
    ValueModel stable;
    FakeTable table;
    table.currentModel = &stable;
    auto valid = BeginTableCellSearch(&table, QStringLiteral("needle"), 2);
    finish(valid);
    check(!valid->cancelled && CompletedTableCellSearchMatches(valid).size() == 2, "valid wrapper lost hits");
#ifdef HAS_TABLE_SEARCH_CURRENT
    check(IsTableCellSearchCurrent(valid), "valid completed wrapper not current");
#endif
    for (int transition = 0; transition < 3; ++transition)
    {
        auto model = std::make_unique<ValueModel>();
        table.currentModel = model.get();
        auto pending = BeginTableCellSearch(&table, QStringLiteral("missing"), 2);
        if (transition == 0) model.reset();
        else if (transition == 1) table.currentModel = nullptr;
        else table.currentModel = &stable;
        stale(pending);
    }
    for (int transition = 0; transition < 3; ++transition)
    {
        auto model = std::make_unique<ValueModel>();
        table.currentModel = model.get();
        auto completed = BeginTableCellSearch(&table, QStringLiteral("needle"), 2);
        finish(completed);
        if (transition == 0) model.reset();
        else if (transition == 1) table.currentModel = nullptr;
        else table.currentModel = &stable;
        empty(completed);
#ifdef HAS_TABLE_SEARCH_CURRENT
        check(!IsTableCellSearchCurrent(completed), "destroyed/detached/replaced completed model stayed current");
#endif
    }
    auto dyingTable = std::make_unique<FakeTable>();
    dyingTable->currentModel = &stable;
    auto deadHost = BeginTableCellSearch(dyingTable.get(), QStringLiteral("missing"), 2);
    dyingTable.reset();
    stale(deadHost);
    auto noScan = std::make_shared<TableCellSearchCursor>();
    noScan->table = &table;
    table.currentModel = &stable;
    stale(noScan);
    empty(BeginTableCellSearch(nullptr, QStringLiteral("needle"), 2));
    table.currentModel = nullptr;
    empty(BeginTableCellSearch(&table, QStringLiteral("needle"), 2));
    empty({});
    check(ContinueTableCellSearch({}), "null wrapper remained pending");
    std::printf("PASS %d actual-wrapper assertions; no Widgets\n", checks);
    return 0;
}
