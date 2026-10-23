"""Exercise the production pending-apply batch with a memory-only transaction facade."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--header',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);source=a.source.read_text(encoding='utf-8-sig');header=a.header.read_text(encoding='utf-8-sig')
def structure(name):
 start=header.index('    struct '+name+'\n');end=header.index('    };',start)+6;return header[start:end]
start=source.index('RegistryDock::PendingApplyResult RegistryDock::applyPendingValueBatch(');production=source[start:source.index('void RegistryDock::applyPendingChanges()',start)]
prefix=r"""
#include <QString>
#include <QByteArray>
#include <QVector>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using DWORD=std::uint32_t;
constexpr DWORD REG_NONE=0;
int checks=0;void check(bool value){++checks;if(!value){std::fprintf(stderr,"FAIL %d\n",checks);std::abort();}}
struct RegistryAccessContext{int viewBits=0;bool useR0=false;};
struct RegistryApplyValueState{bool exists=false;quint32 type=0;QByteArray data;};
struct RegistryApplyResult{QString error;};
struct Call{QString path,name;RegistryApplyValueState before,after;RegistryAccessContext context;};
QVector<Call> calls;int failureIndex=-1;bool emptyError=false,closeAfterFirst=false;std::shared_ptr<std::atomic_bool> closedFlag;
class RegistryValueTransactions{public:static bool apply(const QString&path,const QString&name,const RegistryApplyValueState&before,const RegistryApplyValueState&after,const RegistryAccessContext&context,RegistryApplyResult&result,const std::atomic_bool*cancel){check(cancel==closedFlag.get());const int index=static_cast<int>(calls.size());calls.push_back({path,name,before,after,context});if(closeAfterFirst&&index==0)closedFlag->store(true);if(index==failureIndex){if(!emptyError)result.error=QStringLiteral("unverified partial");return false;}return true;}};
class RegistryDock{public:
"""
prefix+=structure('PendingValueChange')+'\n'+structure('PendingApplyResult')+r"""
static PendingApplyResult applyPendingValueBatch(const QVector<PendingValueChange>&,const std::shared_ptr<std::atomic_bool>&);
};
"""
tests=r"""
void reset(){calls.clear();failureIndex=-1;emptyError=closeAfterFirst=false;closedFlag=std::make_shared<std::atomic_bool>(false);}
RegistryDock::PendingValueChange change(int index){RegistryDock::PendingValueChange c;c.keyPath=QStringLiteral("HKCU\\测试 %1").arg(index);c.name=index==0?QString():QStringLiteral(" %1 ").arg(index);c.beforeExists=index%2==0;c.beforeType=3;c.beforeData=QByteArray::fromHex("0080ff00");c.afterType=7;c.afterData=QByteArray::fromHex("410000008000");c.deleteValue=index%3==1;c.viewBits=index%3==0?0:index%3==1?32:64;c.useR0=index%4==0;return c;}
int main(){reset();QVector<RegistryDock::PendingValueChange> input{change(0),change(1),change(2)};failureIndex=1;auto result=RegistryDock::applyPendingValueBatch(input,closedFlag);check(calls.size()==3&&result.applied.size()==2&&result.recoverable.size()==2&&result.remaining.size()==1);check(result.remaining[0].result==QStringLiteral("unverified partial"));
 for(qsizetype i=0;i<input.size();++i){check(calls[i].path==input[i].keyPath&&calls[i].name==input[i].name);check(calls[i].before.exists==input[i].beforeExists&&calls[i].before.type==input[i].beforeType&&calls[i].before.data==input[i].beforeData);check(calls[i].after.exists==!input[i].deleteValue&&calls[i].after.type==input[i].afterType&&calls[i].after.data==input[i].afterData);check(calls[i].context.viewBits==input[i].viewBits&&calls[i].context.useR0==input[i].useR0);check(input[i].result.isEmpty());}
 reset();closedFlag->store(true);result=RegistryDock::applyPendingValueBatch(input,closedFlag);check(calls.isEmpty()&&result.applied.isEmpty()&&result.remaining.size()==3);for(const auto&value:result.remaining)check(value.result.isEmpty());
 reset();closeAfterFirst=true;result=RegistryDock::applyPendingValueBatch(input,closedFlag);check(calls.size()==1&&result.applied.size()==1&&result.recoverable.size()==1&&result.remaining.size()==2);check(result.remaining[0].keyPath==input[1].keyPath&&result.remaining[0].result.isEmpty());
 reset();failureIndex=0;emptyError=true;result=RegistryDock::applyPendingValueBatch(input,closedFlag);check(result.remaining.size()==1&&result.remaining[0].result==QStringLiteral("回读不一致，请重新读取后核对"));check(calls.size()==3&&result.recoverable.size()==2);
 reset();QVector<RegistryDock::PendingValueChange> full;for(int i=0;i<1024;++i)full.push_back(change(i));result=RegistryDock::applyPendingValueBatch(full,closedFlag);check(calls.size()==1024&&result.applied.size()==1024&&result.recoverable.size()==1024&&result.remaining.isEmpty());for(qsizetype i=0;i<full.size();++i){check(calls[i].path==full[i].keyPath&&calls[i].before.data==full[i].beforeData&&calls[i].after.data==full[i].afterData);check(result.applied[i].keyPath==full[i].keyPath&&result.applied[i].result==QStringLiteral("已应用并回读验证"));}
 std::printf("registry_pending_apply_worker: %d checks PASS;1024ordered immutable drafts;memory backend only\n",checks);return 0;}
"""
fixture=a.out/'fixture.cpp';fixture.write_text(prefix+production+tests,encoding='utf-8');qt=Path('D:/Software/Qt/6.9.3/msvc2022_64');cmd=a.out/'test.cmd';cmd.write_text('\n'.join(['@echo off','call "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%',f'cl.exe /nologo /std:c++20 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /external:W0 /external:I"{qt / "include"}" /external:I"{qt / "include/QtCore"}" "{fixture.resolve()}" /Fo"{(a.out / "test.obj").resolve()}" /Fe"{(a.out / "test.exe").resolve()}" /link /LIBPATH:"{qt / "lib"}" Qt6Core.lib','if errorlevel 1 exit /b %errorlevel%',f'set "PATH={qt / "bin"};%PATH%"',f'"{(a.out / "test.exe").resolve()}"'])+'\n',encoding='utf-8');r=subprocess.run(['cmd.exe','/d','/c',str(cmd.resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace');(a.out/'test.log').write_text(r.stdout,encoding='utf-8');print(r.stdout);raise SystemExit(r.returncode)
