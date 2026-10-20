#pragma once
#include "WorkbenchContinuation.h"
#include "MemoryWriteTransaction.h"
#include <QCoreApplication>
#include <QPointer>
#include <QScopeGuard>
#include <QString>
#include <vector>
#include <optional>
namespace ks::ui {
enum class Int3LeaveScenario { DockDetach, ProcessChange, MainWindowClose };
enum class CommitEntryStatus { Started, Pending, Busy, Rejected };
struct FakeOverlay { bool pending=true;int discards=0; bool HasPendingPatches()const{return pending;} std::uint64_t PendingByteCount()const{return pending?4:0;} std::vector<int> DiffBlocks()const{return pending?std::vector<int>{1}:std::vector<int>{};} void DiscardAll(){pending=false;++discards;} };
struct FakeCanvas { int notifications=0;void notifyOverlayChanged(){++notifications;} };
struct FakePane {FakeOverlay data;FakeCanvas view;FakeOverlay&overlay(){return data;}FakeCanvas*canvas(){return &view;}};
struct FakeTarget { ksword::memwb::RevisionSnapshot rev;ksword::memwb::MemoryTargetSession session; struct Capture {ksword::memwb::MemoryTargetSession session;ksword::memwb::RevisionSnapshot rev;}; Capture capture(){return {session,rev};}bool isStale(ksword::memwb::RevisionSnapshot r){return rev!=r;}void noteContentChanged(){++rev.content;}struct Revs { std::uint64_t source;std::uint64_t Source()const{return source;} };Revs revisions()const{return {rev.source};} };
struct FakeStatus {int diagnostics=0;void setDiagnosticsText(QString,bool){++diagnostics;} };
struct FakeConfirm {ksword::memwb::ModeSwitchDecision decision=ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;int prompts=0;std::function<void()>during; auto PromptLeaveWithPending(std::uint64_t,std::uint64_t,const QString&){++prompts;if(during)during();return decision;} };
struct FakeWrite {
 bool busy=false;int starts=0;std::function<void(const ksword::memwb::CommitReport&)>finish;CommitEntryStatus entry=CommitEntryStatus::Pending;
 bool lastCommitProjectionMatched()const{return projection;}bool projection=true;
 bool isCommitting()const{return busy;}struct Attempt{CommitEntryStatus status;};
 template<class F>Attempt commitPendingNow(F f){++starts;finish=std::move(f);return{entry};}
};
struct Int3Controller {
 struct LeavePlan {bool allow=true;bool allowed()const{return allow;}}; bool allow=true,applyOkay=true;int prepares=0,applies=0;std::function<void()>duringPrepare,duringApply;
 LeavePlan PrepareLeave(QObject*,Int3LeaveScenario){++prepares;if(duringPrepare)duringPrepare();return{allow};}
 bool HasUnrestoredForCurrentTarget()const{return false;}
 bool ApplyPreparedLeave(const LeavePlan&){++applies;if(duringApply)duringApply();return applyOkay;}
};
struct WorkbenchShared { static WorkbenchShared&Instance(){static WorkbenchShared x;return x;}Int3Controller controller;Int3Controller&Int3(){return controller;} };
namespace workbench_messages {inline QString Translate(ksword::memwb::CommitOutcome){return {};}}
using TargetCapture=FakeTarget::Capture;
struct MemoryWorkbenchView:QObject {
 FakeTarget*target_;FakePane*hexPane_;FakeWrite*writeController_;FakeConfirm*confirmations_;FakeStatus*statusBar_;bool leaveSequencePending_=false,memoryDebugMode_=false;std::uint64_t leaveSequenceTicket_=0;int refreshes=0,syncs=0;bool int3KeptByLeaveGuard_=false;std::optional<TargetCapture>quitCapture_;std::function<void(int)>duringSync;
 MemoryWorkbenchView(FakeTarget&t,FakePane&p,FakeWrite&w,FakeConfirm&c,FakeStatus&s):target_(&t),hexPane_(&p),writeController_(&w),confirmations_(&c),statusBar_(&s){}
 void syncInt3Context(){++syncs;if(duringSync)duringSync(syncs);}void refreshPendingPatchesDisplay(){++refreshes;}
 LeaveEntryStatus confirmQuit(LeaveCompletion completion={});
 void cancelQuitPreparation();
 bool quitPreparationCurrent();
 LeaveEntryStatus runLeaveSequence(const QString&,Int3LeaveScenario,LeaveCompletion);
};
}
