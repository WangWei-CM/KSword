"""Compile and run extracted production filtering with QtCore only."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root',type=Path,required=True)
    parser.add_argument('--vcvars',type=Path,required=True)
    parser.add_argument('--source-root',type=Path)
    parser.add_argument('--support-root',type=Path)
    parser.add_argument('--build-dir',type=Path)
    args=parser.parse_args()
    root=(args.source_root or Path(__file__).resolve().parents[2]).resolve()
    support=(args.support_root or root).resolve()
    build=(args.build_dir or root/'.codex-build-logs/table-filter-identity-tests').resolve();build.mkdir(parents=True,exist_ok=True)
    ui=Path('Ksword5.1/Ksword5.1/UI')
    production=root/ui/'TableSearchSupport.cpp'
    text=production.read_text(encoding='utf-8-sig')
    methods=text[text.index('        bool applyResultFilter('):text.index('        void refreshPresentation()')]
    methods+=text[text.index('        void observeFilterModel('):text.index('        void installObservers()')]
    (build/'table_filter_production.inc').write_text(methods,encoding='utf-8')
    qt=args.qt_root.resolve();source=Path(__file__).with_name('table_filter_incremental_identity_test.cpp').resolve()
    exe=build/'filter-test.exe'
    command=['cl','/nologo','/std:c++17','/Zc:__cplusplus','/permissive-','/EHsc','/MD','/utf-8','/O2',
        '/I'+str(qt/'include'),'/I'+str(qt/'include/QtCore'),'/I'+str(build),'/I'+str(support/ui),str(source),
        '/Fo'+str(build/'filter-test.obj'),'/Fe'+str(exe),'/link','/LIBPATH:'+str(qt/'lib'),'Qt6Core.lib']
    script=build/'build-filter.cmd';script.write_text('@echo off\ncall '+subprocess.list2cmdline([str(args.vcvars.resolve())])+' >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\nexit /b %errorlevel%\n',encoding='utf-8')
    compiled=subprocess.run([str(script)],capture_output=True,text=True)
    (build/'filter-build.log').write_text(compiled.stdout+compiled.stderr,encoding='utf-8');print(compiled.stdout+compiled.stderr,end='')
    if compiled.returncode:return compiled.returncode
    env=dict(os.environ);env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','')
    tested=subprocess.run([str(exe)],capture_output=True,text=True,env=env)
    (build/'filter-test.log').write_text(tested.stdout+tested.stderr,encoding='utf-8');print(tested.stdout+tested.stderr,end='')
    evidence={'source_sha256':hashlib.sha256(production.read_bytes()).hexdigest(),
        'methods_sha256':hashlib.sha256(methods.encode()).hexdigest(),
        'identity_header_sha256':hashlib.sha256((support/ui/'TableRowFilterIdentityState.h').read_bytes()).hexdigest(),
        'exit_code':tested.returncode}
    (build/'provenance.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
    return tested.returncode

if __name__=='__main__':raise SystemExit(main())
