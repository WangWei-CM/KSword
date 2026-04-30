#include "pch.h"
#include "MonitorPipe.h"
#include "../MonitorAgent.h"

namespace apimon
{
    std::uint32_t FlushPendingMonitorEvents(const std::uint32_t maxPacketsToFlush);

    namespace
    {
        SRWLOCK g_pipeLock = SRWLOCK_INIT;              // g_pipeLock：保护 g_pipeHandle 的读写与发送序列。
        HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;     // g_pipeHandle：当前已连接的命名管道句柄。
        // g_pipeHandleValue：无锁句柄快照，供 Hook 快速判断“是否监控管道句柄”，避免在 WriteFile Hook 中重入锁导致死锁。
        std::atomic_uintptr_t g_pipeHandleValue{ 0 };
        SRWLOCK g_queueLock = SRWLOCK_INIT;             // g_queueLock：保护待发送事件队列。
        constexpr std::size_t kMaxPendingPacketCount = 4096; // kMaxPendingPacketCount：固定环形队列容量，避免 Hook 热路径动态分配。
        constexpr std::uint32_t kMaxFlushBatchCount = 256;   // kMaxFlushBatchCount：单次写管道最多搬运的事件数，控制栈缓冲大小。
        std::array<ks::winapi_monitor::ApiMonitorEventPacket, kMaxPendingPacketCount> g_pendingPacketRing{}; // 固定事件环形缓冲。
        std::size_t g_pendingPacketHead = 0;                 // g_pendingPacketHead：下一条待发送事件所在槽位。
        std::size_t g_pendingPacketCount = 0;                // g_pendingPacketCount：当前环形队列中有效事件数量。
        std::unique_ptr<std::thread> g_senderThread;   // g_senderThread：后台发送线程。
        std::atomic_bool g_senderStopFlag{ false };    // g_senderStopFlag：后台发送线程停止信号。
        HANDLE g_queueWakeEvent = nullptr;             // g_queueWakeEvent：待发送事件唤醒事件。
        constexpr DWORD kPipeConnectPollMs = 200;       // kPipeConnectPollMs：等待客户端连接时的轮询间隔。
        constexpr DWORD kPipeConnectTimeoutMs = 45000;  // kPipeConnectTimeoutMs：等待 UI 侧连入的最大时长。

        // CopyWideTextRaw 作用：
        // - 输入：sourceText 为可空宽字符串，targetBuffer/charCount 为目标定长缓冲；
        // - 处理：按 maxChars 限制复制并始终补 NUL，不进行堆分配；
        // - 返回：无返回值，调用者直接使用 targetBuffer。
        void CopyWideTextRaw(
            const wchar_t* const sourceText,
            wchar_t* const targetBuffer,
            const std::size_t charCount,
            const std::size_t maxChars)
        {
            if (targetBuffer == nullptr || charCount == 0)
            {
                return;
            }

            const std::size_t copyLimit = std::min<std::size_t>(charCount - 1, maxChars);
            std::size_t copyLength = 0;
            if (sourceText != nullptr)
            {
                while (copyLength < copyLimit && sourceText[copyLength] != L'\0')
                {
                    targetBuffer[copyLength] = sourceText[copyLength];
                    ++copyLength;
                }
            }
            targetBuffer[copyLength] = L'\0';
        }

        // CopyWideText 作用：
        // - 输入：sourceText 为 std::wstring，targetBuffer/charCount 为协议包字段；
        // - 处理：复用 Raw 版本完成截断复制；
        // - 返回：无返回值，仅写入目标缓冲。
        void CopyWideText(const std::wstring& sourceText, wchar_t* const targetBuffer, const std::size_t charCount)
        {
            CopyWideTextRaw(sourceText.c_str(), targetBuffer, charCount, sourceText.size());
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
            // 先清空无锁快照，确保 Hook 侧不会在关闭阶段继续把该句柄当作可用监控管道。
            g_pipeHandleValue.store(0);
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

                while (FlushPendingMonitorEvents(256) != 0)
                {
                    // 停止前尽量清空已入队事件；每轮仍保持小批量，避免长时间持有管道锁。
                }
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
        g_pipeHandleValue.store(reinterpret_cast<std::uintptr_t>(pipeHandle));
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
        g_pendingPacketHead = 0;
        g_pendingPacketCount = 0;
        ::ReleaseSRWLockExclusive(&g_queueLock);
    }

    bool SendMonitorEvent(
        const ks::winapi_monitor::EventCategory categoryValue,
        const wchar_t* moduleName,
        const wchar_t* apiName,
        const std::int32_t resultCode,
        const std::wstring& detailText)
    {
        return SendMonitorEventRaw(
            categoryValue,
            moduleName,
            apiName,
            resultCode,
            detailText.c_str());
    }

    bool SendMonitorEventRaw(
        const ks::winapi_monitor::EventCategory categoryValue,
        const wchar_t* moduleName,
        const wchar_t* apiName,
        const std::int32_t resultCode,
        const wchar_t* detailText)
    {
        ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
        packetValue.pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
        packetValue.tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        packetValue.timestamp100ns = QueryNow100ns();
        packetValue.category = static_cast<std::uint32_t>(categoryValue);
        packetValue.resultCode = resultCode;

        const std::size_t detailLimit = std::min<std::size_t>(
            ActiveConfig().detailLimitChars,
            ks::winapi_monitor::kMaxDetailChars - 1);
        CopyWideTextRaw(moduleName, packetValue.moduleName, std::size(packetValue.moduleName), ks::winapi_monitor::kMaxModuleNameChars - 1);
        CopyWideTextRaw(apiName, packetValue.apiName, std::size(packetValue.apiName), ks::winapi_monitor::kMaxApiNameChars - 1);
        CopyWideTextRaw(detailText, packetValue.detailText, std::size(packetValue.detailText), detailLimit);

        ::AcquireSRWLockExclusive(&g_queueLock);
        if (g_pendingPacketCount >= kMaxPendingPacketCount)
        {
            ::ReleaseSRWLockExclusive(&g_queueLock);
            return false;
        }

        const std::size_t tailIndex = (g_pendingPacketHead + g_pendingPacketCount) % kMaxPendingPacketCount;
        g_pendingPacketRing[tailIndex] = packetValue;
        ++g_pendingPacketCount;
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

        std::array<ks::winapi_monitor::ApiMonitorEventPacket, kMaxFlushBatchCount> packetBatch{};
        std::size_t flushCount = 0;

        ::AcquireSRWLockExclusive(&g_queueLock);
        flushCount = std::min<std::size_t>(
            std::min<std::size_t>(maxPacketsToFlush, kMaxFlushBatchCount),
            g_pendingPacketCount);
        for (std::size_t indexValue = 0; indexValue < flushCount; ++indexValue)
        {
            packetBatch[indexValue] = g_pendingPacketRing[g_pendingPacketHead];
            g_pendingPacketHead = (g_pendingPacketHead + 1) % kMaxPendingPacketCount;
            --g_pendingPacketCount;
        }
        ::ReleaseSRWLockExclusive(&g_queueLock);

        if (flushCount == 0)
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
        for (std::size_t indexValue = 0; indexValue < flushCount; ++indexValue)
        {
            const auto& packetValue = packetBatch[indexValue];
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
        if (handleValue == nullptr || handleValue == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const std::uintptr_t monitorPipeHandleValue = g_pipeHandleValue.load();
        if (monitorPipeHandleValue == 0)
        {
            return false;
        }

        // 通过无锁快照比较句柄值，避免 FlushPendingMonitorEvents 持有 g_pipeLock 时，
        // WriteFile Hook 再次尝试加锁造成同线程重入死锁。
        return reinterpret_cast<std::uintptr_t>(handleValue) == monitorPipeHandleValue;
    }
}
