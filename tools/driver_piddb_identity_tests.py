"""Qt-free exact identity/force forwarding and caller source-contract checks."""
from pathlib import Path
import argparse,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def block(source,anchor):
    start=source.index(anchor);left=source.index('{',start);depth=1;end=left+1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[start:end]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source-root',type=Path,default=ROOT);args=parser.parse_args()
    area=args.source_root/'Ksword5.1/Ksword5.1/DriverDock';source=(area/'DriverDock.Integrity.cpp').read_text(encoding='utf-8-sig')
    equal=block(source,'bool samePiDdbIdentity(');execute=block(source,'ksword::ark::PiDdbDeleteResult DriverDock::executePiDdbRequest(')
    start=block(source,'void DriverDock::deleteSelectedPiDdbEntry()');confirm=block(source,'void DriverDock::confirmPiDdbDelete(')
    assert 'submit<ksword::ark::PiDdbEntry' in start and 'executePiDdbRequest(identity, false)' in start
    ordered=['KSWORD_ARK_PIDDB_DELETE_STATUS_FORCE_REQUIRED','QMessageBox::warning','QInputDialog::getText','confirmation != QStringLiteral("DELETE PIDDB")','submitTransaction<','executePiDdbRequest(identity, true)']
    indexes=[confirm.index(token) for token in ordered];assert indexes==sorted(indexes)
    assert 'samePiDdbIdentity(current, expected)' in confirm and 'return client.deletePiDdbEntry(expected, commit, commit)' in execute
    assert 'm_piDdbDeletePhase != PiDdbDeletePhase::Idle' in start
    assert source.count('client.deletePiDdbEntry(')==1
    assert confirm.count('guardThis == nullptr')>=3 and 'commitOwner == nullptr' in confirm
    fixture=r"""
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
namespace ksword::ark {
struct PiDdbEntry { std::uint64_t entryAddress=0;std::uint32_t timeDateStamp=0;long loadStatus=0;std::wstring driverName; };
struct PiDdbDeleteResult { bool ok=false;std::uint32_t status=0; };
struct Call { PiDdbEntry identity;bool force;bool confirm; };
static std::vector<Call> calls;
class DriverClient { public: PiDdbDeleteResult deletePiDdbEntry(const PiDdbEntry& identity,bool force,bool confirm)const {
    calls.push_back({identity,force,confirm});return {force,static_cast<std::uint32_t>(calls.size())};
} };
}
class DriverDock { public: static ksword::ark::PiDdbDeleteResult executePiDdbRequest(const ksword::ark::PiDdbEntry&,bool); };
__EQUAL__
__EXECUTE__
int main(){
    using namespace ksword::ark;int checks=0;
    for(std::uint32_t i=0;i<4096;++i){
        PiDdbEntry expected;expected.entryAddress=0xfffff80000000000ULL+i*32ULL;
        expected.timeDateStamp=0xffffffffU-i;expected.loadStatus=i%2?-1073741823L:0;
        expected.driverName=L"测试Driver Ω "+std::to_wstring(i);
        if(!samePiDdbIdentity(expected,expected))return 1;
        auto changed=expected;changed.entryAddress^=1;if(samePiDdbIdentity(expected,changed))return 2;
        changed=expected;changed.timeDateStamp^=1;if(samePiDdbIdentity(expected,changed))return 3;
        changed=expected;changed.loadStatus^=1;if(samePiDdbIdentity(expected,changed))return 4;
        changed=expected;changed.driverName+=L"X";if(samePiDdbIdentity(expected,changed))return 5;
        const auto first=DriverDock::executePiDdbRequest(expected,false);
        const auto second=DriverDock::executePiDdbRequest(expected,true);
        const auto& preflight=calls[calls.size()-2];const auto& commit=calls.back();
        if(!samePiDdbIdentity(preflight.identity,expected)||preflight.force||preflight.confirm||first.ok)return 6;
        if(!samePiDdbIdentity(commit.identity,expected)||!commit.force||!commit.confirm||!second.ok)return 7;
        if(second.status!=calls.size()||first.status+1!=second.status)return 8;
        checks+=8;
    }
    if(calls.size()!=8192)return 9;
    std::cout<<"PASS identity_checks="<<checks<<" backend_calls="<<calls.size()<<" identity_fields=4 preflight=false,false commit=true,true retries=0 GUI=none driver=mock\n";
}
""".replace('__EQUAL__',equal).replace('__EXECUTE__',execute)
    vc=Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207');sdk=Path('C:/Program Files (x86)/Windows Kits/10');version='10.0.26100.0'
    if not (vc/'bin/Hostx64/x64/cl.exe').is_file():vc=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
    with tempfile.TemporaryDirectory(prefix='ksword_piddb_identity_')as temporary:
        out=Path(temporary);cpp=out/'fixture.cpp';exe=out/'fixture.exe';cpp.write_text(fixture,encoding='utf-8')
        flags=['/nologo','/std:c++17','/EHsc','/O2','/W4','/WX','/external:W0','/utf-8','/I'+str(vc/'include')]
        flags+=['/external:I'+str(sdk/'Include'/version/part) for part in ('ucrt','shared','um')]
        command=[str(vc/'bin/Hostx64/x64/cl.exe'),*flags,str(cpp),'/Fo'+str(out/'fixture.obj'),'/Fe'+str(exe),'/link','/LIBPATH:'+str(vc/'lib/x64')]
        command+=['/LIBPATH:'+str(sdk/'Lib'/version/part/'x64') for part in ('ucrt','um')]
        subprocess.run(command,cwd=out,check=True);subprocess.run([str(exe)],cwd=out,check=True)
if __name__=='__main__':main()
