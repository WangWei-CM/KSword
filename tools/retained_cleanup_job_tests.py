"""QtCore-only retained cleanup owner/lease tests. No widgets or native business IO."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--repository-root',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--source-root',type=Path)
p.add_argument('--registry-source-root',type=Path)
p.add_argument('--output-dir',type=Path,required=True)
a=p.parse_args();root=a.repository_root.resolve();source=(a.source_root or root).resolve();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
rel=Path('Ksword5.1/Ksword5.1/UI');proof={'headers':{}}
for name in ('RetainedCleanupJob.h','AsyncCleanupRegistry.h'):
    src=source/rel/name
    if not src.exists() and a.registry_source_root:src=a.registry_source_root.resolve()/rel/name
    if not src.exists():src=root/rel/name
    data=src.read_bytes()
    proof['headers'][name]={'path':str(src),'sha256':hashlib.sha256(data).hexdigest()}
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
struct ReservationGate{std::mutex mutex;int count=0,badAttempt=0;bool closing=false;};
static std::atomic_bool pauseReservationNotify=false;static QSemaphore reservationNotifyPaused,releaseReservationNotify;
// Old readiness headers lack the notification API; the same fixture exercises their original requestCleanup contract.
template<class T>static bool notifyReservationReady(const std::shared_ptr<T>& job){if constexpr(requires {job->notifyReadyToAttempt();})return job->notifyReadyToAttempt();else return job->requestCleanup();}
struct Reservation{std::shared_ptr<ReservationGate> gate;std::shared_ptr<Job> job;~Reservation(){bool final=false;{std::lock_guard<std::mutex> lock(gate->mutex);--gate->count;final=gate->closing&&gate->count==0;}if(final){if(pauseReservationNotify){reservationNotifyPaused.release();releaseReservationNotify.acquire();}notifyReservationReady(job);}}};
static auto reserve(const std::shared_ptr<ReservationGate>& gate,const std::shared_ptr<Job>& job){std::lock_guard<std::mutex> lock(gate->mutex);if(gate->closing)return std::shared_ptr<Reservation>{};++gate->count;auto value=std::make_shared<Reservation>();value->gate=gate;value->job=job;return value;}
static auto makeReservedJob(const std::shared_ptr<Stats>& stats,const std::shared_ptr<ReservationGate>& gate,std::weak_ptr<ValueOwner>& weak){auto owner=std::make_shared<ValueOwner>(stats);weak=owner;return Job::create([owner,gate]{{std::lock_guard<std::mutex> lock(gate->mutex);if(gate->count!=0)++gate->badAttempt;}return owner->cleanup();},0,[owner,gate]{(void)owner;std::lock_guard<std::mutex> lock(gate->mutex);gate->closing=true;},[owner,gate]{(void)owner;std::lock_guard<std::mutex> lock(gate->mutex);return gate->count==0;});}

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
 {
  cleanBase();pool->setMaxThreadCount(1);QSemaphore occupied,unblock;pool->start(QRunnable::create([&]{occupied.release();unblock.acquire();}));check(occupied.tryAcquire(1,5000),"single Core worker intentionally occupied before shutdown");auto stats=std::make_shared<Stats>();stats->success=true;auto gate=std::make_shared<ReservationGate>();std::weak_ptr<ValueOwner> weak;auto job=makeReservedJob(stats,gate,weak);auto reservation=reserve(gate,job);check(bool(reservation),"accepted resource reservation frozen before shutdown");registry().beginShutdown();check(job->isCleanupRequested()&&!job->isInFlight()&&!job->hasFailed()&&job->attemptCount()==0&&registry().pending()==1,"not-ready cleanup remains deferred without worker or false failure");check(!reserve(gate,job),"synchronous closing rejects any new reservation");check(ks::ui::RetryRetainedCleanupJobs()==0,"global failed-job Retry does not misclassify deferred reservation");job.reset();std::atomic_int businessCalls=0;pool->start(QRunnable::create([reservation=std::move(reservation),&businessCalls,weak]() mutable{if(!weak.expired())++businessCalls;reservation.reset();}));check(!weak.expired()&&stats->calls==0,"GUI wrapper gone still retains accepted resource via reservation");unblock.release();pump([&]{return registry().pending()==0;});check(businessCalls==1&&stats->calls==1&&gate->badAttempt==0,"accepted task runs first then last reservation schedules one checked cleanup on pool1");check(weak.expired()&&stats->guiDestroy==0&&stats->destroyed==1,"attempt stop and readiness closures all release final value owner on backend");registry().cancelShutdown();pool->setMaxThreadCount(4);
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();auto gate=std::make_shared<ReservationGate>();std::weak_ptr<ValueOwner> weak;auto job=makeReservedJob(stats,gate,weak);auto reservation=reserve(gate,job);registry().beginShutdown();check(!job->hasFailed()&&stats->calls==0,"active reservation never calls cleanup or sets failure");reservation.reset();pump([&]{return job->hasFailed()&&!job->isInFlight();});check(stats->calls==1&&registry().pending()==1&&!weak.expired(),"ready cleanup false retains real failure and owner");stats->success=true;check(ks::ui::RetryRetainedCleanupJobs()==1,"Main Retry remains available for genuine ready cleanup failure");pump([&]{return registry().pending()==0;});check(stats->calls==2&&gate->badAttempt==0&&weak.expired(),"true checked retry completes after readiness without lost trigger");registry().cancelShutdown();
 }
 for(int variant=0;variant<2;++variant){
  cleanBase();auto stats=std::make_shared<Stats>();stats->throwOnce=(variant==1);auto gate=std::make_shared<ReservationGate>();std::weak_ptr<ValueOwner> weak;auto job=makeReservedJob(stats,gate,weak);auto reservation=reserve(gate,job);pauseSignal=true;pauseReservationNotify=true;std::thread shutdown([]{registry().beginShutdown();});check(signalPaused.tryAcquire(1,5000),"shutdown request pauses after closing before initial readiness check");std::thread finalRelease([reservation=std::move(reservation)]() mutable {reservation.reset();});check(reservationNotifyPaused.tryAcquire(1,5000),"final reservation pauses after count reaches zero before readiness notification");pauseSignal=false;releaseSignal.release();shutdown.join();pump([&]{return job->hasFailed()&&!job->isInFlight();});pool->waitForDone();check(stats->calls==1&&registry().pending()==1&&!weak.expired(),"initial false or throw retains resource owner and lease before late notification");pauseReservationNotify=false;releaseReservationNotify.release();finalRelease.join();pool->waitForDone();check(stats->calls==1&&job->hasFailed()&&registry().pending()==1&&!weak.expired(),"late readiness notification cannot automatically retry failed initial cleanup");check(!notifyReservationReady(job),"repeated readiness notifications never consume explicit failed Retry");job.reset();pool->waitForDone();check(stats->calls==1&&registry().pending()==1&&!weak.expired(),"late notification and wrapper destruction retain failed owner until explicit Retry");stats->success=true;check(ks::ui::RetryRetainedCleanupJobs()==1,"only actual explicit Retry queues second attempt after late readiness");pump([&]{return registry().pending()==0;});check(stats->calls==2&&weak.expired()&&stats->guiDestroy==0,"explicit Retry releases all old owners on backend after late notify race");registry().cancelShutdown();
 }
 {
  cleanBase();auto stats=std::make_shared<Stats>();stats->success=true;auto gate=std::make_shared<ReservationGate>();std::weak_ptr<ValueOwner> weak;auto job=makeReservedJob(stats,gate,weak);check(!notifyReservationReady(job)&&!job->isCleanupRequested()&&stats->calls==0,"readiness notification cannot stop an unrequested active session");job->requestCleanup();pump([&]{return registry().pending()==0;});check(stats->calls==1&&weak.expired(),"later explicit stop still owns exactly one initial cleanup");
 }
 {
  cleanBase();int failures=0;for(int n=0;n<128;++n){auto stats=std::make_shared<Stats>();stats->success=true;auto gate=std::make_shared<ReservationGate>();std::weak_ptr<ValueOwner> weak;auto job=makeReservedJob(stats,gate,weak);std::shared_ptr<Reservation> reservation;std::thread accept([&]{reservation=reserve(gate,job);});std::thread stop([]{registry().beginShutdown();});accept.join();stop.join();reservation.reset();pump([&]{return registry().pending()==0;});if(stats->calls!=1||gate->badAttempt!=0||!weak.expired())++failures;registry().cancelShutdown();}check(failures==0,"concurrent reserve versus closing preserves once-only cleanup and no reservation IO overlap");
 }
 pool->waitForDone();pool->setMaxThreadCount(original);check(registry().pending()==0&&ks::ui::RetryRetainedCleanupJobs()==0,"retained cleanup store drains fully");std::cout<<"RETAINED_CLEANUP_CHECKS="<<checks<<"\nGUI_WIDGETS_CREATED=0\nBUSINESS_IO_CALLS=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
(out/'fixture.cpp').write_text(fixture,encoding='utf-8')
proof['fixture_sha256']=hashlib.sha256((out/'fixture.cpp').read_bytes()).hexdigest()
(out/'inputs.json').write_text(json.dumps(proof,indent=2),encoding='utf-8')
vc=Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207');qt=root/'.deps/Qt/6.9.3/msvc2022_64'
if not (qt/'lib/Qt6Core.lib').exists():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
sdk=Path('C:/Program Files (x86)/Windows Kits/10');flags=['/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/utf-8','/EHsc','/MD','/W4','/WX','/external:W0','/DNOMINMAX','/DQT_CORE_LIB','/I'+str(out),'/I'+str(vc/'include')]
for mod in ('','QtCore'):flags+=['/external:I'+str(qt/'include'/mod)]
for part in ('ucrt','shared','um'):flags+=['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
log=out/'result.log';log.write_text('PRODUCTION_HEADER=RetainedCleanupJob.h\n',encoding='utf-8')
def run(command,env=None):
    r=subprocess.run(command,cwd=out,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
    with log.open('a',encoding='utf-8')as stream:stream.write('COMMAND='+json.dumps(command)+'\n'+r.stdout+r.stderr+'\nEXIT_CODE='+str(r.returncode)+'\n')
    print(r.stdout+r.stderr,end='');r.check_returncode()
obj=out/'fixture.obj';run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(out/'fixture.cpp'),'/Fo'+str(obj)]);exe=out/'retained_cleanup_tests.exe';libs=['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64')for part in('ucrt','um')]
run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),str(obj),*libs,str(qt/'lib/Qt6Core.lib')]);env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');run([str(exe)],env)
