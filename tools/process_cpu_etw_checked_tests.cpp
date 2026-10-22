#include "process_cpu_etw_fake_api.h"
#include <algorithm>
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
static thread_local DWORD fakeLastError=ERROR_SUCCESS;
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
        bool orphanPresent=false,orphanHasProcessHandle=false,orphanProcessHandleOpen=false,enumerationIncomplete=false;
        unsigned resourceStartFailures=0,enumerations=0,orphanQueries=0,orphanStops=0,orphanOpens=0,orphanWaits=0,orphanCloses=0;
        DWORD orphanOpenError=ERROR_INVALID_PARAMETER,orphanWaitResult=WAIT_OBJECT_0;
        std::wstring orphanName;
        GUID orphanGuid{};
        ULONG orphanMode=0,orphanFlags=0;
        unsigned queryChange=0;
    } backend;
    unsigned orphanScenarios=0;
    constexpr DWORD orphanPid=4000;
    constexpr TRACEHANDLE orphanHandle=91;
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
        check(!backend.orphanProcessHandleOpen);
        backend.orphanPresent=backend.orphanHasProcessHandle=backend.enumerationIncomplete=false;
        backend.resourceStartFailures=backend.enumerations=backend.orphanQueries=backend.orphanStops=0;
        backend.orphanOpens=backend.orphanWaits=backend.orphanCloses=backend.queryChange=0;
        backend.orphanOpenError=ERROR_INVALID_PARAMETER;backend.orphanWaitResult=WAIT_OBJECT_0;
        backend.orphanName.clear();backend.orphanGuid={};backend.orphanMode=backend.orphanFlags=0;
    }
    void prepareOrphan()
    {
        reset();std::lock_guard lock(backend.mutex);++orphanScenarios;
        backend.orphanPresent=true;backend.resourceStartFailures=1;
        backend.orphanName=L"KSword.ProcessCpuCore.4000";
        backend.orphanGuid={0xd4392f18U^orphanPid,0xb98b,0x4cc7,{0xa4,0x93,0x3f,0x14,0x5c,0xd1,0xd9,0x72}};
        backend.orphanMode=EVENT_TRACE_REAL_TIME_MODE|EVENT_TRACE_SYSTEM_LOGGER_MODE;
        backend.orphanFlags=EVENT_TRACE_FLAG_THREAD|EVENT_TRACE_FLAG_CSWITCH;
    }
    void fillOrphanProperties(PEVENT_TRACE_PROPERTIES properties,const bool queried)
    {
        check(properties->LoggerNameOffset<properties->Wnode.BufferSize);
        check(properties->Wnode.BufferSize-properties->LoggerNameOffset>=(backend.orphanName.size()+1)*sizeof(wchar_t));
        auto* name=reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(properties)+properties->LoggerNameOffset);
        std::copy(backend.orphanName.begin(),backend.orphanName.end(),name);name[backend.orphanName.size()]=L'\0';
        properties->Wnode.Guid=backend.orphanGuid;properties->Wnode.HistoricalContext=orphanHandle;
        properties->LogFileMode=backend.orphanMode;properties->EnableFlags=backend.orphanFlags;
        if(queried&&backend.queryChange==1) properties->Wnode.Guid.Data4[7]^=1;
        if(queried&&backend.queryChange==2) properties->Wnode.HistoricalContext=orphanHandle+1;
    }
}
WORD WINAPI CpuFakeGetActiveProcessorGroupCount() { return 1; }
DWORD WINAPI CpuFakeGetActiveProcessorCount(WORD group) { check(group==0);return 2; }
DWORD WINAPI CpuFakeGetCurrentProcessId() { return 12345; }
VOID WINAPI CpuFakeGetSystemInfo(LPSYSTEM_INFO info) { info->dwNumberOfProcessors=2; }
BOOL WINAPI CpuFakeQueryPerformanceCounter(LARGE_INTEGER* value) { value->QuadPart=100000;return TRUE; }
DWORD WINAPI CpuFakeGetLastError() { return fakeLastError; }
HANDLE WINAPI CpuFakeOpenThread(DWORD,BOOL,DWORD) { return reinterpret_cast<HANDLE>(37); }
DWORD WINAPI CpuFakeGetProcessIdOfThread(HANDLE) { return 42; }
BOOL WINAPI CpuFakeCloseHandle(HANDLE value)
{
    if(value==reinterpret_cast<HANDLE>(37)) return TRUE;
    std::lock_guard lock(backend.mutex);check(value==reinterpret_cast<HANDLE>(45)&&backend.orphanProcessHandleOpen);
    backend.orphanProcessHandleOpen=false;++backend.orphanCloses;return TRUE;
}
ULONG WINAPI CpuFakeQueryAllTracesW(PEVENT_TRACE_PROPERTIES* properties,ULONG capacity,PULONG count)
{
    std::lock_guard lock(backend.mutex);++backend.enumerations;check(capacity>=64);
    *count=backend.orphanPresent?1U:0U;
    if(backend.orphanPresent) fillOrphanProperties(properties[0],false);
    if(backend.enumerationIncomplete) { *count=capacity+1;return ERROR_MORE_DATA; }
    return ERROR_SUCCESS;
}
HANDLE WINAPI CpuFakeOpenProcess(DWORD rights,BOOL inherit,DWORD pid)
{
    std::lock_guard lock(backend.mutex);check(rights==SYNCHRONIZE&&inherit==FALSE&&pid==orphanPid);
    ++backend.orphanOpens;
    if(!backend.orphanHasProcessHandle) { fakeLastError=backend.orphanOpenError;return nullptr; }
    check(!backend.orphanProcessHandleOpen);backend.orphanProcessHandleOpen=true;return reinterpret_cast<HANDLE>(45);
}
DWORD WINAPI CpuFakeWaitForSingleObject(HANDLE process,DWORD timeout)
{
    std::lock_guard lock(backend.mutex);check(process==reinterpret_cast<HANDLE>(45)&&backend.orphanProcessHandleOpen&&timeout==0);
    ++backend.orphanWaits;return backend.orphanWaitResult;
}
ULONG WINAPI CpuFakeStartTraceW(PTRACEHANDLE handle,LPCWSTR name,PEVENT_TRACE_PROPERTIES)
{
    std::lock_guard lock(backend.mutex);check(!backend.session);++backend.starts;
    if(backend.resourceStartFailures) { --backend.resourceStartFailures;return ERROR_NO_SYSTEM_RESOURCES; }
    if(backend.startError!=ERROR_SUCCESS) return backend.startError;
    backend.identity=name;backend.session=true;backend.endConsumer=false;backend.consumerEntered=false;*handle=17;
    if(backend.allocationFailsAfterStart) failNextAllocation=true;
    return ERROR_SUCCESS;
}
ULONG WINAPI CpuFakeControlTraceW(TRACEHANDLE handle,LPCWSTR name,PEVENT_TRACE_PROPERTIES properties,ULONG action)
{
    std::lock_guard lock(backend.mutex);
    if(std::wstring(name)!=L"KSword.ProcessCpuCore.12345")
    {
        check(backend.orphanPresent&&handle==orphanHandle&&backend.orphanName==name);
        if(action==EVENT_TRACE_CONTROL_QUERY)
        {
            ++backend.orphanQueries;fillOrphanProperties(properties,true);return ERROR_SUCCESS;
        }
        check(action==EVENT_TRACE_CONTROL_STOP);
        if(backend.orphanHasProcessHandle) check(backend.orphanProcessHandleOpen);
        ++backend.orphanStops;backend.orphanPresent=false;return ERROR_SUCCESS;
    }
    check(action==EVENT_TRACE_CONTROL_STOP);
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
    if(backend.openError!=ERROR_SUCCESS) { fakeLastError=backend.openError;return INVALID_PROCESSTRACE_HANDLE; }
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
    for(const bool exitedHandle:{false,true})
    {
        // Both positive exit proofs: absent PID, or a signaled handle held through STOP.
        prepareOrphan();unsigned starts=0;
        {std::lock_guard lock(backend.mutex);backend.orphanHasProcessHandle=exitedHandle;starts=backend.starts;}
        ProcessCpuCoreEtwMonitor monitor;check(monitor.Start());
        {
            std::lock_guard lock(backend.mutex);
            check(backend.starts==starts+2&&backend.enumerations==1&&backend.orphanQueries==1);
            check(backend.orphanStops==1&&!backend.orphanPresent&&!backend.orphanProcessHandleOpen);
            check(backend.orphanOpens==(exitedHandle?1U:2U));
            check(backend.orphanWaits==(exitedHandle?1U:0U)&&backend.orphanCloses==(exitedHandle?1U:0U));
        }
        check(monitor.StopChecked());
    }
    {
        // Even a successful orphan STOP cannot hide continued external resource exhaustion.
        prepareOrphan();unsigned starts=0;
        {std::lock_guard lock(backend.mutex);backend.startError=ERROR_NO_SYSTEM_RESOURCES;starts=backend.starts;}
        ProcessCpuCoreEtwMonitor monitor;check(!monitor.Start());
        const auto health=monitor.SnapshotHealth();check(health.errorCodeKnown&&health.errorCode==ERROR_NO_SYSTEM_RESOURCES);
        {std::lock_guard lock(backend.mutex);check(backend.starts==starts+2&&backend.orphanStops==1&&backend.enumerations==1);}
        check(monitor.StopChecked());
    }
    for(unsigned variant=0;variant<8;++variant)
    {
        // Active/denied/unknown/failed wait, foreign name, wrong GUID/mode/flags: preserve.
        prepareOrphan();unsigned starts=0;
        {
            std::lock_guard lock(backend.mutex);starts=backend.starts;
            if(variant==0) { backend.orphanHasProcessHandle=true;backend.orphanWaitResult=WAIT_TIMEOUT; }
            if(variant==1) backend.orphanOpenError=ERROR_ACCESS_DENIED;
            if(variant==2) backend.orphanOpenError=ERROR_GEN_FAILURE;
            if(variant==3) { backend.orphanHasProcessHandle=true;backend.orphanWaitResult=WAIT_FAILED; }
            if(variant==4) backend.orphanName+=L".foreign";
            if(variant==5) backend.orphanGuid.Data4[7]^=1;
            if(variant==6) backend.orphanMode=EVENT_TRACE_REAL_TIME_MODE;
            if(variant==7) backend.orphanFlags=EVENT_TRACE_FLAG_THREAD;
        }
        ProcessCpuCoreEtwMonitor monitor;check(!monitor.Start());
        {
            std::lock_guard lock(backend.mutex);
            check(backend.starts==starts+1&&backend.orphanPresent&&backend.orphanStops==0&&backend.orphanQueries==0);
            check(backend.orphanOpens==(variant<4?1U:0U)&&!backend.orphanProcessHandleOpen);
        }
        check(monitor.StopChecked());
    }
    for(unsigned changed:{1U,2U})
    {
        // Enumeration is not authority to STOP after a changed QUERY GUID or handle.
        prepareOrphan();unsigned starts=0;
        {std::lock_guard lock(backend.mutex);backend.queryChange=changed;starts=backend.starts;}
        ProcessCpuCoreEtwMonitor monitor;check(!monitor.Start());
        {
            std::lock_guard lock(backend.mutex);
            check(backend.starts==starts+1&&backend.orphanPresent&&backend.orphanStops==0);
            check(backend.orphanQueries==1&&backend.orphanOpens==1);
        }
        check(monitor.StopChecked());
    }
    {
        // A partially filled list must not authorize cleanup, including after one resize.
        prepareOrphan();unsigned starts=0;
        {std::lock_guard lock(backend.mutex);backend.enumerationIncomplete=true;starts=backend.starts;}
        ProcessCpuCoreEtwMonitor monitor;check(!monitor.Start());
        {
            std::lock_guard lock(backend.mutex);
            check(backend.starts==starts+1&&backend.enumerations==2&&backend.orphanPresent);
            check(backend.orphanStops==0&&backend.orphanQueries==0&&backend.orphanOpens==0);
        }
        check(monitor.StopChecked());
    }
    reset();std::cout<<"{\"result\":\"passed\",\"checks\":"<<checks.load()<<",\"orphan_scenarios\":"<<orphanScenarios<<",\"scope\":\"actual CPU CPP forced fake WinAPI; no real ETW/queries\"}\n";
}
