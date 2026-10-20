"""Compile the actual dump-read value helpers and worker against in-memory I/O.

This fixture creates no widgets and never opens a dump or user file. Its fake
QFile adapter exercises the production worker, including its post-read identity
check. HostX64 MSVC is required, matching the project's normal compiler.
"""
from pathlib import Path
import os
import subprocess
import tempfile


def build_environment():
    candidates = [Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207'),
                  Path('D:/Software/VS/VC/Tools/MSVC/14.44.35207')]
    vc = next((item for item in candidates if (item / 'bin/Hostx64/x64/cl.exe').is_file()), None)
    if vc is None:
        raise RuntimeError('HostX64 compiler is unavailable')
    sdk = Path('C:/Program Files (x86)/Windows Kits/10')
    version = max(item.name for item in (sdk / 'Lib').iterdir() if (item / 'ucrt/x64/ucrt.lib').is_file())
    qt_candidates = [Path(__file__).resolve().parents[1] / '.deps/Qt/6.9.3/msvc2022_64',
                     Path('D:/Software/Qt/6.9.3/msvc2022_64')]
    qt = next((item for item in qt_candidates if (item / 'include/QtCore/QString').is_file()), None)
    if qt is None:
        raise RuntimeError('Qt 6.9.3 x64 development files are unavailable')
    return vc, sdk, version, qt


def compile_command(vc, sdk, version, qt, root):
    command = [str(vc / 'bin/Hostx64/x64/cl.exe'), '/nologo', '/std:c++17', '/permissive-', '/Zc:__cplusplus',
               '/EHsc', '/MD', '/O2', '/W4', '/WX', '/external:W0', '/utf-8',
               '/DQT_NO_DEBUG', '/DQT_CORE_LIB', '/I' + str(root / 'Ksword5.1/Ksword5.1')]
    command += ['/external:I' + str(vc / 'include')]
    command += ['/external:I' + str(sdk / 'Include' / version / name) for name in ('ucrt', 'shared', 'um')]
    command += ['/external:I' + str(qt / 'include'), '/external:I' + str(qt / 'include/QtCore')]
    return command


def main():
    root = Path(__file__).resolve().parents[1]
    # Standalone ignored step copies resolve dependencies from the repository.
    dependency_root = root
    while not (dependency_root / 'shared').is_dir():
        if dependency_root.parent == dependency_root:
            raise RuntimeError('Repository dependency root was not found')
        dependency_root = dependency_root.parent
    source = root / 'Ksword5.1/Ksword5.1/MinidumpDock/DumpMemoryView.cpp'
    text = source.read_text(encoding='utf-8-sig')
    value_types = text[text.index('struct DumpMemorySource'):text.index('namespace\n{')]
    worker = text[text.index('    // kMaximumReadBytes:'):] if '    // kMaximumReadBytes:' in text else text[text.index('    // kMaximumReadBytes：'):]
    worker = worker[:worker.index('    QString inputStyle()')]
    # Adapt only the external effects; the production value/worker bodies stay
    # exact, and fixture checks ensure no widget or translation call is included.
    assert 'DumpMemoryView::' not in worker and 'ks::i18n' not in worker
    worker = worker.replace('const ks::ui::AsyncOperationToken& token', 'const MemoryReadToken& token')
    worker = worker.replace('QFileInfo', 'MemoryFileInfo').replace('QFile ', 'MemoryFile ')
    fixture = r'''
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QIODevice>
#include <QTimeZone>
#include <QString>
#include "MinidumpDock/MinidumpFormat.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
__VALUE_TYPES__
struct MemoryReadToken { bool* canceled; bool isCanceled() const { return *canceled; } };
struct MemoryDevice {
    QByteArray bytes;
    qint64 size = 0;
    qint64 modified = 1000;
    bool exists = true;
    bool isFile = true;
    bool opens = true;
    bool seeks = true;
    bool shortReads = false;
    bool mutateOnRead = false;
    bool cancelOnRead = false;
    bool* canceled = nullptr;
    int opensCount = 0;
    int readsCount = 0;
    int identityCount = 0;
} device;
struct MemoryFileInfo {
    explicit MemoryFileInfo(const QString&) { ++device.identityCount; }
    bool exists() const { return device.exists; }
    bool isFile() const { return device.isFile; }
    qint64 size() const { return device.size; }
    QDateTime lastModified() const { return QDateTime::fromMSecsSinceEpoch(device.modified, QTimeZone::UTC); }
};
struct MemoryFile {
    qint64 offset = 0;
    explicit MemoryFile(const QString&) {}
    bool open(QIODevice::OpenMode) { ++device.opensCount; return device.opens; }
    bool seek(qint64 value) { offset = value; return device.seeks; }
    QByteArray read(qint64 count) {
        ++device.readsCount;
        auto value = device.bytes.mid(offset, count);
        if (device.shortReads && !value.isEmpty()) value.chop(1);
        if (device.mutateOnRead) ++device.modified;
        if (device.cancelOnRead) *device.canceled = true;
        return value;
    }
    qint64 size() const { return device.size; }
    QString errorString() const { return QStringLiteral("memory open failed"); }
};
namespace {
__WORKER__
}
int checks = 0;
void check(bool value) { ++checks; if (!value) throw std::runtime_error("check " + std::to_string(checks)); }
DumpMemorySource sampleSource() {
    DumpMemorySource source;
    source.filePath = QStringLiteral("in-memory");
    source.fileSize = 4096;
    source.modifiedUtcMs = 1000;
    source.ranges.push_back({0x1000, 512, 1024, QStringLiteral("range")});
    return source;
}
void resetDevice(bool& canceled) {
    canceled = false;
    device = {};
    device.bytes = QByteArray(4096, '\0');
    device.size = 4096;
    device.canceled = &canceled;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto source = sampleSource();
    auto plan = planDumpRead(source, 0x1000, 256);
    check(plan.status == DumpMemoryReadStatus::Success && plan.fileOffset == 512 && plan.bytes == 256);
    check(planDumpRead(source, 0x13ff, 65536).bytes == 1);
    check(planDumpRead(source, 0x1400, 256).status == DumpMemoryReadStatus::NotCaptured);
    check(planDumpRead(source, 0x0fff, 256).status == DumpMemoryReadStatus::NotCaptured);
    check(planDumpRead(source, 0x1000, 0).status == DumpMemoryReadStatus::OffsetInvalid);
    source.ranges[0].bytes = 200000; source.fileSize = 300000;
    check(planDumpRead(source, 0x1000, 100000).bytes == 65536);
    source = sampleSource(); source.fileSize = 512;
    check(planDumpRead(source, 0x1000, 1).status == DumpMemoryReadStatus::OffsetInvalid);
    source = sampleSource(); source.ranges[0].fileOffset = std::numeric_limits<std::uint64_t>::max();
    source.fileSize = std::numeric_limits<std::uint64_t>::max();
    check(planDumpRead(source, 0x1001, 1).status == DumpMemoryReadStatus::OffsetInvalid);
    source.ranges[0].fileOffset = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    check(planDumpRead(source, 0x1000, 1).status == DumpMemoryReadStatus::OffsetInvalid);
    source = sampleSource(); source.filePath.clear();
    check(planDumpRead(source, 0x1000, 1).status == DumpMemoryReadStatus::NoFile);
    source = sampleSource(); source.ranges.push_back({0x1400, 3000, 256, {}});
    check(planDumpRead(source, 0x13ff, 256).bytes == 1);
    check(planDumpRead(source, 0x1400, 256).fileOffset == 3000);
    // Differential address/length matrix retains single-range reads and caps.
    for (std::uint64_t address = 0xff0; address < 0x1520; ++address) {
        for (auto count : {1ull, 256ull, 4096ull, 65536ull, 100000ull}) {
            auto value = planDumpRead(source, address, count);
            int index = address >= 0x1000 && address < 0x1400 ? 0 : address >= 0x1400 && address < 0x1500 ? 1 : -1;
            check(value.rangeIndex == index);
            if (index >= 0) {
                const auto& range = source.ranges[static_cast<std::size_t>(index)];
                check(value.status == DumpMemoryReadStatus::Success);
                check(value.bytes == std::min(range.bytes - (address - range.virtualAddress), std::min<std::uint64_t>(count, 65536)));
                check(value.fileOffset == range.fileOffset + address - range.virtualAddress);
            }
        }
    }
    // Initial selection scans at most 512 bytes per range and stops on cancel.
    source = sampleSource(); source.ranges.clear(); source.fileSize = 200000ull * 512;
    for (std::uint64_t i = 0; i < 200000; ++i) source.ranges.push_back({0x1000 + i * 4096, i * 512, 512, {}});
    std::uint64_t probes = 0;
    const auto last = preferredDumpAddress(source, [&probes](auto offset, auto count) {
        ++probes; check(count == 512); return offset == 199999ull * 512;
    }, [] { return false; });
    check(last == source.ranges.back().virtualAddress && probes == 200000);
    probes = 0;
    preferredDumpAddress(source, [&probes](auto, auto) { ++probes; return false; }, [&probes] { return probes == 17; });
    check(probes == 17);
    source.faultingAddress = source.ranges[5].virtualAddress + 3; probes = 0;
    check(preferredDumpAddress(source, [&probes](auto, auto) { ++probes; return false; }, [] { return false; }) == source.faultingAddress);
    check(probes == 0);
    // Actual worker with in-memory effects: no dump or filesystem access.
    bool canceled = false; MemoryReadToken token{&canceled}; resetDevice(canceled);
    auto shared = std::make_shared<DumpMemorySource>(sampleSource());
    DumpMemoryReadRequest request{shared, 123, 0x1004, 256, false};
    device.bytes[516] = '\x7f';
    auto result = readDumpMemory(request, token);
    check(result.status == DumpMemoryReadStatus::Success && result.address == 0x1004 && result.bytes.size() == 256);
    check(result.sourceGeneration == 123 && result.bytes[0] == '\x7f' && device.opensCount == 1 && device.identityCount == 2);
    resetDevice(canceled); device.modified = 1001;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::FileChanged && device.opensCount == 0);
    resetDevice(canceled); device.opens = false;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::OpenFailed);
    resetDevice(canceled); device.seeks = false;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::ReadFailed);
    resetDevice(canceled); device.shortReads = true;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::ReadFailed);
    resetDevice(canceled); device.mutateOnRead = true;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::FileChanged);
    resetDevice(canceled); device.cancelOnRead = true;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::Canceled);
    resetDevice(canceled); canceled = true;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::Canceled && device.opensCount == 0);
    resetDevice(canceled); request.chooseInitialAddress = true;
    device.bytes[1023] = 1; // byte 511 is in the 512-byte initial sample.
    check(readDumpMemory(request, token).address == 0x1000 && device.readsCount == 2);
    resetDevice(canceled); shared->faultingAddress = 0x1009;
    check(readDumpMemory(request, token).address == 0x1009 && device.readsCount == 1);
    resetDevice(canceled); shared->faultingAddress = 0; device.cancelOnRead = true;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::Canceled && device.readsCount == 1);
    resetDevice(canceled); request.chooseInitialAddress = false; request.address = 0x1400;
    check(readDumpMemory(request, token).status == DumpMemoryReadStatus::NotCaptured && device.opensCount == 0);
    ks::minidump::DumpParseResult parse;
    parse.filePath = shared->filePath; parse.fileSize = shared->fileSize; parse.fileLastModifiedUtcMs = shared->modifiedUtcMs;
    parse.pointerSize = shared->pointerSize; parse.capturedMemoryRanges = shared->ranges;
    check(sameDumpMemorySource(*shared, parse));
    ++parse.capturedMemoryRanges[0].fileOffset; check(!sameDumpMemorySource(*shared, parse));
    --parse.capturedMemoryRanges[0].fileOffset; parse.faultingAddress = 0x1000; check(!sameDumpMemorySource(*shared, parse));
    parse.faultingAddress = 0; parse.pointerSize = 4; check(!sameDumpMemorySource(*shared, parse));
    parse.pointerSize = shared->pointerSize;
    shared->modules.push_back({QStringLiteral("module"), 0x1000, 512}); parse.modules = shared->modules;
    check(sameDumpMemorySource(*shared, parse)); ++parse.modules[0].base; check(!sameDumpMemorySource(*shared, parse));
    parse.modules = shared->modules;
    shared->regions.push_back({0x1000, 512, QStringLiteral("state"), {}, {}, {}}); parse.memoryRegions = shared->regions;
    check(sameDumpMemorySource(*shared, parse)); parse.memoryRegions[0].protect = QStringLiteral("changed");
    check(!sameDumpMemorySource(*shared, parse));
    std::cout << "PASS checks=" << checks << " ranges=200000 memory_io=true widgets=0 real_files=0\n";
}
'''.replace('__VALUE_TYPES__', value_types).replace('__WORKER__', worker)
    vc, sdk, version, qt = build_environment()
    with tempfile.TemporaryDirectory(prefix='ksword_dump_read_') as temporary:
        out = Path(temporary)
        cpp, exe = out / 'fixture.cpp', out / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        command = compile_command(vc, sdk, version, qt, dependency_root)
        command += [str(cpp), '/Fo' + str(out / 'fixture.obj'), '/Fe' + str(exe), '/link',
                    '/LIBPATH:' + str(vc / 'lib/x64'), '/LIBPATH:' + str(qt / 'lib'), 'Qt6Core.lib']
        command += ['/LIBPATH:' + str(sdk / 'Lib' / version / name / 'x64') for name in ('ucrt', 'um')]
        subprocess.run(command, cwd=out, check=True)
        environment = os.environ.copy()
        environment['PATH'] = str(qt / 'bin') + os.pathsep + environment.get('PATH', '')
        subprocess.run([str(exe)], cwd=out, env=environment, check=True)
    # Callers and worker boundary are checked without constructing a widget.
    members = text[text.index('DumpMemoryView::DumpMemoryView'):]
    assert '.open(' not in members and '.seek(' not in members and '.read(' not in members
    assert 'm_readOperation->isInFlight()' in members
    destructor = members[members.index('DumpMemoryView::~DumpMemoryView'):members.index('void DumpMemoryView::changeEvent')]
    assert 'm_readOperation->close();' in destructor
    retranslate = members[members.index('void DumpMemoryView::retranslateUi'):members.index('bool DumpMemoryView::readAddressText')]
    assert 'submitRead' not in retranslate and 'loadAddress' not in retranslate
    print('PASS source_boundary=worker_only_io lifetime=shared_async_operation language=no_read')
    run_operation_fixture(vc, sdk, version, qt, dependency_root)


def run_operation_fixture(vc, sdk, version, qt, root):
    fixture = r'''
#include "UI/AsyncOperation.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace ks::ui;
struct Gate {
    std::atomic_bool release{false};
    std::atomic_int active{0}, maximum{0}, calls{0}, cleanup{0};
};
struct Request { int value; std::shared_ptr<Gate> gate; };
int work(const Request& request, const AsyncOperationToken&) {
    ++request.gate->calls;
    const int active = ++request.gate->active;
    int previous = request.gate->maximum.load();
    while (previous < active && !request.gate->maximum.compare_exchange_weak(previous, active)) {}
    while (!request.gate->release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    --request.gate->active;
    if (request.value < 0) throw std::runtime_error("controlled value failure");
    return request.value;
}
void check(bool value) { if (!value) throw std::runtime_error("operation assertion"); }
template<class Predicate> void spin(Predicate done) {
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1);
    }
    check(done());
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    AsyncOperation operation(&app);
    std::vector<int> applied; int failed = 0;
    auto submit = [&](int value, std::shared_ptr<Gate> gate) {
        return operation.submit<Request,int>(Request{value, gate}, work,
            [&applied](int result, std::uint64_t) { applied.push_back(result); return true; },
            [&failed](std::exception_ptr) { ++failed; },
            [gate](AsyncOperationOutcome) { ++gate->cleanup; });
    };
    // A is deliberately blocked like one OS I/O; only latest pending C runs.
    auto gate = std::make_shared<Gate>();
    submit(1, gate); spin([&] { return gate->active.load() == 1; });
    submit(2, gate); submit(3, gate);
    check(operation.isInFlight() && operation.hasPending());
    gate->release = true; spin([&] { return !operation.isInFlight(); });
    check(applied == std::vector<int>{3} && failed == 0 && gate->maximum == 1 && gate->calls == 2 && gate->cleanup == 3);
    // Cancel invalidates late completion but retains the actual execution slot.
    gate = std::make_shared<Gate>(); submit(4, gate); spin([&] { return gate->active == 1; });
    operation.cancel(); check(operation.isInFlight() && !operation.isBusy());
    gate->release = true; spin([&] { return !operation.isInFlight(); });
    check(applied == std::vector<int>{3} && gate->cleanup == 1);
    // Source replacement suppresses A and applies B after A actually returns.
    gate = std::make_shared<Gate>(); submit(10, gate); spin([&] { return gate->active == 1; });
    operation.cancel(); submit(20, gate); gate->release = true;
    spin([&] { return !operation.isInFlight(); });
    check(applied == std::vector<int>({3,20}) && gate->maximum == 1 && gate->calls == 2 && gate->cleanup == 2);
    // Worker exceptions return through the UI failure path and unlock the slot.
    gate = std::make_shared<Gate>(); gate->release = true; submit(-1, gate);
    spin([&] { return !operation.isInFlight(); }); check(failed == 1 && gate->cleanup == 1);
    submit(30, gate); spin([&] { return !operation.isInFlight(); });
    check(applied.back() == 30 && gate->cleanup == 2);
    // Closing before members/children are destroyed rejects late delivery.
    gate = std::make_shared<Gate>();
    auto closing = std::make_unique<AsyncOperation>(&app);
    closing->submit<Request,int>(Request{40, gate}, work,
        [&applied](int value, std::uint64_t) { applied.push_back(value); return true; },
        [&failed](std::exception_ptr) { ++failed; },
        [gate](AsyncOperationOutcome) { ++gate->cleanup; });
    spin([&] { return gate->active == 1; }); closing->close(); closing.reset();
    gate->release = true; spin([&] { return gate->active == 0; });
    QCoreApplication::processEvents();
    check(applied.back() == 30 && gate->cleanup == 1 && failed == 1);
    operation.close();
    std::cout << "PASS async_core=latest_pending,cancel,source_replace,failure,close maximum_active=1 widgets=0\n";
}
'''
    with tempfile.TemporaryDirectory(prefix='ksword_dump_async_core_') as temporary:
        out = Path(temporary)
        cpp, exe = out / 'fixture.cpp', out / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        command = compile_command(vc, sdk, version, qt, root)
        command += [str(cpp), str(root / 'Ksword5.1/Ksword5.1/UI/AsyncOperation.cpp'),
                    '/Fo' + str(out) + os.sep, '/Fe' + str(exe), '/link', '/LIBPATH:' + str(vc / 'lib/x64'),
                    '/LIBPATH:' + str(qt / 'lib'), 'Qt6Core.lib']
        command += ['/LIBPATH:' + str(sdk / 'Lib' / version / name / 'x64') for name in ('ucrt', 'um')]
        subprocess.run(command, cwd=out, check=True)
        environment = os.environ.copy()
        environment['PATH'] = str(qt / 'bin') + os.pathsep + environment.get('PATH', '')
        subprocess.run([str(exe)], cwd=out, env=environment, check=True)


if __name__ == '__main__':
    main()
