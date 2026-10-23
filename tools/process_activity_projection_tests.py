"""Replay production snapshot-version and refresh decisions without Qt or system sampling."""
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
    return source[start:end].replace("ProcessDock::", "")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_path = args.source or root / "Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp"
    output = args.output or root / ".codex-build-logs/process-activity-projection-tests"
    output.mkdir(parents=True, exist_ok=True)
    source = source_path.read_text(encoding="utf-8-sig")
    actual = "\n".join(method(source, signature) for signature in (
        "bool ProcessDock::isProcessActivitySnapshotProjectionCurrent() const",
        "bool ProcessDock::shouldRebuildProcessTableForRefresh(const bool forceUiRefresh) const"))
    fixture = r"""
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
struct Sample { std::uint64_t sequence; };
struct Fixture {
 bool visible=true, active=false;
 bool m_activityTableSnapshotProjectionValid=false;
 std::uint64_t m_activityTableSnapshotRecordingTick=1, m_activityRecordingStartTick100ns=1;
 std::uint64_t m_activityTableSnapshotSequence=10;
 int m_activityTableSnapshotIndex=0;
 std::deque<Sample> m_activitySamples{{10},{11}};
 bool m_processTableProjectionPending=false;
 std::chrono::steady_clock::time_point m_lastProcessTableRebuildTime=std::chrono::steady_clock::now();
 bool isProcessListPageVisibleForRecording() const { return visible; }
 bool isProcessActivityTableSnapshotActive() const { return active; }
 int tableRefreshIntervalMillisecondsFromInput() const { return 2000; }
 __METHODS__
};
int main() {
 Fixture f;
 assert(!f.isProcessActivitySnapshotProjectionCurrent());
 f.m_activityTableSnapshotProjectionValid=true;
 assert(f.isProcessActivitySnapshotProjectionCurrent());
 f.active=true;
 for(int n=0;n<10000;++n) { f.m_activitySamples.push_back({static_cast<std::uint64_t>(n+12)}); assert(!f.shouldRebuildProcessTableForRefresh(false)); }
 f.m_activityTableSnapshotIndex=1;
 assert(!f.isProcessActivitySnapshotProjectionCurrent());
 assert(f.shouldRebuildProcessTableForRefresh(false));
 f.m_activityTableSnapshotIndex=-1;
 assert(f.isProcessActivitySnapshotProjectionCurrent());
 assert(!f.shouldRebuildProcessTableForRefresh(false));
 f.m_activityRecordingStartTick100ns=2;
 assert(!f.isProcessActivitySnapshotProjectionCurrent());
 f.m_activityRecordingStartTick100ns=1;
 f.m_processTableProjectionPending=true;
 assert(f.shouldRebuildProcessTableForRefresh(false));
 f.visible=false;
 assert(!f.shouldRebuildProcessTableForRefresh(false));
 assert(!f.shouldRebuildProcessTableForRefresh(true));
 f.visible=true; f.active=false; f.m_processTableProjectionPending=false;
 assert(!f.shouldRebuildProcessTableForRefresh(false));
 f.m_lastProcessTableRebuildTime-=std::chrono::seconds(3);
 assert(f.shouldRebuildProcessTableForRefresh(false));
 f.m_lastProcessTableRebuildTime=std::chrono::steady_clock::now();
 assert(f.shouldRebuildProcessTableForRefresh(true));
 std::cout << "10014 production decision assertions passed\n";
}
""".replace("__METHODS__", actual)
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
