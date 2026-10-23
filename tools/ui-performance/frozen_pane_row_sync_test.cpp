#include "../../Ksword5.1/Ksword5.1/UI/FrozenPaneRowSync.h"
#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>
#include <random>

static int checks=0;
static void check(bool value,const char* text)
{ ++checks;if(!value){std::fprintf(stderr,"FAIL %s at %d\n",text,checks);std::exit(1);} }
using Sync=ks::ui::FrozenPaneRowSync;
struct SourceRow { int id=0,height=20;bool hidden=false; };
struct PaneRow { int height=20;bool hidden=false; };
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    std::mt19937 random(43891);
    QVector<SourceRow> source;QVector<PaneRow> left,top;
    int nextId=0,reads=0,writes=0;
    QHash<int,int> frozenById;
    auto desired=[&]() { QHash<int,int> result;for(int row=0;row<source.size();++row)
        if(frozenById.contains(source.at(row).id))result.insert(row,frozenById.value(source.at(row).id));return result; };
    auto add=[&](int at,int count) {for(int i=0;i<count;++i){source.insert(at,{++nextId,20+int(random()%80),bool(random()%3==0)});left.insert(at,{});top.insert(at,{});} };
    add(0,100000);
    Sync leftSync(Sync::Mode::SourceRows),topSync(Sync::Mode::FrozenRows);
    leftSync.reset(source.size());topSync.reset(source.size());
    frozenById.insert(source.at(2).id,37);frozenById.insert(source.at(99999).id,91);topSync.setFrozenRows(desired());
    auto read=[&](int row) {++reads;check(row>=0&&row<source.size(),"read row remains within current source");return Sync::Row{source.at(row).hidden,source.at(row).height};};
    auto writeLeft=[&](int row,Sync::Row value) {++writes;check(row>=0&&row<left.size(),"left row remains within current pane");left[row].hidden=value.hidden;if(!value.hidden)left[row].height=value.height;return true;};
    auto writeTop=[&](int row,Sync::Row value) {++writes;check(row>=0&&row<top.size(),"top row remains within current pane");top[row].hidden=value.hidden;if(!value.hidden)top[row].height=value.height;return true;};
    auto noRead=[&](int) {check(false,"frozen-only projection must not inspect full source row data");return Sync::Row{};};
    const int first=leftSync.drain(read,writeLeft,1000,17);
    check(first==17&&reads==17&&leftSync.pending(),"initial large mapping obeys deterministic row budget");
    auto finish=[&]() {for(int i=0;i<100000&&(leftSync.pending()||topSync.pending());++i){leftSync.drain(read,writeLeft,1000,73);topSync.drain(noRead,writeTop,1000,73);}check(!leftSync.pending()&&!topSync.pending(),"all section work completes without row cap truncation");};
    auto equal=[&]() {const auto frozen=desired();for(int row=0;row<source.size();++row){
        check(left.at(row).hidden==source.at(row).hidden,"left hidden state equals the real source header");
        if(!source.at(row).hidden)check(left.at(row).height==source.at(row).height,"variable visible source row height preserved");
        check(top.at(row).hidden==!frozen.contains(row),"top pane shows exactly frozen identities");
        if(frozen.contains(row))check(top.at(row).height==frozen.value(row),"frozen original extent preserved");}};
    finish();equal();
    reads=writes=0;
    for(int iteration=0;iteration<100;++iteration){topSync.setFrozenRows(desired());leftSync.drain(read,writeLeft);topSync.drain(noRead,writeTop);}
    check(reads==0&&writes==0,"unchanged geometry/show preparation performs no row mapping scans");
    source[54321].hidden=!source[54321].hidden;source[54321].height=93;leftSync.dirtyRow(54321);
    leftSync.drain(read,writeLeft,1000,256);
    check(reads==1&&writes==1,"single source hide/height change mirrors one row only");
    reads=writes=0;frozenById.clear();frozenById.insert(source.at(50000).id,44);topSync.setFrozenRows(desired());topSync.drain(noRead,writeTop,1000,256);
    check(reads==0&&writes==3,"frozen set changes touch old/new frozen identities only");
    equal();
    reads=writes=0;
    topSync.sourceRowChanged(54321);
    topSync.drain(noRead,writeTop,1000,256);
    check(!topSync.pending() && writes==0,"top pane ignores unrelated source row changes");
    topSync.sourceRowChanged(50000);
    topSync.drain(noRead,writeTop,1000,256);
    check(writes==1,"top pane tracks a source change to its frozen row only");
    reads=writes=0;
    leftSync.resizeRowCount(source.size());leftSync.layoutChanged();
    leftSync.drain(read,writeLeft);
    check(reads==0&&writes==0,"settled same-size model reset/layout performs no row mirror scan");
    source.clear();left.clear();top.clear();frozenById.clear();add(0,1000);
    leftSync.reset(source.size());topSync.reset(source.size());
    leftSync.drain(read,writeLeft,1000,17);
    leftSync.resizeRowCount(source.size()); // retain an in-progress cursor
    const int beforeResetContinuation=reads;
    leftSync.drain(read,writeLeft,1000,17);
    check(reads-beforeResetContinuation==17,"same-size reset retains budgeted continuation instead of restarting");
    // Simulate both headers moving a section during an unfinished baseline;
    // unknown permutation is reconciled in bounded slices, never on scroll.
    std::swap(source[2],source[900]);std::swap(left[2],left[900]);
    leftSync.layoutChanged();finish();equal();
    // Rebuild a small mixed dataset, then mutate structures while only part of
    // initial/inserted ranges have been projected (not just after drain).
    source.clear();left.clear();top.clear();frozenById.clear();add(0,80);leftSync.reset(source.size());topSync.reset(source.size());
    for(int iteration=0;iteration<1500;++iteration)
    {
        const int op=int(random()%7);
        if(op==0||source.isEmpty())
        {
            const int at=int(random()%(source.size()+1)),count=1+int(random()%5);
            add(at,count);leftSync.rowsInserted(at,count);topSync.rowsInserted(at,count);
        }
        else if(op==1)
        {
            const int at=int(random()%source.size()),count=std::min(1+int(random()%3),int(source.size())-at);
            for(int i=at;i<at+count;++i)frozenById.remove(source.at(i).id);
            source.remove(at,count);left.remove(at,count);top.remove(at,count);
            leftSync.rowsRemoved(at,count);topSync.rowsRemoved(at,count);
        }
        else if(op==2)
        {
            const int row=int(random()%source.size());source[row].hidden=!source[row].hidden;source[row].height=20+int(random()%150);
            leftSync.dirtyRow(row);
        }
        else if(op==3)
        {
            const int row=int(random()%source.size());const int id=source.at(row).id;
            if(frozenById.contains(id))frozenById.remove(id);else frozenById.insert(id,20+int(random()%100));
        }
        else if(op==4)
        {
            source.clear();left.clear();top.clear();frozenById.clear();add(0,20+int(random()%70));
            leftSync.reset(source.size());topSync.reset(source.size());
        }
        topSync.setFrozenRows(desired());
        const int limit=1+int(random()%13);
        check(leftSync.drain(read,writeLeft,1000,limit)<=limit,"incremental source slice respects work budget");
        check(topSync.drain(noRead,writeTop,1000,limit)<=limit,"incremental frozen slice respects work budget");
        if(iteration%5==0){finish();equal();}
    }
    finish();equal();
    // Sparse visible identities and lower-viewport whitespace must never turn
    // into the closed interval [firstLogicalRow,lastLogicalRow] or model end.
    QVector<int> visibleIds{2,10000,999999};QVector<int> positions{-10,20,60},heights{30,40,100};
    QVector<int> visited;int lookups=0;
    ks::ui::ForEachViewportTableRow(100,[&](int y){++lookups;for(int i=0;i<visibleIds.size();++i)if(y>=positions.at(i)&&y<positions.at(i)+heights.at(i))return visibleIds.at(i);return -1;},
        [&](int row){return positions.at(visibleIds.indexOf(row));},[&](int row){return heights.at(visibleIds.indexOf(row));},[&](int row){visited.push_back(row);});
    check(visited==visibleIds&&lookups==3,"viewport work depends on three visible rows, not million-row logical span");
    visited.clear();lookups=0;
    ks::ui::ForEachViewportTableRow(400,[&](int y){++lookups;return y<20?1:y<50?500000:-1;},
        [&](int row){return row==1?0:20;},[&](int row){return row==1?20:30;},[&](int row){visited.push_back(row);});
    check(visited==QVector<int>({1,500000})&&lookups==3,"blank bottom stops immediately without model-end fallback");
    visited.clear();ks::ui::ForEachViewportTableRow(0,[](int){return 1;},[](int){return 0;},[](int){return 20;},[&](int row){visited.push_back(row);});
    check(visited.isEmpty(),"zero-height viewport has no row work");
    std::printf("PASS %d pure QtCore freeze-row sync checks\n",checks);
}
