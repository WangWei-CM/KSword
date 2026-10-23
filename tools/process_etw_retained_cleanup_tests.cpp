#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtCore/QThreadPool>
#include <QtCore/QRunnable>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include "__LIFECYCLE_HEADER__"
#include "__RETAINED_HEADER__"
static std::size_t checks = 0;
static void check(bool value) { ++checks; if (!value) { std::cerr << "failed assertion " << checks << '\n'; std::abort(); } }
static std::atomic<int> invalid{0}, destroyedOwned{0};
namespace ks::network
{
    struct Health { bool cleanupPending = false; };
    class ProcessNetworkEtwMonitor
    {
    public:
        std::atomic<bool> owned{false}, startSucceeds{true}, forcePending{false};
        std::atomic<int> failStops{0}, starts{0}, stops{0}, prunes{0}, operations{0}, delayMs{0};
        ~ProcessNetworkEtwMonitor() { if (owned) ++destroyedOwned; }
        bool Start()
        {
            if (++operations != 1 || owned) ++invalid;
            ++starts; owned = true;
            --operations;
            return startSucceeds;
        }
        bool StopChecked()
        {
            if (++operations != 1) ++invalid;
            ++stops; QThread::msleep(static_cast<unsigned long>(delayMs.load()));
            const bool fail = failStops.load() > 0;
            if (fail) --failStops; else owned = false;
            --operations;
            return !fail;
        }
        Health SnapshotHealth() const { return {owned.load() || forcePending.load()}; }
        void PruneCounters(const std::initializer_list<int>) { if (owned) ++invalid; ++prunes; }
    };
}
namespace ks::process
{
    class ProcessCpuCoreEtwMonitor
    {
    public:
        std::atomic<bool> owned{false}; std::atomic<int> starts{0}, stops{0}, drains{0}, delayMs{0}, failStops{0};
        ~ProcessCpuCoreEtwMonitor() { if (owned) ++destroyedOwned; }
        bool Start() { ++starts; owned = true; return true; }
        bool StopChecked()
        {
            ++stops; QThread::msleep(static_cast<unsigned long>(delayMs.load()));
            if(failStops>0) { --failStops;return false; }
            owned=false;return true;
        }
        ks::network::Health SnapshotHealth() const { return {owned.load()}; }
        int SnapshotAndReset() { if (owned) ++invalid; ++drains; return 0; }
    };
}
__HELPERS__
static bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 10000)
{
    QElapsedTimer clock; clock.start();
    while (!predicate())
    {
        if (clock.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    QCoreApplication::processEvents(); return true;
}
template<class Service> static void request(const std::shared_ptr<ks::process::ProcessEtwCaptureLifecycle>& life, const std::shared_ptr<Service>& service, bool desired)
{
    requestProcessEtwCapture(life,service,desired,[](bool,const Service&){});
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv); QThreadPool::globalInstance()->setMaxThreadCount(4);
    auto& registry=ks::ui::AsyncCleanupRegistry::global(); registry.cancelShutdown();
    std::uint64_t heartbeat=0; QTimer heart; QObject::connect(&heart,&QTimer::timeout,[&](){++heartbeat;}); heart.start(1);
    const auto baseline=registry.pending();
    // Ordinary failed Stop remains owned/pending and cannot spin/drain or report active.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>();
        auto job=makeProcessEtwCleanupOwner(life,service); check(job!=nullptr && registry.pending()==baseline+1);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        service->failStops=1; request(life,service,false);
        check(waitUntil([&](){return life->CleanupPending();}));
        QThread::msleep(20); check(service->stops==1 && service->prunes==0 && service->owned);
        check(!life->IsSettledActive()); bool consumed=false;
        check(!life->SnapshotIfCurrent(life->Generation(),[&](){consumed=true;}) && !consumed);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        check(service->starts==2 && service->stops==2 && service->prunes==1);
        check(job->requestCleanup()); check(waitUntil([&](){return job->isComplete();}));
        check(!service->owned && !life->Request(true)); check(registry.pending()==baseline);
    }
    // Failed Start may retain resources; one initial cleanup attempt then explicit Retry.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>();
        service->startSucceeds=false; service->failStops=1;
        auto job=makeProcessEtwCleanupOwner(life,service); std::weak_ptr<ks::network::ProcessNetworkEtwMonitor> weak=service;
        request(life,service,true); check(waitUntil([&](){return life->CleanupPending();}));
        QThread::msleep(20); check(service->starts==1 && service->stops==0 && service->prunes==0);
        check(job->requestCleanup()); check(waitUntil([&](){return job->hasFailed();}));
        check(registry.pending()==baseline+1 && !job->isComplete() && service->owned);
        service.reset(); check(!weak.expired());
        check(!life->Reopen());
        job.reset(); // Dock wrapper gone: the global value-state still pins service + lease
        check(!weak.expired());
        ks::ui::RetryRetainedCleanupJobs();
        check(waitUntil([&](){return registry.pending()==baseline && weak.expired();}));
    }
    // Dock closes value state before publishing one retained attempt: no hidden second STOP.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        service->failStops=1; life->Close(); request(life,service,false);
        check(job->requestCleanup()); check(waitUntil([&](){return job->hasFailed();}));
        QThreadPool::globalInstance()->waitForDone();
        check(service->stops==1 && service->owned && registry.pending()==baseline+1);
        check(job->requestCleanup()); check(waitUntil([&](){return job->isComplete();}));
        check(service->stops==2 && !service->owned);
    }
    // Queue close before a selected Start can take the backend gate: no late restart.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        auto held=life->LockBackgroundOperation(); const auto generation=life->Generation();
        request(life,service,true); check(job->requestCleanup());
        check(waitUntil([&](){return life->Generation()>generation+1;}));
        held.unlock(); check(waitUntil([&](){return job->isComplete();}));
        QThreadPool::globalInstance()->waitForDone(); check(service->starts==0 && !service->owned);
    }
    // Global shutdown, cancellation, exact failed-lease retention and fresh admission.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        service->failStops=1; registry.beginShutdown();
        check(waitUntil([&](){return job->hasFailed();})); check(registry.pending()==baseline+1);
        auto denied=makeProcessEtwCleanupOwner(life,service); check(denied==nullptr);
        registry.cancelShutdown(); check(!life->Reopen());
        ks::ui::RetryRetainedCleanupJobs(); check(waitUntil([&](){return job->isComplete();}));
        check(life->Reopen()); auto fresh=makeProcessEtwCleanupOwner(life,service); check(fresh!=nullptr);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        check(fresh->requestCleanup()); check(waitUntil([&](){return fresh->isComplete();}));
    }
    // Accepted-but-unreturned admission may keep a late cleanup closure: never reuse that epoch.
    {
        auto oldLife=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto oldService=std::make_shared<ks::network::ProcessNetworkEtwMonitor>();
        auto hiddenReceipt=makeProcessEtwCleanupOwner(oldLife,oldService);
        std::weak_ptr<ks::network::ProcessNetworkEtwMonitor> oldWeak=oldService;
        oldLife->Close(); const auto nextGeneration=oldLife->Generation()+1;
        releaseUnadmittedProcessEtwValue(std::move(oldService));
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>(nextGeneration);
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        check(life->Generation()>oldLife->Generation());
        check(hiddenReceipt->requestCleanup()); check(waitUntil([&](){return hiddenReceipt->isComplete();}));
        check(waitUntil([&](){return oldWeak.expired();}));
        check(service->owned && service->stops==0 && life->IsSettledActive());
        check(job->requestCleanup()); check(waitUntil([&](){return job->isComplete();}));
    }
    // A contradictory health receipt cannot release the lease just because bool was true.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        service->forcePending=true; check(job->requestCleanup()); check(waitUntil([&](){return job->hasFailed();}));
        check(service->prunes==0 && registry.pending()==baseline+1);
        service->forcePending=false; ks::ui::RetryRetainedCleanupJobs(); check(waitUntil([&](){return job->isComplete();}));
        check(service->prunes==1);
    }
    // In-flight attempts reject duplicate Retry; slow joins stay off the event loop.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::process::ProcessCpuCoreEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        request(life,service,true); check(waitUntil([&](){return life->IsSettledActive();}));
        service->delayMs=120; const auto beat=heartbeat; check(job->requestCleanup());
        check(waitUntil([&](){return service->stops==1;}));
        for(int i=0;i<100;++i) check(!job->requestCleanup());
        check(waitUntil([&](){return job->isComplete();}));
        check(heartbeat>beat+10 && service->stops==1 && service->drains==1 && !service->owned);
    }
    // CPU checked failure follows the same strong-owner/lease and no-drain rules.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::process::ProcessCpuCoreEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        request(life,service,true);check(waitUntil([&](){return life->IsSettledActive();}));
        service->failStops=1;check(job->requestCleanup());check(waitUntil([&](){return job->hasFailed();}));
        check(service->owned&&service->drains==0&&registry.pending()==baseline+1&&life->CleanupPending());
        ks::ui::RetryRetainedCleanupJobs();check(waitUntil([&](){return job->isComplete();}));
        check(!service->owned&&service->drains==1);
    }
    // Many concurrent demand intents cannot overlap Start/Stop or retire a live owner.
    {
        auto life=std::make_shared<ks::process::ProcessEtwCaptureLifecycle>();
        auto service=std::make_shared<ks::network::ProcessNetworkEtwMonitor>(); auto job=makeProcessEtwCleanupOwner(life,service);
        std::thread first([&](){for(int i=0;i<5000;++i) request(life,service,(i%3)!=0);});
        std::thread second([&](){for(int i=0;i<5000;++i) request(life,service,(i%5)!=0);});
        first.join(); second.join(); check(job->requestCleanup());
        check(waitUntil([&](){return job->isComplete();})); QThreadPool::globalInstance()->waitForDone();
        check(!service->owned && !life->Request(true));
    }
    QThreadPool::globalInstance()->waitForDone(); check(registry.pending()==baseline && invalid==0 && destroyedOwned==0);
    std::cout<<"{\"result\":\"passed\",\"checks\":"<<checks<<",\"heartbeat\":"<<heartbeat<<",\"invalid_overlap_or_drain\":"<<invalid<<",\"owned_destructors\":"<<destroyedOwned<<"}\n";
}
