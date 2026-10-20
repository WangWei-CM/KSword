// Actual QtCore value model/projection only; no widgets or system collectors.
#include "../Ksword5.1/Ksword5.1/UI/ImmutableValueTableModel.h"
#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QJsonObject>
#include <QPersistentModelIndex>
#include <cstdio>
#include <thread>
int main(int argc,char**argv)
{
    QCoreApplication app(argc,argv);using namespace ks::ui;
    int checks=0,failures=0;const auto expect=[&](bool ok,const char*what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}};
    QVector<ImmutableTableRow> rows;for(int i=0;i<100000;++i){ImmutableTableRow row;row.key=QString("SID:%1").arg(i);row.cells={QString("name%1").arg(i,6,10,QLatin1Char('0')),QString::number(i),i%2?"disabled":"enabled"};row.roles.insert(Qt::UserRole,row.key);row.roles.insert(Qt::UserRole+1,i%2!=0);rows.push_back(std::move(row));}
    const auto source=MakeImmutableTableSource(std::move(rows));expect(source&&source->rows.size()==100000&&source->byKey.size()==100000,"complete100k canonical values");
    auto last=ProjectImmutableTable(source,"name099999",-1,Qt::AscendingOrder,"SID:99999",[]{return false;});expect(last&&last->rows.size()==1&&last->source==source&&last->currentRow==0,"last row full-source filter and selected identity");
    auto hidden=ProjectImmutableTable(source,"name000000",-1,Qt::AscendingOrder,"SID:99999",[]{return false;});expect(hidden->currentRow==-1&&hidden->source->byKey.contains("SID:99999"),"temporarily hidden current identity retained canonically");
    auto clear=ProjectImmutableTable(source,{},0,Qt::DescendingOrder,"SID:99999",[]{return false;});expect(clear->rows.size()==100000&&clear->rows.front()==99999&&clear->currentRow==0,"worker sort complete source and restore selected canonical identity");
    auto ascending=ProjectImmutableTable(source,"ENABLED",0,Qt::AscendingOrder,"SID:0",[]{return false;});expect(ascending->rows.size()==50000&&ascending->currentRow==0,"case insensitive filter over complete formatted cells");
    int cancelChecks=0;expect(!ProjectImmutableTable(source,{},-1,Qt::AscendingOrder,{},[&]{return ++cancelChecks>1;}),"obsolete query cooperatively cancelled");
    ImmutableValueTableModel model(3,[](int column){return QString("header%1").arg(column);});QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::Fatal);
    auto previous=model.replace(last);expect(!previous&&model.rowCount()==1&&model.columnCount()==3,"stable Qt model owns immutable projection");
    expect(model.index(0,0).data(Qt::UserRole).toString()=="SID:99999"&&model.index(0,0).data(Qt::UserRole+1).toBool(),"SID and disabled roles exact");
    expect(!model.index(0,1).data(Qt::UserRole).isValid()&&model.index(0,1).data(Qt::ToolTipRole).toString()=="99999","account roles first-column only; tooltip exact");
    QPersistentModelIndex old=model.index(0,0);auto retired=model.replace(clear);expect(retired==last&&!old.isValid()&&model.rowCount()==100000,"reset invalidates stale indexes while immutable retired values remain valid");
    expect(model.headerData(1,Qt::Horizontal,Qt::DisplayRole).toString()=="header1","header producer explicit");
    bool sortRequested=false;model.sortRequested=[&](int column,Qt::SortOrder order){sortRequested=column==2&&order==Qt::DescendingOrder;};model.sort(2,Qt::DescendingOrder);expect(sortRequested,"GUI sort request does not sort the Qt model synchronously");
    ImmutableTableRow process;process.key="PID:42@creation:9";process.cells={"42","process"};const QJsonObject identity{{"pid",42},{"creationTime100ns","9"}};process.roles.insert(Qt::UserRole,identity);process.rolesInAllColumns=true;
    auto reused=process;reused.key="PID:42@creation:10";reused.roles[Qt::UserRole]=QJsonObject{{"pid",42},{"creationTime100ns","10"}};auto identities=MakeImmutableTableSource({process,reused});auto identityProjection=ProjectImmutableTable(identities,{},-1,Qt::AscendingOrder,process.key,[]{return false;});model.replace(identityProjection);
    expect(identities->rows.size()==2&&identityProjection->currentRow==0&&model.index(0,1).data(Qt::UserRole).toJsonObject()==identity,"samePID differentcreation identities retained; roles everycolumn");
    expect(!MakeImmutableTableSource({process,process}),"duplicate canonical identity rejected");process.key.clear();expect(!MakeImmutableTableSource({process}),"empty identity rejected");
    auto empty=ProjectImmutableTable(MakeImmutableTableSource({}),{},-1,Qt::AscendingOrder,{},[]{return false;});model.replace(empty);expect(model.rowCount()==0&&empty->source,"explicit empty source distinct from unavailable");
    bool workerOK=false;std::thread worker([&]{auto projection=ProjectImmutableTable(source,"99999",0,Qt::DescendingOrder,"SID:99999",[]{return false;});workerOK=projection&&projection->source->rows.size()==100000&&projection->rows.size()==1;});worker.join();expect(workerOK,"worker pure projection without model/widget access");
    {auto* dying=new ImmutableValueTableModel(3,[](int){return QString();});QPointer<ImmutableValueTableModel> alive(dying);QObject::connect(dying,&QAbstractItemModel::modelAboutToBeReset,&app,[dying]{delete dying;});auto abandoned=dying->replace(last);expect(!alive&&abandoned==last,"aboutToReset deleting model returns incoming value without accessafterdelete");}
    {auto* dying=new ImmutableValueTableModel(3,[](int){return QString();});QPointer<ImmutableValueTableModel> alive(dying);QObject::connect(dying,&QAbstractItemModel::modelReset,&app,[dying]{delete dying;});auto abandoned=dying->replace(last);expect(!alive&&!abandoned,"endReset deleting model returns safely");}
    std::printf("IMMUTABLE_VALUE_TABLE_CHECKS=%d FAILURES=%d\n",checks,failures);return failures?1:0;
}
