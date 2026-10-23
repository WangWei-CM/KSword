"""Extract production value workers and replace all registry IO with in-memory receipts."""
import argparse, re, subprocess, os
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,required=True)
parser.add_argument('--out',type=Path,required=True)
a=parser.parse_args(); source=a.source.read_text(encoding='utf-8-sig');a.out.mkdir(parents=True,exist_ok=True)
def take(first,last):
    start=source.index(first);return source[start:source.index(last,start)]
production='\n'.join([
    'LPCWSTR wide(const QString& text) { return reinterpret_cast<LPCWSTR>(text.utf16()); }',
    take('    struct Key\n','    void setStatus('),
    'LSTATUS linkTraversalStatus(HKEY key);',
    take('    // Permission workers retain only','    class PermissionsDialog final'),
    take('    LSTATUS linkTraversalStatus(HKEY key)\n    {','    // Each path component')])
prefix=r"""
#include <Windows.h>
#include <Sddl.h>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QMap>
#include <QVector>
#include <utility>
#include <cstdio>
#include <cstdlib>
static int checks=0;
void check(bool value) { ++checks; if (!value) { std::fprintf(stderr,"check failed %d\n",checks); std::abort(); } }
struct OpenRecord {QString path; REGSAM access; DWORD options;};
QMap<quintptr,QString> handles;
QVector<OpenRecord> opened;
quintptr nextHandle=1024;
QByteArray current;
int attempts=0,successful=0;
LSTATUS openStatus=ERROR_SUCCESS,writeProbe=ERROR_SUCCESS,writeStatus=ERROR_SUCCESS;
bool failReadBack=false,mismatch=false,link=false;
SECURITY_INFORMATION writtenParts=0;
QByteArray descriptor(const QString& text) {
 PSECURITY_DESCRIPTOR raw=nullptr;
 check(ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(text.utf16()),SDDL_REVISION_1,&raw,nullptr)!=FALSE);
 QByteArray result(reinterpret_cast<const char*>(raw),GetSecurityDescriptorLength(raw));LocalFree(raw);return result;
}
QString daclText(PSECURITY_DESCRIPTOR descriptor) {
 LPWSTR raw=nullptr;check(ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor,SDDL_REVISION_1,DACL_SECURITY_INFORMATION,&raw,nullptr)!=FALSE);
 QString result=QString::fromWCharArray(raw);LocalFree(raw);return result;
}
LSTATUS fakeOpen(HKEY parent,LPCWSTR name,DWORD options,REGSAM access,PHKEY key) {
 if(openStatus!=ERROR_SUCCESS)return openStatus;
 if((access&WRITE_DAC)&&writeProbe!=ERROR_SUCCESS)return writeProbe;
 QString path=handles.value(reinterpret_cast<quintptr>(parent),QStringLiteral("ROOT"));
 if(*name)path+=QStringLiteral("\\")+QString::fromWCharArray(name);
 opened.push_back({path,access,options});*key=reinterpret_cast<HKEY>(nextHandle++);handles.insert(reinterpret_cast<quintptr>(*key),path);return ERROR_SUCCESS;
}
LSTATUS fakeClose(HKEY key) {check(handles.remove(reinterpret_cast<quintptr>(key))==1);return ERROR_SUCCESS;}
LSTATUS fakeQuery(HKEY,LPCWSTR,LPDWORD,LPDWORD type,LPBYTE,LPDWORD size) {if(!link)return ERROR_FILE_NOT_FOUND;*type=REG_LINK;*size=2;return ERROR_SUCCESS;}
LSTATUS fakeGet(HKEY key,SECURITY_INFORMATION parts,PSECURITY_DESCRIPTOR output,LPDWORD size) {
 check(handles.contains(reinterpret_cast<quintptr>(key)));check(parts==(OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION));
 if(failReadBack&&successful)return ERROR_ACCESS_DENIED;
 if(!output||*size<static_cast<DWORD>(current.size())){*size=static_cast<DWORD>(current.size());return ERROR_INSUFFICIENT_BUFFER;}
 memcpy(output,current.data(),current.size());*size=static_cast<DWORD>(current.size());return ERROR_SUCCESS;
}
LSTATUS fakeSet(HKEY key,SECURITY_INFORMATION parts,PSECURITY_DESCRIPTOR input) {
 check(handles.contains(reinterpret_cast<quintptr>(key)));++attempts;writtenParts=parts;
 check((parts&DACL_SECURITY_INFORMATION)!=0);check((parts&(OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|SACL_SECURITY_INFORMATION))==0);
 if(writeStatus!=ERROR_SUCCESS)return writeStatus;
 ++successful;current=descriptor(QStringLiteral("O:SYG:SY")+(mismatch?QStringLiteral("D:(A;;KR;;;SY)"):daclText(input)));return ERROR_SUCCESS;
}
#define RegOpenKeyExW fakeOpen
#define RegCloseKey fakeClose
#define RegQueryValueExW fakeQuery
#define RegGetKeySecurity fakeGet
#define RegSetKeySecurity fakeSet
"""
tests=r"""
void reset() {
 check(handles.isEmpty());opened.clear();attempts=successful=0;openStatus=writeProbe=writeStatus=ERROR_SUCCESS;
 failReadBack=mismatch=link=false;writtenParts=0;current=descriptor(QStringLiteral("O:SYG:SYD:(A;;KR;;;SY)"));
}
int main() {
 const PermissionTarget target{QStringLiteral("HKCU\\测试 保留空格\\Leaf"),KEY_WOW64_32KEY};
 reset();auto initial=readPermissionSnapshot(target);check(initial.status==ERROR_SUCCESS);check(initial.writeStatus==ERROR_SUCCESS);check(!initial.owner.isEmpty()&&!initial.group.isEmpty()&&!initial.dacl.isEmpty());check(initial.baseline==current);check(handles.isEmpty());
 for(const auto& call:opened){check(call.options==REG_OPTION_OPEN_LINK);check((call.access&KEY_WOW64_32KEY)!=0);}
 check(opened.last().path.endsWith(QStringLiteral("测试 保留空格\\Leaf")));
 reset();writeProbe=ERROR_ACCESS_DENIED;auto denied=readPermissionSnapshot(target);check(denied.status==ERROR_SUCCESS);check(denied.writeStatus==ERROR_ACCESS_DENIED);check(handles.isEmpty());
 reset();openStatus=ERROR_ACCESS_DENIED;check(readPermissionSnapshot(target).status==ERROR_ACCESS_DENIED);check(opened.isEmpty());
 reset();link=true;check(readPermissionSnapshot(target).status==ERROR_ACCESS_DENIED);check(handles.isEmpty());
 reset();check(readPermissionSnapshot({QStringLiteral("HKCU\\\\Leaf"),KEY_WOW64_64KEY}).status==ERROR_INVALID_PARAMETER);check(handles.isEmpty());
 reset();check(readPermissionSnapshot({QStringLiteral("HKCU")+QChar(0)+QStringLiteral("x"),0}).status==ERROR_INVALID_PARAMETER);check(opened.isEmpty());
 using Kind=PermissionApplyResult::Kind;
 auto request=[&] {return PermissionApplyRequest{target,current,daclText(reinterpret_cast<PSECURITY_DESCRIPTOR>(current.data())),QStringLiteral("D:(A;;KA;;;SY)")};};
 reset();auto good=request();auto result=applyPermissionRequest(good);check(result.kind==Kind::Applied&&result.submitted);check(attempts==1&&successful==1);check(result.actual.dacl==result.requested);check(result.actual.baseline==current);check((writtenParts&UNPROTECTED_DACL_SECURITY_INFORMATION)!=0);check(handles.isEmpty());
 reset();auto protectedRequest=request();protectedRequest.input=QStringLiteral("D:P(A;;KA;;;SY)");auto protectedResult=applyPermissionRequest(protectedRequest);check(protectedResult.kind==Kind::Applied);check((writtenParts&PROTECTED_DACL_SECURITY_INFORMATION)!=0);check(handles.isEmpty());
 reset();auto unchanged=request();unchanged.input=unchanged.original;check(applyPermissionRequest(unchanged).kind==Kind::Unchanged);check(attempts==0&&opened.isEmpty());
 for(const auto& invalid:QStringList{QStringLiteral("O:SYD:(A;;KA;;;SY)"),QStringLiteral("G:SYD:(A;;KA;;;SY)"),QStringLiteral("D:(A;;KA;;;SY)S:(AU;SA;KA;;;SY)")}) {
  reset();auto rejected=request();rejected.input=invalid;check(applyPermissionRequest(rejected).kind==Kind::DaclOnly);check(attempts==0&&opened.isEmpty());
 }
 reset();auto nul=request();nul.input+=QChar(0);nul.input+=QStringLiteral("D:");check(applyPermissionRequest(nul).kind==Kind::Nul);check(attempts==0&&opened.isEmpty());
 reset();auto malformed=request();malformed.input=QStringLiteral("not-sddl");check(applyPermissionRequest(malformed).kind==Kind::InvalidSddl);check(attempts==0&&opened.isEmpty());
 reset();auto stale=request();current=descriptor(QStringLiteral("O:SYG:SYD:(A;;KW;;;SY)"));check(applyPermissionRequest(stale).kind==Kind::Conflict);check(attempts==0&&handles.isEmpty());
 reset();auto writeDenied=request();writeStatus=ERROR_ACCESS_DENIED;auto failed=applyPermissionRequest(writeDenied);check(failed.kind==Kind::Access&&!failed.submitted);check(attempts==1&&successful==0&&handles.isEmpty());
 reset();auto uncertain=request();failReadBack=true;auto unverified=applyPermissionRequest(uncertain);check(unverified.kind==Kind::ReadBack&&unverified.submitted);check(successful==1&&handles.isEmpty());
 reset();auto mismatchRequest=request();mismatch=true;auto different=applyPermissionRequest(mismatchRequest);check(different.kind==Kind::Applied&&different.submitted);check(different.actual.dacl!=different.requested);check(handles.isEmpty());
 reset();auto mutableRequest=request();const auto frozen=mutableRequest;mutableRequest.target.path=QStringLiteral("HKLM\\Other");mutableRequest.target.view=KEY_WOW64_64KEY;mutableRequest.baseline.clear();mutableRequest.input=QStringLiteral("D:");
 auto frozenResult=applyPermissionRequest(frozen);check(frozenResult.kind==Kind::Applied);check(opened.last().path.endsWith(QStringLiteral("测试 保留空格\\Leaf")));check((opened.last().access&KEY_WOW64_32KEY)!=0);check(handles.isEmpty());
 std::printf("registry_permission_workers: %d checks PASS; all registry calls fake; native SDDL memory-only\n",checks);return 0;
}
"""
prefix=re.sub(r'\bcurrent\b','fakeCurrent',prefix)
tests=re.sub(r'\bcurrent\b','fakeCurrent',tests)
fixture=a.out/'permission_workers.cpp';fixture.write_text(prefix+production+tests,encoding='utf-8')
qt=Path('D:/Software/Qt/6.9.3/msvc2022_64');exe=a.out/'permission_workers.exe'
cmd=a.out/'test.cmd'
lines=['@echo off','call "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%',
f'cl.exe /nologo /std:c++20 /Zc:__cplusplus /EHsc /utf-8 /W4 /WX /MD /wd4100 /DUNICODE /D_UNICODE /I"{qt / "include"}" /I"{qt / "include/QtCore"}" "{fixture.resolve()}" /Fo"{(a.out / "test.obj").resolve()}" /Fe"{exe.resolve()}" /link /LIBPATH:"{qt / "lib"}" Qt6Core.lib Advapi32.lib',
'if errorlevel 1 exit /b %errorlevel%',f'set "PATH={qt / "bin"};%PATH%"',f'"{exe.resolve()}"']
cmd.write_text('\n'.join(lines)+'\n',encoding='utf-8')
result=subprocess.run(['cmd.exe','/d','/c',str(cmd.resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace')
(a.out/'test.log').write_text(result.stdout,encoding='utf-8');print(result.stdout);raise SystemExit(result.returncode)
