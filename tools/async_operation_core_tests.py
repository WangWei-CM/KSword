"""Compile production AsyncOperation with QtCore only; no GUI or business IO."""
from pathlib import Path
import argparse, os, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repository-root', type=Path, default=Path(__file__).resolve().parents[1])
p.add_argument('--source-root', type=Path)
p.add_argument('--output-dir', type=Path, required=True)
a = p.parse_args()
root = a.repository_root.resolve()
source = (a.source_root or root).resolve()
out = a.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
ui = Path('Ksword5.1/Ksword5.1/UI')
for name in ('AsyncOperation.h', 'AsyncOperation.cpp', 'AsyncUiDispatcher.h'):
    src = source / ui / name
    (out / name).write_bytes((src if src.exists() else root / ui / name).read_bytes())
fixture = r'''
#include "AsyncOperation.h"
#include <QCoreApplication>
#include <QElapsedTimer>
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
 pool->waitForDone();pool->setMaxThreadCount(threads);
 std::cout<<"ASYNC_CORE_CHECKS="<<checks<<"\nPOOL_ORDER=high,default,low\nGUI_WIDGETS_CREATED=0\nBUSINESS_IO_CALLS=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
(out / 'fixture.cpp').write_text(fixture, encoding='utf-8')
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
    with log.open('a',encoding='utf-8') as stream:stream.write(result.stdout+result.stderr+'\nEXIT_CODE='+str(result.returncode)+'\n')
    print(result.stdout+result.stderr,end='')
    result.check_returncode()
objects=[]
for name in ('fixture','AsyncOperation'):
    obj=out/(name+'.obj');run([str(vc/'bin/Hostx64/x64/cl.exe'),*flags,'/c',str(out/(name+'.cpp')),'/Fo'+str(obj)]);objects.append(str(obj))
exe=out/'async_core_tests.exe'
libs=['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64') for part in ('ucrt','um')]
run([str(vc/'bin/Hostx64/x64/link.exe'),'/nologo','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),*objects,*libs,str(qt/'lib/Qt6Core.lib')])
env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');run([str(exe)],env)
