"""Run actual external optimization worker/session with memory files and a fake child process."""
import argparse,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--header',type=Path,required=True);p.add_argument('--ui-dir',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
source=a.source.read_text(encoding='utf-8-sig')
def function(name):
    m=re.search(r'(?:RegistryOptimizationPage::ExternalActionResult|QStringList|QString|bool|void)\s+'+re.escape(name)+r'\(',source);assert m,name
    brace=source.index('{',m.start());depth=1;end=brace+1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[m.start():end]+'\n'
prefix=r'''
#include <Windows.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QProcess>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include "AsyncOperation.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <source_location>
#include <cstdio>
#include <cstdlib>
std::atomic<int> checks{0};void check(bool ok,std::source_location loc=std::source_location::current()){const auto count=checks.fetch_add(1)+1;if(!ok){std::fprintf(stderr,"FAIL %d line %u\n",count,loc.line());std::abort();}}
using RealDir=QDir;
QMap<QString,QByteArray> files;QSet<QString> directories;std::atomic<int> removals{0},starts{0},kills{0},processObjects{0},processDestructions{0};
std::atomic_bool childAlive{false},releaseChild{true},acknowledgeKill{true},killRequested{false};
bool startupOk=true,tempOk=true,mkdirOk=true,writeOk=true,commitOk=true;int processExit=0,removeFailures=0;bool crashExit=false;QString processError;QString launchedProgram;QStringList launchedArguments;QByteArray stderrBytes;QByteArray extractedBytes;int tempSequence=0; qint64 maxWrite=0;std::function<void()> afterRead;
struct MockDir
{
 QString p;explicit MockDir(const QString&v=QString()):p(RealDir::cleanPath(v)){}bool exists()const{return directories.contains(p);}bool removeRecursively(){check(!childAlive.load()&&processObjects.load()==0);++removals;if(removeFailures){--removeFailures;return false;}for(auto it=files.begin();it!=files.end();)if(it.key().startsWith(p+QLatin1Char('/')))it=files.erase(it);else++it;directories.remove(p);return true;}
 bool mkpath(const QString&){if(!mkdirOk)return false;directories.insert(p);return true;}QString absolutePath()const{return p;}QString filePath(const QString&name)const{return RealDir(p).filePath(name);}static QString cleanPath(const QString&v){return RealDir::cleanPath(v);}static QString fromNativeSeparators(const QString&v){return RealDir::fromNativeSeparators(v);}static QString toNativeSeparators(const QString&v){return RealDir::toNativeSeparators(v);}static QString currentPath(){return QStringLiteral("/cwd");}
};
struct MockInfo {QString p;explicit MockInfo(const QString&v):p(RealDir::fromNativeSeparators(v)){}static bool exists(const QString&v){return files.contains(RealDir::fromNativeSeparators(v));}MockDir dir()const{return MockDir(p.left(p.lastIndexOf(QLatin1Char('/'))));}};
struct MockTemp {QString p;bool autoRemove=true;MockTemp():p(QStringLiteral("/temporary/%1").arg(++tempSequence)){if(tempOk)directories.insert(p);}bool isValid()const{return tempOk;}QString path()const{return p;}void setAutoRemove(bool v){autoRemove=v;}~MockTemp(){if(autoRemove&&tempOk)MockDir(p).removeRecursively();}};
struct MockFile {QString p;QByteArray bytes;qsizetype at=0;explicit MockFile(const QString&v):p(v){}bool open(QIODevice::OpenMode){if(!files.contains(p))return false;bytes=files[p];return true;}bool atEnd()const{return at>=bytes.size();}QByteArray read(qint64 n){const auto value=bytes.mid(at,n);at+=value.size();if(afterRead)afterRead();return value;}QFileDevice::FileError error()const{return QFileDevice::NoError;}};
struct MockSave {QString p;QByteArray bytes;explicit MockSave(const QString&v):p(v){}void setDirectWriteFallback(bool value){check(!value);}bool open(QIODevice::OpenMode){return writeOk;}qint64 write(const char*data,qint64 n){if(!writeOk)return -1;const auto accepted=maxWrite?std::min(n,maxWrite):n;bytes.append(data,accepted);return accepted;}bool commit(){if(!commitOk)return false;files[p]=bytes;return true;}void cancelWriting(){bytes.clear();}};
struct MockProcess
{
 enum ProcessState{NotRunning,Starting,Running};enum ExitStatus{NormalExit,CrashExit};ProcessState status=NotRunning;QString program;QStringList arguments;
 MockProcess(){++processObjects;}~MockProcess(){check(status==NotRunning);--processObjects;++processDestructions;}
 void setProgram(const QString&v){program=v;}void setArguments(const QStringList&v){arguments=v;}void start(){++starts;launchedProgram=program;launchedArguments=arguments;killRequested.store(false);status=startupOk?Running:NotRunning;childAlive.store(startupOk);}bool waitForStarted(int timeout){check(timeout==-1);return startupOk;}QString errorString()const{return processError;}ProcessState state()const{return status;}void kill(){++kills;killRequested.store(true);}
 bool waitForFinished(int timeout){check(timeout==50);QThread::msleep(1);if((killRequested.load()&&acknowledgeKill.load())||releaseChild.load()){status=NotRunning;childAlive.store(false);if(!killRequested.load()&&program==QStringLiteral("tar.exe")&&processExit==0&&!crashExit){const auto root=RealDir::fromNativeSeparators(arguments.at(3));files[RealDir(root).filePath(arguments.at(4))]=extractedBytes;}return true;}return false;}
 int exitCode()const{return processExit;}ExitStatus exitStatus()const{return crashExit?CrashExit:NormalExit;}QByteArray readAllStandardError(){return stderrBytes;}
};
#define QDir MockDir
#define QFileInfo MockInfo
#define QFile MockFile
#define QSaveFile MockSave
#define QTemporaryDir MockTemp
#define QProcess MockProcess
'''
prefix+='\n#include "'+str(a.header.resolve()).replace('\\','/')+'"\n'
prefix+=r'''
QString expandEnvironmentPath(const QString&v){return v;}
namespace ks::ui {struct FieldDocument{QStringList nodes;FieldDocument&field(const QString&name,const QString&value,bool=false){nodes<<name<<value;return *this;}};bool promptForPrivilegeFailure(QObject*,const QString&,const QString&){return false;}}
struct DetailView {void setDocument(const ks::ui::FieldDocument&){}};
constexpr auto kStateApplyGenerationProperty="registry_external_test_generation";
class RegistryOptimizationPage:public QObject {public:
 struct ExternalActionResult{bool ok=false,canceled=false,published=false;QString error;};
 std::unique_ptr<ks::ui::AsyncOperation>m_ordinaryOperation;std::shared_ptr<ks::registry::external::Session>m_externalSession;bool m_stateApplyInProgress=true,m_ordinaryReceipt=false,m_stateApplyCancelRequested=false,m_deferredProfileReload=false,m_deferredItemRebuild=false,m_stateApplyAllOk=true,m_stateApplyRestartExplorer=false,controls=false;
 QString m_stateApplyCancelReason,status;int m_stateApplyTableRow=0,m_stateApplyNextActionIndex=0,continued=0,reloads=0,rebuilds=0;QVector<QJsonObject>m_stateApplyActions;QJsonObject m_stateApplyActiveAction;ks::ui::FieldDocument m_stateApplyDetailLines;DetailView detail;DetailView*m_detailText=&detail;
 RegistryOptimizationPage(){setProperty(kStateApplyGenerationProperty,qulonglong(9));m_ordinaryOperation=std::make_unique<ks::ui::AsyncOperation>(this);m_ordinaryOperation->setStateChangedCallback([this]{if(m_stateApplyInProgress&&!m_ordinaryOperation->isBusy()&&!m_ordinaryReceipt)cancelStateApply(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));});}
 ~RegistryOptimizationPage()override{if(m_externalSession){m_externalSession->stop();m_externalSession.reset();}m_ordinaryOperation->close();}
 void setApplyControlsEnabled(bool v){controls=v;}void updateStatusText(const QString&v){status=v;}void loadOptimizationProfile(){++reloads;}void rebuildItemTable(){++rebuilds;}void continueStateApply(){++continued;}
 bool startExternalAction(const QJsonObject&,QString*);static ExternalActionResult executeExternalAction(const QJsonObject&,const QString&,const QString&,const std::shared_ptr<ks::registry::external::Session::Work>&);void completePendingAction(bool,const QString&);void cancelStateApply(const QString&=QString());
};
'''
helpers='\n'.join(function(x) for x in ['jsonString','splitZipFileReference','registryOptimizationAssetCandidates','shouldSkipActionError'])
methods='\n'.join(function('RegistryOptimizationPage::'+x) for x in ['startExternalAction','executeExternalAction','completePendingAction','cancelStateApply'])
tests=r'''
void until(const std::function<bool()>&done){QElapsedTimer timer;timer.start();while(!done()&&timer.elapsed()<10000){QCoreApplication::processEvents();QThread::msleep(1);}if(!done())std::fprintf(stderr,"TIMEOUT pending=%zu starts=%d kills=%d alive=%d removals=%d objects=%d\n",ks::ui::AsyncCleanupRegistry::global().pending(),starts.load(),kills.load(),childAlive.load(),removals.load(),processObjects.load());check(done());}
void reset(){check(!childAlive.load()&&processObjects.load()==0&&ks::ui::AsyncCleanupRegistry::global().pending()==0);files.clear();directories.clear();directories.insert(QStringLiteral("/destination"));files[QStringLiteral("/app/profiles/registry_optimization_assets/Config/Data.zip")]=QByteArray("zip");files[RealDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles/registry_optimization_assets/Config/Data.zip"))]=QByteArray("zip");extractedBytes=QByteArray(3*1024*1024+41,'Z');starts=0;kills=0;removals=0;processDestructions=0;releaseChild=acknowledgeKill=true;killRequested=false;startupOk=tempOk=mkdirOk=writeOk=commitOk=true;processExit=removeFailures=0;crashExit=false;processError.clear();stderrBytes.clear();maxWrite=0;afterRead={};}
QJsonObject zip(){return {{QStringLiteral("action_family"),QStringLiteral("FileCreateByZIP")},{QStringLiteral("Path"),QStringLiteral("/destination/output.bin")},{QStringLiteral("ZIPFile"),QStringLiteral("Config/Data.zip/内部 目录/原始.bin")}};}
QJsonObject cmd(){return {{QStringLiteral("action_family"),QStringLiteral("ExplorerNotify")},{QStringLiteral("Type"),QStringLiteral("Cmd")},{QStringLiteral("Cmd"),QStringLiteral("echo original frozen command")}};}
RegistryOptimizationPage::ExternalActionResult direct(const QJsonObject&action){auto session=ks::registry::external::Session::create();check(bool(session));auto work=session->reserve();check(bool(work));const auto result=RegistryOptimizationPage::executeExternalAction(action,QStringLiteral("/app"),QStringLiteral("/cwd"),work);session.reset();work.reset();until([]{return ks::ui::AsyncCleanupRegistry::global().pending()==0;});return result;}
int main(int argc,char**argv){QCoreApplication app(argc,argv);auto&registry=ks::ui::AsyncCleanupRegistry::global();registry.cancelShutdown();QThreadPool::globalInstance()->setMaxThreadCount(1);
 reset();maxWrite=7;auto result=direct(zip());check(result.ok&&result.published&&!result.canceled);check(files[QStringLiteral("/destination/output.bin")]==extractedBytes&&removals==1&&processDestructions==1);check(launchedProgram==QStringLiteral("tar.exe")&&launchedArguments.last()==QStringLiteral("内部 目录/原始.bin"));
 reset();files[QStringLiteral("/destination/output.bin")]=QByteArray("old destination");commitOk=false;result=direct(zip());check(!result.ok&&!result.published&&files[QStringLiteral("/destination/output.bin")]==QByteArray("old destination")&&removals==1);
 reset();files[QStringLiteral("/destination/output.bin")]=QByteArray("old destination");writeOk=false;result=direct(zip());check(!result.ok&&files[QStringLiteral("/destination/output.bin")]==QByteArray("old destination"));
 reset();startupOk=false;processError=QStringLiteral("precise startup failure");result=direct(cmd());check(!result.ok&&result.error==processError&&starts==1&&processDestructions==1);
 reset();processExit=37;stderrBytes=QByteArray(" precise native stderr \n");result=direct(cmd());check(!result.ok&&result.error==QStringLiteral("precise native stderr"));
 reset();result=direct(cmd());check(result.ok&&!result.published&&launchedProgram==QStringLiteral("cmd.exe")&&launchedArguments==QStringList({QStringLiteral("/c"),QStringLiteral("echo original frozen command")}));
 reset();auto invalid=zip();invalid.insert(QStringLiteral("ZIPFile"),QStringLiteral("wrong"));result=direct(invalid);check(!result.ok&&starts==0&&removals==0);
 reset();auto session=ks::registry::external::Session::create();auto work=session->reserve();files[QStringLiteral("/destination/output.bin")]=QByteArray("old destination");afterRead=[runtime=work->runtime()]{runtime->requestStop();};result=RegistryOptimizationPage::executeExternalAction(zip(),QStringLiteral("/app"),QStringLiteral("/cwd"),work);check(!result.ok&&result.canceled&&!result.published&&files[QStringLiteral("/destination/output.bin")]==QByteArray("old destination"));afterRead={};session.reset();work.reset();until([&]{return registry.pending()==0;});
 // Explicit cancel waits for a physical termination acknowledgement before deleting ZIP resources.
 reset();releaseChild=false;acknowledgeKill=false;auto*page=new RegistryOptimizationPage;page->m_stateApplyActiveAction=zip();page->m_stateApplyActions={zip(),cmd()};QString error;check(page->startExternalAction(zip(),&error));until([]{return childAlive.load();});check(registry.pending()==2);int beats=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,[&]{++beats;});heartbeat.start(1);page->cancelStateApply(QStringLiteral("explicit cancel"));until([&]{return kills.load()>0&&beats>8;});check(page->m_stateApplyInProgress&&page->m_stateApplyCancelRequested&&!page->controls&&removals==0&&registry.pending()==2);acknowledgeKill=true;until([&]{return registry.pending()==0&&!page->m_stateApplyInProgress;});check(starts==1&&removals==1&&page->m_stateApplyNextActionIndex==0&&page->continued==0&&!files.contains(QStringLiteral("/destination/output.bin")));delete page;
 // Closing the GUI is nonblocking; no later delivery touches it and the retained resource lease persists.
 reset();releaseChild=false;acknowledgeKill=false;page=new RegistryOptimizationPage;page->m_stateApplyActiveAction=zip();check(page->startExternalAction(zip(),&error));until([]{return childAlive.load();});QElapsedTimer clock;clock.start();delete page;check(clock.elapsed()<50&&registry.pending()==2);until([]{return kills.load()>0;});check(removals==0);acknowledgeKill=true;until([&]{return registry.pending()==0;});check(removals==1&&processObjects==0);
 // Shutdown has a weak-value stop registration, then Cancel admits only a fresh action epoch.
 reset();releaseChild=false;acknowledgeKill=false;page=new RegistryOptimizationPage;page->m_stateApplyActiveAction=zip();check(page->startExternalAction(zip(),&error));until([]{return childAlive.load();});registry.beginShutdown();registry.cancelShutdown();until([]{return kills.load()>0;});check(registry.pending()==2&&removals==0);acknowledgeKill=true;until([&]{return registry.pending()==0&&!page->m_stateApplyInProgress;});delete page;reset();result=direct(cmd());check(result.ok);

 // A close before the one worker slot becomes available must not start a child.
 reset();std::atomic_bool blockerEntered{false},blockerRelease{false},blockerDone{false};QThreadPool::globalInstance()->start(QRunnable::create([&]{blockerEntered=true;while(!blockerRelease.load())QThread::msleep(1);blockerDone=true;}));until([&]{return blockerEntered.load();});page=new RegistryOptimizationPage;page->m_stateApplyActiveAction=zip();check(page->startExternalAction(zip(),&error));check(registry.pending()==2&&starts==0);delete page;blockerRelease=true;until([&]{return blockerDone.load()&&registry.pending()==0;});check(starts==0&&removals==0);
 // Admission frozen between resource reservation and transaction queues no native work.
 reset();session=ks::registry::external::Session::create();work=session->reserve();registry.beginShutdown();ks::ui::AsyncOperation rejected(nullptr);int deliveries=0;check(rejected.submitTransaction<int,int>(1,[](int){return 1;},[&](int,std::uint64_t){++deliveries;return true;},[](std::exception_ptr){check(false);})==0);work.reset();session.reset();until([&]{return registry.pending()==0;});check(starts==0&&deliveries==0&&!ks::registry::external::Session::create());registry.cancelShutdown();
 // Failed directory cleanup retains the exact generated root and only explicit Main Retry may remove it.
 reset();removeFailures=1;session=ks::registry::external::Session::create();work=session->reserve();auto job=session->job();result=RegistryOptimizationPage::executeExternalAction(zip(),QStringLiteral("/app"),QStringLiteral("/cwd"),work);session->stop();session.reset();work.reset();check(job->requestCleanup());until([&]{return job->hasFailed();});check(result.ok&&registry.pending()==1&&removals==1);const auto attempt=job->attemptCount();check(!job->notifyReadyToAttempt());QThreadPool::globalInstance()->waitForDone();check(job->attemptCount()==attempt&&removals==1);job.reset();check(registry.pending()==1);check(ks::ui::RetryRetainedCleanupJobs()==1);until([&]{return registry.pending()==0;});check(removals==2);
 heartbeat.stop();QThreadPool::globalInstance()->setMaxThreadCount(8);std::printf("registry_external_action: %d checks PASS fakeChild=all memoryFiles=all fullZIP=%lld physicalTermination=PASS cancelCloseShutdown=PASS retryOnly=PASS beats=%d GUI=0 realProcess=0 registry=0\n",checks.load(),static_cast<long long>(extractedBytes.size()),beats);return 0;}
'''
fixture=(prefix+helpers+methods+tests).replace('const QString&=QString()', 'const QString& =QString()')
cpp=a.out/'external.cpp';cpp.write_text(fixture,encoding='utf-8');qt=Path('D:/Software/Qt/6.9.3/msvc2022_64');exe=a.out/'external.exe';cmd=a.out/'test.cmd'
lines=['@echo off','call "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%',f'cl.exe /nologo /std:c++20 /Zc:__cplusplus /EHsc /utf-8 /W4 /WX /MD /wd4100 /DUNICODE /D_UNICODE /I"{a.ui_dir.resolve()}" /I"{qt/"include"}" /I"{qt/"include/QtCore"}" "{cpp.resolve()}" "{(a.ui_dir/"AsyncOperation.cpp").resolve()}" /Fo"{a.out.resolve()}/" /Fe"{exe.resolve()}" /link /LIBPATH:"{qt/"lib"}" Qt6Core.lib','if errorlevel 1 exit /b %errorlevel%',f'set "PATH={qt/"bin"};%PATH%"',f'"{exe.resolve()}"']
cmd.write_text('\n'.join(lines)+'\n',encoding='utf-8');result=subprocess.run(['cmd.exe','/d','/c',str(cmd.resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace');(a.out/'test.log').write_text(result.stdout,encoding='utf-8');print(result.stdout);raise SystemExit(result.returncode)
