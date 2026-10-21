#include "process_cpu_etw_fake_api.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

static thread_local bool failNextAllocation=false;
void* operator new(std::size_t size)
{
    if(std::exchange(failNextAllocation,false)) throw std::bad_alloc();
    if(auto* value=std::malloc(size==0?1:size)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value,std::size_t) noexcept { std::free(value); }
namespace
{
    std::atomic<std::size_t> checks{0};
    void check(bool value) { ++checks; if(!value) { std::cerr<<"failed check "<<checks.load()<<'\n'; std::abort(); } }
    struct Backend
    {
        std::mutex mutex; std::condition_variable changed;
        bool session=false,consumer=false,endConsumer=false,consumerEntered=false;
        unsigned starts=0,stops=0,closes=0,failStops=0,failCloses=0,failStopsOnOpen=0,failClosesOnOpen=0;
        ULONG startError=ERROR_SUCCESS,openError=ERROR_SUCCESS,processError=ERROR_CANCELLED;
        bool threadAllocationFails=false,allocationFailsAfterStart=false;
        std::wstring identity;
    } backend;
    template<class Predicate> void eventually(Predicate predicate)
    {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!predicate())
        {
            if(std::chrono::steady_clock::now()>end) throw std::runtime_error("fake timeout");
            std::this_thread::yield();
        }
    }
    void reset()
    {
        std::lock_guard lock(backend.mutex);check(!backend.session&&!backend.consumer);
        backend.endConsumer=backend.consumerEntered=false;backend.failStops=backend.failCloses=backend.failStopsOnOpen=backend.failClosesOnOpen=0;
        backend.startError=backend.openError=ERROR_SUCCESS;backend.processError=ERROR_CANCELLED;
        backend.threadAllocationFails=backend.allocationFailsAfterStart=false;
    }
}
WORD WINAPI CpuFakeGetActiveProcessorGroupCount() { return 1; }
DWORD WINAPI CpuFakeGetActiveProcessorCount(WORD group) { check(group==0);return 2; }
DWORD WINAPI CpuFakeGetCurrentProcessId() { return 12345; }
VOID WINAPI CpuFakeGetSystemInfo(LPSYSTEM_INFO info) { info->dwNumberOfProcessors=2; }
BOOL WINAPI CpuFakeQueryPerformanceCounter(LARGE_INTEGER* value) { value->QuadPart=100000;return TRUE; }
DWORD WINAPI CpuFakeGetLastError() { std::lock_guard lock(backend.mutex);return backend.openError; }
HANDLE WINAPI CpuFakeOpenThread(DWORD,BOOL,DWORD) { return reinterpret_cast<HANDLE>(37); }
DWORD WINAPI CpuFakeGetProcessIdOfThread(HANDLE) { return 42; }
BOOL WINAPI CpuFakeCloseHandle(HANDLE value) { check(value==reinterpret_cast<HANDLE>(37));return TRUE; }
ULONG WINAPI CpuFakeStartTraceW(PTRACEHANDLE handle,LPCWSTR name,PEVENT_TRACE_PROPERTIES)
{
    std::lock_guard lock(backend.mutex);check(!backend.session);++backend.starts;
    if(backend.startError!=ERROR_SUCCESS) return backend.startError;
    backend.identity=name;backend.session=true;backend.endConsumer=false;backend.consumerEntered=false;*handle=17;
    if(backend.allocationFailsAfterStart) failNextAllocation=true;
    return ERROR_SUCCESS;
}
ULONG WINAPI CpuFakeControlTraceW(TRACEHANDLE handle,LPCWSTR name,PEVENT_TRACE_PROPERTIES properties,ULONG action)
{
    std::lock_guard lock(backend.mutex);check(action==EVENT_TRACE_CONTROL_STOP);
    check(std::wstring(name)==L"KSword.ProcessCpuCore.12345");
    if(backend.session) check((handle==0||handle==17)&&backend.identity==name);
    ++backend.stops;
    if(backend.failStops) { --backend.failStops;return ERROR_ACCESS_DENIED; }
    const bool existed=backend.session;backend.session=false;properties->EventsLost=7;backend.changed.notify_all();
    return existed?ERROR_SUCCESS:ERROR_WMI_INSTANCE_NOT_FOUND;
}
PROCESSTRACE_HANDLE WINAPI CpuFakeOpenTraceW(PEVENT_TRACE_LOGFILEW trace)
{
    std::lock_guard lock(backend.mutex);check(trace->Context!=nullptr);
    backend.failStops+=backend.failStopsOnOpen;backend.failCloses+=backend.failClosesOnOpen;
    if(backend.openError!=ERROR_SUCCESS) return INVALID_PROCESSTRACE_HANDLE;
    backend.consumer=true;
    if(backend.threadAllocationFails) failNextAllocation=true;
    return 23;
}
ULONG WINAPI CpuFakeCloseTrace(PROCESSTRACE_HANDLE handle)
{
    std::lock_guard lock(backend.mutex);check(handle==23);++backend.closes;
    if(backend.failCloses) { --backend.failCloses;return ERROR_GEN_FAILURE; }
    backend.consumer=false;backend.changed.notify_all();return ERROR_SUCCESS;
}
ULONG WINAPI CpuFakeProcessTrace(PROCESSTRACE_HANDLE* handle,ULONG count,LPFILETIME,LPFILETIME)
{
    std::unique_lock lock(backend.mutex);check(count==1&&*handle==23);backend.consumerEntered=true;backend.changed.notify_all();
    backend.changed.wait(lock,[]{return !backend.consumer||!backend.session||backend.endConsumer;});return backend.processError;
}
int main()
{
    using ks::process::ProcessCpuCoreEtwMonitor;
    {
        ProcessCpuCoreEtwMonitor monitor;check(monitor.Start());
        auto sample=monitor.SnapshotAndReset();check(sample.monitorRunning&&sample.processors.size()==2&&!sample.sampleReady);
        unsigned before=0;std::wstring identity;
        {std::lock_guard lock(backend.mutex);backend.failStops=2;before=backend.starts;identity=backend.identity;}
        check(!monitor.StopChecked());auto health=monitor.SnapshotHealth();
        check(!health.isRunning&&health.cleanupPending&&health.stopErrorKnown&&health.stopError==ERROR_ACCESS_DENIED);
        check(health.errorCodeKnown&&health.errorCode==ERROR_ACCESS_DENIED);
        check(!monitor.Start());
        {std::lock_guard lock(backend.mutex);check(backend.starts==before&&backend.identity==identity&&backend.session);}
        check(monitor.StopChecked());check(!monitor.SnapshotHealth().cleanupPending);
        check(monitor.Start());check(monitor.StopChecked());
    }
    reset();
    {
        // STOP and CloseTrace fail while the consumer is live: no blocking join or lost owner.
        auto monitor=std::make_shared<ProcessCpuCoreEtwMonitor>();check(monitor->Start());
        eventually([]{std::lock_guard lock(backend.mutex);return backend.consumerEntered;});
        {std::lock_guard lock(backend.mutex);backend.failStops=1;backend.failCloses=1;}
        std::function<bool()> retained=[monitor]{return monitor->StopChecked();};std::weak_ptr<ProcessCpuCoreEtwMonitor> weak=monitor;monitor.reset();
        const auto started=std::chrono::steady_clock::now();check(!retained());
        check(std::chrono::steady_clock::now()-started<std::chrono::seconds(1));check(!weak.expired());
        {std::lock_guard lock(backend.mutex);check(backend.session&&backend.consumer);}
        check(retained());retained={};check(weak.expired());
    }
    reset();
    {
        ProcessCpuCoreEtwMonitor monitor;check(monitor.Start());
        {std::lock_guard lock(backend.mutex);backend.failStops=2;backend.processError=ERROR_INVALID_DATA;backend.endConsumer=true;backend.changed.notify_all();}
        eventually([&]{return !monitor.IsRunning();});
        const auto health=monitor.SnapshotHealth();check(health.cleanupPending&&health.stopError==ERROR_ACCESS_DENIED);
        check(health.errorCodeKnown&&health.errorCode==ERROR_INVALID_DATA);
        unsigned starts=0;{std::lock_guard lock(backend.mutex);starts=backend.starts;}
        check(!monitor.Start());{std::lock_guard lock(backend.mutex);check(backend.starts==starts);}
        check(monitor.StopChecked());
    }
    reset();
    {
        // OpenTrace failure after accepted StartTrace: compensating STOP failure must survive.
        ProcessCpuCoreEtwMonitor monitor;
        {std::lock_guard lock(backend.mutex);backend.openError=ERROR_INVALID_PARAMETER;}
        // Skip the absence probe, then fail compensation by setting the fault in OpenTrace.
        {std::lock_guard lock(backend.mutex);backend.failStops=0;}
        check(!monitor.Start());check(!monitor.SnapshotHealth().cleanupPending);
    }
    reset();
    {
        // Fake OpenTrace injects resource faults after the orphan probe, not before StartTrace.
        ProcessCpuCoreEtwMonitor monitor;
        unsigned before=0;
        {std::lock_guard lock(backend.mutex);backend.openError=ERROR_INVALID_PARAMETER;backend.failStopsOnOpen=2;}
        check(!monitor.Start());const auto health=monitor.SnapshotHealth();
        check(health.cleanupPending&&health.stopError==ERROR_ACCESS_DENIED&&health.errorCodeKnown&&health.errorCode==ERROR_INVALID_PARAMETER);
        {std::lock_guard lock(backend.mutex);before=backend.starts;backend.failStopsOnOpen=0;}
        check(!monitor.Start());{std::lock_guard lock(backend.mutex);check(backend.starts==before&&backend.session);}
        check(monitor.StopChecked());
    }
    reset();
    {
        ProcessCpuCoreEtwMonitor monitor;
        {std::lock_guard lock(backend.mutex);backend.threadAllocationFails=true;backend.failStopsOnOpen=1;backend.failClosesOnOpen=1;}
        check(!monitor.Start());check(monitor.LastErrorText().find("failed to create CSwitch")!=std::string::npos);
        check(monitor.SnapshotHealth().cleanupPending&&monitor.SnapshotHealth().stopError==ERROR_ACCESS_DENIED);
        check(monitor.StopChecked());
    }
    reset();
    {
        ProcessCpuCoreEtwMonitor monitor;check(monitor.Start());
        {std::lock_guard lock(backend.mutex);backend.failCloses=3;}
        check(!monitor.StopChecked());check(monitor.SnapshotHealth().cleanupPending);
        eventually([&]{return monitor.StopChecked();});check(!monitor.SnapshotHealth().cleanupPending);
    }
    reset();
    {
        // The pre-existing private-name orphan target is also retained on rejected cleanup.
        ProcessCpuCoreEtwMonitor monitor;
        {std::lock_guard lock(backend.mutex);backend.session=true;backend.identity=L"KSword.ProcessCpuCore.12345";backend.failStops=2;}
        unsigned before=0;{std::lock_guard lock(backend.mutex);before=backend.starts;}
        check(!monitor.Start()&&monitor.SnapshotHealth().cleanupPending);
        check(!monitor.Start());{std::lock_guard lock(backend.mutex);check(backend.starts==before);}
        check(monitor.StopChecked());check(monitor.Start());check(monitor.StopChecked());
    }
    reset();
    {
        ProcessCpuCoreEtwMonitor monitor;
        {std::lock_guard lock(backend.mutex);backend.startError=ERROR_NOT_SUPPORTED;}
        check(!monitor.Start());auto health=monitor.SnapshotHealth();
        check(!health.cleanupPending&&health.errorCodeKnown&&health.errorCode==ERROR_NOT_SUPPORTED);check(monitor.StopChecked());
    }
    reset();
    {
        // Identity allocation faults happen before either orphan STOP or StartTrace.
        ProcessCpuCoreEtwMonitor monitor;
        unsigned starts=0,stops=0;
        {std::lock_guard lock(backend.mutex);starts=backend.starts;stops=backend.stops;}
        failNextAllocation=true;bool threw=false;
        try { (void)monitor.Start(); } catch(const std::bad_alloc&) { threw=true; }
        check(threw&&!failNextAllocation);
        {std::lock_guard lock(backend.mutex);check(backend.starts==starts&&backend.stops==stops&&!backend.session);}
        check(!monitor.SnapshotHealth().cleanupPending);check(monitor.StopChecked());
    }
    reset();
    {
        // First allocation after accepted StartTrace cannot lose the native identity.
        // The next allocation is now within the guarded thread setup, not identity publication.
        ProcessCpuCoreEtwMonitor monitor;
        {std::lock_guard lock(backend.mutex);backend.allocationFailsAfterStart=true;backend.failStopsOnOpen=1;backend.failClosesOnOpen=1;}
        bool escaped=false,started=true;
        try { started=monitor.Start(); } catch(...) { escaped=true; }
        check(!escaped&&!started&&!failNextAllocation);
        check(monitor.SnapshotHealth().cleanupPending&&monitor.SnapshotHealth().stopError==ERROR_ACCESS_DENIED);
        {std::lock_guard lock(backend.mutex);check(backend.session&&backend.identity==L"KSword.ProcessCpuCore.12345");}
        check(monitor.StopChecked());check(!monitor.SnapshotHealth().cleanupPending);
    }
    reset();std::cout<<"{\"result\":\"passed\",\"checks\":"<<checks.load()<<",\"scope\":\"actual CPU CPP forced fake WinAPI; no real ETW/queries\"}\n";
}
