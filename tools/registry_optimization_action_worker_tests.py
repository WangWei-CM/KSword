"""Run production optimization value executors and action/cancel methods on QtCore fake hosts."""
import argparse,re,subprocess,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--ui-dir',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);source=a.source.read_text(encoding='utf-8-sig')
def function(name):
 pattern=r'(?:bool|void|QString|REGSAM|quint64|QByteArray)\s+'+re.escape(name)+r'\('
 match=re.search(pattern,source);assert match,name
 brace=source.index('{',match.start());depth=1;end=brace+1
 while depth:
  if source[end]=='{':depth+=1
  elif source[end]=='}':depth-=1
  end+=1
 return source[match.start():end]+'\n'
helpers='\n'.join(function(x) for x in ['jsonString','actionRegistryViewFlags','parseRegistryType','parseHexInteger','hexStringToBytes','writeUnsignedLittleEndian','readUnsignedLittleEndian','stringToUtf16RegistryData','registryDataFromJsonText','shouldSkipActionError'])
methods='\n'.join(function('RegistryOptimizationPage::'+x) for x in ['executeAction','describeActionResult','executeRegistryWriteAction','executeRegistryDeleteAction','executeRegistryMoveAction','executeServiceStartAction','executeExplorerNotifyAction','submitOrdinaryAction','acceptOrdinaryAction','cancelStateApply'])
prefix=r"""
#include <Windows.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QStringList>
#include <QVector>
#include "AsyncOperation.h"
#include "AsyncCleanupRegistry.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdio>
#include <cstdlib>
int checkCount=0;void check(bool value){++checkCount;if(!value){std::fprintf(stderr,"FAIL %d\n",checkCount);std::abort();}}
struct RegistryAccessContext {int viewBits=0;bool useR0=false;};
struct RegistryValueState {QString name;quint32 type=0;QByteArray data;bool exists=false;bool complete=true;quint32 requiredBytes=0;};
RegistryValueState fakeLive;QString fakePath,fakeName,fakeTarget,fakeNewName;RegistryAccessContext fakeContext;quint32 fakeType=0;QByteArray fakeBytes;bool fakeIsValue=false;
int fakeReads=0,fakeWrites=0,fakeDeletes=0,fakeMoves=0,fakeBroadcasts=0,fakeCloses=0;bool fakeReadOk=true,fakeNativeOk=true,fakeCom=false;
std::mutex ioMutex;std::condition_variable ioWake;bool blockWrite=false,released=false;std::atomic<bool> writeEntered{false};
class RegistryWorkbenchAccess {public:static bool read(const QString&path,const QString&name,const RegistryAccessContext& context,RegistryValueState* state,QString*error){++fakeReads;fakePath=path;fakeName=name;fakeContext=context;if(!fakeReadOk){*error=QStringLiteral("read failed");return false;}*state=fakeLive;return true;}};
namespace ks::registry::optimization {
bool write(const QString&path,const QString&name,quint32 type,const QByteArray&bytes,const RegistryValueState&expected,const RegistryAccessContext&context,QString*error){
 {std::unique_lock<std::mutex>lock(ioMutex);writeEntered.store(true);if(blockWrite)ioWake.wait(lock,[]{return released;});}
 if(expected.exists!=fakeLive.exists||expected.type!=fakeLive.type||expected.data!=fakeLive.data){*error=QStringLiteral("baseline conflict");return false;}
 ++fakeWrites;fakePath=path;fakeName=name;fakeType=type;fakeBytes=bytes;fakeContext=context;return fakeNativeOk;
}
bool remove(const QString&path,const QString&name,bool isValue,const RegistryAccessContext&context,QString*){++fakeDeletes;fakePath=path;fakeName=name;fakeIsValue=isValue;fakeContext=context;return fakeNativeOk;}
bool move(const QString&path,const QString&name,const QString&target,const QString&newName,bool isValue,const RegistryAccessContext&context,QString*){++fakeMoves;fakePath=path;fakeName=name;fakeTarget=target;fakeNewName=newName;fakeIsValue=isValue;fakeContext=context;return fakeNativeOk;}
}
QString winErrorText(LONG value){return QString::number(value);}
SC_HANDLE fakeManager(LPCWSTR,LPCWSTR,DWORD access){check(access==SC_MANAGER_CONNECT);return reinterpret_cast<SC_HANDLE>(11);}
SC_HANDLE fakeService(SC_HANDLE manager,LPCWSTR name,DWORD access){check(manager==reinterpret_cast<SC_HANDLE>(11)&&access==SERVICE_CHANGE_CONFIG);fakeName=QString::fromWCharArray(name);return reinterpret_cast<SC_HANDLE>(12);}
BOOL fakeChange(SC_HANDLE service,DWORD first,DWORD start,DWORD third,LPCWSTR,LPCWSTR,LPDWORD,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR){check(service==reinterpret_cast<SC_HANDLE>(12)&&first==SERVICE_NO_CHANGE&&third==SERVICE_NO_CHANGE);fakeType=start;return fakeNativeOk;}
BOOL fakeClose(SC_HANDLE){++fakeCloses;return TRUE;}
#define OpenSCManagerW fakeManager
#define OpenServiceW fakeService
#define ChangeServiceConfigW fakeChange
#define CloseServiceHandle fakeClose
struct ExplorerNotifyOutcome {bool succeeded=false;QString errorTextValue;};
ExplorerNotifyOutcome runExplorerNotifyBroadcast(const QJsonObject&,bool initialize){++fakeBroadcasts;fakeCom=initialize;return {fakeNativeOk,QStringLiteral("broadcast failed")};}
namespace ks::ui {struct FieldDocument {QStringList nodes;FieldDocument&field(const QString&name,const QString&value,bool=false){nodes<<name<<value;return *this;}};}
struct DetailView {void setDocument(const ks::ui::FieldDocument&) {}};
constexpr auto kStateApplyGenerationProperty="registry_fixture_generation";
class RegistryOptimizationPage:public QObject {public:
 struct OrdinaryActionResult {bool ok=false;bool restartExplorer=false;ks::ui::FieldDocument details;};
 std::unique_ptr<ks::ui::AsyncOperation>m_ordinaryOperation;bool m_stateApplyInProgress=false,m_ordinaryReceipt=false,m_stateApplyCancelRequested=false,m_deferredProfileReload=false,m_deferredItemRebuild=false,m_stateApplyAllOk=true,m_stateApplyRestartExplorer=false,controls=true;
 QString m_stateApplyCancelReason,m_stateApplyZipDestinationPath,status;int m_stateApplyTableRow=0,m_stateApplyNextActionIndex=0,continued=0,reloads=0,rebuilds=0;
 QVector<QJsonObject>m_stateApplyActions;QJsonObject m_stateApplyActiveAction;ks::ui::FieldDocument m_stateApplyDetailLines;DetailView detail;DetailView*m_detailText=&detail;QProcess*m_stateApplyProcess=nullptr;QTemporaryDir*m_stateApplyTemporaryDir=nullptr;
 RegistryOptimizationPage(){m_ordinaryOperation=std::make_unique<ks::ui::AsyncOperation>(this);m_ordinaryOperation->setStateChangedCallback([this]{if(m_stateApplyInProgress&&!m_ordinaryOperation->isBusy()&&!m_ordinaryReceipt)cancelStateApply(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));});}
 ~RegistryOptimizationPage()override{m_ordinaryOperation->close();}
 void setApplyControlsEnabled(bool value){controls=value;}void updateStatusText(const QString&value){status=value;}void loadOptimizationProfile(){++reloads;}void rebuildItemTable(){++rebuilds;}void continueStateApply(){++continued;}
 static bool executeAction(const QJsonObject&,ks::ui::FieldDocument*,bool*,const RegistryValueState* =nullptr);
 static bool describeActionResult(const QJsonObject&,bool,const QString&,ks::ui::FieldDocument*,bool*);
 static bool executeRegistryWriteAction(const QJsonObject&,QString*,const RegistryValueState* =nullptr);
 static bool executeRegistryDeleteAction(const QJsonObject&,QString*);static bool executeRegistryMoveAction(const QJsonObject&,QString*);static bool executeServiceStartAction(const QJsonObject&,QString*);static bool executeExplorerNotifyAction(const QJsonObject&,QString*);
 void submitOrdinaryAction(const QJsonObject&,const RegistryValueState* =nullptr);void acceptOrdinaryAction(const OrdinaryActionResult&,quint64);void cancelStateApply(const QString&reasonText=QString());
};
"""
tests=r"""
void reset(){fakeLive={};fakeLive.exists=true;fakeLive.type=REG_DWORD;fakeLive.data=QByteArray::fromHex("01000000");fakeReads=fakeWrites=fakeDeletes=fakeMoves=fakeBroadcasts=fakeCloses=0;fakeReadOk=fakeNativeOk=true;blockWrite=released=false;writeEntered.store(false);fakeCom=false;}
QJsonObject action(QString family){return {{QStringLiteral("action_family"),family},{QStringLiteral("Key"),QStringLiteral("HKCU\\测试 空格")},{QStringLiteral("Value"),QString()},{QStringLiteral("Wow64"),QStringLiteral("True")},{QStringLiteral("Type"),QStringLiteral("REG_DWORD")},{QStringLiteral("Data"),QStringLiteral("FF")}};}
void until(const std::function<bool()>&done){QElapsedTimer timer;timer.start();while(!done()&&timer.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(1);}check(done());}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QString error;
 reset();auto write=action(QStringLiteral("RegWrite"));check(RegistryOptimizationPage::executeRegistryWriteAction(write,&error));check(fakeReads==1&&fakeWrites==1&&fakeContext.viewBits==32&&!fakeContext.useR0);check(fakeName.isEmpty()&&fakeBytes==QByteArray::fromHex("ff000000"));
 reset();auto captured=fakeLive;check(RegistryOptimizationPage::executeRegistryWriteAction(write,&error,&captured));check(fakeReads==0&&fakeWrites==1);
 reset();captured=fakeLive;fakeLive.data=QByteArray::fromHex("02000000");check(!RegistryOptimizationPage::executeRegistryWriteAction(write,&error,&captured));check(fakeWrites==0&&error==QStringLiteral("baseline conflict"));
 reset();auto bitwise=write;bitwise.insert(QStringLiteral("Operator"),QStringLiteral("|"));bitwise.insert(QStringLiteral("Data"),QStringLiteral("80"));check(RegistryOptimizationPage::executeRegistryWriteAction(bitwise,&error));check(fakeBytes==QByteArray::fromHex("81000000"));
 reset();fakeLive.type=REG_BINARY;check(!RegistryOptimizationPage::executeRegistryWriteAction(bitwise,&error));check(fakeWrites==0);
 reset();auto binary=write;binary.insert(QStringLiteral("Type"),QStringLiteral("REG_BINARY"));binary.insert(QStringLiteral("Data"),QStringLiteral("FF 80 00"));binary.insert(QStringLiteral("Operator"),QStringLiteral("|"));fakeLive.type=REG_BINARY;fakeLive.data=QByteArray::fromHex("01");check(RegistryOptimizationPage::executeRegistryWriteAction(binary,&error));check(fakeBytes==QByteArray::fromHex("ff8000"));
 reset();auto deletion=action(QStringLiteral("RegDelete"));check(RegistryOptimizationPage::executeRegistryDeleteAction(deletion,&error));check(fakeDeletes==1&&fakeIsValue&&fakeName.isEmpty()&&fakeContext.viewBits==32);deletion.remove(QStringLiteral("Value"));check(RegistryOptimizationPage::executeRegistryDeleteAction(deletion,&error));check(!fakeIsValue);
 reset();auto movement=action(QStringLiteral("RegMove"));movement.insert(QStringLiteral("NewValue"),QStringLiteral(" New "));check(RegistryOptimizationPage::executeRegistryMoveAction(movement,&error));check(fakeTarget==movement.value(QStringLiteral("Key")).toString()&&fakeNewName==QStringLiteral("New")&&fakeIsValue&&fakeContext.viewBits==32);
 reset();auto service=action(QStringLiteral("SetServiceStart"));service.insert(QStringLiteral("Name"),QStringLiteral(" Service "));service.insert(QStringLiteral("Type"),QStringLiteral("3"));check(RegistryOptimizationPage::executeServiceStartAction(service,&error));check(fakeName==QStringLiteral("Service")&&fakeType==3&&fakeCloses==2);
 reset();auto explorer=action(QStringLiteral("ExplorerNotify"));explorer.insert(QStringLiteral("Type"),QStringLiteral("AssocChanged"));check(RegistryOptimizationPage::executeExplorerNotifyAction(explorer,&error));check(fakeBroadcasts==1&&fakeCom);
 reset();ks::ui::FieldDocument details;bool restart=false;auto unknown=action(QStringLiteral("Unknown"));unknown.insert(QStringLiteral("SkipError"),QStringLiteral("2"));unknown.insert(QStringLiteral("activate_restart"),QStringLiteral("Explorer"));check(RegistryOptimizationPage::executeAction(unknown,&details,&restart));check(details.nodes.contains(QStringLiteral("跳过"))&&restart);
 reset();RegistryOptimizationPage page;page.m_stateApplyInProgress=true;page.controls=false;page.setProperty(kStateApplyGenerationProperty,qulonglong(9));page.m_stateApplyActions={write,write};page.m_stateApplyActiveAction=write;blockWrite=true;int beats=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,[&]{++beats;});heartbeat.start(1);page.submitOrdinaryAction(write);until([]{return writeEntered.load();});until([&]{return beats>=12;});check(ks::ui::AsyncCleanupRegistry::global().pending()==1);page.cancelStateApply(QStringLiteral("用户已取消应用。"));check(page.m_stateApplyInProgress&&page.m_stateApplyCancelRequested&&!page.controls&&page.m_stateApplyNextActionIndex==0);page.m_deferredItemRebuild=true;
 {std::lock_guard<std::mutex>lock(ioMutex);released=true;ioWake.notify_all();}until([&]{return !page.m_stateApplyInProgress;});until([&]{return page.rebuilds==1;});check(fakeWrites==1&&page.continued==0&&page.controls);check(page.m_stateApplyDetailLines.nodes.contains(QStringLiteral("成功"))&&page.m_stateApplyDetailLines.nodes.contains(QStringLiteral("已取消")));check(ks::ui::AsyncCleanupRegistry::global().pending()==0);
 reset();auto*closing=new RegistryOptimizationPage;closing->m_stateApplyInProgress=true;closing->m_stateApplyActiveAction=write;closing->setProperty(kStateApplyGenerationProperty,qulonglong(1));blockWrite=true;closing->submitOrdinaryAction(write);until([]{return writeEntered.load();});delete closing;check(ks::ui::AsyncCleanupRegistry::global().pending()==1);{std::lock_guard<std::mutex>lock(ioMutex);released=true;ioWake.notify_all();}until([]{return ks::ui::AsyncCleanupRegistry::global().pending()==0;});check(fakeWrites==1);
 heartbeat.stop();std::printf("registry_optimization_action_workers: %d checks PASS;timerbeats=%d;all registry/SCM/broadcast backends fake;QtCore only\n",checkCount,beats);return 0;}
"""
(a.out/'fixture.cpp').write_text(prefix+helpers+methods+tests,encoding='utf-8')
for name in ['AsyncOperation.h','AsyncOperation.cpp','AsyncUiDispatcher.h','AsyncCleanupRegistry.h']:shutil.copy2(a.ui_dir/name,a.out/name)
qt=Path('D:/Software/Qt/6.9.3/msvc2022_64');cmd=a.out/'test.cmd';cmd.write_text('\n'.join(['@echo off','call "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%',f'cl.exe /nologo /std:c++20 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /wd4100 /external:W0 /I"{a.out.resolve()}" /external:I"{qt / "include"}" /external:I"{qt / "include/QtCore"}" "{(a.out / "fixture.cpp").resolve()}" "{(a.out / "AsyncOperation.cpp").resolve()}" /Fo"{a.out.resolve().as_posix()}/" /Fe"{(a.out / "test.exe").resolve()}" /link /LIBPATH:"{qt / "lib"}" Qt6Core.lib','if errorlevel 1 exit /b %errorlevel%',f'set "PATH={qt / "bin"};%PATH%"',f'"{(a.out / "test.exe").resolve()}"'])+'\n',encoding='utf-8')
r=subprocess.run(['cmd.exe','/d','/c',str(cmd.resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace');(a.out/'test.log').write_text(r.stdout,encoding='utf-8');print(r.stdout);raise SystemExit(r.returncode)
