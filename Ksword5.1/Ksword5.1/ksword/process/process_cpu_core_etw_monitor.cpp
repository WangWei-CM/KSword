#include "process_cpu_core_etw_monitor.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <string_view>
#include <utility>

#pragma comment(lib, "Advapi32.lib")

namespace
{
    // Microsoft-Windows-Kernel-Thread classic provider (Thread_V2)。
    constexpr GUID KernelThreadProviderGuid{
        0x3d6fa8d1,
        0xfe05,
        0x11d0,
        { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c }
    };

    // 独立 System Logger 会话 GUID 基值；不得复用 SystemTraceControlGuid。
    constexpr GUID KswordCpuCoreSessionGuidBase{
        0xd4392f18,
        0xb98b,
        0x4cc7,
        { 0xa4, 0x93, 0x3f, 0x14, 0x5c, 0xd1, 0xd9, 0x72 }
    };

    constexpr std::uint8_t ThreadStartOpcode = 1;
    constexpr std::uint8_t ThreadEndOpcode = 2;
    constexpr std::uint8_t ThreadDataCollectionStartOpcode = 3;
    constexpr std::uint8_t ThreadDataCollectionEndOpcode = 4;
    constexpr std::uint8_t ContextSwitchOpcode = 36;
    constexpr std::size_t EtwSessionNameCapacity = 128;

    struct TracePropertiesBlock
    {
        EVENT_TRACE_PROPERTIES properties{};
        wchar_t loggerName[EtwSessionNameCapacity]{};
    };

    GUID buildSessionGuid(const DWORD processId)
    {
        GUID sessionGuid = KswordCpuCoreSessionGuidBase;
        sessionGuid.Data1 ^= static_cast<unsigned long>(processId);
        return sessionGuid;
    }

    GUID buildSessionGuid()
    {
        return buildSessionGuid(::GetCurrentProcessId());
    }

    // Query buffers must also accommodate unrelated sessions' maximum-length names.
    struct TraceQueryPropertiesBlock
    {
        EVENT_TRACE_PROPERTIES properties{};
        wchar_t loggerName[1025]{};
        wchar_t logFileName[1025]{};
    };

    void initializeTraceQueryProperties(TraceQueryPropertiesBlock& block)
    {
        block = TraceQueryPropertiesBlock{};
        block.properties.Wnode.BufferSize = sizeof(TraceQueryPropertiesBlock);
        block.properties.LoggerNameOffset = static_cast<ULONG>(offsetof(TraceQueryPropertiesBlock, loggerName));
        block.properties.LogFileNameOffset = static_cast<ULONG>(offsetof(TraceQueryPropertiesBlock, logFileName));
    }

    bool identifyKswordSystemLogger(const TraceQueryPropertiesBlock& block, DWORD& processId)
    {
        if (block.properties.LoggerNameOffset != offsetof(TraceQueryPropertiesBlock, loggerName)
            || block.properties.LogFileMode != (EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE)
            || block.properties.EnableFlags != (EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_CSWITCH)
            || block.properties.Wnode.HistoricalContext == 0)
        {
            return false;
        }
        std::size_t length = 0;
        while (length < 1025 && block.loggerName[length] != L'\0') { ++length; }
        if (length == 1025) { return false; }
        const std::wstring_view name(block.loggerName, length);
        constexpr std::wstring_view prefix = L"KSword.ProcessCpuCore.";
        if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix
            || name[prefix.size()] < L'1' || name[prefix.size()] > L'9')
        {
            return false;
        }
        DWORD ownerPid = 0;
        for (std::size_t i = prefix.size(); i < name.size(); ++i)
        {
            if (name[i] < L'0' || name[i] > L'9') { return false; }
            const DWORD digit = static_cast<DWORD>(name[i] - L'0');
            if (ownerPid > (MAXDWORD - digit) / 10U) { return false; }
            ownerPid = ownerPid * 10U + digit;
        }
        if (ownerPid == ::GetCurrentProcessId()
            || !::IsEqualGUID(block.properties.Wnode.Guid, buildSessionGuid(ownerPid)))
        {
            return false;
        }
        processId = ownerPid;
        return true;
    }

    bool openExitedProcess(const DWORD processId, HANDLE& process)
    {
        process = ::OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (process == nullptr)
        {
            // Only a nonexistent nonzero PID proves absence; access denied and
            // every other error leave that session alone.
            return processId != 0 && ::GetLastError() == ERROR_INVALID_PARAMETER;
        }
        return ::WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    }

    bool stopExitedKswordSystemLoggers()
    {
        try
        {
            std::vector<TraceQueryPropertiesBlock> blocks(64);
            std::vector<PEVENT_TRACE_PROPERTIES> properties(blocks.size());
            const auto prepare = [&]() {
                for (std::size_t i = 0; i < blocks.size(); ++i)
                {
                    initializeTraceQueryProperties(blocks[i]);
                    properties[i] = &blocks[i].properties;
                }
            };
            prepare();
            ULONG count = 0;
            ULONG status = ::QueryAllTracesW(properties.data(), static_cast<ULONG>(properties.size()), &count);
            // One bounded retry covers the documented configurable system cap.
            if (status == ERROR_MORE_DATA && count > blocks.size() && count <= 256U)
            {
                blocks.resize(count);
                properties.resize(count);
                prepare();
                status = ::QueryAllTracesW(properties.data(), static_cast<ULONG>(properties.size()), &count);
            }
            if (status != ERROR_SUCCESS || count > blocks.size()) { return false; }

            bool stopped = false;
            for (ULONG i = 0; i < count; ++i)
            {
                const TraceQueryPropertiesBlock& candidate = blocks[i];
                DWORD ownerPid = 0;
                if (!identifyKswordSystemLogger(candidate, ownerPid)) { continue; }
                HANDLE ownerProcess = nullptr;
                const bool exited = openExitedProcess(ownerPid, ownerProcess);
                if (exited)
                {
                    // Re-query the exact enumerated session before STOP and
                    // reject any observed identity or session-handle change.
                    const TRACEHANDLE handle = candidate.properties.Wnode.HistoricalContext;
                    TraceQueryPropertiesBlock current;
                    initializeTraceQueryProperties(current);
                    current.properties.Wnode.Guid = candidate.properties.Wnode.Guid;
                    DWORD currentPid = 0;
                    const ULONG queryStatus = ::ControlTraceW(handle, candidate.loggerName,
                        &current.properties, EVENT_TRACE_CONTROL_QUERY);
                    if (queryStatus == ERROR_SUCCESS && identifyKswordSystemLogger(current, currentPid)
                        && currentPid == ownerPid && current.properties.Wnode.HistoricalContext == handle
                        // A retained exited-process handle prevents PID reuse;
                        // absent PIDs must be checked again after the query.
                        && (ownerProcess != nullptr || openExitedProcess(ownerPid, ownerProcess)))
                    {
                        current.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
                        current.properties.LogFileNameOffset = 0;
                        const ULONG stopStatus = ::ControlTraceW(handle, candidate.loggerName,
                            &current.properties, EVENT_TRACE_CONTROL_STOP);
                        stopped = stopped || stopStatus == ERROR_SUCCESS;
                    }
                }
                if (ownerProcess != nullptr) { ::CloseHandle(ownerProcess); }
            }
            return stopped;
        }
        catch (const std::bad_alloc&)
        {
            // Keep the original StartTrace error when recovery cannot allocate.
            return false;
        }
    }

    std::vector<ks::process::EtwLogicalProcessorCoordinate> enumerateActiveProcessors()
    {
        std::vector<ks::process::EtwLogicalProcessorCoordinate> processors;
        const WORD groupCount = ::GetActiveProcessorGroupCount();
        for (WORD group = 0; group < groupCount; ++group)
        {
            const DWORD processorCount = ::GetActiveProcessorCount(group);
            for (DWORD number = 0; number < processorCount; ++number)
            {
                ks::process::EtwLogicalProcessorCoordinate coordinate;
                coordinate.processorIndex = static_cast<std::uint32_t>(processors.size());
                coordinate.group = static_cast<std::uint16_t>(group);
                coordinate.number = static_cast<std::uint16_t>(number);
                processors.push_back(coordinate);
            }
        }

        // 旧系统/受限环境兜底：至少保留一个槽位，避免后续空向量越界。
        if (processors.empty())
        {
            SYSTEM_INFO systemInfo{};
            ::GetSystemInfo(&systemInfo);
            const DWORD processorCount = std::max<DWORD>(1, systemInfo.dwNumberOfProcessors);
            for (DWORD number = 0; number < processorCount; ++number)
            {
                processors.push_back(ks::process::EtwLogicalProcessorCoordinate{
                    static_cast<std::uint32_t>(processors.size()),
                    0,
                    static_cast<std::uint16_t>(number) });
            }
        }
        return processors;
    }

    void initializeTraceProperties(
        TracePropertiesBlock* const block,
        const std::wstring& sessionName,
        const std::size_t processorCount)
    {
        if (block == nullptr)
        {
            return;
        }

        *block = TracePropertiesBlock{};
        block->properties.Wnode.BufferSize = sizeof(TracePropertiesBlock);
        block->properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        block->properties.Wnode.ClientContext = 1; // QPC 时间戳，供同一核心内做差。
        block->properties.Wnode.Guid = buildSessionGuid();
        block->properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        block->properties.EnableFlags = EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_CSWITCH;
        block->properties.BufferSize = 64; // KiB；CSwitch 是高频小事件，避免使用超大缓冲。
        block->properties.MinimumBuffers = 0; // 交由 ETW 按逻辑处理器数调整。
        block->properties.MaximumBuffers = static_cast<ULONG>(std::clamp<std::size_t>(
            processorCount * 4,
            64,
            1024));
        block->properties.FlushTimer = 1;
        block->properties.LoggerNameOffset = static_cast<ULONG>(offsetof(TracePropertiesBlock, loggerName));
        wcsncpy_s(block->loggerName, EtwSessionNameCapacity, sessionName.c_str(), _TRUNCATE);
    }

    std::wstring buildSessionName()
    {
        // PID 是会话所有权的一部分：名称稳定、可诊断，且不会和另一个存活进程冲突。
        return L"KSword.ProcessCpuCore." + std::to_wstring(::GetCurrentProcessId());
    }

    ULONG stopOwnedTraceSession(
        const TRACEHANDLE sessionHandle,
        const std::wstring& sessionName,
        const std::size_t processorCount,
        std::uint64_t* const eventsLostOut)
    {
        if (sessionName.empty())
        {
            return ERROR_SUCCESS;
        }

        TracePropertiesBlock propertiesBlock;
        initializeTraceProperties(&propertiesBlock, sessionName, processorCount);
        const ULONG stopStatus = ::ControlTraceW(
            sessionHandle,
            sessionName.c_str(),
            &propertiesBlock.properties,
            EVENT_TRACE_CONTROL_STOP);
        if (stopStatus == ERROR_SUCCESS && eventsLostOut != nullptr)
        {
            *eventsLostOut = static_cast<std::uint64_t>(propertiesBlock.properties.EventsLost);
        }
        return stopStatus;
    }

    std::string makeEtwErrorText(const char* const operation, const ULONG errorCode)
    {
        std::ostringstream stream;
        stream << operation << " failed (Win32 error " << errorCode << ")";
        if (errorCode == ERROR_ACCESS_DENIED)
        {
            stream << "; administrator or Performance Log Users permission is required";
        }
        return stream.str();
    }

    double calculateUsagePercent(
        const std::uint64_t runtimeTicks,
        const std::uint64_t observedTicks)
    {
        if (observedTicks == 0)
        {
            return 0.0;
        }
        return std::clamp(
            (static_cast<double>(runtimeTicks) / static_cast<double>(observedTicks)) * 100.0,
            0.0,
            100.0);
    }
} // namespace

namespace ks::process
{
    std::uint64_t BuildCpuThreadIdentity(
        const std::uint32_t processId,
        const std::uint32_t threadId)
    {
        return (static_cast<std::uint64_t>(processId) << 32U) |
            static_cast<std::uint64_t>(threadId);
    }

    ProcessCpuCoreEtwMonitor::ProcessCpuCoreEtwMonitor()
        : m_processors(enumerateActiveProcessors()),
          m_lastTimestampByProcessor(m_processors.size(), 0),
          m_currentThreadIdByProcessor(m_processors.size(), 0),
          m_currentThreadKnownByProcessor(m_processors.size(), false),
          m_observedTicksByProcessor(m_processors.size(), 0)
    {
    }

    ProcessCpuCoreEtwMonitor::~ProcessCpuCoreEtwMonitor()
    {
        Stop();
        // Compatibility fallback for legacy owners; checked callers retain this
        // value on failure, so their GUI path never enters this final join.
        if (m_consumerThread.joinable()) m_consumerThread.join();
    }

    bool ProcessCpuCoreEtwMonitor::Start()
    {
        std::lock_guard<std::mutex> lifecycleGuard(m_lifecycleMutex);
        if (m_running.load(std::memory_order_acquire))
        {
            return true;
        }

        if (!stopLocked()) return false; // never overwrite an incomplete cleanup identity
        setLastErrorText(std::string());
        m_bufferEventsLost.store(0, std::memory_order_relaxed);
        m_sessionEventsLost.store(0, std::memory_order_relaxed);
        m_stopRequested.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> counterGuard(m_counterMutex);
            m_processRuntimeByPid.clear();
            m_threadRuntimeByIdentity.clear();
            m_processIdByThreadId.clear();
            m_unresolvedThreadIds.clear();
            std::fill(m_observedTicksByProcessor.begin(), m_observedTicksByProcessor.end(), 0);
            resetProcessorBaselinesLocked();
            m_contextSwitchEvents = 0;
            m_lastObservedBufferLoss = 0;
            m_lastSnapshotEventsLost = 0;
        }

        const std::wstring sessionName = buildSessionName();
        // Freeze the only allocating identity publication before any native ownership.
        // After StartTrace succeeds, handle and receipt publication cannot throw.
        m_sessionName = sessionName;
        // 清理由同一 PID 上一次异常终止遗留的同名会话。当前实例若仍在运行已在
        // 函数入口返回，因此这里不会停止另一个存活进程拥有的会话。
        std::uint64_t ignoredOrphanLoss = 0;
        const ULONG orphanStopStatus = stopOwnedTraceSession(0, sessionName, m_processors.size(), &ignoredOrphanLoss);
        if (orphanStopStatus != ERROR_SUCCESS && orphanStopStatus != ERROR_WMI_INSTANCE_NOT_FOUND)
        {
            // The pre-frozen name preserves the same-PID private cleanup target.
            m_sessionReleased.store(false, std::memory_order_release);
            m_cleanupPending.store(true, std::memory_order_release);
            m_stopError.store(orphanStopStatus, std::memory_order_release);
            setLastErrorText(makeEtwErrorText("ControlTraceW(STOP)", orphanStopStatus), orphanStopStatus, true);
            return false;
        }
        m_stopError.store(ERROR_SUCCESS, std::memory_order_release);
        TracePropertiesBlock propertiesBlock;
        initializeTraceProperties(&propertiesBlock, sessionName, m_processors.size());

        TRACEHANDLE sessionHandle = 0;
        ULONG startStatus = ::StartTraceW(
            &sessionHandle,
            sessionName.c_str(),
            &propertiesBlock.properties);
        if (startStatus == ERROR_NO_SYSTEM_RESOURCES && stopExitedKswordSystemLoggers())
        {
            initializeTraceProperties(&propertiesBlock, sessionName, m_processors.size());
            sessionHandle = 0;
            startStatus = ::StartTraceW(&sessionHandle, sessionName.c_str(), &propertiesBlock.properties);
        }
        if (startStatus != ERROR_SUCCESS)
        {
            setLastErrorText(makeEtwErrorText("StartTraceW(CSwitch)", startStatus), startStatus, true);
            return false;
        }

        m_sessionHandle = sessionHandle;
        m_sessionReleased.store(false, std::memory_order_release);
        m_cleanupPending.store(true, std::memory_order_release);
        EVENT_TRACE_LOGFILEW traceLog{};
        traceLog.LoggerName = const_cast<wchar_t*>(m_sessionName.c_str());
        traceLog.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        traceLog.EventRecordCallback = &ProcessCpuCoreEtwMonitor::eventRecordCallback;
        traceLog.BufferCallback = &ProcessCpuCoreEtwMonitor::bufferCallback;
        traceLog.Context = this;

        const PROCESSTRACE_HANDLE consumerHandle = ::OpenTraceW(&traceLog);
        if (consumerHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            const ULONG openTraceError = ::GetLastError();
            (void)stopLocked();
            setLastErrorText(makeEtwErrorText("OpenTraceW(CSwitch)", openTraceError), openTraceError, true);
            return false;
        }

        m_consumerHandle.store(consumerHandle, std::memory_order_release);
        m_consumerFinished.store(false, std::memory_order_release);
        m_running.store(true, std::memory_order_release);
        try
        {
            m_consumerThread = std::thread(
                &ProcessCpuCoreEtwMonitor::consumeTrace,
                this,
                consumerHandle,
                sessionHandle,
                m_sessionName);
        }
        catch (...)
        {
            m_running.store(false, std::memory_order_release);
            m_consumerFinished.store(true, std::memory_order_release);
            (void)stopLocked();
            setLastErrorText("failed to create CSwitch ETW consumer thread");
            return false;
        }
        return true;
    }

    void ProcessCpuCoreEtwMonitor::Stop()
    {
        (void)StopChecked();
    }

    bool ProcessCpuCoreEtwMonitor::StopChecked()
    {
        std::lock_guard<std::mutex> lifecycleGuard(m_lifecycleMutex);
        return stopLocked();
    }

    bool ProcessCpuCoreEtwMonitor::closeOwnedConsumer()
    {
        const auto handle = m_consumerHandle.exchange(INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel);
        if (handle == INVALID_PROCESSTRACE_HANDLE) return true;
        const ULONG result = ::CloseTrace(handle);
        if (result == ERROR_SUCCESS || result == ERROR_CTX_CLOSE_PENDING || result == ERROR_INVALID_HANDLE) return true;
        auto empty = INVALID_PROCESSTRACE_HANDLE;
        (void)m_consumerHandle.compare_exchange_strong(empty, handle, std::memory_order_acq_rel);
        setLastErrorText(makeEtwErrorText("CloseTrace", result), result, true);
        return false;
    }

    bool ProcessCpuCoreEtwMonitor::stopLocked()
    {
        m_stopRequested.store(true, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        if (!m_sessionReleased.load(std::memory_order_acquire) && !m_sessionName.empty())
        {
            std::uint64_t sessionEventsLost = 0;
            const ULONG result = stopOwnedTraceSession(m_sessionHandle, m_sessionName, m_processors.size(), &sessionEventsLost);
            if (result == ERROR_SUCCESS || result == ERROR_WMI_INSTANCE_NOT_FOUND)
            {
                if (result == ERROR_SUCCESS) m_sessionEventsLost.store(sessionEventsLost, std::memory_order_relaxed);
                m_sessionReleased.store(true, std::memory_order_release);
                m_stopError.store(ERROR_SUCCESS, std::memory_order_release);
            }
            else
            {
                m_stopError.store(result, std::memory_order_release);
                setLastErrorText(makeEtwErrorText("ControlTraceW(STOP)", result), result, true);
            }
        }
        const bool consumerClosed = closeOwnedConsumer();
        if (!consumerClosed && !m_consumerFinished.load(std::memory_order_acquire)) return false;
        if (m_consumerThread.joinable()) m_consumerThread.join();
        const bool complete = m_sessionReleased.load(std::memory_order_acquire)
            && m_consumerHandle.load(std::memory_order_acquire) == INVALID_PROCESSTRACE_HANDLE;
        if (complete)
        {
            m_sessionHandle = 0;
            m_sessionName.clear();
            m_cleanupPending.store(false, std::memory_order_release);
        }
        return complete;
    }

    ProcessCpuCoreEtwHealth ProcessCpuCoreEtwMonitor::SnapshotHealth() const
    {
        ProcessCpuCoreEtwHealth health;
        health.isRunning = IsRunning();
        health.cleanupPending = m_cleanupPending.load(std::memory_order_acquire);
        health.stopError = m_stopError.load(std::memory_order_acquire);
        health.stopErrorKnown = health.stopError != ERROR_SUCCESS;
        std::lock_guard<std::mutex> errorGuard(m_errorMutex);
        health.errorCodeKnown = m_lastErrorCodeKnown;
        health.errorCode = m_lastErrorCode;
        health.errorText = m_lastErrorText;
        return health;
    }

    bool ProcessCpuCoreEtwMonitor::IsRunning() const
    {
        return m_running.load(std::memory_order_acquire);
    }

    std::string ProcessCpuCoreEtwMonitor::LastErrorText() const
    {
        std::lock_guard<std::mutex> errorGuard(m_errorMutex);
        return m_lastErrorText;
    }

    CpuCoreUsageSnapshot ProcessCpuCoreEtwMonitor::SnapshotAndReset()
    {
        CpuCoreUsageSnapshot snapshot;
        snapshot.monitorRunning = IsRunning();
        const std::uint64_t totalEventsLost = std::max(
            m_bufferEventsLost.load(std::memory_order_relaxed),
            m_sessionEventsLost.load(std::memory_order_relaxed));
        snapshot.diagnosticText = LastErrorText();

        std::lock_guard<std::mutex> counterGuard(m_counterMutex);
        snapshot.eventsLost = totalEventsLost >= m_lastSnapshotEventsLost
            ? totalEventsLost - m_lastSnapshotEventsLost
            : totalEventsLost;
        snapshot.dataLossDetected = snapshot.eventsLost != 0;
        m_lastSnapshotEventsLost = totalEventsLost;
        LARGE_INTEGER snapshotTimestamp{};
        if (::QueryPerformanceCounter(&snapshotTimestamp) != FALSE)
        {
            const std::uint64_t nowTicks = static_cast<std::uint64_t>(snapshotTimestamp.QuadPart);
            for (std::size_t processorIndex = 0;
                 processorIndex < m_processors.size();
                 ++processorIndex)
            {
                const std::uint64_t previousTimestamp =
                    m_lastTimestampByProcessor[processorIndex];
                if (!m_currentThreadKnownByProcessor[processorIndex] ||
                    previousTimestamp == 0 ||
                    nowTicks <= previousTimestamp)
                {
                    continue;
                }

                // CSwitch 只在调度切换点出现；快照时必须把“最后一次切换至今”的
                // 连续运行区间结算给当前线程，否则长时间不切换的满核线程会漏算。
                const std::uint64_t deltaTicks = nowTicks - previousTimestamp;
                m_observedTicksByProcessor[processorIndex] += deltaTicks;
                recordThreadRuntimeLocked(
                    processorIndex,
                    m_currentThreadIdByProcessor[processorIndex],
                    deltaTicks);
                m_lastTimestampByProcessor[processorIndex] = nowTicks;
            }
        }
        snapshot.processors = m_processors;
        snapshot.sampleReadyByProcessor.resize(m_processors.size(), false);
        for (std::size_t processorIndex = 0; processorIndex < m_processors.size(); ++processorIndex)
        {
            snapshot.sampleReadyByProcessor[processorIndex] =
                m_observedTicksByProcessor[processorIndex] != 0;
        }
        snapshot.contextSwitchEvents = m_contextSwitchEvents;
        snapshot.sampleReady = std::any_of(
            m_observedTicksByProcessor.cbegin(),
            m_observedTicksByProcessor.cend(),
            [](const std::uint64_t observedTicks) { return observedTicks != 0; });

        snapshot.liveThreadIdentities.reserve(m_processIdByThreadId.size());
        for (const auto& ownerPair : m_processIdByThreadId)
        {
            if (ownerPair.first != 0 && ownerPair.second != 0)
            {
                snapshot.liveThreadIdentities.push_back(
                    BuildCpuThreadIdentity(ownerPair.second, ownerPair.first));
            }
        }
        std::sort(snapshot.liveThreadIdentities.begin(), snapshot.liveThreadIdentities.end());

        const auto buildSeries = [this](
            const std::uint32_t processId,
            const std::uint32_t threadId,
            const RuntimeCounter& runtimeCounter) -> CpuCoreUsageSeries
        {
            CpuCoreUsageSeries series;
            series.processId = processId;
            series.threadId = threadId;
            series.percentByProcessor.resize(m_processors.size(), 0.0);
            series.sampleReadyByProcessor.resize(m_processors.size(), false);
            for (std::size_t processorIndex = 0;
                 processorIndex < m_processors.size();
                 ++processorIndex)
            {
                const std::uint64_t observedTicks = m_observedTicksByProcessor[processorIndex];
                series.sampleReadyByProcessor[processorIndex] = observedTicks != 0;
                const std::uint64_t runtimeTicks =
                    processorIndex < runtimeCounter.ticksByProcessor.size()
                    ? runtimeCounter.ticksByProcessor[processorIndex]
                    : 0;
                const double percent = calculateUsagePercent(runtimeTicks, observedTicks);
                series.percentByProcessor[processorIndex] = percent;
                series.coreEquivalentPercent += percent;
            }
            if (threadId != 0)
            {
                series.coreEquivalentPercent = std::clamp(series.coreEquivalentPercent, 0.0, 100.0);
            }
            return series;
        };

        snapshot.processUsageByPid.reserve(m_processRuntimeByPid.size());
        for (const auto& runtimePair : m_processRuntimeByPid)
        {
            snapshot.processUsageByPid.emplace(
                runtimePair.first,
                buildSeries(runtimePair.first, 0, runtimePair.second));
        }

        snapshot.threadUsageByIdentity.reserve(m_threadRuntimeByIdentity.size());
        for (const auto& runtimePair : m_threadRuntimeByIdentity)
        {
            const std::uint32_t processId = static_cast<std::uint32_t>(runtimePair.first >> 32U);
            const std::uint32_t threadId = static_cast<std::uint32_t>(runtimePair.first & 0xffffffffULL);
            snapshot.threadUsageByIdentity.emplace(
                runtimePair.first,
                buildSeries(processId, threadId, runtimePair.second));
        }

        m_processRuntimeByPid.clear();
        m_threadRuntimeByIdentity.clear();
        std::fill(m_observedTicksByProcessor.begin(), m_observedTicksByProcessor.end(), 0);
        m_contextSwitchEvents = 0;
        return snapshot;
    }

    void WINAPI ProcessCpuCoreEtwMonitor::eventRecordCallback(PEVENT_RECORD const eventRecord)
    {
        if (eventRecord == nullptr || eventRecord->UserContext == nullptr)
        {
            return;
        }
        static_cast<ProcessCpuCoreEtwMonitor*>(eventRecord->UserContext)->recordEvent(*eventRecord);
    }

    ULONG WINAPI ProcessCpuCoreEtwMonitor::bufferCallback(PEVENT_TRACE_LOGFILEW const logFile)
    {
        if (logFile == nullptr || logFile->Context == nullptr)
        {
            return TRUE;
        }

        auto* const monitor = static_cast<ProcessCpuCoreEtwMonitor*>(logFile->Context);
        if (logFile->EventsLost != 0)
        {
            monitor->m_bufferEventsLost.store(logFile->EventsLost, std::memory_order_relaxed);
            std::lock_guard<std::mutex> counterGuard(monitor->m_counterMutex);
            if (monitor->m_lastObservedBufferLoss != logFile->EventsLost)
            {
                // 丢事件后不能把跨缺口时长归给当前 OldThreadId；重建全部核心基线。
                monitor->resetProcessorBaselinesLocked();
                monitor->m_lastObservedBufferLoss = logFile->EventsLost;
            }
        }
        return monitor->m_stopRequested.load(std::memory_order_acquire) ? FALSE : TRUE;
    }

    void ProcessCpuCoreEtwMonitor::consumeTrace(
        const PROCESSTRACE_HANDLE consumerHandle,
        const TRACEHANDLE sessionHandle,
        std::wstring sessionName)
    {
        PROCESSTRACE_HANDLE activeConsumerHandle = consumerHandle;
        const ULONG processStatus = ::ProcessTrace(&activeConsumerHandle, 1, nullptr, nullptr);
        (void)closeOwnedConsumer();

        if (!m_stopRequested.load(std::memory_order_acquire) &&
            processStatus != ERROR_SUCCESS &&
            processStatus != ERROR_CANCELLED)
        {
            setLastErrorText(makeEtwErrorText("ProcessTrace(CSwitch)", processStatus), processStatus, true);
            std::uint64_t eventsLost = 0;
            const ULONG result = stopOwnedTraceSession(sessionHandle, sessionName, m_processors.size(), &eventsLost);
            if (result == ERROR_SUCCESS || result == ERROR_WMI_INSTANCE_NOT_FOUND)
            {
                if (result == ERROR_SUCCESS) m_sessionEventsLost.store(eventsLost, std::memory_order_relaxed);
                m_sessionReleased.store(true, std::memory_order_release);
            }
            else m_stopError.store(result, std::memory_order_release);
        }
        m_consumerFinished.store(true, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
    }

    void ProcessCpuCoreEtwMonitor::recordEvent(const EVENT_RECORD& eventRecord)
    {
        if (!::IsEqualGUID(eventRecord.EventHeader.ProviderId, KernelThreadProviderGuid))
        {
            return;
        }

        const std::uint8_t opcode = eventRecord.EventHeader.EventDescriptor.Opcode;
        if (opcode == ContextSwitchOpcode)
        {
            recordContextSwitchEvent(eventRecord);
            return;
        }
        if (opcode == ThreadStartOpcode ||
            opcode == ThreadEndOpcode ||
            opcode == ThreadDataCollectionStartOpcode ||
            opcode == ThreadDataCollectionEndOpcode)
        {
            recordThreadLifecycleEvent(eventRecord, opcode);
        }
    }

    void ProcessCpuCoreEtwMonitor::recordThreadLifecycleEvent(
        const EVENT_RECORD& eventRecord,
        const std::uint8_t opcode)
    {
        // Thread_V2 TypeGroup1 前两个 MOF 字段固定为 ProcessId、TThreadId；
        // 用 memcpy 避免 UserData 未对齐时直接解引用。
        if (eventRecord.UserData == nullptr || eventRecord.UserDataLength < sizeof(std::uint32_t) * 2)
        {
            return;
        }
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        std::memcpy(&processId, eventRecord.UserData, sizeof(processId));
        std::memcpy(
            &threadId,
            static_cast<const std::uint8_t*>(eventRecord.UserData) + sizeof(processId),
            sizeof(threadId));
        if (threadId == 0)
        {
            return;
        }

        std::lock_guard<std::mutex> counterGuard(m_counterMutex);
        if (opcode == ThreadStartOpcode || opcode == ThreadDataCollectionStartOpcode)
        {
            m_processIdByThreadId[threadId] = processId;
            m_unresolvedThreadIds.erase(threadId);
        }
        else
        {
            m_processIdByThreadId.erase(threadId);
            m_unresolvedThreadIds.erase(threadId);
        }
    }

    void ProcessCpuCoreEtwMonitor::recordContextSwitchEvent(const EVENT_RECORD& eventRecord)
    {
        // CSwitch 前两个字段固定为 NewThreadId、OldThreadId。
        if (eventRecord.UserData == nullptr || eventRecord.UserDataLength < sizeof(std::uint32_t) * 2)
        {
            return;
        }
        std::uint32_t newThreadId = 0;
        std::uint32_t oldThreadId = 0;
        std::memcpy(&newThreadId, eventRecord.UserData, sizeof(newThreadId));
        std::memcpy(
            &oldThreadId,
            static_cast<const std::uint8_t*>(eventRecord.UserData) + sizeof(newThreadId),
            sizeof(oldThreadId));

        const std::uint32_t processorIndex = ::GetEventProcessorIndex(&eventRecord);
        if (processorIndex >= m_processors.size())
        {
            return;
        }
        const std::uint64_t timestamp = static_cast<std::uint64_t>(eventRecord.EventHeader.TimeStamp.QuadPart);

        std::lock_guard<std::mutex> counterGuard(m_counterMutex);
        ++m_contextSwitchEvents;
        if (newThreadId != 0 &&
            m_processIdByThreadId.find(newThreadId) == m_processIdByThreadId.end())
        {
            (void)resolveThreadOwnerLocked(newThreadId);
        }

        const std::uint64_t previousTimestamp = m_lastTimestampByProcessor[processorIndex];
        if (previousTimestamp != 0 && timestamp <= previousTimestamp)
        {
            return;
        }
        if (previousTimestamp != 0)
        {
            const std::uint64_t deltaTicks = timestamp - previousTimestamp;
            m_observedTicksByProcessor[processorIndex] += deltaTicks;
            recordThreadRuntimeLocked(processorIndex, oldThreadId, deltaTicks);
        }
        m_lastTimestampByProcessor[processorIndex] = timestamp;
        m_currentThreadIdByProcessor[processorIndex] = newThreadId;
        m_currentThreadKnownByProcessor[processorIndex] = true;
    }

    void ProcessCpuCoreEtwMonitor::recordThreadRuntimeLocked(
        const std::size_t processorIndex,
        const std::uint32_t threadId,
        const std::uint64_t deltaTicks)
    {
        if (threadId == 0 || deltaTicks == 0 || processorIndex >= m_processors.size())
        {
            return; // Idle 线程只进入分母，不归入任何用户进程。
        }

        const std::uint32_t processId = resolveThreadOwnerLocked(threadId);
        if (processId == 0)
        {
            return;
        }

        RuntimeCounter& processCounter = m_processRuntimeByPid[processId];
        if (processCounter.ticksByProcessor.size() != m_processors.size())
        {
            processCounter.ticksByProcessor.assign(m_processors.size(), 0);
        }
        processCounter.ticksByProcessor[processorIndex] += deltaTicks;

        RuntimeCounter& threadCounter =
            m_threadRuntimeByIdentity[BuildCpuThreadIdentity(processId, threadId)];
        if (threadCounter.ticksByProcessor.size() != m_processors.size())
        {
            threadCounter.ticksByProcessor.assign(m_processors.size(), 0);
        }
        threadCounter.ticksByProcessor[processorIndex] += deltaTicks;
    }

    std::uint32_t ProcessCpuCoreEtwMonitor::resolveThreadOwnerLocked(const std::uint32_t threadId)
    {
        const auto ownerIt = m_processIdByThreadId.find(threadId);
        if (ownerIt != m_processIdByThreadId.end())
        {
            return ownerIt->second;
        }
        if (m_unresolvedThreadIds.find(threadId) != m_unresolvedThreadIds.end())
        {
            return 0;
        }

        // 线程 rundown 尚未到达时只查询一次句柄；随后由 Start/DCStart 事件覆盖缓存。
        std::uint32_t processId = 0;
        HANDLE threadHandle = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
        if (threadHandle != nullptr)
        {
            processId = static_cast<std::uint32_t>(::GetProcessIdOfThread(threadHandle));
            ::CloseHandle(threadHandle);
        }
        if (processId != 0)
        {
            m_processIdByThreadId[threadId] = processId;
        }
        else
        {
            m_unresolvedThreadIds.insert(threadId);
        }
        return processId;
    }

    void ProcessCpuCoreEtwMonitor::resetProcessorBaselinesLocked()
    {
        std::fill(m_lastTimestampByProcessor.begin(), m_lastTimestampByProcessor.end(), 0);
        std::fill(m_currentThreadIdByProcessor.begin(), m_currentThreadIdByProcessor.end(), 0);
        std::fill(m_currentThreadKnownByProcessor.begin(), m_currentThreadKnownByProcessor.end(), false);
    }

    void ProcessCpuCoreEtwMonitor::setLastErrorText(std::string errorText, const ULONG errorCode, const bool codeKnown)
    {
        std::lock_guard<std::mutex> errorGuard(m_errorMutex);
        m_lastErrorText = std::move(errorText);
        m_lastErrorCode = errorCode;
        m_lastErrorCodeKnown = codeKnown;
    }
} // namespace ks::process
