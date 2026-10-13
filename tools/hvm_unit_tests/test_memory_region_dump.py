"""验证真实区域 Dump 函数的读取、身份、取消和原子文件发布逻辑。

生产函数按原文抽取；Win32 查询、内存门面及 QSaveFile 仅在外部接口
处替代。类型来自生产头文件。测试不加载 Qt/驱动，不读取真实进程。
运行：python tools/hvm_unit_tests/test_memory_region_dump.py
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

# 导入辅助模块也不生成 output/ 之外的 Python 缓存产物。
sys.dont_write_bytecode = True
from test_memory_backend import QT_SHIM
from test_memory_bookmarks import extract_method
from test_memory_client import extract_struct


ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = ROOT / "Ksword5.1/Ksword5.1/MemoryDock"
PRODUCTION = DIRECTORY / "MemoryDock.RegionDump.cpp"
OUTPUT = ROOT / "output"


# 外部接口替代：所有身份、区域和文件状态均由测试脚本控制。
PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
// Qt 字符串拼接仅保留内容语义，原因码仍由生产函数原文生成。
static QString operator+(const QString& left, const QString& right) {
    QString result = left;
    result += right;
    return result;
}
#define _KERNEL_MODE 1
#include "KswordArkMemoryIoctl.h"
using HANDLE = void*;
using LPCVOID = const void*;
using SIZE_T = std::size_t;
using BOOL = int;
using DWORD = unsigned long;
using qint64 = std::int64_t;
constexpr int FALSE = 0;
constexpr unsigned WAIT_TIMEOUT = 258;
constexpr unsigned MEM_COMMIT = 0x1000;
constexpr unsigned MEM_RESERVE = 0x2000;
constexpr unsigned MEM_FREE = 0x10000;
constexpr unsigned PAGE_READWRITE = 4;
struct FILETIME { std::uint32_t dwLowDateTime{}, dwHighDateTime{}; };
struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress{};
    SIZE_T RegionSize{};
    std::uint32_t Protect{}, State{}, Type{};
};

// 进程租约替代：读中和写中可改变身份，验证后续步骤必须拒绝。
static std::uint32_t processPid = 77;
static std::uint64_t processCreation = 0x123456789ULL;
static bool processAlive = true, timesAvailable = true;
static unsigned identityChecks;
static unsigned GetProcessId(HANDLE) { return processPid; }
static unsigned WaitForSingleObject(HANDLE, unsigned) {
    ++identityChecks;
    return processAlive ? WAIT_TIMEOUT : 0;
}
static BOOL GetProcessTimes(HANDLE, FILETIME* creation, FILETIME*, FILETIME*, FILETIME*) {
    creation->dwLowDateTime = static_cast<std::uint32_t>(processCreation);
    creation->dwHighDateTime = static_cast<std::uint32_t>(processCreation >> 32U);
    return timesAvailable;
}
static DWORD GetLastError() { return 5; }
static unsigned toDwordPid(std::uint32_t pid) { return pid; }
static bool isReadableProtect(std::uint32_t protect) { return protect == PAGE_READWRITE; }

// 查询替代始终返回显式元数据；指定调用可失败或返回短结构。
static MEMORY_BASIC_INFORMATION queriedRegion;
static unsigned queryCalls, failedQueryCall, shortQueryCall;
static bool dataStaged, failQueryAfterWrite, shortQueryAfterWrite;
static std::vector<std::uint64_t> queriedAddresses;
// 每个元数据字段可独立改变，用来验证读后的查询没有遗漏字段。
static void ChangeRegion(unsigned field) {
    if (field == 1) queriedRegion.BaseAddress = reinterpret_cast<void*>(0xF000);
    if (field == 2) queriedRegion.RegionSize += 4096;
    if (field == 3) queriedRegion.Protect ^= 2U;
    if (field == 4) queriedRegion.State = MEM_RESERVE;
    if (field == 5) queriedRegion.Type ^= 0x40000U;
}
static SIZE_T VirtualQueryEx(HANDLE, LPCVOID address, MEMORY_BASIC_INFORMATION* value, SIZE_T size) {
    ++queryCalls;
    queriedAddresses.push_back(reinterpret_cast<std::uintptr_t>(address));
    if (queryCalls == failedQueryCall || (dataStaged && failQueryAfterWrite)) return 0;
    *value = queriedRegion;
    return queryCalls == shortQueryCall || (dataStaged && shortQueryAfterWrite) ? size - 1 : size;
}
static std::vector<float> defaultProgress;
struct Progress {
    void set(int, const char*, int, float value) { defaultProgress.push_back(value); }
} kPro;
namespace QIODevice { constexpr int WriteOnly = 1; }

// 文件替代用 pending 与 destination 区分未提交内容和旧目标。
static std::vector<char> destination, pending;
static unsigned openCalls, writeCalls, commitCalls, failedWriteCall;
static unsigned cancelDuringWriteCall, exitDuringWriteCall, regionChangeDuringWriteCall;
static unsigned identityChangeDuringWriteCall;
static bool failOpen, failCommit, fallbackDisabled, cancellation;
// 共享异步控制器只替代 cancel() 外部调用，实际状态仲裁函数保持生产原文。
struct CancelController {
    unsigned cancelCalls = 0;
    void cancel() { ++cancelCalls; cancellation = true; }
};
class QSaveFile {
public:
    explicit QSaveFile(const QString&) {}
    void setDirectWriteFallback(bool enabled) { fallbackDisabled = !enabled; }
    bool open(int) { ++openCalls; pending.clear(); return !failOpen; }
    QString errorString() const { return QStringLiteral("fixture file failure"); }
    void cancelWriting() { pending.clear(); }
    qint64 write(const char* data, qint64 length) {
        ++writeCalls;
        if (writeCalls == failedWriteCall) return length > 0 ? length - 1 : -1;
        if (length < 0) return -1;
        pending.insert(pending.end(), data, data + length);
        dataStaged = true;
        if (writeCalls == cancelDuringWriteCall) cancellation = true;
        if (writeCalls == exitDuringWriteCall) processAlive = false;
        if (writeCalls == identityChangeDuringWriteCall) ++processCreation;
        if (writeCalls == regionChangeDuringWriteCall) queriedRegion.Type ^= 0x40000U;
        return length;
    }
    bool commit() {
        ++commitCalls;
        if (failCommit) return false;
        destination = pending;
        return true;
    }
};

// 内存门面记录每次固定后端、PID、地址和长度，不执行真实读取。
namespace ksword::memory_backend {
struct ReadCall {
    MemoryAccessBackend backend;
    std::uint32_t pid;
    std::uint64_t address, length;
};
static std::vector<ReadCall> reads;
static std::deque<AccessOutcome> readScript;
static bool exitDuringRead, identityChangeDuringRead, cancelDuringRead;
static unsigned regionChangeDuringReadField;
AccessOutcome readVirtual(MemoryAccessBackend backend, const DdmaSession&, std::uint32_t pid,
    std::uint64_t address, std::uint64_t length, bool) {
    reads.push_back({backend, pid, address, length});
    if (exitDuringRead) processAlive = false;
    if (identityChangeDuringRead) ++processCreation;
    if (cancelDuringRead) cancellation = true;
    ChangeRegion(regionChangeDuringReadField);
    if (!readScript.empty()) {
        auto result = readScript.front();
        readScript.pop_front();
        return result;
    }
    AccessOutcome result;
    result.ok = true;
    result.bytesDone = length;
    result.data = QByteArray(static_cast<qsizetype>(length), static_cast<char>(reads.size()));
    return result;
}
}
'''


# 函数声明的 RegionEntry 会在 main() 中从 MemoryDock.h 原文插入。
DECLARATION = r'''
    static bool dumpMemoryRegionToFile(std::uint32_t, const std::shared_ptr<void>&,
        std::uint64_t, const RegionEntry&, ksword::memory_backend::MemoryAccessBackend,
        const QString&, int, const std::function<bool()>&, QString&,
        const std::function<bool()>& = {},
        const std::function<void(std::uint64_t, std::uint64_t)>& = {});
    bool tryCancelMemoryRegionDump();
    std::shared_ptr<std::atomic<RegionDumpPhase>> m_regionDumpPhase;
    CancelController* m_regionDumpOperation = nullptr;
};
'''


# 断言强调可观察行为：无头原始字节、固定通道及失败不替换旧文件。
TESTS = r'''
using namespace ksword::memory_backend;
static unsigned checks, failures;
static constexpr std::uint64_t kCreationTime = 0x123456789ULL;
static constexpr std::uint64_t kBase = 0x10000;
static void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
static MemoryDock::RegionEntry CapturedRegion(std::uint64_t size = 4096) {
    MemoryDock::RegionEntry region;
    region.baseAddress = kBase;
    region.regionSize = size;
    region.protect = PAGE_READWRITE;
    region.state = MEM_COMMIT;
    region.type = 0x20000;
    return region;
}
static void Reset(const MemoryDock::RegionEntry& region = CapturedRegion()) {
    processPid = 77; processCreation = kCreationTime;
    processAlive = timesAvailable = true; identityChecks = 0;
    queriedRegion.BaseAddress = reinterpret_cast<void*>(region.baseAddress);
    queriedRegion.RegionSize = static_cast<SIZE_T>(region.regionSize);
    queriedRegion.Protect = region.protect;
    queriedRegion.State = region.state;
    queriedRegion.Type = region.type;
    queryCalls = failedQueryCall = shortQueryCall = 0; queriedAddresses.clear();
    dataStaged = failQueryAfterWrite = shortQueryAfterWrite = false;
    destination = {'o', 'l', 'd'}; pending.clear();
    defaultProgress.clear();
    openCalls = writeCalls = commitCalls = failedWriteCall = 0;
    cancelDuringWriteCall = exitDuringWriteCall = regionChangeDuringWriteCall = 0;
    identityChangeDuringWriteCall = regionChangeDuringReadField = 0;
    failOpen = failCommit = fallbackDisabled = cancellation = false;
    reads.clear(); readScript.clear();
    exitDuringRead = identityChangeDuringRead = cancelDuringRead = false;
}
static bool Dump(QString& error, const MemoryDock::RegionEntry& region = CapturedRegion(),
    MemoryAccessBackend backend = MemoryAccessBackend::UserMode,
    std::uint32_t pid = 77, std::uint64_t creation = kCreationTime,
    bool emptyHandle = false, bool emptyCancellation = false,
    const std::function<bool()>& beginCommit = {},
    const std::function<void(std::uint64_t, std::uint64_t)>& progress = {}) {
    const std::shared_ptr<void> handle(emptyHandle ? nullptr : reinterpret_cast<void*>(1), [](void*) {});
    const std::function<bool()> canceled = emptyCancellation
        ? std::function<bool()>{} : std::function<bool()>{[] { return cancellation; }};
    return MemoryDock::dumpMemoryRegionToFile(pid, handle, creation, region, backend,
        QStringLiteral("target.bin"), 123, canceled, error, beginCommit, progress);
}
static void Uncommitted(const QString& error, const char* label) {
    Check(destination == std::vector<char>({'o', 'l', 'd'}), label);
    Check(!error.isEmpty(), "failure has a diagnostic");
    Check(commitCalls == 0, "failed work never publishes pending output");
}

// 成功案例同时验证全部字节和每个块边界，不只检查返回 true。
static void TestRawBytesAndFixedBackend() {
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver}) {
        for (const std::uint64_t size : {1ULL, 4096ULL, 1048576ULL, 2097169ULL}) {
            const auto region = CapturedRegion(size);
            Reset(region); QString error;
            Check(Dump(error, region, backend), "complete selected region succeeds on R3 and R0");
            Check(error.isEmpty() && commitCalls == 1 && fallbackDisabled,
                  "complete output commits once with direct fallback disabled");
            Check(destination.size() == size, "raw output has exact requested size without a KMDUMP header");
            const auto expectedCalls = static_cast<std::size_t>((size + KSWORD_ARK_MEMORY_READ_MAX_BYTES - 1)
                / KSWORD_ARK_MEMORY_READ_MAX_BYTES);
            Check(reads.size() == expectedCalls, "single and multiple requests honor the 1 MiB limit");
            std::uint64_t offset = 0;
            bool exactReads = true, exactBytes = destination.size() == size;
            for (std::size_t index = 0; index < reads.size(); ++index) {
                const auto length = std::min<std::uint64_t>(size - offset, KSWORD_ARK_MEMORY_READ_MAX_BYTES);
                exactReads = exactReads && reads[index].backend == backend && reads[index].pid == 77
                    && reads[index].address == kBase + offset && reads[index].length == length;
                if (offset + length > destination.size()) exactBytes = false;
                else exactBytes = exactBytes && std::all_of(destination.begin() + offset,
                    destination.begin() + offset + length,
                    [index](char value) { return value == static_cast<char>(index + 1); });
                offset += length;
            }
            Check(exactReads && offset == size, "fixed backend reads exact contiguous range including tail");
            Check(exactBytes, "every output byte comes from the corresponding captured read");
            Check(defaultProgress.size() == reads.size() && !defaultProgress.empty()
                && std::all_of(defaultProgress.begin(), defaultProgress.end(),
                    [](float value) { return value >= 0.0f && value < 1.0f; })
                && defaultProgress.back() > 0.98f && defaultProgress.back() <= 0.99f,
                "default progress remains a fraction below completion until real file commit");
            Check(queryCalls >= 2 && std::all_of(queriedAddresses.begin(), queriedAddresses.end(),
                [](std::uint64_t address) { return address == kBase; }),
                "selected region metadata is checked initially and before commit");
        }
    }
    Reset(); QString error;
    Check(Dump(error, CapturedRegion(), MemoryAccessBackend::UserMode, 77, kCreationTime, false, true),
          "an absent optional cancellation callback is safe");
}

// 无效参数必须在读取或写文件前拒绝，且不能自行替换成另一个后端。
static void TestParameterFailures() {
    for (const auto backend : {MemoryAccessBackend::Hvm, MemoryAccessBackend::Ddma,
        static_cast<MemoryAccessBackend>(-1), static_cast<MemoryAccessBackend>(99)}) {
        Reset(); QString error;
        Check(!Dump(error, CapturedRegion(), backend), "only captured R3 or R0 backend is accepted");
        Uncommitted(error, "unsupported backend keeps old file");
        Check(reads.empty() && openCalls == 0, "unsupported backend causes no memory or file I/O");
    }
    for (int mode = 0; mode < 7; ++mode) {
        auto region = CapturedRegion();
        if (mode == 0) region.regionSize = 0;
        if (mode == 1) { region.baseAddress = (std::numeric_limits<std::uint64_t>::max)() - 15; region.regionSize = 32; }
        if (mode == 2) region.state = MEM_RESERVE;
        if (mode == 3) region.state = MEM_FREE;
        if (mode == 6) region.regionSize = static_cast<std::uint64_t>((std::numeric_limits<qint64>::max)()) + 1;
        Reset(region); QString error;
        if (mode == 6) readScript.push_back({}); // 缺失长度门禁时也只读一块，避免测试无界分配。
        Check(!Dump(error, region, MemoryAccessBackend::UserMode, mode == 4 ? 0 : 77,
            mode == 5 ? 0 : kCreationTime), "zero invalid range state PID or identity is refused");
        Uncommitted(error, "invalid parameter cannot replace old output");
        Check(reads.empty() && openCalls == 0, "invalid parameter fails before file open and target read");
    }
    Reset(); QString error;
    Check(!Dump(error, CapturedRegion(), MemoryAccessBackend::UserMode, 77, kCreationTime, true),
          "missing independent process handle lease is refused");
    Uncommitted(error, "missing lease keeps old output");
}

// 前置身份失败不读内存；读中变化必须在写本块前被发现。
static void TestIdentityFailures() {
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver}) {
        for (int mode = 0; mode < 6; ++mode) {
            Reset();
            if (mode == 0) processAlive = false;
            if (mode == 1) processPid = 88;
            if (mode == 2) ++processCreation;
            if (mode == 3) timesAvailable = false;
            if (mode == 4) exitDuringRead = true;
            if (mode == 5) identityChangeDuringRead = true;
            QString error;
            Check(!Dump(error, CapturedRegion(), backend), "exited unavailable or mismatched process identity is refused");
            Uncommitted(error, "stale target never replaces old file");
            Check(writeCalls == 0, "identity change during read is found before writing that block");
            Check(mode >= 4 || reads.empty(), "initial identity failure causes no memory read");
        }
        for (int mode = 0; mode < 2; ++mode) {
            Reset();
            if (mode == 0) exitDuringWriteCall = 1;
            if (mode == 1) identityChangeDuringWriteCall = 1;
            QString error;
            Check(!Dump(error, CapturedRegion(), backend), "process exit or identity change after final write prevents commit");
            Uncommitted(error, "precommit identity recheck protects the old file");
            Check(reads.size() == 1 && writeCalls == 1, "final identity failure exercises a completely staged region");
        }
    }
}

// 查询的每个身份字段都重要；最终复核也不能略过。
static void TestRegionMetadataFailures() {
    for (int mode = 0; mode < 7; ++mode) {
        Reset();
        if (mode == 0) queriedRegion.BaseAddress = reinterpret_cast<void*>(kBase - 4096);
        if (mode == 1) queriedRegion.RegionSize += 4096;
        if (mode == 2) queriedRegion.Protect ^= 2U;
        if (mode == 3) queriedRegion.State = MEM_RESERVE;
        if (mode == 4) queriedRegion.Type ^= 0x40000U;
        if (mode == 5) failedQueryCall = 1;
        if (mode == 6) shortQueryCall = 1;
        QString error;
        Check(!Dump(error), "changed or failed initial region query is refused");
        Uncommitted(error, "initial region failure keeps old output");
        Check(reads.empty(), "initial metadata failure performs no region reads");
    }
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver}) {
        for (unsigned field = 1; field <= 5; ++field) {
            Reset(); regionChangeDuringReadField = field; QString error;
            Check(!Dump(error, CapturedRegion(), backend), "each region metadata change during read is refused");
            Uncommitted(error, "metadata changed during read keeps the old file");
            Check(reads.size() == 1 && writeCalls == 0,
                  "changed region is rechecked before writing bytes returned by the read");
        }
    }
    for (int mode = 0; mode < 3; ++mode) {
        Reset();
        if (mode == 0) regionChangeDuringWriteCall = 1;
        if (mode == 1) failQueryAfterWrite = true;
        if (mode == 2) shortQueryAfterWrite = true;
        QString error;
        Check(!Dump(error), "region change failed or short final query prevents commit");
        Uncommitted(error, "final region recheck protects the existing file");
        Check(reads.size() == 1 && writeCalls == 1 && queryCalls >= 2,
              "final metadata failure checks the staged complete region");
    }
}

// 初始、读中、块间和最终取消均必须保留旧目标，不发布前缀。
static void TestCancellation() {
    Reset(); cancellation = true; QString initialError;
    Check(!Dump(initialError), "initial canceled request is refused");
    Uncommitted(initialError, "initial cancellation keeps old file");
    Check(reads.empty() && writeCalls == 0, "initial cancellation performs no target read or write");
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver}) {
        Reset(); cancelDuringRead = true; QString readError;
        Check(!Dump(readError, CapturedRegion(), backend), "cancellation during read stops before writing its bytes");
        Uncommitted(readError, "read cancellation cannot publish stale data");
        Check(reads.size() == 1 && writeCalls == 0, "read cancellation is checked after the external read");
        const auto longRegion = CapturedRegion(2097169);
        Reset(longRegion); cancelDuringWriteCall = 1; QString betweenError;
        Check(!Dump(betweenError, longRegion, backend), "cancellation between chunks stops further memory reads");
        Uncommitted(betweenError, "between-chunk cancellation cannot publish a prefix");
        Check(reads.size() == 1 && writeCalls == 1, "next chunk is not read after cancellation");
        Reset(); cancelDuringWriteCall = 1; QString finalError;
        Check(!Dump(finalError, CapturedRegion(), backend), "cancellation after final write prevents atomic commit");
        Uncommitted(finalError, "precommit cancellation protects the old file");
    }
}

// 多种不完整结果独立验证，避免只删一个长度判据仍然全绿。
static void TestReadFailures() {
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver}) {
        for (int mode = 0; mode < 8; ++mode) {
            Reset(); AccessOutcome outcome;
            outcome.ok = true;
            outcome.bytesDone = 4096;
            outcome.data = QByteArray(4096, 'p');
            if (mode == 0) outcome.ok = false;
            if (mode == 1) outcome.partial = true;
            if (mode == 2) { outcome.bytesDone = 0; outcome.data = QByteArray(); }
            if (mode == 3) { outcome.bytesDone = 17; outcome.data = QByteArray(17, 'p'); }
            if (mode == 4) outcome.data = QByteArray(17, 'p');
            if (mode == 5) outcome.bytesDone = 17;
            if (mode == 6) outcome.data = QByteArray(4097, 'p');
            if (mode == 7) outcome.bytesDone = 4097;
            readScript.push_back(outcome); QString error;
            Check(!Dump(error, CapturedRegion(), backend), "failed partial empty or inconsistent read is refused");
            Uncommitted(error, "invalid read keeps old output");
            Check(writeCalls == 0 && reads.size() == 1 && reads[0].backend == backend,
                  "invalid read is not written or retried through another backend");
        }
        const auto longRegion = CapturedRegion(2097169);
        Reset(longRegion);
        AccessOutcome full;
        full.ok = true; full.bytesDone = KSWORD_ARK_MEMORY_READ_MAX_BYTES;
        full.data = QByteArray(KSWORD_ARK_MEMORY_READ_MAX_BYTES, 'p');
        readScript.push_back(full); readScript.push_back({}); QString error;
        Check(!Dump(error, longRegion, backend), "later chunk failure cannot publish a complete-looking prefix");
        Uncommitted(error, "later read failure preserves the destination");
        Check(reads.size() == 2 && writeCalls == 1,
              "failure after one real chunk stops before writing or reading later chunks");
    }
}

// 文件故障覆盖打开、每一块短写和最终原子提交失败。
static void TestFileFailures() {
    Reset(); failOpen = true; QString openError;
    Check(!Dump(openError), "failed file open is not success");
    Uncommitted(openError, "open failure preserves old target");
    Check(reads.empty() && fallbackDisabled, "open failure uses atomic mode and performs no memory read");
    const auto longRegion = CapturedRegion(2097169);
    for (unsigned failingWrite = 1; failingWrite <= 3; ++failingWrite) {
        Reset(longRegion); failedWriteCall = failingWrite; QString error;
        Check(!Dump(error, longRegion), "each short block write stops the transaction");
        Uncommitted(error, "short write preserves old destination");
        Check(writeCalls == failingWrite && reads.size() == failingWrite,
              "file failure stops at its exact chunk without extra reads");
    }
    Reset(); failCommit = true; QString commitError;
    Check(!Dump(commitError), "failed atomic commit is reported as failure");
    Check(!commitError.isEmpty() && commitCalls == 1 && writeCalls == 1,
          "commit failure carries a reason after staging the full region");
    Check(destination == std::vector<char>({'o', 'l', 'd'}), "commit failure keeps old target bytes");
}

// 提交仲裁必须在完整读取与最终复核之后执行，拒绝时绝不发布。
static void TestCommitArbitrationAndProgress() {
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver}) {
        Reset(); unsigned arbitrationCalls = 0; QString vetoError;
        const auto veto = [&arbitrationCalls] { ++arbitrationCalls; return false; };
        Check(!Dump(vetoError, CapturedRegion(), backend, 77, kCreationTime, false, false, veto),
              "commit arbitration can veto an otherwise complete export");
        Uncommitted(vetoError, "commit veto leaves old destination untouched");
        Check(arbitrationCalls == 1 && reads.size() == 1 && writeCalls == 1 && queryCalls >= 2,
              "arbitration runs once after full staging and final metadata checks");

        const auto region = CapturedRegion(2097169);
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> expected{
            {1048576, region.regionSize}, {2097152, region.regionSize}, {region.regionSize, region.regionSize}};
        for (const bool commitFailure : {false, true}) {
            Reset(region); failCommit = commitFailure; arbitrationCalls = 0;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> observed;
            const auto reportProgress = [&observed](std::uint64_t completed, std::uint64_t total) {
                observed.emplace_back(completed, total);
            };
            const auto permit = [&arbitrationCalls, &observed, &expected] {
                ++arbitrationCalls;
                Check(observed == expected && reads.size() == 3 && writeCalls == 3,
                      "commit arbitration receives a fully staged range with exact tail progress");
                return true;
            };
            QString error;
            Check(Dump(error, region, backend, 77, kCreationTime, false, false, permit, reportProgress)
                == !commitFailure, "commit result remains decisive after arbitration permits publishing");
            Check(observed == expected, "progress reports exact completed and total bytes for all chunks and tail");
            Check(defaultProgress.empty(), "explicit progress callback replaces the default progress publication");
            Check(arbitrationCalls == 1 && commitCalls == 1,
                  "allowed export attempts one atomic commit without repeated arbitration");
            if (commitFailure) {
                Check(!error.isEmpty() && destination == std::vector<char>({'o', 'l', 'd'}),
                      "failed commit after permitted arbitration preserves old bytes and reports failure");
            } else {
                Check(error.isEmpty() && destination.size() == region.regionSize,
                      "permitted successful commit publishes the full raw region");
            }
        }

        // 取消或读取失败不能提前调用提交仲裁，并且失败块不产生完成进度。
        for (int mode = 0; mode < 2; ++mode) {
            Reset(region); arbitrationCalls = 0;
            if (mode == 0) readScript.push_back({});
            if (mode == 1) cancelDuringRead = true;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> observed;
            const auto reportProgress = [&observed](std::uint64_t completed, std::uint64_t total) {
                observed.emplace_back(completed, total);
            };
            const auto permit = [&arbitrationCalls] { ++arbitrationCalls; return true; };
            QString error;
            Check(!Dump(error, region, backend, 77, kCreationTime, false, false, permit, reportProgress),
                  "read failure or cancellation does not reach commit arbitration");
            Uncommitted(error, "unfinished work cannot publish through the arbitration callback");
            Check(arbitrationCalls == 0 && observed.empty(),
                  "failed first block emits neither completed progress nor arbitration attempt");
        }
    }
}

// 实际取消函数决定阶段转移；提交回调模拟外部 compare_exchange 仲裁边界。
static void TestCancellationPhaseArbitration() {
    using Phase = MemoryDock::RegionDumpPhase;
    for (int mode = 0; mode < 3; ++mode) {
        Reset(); MemoryDock dock; CancelController controller;
        if (mode == 1) dock.m_regionDumpPhase = std::make_shared<std::atomic<Phase>>(Phase::Reading);
        if (mode == 2) dock.m_regionDumpOperation = &controller;
        Check(!dock.tryCancelMemoryRegionDump() && controller.cancelCalls == 0,
              "missing phase or operation refuses cancellation without touching a controller");
    }
    Reset(); MemoryDock dock; CancelController controller;
    dock.m_regionDumpOperation = &controller;
    dock.m_regionDumpPhase = std::make_shared<std::atomic<Phase>>(Phase::Reading);
    Check(dock.tryCancelMemoryRegionDump() && dock.m_regionDumpPhase->load() == Phase::CancelRequested
        && controller.cancelCalls == 1, "reading phase accepts cancellation and invokes cancel exactly once");
    Check(dock.tryCancelMemoryRegionDump() && controller.cancelCalls == 1,
          "repeated cancel-requested calls are accepted idempotently without a second cancel");
    for (const auto phase : {Phase::Committing, Phase::Committed, Phase::Failed}) {
        Reset(); controller.cancelCalls = 0; dock.m_regionDumpPhase->store(phase);
        Check(!dock.tryCancelMemoryRegionDump() && controller.cancelCalls == 0
            && dock.m_regionDumpPhase->load() == phase,
            "committing committed and failed stages refuse cancellation and preserve their state");
    }
    Reset(); controller.cancelCalls = 0; dock.m_regionDumpPhase->store(Phase::CancelRequested);
    Check(dock.tryCancelMemoryRegionDump() && controller.cancelCalls == 0,
          "an already canceled phase is idempotent even when no new cancel call is needed");

    // 取消先赢：真实取消函数将阶段变为 CancelRequested，提交回调随后无法进入提交。
    Reset(); controller.cancelCalls = 0; dock.m_regionDumpPhase->store(Phase::Reading);
    const auto cancelWins = [&dock] {
        Check(dock.tryCancelMemoryRegionDump(), "cancel request can win the final publication arbitration");
        Phase expected = Phase::Reading;
        return dock.m_regionDumpPhase->compare_exchange_strong(expected, Phase::Committing);
    };
    QString canceledError;
    Check(!Dump(canceledError, CapturedRegion(), MemoryAccessBackend::UserMode, 77, kCreationTime,
        false, false, cancelWins), "accepted cancellation vetoes a fully staged file commit");
    Uncommitted(canceledError, "cancel winning arbitration preserves old file bytes");
    Check(controller.cancelCalls == 1 && dock.m_regionDumpPhase->load() == Phase::CancelRequested,
          "cancel winning arbitration has one controller cancel and keeps canceled phase");

    // 提交先赢：真实取消函数必须拒绝，不能把已发布文件反馈成取消成功。
    Reset(); controller.cancelCalls = 0; dock.m_regionDumpPhase->store(Phase::Reading);
    const auto commitWins = [&dock, &controller] {
        Phase expected = Phase::Reading;
        const bool enteredCommit = dock.m_regionDumpPhase->compare_exchange_strong(expected, Phase::Committing);
        Check(enteredCommit && !dock.tryCancelMemoryRegionDump() && controller.cancelCalls == 0,
              "publication entering Committing prevents a later cancellation acknowledgment");
        return enteredCommit;
    };
    QString committedError;
    Check(Dump(committedError, CapturedRegion(), MemoryAccessBackend::StandardDriver, 77, kCreationTime,
        false, false, commitWins) && commitCalls == 1 && destination.size() == 4096,
        "commit winning arbitration publishes once without executing controller cancellation");
}
int main() {
    TestRawBytesAndFixedBackend();
    TestParameterFailures();
    TestIdentityFailures();
    TestRegionMetadataFailures();
    TestCancellation();
    TestReadFailures();
    TestFileFailures();
    TestCommitArbitrationAndProgress();
    TestCancellationPhaseArbitration();
    std::cout << "Production memory region dump: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
'''


def main() -> None:
    """抽取生产函数并编译运行；产物只写既有 output/，最终删除本次文件。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=PRODUCTION)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    parser.add_argument("--work-dir", type=Path, default=OUTPUT)
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        parser.error("G++ or Clang++ is required")
    work_dir = args.work_dir.resolve()
    output_root = OUTPUT.resolve()
    if not work_dir.is_dir() or not work_dir.is_relative_to(output_root):
        parser.error("--work-dir must be an existing directory inside repository output/")

    # 生产函数、区域结构和门面类型原文保留，只替换 Qt include 边界。
    source = args.source.read_text(encoding="utf-8-sig")
    method = extract_method(source, "MemoryDock::dumpMemoryRegionToFile")
    cancellation_method = extract_method(source, "MemoryDock::tryCancelMemoryRegionDump")
    dock_header = (DIRECTORY / "MemoryDock.h").read_text(encoding="utf-8-sig")
    region = extract_struct(dock_header, "RegionEntry")
    phase_match = re.search(r"enum class RegionDumpPhase\s*\{[^}]*\};", dock_header)
    if phase_match is None:
        raise ValueError("Production enum missing: RegionDumpPhase")
    facade = (DIRECTORY / "MemoryAccessBackend.h").read_text(encoding="utf-8-sig")
    facade = facade.replace("#pragma once", "").replace("#include <QByteArray>", "").replace("#include <QString>", "")
    # 每次使用唯一文件名；不覆盖并行构建产物，也不创建临时构建目录。
    basename = f"memory-region-dump-{uuid.uuid4().hex}"
    cpp_path = work_dir / f"{basename}.cpp"
    binary = work_dir / (f"{basename}.exe" if os.name == "nt" else basename)
    shim = QT_SHIM.replace("#pragma once", "")
    generated = (shim + facade + PRELUDE + "class MemoryDock {\npublic:\n" + region
        + phase_match.group() + DECLARATION + method + cancellation_method + TESTS)
    try:
        cpp_path.write_text(generated, encoding="utf-8")
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-O2",
            "-I", str(ROOT / "shared/driver"), str(cpp_path), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    finally:
        # 删除前校验每个实际目标；只移除本次生成的两个普通文件。
        for artifact in (cpp_path, binary):
            if artifact.resolve().parent != work_dir:
                raise RuntimeError("Refusing to clean outside the test output directory")
            artifact.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
