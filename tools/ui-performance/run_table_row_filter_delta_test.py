from pathlib import Path
import argparse,hashlib,json,os,subprocess
p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=Path.cwd());p.add_argument('--build-dir',type=Path,required=True);a=p.parse_args();build=a.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
test=Path(__file__).with_name('table_row_filter_delta_test.cpp');header=a.source_root.resolve()/'Ksword5.1/Ksword5.1/UI/TableRowFilterIdentityState.h';qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
actual=build/'test_actual.cpp';s=test.read_text(encoding='utf-8');marker='#include "../../Ksword5.1/Ksword5.1/UI/TableRowFilterIdentityState.h"';assert marker in s;s=s.replace(marker,'#include "'+header.as_posix()+'"',1);actual.write_text(s,encoding='utf-8')
cmd=['cl','/nologo','/std:c++17','/Zc:__cplusplus','/permissive-','/EHsc','/MD','/utf-8','/O2',f'/I{qt/"include"}',f'/I{qt/"include/QtCore"}',str(actual),f'/Fo{build}/',f'/Fe{build/"test.exe"}','/link',f'/LIBPATH:{qt/"lib"}','Qt6Core.lib'];batch=build/'compile.cmd';batch.write_text('@echo off\ncall D:\\Software\\VS\\VC\\Auxiliary\\Build\\vcvars64.bat >nul\n'+subprocess.list2cmdline(cmd)+'\nexit /b %errorlevel%\n',encoding='utf-8')
r=subprocess.run(['cmd','/d','/c',str(batch)],capture_output=True,text=True);log=r.stdout+r.stderr
if not r.returncode:
    env=os.environ.copy();env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');r=subprocess.run([str(build/'test.exe')],capture_output=True,text=True,env=env);log+=r.stdout+r.stderr
(build/'test.log').write_text(log,encoding='utf-8');print(log);(build/'provenance.json').write_text(json.dumps({'header_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(test.read_bytes()).hexdigest(),'exit_code':r.returncode},indent=2)+'\n',encoding='utf-8');raise SystemExit(r.returncode)
