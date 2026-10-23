"""Build/run QtCore workbench write tests with fake IO and native anchor seams."""
from pathlib import Path
import json
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
qt = Path(os.environ.get('KSWORD_QT_DIR', root / '.deps/Qt/6.9.3/msvc2022_64'))
if not (qt / 'bin/moc.exe').is_file():
    qt = Path('D:/Software/Qt/6.9.3/msvc2022_64')
developer = Path('D:/Software/VS/Common7/Tools/VsDevCmd.bat')
if not developer.is_file():
    developer = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat')
if not developer.is_file() or not (qt / 'bin/moc.exe').is_file():
    raise SystemExit('HostX64 MSVC and Qt 6 are required; no product or driver runs are attempted.')
out = root / '.codex-build-logs/memwb-controller-tests'
out.mkdir(parents=True, exist_ok=True)
ui = root / 'Ksword5.1/Ksword5.1/UI'
wb = ui / 'MemoryWorkbench'
core = root / 'shared/evidence/memory_workbench'
sources = [Path(__file__).with_name('ControllerTests.cpp'), wb / 'WorkbenchWriteController.cpp',
    wb / 'WorkbenchWriteController.PendingStage.cpp', wb / 'WorkbenchWriteOperationRunner.cpp',
    wb / 'WorkbenchFrozenWriteContext.cpp', wb / 'WorkbenchWriteLedger.cpp', wb / 'WorkbenchTarget.cpp',
    ui / 'AsyncOperation.cpp', core / 'MemoryWriteOperation.cpp',
    out / 'moc_WorkbenchTarget.cpp', out / 'moc_WorkbenchWriteController.cpp']
sources += [core / name for name in ['MemoryDiffOverlay.cpp', 'MemoryDiffOverlay.Patches.cpp',
    'MemoryTargetSession.cpp', 'MemoryTargetTracker.cpp', 'MemoryModuleDirectory.cpp',
    'MemoryAddressExpr.cpp', 'SessionAddressResolver.cpp', 'MemoryProcessMatch.cpp',
    'MemoryIoByteStore.cpp', 'MemoryKernelMutation.cpp', 'MemoryEditJournal.cpp',
    'PointerChainBindings.cpp', 'MemoryAddressBook.cpp']]
sources += [root / 'shared/evidence/PointerChain.cpp']
args = ['/nologo', '/std:c++20', '/EHsc', '/MD', '/O2', '/Gy', '/W4', '/WX', '/utf-8',
    '/Zc:__cplusplus', '/permissive-', '/DQT_CORE_LIB', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
    f'/I"{wb}"', f'/I"{ui}"', f'/I"{qt / "include"}"', f'/I"{qt / "include/QtCore"}"',
    f'/Fe"{out / "controller-tests.exe"}"', f'/Fo"{str(out)}/"']
args += [f'"{source}"' for source in sources]
args += ['/link', f'/LIBPATH:"{qt / "lib"}"', 'Qt6Core.lib', '/OPT:REF']
(out / 'tests.rsp').write_text(' '.join(args) + '\n', encoding='utf-8')
cmd = ['@echo off', f'call "{developer}" -arch=x64 -host_arch=x64', 'if errorlevel 1 exit /b %errorlevel%']
for name in ['WorkbenchTarget', 'WorkbenchWriteController']:
    cmd += [f'"{qt / "bin/moc.exe"}" -I"{wb}" "{wb / (name + ".h")}" -o "{out / ("moc_" + name + ".cpp")}"',
            'if errorlevel 1 exit /b %errorlevel%']
cmd += [f'cl.exe @"{out / "tests.rsp"}"', 'if errorlevel 1 exit /b %errorlevel%',
    f'set "PATH={qt / "bin"};%PATH%"', f'"{out / "controller-tests.exe"}"', 'exit /b %errorlevel%']
(out / 'run.cmd').write_text('\r\n'.join(cmd) + '\r\n', encoding='ascii')
run = subprocess.run(['cmd', '/d', '/c', str(out / 'run.cmd')], cwd=out, capture_output=True)
(out / 'run.log').write_bytes(run.stdout + run.stderr)
(out / 'result.json').write_text(json.dumps({'exit_code': run.returncode,
    'boundary': 'QtCore actual controller/runner/core/frozen ports/ledger/target; fake IO/identity/confirmation/native anchors only'}, indent=2) + '\n', encoding='utf-8')
print((run.stdout + run.stderr).decode('utf-8', errors='replace'))
sys.exit(run.returncode)
