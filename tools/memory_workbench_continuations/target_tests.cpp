#include "WorkbenchTarget.h"
#include <QCoreApplication>
#include <QTimer>
#include <QThread>
#include <cassert>
#include <cstdio>
#include <map>
using namespace ks::ui;
using namespace ksword::memwb;
namespace { int checks=0, opened=0, closed=0; bool alive=true; std::uint64_t generation=1; }
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
namespace ks::ui {
AnchorInfo AcquireAnchorForPid(std::uint32_t pid) { ++opened; return {false,1000+pid,64,reinterpret_cast<void*>(static_cast<std::uintptr_t>(pid)),false,0}; }
AnchorInfo AcquireAnchorFromDockHandle(void*) { return {}; }
void ReleaseAnchorHandle(void* handle) noexcept { if(handle) ++closed; }
std::optional<bool> QueryAnchorAlive(void*) noexcept { return alive; }
void WorkbenchTarget::refreshProcessModules(bool) {}
void WorkbenchTarget::refreshKernelModules(bool) {}
}
class Services final:public IWorkbenchServices {
public:
ModuleEnumResult enumerateProcessModules(std::uint32_t,std::uint64_t)override{return {};}
ModuleEnumResult enumerateKernelModules()override{return {};}
std::vector<ProcessCandidate> processCandidates()override{return {};}
PointerReadResult readPointer(const MemoryTargetSession&,std::uint64_t,std::uint32_t)override{return {};}
std::uint64_t ddmaGeneration()override{return generation;}
};
int main(int argc,char**argv) {
 QCoreApplication app(argc,argv); int callbacks=0; LeaveCompletion pending;
 WorkbenchTarget target(std::make_unique<Services>());
 CHECK(target.requestScope(Scope::ProcessVirtual)==LeaveEntryStatus::Completed);
 CHECK(callbacks==0);
 target.setLeaveGuard([&](LeaveReason,LeaveCompletion c){pending=std::move(c);return LeaveEntryStatus::Pending;});
 CHECK(target.requestScope(Scope::KernelVirtual,[&](bool ok){CHECK(ok);CHECK(QThread::currentThread()==app.thread());++callbacks;})==LeaveEntryStatus::Pending);
 CHECK(target.session().scope==Scope::ProcessVirtual);
 CHECK(target.requestChannel(Channel::StandardDriver)==LeaveEntryStatus::Busy);
 pending(true);CHECK(target.session().scope==Scope::KernelVirtual);CHECK(callbacks==1);
 pending(true);CHECK(callbacks==1);
 CHECK(target.requestScope(Scope::Physical,[&](bool ok){CHECK(!ok);++callbacks;})==LeaveEntryStatus::Pending);
 pending(false); CHECK(target.session().scope==Scope::KernelVirtual);CHECK(callbacks==2);
 CHECK(target.requestScope(Scope::Physical,[&](bool ok){CHECK(!ok);++callbacks;})==LeaveEntryStatus::Pending);
 target.requestReload();pending(true);CHECK(target.session().scope==Scope::KernelVirtual);CHECK(callbacks==3);
 CHECK(target.requestScope(Scope::Physical,[&](bool ok){CHECK(!ok);++callbacks;})==LeaveEntryStatus::Pending);
 target.setPolicy({false,false,true});pending(true);CHECK(target.session().scope==Scope::KernelVirtual);CHECK(callbacks==4);
 target.setPolicy({});
 CHECK(target.requestScope(Scope::Physical,[&](bool ok){CHECK(ok);++callbacks;})==LeaveEntryStatus::Pending);
 target.noteContentChanged();pending(true);CHECK(target.session().scope==Scope::Physical);CHECK(callbacks==5);
 CHECK(target.requestScope(static_cast<Scope>(999))==LeaveEntryStatus::Rejected);
 target.setLeaveGuard({}); CHECK(target.requestScope(Scope::ProcessVirtual)==LeaveEntryStatus::Completed);
 target.setLeaveGuard([&](LeaveReason,LeaveCompletion c){pending=std::move(c);return LeaveEntryStatus::Pending;});
 const int beforeOpen=opened,beforeClose=closed;
 CHECK(target.requestPin(42,1042,[&](bool ok){CHECK(ok);++callbacks;})==LeaveEntryStatus::Pending);
 CHECK(opened==beforeOpen+1);CHECK(closed==beforeClose);CHECK(target.session().pid==0);
 pending(true);CHECK(target.session().pid==42);CHECK(closed==beforeClose);pending(true);CHECK(callbacks==6);
 CHECK(target.requestPin(43,1043,[&](bool ok){CHECK(!ok);++callbacks;})==LeaveEntryStatus::Pending);
 alive=false;pending(true);alive=true;CHECK(target.session().pid==42);CHECK(closed==beforeClose+1);
 pending={};
 {
  auto doomed=std::make_unique<WorkbenchTarget>(std::make_unique<Services>());
  doomed->setLeaveGuard([&](LeaveReason,LeaveCompletion c){pending=std::move(c);return LeaveEntryStatus::Pending;});
  CHECK(doomed->requestPin(44,1044,[&](bool){CHECK(false);})==LeaveEntryStatus::Pending);
  doomed.reset();pending(true);pending={};CHECK(closed==beforeClose+2);
 }
 target.setLeaveGuard({});CHECK(target.requestChannel(Channel::Ddma)==LeaveEntryStatus::Completed);
 target.setLeaveGuard([&](LeaveReason,LeaveCompletion c){pending=std::move(c);return LeaveEntryStatus::Pending;});
 CHECK(target.requestScope(Scope::Physical,[&](bool ok){CHECK(!ok);++callbacks;})==LeaveEntryStatus::Pending);
 ++generation;pending(true);CHECK(target.session().scope==Scope::ProcessVirtual);
 target.setLeaveGuard({});CHECK(target.requestChannel(Channel::UserMode)==LeaveEntryStatus::Completed);
 target.setPolicy({false,false,false});
 target.setLeaveGuard([&](LeaveReason,LeaveCompletion c){pending=std::move(c);return LeaveEntryStatus::Pending;});
 CHECK(target.clearMemoryDebugTarget([&](bool ok){CHECK(ok);++callbacks;})==LeaveEntryStatus::Pending);
 CHECK(target.session().pid==42);CHECK(target.clearMemoryDebugTarget()==LeaveEntryStatus::Busy);
 pending(true);CHECK(target.session().pid==0);const int afterClear=callbacks;pending(true);CHECK(callbacks==afterClear);
 target.setPolicy({});
 CHECK(target.requestScope(Scope::Physical)==LeaveEntryStatus::Pending);pending(true);CHECK(target.session().scope==Scope::Physical);
 int beats=0; QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{++beats;});heartbeat.start(1);
 CHECK(target.requestScope(Scope::KernelVirtual,[&](bool ok){CHECK(ok);++callbacks;app.quit();})==LeaveEntryStatus::Pending);
 QTimer::singleShot(40,&app,[&]{pending(true);});app.exec();CHECK(beats>5);CHECK(target.session().scope==Scope::KernelVirtual);
 pending={}; std::printf("checks=%d callbacks=%d heartbeat=%d anchors=%d/%d\n",checks,callbacks,beats,opened,closed); return 0;
}
