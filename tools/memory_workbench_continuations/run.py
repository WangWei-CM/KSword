"""QtCore-only continuation tests; no product, widget, driver or live target run."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess
p=argparse.ArgumentParser();p.add_argument('--repository-root',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--output-dir',type=Path,required=True);args=p.parse_args()
root=args.repository_root.resolve();out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True);test=Path(__file__).resolve().parent;ui=root/'Ksword5.1/Ksword5.1/UI/MemoryWorkbench';shared=root/'shared/evidence/memory_workbench'
qt=next((x for x in [root/'.deps/Qt/6.9.3/msvc2022_64',Path('D:/Software/Qt/6.9.3/msvc2022_64')] if (x/'bin/moc.exe').exists()),None)
vs=next((x for x in [Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat'),Path('D:/Software/VS/Common7/Tools/VsDevCmd.bat')] if x.exists()),None)
if not qt or not vs:raise SystemExit('x64 MSVC and Qt 6.9.3 MSVC are required')
source=ui/'MemoryWorkbenchView.cpp';text=source.read_text(encoding='utf-8-sig');begin=text.index('    LeaveEntryStatus MemoryWorkbenchView::confirmQuit(');end=text.index('    // wireActions',begin);function=text[begin:end]
(out/'MemoryWorkbenchView.LeaveExact.cpp').write_text('#include "view_leave_shim.h"\nnamespace ks::ui {\n'+function+'\n}\n',encoding='utf-8')
common=['/nologo','/std:c++20','/EHsc','/MD','/O2','/Gy','/W4','/WX','/utf-8','/Zc:__cplusplus','/permissive-']+[f'/I"{x}"' for x in [test,ui,shared,qt/'include',qt/'include/QtCore']]
target=[ui/'WorkbenchTarget.cpp',out/'moc_WorkbenchTarget.cpp',test/'target_tests.cpp']+[shared/(x+'.cpp') for x in ['MemoryTargetSession','MemoryTargetTracker','MemoryModuleDirectory','MemoryAddressExpr','SessionAddressResolver','MemoryProcessMatch']]
leave=[out/'MemoryWorkbenchView.LeaveExact.cpp',test/'view_leave_tests.cpp',shared/'MemoryTargetSession.cpp']
for name,sources in [('target',target),('leave',leave)]:
 rsp=common+[f'"{x}"' for x in sources]+[f'/Fo"{out}/"',f'/Fe"{out}/{name}.exe"','/link',f'/LIBPATH:"{qt}/lib"','Qt6Core.lib','/OPT:REF'];(out/(name+'.rsp')).write_text(' '.join(rsp),encoding='utf-8')
cmd=f'@echo off\ncall "{vs}" -arch=x64 -host_arch=x64\nif errorlevel 1 exit /b %errorlevel%\n"{qt}/bin/moc.exe" -I"{ui}" "{ui}/WorkbenchTarget.h" -o "{out}/moc_WorkbenchTarget.cpp"\nif errorlevel 1 exit /b %errorlevel%\ncl.exe @"{out}/target.rsp"\nif errorlevel 1 exit /b %errorlevel%\ncl.exe @"{out}/leave.rsp"\nexit /b %errorlevel%\n';(out/'compile.cmd').write_text(cmd,encoding='utf-8')
compiled=subprocess.run(['cmd.exe','/d','/c',str(out/'compile.cmd')],capture_output=True,text=True,encoding='utf-8',errors='replace');(out/'compile.log').write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
if compiled.returncode:print(compiled.stdout+compiled.stderr);raise SystemExit(compiled.returncode)
env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env['PATH'];results=[]
for name in ['target','leave']:
 r=subprocess.run([str(out/(name+'.exe'))],env=env,capture_output=True,text=True);results.append({'test':name,'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr});print(r.stdout.strip());
 if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
(out/'results.json').write_text(json.dumps({'method_source_sha256':hashlib.sha256(function.encode()).hexdigest(),'results':results},indent=2),encoding='utf-8')
