"""QtCore values only: compile actual report writer adapter with a memory backend."""
from pathlib import Path
import argparse, os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=Path,default=ROOT)
    args=parser.parse_args()
    source=(args.source_root/'Ksword5.1/Ksword5.1/MinidumpDock/MinidumpDock.cpp').read_text(encoding='utf-8-sig')
    a=source.index('    template<class Writer>\n    bool writeExportBytes(')
    left=source.index('{',a);end=left+1;depth=1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    function=source[a:end]
    fixture=r"""
#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <algorithm>
#include <iostream>
__PRODUCTION__
struct MemoryWriter {
    QByteArray staged, committed = "old";
    qint64 chunk = 7, failAfter = -1;
    int opens = 0, commits = 0, cancels = 0;
    bool deny = false, failCommit = false, oversized = false;
    bool open(QIODevice::OpenMode) { ++opens;return !deny; }
    qint64 write(const char* data,qint64 count) {
        if (oversized) return count+1;
        if (failAfter >= 0 && staged.size() >= failAfter) return 0;
        const qint64 bytes=std::min(count,chunk);
        staged.append(data,bytes);return bytes;
    }
    bool commit() { ++commits;if(failCommit)return false;committed=staged;return true; }
    void cancelWriting() { ++cancels; }
    QString errorString() const { return QStringLiteral("mock disk failure"); }
};
int main() {
    int checks=0;
    const QByteArray utf8=QString::fromUtf8("报告 / Δοκιμή / 🙂\n").repeated(65536).toUtf8();
    for(const QByteArray& bytes : {QByteArray(),QByteArray("a"),utf8}) {
        for(const qint64 chunk : {1LL,7LL,4096LL}) {
            MemoryWriter writer;writer.chunk=chunk;QString error;
            if(!writeExportBytes(writer,bytes,error)||writer.committed!=bytes||writer.commits!=1||writer.cancels||writer.opens!=1||!error.isEmpty())return 1;
            ++checks;
        }
    }
    for(int mode=0;mode<4;++mode) {
        MemoryWriter writer;QString error;
        writer.deny=mode==0;writer.failAfter=mode==1?9:-1;
        writer.failCommit=mode==2;writer.oversized=mode==3;
        if(writeExportBytes(writer,QByteArray("abcdefghijklmnopqrstuvwxyz"),error))return 2;
        if(writer.committed!="old"||error!=writer.errorString())return 3;
        if((mode==1||mode==3)&&writer.cancels!=1)return 4;
        if(mode!=2&&writer.commits)return 5;
        ++checks;
    }
    std::cout<<"PASS writer_cases="<<checks<<" utf8_bytes="<<utf8.size()<<" positive_short_writes=complete denied/zero/oversize/commit_fail=reported old_destination=preserved widgets=0 business_io=0\n";
}
""".replace('__PRODUCTION__',function)
    vc=Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207')
    if not (vc/'bin/Hostx64/x64/cl.exe').is_file():vc=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
    qt=ROOT/'.deps/Qt/6.9.3/msvc2022_64'
    if not (qt/'lib/Qt6Core.lib').is_file():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
    sdk=Path('C:/Program Files (x86)/Windows Kits/10');version='10.0.26100.0'
    with tempfile.TemporaryDirectory(prefix='ksword_export_writer_') as temporary:
        out=Path(temporary);cpp=out/'fixture.cpp';exe=out/'fixture.exe';cpp.write_text(fixture,encoding='utf-8')
        flags=['/nologo','/std:c++20','/Zc:__cplusplus','/permissive-','/utf-8','/EHsc','/MD','/O2','/W4','/WX','/external:W0','/DNOMINMAX','/DQT_CORE_LIB','/I'+str(vc/'include')]
        for module in ('','QtCore'):flags+=['/external:I'+str(qt/'include'/module)]
        for part in ('ucrt','shared','um'):flags+=['/external:I'+str(sdk/'Include'/version/part)]
        command=[str(vc/'bin/Hostx64/x64/cl.exe'),*flags,str(cpp),'/Fo'+str(out/'fixture.obj'),'/Fe'+str(exe),'/link','/LIBPATH:'+str(vc/'lib/x64')]
        command+=['/LIBPATH:'+str(sdk/'Lib'/version/part/'x64') for part in ('ucrt','um')]+[str(qt/'lib/Qt6Core.lib')]
        subprocess.run(command,cwd=out,check=True)
        environment=dict(os.environ);environment['PATH']=str(qt/'bin')+os.pathsep+environment.get('PATH','')
        subprocess.run([str(exe)],cwd=out,env=environment,check=True)

if __name__=='__main__':main()
