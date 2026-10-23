#include "TableRowFilterIdentityState.h"
#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QPointer>
#include <QVector>
#include <cstdio>
#include <cstdlib>
#include <memory>

class Model final : public QAbstractTableModel
{
public:
    int count=30;
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:count;}
    int columnCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:2;}
    QVariant data(const QModelIndex&,int) const override {return {};}
    void resetRows(){beginResetModel();endResetModel();}
};
class FakeTable final : public QObject
{
public:
    Model* value;
    QVector<bool> hidden;
    QSet<QPersistentModelIndex> frozen;
    ks::ui::TableRowFilterIdentityState* state=nullptr;
    explicit FakeTable(Model* model):value(model),hidden(model->count,false)
    {connect(model,&QAbstractItemModel::modelReset,this,[this]{hidden.resize(value->count);});}
    Model* model() const {return value;}
    QModelIndex rootIndex() const {return {};}
    bool isRowHidden(int row) const {return hidden.at(row);}
    void setRowHidden(int row,bool hide) {hidden[row]=hide;}
};
using QTableView=FakeTable;
namespace ks::ui
{
    QTableView* filterSource(const QTableView* table){return const_cast<QTableView*>(table);}
    TableRowFilterIdentityState* rowFilterState(QTableView* table,bool){return table?table->state:nullptr;}
    bool isRowHiddenByFreeze(const QTableView* table,int row)
    {return table->frozen.contains(QPersistentModelIndex(table->model()->index(row,0)));}
    class TableFrozenPaneController final : public QObject
    {
    public:
        struct FrozenLine {QPersistentModelIndex index;};
        QPointer<QTableView> m_targetTable;
        QVector<FrozenLine> m_frozenRowLines;
        void unfreezeAllRows();
        void publishFrozenSections()
        {
            if(!m_targetTable)return;
            m_targetTable->frozen.clear();
            for(const auto& line:m_frozenRowLines)m_targetTable->frozen.insert(line.index);
        }
    };
#include "table_reset_freeze_actual.inc"
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);int checks=0;
    const auto check=[&](bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}};
    for(bool ledgerFirst:{false,true})for(bool exclusive:{false,true})for(int layer=0;layer<4;++layer)
        for(int legacy=0;legacy<(exclusive?1:3);++legacy)
    {
        Model model;FakeTable table(&model);ks::ui::TableFrozenPaneController controller;
        controller.m_targetTable=&table;
        const auto unfreeze=[&]{controller.unfreezeAllRows();};
        if(!ledgerFirst)QObject::connect(&model,&QAbstractItemModel::modelAboutToBeReset,&controller,unfreeze);
        if(legacy==1)table.hidden[4]=true;
        auto state=std::make_unique<ks::ui::TableRowFilterIdentityState>(&model,QModelIndex(),
            [&](int row){return table.hidden.at(row);},
            [&](int row){return ks::ui::isRowHiddenByFreeze(&table,row);},
            [&](int row,bool value){table.hidden[row]=value;},[] {},[&]{
                QSet<int> rows;for(const auto& id:table.frozen)if(id.isValid())rows.insert(id.row());return rows;});
        table.state=state.get();
        if(ledgerFirst)QObject::connect(&model,&QAbstractItemModel::modelAboutToBeReset,&controller,unfreeze);
        state->setExclusiveBusinessOwnership(exclusive);
        controller.m_frozenRowLines.push_back({model.index(4,0)});controller.publishFrozenSections();table.hidden[4]=true;
        if(layer&1)state->setBusiness({model.index(4,0)});
        if(layer&2)state->setSearch(4,true);
        if(legacy==2)QObject::connect(&model,&QAbstractItemModel::modelReset,&table,[&]{table.hidden[4]=true;});
        model.resetRows();
        check(controller.m_frozenRowLines.isEmpty(),"actual unfreeze body clears old frozen identities");
        check(!state->businessExcludes(model.index(4,0)),"old business identity cancels before new data");
        check(state->legacyExcludes(4)==(legacy!=0),"old business/search/freeze is not new legacy in either aboutReset connection order");
        int events=0;while(state->projectionPending()&&events++<10000)QCoreApplication::processEvents();
        check(!state->projectionPending(),"combined projection completes");
        check(table.hidden[4]==(legacy!=0),"only real legacy remains physically hidden after reset");
        check(ks::ui::IsTableRowExcludedByFilter(&table,4)==(legacy!=0),"actual public adapter agrees with new row identity");
    }
    std::printf("PASS %d actual reset/freeze composition checks; no Widgets\n",checks);
}
