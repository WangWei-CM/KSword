#include "WinAPIDock.h"

// ============================================================
// WinAPIDock.Pipe.cpp
// 作用：
// 1) 实现与 APIMonitor_x64 命名管道服务端的连接与读取；
// 2) 把后台收到的固定长度事件包转换成 UI 可显示的 EventRow；
// 3) 统一处理后台线程退出、连接失败和事件表批量刷新。
// ============================================================

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QFile>
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
        rowValue.resultText = packetResultCodeText(packetValue.resultCode);
        rowValue.pidTidText = QStringLiteral("%1 / %2").arg(packetValue.pid).arg(packetValue.tid);
        rowValue.detailText = packetWideText(packetValue.detailText);
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
    QPointer<WinAPIDock> guardThis(this);

    m_pipeThread = std::make_unique<std::thread>([guardThis, pipeNameText, sessionPidValue]() {
        HANDLE pipeHandle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 120; ++attempt)
        {
            if (guardThis == nullptr || guardThis->m_pipeStopFlag.load())
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
                QMetaObject::invokeMethod(qApp, [guardThis, lastError]() {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->appendInternalEvent(
                        QStringLiteral("内部"),
                        QStringLiteral("命名管道连接失败"),
                        QStringLiteral("CreateFileW 返回错误码 %1。").arg(lastError));
                }, Qt::QueuedConnection);
                guardThis->m_pipeRunning.store(false);
                return;
            }

            ::WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipeNameText.utf16()), 250);
            ::Sleep(120);
        }

        if (pipeHandle == INVALID_HANDLE_VALUE)
        {
            QMetaObject::invokeMethod(qApp, [guardThis]() {
                if (guardThis == nullptr)
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

        QMetaObject::invokeMethod(qApp, [guardThis, sessionPidValue]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->appendInternalEvent(
                QStringLiteral("内部"),
                QStringLiteral("管道已连接"),
                QStringLiteral("已与 PID=%1 的 Agent 建立命名管道连接。").arg(sessionPidValue));
            guardThis->updateActionState();
            guardThis->updateStatusLabel();
        }, Qt::QueuedConnection);

        while (!guardThis->m_pipeStopFlag.load())
        {
            ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
            DWORD bytesRead = 0;
            const BOOL readOk = ::ReadFile(
                pipeHandle,
                &packetValue,
                kApiMonitorPacketSize,
                &bytesRead,
                nullptr);
            if (readOk == FALSE || bytesRead == 0)
            {
                break;
            }
            if (bytesRead < sizeof(packetValue)
                || packetValue.size != sizeof(packetValue)
                || packetValue.version != ks::winapi_monitor::kProtocolVersion)
            {
                continue;
            }

            std::uint32_t childPidValue = 0;
            if (tryExtractAutoInjectChildPid(packetValue, &childPidValue))
            {
                QMetaObject::invokeMethod(qApp, [guardThis, childPidValue]() {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->startChildPipeReadThread(childPidValue);
                }, Qt::QueuedConnection);
            }

            EventRow rowValue = packetToEventRow(packetValue);

            {
                std::lock_guard<std::mutex> lock(guardThis->m_pendingMutex);
                guardThis->m_pendingRows.push_back(std::move(rowValue));
            }
        }

        const std::uintptr_t storedHandleValue = guardThis->m_pipeHandleValue.exchange(0);
        if (storedHandleValue != 0)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(storedHandleValue));
        }

        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_pipeConnected.store(false);
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
        m_childSessionPids.push_back(childPidValue);
    }

    const QString pipeNameText = QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(childPidValue));
    QPointer<WinAPIDock> guardThis(this);
    auto childThread = std::make_unique<std::thread>([guardThis, pipeNameText, childPidValue]() {
        HANDLE pipeHandle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 120; ++attempt)
        {
            if (guardThis == nullptr || guardThis->m_pipeStopFlag.load())
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
                QMetaObject::invokeMethod(qApp, [guardThis, childPidValue, lastError]() {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
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
            QMetaObject::invokeMethod(qApp, [guardThis, childPidValue]() {
                if (guardThis == nullptr)
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

        QMetaObject::invokeMethod(qApp, [guardThis, childPidValue]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->appendInternalEvent(
                QStringLiteral("内部"),
                QStringLiteral("子进程管道已连接"),
                QStringLiteral("已开始接收 PID=%1 的自动注入 Agent 事件。").arg(childPidValue));
        }, Qt::QueuedConnection);

        while (!guardThis->m_pipeStopFlag.load())
        {
            ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
            DWORD bytesRead = 0;
            const BOOL readOk = ::ReadFile(
                pipeHandle,
                &packetValue,
                kApiMonitorPacketSize,
                &bytesRead,
                nullptr);
            if (readOk == FALSE || bytesRead == 0)
            {
                break;
            }
            if (bytesRead < sizeof(packetValue)
                || packetValue.size != sizeof(packetValue)
                || packetValue.version != ks::winapi_monitor::kProtocolVersion)
            {
                continue;
            }

            EventRow rowValue = packetToEventRow(packetValue);
            {
                std::lock_guard<std::mutex> lock(guardThis->m_pendingMutex);
                guardThis->m_pendingRows.push_back(std::move(rowValue));
            }
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
        }
    }
}

void WinAPIDock::flushPendingRows()
{
    std::vector<EventRow> rowList;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        rowList.swap(m_pendingRows);
    }

    if (rowList.empty())
    {
        return;
    }

    for (const EventRow& rowValue : rowList)
    {
        appendEventRow(rowValue);
    }

    while (m_eventTable != nullptr && m_eventTable->rowCount() > 12000)
    {
        m_eventTable->removeRow(0);
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
        const QBrush internalBrush(QColor(QStringLiteral("#4B7FB8")));
        categoryItem->setForeground(internalBrush);
        apiItem->setForeground(internalBrush);
    }
    else if (rowValue.resultText != QStringLiteral("OK"))
    {
        const QBrush errorBrush(QColor(QStringLiteral("#B74343")));
        resultItem->setForeground(errorBrush);
    }

    m_eventTable->setItem(row, EventColumnTime100ns, timeItem);
    m_eventTable->setItem(row, EventColumnCategory, categoryItem);
    m_eventTable->setItem(row, EventColumnApi, apiItem);
    m_eventTable->setItem(row, EventColumnResult, resultItem);
    m_eventTable->setItem(row, EventColumnPidTid, pidTidItem);
    m_eventTable->setItem(row, EventColumnDetail, detailItem);
}
