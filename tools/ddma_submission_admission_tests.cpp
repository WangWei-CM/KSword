#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QPromise>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace fake
{
    enum class Mode { Accept, Full, StandardException, NonstandardException };
    Mode mode = Mode::Accept;
    int admissionCalls = 0;
    std::vector<std::function<void()>> accepted;

    void reset(const Mode nextMode)
    {
        mode = nextMode;
        admissionCalls = 0;
        accepted.clear();
    }

    std::function<void()> takeAccepted()
    {
        if (accepted.size() != 1) { throw std::logic_error("expected one accepted task"); }
        auto task = std::move(accepted.front());
        accepted.clear();
        return task;
    }
}

// Only the submit boundary is fake. Production templates below retain their
// actual QPromise/QFutureWatcher/QObject ownership and exception handling.
namespace ksword::memory_backend
{
    bool submitDdmaTransaction(std::function<void()> transaction)
    {
        ++fake::admissionCalls;
        switch (fake::mode)
        {
        case fake::Mode::Full: return false;
        case fake::Mode::StandardException: throw std::runtime_error("fake admission std exception");
        case fake::Mode::NonstandardException: throw 23;
        case fake::Mode::Accept:
            fake::accepted.push_back(std::move(transaction));
            return true;
        }
        return false;
    }
}

#include "DdmaSubmitPageTransaction.Extracted.h"

namespace
{
    using namespace std::chrono_literals;
    int checks = 0;
    int failures = 0;

    struct Payload
    {
        int answer = 0;
    };
    using TaskResult = DdmaTaskResult<Payload>;
    using Watcher = QFutureWatcher<TaskResult>;

    void expect(const bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL " << message << '\n';
        }
    }

    void drainEvents()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    template<class Predicate>
    bool until(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
        {
            drainEvents();
            std::this_thread::yield();
        }
        return predicate();
    }

    Watcher* watcherFor(QObject& receiver)
    {
        const auto children = receiver.children();
        expect(children.size() == 1, "exactly one watcher is created");
        return children.isEmpty() ? nullptr : dynamic_cast<Watcher*>(children.front());
    }

    void rejectedBeforeAdmission(const fake::Mode mode)
    {
        fake::reset(mode);
        QObject receiver;
        int workCalls = 0;
        int applyCalls = 0;
        int finishedSignals = 0;
        TaskResult applied;
        bool escaped = false;
        try
        {
            submitPageTransaction<Payload>(&receiver, [&] { ++workCalls; return Payload{91}; },
                [&](const TaskResult& result) { ++applyCalls; applied = result; });
        }
        catch (...)
        {
            escaped = true;
        }
        expect(!escaped, "submission exception never escapes into GUI caller");
        expect(fake::admissionCalls == 1 && fake::accepted.empty() && workCalls == 0,
            "rejected admission is not retried and never executes work");
        auto* watcher = watcherFor(receiver);
        expect(watcher != nullptr, "rejected submission still owns real future watcher");
        if (!watcher) { return; }
        const auto future = watcher->future();
        QObject::connect(watcher, &Watcher::finished, &receiver, [&] { ++finishedSignals; });
        expect(until([&] { return applyCalls == 1 && finishedSignals == 1; }),
            "rejected submission delivers one failed result and finished signal");
        expect(future.isFinished() && !future.isCanceled() && future.resultCount() == 1,
            "rejected future is explicitly finished with exactly one result");
        expect(!applied.failure.isEmpty() && applied.value.answer == 0,
            "rejected result cannot look successful");
        if (mode == fake::Mode::StandardException)
        {
            expect(applied.failure == QStringLiteral("fake admission std exception"),
                "standard admission exception is retained");
        }
        if (mode == fake::Mode::NonstandardException)
        {
            expect(applied.failure == QStringLiteral("DDMA transaction failed unexpectedly."),
                "nonstandard admission exception becomes explicit failure");
        }
        if (mode == fake::Mode::Full)
        {
            expect(applied.failure.contains(QStringLiteral("queue is full")),
                "full queue has its explicit rejection result");
        }
        for (int i = 0; i < 4; ++i) { drainEvents(); }
        expect(applyCalls == 1 && finishedSignals == 1 && fake::admissionCalls == 1 && workCalls == 0,
            "rejected completion has no later duplicate result, finish signal, or retry");
    }

    void acceptedCompletion(const int workerException)
    {
        fake::reset(fake::Mode::Accept);
        QObject receiver;
        int workCalls = 0;
        int applyCalls = 0;
        int finishedSignals = 0;
        TaskResult applied;
        std::thread::id workThread;
        std::thread::id applyThread;
        submitPageTransaction<Payload>(&receiver, [&]() -> Payload {
            ++workCalls;
            workThread = std::this_thread::get_id();
            if (workerException == 1) { throw std::runtime_error("fake worker std exception"); }
            if (workerException == 2) { throw 31; }
            return Payload{91};
        }, [&](const TaskResult& result) {
            ++applyCalls;
            applied = result;
            applyThread = std::this_thread::get_id();
        });
        auto* watcher = watcherFor(receiver);
        expect(watcher != nullptr, "accepted submission owns actual future watcher");
        if (!watcher) { return; }
        const auto future = watcher->future();
        QObject::connect(watcher, &Watcher::finished, &receiver, [&] { ++finishedSignals; });
        expect(fake::admissionCalls == 1 && fake::accepted.size() == 1
            && applyCalls == 0 && !future.isFinished(), "accepted task stays pending until actual work completes");
        auto task = fake::takeAccepted();
        std::exception_ptr escaped;
        std::thread worker([task = std::move(task), &escaped]() mutable {
            try { task(); }
            catch (...) { escaped = std::current_exception(); }
        });
        worker.join();
        expect(!escaped && workCalls == 1, "worker exceptions do not escape promise task wrapper");
        expect(until([&] { return applyCalls == 1 && finishedSignals == 1; }),
            "accepted completion delivers exactly once");
        expect(future.isFinished() && !future.isCanceled() && future.resultCount() == 1,
            "accepted task explicitly finishes real future with one result");
        expect(workThread != std::this_thread::get_id() && applyThread == std::this_thread::get_id(),
            "work runs on worker and apply runs on QObject receiver thread");
        if (workerException == 0)
        {
            expect(applied.failure.isEmpty() && applied.value.answer == 91, "accepted successful result is preserved");
        }
        else
        {
            expect(!applied.failure.isEmpty() && applied.value.answer == 0, "worker exception cannot become successful result");
            if (workerException == 1)
            {
                expect(applied.failure == QStringLiteral("fake worker std exception"), "standard worker exception is retained");
            }
        }
        for (int i = 0; i < 4; ++i) { drainEvents(); }
        expect(applyCalls == 1 && finishedSignals == 1 && fake::admissionCalls == 1 && workCalls == 1,
            "accepted completion has no duplicate delivery or admission retry");
    }

    void receiverDestroyedBeforeCompletion()
    {
        fake::reset(fake::Mode::Accept);
        auto receiver = std::make_unique<QObject>();
        std::atomic<int> workCalls{0};
        int applyCalls = 0;
        int watcherDestroyed = 0;
        auto dependency = std::make_shared<int>(91);
        std::weak_ptr<int> retainedDependency = dependency;
        auto started = std::make_shared<std::promise<void>>();
        auto startedFuture = started->get_future();
        auto release = std::make_shared<std::promise<void>>();
        auto released = release->get_future().share();
        submitPageTransaction<Payload>(receiver.get(), [dependency, started, released, &workCalls] {
            ++workCalls;
            started->set_value();
            released.wait();
            return Payload{*dependency};
        }, [&](const TaskResult&) { ++applyCalls; });
        auto* watcher = watcherFor(*receiver);
        expect(watcher != nullptr, "receiver lifetime test owns actual watcher");
        if (!watcher) { return; }
        const auto future = watcher->future();
        QObject::connect(watcher, &QObject::destroyed, [&] { ++watcherDestroyed; });
        dependency.reset();
        auto task = fake::takeAccepted();
        std::exception_ptr escaped;
        std::thread worker([task = std::move(task), &escaped]() mutable {
            try { task(); }
            catch (...) { escaped = std::current_exception(); }
        });
        const bool taskStarted = startedFuture.wait_for(2s) == std::future_status::ready;
        receiver.reset();
        const bool watcherRemovedBeforeFinish = watcherDestroyed == 1 && !future.isFinished();
        const bool retainedWhileRunning = !retainedDependency.expired();
        release->set_value();
        worker.join();
        for (int i = 0; i < 4; ++i) { drainEvents(); }
        expect(taskStarted && watcherRemovedBeforeFinish,
            "destroying receiver removes watcher while accepted task still runs");
        expect(retainedWhileRunning && retainedDependency.expired(),
            "accepted work retains its dependency until worker completion independently of receiver");
        expect(!escaped && workCalls.load() == 1 && fake::admissionCalls == 1,
            "receiver destruction neither cancels accepted work nor retries admission");
        expect(applyCalls == 0 && watcherDestroyed == 1,
            "destroyed receiver is never called by completion");
        expect(future.isFinished() && !future.isCanceled() && future.resultCount() == 1
            && future.result().value.answer == 91 && future.result().failure.isEmpty(),
            "real promise finishes with retained result after watcher destruction");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    rejectedBeforeAdmission(fake::Mode::StandardException);
    rejectedBeforeAdmission(fake::Mode::NonstandardException);
    rejectedBeforeAdmission(fake::Mode::Full);
    acceptedCompletion(0);
    acceptedCompletion(1);
    acceptedCompletion(2);
    receiverDestroyedBeforeCompletion();
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " DdmaSubmissionAdmission "
        << checks << " checks; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
