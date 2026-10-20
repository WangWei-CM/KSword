#include "../../Ksword5.1/Ksword5.1/UI/TableRowFilterIdentityState.h"
#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QVector>
#include <memory>
#include <cstdio>
#include <cstdlib>

static int checks=0;
static void check(bool value,const char* message)
{++checks;if(!value){std::fprintf(stderr,"FAIL %s at %d\n",message,checks);std::exit(1);}}
class Model final : public QAbstractTableModel
{
public:
    int count=0;
    mutable int rowReads=0;
    int rowCount(const QModelIndex& parent={}) const override {++rowReads;return parent.isValid()?0:count;}
    int columnCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:2;}
    QVariant data(const QModelIndex&,int) const override {return {};}
    void resetRows(int value){beginResetModel();count=value;endResetModel();}
};
struct Host final : QObject
{
    Model model;
    QVector<bool> hidden;
    QSet<QPersistentModelIndex> frozen;
    int writes=0;
    std::unique_ptr<ks::ui::TableRowFilterIdentityState> state;
    explicit Host(int count)
    {
        model.resetRows(count);hidden.resize(count);
        // Match Qt6.9.3 QHeaderView::reset: retained sections keep old hidden bits.
        connect(&model,&QAbstractItemModel::modelReset,this,[this]{hidden.resize(model.count);});
    }
    void bind(bool exclusive=false)
    {
        state=std::make_unique<ks::ui::TableRowFilterIdentityState>(&model,QModelIndex(),
            [this](int row){return hidden.at(row);},
            [this](int row){return frozen.contains(QPersistentModelIndex(model.index(row,0)));},
            [this](int row,bool value){++writes;hidden[row]=value;},[] {},[this]{
                QSet<int> rows;for(const auto& id:frozen)if(id.isValid())rows.insert(id.row());return rows;},nullptr,exclusive);
    }
    void finish(){int events=0;while(state->projectionPending()&&events++<10000)QCoreApplication::processEvents();check(!state->projectionPending(),"projection terminates");}
};
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    {
        Host host(30);host.hidden[2]=true;host.bind();
        host.frozen.insert(host.model.index(12,0));host.hidden[12]=true;
        host.state->setBusiness({host.model.index(4,0)});host.state->setSearch(8,true);
        const int before=host.writes;host.model.resetRows(30);
        check(host.writes-before==3,"legacy-compatible aboutReset clears only shared/frozen-owned bits");
        check(host.hidden[2]&&!host.hidden[4]&&!host.hidden[8]&&!host.hidden[12],"same-size header retention does not leak shared identities");
        // A legacy producer's same-position hide emits no Qt change when it was
        // already true. The compatibility path makes this new intent observable.
        host.hidden[4]=true;host.hidden[6]=true;
        host.state->setSearch(0,true);host.state->clearSearch();
        check(host.state->legacyExcludes(4)&&host.state->legacyExcludes(6),"post-reset direct legacy same-position intent preserved");
        check(!host.state->excludes(8)&&!host.state->excludes(12),"old search/freeze do not become new legacy");
        check(!host.state->projectionPending(),"legacy-compatible reset has no unfinished physical projection");
    }
    {
        Host host(20000);host.model.rowReads=0;host.bind(true);
        check(host.model.rowReads==0,"already declared exclusive model binding skips all-row legacy capture");
        host.model.resetRows(20000);
        check(host.model.rowReads<=4,"exclusive reset reads only one generation sentinel and never enumerates new rows");
    }
    for (bool producerBeforeLedger : {false,true})
    {
        Host host(30);
        const auto legacy = [&] { host.hidden[4]=true; };
        if (producerBeforeLedger) QObject::connect(&host.model,&QAbstractItemModel::modelReset,&host,legacy);
        host.bind();host.state->setBusiness({host.model.index(4,0)});
        if (!producerBeforeLedger) QObject::connect(&host.model,&QAbstractItemModel::modelReset,&host,legacy);
        host.model.resetRows(30);
        check(host.state->legacyExcludes(4),"same-position legacy modelReset handler preserved in either connection order");
    }
    {
        Host host(20000);host.bind();host.state->setExclusiveBusinessOwnership(true);
        QSet<QPersistentModelIndex> even;
        for(int row=0;row<host.model.count;row+=2)even.insert(host.model.index(row,0));
        host.state->setBusiness(even);
        const int before=host.writes;host.model.resetRows(20000);
        check(host.writes==before,"exclusive reset performs no synchronous row visibility writes");
        check(host.state->projectionPending()&&!host.state->excludes(0)&&!host.state->legacyExcludes(0),"canonical reset rejects stale physical ownership before slices");
        QCoreApplication::processEvents();
        check(host.writes-before<=256&&host.state->projectionPending(),"exclusive reset shares deterministic bounded projection wake");
        host.state->setBusiness({host.model.index(0,0),host.model.index(7,0),host.model.index(19999,0)},true);
        host.finish();
        for(int row=0;row<host.model.count;++row)
        {
            const bool excluded=row==0||row==7||row==19999;
            check(host.hidden.at(row)==excluded,"new business projection wins over queued old-reset cleanup");
            check(host.state->excludes(row)==excluded,"new canonical rows match physical completion");
            check(!host.state->legacyExcludes(row),"no retained old shared bits are recaptured as legacy");
        }
        host.state->setBusiness({host.model.index(0,0)});
        host.state->setBusiness({},true); // canonical clear with unfinished old hidden bit
        host.model.resetRows(20000);host.finish();
        for(bool hidden:host.hidden)check(!hidden,"reset includes unfinished unexclude physical responsibility");
    }
    {
        Host host(20);host.hidden[3]=true;host.bind();
        host.state->setExclusiveBusinessOwnership(true);
        check(host.state->businessExcludes(host.model.index(3,0))&&!host.state->legacyExcludes(3),"explicit ownership transition preserves old legacy as canonical identity");
        host.state->setBusiness({});check(!host.hidden[3],"first complete canonical submission can clear migrated legacy");
        host.state->setBusiness({host.model.index(4,0)});
        host.model.resetRows(20);check(host.state->projectionPending(),"exclusive reset owns pending stale bits");
        host.state->setExclusiveBusinessOwnership(false);
        check(!host.state->projectionPending()&&!host.hidden[4],"demotion settles projection before restoring legacy writers");
        host.model.resetRows(20);host.hidden[4]=true;
        check(host.state->legacyExcludes(4),"legacy producer remains valid after explicit ownership demotion");
    }
    std::printf("PASS %d retained-header reset ownership checks; no Widgets\n",checks);
}
