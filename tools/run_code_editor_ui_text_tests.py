"""Compile real editor TUs and run its extracted pure status formatter.

No QApplication, QWidget, product, native dialog or business operation is run.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-dir', type=Path)
    parser.add_argument('--language-source-dir', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    repository = next(p for p in Path(__file__).resolve().parents if (p/'AGENTS.md').is_file())
    product = repository/'Ksword5.1/Ksword5.1'
    editor = (args.source_dir or product/'UI').resolve()
    language = (args.language_source_dir or product/'Internationalization').resolve()
    output = args.output_dir.resolve()
    if not output.is_relative_to(repository/'.codex-build-logs'): raise ValueError('Output must remain under .codex-build-logs')
    output.mkdir(parents=True, exist_ok=True)
    overlay = output/'overlay/Ksword5.1/Ksword5.1'
    (overlay/'UI').mkdir(parents=True, exist_ok=True)
    (overlay/'Internationalization').mkdir(exist_ok=True)
    # Local headers use relative includes and pragma-once identities. Mirror
    # headers so all transitive references see one candidate LanguageManager,
    # while product sources and their working tree remain untouched.
    for header in product.rglob('*'):
        if header.is_file() and header.suffix.lower() in ('.h','.hpp','.inl'):
            target=overlay/header.relative_to(product)
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(header,target)
    for header in (repository/'shared').rglob('*'):
        if header.is_file() and header.suffix.lower() in ('.h','.hpp','.inl'):
            target=output/'overlay'/header.relative_to(repository)
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(header,target)
    for name in ('CodeEditorWidget.cpp','CodeEditorWidget.h','CodeEditorWidget.Export.cpp'):
        shutil.copy2(editor/name, overlay/'UI'/name)
    shutil.copy2(language/'LanguageManager.h',overlay/'Internationalization/LanguageManager.h')
    source = (editor/'CodeEditorWidget.cpp').read_text(encoding='utf-8')
    begin = source.index('void CodeEditorWidget::refreshOperationStatus()')
    end = source.index('\nvoid CodeEditorWidget::initializeConnections()',begin)
    (output/'editor_status_body.inc').write_text(source[begin:end].replace('CodeEditorWidget::','StatusHarness::'),encoding='utf-8')
    qt = next(p for p in (repository/'.deps/Qt/6.9.3/msvc2022_64',Path(r'D:\Software\Qt\6.9.3\msvc2022_64')) if (p/'lib/Qt6Core.lib').is_file())
    vc = next(p for p in (Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'),Path(r'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat')) if p.is_file())
    flags = ['/nologo','/O2','/EHsc','/std:c++latest','/Zc:__cplusplus','/permissive-','/MD','/utf-8','/DNDEBUG','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DUNICODE','/D_UNICODE','/DQT_CORE_LIB','/DQT_GUI_LIB','/DQT_WIDGETS_LIB']
    for path in (overlay/'UI',overlay,product/'UI',product,repository/'third_party/zstd',repository/'third_party/zydis',language,output): flags.append('/I'+str(path))
    for module in ('','QtCore','QtGui','QtWidgets','QtSvg','QtNetwork'): flags.append('/I'+str(qt/'include'/module))
    commands = []
    for name in ('CodeEditorWidget.cpp','CodeEditorWidget.Export.cpp'):
        commands.append(['cl',*flags,'/W3','/c',str(overlay/'UI'/name),'/Fo'+str(output/(name+'.obj'))])
    exe=output/'editor_ui_text_tests.exe'
    commands.append(['cl',*flags,'/W4','/WX',str(Path(__file__).with_name('code_editor_ui_text_tests.cpp')),str(language/'LanguageManager.cpp'),'/Fo'+str(output)+'\\','/Fe'+str(exe),'/link','/LIBPATH:'+str(qt/'lib'),'Qt6Widgets.lib','Qt6Gui.lib','Qt6Core.lib','user32.lib'])
    batch=['@echo off','call "'+str(vc)+'" >nul','if errorlevel 1 exit /b %errorlevel%']
    for index,command in enumerate(commands):
        response=output/f'compile-{index}.rsp'
        response.write_text(subprocess.list2cmdline(command[1:]),encoding='utf-8')
        batch += ['cl @"'+str(response)+'"','if errorlevel 1 exit /b %errorlevel%']
    batch.append('exit /b 0')
    (output/'build.cmd').write_text('\n'.join(batch),encoding='utf-8')
    build=subprocess.run(['cmd','/d','/c',str(output/'build.cmd')],cwd=output,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (output/'build.log').write_bytes(build.stdout)
    print(build.stdout.decode('utf-8',errors='replace'))
    result={'editor_sha256':hashlib.sha256((editor/'CodeEditorWidget.cpp').read_bytes()).hexdigest(),'language_sha256':hashlib.sha256((language/'LanguageManager.cpp').read_bytes()).hexdigest(),'build_returncode':build.returncode,'runtime_scope':'QCoreApplication only; pure extracted formatter; no GUI or business execution'}
    code=build.returncode
    if not code:
        env=os.environ.copy(); env['PATH']=str(qt/'bin')+os.pathsep+env['PATH']
        run=subprocess.run([str(exe)],cwd=output,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (output/'run.log').write_bytes(run.stdout)
        print(run.stdout.decode('utf-8',errors='replace'))
        result['run_returncode']=run.returncode; code=run.returncode
    (output/'manifest.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return code


if __name__=='__main__': raise SystemExit(main())
