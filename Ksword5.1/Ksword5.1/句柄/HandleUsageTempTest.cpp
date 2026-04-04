// ============================================================
// HandleUsageTempTest.cpp
// 作用：
// - 提供独立控制台测试程序，验证“旧句柄表 + DuplicateHandle +
//   GetFinalPathNameByHandleW”这条专用文件占用扫描链路；
// - 采用 16 个工作线程 + 1 个监管线程；
// - 当某个工作线程超过 0.5 秒无心跳时，监管线程放弃该线程并从
//   “上次进度 + 2” 位置继续创建新线程扫描。
// ============================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // kWorkerCount：固定 16 个分片线程，和用户指定策略保持一致。
    constexpr std::size_t kWorkerCount = 16;
    // kHeartbeatTimeoutMs：工作线程 0.5 秒无心跳则判定卡死。
    constexpr std::uint64_t kHeartbeatTimeoutMs = 500;
    // kMonitorSleepMs：监管线程轮询间隔，尽量短但不空转。
    constexpr DWORD kMonitorSleepMs = 80;

    // SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE：
    // - 对齐旧版 NtQuerySystemInformation(SystemHandleInformation=16)；
    // - 本测试程序只保留扫描所需字段。
    struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE
    {
        ULONG processId = 0;            // processId：拥有该句柄的进程 PID。
        UCHAR objectTypeNumber = 0;     // objectTypeNumber：对象类型编号。
        UCHAR flags = 0;                // flags：句柄属性位。
        USHORT handleValue = 0;         // handleValue：句柄值（旧结构为 USHORT）。
        PVOID objectAddress = nullptr;  // objectAddress：内核对象地址。
        ACCESS_MASK grantedAccess = 0;  // grantedAccess：访问掩码。
    };

    // SYSTEM_HANDLE_INFORMATION_NATIVE：
    // - 对齐旧版 SystemHandleInformation 返回头；
    // - handles 是变长尾数组。
    struct SYSTEM_HANDLE_INFORMATION_NATIVE
    {
        ULONG handleCount = 0;                                  // handleCount：系统句柄总数。
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE handles[1] = {};  // handles：句柄条目数组。
    };

    // UniqueHandle：
    // - 最小 RAII 句柄封装；
    // - 便于控制台测试程序在大量 continue 分支里安全释放句柄。
    class UniqueHandle final
    {
    public:
        explicit UniqueHandle(HANDLE handleValue = nullptr)
            : m_handle(handleValue)
        {
        }

        ~UniqueHandle()
        {
            reset(nullptr);
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept
            : m_handle(other.m_handle)
        {
            other.m_handle = nullptr;
        }

        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }
            reset(nullptr);
            m_handle = other.m_handle;
            other.m_handle = nullptr;
            return *this;
        }

        HANDLE get() const
        {
            return m_handle;
        }

        bool valid() const
        {
            return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
        }

        void reset(HANDLE newHandle)
        {
            if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_handle);
            }
            m_handle = newHandle;
        }

    private:
        HANDLE m_handle = nullptr; // m_handle：当前持有的系统句柄。
    };

    // normalizePathForCompare：
    // - 统一路径比较格式；
    // - 处理 \\?\ 和 \\?\UNC\ 前缀；
    // - 转成反斜杠、小写、去尾斜杠。
    std::wstring normalizePathForCompare(std::wstring text)
    {
        std::replace(text.begin(), text.end(), L'/', L'\\');

        if (text.rfind(L"\\\\?\\UNC\\", 0) == 0)
        {
            text = L"\\\\" + text.substr(8);
        }
        else if (text.rfind(L"\\\\?\\", 0) == 0)
        {
            text = text.substr(4);
        }

        while (text.size() > 3 && !text.empty() && text.back() == L'\\')
        {
            text.pop_back();
        }

        std::transform(
            text.begin(),
            text.end(),
            text.begin(),
            [](wchar_t ch)
            {
                return static_cast<wchar_t>(::towlower(ch));
            });
        return text;
    }

    // getTickMs：
    // - 统一读取当前毫秒级时间戳；
    // - 供心跳与监管线程判断“卡死”使用。
    std::uint64_t getTickMs()
    {
        return static_cast<std::uint64_t>(::GetTickCount64());
    }

    // g_logMutex：
    // - 保护控制台调试输出；
    // - 避免多个线程同时写控制台造成内容交错。
    std::mutex g_logMutex;

    // logDebug：
    // - 输出统一格式的调试日志；
    // - 自动带上时间戳和线程 ID，便于观察线程卡死与续扫行为。
    void logDebug(const std::wstring& messageText)
    {
        const std::wstring fullText =
            L"[DBG tick=" + std::to_wstring(getTickMs()) +
            L" tid=" + std::to_wstring(::GetCurrentThreadId()) +
            L"] " + messageText + L"\r\n";

        std::lock_guard<std::mutex> lock(g_logMutex);
        const HANDLE stdoutHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (stdoutHandle != nullptr && stdoutHandle != INVALID_HANDLE_VALUE)
        {
            DWORD writtenChars = 0;
            (void)::WriteConsoleW(
                stdoutHandle,
                fullText.c_str(),
                static_cast<DWORD>(fullText.size()),
                &writtenChars,
                nullptr);
            return;
        }

        std::wcout << fullText;
    }

    // shouldEmitCounterLog：
    // - 失败计数日志做节流；
    // - 前 8 次全部输出，后续每 256 次输出一次，避免刷爆控制台。
    bool shouldEmitCounterLog(const std::size_t countValue)
    {
        return countValue <= 8 || (countValue % 256) == 0;
    }

    // writeConsoleLine：
    // - 向控制台稳定输出一行文本；
    // - 避免 std::wcout 在不同控制台编码/缓冲模式下表现不一致。
    void writeConsoleLine(const std::wstring& messageText)
    {
        const std::wstring fullText = messageText + L"\r\n";
        const HANDLE stdoutHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (stdoutHandle != nullptr && stdoutHandle != INVALID_HANDLE_VALUE)
        {
            DWORD writtenChars = 0;
            if (::WriteConsoleW(
                stdoutHandle,
                fullText.c_str(),
                static_cast<DWORD>(fullText.size()),
                &writtenChars,
                nullptr) != FALSE)
            {
                return;
            }
        }

        std::wcout << fullText;
    }

    // writeConsoleText：
    // - 向控制台输出不自动换行的文本；
    // - 供输入提示使用。
    void writeConsoleText(const std::wstring& messageText)
    {
        const HANDLE stdoutHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (stdoutHandle != nullptr && stdoutHandle != INVALID_HANDLE_VALUE)
        {
            DWORD writtenChars = 0;
            if (::WriteConsoleW(
                stdoutHandle,
                messageText.c_str(),
                static_cast<DWORD>(messageText.size()),
                &writtenChars,
                nullptr) != FALSE)
            {
                return;
            }
        }

        std::wcout << messageText;
        std::wcout.flush();
    }

    // readConsoleLine：
    // - 优先走 ReadConsoleW 读取交互式控制台输入；
    // - 若当前不是交互控制台，则回退到 std::getline，便于管道测试。
    bool readConsoleLine(std::wstring& textOut)
    {
        textOut.clear();

        const HANDLE stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
        DWORD consoleMode = 0;
        if (stdinHandle == nullptr ||
            stdinHandle == INVALID_HANDLE_VALUE ||
            ::GetConsoleMode(stdinHandle, &consoleMode) == FALSE)
        {
            return static_cast<bool>(std::getline(std::wcin, textOut));
        }

        std::vector<wchar_t> buffer(8192, L'\0');
        DWORD readChars = 0;
        if (::ReadConsoleW(
            stdinHandle,
            buffer.data(),
            static_cast<DWORD>(buffer.size() - 1),
            &readChars,
            nullptr) == FALSE)
        {
            return false;
        }

        textOut.assign(buffer.data(), buffer.data() + readChars);
        while (!textOut.empty() &&
            (textOut.back() == L'\r' || textOut.back() == L'\n' || textOut.back() == L'\0'))
        {
            textOut.pop_back();
        }
        return true;
    }

    // getProcessNameMap：
    // - 一次性构建 PID -> 进程名映射；
    // - 避免结果打印时每次都再走 ToolHelp 快照。
    std::unordered_map<std::uint32_t, std::wstring> getProcessNameMap()
    {
        std::unordered_map<std::uint32_t, std::wstring> resultMap;
        UniqueHandle snapshotHandle(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshotHandle.valid())
        {
            return resultMap;
        }

        PROCESSENTRY32W processEntry{};
        processEntry.dwSize = sizeof(processEntry);
        BOOL hasItem = ::Process32FirstW(snapshotHandle.get(), &processEntry);
        while (hasItem != FALSE)
        {
            resultMap[processEntry.th32ProcessID] = processEntry.szExeFile;
            hasItem = ::Process32NextW(snapshotHandle.get(), &processEntry);
        }
        return resultMap;
    }

    // enableDebugPrivilege：
    // - 为当前进程启用 SeDebugPrivilege；
    // - 便于跨进程 DuplicateHandle 成功率更高。
    bool enableDebugPrivilege()
    {
        HANDLE rawTokenHandle = nullptr;
        if (::OpenProcessToken(
            ::GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &rawTokenHandle) == FALSE)
        {
            return false;
        }

        UniqueHandle tokenHandle(rawTokenHandle);
        LUID privilegeLuid{};
        if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privilegeLuid) == FALSE)
        {
            return false;
        }

        TOKEN_PRIVILEGES tokenPrivileges{};
        tokenPrivileges.PrivilegeCount = 1;
        tokenPrivileges.Privileges[0].Luid = privilegeLuid;
        tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (::AdjustTokenPrivileges(
            tokenHandle.get(),
            FALSE,
            &tokenPrivileges,
            sizeof(tokenPrivileges),
            nullptr,
            nullptr) == FALSE)
        {
            return false;
        }

        return ::GetLastError() == ERROR_SUCCESS;
    }

    // querySystemHandles：
    // - 调用旧版 SystemHandleInformation(16) 抓取系统句柄快照；
    // - 返回整个缓冲区，供所有线程只读共享。
    bool querySystemHandles(std::vector<std::uint8_t>& bufferOut)
    {
        bufferOut.clear();

        using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
        const HMODULE ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdllModule == nullptr)
        {
            return false;
        }

        const auto querySystemInformation =
            reinterpret_cast<NtQuerySystemInformationFn>(
                ::GetProcAddress(ntdllModule, "NtQuerySystemInformation"));
        if (querySystemInformation == nullptr)
        {
            return false;
        }

        ULONG bufferSize = 0x10000;
        for (int attemptIndex = 0; attemptIndex < 12; ++attemptIndex)
        {
            bufferOut.assign(bufferSize, 0);
            NTSTATUS status = querySystemInformation(16, bufferOut.data(), bufferSize, nullptr);
            if (status == static_cast<NTSTATUS>(0xC0000004))
            {
                bufferSize *= 2;
                continue;
            }
            return status >= 0;
        }
        return false;
    }

    // openTargetPathHandle：
    // - 以共享读写删除方式打开目标路径；
    // - 目录会自动带 FILE_FLAG_BACKUP_SEMANTICS。
    bool openTargetPathHandle(
        const std::wstring& inputPath,
        const bool directoryMode,
        UniqueHandle& handleOut)
    {
        handleOut.reset(nullptr);
        const DWORD flags = directoryMode ? FILE_FLAG_BACKUP_SEMANTICS : 0;
        HANDLE rawHandle = ::CreateFileW(
            inputPath.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            flags,
            nullptr);
        if (rawHandle == INVALID_HANDLE_VALUE || rawHandle == nullptr)
        {
            return false;
        }
        handleOut.reset(rawHandle);
        return true;
    }

    // resolveFileTypeIndex：
    // - 先打开目标路径拿到“当前进程句柄值”；
    // - 再从系统句柄表里找到该句柄对应的 objectTypeNumber；
    // - 以此动态确定 File TypeIndex。
    bool resolveFileTypeIndex(
        const HANDLE localTargetHandle,
        const SYSTEM_HANDLE_INFORMATION_NATIVE* handleInfo,
        std::uint8_t& fileTypeIndexOut)
    {
        fileTypeIndexOut = 0;
        if (localTargetHandle == nullptr || localTargetHandle == INVALID_HANDLE_VALUE || handleInfo == nullptr)
        {
            return false;
        }

        const DWORD currentProcessId = ::GetCurrentProcessId();
        const USHORT localHandleValue = static_cast<USHORT>(reinterpret_cast<ULONG_PTR>(localTargetHandle));
        for (ULONG index = 0; index < handleInfo->handleCount; ++index)
        {
            const SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE& row = handleInfo->handles[index];
            if (row.processId != currentProcessId)
            {
                continue;
            }
            if (row.handleValue != localHandleValue)
            {
                continue;
            }

            fileTypeIndexOut = row.objectTypeNumber;
            return fileTypeIndexOut != 0;
        }
        return false;
    }

    // MatchRecord：
    // - 保存一次命中的最终输出记录；
    // - 统一由主线程在末尾打印，避免多线程控制台输出错乱。
    struct MatchRecord
    {
        std::uint32_t processId = 0;    // processId：命中进程 PID。
        std::wstring processName;       // processName：命中进程名。
        std::uint64_t handleValue = 0;  // handleValue：命中句柄值。
        std::wstring finalPath;         // finalPath：命中后的最终 DOS 路径。
        std::size_t sourceIndex = 0;    // sourceIndex：原句柄表中的条目索引。
    };

    // SharedScanState：
    // - 所有工作线程共享的只读扫描上下文 + 结果聚合区；
    // - 线程只读访问句柄表，只在结果区持锁写入。
    struct SharedScanState
    {
        const SYSTEM_HANDLE_INFORMATION_NATIVE* handleInfo = nullptr; // handleInfo：系统句柄表快照。
        std::size_t handleCount = 0;                                  // handleCount：句柄条目数。
        std::uint8_t fileTypeIndex = 0;                               // fileTypeIndex：动态解析出的 File 类型编号。
        bool directoryMode = false;                                   // directoryMode：目标是否为目录。
        std::wstring normalizedTargetPath;                            // normalizedTargetPath：标准化目标路径。
        std::unordered_map<std::uint32_t, std::wstring> processNameMap; // processNameMap：PID -> 进程名。

        std::mutex resultMutex;                                       // resultMutex：保护结果集合。
        std::vector<MatchRecord> matchList;                           // matchList：命中结果列表。
        std::unordered_set<std::uint64_t> emittedHandleKeySet;        // emittedHandleKeySet：去重后的 PID+Handle 键。

        std::atomic<std::size_t> openProcessFailedCount = 0;          // openProcessFailedCount：OpenProcess 失败次数。
        std::atomic<std::size_t> duplicateFailedCount = 0;            // duplicateFailedCount：DuplicateHandle 失败次数。
        std::atomic<std::size_t> pathQueryFailedCount = 0;            // pathQueryFailedCount：路径查询失败次数。
        std::atomic<std::size_t> nonDiskFileSkippedCount = 0;         // nonDiskFileSkippedCount：非磁盘 File 句柄跳过次数。
        std::atomic<std::size_t> timeoutRespawnCount = 0;             // timeoutRespawnCount：因卡死被监管线程续扫的次数。
    };

    // WorkerAttempt：
    // - 表示某个分片任务的一次线程尝试；
    // - 线程卡死后不会强杀，只会被标记 abandoned 并由监管线程续扫。
    struct WorkerAttempt
    {
        std::size_t slotId = 0;                                       // slotId：所属分片编号。
        std::size_t startIndex = 0;                                   // startIndex：本次尝试开始扫描索引。
        std::size_t endIndex = 0;                                     // endIndex：本次尝试结束扫描索引。
        std::atomic<std::size_t> progressIndex = 0;                   // progressIndex：扫描前写入的当前索引。
        std::atomic<std::uint64_t> lastAliveTickMs = 0;               // lastAliveTickMs：最近一次心跳时间。
        std::atomic<bool> completed = false;                          // completed：本次尝试是否已正常完成。
        std::atomic<bool> abandoned = false;                          // abandoned：监管线程是否已判定超时并放弃。
    };

    // WorkerSlotState：
    // - 记录一个分片当前活动尝试；
    // - 监管线程会在这里替换为新的续扫尝试。
    struct WorkerSlotState
    {
        std::size_t slotId = 0;                                       // slotId：分片编号。
        std::size_t rangeBegin = 0;                                   // rangeBegin：分片起始索引。
        std::size_t rangeEnd = 0;                                     // rangeEnd：分片结束索引。
        std::shared_ptr<WorkerAttempt> activeAttempt;                 // activeAttempt：当前活动尝试。
        bool finished = false;                                        // finished：该分片是否已完成。
    };

    // buildHandleKey：
    // - 构造 PID + Handle 的去重键；
    // - 防止监管线程续扫后重复记录同一句柄。
    std::uint64_t buildHandleKey(const SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE& row)
    {
        return (static_cast<std::uint64_t>(row.processId) << 32) ^
            static_cast<std::uint64_t>(row.handleValue);
    }

    // workerThreadProc：
    // - 工作线程主体；
    // - 每次扫描前先写心跳与进度；
    // - 若被监管线程标记 abandoned，则在下一个可返回点主动停止。
    void workerThreadProc(
        const std::shared_ptr<WorkerAttempt>& attempt,
        SharedScanState* sharedState)
    {
        if (attempt == nullptr || sharedState == nullptr || sharedState->handleInfo == nullptr)
        {
            return;
        }

        {
            std::wstringstream stream;
            stream
                << L"worker start"
                << L" slot=" << attempt->slotId
                << L" range=[" << attempt->startIndex << L"," << attempt->endIndex << L"]";
            logDebug(stream.str());
        }

        for (std::size_t index = attempt->startIndex; index <= attempt->endIndex; ++index)
        {
            if (attempt->abandoned.load())
            {
                std::wstringstream stream;
                stream
                    << L"worker abandoned"
                    << L" slot=" << attempt->slotId
                    << L" progress=" << attempt->progressIndex.load();
                logDebug(stream.str());
                return;
            }

            attempt->progressIndex.store(index);
            attempt->lastAliveTickMs.store(getTickMs());

            if (((index - attempt->startIndex) % 8192) == 0)
            {
                std::wstringstream stream;
                stream
                    << L"worker progress"
                    << L" slot=" << attempt->slotId
                    << L" index=" << index
                    << L" end=" << attempt->endIndex;
                logDebug(stream.str());
            }

            const SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE& row = sharedState->handleInfo->handles[index];
            if (row.objectTypeNumber != sharedState->fileTypeIndex)
            {
                continue;
            }

            UniqueHandle ownerProcessHandle(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, row.processId));
            if (!ownerProcessHandle.valid())
            {
                const std::size_t failCount = sharedState->openProcessFailedCount.fetch_add(1) + 1;
                if (shouldEmitCounterLog(failCount))
                {
                    std::wstringstream stream;
                    stream
                        << L"OpenProcess failed"
                        << L" slot=" << attempt->slotId
                        << L" index=" << index
                        << L" pid=" << row.processId
                        << L" error=" << ::GetLastError()
                        << L" count=" << failCount;
                    logDebug(stream.str());
                }
                continue;
            }

            HANDLE duplicatedHandleRaw = nullptr;
            const BOOL duplicateOk = ::DuplicateHandle(
                ownerProcessHandle.get(),
                reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(row.handleValue)),
                ::GetCurrentProcess(),
                &duplicatedHandleRaw,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS);
            if (duplicateOk == FALSE || duplicatedHandleRaw == nullptr)
            {
                const std::size_t failCount = sharedState->duplicateFailedCount.fetch_add(1) + 1;
                if (shouldEmitCounterLog(failCount))
                {
                    std::wstringstream stream;
                    stream
                        << L"DuplicateHandle failed"
                        << L" slot=" << attempt->slotId
                        << L" index=" << index
                        << L" pid=" << row.processId
                        << L" handle=0x" << std::hex << row.handleValue << std::dec
                        << L" error=" << ::GetLastError()
                        << L" count=" << failCount;
                    logDebug(stream.str());
                }
                continue;
            }

            UniqueHandle duplicatedHandle(duplicatedHandleRaw);
            if (::GetFileType(duplicatedHandle.get()) != FILE_TYPE_DISK)
            {
                const std::size_t skipCount = sharedState->nonDiskFileSkippedCount.fetch_add(1) + 1;
                if (shouldEmitCounterLog(skipCount))
                {
                    std::wstringstream stream;
                    stream
                        << L"non-disk file handle skipped"
                        << L" slot=" << attempt->slotId
                        << L" index=" << index
                        << L" pid=" << row.processId
                        << L" handle=0x" << std::hex << row.handleValue << std::dec
                        << L" count=" << skipCount;
                    logDebug(stream.str());
                }
                continue;
            }

            // 在潜在卡点前再次刷新心跳，便于监管线程尽量接近真实卡点位置。
            attempt->progressIndex.store(index);
            attempt->lastAliveTickMs.store(getTickMs());

            wchar_t finalPathBuffer[32768] = {};
            const DWORD finalPathLength = ::GetFinalPathNameByHandleW(
                duplicatedHandle.get(),
                finalPathBuffer,
                static_cast<DWORD>(std::size(finalPathBuffer)),
                VOLUME_NAME_DOS);
            if (finalPathLength == 0 || finalPathLength >= std::size(finalPathBuffer))
            {
                const DWORD lastError = ::GetLastError();
                const std::size_t failCount = sharedState->pathQueryFailedCount.fetch_add(1) + 1;
                if (shouldEmitCounterLog(failCount))
                {
                    std::wstringstream stream;
                    stream
                        << L"GetFinalPathNameByHandleW failed"
                        << L" slot=" << attempt->slotId
                        << L" index=" << index
                        << L" pid=" << row.processId
                        << L" handle=0x" << std::hex << row.handleValue << std::dec
                        << L" len=" << finalPathLength
                        << L" error=" << lastError
                        << L" count=" << failCount;
                    logDebug(stream.str());
                }
                continue;
            }

            std::wstring normalizedFinalPath = normalizePathForCompare(finalPathBuffer);
            bool matched = false;
            if (sharedState->directoryMode)
            {
                matched = normalizedFinalPath == sharedState->normalizedTargetPath ||
                    normalizedFinalPath.rfind(sharedState->normalizedTargetPath + L"\\", 0) == 0;
            }
            else
            {
                matched = normalizedFinalPath == sharedState->normalizedTargetPath;
            }

            if (!matched)
            {
                continue;
            }

            const std::uint64_t handleKey = buildHandleKey(row);
            std::lock_guard<std::mutex> lock(sharedState->resultMutex);
            if (sharedState->emittedHandleKeySet.find(handleKey) != sharedState->emittedHandleKeySet.end())
            {
                continue;
            }

            sharedState->emittedHandleKeySet.insert(handleKey);
            MatchRecord record{};
            record.processId = row.processId;
            record.handleValue = row.handleValue;
            record.sourceIndex = index;
            const auto processNameIt = sharedState->processNameMap.find(row.processId);
            if (processNameIt != sharedState->processNameMap.end())
            {
                record.processName = processNameIt->second;
            }
            else
            {
            record.processName = L"PID_" + std::to_wstring(row.processId);
            }
            record.finalPath = finalPathBuffer;
            sharedState->matchList.push_back(std::move(record));

            std::wstringstream stream;
            stream
                << L"match found"
                << L" slot=" << attempt->slotId
                << L" index=" << index
                << L" pid=" << row.processId
                << L" handle=0x" << std::hex << row.handleValue << std::dec
                << L" path=" << finalPathBuffer;
            logDebug(stream.str());
        }

        attempt->completed.store(true);
        attempt->lastAliveTickMs.store(getTickMs());

        {
            std::wstringstream stream;
            stream
                << L"worker completed"
                << L" slot=" << attempt->slotId
                << L" finalProgress=" << attempt->progressIndex.load();
            logDebug(stream.str());
        }
    }

    // startAttempt：
    // - 为某个分片创建新的扫描尝试；
    // - 线程采用 detach，避免卡死线程在进程退出前阻塞 join。
    std::shared_ptr<WorkerAttempt> startAttempt(
        const std::size_t slotId,
        const std::size_t startIndex,
        const std::size_t endIndex,
        SharedScanState* sharedState)
    {
        auto attempt = std::make_shared<WorkerAttempt>();
        attempt->slotId = slotId;
        attempt->startIndex = startIndex;
        attempt->endIndex = endIndex;
        attempt->progressIndex.store(startIndex);
        attempt->lastAliveTickMs.store(getTickMs());

        {
            std::wstringstream stream;
            stream
                << L"spawn worker attempt"
                << L" slot=" << slotId
                << L" start=" << startIndex
                << L" end=" << endIndex;
            logDebug(stream.str());
        }

        std::thread workerThread(workerThreadProc, attempt, sharedState);
        workerThread.detach();
        return attempt;
    }

    // monitorThreadProc：
    // - 监管线程持续检查 16 个分片；
    // - 若某个活动尝试超过 0.5 秒无心跳，则将其标记 abandoned；
    // - 然后从“上次进度 + 2”位置补起一个新线程继续扫描。
    void monitorThreadProc(
        std::vector<WorkerSlotState>* slotStateList,
        SharedScanState* sharedState,
        std::atomic<bool>* monitorStopFlag)
    {
        if (slotStateList == nullptr || sharedState == nullptr || monitorStopFlag == nullptr)
        {
            return;
        }

        logDebug(L"monitor start");

        while (!monitorStopFlag->load())
        {
            bool allFinished = true;
            const std::uint64_t nowTickMs = getTickMs();

            for (WorkerSlotState& slotState : *slotStateList)
            {
                if (slotState.finished)
                {
                    continue;
                }
                allFinished = false;

                if (slotState.activeAttempt == nullptr)
                {
                    slotState.finished = true;
                    std::wstringstream stream;
                    stream
                        << L"slot finished without active attempt"
                        << L" slot=" << slotState.slotId;
                    logDebug(stream.str());
                    continue;
                }

                if (slotState.activeAttempt->completed.load())
                {
                    slotState.finished = true;
                    std::wstringstream stream;
                    stream
                        << L"slot completed"
                        << L" slot=" << slotState.slotId
                        << L" progress=" << slotState.activeAttempt->progressIndex.load();
                    logDebug(stream.str());
                    continue;
                }

                const std::uint64_t lastAliveTickMs = slotState.activeAttempt->lastAliveTickMs.load();
                if (nowTickMs <= lastAliveTickMs || (nowTickMs - lastAliveTickMs) <= kHeartbeatTimeoutMs)
                {
                    continue;
                }

                const std::size_t resumeIndex = slotState.activeAttempt->progressIndex.load() + 2;
                slotState.activeAttempt->abandoned.store(true);
                const std::size_t respawnCount = sharedState->timeoutRespawnCount.fetch_add(1) + 1;

                {
                    std::wstringstream stream;
                    stream
                        << L"worker timeout detected"
                        << L" slot=" << slotState.slotId
                        << L" lastAlive=" << lastAliveTickMs
                        << L" now=" << nowTickMs
                        << L" progress=" << slotState.activeAttempt->progressIndex.load()
                        << L" resume=" << resumeIndex
                        << L" respawnCount=" << respawnCount;
                    logDebug(stream.str());
                }

                if (resumeIndex > slotState.rangeEnd)
                {
                    slotState.finished = true;
                    std::wstringstream stream;
                    stream
                        << L"slot timeout but range exhausted"
                        << L" slot=" << slotState.slotId
                        << L" rangeEnd=" << slotState.rangeEnd;
                    logDebug(stream.str());
                    continue;
                }

                slotState.activeAttempt = startAttempt(
                    slotState.slotId,
                    resumeIndex,
                    slotState.rangeEnd,
                    sharedState);
            }

            if (allFinished)
            {
                monitorStopFlag->store(true);
                logDebug(L"monitor stop: all slots finished");
                return;
            }

            ::Sleep(kMonitorSleepMs);
        }

        logDebug(L"monitor stop: flag set");
    }
}

// ============================================================
// wmain：
// - 主入口负责输入路径、抓句柄快照、解析 File TypeIndex；
// - 然后把快照分成 16 段交给工作线程；
// - 最后等待监管线程收敛并输出结果。
// ============================================================
int wmain()
{
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    logDebug(L"program start");

    const bool debugPrivilegeEnabled = enableDebugPrivilege();
    writeConsoleLine(
        L"SeDebugPrivilege=" + std::wstring(debugPrivilegeEnabled ? L"Enabled" : L"Disabled"));
    logDebug(debugPrivilegeEnabled ? L"SeDebugPrivilege enabled" : L"SeDebugPrivilege disabled");

    writeConsoleText(L"Input target path: ");
    std::wstring inputPath;
    if (!readConsoleLine(inputPath))
    {
        logDebug(L"readConsoleLine failed");
        writeConsoleLine(L"Read input failed.");
        ::system("pause");
        return 0;
    }
    if (inputPath.empty())
    {
        logDebug(L"input path empty");
        writeConsoleLine(L"Input path is empty.");
        ::system("pause");
        return 0;
    }

    {
        std::wstringstream stream;
        stream << L"input path=" << inputPath;
        logDebug(stream.str());
    }

    const DWORD fileAttributes = ::GetFileAttributesW(inputPath.c_str());
    if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    {
        std::wstringstream stream;
        stream << L"GetFileAttributesW failed, error=" << ::GetLastError();
        logDebug(stream.str());
        writeConsoleLine(L"Target path does not exist or is inaccessible.");
        ::system("pause");
        return 0;
    }

    const bool directoryMode = (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    logDebug(directoryMode ? L"target mode=directory" : L"target mode=file");
    UniqueHandle targetHandle;
    if (!openTargetPathHandle(inputPath, directoryMode, targetHandle))
    {
        std::wstringstream stream;
        stream << L"openTargetPathHandle failed, error=" << ::GetLastError();
        logDebug(stream.str());
        writeConsoleLine(L"Target path open failed.");
        ::system("pause");
        return 0;
    }
    logDebug(L"target path opened successfully");
    {
        std::wstringstream stream;
        stream
            << L"target handle value=0x"
            << std::hex << reinterpret_cast<std::uintptr_t>(targetHandle.get());
        logDebug(stream.str());
    }

    std::vector<std::uint8_t> handleBuffer;
    logDebug(L"querySystemHandles begin");
    if (!querySystemHandles(handleBuffer))
    {
        logDebug(L"querySystemHandles failed");
        writeConsoleLine(L"System handle snapshot query failed.");
        ::system("pause");
        return 0;
    }
    {
        std::wstringstream stream;
        stream << L"querySystemHandles success, bufferBytes=" << handleBuffer.size();
        logDebug(stream.str());
    }

    const auto* handleInfo =
        reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_NATIVE*>(handleBuffer.data());
    if (handleInfo == nullptr || handleInfo->handleCount == 0)
    {
        logDebug(L"handle snapshot empty");
        writeConsoleLine(L"System handle snapshot is empty.");
        ::system("pause");
        return 0;
    }
    {
        std::wstringstream stream;
        stream << L"snapshot handle count=" << static_cast<std::size_t>(handleInfo->handleCount);
        logDebug(stream.str());
    }

    std::uint8_t fileTypeIndex = 0;
    if (!resolveFileTypeIndex(targetHandle.get(), handleInfo, fileTypeIndex))
    {
        logDebug(L"resolveFileTypeIndex failed");
        writeConsoleLine(L"Resolve File TypeIndex failed.");
        ::system("pause");
        return 0;
    }
    {
        std::wstringstream stream;
        stream << L"resolved FileTypeIndex=" << static_cast<unsigned int>(fileTypeIndex);
        logDebug(stream.str());
    }

    SharedScanState sharedState{};
    sharedState.handleInfo = handleInfo;
    sharedState.handleCount = handleInfo->handleCount;
    sharedState.fileTypeIndex = fileTypeIndex;
    sharedState.directoryMode = directoryMode;
    sharedState.normalizedTargetPath = normalizePathForCompare(inputPath);
    sharedState.processNameMap = getProcessNameMap();
    sharedState.emittedHandleKeySet.reserve(512);
    sharedState.matchList.reserve(128);

    {
        std::wstringstream stream;
        stream
            << L"snapshot handle count=" << handleInfo->handleCount
            << L", FileTypeIndex=" << static_cast<unsigned int>(fileTypeIndex)
            << L", WorkerCount=" << kWorkerCount;
        writeConsoleLine(stream.str());
    }

    std::vector<WorkerSlotState> slotStateList;
    slotStateList.reserve(kWorkerCount);
    const std::size_t totalHandleCount = static_cast<std::size_t>(handleInfo->handleCount);
    const std::size_t chunkSize = (totalHandleCount + kWorkerCount - 1) / kWorkerCount;
    {
        std::wstringstream stream;
        stream
            << L"prepare slots"
            << L" totalHandleCount=" << totalHandleCount
            << L" chunkSize=" << chunkSize;
        logDebug(stream.str());
    }

    for (std::size_t slotId = 0; slotId < kWorkerCount; ++slotId)
    {
        const std::size_t rangeBegin = slotId * chunkSize;
        if (rangeBegin >= totalHandleCount)
        {
            break;
        }

        const std::size_t rangeEnd = std::min(totalHandleCount, rangeBegin + chunkSize) - 1;
        WorkerSlotState slotState{};
        slotState.slotId = slotId;
        slotState.rangeBegin = rangeBegin;
        slotState.rangeEnd = rangeEnd;
        slotState.activeAttempt = startAttempt(slotId, rangeBegin, rangeEnd, &sharedState);
        slotStateList.push_back(std::move(slotState));
    }

    std::atomic<bool> monitorStopFlag = false;
    std::thread monitorThread(monitorThreadProc, &slotStateList, &sharedState, &monitorStopFlag);
    monitorThread.join();
    logDebug(L"monitor joined");

    {
        std::lock_guard<std::mutex> lock(sharedState.resultMutex);
        std::sort(
            sharedState.matchList.begin(),
            sharedState.matchList.end(),
            [](const MatchRecord& leftRecord, const MatchRecord& rightRecord)
            {
                if (leftRecord.processId != rightRecord.processId)
                {
                    return leftRecord.processId < rightRecord.processId;
                }
                if (leftRecord.handleValue != rightRecord.handleValue)
                {
                    return leftRecord.handleValue < rightRecord.handleValue;
                }
                return leftRecord.sourceIndex < rightRecord.sourceIndex;
            });

        for (const MatchRecord& record : sharedState.matchList)
        {
            std::wcout
                << L"Handle[" << record.sourceIndex << L"] "
                << L"PID=" << record.processId
                << L" Name=" << record.processName
                << L" Handle=0x" << std::hex << record.handleValue << std::dec
                << L" Path=" << record.finalPath
                << std::endl;
        }
    }

    {
        std::wstringstream stream;
        stream
            << L"scan done, matched=" << sharedState.matchList.size()
            << L", openProcessFail=" << sharedState.openProcessFailedCount.load()
            << L", duplicateFail=" << sharedState.duplicateFailedCount.load()
            << L", pathFail=" << sharedState.pathQueryFailedCount.load()
            << L", nonDiskSkip=" << sharedState.nonDiskFileSkippedCount.load()
            << L", timeoutRespawn=" << sharedState.timeoutRespawnCount.load();
        writeConsoleLine(stream.str());
    }
    logDebug(L"program end");

    ::system("pause");
    return 0;
}
