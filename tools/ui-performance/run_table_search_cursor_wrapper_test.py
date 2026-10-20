"""Compile verbatim production wrapper bodies with a QObject table host (no GUI)."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--source-root', type=Path, default=Path.cwd())
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--qt-dir', type=Path, default=Path('D:/Software/Qt/6.9.3/msvc2022_64'))
parser.add_argument('--vcvars', type=Path, default=Path('D:/Software/VS/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
source_root = args.source_root.resolve()
build_dir = args.build_dir.resolve()
build_dir.mkdir(parents=True, exist_ok=True)
ui = source_root / 'Ksword5.1/Ksword5.1/UI'
production = ui / 'TableSearchSupport.cpp'
text = production.read_text(encoding='utf-8')
start = text.index('    struct TableCellSearchCursor')
end = text.index('    void RevealTableCellSearchMatch', start)
body = text[start:end]
extracted = build_dir / 'table_search_wrapper_actual.inc'
extracted.write_text(body, encoding='utf-8')
test = Path(__file__).with_name('table_search_cursor_wrapper_test.cpp').resolve()
qt = args.qt_dir.resolve()
compiler = ['cl', '/nologo', '/std:c++17', '/Zc:__cplusplus', '/permissive-', '/EHsc', '/MD', '/utf-8', '/O2']
if 'bool IsTableCellSearchCurrent(' in body:
    compiler += ['/DHAS_TABLE_SEARCH_CURRENT=1']
compiler += [f'/I{qt / "include"}', f'/I{qt / "include/QtCore"}', f'/I{ui}', f'/I{build_dir}', str(test),
             f'/Fo{build_dir / "wrapper.obj"}', f'/Fe{build_dir / "wrapper.exe"}', '/link', f'/LIBPATH:{qt / "lib"}', 'Qt6Core.lib']
batch = build_dir / 'compile-wrapper.cmd'
batch.write_text('@echo off\ncall "' + str(args.vcvars.resolve()) + '" >nul\n' + subprocess.list2cmdline(compiler)
                 + '\nexit /b %errorlevel%\n', encoding='utf-8')
result = subprocess.run(['cmd', '/d', '/c', str(batch)], cwd=build_dir)
if result.returncode:
    raise SystemExit(result.returncode)
env = os.environ.copy()
env['PATH'] = str(qt / 'bin') + os.pathsep + env.get('PATH', '')
result = subprocess.run([str(build_dir / 'wrapper.exe')], cwd=build_dir, env=env)
provenance = {'source': str(production), 'source_sha256': hashlib.sha256(production.read_bytes()).hexdigest(),
              'actual_wrapper_sha256': hashlib.sha256(body.encode()).hexdigest(),
              'core_header_sha256': hashlib.sha256((ui / 'ModelTextSearchCursor.h').read_bytes()).hexdigest(),
              'has_current_check': 'bool IsTableCellSearchCurrent(' in body, 'exit_code': result.returncode}
(build_dir / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
raise SystemExit(result.returncode)
