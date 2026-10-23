#include "../KswordARKLight/Ui/BackgroundWork.h"

#include <cassert>
#include <deque>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

using Ksword::Ui::BackgroundWorkRegistry;

int main() {
    std::deque<std::function<void()>> worker;
    bool failLaunch = true;
    BackgroundWorkRegistry registry([&](std::function<void()> task) {
        if (failLaunch) throw std::runtime_error("fake thread resource exhaustion");
        worker.push_back(std::move(task));
    });
    auto accepted = registry.tryAccept();
    assert(accepted && registry.pending() == 1);
    registry.beginClosing();
    assert(registry.closing() && !registry.tryAccept());
    bool ran = false;
    auto owner = std::make_shared<int>(7);
    const std::weak_ptr<int> weak = owner;
    auto cleanup = registry.retire([owner, &ran] { ran = true; return *owner == 7; });
    owner.reset();
    assert(!ran && registry.pending() == 2 && !weak.expired());
    assert(cleanup->failure() == BackgroundWorkRegistry::Failure::ThreadCreation);
    accepted.reset();
    assert(registry.pending() == 1);
    bool unstartedRan = false;
    auto unstarted = registry.retire([&] { unstartedRan = true; return true; });
    assert(unstarted->failure() == BackgroundWorkRegistry::Failure::ThreadCreation);
    assert(registry.withdrawUnstarted(unstarted));
    assert(unstarted->complete() && !unstartedRan && registry.pending() == 1);
    failLaunch = false;
    registry.retry();
    assert(!ran && worker.size() == 1);
    assert(!registry.withdrawUnstarted(cleanup));
    auto runOne = [&] { auto task = std::move(worker.front()); worker.pop_front(); task(); };
    runOne();
    assert(ran && cleanup->complete() && registry.pending() == 0 && weak.expired());
    registry.cancelClosing();
    assert(!registry.closing() && registry.tryAccept());

    unsigned stops = 0;
    bool stopSucceeds = false;
    auto stop = registry.retire([&] { ++stops; return stopSucceeds; });
    auto next = registry.retire([] { return true; });
    assert(worker.size() == 1);
    runOne();
    assert(stops == 1 && !stop->complete() && worker.size() == 1);
    assert(stop->failure() == BackgroundWorkRegistry::Failure::Operation);
    assert(!registry.withdrawUnstarted(stop));
    runOne();
    assert(next->complete() && registry.pending() == 1);
    registry.retry();
    assert(worker.empty() && stops == 1);
    registry.beginClosing();
    registry.cancelClosing();
    assert(stops == 1 && !stop->complete());
    stopSucceeds = true;
    registry.retry(true);
    assert(worker.size() == 1 && stops == 1);
    runOne();
    assert(stops == 2 && stop->complete() && registry.pending() == 0);

    // Completed cleanup never runs twice; reentrant queueing remains serial.
    registry.retry(true);
    assert(worker.empty());
    auto parent = registry.retire([&] { registry.retire([] { return true; }); return true; });
    assert(worker.size() == 1);
    runOne();
    assert(parent->complete() && worker.size() == 1 && registry.pending() == 1);
    runOne();
    assert(registry.pending() == 0);
    struct Session {
        std::mutex mutex;
        std::condition_variable idle;
        std::atomic_size_t pending{0};
        bool owns = false;
        unsigned stops = 0;
    };
    auto session = std::make_shared<Session>();
    auto held = Ksword::Ui::HoldPendingOperation(session);
    std::function<void()> acceptedStart = [session, held] {
        std::lock_guard lock(session->mutex);
        session->owns = true;
    };
    held.reset();
    std::mutex enteredMutex;
    std::condition_variable enteredCondition;
    bool entered = false;
    std::atomic_bool finished = false;
    BackgroundWorkRegistry actualWorker;
    auto retired = actualWorker.retire([&] {
        { std::lock_guard lock(enteredMutex); entered = true; }
        enteredCondition.notify_all();
        std::unique_lock lock(session->mutex);
        session->idle.wait(lock, [&] { return session->pending.load() == 0; });
        if (session->owns) { ++session->stops; session->owns = false; }
        finished.store(true);
        enteredCondition.notify_all();
        return true;
    });
    { std::unique_lock lock(enteredMutex); enteredCondition.wait(lock, [&] { return entered; }); }
    assert(!finished.load());
    acceptedStart();
    assert(session->pending.load() == 1);
    acceptedStart = {};
    { std::unique_lock lock(enteredMutex); enteredCondition.wait(lock, [&] { return finished.load(); }); }
    while (!retired->complete()) std::this_thread::yield();
    assert(session->stops == 1 && !session->owns && actualWorker.pending() == 0);
    actualWorker.cancelClosing();
    assert(session->stops == 1 && !session->owns);
    std::cout << "PASS: admission, retained launch failure, serial cleanup, explicit STOP retry, capture release, cancel semantics\n";
}
