from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess


parser = argparse.ArgumentParser(description='Test exact DDMA submission against the production cleanup registry and a fake queue.')
parser.add_argument('--repository-root', type=Path, required=True)
parser.add_argument('--source', type=Path, required=True, help='Production MemoryAccessBackend.cpp')
parser.add_argument('--proof-directory', type=Path, required=True)
parser.add_argument('--qt-root', type=Path, required=True)
parser.add_argument('--vcvars', type=Path, required=True)
parser.add_argument('--runs', type=int, default=20)
args = parser.parse_args()
if args.runs < 1:
    parser.error('--runs must be positive')
root = args.repository_root.resolve()
proof = args.proof_directory.resolve()
proof.mkdir(parents=True, exist_ok=True)
production = args.source.resolve()
qt = args.qt_root.resolve()
vcvars = args.vcvars.resolve()
source_bytes = production.read_bytes()
source = source_bytes.decode('utf-8')
start = source.index('    bool submitDdmaTransaction(std::function<void()> transaction)')
end = source.index('    std::uint32_t ddmaTransferBytes()', start)
exact = source[start:end]
assert 'AsyncCleanupRegistry::global().acquire()' in exact
assert exact.index('acquire()') < exact.index('queue.trySubmit(')
assert exact.index('auto acceptedCleanup') < exact.index('auto work') < exact.index('transactionLock')
extracted = proof/'DdmaCleanupSubmission.Extracted.h'
extracted.write_bytes(('namespace ksword::memory_backend\n{\n'+exact+'}\n').encode('utf-8'))
registry = root/'Ksword5.1/Ksword5.1/UI/AsyncCleanupRegistry.h'
backend_header = root/'Ksword5.1/Ksword5.1/MemoryDock/MemoryAccessBackend.h'
serial_header = root/'shared/evidence/SerialTransactionQueue.h'
main_window = root/'Ksword5.1/Ksword5.1/MainWindow.cpp'
main_source = main_window.read_text(encoding='utf-8')
close_start = main_source.index('void MainWindow::closeEvent(')
close_end = main_source.index('\nvoid MainWindow::', close_start+1)
close_body = main_source[close_start:close_end]
assert close_body.index('AsyncCleanupRegistry::global().beginShutdown()') < close_body.index('AsyncCleanupRegistry::global().pending()')
assert close_body.index('AsyncCleanupRegistry::global().pending()') < close_body.index('prepareR0DriverServiceStop')
assert 'AsyncCleanupRegistry::global().cancelShutdown()' in main_source
manifest = {
    'production_source': str(production),
    'production_source_sha256': hashlib.sha256(source_bytes).hexdigest(),
    'source_start_line': source[:start].count('\n')+1,
    'source_end_line': source[:end].count('\n'),
    'function_sha256': hashlib.sha256(exact.encode('utf-8')).hexdigest(),
    'registry_header': str(registry),
    'registry_sha256': hashlib.sha256(registry.read_bytes()).hexdigest(),
    'backend_header_sha256': hashlib.sha256(backend_header.read_bytes()).hexdigest(),
    'serial_queue_header_sha256': hashlib.sha256(serial_header.read_bytes()).hexdigest(),
    'main_window_sha256': hashlib.sha256(main_window.read_bytes()).hexdigest(),
    'main_close_order_static_checks': 'beginShutdown < pending branch < prepareR0DriverServiceStop; cancellation reopens registry',
    'exact_source_substring_verified': exact in extracted.read_bytes().decode('utf-8'),
    'boundary': 'Exact production submitDdmaTransaction and actual AsyncCleanupRegistry/AsyncShutdownWait. Fake queue admission, gate/state stubs, fake MainWindow R0 stop/quit spies; actual QtCore event loop/QObject destruction. No QWidget, product, service, driver, or real target IO.'
}
(proof/'extraction-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
test = Path(__file__).resolve().with_suffix('.cpp')
exe = proof/'ddma_cleanup_lease_tests.exe'
arguments = ['cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/O2', '/W4', '/WX',
    '/utf-8', '/Zc:__cplusplus', '/permissive-', '/DNOMINMAX',
    f'/I{proof}', f'/I{registry.parent}', f'/I{qt}/include', f'/I{qt}/include/QtCore', str(test),
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
full_arguments = ['cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/O2', '/W4', '/WX',
    '/utf-8', '/Zc:__cplusplus', '/permissive-', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
    f'/I{backend_header.parent}', f'/I{qt}/include', f'/I{qt}/include/QtCore', '/c', str(production),
    f'/Fo{proof}/MemoryAccessBackend.obj']
full_script = f'@echo off\ncall "{vcvars}"\nif errorlevel 1 exit /b %errorlevel%\n'
full_script += subprocess.list2cmdline(full_arguments)+'\nexit /b %errorlevel%\n'
(proof/'compile-full-backend.cmd').write_text(full_script, encoding='utf-8')
full_build = subprocess.run(['cmd.exe', '/d', '/c', str(proof/'compile-full-backend.cmd')], cwd=proof,
    capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
(proof/'compile-full-backend.log').write_text(full_build.stdout+full_build.stderr, encoding='utf-8')
if full_build.returncode:
    print(full_build.stdout, full_build.stderr)
    raise SystemExit(full_build.returncode)
environment = os.environ.copy()
environment['PATH'] = str(qt/'bin')+os.pathsep+environment.get('PATH','')
runs = []
for index in range(args.runs):
    run = subprocess.run([str(exe)], cwd=proof, env=environment, capture_output=True,
        text=True, encoding='utf-8', errors='replace', timeout=10)
    runs.append({'run': index+1, 'exit_code': run.returncode, 'stdout': run.stdout.strip(), 'stderr': run.stderr.strip()})
    if run.returncode or run.stderr or not re.fullmatch(r'PASS DdmaCleanupLease \d+ checks; 0 failures; heartbeat=\d+', run.stdout.strip()):
        (proof/'failure-evidence.json').write_text(json.dumps(runs, indent=2)+'\n', encoding='utf-8')
        print(run.stdout, run.stderr)
        raise SystemExit(run.returncode or 1)
evidence = {'result': 'PASS', 'compile_exit_code': build.returncode,
    'full_production_translation_unit_compile_exit_code': full_build.returncode,
    'function_sha256': manifest['function_sha256'],
    'test_source_sha256': hashlib.sha256(test.read_bytes()).hexdigest(),
    'repetitions': len(runs), 'runs': runs, 'boundary': manifest['boundary']}
(proof/'test-evidence.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
print(f'PASS {len(runs)} repetitions; {runs[-1]["stdout"]}; zero stderr')
