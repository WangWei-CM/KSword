"""QtCore-only lifecycle checks over the production debug capture session.

The fixture extracts the complete production session and canonical buffer, replacing
only ArkDriverClient/DriverHandle with blocking value mocks. No widget, device,
service, clipboard, or application GUI is created.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def block(text, anchor):
    start = text.index(anchor)
    left = text.index("{", start)
    depth, end = 1, left + 1
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[start:end]


def locate(source_root, relative):
    for root in (source_root, *source_root.parents, Path.cwd(), *Path.cwd().parents):
        path = root / relative
        if path.is_file():
            return path.resolve()
    raise FileNotFoundError(relative)


FIXTURE = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QEventLoop>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "__DISPATCHER__"
#include "__REGISTRY__"
#include "__PROTOCOL__"

using namespace std::chrono_literals;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL " << message << '\n';
        std::abort();
    }
}

std::atomic<unsigned int> eventSequence{0U};

struct Blocker final
{
    std::mutex mutex;
    std::condition_variable changed;
    bool blocked = false;
    bool entered = false;
    bool released = false;

    void arrive()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this]() { return !blocked || released; });
    }

    void awaitEntry()
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, 5s, [this]() { return entered; }), "mock phase entered");
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

struct Scenario final
{
    Blocker start;
    Blocker drain;
    Blocker stop;
    bool openFails = false;
    bool startFails = false;
    bool startThrows = false;
    bool drainFails = false;
    bool drainThrows = false;
    bool stopThrows = false;
    bool emitRecord = false;
    std::atomic<int> opens{0}, starts{0}, drains{0}, stops{0}, closes{0}, handles{0};
    std::atomic<unsigned int> startSequence{0U}, stopSequence{0U}, closeSequence{0U};
};

std::mutex scenarioMutex;
std::deque<std::shared_ptr<Scenario>> scenarios;

void enqueueScenario(const std::shared_ptr<Scenario>& scenario)
{
    std::lock_guard<std::mutex> lock(scenarioMutex);
    scenarios.push_back(scenario);
}

namespace ksword::ark
{
    struct IoResult final { bool ok = true; std::string message = "mock"; };
    struct DebugOutputControlResult final { IoResult io; bool unsupported = false; };
    struct DebugOutputRecord final
    {
        std::string text;
        std::uint64_t sequence = 0;
        unsigned long componentId = 0, level = 0, flags = 0;
    };
    struct DebugOutputDrainResult final
    {
        IoResult io;
        std::uint64_t nextSequence = 0, lostBeforeFirst = 0, droppedCount = 0;
        unsigned long responseFlags = 0;
        std::vector<DebugOutputRecord> records;
    };

    class DriverHandle final
    {
    public:
        DriverHandle() noexcept = default;
        explicit DriverHandle(std::shared_ptr<Scenario> scenario) : state(std::move(scenario))
        {
            ++state->handles;
        }
        ~DriverHandle() { reset(); }
        DriverHandle(const DriverHandle&) = delete;
        DriverHandle& operator=(const DriverHandle&) = delete;
        DriverHandle(DriverHandle&& other) noexcept : state(std::move(other.state)) {}
        DriverHandle& operator=(DriverHandle&& other) noexcept
        {
            reset();
            state = std::move(other.state);
            return *this;
        }
        bool isValid() const noexcept { return state != nullptr; }
        void reset() noexcept
        {
            if (state)
            {
                state->closeSequence.store(++eventSequence);
                --state->handles;
                ++state->closes;
                state.reset();
            }
        }
    private:
        std::shared_ptr<Scenario> state;
    };

    class DriverClient final
    {
    public:
        DriverClient()
        {
            std::lock_guard<std::mutex> lock(scenarioMutex);
            check(!scenarios.empty(), "one mock transport per production session");
            state = scenarios.front();
            scenarios.pop_front();
        }

        DriverHandle open() const
        {
            ++state->opens;
            if (state->openFails)
            {
                ::SetLastError(ERROR_FILE_NOT_FOUND);
                return {};
            }
            return DriverHandle(state);
        }

        DebugOutputControlResult controlDebugOutput(DriverHandle& handle, unsigned long action) const
        {
            check(handle.isValid(), "control uses session-owned valid handle");
            DebugOutputControlResult result;
            if (action == KSWORD_ARK_DEBUG_OUTPUT_ACTION_START)
            {
                ++state->starts;
                state->startSequence.store(++eventSequence);
                state->start.arrive();
                if (state->startThrows) throw std::runtime_error("post-START formatting exception");
                result.io.ok = !state->startFails;
                result.unsupported = state->startFails;
            }
            else
            {
                check(action == KSWORD_ARK_DEBUG_OUTPUT_ACTION_STOP, "only START/STOP are sent");
                ++state->stops;
                state->stop.arrive();
                state->stopSequence.store(++eventSequence);
                if (state->stopThrows) throw std::runtime_error("STOP result exception");
            }
            return result;
        }

        DebugOutputDrainResult drainDebugOutput(
            DriverHandle& handle, std::uint64_t afterSequence, unsigned long maximum) const
        {
            check(handle.isValid(), "drain uses session-owned valid handle");
            check(maximum == KSWORD_ARK_DEBUG_OUTPUT_DEFAULT_DRAIN_RECORDS, "original drain batch preserved");
            const int count = ++state->drains;
            state->drain.arrive();
            if (state->drainThrows) throw std::runtime_error("drain exception");
            DebugOutputDrainResult result;
            result.io.ok = !state->drainFails;
            result.nextSequence = afterSequence;
            if (state->emitRecord && count == 1)
            {
                DebugOutputRecord record;
                record.text = "controlled mock output";
                record.sequence = 123U;
                record.componentId = 0xAU;
                record.level = 0xBU;
                record.flags = KSWORD_ARK_DEBUG_OUTPUT_RECORD_FLAG_TEXT_TRUNCATED;
                result.records.push_back(std::move(record));
                result.nextSequence = 123U;
                result.responseFlags = KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_OVERFLOW;
                result.lostBeforeFirst = 7U;
                result.droppedCount = 4U;
            }
            return result;
        }
    private:
        std::shared_ptr<Scenario> state;
    };
}

__RECORD__;
__SNAPSHOT__;
__BUFFER__;
__BUFFER_METHODS__
__SESSION__;

struct CallbackProbe final
{
    int projections = 0;
    int states = 0;
    int finished = 0;
    int total() const { return projections + states; }
};

struct Harness final
{
    std::unique_ptr<QObject> receiver = std::make_unique<QObject>();
    std::shared_ptr<ks::ui::AsyncUiDispatcher> dispatcher =
        std::make_shared<ks::ui::AsyncUiDispatcher>(receiver.get());
    std::shared_ptr<DebugOutputBuffer> buffer = std::make_shared<DebugOutputBuffer>();
    std::shared_ptr<CallbackProbe> probe = std::make_shared<CallbackProbe>();
    std::shared_ptr<DebugCaptureSession> owner;
    std::weak_ptr<DebugCaptureSession> weak;

    explicit Harness(const std::shared_ptr<Scenario>& scenario, bool visible = true)
    {
        enqueueScenario(scenario);
        owner = std::make_shared<DebugCaptureSession>();
        weak = owner;
        owner->buffer = buffer;
        owner->dispatcher = dispatcher;
        buffer->setVisible(visible);
        owner->projectionWake = [view = probe]() { ++view->projections; };
        owner->stateChanged = [view = probe, current = weak, scenario]()
        {
            ++view->states;
            const auto session = current.lock();
            if (session && session->finished.load())
            {
                check(scenario->handles.load() == 0, "finished is published after handle close");
                check(scenario->closes.load() == (scenario->openFails ? 0 : 1), "one close before finished callback");
                if (scenario->stops.load() != 0)
                    check(scenario->stopSequence.load() < scenario->closeSequence.load(), "STOP returns before close");
                ++view->finished;
            }
        };
    }

    void start() { owner->start(); }

    void closeAndRelease()
    {
        dispatcher->close();
        if (owner) owner->requestStop();
        owner.reset();
        receiver.reset();
    }

    ~Harness() { closeAndRelease(); }
};

template<class Predicate>
void until(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate())
    {
        check(std::chrono::steady_clock::now() < deadline, message);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        std::this_thread::sleep_for(1ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
}

void awaitCompletion(Harness& harness)
{
    until([&]() { return harness.owner->finished.load(); }, "production session completed");
    until([&]() { return harness.probe->finished > 0; }, "completion callback delivered");
    harness.owner.reset();
    until([&]() { return harness.weak.expired(); }, "session thread releases final owner");
}

void blockedCase(int phase, int& heartbeats)
{
    auto scenario = std::make_shared<Scenario>();
    Blocker* blocker = nullptr;
    if (phase == 0) { blocker = &scenario->start; blocker->blocked = true; }
    if (phase == 1) { blocker = &scenario->drain; blocker->blocked = true; scenario->emitRecord = true; }
    if (phase == 2) { blocker = &scenario->stop; blocker->blocked = true; scenario->drainFails = true; }
    check(blocker != nullptr, "known blocking phase");
    Harness harness(scenario);
    harness.start();
    blocker->awaitEntry();
    check(!harness.owner->finished.load(), "blocked request cannot claim completion");
    harness.owner->requestStop();
    check(harness.owner->stopRequested.load(), "stop request accepted while transport is blocked");
    const int callbacksAtClose = harness.probe->total();
    harness.closeAndRelease();
    check(!harness.weak.expired(), "blocked worker owns session after GUI owner release");
    check(scenario->closes.load() == 0, "owner release does not close handle under IOCTL");
    const int heartbeatAtClose = heartbeats;
    until([&]() { return heartbeats >= heartbeatAtClose + 3; }, "QtCore heartbeat continues while request remains blocked");
    check(harness.probe->total() == callbacksAtClose, "closed dispatcher drops queued and late callbacks");
    blocker->release();
    until([&]() { return harness.weak.expired(); }, "detached completed session is reclaimed");
    check(scenario->starts.load() == 1 && scenario->stops.load() == 1, "blocked capture eventually STOPs exactly once");
    check(scenario->closes.load() == 1 && scenario->handles.load() == 0, "blocked capture eventually closes exactly once");
    check(scenario->stopSequence.load() < scenario->closeSequence.load(), "blocking STOP completes before close");
    check(harness.probe->total() == callbacksAtClose, "no callback escapes close after transport returns");
    if (phase == 1)
    {
        const auto snapshot = harness.buffer->snapshot(0, true, true);
        bool raw = false, overflow = false, dropped = false, truncated = false;
        for (const auto& line : snapshot.records)
        {
            raw |= line.sourceText.contains(QStringLiteral("[Seq=123][Component=0x0000000A][Level=0x0000000B] controlled mock output"));
            overflow |= line.sourceText.contains(QStringLiteral("7")) && line.translateSourceText;
            dropped |= line.sourceText.contains(QStringLiteral("4")) && line.translateSourceText;
            truncated |= !line.localizedSuffixSource.isEmpty();
        }
        check(raw && overflow && dropped && truncated, "in-flight accepted batch and loss/truncation semantics survive stop");
    }
}

void failureCases()
{
    for (int mode = 0; mode < 5; ++mode)
    {
        auto scenario = std::make_shared<Scenario>();
        scenario->openFails = mode == 0;
        scenario->startFails = mode == 1;
        scenario->startThrows = mode == 2;
        scenario->drainThrows = mode == 3;
        scenario->stopThrows = mode == 4;
        scenario->drainFails = mode == 4;
        Harness harness(scenario);
        harness.start();
        awaitCompletion(harness);
        check(scenario->opens.load() == 1, "failure opens at most once");
        check(scenario->starts.load() == (mode == 0 ? 0 : 1), "failure START count");
        check(scenario->stops.load() == (mode >= 2 ? 1 : 0), "exception after START preserves one STOP attempt");
        check(scenario->closes.load() == (mode == 0 ? 0 : 1), "failure close count");
    }
    // Exercise the quick failure / thread-member-install boundary repeatedly.
    for (int round = 0; round < 64; ++round)
    {
        auto scenario = std::make_shared<Scenario>();
        scenario->openFails = true;
        Harness harness(scenario);
        harness.start();
        awaitCompletion(harness);
    }
}

void gateCases(int& heartbeats)
{
    for (int cancelWaiting = 0; cancelWaiting < 2; ++cancelWaiting)
    {
        auto oldScenario = std::make_shared<Scenario>();
        oldScenario->drain.blocked = true;
        Harness old(oldScenario);
        old.start();
        oldScenario->drain.awaitEntry();
        old.closeAndRelease();

        auto nextScenario = std::make_shared<Scenario>();
        Harness next(nextScenario);
        next.start();
        const int before = heartbeats;
        until([&]() { return heartbeats >= before + 3; }, "heartbeat while another session owns global lease");
        check(nextScenario->opens.load() == 0, "new session cannot open or START before old STOP/close");
        if (cancelWaiting) next.closeAndRelease();
        oldScenario->drain.release();
        until([&]() { return old.weak.expired(); }, "old lease owner eventually reclaimed");
        if (cancelWaiting)
        {
            until([&]() { return next.weak.expired(); }, "canceled waiter eventually reclaimed");
            check(nextScenario->opens.load() == 0 && nextScenario->starts.load() == 0 &&
                nextScenario->stops.load() == 0 && nextScenario->closes.load() == 0,
                "canceled waiter skips all device side effects");
        }
        else
        {
            nextScenario->drain.awaitEntry();
            check(oldScenario->closeSequence.load() < nextScenario->startSequence.load(), "new START follows old STOP and close");
            next.owner->requestStop();
            awaitCompletion(next);
            check(nextScenario->stops.load() == 1 && nextScenario->closes.load() == 1, "serialized replacement cleans once");
        }
    }
}

void idleCases()
{
    // Long waits turn a lost wake into a watchdog failure, without claiming a GUI latency bound.
    for (int round = 0; round < 256; ++round)
    {
        auto scenario = std::make_shared<Scenario>();
        Harness harness(scenario);
        std::atomic_bool entered{false}, returned{false};
        if ((round % 2) == 0) harness.owner->requestStop();
        std::thread waiter([session = harness.owner, &entered, &returned]()
        {
            entered.store(true);
            session->waitForStop(30s);
            returned.store(true);
        });
        until([&]() { return entered.load(); }, "idle waiter entered");
        harness.owner->requestStop();
        until([&]() { return returned.load(); }, "idle wait observes stop without timeout");
        waiter.join();
        check(scenario->opens.load() == 0, "idle helper test never opens transport");
    }
}

void dispatcherCases()
{
    QObject receiver;
    auto dispatcher = std::make_shared<ks::ui::AsyncUiDispatcher>(&receiver);
    int delivered = 0, canceled = 0;
    check(dispatcher->post([&]() { ++delivered; }, [&]() { ++canceled; }), "queued callback accepted before close");
    dispatcher->close();
    QCoreApplication::sendPostedEvents(&receiver);
    QCoreApplication::processEvents();
    check(delivered == 0 && canceled == 1, "queued callback canceled after dispatcher close");
    check(!dispatcher->post([&]() { ++delivered; }, [&]() { ++canceled; }), "late callback refused after close");
    check(delivered == 0 && canceled == 2, "refused callback releases cancellation owner");

    auto disposable = std::make_unique<QObject>();
    auto deletingDispatcher = std::make_shared<ks::ui::AsyncUiDispatcher>(disposable.get());
    check(deletingDispatcher->post([&]() { ++delivered; }, [&]() { ++canceled; }), "callback queued for dying receiver");
    deletingDispatcher->close();
    disposable.reset();
    check(delivered == 0 && canceled == 3, "destroying receiver releases its pending functor safely");
}

void bufferCases()
{
    DebugOutputBuffer buffer;
    for (int index = 0; index < 2100; ++index)
    {
        DebugOutputLineRecord record;
        record.sourceText = QString::number(index);
        check(!buffer.append({std::move(record)}), "hidden buffer never requests projection");
    }
    buffer.setVisible(true);
    auto snapshot = buffer.snapshot(0, false, false);
    check(snapshot.records.size() == 2000U && snapshot.records.front().sourceText == QStringLiteral("100"), "canonical original retention window preserved");
    DebugOutputLineRecord record;
    record.sourceText = QStringLiteral("latest");
    check(buffer.append({record}), "first visible batch requests one wake");
    check(!buffer.append({record}), "second visible batch shares pending wake");
    buffer.snapshot(snapshot.lastSequence, false, true);
    check(buffer.append({record}), "flush returns wake ownership");
    const auto cleared = buffer.clear();
    check(!buffer.append({record}), "clear keeps outstanding wake ownership");
    snapshot = buffer.snapshot(cleared, false, true);
    check(snapshot.records.size() == 1U && !snapshot.replace, "clear does not replay earlier source");
}

void testShutdownRegistry()
{
    auto& registry = ks::ui::AsyncCleanupRegistry::global();
    check(registry.pending() == 0, "no earlier capture leases");
    auto scenario = std::make_shared<Scenario>();
    scenario->drain.blocked = true;
    scenario->stop.blocked = true;
    Harness capture(scenario);
    capture.start();
    scenario->drain.awaitEntry();
    check(registry.pending() == 1, "accepted capture keeps process alive");
    registry.beginShutdown();
    check(capture.owner->stopRequested.load(), "shutdown invokes weak value stop callback");
    auto rejected = std::make_shared<Scenario>();
    Harness refused(rejected);
    bool rejectedStart = false;
    try { refused.start(); } catch (const std::runtime_error&) { rejectedStart = true; }
    check(rejectedStart && rejected->opens.load() == 0, "shutdown gate rejects new capture before open");
    scenario->drain.release();
    scenario->stop.awaitEntry();
    check(registry.pending() == 1 && scenario->handles.load() == 1,
        "blocked STOP retains lease and handle");
    scenario->stop.release();
    awaitCompletion(capture);
    check(registry.pending() == 0 && scenario->handles.load() == 0,
        "lease released only after STOP and close");
    registry.cancelShutdown();
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    testShutdownRegistry();
    int heartbeats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &application, [&]() { ++heartbeats; });
    heartbeat.start(1);
    bufferCases();
    dispatcherCases();
    for (int phase = 0; phase < 3; ++phase) blockedCase(phase, heartbeats);
    failureCases();
    gateCases(heartbeats);
    idleCases();
    check(scenarios.empty(), "all supplied mocks were used");
    std::cout << "PASS production_session=extracted production_buffer=extracted dispatcher=production "
              << "blocked=START,drain,STOP owner_release=nonblocking callbacks_after_close=0 "
              << "STOP=once close_before_finished=yes gate=serialized,canceled_no_START "
              << "idle_stop_races=256 fast_open_failures=64 exceptions=START,drain,STOP shutdown_lease=STOP_close registry_reject=yes "
              << "QtCore_heartbeats=" << heartbeats << " GUI=none driver=mock\n";
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--cleanup-registry", type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    area = source_root / "Ksword5.1/Ksword5.1/DriverDock"
    header = (area / "DriverDock.h").read_text(encoding="utf-8-sig")
    source = (area / "DriverDock.DebugAndUtils.cpp").read_text(encoding="utf-8-sig")
    session = block(source, "struct DriverDock::DebugCaptureSession final").replace("DriverDock::", "")
    begin = source.index("bool DriverDock::DebugOutputBuffer::append(")
    end = source.index("void DriverDock::scheduleDebugOutputFlush(", begin)
    methods = source[begin:end].replace("DriverDock::", "")
    for name in ("stopDebugOutputCapture", "releaseDebugOutputCapture"):
        function = block(source, "void DriverDock::" + name + "()")
        assert "requestStop()" in function and "join(" not in function
    assert "[session = shared_from_this()]" in session
    assert "stopRequired = true" in session and "driverHandle.reset()" in session
    registry = args.cleanup_registry.resolve() if args.cleanup_registry else locate(source_root, "Ksword5.1/Ksword5.1/UI/AsyncCleanupRegistry.h")
    dispatcher = locate(source_root, "Ksword5.1/Ksword5.1/UI/AsyncUiDispatcher.h")
    protocol = locate(source_root, "shared/driver/KswordArkDebugOutputIoctl.h")
    fixture = FIXTURE.replace("__REGISTRY__", registry.as_posix()).replace("__DISPATCHER__", dispatcher.as_posix()).replace("__PROTOCOL__", protocol.as_posix())
    fixture = fixture.replace("__RECORD__", block(header, "struct DebugOutputLineRecord"))
    fixture = fixture.replace("__SNAPSHOT__", block(header, "struct DebugOutputSnapshot"))
    fixture = fixture.replace("__BUFFER__", block(header, "class DebugOutputBuffer final"))
    fixture = fixture.replace("__BUFFER_METHODS__", methods).replace("__SESSION__", session)

    candidates = [Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"),
                  Path("D:/Software/VS/VC/Tools/MSVC/14.44.35207")]
    vc = next((item for item in candidates if (item / "bin/Hostx64/x64/cl.exe").is_file()), None)
    if vc is None:
        raise RuntimeError("HostX64 compiler is unavailable")
    qt_candidates = [Path(os.environ.get("KSWORD_QT_DIR", "__not_configured__")),
                     locate(source_root, "AGENTS.md").parent / ".deps/Qt/6.9.3/msvc2022_64",
                     Path("D:/Software/Qt/6.9.3/msvc2022_64")]
    qt = next((item for item in qt_candidates if (item / "lib/Qt6Core.lib").is_file()), None)
    if qt is None:
        raise RuntimeError("Qt6Core development runtime is unavailable")
    sdk = Path("C:/Program Files (x86)/Windows Kits/10")
    version = max(item.name for item in (sdk / "Lib").iterdir() if (item / "ucrt/x64/ucrt.lib").is_file())
    with tempfile.TemporaryDirectory(prefix="ksword_debug_session_") as temporary:
        out = Path(temporary)
        cpp, exe = out / "fixture.cpp", out / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        command = [str(vc / "bin/Hostx64/x64/cl.exe"), "/nologo", "/std:c++17", "/Zc:__cplusplus", "/permissive-", "/EHsc", "/O2", "/MD",
                   "/W4", "/WX", "/external:W0", "/utf-8", "/I" + str(vc / "include"),
                   "/external:I" + str(qt / "include")]
        command += ["/external:I" + str(sdk / "Include" / version / name) for name in ("ucrt", "shared", "um")]
        command += [str(cpp), "/Fo" + str(out / "fixture.obj"), "/Fe" + str(exe), "/link",
                    "/LIBPATH:" + str(vc / "lib/x64"), "/LIBPATH:" + str(qt / "lib"), "Qt6Core.lib"]
        command += ["/LIBPATH:" + str(sdk / "Lib" / version / name / "x64") for name in ("ucrt", "um")]
        subprocess.run(command, cwd=out, check=True, timeout=90)
        runtime_env = os.environ.copy()
        runtime_env["PATH"] = str(qt / "bin") + os.pathsep + str(vc / "bin/Hostx64/x64") + os.pathsep + runtime_env.get("PATH", "")
        subprocess.run([str(exe)], cwd=out, env=runtime_env, check=True, timeout=60)


if __name__ == "__main__":
    main()
