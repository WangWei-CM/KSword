from pathlib import Path
import argparse,subprocess,hashlib,json,os
p=argparse.ArgumentParser();p.add_argument('--source-root',required=True);p.add_argument('--build-dir',required=True);a=p.parse_args()
root=Path(a.source_root).resolve();out=Path(a.build_dir).resolve();out.mkdir(parents=True,exist_ok=True)
ui=root/'Ksword5.1/Ksword5.1/UI';source=(ui/'TableSearchSupport.cpp').read_text(encoding='utf-8')
def method(signature):
    start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[start:end]
getter=method('        ks::ui::TableSearchResultFilterSpec resultFilterSpec() const')
row=method('        bool rowMatchesQuery(const int row) const')
capture=method('    TableSearchResultFilterSpec CaptureTableSearchResultFilterSpec(const QTableView* tableView)')
actual='class TableSearchAccessWidget { public: QPointer<QTableView> m_tableView; QPointer<QAbstractItemModel> m_filterModel; QModelIndex m_filterRoot; QVector<int> m_searchColumns; QString m_resultFilterQuery; bool m_resultFilterActive=false;\n'+getter+'\n'+row+'\n};\nnamespace ks::ui {\n'+capture+'\n}\n'
(out/'actual_search_filter_methods.inc').write_text(actual,encoding='utf-8')
qt=Path(r'D:\Software\Qt\6.9.3\msvc2022_64');test=root/'tools/ui-performance/table_search_filter_snapshot_test.cpp'
args=['cl','/nologo','/std:c++17','/Zc:__cplusplus','/permissive-','/EHsc','/MD','/utf-8','/O2','/I'+str(qt/'include'),'/I'+str(qt/'include/QtCore'),'/I'+str(ui),'/I'+str(out),str(test),'/Fo'+str(out/'test.obj'),'/Fe'+str(out/'test.exe'),'/link','/LIBPATH:'+str(qt/'lib'),'Qt6Core.lib']
cmd='@echo off\ncall D:\\Software\\VS\\VC\\Auxiliary\\Build\\vcvars64.bat >nul\n'+subprocess.list2cmdline(args)+'\nif errorlevel 1 exit /b %errorlevel%\nset "PATH='+str(qt/'bin')+';%PATH%"\n"'+str(out/'test.exe')+'"\nexit /b %errorlevel%\n';(out/'run.cmd').write_text(cmd,encoding='utf-8')
r=subprocess.run([str(out/'run.cmd')],capture_output=True,text=True);print(r.stdout+r.stderr);(out/'test.log').write_text(r.stdout+r.stderr,encoding='utf-8')
sha=lambda data:hashlib.sha256(data).hexdigest()
(out/'provenance.json').write_text(json.dumps({'source_sha256':sha((ui/'TableSearchSupport.cpp').read_bytes()),'actual_methods_sha256':sha(actual.encode()),'value_header_sha256':sha((ui/'TableSearchFilterSnapshot.h').read_bytes()),'test_sha256':sha(test.read_bytes()),'exit_code':r.returncode},indent=2)+'\n')
raise SystemExit(r.returncode)
