#include "ProcessTraceMonitorWidget.h"

// ============================================================
// ProcessTraceMonitorWidget.Runtime.cpp
// 作用：
// 1) 实现 ETW 会话控制、开始/停止/暂停状态切换；
// 2) 维护目标进程树的播种与运行期快照同步；
// 3) 把重型运行时控制逻辑从事件解码文件中拆出，控制单文件体积。
// ============================================================

#include <QApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#pragma comment(lib, "Tdh.lib")

namespace
{
    // ProviderSelectionResolved：
    // - 作用：保存“已解析出 GUID 且准备启用”的 Provider；
    // - 调用：startMonitoring 的后台 ETW 线程内部使用。
    struct ProviderSelectionResolved
    {
        QString providerName;
        QString providerGuidText;
        QString providerTypeText;
        GUID providerGuid{};
    };

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
            QStringLiteral("Microsoft-Windows-Defender")
        };
    }

    void stopActiveKswordTraceSessionsByPrefix(const QStringList& sessionPrefixList)
    {
        constexpr ULONG kQuerySessionCapacity = 96;
        constexpr ULONG kTraceNameChars = 1024;
        constexpr ULONG kLogFileChars = 1024;
        constexpr ULONG kPropertyBufferSize =
            sizeof(EVENT_TRACE_PROPERTIES)
            + (kTraceNameChars + kLogFileChars) * sizeof(wchar_t);

        std::vector<std::vector<unsigned char>> propertyBufferList(
            kQuerySessionCapacity,
            std::vector<unsigned char>(kPropertyBufferSize, 0));
        std::vector<EVENT_TRACE_PROPERTIES*> propertyPointerList(kQuerySessionCapacity, nullptr);

        for (ULONG indexValue = 0; indexValue < kQuerySessionCapacity; ++indexValue)
        {
            auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBufferList[indexValue].data());
            properties->Wnode.BufferSize = kPropertyBufferSize;
            properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            properties->LogFileNameOffset =
                sizeof(EVENT_TRACE_PROPERTIES) + kTraceNameChars * sizeof(wchar_t);
            propertyPointerList[indexValue] = properties;
        }

        ULONG sessionCount = kQuerySessionCapacity;
        const ULONG queryStatus = ::QueryAllTracesW(
            propertyPointerList.data(),
            kQuerySessionCapacity,
            &sessionCount);
        if (queryStatus != ERROR_SUCCESS && queryStatus != ERROR_MORE_DATA)
        {
            return;
        }

        for (ULONG indexValue = 0; indexValue < sessionCount && indexValue < kQuerySessionCapacity; ++indexValue)
        {
            const EVENT_TRACE_PROPERTIES* properties = propertyPointerList[indexValue];
            if (properties == nullptr || properties->LoggerNameOffset == 0)
            {
                continue;
            }

            const wchar_t* loggerNamePointer = reinterpret_cast<const wchar_t*>(
                propertyBufferList[indexValue].data() + properties->LoggerNameOffset);
            const QString loggerNameText = QString::fromWCharArray(loggerNamePointer).trimmed();
            if (loggerNameText.isEmpty())
            {
                continue;
            }

            const bool shouldStop = std::any_of(
                sessionPrefixList.begin(),
                sessionPrefixList.end(),
                [&loggerNameText](const QString& prefixText) {
                    return !prefixText.trimmed().isEmpty()
                        && loggerNameText.startsWith(prefixText, Qt::CaseInsensitive);
                });
            if (!shouldStop)
            {
                continue;
            }

            const std::wstring loggerNameWide = loggerNameText.toStdWString();
            std::vector<unsigned char> stopBuffer(
                sizeof(EVENT_TRACE_PROPERTIES) + (loggerNameWide.size() + 1) * sizeof(wchar_t),
                0);
            auto* stopProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stopBuffer.data());
            stopProperties->Wnode.BufferSize = static_cast<ULONG>(stopBuffer.size());
            stopProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            wchar_t* stopLoggerNamePointer = reinterpret_cast<wchar_t*>(
                stopBuffer.data() + stopProperties->LoggerNameOffset);
            ::wcscpy_s(stopLoggerNamePointer, loggerNameWide.size() + 1, loggerNameWide.c_str());
            ::ControlTraceW(0, stopLoggerNamePointer, stopProperties, EVENT_TRACE_CONTROL_STOP);
        }
    }
}

void ProcessTraceMonitorWidget::startMonitoring()
{
    if (m_captureRunning.load())
    {
        if (m_capturePaused.load())
        {
            setMonitoringPaused(false);
        }
        return;
    }

    if (m_targetProcessList.empty())
    {
        QMessageBox::information(this, QStringLiteral("进程定向监控"), QStringLiteral("请先添加至少一个监控目标。"));
        return;
    }

    if (m_captureThread != nullptr && m_captureThread->joinable())
    {
        m_captureThread->join();
        m_captureThread.reset();
    }

    // 启动前先清空旧结果，避免不同批次事件混到一起。
    if (m_eventTable != nullptr)
    {
        m_eventTable->clearContents();
        m_eventTable->setRowCount(0);
    }
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRows.clear();
    }

    // 用一轮进程快照先播种目标进程树，确保已有子进程在 ETW 会话启动前也被纳入。
    std::vector<ks::process::ProcessRecord> processList = ks::process::EnumerateProcesses(
        ks::process::ProcessEnumStrategy::Auto);
    seedTrackedProcessTree(processList);
    refreshTargetTable();

    m_captureRunning.store(true);
    m_capturePaused.store(false);
    m_captureStopFlag.store(false);
    m_runtimeRefreshPending.store(false);
    m_sessionHandle.store(0);
    m_traceHandle.store(0);
    stopActiveKswordTraceSessionsByPrefix(QStringList{ QStringLiteral("KswordProcessTrace") });
    m_sessionName = QStringLiteral("KswordProcessTrace");

    if (m_captureProgressPid == 0)
    {
        m_captureProgressPid = kPro.add("监控", "进程定向监控");
    }
    kPro.set(m_captureProgressPid, "准备固定 ETW Provider 与目标进程树", 0, 10.0f);

    if (m_uiUpdateTimer != nullptr && !m_uiUpdateTimer->isActive())
    {
        m_uiUpdateTimer->start();
    }
    if (m_runtimeRefreshTimer != nullptr && !m_runtimeRefreshTimer->isActive())
    {
        m_runtimeRefreshTimer->start();
    }

    updateActionState();
    updateStatusLabel();

    kLogEvent startEvent;
    info << startEvent
        << "[ProcessTraceMonitorWidget] 启动进程定向监控, targetCount="
        << m_targetProcessList.size()
        << eol;

    QPointer<ProcessTraceMonitorWidget> guardThis(this);
    m_captureThread = std::make_unique<std::thread>([guardThis]() {
        if (guardThis == nullptr)
        {
            return;
        }

        // 固定 Provider 解析：
        // - 先枚举系统全部 Provider；
        // - 再按预设名称做精确/模糊匹配；
        // - 只要能解析出 GUID，就纳入本轮会话。
        QStringList wantedProviderNames = defaultProviderNameList();
        std::vector<ProviderSelectionResolved> selectedProviders;

        ULONG providerBufferSize = 0;
        ULONG enumerateStatus = ::TdhEnumerateProviders(nullptr, &providerBufferSize);
        if (enumerateStatus == ERROR_INSUFFICIENT_BUFFER && providerBufferSize > 0)
        {
            std::vector<unsigned char> providerBuffer(providerBufferSize, 0);
            auto* providerInfo = reinterpret_cast<PROVIDER_ENUMERATION_INFO*>(providerBuffer.data());
            enumerateStatus = ::TdhEnumerateProviders(providerInfo, &providerBufferSize);
            if (enumerateStatus == ERROR_SUCCESS && providerInfo != nullptr)
            {
                std::vector<ProviderSelectionResolved> allProviders;
                allProviders.reserve(providerInfo->NumberOfProviders);
                for (ULONG indexValue = 0; indexValue < providerInfo->NumberOfProviders; ++indexValue)
                {
                    const TRACE_PROVIDER_INFO& traceInfo = providerInfo->TraceProviderInfoArray[indexValue];
                    const wchar_t* providerNamePointer = reinterpret_cast<const wchar_t*>(
                        providerBuffer.data() + traceInfo.ProviderNameOffset);

                    ProviderSelectionResolved entry;
                    entry.providerName = providerNamePointer != nullptr
                        ? QString::fromWCharArray(providerNamePointer)
                        : QStringLiteral("<Unknown>");
                    entry.providerGuidText = guardThis->guidToText(traceInfo.ProviderGuid);
                    entry.providerTypeText = guardThis->providerTypeFromName(entry.providerName);
                    entry.providerGuid = traceInfo.ProviderGuid;
                    allProviders.push_back(entry);
                }

                for (const QString& wantedName : wantedProviderNames)
                {
                    auto found = std::find_if(
                        allProviders.begin(),
                        allProviders.end(),
                        [wantedName](const ProviderSelectionResolved& item) {
                            return item.providerName.compare(wantedName, Qt::CaseInsensitive) == 0;
                        });
                    if (found == allProviders.end())
                    {
                        found = std::find_if(
                            allProviders.begin(),
                            allProviders.end(),
                            [wantedName](const ProviderSelectionResolved& item) {
                                return item.providerName.contains(wantedName, Qt::CaseInsensitive)
                                    || wantedName.contains(item.providerName, Qt::CaseInsensitive);
                            });
                    }
                    if (found == allProviders.end())
                    {
                        continue;
                    }

                    const bool duplicate = std::any_of(
                        selectedProviders.begin(),
                        selectedProviders.end(),
                        [found](const ProviderSelectionResolved& item) {
                            return ::IsEqualGUID(item.providerGuid, found->providerGuid) != FALSE;
                        });
                    if (!duplicate)
                    {
                        selectedProviders.push_back(*found);
                    }
                }
            }
        }

        if (selectedProviders.empty())
        {
            QMetaObject::invokeMethod(qApp, [guardThis]() {
                if (guardThis == nullptr)
                {
                    return;
                }

                guardThis->m_captureRunning.store(false);
                guardThis->m_capturePaused.store(false);
                if (guardThis->m_runtimeRefreshTimer != nullptr)
                {
                    guardThis->m_runtimeRefreshTimer->stop();
                }
                if (guardThis->m_uiUpdateTimer != nullptr)
                {
                    guardThis->m_uiUpdateTimer->stop();
                }
                kPro.set(guardThis->m_captureProgressPid, "预置 Provider 解析失败", 0, 100.0f);
                QMessageBox::warning(
                    guardThis,
                    QStringLiteral("进程定向监控"),
                    QStringLiteral("未解析到任何可用的 ETW Provider，监控无法启动。"));
                guardThis->updateActionState();
                guardThis->updateStatusLabel();
            }, Qt::QueuedConnection);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(guardThis->m_runtimeMutex);
            guardThis->m_activeProviderList.clear();
            for (const ProviderSelectionResolved& provider : selectedProviders)
            {
                ProviderEntry entry;
                entry.providerName = provider.providerName;
                entry.providerGuidText = provider.providerGuidText;
                entry.providerTypeText = provider.providerTypeText;
                guardThis->m_activeProviderList.push_back(entry);
            }
        }

        const std::wstring sessionNameWide = guardThis->m_sessionName.toStdWString();
        const ULONG propertyBufferSize = static_cast<ULONG>(
            sizeof(EVENT_TRACE_PROPERTIES) + (sessionNameWide.size() + 1) * sizeof(wchar_t));
        std::vector<unsigned char> propertyBuffer(propertyBufferSize, 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());

        properties->Wnode.BufferSize = propertyBufferSize;
        properties->Wnode.ClientContext = 2;
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties->BufferSize = 256;
        properties->MinimumBuffers = 32;
        properties->MaximumBuffers = 128;

        wchar_t* loggerNamePointer = reinterpret_cast<wchar_t*>(propertyBuffer.data() + properties->LoggerNameOffset);
        if (!sessionNameWide.empty())
        {
            ::wcscpy_s(loggerNamePointer, sessionNameWide.size() + 1, sessionNameWide.c_str());
        }

        TRACEHANDLE sessionHandle = 0;
        ULONG startStatus = ::StartTraceW(&sessionHandle, loggerNamePointer, properties);
        if (startStatus == ERROR_ALREADY_EXISTS)
        {
            ::ControlTraceW(0, loggerNamePointer, properties, EVENT_TRACE_CONTROL_STOP);
            startStatus = ::StartTraceW(&sessionHandle, loggerNamePointer, properties);
        }

        if (startStatus != ERROR_SUCCESS)
        {
            QMetaObject::invokeMethod(qApp, [guardThis, startStatus]() {
                if (guardThis == nullptr)
                {
                    return;
                }

                guardThis->m_captureRunning.store(false);
                guardThis->m_capturePaused.store(false);
                if (guardThis->m_runtimeRefreshTimer != nullptr)
                {
                    guardThis->m_runtimeRefreshTimer->stop();
                }
                if (guardThis->m_uiUpdateTimer != nullptr)
                {
                    guardThis->m_uiUpdateTimer->stop();
                }
                kPro.set(guardThis->m_captureProgressPid, "ETW 会话启动失败", 0, 100.0f);
                QMessageBox::warning(
                    guardThis,
                    QStringLiteral("进程定向监控"),
                    QStringLiteral("StartTraceW 失败，错误码=%1。").arg(startStatus));
                guardThis->updateActionState();
                guardThis->updateStatusLabel();
            }, Qt::QueuedConnection);
            return;
        }

        guardThis->m_sessionHandle.store(static_cast<std::uint64_t>(sessionHandle));
        kPro.set(guardThis->m_captureProgressPid, "启用固定 Provider 集合", 0, 30.0f);

        int enableSuccessCount = 0;
        for (const ProviderSelectionResolved& provider : selectedProviders)
        {
            if (guardThis == nullptr || guardThis->m_captureStopFlag.load())
            {
                break;
            }

            const ULONG enableStatus = ::EnableTraceEx2(
                sessionHandle,
                &provider.providerGuid,
                EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                TRACE_LEVEL_VERBOSE,
                0xFFFFFFFFFFFFFFFFULL,
                0,
                0,
                nullptr);
            if (enableStatus == ERROR_SUCCESS)
            {
                ++enableSuccessCount;
            }
            else
            {
                kLogEvent event;
                warn << event
                    << "[ProcessTraceMonitorWidget] EnableTraceEx2失败, provider="
                    << provider.providerName.toStdString()
                    << ", status="
                    << enableStatus
                    << eol;
            }
        }

        if (enableSuccessCount == 0)
        {
            ::ControlTraceW(sessionHandle, loggerNamePointer, properties, EVENT_TRACE_CONTROL_STOP);
            guardThis->m_sessionHandle.store(0);
            QMetaObject::invokeMethod(qApp, [guardThis]() {
                if (guardThis == nullptr)
                {
                    return;
                }

                guardThis->m_captureRunning.store(false);
                guardThis->m_capturePaused.store(false);
                if (guardThis->m_runtimeRefreshTimer != nullptr)
                {
                    guardThis->m_runtimeRefreshTimer->stop();
                }
                if (guardThis->m_uiUpdateTimer != nullptr)
                {
                    guardThis->m_uiUpdateTimer->stop();
                }
                kPro.set(guardThis->m_captureProgressPid, "Provider 启用失败", 0, 100.0f);
                QMessageBox::warning(
                    guardThis,
                    QStringLiteral("进程定向监控"),
                    QStringLiteral("固定 Provider 集合全部启用失败，监控已停止。"));
                guardThis->updateActionState();
                guardThis->updateStatusLabel();
            }, Qt::QueuedConnection);
            return;
        }

        EVENT_TRACE_LOGFILEW traceLogFile{};
        traceLogFile.LoggerName = loggerNamePointer;
        traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        traceLogFile.EventRecordCallback = &ProcessTraceMonitorWidget::processTraceEtwCallback;
        traceLogFile.Context = guardThis.data();

        TRACEHANDLE traceHandle = ::OpenTraceW(&traceLogFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            ::ControlTraceW(sessionHandle, loggerNamePointer, properties, EVENT_TRACE_CONTROL_STOP);
            guardThis->m_sessionHandle.store(0);

            const ULONG lastError = ::GetLastError();
            QMetaObject::invokeMethod(qApp, [guardThis, lastError]() {
                if (guardThis == nullptr)
                {
                    return;
                }

                guardThis->m_captureRunning.store(false);
                guardThis->m_capturePaused.store(false);
                if (guardThis->m_runtimeRefreshTimer != nullptr)
                {
                    guardThis->m_runtimeRefreshTimer->stop();
                }
                if (guardThis->m_uiUpdateTimer != nullptr)
                {
                    guardThis->m_uiUpdateTimer->stop();
                }
                kPro.set(guardThis->m_captureProgressPid, "OpenTrace 失败", 0, 100.0f);
                QMessageBox::warning(
                    guardThis,
                    QStringLiteral("进程定向监控"),
                    QStringLiteral("OpenTraceW 失败，错误码=%1。").arg(lastError));
                guardThis->updateActionState();
                guardThis->updateStatusLabel();
            }, Qt::QueuedConnection);
            return;
        }

        guardThis->m_traceHandle.store(static_cast<std::uint64_t>(traceHandle));
        kPro.set(guardThis->m_captureProgressPid, "开始接收目标相关 ETW 事件", 0, 55.0f);

        const ULONG processStatus = ::ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        const std::uint64_t ownedTraceHandle = guardThis->m_traceHandle.exchange(0);
        if (ownedTraceHandle != 0)
        {
            ::CloseTrace(static_cast<TRACEHANDLE>(ownedTraceHandle));
        }

        const std::uint64_t ownedSessionHandle = guardThis->m_sessionHandle.exchange(0);
        if (ownedSessionHandle != 0)
        {
            ::ControlTraceW(
                static_cast<TRACEHANDLE>(ownedSessionHandle),
                loggerNamePointer,
                properties,
                EVENT_TRACE_CONTROL_STOP);
        }

        QMetaObject::invokeMethod(qApp, [guardThis, processStatus]() {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_captureRunning.store(false);
            guardThis->m_capturePaused.store(false);
            if (guardThis->m_runtimeRefreshTimer != nullptr)
            {
                guardThis->m_runtimeRefreshTimer->stop();
            }
            if (guardThis->m_uiUpdateTimer != nullptr)
            {
                guardThis->m_uiUpdateTimer->stop();
            }
            kPro.set(guardThis->m_captureProgressPid, "进程定向监控结束", 0, 100.0f);

            if (guardThis->m_uiUpdateTimer != nullptr)
            {
                guardThis->flushPendingRows();
            }

            if (processStatus == ERROR_SUCCESS)
            {
                guardThis->updateActionState();
                guardThis->updateStatusLabel();
            }
            else
            {
                guardThis->updateActionState();
                guardThis->updateStatusLabel();
                QMessageBox::warning(
                    guardThis,
                    QStringLiteral("进程定向监控"),
                    QStringLiteral("ProcessTrace 结束，状态码=%1。").arg(processStatus));
            }
        }, Qt::QueuedConnection);
    });
}

void ProcessTraceMonitorWidget::stopMonitoring()
{
    stopMonitoringInternal(false);
}

void ProcessTraceMonitorWidget::stopMonitoringInternal(const bool waitForThread)
{
    m_captureStopFlag.store(true);

    const std::uint64_t ownedTraceHandle = m_traceHandle.exchange(0);
    if (ownedTraceHandle != 0)
    {
        ::CloseTrace(static_cast<TRACEHANDLE>(ownedTraceHandle));
    }

    const std::uint64_t ownedSessionHandle = m_sessionHandle.exchange(0);
    if (ownedSessionHandle != 0)
    {
        const std::wstring sessionNameWide = m_sessionName.toStdWString();
        const ULONG propertyBufferSize = static_cast<ULONG>(
            sizeof(EVENT_TRACE_PROPERTIES) + (sessionNameWide.size() + 1) * sizeof(wchar_t));
        std::vector<unsigned char> propertyBuffer(propertyBufferSize, 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());
        properties->Wnode.BufferSize = propertyBufferSize;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        wchar_t* loggerNamePointer = reinterpret_cast<wchar_t*>(propertyBuffer.data() + properties->LoggerNameOffset);
        if (!sessionNameWide.empty())
        {
            ::wcscpy_s(loggerNamePointer, sessionNameWide.size() + 1, sessionNameWide.c_str());
        }

        ::ControlTraceW(
            static_cast<TRACEHANDLE>(ownedSessionHandle),
            loggerNamePointer,
            properties,
            EVENT_TRACE_CONTROL_STOP);
    }

    if (m_runtimeRefreshTimer != nullptr && m_runtimeRefreshTimer->isActive())
    {
        m_runtimeRefreshTimer->stop();
    }
    if (m_uiUpdateTimer != nullptr && m_uiUpdateTimer->isActive())
    {
        m_uiUpdateTimer->stop();
    }

    if (m_captureThread == nullptr || !m_captureThread->joinable())
    {
        m_captureThread.reset();
        m_captureRunning.store(false);
        m_capturePaused.store(false);
        updateActionState();
        updateStatusLabel();
        return;
    }

    if (waitForThread)
    {
        m_captureThread->join();
        m_captureThread.reset();
        m_captureRunning.store(false);
        m_capturePaused.store(false);
        updateActionState();
        updateStatusLabel();
        return;
    }

    std::unique_ptr<std::thread> joinThread = std::move(m_captureThread);
    QPointer<ProcessTraceMonitorWidget> guardThis(this);
    std::thread([joinThread = std::move(joinThread), guardThis]() mutable {
        if (joinThread != nullptr && joinThread->joinable())
        {
            joinThread->join();
        }

        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_captureRunning.store(false);
            guardThis->m_capturePaused.store(false);
            guardThis->updateActionState();
            guardThis->updateStatusLabel();
        }, Qt::QueuedConnection);
    }).detach();
}

void ProcessTraceMonitorWidget::setMonitoringPaused(const bool paused)
{
    if (!m_captureRunning.load())
    {
        return;
    }

    m_capturePaused.store(paused);
    updateActionState();
    updateStatusLabel();

    kLogEvent event;
    info << event
        << "[ProcessTraceMonitorWidget] 监控暂停状态变更, paused="
        << (paused ? "true" : "false")
        << eol;
}

void ProcessTraceMonitorWidget::refreshTrackedProcessSnapshotAsync()
{
    if (!m_captureRunning.load() || m_runtimeRefreshPending.exchange(true))
    {
        return;
    }

    QPointer<ProcessTraceMonitorWidget> guardThis(this);
    std::thread([guardThis]() {
        std::vector<ks::process::ProcessRecord> processList = ks::process::EnumerateProcesses(
            ks::process::ProcessEnumStrategy::Auto);
        QMetaObject::invokeMethod(qApp, [guardThis, processList = std::move(processList)]() {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_runtimeRefreshPending.store(false);
            guardThis->syncTrackedProcessTree(processList);
            guardThis->refreshTargetTable();
            guardThis->updateStatusLabel();
        }, Qt::QueuedConnection);
    }).detach();
}

void ProcessTraceMonitorWidget::seedTrackedProcessTree(const std::vector<ks::process::ProcessRecord>& processList)
{
    std::lock_guard<std::mutex> lock(m_runtimeMutex);
    m_trackedProcessMap.clear();

    for (TargetProcessEntry& targetEntry : m_targetProcessList)
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

        m_trackedProcessMap[trackedProcess.pid] = trackedProcess;
    }

    bool added = true;
    while (added)
    {
        added = false;
        for (const ks::process::ProcessRecord& record : processList)
        {
            if (record.pid == 0 || m_trackedProcessMap.find(record.pid) != m_trackedProcessMap.end())
            {
                continue;
            }

            const auto parentIt = m_trackedProcessMap.find(record.parentPid);
            if (parentIt == m_trackedProcessMap.end())
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
            m_trackedProcessMap[trackedProcess.pid] = trackedProcess;
            added = true;
        }
    }
}

void ProcessTraceMonitorWidget::syncTrackedProcessTree(const std::vector<ks::process::ProcessRecord>& processList)
{
    std::lock_guard<std::mutex> lock(m_runtimeMutex);

    for (auto& [pidValue, trackedProcess] : m_trackedProcessMap)
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

            auto existingIt = m_trackedProcessMap.find(record.pid);
            if (existingIt != m_trackedProcessMap.end())
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

                    const auto parentIt = m_trackedProcessMap.find(record.parentPid);
                    if (parentIt == m_trackedProcessMap.end())
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

            const auto parentIt = m_trackedProcessMap.find(record.parentPid);
            if (parentIt == m_trackedProcessMap.end())
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
            m_trackedProcessMap[trackedProcess.pid] = trackedProcess;
            added = true;
        }
    }

    pruneStaleTrackedProcesses();

    for (TargetProcessEntry& targetEntry : m_targetProcessList)
    {
        const auto trackedIt = m_trackedProcessMap.find(targetEntry.pid);
        if (trackedIt == m_trackedProcessMap.end())
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
}

void ProcessTraceMonitorWidget::pruneStaleTrackedProcesses()
{
    FILETIME fileTimeValue{};
    ::GetSystemTimeAsFileTime(&fileTimeValue);
    ULARGE_INTEGER currentTimeValue{};
    currentTimeValue.LowPart = fileTimeValue.dwLowDateTime;
    currentTimeValue.HighPart = fileTimeValue.dwHighDateTime;
    const std::uint64_t currentTime100ns = static_cast<std::uint64_t>(currentTimeValue.QuadPart);
    const std::uint64_t kRecentEventKeepWindow100ns = 15ULL * 1000ULL * 1000ULL * 10ULL;

    std::vector<std::uint32_t> pidListNeedErase;
    pidListNeedErase.reserve(m_trackedProcessMap.size());

    for (auto& [pidValue, trackedProcess] : m_trackedProcessMap)
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
        m_trackedProcessMap.erase(pidValue);
    }
}
