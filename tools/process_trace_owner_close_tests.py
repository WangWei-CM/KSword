from pathlib import Path
import argparse,hashlib,json,subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--repository-root',type=Path,default=Path('.'))
parser.add_argument('--source-root',type=Path,required=True)
parser.add_argument('--cleanup-source-root',type=Path,required=True)
parser.add_argument('--output-dir',type=Path,required=True)
args=parser.parse_args()
root=args.repository_root.resolve();source_root=args.source_root.resolve();cleanup=args.cleanup_source_root.resolve();out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
source_path=source_root/'Ksword5.1/Ksword5.1/MonitorDock/ProcessTraceMonitorWidget.Runtime.cpp'
source=source_path.read_text(encoding='utf-8-sig')
a=source.index('void ProcessTraceMonitorWidget::stopMonitoringInternal(');o=source.index('{',a);d=1;b=o+1
while d:d+=(source[b]=='{')-(source[b]=='}');b+=1
method=source[a:b].replace('ProcessTraceMonitorWidget::','Owner::')
prefix=r'''#include "RetainedCleanupJob.h"
#include <QCoreApplication>
#include <QThreadPool>
#include <atomic>
#include <cstdio>
#include <thread>
static int checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::printf("FAIL %d: " #x "\n",__LINE__);return 19;}}while(false)
struct Timer {void stop(){}void start(){}bool isActive(){return false;}};
struct Owner {struct Capture{std::atomic_bool m_captureStopFlag{false};};std::shared_ptr<Capture>m_capture=std::make_shared<Capture>();std::shared_ptr<ks::ui::RetainedCleanupJob>m_captureCleanup;std::uint64_t m_captureStopTime100ns=0;Timer*m_runtimeRefreshTimer=nullptr,*m_uiUpdateTimer=nullptr;std::uint64_t currentSystemTime100ns(){return 1;}void refreshTimelineRange(bool){}void updateActionState(){}void updateStatusLabel(){}void stopMonitoringInternal(bool);};
static const auto uiThread=std::this_thread::get_id();
static std::atomic_bool destroyedOnBackend{false};
struct Resource{~Resource(){destroyedOnBackend.store(std::this_thread::get_id()!=uiThread);}};
'''
suffix=r'''
int main(int argc,char**argv){QCoreApplication app(argc,argv);
 std::atomic_int calls{0};std::atomic_bool allow{false};Owner owner;
 auto resource=std::make_shared<Resource>();std::weak_ptr<Resource> weak=resource;
 owner.m_captureCleanup=ks::ui::RetainedCleanupJob::create([resource=std::move(resource),&calls,&allow](){++calls;return allow.load();});
 CHECK(bool(owner.m_captureCleanup));owner.stopMonitoringInternal(false);QThreadPool::globalInstance()->waitForDone();
 CHECK(calls.load()==1);CHECK(owner.m_captureCleanup->hasFailed());CHECK(ks::ui::AsyncCleanupRegistry::global().pending()==1);CHECK(!weak.expired());
 owner.stopMonitoringInternal(true);QThreadPool::globalInstance()->waitForDone();
 CHECK(calls.load()==1);CHECK(owner.m_capture->m_captureStopFlag.load());CHECK(owner.m_captureCleanup->hasFailed());CHECK(!weak.expired());
 owner.m_captureCleanup.reset();QThreadPool::globalInstance()->waitForDone();CHECK(calls.load()==1);CHECK(!weak.expired());CHECK(ks::ui::AsyncCleanupRegistry::global().pending()==1);
 allow.store(true);CHECK(ks::ui::RetryRetainedCleanupJobs()==1);QThreadPool::globalInstance()->waitForDone();
 CHECK(calls.load()==2);CHECK(weak.expired());CHECK(destroyedOnBackend.load());CHECK(ks::ui::AsyncCleanupRegistry::global().pending()==0);
 // An owner first destroyed before any Stop still admits exactly one cleanup.
 Owner firstClose;std::atomic_int firstCalls{0};firstClose.m_captureCleanup=ks::ui::RetainedCleanupJob::create([&firstCalls](){++firstCalls;return true;});firstClose.stopMonitoringInternal(true);QThreadPool::globalInstance()->waitForDone();CHECK(firstCalls.load()==1);CHECK(firstClose.m_captureCleanup->isComplete());CHECK(ks::ui::AsyncCleanupRegistry::global().pending()==0);
 // Explicit Stop is still an explicit retry when the owner remains alive.
 Owner explicitRetry;std::atomic_int retryCalls{0};explicitRetry.m_captureCleanup=ks::ui::RetainedCleanupJob::create([&retryCalls](){return ++retryCalls>=2;});explicitRetry.stopMonitoringInternal(false);QThreadPool::globalInstance()->waitForDone();CHECK(retryCalls.load()==1);CHECK(explicitRetry.m_captureCleanup->hasFailed());explicitRetry.stopMonitoringInternal(false);QThreadPool::globalInstance()->waitForDone();CHECK(retryCalls.load()==2);CHECK(explicitRetry.m_captureCleanup->isComplete());CHECK(ks::ui::AsyncCleanupRegistry::global().pending()==0);
 std::printf("PASS OWNER_CLOSE_CHECKS=%d ATTEMPTS_AFTER_CLOSE=1 MAIN_RETRY_FINAL=2 GUI=0 BUSINESS=0\n",checks);return 0;}
'''
qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
results=[]
for negative in [False,True]:
 name='negative' if negative else 'fixture';body=method
 if negative:
  old='if (m_captureCleanup && (!ownerClosing || !m_captureCleanup->isCleanupRequested()))'
  assert body.count(old)==1
  body=body.replace(old,'if (m_captureCleanup)',1).replace('    m_capture->m_captureStopFlag.store(true);','    (void)ownerClosing;\n    m_capture->m_captureStopFlag.store(true);',1)
 p=out/f'{name}.cpp';p.write_text(prefix+body+suffix,encoding='utf-8')
 flags=['/nologo','/std:c++20','/Zc:__cplusplus','/EHsc','/W4','/WX','/MD','/utf-8','/I"'+str(qt/'include')+'"','/I"'+str(qt/'include/QtCore')+'"','/I"'+str(cleanup/'Ksword5.1/Ksword5.1/UI')+'"','/I"'+str(root/'Ksword5.1/Ksword5.1/UI')+'"','/Fo"'+str(out/f'{name}.obj')+'"','/Fe"'+str(out/f'{name}.exe')+'"','"'+str(p)+'"','/link','/LIBPATH:"'+str(qt/'lib')+'"','Qt6Core.lib']
 rsp=out/f'{name}.rsp';rsp.write_text(' '.join(flags),encoding='utf-8')
 cmd=out/f'{name}.cmd';cmd.write_text('@echo off\ncall "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul\ncl.exe @"'+str(rsp)+'"\nif errorlevel 1 exit /b %errorlevel%\nset "PATH='+str(qt/'bin')+';%PATH%"\n"'+str(out/f'{name}.exe')+'"\nexit /b %errorlevel%\n',encoding='utf-8')
 result=subprocess.run(['cmd','/c',str(cmd)],cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 (out/f'{name}.log').write_text(result.stdout,encoding='utf-8')
 expected=19 if negative else 0
 if result.returncode!=expected:raise RuntimeError((name,result.returncode,result.stdout))
 results.append({'variant':name,'exit_code':result.returncode,'expected':expected,'fixture_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'stdout':result.stdout})
inputs={'source':str(source_path),'source_sha256':hashlib.sha256(source_path.read_bytes()).hexdigest(),'method_sha256':hashlib.sha256(method.encode()).hexdigest(),'headers':{name:{'source':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for name,p in [('RetainedCleanupJob.h',cleanup/'Ksword5.1/Ksword5.1/UI/RetainedCleanupJob.h'),('AsyncCleanupRegistry.h',root/'Ksword5.1/Ksword5.1/UI/AsyncCleanupRegistry.h')]}}
(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n',encoding='utf-8');(out/'results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
print('PASS exact production owner-close/failed-owner retention/Main Retry; old unconditional retry negative caught')
