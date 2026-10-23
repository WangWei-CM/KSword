"""QtCore-only production legacy-read scheduling with an in-memory delayed backend."""
import argparse,subprocess,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--ui-dir',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
s=a.source.read_text(encoding='utf-8-sig');start=s.index('ks::ui::AsyncOperation& RegistryDock::legacyMutationOperation()');production=s[start:s.index('void RegistryDock::createSubKey()',start)]
fixture=r"""
#include "AsyncOperation.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
#include <QString>
#include <QByteArray>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdio>
#include <cstdlib>
static int checks=0;
void check(bool x){++checks;if(!x){std::fprintf(stderr,"FAIL check %d\n",checks);std::abort();}}
struct RegistryAccessContext {int viewBits=0;bool useR0=false;};
struct RegistryValueState {QString name;quint32 type=0;QByteArray data;bool exists=false;bool complete=true;quint32 requiredBytes=0;};
std::mutex mutex;std::condition_variable wake;bool released=false;bool shouldThrow=false;std::atomic<int> active{0};
QString observedPath,observedName;RegistryAccessContext observedContext;QByteArray mockReadBytes;
class RegistryWorkbenchAccess {public:static bool read(const QString& path,const QString& name,const RegistryAccessContext& context,RegistryValueState* state,QString*){
 {std::unique_lock<std::mutex> lock(mutex);observedPath=path;observedName=name;observedContext=context;++active;wake.wait(lock,[]{return released;});}
 --active;if(shouldThrow)throw 1;state->name=name;state->exists=true;state->type=3;state->data=mockReadBytes;return true;
}};
class RegistryDock:public QObject {public:
 std::unique_ptr<ks::ui::AsyncOperation> m_legacyMutationOperation;bool m_applyingChanges=false,m_legacyMutationReceipt=false;QString status;int updates=0;
 ~RegistryDock()override{if(m_legacyMutationOperation)m_legacyMutationOperation->close();}
 void updatePendingChanges(){++updates;}
 void updateStatusBar(const QString& s){status=s;}
 ks::ui::AsyncOperation& legacyMutationOperation();void finishLegacyMutation();
 void readLegacyValue(const QString&,const QString&,const RegistryAccessContext&,std::function<void(const RegistryValueState&,const QString&,bool)>);
};
"""+production+r"""
void until(const std::function<bool()>& done){QElapsedTimer t;t.start();while(!done()&&t.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(1);}check(done());}
void release(){std::lock_guard<std::mutex> lock(mutex);released=true;wake.notify_all();}
void reset(){check(active.load()==0);std::lock_guard<std::mutex> lock(mutex);released=false;shouldThrow=false;observedPath.clear();observedName.clear();}
int main(int argc,char**argv){QCoreApplication app(argc,argv);mockReadBytes=QByteArray(16*1024*1024,'\0');for(qsizetype i=0;i<mockReadBytes.size();++i)mockReadBytes[i]=static_cast<char>((i*131)%256);
 reset();RegistryDock host;QString path=QStringLiteral("HKEY_CURRENT_USER\\测试 空格"),name;RegistryAccessContext context{32,true};bool applied=false;int heartbeats=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,[&]{++heartbeats;});heartbeat.start(1);
 host.readLegacyValue(path,name,context,[&](const RegistryValueState& value,const QString& error,bool ok){check(!host.m_applyingChanges);check(!host.m_legacyMutationOperation->isBusy());check(ok&&error.isEmpty());check(value.exists&&value.complete&&value.data==mockReadBytes&&value.name.isEmpty());applied=true;});
 path=QStringLiteral("Other");name=QStringLiteral("Other");context={64,false};until([]{return active.load()==1;});check(host.m_applyingChanges);until([&]{return heartbeats>=12;});check(!applied);
 {std::lock_guard<std::mutex> lock(mutex);check(observedPath==QStringLiteral("HKEY_CURRENT_USER\\测试 空格"));check(observedName.isEmpty());check(observedContext.viewBits==32&&observedContext.useR0);}
 release();until([&]{return applied;});check(host.updates==2);
 reset();int closedCallbacks=0;auto* closing=new RegistryDock;closing->readLegacyValue(QStringLiteral("Frozen"),QStringLiteral("N"),{0,false},[&](const RegistryValueState&,const QString&,bool){++closedCallbacks;});until([]{return active.load()==1;});QElapsedTimer closingTimer;closingTimer.start();delete closing;const auto closeElapsed=closingTimer.elapsed();release();until([]{return active.load()==0;});for(int i=0;i<10;++i){QCoreApplication::processEvents();QThread::msleep(1);}check(closedCallbacks==0);
 reset();shouldThrow=true;RegistryDock failure;bool failedCallback=false;failure.readLegacyValue(QStringLiteral("Throw"),QString(),{0,false},[&](const RegistryValueState&,const QString&,bool){failedCallback=true;});until([]{return active.load()==1;});release();until([&]{return !failure.m_applyingChanges;});check(!failedCallback);check(failure.status==QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
 heartbeat.stop();std::printf("registry_legacy_read_service: %d checks PASS;fullbytes=%lld heartbeats=%d close_ms=%lld;QtCore only,fake backend\n",checks,static_cast<long long>(mockReadBytes.size()),heartbeats,static_cast<long long>(closeElapsed));return 0;}
"""
(a.out/'fixture.cpp').write_text(fixture,encoding='utf-8')
for name in ['AsyncOperation.h','AsyncOperation.cpp','AsyncUiDispatcher.h','AsyncCleanupRegistry.h','PerfTrace.h']:shutil.copy2(a.ui_dir/name,a.out/name)
qt=Path('D:/Software/Qt/6.9.3/msvc2022_64');cmd=a.out/'test.cmd';cmd.write_text('\n'.join(['@echo off','call "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%',f'cl.exe /nologo /std:c++20 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /external:W0 /I"{a.out.resolve()}" /external:I"{qt / "include"}" /external:I"{qt / "include/QtCore"}" "{(a.out / "fixture.cpp").resolve()}" "{(a.out / "AsyncOperation.cpp").resolve()}" /Fo"{a.out.resolve().as_posix()}/" /Fe"{(a.out / "test.exe").resolve()}" /link /LIBPATH:"{qt / "lib"}" Qt6Core.lib','if errorlevel 1 exit /b %errorlevel%',f'set "PATH={qt / "bin"};%PATH%"',f'"{(a.out / "test.exe").resolve()}"'])+'\n',encoding='utf-8')
r=subprocess.run(['cmd.exe','/d','/c',str(cmd.resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace');(a.out/'test.log').write_text(r.stdout,encoding='utf-8');print(r.stdout);raise SystemExit(r.returncode)
