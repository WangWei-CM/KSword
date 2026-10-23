// Actual shared production helpers/model only; no widgets/collectors.
#include "../Ksword5.1/Ksword5.1/WindowDock/WindowImmutableTableSupport.h"
#include <QCoreApplication>
#include <QAbstractItemModelTester>
#include <cstdio>
#include <thread>
struct FrozenTranslator { QString displayText(const QString& cell) const { return QString("translated:")+cell; } };
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv); int checks=0,failures=0;
    auto expect=[&](bool ok,const char*name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}};
    QVector<QStringList> rows;
    for(int i=0;i<65536;++i){QStringList row;for(int c=0;c<15;++c)row.append(QString("%1:%2").arg(i).arg(c));row[2]=QString::number(i%22);rows.append(std::move(row));}
    ks::ui::ImmutableTableSource source;
    std::thread worker([&]{source=ks::window::makeWindowTableSource(std::move(rows),{0,2,4,5});});worker.join();
    expect(source&&source->rows.size()==65536&&source->byKey.size()==65536,"all65536 canonical GUI handle slots retained");
    expect(source->rows.last().cells.size()==15&&source->rows.last().cells.last()=="65535:14","last complete raw field survives worker construction");
    const auto selected=source->rows.last().key;
    auto match=ks::ui::ProjectImmutableTable(source,"65535:13 65535:14",-1,Qt::AscendingOrder,selected,[]{return false;},{selected});
    expect(match&&match->rows.size()==1&&match->currentRow==0,"never exposed last row and cross-column phrase searched from cached full document");
    auto typed=ks::ui::ProjectImmutableTable(source,{},0,Qt::DescendingOrder,selected,[]{return false;},{selected},[](const auto&r){return r.cells.value(2).toInt()==16;});
    int expectedType=0;for(const auto&r:source->rows)if(r.cells.value(2)=="16")++expectedType;
    expect(typed->rows.size()==expectedType&&typed->source==source,"TypeId exact numeric predicate runs over full canonical source");
    auto restored=ks::ui::ProjectImmutableTable(source,{},0,Qt::AscendingOrder,selected,[]{return false;},{selected});
    expect(restored->rows.size()==65536&&restored->currentRow>=0&&restored->selectedRanges.size()==1,"clear search and type restores complete rows/selected identity");
    ks::ui::ImmutableValueTableModel model(15,[](int c){return QString("column%1").arg(c);});
    QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::Fatal);model.replace(restored);
    qsizetype cells=0;bool sawLast=false;for(int r=0;r<model.rowCount();++r)for(int c=0;c<model.columnCount();++c){const auto v=model.index(r,c).data().toString();++cells;if(v=="65535:14")sawLast=true;}
    expect(cells==983040&&sawLast,"full logical model serialization sees983040cells including offscreenlastfield; no viewport cap");
    QVector<QStringList> duplicate{{"same","row"},{"same","row"},{"","diagnostic"}};
    auto repeats=ks::window::makeWindowTableSource(std::move(duplicate),{0});
    expect(repeats&&repeats->rows.size()==3&&repeats->rows[0].key!=repeats->rows[1].key,"repeated partial diagnostic records never collapse");
    QVector<QStringList> reverse; reverse.append(source->rows.last().cells);reverse.append(source->rows.first().cells);
    auto reordered=ks::window::makeWindowTableSource(std::move(reverse),{0,2,4,5});
    expect(reordered->rows[0].key==selected,"canonical object identity independent of source order/filter");
    const auto formatted=ks::window::formatWindowTableSource(source,FrozenTranslator{});
    expect(formatted->rows.size()==source->rows.size()&&formatted->rows.last().key==selected&&formatted->byKey==source->byKey,"worker language projection preserves complete canonical identity map");
    expect(formatted->rows.last().cells.last()=="translated:65535:14"&&formatted->rows.last().searchText.contains("translated:65535:13 translated:65535:14"),"every formatted cell and joined search uses frozen translator");
    std::printf("WINDOW_TABLE_VALUE_CHECKS=%d FAILURES=%d\n",checks,failures);return failures?1:0;
}
