"""QtCore-only retained cleanup owner/lease tests. No widgets or native business IO."""
from pathlib import Path
import argparse, os, subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--repository-root',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--source-root',type=Path)
p.add_argument('--registry-source-root',type=Path)
p.add_argument('--output-dir',type=Path,required=True)
a=p.parse_args();root=a.repository_root.resolve();source=(a.source_root or root).resolve();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
rel=Path('Ksword5.1/Ksword5.1/UI')
for name in ('RetainedCleanupJob.h','AsyncCleanupRegistry.h'):
    src=source/rel/name
    if not src.exists() and a.registry_source_root:src=a.registry_source_root.resolve()/rel/name
    if not src.exists():src=root/rel/name
    data=src.read_bytes()
    if name=='RetainedCleanupJob.h':
        # Only add deterministic pauses to the copied production header; no control-flow replacement.
        header=data.decode('utf-8').replace('\r\n','\n')
        assert header.count('stopSignaled.store(true);')==1
        assert header.count('auto job = std::shared_ptr<RetainedCleanupJob>')==1
        header=header.replace('#pragma once','#pragma once\nvoid RetainedCleanupTestSignalPause() noexcept;\nvoid RetainedCleanupTestCreatePause() noexcept;',1)
        header=header.replace('stopSignaled.store(true);','stopSignaled.store(true); RetainedCleanupTestSignalPause();',1)
        header=header.replace('auto job = std::shared_ptr<RetainedCleanupJob>','RetainedCleanupTestCreatePause();\n                auto job = std::shared_ptr<RetainedCleanupJob>',1)
        data=header.encode('utf-8')
    (out/name).write_bytes(data)
fixture=r'''
#include "RetainedCleanupJob.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <thread>
static std::atomic_bool pauseCreate=false,pauseSignal=false;static QSemaphore createPaused,releaseCreate,signalPaused,releaseSignal;
void RetainedCleanupTestCreatePause() noexcept{if(pauseCreate){createPaused.release();releaseCreate.acquire();}}
void RetainedCleanupTestSignalPause() noexcept{if(pauseSignal){signalPaused.release();releaseSignal.acquire();}}
static int checks=0;static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
template<class Predicate>void pump(Predicate predicate){QElapsedTimer time;time.start();while(!predicate()&&time.elapsed()<5000){QCoreApplication::processEvents();std::this_thread::yield();}check(predicate(),"Core completion timeout");}
struct Stats {std::thread::id gui=std::this_thread::get_id();std::atomic_int calls=0,active=0,maxActive=0,destroyed=0,guiDestroy=0,guiAttempt=0;std::atomic_bool success=false,block=false,throwOnce=false;QSemaphore entered,release;};
struct ValueOwner {
 std::shared_ptr<Stats> stats;explicit ValueOwner(std::shared_ptr<Stats> value):stats(std::move(value)){}
 ~ValueOwner(){if(std::this_thread::get_id()==stats->gui)++stats->guiDestroy;++stats->destroyed;}
 bool cleanup(){++stats->calls;if(std::this_thread::get_id()==stats->gui)++stats->guiAttempt;const auto now=++stats->active;int previous=stats->maxActive.load();while(previous<now&&!stats->maxActive.compare_exchange_weak(previous,now)){}struct Exit{std::shared_ptr<Stats> stats;~Exit(){--stats->active;}} exit{stats};if(stats->block){stats->entered.release();stats->release.acquire();}if(stats->throwOnce.exchange(false))throw std::runtime_error("fake checked stop throws");return stats->success;}
};
using Job=ks::ui::RetainedCleanupJob;
static auto makeJob(const std::shared_ptr<Stats>& stats,std::weak_ptr<ValueOwner>& weak){auto owner=std::make_shared<ValueOwner>(stats);weak=owner;return Job::create([owner]{return owner->cleanup();});}
static auto& registry(){return ks::ui::AsyncCleanupRegistry::global();}
static void cleanBase(){registry().cancelShutdown();check(registry().pending()==0,"no accepted owner lost between cases");}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{auto* pool=QThreadPool::globalInstance();const auto original=pool->maxThreadCount();pool->setMaxThreadCount(4);
 {
  cleanBase();auto stats=std::make_shared<Stats>();std::weak_ptr<ValueOwner> weak;auto job=makeJob(stats,weak);check(bool(job)&&registry().pending()==1&&!job->isCleanupRequested(),"create retains value owner and lease before session start");check(ks::ui::RetryRetainedCleanupJobs()==0&&stats->calls==0,"global retry never stops unrequested active session");check(job->requestCleanup(),"explicit cleanup accepted for backend");pump([&]{return job->hasFailed()&&!job->isInFlight();});check(job->attemptCount()==1&&stats->calls==1&&registry().pending()==1&&!weak.expired(),"false checked stop retains owner and exit lease");for(int n=0;n<64;++n)QCoreApplication::processEvents();check(stats->calls==1&&stats->destroyed==0,"failed cleanup does not spin or destruct service");stats->success=true;check(ks::ui::RetryRetainedCleanupJobs()==1,"explicit global retry schedules failed owner once");pump([&]{return job->isComplete()&&registry().pending()==0;});check(stats->calls==2&&stats->maxActive==1&&weak.expired()&&stats->destroyed==1&&stats->guiDestroy==0&&stats->guiAttempt==0,"success releases final value owner on backend before lease while wrapper remains");check(!job->requestCleanup(),"complete job rejects any new request");check(ks::ui::RetryRetainedCleanupJobs()==0,"complete store has no retry work");job.reset();check(stats->calls==2,"completed wrapper destruction never retries");
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();stats->block=true;stats->success=true;std::weak_ptr<ValueOwner> weak;auto job=makeJob(stats,weak);registry().beginShutdown();check(job->isCleanupRequested()&&stats->entered.tryAcquire(1,5000),"beginShutdown requests previously active job once");check(!Job::create([]{return true;}),"shutdown rejects new cleanup owner admission before start");for(int n=0;n<16;++n)check(!job->requestCleanup(),"in-flight requests do not queue duplicates");check(ks::ui::RetryRetainedCleanupJobs()==0&&registry().pending()==1&&!weak.expired(),"in-flight lease held and no implicit retry");int beats=0;QTimer heartbeat;heartbeat.setInterval(1);QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{++beats;});heartbeat.start();pump([&]{return beats>=3;});heartbeat.stop();check(stats->guiAttempt==0&&stats->calls==1,"blocked cleanup leaves Core event loop responsive");stats->release.release();pump([&]{return job->isComplete()&&registry().pending()==0;});check(weak.expired()&&stats->guiDestroy==0,"final handle owner released after blocked backend completes");registry().cancelShutdown();
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();stats->block=true;stats->success=true;std::weak_ptr<ValueOwner> weak;auto job=makeJob(stats,weak);job.reset();check(stats->entered.tryAcquire(1,5000)&&registry().pending()==1&&!weak.expired(),"last UI wrapper destruction requests once and retains owner asynchronously");stats->release.release();pump([&]{return registry().pending()==0;});check(stats->calls==1&&stats->destroyed==1&&stats->guiDestroy==0,"automatic first cleanup finishes only on backend");
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();std::weak_ptr<ValueOwner> weak;auto job=makeJob(stats,weak);job->requestCleanup();pump([&]{return job->hasFailed()&&!job->isInFlight();});job.reset();pool->waitForDone();check(stats->calls==1&&!weak.expired()&&registry().pending()==1,"explicit failed close plus wrapper destruction is one attempt");stats->success=true;check(ks::ui::RetryRetainedCleanupJobs()==1,"global retry retains work after UI handle is gone");pump([&]{return registry().pending()==0;});check(weak.expired()&&stats->calls==2&&stats->guiDestroy==0,"orphaned failed owner released after real success");
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();stats->throwOnce=true;stats->success=true;std::weak_ptr<ValueOwner> weak;auto job=makeJob(stats,weak);job->requestCleanup();pump([&]{return job->hasFailed()&&!job->isInFlight();});check(registry().pending()==1&&!weak.expired()&&stats->calls==1,"exception also retains accepted owner");check(job->requestCleanup(),"explicit job demand retries one failed attempt");pump([&]{return job->isComplete()&&registry().pending()==0;});check(stats->calls==2&&weak.expired()&&stats->guiDestroy==0,"throw then success performs one final backend release");
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();std::weak_ptr<ValueOwner> weak;auto job=makeJob(stats,weak);registry().beginShutdown();pump([&]{return job->hasFailed()&&!job->isInFlight();});registry().beginShutdown();pool->waitForDone();check(stats->calls==1&&registry().pending()==1,"repeated beginShutdown does not replace explicit Retry policy");stats->success=true;check(ks::ui::RetryRetainedCleanupJobs()==1,"user Retry can continue old lease while admission frozen");pump([&]{return registry().pending()==0;});registry().cancelShutdown();check(bool(registry().acquire()),"cancel exit reopens admission after retained cleanup success");
 }
 {
  cleanBase();pool->setMaxThreadCount(1);QSemaphore occupied,unblock;pool->start(QRunnable::create([&]{occupied.release();unblock.acquire();}));check(occupied.tryAcquire(1,5000),"global pool intentionally saturated");auto stats=std::make_shared<Stats>();stats->success=true;auto owner=std::make_shared<ValueOwner>(stats);std::weak_ptr<ValueOwner> weak=owner;std::atomic_int stopSignals=0;auto job=Job::create([owner]{return owner->cleanup();},0,[owner,&stopSignals]{++stopSignals;});owner.reset();registry().beginShutdown();check(stopSignals==1&&job->isCleanupRequested()&&stats->calls==0,"registry synchronously stopSignals stop before queued attempt despite saturated pool");for(int n=0;n<16;++n)check(!job->requestCleanup(),"busy signal requests do not queue another attempt");check(stopSignals==1&&registry().pending()==1&&!weak.expired(),"stop signal is once and its value closure remains owned");unblock.release();pump([&]{return job->isComplete()&&registry().pending()==0;});check(stats->calls==1&&weak.expired()&&stats->guiDestroy==0,"both value closures released on backend before lease");registry().cancelShutdown();pool->setMaxThreadCount(4);
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();stats->success=true;auto owner=std::make_shared<ValueOwner>(stats);std::weak_ptr<ValueOwner> weak=owner;QSemaphore signaled,finishSignal;std::atomic_int stopSignals=0;auto job=Job::create([owner]{return owner->cleanup();},0,[owner,&stopSignals,&signaled,&finishSignal]{++stopSignals;signaled.release();finishSignal.acquire();});owner.reset();std::thread requester([job]{job->requestCleanup();});check(signaled.tryAcquire(1,5000),"request signal entered before attempt");check(!job->requestCleanup()&&stats->calls==0,"concurrent request cannot execute attempt before synchronous signal returns");finishSignal.release();requester.join();pump([&]{return job->isComplete()&&registry().pending()==0;});check(stopSignals==1&&stats->calls==1&&weak.expired()&&stats->guiDestroy==0,"concurrent signal exactly once before final backend cleanup");
 }
 for(int variant=0;variant<2;++variant){
  cleanBase();auto stats=std::make_shared<Stats>();stats->throwOnce=(variant==1);std::weak_ptr<ValueOwner> weak;std::atomic_bool returnedNull=false;pauseCreate=true;pauseSignal=true;std::thread creator([&]{auto job=makeJob(stats,weak);returnedNull=!job;});check(createPaused.tryAcquire(1,5000),"create pauses after accepted lease before ready publication");std::thread shutdown([]{registry().beginShutdown();});check(signalPaused.tryAcquire(1,5000),"original registry request pauses after stop signal before ready check");releaseCreate.release();creator.join();check(returnedNull,"shutdown during create never returns startable old epoch");pump([&]{return stats->calls==1;});pool->waitForDone();check(stats->calls==1&&registry().pending()==1&&!weak.expired(),"create handoff queues exactly one false or throwing initial attempt");pauseCreate=false;pauseSignal=false;releaseSignal.release();shutdown.join();pool->waitForDone();check(stats->calls==1&&registry().pending()==1,"late original callback cannot turn initial handoff into automatic retry");stats->success=true;check(ks::ui::RetryRetainedCleanupJobs()==1,"explicit Retry remains available after initialization handoff failure");pump([&]{return registry().pending()==0;});check(stats->calls==2&&weak.expired()&&stats->guiDestroy==0,"one explicit retry finally releases old epoch on backend");registry().cancelShutdown();
 }
 pool->waitForDone();pool->setMaxThreadCount(original);check(registry().pending()==0&&ks::ui::RetryRetainedCleanupJobs()==0,"retained cleanup store drains fully");std::cout<<"RETAINED_CLEANUP_CHECKS="<<checks<<"\nGUI_WIDGETS_CREATED=0\nBUSINESS_IO_CALLS=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
(out/'fixture.cpp').write_text(fixture,encoding='utf-8')
vc=Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207');qt=root/'.deps/Qt/6.9.3/msvc2022_64'
if not (qt/'lib/Qt6Core.lib').exists():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
sdk=Path('C:/Program Files (x86)/Windows Kits/10');flags=['/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/utf-8','/EHsc','/MD','/W4','/WX','/external:W0','/DNOMINMAX','/DQT_CORE_LIB','/I'+str(out),'/I'+str(vc/'include')]
for mod in ('','QtCore'):flags+=['/external:I'+str(qt/'include'/mod)]
for part in ('ucrt','shared','um'):flags+=['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
log=out/'result.log';log.write_text('PRODUCTION_HEADER=RetainedCleanupJob.h\n',encoding='utf-8')
def run(command,env=None):
    r=subprocess.run(command,cwd=out,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
    with log.open('a',encoding='utf-8')as stream:stream.write(r.stdout+r.stderr+'\nEXIT_CODE='+str(r.returncode)+'\n')
    print(r.stdout+r.stderr,end='');r.check_returncode()
obj=out/'fixture.obj';run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(out/'fixture.cpp'),'/Fo'+str(obj)]);exe=out/'retained_cleanup_tests.exe';libs=['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64')for part in('ucrt','um')]
run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),str(obj),*libs,str(qt/'lib/Qt6Core.lib')]);env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');run([str(exe)],env)
