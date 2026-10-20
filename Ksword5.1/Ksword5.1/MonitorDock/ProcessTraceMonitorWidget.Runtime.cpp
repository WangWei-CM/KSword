#include "ProcessTraceMonitorWidget.h"
#include <QMessageBox>
#include <QTableWidget>
#include <QTimer>
#include <QPushButton>
#include <QLabel>
#include <QThreadPool>
#include <QRunnable>
#include <QDateTime>
#include <algorithm>
#include <Windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

namespace
{
    struct ProviderSelectionResolved
    {
        QString providerName;
        QString providerGuidText;
        QString providerTypeText;
        GUID providerGuid{};
    };
    std::atomic<std::uint64_t> captureSerial{ 0 };
    std::vector<unsigned char> traceProperties(const QString& sessionName)
    {
        const auto name = sessionName.toStdWString();
        std::vector<unsigned char> buffer(sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t), 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
        properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
        properties->Wnode.ClientContext = 2;
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties->BufferSize = 256;
        properties->MinimumBuffers = 32;
        properties->MaximumBuffers = 128;
        ::wcscpy_s(reinterpret_cast<wchar_t*>(buffer.data() + properties->LoggerNameOffset), name.size() + 1, name.c_str());
        return buffer;
    }
    // defaultProviderNameList：
    // - 作用：定义“进程定向监控”固定启用的宽覆盖 Provider 名单；
    // - 调用：startMonitoring 的后台线程按名称解析 GUID 后统一启用。
    QStringList defaultProviderNameList()
    {
        return QStringList{
            QStringLiteral("Microsoft-Windows-Kernel-Process"),
            QStringLiteral("Microsoft-Windows-Kernel-Thread"),
            QStringLiteral("Microsoft-Windows-Kernel-Image"),
            QStringLiteral("Microsoft-Windows-Kernel-File"),
            QStringLiteral("Microsoft-Windows-Kernel-Registry"),
            QStringLiteral("Microsoft-Windows-TCPIP"),
            QStringLiteral("Microsoft-Windows-DNS-Client"),
            QStringLiteral("Microsoft-Windows-Winsock-AFD"),
            QStringLiteral("Microsoft-Windows-PowerShell"),
            QStringLiteral("Microsoft-Windows-WMI-Activity"),
            QStringLiteral("Microsoft-Windows-TaskScheduler"),
            QStringLiteral("Microsoft-Windows-Security-Auditing"),
            QStringLiteral("Microsoft-Windows-Windows Defender")
        };
    }


}

void ProcessTraceMonitorWidget::startMonitoring()
{
    if (m_capture->m_captureRunning.load())
    {
        if (m_capture->m_capturePaused.load()) setMonitoringPaused(false);
        return;
    }
    if (m_captureCleanup && !m_captureCleanup->isComplete())
    {
        // Explicit Start while a failed cleanup is visible retries that epoch only.
        m_captureCleanup->requestCleanup();
        return;
    }
    if (m_targetProcessList.empty())
    {
        QMessageBox::information(this, QStringLiteral("进程定向监控"), QStringLiteral("请先添加至少一个监控目标。"));
        return;
    }
    const auto state = std::make_shared<CaptureState>();
    state->targets = m_targetProcessList;
    state->m_sessionName = QStringLiteral("KswordProcessTrace-%1-%2").arg(::GetCurrentProcessId()).arg(++captureSerial);
    auto cleanup = ks::ui::RetainedCleanupJob::create([state]() { return cleanupCapture(state); }, 0,
        [state]() { state->m_captureStopFlag.store(true); });
    if (!cleanup) return; // This owner is discarded; never reopen an epoch rejected during shutdown.
    m_capture = state;
    m_captureCleanup = cleanup;
    m_captureCompletionApplied = false;
    m_appliedTargetVersion = 0;
    if (m_eventTable)
    {
        m_eventTable->clearContents();
        m_eventTable->setRowCount(0);
    }
    m_timelineEventPoints.clear();
    m_captureStartTime100ns = currentSystemTime100ns();
    m_captureStopTime100ns = 0;
    m_timelineSelectionStart100ns = m_captureStartTime100ns;
    m_timelineSelectionEnd100ns = m_captureStartTime100ns;
    m_timelineUserSelectionActive = false;
    if (m_eventTimelineWidget) m_eventTimelineWidget->resetTimeline(m_captureStartTime100ns);
    m_lastTimelineRefreshMs = 0;
    if (!m_captureProgressPid) m_captureProgressPid = kPro.addReusable(this, "监控", "进程定向监控");
    kPro.set(m_captureProgressPid, "准备固定 ETW Provider 与目标进程树", 0, 10.0f);
    {
        // The job's synchronous stop signal and this gate close the admission/start race.
        std::lock_guard<std::mutex> gate(state->operationGate);
        if (state->m_captureStopFlag.load() || cleanup->isCleanupRequested()) return;
        state->finished.store(false);
        state->m_captureRunning.store(true);
        state->cleanupPending.store(true);
        try { state->m_captureThread = std::make_unique<std::thread>([state]() { runCapture(state); }); }
        catch (...)
        {
            state->startupError.store(ERROR_NOT_ENOUGH_MEMORY);
            state->finished.store(true);
            state->m_captureRunning.store(false);
        }
    }
    if (m_uiUpdateTimer && !m_uiUpdateTimer->isActive()) m_uiUpdateTimer->start();
    if (m_runtimeRefreshTimer && !m_runtimeRefreshTimer->isActive()) m_runtimeRefreshTimer->start();
    updateActionState();
    updateStatusLabel();
}

void ProcessTraceMonitorWidget::runCapture(const std::shared_ptr<CaptureState>& state)
{
    struct Completion
    {
        CaptureState& state;
        ~Completion() { state.m_captureRunning.store(false); state.m_capturePaused.store(false); state.finished.store(true); }
    } completion{ *state };
    try
    {
        if (state->m_captureStopFlag.load()) return;
        const auto processes = ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto);
        seedTrackedProcessTree(*state, processes);
        if (state->m_captureStopFlag.load()) return;
        std::vector<ProviderSelectionResolved> selectedProviders;
        ULONG bytes = 0;
        ULONG status = ::TdhEnumerateProviders(nullptr, &bytes);
        if (status == ERROR_INSUFFICIENT_BUFFER && bytes)
        {
            std::vector<unsigned char> buffer(bytes, 0);
            auto* info = reinterpret_cast<PROVIDER_ENUMERATION_INFO*>(buffer.data());
            status = ::TdhEnumerateProviders(info, &bytes);
            if (status == ERROR_SUCCESS)
            {
                for (const auto& wanted : defaultProviderNameList())
                {
                    const TRACE_PROVIDER_INFO* selected = nullptr;
                    QString selectedName;
                    for (ULONG index = 0; index < info->NumberOfProviders; ++index)
                    {
                        const auto& entry = info->TraceProviderInfoArray[index];
                        const QString name = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(buffer.data() + entry.ProviderNameOffset));
                        if (name.compare(wanted, Qt::CaseInsensitive) == 0) { selected = &entry; selectedName = name; break; }
                        if (!selected && (name.contains(wanted, Qt::CaseInsensitive) || wanted.contains(name, Qt::CaseInsensitive)))
                        { selected = &entry; selectedName = name; }
                    }
                    if (!selected) continue;
                    if (std::any_of(selectedProviders.begin(), selectedProviders.end(), [selected](const auto& entry) { return !!::IsEqualGUID(entry.providerGuid, selected->ProviderGuid); })) continue;
                    selectedProviders.push_back({ selectedName, guidToText(selected->ProviderGuid), providerTypeFromName(selectedName), selected->ProviderGuid });
                }
            }
        }
        if (selectedProviders.empty()) { state->startupError.store(status == ERROR_SUCCESS ? ERROR_NOT_FOUND : status); return; }
        {
            std::lock_guard<std::mutex> lock(state->m_runtimeMutex);
            for (const auto& provider : selectedProviders)
                state->m_activeProviderList.push_back({ provider.providerName, provider.providerGuidText, provider.providerTypeText });
        }
        TRACEHANDLE trace = INVALID_PROCESSTRACE_HANDLE;
        {
            std::lock_guard<std::mutex> gate(state->operationGate);
            if (state->m_captureStopFlag.load()) return;
            auto buffer = traceProperties(state->m_sessionName);
            auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
            auto* name = reinterpret_cast<wchar_t*>(buffer.data() + properties->LoggerNameOffset);
            TRACEHANDLE session = 0;
            status = ::StartTraceW(&session, name, properties);
            // A unique owned name never permits stopping another process's session.
            if (status != ERROR_SUCCESS) { state->startupError.store(status); return; }
            state->m_sessionHandle.store(session);
            int enabled = 0;
            for (const auto& provider : selectedProviders)
            {
                if (state->m_captureStopFlag.load()) break;
                if (::EnableTraceEx2(session, &provider.providerGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                    TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFFULL, 0, 0, nullptr) == ERROR_SUCCESS) ++enabled;
            }
            if (!enabled || state->m_captureStopFlag.load()) { state->startupError.store(enabled ? ERROR_CANCELLED : ERROR_NOT_SUPPORTED); return; }
            EVENT_TRACE_LOGFILEW log{};
            log.LoggerName = name;
            log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
            log.EventRecordCallback = &ProcessTraceMonitorWidget::processTraceEtwCallback;
            log.Context = state.get();
            trace = ::OpenTraceW(&log);
            if (trace == INVALID_PROCESSTRACE_HANDLE) { state->startupError.store(::GetLastError()); return; }
            state->m_traceHandle.store(trace);
        }
        state->processStarted.store(true);
        state->processStatus.store(::ProcessTrace(&trace, 1, nullptr, nullptr));
    }
    catch (...) { state->startupError.store(ERROR_UNHANDLED_EXCEPTION); }
}

bool ProcessTraceMonitorWidget::cleanupCapture(const std::shared_ptr<CaptureState>& state)
{
    state->m_captureStopFlag.store(true);
    bool stopped = true;
    {
        std::lock_guard<std::mutex> gate(state->operationGate);
        if (const auto session = state->m_sessionHandle.load())
        {
            auto buffer = traceProperties(state->m_sessionName);
            auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
            auto* name = reinterpret_cast<wchar_t*>(buffer.data() + properties->LoggerNameOffset);
            const ULONG status = ::ControlTraceW(session, name, properties, EVENT_TRACE_CONTROL_STOP);
            stopped = status == ERROR_SUCCESS || status == ERROR_WMI_INSTANCE_NOT_FOUND;
            if (stopped) state->m_sessionHandle.store(0);
            else state->operationError.store(status); // Keep exact owned name and handle for Retry.
        }
        if ((!stopped || state->finished.load()) && state->m_traceHandle.load())
        {
            const ULONG status = ::CloseTrace(state->m_traceHandle.load());
            if (status == ERROR_SUCCESS || status == ERROR_CTX_CLOSE_PENDING) state->m_traceHandle.store(0);
            else { state->operationError.store(status); if (!state->finished.load()) return false; }
        }
        if (!stopped && !state->finished.load() && state->m_traceHandle.load()) return false;
    }
    // Only the retained worker joins. Successful STOP drains the final buffer first.
    if (state->m_captureThread && state->m_captureThread->joinable()) state->m_captureThread->join();
    state->m_captureThread.reset();
    {
        std::lock_guard<std::mutex> gate(state->operationGate);
        if (const auto trace = state->m_traceHandle.load())
        {
            const ULONG status = ::CloseTrace(trace);
            if (status == ERROR_SUCCESS || status == ERROR_CTX_CLOSE_PENDING) state->m_traceHandle.store(0);
            else state->operationError.store(status);
        }
        const bool complete = state->m_sessionHandle.load() == 0 && state->m_traceHandle.load() == 0 && state->finished.load();
        state->cleanupPending.store(!complete);
        if (complete) state->operationError.store(ERROR_SUCCESS);
        return complete;
    }
}

void ProcessTraceMonitorWidget::stopMonitoring() { stopMonitoringInternal(false); }

void ProcessTraceMonitorWidget::stopMonitoringInternal(const bool ownerClosing)
{
    m_capture->m_captureStopFlag.store(true);
    // Owner destruction signals stop but does not retry a failed retained cleanup.
    // Explicit Stop and MainWindow Retry remain the retry actions.
    if (m_captureCleanup && (!ownerClosing || !m_captureCleanup->isCleanupRequested()))
        m_captureCleanup->requestCleanup();
    if (m_captureStopTime100ns == 0) m_captureStopTime100ns = currentSystemTime100ns();
    if (m_runtimeRefreshTimer) m_runtimeRefreshTimer->stop();
    if (m_uiUpdateTimer && !m_uiUpdateTimer->isActive()) m_uiUpdateTimer->start();
    refreshTimelineRange(true);
    updateActionState();
    updateStatusLabel();
}

void ProcessTraceMonitorWidget::pollCaptureState()
{
    const auto state = m_capture;
    if (state->finished.load() && m_captureCleanup && !m_captureCleanup->isCleanupRequested()) m_captureCleanup->requestCleanup();
    bool targetChanged = false;
    {
        std::lock_guard<std::mutex> lock(state->m_runtimeMutex);
        if (m_appliedTargetVersion != state->targetVersion)
        {
            m_targetProcessList = state->targets;
            m_appliedTargetVersion = state->targetVersion;
            targetChanged = true;
        }
    }
    if (targetChanged) refreshTargetTable();
    if (state->finished.load() && !m_captureCompletionApplied)
    {
        m_captureCompletionApplied = true;
        if (!m_captureStopTime100ns) m_captureStopTime100ns = currentSystemTime100ns();
        if (m_runtimeRefreshTimer) m_runtimeRefreshTimer->stop();
        kPro.set(m_captureProgressPid, "进程定向监控结束", 0, 100.0f);
        refreshTimelineRange(true);
        updateActionState();
        updateStatusLabel();
    }
    updateStatusLabel();
    if (m_stopButton) m_stopButton->setEnabled(state->m_captureRunning.load() || (m_captureCleanup && !m_captureCleanup->isComplete()));
}

void ProcessTraceMonitorWidget::setMonitoringPaused(const bool paused)
{
    if (!m_capture->m_captureRunning.load() || m_capture->m_captureStopFlag.load()) return;
    m_capture->m_capturePaused.store(paused);
    updateActionState();
    updateStatusLabel();
}

void ProcessTraceMonitorWidget::refreshTrackedProcessSnapshotAsync()
{
    const auto state = m_capture;
    if (!state->m_captureRunning.load() || state->m_captureStopFlag.load() || state->m_runtimeRefreshPending.exchange(true)) return;
    const auto lease = ks::ui::AsyncCleanupRegistry::global().acquire();
    if (!lease) { state->m_runtimeRefreshPending.store(false); return; }
    try
    {
        QThreadPool::globalInstance()->start(QRunnable::create([lease, state]()
        {
            try
            {
                if (!state->m_captureStopFlag.load())
                    syncTrackedProcessTree(*state, ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto));
            }
            catch (...) {}
            state->m_runtimeRefreshPending.store(false);
        }));
    }
    catch (...) { state->m_runtimeRefreshPending.store(false); }
}

void ProcessTraceMonitorWidget::seedTrackedProcessTree(CaptureState& state, const std::vector<ks::process::ProcessRecord>& processList)
{
    // Expensive closure/pruning runs on a private value snapshot. GUI readers only
    // contend with short copy/swap sections, never the system-sized tree walk.
    CaptureState next;
    std::uint64_t baseVersion = 0;
    std::uint64_t baseRuntimeRevision = 0;
    {
        std::lock_guard<std::mutex> lock(state.m_runtimeMutex);
        baseVersion = state.targetVersion;
        baseRuntimeRevision = state.runtimeRevision;
        next.targets = state.targets;
    }

    next.m_trackedProcessMap.clear();

    for (TargetProcessEntry& targetEntry : next.targets)
    {
        targetEntry.alive = false;
        const auto found = std::find_if(
            processList.begin(),
            processList.end(),
            [&targetEntry](const ks::process::ProcessRecord& record) {
                return record.pid == targetEntry.pid;
            });

        RuntimeTrackedProcess trackedProcess;
        trackedProcess.pid = targetEntry.pid;
        trackedProcess.parentPid = 0;
        trackedProcess.rootPid = targetEntry.pid;
        trackedProcess.processName = targetEntry.processName;
        trackedProcess.imagePath = targetEntry.imagePath;
        trackedProcess.creationTime100ns = targetEntry.creationTime100ns;
        trackedProcess.alive = targetEntry.alive;
        trackedProcess.isRoot = true;
        trackedProcess.staleSnapshotRounds = 0;
        trackedProcess.lastRelatedEventTime100ns = 0;

        if (found != processList.end())
        {
            const bool creationMatches = targetEntry.creationTime100ns == 0
                || found->creationTime100ns == 0
                || targetEntry.creationTime100ns == found->creationTime100ns;
            if (creationMatches)
            {
                trackedProcess.parentPid = found->parentPid;
                trackedProcess.processName = QString::fromStdString(found->processName);
                trackedProcess.imagePath = QString::fromStdString(found->imagePath);
                trackedProcess.creationTime100ns = found->creationTime100ns;
                trackedProcess.alive = true;

                targetEntry.processName = trackedProcess.processName;
                targetEntry.imagePath = trackedProcess.imagePath;
                targetEntry.creationTime100ns = trackedProcess.creationTime100ns;
                targetEntry.alive = true;
            }
        }

        next.m_trackedProcessMap[trackedProcess.pid] = trackedProcess;
    }

    bool added = true;
    while (added)
    {
        added = false;
        for (const ks::process::ProcessRecord& record : processList)
        {
            if (record.pid == 0 || next.m_trackedProcessMap.find(record.pid) != next.m_trackedProcessMap.end())
            {
                continue;
            }

            const auto parentIt = next.m_trackedProcessMap.find(record.parentPid);
            if (parentIt == next.m_trackedProcessMap.end())
            {
                continue;
            }

            RuntimeTrackedProcess trackedProcess;
            trackedProcess.pid = record.pid;
            trackedProcess.parentPid = record.parentPid;
            trackedProcess.rootPid = parentIt->second.rootPid;
            trackedProcess.processName = QString::fromStdString(record.processName);
            trackedProcess.imagePath = QString::fromStdString(record.imagePath);
            trackedProcess.creationTime100ns = record.creationTime100ns;
            trackedProcess.alive = true;
            trackedProcess.isRoot = false;
            trackedProcess.staleSnapshotRounds = 0;
            trackedProcess.lastRelatedEventTime100ns = 0;
            next.m_trackedProcessMap[trackedProcess.pid] = trackedProcess;
            added = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state.m_runtimeMutex);
        if (state.targetVersion != baseVersion || state.runtimeRevision != baseRuntimeRevision) return;
        state.m_trackedProcessMap.swap(next.m_trackedProcessMap);
        ++state.runtimeRevision;
        state.targets.swap(next.targets);
        ++state.targetVersion;
    }
}

void ProcessTraceMonitorWidget::syncTrackedProcessTree(CaptureState& state, const std::vector<ks::process::ProcessRecord>& processList)
{
    // Expensive closure/pruning runs on a private value snapshot. GUI readers only
    // contend with short copy/swap sections, never the system-sized tree walk.
    CaptureState next;
    std::uint64_t baseVersion = 0;
    std::uint64_t baseRuntimeRevision = 0;
    {
        std::lock_guard<std::mutex> lock(state.m_runtimeMutex);
        baseVersion = state.targetVersion;
        baseRuntimeRevision = state.runtimeRevision;
        next.targets = state.targets;
        next.m_trackedProcessMap = state.m_trackedProcessMap;
    }


    for (auto& [pidValue, trackedProcess] : next.m_trackedProcessMap)
    {
        (void)pidValue;
        trackedProcess.alive = false;
    }

    bool added = true;
    while (added)
    {
        added = false;
        for (const ks::process::ProcessRecord& record : processList)
        {
            if (record.pid == 0)
            {
                continue;
            }

            auto existingIt = next.m_trackedProcessMap.find(record.pid);
            if (existingIt != next.m_trackedProcessMap.end())
            {
                const bool creationMatches = existingIt->second.creationTime100ns == 0
                    || record.creationTime100ns == 0
                    || existingIt->second.creationTime100ns == record.creationTime100ns;
                if (!creationMatches)
                {
                    if (existingIt->second.isRoot)
                    {
                        existingIt->second.alive = false;
                        continue;
                    }

                    const auto parentIt = next.m_trackedProcessMap.find(record.parentPid);
                    if (parentIt == next.m_trackedProcessMap.end())
                    {
                        existingIt->second.alive = false;
                        continue;
                    }

                    existingIt->second.parentPid = record.parentPid;
                    existingIt->second.rootPid = parentIt->second.rootPid;
                    existingIt->second.processName = QString::fromStdString(record.processName);
                    existingIt->second.imagePath = QString::fromStdString(record.imagePath);
                    existingIt->second.creationTime100ns = record.creationTime100ns;
                    existingIt->second.alive = true;
                    existingIt->second.isRoot = false;
                    existingIt->second.staleSnapshotRounds = 0;
                    continue;
                }

                existingIt->second.parentPid = record.parentPid;
                existingIt->second.processName = QString::fromStdString(record.processName);
                existingIt->second.imagePath = QString::fromStdString(record.imagePath);
                existingIt->second.creationTime100ns = record.creationTime100ns;
                existingIt->second.alive = true;
                existingIt->second.staleSnapshotRounds = 0;
                continue;
            }

            const auto parentIt = next.m_trackedProcessMap.find(record.parentPid);
            if (parentIt == next.m_trackedProcessMap.end())
            {
                continue;
            }

            RuntimeTrackedProcess trackedProcess;
            trackedProcess.pid = record.pid;
            trackedProcess.parentPid = record.parentPid;
            trackedProcess.rootPid = parentIt->second.rootPid;
            trackedProcess.processName = QString::fromStdString(record.processName);
            trackedProcess.imagePath = QString::fromStdString(record.imagePath);
            trackedProcess.creationTime100ns = record.creationTime100ns;
            trackedProcess.alive = true;
            trackedProcess.isRoot = false;
            trackedProcess.staleSnapshotRounds = 0;
            trackedProcess.lastRelatedEventTime100ns = 0;
            next.m_trackedProcessMap[trackedProcess.pid] = trackedProcess;
            added = true;
        }
    }

    pruneStaleTrackedProcesses(next);

    for (TargetProcessEntry& targetEntry : next.targets)
    {
        const auto trackedIt = next.m_trackedProcessMap.find(targetEntry.pid);
        if (trackedIt == next.m_trackedProcessMap.end())
        {
            targetEntry.alive = false;
            continue;
        }

        const bool creationMatches = targetEntry.creationTime100ns == 0
            || trackedIt->second.creationTime100ns == 0
            || targetEntry.creationTime100ns == trackedIt->second.creationTime100ns;
        targetEntry.alive = trackedIt->second.alive && creationMatches;
        if (!creationMatches)
        {
            continue;
        }
        if (!trackedIt->second.processName.trimmed().isEmpty())
        {
            targetEntry.processName = trackedIt->second.processName;
        }
        if (!trackedIt->second.imagePath.trimmed().isEmpty())
        {
            targetEntry.imagePath = trackedIt->second.imagePath;
        }
        if (trackedIt->second.creationTime100ns != 0)
        {
            targetEntry.creationTime100ns = trackedIt->second.creationTime100ns;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state.m_runtimeMutex);
        if (state.targetVersion != baseVersion || state.runtimeRevision != baseRuntimeRevision) return;
        state.m_trackedProcessMap.swap(next.m_trackedProcessMap);
        ++state.runtimeRevision;
        state.targets.swap(next.targets);
        ++state.targetVersion;
    }
}

void ProcessTraceMonitorWidget::pruneStaleTrackedProcesses(CaptureState& state)
{
    FILETIME fileTimeValue{};
    ::GetSystemTimeAsFileTime(&fileTimeValue);
    ULARGE_INTEGER currentTimeValue{};
    currentTimeValue.LowPart = fileTimeValue.dwLowDateTime;
    currentTimeValue.HighPart = fileTimeValue.dwHighDateTime;
    const std::uint64_t currentTime100ns = static_cast<std::uint64_t>(currentTimeValue.QuadPart);
    const std::uint64_t kRecentEventKeepWindow100ns = 15ULL * 1000ULL * 1000ULL * 10ULL;

    std::vector<std::uint32_t> pidListNeedErase;
    pidListNeedErase.reserve(state.m_trackedProcessMap.size());

    for (auto& [pidValue, trackedProcess] : state.m_trackedProcessMap)
    {
        (void)pidValue;
        if (trackedProcess.alive)
        {
            trackedProcess.staleSnapshotRounds = 0;
            continue;
        }

        ++trackedProcess.staleSnapshotRounds;
        const bool recentEventHit = trackedProcess.lastRelatedEventTime100ns != 0
            && trackedProcess.lastRelatedEventTime100ns <= currentTime100ns
            && (currentTime100ns - trackedProcess.lastRelatedEventTime100ns) <= kRecentEventKeepWindow100ns;
        if (!trackedProcess.isRoot
            && trackedProcess.staleSnapshotRounds >= 3
            && !recentEventHit)
        {
            pidListNeedErase.push_back(trackedProcess.pid);
        }
    }

    for (const std::uint32_t pidValue : pidListNeedErase)
    {
        state.m_trackedProcessMap.erase(pidValue);
    }
}
