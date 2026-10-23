"""Replay actual canonical-retention interfaces with QtCore, without widgets or system actions."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import time


def declaration(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--qt", type=Path, default=Path("D:/Software/Qt/6.9.3/msvc2022_64"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_path = args.source or root / "Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp"
    header_path = args.header or root / "Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.h"
    history_header = header_path.with_name("ProcessActivityHistory.h")
    source = source_path.read_text(encoding="utf-8-sig")
    header = header_path.read_text(encoding="utf-8-sig")
    output = args.output or root / ".codex-build-logs/process-activity-retention-tests"
    output.mkdir(parents=True, exist_ok=True)
    signatures = [
        "void ProcessDock::scheduleProcessActivityHistoryRetirement()",
        "void ProcessDock::pumpProcessActivityHistoryRetirement()",
        "void ProcessDock::retireProcessActivityHistory(",
        "std::size_t ProcessDock::trimProcessActivitySamples()",
    ]
    methods = "\n\n".join(declaration(source, value) for value in signatures)
    types = declaration(header, "    struct ProcessActivityProcessPoint") + ";\n" + declaration(header, "    struct ProcessActivitySample") + ";"
    fixture = Path(__file__).with_suffix(".cpp").read_text(encoding="utf-8-sig")
    fixture = fixture.replace("__HISTORY_HEADER__", history_header.resolve().as_posix()).replace("__METHODS__", methods).replace("__TYPES__", types)
    cpp = output / "fixture.cpp"
    cpp.write_text(fixture, encoding="utf-8")
    developers = [Path("D:/Software/VS/Common7/Tools/VsDevCmd.bat"), Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat")]
    developer = next(path for path in developers if path.exists())
    command = ["cl.exe", "/nologo", "/Zc:__cplusplus", "/std:c++20", "/utf-8", "/EHsc", "/MD", "/O2", "/W4", "/WX", "/I" + str(args.qt / "include"), "/I" + str(args.qt / "include/QtCore"), str(cpp.resolve()), "/Fo" + str((output / "fixture.obj").resolve()), "/Fe" + str((output / "fixture.exe").resolve()), "/link", "/LIBPATH:" + str(args.qt / "lib"), "Qt6Core.lib"]
    build = output / "build.cmd"
    build.write_text('@echo off\ncall "' + str(developer) + '" -arch=x64 -host_arch=x64 >nul\n' + subprocess.list2cmdline(command) + '\nexit /b %errorlevel%\n', encoding="utf-8")
    compiled = subprocess.run(["cmd.exe", "/d", "/c", str(build.resolve())], capture_output=True, text=True)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr)
        raise SystemExit(compiled.returncode)
    environment = dict(os.environ)
    environment["PATH"] = str(args.qt / "bin") + os.pathsep + environment.get("PATH", "")
    started = time.perf_counter()
    result = subprocess.run([str((output / "fixture.exe").resolve())], env=environment, capture_output=True, text=True, timeout=45)
    evidence = {"returncode": result.returncode, "elapsed_seconds": round(time.perf_counter() - started, 3), "stdout": result.stdout, "stderr": result.stderr, "scope": "actual history/lane header plus production retirement and trim methods; QtCore only, fake Dock/Spin, no widgets or system actions"}
    (output / "result.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(evidence, ensure_ascii=False))
    if result.returncode:
        raise SystemExit(result.returncode)
    assert "m_activitySamples.NotePolicyChange();" in source
    assert "retireProcessActivityHistory(true);" in declaration(source, "ProcessDock::~ProcessDock()")
    assert "retireProcessActivityHistory();\n        if (m_activityChartWidget != nullptr) m_activityChartWidget->notifyHistoryReset();" in source
    assert "m_activitySamples.pop_front()" not in source
    assert "m_activitySamples.Clear();" in methods
    assert "m_activitySamples.push_back(std::move(sample));" in source
    assert "if (!isProcessActivityRecordingAllowedNow())" in declaration(source, "void ProcessDock::appendProcessActivitySample()")
    print("PASS: policy revision, clear/reset/close, unchanged sampling admission and logical-only trim wiring")


if __name__ == "__main__":
    main()
