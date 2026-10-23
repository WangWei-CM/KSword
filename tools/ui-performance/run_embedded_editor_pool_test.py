from pathlib import Path
import argparse,hashlib,json,os,subprocess
p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True);p.add_argument('--build-dir',type=Path,required=True);a=p.parse_args();build=a.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
source=a.source_root.resolve()/'Ksword5.1/Ksword5.1/UI/DetailLayoutHost.cpp';s=source.read_text(encoding='utf-8')
def method(marker):
    start=s.index(marker);opening=s.index('{',start);level=1;end=opening+1
    while level:
        if s[end]=='{':level+=1
        elif s[end]=='}':level-=1
        end+=1
    return s[start:end]+'\n'
actual=method('int visibleTreeColumn(')
for marker in ['int ks::ui::DetailLayoutHost::embeddedOriginalRowHeight','void ks::ui::DetailLayoutHost::updateEmbeddedEditorGeometries','QList<QPersistentModelIndex> ks::ui::DetailLayoutHost::visibleEmbeddedRows','void ks::ui::DetailLayoutHost::releaseEmbeddedEditor','bool ks::ui::DetailLayoutHost::realizeEmbeddedEntry','void ks::ui::DetailLayoutHost::trimEmbeddedEditorPool','void ks::ui::DetailLayoutHost::flushEmbeddedEditorGeometries']:actual+=method(marker)
for old,new in [('qobject_cast','test_cast'),('QTableView','MockTable'),('QTreeWidget','MockTree'),('QWidget','MockWidget'),('CodeEditorWidget','MockText'),('StructuredFieldView','MockFields')]:actual=actual.replace(old,new)
(build/'embedded_editor_pool_actual.inc').write_text(actual,encoding='utf-8');test=Path(__file__).with_name('embedded_editor_pool_test.cpp');qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
cmd=['cl','/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/EHsc','/MD','/utf-8','/O2',f'/I{build}',f'/I{qt/"include"}',f'/I{qt/"include/QtCore"}',str(test.resolve()),f'/Fo{build}/',f'/Fe{build/"test.exe"}','/link',f'/LIBPATH:{qt/"lib"}','Qt6Core.lib'];batch=build/'compile.cmd';batch.write_text('@echo off\ncall D:\\Software\\VS\\VC\\Auxiliary\\Build\\vcvars64.bat >nul\n'+subprocess.list2cmdline(cmd)+'\nexit /b %errorlevel%\n',encoding='utf-8')
r=subprocess.run(['cmd','/d','/c',str(batch)],capture_output=True,text=True);log=r.stdout+r.stderr
if not r.returncode:
    env=os.environ.copy();env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','');r=subprocess.run([str(build/'test.exe')],capture_output=True,text=True,env=env);log+=r.stdout+r.stderr
(build/'test.log').write_text(log,encoding='utf-8');print(log);(build/'provenance.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'actual_methods_sha256':hashlib.sha256(actual.encode()).hexdigest(),'test_sha256':hashlib.sha256(test.read_bytes()).hexdigest(),'exit_code':r.returncode},indent=2)+'\n',encoding='utf-8');raise SystemExit(r.returncode)
