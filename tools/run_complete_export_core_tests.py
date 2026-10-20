"""Compile real complete export/async/value translation TUs, run QtCore + fake IO.

The native QSaveFile production TU is compiled separately. A test-only include
replacement then runs the identical complete export TU with in-memory fake
QSaveFile; no GUI, clipboard, business process or real output file is touched.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository-root', type=Path)
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--i18n-source-dir', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    root = args.repository_root
    if not root:
        root = next(path for path in Path(__file__).resolve().parents if (path / 'AGENTS.md').is_file())
    root = root.resolve()
    source = (args.source_root or root).resolve()
    output = args.output_dir.resolve()
    if not output.is_relative_to(root / '.codex-build-logs'):
        raise ValueError('Only ignored .codex-build-logs artifacts may be created')
    output.mkdir(parents=True, exist_ok=True)
    ui = output / 'UI'
    i18n = output / 'Internationalization'
    fake = output / 'fake'
    for path in (ui, i18n, fake):
        path.mkdir(exist_ok=True)
    manifest = {'scope': 'QCoreApplication + fake devices, no QWidget/clipboard/business/real file writes', 'sources': {}}
    for name in ('CompleteExport.h', 'CompleteExport.cpp', 'AsyncOperation.h', 'AsyncOperation.cpp',
                 'AsyncUiDispatcher.h', 'AsyncCleanupRegistry.h'):
        relative = Path('Ksword5.1/Ksword5.1/UI') / name
        candidate = source / relative
        original = candidate if candidate.is_file() else root / relative
        data = original.read_bytes()
        (ui / name).write_bytes(data)
        manifest['sources'][name] = hashlib.sha256(data).hexdigest()
    language = (args.i18n_source_dir or root / 'Ksword5.1/Ksword5.1/Internationalization').resolve()
    for name in ('LanguageManager.h', 'LanguageManager.cpp'):
        data = (language / name).read_bytes()
        (i18n / name).write_bytes(data)
        manifest['sources'][name] = hashlib.sha256(data).hexdigest()
    fixture = Path(__file__).with_name('complete_export_core_tests.cpp')
    fake_source = Path(__file__).with_name('complete_export_fake_save_file.h')
    (output / 'fixture.cpp').write_bytes(fixture.read_bytes())
    (fake / 'QSaveFile').write_bytes(fake_source.read_bytes())
    manifest['sources']['fixture.cpp'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    manifest['sources']['fake/QSaveFile'] = hashlib.sha256(fake_source.read_bytes()).hexdigest()
    vc = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
    if not (vc / 'bin/Hostx64/x64/cl.exe').is_file():
        vc = Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207')
    qt = root / '.deps/Qt/6.9.3/msvc2022_64'
    if not (qt / 'lib/Qt6Core.lib').is_file():
        qt = Path('D:/Software/Qt/6.9.3/msvc2022_64')
    sdk = Path('C:/Program Files (x86)/Windows Kits/10')
    flags = ['/nologo', '/std:c++20', '/Zc:__cplusplus', '/permissive-', '/utf-8', '/EHsc', '/MD', '/O2',
             '/W4', '/WX', '/external:W0', '/DNOMINMAX', '/DUNICODE', '/D_UNICODE',
             '/DQT_CORE_LIB', '/DQT_GUI_LIB', '/DQT_WIDGETS_LIB', '/I'+str(output), '/I'+str(vc/'include')]
    for module in ('', 'QtCore', 'QtGui', 'QtWidgets'):
        flags += ['/external:I'+str(qt/'include'/module)]
    for part in ('ucrt', 'shared', 'um'):
        flags += ['/external:I'+str(sdk/'Include/10.0.26100.0'/part)]
    log = output / 'result.log'
    log.write_text('HOST_TOOLS=Hostx64/x64\n', encoding='utf-8')

    def run(command: list[str], environment=None) -> int:
        result = subprocess.run(command, cwd=output, env=environment, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=60)
        text = result.stdout + result.stderr
        with log.open('a', encoding='utf-8') as stream:
            stream.write(text + '\nEXIT_CODE=' + str(result.returncode) + '\n')
        print(text, end='', flush=True)
        return result.returncode

    compiler = str(vc/'bin/Hostx64/x64/cl.exe')
    native = run([compiler, *flags, '/c', str(ui/'CompleteExport.cpp'), '/Fo'+str(output/'CompleteExport.native.obj')])
    manifest['native_tu_returncode'] = native
    if native:
        (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
        return native
    objects = []
    for name, path, extra in (('fixture', output/'fixture.cpp', []),
                              ('CompleteExport.fake', ui/'CompleteExport.cpp', ['/I'+str(fake)]),
                              ('AsyncOperation', ui/'AsyncOperation.cpp', []),
                              ('LanguageManager', i18n/'LanguageManager.cpp', [])):
        obj = output/(name+'.obj')
        code = run([compiler, *extra, *flags, '/c', str(path), '/Fo'+str(obj)])
        if code:
            manifest['build_returncode'] = code
            (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
            return code
        objects.append(str(obj))
    exe = output/'complete_export_core_tests.exe'
    libraries = ['/LIBPATH:'+str(vc/'lib/x64')]
    libraries += ['/LIBPATH:'+str(sdk/'Lib/10.0.26100.0'/part/'x64') for part in ('ucrt', 'um')]
    code = run([str(vc/'bin/Hostx64/x64/link.exe'), '/nologo', '/SUBSYSTEM:CONSOLE', '/OUT:'+str(exe),
                *objects, *libraries, *(str(qt/'lib'/(name+'.lib')) for name in ('Qt6Widgets', 'Qt6Gui', 'Qt6Core')), 'user32.lib'])
    manifest['build_returncode'] = code
    if not code:
        environment = dict(os.environ)
        environment['PATH'] = str(qt/'bin')+os.pathsep+environment.get('PATH', '')
        code = run([str(exe)], environment)
        manifest['run_returncode'] = code
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return code


if __name__ == '__main__':
    raise SystemExit(main())
