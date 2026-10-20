"""QtCore value-only shutdown/quit fixture plus production exit-path source checks."""
from pathlib import Path
import argparse, importlib.util, json, os, re, subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--repository-root',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--source-root',type=Path)
p.add_argument('--registry-source-root',type=Path)
p.add_argument('--output-dir',type=Path,required=True)
p.add_argument('--counterexample-crash-source',type=Path,help='Old production crash source expected to fail the new exit-race matrix')
p.add_argument('--counterexample-startup-source',type=Path,help='Old main.cpp expected to reject a standalone pre-Qt privilege marker')
a=p.parse_args();root=a.repository_root.resolve();source=(a.source_root or root).resolve();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
rel=Path('Ksword5.1/Ksword5.1');s=(source/rel/'MainWindow.cpp').read_text(encoding='utf-8-sig');checks=0
def check(value,message):
    global checks
    if not value:raise RuntimeError(message)
    checks+=1
close=s[s.index('void MainWindow::closeEvent('):s.index('void MainWindow::beginCoordinatedShutdownWait(')]
check(s.count('QCoreApplication::exit(0);')==1 and 'QCoreApplication::quit();' not in s and 'QApplication::quit(' not in s,'MainWindow has one approved final event-loop exit')
check(s.index('QCoreApplication::exit(0);')>s.index('void MainWindow::finishCoordinatedShutdown('),'quit occurs only in final method')
check(close.index('prepareWorkbenchQuit()')<close.index('AsyncCleanupRegistry::global().beginShutdown()')<close.index('prepareR0DriverServiceStop()')<close.index('executeR0ServiceStopOnWorker()'),'memory approval, session cleanup then R0 stop ordering')
check('hide();' not in close and 'asyncShutdownStarted' not in close and 'QTimer::singleShot' not in close,'no hidden fixed-time exit fallback')
check('queryR0DriverServiceRunning(' not in close,'exit status query moved to value worker')
worker=close[close.index('const auto work = [serviceState]()'):close.index('try { QThreadPool::globalInstance()->start(work); }')]
check(all(x not in worker for x in ('this','QWidget','QCoreApplication','QApplication','QPointer')),'exit worker contains values only')
check('serviceState->finished.store(true' in worker and worker.index('executeR0ServiceStopOnWorker()')<worker.index('serviceState->finished.store(true'),'service cleanup publishes final completion')
check('QMessageBox::Retry | QMessageBox::Cancel' in s and 'else cancelCoordinatedShutdown();' in s,'timeout offers retry/cancel without forced exit')
check(s.count('ks::ui::RequestCoordinatedApplicationRestart(')==3,'all runtime MainWindow restart modes defer their launch')
check('ks::ui::RemoveCoordinatedApplicationQuit(this);' in s,'derived destructor invalidates registration first')
priv=(source/rel/'Framework/PrivilegeElevationPrompt.h').read_text(encoding='utf-8-sig')
check('RequestCoordinatedApplicationRestart(' in priv and 'QCoreApplication::quit();' not in priv,'shared elevation prompt defers its launch until cleanup ready')
finish=s[s.index('void MainWindow::finishCoordinatedShutdown('):s.index('bool MainWindow::requestCoordinatedRestart(')]
check(finish.index('AsyncCleanupRegistry::global().pending() == 0')<finish.index('m_shutdownRestart.launchWhenReady()')<finish.index('QCoreApplication::exit(0);'),'runtime successor launches only after cleanup and immediately before final quit')
check('if (!launched)' in finish and 'cancelCoordinatedShutdown();' in finish,'failed successor launch retains original runtime instance')
check(close.index('m_shutdownRestart.cancel();')<close.index('AsyncCleanupRegistry::global().beginShutdown()'),'earliest memory rejection clears restart intent before admission freeze')
check('cancelCoordinatedShutdown(true);' in s,'late MemoryDock reapproval preserves accepted restart intent')
check('serviceState->outcome = executeR0ServiceStopOnWorker();\n                    (void)queryRunning();' in s and 'm_r0DriverServiceRunning = m_shutdownServiceState->running;' in s,'launch failure/exit cancellation uses actual post-stop service state')
startup=(source/rel/'main.cpp').read_text(encoding='utf-8-sig');crash=(source/'shared/crash/WinCrashHandler.cpp').read_text(encoding='utf-8-sig');crash_header=(source/'shared/crash/WinCrashHandler.h').read_text(encoding='utf-8-sig')
check(startup.index('InspectRestartPredecessorFromCommandLine(')<startup.index('initializeProcessDpiAwareness();',startup.index('int main('))<startup.index('QApplication app('),'validated restart wait occurs before business window/application initialization')
check('&& !restartParentExited)' in startup and 'restart handoff rejected or timed out before business initialization' in startup,'all failed restart states exit before first business initialization')
check('ShellExecuteExW(&launch)' in priv and 'ValidateRestartChildProcess(launch.hProcess)' in priv and 'ValidateRestartChildProcess(launch.hProcess)' in s,'runas validates actual created child before original instance requests quit')
check('ValidateRestartChildProcess(' not in startup and startup.count('parametersForPreQtRestart();')==2 and 'ArgumentsWithoutRestartPredecessorWait(std::move(arguments))' in startup,'pre-Qt relaunches strip runtime predecessor wait and use their own executable launch result')
check('if (restartStatus != ks::crash::RestartPredecessorStatus::NotRequested' in startup,'privilege marker alone does not activate the runtime predecessor gate')
spec=importlib.util.spec_from_file_location('extract',root/'tools/async_operation_tests.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
legacy='bool WaitForCrashRestartTargetFromCommandLine('
check(extract.extract_method(crash,legacy)==extract.extract_method((root/'shared/crash/WinCrashHandler.cpp').read_text(encoding='utf-8-sig'),legacy),'legacy crash reporter boolean implementation is unchanged')
times=extract.extract_method(crash,'bool IsParentOlderThanChild(')
check(times.count('::GetProcessTimes(')==2 and '::CompareFileTime(&parentCreated, &childCreated) <= 0' in times,'live parent/child identity rejects recycled newer process IDs')
for lang in ('zh-CN','en-US'):
    pack=json.loads((source/rel/'languages'/f'{lang}.json').read_text(encoding='utf-8-sig'))['translations']
    check(all('main.shutdown.wait.'+key in pack for key in ('title','status','message')),'all new shutdown text has '+lang+' translations')
for name in ('AsyncCleanupRegistry.h','CoordinatedApplicationQuit.h'):
    src=source/rel/'UI'/name
    if not src.exists() and a.registry_source_root:src=a.registry_source_root.resolve()/rel/'UI'/name
    if not src.exists():src=root/rel/'UI'/name
    (out/name).write_bytes(src.read_bytes())
fixture=r'''
#include "AsyncCleanupRegistry.h"
#include "CoordinatedApplicationQuit.h"
#include <QCoreApplication>
#include <Windows.h>
#include <shellapi.h>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QTimer>
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
static int checks=0;static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
static std::wstring preQtCommand,preQtCaptured;static bool preQtCreated=true;static int preQtCloses=0;
constexpr wchar_t kPrivilegeRestartArgument[]=L"--ksword-privilege-restart";
static const wchar_t* PreQtGetCommandLineW(){return preQtCommand.c_str();}
static std::wstring queryCurrentExecutablePath(){return L"C:\\own app\\Ksword5.1.exe";}
static std::wstring resolveExecutableDirectoryPath(const std::wstring&){return L"C:\\own app";}
static BOOL PreQtShellExecuteExW(SHELLEXECUTEINFOW* launch){preQtCaptured=launch->lpParameters;launch->hProcess=preQtCreated?reinterpret_cast<HANDLE>(42):nullptr;return preQtCreated;}
static BOOL PreQtCreateProcessW(LPCWSTR,LPWSTR command,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION result){preQtCaptured=command;if(preQtCreated){result->hProcess=reinterpret_cast<HANDLE>(42);result->hThread=reinterpret_cast<HANDLE>(43);}return preQtCreated;}
static BOOL PreQtCloseHandle(HANDLE){++preQtCloses;return TRUE;}
#define GetCommandLineW PreQtGetCommandLineW
#define ShellExecuteExW PreQtShellExecuteExW
#define CreateProcessW PreQtCreateProcessW
#define CloseHandle PreQtCloseHandle
__PREQT_METHODS__
#undef GetCommandLineW
#undef ShellExecuteExW
#undef CreateProcessW
#undef CloseHandle
static QStringList parsedArguments(const std::wstring& command){int count=0;auto values=::CommandLineToArgvW(command.c_str(),&count);check(values!=nullptr,"actual Windows argv parser accepts emitted restart text");QStringList result;for(int i=0;i<count;++i)result.push_back(QString::fromWCharArray(values[i]));::LocalFree(values);return result;}
static void preQtRestartTests(){
 const auto marker=QString::fromWCharArray(kPrivilegeRestartArgument);const auto key=QStringLiteral("--ksword-crash-restart-wait-pid");
 preQtCommand=L"\"C:\\own app\\Ksword5.1.exe\" --retain \"some value\" --ksword-crash-restart-wait-pid 41 --scale \"\" --KSWORD-CRASH-RESTART-WAIT-PID 0x80 --mcp-no-auto-elevation";
 const QStringList expected={QStringLiteral("app"),QStringLiteral("--retain"),QStringLiteral("some value"),QStringLiteral("--scale"),QString(),QStringLiteral("--mcp-no-auto-elevation"),marker};
 check(parsedArguments(L"app "+parametersForPreQtRestart())==expected,"pre-Qt startup keeps business args and empty value while removing every inherited wait PID");
 preQtCreated=true;preQtCloses=0;check(tryLaunchElevatedSelfBeforeSplash()&&preQtCloses==1,"pre-Qt UAC launch success closes only its returned process handle");check(parsedArguments(L"app "+preQtCaptured)==expected,"pre-Qt UAC launch has privilege marker without predecessor PID");
 preQtCreated=false;preQtCloses=0;check(!tryLaunchElevatedSelfBeforeSplash()&&preQtCloses==0,"UAC cancellation or failure keeps the original pre-Qt startup path");
 preQtCreated=true;preQtCloses=0;check(tryCreateRestartedSelfBeforeSplash()&&preQtCloses==2,"pre-Qt scale restart uses CreateProcess success and releases both handles");auto scaled=parsedArguments(preQtCaptured);check(scaled.mid(1)==expected.mid(1),"scale restart retains the same business argv and strips stale runtime wait");
 preQtCreated=false;check(!tryCreateRestartedSelfBeforeSplash(),"failed scale relaunch does not abandon current startup");
 check(!scaled.contains(key,Qt::CaseInsensitive)&&scaled.count(marker)==1,"pre-Qt command has one privilege marker and no inherited runtime wait");
}
static void runtimeRestartTests(QCoreApplication& app){
 using Intent=ks::ui::DeferredApplicationRestart;using Wait=ks::ui::AsyncShutdownWait;
 {Intent intent;int launches=0;check(intent.accept([&]{++launches;return true;}),"first restart intent accepted");intent.cancel();check(!intent.hasIntent()&&!intent.launchWhenReady()&&launches==0,"first memory approval cancel cannot poison later ordinary close");}
 for(int mode=0;mode<3;++mode){
  QObject lifetime;ks::ui::AsyncCleanupRegistry registry;Wait wait;Intent intent;QSemaphore entered,stop,finish;int launches=0;std::int64_t now=0,launchedAt=-1;bool wantExit=true,parentExited=false,parentKept=false,retried=false,released=false,completed=false,failed=false;
  auto lease=registry.acquire([&]{stop.release();});std::thread worker([lease=std::move(lease),&entered,&stop,&finish]()mutable{entered.release();stop.acquire();finish.acquire();lease.reset();});check(entered.tryAcquire(1,5000),"40s fake accepted cleanup active");
  check(intent.accept([&]{++launches;launchedAt=now;return mode!=2;}),"runtime launcher captured before shutdown");check(!intent.accept([]{return true;}),"later restart cannot replace accepted intent");registry.beginShutdown();wait.start(0,32000);QTimer poll;poll.setInterval(1);
  QObject::connect(&poll,&QTimer::timeout,&lifetime,[&]{now+=1000;const auto status=wait.poll(now,registry.pending());if(status==Wait::Status::TimedOut&&wantExit){check(now>=32000&&launches==0,"32s timeout has no prematurely launched child");if(mode==1){wantExit=false;intent.cancel();registry.cancelShutdown();parentKept=true;}else{check(intent.hasIntent(),"Retry preserves original runtime intent");retried=true;wait.start(now,32000);}}if(now>=40000&&!released){check(launches==0,"child35s deadline cannot start before40s cleanup ready");released=true;finish.release();}if(released&&registry.pending()==0){if(wantExit){check(intent.hasIntent(),"accepted intent remains until ready boundary");if(intent.launchWhenReady())parentExited=true;else{parentKept=true;registry.cancelShutdown();}}else parentKept=true;completed=true;poll.stop();QCoreApplication::quit();}});
  QTimer::singleShot(2000,&lifetime,[&]{failed=true;finish.release();QCoreApplication::quit();});poll.start();app.exec();worker.join();check(completed&&!failed,"40s fake combination completes with Core event-loop heartbeats");
  if(mode==0)check(retried&&parentExited&&!parentKept&&launches==1&&launchedAt>=40000,"Retry then40s completion launches once at ready and retains replacement");
  else if(mode==1)check(parentKept&&!parentExited&&launches==0&&!intent.hasIntent(),"Cancel keeps original and never launches stale successor");
  else check(retried&&parentKept&&!parentExited&&launches==1&&!intent.hasIntent(),"failed ready-time launch preserves original without stale retry");
 }
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 const QString key=QStringLiteral("--ksword-crash-restart-wait-pid");
 const QStringList args={QStringLiteral("app.exe"),key,QStringLiteral("11"),QStringLiteral("--retain"),QStringLiteral("some value"),key.toUpper(),QStringLiteral("0xA0"),QStringLiteral("--other"),QString()};
 const auto replaced=ks::ui::ArgumentsWithRestartPredecessorWait(args,12345);check(replaced==QStringList({QStringLiteral("app.exe"),QStringLiteral("--retain"),QStringLiteral("some value"),QStringLiteral("--other"),QString(),key,QStringLiteral("12345")}),"replace only old wait keys and values while preserving remaining argv");
 check(ks::ui::ArgumentsWithRestartPredecessorWait({QStringLiteral("app"),key,QStringLiteral("--keep")},2)==QStringList({QStringLiteral("app"),QStringLiteral("--keep"),key,QStringLiteral("2")}),"malformed wait key cannot consume next unrelated option");
 check(ks::ui::QuoteCoordinatedRestartArgument(QString())==QStringLiteral("\"\""),"empty restart argument preserved");check(ks::ui::QuoteCoordinatedRestartArgument(QStringLiteral("C:\\space dir\\"))==QStringLiteral("\"C:\\space dir\\\\\""),"trailing slash in quoted path preserved");check(ks::ui::QuoteCoordinatedRestartArgument(QStringLiteral("a\"b"))==QStringLiteral("\"a\\\"b\""),"embedded argument quote escaped");
 int first=0,second=0;auto oldOwner=std::make_unique<QObject>();auto newOwner=std::make_unique<QObject>();
 ks::ui::InstallCoordinatedApplicationQuit(oldOwner.get(),[&]{++first;});ks::ui::RequestCoordinatedApplicationQuit();check(first==1,"quit uses registered owner");
 ks::ui::InstallCoordinatedApplicationQuit(newOwner.get(),[&]{++second;});oldOwner.reset();ks::ui::RequestCoordinatedApplicationQuit();check(first==1&&second==1,"old owner destroy cannot erase newer registration");ks::ui::RemoveCoordinatedApplicationQuit(newOwner.get());check(!ks::ui::coordinated_quit_detail::state().owner&&!ks::ui::coordinated_quit_detail::state().request,"explicit removal invalidates derived lifetime");
 ks::ui::InstallCoordinatedApplicationQuit(newOwner.get(),[&]{++second;});newOwner.reset();check(!ks::ui::coordinated_quit_detail::state().owner&&!ks::ui::coordinated_quit_detail::state().request,"destroyed owner releases stored callback");
 {
  QObject restartOwner;ks::ui::DeferredApplicationRestart intent;int launches=0;
  ks::ui::InstallCoordinatedApplicationQuit(&restartOwner,[]{},[&](ks::ui::DeferredApplicationRestart::Launch launch){return intent.accept(std::move(launch));});
  check(ks::ui::RequestCoordinatedApplicationRestart([&]{++launches;return true;})&&launches==0&&intent.hasIntent(),"restart bridge queues intent without launching before ready");
  check(!ks::ui::RequestCoordinatedApplicationRestart([]{return true;}),"restart bridge rejects replacing accepted runtime intent");
  ks::ui::RemoveCoordinatedApplicationQuit(&restartOwner);check(!ks::ui::coordinated_quit_detail::state().restart&&!ks::ui::RequestCoordinatedApplicationRestart([]{return true;}),"removed owner cannot accept or launch runtime restart");intent.cancel();check(launches==0,"canceled bridged intent does not survive as stale launcher");
 }
 ks::ui::AsyncCleanupRegistry registry;ks::ui::AsyncShutdownWait wait;QSemaphore entered,stopRequested,allowFinish;std::atomic_int stopped=0,closed=0;int heartbeats=0;bool requested=false,finished=false,timedOut=false;
 auto lease=registry.acquire([&]{stopRequested.release();});std::thread worker([lease=std::move(lease),&entered,&stopRequested,&allowFinish,&stopped,&closed]()mutable{entered.release();stopRequested.acquire();++stopped;allowFinish.acquire();++closed;lease.reset();});check(entered.tryAcquire(1,5000),"mock session active");
 QObject owner;QElapsedTimer elapsed;QTimer poll;poll.setInterval(1);ks::ui::InstallCoordinatedApplicationQuit(&owner,[&]{requested=true;registry.beginShutdown();elapsed.start();wait.start(0,15);poll.start();});
 QObject::connect(&poll,&QTimer::timeout,&app,[&]{++heartbeats;const auto state=wait.poll(elapsed.elapsed(),registry.pending());if(state==ks::ui::AsyncShutdownWait::Status::TimedOut){timedOut=true;registry.cancelShutdown();auto later=registry.acquire();check(bool(later),"cancel admits new value work");later.reset();registry.beginShutdown();wait.start(elapsed.elapsed(),1000);}if(state==ks::ui::AsyncShutdownWait::Status::Ready){finished=true;poll.stop();QCoreApplication::quit();}});
 QTimer::singleShot(0,&app,[]{ks::ui::RequestCoordinatedApplicationQuit();});QTimer::singleShot(45,&app,[&]{allowFinish.release();});QTimer::singleShot(2000,&app,[&]{allowFinish.release();QCoreApplication::quit();});app.exec();worker.join();
 check(requested&&finished&&timedOut,"quit waits, reaches timeout policy, retries and finishes");check(heartbeats>=5&&stopped==1&&closed==1&&registry.pending()==0,"event loop remains live until final STOP/handle close/lease release");ks::ui::RemoveCoordinatedApplicationQuit(&owner);
 preQtRestartTests();runtimeRestartTests(app);
 std::cout<<"SHUTDOWN_CORE_CHECKS="<<checks<<"\nGUI_WIDGETS_CREATED=0\nBUSINESS_IO_CALLS=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
preqt_names=('std::wstring parametersForPreQtRestart(', 'bool tryLaunchElevatedSelfBeforeSplash(', 'bool tryCreateRestartedSelfBeforeSplash(')
preqt_methods='\n'.join(extract.extract_method(startup,name)for name in preqt_names)
fixture=fixture.replace('__PREQT_METHODS__',preqt_methods)
(out/'fixture.cpp').write_text(fixture,encoding='utf-8')
vc=Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207');qt=root/'.deps/Qt/6.9.3/msvc2022_64'
if not (qt/'lib/Qt6Core.lib').exists():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
sdk=Path('C:/Program Files (x86)/Windows Kits/10');flags=['/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/utf-8','/EHsc','/MD','/W4','/WX','/external:W0','/DNOMINMAX','/DQT_CORE_LIB','/I'+str(out),'/I'+str(vc/'include')]
for mod in ('','QtCore'):flags+=['/external:I'+str(qt/'include'/mod)]
for part in ('ucrt','shared','um'):flags+=['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
log=out/'result.log';log.write_text('SOURCE_PATH_CHECKS='+str(checks)+'\n',encoding='utf-8')
def run(command,env=None,expected_failure=None):
    r=subprocess.run(command,cwd=out,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
    with log.open('a',encoding='utf-8')as f:f.write(r.stdout+r.stderr+'\nEXIT_CODE='+str(r.returncode)+'\n')
    print(r.stdout+r.stderr,end='')
    if expected_failure is not None:
        check(r.returncode==1 and expected_failure in r.stderr,'old production source must fail the targeted exit-race assertion')
        print('OLD_SOURCE_COUNTEREXAMPLE=PASS');return
    r.check_returncode()
obj=out/'fixture.obj';run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(out/'fixture.cpp'),'/Fo'+str(obj)]);exe=out/'shutdown_tests.exe';libs=['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64')for part in('ucrt','um')]
run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),str(obj),*libs,str(qt/'lib/Qt6Core.lib'),'shell32.lib']);env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');run([str(exe)],env);print('SOURCE_PATH_CHECKS='+str(checks))
matrix=r'''
#include <cstdint>
#include <iostream>
#include <stdexcept>
using HANDLE=std::uintptr_t;using DWORD=unsigned long;
constexpr DWORD MAXDWORD=0xffffffffUL,WAIT_OBJECT_0=0,WAIT_TIMEOUT=258,WAIT_FAILED=0xffffffffUL,ERROR_INVALID_PARAMETER=87,ERROR_ACCESS_DENIED=5,SYNCHRONIZE=1,PROCESS_QUERY_LIMITED_INFORMATION=2,STILL_ACTIVE=259;constexpr int FALSE=0;constexpr const wchar_t* kCrashRestartWaitPidArgument=L"wait";
__ENUM__
static bool startupRejected(bool privilegeRestartLaunch,RestartPredecessorStatus restartStatus){(void)privilegeRestartLaunch;__GATE__}
static bool requested=true,parsed=true,direct=true,same=true,imageQuery=true,older=true,exitQuery=true,exitDuringImageQuery=false;static unsigned long long pid=100;static HANDLE opened=100;static DWORD error=0,waitResult=WAIT_OBJECT_0,probeResult=WAIT_TIMEOUT,childId=300,fakeChildStatus=STILL_ACTIVE;static int opens=0,waits=0,probes=0,imageQueries=0,closes=0,checks=0;
static void check(bool v,const char* message){if(!v)throw std::runtime_error(message);++checks;}
bool CommandLineHasArgument(const wchar_t*){return requested;}bool TryReadArgumentValue(const wchar_t*,unsigned long long* value){*value=pid;return parsed;}DWORD GetCurrentProcessId(){return 200;}HANDLE GetCurrentProcess(){return 200;}DWORD GetProcessId(HANDLE){return childId;}bool GetExitCodeProcess(HANDLE,DWORD* value){*value=fakeChildStatus;return exitQuery;}bool IsCurrentProcessDirectChildOf(DWORD value){return value==pid&&direct;}bool IsProcessDirectChildOf(DWORD value,DWORD parent){return value==childId&&parent==200&&direct;}HANDLE OpenProcess(DWORD,int,DWORD value){++opens;check(value==pid,"only requested predecessor is opened");return opened;}DWORD GetLastError(){return error;}bool IsSameExecutableProcess(HANDLE,bool* succeeded=nullptr){++imageQueries;if(succeeded)*succeeded=imageQuery;if(exitDuringImageQuery)probeResult=WAIT_OBJECT_0;return imageQuery&&same;}bool IsParentOlderThanChild(HANDLE,HANDLE){return older;}void CloseHandle(HANDLE){++closes;}DWORD WaitForSingleObject(HANDLE,DWORD timeout){if(timeout==0){++probes;return probeResult;}++waits;check(timeout==35000,"bounded predecessor timeout preserved after identity validation");return waitResult;}
__METHODS__
static void reset(){requested=parsed=direct=same=imageQuery=older=exitQuery=true;exitDuringImageQuery=false;pid=100;opened=100;error=0;waitResult=WAIT_OBJECT_0;probeResult=WAIT_TIMEOUT;childId=300;fakeChildStatus=STILL_ACTIVE;opens=waits=probes=imageQueries=closes=0;}
int main(){try{using S=RestartPredecessorStatus;
 check(!startupRejected(true,S::NotRequested),"standalone pre-Qt privilege marker is allowed without a runtime predecessor wait");
 check(!startupRejected(false,S::NotRequested),"ordinary startup has no restart gate");
 for(auto status:{S::InvalidArgument,S::ValidationRejected,S::WaitFailed,S::TimedOut})check(startupRejected(true,status)&&startupRejected(false,status),"explicit runtime predecessor failures remain rejected before Qt");
 for(auto status:{S::PredecessorExited,S::PredecessorAlreadyExited})check(!startupRejected(true,status)&&!startupRejected(false,status),"validated runtime predecessor exits allow continuation");
 reset();requested=false;check(InspectRestartPredecessorFromCommandLine(35000)==S::NotRequested&&opens==0,"ordinary launch performs no predecessor work");
 for(auto value:{0ULL,200ULL,0x100000000ULL}){reset();pid=value;check(InspectRestartPredecessorFromCommandLine(35000)==S::InvalidArgument&&opens==0,"invalid/self/oversized PID rejected before open");}
 reset();parsed=false;check(InspectRestartPredecessorFromCommandLine(35000)==S::InvalidArgument,"malformed PID rejected");
 reset();direct=false;check(InspectRestartPredecessorFromCommandLine(35000)==S::ValidationRejected&&opens==0&&waits==0,"unrelated or broker parent cannot wait arbitrary PID");
 reset();opened=0;error=ERROR_INVALID_PARAMETER;check(InspectRestartPredecessorFromCommandLine(35000)==S::PredecessorAlreadyExited&&waits==0,"only exact recorded but absent parent is already exited");
 reset();opened=0;error=ERROR_ACCESS_DENIED;check(InspectRestartPredecessorFromCommandLine(35000)==S::ValidationRejected&&waits==0,"access failure cannot masquerade as parent exit");
 reset();imageQuery=false;probeResult=WAIT_OBJECT_0;check(InspectRestartPredecessorFromCommandLine(35000)==S::PredecessorAlreadyExited&&probes==1&&waits==0&&closes==1,"exited direct parent with retained process object proceeds after zero-time exit probe");
 reset();imageQuery=false;exitDuringImageQuery=true;check(InspectRestartPredecessorFromCommandLine(35000)==S::PredecessorAlreadyExited&&probes==1&&waits==0&&closes==1,"parent exiting during image query is observed by subsequent probe");
 reset();same=false;check(InspectRestartPredecessorFromCommandLine(35000)==S::ValidationRejected&&probes==0&&waits==0&&closes==1,"different live executable rejected without exit exception");
 reset();same=false;probeResult=WAIT_OBJECT_0;check(InspectRestartPredecessorFromCommandLine(35000)==S::ValidationRejected&&probes==0&&waits==0&&closes==1,"known wrong executable stays rejected even if subsequently exited");
 reset();imageQuery=false;check(InspectRestartPredecessorFromCommandLine(35000)==S::ValidationRejected&&probes==1&&waits==0&&closes==1,"access-denied image query on live parent cannot masquerade as exit");
 reset();imageQuery=false;probeResult=WAIT_FAILED;check(InspectRestartPredecessorFromCommandLine(35000)==S::WaitFailed&&probes==1&&waits==0&&closes==1,"failed zero-time exit probe stays a startup failure");
 reset();imageQuery=false;older=false;probeResult=WAIT_OBJECT_0;check(InspectRestartPredecessorFromCommandLine(35000)==S::ValidationRejected&&imageQueries==0&&probes==0&&closes==1,"exited process with recycled or unverified creation time cannot proceed");
 reset();older=false;check(InspectRestartPredecessorFromCommandLine(35000)==S::ValidationRejected&&waits==0&&closes==1,"recycled newer PID rejected");
 reset();check(InspectRestartPredecessorFromCommandLine(35000)==S::PredecessorExited&&waits==1&&closes==1,"validated parent observed exited");
 reset();waitResult=WAIT_TIMEOUT;check(InspectRestartPredecessorFromCommandLine(35000)==S::TimedOut&&closes==1,"timeout remains failure before business initialization");
 reset();waitResult=WAIT_FAILED;check(InspectRestartPredecessorFromCommandLine(35000)==S::WaitFailed&&closes==1,"wait failure remains failure");
 reset();check(ValidateRestartChildProcess(300),"parent accepts exact same-exe direct child");direct=false;check(!ValidateRestartChildProcess(300),"parent rejects brokered non-child");direct=true;same=false;check(!ValidateRestartChildProcess(300),"parent rejects wrong executable");same=true;older=false;check(!ValidateRestartChildProcess(300),"parent rejects recycled child identity");older=true;check(!ValidateRestartChildProcess(0),"parent rejects missing process handle");childId=0;check(!ValidateRestartChildProcess(300),"parent rejects unqueryable child ID");
 reset();fakeChildStatus=2;check(!ValidateRestartChildProcess(300),"parent cannot abandon itself for exited successor");fakeChildStatus=STILL_ACTIVE;exitQuery=false;check(!ValidateRestartChildProcess(300),"unqueryable successor liveness rejects handoff");
 std::cout<<"RESTART_MATRIX_CHECKS="<<checks<<"\nREAL_PROCESS_LAUNCHES=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
matrix=matrix.replace('childId','fakeChildId')
enum=re.search(r'enum class RestartPredecessorStatus\s*\{.*?\};',crash_header,re.S).group(0)
methods='\n'.join(extract.extract_method(crash,name)for name in('bool ValidateRestartChildProcess(','RestartPredecessorStatus InspectRestartPredecessorFromCommandLine('))
def startup_gate(text):
    begin=text.index('const bool crashRestartWait =');end=text.index('startupTraceRaw("restart handoff rejected or timed out before business initialization")',begin);part=text[begin:end];guard=part.index('if (');variables=part[:guard];condition=re.search(r'if \((.*?)\)\s*\{',part[guard:],re.S).group(1);return variables.replace('ks::crash::','')+'return '+condition.replace('ks::crash::','')+';'
def run_matrix(methods,label,expected_failure=None,startup_text=startup):
    cpp=out/(label+'.cpp');cpp.write_text(matrix.replace('__ENUM__',enum).replace('__METHODS__',methods).replace('__GATE__',startup_gate(startup_text)),encoding='utf-8');obj=out/(label+'.obj');run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(cpp),'/Fo'+str(obj)]);exe=out/(label+'_tests.exe');run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),str(obj),*libs]);run([str(exe)],env,expected_failure)
run_matrix(methods,'matrix')
if a.counterexample_crash_source:
    old=a.counterexample_crash_source.read_text(encoding='utf-8-sig')
    old_methods='\n'.join(extract.extract_method(old,name)for name in('bool ValidateRestartChildProcess(','RestartPredecessorStatus InspectRestartPredecessorFromCommandLine('))
    run_matrix(old_methods,'old_matrix','exited direct parent with retained process object proceeds after zero-time exit probe')

if a.counterexample_startup_source:
    old_startup=a.counterexample_startup_source.read_text(encoding="utf-8-sig")
    run_matrix(methods,"old_gate_matrix","standalone pre-Qt privilege marker is allowed without a runtime predecessor wait",old_startup)
