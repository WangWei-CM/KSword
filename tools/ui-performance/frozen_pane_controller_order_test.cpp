#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QRect>
#include <QVector>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include "FrozenPaneRowSync.h"

class FakeHeader final : public QObject
{
public:
    int sectionSize(int) const { return 24; }
    void resizeSection(int,int) {}
};
using QHeaderView = FakeHeader;
class FakeTable final : public QObject
{
public:
    QPointer<QAbstractItemModel> currentModel;
    FakeHeader header;
    QRect geometryValue{0,0,640,240};
    QVector<int> visibleRows{99990,99999};
    bool visible = false;
    int prematureShows = 0;
    std::function<bool()> mayShow;
    QAbstractItemModel* model() const { return currentModel.data(); }
    QModelIndex rootIndex() const { return {}; }
    FakeTable* viewport() { return this; }
    const FakeTable* viewport() const { return this; }
    int height() const { return geometryValue.height(); }
    int width() const { return geometryValue.width(); }
    QRect geometry() const { return geometryValue; }
    bool isRightToLeft() const { return false; }
    FakeHeader* verticalHeader() { return &header; }
    int rowAt(int y) const { return y >= 0 && y/24 < visibleRows.size() ? visibleRows.at(y/24) : -1; }
    int rowViewportPosition(int row) const { return visibleRows.indexOf(row)*24; }
    int rowHeight(int) const { return 24; }
    bool isRowHidden(int) const { return false; }
    void setRowHidden(int,bool) {}
    void setGeometry(int x,int y,int width,int height) { geometryValue={x,y,width,height}; }
    void setVisible(bool value) { if(value && mayShow && !mayShow()) ++prematureShows; visible=value; }
    void raise() {}
};
using QTableView = FakeTable;
class ValueModel final : public QAbstractTableModel
{
public:
    int rowCount(const QModelIndex& parent={}) const override { return parent.isValid()?0:100000; }
    int columnCount(const QModelIndex& parent={}) const override { return parent.isValid()?0:1; }
    QVariant data(const QModelIndex&,int) const override { return {}; }
};
namespace ks::ui
{
    int headerBandHeight(const QTableView*) { return 24; }
    int headerBandWidth(const QTableView*) { return 24; }
    class TableFrozenPaneController final : public QObject
    {
    public:
        QPointer<QTableView> m_targetTable,m_topPane,m_leftPane,m_cornerPane;
        QHash<QTableView*,std::shared_ptr<FrozenPaneRowSync>> m_rowSyncStates;
        int m_appliedFrozenWidth=100,m_appliedFrozenHeight=0;
        int scheduled=0;
        std::shared_ptr<FrozenPaneRowSync> rowSyncFor(QTableView*,bool);
        bool rowSyncPending(QTableView*) const;
        void mirrorVisibleRowLayout(QTableView*);
        void layoutPanes();
        void scheduleRowSync() { ++scheduled; }
        void applyFrozenColumnFilter(QTableView*) {}
        void applyFrozenRowFilter(QTableView*) {}
        void mirrorColumnLayout(QTableView*) {}
    };
#include "frozen_pane_controller_actual.inc"
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    ValueModel model;
    QTableView source,pane;
    source.currentModel=&model;pane.currentModel=&model;
    ks::ui::TableFrozenPaneController host;
    host.m_targetTable=&source;host.m_leftPane=&pane;
    pane.mayShow=[&] { return host.m_rowSyncStates.contains(&pane) && !host.rowSyncPending(&pane); };
    auto check=[](bool value,const char* message) {if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}};
    host.layoutPanes();
    check(host.m_rowSyncStates.contains(&pane),"left SourceRows state must exist in first layout");
    check(host.rowSyncPending(&pane)&&!pane.visible&&pane.prematureShows==0,"first visibility waits for coherent state");
    auto state=host.m_rowSyncStates.value(&pane);
    state->drain([](int){return ks::ui::FrozenPaneRowSync::Row{true,0};},[](int,auto){return true;},1000,17);
    host.layoutPanes();
    check(!pane.visible&&pane.prematureShows==0,"partial initialization cannot reveal default rows");
    int slices=0;
    while(state->pending()&&slices++<100000)
        state->drain([](int){return ks::ui::FrozenPaneRowSync::Row{true,0};},[](int,auto){return true;},1000,256);
    check(!state->pending(),"bounded initialization completes");
    host.layoutPanes();
    check(pane.visible&&pane.prematureShows==0,"coherent pane becomes visible");
    std::printf("PASS 5 actual-controller-order assertions; no Widgets\n");
}
