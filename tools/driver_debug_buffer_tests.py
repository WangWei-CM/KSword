"""Qt-free concurrency and retention checks over production debug-buffer methods."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, anchor):
    start = text.index(anchor)
    left = text.index("{", start)
    depth = 1
    end = left + 1
    while depth:
        if text[end] == "{": depth += 1
        elif text[end] == "}": depth -= 1
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    area = args.source_root / "Ksword5.1/Ksword5.1/DriverDock"
    header = (area / "DriverDock.h").read_text(encoding="utf-8-sig")
    source = (area / "DriverDock.DebugAndUtils.cpp").read_text(encoding="utf-8-sig")
    production_class = block(header, "class DebugOutputBuffer final") + ";"
    start = source.index("bool DriverDock::DebugOutputBuffer::append(")
    end = source.index("void DriverDock::scheduleDebugOutputFlush()", start)
    methods = source[start:end].replace("DriverDock::", "")
    # The production worker sends one batch for every drain; protocol/loss reporting stays present.
    session = block(source, "struct DriverDock::DebugCaptureSession final")
    worker = block(session, "    void capture()")
    assert "append(std::move(outputLines))" in worker
    assert "QMetaObject::invokeMethod" not in worker
    append = block(session, "    void append(std::vector<DebugOutputLineRecord> records)")
    assert "if (buffer->append(std::move(records))) dispatcher->post(projectionWake);" in append
    assert "ACTION_STOP" in block(session, "    void run() noexcept")
    for token in ("nextSequence = drainResult.nextSequence", "lostBeforeFirst", "droppedCount", "TEXT_TRUNCATED"):
        assert token in worker, token
    fixture = r"""
#include <atomic>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
struct DebugOutputLineRecord { int value = 0; std::uint64_t sequence = 0; };
struct DebugOutputSnapshot {
    std::vector<DebugOutputLineRecord> records;
    std::uint64_t lastSequence = 0;
    bool visible = false;
    bool replace = false;
};
__CLASS__
__METHODS__
int main() {
    DebugOutputBuffer buffer;
    // Hidden acceptance never queues display work and retains precisely the existing tail.
    for (int i = 0; i < 10000; ++i) if (buffer.append({{i, 0}})) return 1;
    auto hidden = buffer.snapshot(0, false, false);
    if (hidden.visible || !hidden.records.empty() || hidden.lastSequence != 10000) return 2;
    buffer.setVisible(true);
    auto tail = buffer.snapshot(0, false, false);
    if (!tail.visible || !tail.replace || tail.records.size() != 2000 || tail.records.front().value != 8000 || tail.records.back().value != 9999) return 3;
    // One outstanding wake covers arbitrary batches, including a synchronous Show projection.
    if (!buffer.append({{10000, 0}})) return 4;
    for (int i = 10001; i < 10500; ++i) if (buffer.append({{i, 0}})) return 5;
    auto show = buffer.snapshot(10000, false, false);
    if (show.replace || show.records.size() != 500) return 6;
    if (buffer.append({{10500, 0}})) return 7; // Show must not release the existing wake.
    auto incremental = buffer.snapshot(show.lastSequence, false, true);
    if (incremental.records.size() != 1 || incremental.replace) return 8;
    if (!buffer.append({{10501, 0}})) return 9;
    // Clear preserves pending-wake ownership and cannot replay pre-clear source records.
    const auto cleared = buffer.clear();
    if (buffer.append({{10502, 0}})) return 10;
    auto fresh = buffer.snapshot(cleared, false, true);
    if (fresh.records.size() != 1 || fresh.records[0].value != 10502 || fresh.replace) return 11;
    auto translated = buffer.snapshot(fresh.lastSequence, true, false);
    if (!translated.replace || translated.records.size() != 1) return 12;
    buffer.clear();
    // Concurrent acceptance/snapshot keeps sequence monotonic and every accepted event in its
    // original most-recent-2000 retention window; no GUI widget or real driver is involved.
    const auto base = buffer.snapshot(0, false, true).lastSequence;
    std::atomic<int> done{0}, wakes{0};
    std::vector<std::thread> producers;
    for (int thread = 0; thread < 4; ++thread) producers.emplace_back([&, thread] {
        for (int batch = 0; batch < 1000; ++batch) {
            std::vector<DebugOutputLineRecord> records;
            for (int i = 0; i < 32; ++i) records.push_back({thread * 32000 + batch * 32 + i, 0});
            if (buffer.append(std::move(records))) ++wakes;
        }
        ++done;
    });
    auto sequence = base;
    std::uint64_t consumed = 0;
    while (done.load() < 4) {
        auto snapshot = buffer.snapshot(sequence, false, true);
        std::uint64_t previous = snapshot.records.empty() ? sequence : snapshot.records.front().sequence - 1U;
        for (const auto& record : snapshot.records) {
            if (record.sequence != ++previous) return 13;
            ++consumed;
        }
        sequence = snapshot.lastSequence;
        std::this_thread::yield();
    }
    for (auto& producer : producers) producer.join();
    auto final = buffer.snapshot(0, true, true);
    if (final.lastSequence != base + 128000 || final.records.size() != 2000) return 14;
    if (final.records.front().sequence != final.lastSequence - 1999) return 15;
    std::cout << "PASS accepted=128000 retained=2000 producers=4 batch=32 wakes=" << wakes
              << " consumed_in_snapshots=" << consumed << " hidden=no_wake clear=no_replay show=single_wake\n";
}
""".replace("__CLASS__", production_class).replace("__METHODS__", methods)
    candidates = [Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"),
                  Path("D:/Software/VS/VC/Tools/MSVC/14.44.35207")]
    vc = next((item for item in candidates if (item / "bin/Hostx64/x64/cl.exe").is_file()), candidates[0])
    sdk = Path("C:/Program Files (x86)/Windows Kits/10")
    version = max(item.name for item in (sdk / "Lib").iterdir() if (item / "ucrt/x64/ucrt.lib").is_file())
    compiler = vc / "bin/Hostx64/x64/cl.exe"
    if not compiler.is_file(): raise RuntimeError("HostX64 compiler is unavailable")
    with tempfile.TemporaryDirectory(prefix="ksword_debug_buffer_") as temporary:
        out = Path(temporary)
        cpp, exe = out / "fixture.cpp", out / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        command = [str(compiler), "/nologo", "/std:c++17", "/EHsc", "/O2", "/W4", "/WX", "/external:W0", "/utf-8", "/I" + str(vc / "include")]
        command += ["/external:I" + str(sdk / "Include" / version / name) for name in ("ucrt", "shared", "um")]
        command += [str(cpp), "/Fo" + str(out / "fixture.obj"), "/Fe" + str(exe), "/link", "/LIBPATH:" + str(vc / "lib/x64")]
        command += ["/LIBPATH:" + str(sdk / "Lib" / version / name / "x64") for name in ("ucrt", "um")]
        subprocess.run(command, cwd=out, check=True)
        subprocess.run([str(exe)], cwd=out, check=True)


if __name__ == "__main__": main()
