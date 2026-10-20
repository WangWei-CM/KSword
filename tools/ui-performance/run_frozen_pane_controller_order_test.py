"""Exercise production layout/registration method order with QObject mocks."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
parser=argparse.ArgumentParser()
parser.add_argument('--source-root',type=Path,default=Path.cwd())
parser.add_argument('--build-dir',type=Path,required=True)
args=parser.parse_args()
build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
ui=args.source_root.resolve()/'Ksword5.1/Ksword5.1/UI'
source=ui/'TableFreezeSupport.cpp';text=source.read_text(encoding='utf-8')
def body(marker):
    start=text.index(marker);brace=text.index('{',start);depth=1;end=brace+1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    return text[start:end]
actual='\n\n'.join(body(marker) for marker in [
    '    std::shared_ptr<FrozenPaneRowSync> TableFrozenPaneController::rowSyncFor(',
    '    bool TableFrozenPaneController::rowSyncPending(',
    '    void TableFrozenPaneController::mirrorVisibleRowLayout(',
    '    void TableFrozenPaneController::layoutPanes()'])
(build/'frozen_pane_controller_actual.inc').write_text(actual,encoding='utf-8')
qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
command=['cl','/nologo','/std:c++17','/Zc:__cplusplus','/permissive-','/EHsc','/MD','/utf-8','/O2',
         f'/I{qt/"include"}',f'/I{qt/"include/QtCore"}',f'/I{ui}',f'/I{build}',
         str(Path(__file__).with_name('frozen_pane_controller_order_test.cpp').resolve()),
         f'/Fo{build/"test.obj"}',f'/Fe{build/"test.exe"}','/link',f'/LIBPATH:{qt/"lib"}','Qt6Core.lib']
batch=build/'compile.cmd';batch.write_text('@echo off\ncall D:\\Software\\VS\\VC\\Auxiliary\\Build\\vcvars64.bat >nul\n'
    +subprocess.list2cmdline(command)+'\nexit /b %errorlevel%\n',encoding='utf-8')
r=subprocess.run(['cmd','/d','/c',str(batch)],cwd=build)
if r.returncode:raise SystemExit(r.returncode)
env=os.environ.copy();env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','')
r=subprocess.run([str(build/'test.exe')],cwd=build,env=env)
(build/'provenance.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
    'actual_methods_sha256':hashlib.sha256(actual.encode()).hexdigest(),'exit_code':r.returncode},indent=2)+'\n',encoding='utf-8')
raise SystemExit(r.returncode)
