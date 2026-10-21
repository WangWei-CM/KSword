"""QtCore fake-service replay of actual Process ETW adapters and retained cleanup API."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import time


def declaration(source, signature):
    start=source.index(signature); brace=source.index('{',start); depth=1; end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path)
    parser.add_argument('--retained-header',type=Path)
    parser.add_argument('--registry-dir',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--qt',type=Path,default=Path('D:/Software/Qt/6.9.3/msvc2022_64'))
    args=parser.parse_args();root=Path(__file__).resolve().parents[1]
    source_path=args.source or root/'Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp'
    retained=args.retained_header or root/'Ksword5.1/Ksword5.1/UI/RetainedCleanupJob.h'
    registry=args.registry_dir or retained.parent
    output=args.output or root/'.codex-build-logs/process-etw-retained-cleanup-tests';output.mkdir(parents=True,exist_ok=True)
    source=source_path.read_text(encoding='utf-8-sig');a=source.index('    template<typename Service>\n    bool stopProcessEtwCaptureService');z=source.index('    /*\n     * R-1',a)
    fixture=Path(__file__).with_suffix('.cpp').read_text(encoding='utf-8-sig').replace('__HELPERS__',source[a:z]).replace('__LIFECYCLE_HEADER__',source_path.with_name('ProcessEtwCaptureLifecycle.h').resolve().as_posix()).replace('__RETAINED_HEADER__',retained.resolve().as_posix())
    cpp=output/'fixture.cpp';cpp.write_text(fixture,encoding='utf-8')
    developers=[Path('D:/Software/VS/Common7/Tools/VsDevCmd.bat'),Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat')];developer=next(p for p in developers if p.exists())
    command=['cl.exe','/nologo','/Zc:__cplusplus','/std:c++20','/utf-8','/EHsc','/MD','/O2','/W4','/WX','/I'+str(args.qt/'include'),'/I'+str(args.qt/'include/QtCore'),'/I'+str(registry.resolve()),str(cpp.resolve()),'/Fo'+str((output/'fixture.obj').resolve()),'/Fe'+str((output/'fixture.exe').resolve()),'/link','/LIBPATH:'+str(args.qt/'lib'),'Qt6Core.lib']
    build=output/'build.cmd';build.write_text('@echo off\ncall "'+str(developer)+'" -arch=x64 -host_arch=x64 >nul\n'+subprocess.list2cmdline(command)+'\nexit /b %errorlevel%\n',encoding='utf-8')
    compiled=subprocess.run(['cmd.exe','/d','/c',str(build.resolve())],capture_output=True,text=True);(output/'compile.log').write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
    if compiled.returncode:print(compiled.stdout+compiled.stderr);raise SystemExit(compiled.returncode)
    env=dict(os.environ);env['PATH']=str(args.qt/'bin')+os.pathsep+env.get('PATH','');started=time.perf_counter()
    result=subprocess.run([str((output/'fixture.exe').resolve())],env=env,capture_output=True,text=True,timeout=45)
    record={'returncode':result.returncode,'elapsed_seconds':round(time.perf_counter()-started,3),'stdout':result.stdout,'stderr':result.stderr,'scope':'actual lifecycle+service adapter/request/cleanup-owner factory and actual RetainedCleanupJob+registry; QtCore fake network/CPU services only, no GUI/ETW/OS session'}
    (output/'result.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps(record))
    if result.returncode:raise SystemExit(result.returncode)
    for method,owner in [('void ProcessDock::ensureProcessNetworkTrafficCaptureStarted()','m_processNetworkCaptureOwner'),('void ProcessDock::ensureCpuCoreUsageCaptureStarted()','m_cpuCoreCaptureOwner')]:
        value=declaration(source,method)
        assert value.index('makeProcessEtwCleanupOwner(')<value.index('requestProcessEtwCapture(')
        assert value.index(owner+'->isCleanupRequested()')<value.index('requestProcessEtwCapture(')
        assert '->Close();' in value
        assert 'releaseUnadmittedProcessEtwValue(std::move(' in value
        assert 'Generation() + 1' in value
    destructor=declaration(source,'ProcessDock::~ProcessDock()')
    assert destructor.index('m_processNetworkCaptureOwner->requestCleanup()')<destructor.index('releaseCaptureOwners')
    assert destructor.index('m_cpuCoreCaptureOwner->requestCleanup()')<destructor.index('releaseCaptureOwners')
    pump=source[source.index('    template<typename Service, typename StartedCallback>'):z]
    assert pump.index('LockBackgroundOperation()')<pump.index('NextOperation()')
    print('PASS: admission before Start, requested-owner guard, close request before value release and backend-gated operation selection')


if __name__=='__main__':main()
