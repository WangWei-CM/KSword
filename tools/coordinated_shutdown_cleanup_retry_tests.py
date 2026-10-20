"""QtCore-only checked cleanup retry integration; no GUI/native business execution."""
from pathlib import Path
import argparse, os, re, subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--repository-root',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--source-root',type=Path)
p.add_argument('--cleanup-source-root',type=Path)
p.add_argument('--quit-source-root',type=Path)
p.add_argument('--registry-source-root',type=Path)
p.add_argument('--output-dir',type=Path,required=True)
a=p.parse_args();root=a.repository_root.resolve();source=(a.source_root or root).resolve();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
ui=Path('Ksword5.1/Ksword5.1/UI')
for name,other in [('RetainedCleanupJob.h',a.cleanup_source_root),('CoordinatedApplicationQuit.h',a.quit_source_root),('AsyncCleanupRegistry.h',a.registry_source_root)]:
    src=source/ui/name
    if not src.exists() and other:src=other.resolve()/ui/name
    if not src.exists():src=root/ui/name
    (out/name).write_bytes(src.read_bytes())
main=(source/'Ksword5.1/Ksword5.1/MainWindow.cpp').read_text(encoding='utf-8')
match=re.search(r'if \(choice == QMessageBox::Retry\)\s*\{([^{}]+)\}\s*else cancelCoordinatedShutdown\(\);',main)
assert match and main.count('ks::ui::RetryRetainedCleanupJobs();')==1
retry=match.group(1).strip()
assert retry=='ks::ui::RetryRetainedCleanupJobs();\n            beginCoordinatedShutdownWait();'
assert main.index('if (!guard) return;',main.index('const auto choice ='))<match.start()
assert '#include "UI/RetainedCleanupJob.h"' in main
fixture=r'''
#include "RetainedCleanupJob.h"
#include "CoordinatedApplicationQuit.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <thread>
static int checks=0;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
template<class Predicate>static void pump(Predicate predicate){QElapsedTimer elapsed;elapsed.start();while(!predicate()&&elapsed.elapsed()<5000){QCoreApplication::processEvents();std::this_thread::yield();}check(predicate(),"Core completion timeout");}
struct Stats{std::thread::id gui=std::this_thread::get_id();std::atomic_int calls=0,destroyed=0,guiAttempt=0,guiDestroy=0;std::atomic_bool success=false,block=false;QSemaphore entered,release;};
struct Owner{std::shared_ptr<Stats> stats;explicit Owner(std::shared_ptr<Stats> value):stats(std::move(value)){}~Owner(){++stats->destroyed;if(std::this_thread::get_id()==stats->gui)++stats->guiDestroy;}bool stop(){++stats->calls;if(std::this_thread::get_id()==stats->gui)++stats->guiAttempt;if(stats->block){stats->entered.release();stats->release.acquire();}return stats->success;}};
static auto& registry(){return ks::ui::AsyncCleanupRegistry::global();}
struct Host{ks::ui::AsyncShutdownWait wait;ks::ui::DeferredApplicationRestart restart;std::int64_t now=0;int waitingIntervals=0;void beginCoordinatedShutdownWait(){++waitingIntervals;wait.start(now,32000);}void retryFromProduction(){
// PRODUCTION_RETRY
}void cancel(){restart.cancel();registry().cancelShutdown();}};
static auto makeJob(const std::shared_ptr<Stats>& stats,std::weak_ptr<Owner>& weak){auto owner=std::make_shared<Owner>(stats);weak=owner;return ks::ui::RetainedCleanupJob::create([owner]{return owner->stop();});}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 auto* pool=QThreadPool::globalInstance();const auto original=pool->maxThreadCount();pool->setMaxThreadCount(2);
 {
  registry().cancelShutdown();Host host;auto stats=std::make_shared<Stats>();std::weak_ptr<Owner> weak;auto job=makeJob(stats,weak);check(bool(job),"initial owner admitted before native start");int launches=0;check(host.restart.accept([&]{++launches;return true;}),"restart intent accepted without launch");host.beginCoordinatedShutdownWait();registry().beginShutdown();pump([&]{return job->hasFailed()&&!job->isInFlight();});check(stats->calls==1&&registry().pending()==1&&launches==0,"checked stop failure retains owner lease and restart intent");host.now=32000;check(host.wait.poll(host.now,registry().pending())==ks::ui::AsyncShutdownWait::Status::TimedOut,"visible32s timeout with pending failed cleanup");registry().beginShutdown();pool->waitForDone();check(stats->calls==1,"beginShutdown never substitutes implicit retry");stats->block=true;stats->success=true;host.retryFromProduction();check(stats->entered.tryAcquire(1,5000),"actual MainWindow Retry body queues checked cleanup on backend");check(host.waitingIntervals==2&&host.restart.hasIntent()&&launches==0,"Retry resets bounded interval and preserves frozen restart");int beats=0;QTimer heartbeat;heartbeat.setInterval(1);QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{++beats;});heartbeat.start();pump([&]{return beats>=3;});heartbeat.stop();host.now=40000;check(host.wait.poll(host.now,registry().pending())==ks::ui::AsyncShutdownWait::Status::Waiting&&stats->calls==2,"40s still waits for actual stop and never creates early child");job.reset();check(!weak.expired()&&registry().pending()==1,"UI wrapper destruction does not release blocked native owner");stats->release.release();pump([&]{return registry().pending()==0;});check(host.wait.poll(host.now,registry().pending())==ks::ui::AsyncShutdownWait::Status::Ready&&weak.expired(),"real checked cleanup success releases lease before ready");check(host.restart.launchWhenReady()&&launches==1&&!host.restart.hasIntent(),"ready boundary invokes exactly one deferred launcher");check(stats->guiAttempt==0&&stats->guiDestroy==0&&stats->destroyed==1,"attempt and final resource owner destruction remain backend");
 }
 {
  registry().cancelShutdown();Host host;auto stats=std::make_shared<Stats>();std::weak_ptr<Owner> weak;auto job=makeJob(stats,weak);int launches=0;host.restart.accept([&]{++launches;return true;});registry().beginShutdown();pump([&]{return job->hasFailed()&&!job->isInFlight();});host.cancel();job.reset();pool->waitForDone();check(!host.restart.hasIntent()&&launches==0,"Cancel clears intent and does not launch");check(stats->calls==1&&!weak.expired()&&registry().pending()==1,"Cancel retains failed owner without automatic retry");{auto fresh=registry().acquire();check(bool(fresh),"Cancel reopens new admission while old epoch remains owned");}stats->success=true;host.retryFromProduction();pump([&]{return registry().pending()==0;});check(stats->calls==2&&weak.expired()&&stats->guiDestroy==0,"later explicit Retry completes orphaned old epoch");check(!host.restart.launchWhenReady()&&launches==0,"later ordinary close cannot resurrect canceled launcher");
 }
 pool->waitForDone();pool->setMaxThreadCount(original);registry().cancelShutdown();check(registry().pending()==0&&ks::ui::RetryRetainedCleanupJobs()==0,"all retained owners drained");std::cout<<"SHUTDOWN_RETRY_CHECKS="<<checks<<"\nSOURCE_PATH_CHECKS=4\nGUI_WIDGETS_CREATED=0\nBUSINESS_IO_CALLS=0\nREAL_PROCESS_LAUNCHES=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''.replace('// PRODUCTION_RETRY',retry)
(out/'fixture.cpp').write_text(fixture,encoding='utf-8')
vc=Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207');qt=root/'.deps/Qt/6.9.3/msvc2022_64'
if not (qt/'lib/Qt6Core.lib').exists():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
sdk=Path('C:/Program Files (x86)/Windows Kits/10');flags=['/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/utf-8','/EHsc','/MD','/W4','/WX','/external:W0','/DNOMINMAX','/DQT_CORE_LIB','/I'+str(out),'/I'+str(vc/'include')]
for mod in ('','QtCore'):flags+=['/external:I'+str(qt/'include'/mod)]
for part in ('ucrt','shared','um'):flags+=['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
log=out/'result.log';log.write_text('PRODUCTION_RETRY_BODY=MainWindow.cpp\n',encoding='utf-8')
def run(command,env=None):
    r=subprocess.run(command,cwd=out,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
    with log.open('a',encoding='utf-8')as stream:stream.write(r.stdout+r.stderr+'\nEXIT_CODE='+str(r.returncode)+'\n')
    print(r.stdout+r.stderr,end='');r.check_returncode()
obj=out/'fixture.obj';run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(out/'fixture.cpp'),'/Fo'+str(obj)]);exe=out/'shutdown_retry_tests.exe';libs=['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64')for part in('ucrt','um')]
run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),str(obj),*libs,str(qt/'lib/Qt6Core.lib')]);env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');run([str(exe)],env)
