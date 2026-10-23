#include "view_leave_shim.h"
#include <QTimer>
#include <cstdio>
using namespace ks::ui;
using namespace ksword::memwb;
namespace {int checks=0;}
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::abort();}}while(0)
struct Case {FakeTarget t;FakePane p;FakeWrite w;FakeConfirm c;FakeStatus s;MemoryWorkbenchView v{t,p,w,c,s};int callbacks=0;bool allowed=false;Case(){WorkbenchShared::Instance().controller={};}LeaveEntryStatus start(){return v.runLeaveSequence({},Int3LeaveScenario::ProcessChange,[this](bool x){++callbacks;allowed=x;});}void complete(CommitOutcome outcome,bool clear=true){if(clear)p.data.pending=false;CommitReport r;r.outcome=outcome;w.finish(r);}auto&int3(){return WorkbenchShared::Instance().controller;}};
int main(int argc,char**argv){QCoreApplication app(argc,argv);
 {Case x; x.c.decision=ModeSwitchDecision::Cancel;CHECK(x.start()==LeaveEntryStatus::Rejected);CHECK(x.int3().prepares==0);CHECK(x.w.starts==0);CHECK(x.p.data.pending);}
 {Case x;x.int3().allow=false;CHECK(x.start()==LeaveEntryStatus::Rejected);CHECK(x.c.prompts==1);CHECK(x.int3().prepares==1);CHECK(x.w.starts==0);CHECK(x.int3().applies==0);CHECK(x.p.data.pending);}
 {Case x;x.int3().duringPrepare=[&]{CHECK(x.w.starts==0);CHECK(x.p.data.discards==0);CHECK(x.start()==LeaveEntryStatus::Busy);};CHECK(x.start()==LeaveEntryStatus::Pending);CHECK(x.callbacks==0);CHECK(x.int3().applies==0);x.complete(CommitOutcome::Committed);CHECK(x.callbacks==1&&x.allowed);CHECK(x.int3().applies==1);x.complete(CommitOutcome::Committed);CHECK(x.callbacks==1&&x.int3().applies==1);}
 {Case x;CHECK(x.start()==LeaveEntryStatus::Pending);x.complete(CommitOutcome::WriteFailed,false);CHECK(x.callbacks==1&&!x.allowed);CHECK(x.p.data.pending);CHECK(x.int3().applies==0);}
 {Case x;CHECK(x.start()==LeaveEntryStatus::Pending);x.complete(CommitOutcome::Committed,false);CHECK(!x.allowed&&x.int3().applies==0);}
 {Case x;CHECK(x.start()==LeaveEntryStatus::Pending);x.w.projection=false;x.complete(CommitOutcome::Committed);CHECK(!x.allowed&&x.int3().applies==0);}
 {Case x;CHECK(x.start()==LeaveEntryStatus::Pending);++x.t.rev.source;x.complete(CommitOutcome::Committed);CHECK(!x.allowed&&x.int3().applies==0);}
 {Case x;CHECK(x.start()==LeaveEntryStatus::Pending);++x.v.leaveSequenceTicket_;x.complete(CommitOutcome::Committed);CHECK(x.callbacks==1&&!x.allowed);CHECK(x.int3().applies==0);}
 {Case x;CHECK(x.start()==LeaveEntryStatus::Pending);++x.t.rev.content;x.complete(CommitOutcome::NoChange);CHECK(x.allowed&&x.int3().applies==1);}
 {Case x;x.c.decision=ModeSwitchDecision::DiscardThenSwitch;x.int3().duringPrepare=[&]{CHECK(x.p.data.discards==0);};CHECK(x.start()==LeaveEntryStatus::Completed);CHECK(x.callbacks==0);CHECK(x.p.data.discards==1);CHECK(x.t.rev.content==1);CHECK(x.int3().applies==1);CHECK(x.v.refreshes==1);}
 {Case x;x.c.during=[&]{++x.t.rev.content;};CHECK(x.start()==LeaveEntryStatus::Rejected);CHECK(x.int3().prepares==0);CHECK(x.p.data.pending);}
 {Case x;x.w.entry=CommitEntryStatus::Busy;CHECK(x.start()==LeaveEntryStatus::Busy);CHECK(x.callbacks==0);CHECK(!x.v.leaveSequencePending_);CHECK(x.p.data.pending);}
 {Case x;x.p.data.pending=false;CHECK(x.start()==LeaveEntryStatus::Completed);CHECK(x.int3().applies==1);CHECK(x.w.starts==0);}
 {Case x;x.v.confirmations_=nullptr;CHECK(x.start()==LeaveEntryStatus::Rejected);CHECK(x.p.data.pending);}
 {Case x;int callbacks=0;auto v=std::make_unique<MemoryWorkbenchView>(x.t,x.p,x.w,x.c,x.s);CHECK(v->runLeaveSequence({},Int3LeaveScenario::ProcessChange,[&](bool){++callbacks;})==LeaveEntryStatus::Pending);v.reset();x.complete(CommitOutcome::Committed);CHECK(callbacks==0);CHECK(x.int3().applies==0);}
 {Case x;int beats=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{++beats;});heartbeat.start(1);CHECK(x.start()==LeaveEntryStatus::Pending);QTimer::singleShot(40,&app,[&]{x.complete(CommitOutcome::Committed);app.quit();});app.exec();CHECK(beats>5);CHECK(x.allowed);}
 {Case x;int done=0;bool allowed=false;CHECK(x.v.confirmQuit([&](bool yes){++done;allowed=yes;})==LeaveEntryStatus::Pending);x.complete(CommitOutcome::Committed);CHECK(done==1&&allowed&&x.v.quitPreparationCurrent());++x.t.rev.content;CHECK(!x.v.quitPreparationCurrent());x.v.cancelQuitPreparation();CHECK(!x.v.quitPreparationCurrent());}
 {Case x;bool allowed=true;x.v.duringSync=[&](int count){if(count==3)++x.t.rev.source;};CHECK(x.v.confirmQuit([&](bool yes){allowed=yes;})==LeaveEntryStatus::Pending);x.complete(CommitOutcome::Committed);CHECK(!allowed&&!x.v.quitPreparationCurrent());}
 {Case x;x.p.data.pending=false;CHECK(x.v.confirmQuit()==LeaveEntryStatus::Completed);CHECK(x.v.quitPreparationCurrent());++x.t.rev.source;CHECK(!x.v.quitPreparationCurrent());}
 std::printf("view leave/quit checks=%d\n",checks);return 0;}
