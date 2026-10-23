#include "AsyncCleanupRegistry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace fake
{
    enum class Fault { None, ConstructStandard, ConstructOther, Full, SubmitStandard, SubmitOther };
    Fault fault = Fault::None;
    int constructors = 0;
    int submissions = 0;
    int constructorsWithoutRegistration = 0;
    int submissionsWithoutRegistration = 0;
    std::mutex mutex;
    std::vector<std::function<void()>> queued;
    std::vector<std::function<void()>> completed; // Deliberately retain completed callables.

    std::function<void()> take()
    {
        std::lock_guard lock(mutex);
        if (queued.empty()) return {};
        auto work = std::move(queued.front());
        queued.erase(queued.begin());
        return work;
    }
    int runAll()
    {
        int exceptions = 0;
        while (auto work = take())
        {
            try { work(); } catch (...) { ++exceptions; }
            std::lock_guard lock(mutex);
            completed.push_back(std::move(work));
        }
        return exceptions;
    }
}

namespace ksword::evidence
{
    // Only the queue admission/dispatch boundary is fake. The production
    // central function and production cleanup registry are extracted/used exact.
    class SerialTransactionQueue
    {
    public:
        explicit SerialTransactionQueue(std::size_t)
        {
            ++fake::constructors;
            if (ks::ui::AsyncCleanupRegistry::global().pending() == 0) ++fake::constructorsWithoutRegistration;
            if (fake::fault == fake::Fault::ConstructStandard) throw std::runtime_error("queue startup failed");
            if (fake::fault == fake::Fault::ConstructOther) throw 31;
        }
        bool trySubmit(std::function<void()> work)
        {
            ++fake::submissions;
            if (ks::ui::AsyncCleanupRegistry::global().pending() == 0) ++fake::submissionsWithoutRegistration;
            if (fake::fault == fake::Fault::Full) return false;
            if (fake::fault == fake::Fault::SubmitStandard) throw std::runtime_error("queue admission failed");
            if (fake::fault == fake::Fault::SubmitOther) throw 37;
            std::lock_guard lock(fake::mutex);
            fake::queued.push_back(std::move(work));
            return true;
        }
    };
}

namespace ksword::memory_backend
{
    std::recursive_mutex& ddmaTransactionMutex() { static std::recursive_mutex mutex; return mutex; }
    void captureDdmaSession() {}
    void mutableDdmaScratchFailures() {}
    void scratchReceiptMutex() {}
}

#include "DdmaCleanupSubmission.Extracted.h"

namespace
{
    using namespace std::chrono_literals;
    using Registry = ks::ui::AsyncCleanupRegistry;
    namespace backend = ksword::memory_backend;
    int checks = 0;
    int failureCount = 0;
    int slowHeartbeats = 0;

    void expect(const bool value, const char* message)
    {
        ++checks;
        if (!value) { ++failureCount; std::cerr << "FAIL " << message << '\n'; }
    }

    void startupFailures()
    {
        Registry::global().beginShutdown();
        expect(!backend::submitDdmaTransaction([] {}), "initial shutdown rejects before queue startup");
        expect(fake::constructors == 0 && fake::submissions == 0 && Registry::global().pending() == 0,
            "initial shutdown reaches neither queue constructor nor admission");
        Registry::global().cancelShutdown();
        int workCalls = 0;
        for (const auto fault : {fake::Fault::ConstructStandard, fake::Fault::ConstructOther})
        {
            fake::fault = fault;
            bool threw = false;
            try { (void)backend::submitDdmaTransaction([&] { ++workCalls; }); }
            catch (...) { threw = true; }
            expect(threw, "queue startup failure propagates before acceptance");
            expect(Registry::global().pending() == 0, "queue startup failure returns its registration exactly once");
            expect(fake::submissions == 0 && workCalls == 0, "queue startup failure executes no work or admission retry");
        }
        expect(fake::constructors == 2, "failureCount static startup can be retried by a later explicit submission");
    }

    void admissionFailures()
    {
        int workCalls = 0;
        for (const auto fault : {fake::Fault::Full, fake::Fault::SubmitStandard, fake::Fault::SubmitOther})
        {
            fake::fault = fault;
            const int priorSubmissions = fake::submissions;
            bool accepted = false;
            bool threw = false;
            try { accepted = backend::submitDdmaTransaction([&] { ++workCalls; }); }
            catch (...) { threw = true; }
            expect(!accepted, "full/throwing admission accepts no transaction");
            expect(threw == (fault != fake::Fault::Full), "only actual admission exceptions propagate");
            expect(fake::submissions == priorSubmissions + 1, "admission rejection never retries");
            expect(Registry::global().pending() == 0 && workCalls == 0,
                "full/throwing admission releases exactly one lease without execution");
            expect(fake::runAll() == 0, "rejected admission left no queued callable");
        }
        fake::fault = fake::Fault::None;
    }

    void ownerCloseAndWorkerExceptions()
    {
        int workCalls = 0;
        auto page = std::make_unique<QObject>();
        auto existingOwnerLease = Registry::global().acquire();
        expect(backend::submitDdmaTransaction([&] { ++workCalls; }), "owner-close transaction admitted");
        expect(Registry::global().pending() == 2, "central transaction has independent registration");
        page.reset();
        existingOwnerLease.reset();
        expect(Registry::global().pending() == 1 && workCalls == 0,
            "closing page cannot release the accepted central lease");
        expect(fake::runAll() == 0 && workCalls == 1, "accepted work survives owner closure");
        expect(Registry::global().pending() == 0, "finished work releases lease even when completed callable is retained");
        for (const bool standard : {true, false})
        {
            bool resourceEnded = false;
            bool endedWhileRegistered = false;
            struct ExceptionResources
            {
                bool& ended;
                bool& registered;
                ExceptionResources(bool& endedValue, bool& registeredValue)
                    : ended(endedValue), registered(registeredValue) {}
                ~ExceptionResources()
                {
                    registered = Registry::global().pending() != 0;
                    ended = true;
                }
            };
            auto resources = std::make_shared<ExceptionResources>(resourceEnded, endedWhileRegistered);
            expect(backend::submitDdmaTransaction([standard, resources] {
                if (standard) throw std::runtime_error("worker failed");
                throw 41;
            }), "throwing worker admitted");
            resources.reset();
            expect(Registry::global().pending() == 1, "throwing work remains registered before completion");
            expect(fake::runAll() == 1, "worker exception propagates to queue boundary once");
            expect(resourceEnded && endedWhileRegistered, "throwing work captures destruct before their registration ends");
            expect(Registry::global().pending() == 0, "worker unwind releases its central registration");
        }
    }

    void shutdownRejectionAndCancel()
    {
        int workCalls = 0;
        expect(backend::submitDdmaTransaction([&] { ++workCalls; }), "first queued transaction admitted");
        Registry::global().beginShutdown();
        expect(Registry::global().pending() == 1, "shutdown never drops an accepted queued transaction");
        const int previous = fake::submissions;
        expect(!backend::submitDdmaTransaction([&] { ++workCalls; }), "shutdown rejects new central admission");
        expect(fake::submissions == previous, "shutdown rejection occurs before touching queue");
        Registry::global().cancelShutdown();
        expect(Registry::global().pending() == 1, "cancel reopens admission without pretending old work finished");
        expect(backend::submitDdmaTransaction([&] { ++workCalls; }), "new submission is accepted after cancel");
        expect(Registry::global().pending() == 2, "cancel keeps both accepted registrations");
        expect(fake::runAll() == 0 && workCalls == 2, "cancel does not cancel or duplicate accepted work");
        expect(Registry::global().pending() == 0, "both transactions eventually drain");
        Registry::global().beginShutdown();
        Registry::global().beginShutdown();
        expect(Registry::global().pending() == 0, "repeated shutdown has no phantom accepted registration");
        Registry::global().cancelShutdown();
    }

    void slowTransactionExitOrdering()
    {
        Registry::global().cancelShutdown();
        fake::fault = fake::Fault::None;
        std::atomic<bool> started{false};
        std::atomic<bool> workEnded{false};
        std::atomic<bool> capturedResourcesEnded{false};
        std::atomic<bool> resourceWasRegistered{false};
        std::atomic<bool> workerDone{false};
        bool r0Stopped = false;
        bool appQuit = false;
        bool earlyStop = false;
        bool shutdownStarted = false;
        bool timeoutObserved = false;
        bool rejectedWhileWaiting = false;
        bool deadline = false;
        int pageDestroyed = 0;
        int unexpectedWork = 0;
        auto page = std::make_unique<QObject>();
        QObject::connect(page.get(), &QObject::destroyed, [&] { ++pageDestroyed; });
        struct Resources
        {
            std::atomic<bool>& ended;
            std::atomic<bool>& registered;
            Resources(std::atomic<bool>& endedValue, std::atomic<bool>& registeredValue)
                : ended(endedValue), registered(registeredValue) {}
            ~Resources()
            {
                registered.store(Registry::global().pending() != 0, std::memory_order_release);
                ended.store(true, std::memory_order_release);
            }
        };
        auto resources = std::make_shared<Resources>(capturedResourcesEnded, resourceWasRegistered);
        expect(backend::submitDdmaTransaction([&, resources] {
            started.store(true, std::memory_order_release);
            std::this_thread::sleep_for(200ms); // Value-only fake work; no driver/device.
            workEnded.store(true, std::memory_order_release);
        }), "slow transaction admitted");
        resources.reset();
        expect(Registry::global().pending() == 1, "slow accepted work is registered before worker starts");
        ks::ui::AsyncShutdownWait exitWait;
        QElapsedTimer clock;
        clock.start();
        QEventLoop loop;
        QTimer heartbeat;
        heartbeat.setTimerType(Qt::PreciseTimer);
        heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] {
            if (started.load(std::memory_order_acquire) && !workEnded.load(std::memory_order_acquire)) ++slowHeartbeats;
            if (!shutdownStarted && started.load(std::memory_order_acquire))
            {
                shutdownStarted = true;
                page.reset();
                Registry::global().beginShutdown();
                exitWait.start(clock.elapsed(), 20); // Timeout must never authorize R0 stop.
                const auto before = fake::submissions;
                rejectedWhileWaiting = !backend::submitDdmaTransaction([&] { ++unexpectedWork; })
                    && fake::submissions == before;
            }
            if (!shutdownStarted) return;
            const auto status = exitWait.poll(clock.elapsed(), Registry::global().pending());
            if (status == ks::ui::AsyncShutdownWait::Status::TimedOut) timeoutObserved = true;
            if (status == ks::ui::AsyncShutdownWait::Status::Ready)
            {
                earlyStop = !workEnded.load(std::memory_order_acquire)
                    || !capturedResourcesEnded.load(std::memory_order_acquire);
                r0Stopped = true; // Fake MainWindow's R0 stop spy, not a service call.
                appQuit = r0Stopped;
                loop.quit();
            }
        });
        QTimer::singleShot(3000, &loop, [&] { deadline = true; loop.quit(); });
        std::thread worker([&] { (void)fake::runAll(); workerDone.store(true, std::memory_order_release); });
        heartbeat.start();
        loop.exec();
        worker.join();
        expect(!deadline && workerDone.load(), "slow fake transaction completes under event loop");
        expect(slowHeartbeats >= 10, "QCore heartbeat remains responsive during 200ms transaction");
        expect(pageDestroyed == 1 && rejectedWhileWaiting && unexpectedWork == 0,
            "page close and shutdown rejection do not cancel or add accepted work");
        expect(timeoutObserved && !earlyStop, "shutdown timeout cannot authorize early R0 stop");
        expect(workEnded.load() && capturedResourcesEnded.load() && resourceWasRegistered.load(),
            "captured worker resources are destroyed before their registration is released");
        expect(r0Stopped && appQuit && Registry::global().pending() == 0,
            "fake R0 stop and quit occur only after accepted task resources finish");
        Registry::global().cancelShutdown();
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    startupFailures();
    admissionFailures();
    ownerCloseAndWorkerExceptions();
    shutdownRejectionAndCancel();
    slowTransactionExitOrdering();
    expect(fake::constructorsWithoutRegistration == 0 && fake::submissionsWithoutRegistration == 0,
        "registration always precedes queue construction and irreversible admission");
    std::cout << (failureCount == 0 ? "PASS" : "FAIL") << " DdmaCleanupLease " << checks
        << " checks; " << failureCount << " failures; heartbeat=" << slowHeartbeats << '\n';
    return failureCount == 0 ? 0 : 1;
}
