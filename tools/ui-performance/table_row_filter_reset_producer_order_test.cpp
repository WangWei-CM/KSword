#include "TableRowFilterIdentityState.h"
#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QVector>
#include <cstdio>
#include <cstdlib>
#include <memory>

template<class State>
auto noteRow(State* state,int row,int) -> decltype(state->rowVisibilityChanged(row),void())
{state->rowVisibilityChanged(row);}
template<class State> void noteRow(State*,int,long) {}
class Model final : public QAbstractTableModel
{
public:
    int count;
    explicit Model(int value):count(value) {}
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:count;}
    int columnCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:2;}
    QVariant data(const QModelIndex&,int) const override {return {};}
    void resetRows(int value){beginResetModel();count=value;endResetModel();}
};
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);int checks=0;
    const auto check=[&](bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL %s at %d\n",message,checks);std::exit(1);}};
    for(bool producerFirst:{false,true})for(bool incremental:{false,true})for(bool exclusive:{false,true})
        for(int initialRows:{0,20})for(bool lateLegacy:{false,true})
    {
        if(exclusive&&lateLegacy)continue;
        Model model(initialRows);QObject host;QVector<bool> hidden(initialRows,false);
        QObject::connect(&model,&QAbstractItemModel::modelReset,&host,[&]{hidden.resize(model.count);});
        std::unique_ptr<ks::ui::TableRowFilterIdentityState> state;
        const auto write=[&](int row,bool value) {
            // Match Qt header timing: signal while the old hidden bit remains.
            if(hidden.at(row)!=value)noteRow(state.get(),row,0);
            hidden[row]=value;
        };
        const auto produce=[&]{state->setBusiness({model.index(4,0),model.index(7,0)},incremental);};
        if(producerFirst)QObject::connect(&model,&QAbstractItemModel::modelReset,&host,produce);
        state=std::make_unique<ks::ui::TableRowFilterIdentityState>(&model,QModelIndex(),
            [&](int row){return hidden.at(row);},[](int){return false;},write,[] {});
        state->setExclusiveBusinessOwnership(exclusive);
        if(!producerFirst)QObject::connect(&model,&QAbstractItemModel::modelReset,&host,produce);
        if(lateLegacy)QObject::connect(&model,&QAbstractItemModel::modelReset,&host,[&]{write(13,true);});
        if(initialRows)state->setBusiness({model.index(4,0),model.index(9,0)});
        model.resetRows(20);
        check(state->businessExcludes(model.index(4,0))&&state->businessExcludes(model.index(7,0)),
            "new canonical accepted by earlier/later modelReset producer survives final ledger slot");
        state->setBusiness({},incremental); // before queued callbacks can mask a lost legacy baseline
        if(lateLegacy)
        {
            state->setSearch(13,true);state->clearSearch();
            check(hidden[13],"late direct legacy row remains hidden through immediate search clear");
        }
        int events=0;while(state->projectionPending()&&events++<10000)QCoreApplication::processEvents();
        check(!state->projectionPending(),"new generation and immediate clear settle without lost ownership");
        for(int row=0;row<model.count;++row)
        {
            check(hidden.at(row)==(lateLegacy&&row==13),"clear restores all new source rows except actual legacy");
            check(state->excludes(row)==(lateLegacy&&row==13),"new canonical clear never becomes permanent legacy");
        }
    }
    std::printf("PASS %d actual modelReset producer-order checks; no Widgets\n",checks);
}
