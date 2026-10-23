#include "FlatTableModel.h"

#include <QCoreApplication>
#include <QItemSelectionModel>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>

#include <iostream>
#include <random>
#include <tuple>
#include <unordered_map>

// Translation is unrelated to model identity/notifications; this QtCore-only executable
// supplies the unchanged translation boundary without linking product GUI initialization.
namespace ks::i18n
{
    struct LanguageManager::State {};
    LanguageManager::LanguageManager() = default;
    LanguageManager::~LanguageManager() = default;
    LanguageManager& LanguageManager::instance() { static LanguageManager manager; return manager; }
    QString LanguageManager::sourceText(const QString& value) const { return value; }
    QString LanguageManager::displayText(const QString& value) const { return value; }
    bool LanguageManager::eventFilter(QObject* object, QEvent* event) { return QObject::eventFilter(object, event); }
}

struct Row { int key; QString name; int value; };
using Model = ks::ui::FlatTableModel<Row>;
struct Signal { int firstRow, lastRow, firstColumn, lastColumn; QList<int> roles; };
int failures = 0;
int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; } } while (false)

std::vector<Model::ColumnSpec> columns() { return {{QStringLiteral("name")}, {QStringLiteral("value")}}; }
QVariant cell(const Row& row, int column, int role)
{
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
    return column == 0 ? QVariant(row.name) : QVariant(row.value);
}
std::string key(const Row& row) { return std::to_string(row.key); }
std::vector<Model::ColumnChange> delta(const Row& oldRow, const Row& newRow)
{
    std::vector<Model::ColumnChange> result;
    if (oldRow.name != newRow.name) result.push_back({0, 0, {Qt::ToolTipRole, Qt::DisplayRole, Qt::DisplayRole}});
    if (oldRow.value != newRow.value) result.push_back({1, 1, {Qt::DisplayRole}});
    return result;
}
void observe(Model& model, std::vector<Signal>& changes, QObject& owner)
{
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &owner,
        [&changes](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles)
        { changes.push_back({first.row(), last.row(), first.column(), last.column(), roles}); });
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<Row> original={{1, QStringLiteral("a"), 10}, {2, QStringLiteral("b"), 20}, {3, QStringLiteral("c"), 30}};
    {
        Model legacy(columns(), cell, nullptr, {}, key);
        std::vector<Signal> changes; QObject owner; observe(legacy, changes, owner);
        auto stats=legacy.setRows(original);
        CHECK(stats.insertedRowCount==3 && stats.updatedRowCount==3 && !stats.modelReset);
        CHECK(changes.size()==1 && changes[0].roles.empty());
        changes.clear();stats=legacy.setRows(original);
        CHECK(changes.size()==1 && stats.updatedRowCount==3);
        legacy.setRowChangeResolver(delta);changes.clear();stats=legacy.setRows(original);
        CHECK(changes.empty() && stats.updatedRowCount==0);
        legacy.setRowChangeResolver({});changes.clear();legacy.setRows(original);
        CHECK(changes.size()==1 && changes[0].roles.empty());
    }
    {
        Model noKeys(columns(), cell, nullptr, {}, {}, delta);
        auto stats=noKeys.setRows(original);
        CHECK(stats.modelReset && stats.updatedRowCount==3);
        stats=noKeys.setRows(original);CHECK(stats.modelReset);
    }
    {
        Model model(columns(), cell, nullptr, {}, key, delta);
        std::vector<Signal> changes;QObject owner;observe(model, changes, owner);
        int inserts=0, removes=0, layouts=0, resets=0;
        QObject::connect(&model,&QAbstractItemModel::rowsInserted,&owner,[&](const QModelIndex&,int first,int last){ inserts+=last-first+1; CHECK(model.rowCount()>=last+1); });
        QObject::connect(&model,&QAbstractItemModel::rowsRemoved,&owner,[&](const QModelIndex&,int first,int last){removes+=last-first+1;});
        QObject::connect(&model,&QAbstractItemModel::layoutChanged,&owner,[&](){++layouts;});
        QObject::connect(&model,&QAbstractItemModel::modelReset,&owner,[&](){++resets;});
        auto stats=model.setRows(original);
        CHECK(inserts==3 && changes.empty() && stats.updatedRowCount==0);
        stats=model.setRows(original);CHECK(changes.empty() && layouts==0 && resets==0);
        CHECK(stats.updatedRowCount==0 && stats.insertedRowCount==0 && stats.removedRowCount==0);
        auto next=original;next[0].name=QStringLiteral("new");stats=model.setRows(next);
        CHECK(stats.updatedRowCount==1 && changes.size()==1);
        CHECK(changes[0].firstRow==0 && changes[0].lastRow==0 && changes[0].firstColumn==0 && changes[0].lastColumn==0);
        CHECK(changes[0].roles==QList<int>({Qt::DisplayRole,Qt::ToolTipRole}));
        CHECK(model.data(model.index(0,0),Qt::DisplayRole).toString()==QStringLiteral("new"));
        changes.clear();next[0].value=11;next[1].value=21;stats=model.setRows(next);
        CHECK(stats.updatedRowCount==2 && changes.size()==1);
        CHECK(changes[0].firstRow==0 && changes[0].lastRow==1 && changes[0].firstColumn==1 && changes[0].lastColumn==1);
        CHECK(changes[0].roles==QList<int>({Qt::DisplayRole}));
        changes.clear();next[0].value=12;next[2].value=31;model.setRows(next);
        CHECK(changes.size()==2 && changes[0].firstRow==0 && changes[1].firstRow==2);

        QPersistentModelIndex retained(model.index(1,0));
        QPersistentModelIndex removed(model.index(0,0));
        QItemSelectionModel selection(&model);selection.select(retained,QItemSelectionModel::Select);
        QSortFilterProxyModel proxy;proxy.setSourceModel(&model);proxy.setSortRole(Qt::DisplayRole);proxy.sort(1);
        changes.clear();inserts=removes=layouts=0;
        std::vector<Row> moved={{3,QStringLiteral("c"),31},{4,QStringLiteral("d"),40},{2,QStringLiteral("b2"),22}};
        stats=model.setRows(moved);
        CHECK(inserts==1 && removes==1 && layouts==1 && !stats.modelReset);
        CHECK(stats.updatedRowCount==1 && stats.insertedRowCount==1 && stats.removedRowCount==1 && stats.orderChanged);
        CHECK(!removed.isValid() && retained.isValid() && retained.row()==2);
        CHECK(retained.data().toString()==QStringLiteral("b2"));
        CHECK(selection.selectedIndexes().size()==1 && selection.selectedIndexes()[0].row()==2);
        CHECK(changes.size()==2 && changes[0].firstRow==2 && changes[1].firstRow==2);
        CHECK(proxy.rowCount()==3 && proxy.index(0,1).data().toInt()==22);
        // A moved retained row must still compare against its original key, not its old/new position.
        changes.clear();std::swap(moved[0],moved[2]);model.setRows(moved);
        CHECK(changes.empty() && retained.row()==0 && retained.data().toString()==QStringLiteral("b2"));
        CHECK(proxy.index(0,0).data().toString()==QStringLiteral("b2"));
        changes.clear();stats=model.setRows({});
        CHECK(model.rowCount()==0 && stats.removedRowCount==3 && stats.updatedRowCount==0 && changes.empty());
        CHECK(!retained.isValid() && selection.selectedIndexes().empty());
    }
    {
        Model model(columns(),cell,nullptr,{},key,delta);QObject owner;std::vector<Signal> changes;observe(model,changes,owner);
        model.setRows(original);
        model.setRowChangeResolver([](const Row&,const Row&){return std::vector<Model::ColumnChange>{{0,1,{Qt::ToolTipRole}},{0,0,{Qt::DisplayRole}}};});
        model.setRows(original);
        CHECK(changes.size()==2 && changes[0].firstRow==0 && changes[0].lastRow==2);
        CHECK(changes[0].firstColumn==0 && changes[0].roles==QList<int>({Qt::DisplayRole,Qt::ToolTipRole}));
        CHECK(changes[1].firstColumn==1 && changes[1].roles==QList<int>({Qt::ToolTipRole}));
        changes.clear();model.setRowChangeResolver([](const Row&,const Row&){return std::vector<Model::ColumnChange>{{0,1,{}},{1,1,{Qt::DisplayRole}}};});
        model.setRows(original);CHECK(changes.size()==1 && changes[0].firstColumn==0 && changes[0].lastColumn==1 && changes[0].roles.empty());
#ifdef QT_NO_DEBUG
        for (const Model::ColumnChange bad : {Model::ColumnChange{-1,0,{Qt::DisplayRole}},Model::ColumnChange{0,5,{}},Model::ColumnChange{1,0,{}},Model::ColumnChange{0,0,{-1}}})
        {
            changes.clear();model.setRowChangeResolver([bad](const Row&,const Row&){return std::vector<Model::ColumnChange>{bad};});
            model.setRows(original);CHECK(changes.size()==1 && changes[0].firstColumn==0 && changes[0].lastColumn==1 && changes[0].roles.empty());
        }
#endif
        changes.clear();int resets=0;QObject::connect(&model,&QAbstractItemModel::modelReset,&owner,[&](){++resets;});
        QPersistentModelIndex persistent(model.index(0,0));
        auto duplicate=original;duplicate[1].key=duplicate[0].key;auto stats=model.setRows(duplicate);
        CHECK(stats.modelReset && resets==1 && changes.empty() && !persistent.isValid());
        stats=model.setRows(original);CHECK(stats.modelReset && resets==2); // old duplicates also require reset
        model.clearRows();CHECK(model.rowCount()==0 && resets==3);
    }
    {
        // Exercise final-row deltas while rows are simultaneously removed, inserted and moved.
        // The oracle is keyed by identity and never uses the model's implementation helpers.
        Model model(columns(),cell,nullptr,{},key,delta);QObject owner;std::vector<Signal> changes;observe(model,changes,owner);
        std::vector<Row> current=original;model.setRows(current);
        std::mt19937 random(0x5a1701);
        int nextKey=4;
        for (int iteration=0;iteration<200;++iteration)
        {
            std::unordered_map<int,Row> previous;
            std::unordered_map<int,QPersistentModelIndex> indexes;
            for (int row=0;row<static_cast<int>(current.size());++row)
            {
                previous.emplace(current[row].key,current[row]);indexes.emplace(current[row].key,model.index(row,0));
            }
            std::vector<Row> next=current;
            if (!next.empty() && (random()%3)!=0) next.erase(next.begin()+random()%next.size());
            if (next.size()<20 && (random()%3)!=0) next.push_back({nextKey++,QStringLiteral("new"),int(random()%100)});
            for (Row& row:next)
            {
                if ((random()%5)==0) row.name=QString::number(random()%100);
                if ((random()%3)==0) row.value=int(random()%100);
            }
            std::shuffle(next.begin(),next.end(),random);changes.clear();
            const auto stats=model.setRows(next);
            CHECK(!stats.modelReset && model.rowCount()==static_cast<int>(next.size()));
            std::vector<std::vector<bool>> notified(next.size(),std::vector<bool>(2,false));
            for (const Signal& change:changes)
            {
                CHECK(change.firstRow>=0 && change.lastRow<static_cast<int>(next.size()) && change.firstRow<=change.lastRow);
                CHECK(change.firstColumn>=0 && change.lastColumn<2 && change.firstColumn<=change.lastColumn);
                for (int row=change.firstRow;row<=change.lastRow;++row)
                    for (int column=change.firstColumn;column<=change.lastColumn;++column)
                    {
                        CHECK(!notified[row][column]);notified[row][column]=true;
                        CHECK(change.roles==(column==0 ? QList<int>({Qt::DisplayRole,Qt::ToolTipRole}) : QList<int>({Qt::DisplayRole})));
                    }
            }
            int expectedUpdated=0;
            for (int row=0;row<static_cast<int>(next.size());++row)
            {
                const auto old=previous.find(next[row].key);
                const bool nameChanged=old!=previous.end() && old->second.name!=next[row].name;
                const bool valueChanged=old!=previous.end() && old->second.value!=next[row].value;
                CHECK(notified[row][0]==nameChanged && notified[row][1]==valueChanged);
                if(nameChanged || valueChanged) ++expectedUpdated;
                CHECK(model.rowAt(row)->key==next[row].key);
                if(old!=previous.end()) CHECK(indexes.at(next[row].key).isValid() && indexes.at(next[row].key).row()==row);
            }
            CHECK(stats.updatedRowCount==expectedUpdated);
            for(const auto& index:indexes)
            {
                const bool retained=std::any_of(next.cbegin(),next.cend(),[&index](const Row& row){return row.key==index.first;});
                CHECK(index.second.isValid()==retained);
            }
            current=std::move(next);
        }
    }
    {
        Model model(columns(),cell,nullptr,{},key,delta);QObject owner;std::vector<Signal> changes;observe(model,changes,owner);
        std::vector<Row> many;many.reserve(10000);
        for(int row=0;row<10000;++row) many.push_back({row,QStringLiteral("same"),row});
        model.setRows(many);CHECK(changes.empty());
        model.setRows(many);CHECK(changes.empty());
        for(Row& row:many) ++row.value;
        const auto stats=model.setRows(many);
        CHECK(stats.updatedRowCount==10000 && changes.size()==1);
        CHECK(changes[0].firstRow==0 && changes[0].lastRow==9999 && changes[0].firstColumn==1 && changes[0].lastColumn==1);
    }
    // Synchronous observer replacements never nest Qt structural pairs.
    for(int boundary=0;boundary<9;++boundary)
    {
        Model model(columns(),cell,nullptr,{},key,delta);model.setRows(original);QObject owner;
        bool entered=false;int depth=0,maxDepth=0;
        const std::vector<Row> replacement={{91,QStringLiteral("latest"),91},{92,QStringLiteral("latest"),92}};
        const auto nested=[&]{if(entered)return;entered=true;model.setRows({{90,QStringLiteral("superseded"),90}});model.setRows(replacement);};
        QObject::connect(&model,&QAbstractItemModel::rowsAboutToBeRemoved,&owner,[&]{maxDepth=std::max(maxDepth,++depth);});
        QObject::connect(&model,&QAbstractItemModel::rowsRemoved,&owner,[&]{--depth;});
        QObject::connect(&model,&QAbstractItemModel::rowsAboutToBeInserted,&owner,[&]{maxDepth=std::max(maxDepth,++depth);});
        QObject::connect(&model,&QAbstractItemModel::rowsInserted,&owner,[&]{--depth;});
        QObject::connect(&model,&QAbstractItemModel::modelAboutToBeReset,&owner,[&]{maxDepth=std::max(maxDepth,++depth);});
        QObject::connect(&model,&QAbstractItemModel::modelReset,&owner,[&]{--depth;});
        QObject::connect(&model,&QAbstractItemModel::layoutAboutToBeChanged,&owner,[&]{maxDepth=std::max(maxDepth,++depth);});
        QObject::connect(&model,&QAbstractItemModel::layoutChanged,&owner,[&]{--depth;});
        if(boundary==0)QObject::connect(&model,&QAbstractItemModel::rowsAboutToBeRemoved,&owner,nested);
        if(boundary==1)QObject::connect(&model,&QAbstractItemModel::rowsRemoved,&owner,nested);
        if(boundary==2)QObject::connect(&model,&QAbstractItemModel::rowsAboutToBeInserted,&owner,nested);
        if(boundary==3)QObject::connect(&model,&QAbstractItemModel::rowsInserted,&owner,nested);
        if(boundary==4)QObject::connect(&model,&QAbstractItemModel::layoutAboutToBeChanged,&owner,nested);
        if(boundary==5)QObject::connect(&model,&QAbstractItemModel::layoutChanged,&owner,nested);
        if(boundary==6)QObject::connect(&model,&QAbstractItemModel::modelAboutToBeReset,&owner,nested);
        if(boundary==7)QObject::connect(&model,&QAbstractItemModel::modelReset,&owner,nested);
        if(boundary==8)QObject::connect(&model,&QAbstractItemModel::dataChanged,&owner,nested);
        if(boundary<=1)model.setRows({original[0]});
        else if(boundary<=3)model.setRows({original[0],original[1],original[2],{4,QStringLiteral("insert"),4}});
        else if(boundary<=5)model.setRows({original[2],original[0],original[1]});
        else if(boundary<=7)model.clearRows();
        else{auto next=original;next[0].name=QStringLiteral("changed");next[2].value=45;model.setRows(next);}
        CHECK(entered&&depth==0&&maxDepth<=1);CHECK(model.rowCount()==2);CHECK(model.rowAt(0)->key==91&&model.rowAt(1)->key==92);
    }
    {
        Model model(columns(),cell,nullptr,{},key,delta);model.setRows(original);QObject owner;bool once=false;
        QObject::connect(&model,&QAbstractItemModel::dataChanged,&owner,[&]{if(!once){once=true;model.clearRows();}});
        auto next=original;next[0].name=QStringLiteral("new");next[2].value=31;model.setRows(next);
        CHECK(model.rowCount()==0);model.setRows(original);CHECK(model.rowCount()==3);
    }
    // Callable ownership and model lifetime at user resolver boundaries.
    for(int resolverKind=0;resolverKind<2;++resolverKind)
    {
        Model* model=new Model(columns(),cell,nullptr,{},key,delta);model->setRows(original);bool once=false;
        if(resolverKind==0)model->setRowChangeResolver([&](const Row&a,const Row&b){if(!once){once=true;model->setRows({{9,QStringLiteral("resolver latest"),9}});}return delta(a,b);});
        else model->setRowChangeResolver([&](const Row&,const Row&){delete model;model=nullptr;return std::vector<Model::ColumnChange>{{0,0,{Qt::DisplayRole}}};});
        auto next=original;next[0].name=QStringLiteral("new");model->setRows(next);
        if(resolverKind==0){CHECK(once&&model->rowCount()==1&&model->rowAt(0)->key==9);delete model;}
        else CHECK(model==nullptr);
    }
    for(int replacementKind=0;replacementKind<2;++replacementKind)
    {
        Model* model=nullptr;bool active=false,once=false;
        Model::KeyResolver callback=[&](const Row&r){if(active&&!once){once=true;if(replacementKind==0)model->setRows({{8,QStringLiteral("key latest"),8}});else{delete model;model=nullptr;}}return key(r);};
        model=new Model(columns(),cell,nullptr,{},callback,delta);model->setRows(original);active=true;model->setRows(original);
        if(replacementKind==0){CHECK(model->rowCount()==1&&model->rowAt(0)->key==8);delete model;}else CHECK(model==nullptr);
    }
    {
        Model* model=new Model(columns(),cell,nullptr,{},key,delta);model->setRows(original);QObject owner;int signalCount=0;
        QObject::connect(model,&QAbstractItemModel::dataChanged,&owner,[&]{++signalCount;delete model;model=nullptr;});
        auto next=original;next[0].name=QStringLiteral("new");next[2].value=45;model->setRows(next);CHECK(model==nullptr&&signalCount==1);
    }
    // Layout observers may create persistent indexes which must also remap.
    {
        Model model(columns(),cell,nullptr,{},key,delta);model.setRows(original);QObject owner;QPersistentModelIndex late;
        QObject::connect(&model,&QAbstractItemModel::layoutAboutToBeChanged,&owner,[&]{late=model.index(0,0);});
        model.setRows({original[2],original[0],original[1]});CHECK(late.isValid()&&late.row()==1&&late.data().toString()==QStringLiteral("a"));
    }
    std::cout << "checks=" << checks << " failures=" << failures << "\n";
    return failures==0 ? 0 : 1;
}
