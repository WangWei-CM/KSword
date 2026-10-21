#include <QCoreApplication>
#include <QThreadPool>
#include <QRunnable>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
__TYPES__
__HELPER__
struct Tracked
{
    inline static std::atomic<unsigned> disposed{0};
    inline static std::atomic<unsigned> disposedOnCaller{0};
    inline static std::thread::id caller;
    ~Tracked()
    {
        disposed.fetch_add(1);
        if (std::this_thread::get_id() == caller) disposedOnCaller.fetch_add(1);
    }
};
using Clock=std::chrono::steady_clock;
double milliseconds(const Clock::time_point begin)
{
    return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
}
void fill(std::deque<ProcessActivitySample>& history)
{
    for (unsigned index=0;index<2000;++index)
    {
        ProcessActivitySample sample;
        sample.sequence=index;
        sample.processes.resize(200);
        for (auto& point:sample.processes)
        {
            point.identityKey="synthetic-process-identity-with-a-non-SSO-string";
            point.imagePath="C:/synthetic/fixture-only/process.exe";
        }
        history.push_back(std::move(sample));
    }
}
int main(int argc,char** argv)
{
    QCoreApplication application(argc,argv);
    Tracked::caller=std::this_thread::get_id();
    std::deque<Tracked> tracked(10000);
    retireProcessActivityHistory(tracked);
    assert(tracked.empty());
    for (unsigned index=0;index<100;++index) retireProcessActivityHistory(tracked);
    assert(QThreadPool::globalInstance()->waitForDone(30000));
    assert(Tracked::disposed==10000 && Tracked::disposedOnCaller==0);
    std::deque<ProcessActivitySample> history;
    fill(history);
    auto begin=Clock::now();history.clear();const double baseline=milliseconds(begin);
    fill(history);
    begin=Clock::now();retireProcessActivityHistory(history);const double handoff=milliseconds(begin);
    assert(history.empty());
    ProcessActivitySample next;next.sequence=0;history.push_back(next);
    assert(QThreadPool::globalInstance()->waitForDone(30000));
    assert(history.size()==1 && history.front().sequence==0);
    std::cout<<"{\"samples\":2000,\"processes_per_sample\":200,\"baseline_sync_clear_ms\":"<<baseline
        <<",\"retire_main_ms\":"<<handoff<<"}\n";
    retireProcessActivityHistory(history);
    assert(QThreadPool::globalInstance()->waitForDone(30000));
    std::cout<<"PASS: real sample declarations, worker-only destruction, empty repeat, new generation identity\n";
}
