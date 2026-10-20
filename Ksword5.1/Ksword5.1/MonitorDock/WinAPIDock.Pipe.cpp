#include "WinAPIDock.h"
#include "../UI/ThemeItemForeground.h"
#include "../theme.h"
#include "../Framework/PrivilegeElevationPrompt.h"
#include "../UI/TableInteractionSupport.h"

// ============================================================
// WinAPIDock.Pipe.cpp
// 作用：
// 1) 实现与 APIMonitor_x64 命名管道服务端的连接与读取；
// 2) 把后台收到的固定长度事件包转换成 UI 可显示的 EventRow；
// 3) 统一处理后台线程退出、连接失败和事件表批量刷新。
// ============================================================

#include <QApplication>
#include <QAbstractItemModel>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QCryptographicHash>
#include <QMetaObject>
#include <QPointer>
#include <QTableWidget>
#include <QTableWidgetItem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>

namespace
{
    // kApiMonitorPacketSize：
    // - 作用：统一 UI 侧命名管道每次读取的固定包大小；
    // - 调用：主目标管道与自动注入子进程管道复用同一协议结构。
    constexpr DWORD kApiMonitorPacketSize = static_cast<DWORD>(sizeof(ks::winapi_monitor::ApiMonitorEventPacket));

    // packetWideText：
    // - 作用：把固定长度 wchar_t 缓冲转成 QString；
    // - 调用：解析 Agent 回传的 moduleName/apiName/detailText 时复用。
    template <std::size_t kCount>
    QString packetWideText(const wchar_t(&bufferValue)[kCount])
    {
        QString textValue = QString::fromWCharArray(bufferValue, static_cast<int>(kCount));
        const int nullIndex = textValue.indexOf(QChar(u'\0'));
        if (nullIndex >= 0)
        {
            textValue.truncate(nullIndex);
        }
        return textValue.trimmed();
    }

    // packetEventCategoryText：
    // - 作用：把协议分类码转换为 UI 文本；
    // - 调用：Pipe.cpp 本地解析主进程/子进程事件包时使用；
    // - 返回：分类中文名，未知分类返回“未知”。
    QString packetEventCategoryText(const std::uint32_t categoryValue)
    {
        switch (static_cast<ks::winapi_monitor::EventCategory>(categoryValue))
        {
        case ks::winapi_monitor::EventCategory::File:
            return QStringLiteral("文件");
        case ks::winapi_monitor::EventCategory::Registry:
            return QStringLiteral("注册表");
        case ks::winapi_monitor::EventCategory::Network:
            return QStringLiteral("网络");
        case ks::winapi_monitor::EventCategory::Process:
            return QStringLiteral("进程");
        case ks::winapi_monitor::EventCategory::Loader:
            return QStringLiteral("加载器");
        case ks::winapi_monitor::EventCategory::Clipboard:
            return QStringLiteral("剪贴板");
        case ks::winapi_monitor::EventCategory::Internal:
            return QStringLiteral("内部");
        default:
            break;
        }
        return QStringLiteral("未知");
    }

    // packetResultCodeText：
    // - 作用：把 Agent resultCode 转成表格文本；
    // - 调用：Pipe.cpp 本地解析事件包时使用，避免访问 WinAPIDock 私有静态函数；
    // - 返回：0 返回 OK，非 0 返回十进制错误码。
    QString packetResultCodeText(const std::int32_t resultCodeValue)
    {
        return resultCodeValue == 0
            ? QStringLiteral("OK")
            : QString::number(resultCodeValue);
    }

    // packetToEventRow：
    // - 作用：把 Agent 固定包转换成 WinAPIDock::EventRow；
    // - 调用：主进程管道和子进程管道读取线程共用；
    // - 返回：转换后的事件行，调用者负责入队或显示。
    WinAPIDock::EventRow packetToEventRow(const ks::winapi_monitor::ApiMonitorEventPacket& packetValue)
    {
        WinAPIDock::EventRow rowValue;
        rowValue.time100nsText = QString::number(static_cast<qulonglong>(packetValue.timestamp100ns));
        rowValue.categoryText = packetEventCategoryText(packetValue.category);

        const QString moduleNameText = packetWideText(packetValue.moduleName);
        const QString apiNameText = packetWideText(packetValue.apiName);
        rowValue.apiText = moduleNameText.trimmed().isEmpty()
            ? apiNameText
            : QStringLiteral("%1!%2").arg(moduleNameText, apiNameText);
        rowValue.resultKnown = packetValue.resultKind == static_cast<std::uint32_t>(ks::winapi_monitor::EventResultKind::StatusCode);
        rowValue.resultText = rowValue.resultKnown ? packetResultCodeText(packetValue.resultCode) : QStringLiteral("未采集");
        rowValue.pidTidText = QStringLiteral("%1 / %2").arg(packetValue.pid).arg(packetValue.tid);
        rowValue.detailText = packetWideText(packetValue.detailText);
        QString phase;
        switch (static_cast<ks::winapi_monitor::EventKind>(packetValue.eventKind)) {
        case ks::winapi_monitor::EventKind::IoSubmit: phase = QObject::tr("提交"); break;
        case ks::winapi_monitor::EventKind::IoWait: phase = QObject::tr("等待完成"); break;
        case ks::winapi_monitor::EventKind::IoComplete: phase = QObject::tr("完成"); break;
        case ks::winapi_monitor::EventKind::IoCancel: phase = QObject::tr("取消结果"); break;
        default: break;
        }
        if (!phase.isEmpty()) rowValue.detailText.prepend(QStringLiteral("[%1 #%2] ").arg(phase).arg(packetValue.operationId));
        rowValue.sourcePid = packetValue.pid;
        if (packetValue.category == static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal)
            && moduleNameText == QStringLiteral("Agent")) rowValue.agentStatus = apiNameText;
        rowValue.internalEvent =
            packetValue.category == static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal);
        return rowValue;
    }

    // tryExtractAutoInjectChildPid：
    // - 作用：从 Agent 内部 AutoInjectChild 事件详情中提取 childPid；
    // - 调用：主目标管道收到子进程注入成功事件后，启动对应子管道读取线程；
    // - 返回：成功解析返回 true，并通过 childPidOut 输出 PID。
    bool tryExtractAutoInjectChildPid(
        const ks::winapi_monitor::ApiMonitorEventPacket& packetValue,
        std::uint32_t* const childPidOut)
    {
        if (childPidOut == nullptr)
        {
            return false;
        }
        *childPidOut = 0;

        const QString apiNameText = packetWideText(packetValue.apiName);
        if (packetValue.category != static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal)
            || apiNameText != QStringLiteral("AutoInjectChild"))
        {
            return false;
        }

        const QString detailText = packetWideText(packetValue.detailText);
        const QString markerText = QStringLiteral("childPid=");
        const int markerIndex = detailText.indexOf(markerText);
        if (markerIndex < 0)
        {
            return false;
        }

        int valueStart = markerIndex + markerText.size();
        int valueEnd = valueStart;
        while (valueEnd < detailText.size() && detailText.at(valueEnd).isDigit())
        {
            ++valueEnd;
        }

        bool parseOk = false;
        const std::uint32_t pidValue = detailText.mid(valueStart, valueEnd - valueStart).toUInt(&parseOk, 10);
        if (!parseOk || pidValue == 0)
        {
            return false;
        }

        *childPidOut = pidValue;
        return true;
    }
}

void WinAPIDock::startPipeReadThread()
{
    if (m_pipeThread != nullptr && m_pipeThread->joinable())
    {
        m_pipeThread->join();
        m_pipeThread.reset();
    }

    const QString pipeNameText = m_currentPipeName;
    const std::uint32_t sessionPidValue = m_currentSessionPid;
    const auto expectedSession = ks::winapi_monitor::sessionIdentity(m_currentSessionId.toStdWString());
    const std::uint64_t generation = m_sessionGeneration.load();
    QPointer<WinAPIDock> guardThis(this);

    m_pipeThread = std::make_unique<std::thread>([guardThis, generation, pipeNameText, sessionPidValue, expectedSession]() {
        HANDLE pipeHandle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 120; ++attempt)
        {
            if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation || guardThis->m_pipeStopFlag.load())
            {
                return;
            }

            pipeHandle = ::CreateFileW(
                reinterpret_cast<LPCWSTR>(pipeNameText.utf16()),
                GENERIC_READ,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (pipeHandle != INVALID_HANDLE_VALUE)
            {
                break;
            }

            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PIPE_BUSY)
            {
                QMetaObject::invokeMethod(qApp, [guardThis, generation, lastError]() {
                    if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
                    {
                        return;
                    }
                    (void)ks::ui::promptForPrivilegeFailure(
                        guardThis,
                        QStringLiteral("连接 API 监控命名管道"),
                        lastError);
                    guardThis->appendInternalEvent(
                        QStringLiteral("内部"),
                        QStringLiteral("命名管道连接失败"),
                        QStringLiteral("CreateFileW 返回错误码 %1。").arg(lastError));
                    guardThis->updateActionState();
                    guardThis->updateStatusLabel();
                }, Qt::QueuedConnection);
                guardThis->m_pipeRunning.store(false);
                return;
            }

            ::WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipeNameText.utf16()), 250);
            ::Sleep(120);
        }

        if (pipeHandle == INVALID_HANDLE_VALUE)
        {
            QMetaObject::invokeMethod(qApp, [guardThis, generation]() {
                if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
                {
                    return;
                }
                guardThis->appendInternalEvent(
                    QStringLiteral("内部"),
                    QStringLiteral("命名管道连接超时"),
                    QStringLiteral("在限定时间内未等到 Agent 建立管道服务端。"));
                guardThis->m_pipeRunning.store(false);
                guardThis->m_pipeConnected.store(false);
                guardThis->updateActionState();
                guardThis->updateStatusLabel();
            }, Qt::QueuedConnection);
            return;
        }

        guardThis->m_pipeHandleValue.store(reinterpret_cast<std::uintptr_t>(pipeHandle));
        guardThis->m_pipeConnected.store(true);
        kPro.set(guardThis->m_sessionProgressPid, "Agent 已连接，开始接收 WinAPI 事件", 0, 70.0f);

        QMetaObject::invokeMethod(qApp, [guardThis, generation, sessionPidValue]() {
            if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
            {
                return;
            }
            guardThis->appendInternalEvent(
                QStringLiteral("内部"),
                QStringLiteral("管道已连接"),
                QStringLiteral("已与 PID=%1 的 Agent 建立命名管道连接。").arg(sessionPidValue));
            guardThis->m_hookState = HookState::Installing;
            guardThis->updateActionState();
            guardThis->updateStatusLabel();
        }, Qt::QueuedConnection);

        ks::winapi_monitor::CoverageSnapshotReceiver coverageReceiver(sessionPidValue, expectedSession);
        while (!guardThis->m_pipeStopFlag.load())
        {
            ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
            if (!ks::winapi_monitor::readEventPacket(pipeHandle, &packetValue, guardThis->m_pipeStopFlag))
            {
                break;
            }
            if (packetValue.size != sizeof(packetValue)
                || packetValue.version != ks::winapi_monitor::kProtocolVersion)
            {
                continue;
            }

            if (packetValue.sessionIdentity != expectedSession) continue;
            if (ks::winapi_monitor::isCoverageEvent(packetValue.eventKind))
            {
                if (coverageReceiver.consume(packetValue))
                    guardThis->queueCoverageSnapshot(sessionPidValue, expectedSession, coverageReceiver.revision(), coverageReceiver.items(), false, generation);
                else if (coverageReceiver.stale())
                    guardThis->queueCoverageSnapshot(sessionPidValue, expectedSession, coverageReceiver.revision(), {}, true, generation);
                continue;
            }
            std::uint32_t childPidValue = 0;
            if (tryExtractAutoInjectChildPid(packetValue, &childPidValue))
            {
                QMetaObject::invokeMethod(qApp, [guardThis, generation, childPidValue]() {
                    if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
                    {
                        return;
                    }
                    guardThis->startChildPipeReadThread(childPidValue);
                }, Qt::QueuedConnection);
            }

            guardThis->enqueuePendingRow(packetToEventRow(packetValue), generation);
        }

        guardThis->queueCoverageSnapshot(sessionPidValue, expectedSession, coverageReceiver.revision(), {}, true, generation);
        const std::uintptr_t storedHandleValue = guardThis->m_pipeHandleValue.exchange(0);
        if (storedHandleValue != 0)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(storedHandleValue));
        }

        const bool retryConnection = !guardThis->m_pipeStopFlag.load()
            && guardThis->m_pipeRunning.load()
            && guardThis->m_pipeReconnectAttempts.fetch_add(1) < kPipeReconnectLimit;

        QMetaObject::invokeMethod(qApp, [guardThis, generation, retryConnection]() {
            if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
            {
                return;
            }
            guardThis->m_pipeConnected.store(false);
            guardThis->m_hookState = HookState::Waiting;
            if (retryConnection && !guardThis->m_pipeStopFlag.load() && guardThis->m_pipeRunning.load())
            {
                guardThis->appendInternalEvent(
                    QStringLiteral("内部"),
                    QStringLiteral("管道短暂断开"),
                    QStringLiteral("Agent 正在切换会话，正在重新连接命名管道。"));
                guardThis->startPipeReadThread();
                return;
            }
            if (guardThis->m_pipeStopFlag.load())
            {
                guardThis->appendInternalEvent(
                    QStringLiteral("内部"),
                    QStringLiteral("会话结束"),
                    QStringLiteral("本地已停止接收 WinAPI 事件。"));
            }
            else
            {
                guardThis->appendInternalEvent(
                    QStringLiteral("内部"),
                    QStringLiteral("管道已断开"),
                    QStringLiteral("Agent 主动关闭或目标进程已退出。"));
            }
            guardThis->m_pipeRunning.store(false);
            guardThis->updateActionState();
            guardThis->updateStatusLabel();
            kPro.set(guardThis->m_sessionProgressPid, "WinAPI 事件接收结束", 0, 100.0f);
        }, Qt::QueuedConnection);
    });
}

void WinAPIDock::startChildPipeReadThread(const std::uint32_t childPidValue)
{
    if (childPidValue == 0 || childPidValue == m_currentSessionPid || m_pipeStopFlag.load())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_childPipeMutex);
        if (std::find(m_childSessionPids.begin(), m_childSessionPids.end(), childPidValue) != m_childSessionPids.end())
        {
            return;
        }
    }

    ks::winapi_monitor::SessionLease lease;
    DWORD leaseError = 0;
    if (!lease.acquire(childPidValue, &leaseError))
    {
        appendInternalEvent(QStringLiteral("内部"), QStringLiteral("子进程管道连接失败"),
            QStringLiteral("无法占用 PID=%1 的 Agent 会话（错误码 %2）；该进程可能已由 API 监控或剪贴板保护占用。").arg(childPidValue).arg(leaseError));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_childPipeMutex);
        if (std::find(m_childSessionPids.begin(), m_childSessionPids.end(), childPidValue) != m_childSessionPids.end())
        {
            return;
        }
        m_childSessionPids.push_back(childPidValue);
        m_childSessionLeases.push_back(std::move(lease));
    }

    const QString pipeNameText = QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(childPidValue));
    wchar_t childSession[1024]{};
    const auto configPath = ks::winapi_monitor::buildConfigPathForPid(childPidValue);
    ::GetPrivateProfileStringW(L"monitor", L"session_id", L"", childSession, 1024, configPath.c_str());
    const std::wstring childSessionText(childSession);
    if (childSessionText.rfind(m_currentSessionId.toStdWString() + L"_", 0) != 0) return;
    const auto expectedSession = ks::winapi_monitor::sessionIdentity(childSessionText);
    const std::uint64_t generation = m_sessionGeneration.load();
    QPointer<WinAPIDock> guardThis(this);
    auto childThread = std::make_unique<std::thread>([guardThis, generation, pipeNameText, childPidValue, expectedSession]() {
        HANDLE pipeHandle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 120; ++attempt)
        {
            if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation || guardThis->m_pipeStopFlag.load())
            {
                return;
            }

            pipeHandle = ::CreateFileW(
                reinterpret_cast<LPCWSTR>(pipeNameText.utf16()),
                GENERIC_READ,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (pipeHandle != INVALID_HANDLE_VALUE)
            {
                break;
            }

            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PIPE_BUSY)
            {
                QMetaObject::invokeMethod(qApp, [guardThis, generation, childPidValue, lastError]() {
                    if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
                    {
                        return;
                    }
                    (void)ks::ui::promptForPrivilegeFailure(
                        guardThis,
                        QStringLiteral("连接子进程 API 监控命名管道"),
                        lastError);
                    guardThis->appendInternalEvent(
                        QStringLiteral("内部"),
                        QStringLiteral("子进程管道连接失败"),
                        QStringLiteral("PID=%1 CreateFileW 错误码 %2。").arg(childPidValue).arg(lastError));
                }, Qt::QueuedConnection);
                return;
            }

            ::WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipeNameText.utf16()), 250);
            ::Sleep(120);
        }

        if (pipeHandle == INVALID_HANDLE_VALUE)
        {
            QMetaObject::invokeMethod(qApp, [guardThis, generation, childPidValue]() {
                if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
                {
                    return;
                }
                guardThis->appendInternalEvent(
                    QStringLiteral("内部"),
                    QStringLiteral("子进程管道连接超时"),
                    QStringLiteral("PID=%1 的 Agent 管道未在限定时间内建立。").arg(childPidValue));
            }, Qt::QueuedConnection);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(guardThis->m_childPipeMutex);
            guardThis->m_childPipeHandleValues.push_back(reinterpret_cast<std::uintptr_t>(pipeHandle));
        }

        QMetaObject::invokeMethod(qApp, [guardThis, generation, childPidValue]() {
            if (guardThis == nullptr || guardThis->m_sessionGeneration.load() != generation)
            {
                return;
            }
            guardThis->appendInternalEvent(
                QStringLiteral("内部"),
                QStringLiteral("子进程管道已连接"),
                QStringLiteral("已开始接收 PID=%1 的自动注入 Agent 事件。").arg(childPidValue));
        }, Qt::QueuedConnection);

        ks::winapi_monitor::CoverageSnapshotReceiver coverageReceiver(childPidValue, expectedSession);
        while (!guardThis->m_pipeStopFlag.load())
        {
            ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
            if (!ks::winapi_monitor::readEventPacket(pipeHandle, &packetValue, guardThis->m_pipeStopFlag))
            {
                break;
            }
            if (packetValue.size != sizeof(packetValue)
                || packetValue.version != ks::winapi_monitor::kProtocolVersion)
            {
                continue;
            }

            if (packetValue.sessionIdentity != expectedSession) continue;
            if (ks::winapi_monitor::isCoverageEvent(packetValue.eventKind))
            {
                if (coverageReceiver.consume(packetValue))
                    guardThis->queueCoverageSnapshot(childPidValue, expectedSession, coverageReceiver.revision(), coverageReceiver.items(), false, generation);
                else if (coverageReceiver.stale())
                    guardThis->queueCoverageSnapshot(childPidValue, expectedSession, coverageReceiver.revision(), {}, true, generation);
                continue;
            }
            std::uint32_t descendantPid = 0;
            if (tryExtractAutoInjectChildPid(packetValue, &descendantPid))
            {
                QMetaObject::invokeMethod(qApp, [guardThis, generation, descendantPid]() {
                    if (!guardThis || guardThis->m_sessionGeneration.load() != generation) return;
                    guardThis->startChildPipeReadThread(descendantPid);
                }, Qt::QueuedConnection);
            }
            guardThis->enqueuePendingRow(packetToEventRow(packetValue), generation);
        }

        bool shouldClosePipeHandle = false;
        {
            std::lock_guard<std::mutex> lock(guardThis->m_childPipeMutex);
            auto& handleList = guardThis->m_childPipeHandleValues;
            const std::uintptr_t handleValue = reinterpret_cast<std::uintptr_t>(pipeHandle);
            const auto handleIt = std::find(handleList.begin(), handleList.end(), handleValue);
            if (handleIt != handleList.end())
            {
                handleList.erase(handleIt);
                shouldClosePipeHandle = true;
            }
        }
        if (shouldClosePipeHandle)
        {
            guardThis->queueCoverageSnapshot(childPidValue, expectedSession, coverageReceiver.revision(), {}, true, generation);
            ::CloseHandle(pipeHandle);
        }
    });

    std::lock_guard<std::mutex> lock(m_childPipeMutex);
    m_childPipeThreads.push_back(std::move(childThread));
}

void WinAPIDock::closeChildPipeHandles()
{
    std::vector<std::uintptr_t> handleList;
    {
        std::lock_guard<std::mutex> lock(m_childPipeMutex);
        handleList.swap(m_childPipeHandleValues);
    }

    for (const std::uintptr_t handleValue : handleList)
    {
        if (handleValue != 0)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(handleValue));
        }
    }
}

void WinAPIDock::joinChildPipeThreads()
{
    std::vector<std::unique_ptr<std::thread>> threadList;
    {
        std::lock_guard<std::mutex> lock(m_childPipeMutex);
        threadList.swap(m_childPipeThreads);
    }

    for (std::unique_ptr<std::thread>& threadPointer : threadList)
    {
        if (threadPointer != nullptr && threadPointer->joinable())
        {
            // Each child reader owns a synchronous ReadFile on its pipe handle.
            (void)::CancelSynchronousIo(threadPointer->native_handle());
        }
    }

    for (std::unique_ptr<std::thread>& threadPointer : threadList)
    {
        if (threadPointer != nullptr && threadPointer->joinable())
        {
            threadPointer->join();
        }
    }
}

void WinAPIDock::writeChildStopFlags()
{
    std::vector<std::uint32_t> childPidList;
    {
        std::lock_guard<std::mutex> lock(m_childPipeMutex);
        childPidList = m_childSessionPids;
    }

    for (const std::uint32_t childPidValue : childPidList)
    {
        const QString stopFlagPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(childPidValue));
        QFile stopFile(stopFlagPath);
        if (stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            stopFile.write("stop");
            stopFile.close();
            std::wstring aclError;
            (void)ks::winapi_monitor::grantSessionFileRead(
                stopFlagPath.toStdWString(), childPidValue, &aclError);
            if (!m_currentStopFlagPath.trimmed().isEmpty())
            {
                (void)ks::winapi_monitor::grantSessionFileRead(
                    m_currentStopFlagPath.toStdWString(), childPidValue, &aclError);
            }
        }
    }
}

void WinAPIDock::enqueuePendingRow(EventRow rowValue, const std::uint64_t generation)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if (generation != m_sessionGeneration.load()) return;
    if (rowValue.agentStatus == QStringLiteral("EventsDropped"))
    {
        bool ok = false;
        const auto count = rowValue.detailText.toULongLong(&ok);
        if (ok) m_agentDroppedRows[rowValue.sourcePid] = std::max<std::uint64_t>(m_agentDroppedRows[rowValue.sourcePid], count);
    }
    if (m_pendingRows.size() >= kPendingRowCapacity)
    {
        const auto ordinary = std::find_if(m_pendingRows.begin(), m_pendingRows.end(), [](const EventRow& row) { return !row.internalEvent; });
        if (ordinary != m_pendingRows.end()) m_pendingRows.erase(ordinary);
        else m_pendingRows.pop_front();
        ++m_pendingDroppedRows;
    }
    m_pendingRows.push_back(std::move(rowValue));
}

void WinAPIDock::flushPendingRows()
{
    flushCoverageUpdates();
    // 表格菜单打开时不提前取走管道事件；否则同键刷新被覆盖后，已经 drain 的
    // EventRow 会丢失，同时 removeRows(0, ...) 会让菜单保存的行号失效。
    const QPointer<WinAPIDock> guardThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("winapi-event-flush"),
        { m_eventTable },
        [guardThis]()
        {
            if (!guardThis.isNull())
            {
                guardThis->flushPendingRows();
            }
        }))
    {
        return;
    }

    std::vector<EventRow> rowList;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        const std::size_t takeCount = std::min(kUiFlushRowLimit, m_pendingRows.size());
        rowList.reserve(takeCount);
        for (std::size_t index = 0; index < takeCount; ++index)
        {
            rowList.push_back(std::move(m_pendingRows.front()));
            m_pendingRows.pop_front();
        }
    }

    if (rowList.empty())
    {
        updateStatusLabel();
        return;
    }

    QElapsedTimer budgetTimer;
    budgetTimer.start();

    std::size_t renderedCount = 0;
    if (m_eventTable != nullptr)
    {
        const bool updatesEnabled = m_eventTable->updatesEnabled();
        m_eventTable->setUpdatesEnabled(false);

        for (const EventRow& rowValue : rowList)
        {
            appendEventRow(rowValue);
            ++renderedCount;
            if (budgetTimer.elapsed() >= kUiFlushBudgetMs)
            {
                break;
            }
        }

        const int removeCount = std::max(0, m_eventTable->rowCount() - 12000);
        if (removeCount > 0 && m_eventTable->model() != nullptr)
        {
            for (int row = 0; row < removeCount; ++row)
                if (!m_eventTable->isRowHidden(row)) --m_visibleEventCount;
            m_evictedRows += removeCount;
            m_eventTable->model()->removeRows(0, removeCount);
        }

        m_eventTable->setUpdatesEnabled(updatesEnabled);
        if (updatesEnabled && m_eventTable->viewport() != nullptr)
        {
            m_eventTable->viewport()->update();
        }
    }

    if (renderedCount < rowList.size())
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        for (std::size_t index = rowList.size(); index > renderedCount; --index)
        {
            m_pendingRows.push_front(std::move(rowList[index - 1]));
        }

        while (m_pendingRows.size() > kPendingRowCapacity)
        {
            m_pendingRows.pop_back();
            ++m_pendingDroppedRows;
        }
    }

    applyEventFilter();
    updateActionState();
    updateStatusLabel();

    if (m_eventKeepBottomCheck != nullptr
        && m_eventKeepBottomCheck->isChecked()
        && m_eventTable != nullptr)
    {
        m_eventTable->scrollToBottom();
    }
}

void WinAPIDock::appendEventRow(const EventRow& rowValue)
{
    if (rowValue.sourcePid == m_currentSessionPid && !rowValue.agentStatus.isEmpty())
    {
        if (rowValue.agentStatus == QStringLiteral("SessionReady"))
        {
            m_hookState = HookState::Installing;
            const QString marker = QString::fromLatin1("definitionSha256=");
            const QString remoteHash = rowValue.detailText.startsWith(marker) ? rowValue.detailText.mid(marker.size(), 64) : QString();
            m_definitionMismatch = m_coverageDefinitionHash.isEmpty() || remoteHash.size() != 64
                || remoteHash.toLatin1() != m_coverageDefinitionHash;
        }
        else if (rowValue.agentStatus == QStringLiteral("InstallHooksFailed")) m_hookState = HookState::Failed;
        else if (rowValue.agentStatus == QStringLiteral("HooksPartial")) m_hookState = HookState::Partial;
        else if (rowValue.agentStatus == QStringLiteral("HooksInstalled"))
        {
            if (m_hookState != HookState::Partial) m_hookState = HookState::Active;
            kPro.set(m_sessionProgressPid, "WinAPI Hook 安装完成", 0, 100.0f);
        }
        else if (rowValue.agentStatus == QStringLiteral("HooksRemoved")) m_hookState = HookState::Removed;
        if (m_hookState == HookState::Failed) kPro.set(m_sessionProgressPid, "WinAPI Hook 安装失败", 0, 100.0f);
    }
    if (m_eventTable == nullptr)
    {
        return;
    }

    const int row = m_eventTable->rowCount();
    m_eventTable->insertRow(row);

    QTableWidgetItem* timeItem = createReadOnlyItem(rowValue.time100nsText);
    QTableWidgetItem* categoryItem = createReadOnlyItem(rowValue.categoryText);
    QTableWidgetItem* apiItem = createReadOnlyItem(rowValue.apiText);
    QTableWidgetItem* resultItem = createReadOnlyItem(rowValue.resultText);
    QTableWidgetItem* pidTidItem = createReadOnlyItem(rowValue.pidTidText);
    QTableWidgetItem* detailItem = createReadOnlyItem(rowValue.detailText);

    if (rowValue.internalEvent)
    {
        ks::ui::ApplyThemeItemForeground(categoryItem, ks::ui::ItemForegroundRole::Info);
        ks::ui::ApplyThemeItemForeground(apiItem, ks::ui::ItemForegroundRole::Info);
    }
    else if (rowValue.resultKnown && rowValue.resultText != QStringLiteral("OK"))
    {
        ks::ui::ApplyThemeItemForeground(resultItem, ks::ui::ItemForegroundRole::Error);
    }

    m_eventTable->setItem(row, EventColumnTime100ns, timeItem);
    m_eventTable->setItem(row, EventColumnCategory, categoryItem);
    m_eventTable->setItem(row, EventColumnApi, apiItem);
    m_eventTable->setItem(row, EventColumnResult, resultItem);
    m_eventTable->setItem(row, EventColumnPidTid, pidTidItem);
    m_eventTable->setItem(row, EventColumnDetail, detailItem);
    const QString searchText = QStringList{rowValue.time100nsText, rowValue.categoryText, rowValue.apiText,
        rowValue.resultText, rowValue.pidTidText, rowValue.detailText}.join(QStringLiteral(" | "));
    timeItem->setData(Qt::UserRole, searchText);
    const bool visible = m_eventFilterKeyword.isEmpty() || searchText.contains(m_eventFilterKeyword, Qt::CaseInsensitive);
    m_eventTable->setRowHidden(row, !visible);
    if (visible) ++m_visibleEventCount;
}
