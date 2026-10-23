"""Pure QCore fixture: actual DriverDock scheduling methods, real shared controller, fake collector.
No QWidget, driver, service, filesystem business operation or product launch.
"""
from pathlib import Path
import argparse, importlib.util, subprocess, os
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repository-root',type=Path,default=Path(__file__).resolve().parents[1])
parser.add_argument('--source-root',type=Path)
parser.add_argument('--output-dir',type=Path)
parser.add_argument('--shared-overlay',type=Path)
args=parser.parse_args()
root=args.repository_root.resolve()
source=(args.source_root or root).resolve()
shared=(args.shared_overlay or root/'Ksword5.1/Ksword5.1/UI').resolve()
step=(args.output_dir or root/'output/driver-module-evidence-tests').resolve()
step.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('extract',root/'tools/async_operation_tests.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
s=(source/'Ksword5.1/Ksword5.1/DriverDock/DriverDock.Evidence.cpp').read_text(encoding='utf-8-sig')
methods='\n'.join(module.extract_method(s,'DriverDock::'+name) for name in ('cancelLoadedModuleEvidenceQuery()','refreshLoadedModuleEvidenceAsync()'))
fixture=r'''
#include "AsyncOperation.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QList>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <iostream>
#include <vector>
#include <thread>
#include <stdexcept>
#include <chrono>
#include <algorithm>
static int checks=0;
static void check(bool ok,const char* text){if(!ok) throw std::runtime_error(text);++checks;}
struct Gate { std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;
 void enter(){std::unique_lock lock(mutex);entered=true;cv.notify_all();if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return released;}))throw std::runtime_error("gate timeout");}
 void await(){std::unique_lock lock(mutex);check(cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}),"worker did not enter");}
 void release(){std::lock_guard lock(mutex);released=true;cv.notify_all();}
};
static std::shared_ptr<Gate> gate;
static std::mutex statsMutex;
static std::vector<int> started,objects;
static std::atomic_int active=0,maxActive=0,lateApply=0;
namespace ks::ui {
 static bool blocked=false;
 static std::vector<std::function<void()>> deferred;
 bool DeferTableUiCommitIfContextMenuOpen(QObject*,const QString&,const QList<void*>&,std::function<void()> action){if(!blocked)return false;deferred.push_back(std::move(action));return true;}
}
struct FakeLabel {QString text;void setText(const QString& t){text=t;}};
struct FakeButton {bool enabled=true;void setEnabled(bool value){enabled=value;}};
static QString driverText(const char*,const QString& text){return text;}
class DriverDock : public QObject {
 public:
 struct LoadedKernelModuleRecord {int identity=0;};
 struct LoadedModuleEvidenceRecord {int identity=0;};
 std::vector<LoadedKernelModuleRecord> m_loadedModuleCache;
 std::vector<LoadedModuleEvidenceRecord> m_loadedModuleEvidenceCache;
 std::unique_ptr<ks::ui::AsyncOperation> m_moduleEvidenceOperation;
 bool m_moduleEvidenceQuerying=false;
 std::uint64_t m_moduleEvidenceQueryTicket=0;
 FakeButton button;FakeLabel label;
 FakeButton* m_refreshModuleEvidenceButton=&button;
 FakeLabel* m_moduleEvidenceStatusLabel=&label;
 void* m_moduleTable=nullptr;
 int rebuilds=0;
 ~DriverDock(){if(m_moduleEvidenceOperation)m_moduleEvidenceOperation->close();}
 void rebuildLoadedModuleTable(){++rebuilds;}
 void updateLoadedModuleEvidenceStatusText(){label.text=m_moduleEvidenceQuerying?"busy":"complete";}
 void cancelLoadedModuleEvidenceQuery();
 void refreshLoadedModuleEvidenceAsync();
 static std::vector<LoadedModuleEvidenceRecord> collectEvidenceForLoadedModules(const std::vector<LoadedKernelModuleRecord>& rows,const ks::ui::AsyncOperationToken& token){
  struct Exit {~Exit(){--active;}} exit;
  const int now=++active;maxActive.store(std::max(maxActive.load(),now));
  const int id=rows.front().identity;
  {std::lock_guard lock(statsMutex);started.push_back(id);}
  if(id==1&&gate)gate->enter();
  // Fake synchronous signature: cancellation is observed immediately after it returns.
  if(token.isCanceled())return {};
  std::vector<LoadedModuleEvidenceRecord> out;
  for(const auto& row:rows){if(token.isCanceled())return {};{std::lock_guard lock(statsMutex);objects.push_back(row.identity);}out.push_back({row.identity});}
  return out;
 }
};
__METHODS__
template<class Pred>void pump(Pred pred){QElapsedTimer timer;timer.start();while(!pred()&&timer.elapsed()<5000){QCoreApplication::processEvents();std::this_thread::yield();}check(pred(),"completion timeout");}
static void idle(DriverDock& dock){pump([&]{return !dock.m_moduleEvidenceOperation->isInFlight();});}
static void reset(){started.clear();objects.clear();active=0;maxActive=0;ks::ui::deferred.clear();ks::ui::blocked=false;gate.reset();}
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{
 {
 reset();DriverDock dock;gate=std::make_shared<Gate>();dock.m_loadedModuleCache={{1}};dock.refreshLoadedModuleEvidenceAsync();gate->await();
 for(int id:{2,3,4}){dock.cancelLoadedModuleEvidenceQuery();dock.m_loadedModuleCache={{id},{id+100}};dock.refreshLoadedModuleEvidenceAsync();}
 check(dock.m_moduleEvidenceOperation->hasPending(),"latest snapshot must be pending");check(active==1,"single active while signature blocks");
 gate->release();idle(dock);check(maxActive==1,"no overlapping evidence workers");check(started==std::vector<int>({1,4}),"only latest pending snapshot may launch");check(objects==std::vector<int>({4,104}),"canceled signature must not begin object queries");
 check(dock.m_loadedModuleEvidenceCache.size()==2&&dock.m_loadedModuleEvidenceCache[1].identity==104,"complete latest snapshot and order preserved");check(dock.rebuilds==1&&dock.button.enabled,"one final UI commit and button restored");
 }
 {
 reset();DriverDock dock;gate=std::make_shared<Gate>();dock.m_loadedModuleCache={{1}};dock.refreshLoadedModuleEvidenceAsync();gate->await();dock.m_loadedModuleCache={{2}};dock.refreshLoadedModuleEvidenceAsync();
 dock.cancelLoadedModuleEvidenceQuery();dock.m_loadedModuleCache.clear();dock.m_loadedModuleEvidenceCache.clear();gate->release();idle(dock);
 check(started==std::vector<int>({1}),"failed or empty module refresh must remove pending scan");check(dock.rebuilds==0,"canceled result cannot refill empty module list");
 }
 {
 reset();DriverDock dock;ks::ui::blocked=true;dock.m_loadedModuleCache={{5}};dock.refreshLoadedModuleEvidenceAsync();pump([&]{return ks::ui::deferred.size()==1;});
 dock.m_loadedModuleCache={{6}};dock.refreshLoadedModuleEvidenceAsync();pump([&]{return ks::ui::deferred.size()==2;});check(started==std::vector<int>({5,6}),"menu-deferred old result must not stall new worker");
 ks::ui::blocked=false;auto old=ks::ui::deferred[0],latest=ks::ui::deferred[1];old();check(dock.rebuilds==0,"old menu replay before latest must be ignored");latest();old();check(dock.m_loadedModuleEvidenceCache.front().identity==6&&dock.rebuilds==1,"old delayed menu result cannot replace latest");check(!dock.m_moduleEvidenceOperation->isInFlight(),"deferred completion releases active slot");
 }
 {
 reset();auto dock=std::make_unique<DriverDock>();ks::ui::blocked=true;dock->m_loadedModuleCache={{7}};dock->refreshLoadedModuleEvidenceAsync();pump([&]{return ks::ui::deferred.size()==1;});dock.reset();ks::ui::deferred[0]();check(active==0,"owner destruction permits no late apply");
 }
 {
 reset();auto dock=std::make_unique<DriverDock>();gate=std::make_shared<Gate>();dock->m_loadedModuleCache={{1}};dock->refreshLoadedModuleEvidenceAsync();gate->await();dock->m_loadedModuleCache={{8}};dock->refreshLoadedModuleEvidenceAsync();dock.reset();gate->release();pump([&]{return active==0;});QCoreApplication::processEvents();check(started==std::vector<int>({1})&&objects.empty(),"close cancels active stages and pending request");
 }
 QThreadPool::globalInstance()->waitForDone();std::cout<<"DM07_CORE_MOCK_CHECKS="<<checks<<"\nGUI_WIDGETS_CREATED=0\nBUSINESS_IO_CALLS=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
'''
(step/'dm07_core_fixture.cpp').write_text(fixture.replace('__METHODS__',methods),encoding='utf-8')
vc=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
if not (vc/'bin/Hostx64/x64/cl.exe').is_file():vc=Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207')
qt=root/'.deps/Qt/6.9.3/msvc2022_64'
if not (qt/'lib/Qt6Core.lib').is_file():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
sdk=Path('C:/Program Files (x86)/Windows Kits/10')
flags=['/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/utf-8','/EHsc','/MD','/W4','/WX','/external:W0','/DNOMINMAX','/DUNICODE','/D_UNICODE','/DQT_CORE_LIB','/I'+str(shared),'/I'+str(root/'Ksword5.1/Ksword5.1/UI'),'/I'+str(vc/'include')]
for mod in ('','QtCore'):flags+=['/external:I'+str(qt/'include'/mod)]
for part in ('ucrt','shared','um'):flags+=['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
log=step/'mock.log';log.write_text('EXTRACTED_METHODS=2\nSHARED_CONTROLLER=production AsyncOperation.cpp\n',encoding='utf-8')
def run(cmd,env=None):
 r=subprocess.run(cmd,cwd=step,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
 with log.open('a',encoding='utf-8') as f:f.write(r.stdout+r.stderr+'\nEXIT_CODE='+str(r.returncode)+'\n')
 print(r.stdout+r.stderr);r.check_returncode()
objs=[]
for src,name in [(step/'dm07_core_fixture.cpp','fixture'),(shared/'AsyncOperation.cpp','controller')]:
 obj=step/(name+'.obj');run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(src),'/Fo'+str(obj)]);objs.append(str(obj))
exe=step/'dm07_core_fixture.exe'
libs=['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/p/'x64') for p in ('ucrt','um')]
run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),*objs,*libs,str(qt/'lib/Qt6Core.lib')])
env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');run([str(exe)],env)
