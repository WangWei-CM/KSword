"""Follow an owned child of the other architecture with the production Agent."""
import configparser
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid
from compiler_environment import target_environment
from live_fixture_support import LiveFixture, ROOT, ARCHITECTURE, kernel, INVALID, session_identity
from injection_host_support import InjectionHost

child_source=r'''
#include <Windows.h>
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 1;auto event=OpenEventW(SYNCHRONIZE,FALSE,argv[1]);if(!event)return 2;
 WaitForSingleObject(event,90000);CloseHandle(event);return 0;
}
'''
parent_source=r'''
#include "ApiMonitorInjection.h"
#include <cstdio>
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 1;printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 auto quit=CreateEventW(nullptr,TRUE,FALSE,L"EVENT_NAME");if(!quit)return 2;
 PROCESS_INFORMATION child{};
 for(int c;(c=getchar())!=EOF;){
  if(c=='L'&&!LoadLibraryW(argv[1]))return 3;
  if(c=='C'){
   auto command=ks::winapi_monitor::quoteWindowsArgument(LR"(CHILD_PATH)")
    +L" "+ks::winapi_monitor::quoteWindowsArgument(L"EVENT_NAME");
   STARTUPINFOW startup{};startup.cb=sizeof(startup);
   if(!CreateProcessW(LR"(CHILD_PATH)",command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return 4;
   printf("CHILD %lu\n",child.dwProcessId);fflush(stdout);
  }
  if(c=='Q'){SetEvent(quit);if(child.hProcess)WaitForSingleObject(child.hProcess,5000);break;}
 }
 if(child.hThread)CloseHandle(child.hThread);if(child.hProcess)CloseHandle(child.hProcess);CloseHandle(quit);return 0;
}
'''
other="x64" if ARCHITECTURE=="x86" else "x86"
with InjectionHost() as host, tempfile.TemporaryDirectory(prefix="ksword_child_arch_") as temporary:
    folder=Path(temporary);cpp=folder/"child.cpp";cpp.write_text(child_source,encoding="utf-8")
    environment=target_environment(other);compiler=shutil.which("cl",path=environment["PATH"])
    child=folder/"child.exe"
    subprocess.run([compiler,"/nologo","/EHsc","/std:c++17","/MT",str(cpp),"/Fe:"+str(child),
                    "/Fo:"+str(folder/"child.obj")],env=environment,check=True)
    event="Local\\KSwordChildTest_"+uuid.uuid4().hex
    source=parent_source.replace("EVENT_NAME",event.replace("\\","\\\\")).replace("CHILD_PATH",str(child))
    source=source.replace('#include "ApiMonitorInjection.h"','#include "'+(ROOT/"shared/ApiMonitorInjection.h").as_posix()+'"')
    observer=None;config_path=stop_path=None
    try:
        with LiveFixture(source,{"enable_process":1,"auto_inject_child":1},injector=host.inject) as parent:
            parent.wait_for(lambda f:any(e.api=="HooksInstalled" for e in f.events))
            parent.command("C")
            parent.wait_for(lambda f:any(e.api in ("AutoInjectChild","AutoInjectChildFailed") for e in f.events))
            result=next(e for e in parent.events if e.api in ("AutoInjectChild","AutoInjectChildFailed"))
            assert result.api=="AutoInjectChild",result.detail
            line=parent.process.stdout.readline().decode().strip();assert line.startswith("CHILD "),line
            pid=int(line.split()[1]);session_directory=Path(os.environ.get("WINDIR", "C:/Windows"))/"Temp"/"KswordApiMon"
            config_path=session_directory/f"config_{pid}.ini";stop_path=session_directory/f"stop_{pid}.flag"
            config=configparser.ConfigParser(interpolation=None);config.read(config_path,encoding="utf-16")
            values=config["monitor"]
            assert Path(values["agent_dll_path"]).name==f"APIMonitor_{other}.dll"
            assert Path(values["root_stop_flag_path"])==parent.stop
            assert values["session_id"].startswith(parent.session_text+"_")
            parent_config=configparser.ConfigParser(interpolation=None);parent_config.read(parent.config,encoding="utf-16")
            for key in ("injection_broker_pipe","injection_broker_token","injection_broker_pid","injection_broker_creation"):
                assert values[key]==parent_config["monitor"][key]
            observer=object.__new__(LiveFixture)
            observer.pid=pid;observer.session=session_identity(values["session_id"])
            observer.process=parent.process;observer.handle=None;observer.bytes=bytearray()
            observer.events=[];observer.snapshots=[];observer.pending_snapshot=None
            observer.connect()
            observer.wait_for(lambda f:any(e.api=="HooksInstalled" for e in f.events) and bool(f.snapshots))
            parent.stop.write_text("stop")
            parent.wait_for(lambda f:any(e.api=="HooksRemoved" for e in f.events),timeout=30)
            observer.wait_for(lambda f:any(e.api=="HooksRemoved" for e in f.events),timeout=30)
    finally:
        if observer and observer.handle not in (None,INVALID):kernel.CloseHandle(observer.handle)
        if config_path:config_path.unlink(missing_ok=True)
        if stop_path:stop_path.unlink(missing_ok=True)
print(f"PASS: {ARCHITECTURE} parent follows {other} child; matching DLL, inherited session, complete coverage and root stop")
