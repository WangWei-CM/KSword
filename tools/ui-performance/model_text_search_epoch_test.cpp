#include "../../Ksword5.1/Ksword5.1/UI/ModelTextSearchCursor.h"
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QStringList>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

class TreeModel final : public QAbstractItemModel
{
public:
    struct Node { Node* parent; QStringList values; std::vector<std::unique_ptr<Node>> children; };
    TreeModel() : root{nullptr, {}, {}} {}
    Node root;
    mutable int reads = 0;
    Node* node(const QModelIndex& index) const { return index.isValid() ? static_cast<Node*>(index.internalPointer()) : const_cast<Node*>(&root); }
    int row(Node* item) const
    {
        if (item == &root || item->parent == nullptr) return -1;
        const auto& siblings = item->parent->children;
        for (int i = 0; i < static_cast<int>(siblings.size()); ++i) if (siblings[i].get() == item) return i;
        return -1;
    }
    QModelIndex index(int rowValue, int column, const QModelIndex& parent = {}) const override
    {
        Node* owner = node(parent);
        if (rowValue < 0 || rowValue >= static_cast<int>(owner->children.size()) || column < 0 || column >= 3) return {};
        return createIndex(rowValue, column, owner->children[rowValue].get());
    }
    QModelIndex parent(const QModelIndex& indexValue) const override
    {
        if (!indexValue.isValid()) return {};
        Node* owner = node(indexValue)->parent;
        return owner == &root ? QModelIndex() : createIndex(row(owner), 0, owner);
    }
    int rowCount(const QModelIndex& parent = {}) const override
    { return parent.isValid() && parent.column() != 0 ? 0 : static_cast<int>(node(parent)->children.size()); }
    int columnCount(const QModelIndex& = {}) const override { return 3; }
    QVariant data(const QModelIndex& indexValue, int role = Qt::DisplayRole) const override
    {
        if (!indexValue.isValid() || role != Qt::DisplayRole) return {};
        ++reads;
        return node(indexValue)->values.value(indexValue.column());
    }
    Node* append(QStringList values, Node* owner = nullptr)
    {
        if (owner == nullptr) owner = &root;
        const QModelIndex ownerIndex = owner == &root ? QModelIndex() : createIndex(row(owner), 0, owner);
        const int at = static_cast<int>(owner->children.size());
        beginInsertRows(ownerIndex, at, at);
        auto child = std::make_unique<Node>(); child->parent = owner; child->values = std::move(values);
        Node* result = child.get(); owner->children.push_back(std::move(child));
        endInsertRows();
        return result;
    }
    void change(int rowValue, QString value, int role)
    {
        if (role == Qt::DisplayRole) root.children.at(rowValue)->values[0] = std::move(value);
        emit dataChanged(index(rowValue, 0), index(rowValue, 0), {role});
    }
    void clear()
    {
        beginResetModel(); root.children.clear(); endResetModel();
    }
};

static int checks = 0;
static void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void finish(ks::ui::ModelTextSearchCursor& cursor, int units = 7)
{
    for (int iteration = 0; iteration < 200000; ++iteration)
    {
        const auto status = cursor.advance(1000, units);
        if (status == ks::ui::ModelTextSearchCursor::Status::Complete) return;
        check(status == ks::ui::ModelTextSearchCursor::Status::Pending, "unexpected cancel");
    }
    check(false, "scan did not terminate");
}
static void compare(TreeModel& model, QString query, QVector<int> columns, int limit)
{
    QModelIndexList expected;
    for (int column : columns)
    {
        if (expected.size() >= limit) break;
        expected += model.match(model.index(0, column), Qt::DisplayRole, query,
            limit - expected.size(), Qt::MatchContains | Qt::MatchRecursive);
    }
    ks::ui::ModelTextSearchCursor cursor(&model, query, columns, limit);
    finish(cursor, 1);
    check(cursor.matches().size() == expected.size(), "match count differs from Qt");
    for (int i = 0; i < expected.size(); ++i)
    {
        check(cursor.matches().at(i).index == expected.at(i), "DFS/column result order differs from Qt");
        check(cursor.matches().at(i).text == expected.at(i).data().toString(), "display text changed");
    }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TreeModel model;
    for (int row = 0; row < 10000; ++row)
        model.append({QStringLiteral("row %1").arg(row), QStringLiteral("BETA"), QStringLiteral("搜索 %1").arg(row)});
    model.reads = 0;
    ks::ui::ModelTextSearchCursor noHit(&model, QStringLiteral("NEVER"), {0, 1, 2}, 50);
    check(noHit.advance(1000, 7) == ks::ui::ModelTextSearchCursor::Status::Pending, "no-hit scan must yield");
    check(model.reads <= 7, "work budget exceeded");
    finish(noHit);
    check(model.reads == 30000 && noHit.matches().isEmpty(), "no-hit scan silently truncated or repeated");
    compare(model, QStringLiteral("row 9999"), {0, 1, 2}, 50);
    compare(model, QStringLiteral("beta"), {2, 1}, 3);
    compare(model, QStringLiteral("搜索"), {2}, 7);
    TreeModel tree;
    auto* a = tree.append({QStringLiteral("match A"), QStringLiteral("top"), QStringLiteral("match 2")});
    auto* b = tree.append({QStringLiteral("match B"), QStringLiteral("second"), QStringLiteral("none")}, a);
    tree.append({QStringLiteral("match C"), QStringLiteral("third"), QStringLiteral("match child")}, b);
    tree.append({QStringLiteral("match D"), QStringLiteral("next"), QStringLiteral("match final")});
    compare(tree, QStringLiteral("match"), {0, 2}, 50);
    compare(tree, QStringLiteral("match"), {2, 0}, 3);
    ks::ui::ModelTextSearchCursor changed(&model, QStringLiteral("new match"), {0}, 50);
    changed.advance(1000, 10);
    model.append({QStringLiteral("new match tail"), {}, {}});
    model.change(0, QStringLiteral("new match head"), Qt::DisplayRole);
    finish(changed);
    check(changed.matches().size() == 2, "insert/display revision restart lost results");
    check(changed.matches().first().index.row() == 0 && changed.matches().last().index.row() == 10000,
        "restart order differs");
    ks::ui::ModelTextSearchCursor reset(&tree, QStringLiteral("replacement"), {0}, 10);
    reset.advance(1000, 2); tree.clear(); tree.append({QStringLiteral("replacement"), {}, {}}); finish(reset);
    check(reset.matches().size() == 1 && reset.matches().first().index.isValid(), "reset retained stale index");
    reset.setColumns({1}); finish(reset); check(reset.matches().isEmpty(), "column change did not restart");
    int firstAccepted = 9997;
    model.reads = 0;
    ks::ui::ModelTextSearchCursor filtered(&model, QStringLiteral("beta"), {1}, 3,
        [&](const QModelIndex& index) { return index.row() >= firstAccepted; });
    finish(filtered);
    check(filtered.matches().size() == 3 && filtered.matches().first().index.row() == 9997,
        "business exclusions consume work budget but never the accepted hit cap");
    check(model.reads == 3, "excluded rows do not fetch DisplayRole strings");
    firstAccepted = 9999; filtered.invalidate(); finish(filtered);
    check(filtered.matches().size() == 1 && filtered.matches().first().index.row() == 9999,
        "external filter identity revision restarts completed cursor");
    TreeModel filteredTree;
    auto* hiddenParent = filteredTree.append({QStringLiteral("match hidden"),{}, {}});
    filteredTree.append({QStringLiteral("match hidden child"),{}, {}},hiddenParent);
    filteredTree.append({QStringLiteral("match visible"),{}, {}});
    ks::ui::ModelTextSearchCursor branchFilter(&filteredTree,QStringLiteral("match"),{0},5,
        [](const QModelIndex& index) { return index.parent().isValid() || index.row()!=0; });
    finish(branchFilter);
    check(branchFilter.matches().size()==1 && branchFilter.matches().first().index.row()==1,
        "business-excluded root never leaks descendant matches");
    const QModelIndex scopeRoot=filteredTree.index(0,0);
    ks::ui::ModelTextSearchCursor rooted(&filteredTree,QStringLiteral("match"),{0},5,{},scopeRoot);
    finish(rooted);
    check(rooted.matches().size()==1 && rooted.matches().first().index.parent()==scopeRoot,
        "table-root search scans only descendants of the current view root");
    rooted.setRoot({});finish(rooted);
    check(rooted.matches().size()==3,"changing view root restarts complete matching");
    ks::ui::ModelTextSearchCursor epoch(&model,QStringLiteral("row"),{0},4);
    finish(epoch);check(epoch.isCurrent({0},{}),"completed table cursor retains its input epoch");
    model.change(1,{},Qt::UserRole);check(epoch.isCurrent({0},{}),"unrelated roles do not invalidate a finished table epoch");
    model.change(1,QStringLiteral("row changed"),Qt::DisplayRole);
    check(!epoch.isCurrent({0},{}),"earlier completed table invalidates while a later table scans");
    finish(epoch);check(epoch.isCurrent({0},{}),"rescanned table completes at newest epoch");
    check(!epoch.isCurrent({1},{}),"visible column changes invalidate a finished epoch");
    check(!epoch.isCurrent({0},filteredTree.index(0,0)),"table view root changes invalidate a finished epoch");
    auto dying = std::make_unique<TreeModel>();
    ks::ui::ModelTextSearchCursor destroyed(dying.get(), QStringLiteral("x"), {0}, 10);
    dying.reset();
    check(destroyed.advance() == ks::ui::ModelTextSearchCursor::Status::Cancelled, "destroyed model not cancelled");
    std::printf("PASS: %d assertions; QCoreApplication only; 30000-cell no-hit scan complete\n", checks);
}
