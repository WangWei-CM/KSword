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
#include <QMetaObject>
#include <QPointer>
#include <QTableWidget>
#include <QTableWidgetItem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
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
                static_cast<DWORD>(sizeof(packetValue)),
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

            EventRow rowValue;
            rowValue.time100nsText = QString::number(static_cast<qulonglong>(packetValue.timestamp100ns));
            rowValue.categoryText = eventCategoryText(packetValue.category);

            const QString moduleNameText = packetWideText(packetValue.moduleName);
            const QString apiNameText = packetWideText(packetValue.apiName);
            rowValue.apiText = moduleNameText.trimmed().isEmpty()
                ? apiNameText
                : QStringLiteral("%1!%2").arg(moduleNameText, apiNameText);
            rowValue.resultText = resultCodeText(packetValue.resultCode);
            rowValue.pidTidText = QStringLiteral("%1 / %2").arg(packetValue.pid).arg(packetValue.tid);
            rowValue.detailText = packetWideText(packetValue.detailText);
            rowValue.internalEvent =
                packetValue.category == static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal);

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
