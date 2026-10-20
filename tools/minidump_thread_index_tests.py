"""Compile the production ThreadId index in a Qt-free differential fixture."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source_root / "Ksword5.1/Ksword5.1/MinidumpDock/MinidumpParser.Streams.cpp").read_text(encoding="utf-8-sig")
    start = source.index("    ThreadIdIndex BuildThreadIdIndex(")
    end = source.index("\n    void ParseThreadInfoList(", start)
    production = source[start:end]
    info = source[end:source.index("\n    void ParseThreadNames(", end)]
    names = source[source.index("\n    void ParseThreadNames(", end):source.index("\n    void ParseModuleList(", end)]
    assert "threadIndex.find(info.ThreadId)" in info and "for (ThreadEntry& entry : result.threads)" not in info
    assert "threadIndex.find(entry.ThreadId)" in names and "for (ThreadEntry& thread : result.threads)" not in names
    fixture = r"""
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>
struct ThreadEntry { std::uint32_t threadId; };
using ThreadIdIndex = std::unordered_map<std::uint32_t, std::size_t>;
__PRODUCTION__
int main() {
    std::uint64_t checks = 0, oldComparisons = 0;
    for (const std::size_t count : {0U, 1000U, 10000U, 100000U}) {
        std::vector<ThreadEntry> threads;
        for (std::size_t i = 0; i < count; ++i) threads.push_back({static_cast<std::uint32_t>(i * 2U)});
        // Duplicate IDs with conflicting positions must retain the original first hit.
        for (std::size_t i = 0; i < count / 3; ++i) threads.push_back({static_cast<std::uint32_t>(i * 2U)});
        const auto lookup = BuildThreadIdIndex(threads);
        if (lookup.size() != count) return 1;
        std::vector<std::uint32_t> ids(count * 2U + 1U);
        std::iota(ids.begin(), ids.end(), 0U);
        std::mt19937 random(42U);
        std::shuffle(ids.begin(), ids.end(), random);
        for (const auto id : ids) {
            const auto found = lookup.find(id);
            const bool present = id % 2U == 0U && id / 2U < count;
            if ((found != lookup.end()) != present) return 2;
            if (present && found->second != id / 2U) return 3;
            // Compare the actual lookup to the previous linear algorithm at practical sizes.
            if (count <= 10000U) {
                std::size_t oldIndex = 0;
                while (oldIndex < threads.size()) {
                    ++oldComparisons;
                    if (threads[oldIndex].threadId == id) break;
                    ++oldIndex;
                }
                if ((oldIndex != threads.size()) != present) return 4;
                if (present && found->second != oldIndex) return 5;
            }
            ++checks;
        }
        // Both supplemental streams may overwrite the same first match repeatedly.
        if (count) {
            const std::vector<std::uint32_t> updates{0U, 2U, 0U, 0xffffffffU, 2U};
            std::vector<int> values(threads.size(), -1);
            for (std::size_t i = 0; i < updates.size(); ++i) {
                const auto it = lookup.find(updates[i]);
                if (it != lookup.end()) values[it->second] = static_cast<int>(i);
            }
            if (values[0] != 2 || values[1] != 4 || values[count] != -1) return 6;
        }
    }
    std::cout << "PASS checks=" << checks << " old_linear_comparisons=" << oldComparisons
              << " capacity=100000 duplicates=first missing=unchanged streams=2\n";
}
""".replace("__PRODUCTION__", production)
    candidates = [Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"),
                  Path("D:/Software/VS/VC/Tools/MSVC/14.44.35207")]
    vc = next((item for item in candidates if (item / "bin/Hostx64/x64/cl.exe").is_file()), candidates[0])
    sdk = Path("C:/Program Files (x86)/Windows Kits/10")
    version = max(item.name for item in (sdk / "Lib").iterdir() if (item / "ucrt/x64/ucrt.lib").is_file())
    compiler = vc / "bin/Hostx64/x64/cl.exe"
    if not compiler.is_file():
        raise RuntimeError("HostX64 compiler is unavailable")
    with tempfile.TemporaryDirectory(prefix="ksword_thread_index_") as temporary:
        out = Path(temporary)
        cpp, exe = out / "fixture.cpp", out / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        command = [str(compiler), "/nologo", "/std:c++17", "/EHsc", "/O2", "/W4", "/WX", "/external:W0", "/utf-8", "/I" + str(vc / "include")]
        command += ["/external:I" + str(sdk / "Include" / version / name) for name in ("ucrt", "shared", "um")]
        command += [str(cpp), "/Fo" + str(out / "fixture.obj"), "/Fe" + str(exe), "/link", "/LIBPATH:" + str(vc / "lib/x64")]
        command += ["/LIBPATH:" + str(sdk / "Lib" / version / name / "x64") for name in ("ucrt", "um")]
        subprocess.run(command, cwd=out, check=True)
        subprocess.run([str(exe)], cwd=out, check=True)


if __name__ == "__main__":
    main()
