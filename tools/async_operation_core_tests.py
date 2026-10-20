"""Compile production AsyncOperation with QtCore only; no GUI or business IO."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repository-root', type=Path, default=Path(__file__).resolve().parents[1])
p.add_argument('--source-root', type=Path)
p.add_argument('--dependency-source-root', type=Path)
p.add_argument('--secondary-dependency-source-root', type=Path)
p.add_argument('--output-dir', type=Path, required=True)
a = p.parse_args()
root = a.repository_root.resolve()
source = (a.source_root or root).resolve()
out = a.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
ui = Path('Ksword5.1/Ksword5.1/UI'); proof={'production_inputs':{}}
for name in ('AsyncOperation.h', 'AsyncOperation.cpp', 'AsyncUiDispatcher.h', 'AsyncCleanupRegistry.h'):
    src = source / ui / name
    if not src.exists() and a.dependency_source_root:
        src = a.dependency_source_root.resolve() / ui / name
    if not src.exists() and a.secondary_dependency_source_root:
        src = a.secondary_dependency_source_root.resolve() / ui / name
    actual=src if src.exists() else root / ui / name;data=actual.read_bytes();(out/name).write_bytes(data)
    proof['production_inputs'][name]={'path':str(actual),'sha256':hashlib.sha256(data).hexdigest()}
fixture = r'''
#include "AsyncOperation.h"
#include "AsyncCleanupRegistry.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QSemaphore>
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
static int checks=0;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
template<class Predicate>void pump(Predicate predicate){QElapsedTimer t;t.start();while(!predicate()&&t.elapsed()<5000){QCoreApplication::processEvents();std::this_thread::yield();}check(predicate(),"completion timeout");}
static void transactionTests(){
 using Op=ks::ui::AsyncOperation;using Outcome=ks::ui::AsyncOperationOutcome;
 auto* pool=QThreadPool::globalInstance();
 {
  QSemaphore entered,release;pool->start(QRunnable::create([&]{entered.release();release.acquire();}));check(entered.tryAcquire(1,5000),"transaction pool blocker entered");
  std::atomic_int work=0,applied=0,cleaned=0,unexpected=0;auto op=std::make_unique<Op>(nullptr);
  op->setStateChangedCallback([&]{op.reset();});
  const auto ticket=op->submitTransaction<int,int>(42,[&](int value){++work;return value;},[&](int,std::uint64_t){++applied;return true;},[&](std::exception_ptr){++unexpected;},[&](Outcome state){if(state!=Outcome::OwnerClosed)++unexpected;++cleaned;});
  check(ticket!=0&&!op,"accepted before state callback destroys owner");check(work==0&&cleaned==0,"queued transaction retains values after owner close");release.release();
  pump([&]{return cleaned==1;});pool->waitForDone();check(work==1&&applied==0&&unexpected==0,"accepted closed transaction runs once and releases without GUI callback");
 }
 {
  Op op(nullptr);QSemaphore entered,release;int applied=0,rejected=0,settled=0;std::atomic_int work=0;
  std::uint64_t ticket=0;ticket=op.submitTransaction<int,int>(7,[&](int value){++work;entered.release();release.acquire();return value;},[&](int value,std::uint64_t generation){check(value==7&&generation==ticket,"transaction keeps result identity");++applied;return true;},[](std::exception_ptr){throw std::runtime_error("unexpected transaction error");},[&](Outcome state){check(state==Outcome::Applied,"accepted transaction applied");++settled;});
  check(entered.tryAcquire(1,5000),"transaction worker entered");
  auto reject=[&](Outcome state){check(state==Outcome::BusyRejected,"busy has explicit rejection outcome");++rejected;};
  check(op.submitTransaction<int,int>(8,[](int v){return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){},reject)==0,"second transaction rejected");
  check(op.submit<int,int>(9,[](int v,const ks::ui::AsyncOperationToken&){return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){},reject)==0,"query cannot replace accepted transaction");
  op.cancel();check(op.isCurrent(ticket)&&op.isBusy()&&!op.hasPending(),"cancel preserves accepted transaction and generation");release.release();pump([&]{return !op.isInFlight();});check(work==1&&applied==1&&settled==1&&rejected==2,"busy requests do not execute or steal receipt");
 }
 {
  Op op(nullptr);int applied=0,settled=0;Outcome state=Outcome::Applied;
  op.submitTransaction<int,int>(1,[](int value){return value;},[&](int,std::uint64_t){++applied;return false;},[](std::exception_ptr){},[&](Outcome value){++settled;state=value;});
  pump([&]{return applied==1;});check(settled==0&&op.isInFlight(),"deferred transaction keeps completion slot");op.close();check(settled==1&&state==Outcome::OwnerClosed,"close settles finished deferred transaction");op.close();check(settled==1,"close cleanup exactly once");
 }
 {
  Op op(nullptr);int failed=0,settled=0;
  op.submitTransaction<int,int>(1,[](int)->int{throw std::runtime_error("fake backend error");},[](int,std::uint64_t)->bool{throw std::runtime_error("must not apply failure");},[&](std::exception_ptr error){check(bool(error),"failure value preserved");++failed;},[&](Outcome state){check(state==Outcome::Failed,"failure final state");++settled;});
  pump([&]{return !op.isInFlight();});check(failed==1&&settled==1,"transaction exception reports and settles once");
 }
 {
  Op op(nullptr);QSemaphore entered,release;std::vector<int> started,applied;std::mutex mutex;
  auto worker=[&](int value,const ks::ui::AsyncOperationToken&){std::lock_guard lock(mutex);started.push_back(value);if(value==1){entered.release();release.acquire();}return value;};
  auto apply=[&](int value,std::uint64_t){applied.push_back(value);return true;};
  op.submit<int,int>(1,worker,apply,[](std::exception_ptr){});check(entered.tryAcquire(1,5000),"ordinary query entered");op.submit<int,int>(2,worker,apply,[](std::exception_ptr){});op.submit<int,int>(3,worker,apply,[](std::exception_ptr){});check(op.hasPending(),"ordinary latest pending preserved");release.release();pump([&]{return !op.isInFlight();});check(started==std::vector<int>({1,3})&&applied==std::vector<int>({3}),"ordinary latest-only behavior unchanged");
 }
 {
  Op op(nullptr);QSemaphore entered,release;int rejected=0,applied=0;
  op.submit<int,int>(1,[&](int value,const ks::ui::AsyncOperationToken&){entered.release();release.acquire();return value;},[](int,std::uint64_t){return true;},[](std::exception_ptr){});check(entered.tryAcquire(1,5000),"cancelable query entered");op.cancel();check(!op.isBusy()&&op.isInFlight(),"canceled query still owns physical slot");
  check(op.submitTransaction<int,int>(2,[](int v){return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){},[&](Outcome state){if(state==Outcome::BusyRejected)++rejected;})==0&&rejected==1,"transaction rejects canceled but running query");release.release();pump([&]{return !op.isInFlight();});
  check(op.submitTransaction<int,int>(3,[](int v){return v;},[&](int,std::uint64_t){++applied;return true;},[](std::exception_ptr){})!=0,"transaction accepted after physical slot released");pump([&]{return !op.isInFlight();});check(applied==1,"later transaction receipt applied");
 }
 {
  Op op(nullptr);int settled=0,applied=0;
  op.submitTransaction<int,int>(1,[](int v){return v;},[&](int,std::uint64_t){++applied;return true;},[](std::exception_ptr){},[&](Outcome state){check(state==Outcome::DeliveryDropped,"dropped transaction callback outcome");++settled;});pool->waitForDone();QCoreApplication::removePostedEvents(&op,QEvent::MetaCall);check(settled==1&&applied==0&&!op.isInFlight(),"dropped GUI delivery releases transaction exactly once");
 }
 {
  Op op(nullptr);op.close();int settled=0;
  check(op.submitTransaction<int,int>(1,[](int v){return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){},[&](Outcome state){check(state==Outcome::OwnerClosed,"closed transaction rejection outcome");++settled;})==0&&settled==1,"closed controller rejects once");
 }
}
static void shutdownTests(){
 using Registry=ks::ui::AsyncCleanupRegistry;using Wait=ks::ui::AsyncShutdownWait;using Op=ks::ui::AsyncOperation;using Outcome=ks::ui::AsyncOperationOutcome;
 Registry registry;int stopped=0;auto first=registry.acquire([&]{++stopped;check(registry.pending()==2,"stop callback runs without registry lock");});auto second=registry.acquire();
 check(registry.pending()==2,"leases counted once");registry.beginShutdown();check(stopped==1&&!registry.acquire(),"shutdown freezes new work and requests stop once");registry.beginShutdown();check(stopped==1,"repeated shutdown idempotent");first.reset();check(registry.pending()==1,"one lease released");registry.cancelShutdown();auto later=registry.acquire();check(bool(later)&&registry.pending()==2,"cancel exit permits later work");second.reset();later.reset();check(registry.pending()==0,"all leases returned");
 Wait wait;wait.start(10,30);check(wait.poll(11,1)==Wait::Status::Waiting,"wait is nonblocking before deadline");check(wait.poll(40,1)==Wait::Status::TimedOut,"unfinished work reaches visible timeout policy");check(wait.poll(400,0)==Wait::Status::Ready,"completed work always permits ready");wait.start(50,30);check(wait.poll(60,1)==Wait::Status::Waiting,"retry resets deadline");
 auto& global=Registry::global();check(global.pending()==0,"previous transaction leases drained");Op op(nullptr);QSemaphore entered,release;int cleaned=0;bool cleanupSawLease=false;
 op.submitTransaction<int,int>(1,[&](int value){entered.release();release.acquire();return value;},[](int,std::uint64_t){return true;},[](std::exception_ptr){},[&](Outcome){cleanupSawLease=global.pending()==1;++cleaned;});check(entered.tryAcquire(1,5000),"shutdown transaction worker entered");check(global.pending()==1,"accepted transaction has exit lease");global.beginShutdown();Op rejected(nullptr);int rejectedCleanup=0;
 check(rejected.submitTransaction<int,int>(2,[](int v){return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){},[&](Outcome state){if(state==Outcome::BusyRejected)++rejectedCleanup;})==0&&rejectedCleanup==1,"shutdown rejects new transaction without running it");op.close();check(global.pending()==1&&cleaned==0,"close cannot release unfinished transaction exit lease");release.release();pump([&]{return global.pending()==0;});check(cleaned==1&&cleanupSawLease,"lease returned only after final cleanup");global.cancelShutdown();
 std::vector<std::thread> threads;std::atomic_int accepted=0;for(int i=0;i<4;++i)threads.emplace_back([&]{for(int n=0;n<1000;++n){auto lease=registry.acquire();if(lease)++accepted;}});for(auto& thread:threads)thread.join();check(accepted==4000&&registry.pending()==0,"concurrent value lease churn drains exactly");
}
static void droppedStateTests(){
 using Op=ks::ui::AsyncOperation;
 auto* pool=QThreadPool::globalInstance();
 {
  auto op=std::make_unique<Op>(nullptr);int states=0,applied=0;bool idle=false;
  op->setStateChangedCallback([&]{++states;idle=!op->isInFlight();});
  op->submitTransaction<int,int>(1,[](int value){return value;},[&](int,std::uint64_t){++applied;return true;},[](std::exception_ptr){});pool->waitForDone();
  QCoreApplication::removePostedEvents(op.get(),QEvent::MetaCall);check(states==1&&applied==0,"drop does not synchronously call GUI state from cleanup thread");pump([&]{return states==2;});check(idle&&!op->isInFlight(),"drop notifies GUI idle without business timer");
 }
 {
  auto op=std::make_unique<Op>(nullptr);int states=0;op->setStateChangedCallback([&]{++states;});op->submitTransaction<int,int>(1,[](int v){return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){});pool->waitForDone();QCoreApplication::removePostedEvents(op.get(),QEvent::MetaCall);op.reset();QCoreApplication::processEvents();check(states==1,"queued drop recovery cannot touch destroyed owner");
 }
 {
  Op op(nullptr);int idle=0;op.setStateChangedCallback([&]{if(!op.isInFlight())++idle;});op.submitTransaction<int,int>(1,[](int v){return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){});pool->waitForDone();QCoreApplication::removePostedEvents(&op,QEvent::MetaCall);
  QSemaphore entered,release;op.submitTransaction<int,int>(2,[&](int v){entered.release();release.acquire();return v;},[](int,std::uint64_t){return true;},[](std::exception_ptr){});check(entered.tryAcquire(1,5000),"new transaction accepted after dropped delivery");QCoreApplication::processEvents();check(idle==0&&op.isInFlight(),"old recovery notification respects new physical request");release.release();pump([&]{return !op.isInFlight();});check(idle==1,"new request owns its one idle completion");
 }
}
struct ThrowCopyRequest {
 std::shared_ptr<std::atomic_int> copies;
 explicit ThrowCopyRequest(std::shared_ptr<std::atomic_int> value):copies(std::move(value)){}
 ThrowCopyRequest(ThrowCopyRequest&&)=default;
 ThrowCopyRequest(const ThrowCopyRequest& other):copies(other.copies){++*copies;throw std::runtime_error("launch request copy failed");}
};
static void launchFailureTests(){
 using Op=ks::ui::AsyncOperation;using Outcome=ks::ui::AsyncOperationOutcome;
 for(int mode=0;mode<3;++mode){
  auto op=std::make_unique<Op>(nullptr);auto copies=std::make_shared<std::atomic_int>(0);int failure=0,cleaned=0,work=0,applied=0;Outcome outcome=Outcome::Applied;
  const auto ticket=op->submitTransaction<ThrowCopyRequest,int>(ThrowCopyRequest(copies),[&](const ThrowCopyRequest&){++work;return 1;},[&](int,std::uint64_t){++applied;return true;},[&](std::exception_ptr error){check(bool(error),"launch failure retains exception");++failure;if(mode==0)op->close();else if(mode==1)op.reset();},[&](Outcome state){outcome=state;++cleaned;});
  check(ticket!=0&&copies->load()==1&&work==0&&applied==0,"request copy throws before worker accepts work");check(failure==1&&cleaned==1,"launch failure close/delete/default each settle cleanup once");check(outcome==(mode==2?Outcome::Failed:Outcome::OwnerClosed),"launch failure final outcome follows owner lifecycle");
 }
}
struct CleanupCaptureStats{std::atomic_int called=0,destroyed=0,calledPending=-1,destructorPending=-1;};
struct CleanupCaptureOwner{std::shared_ptr<CleanupCaptureStats> stats;explicit CleanupCaptureOwner(std::shared_ptr<CleanupCaptureStats> value):stats(std::move(value)){}~CleanupCaptureOwner(){stats->destructorPending=static_cast<int>(ks::ui::AsyncCleanupRegistry::global().pending());++stats->destroyed;}};
static void cleanupCaptureOrderTests(){
 using Op=ks::ui::AsyncOperation;using Outcome=ks::ui::AsyncOperationOutcome;auto& registry=ks::ui::AsyncCleanupRegistry::global();auto* pool=QThreadPool::globalInstance();
 for(int mode=0;mode<5;++mode){
  registry.cancelShutdown();check(registry.pending()==0,"capture-order case starts without stale lease");auto stats=std::make_shared<CleanupCaptureStats>();auto owner=std::make_shared<CleanupCaptureOwner>(stats);std::weak_ptr<CleanupCaptureOwner> weak=owner;Op op(nullptr);QSemaphore entered,release;int applied=0,failed=0;
  op.submitTransaction<int,int>(1,[&,mode](int value){if(mode==3){entered.release();release.acquire();}if(mode==1)throw std::runtime_error("fake worker failure");return value;},[&](int,std::uint64_t){++applied;return true;},[&](std::exception_ptr){++failed;},[owner,stats,mode](Outcome){stats->calledPending=static_cast<int>(ks::ui::AsyncCleanupRegistry::global().pending());++stats->called;if(mode==4)throw std::runtime_error("fake cleanup exception");});owner.reset();
  if(mode==3){check(entered.tryAcquire(1,5000),"owner-close cleanup capture worker entered");op.close();check(stats->destroyed==0&&registry.pending()==1,"unfinished accepted transaction retains cleanup captures and lease");release.release();}
  pool->waitForDone();if(mode==2)QCoreApplication::removePostedEvents(&op,QEvent::MetaCall);else if(mode!=3)pump([&]{return !op.isInFlight();});
  check(stats->called==1&&stats->calledPending==1,"cleanup call occurs once with transaction lease retained");check(stats->destroyed==1&&weak.expired()&&stats->destructorPending==1,"cleanup capture final owner is destroyed before shutdown lease returns");check(registry.pending()==0,"lease returns only after cleanup capture destruction");check(applied==((mode==0||mode==4)?1:0)&&failed==(mode==1?1:0),"capture release preserves actual apply failure dropped and closed result semantics");op.close();check(stats->called==1&&stats->destroyed==1,"later close never repeats cleanup or final value destructor");
 }
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 auto* pool=QThreadPool::globalInstance();const int threads=pool->maxThreadCount();pool->setMaxThreadCount(1);
 QSemaphore entered,release;pool->start(QRunnable::create([&]{entered.release();release.acquire();}));check(entered.tryAcquire(1,5000),"pool blocker entered");
 QObject owner;ks::ui::AsyncOperation low(&owner),normal(&owner),high(&owner);
 std::vector<int> order;std::mutex mutex;int applied=0,cleaned=0;std::atomic_int errors=0;
 auto work=[&](int value,const ks::ui::AsyncOperationToken&){std::lock_guard lock(mutex);order.push_back(value);return value;};
 auto apply=[&](int,std::uint64_t){++applied;return true;};auto failure=[&](std::exception_ptr){++errors;};
 auto cleanup=[&](ks::ui::AsyncOperationOutcome outcome){if(outcome!=ks::ui::AsyncOperationOutcome::Applied)++errors;++cleaned;};
 check(low.submit<int,int>(-1,work,apply,failure,cleanup,-1)!=0,"low accepted");
 check(normal.submit<int,int>(0,work,apply,failure)!=0,"legacy signature accepted");
 check(high.submit<int,int>(1,work,apply,failure,cleanup,1)!=0,"high accepted");
 check(order.empty(),"tasks queue behind actual pool worker");release.release();
 pump([&]{return !low.isInFlight()&&!normal.isInFlight()&&!high.isInFlight();});
 check(order==std::vector<int>({1,0,-1}),"real pool executes high/default/low");check(applied==3&&cleaned==2&&errors==0,"legacy and priority callbacks settle normally");
 pool->waitForDone();transactionTests();launchFailureTests();shutdownTests();droppedStateTests();cleanupCaptureOrderTests();pool->setMaxThreadCount(threads);
 std::cout<<"ASYNC_CORE_CHECKS="<<checks<<"\nPOOL_ORDER=high,default,low\nGUI_WIDGETS_CREATED=0\nBUSINESS_IO_CALLS=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
(out / 'fixture.cpp').write_text(fixture, encoding='utf-8')
proof['fixture_sha256']=hashlib.sha256((out/'fixture.cpp').read_bytes()).hexdigest();(out/'inputs.json').write_text(json.dumps(proof,indent=2),encoding='utf-8')
vc = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
if not (vc / 'bin/Hostx64/x64/cl.exe').is_file():
    vc = Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207')
qt = root / '.deps/Qt/6.9.3/msvc2022_64'
if not (qt / 'lib/Qt6Core.lib').is_file():
    qt = Path('D:/Software/Qt/6.9.3/msvc2022_64')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
flags = ['/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/utf-8','/EHsc','/MD','/W4','/WX','/external:W0','/DNOMINMAX','/DQT_CORE_LIB','/I'+str(out),'/I'+str(vc/'include')]
for mod in ('','QtCore'):flags += ['/external:I'+str(qt/'include'/mod)]
for part in ('ucrt','shared','um'):flags += ['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
log = out / 'result.log'
log.write_text('SHARED_CONTROLLER=production AsyncOperation.cpp\nHOST_TOOLS=Hostx64/x64\n', encoding='utf-8')
def run(command, env=None):
    result = subprocess.run(command,cwd=out,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
    with log.open('a',encoding='utf-8') as stream:stream.write('COMMAND='+json.dumps(command)+'\n'+result.stdout+result.stderr+'\nEXIT_CODE='+str(result.returncode)+'\n')
    print(result.stdout+result.stderr,end='')
    result.check_returncode()
objects=[]
for name in ('fixture','AsyncOperation'):
    obj=out/(name+'.obj');run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(out/(name+'.cpp')),'/Fo'+str(obj)]);objects.append(str(obj))
exe=out/'async_core_tests.exe'
libs=['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64') for part in ('ucrt','um')]
run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),*objects,*libs,str(qt/'lib/Qt6Core.lib')])
env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');run([str(exe)],env)
