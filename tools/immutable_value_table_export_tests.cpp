#include "../Ksword5.1/Ksword5.1/UI/ImmutableValueTableModel.h"
#include <QCoreApplication>
#include <cstdio>
#include <thread>
int main(int argc,char**argv){QCoreApplication app(argc,argv);using namespace ks::ui;int checks=0,failures=0;auto expect=[&](bool ok,const char*n){++checks;if(!ok){++failures;std::printf("FAIL %s\n",n);}};
QVector<ImmutableTableRow> rows;for(int i=0;i<100000;++i){ImmutableTableRow row;row.key=QString::number(i);row.cells={QString("%1").arg(i,6,10,QLatin1Char('0')),QString("value%1").arg(i),i%2?"odd":"even"};rows.append(std::move(row));}auto source=MakeImmutableTableSource(std::move(rows));
auto projection=ProjectImmutableTable(source,"odd",0,Qt::DescendingOrder,"99999",[]{return false;},{"99999","1"});ImmutableValueTableModel model(3,[](int c){return QString::number(c);});model.replace(projection,17);
auto retained=model.projection();const auto version=model.version();model.replace({},18);expect(retained==projection&&version==17&&model.version()==18,"O1 provider pair retains exact old publication identity while model changes");
bool workerOK=false;std::thread worker([&]{quint64 cellCount=0;for(int r=0;r<retained->rows.size();++r)for(int c=0;c<3;++c){const auto value=ImmutableTableDisplayCell(*retained,r,c);if(!value.isEmpty())++cellCount;}workerOK=cellCount==150000&&ImmutableTableDisplayCell(*retained,0,1)=="value99999"&&ImmutableTableDisplayCell(*retained,49999,1)=="value1";});worker.join();expect(workerOK,"stateless worker cell port serializes all50000filtered sorted rows including never-visiblelastrow");
expect(retained->source->rows.size()==100000&&retained->selectedRanges==QVector<QPair<int,int>>{{0,0},{49999,49999}},"canonical source and selected scope are complete under displayed projection order");
expect(ImmutableTableDisplayCell(*retained,-1,0).isEmpty()&&ImmutableTableDisplayCell(*retained,50000,0).isEmpty()&&ImmutableTableDisplayCell(*retained,0,9).isEmpty(),"cell port rejects invalid row and column without borrowed model access");
ImmutableTableRow role;role.key="role";role.cells={"raw","value"};role.columnRoles[1][Qt::DisplayRole]="shown";auto roles=ProjectImmutableTable(MakeImmutableTableSource({role}),{},-1,Qt::AscendingOrder,{},[]{return false;});model.replace(roles,19);expect(ImmutableTableDisplayCell(*roles,0,1)==model.index(0,1).data().toString()&&ImmutableTableDisplayCell(*roles,0,1)=="shown","cell port preserves explicit per-column DisplayRole wording");
std::printf("IMMUTABLE_EXPORT_ADAPTER_CHECKS=%d FAILURES=%d\n",checks,failures);return failures?1:0;}
