"""Replay real chart aggregate and projection cache methods without Qt or sampling."""
from pathlib import Path
import argparse
import subprocess
import shutil


def method(source, signature):
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
    parser.add_argument("--output", type=Path)
    parser.add_argument("--header", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_path = args.source or root / "Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp"
    output = args.output or root / ".codex-build-logs/process-activity-cache-tests"
    output.mkdir(parents=True, exist_ok=True)
    source = source_path.read_text(encoding="utf-8-sig")
    methods = "\n".join(method(source, signature) for signature in (
        "void prepareAggregateCache(", "std::array<double, 5> metricDenominators() const", "void updateSeriesCache("))
    header = args.header or root / "Ksword5.1/Ksword5.1/ProcessDock/ProcessActivitySeriesCache.h"
    fixture = Path(__file__).with_suffix(".cpp").read_text(encoding="utf-8").replace("__METHODS__", methods).replace("__HEADER__", header.resolve().as_posix())
    cpp = output / "fixture.cpp"
    cpp.write_text(fixture, encoding="utf-8")
    compiler = shutil.which("cl")
    exe = output / "fixture.exe"
    command = [compiler or "cl.exe", "/nologo", "/std:c++20", "/utf-8", "/EHsc", "/W4", "/WX", str(cpp.resolve()), "/Fo"+str((output/"fixture.obj").resolve()), "/Fe"+str(exe.resolve())]
    if compiler:
        subprocess.run(command, check=True, cwd=output)
    else:
        candidates = [Path("D:/Software/VS/Common7/Tools/VsDevCmd.bat"), Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat")]
        developer = next((candidate for candidate in candidates if candidate.exists()), None)
        if not developer:
            raise RuntimeError("Run from an x64 MSVC developer shell")
        build = output / "build.cmd"
        build.write_text('@echo off\ncall "'+str(developer)+'" -arch=x64 -host_arch=x64\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\n', encoding="utf-8")
        subprocess.run(["cmd.exe", "/d", "/c", str(build.resolve())], check=True, cwd=output)
    subprocess.run([str(exe.resolve())], check=True)


if __name__ == "__main__":
    main()
