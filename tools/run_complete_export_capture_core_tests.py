"""Compile native model/view adapter; exercise actual queued capture with Core fakes only."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-root', type=Path)
p.add_argument('--repository-root', type=Path)
p.add_argument('--output-dir', type=Path, required=True)
a = p.parse_args()
root = (a.repository_root or next(path for path in Path(__file__).resolve().parents if (path/'AGENTS.md').is_file())).resolve()
source = (a.source_root or root).resolve()
out = a.output_dir.resolve()
if not out.is_relative_to(root/'.codex-build-logs'):
    raise ValueError('Output must remain in ignored .codex-build-logs')
out.mkdir(parents=True, exist_ok=True)
(out/'UI').mkdir(exist_ok=True)
manifest = {'sources': {}, 'scope': 'QCoreApplication fake ports; no Widgets/clipboard/real output files/business'}
for name in ('CompleteExportCapture.h', 'CompleteExportCapture.cpp'):
    data = (source/'Ksword5.1/Ksword5.1/UI'/name).read_bytes()
    (out/'UI'/name).write_bytes(data)
    manifest['sources'][name] = hashlib.sha256(data).hexdigest()
(out/'fixture.cpp').write_bytes(Path(__file__).with_name('complete_export_capture_core_tests.cpp').read_bytes())
vc = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
if not (vc/'bin/Hostx64/x64/cl.exe').is_file():
    vc = Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207')
qt = root/'.deps/Qt/6.9.3/msvc2022_64'
if not (qt/'lib/Qt6Core.lib').is_file():
    qt = Path('D:/Software/Qt/6.9.3/msvc2022_64')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
flags = ['/nologo', '/std:c++20', '/Zc:__cplusplus', '/permissive-', '/utf-8', '/EHsc', '/MD', '/O2', '/W4', '/WX',
         '/external:W0', '/DNOMINMAX', '/DQT_CORE_LIB', '/DQT_GUI_LIB', '/DQT_WIDGETS_LIB', '/I'+str(out), '/I'+str(vc/'include')]
for module in ('', 'QtCore', 'QtGui', 'QtWidgets'):
    flags += ['/external:I'+str(qt/'include'/module)]
for part in ('ucrt', 'shared', 'um'):
    flags += ['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
log = out/'result.log'
log.write_text('HOST_TOOLS=Hostx64/x64\n', encoding='utf-8')
def run(command, env=None):
    result = subprocess.run(command, cwd=out, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
    text = result.stdout+result.stderr
    with log.open('a', encoding='utf-8') as stream:
        stream.write(text+'\nEXIT_CODE='+str(result.returncode)+'\n')
    print(text, end='', flush=True)
    return result.returncode
code = 0
objects = []
for name, path in (('capture', out/'UI/CompleteExportCapture.cpp'), ('fixture', out/'fixture.cpp')):
    obj = out/(name+'.obj')
    code = run([str(vc/'bin/Hostx64/x64/cl.exe'), *flags, '/c', str(path), '/Fo'+str(obj)])
    if code:
        break
    objects.append(str(obj))
manifest['tu_returncode'] = code
if not code:
    exe = out/'complete_export_capture_core_tests.exe'
    libs = ['/LIBPATH:'+str(vc/'lib/x64')]+['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64') for part in ('ucrt', 'um')]
    code = run([str(vc/'bin/Hostx64/x64/link.exe'), '/nologo', '/SUBSYSTEM:CONSOLE', '/OUT:'+str(exe), *objects, *libs,
                *(str(qt/'lib'/(name+'.lib')) for name in ('Qt6Widgets', 'Qt6Gui', 'Qt6Core'))])
    manifest['link_returncode'] = code
    if not code:
        env = dict(os.environ)
        env['PATH'] = str(qt/'bin')+os.pathsep+env.get('PATH', '')
        code = run([str(exe)], env)
        manifest['run_returncode'] = code
(out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
raise SystemExit(code)
