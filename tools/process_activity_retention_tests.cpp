// QtCore only. Production history/lane and actual three Dock methods are used.
#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtCore/QThreadPool>
#include <QtCore/QRunnable>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "__HISTORY_HEADER__"

static std::size_t checks = 0;
static void check(bool value) { ++checks; if (!value) { std::cerr << "failed assertion " << checks << '\n'; std::abort(); } }
static std::thread::id guiThread;
static std::atomic<std::uint64_t> destroyed{0}, wrongThread{0}, live{0};
static std::atomic<bool> block{false}, entered{false}, releaseBlock{false};
static std::uint64_t blockingSequence = 0;
struct Probe
{
    std::uint64_t sequence;
    std::uint64_t elapsedMs;
    std::vector<std::string> payload;
    bool owns = true;
    explicit Probe(std::uint64_t id, bool heavy = false) : sequence(id), elapsedMs(id * 17)
    {
        ++live;
        if (heavy) payload.assign(12, std::string(96, 'x'));
    }
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    Probe(Probe&& other) noexcept : sequence(other.sequence), elapsedMs(other.elapsedMs), payload(std::move(other.payload)), owns(std::exchange(other.owns, false)) {}
    Probe& operator=(Probe&&) = delete;
    ~Probe()
    {
        if (!owns) return;
        if (std::this_thread::get_id() == guiThread) ++wrongThread;
        if (block && sequence == blockingSequence && std::this_thread::get_id() != guiThread)
        {
            entered = true;
            while (!releaseBlock) std::this_thread::yield();
        }
        ++destroyed;
        --live;
    }
};
struct FakeSpin { int current = 50; int value() const { return current; } };
class ProcessDock : public QObject
{
public:
    enum class ActivityHistoryMode { None, All, Recent };
    ActivityHistoryMode m_activityHistoryMode = ActivityHistoryMode::Recent;
    FakeSpin spin;
    FakeSpin* m_activityHistoryLimitSpin = &spin;
    int m_activityTableSnapshotIndex = -1;
    ks::process::ProcessActivityHistory<Probe> m_activitySamples;
    std::shared_ptr<ks::process::ProcessActivityHistoryRetirementLane<Probe>> m_activityHistoryRetirementLane =
        std::make_shared<ks::process::ProcessActivityHistoryRetirementLane<Probe>>();
    QTimer* m_activityHistoryRetirementTimer = nullptr;
    ~ProcessDock() override { retireProcessActivityHistory(true); }
    std::size_t trimProcessActivitySamples();
    void scheduleProcessActivityHistoryRetirement();
    void pumpProcessActivityHistoryRetirement();
    void retireProcessActivityHistory(bool finalizing = false);
};
__METHODS__
__TYPES__
static bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 10000)
{
    QElapsedTimer clock; clock.start();
    while (!predicate())
    {
        if (clock.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return true;
}
static void add(ProcessDock& dock, std::uint64_t sequence, bool heavy = false) { dock.m_activitySamples.push_back(Probe(sequence, heavy)); }
static void assertRange(const ks::process::ProcessActivityHistory<Probe>& history, const std::deque<std::uint64_t>& expected)
{
    check(history.size() == expected.size());
    check(history.empty() == expected.empty());
    check(static_cast<std::size_t>(history.cend() - history.cbegin()) == expected.size());
    std::size_t index = 0;
    for (const auto& sample : history) { check(sample.sequence == expected[index]); check(sample.elapsedMs == expected[index] * 17); ++index; }
    for (std::size_t i = 0; i < expected.size(); ++i) check(history[i].sequence == expected[i]);
    if (!expected.empty())
    {
        check(history.front().sequence == expected.front()); check(history.back().sequence == expected.back());
        const auto mid = expected[expected.size() / 2];
        const auto found = std::lower_bound(history.cbegin(), history.cend(), mid,
            [](const Probe& sample, std::uint64_t value) { return sample.sequence < value; });
        check(found != history.cend() && found->sequence == mid);
        check(history.cbegin()[static_cast<std::ptrdiff_t>(expected.size() - 1)].sequence == expected.back());
    }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); guiThread = std::this_thread::get_id();
    QThreadPool::globalInstance()->setMaxThreadCount(1);
    std::uint64_t heartbeat = 0; QTimer heart; QObject::connect(&heart, &QTimer::timeout, [&]() { ++heartbeat; }); heart.start(1);
    std::uint64_t next = 1;
    // Exact canonical range before any worker can run, extremes N=0/1/H/>H/SIZE_MAX.
    {
        auto dock = std::make_unique<ProcessDock>(); dock->m_activityHistoryMode = ProcessDock::ActivityHistoryMode::All;
        for (std::size_t i = 0; i < 200000; ++i) add(*dock, next++);
        const auto* retainedAddress = &dock->m_activitySamples.back();
        check(dock->trimProcessActivitySamples() == 0);
        check(dock->m_activitySamples.TrimTo(std::numeric_limits<std::size_t>::max()) == 0);
        check(dock->m_activitySamples.TrimTo(200000) == 0);
        dock->m_activityHistoryMode = ProcessDock::ActivityHistoryMode::Recent; dock->spin.current = 1;
        dock->m_activityTableSnapshotIndex = 199999;
        dock->m_activitySamples.NotePolicyChange();
        check(dock->trimProcessActivitySamples() == 199999);
        check(dock->m_activitySamples.size() == 1 && &dock->m_activitySamples.front() == retainedAddress);
        check(dock->m_activityTableSnapshotIndex == 0);
        check(dock->m_activitySamples.front().sequence == 200000);
        // A later All policy never resurrects the logically removed prefix.
        dock->m_activitySamples.NotePolicyChange(); dock->m_activityHistoryMode = ProcessDock::ActivityHistoryMode::All;
        check(dock->trimProcessActivitySamples() == 0 && dock->m_activitySamples.size() == 1);
        check(dock->m_activitySamples.stamp().policy == 2);
        check(dock->m_activitySamples.TrimTo(0) == 1 && dock->m_activitySamples.empty());
        check(dock->m_activitySamples.TrimTo(0) == 0);
        dock.reset();
    }
    check(waitUntil([]() { return live == 0; }));
    check(wrongThread == 0);
    // Block the active lane while changing policy, appending, clearing and closing.
    {
        auto dock = std::make_unique<ProcessDock>();
        for (int i = 0; i < 1000; ++i) add(*dock, next++, true);
        blockingSequence = dock->m_activitySamples.front().sequence; block = true; entered = false; releaseBlock = false;
        dock->spin.current = 10; dock->m_activityTableSnapshotIndex = 3;
        check(dock->trimProcessActivitySamples() == 990);
        check(dock->m_activityTableSnapshotIndex == -1);
        check(waitUntil([]() { return entered.load(); }));
        const auto firstStamp = dock->m_activityHistoryRetirementLane->batchStamp();
        check(dock->m_activityHistoryRetirementLane->active());
        for (int i = 0; i < 500; ++i)
        {
            add(*dock, next++, true); dock->m_activitySamples.NotePolicyChange();
            dock->m_activityHistoryMode = i % 3 == 0 ? ProcessDock::ActivityHistoryMode::None : ProcessDock::ActivityHistoryMode::Recent;
            dock->spin.current = 1 + i % 7;
            dock->trimProcessActivitySamples();
            check(!dock->m_activityHistoryRetirementLane->TryBegin(dock->m_activitySamples, 32));
        }
        check(dock->m_activityHistoryRetirementLane->batchStamp().policy == firstStamp.policy);
        const auto beforeClear = dock->m_activitySamples.stamp();
        dock->retireProcessActivityHistory(); check(dock->m_activitySamples.empty());
        check(dock->m_activitySamples.stamp().session == beforeClear.session + 1);
        add(*dock, next++, true); const auto newSequence = dock->m_activitySamples.front().sequence;
        dock->spin.current = 50; dock->m_activityHistoryMode = ProcessDock::ActivityHistoryMode::Recent;
        check(dock->trimProcessActivitySamples() == 0);
        for (int i = 0; i < 20; ++i) { QCoreApplication::processEvents(); QThread::msleep(1); }
        check(dock->m_activitySamples.size() == 1 && dock->m_activitySamples.front().sequence == newSequence);
        check(heartbeat > 0 && wrongThread == 0);
        auto lane = dock->m_activityHistoryRetirementLane;
        dock.reset(); // no wait, exactly one final backing while the worker is blocked
        check(lane->active()); releaseBlock = true;
        check(waitUntil([&]() { return !lane->active(); }));
        block = false;
    }
    check(waitUntil([]() { return live == 0; })); check(wrongThread == 0);
    // Randomized GUI-interface interleaving with real worker releases; exact oracle.
    {
        auto dock = std::make_unique<ProcessDock>(); std::deque<std::uint64_t> expected;
        std::mt19937 random(0x258016U);
        for (int operation = 0; operation < 12000; ++operation)
        {
            const auto action = random() % 10;
            if (action < 6) { add(*dock, next); expected.push_back(next++); }
            else if (action == 9)
            {
                dock->retireProcessActivityHistory(); expected.clear(); dock->m_activityTableSnapshotIndex = -1;
            }
            else
            {
                dock->m_activitySamples.NotePolicyChange();
                dock->m_activityHistoryMode = action == 6 ? ProcessDock::ActivityHistoryMode::All :
                    action == 7 ? ProcessDock::ActivityHistoryMode::Recent : ProcessDock::ActivityHistoryMode::None;
                dock->spin.current = static_cast<int>(random() % 101);
                const auto limit = static_cast<std::size_t>(dock->spin.current);
                const auto wanted = dock->m_activityHistoryMode == ProcessDock::ActivityHistoryMode::Recent && expected.size() > limit
                    ? expected.size() - limit : 0;
                check(dock->trimProcessActivitySamples() == wanted);
                for (std::size_t i = 0; i < wanted; ++i) expected.pop_front();
            }
            assertRange(dock->m_activitySamples, expected);
            if (operation % 100 == 0) QCoreApplication::processEvents();
        }
        dock.reset();
    }
    check(waitUntil([]() { return live == 0; })); check(wrongThread == 0);
    // Real production sample payload and identity/time preserve through O(1) trim.
    double legacyMs = 0.0, logicalMs = 0.0;
    {
        std::deque<ProcessActivitySample> legacy;
        ks::process::ProcessActivityHistory<ProcessActivitySample> history;
        for (std::uint64_t i = 0; i < 1500; ++i)
        {
            ProcessActivitySample sample; sample.sequence = i; sample.elapsedMs = i * 1000; sample.unixMilliseconds = 100000 + static_cast<std::int64_t>(i);
            sample.processes.resize(160);
            for (auto& point : sample.processes) { point.identityKey = std::to_string(i) + "|100"; point.imagePath.assign(96, 'p'); point.processName.assign(32, 'n'); }
            legacy.push_back(sample); history.push_back(std::move(sample));
        }
        const auto* tail = &history.back();
        QElapsedTimer timer; timer.start(); while (legacy.size() > 1) legacy.pop_front(); legacyMs = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        timer.restart(); check(history.TrimTo(1) == 1499); logicalMs = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        check(&history.front() == tail && history.front().sequence == 1499 && history.front().elapsedMs == 1499000);
        check(history.front().processes[0].identityKey == "1499|100" && history.front().unixMilliseconds == 101499);
        auto lane = std::make_shared<ks::process::ProcessActivityHistoryRetirementLane<ProcessActivitySample>>();
        check(lane->Finalize(history)); QThreadPool::globalInstance()->start(QRunnable::create([lane]() { lane->Drain(); }));
        check(history.empty()); check(waitUntil([&]() { return !lane->active(); }));
    }
    QThreadPool::globalInstance()->waitForDone();
    check(wrongThread == 0 && live == 0);
    std::cout << "{\"result\":\"passed\",\"checks\":" << checks << ",\"destroyed_on_worker\":" << destroyed
        << ",\"wrong_thread_destructors\":" << wrongThread << ",\"heartbeat\":" << heartbeat
        << ",\"legacy_1499x160_pop_ms\":" << legacyMs << ",\"logical_trim_ms\":" << logicalMs << "}\n";
}
