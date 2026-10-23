from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

parser = argparse.ArgumentParser(description='Compile exact production admission templates against a fake QtCore submitter.')
parser.add_argument('--source', type=Path, required=True, help='Production DdmaPage.cpp')
parser.add_argument('--proof-directory', type=Path, required=True)
parser.add_argument('--qt-root', type=Path, required=True)
parser.add_argument('--vcvars', type=Path, required=True)
parser.add_argument('--runs', type=int, default=20)
args = parser.parse_args()
if args.runs < 1:
    parser.error('--runs must be positive')
proof = args.proof_directory.resolve()
proof.mkdir(parents=True, exist_ok=True)
production = args.source.resolve()
qt = args.qt_root.resolve()
vcvars = args.vcvars.resolve()
source = production.read_bytes().decode('utf-8')
result_marker = source.index('    struct DdmaTaskResult')
start = source.rfind('    template<typename Value>', 0, result_marker)
end = source.index('    // formatNtStatus', start)
exact = source[start:end]
assert 'void submitPageTransaction(QObject* page, Work work, Apply apply)' in exact
assert 'catch (const std::exception& error)' in exact
extracted = proof/'DdmaSubmitPageTransaction.Extracted.h'
extracted.write_bytes(('namespace\n{\n'+exact+'}\n').encode('utf-8'))
manifest = {
    'production_source':str(production),
    'source_start_line':source[:start].count('\n')+1,
    'source_end_line':source[:end].count('\n'),
    'template_sha256':hashlib.sha256(exact.encode('utf-8')).hexdigest(),
    'exact_source_substring_verified':exact in extracted.read_bytes().decode('utf-8'),
    'boundary':'Production DdmaTaskResult and submitPageTransaction templates extracted byte-for-byte. Real QPromise/QFutureWatcher/QObject with fake submit boundary; no QWidget or backend/driver IO.'
}
(proof/'extraction-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
test = Path(__file__).resolve().with_suffix('.cpp')
exe = proof/'ddma_submission_admission_tests.exe'
arguments = ['cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/O2', '/W4', '/WX',
    '/utf-8', '/Zc:__cplusplus', '/permissive-', '/DNOMINMAX',
    f'/I{proof}', f'/I{qt}/include', f'/I{qt}/include/QtCore', str(test),
    f'/Fe{exe}', '/link', f'/LIBPATH:{qt}/lib', 'Qt6Core.lib']
script = f'@echo off\ncall "{vcvars}"\nif errorlevel 1 exit /b %errorlevel%\n'
script += subprocess.list2cmdline(arguments)+'\nexit /b %errorlevel%\n'
(proof/'compile.cmd').write_text(script, encoding='utf-8')
(proof/'compile-command.txt').write_text(script, encoding='utf-8')
build = subprocess.run(['cmd.exe', '/d', '/c', str(proof/'compile.cmd')], cwd=proof,
    capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
(proof/'compile.log').write_text(build.stdout+build.stderr, encoding='utf-8')
if build.returncode:
    print(build.stdout, build.stderr)
    raise SystemExit(build.returncode)
environment = os.environ.copy()
environment['PATH'] = str(qt/'bin')+os.pathsep+environment.get('PATH','')
runs = []
for index in range(args.runs):
    run = subprocess.run([str(exe)], cwd=proof, env=environment, capture_output=True,
        text=True, encoding='utf-8', errors='replace', timeout=10)
    runs.append({'run':index+1,'exit_code':run.returncode,'stdout':run.stdout.strip(),'stderr':run.stderr.strip()})
    if run.returncode or run.stderr:
        (proof/'failure-evidence.json').write_text(json.dumps(runs,indent=2)+'\n',encoding='utf-8')
        print(run.stdout, run.stderr)
        raise SystemExit(run.returncode or 1)
evidence = {'result':'PASS','compile_exit_code':build.returncode,'template_sha256':manifest['template_sha256'],
    'test_source_sha256':hashlib.sha256(test.read_bytes()).hexdigest(),'repetitions':len(runs),'runs':runs,
    'boundary':manifest['boundary']}
(proof/'test-evidence.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
print(f'PASS {len(runs)} repetitions; {runs[-1]["stdout"]}; zero stderr')
