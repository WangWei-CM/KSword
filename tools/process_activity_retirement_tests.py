"""Replay the production pure-data retirement helper without any GUI or business session."""
from pathlib import Path
import argparse
import subprocess


def declaration(source, signature):
    start=source.index(signature)
    brace=source.index("{",start)
    depth=1
    end=brace+1
    while depth:
        depth+=(source[end]=="{")-(source[end]=="}")
        end+=1
    return source[start:end]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--source",type=Path)
    parser.add_argument("--header",type=Path)
    parser.add_argument("--output",type=Path)
    parser.add_argument("--qt",type=Path,default=Path("D:/Software/Qt/6.9.3/msvc2022_64"))
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    source=(args.source or root/"Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp").read_text(encoding="utf-8-sig")
    header=(args.header or root/"Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.h").read_text(encoding="utf-8-sig")
    output=args.output or root/".codex-build-logs/process-activity-retirement-tests"
    output.mkdir(parents=True,exist_ok=True)
    helper=declaration(source,"template<class Container>\n    void retireProcessActivityHistory(")
    point=declaration(header,"    struct ProcessActivityProcessPoint")+";"
    sample=declaration(header,"    struct ProcessActivitySample")+";"
    fixture=Path(__file__).with_suffix(".cpp").read_text(encoding="utf-8").replace("__HELPER__",helper).replace("__TYPES__",point+"\n"+sample)
    cpp=output/"fixture.cpp";cpp.write_text(fixture,encoding="utf-8")
    developers=[Path("D:/Software/VS/Common7/Tools/VsDevCmd.bat"),Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat")]
    developer=next(p for p in developers if p.exists())
    command=["cl.exe","/nologo","/Zc:__cplusplus","/std:c++20","/utf-8","/EHsc","/MD","/O2","/W4","/WX","/I"+str(args.qt/"include"),"/I"+str(args.qt/"include/QtCore"),str(cpp.resolve()),"/Fo"+str((output/"fixture.obj").resolve()),"/Fe"+str((output/"fixture.exe").resolve()),"/link","/LIBPATH:"+str(args.qt/"lib"),"Qt6Core.lib"]
    build=output/"build.cmd"
    build.write_text('@echo off\ncall "'+str(developer)+'" -arch=x64 -host_arch=x64 >nul\n'+subprocess.list2cmdline(command)+'\nif errorlevel 1 exit /b %errorlevel%\nset "PATH='+str(args.qt/"bin")+';%PATH%"\n"'+str((output/"fixture.exe").resolve())+'"\n',encoding="utf-8")
    subprocess.run(["cmd.exe","/d","/c",str(build.resolve())],check=True)
    assert "m_activitySamples.clear()" not in source
    destructor=declaration(source,"ProcessDock::~ProcessDock()")
    assert "retireProcessActivityHistory(m_activitySamples)" in destructor
    assert "retireProcessActivityHistory(m_activitySamples);\n        if (m_activityChartWidget != nullptr) m_activityChartWidget->notifyHistoryReset();" in source
    print("PASS: explicit clear, display reset and owner teardown source wiring")


if __name__=="__main__":
    main()
