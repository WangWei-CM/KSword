#include "FileHandleUsageScanner.h"

// ============================================================
// FileHandleUsageScanner.cpp
// 作用：
// - 实现正式版“文件/目录占用扫描”；
// - 文件句柄部分采用旧句柄表 + 16 线程 + 监管线程续扫；
// - 同时补充进程映像与已加载模块两类非 File 句柄占用来源。
// ============================================================

#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <process.h>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>

namespace filedock::handleusage
{
    namespace
    {
        constexpr std::size_t kWorkerCount = 16;
        constexpr std::uint64_t kHeartbeatTimeoutMs = 500;
        constexpr DWORD kMonitorSleepMs = 80;
        constexpr ULONG kSystemHandleInformationClass = 16;
        constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004);
        constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005);
        constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023);
        constexpr float kHandlePathProgressStart = 35.0f;
        constexpr float kHandlePathProgressEnd = 85.0f;

        struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE
        {
            ULONG processId = 0;            // processId：拥有该句柄的进程 PID。
            UCHAR objectTypeNumber = 0;     // objectTypeNumber：对象类型编号。
            UCHAR flags = 0;                // flags：句柄属性位。
            USHORT handleValue = 0;         // handleValue：句柄值。
            PVOID objectAddress = nullptr;  // objectAddress：内核对象地址。
            ACCESS_MASK grantedAccess = 0;  // grantedAccess：访问掩码。
        };

        struct SYSTEM_HANDLE_INFORMATION_NATIVE
        {
            ULONG handleCount = 0;                                  // handleCount：系统句柄总数。
            SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE handles[1] = {};  // handles：句柄条目数组。
        };

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

        struct TargetPathPattern
        {
            QString displayPath;     // displayPath：用户视角的绝对路径。
            QString normalizedPath;  // normalizedPath：标准化比较路径。
            bool directoryMode = false; // directoryMode：true=目录前缀匹配。
        };

        QString normalizePathForCompare(QString pathText)
        {
            pathText = QDir::toNativeSeparators(pathText.trimmed());
            pathText.replace('/', '\\');

            if (pathText.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive))
            {
                pathText = QStringLiteral("\\\\") + pathText.mid(8);
            }
            else if (pathText.startsWith(QStringLiteral("\\\\?\\"), Qt::CaseInsensitive))
            {
                pathText = pathText.mid(4);
            }

            while (pathText.size() > 3 && pathText.endsWith('\\'))
            {
                pathText.chop(1);
            }
            return pathText.toLower();
        }

        std::vector<TargetPathPattern> buildTargetPatterns(const std::vector<QString>& absolutePaths)
        {
            std::vector<TargetPathPattern> patternList;
            patternList.reserve(absolutePaths.size());

            std::set<QString> normalizedSet;
            for (const QString& rawPath : absolutePaths)
            {
                if (rawPath.trimmed().isEmpty())
                {
                    continue;
                }

                QFileInfo fileInfo(rawPath);
                const QString absolutePath = QDir::toNativeSeparators(fileInfo.absoluteFilePath());
                const QString normalizedPath = normalizePathForCompare(absolutePath);
                if (normalizedPath.isEmpty())
                {
                    continue;
                }
                if (normalizedSet.find(normalizedPath) != normalizedSet.end())
                {
                    continue;
                }
                normalizedSet.insert(normalizedPath);

                TargetPathPattern pattern{};
                pattern.displayPath = absolutePath;
                pattern.normalizedPath = normalizedPath;
                pattern.directoryMode = fileInfo.exists() ? fileInfo.isDir() : rawPath.endsWith('\\');
                patternList.push_back(std::move(pattern));
            }
            return patternList;
        }

        bool matchTargetPath(
            const QString& normalizedCandidatePath,
            const std::vector<TargetPathPattern>& patternList,
            QString& matchedTargetPathOut,
            bool& matchedByDirectoryRuleOut)
        {
            matchedTargetPathOut.clear();
            matchedByDirectoryRuleOut = false;

            if (normalizedCandidatePath.trimmed().isEmpty())
            {
                return false;
            }

            for (const TargetPathPattern& pattern : patternList)
            {
                if (!pattern.directoryMode)
                {
                    if (normalizedCandidatePath == pattern.normalizedPath)
                    {
                        matchedTargetPathOut = pattern.displayPath;
                        matchedByDirectoryRuleOut = false;
                        return true;
                    }
                    continue;
                }

                if (normalizedCandidatePath == pattern.normalizedPath ||
                    normalizedCandidatePath.startsWith(pattern.normalizedPath + QChar('\\')))
                {
                    matchedTargetPathOut = pattern.displayPath;
                    matchedByDirectoryRuleOut = true;
                    return true;
                }
            }
            return false;
        }

        QString buildPathRuleText(const QString& sourceText, const bool directoryRule)
        {
            return directoryRule
                ? QStringLiteral("%1-目录前缀").arg(sourceText)
                : QStringLiteral("%1-精确路径").arg(sourceText);
        }

        struct NtApiSet final
        {
            using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

            HMODULE ntdllModule = nullptr;                               // ntdllModule：ntdll 模块句柄。
            NtQuerySystemInformationFn querySystemInformation = nullptr; // querySystemInformation：NtQuerySystemInformation。

            bool ready() const
            {
                return ntdllModule != nullptr && querySystemInformation != nullptr;
            }
        };

        NtApiSet queryNtApis()
        {
            NtApiSet apiSet{};
            apiSet.ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
            if (apiSet.ntdllModule == nullptr)
            {
                apiSet.ntdllModule = ::LoadLibraryW(L"ntdll.dll");
            }
            if (apiSet.ntdllModule == nullptr)
            {
                return apiSet;
            }

            apiSet.querySystemInformation =
                reinterpret_cast<NtApiSet::NtQuerySystemInformationFn>(
                    ::GetProcAddress(apiSet.ntdllModule, "NtQuerySystemInformation"));
            return apiSet;
        }

        bool querySystemHandles(
            const NtApiSet& apiSet,
            std::vector<std::uint8_t>& bufferOut,
            QString& diagnosticTextOut)
        {
            bufferOut.clear();
            diagnosticTextOut.clear();

            if (!apiSet.ready())
            {
                diagnosticTextOut = QStringLiteral("Nt API 不可用。");
                return false;
            }

            ULONG bufferSize = 1024 * 1024;
            for (int attemptIndex = 0; attemptIndex < 10; ++attemptIndex)
            {
                bufferOut.assign(static_cast<std::size_t>(bufferSize), 0);
                ULONG returnLength = 0;
                const NTSTATUS status = apiSet.querySystemInformation(
                    kSystemHandleInformationClass,
                    bufferOut.data(),
                    bufferSize,
                    &returnLength);

                if (status == kStatusInfoLengthMismatch ||
                    status == kStatusBufferOverflow ||
                    status == kStatusBufferTooSmall)
                {
                    bufferSize = std::max<ULONG>(bufferSize * 2, returnLength + 0x10000);
                    continue;
                }
                if (status < 0)
                {
                    diagnosticTextOut = QStringLiteral("NtQuerySystemInformation 失败。");
                    return false;
                }
                if (bufferOut.size() < sizeof(SYSTEM_HANDLE_INFORMATION_NATIVE))
                {
                    diagnosticTextOut = QStringLiteral("句柄快照缓冲区尺寸异常。");
                    return false;
                }
                return true;
            }

            diagnosticTextOut = QStringLiteral("句柄快照缓冲区扩容超过上限。");
            return false;
        }

        std::unordered_map<std::uint32_t, QString> collectProcessNameMap()
        {
            std::unordered_map<std::uint32_t, QString> processNameMap;
            UniqueHandle snapshotHandle(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
            if (!snapshotHandle.valid())
            {
                return processNameMap;
            }

            PROCESSENTRY32W processEntry{};
            processEntry.dwSize = sizeof(processEntry);
            BOOL hasItem = ::Process32FirstW(snapshotHandle.get(), &processEntry);
            while (hasItem != FALSE)
            {
                processNameMap[processEntry.th32ProcessID] = QString::fromWCharArray(processEntry.szExeFile);
                hasItem = ::Process32NextW(snapshotHandle.get(), &processEntry);
            }
            return processNameMap;
        }

        QString queryProcessImagePathCached(
            const std::uint32_t processId,
            std::unordered_map<std::uint32_t, QString>& cacheMap)
        {
            const auto foundIt = cacheMap.find(processId);
            if (foundIt != cacheMap.end())
            {
                return foundIt->second;
            }

            const QString imagePath = QString::fromStdString(ks::process::QueryProcessPathByPid(processId));
            cacheMap.insert_or_assign(processId, imagePath);
            return imagePath;
        }

        bool openTargetPathHandle(const TargetPathPattern& pattern, UniqueHandle& handleOut)
        {
            handleOut.reset(nullptr);
            const DWORD flags = pattern.directoryMode ? FILE_FLAG_BACKUP_SEMANTICS : 0;
            HANDLE rawHandle = ::CreateFileW(
                reinterpret_cast<LPCWSTR>(pattern.displayPath.utf16()),
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

        bool resolveFileTypeIndex(
            const HANDLE localTargetHandle,
            const SYSTEM_HANDLE_INFORMATION_NATIVE* handleInfo,
            std::uint16_t& fileTypeIndexOut)
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

                fileTypeIndexOut = static_cast<std::uint16_t>(row.objectTypeNumber);
                return fileTypeIndexOut != 0;
            }
            return false;
        }

        bool duplicateRemoteHandleToLocal(
            HANDLE sourceProcessHandle,
            std::uint64_t handleValue,
            UniqueHandle& localHandleOut)
        {
            localHandleOut.reset(nullptr);
            if (sourceProcessHandle == nullptr)
            {
                return false;
            }

            HANDLE duplicatedHandle = nullptr;
            const BOOL duplicateOk = ::DuplicateHandle(
                sourceProcessHandle,
                reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(handleValue)),
                ::GetCurrentProcess(),
                &duplicatedHandle,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS);
            if (duplicateOk == FALSE || duplicatedHandle == nullptr)
            {
                return false;
            }

            localHandleOut.reset(duplicatedHandle);
            return true;
        }

        struct FinalPathQueryState
        {
            LONG refCount = 2;                       // refCount：主线程 + 工作线程双引用。
            HANDLE ownedHandle = nullptr;            // ownedHandle：工作线程独占句柄副本。
            DWORD resultLength = 0;                  // resultLength：GetFinalPathNameByHandleW 返回值。
            DWORD lastError = ERROR_GEN_FAILURE;     // lastError：失败时的 Win32 错误码。
            wchar_t pathBuffer[32768] = {};          // pathBuffer：最终 DOS 路径输出缓冲。
        };

        void releaseFinalPathQueryState(FinalPathQueryState* state)
        {
            if (state == nullptr)
            {
                return;
            }
            if (::InterlockedDecrement(&state->refCount) == 0)
            {
                delete state;
            }
        }

        unsigned __stdcall finalPathQueryThreadProc(void* parameter)
        {
            FinalPathQueryState* state = reinterpret_cast<FinalPathQueryState*>(parameter);
            if (state == nullptr)
            {
                return 0;
            }

            state->resultLength = ::GetFinalPathNameByHandleW(
                state->ownedHandle,
                state->pathBuffer,
                static_cast<DWORD>(std::size(state->pathBuffer)),
                VOLUME_NAME_DOS);
            if (state->resultLength > 0 && state->resultLength < std::size(state->pathBuffer))
            {
                state->lastError = ERROR_SUCCESS;
            }
            else
            {
                state->lastError = ::GetLastError();
            }

            if (state->ownedHandle != nullptr && state->ownedHandle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(state->ownedHandle);
                state->ownedHandle = nullptr;
            }
            releaseFinalPathQueryState(state);
            return 0;
        }

        bool queryFinalDosPathWithTimeout(
            HANDLE sourceHandle,
            const DWORD timeoutMs,
            QString& pathOut,
            bool& timedOutOut)
        {
            pathOut.clear();
            timedOutOut = false;
            if (sourceHandle == nullptr || sourceHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            HANDLE workerOwnedHandle = nullptr;
            const BOOL duplicateOk = ::DuplicateHandle(
                ::GetCurrentProcess(),
                sourceHandle,
                ::GetCurrentProcess(),
                &workerOwnedHandle,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS);
            if (duplicateOk == FALSE || workerOwnedHandle == nullptr || workerOwnedHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            FinalPathQueryState* state = new FinalPathQueryState();
            state->ownedHandle = workerOwnedHandle;

            const uintptr_t threadHandleValue = _beginthreadex(
                nullptr,
                0,
                &finalPathQueryThreadProc,
                state,
                0,
                nullptr);
            if (threadHandleValue == 0)
            {
                if (workerOwnedHandle != nullptr && workerOwnedHandle != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(workerOwnedHandle);
                }
                releaseFinalPathQueryState(state);
                return false;
            }

            const HANDLE threadHandle = reinterpret_cast<HANDLE>(threadHandleValue);
            const DWORD waitResult = ::WaitForSingleObject(threadHandle, timeoutMs);
            if (waitResult == WAIT_OBJECT_0)
            {
                if (state->lastError == ERROR_SUCCESS)
                {
                    pathOut = QString::fromWCharArray(state->pathBuffer);
                }
                ::CloseHandle(threadHandle);
                releaseFinalPathQueryState(state);
                return state->lastError == ERROR_SUCCESS && !pathOut.trimmed().isEmpty();
            }

            timedOutOut = (waitResult == WAIT_TIMEOUT);
            ::CloseHandle(threadHandle);
            releaseFinalPathQueryState(state);
            return false;
        }

        struct FileHandleScanSharedState;
        struct WorkerAttempt;
        struct WorkerSlotState;

        std::uint64_t buildHandleKey(const std::uint32_t processId, const std::uint64_t handleValue);
        HANDLE openProcessHandleCached(FileHandleScanSharedState& sharedState, const std::uint32_t processId);
        QString queryProcessImagePathCachedThreadSafe(FileHandleScanSharedState& sharedState, const std::uint32_t processId);
        void workerThreadProc(const std::shared_ptr<WorkerAttempt>& attempt, const std::shared_ptr<FileHandleScanSharedState>& sharedState);
        std::shared_ptr<WorkerAttempt> startAttempt(
            const std::size_t slotId,
            const std::size_t startIndex,
            const std::size_t endIndex,
            const std::shared_ptr<FileHandleScanSharedState>& sharedState);
        void updateHandleStageProgress(
            const int progressPid,
            const std::size_t finishedSlotCount,
            const std::size_t totalSlotCount,
            const std::size_t matchedCount);
        HandleUsageScanResult scanFileHandleOccupancy(
            const std::vector<TargetPathPattern>& targetPatterns,
            const int progressPid);
        void appendSyntheticOccupancyEntries(
            const std::vector<TargetPathPattern>& targetPatterns,
            std::unordered_map<std::uint32_t, QString>& processImagePathCache,
            const std::unordered_map<std::uint32_t, QString>& processNameMap,
            std::vector<HandleUsageEntry>& entryList,
            std::size_t& processImageMatchCountOut,
            std::size_t& loadedModuleMatchCountOut,
            const int progressPid);

        struct FileHandleScanSharedState
        {
            const SYSTEM_HANDLE_INFORMATION_NATIVE* handleInfo = nullptr; // handleInfo：句柄快照原始结构。
            std::size_t handleCount = 0;                                  // handleCount：句柄条目数。
            std::uint16_t fileTypeIndex = 0;                              // fileTypeIndex：动态解析的 File 类型编号。
            std::uint64_t skippedHelperHandleKey = 0;                     // skippedHelperHandleKey：辅助打开目标路径的本进程句柄键。
            std::vector<TargetPathPattern> targetPatterns;                // targetPatterns：目标路径规则列表。
            std::unordered_map<std::uint32_t, QString> processNameMap;    // processNameMap：PID -> 进程名。
            std::unordered_map<std::uint32_t, QString> processImagePathCache; // processImagePathCache：PID -> 镜像路径缓存。
            std::unordered_map<std::uint32_t, std::shared_ptr<UniqueHandle>> ownerProcessHandleCache; // ownerProcessHandleCache：PID -> 打开的进程句柄缓存。
            std::unordered_set<std::uint32_t> failedProcessOpenSet;       // failedProcessOpenSet：OpenProcess 失败 PID 集合。
            std::unordered_set<std::uint64_t> emittedHandleKeySet;        // emittedHandleKeySet：防止续扫后重复命中。
            std::vector<HandleUsageEntry> entryList;                      // entryList：文件句柄命中结果。

            std::mutex dataMutex;                                         // dataMutex：保护缓存和结果容器。
            std::atomic<std::size_t> openProcessFailedCount = 0;          // openProcessFailedCount：OpenProcess 失败次数。
            std::atomic<std::size_t> duplicateFailedCount = 0;            // duplicateFailedCount：DuplicateHandle 失败次数。
            std::atomic<std::size_t> pathQueryFailedCount = 0;            // pathQueryFailedCount：路径查询失败次数。
            std::atomic<std::size_t> nonDiskFileSkippedCount = 0;         // nonDiskFileSkippedCount：非磁盘 File 句柄跳过次数。
            std::atomic<std::size_t> timeoutRespawnCount = 0;             // timeoutRespawnCount：监管线程续扫次数。
            std::atomic<std::size_t> matchedCount = 0;                    // matchedCount：文件句柄命中数量。
            int progressPid = 0;                                          // progressPid：kProgress 任务 PID。
        };

        struct WorkerAttempt
        {
            std::size_t slotId = 0;                                       // slotId：所属分片编号。
            std::size_t startIndex = 0;                                   // startIndex：本次尝试起始索引。
            std::size_t endIndex = 0;                                     // endIndex：本次尝试结束索引。
            std::atomic<std::size_t> progressIndex = 0;                   // progressIndex：扫描前写入的当前索引。
            std::atomic<std::uint64_t> lastAliveTickMs = 0;               // lastAliveTickMs：最近一次心跳时间。
            std::atomic<bool> completed = false;                          // completed：本次尝试是否已完成。
            std::atomic<bool> abandoned = false;                          // abandoned：监管线程是否已放弃该尝试。
        };

        struct WorkerSlotState
        {
            std::size_t slotId = 0;                                       // slotId：分片编号。
            std::size_t rangeBegin = 0;                                   // rangeBegin：该分片的起始索引。
            std::size_t rangeEnd = 0;                                     // rangeEnd：该分片的结束索引。
            std::shared_ptr<WorkerAttempt> activeAttempt;                 // activeAttempt：当前活动尝试。
            bool finished = false;                                        // finished：该分片是否已彻底完成。
        };

        std::uint64_t buildHandleKey(const std::uint32_t processId, const std::uint64_t handleValue)
        {
            return (static_cast<std::uint64_t>(processId) << 32) ^ handleValue;
        }

        HANDLE openProcessHandleCached(FileHandleScanSharedState& sharedState, const std::uint32_t processId)
        {
            std::lock_guard<std::mutex> lock(sharedState.dataMutex);

            const auto cacheIt = sharedState.ownerProcessHandleCache.find(processId);
            if (cacheIt != sharedState.ownerProcessHandleCache.end() && cacheIt->second != nullptr)
            {
                return cacheIt->second->get();
            }
            if (sharedState.failedProcessOpenSet.find(processId) != sharedState.failedProcessOpenSet.end())
            {
                return nullptr;
            }

            auto processHandlePtr = std::make_shared<UniqueHandle>();
            processHandlePtr->reset(::OpenProcess(
                PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                processId));
            if (!processHandlePtr->valid())
            {
                processHandlePtr->reset(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, processId));
            }
            if (!processHandlePtr->valid())
            {
                sharedState.failedProcessOpenSet.insert(processId);
                return nullptr;
            }

            const HANDLE cachedHandle = processHandlePtr->get();
            sharedState.ownerProcessHandleCache.insert_or_assign(processId, processHandlePtr);
            return cachedHandle;
        }

        QString queryProcessImagePathCachedThreadSafe(
            FileHandleScanSharedState& sharedState,
            const std::uint32_t processId)
        {
            {
                std::lock_guard<std::mutex> lock(sharedState.dataMutex);
                const auto foundIt = sharedState.processImagePathCache.find(processId);
                if (foundIt != sharedState.processImagePathCache.end())
                {
                    return foundIt->second;
                }
            }

            const QString imagePath = QString::fromStdString(ks::process::QueryProcessPathByPid(processId));
            std::lock_guard<std::mutex> lock(sharedState.dataMutex);
            sharedState.processImagePathCache.insert_or_assign(processId, imagePath);
            return imagePath;
        }

        void workerThreadProc(
            const std::shared_ptr<WorkerAttempt>& attempt,
            const std::shared_ptr<FileHandleScanSharedState>& sharedState)
        {
            if (attempt == nullptr || sharedState == nullptr || sharedState->handleInfo == nullptr)
            {
                return;
            }

            for (std::size_t index = attempt->startIndex; index <= attempt->endIndex; ++index)
            {
                if (attempt->abandoned.load())
                {
                    return;
                }

                attempt->progressIndex.store(index);
                attempt->lastAliveTickMs.store(static_cast<std::uint64_t>(::GetTickCount64()));

                const SYSTEM_HANDLE_TABLE_ENTRY_INFO_NATIVE& row = sharedState->handleInfo->handles[index];
                if (row.objectTypeNumber != sharedState->fileTypeIndex)
                {
                    continue;
                }

                const std::uint64_t handleKey = buildHandleKey(
                    static_cast<std::uint32_t>(row.processId),
                    static_cast<std::uint64_t>(row.handleValue));
                if (handleKey == sharedState->skippedHelperHandleKey)
                {
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(sharedState->dataMutex);
                    if (sharedState->emittedHandleKeySet.find(handleKey) != sharedState->emittedHandleKeySet.end())
                    {
                        continue;
                    }
                }

                const HANDLE ownerProcessHandle = openProcessHandleCached(*sharedState, row.processId);
                if (ownerProcessHandle == nullptr)
                {
                    sharedState->openProcessFailedCount.fetch_add(1);
                    continue;
                }

                UniqueHandle localHandle;
                if (!duplicateRemoteHandleToLocal(
                    ownerProcessHandle,
                    static_cast<std::uint64_t>(row.handleValue),
                    localHandle))
                {
                    sharedState->duplicateFailedCount.fetch_add(1);
                    continue;
                }

                if (::GetFileType(localHandle.get()) != FILE_TYPE_DISK)
                {
                    sharedState->nonDiskFileSkippedCount.fetch_add(1);
                    continue;
                }

                attempt->progressIndex.store(index);
                attempt->lastAliveTickMs.store(static_cast<std::uint64_t>(::GetTickCount64()));

                QString finalPathText;
                bool pathTimedOut = false;
                if (!queryFinalDosPathWithTimeout(localHandle.get(), 120, finalPathText, pathTimedOut))
                {
                    if (!pathTimedOut)
                    {
                        sharedState->pathQueryFailedCount.fetch_add(1);
                    }
                    continue;
                }

                if (attempt->abandoned.load())
                {
                    return;
                }

                const QString normalizedFinalPath = normalizePathForCompare(finalPathText);
                QString matchedTargetPath;
                bool matchedByDirectoryRule = false;
                if (!matchTargetPath(
                    normalizedFinalPath,
                    sharedState->targetPatterns,
                    matchedTargetPath,
                    matchedByDirectoryRule))
                {
                    continue;
                }

                HandleUsageEntry entry{};
                entry.processId = static_cast<std::uint32_t>(row.processId);
                const auto processNameIt = sharedState->processNameMap.find(entry.processId);
                entry.processName = (processNameIt != sharedState->processNameMap.end())
                    ? processNameIt->second
                    : QStringLiteral("PID_%1").arg(entry.processId);
                entry.processImagePath = queryProcessImagePathCachedThreadSafe(*sharedState, entry.processId);
                entry.handleValue = static_cast<std::uint64_t>(row.handleValue);
                entry.typeIndex = sharedState->fileTypeIndex;
                entry.typeName = QStringLiteral("FileHandle");
                entry.objectName = finalPathText;
                entry.grantedAccess = static_cast<std::uint32_t>(row.grantedAccess);
                entry.attributes = static_cast<std::uint32_t>(row.flags);
                entry.matchedTargetPath = matchedTargetPath;
                entry.matchedByDirectoryRule = matchedByDirectoryRule;
                entry.matchRuleText = buildPathRuleText(QStringLiteral("文件句柄"), matchedByDirectoryRule);

                {
                    std::lock_guard<std::mutex> lock(sharedState->dataMutex);
                    if (sharedState->emittedHandleKeySet.find(handleKey) != sharedState->emittedHandleKeySet.end())
                    {
                        continue;
                    }
                    sharedState->emittedHandleKeySet.insert(handleKey);
                    sharedState->entryList.push_back(std::move(entry));
                }
                sharedState->matchedCount.fetch_add(1);
            }

            attempt->completed.store(true);
            attempt->lastAliveTickMs.store(static_cast<std::uint64_t>(::GetTickCount64()));
        }

        std::shared_ptr<WorkerAttempt> startAttempt(
            const std::size_t slotId,
            const std::size_t startIndex,
            const std::size_t endIndex,
            const std::shared_ptr<FileHandleScanSharedState>& sharedState)
        {
            auto attempt = std::make_shared<WorkerAttempt>();
            attempt->slotId = slotId;
            attempt->startIndex = startIndex;
            attempt->endIndex = endIndex;
            attempt->progressIndex.store(startIndex);
            attempt->lastAliveTickMs.store(static_cast<std::uint64_t>(::GetTickCount64()));

            std::thread workerThread(workerThreadProc, attempt, sharedState);
            workerThread.detach();
            return attempt;
        }

        void updateHandleStageProgress(
            const int progressPid,
            const std::size_t finishedSlotCount,
            const std::size_t totalSlotCount,
            const std::size_t matchedCount)
        {
            if (progressPid <= 0 || totalSlotCount == 0)
            {
                return;
            }

            const float ratio = static_cast<float>(finishedSlotCount) / static_cast<float>(totalSlotCount);
            const float progressValue =
                kHandlePathProgressStart +
                (kHandlePathProgressEnd - kHandlePathProgressStart) * ratio;
            const std::string stepText =
                "扫描文件句柄分片 " +
                std::to_string(finishedSlotCount) + "/" + std::to_string(totalSlotCount) +
                "，已命中 " + std::to_string(matchedCount);
            kPro.set(progressPid, stepText, 0, progressValue);
        }

        HandleUsageScanResult scanFileHandleOccupancy(
            const std::vector<TargetPathPattern>& targetPatterns,
            const int progressPid)
        {
            HandleUsageScanResult result{};

            if (targetPatterns.empty())
            {
                result.diagnosticText = QStringLiteral("未提供有效目标路径。");
                return result;
            }

            if (progressPid > 0)
            {
                kPro.set(progressPid, "准备抓取系统句柄快照", 0, 10.0f);
            }

            const NtApiSet apiSet = queryNtApis();
            std::vector<std::uint8_t> handleBuffer;
            QString snapshotDiagnosticText;
            if (!querySystemHandles(apiSet, handleBuffer, snapshotDiagnosticText))
            {
                result.diagnosticText = snapshotDiagnosticText;
                return result;
            }

            const auto* handleInfo =
                reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_NATIVE*>(handleBuffer.data());
            if (handleInfo == nullptr || handleInfo->handleCount == 0)
            {
                result.diagnosticText = QStringLiteral("系统句柄快照为空。");
                return result;
            }
            result.totalHandleCount = static_cast<std::size_t>(handleInfo->handleCount);

            if (progressPid > 0)
            {
                kPro.set(progressPid, "构建进程名映射", 0, 20.0f);
            }

            const std::unordered_map<std::uint32_t, QString> processNameMap = collectProcessNameMap();
            std::uint16_t fileTypeIndex = 0;
            UniqueHandle helperHandle;
            if (!openTargetPathHandle(targetPatterns.front(), helperHandle) ||
                !resolveFileTypeIndex(helperHandle.get(), handleInfo, fileTypeIndex))
            {
                result.diagnosticText = QStringLiteral("动态 File TypeIndex 解析失败。");
                return result;
            }

            const std::uint64_t helperHandleKey = buildHandleKey(
                ::GetCurrentProcessId(),
                static_cast<std::uint64_t>(reinterpret_cast<ULONG_PTR>(helperHandle.get())));

            auto sharedState = std::make_shared<FileHandleScanSharedState>();
            sharedState->handleInfo = handleInfo;
            sharedState->handleCount = result.totalHandleCount;
            sharedState->fileTypeIndex = fileTypeIndex;
            sharedState->skippedHelperHandleKey = helperHandleKey;
            sharedState->targetPatterns = targetPatterns;
            sharedState->processNameMap = processNameMap;
            sharedState->progressPid = progressPid;
            sharedState->entryList.reserve(128);
            sharedState->emittedHandleKeySet.reserve(256);

            const std::size_t chunkSize = (sharedState->handleCount + kWorkerCount - 1) / kWorkerCount;
            std::vector<WorkerSlotState> slotList;
            slotList.reserve(kWorkerCount);

            for (std::size_t slotId = 0; slotId < kWorkerCount; ++slotId)
            {
                const std::size_t rangeBegin = slotId * chunkSize;
                if (rangeBegin >= sharedState->handleCount)
                {
                    break;
                }

                const std::size_t rangeEnd = std::min(sharedState->handleCount, rangeBegin + chunkSize) - 1;
                WorkerSlotState slot{};
                slot.slotId = slotId;
                slot.rangeBegin = rangeBegin;
                slot.rangeEnd = rangeEnd;
                slot.activeAttempt = startAttempt(slotId, rangeBegin, rangeEnd, sharedState);
                slotList.push_back(std::move(slot));
            }

            updateHandleStageProgress(progressPid, 0, slotList.size(), 0);

            bool allFinished = false;
            while (!allFinished)
            {
                std::size_t finishedSlotCount = 0;
                allFinished = true;
                const std::uint64_t nowTickMs = static_cast<std::uint64_t>(::GetTickCount64());

                for (WorkerSlotState& slot : slotList)
                {
                    if (slot.finished)
                    {
                        ++finishedSlotCount;
                        continue;
                    }

                    allFinished = false;
                    if (slot.activeAttempt == nullptr)
                    {
                        slot.finished = true;
                        ++finishedSlotCount;
                        continue;
                    }
                    if (slot.activeAttempt->completed.load())
                    {
                        slot.finished = true;
                        ++finishedSlotCount;
                        continue;
                    }

                    const std::uint64_t lastAliveTickMs = slot.activeAttempt->lastAliveTickMs.load();
                    if (nowTickMs <= lastAliveTickMs ||
                        (nowTickMs - lastAliveTickMs) <= kHeartbeatTimeoutMs)
                    {
                        continue;
                    }

                    const std::size_t resumeIndex = slot.activeAttempt->progressIndex.load() + 2;
                    slot.activeAttempt->abandoned.store(true);
                    sharedState->timeoutRespawnCount.fetch_add(1);

                    if (resumeIndex > slot.rangeEnd)
                    {
                        slot.finished = true;
                        ++finishedSlotCount;
                        continue;
                    }

                    slot.activeAttempt = startAttempt(slot.slotId, resumeIndex, slot.rangeEnd, sharedState);
                }

                updateHandleStageProgress(
                    progressPid,
                    finishedSlotCount,
                    slotList.size(),
                    sharedState->matchedCount.load());

                if (!allFinished)
                {
                    ::Sleep(kMonitorSleepMs);
                }
            }

            {
                std::lock_guard<std::mutex> lock(sharedState->dataMutex);
                result.entries.insert(
                    result.entries.end(),
                    sharedState->entryList.begin(),
                    sharedState->entryList.end());
            }

            result.fileLikeHandleCount = sharedState->matchedCount.load();
            QStringList diagnosticList;
            diagnosticList.push_back(QStringLiteral("文件TypeIndex:%1").arg(fileTypeIndex));
            if (!snapshotDiagnosticText.trimmed().isEmpty())
            {
                diagnosticList.push_back(snapshotDiagnosticText);
            }
            if (sharedState->openProcessFailedCount.load() > 0)
            {
                diagnosticList.push_back(QStringLiteral("OpenProcess失败:%1").arg(sharedState->openProcessFailedCount.load()));
            }
            if (sharedState->duplicateFailedCount.load() > 0)
            {
                diagnosticList.push_back(QStringLiteral("DuplicateHandle失败:%1").arg(sharedState->duplicateFailedCount.load()));
            }
            if (sharedState->pathQueryFailedCount.load() > 0)
            {
                diagnosticList.push_back(QStringLiteral("路径查询失败:%1").arg(sharedState->pathQueryFailedCount.load()));
            }
            if (sharedState->nonDiskFileSkippedCount.load() > 0)
            {
                diagnosticList.push_back(QStringLiteral("非磁盘File跳过:%1").arg(sharedState->nonDiskFileSkippedCount.load()));
            }
            if (sharedState->timeoutRespawnCount.load() > 0)
            {
                diagnosticList.push_back(QStringLiteral("线程续扫:%1").arg(sharedState->timeoutRespawnCount.load()));
            }
            result.diagnosticText = diagnosticList.join(QStringLiteral(" | "));
            return result;
        }

        void appendSyntheticOccupancyEntries(
            const std::vector<TargetPathPattern>& targetPatterns,
            std::unordered_map<std::uint32_t, QString>& processImagePathCache,
            const std::unordered_map<std::uint32_t, QString>& processNameMap,
            std::vector<HandleUsageEntry>& entryList,
            std::size_t& processImageMatchCountOut,
            std::size_t& loadedModuleMatchCountOut,
            const int progressPid)
        {
            processImageMatchCountOut = 0;
            loadedModuleMatchCountOut = 0;

            if (progressPid > 0)
            {
                kPro.set(progressPid, "扫描进程映像占用", 0, 90.0f);
            }

            std::set<QString> syntheticDedupeSet;
            for (const auto& processPair : processNameMap)
            {
                const std::uint32_t processId = processPair.first;
                const QString imagePath = queryProcessImagePathCached(processId, processImagePathCache);
                const QString normalizedImagePath = normalizePathForCompare(imagePath);
                QString matchedTargetPath;
                bool matchedByDirectoryRule = false;
                if (!matchTargetPath(normalizedImagePath, targetPatterns, matchedTargetPath, matchedByDirectoryRule))
                {
                    continue;
                }

                const QString dedupeKey = QStringLiteral("PI|%1|%2").arg(processId).arg(normalizedImagePath);
                if (syntheticDedupeSet.find(dedupeKey) != syntheticDedupeSet.end())
                {
                    continue;
                }
                syntheticDedupeSet.insert(dedupeKey);

                HandleUsageEntry entry{};
                entry.processId = processId;
                entry.processName = processPair.second;
                entry.processImagePath = imagePath;
                entry.handleValue = 0;
                entry.typeIndex = 0;
                entry.typeName = QStringLiteral("ProcessImage");
                entry.objectName = imagePath;
                entry.matchedTargetPath = matchedTargetPath;
                entry.matchedByDirectoryRule = matchedByDirectoryRule;
                entry.matchRuleText = buildPathRuleText(QStringLiteral("进程映像"), matchedByDirectoryRule);
                entryList.push_back(std::move(entry));
                ++processImageMatchCountOut;
            }

            if (progressPid > 0)
            {
                kPro.set(progressPid, "扫描模块加载占用", 0, 95.0f);
            }

            for (const auto& processPair : processNameMap)
            {
                const std::uint32_t processId = processPair.first;
                UniqueHandle moduleSnapshot(::CreateToolhelp32Snapshot(
                    TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                    processId));
                if (!moduleSnapshot.valid())
                {
                    continue;
                }

                MODULEENTRY32W moduleEntry{};
                moduleEntry.dwSize = sizeof(moduleEntry);
                BOOL hasModule = ::Module32FirstW(moduleSnapshot.get(), &moduleEntry);
                while (hasModule != FALSE)
                {
                    const QString modulePath = QString::fromWCharArray(moduleEntry.szExePath);
                    const QString normalizedModulePath = normalizePathForCompare(modulePath);
                    QString matchedTargetPath;
                    bool matchedByDirectoryRule = false;
                    if (matchTargetPath(normalizedModulePath, targetPatterns, matchedTargetPath, matchedByDirectoryRule))
                    {
                        const QString dedupeKey =
                            QStringLiteral("LM|%1|%2").arg(processId).arg(normalizedModulePath);
                        if (syntheticDedupeSet.find(dedupeKey) == syntheticDedupeSet.end())
                        {
                            syntheticDedupeSet.insert(dedupeKey);

                            HandleUsageEntry entry{};
                            entry.processId = processId;
                            entry.processName = processPair.second;
                            entry.processImagePath = queryProcessImagePathCached(processId, processImagePathCache);
                            entry.handleValue = 0;
                            entry.typeIndex = 0;
                            entry.typeName = QStringLiteral("LoadedModule");
                            entry.objectName = modulePath;
                            entry.matchedTargetPath = matchedTargetPath;
                            entry.matchedByDirectoryRule = matchedByDirectoryRule;
                            entry.matchRuleText = buildPathRuleText(QStringLiteral("模块加载"), matchedByDirectoryRule);
                            entryList.push_back(std::move(entry));
                            ++loadedModuleMatchCountOut;
                        }
                    }
                    hasModule = ::Module32NextW(moduleSnapshot.get(), &moduleEntry);
                }
            }
        }
    }

    HandleUsageScanResult scanHandleUsageByPaths(const std::vector<QString>& absolutePaths, const int progressPid)
    {
        HandleUsageScanResult result{};
        const auto beginTime = std::chrono::steady_clock::now();

        const std::vector<TargetPathPattern> targetPatterns = buildTargetPatterns(absolutePaths);
        if (targetPatterns.empty())
        {
            result.diagnosticText = QStringLiteral("未提供有效目标路径。");
            result.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());
            return result;
        }

        const std::unordered_map<std::uint32_t, QString> processNameMap = collectProcessNameMap();
        std::unordered_map<std::uint32_t, QString> processImagePathCache;

        if (progressPid > 0)
        {
            kPro.set(progressPid, "开始扫描占用来源", 0, 5.0f);
        }

        const HandleUsageScanResult fileHandleResult = scanFileHandleOccupancy(targetPatterns, progressPid);
        result = fileHandleResult;

        appendSyntheticOccupancyEntries(
            targetPatterns,
            processImagePathCache,
            processNameMap,
            result.entries,
            result.processImageMatchCount,
            result.loadedModuleMatchCount,
            progressPid);

        std::sort(
            result.entries.begin(),
            result.entries.end(),
            [](const HandleUsageEntry& leftEntry, const HandleUsageEntry& rightEntry)
            {
                if (leftEntry.processId != rightEntry.processId)
                {
                    return leftEntry.processId < rightEntry.processId;
                }
                if (leftEntry.handleValue != rightEntry.handleValue)
                {
                    return leftEntry.handleValue < rightEntry.handleValue;
                }
                return leftEntry.objectName < rightEntry.objectName;
            });

        result.matchedHandleCount = result.entries.size();
        QStringList diagnosticList;
        if (!result.diagnosticText.trimmed().isEmpty())
        {
            diagnosticList.push_back(result.diagnosticText);
        }
        if (result.processImageMatchCount > 0)
        {
            diagnosticList.push_back(QStringLiteral("进程映像占用:%1").arg(result.processImageMatchCount));
        }
        if (result.loadedModuleMatchCount > 0)
        {
            diagnosticList.push_back(QStringLiteral("模块加载占用:%1").arg(result.loadedModuleMatchCount));
        }
        result.diagnosticText = diagnosticList.join(QStringLiteral(" | "));

        if (progressPid > 0)
        {
            kPro.set(progressPid, "占用扫描完成", 0, 100.0f);
        }

        result.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
        return result;
    }
}
