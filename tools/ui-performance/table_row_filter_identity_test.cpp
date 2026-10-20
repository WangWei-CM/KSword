#include "../../Ksword5.1/Ksword5.1/UI/TableRowFilterIdentityState.h"
#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QVector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>

static int checks = 0;
static void check(bool condition, const char* message)
{ ++checks; if (!condition) { std::fprintf(stderr, "FAIL %s at %d\n",message,checks); std::exit(1); } }
class Model final : public QAbstractTableModel
{
public:
    QVector<int> ids;
    int next = 0, columns = 3;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(ids.size()); }
    int columnCount(const QModelIndex& = {}) const override { return columns; }
    void removeFirstColumn() { beginRemoveColumns({},0,0);--columns;endRemoveColumns(); }
    void insertFirstColumn() { beginInsertColumns({},0,0);++columns;endInsertColumns(); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    { return index.isValid() && role == Qt::DisplayRole ? QVariant(ids.at(index.row())) : QVariant(); }
    void insertAt(int at) { beginInsertRows({},at,at); ids.insert(at,++next); endInsertRows(); }
    void removeAt(int at) { beginRemoveRows({},at,at); ids.removeAt(at); endRemoveRows(); }
    void resetRows(int count) { beginResetModel(); ids.clear(); while (count--) ids.push_back(++next); endResetModel(); }
    void reverseRows()
    {
        emit layoutAboutToBeChanged();
        const auto old = persistentIndexList();
        QVector<int> identity;
        for (const auto& index : old) identity.push_back(ids.at(index.row()));
        std::reverse(ids.begin(),ids.end());
        QModelIndexList updated;
        for (int i=0;i<old.size();++i) updated.push_back(index(ids.indexOf(identity.at(i)),old.at(i).column()));
        changePersistentIndexList(old,updated);
        emit layoutChanged();
    }
};
struct Host : QObject
{
    Model* model;
    QVector<bool> hidden;
    QSet<QPersistentModelIndex> frozen;
    int writes = 0, notifications = 0;
    explicit Host(Model* value) : model(value),hidden(value->rowCount(),false)
    {
        connect(model,&QAbstractItemModel::rowsInserted,this,[this](const QModelIndex&,int first,int last) {
            for (int row=first;row<=last;++row) hidden.insert(row,false);
        });
        connect(model,&QAbstractItemModel::rowsRemoved,this,[this](const QModelIndex&,int first,int last) {
            hidden.remove(first,last-first+1);
        });
        connect(model,&QAbstractItemModel::modelReset,this,[this]() { hidden.resize(model->rowCount()); });
    }
    bool isFrozen(int row) const { return frozen.contains(QPersistentModelIndex(model->index(row,0))); }
};
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    Model model; model.resetRows(120);
    Host host(&model);
    host.hidden[3]=true; // unmanaged legacy filter, retained through all layers
    host.frozen.insert(model.index(5,0)); host.hidden[5]=true;
    ks::ui::TableRowFilterIdentityState state(&model,{},
        [&](int row) { return host.hidden.at(row); },
        [&](int row) { return host.isFrozen(row); },
        [&](int row,bool hidden) { ++host.writes; host.hidden[row]=hidden; },
        [&]() { ++host.notifications; }, [&]() {
            QSet<int> rows; for (const auto& id : host.frozen) if (id.isValid()) rows.insert(id.row());
            return rows;
        });
    QSet<QPersistentModelIndex> excluded{model.index(5,0),model.index(7,0)};
    state.setBusiness(excluded);
    check(state.businessExcludes(model.index(5,2)),"normalize first-column business identity");
    check(state.excludes(5)&&state.excludes(7),"freeze cannot bypass business exclusion");
    const int originalWrites=host.writes;
    for(int i=0;i<100;++i) state.setBusiness(excluded);
    check(host.writes==originalWrites,"unchanged filter performs no row visibility writes");
    state.setSearch(9,true); state.setSearch(7,true); state.clearSearch();
    check(host.hidden[5]&&host.hidden[7]&&!host.hidden[9],"clear search preserves business/freeze");
    state.setBusiness({});
    check(host.hidden[3]&&host.hidden[5]&&!host.hidden[7],"clear business preserves legacy/freeze");
    // A hidden bit is logical-header state. Layout sorting deliberately leaves it
    // at its old position; the ledger must reproject persistent identities.
    state.setBusiness({model.index(7,0)});
    const int excludedId=model.ids.at(7), legacyId=model.ids.at(3);
    model.reverseRows();
    check(host.hidden[model.ids.indexOf(excludedId)],"sort follows business row");
    check(host.hidden[model.ids.indexOf(legacyId)],"sort follows legacy row");
    check(!host.hidden[7],"sort releases stale old hidden section");
    QCoreApplication::processEvents();
    check(host.notifications==1,"same-turn notifications coalesce");
    std::mt19937 random(92791);
    QSet<int> businessIds,searchIds;
    for(int round=0;round<250;++round)
    {
        businessIds.clear();searchIds.clear();excluded.clear();
        for(int row=0;row<model.rowCount();++row)
        {
            if(random()%5==0) { excluded.insert(model.index(row,0));businessIds.insert(model.ids.at(row)); }
        }
        state.setBusiness(excluded);state.clearSearch();
        for(int row=0;row<model.rowCount();++row)
            if(random()%6==0) { state.setSearch(row,true);searchIds.insert(model.ids.at(row)); }
        if(round%4==0) model.reverseRows();
        for(int row=0;row<model.rowCount();++row)
        {
            const bool expected=businessIds.contains(model.ids.at(row))||searchIds.contains(model.ids.at(row))
                ||model.ids.at(row)==legacyId||host.isFrozen(row);
            if (host.hidden.at(row)!=expected)
                std::fprintf(stderr,"round=%d row=%d id=%d expected=%d actual=%d business=%d search=%d legacy=%d frozen=%d\n",
                    round,row,model.ids.at(row),int(expected),int(host.hidden.at(row)),
                    int(businessIds.contains(model.ids.at(row))),int(searchIds.contains(model.ids.at(row))),
                    int(model.ids.at(row)==legacyId),int(host.isFrozen(row)));
            check(host.hidden.at(row)==expected,"random composition matches business identity oracle");
        }
    }
    state.clearSearch();state.setBusiness({});
    model.insertAt(2);
    host.hidden[2]=true; // business legacy handler runs after endInsertRows
    state.clearSearch(); // immediate clear must capture the pending baseline
    check(state.legacyExcludes(2),"post-insert hidden baseline captured before immediate clear");
    state.setSearch(2,true);state.clearSearch();check(host.hidden[2],"new legacy row not exposed");
    const auto removed=QPersistentModelIndex(model.index(4,0));
    state.setBusiness({removed});model.removeAt(4);state.setBusiness({removed});
    check(!removed.isValid(),"removed identity invalidated");
    model.resetRows(20);host.hidden[4]=true;
    state.setBusiness({model.index(6,0)});state.setSearch(8,true);state.clearSearch();
    check(host.hidden[4]&&host.hidden[6]&&!host.hidden[8],"post-reset business/search/legacy layering");
    const quint64 beforeRevision=state.revision();
    state.setSearch(10,true);
    check(state.revision()>beforeRevision,"synchronous revision detects filter changes before queued publication");
    model.removeFirstColumn();
    check(state.excludes(6)&&state.excludes(10)&&state.legacyExcludes(4),"first-column removal reanchors all row layers");
    model.insertFirstColumn();
    check(state.excludes(6)&&state.excludes(10)&&state.legacyExcludes(4),"first-column insertion normalizes row layers");
    state.clearSearch();
    Model other;other.resetRows(5);
    state.setBusiness({other.index(2,0)});
    check(!host.hidden[6]&&host.hidden[4],"wrong model identity ignored, existing filter cleared");
    // Destruction cancels queued notifications through QObject context.
    auto* ephemeral=new ks::ui::TableRowFilterIdentityState(&model,{},
        [&](int row) { return host.hidden.at(row); },[&](int) { return false; },
        [&](int row,bool value) { host.hidden[row]=value; },[]() { std::abort(); });
    ephemeral->setBusiness({model.index(1,0)});delete ephemeral;
    QCoreApplication::processEvents();
    std::printf("PASS %d pure QtCore filter identity checks\n",checks);
}
