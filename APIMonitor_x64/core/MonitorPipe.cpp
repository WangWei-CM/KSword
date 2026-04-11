#include "pch.h"
#include "MonitorPipe.h"
#include "../MonitorAgent.h"

#include <deque>

namespace apimon
{
    std::uint32_t FlushPendingMonitorEvents(const std::uint32_t maxPacketsToFlush);

    namespace
    {
        SRWLOCK g_pipeLock = SRWLOCK_INIT;              // g_pipeLock：保护 g_pipeHandle 的读写与发送序列。
        HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;     // g_pipeHandle：当前已连接的命名管道句柄。
        SRWLOCK g_queueLock = SRWLOCK_INIT;             // g_queueLock：保护待发送事件队列。
        std::deque<ks::winapi_monitor::ApiMonitorEventPacket> g_pendingPacketQueue; // 待发送事件队列。
        constexpr std::size_t kMaxPendingPacketCount = 4096;
        std::unique_ptr<std::thread> g_senderThread;   // g_senderThread：后台发送线程。
        std::atomic_bool g_senderStopFlag{ false };    // g_senderStopFlag：后台发送线程停止信号。
        HANDLE g_queueWakeEvent = nullptr;             // g_queueWakeEvent：待发送事件唤醒事件。
        constexpr DWORD kPipeConnectPollMs = 200;       // kPipeConnectPollMs：等待客户端连接时的轮询间隔。
        constexpr DWORD kPipeConnectTimeoutMs = 45000;  // kPipeConnectTimeoutMs：等待 UI 侧连入的最大时长。

        void CopyWideText(const std::wstring& sourceText, wchar_t* targetBuffer, const std::size_t charCount)
        {
            if (targetBuffer == nullptr || charCount == 0)
            {
                return;
            }

            const std::size_t copyLength = std::min<std::size_t>(charCount - 1, sourceText.size());
            if (copyLength > 0)
            {
                std::memcpy(targetBuffer, sourceText.data(), copyLength * sizeof(wchar_t));
            }
            targetBuffer[copyLength] = L'\0';
        }

        std::uint64_t QueryNow100ns()
        {
            FILETIME fileTimeValue{};
            ::GetSystemTimeAsFileTime(&fileTimeValue);

            ULARGE_INTEGER largeValue{};
            largeValue.LowPart = fileTimeValue.dwLowDateTime;
            largeValue.HighPart = fileTimeValue.dwHighDateTime;
            return static_cast<std::uint64_t>(largeValue.QuadPart);
        }

        void ClosePipeLocked()
        {
            if (g_pipeHandle != INVALID_HANDLE_VALUE)
            {
                // 关闭阶段不再 FlushFileBuffers，避免 UI 已断开或停止读取时把目标线程卡死。
                (void)::CancelIoEx(g_pipeHandle, nullptr);
                (void)::DisconnectNamedPipe(g_pipeHandle);
                ::CloseHandle(g_pipeHandle);
                g_pipeHandle = INVALID_HANDLE_VALUE;
            }
        }

        void EnsureSenderThreadStarted()
        {
            if (g_queueWakeEvent == nullptr)
            {
                g_queueWakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }

            if (g_senderThread != nullptr && g_senderThread->joinable())
            {
                return;
            }

            g_senderStopFlag.store(false);
            g_senderThread = std::make_unique<std::thread>([]() {
                while (!g_senderStopFlag.load())
                {
                    (void)FlushPendingMonitorEvents(256);
                    if (g_senderStopFlag.load())
                    {
                        break;
                    }

                    const DWORD waitResult = (g_queueWakeEvent != nullptr)
                        ? ::WaitForSingleObject(g_queueWakeEvent, 25)
                        : WAIT_TIMEOUT;
                    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT)
                    {
                        ::Sleep(25);
                    }
                }

                (void)FlushPendingMonitorEvents(512);
            });
        }

        bool WaitForClientConnection(const HANDLE pipeHandle, const MonitorConfig& configValue, std::wstring* errorTextOut)
        {
            HANDLE connectEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (connectEvent == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"CreateEventW for ConnectNamedPipe failed. error=" + std::to_wstring(::GetLastError());
                }
                return false;
            }

            OVERLAPPED overlappedValue{};
            overlappedValue.hEvent = connectEvent;

            bool connected = false;
            const BOOL connectOk = ::ConnectNamedPipe(pipeHandle, &overlappedValue);
            if (connectOk != FALSE)
            {
                connected = true;
            }
            else
            {
                DWORD lastError = ::GetLastError();
                if (lastError == ERROR_PIPE_CONNECTED)
                {
                    connected = true;
                    ::SetEvent(connectEvent);
                }
                else if (lastError == ERROR_IO_PENDING)
                {
                    DWORD waitedMs = 0;
                    while (waitedMs < kPipeConnectTimeoutMs)
                    {
                        if (StopRequested() || IsStopFlagPresent(configValue))
                        {
                            (void)::CancelIoEx(pipeHandle, &overlappedValue);
                            if (errorTextOut != nullptr)
                            {
                                *errorTextOut = L"ConnectNamedPipe canceled because stop was requested before UI connected.";
                            }
                            ::CloseHandle(connectEvent);
                            return false;
                        }

                        const DWORD waitResult = ::WaitForSingleObject(connectEvent, kPipeConnectPollMs);
                        if (waitResult == WAIT_OBJECT_0)
                        {
                            DWORD transferredBytes = 0;
                            if (::GetOverlappedResult(pipeHandle, &overlappedValue, &transferredBytes, FALSE) != FALSE
                                || ::GetLastError() == ERROR_PIPE_CONNECTED)
                            {
                                connected = true;
                                break;
                            }

                            lastError = ::GetLastError();
                            if (errorTextOut != nullptr)
                            {
                                *errorTextOut = L"ConnectNamedPipe overlapped completion failed. error=" + std::to_wstring(lastError);
                            }
                            ::CloseHandle(connectEvent);
                            return false;
                        }
                        if (waitResult != WAIT_TIMEOUT)
                        {
                            if (errorTextOut != nullptr)
                            {
                                *errorTextOut = L"WaitForSingleObject for ConnectNamedPipe failed. error=" + std::to_wstring(::GetLastError());
                            }
                            ::CloseHandle(connectEvent);
                            return false;
                        }

                        waitedMs += kPipeConnectPollMs;
                    }

                    if (!connected)
                    {
                        (void)::CancelIoEx(pipeHandle, &overlappedValue);
                        if (errorTextOut != nullptr)
                        {
                            *errorTextOut = L"ConnectNamedPipe timed out waiting for WinAPIDock client.";
                        }
                        ::CloseHandle(connectEvent);
                        return false;
                    }
                }
                else
                {
                    if (errorTextOut != nullptr)
                    {
                        *errorTextOut = L"ConnectNamedPipe start failed. error=" + std::to_wstring(lastError);
                    }
                    ::CloseHandle(connectEvent);
                    return false;
                }
            }

            ::CloseHandle(connectEvent);
            return connected;
        }
    }

    bool StartMonitorPipeServer(const MonitorConfig& configValue, std::wstring* errorTextOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
#ifdef PIPE_REJECT_REMOTE_CLIENTS
        pipeMode |= PIPE_REJECT_REMOTE_CLIENTS;
#endif

        HANDLE pipeHandle = ::CreateNamedPipeW(
            configValue.pipeName.c_str(),
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            pipeMode,
            1,
            64 * 1024,
            64 * 1024,
            0,
            nullptr);
        if (pipeHandle == INVALID_HANDLE_VALUE)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"CreateNamedPipeW failed. error=" + std::to_wstring(::GetLastError());
            }
            return false;
        }

        if (!WaitForClientConnection(pipeHandle, configValue, errorTextOut))
        {
            ::CloseHandle(pipeHandle);
            return false;
        }

        ::AcquireSRWLockExclusive(&g_pipeLock);
        ClosePipeLocked();
        g_pipeHandle = pipeHandle;
        ::ReleaseSRWLockExclusive(&g_pipeLock);
        EnsureSenderThreadStarted();
        return true;
    }

    void StopMonitorPipeServer()
    {
        g_senderStopFlag.store(true);
        if (g_queueWakeEvent != nullptr)
        {
            ::SetEvent(g_queueWakeEvent);
        }
        if (g_senderThread != nullptr && g_senderThread->joinable())
        {
            g_senderThread->join();
        }
        g_senderThread.reset();

        ::AcquireSRWLockExclusive(&g_pipeLock);
        ClosePipeLocked();
        ::ReleaseSRWLockExclusive(&g_pipeLock);

        ::AcquireSRWLockExclusive(&g_queueLock);
        g_pendingPacketQueue.clear();
        ::ReleaseSRWLockExclusive(&g_queueLock);
    }

    bool SendMonitorEvent(
        const ks::winapi_monitor::EventCategory categoryValue,
        const wchar_t* moduleName,
        const wchar_t* apiName,
        const std::int32_t resultCode,
        const std::wstring& detailText)
    {
        ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
        packetValue.pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
        packetValue.tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        packetValue.timestamp100ns = QueryNow100ns();
        packetValue.category = static_cast<std::uint32_t>(categoryValue);
        packetValue.resultCode = resultCode;

        CopyWideText(moduleName != nullptr ? std::wstring(moduleName) : std::wstring(), packetValue.moduleName, std::size(packetValue.moduleName));
        CopyWideText(apiName != nullptr ? std::wstring(apiName) : std::wstring(), packetValue.apiName, std::size(packetValue.apiName));

        const std::size_t detailLimit = std::min<std::size_t>(
            ActiveConfig().detailLimitChars,
            ks::winapi_monitor::kMaxDetailChars - 1);
        const std::wstring trimmedDetail = detailText.size() > detailLimit
            ? detailText.substr(0, detailLimit)
            : detailText;
        CopyWideText(trimmedDetail, packetValue.detailText, std::size(packetValue.detailText));

        ::AcquireSRWLockExclusive(&g_queueLock);
        if (g_pendingPacketQueue.size() >= kMaxPendingPacketCount)
        {
            ::ReleaseSRWLockExclusive(&g_queueLock);
            return false;
        }

        g_pendingPacketQueue.push_back(packetValue);
        ::ReleaseSRWLockExclusive(&g_queueLock);
        if (g_queueWakeEvent != nullptr)
        {
            ::SetEvent(g_queueWakeEvent);
        }
        return true;
    }

    std::uint32_t FlushPendingMonitorEvents(const std::uint32_t maxPacketsToFlush)
    {
        if (maxPacketsToFlush == 0)
        {
            return 0;
        }

        std::deque<ks::winapi_monitor::ApiMonitorEventPacket> packetQueue;
        ::AcquireSRWLockExclusive(&g_queueLock);
        const std::size_t flushCount = std::min<std::size_t>(maxPacketsToFlush, g_pendingPacketQueue.size());
        for (std::size_t indexValue = 0; indexValue < flushCount; ++indexValue)
        {
            packetQueue.push_back(g_pendingPacketQueue.front());
            g_pendingPacketQueue.pop_front();
        }
        ::ReleaseSRWLockExclusive(&g_queueLock);

        if (packetQueue.empty())
        {
            return 0;
        }

        ::AcquireSRWLockExclusive(&g_pipeLock);
        if (g_pipeHandle == INVALID_HANDLE_VALUE)
        {
            ::ReleaseSRWLockExclusive(&g_pipeLock);
            return 0;
        }

        std::uint32_t flushedCount = 0;
        for (const auto& packetValue : packetQueue)
        {
            DWORD bytesWritten = 0;
            const BOOL writeOk = ::WriteFile(
                g_pipeHandle,
                &packetValue,
                static_cast<DWORD>(sizeof(packetValue)),
                &bytesWritten,
                nullptr);
            if (writeOk == FALSE || bytesWritten != sizeof(packetValue))
            {
                ClosePipeLocked();
                break;
            }
            ++flushedCount;
        }

        ::ReleaseSRWLockExclusive(&g_pipeLock);
        return flushedCount;
    }

    bool IsMonitorPipeHandle(const HANDLE handleValue)
    {
        ::AcquireSRWLockShared(&g_pipeLock);
        const bool sameHandle = handleValue != nullptr && handleValue == g_pipeHandle;
        ::ReleaseSRWLockShared(&g_pipeLock);
        return sameHandle;
    }
}
