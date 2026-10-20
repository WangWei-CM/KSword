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
    mutable int rowReads = 0;
    int rowCount(const QModelIndex& parent = {}) const override { ++rowReads; return parent.isValid() ? 0 : int(ids.size()); }
    int columnCount(const QModelIndex& = {}) const override { return columns; }
    void removeFirstColumn() { beginRemoveColumns({},0,0);--columns;endRemoveColumns(); }
    void insertFirstColumn() { beginInsertColumns({},0,0);++columns;endInsertColumns(); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    { return index.isValid() && role == Qt::DisplayRole ? QVariant(ids.at(index.row())) : QVariant(); }
    void insertAt(int at) { beginInsertRows({},at,at); ids.insert(at,++next); endInsertRows(); }
    void removeAt(int at) { beginRemoveRows({},at,at); ids.removeAt(at); endRemoveRows(); }
    void moveRowAt(int first,int destination)
    {
        if(first==destination||first+1==destination)return;
        beginMoveRows({},first,first,{},destination);
        const int id=ids.takeAt(first);ids.insert(destination>first?destination-1:destination,id);
        endMoveRows();
    }
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
    // This synthetic benchmark has no remaining direct business hide writer.
    state.setExclusiveBusinessOwnership(true);
    model.resetRows(20000);host.frozen.clear();
    QSet<QPersistentModelIndex> even,odd;
    for(int row=0;row<model.rowCount();++row)
        (row%2?odd:even).insert(model.index(row,0));
    const int beforeProjection=host.writes;
    state.setBusiness(even,true);
    check(state.projectionPending()&&state.excludes(19998),"canonical fullset commits before GUI projection");
    check(host.writes==beforeProjection,"incremental request performs no synchronous visibility writes");
    state.setBusiness(odd,true); // supersede queued old ticket before first projection
    check(!state.excludes(19998)&&state.excludes(19999),"replacement canonical ticket supersedes old identity set");
    int rounds=0;
    while(state.projectionPending()&&rounds++<10000) QCoreApplication::processEvents();
    check(!state.projectionPending(),"large projection completes without truncation");
    for(int row=0;row<model.rowCount();++row)
        check(host.hidden.at(row)==bool(row%2),"replacement projection writes only latest canonical state");
    // Exactly one projection callback is outstanding and each callback consumes
    // at most 256 rows. A processEvents call handles the previously queued wake.
    const int beforeOneSlice=host.writes;
    state.setBusiness(even,true);QCoreApplication::processEvents();
    const int oneSliceWrites=host.writes-beforeOneSlice;
    check(oneSliceWrites<=256,"one projection wake has a deterministic 256-row upper bound");
    check(state.projectionPending(),"large set remains pending after one bounded wake");
    state.setBusiness(odd,true);
    while(state.projectionPending()) QCoreApplication::processEvents();
    for(int row=0;row<model.rowCount();++row)
        check(host.hidden.at(row)==bool(row%2),"replacement retains unfinished rows but never replays old canonical acceptance");
    bool blocked=true;std::function<void()> resume;
    state.setProjectionDeferrer([&](std::function<void()> retry) {
        if(!blocked)return false;resume=std::move(retry);return true;
    });
    const int beforeBlocked=host.writes;
    state.setBusiness(even,true);
    for(int event=0;event<12;++event)QCoreApplication::processEvents();
    check(state.projectionPending()&&bool(resume),"menu barrier retains pending continuation");
    check(host.writes==beforeBlocked,"blocked projection has no visibility writes or retry polling");
    blocked=false;resume();resume={};
    while(state.projectionPending())QCoreApplication::processEvents();
    for(int row=0;row<model.rowCount();++row)
        check(host.hidden.at(row)==!bool(row%2),"released barrier resumes full current projection");
    state.setBusiness(odd,true);
    model.resetRows(30);host.frozen.clear();
    state.setBusiness({model.index(3,0),model.index(17,0)},true);
    while(state.projectionPending())QCoreApplication::processEvents();
    for(int row=0;row<model.rowCount();++row)
        check(host.hidden.at(row)==(row==3||row==17),"reset cancels queued old model identities");
    bool hiddenView=true;
    host.installEventFilter(&state);
    state.setProjectionDeferrer([&](std::function<void()>) { return hiddenView; });
    const int beforeHidden=host.writes;
    state.setBusiness({model.index(8,0)},true);
    for(int i=0;i<10;++i)QCoreApplication::processEvents();
    check(state.projectionPending()&&host.writes==beforeHidden,"hidden view defers projection without polling");
    hiddenView=false;QEvent showEvent(QEvent::Show);QCoreApplication::sendEvent(&host,&showEvent);
    while(state.projectionPending())QCoreApplication::processEvents();
    check(host.hidden[8]&&!host.hidden[3]&&!host.hidden[17],"Show resumes current queued identity deltas");
    host.removeEventFilter(&state);
    state.setProjectionDeferrer({});
    state.setBusiness({model.index(8,0)},true);
    while(state.projectionPending())QCoreApplication::processEvents();
    const int beforeSortProjection=host.writes;
    model.reverseRows();
    check(state.projectionPending()&&host.writes==beforeSortProjection,"incremental filter sorting schedules identities without synchronous visibility replay");
    while(state.projectionPending())QCoreApplication::processEvents();
    for(int row=0;row<model.rowCount();++row)
        check(host.hidden.at(row)==state.excludes(row),"sorted incremental projection restores canonical row identity");
    state.setBusiness({model.index(8,0)},true);
    state.setBusiness({model.index(12,0)}); // compatibility synchronous API flushes latest state
    check(!state.projectionPending()&&host.hidden[12]&&!host.hidden[8],"synchronous compatibility request settles pending projection before returning");
    // Regression: the old physical header position must be released even
    // after canonical business acceptance has already removed the pending id.
    state.setBusiness({model.index(0,0)});
    state.setBusiness({},true);
    check(state.projectionPending()&&host.hidden[0],"pending unexclude starts with a physical old hidden section");
    model.reverseRows();
    while(state.projectionPending())QCoreApplication::processEvents();
    for(int row=0;row<model.rowCount();++row)
        check(!host.hidden.at(row),"cancel exclusion then sort releases both old logical section and persistent moved identity");
    state.setBusiness({model.index(0,0)});
    state.setBusiness({},true);
    model.moveRowAt(0,model.rowCount());
    while(state.projectionPending())QCoreApplication::processEvents();
    for(int row=0;row<model.rowCount();++row)
        check(!host.hidden.at(row),"cancel exclusion then move releases stale logical hidden section");
    // Destruction cancels queued notifications through QObject context.
    auto* ephemeral=new ks::ui::TableRowFilterIdentityState(&model,{},
        [&](int row) { return host.hidden.at(row); },[&](int) { return false; },
        [&](int row,bool value) { host.hidden[row]=value; },[]() { std::abort(); });
    ephemeral->setBusiness({model.index(1,0)});delete ephemeral;
    QCoreApplication::processEvents();
    {
        Model large; large.resetRows(100000); Host target(&large);
        ks::ui::TableRowFilterIdentityState ledger(&large,{},
            [&](int row){return target.hidden.at(row);},[&](int row){return target.isFrozen(row);},
            [&](int row,bool value){++target.writes;target.hidden[row]=value;},[](){},
            [&](){QSet<int> result;for(const auto& id:target.frozen)if(id.isValid())result.insert(id.row());return result;},nullptr,true);
        QSet<QPersistentModelIndex> all;
        for(int row=0;row<99999;++row)all.insert(large.index(row,0));
        ledger.setBusiness(all,true);
        while(ledger.projectionPending())QCoreApplication::processEvents();
        QSet<QPersistentModelIndex> add{large.index(99999,2)},remove{large.index(500,1)};
        large.rowReads=0;const int beforeWrites=target.writes;
        ledger.setBusinessDelta(add,remove);
        check(large.rowReads<50,"two-identity delta never renormalizes 99999 retained exclusions");
        check(target.writes==beforeWrites&&ledger.projectionPending(),"delta canonical commit precedes budgeted physical projection");
        check(ledger.excludes(99999)&&!ledger.excludes(500),"delta normalizes changed columns into canonical row identities");
        const auto revision=ledger.revision();ledger.setBusinessDelta(add,remove);
        check(ledger.revision()==revision,"same delta has no spurious identity revision");
        while(ledger.projectionPending())QCoreApplication::processEvents();
        check(target.hidden[99999]&&!target.hidden[500],"delta visibility converges");
        const auto settledRevision=ledger.revision();
        const QSet<QPersistentModelIndex> overlap{large.index(99999,1)};
        ledger.setBusinessDelta(overlap,overlap);
        check(ledger.revision()==settledRevision&&!ledger.projectionPending(),"deny-wins overlap with same final state is a no-op");
        ledger.setSearch(5,true);target.frozen.insert(large.index(6,0));target.hidden[6]=true;
        ledger.setBusinessDelta({}, {large.index(5,0),large.index(6,0)});
        while(ledger.projectionPending())QCoreApplication::processEvents();
        check(target.hidden[5]&&target.hidden[6],"business acceptance cannot reveal search exclusion or frozen source row");
        ledger.clearSearch();check(!target.hidden[5]&&target.hidden[6],"search clear preserves freeze and accepted delta");
        Model foreign;foreign.resetRows(1);const auto oldRevision=ledger.revision();
        ledger.setBusinessDelta({foreign.index(0,0),QPersistentModelIndex()},{});
        check(ledger.revision()==oldRevision,"foreign and invalid delta identities ignored");
        const QPersistentModelIndex moved=large.index(500,0);
        ledger.setBusinessDelta({moved},{});large.reverseRows();
        while(ledger.projectionPending())QCoreApplication::processEvents();
        check(ledger.businessExcludes(moved)&&target.hidden[moved.row()],"pending delta follows remapped persistent identity through sorting");
        const QSet<QPersistentModelIndex> stale{large.index(22,0)};large.resetRows(16);
        ledger.setBusinessDelta(stale,{});
        while(ledger.projectionPending())QCoreApplication::processEvents();
        for(int row=0;row<16;++row)check(!ledger.excludes(row)&&!target.hidden[row],"stale pre-reset delta cannot affect new rows");
    }
    std::printf("PASS %d pure QtCore identity/projection checks\n",checks);
}
