"""Exercise production child INI inheritance and root stop propagation."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / "APIMonitor_x64/hook/HookTargets.cpp").read_text(encoding="utf-8-sig")
writer = source[source.index("        std::wstring JoinIniList("):source.index("        // InjectAgentIntoChildProcess")]
main = r'''
int main() {
 using namespace apimon;
 DWORD pid=GetCurrentProcessId();
 auto configPath=ks::winapi_monitor::buildConfigPathForPid(pid);
 auto stopPath=ks::winapi_monitor::buildStopFlagPathForPid(pid);
 auto rootStop=stopPath+L".root-test";
 auto touch=[](const std::wstring& p){HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr); if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);};
 MonitorConfig parent;
 parent.agentDllPath=L"C:\\测试\\Agent.dll";
 parent.sessionId=L"parent-session"; parent.rootStopFlagPath=rootStop;
 parent.enableProcess=false; parent.enableLoader=false; parent.autoInjectChild=true;
 parent.enableClipboard=true; parent.clipboardReadAction=ClipboardPolicyAction::Block;
 parent.clipboardWriteAction=ClipboardPolicyAction::LogOnly;
 parent.rawModuleList={L"ntdll.dll",L"user32.dll"};parent.rawDenyList={L"ntdll!test"};
 CreateDirectoryW(ks::winapi_monitor::buildSessionDirectory().c_str(),nullptr);
 touch(stopPath);
 std::wstring error;
 if(!WriteChildMonitorConfig(pid,parent,&error))return 1;
 MonitorConfig child;
 bool passed=LoadMonitorConfigForCurrentProcess(&child,&error)
 && child.agentDllPath==parent.agentDllPath && child.rootStopFlagPath==rootStop
 && child.sessionId.find(L"parent-session_")==0 && child.autoInjectChild
 && !child.enableProcess && !child.enableLoader && child.enableClipboard
 && child.clipboardReadAction==ClipboardPolicyAction::Block
 && child.clipboardWriteAction==ClipboardPolicyAction::LogOnly
 && child.rawModuleList==parent.rawModuleList && child.rawDenyList==parent.rawDenyList
 && !IsStopFlagPresent(child);
 touch(rootStop);passed=passed && IsStopFlagPresent(child);
 DeleteFileW(configPath.c_str());DeleteFileW(stopPath.c_str());DeleteFileW(rootStop.c_str());
 if(!passed)return 2;
 printf("PASS: child inherits Unicode configuration, policies, session and root stop\n");
}
'''
with tempfile.TemporaryDirectory(prefix="ksword_child_config_") as temp:
 folder=Path(temp); cpp=folder/"fixture.cpp"
 cpp.write_text('#include <cstdio>\n#include "'+(root/"APIMonitor_x64/core/MonitorConfig.cpp").as_posix()+'"\nnamespace apimon {\n'+writer+'\n}\n'+main,encoding="utf-8")
 subprocess.run(["cl","/nologo","/EHsc","/std:c++17","/utf-8","/I"+str(root/"APIMonitor_x64"),str(cpp),"/Fe:"+str(folder/"fixture.exe"),"/Fo:"+str(folder/"fixture.obj"),"/link","Advapi32.lib"],check=True)
 subprocess.run([str(folder/"fixture.exe")],check=True)
