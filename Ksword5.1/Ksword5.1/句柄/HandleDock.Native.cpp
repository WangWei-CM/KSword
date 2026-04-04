#include "HandleDock.h"

#include "HandleObjectTypeWorker.h"

#include <QChar>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>
#include <TlHelp32.h>
#include <winternl.h>

namespace
{
    // SystemExtendedHandleInformation：NtQuerySystemInformation 句柄扩展信息类。
    constexpr ULONG kSystemExtendedHandleInformationClass = 64;
    // OBJECT_INFORMATION_CLASS 常量：统一使用数值避免头文件版本差异。
    constexpr ULONG kObjectBasicInformationClass = 0;
    constexpr ULONG kObjectNameInformationClass = 1;
    constexpr ULONG kObjectTypeInformationClass = 2;
    // 常见 NTSTATUS 常量：用于缓冲区扩容与错误判断。
    constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004);
    constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005);
    constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023);

    // SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE：
    // - 与 SystemExtendedHandleInformation 对齐的句柄条目结构；
    // - 仅包含本模块实际使用的字段。
    struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE
    {
        PVOID objectAddress = nullptr;           // objectAddress：内核对象地址。
        ULONG_PTR uniqueProcessId = 0;           // uniqueProcessId：拥有该句柄的 PID。
        ULONG_PTR handleValue = 0;               // handleValue：句柄值。
        ULONG grantedAccess = 0;                 // grantedAccess：访问掩码。
        USHORT creatorBackTraceIndex = 0;        // creatorBackTraceIndex：创建栈索引。
        USHORT objectTypeIndex = 0;              // objectTypeIndex：对象类型索引。
        ULONG handleAttributes = 0;              // handleAttributes：句柄属性位。
        ULONG reserved = 0;                      // reserved：保留字段。
    };

    // SYSTEM_HANDLE_INFORMATION_EX_NATIVE：
    // - 扩展句柄信息返回头；
    // - handles 数组为变长尾部。
    struct SYSTEM_HANDLE_INFORMATION_EX_NATIVE
    {
        ULONG_PTR numberOfHandles = 0;                                      // numberOfHandles：句柄数量。
        ULONG_PTR reserved = 0;                                             // reserved：保留字段。
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE handles[1] = {};           // handles：变长句柄条目。
    };

    // OBJECT_BASIC_INFORMATION_NATIVE：
    // - NtQueryObject(ObjectBasicInformation) 返回结构；
    // - 用于读取 HandleCount / PointerCount。
    struct OBJECT_BASIC_INFORMATION_NATIVE
    {
        ULONG attributes = 0;                  // attributes：对象属性。
        ACCESS_MASK grantedAccess = 0;         // grantedAccess：对象默认授权掩码。
        ULONG handleCount = 0;                 // handleCount：对象句柄计数。
        ULONG pointerCount = 0;                // pointerCount：对象指针计数。
        ULONG pagedPoolUsage = 0;              // pagedPoolUsage：分页池占用。
        ULONG nonPagedPoolUsage = 0;           // nonPagedPoolUsage：非分页池占用。
        ULONG reserved[3] = {};                // reserved：保留字段。
        ULONG nameInfoSize = 0;                // nameInfoSize：对象名信息尺寸。
        ULONG typeInfoSize = 0;                // typeInfoSize：对象类型信息尺寸。
        ULONG securityDescriptorSize = 0;      // securityDescriptorSize：安全描述符大小。
        LARGE_INTEGER creationTime{};          // creationTime：创建时间。
    };

    // NtApiSet：
    // - 缓存 ntdll 与核心 Nt API 函数地址；
    // - 避免每次刷新重复 GetProcAddress。
    struct NtApiSet final
    {
        using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
        using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        HMODULE ntdllModule = nullptr;                                      // ntdllModule：ntdll 模块句柄。
        NtQuerySystemInformationFn querySystemInformation = nullptr;        // querySystemInformation：NtQuerySystemInformation 地址。
        NtQueryObjectFn queryObject = nullptr;                              // queryObject：NtQueryObject 地址。

        // ready 作用：判断 Nt API 是否全部可用。
        bool ready() const
        {
            return ntdllModule != nullptr &&
                querySystemInformation != nullptr &&
                queryObject != nullptr;
        }
    };

    // UniqueHandle：
    // - Win32 HANDLE RAII 封装；
    // - 防止异常分支或提前 return 造成句柄泄漏。
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

    // RawHandleRecord：
    // - 后台线程内部使用的原始句柄项；
    // - 先存原始数据，再按筛选规则转成 UI 行。
    struct RawHandleRecord
    {
        std::uint32_t processId = 0;       // processId：句柄所属进程 PID。
        std::uint64_t handleValue = 0;     // handleValue：句柄值。
        std::uint16_t typeIndex = 0;       // typeIndex：对象类型索引。
        std::uint64_t objectAddress = 0;   // objectAddress：对象地址。
        std::uint32_t grantedAccess = 0;   // grantedAccess：授权掩码。
        std::uint32_t attributes = 0;      // attributes：句柄属性位。
    };

    // ntStatusToHexText 作用：把 NTSTATUS 格式化为十六进制文本。
    QString ntStatusToHexText(const NTSTATUS statusValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<quint32>(statusValue), 8, 16, QChar('0'))
            .toUpper();
    }

    // queryNtApis 作用：加载 NtQuerySystemInformation/NtQueryObject 函数指针。
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
        apiSet.queryObject =
            reinterpret_cast<NtApiSet::NtQueryObjectFn>(
                ::GetProcAddress(apiSet.ntdllModule, "NtQueryObject"));
        return apiSet;
    }

    // collectProcessNameMap 作用：
    // - 使用 Toolhelp 快照建立 PID->进程名映射；
    // - 避免为每个句柄单独查进程名。
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

    // querySystemHandles 作用：
    // - 通过 NtQuerySystemInformation 读取系统句柄快照；
    // - 对缓冲区自动扩容直到成功或失败。
    bool querySystemHandles(
        const NtApiSet& apiSet,
        std::vector<RawHandleRecord>& recordsOut,
        QString& diagnosticTextOut)
    {
        recordsOut.clear();
        diagnosticTextOut.clear();

        if (!apiSet.ready())
        {
            diagnosticTextOut = QStringLiteral("Nt API 不可用，无法枚举系统句柄。");
            return false;
        }

        ULONG bufferSize = 1024 * 1024;
        for (int attemptIndex = 0; attemptIndex < 10; ++attemptIndex)
        {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(bufferSize), 0);
            ULONG returnLength = 0;
            const NTSTATUS status = apiSet.querySystemInformation(
                kSystemExtendedHandleInformationClass,
                buffer.data(),
                bufferSize,
                &returnLength);

            const bool needGrow =
                status == kStatusInfoLengthMismatch ||
                status == kStatusBufferOverflow ||
                status == kStatusBufferTooSmall;
            if (needGrow)
            {
                const ULONG recommendedSize = (returnLength > bufferSize)
                    ? returnLength + (256 * 1024)
                    : bufferSize * 2;
                bufferSize = std::max<ULONG>(recommendedSize, bufferSize + (256 * 1024));
                continue;
            }

            if (status < 0)
            {
                diagnosticTextOut = QStringLiteral("NtQuerySystemInformation 失败，status=%1")
                    .arg(ntStatusToHexText(status));
                return false;
            }

            if (buffer.size() < sizeof(SYSTEM_HANDLE_INFORMATION_EX_NATIVE))
            {
                diagnosticTextOut = QStringLiteral("句柄快照缓冲区尺寸异常（过小）。");
                return false;
            }

            const auto* handleHeader =
                reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX_NATIVE*>(buffer.data());
            const std::size_t recordCount = static_cast<std::size_t>(handleHeader->numberOfHandles);
            const std::size_t maxRecordsByBuffer =
                (buffer.size() - offsetof(SYSTEM_HANDLE_INFORMATION_EX_NATIVE, handles)) /
                sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE);
            const std::size_t safeRecordCount = std::min(recordCount, maxRecordsByBuffer);

            recordsOut.reserve(safeRecordCount);
            for (std::size_t index = 0; index < safeRecordCount; ++index)
            {
                const SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE& source = handleHeader->handles[index];
                RawHandleRecord row{};
                row.processId = static_cast<std::uint32_t>(source.uniqueProcessId);
                row.handleValue = static_cast<std::uint64_t>(source.handleValue);
                row.typeIndex = static_cast<std::uint16_t>(source.objectTypeIndex);
                row.objectAddress = reinterpret_cast<std::uint64_t>(source.objectAddress);
                row.grantedAccess = static_cast<std::uint32_t>(source.grantedAccess);
                row.attributes = static_cast<std::uint32_t>(source.handleAttributes);
                recordsOut.push_back(row);
            }

            if (safeRecordCount < recordCount)
            {
                diagnosticTextOut = QStringLiteral("句柄记录超出缓冲区，结果已截断。");
            }
            return true;
        }

        diagnosticTextOut = QStringLiteral("句柄快照缓冲区扩容次数已达上限。");
        return false;
    }

    // duplicateRemoteHandleToLocal 作用：
    // - 把远程进程句柄复制到当前进程；
    // - 后续 NtQueryObject 在本地句柄上执行。
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

    // queryUnicodeTextByNtObject 作用：
    // - 通用读取 NtQueryObject 返回的 UNICODE_STRING 文本；
    // - 兼容 NameInformation 与 TypeInformation 结构头。
    bool queryUnicodeTextByNtObject(
        const NtApiSet& apiSet,
        HANDLE objectHandle,
        const ULONG informationClass,
        QString& textOut)
    {
        textOut.clear();
        if (!apiSet.ready() || objectHandle == nullptr)
        {
            return false;
        }

        ULONG bufferSize = 1024;
        for (int attemptIndex = 0; attemptIndex < 8; ++attemptIndex)
        {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(bufferSize), 0);
            ULONG returnLength = 0;
            const NTSTATUS status = apiSet.queryObject(
                objectHandle,
                informationClass,
                buffer.data(),
                bufferSize,
                &returnLength);

            const bool needGrow =
                status == kStatusInfoLengthMismatch ||
                status == kStatusBufferOverflow ||
                status == kStatusBufferTooSmall;
            if (needGrow)
            {
                const ULONG recommendedSize = (returnLength > bufferSize)
                    ? returnLength + 256
                    : bufferSize * 2;
                bufferSize = std::max<ULONG>(recommendedSize, bufferSize + 256);
                continue;
            }

            if (status < 0)
            {
                return false;
            }

            const auto* unicodeValue = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
            if (unicodeValue == nullptr || unicodeValue->Buffer == nullptr || unicodeValue->Length == 0)
            {
                textOut.clear();
                return true;
            }

            textOut = QString::fromWCharArray(
                unicodeValue->Buffer,
                static_cast<int>(unicodeValue->Length / sizeof(wchar_t)));
            return true;
        }
        return false;
    }

    // queryObjectBasicInfo 作用：读取对象的 HandleCount/PointerCount。
    bool queryObjectBasicInfo(
        const NtApiSet& apiSet,
        HANDLE objectHandle,
        OBJECT_BASIC_INFORMATION_NATIVE& basicInfoOut)
    {
        std::memset(&basicInfoOut, 0, sizeof(basicInfoOut));
        if (!apiSet.ready() || objectHandle == nullptr)
        {
            return false;
        }

        const NTSTATUS status = apiSet.queryObject(
            objectHandle,
            kObjectBasicInformationClass,
            &basicInfoOut,
            static_cast<ULONG>(sizeof(basicInfoOut)),
            nullptr);
        return status >= 0;
    }

    // shouldAttemptNameQuery 作用：
    // - 按对象类型决定是否尝试名称解析；
    // - 避免对高风险类型做大量 Name 查询导致卡顿。
    bool shouldAttemptNameQuery(const QString& typeNameText)
    {
        if (typeNameText.trimmed().isEmpty())
        {
            return false;
        }

        const QString normalizedType = typeNameText.toLower();
        static const std::array<const char*, 22> kAllowTypeKeyword{
            "file",
            "directory",
            "symboliclink",
            "key",
            "event",
            "semaphore",
            "mutant",
            "timer",
            "section",
            "desktop",
            "windowstation",
            "port",
            "alpc",
            "job",
            "token",
            "process",
            "thread",
            "device",
            "driver",
            "wmi",
            "iocompletion",
            "filterconnectionport"
        };

        for (const char* keywordText : kAllowTypeKeyword)
        {
            if (normalizedType.contains(QString::fromLatin1(keywordText)))
            {
                return true;
            }
        }
        return false;
    }

    // rowMatchesKeyword 作用：判断行是否命中关键字搜索。
    template<typename TRow>
    bool rowMatchesKeyword(const TRow& row, const QString& keywordText)
    {
        if (keywordText.trimmed().isEmpty())
        {
            return true;
        }

        const QString normalizedKeyword = keywordText.toLower();
        const QString pidText = QString::number(row.processId);
        const QString typeIndexText = QString::number(row.typeIndex);
        const QString handleText = QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(row.handleValue), 0, 16)
            .toLower();
        const QString addressText = QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(row.objectAddress), 0, 16)
            .toLower();
        const QString accessText = QStringLiteral("0x%1")
            .arg(row.grantedAccess, 8, 16, QChar('0'))
            .toLower();

        return row.processName.toLower().contains(normalizedKeyword) ||
            row.typeName.toLower().contains(normalizedKeyword) ||
            row.objectName.toLower().contains(normalizedKeyword) ||
            pidText.contains(normalizedKeyword) ||
            typeIndexText.contains(normalizedKeyword) ||
            handleText.contains(normalizedKeyword) ||
            addressText.contains(normalizedKeyword) ||
            accessText.contains(normalizedKeyword);
    }
}

HandleDock::HandleRefreshResult HandleDock::buildHandleRefreshResult(const HandleRefreshOptions& options)
{
    HandleRefreshResult result{};
    const auto beginTime = std::chrono::steady_clock::now();

    // 第一步：枚举系统句柄快照。
    const NtApiSet apiSet = queryNtApis();
    std::vector<RawHandleRecord> rawRecords;
    QString queryDiagnosticText;
    if (!querySystemHandles(apiSet, rawRecords, queryDiagnosticText))
    {
        result.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
        result.diagnosticText = queryDiagnosticText;
        return result;
    }
    result.totalHandleCount = rawRecords.size();

    // 第二步：构建 PID->进程名映射，提升渲染可读性。
    const std::unordered_map<std::uint32_t, QString> processNameMap = collectProcessNameMap();
    auto processNameOf = [&processNameMap](const std::uint32_t processId) -> QString
        {
            const auto foundIt = processNameMap.find(processId);
            if (foundIt != processNameMap.end())
            {
                return foundIt->second;
            }
            return QStringLiteral("PID_%1").arg(processId);
        };

    // 第三步：优先使用“对象类型页”映射，再以兜底查询补齐缺失类型名。
    std::unordered_map<std::uint16_t, std::string> typeNameCache = options.typeNameCacheByIndex;
    for (const auto& pairItem : options.typeNameMapFromObjectTab)
    {
        typeNameCache[pairItem.first] = pairItem.second;
    }

    std::unordered_map<std::uint32_t, UniqueHandle> processHandleCache;
    std::unordered_set<std::uint32_t> failedProcessOpenSet;
    std::size_t typeQueryFailedCount = 0;

    auto openProcessHandleForDuplicate = [&](const std::uint32_t processId) -> HANDLE
        {
            const auto cacheIt = processHandleCache.find(processId);
            if (cacheIt != processHandleCache.end())
            {
                return cacheIt->second.get();
            }
            if (failedProcessOpenSet.find(processId) != failedProcessOpenSet.end())
            {
                return nullptr;
            }

            UniqueHandle processHandle(::OpenProcess(
                PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                processId));
            if (!processHandle.valid())
            {
                processHandle.reset(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, processId));
            }
            if (!processHandle.valid())
            {
                failedProcessOpenSet.insert(processId);
                return nullptr;
            }

            processHandleCache.insert_or_assign(processId, std::move(processHandle));
            return processHandleCache[processId].get();
        };

    std::unordered_map<std::uint16_t, const RawHandleRecord*> typeRepresentativeMap;
    for (const RawHandleRecord& rawRow : rawRecords)
    {
        if (rawRow.processId == 0)
        {
            continue;
        }
        if (typeNameCache.find(rawRow.typeIndex) != typeNameCache.end())
        {
            continue;
        }
        if (typeRepresentativeMap.find(rawRow.typeIndex) != typeRepresentativeMap.end())
        {
            continue;
        }
        typeRepresentativeMap[rawRow.typeIndex] = &rawRow;
    }

    for (const auto& pairItem : typeRepresentativeMap)
    {
        const std::uint16_t typeIndex = pairItem.first;
        const RawHandleRecord* representativeRow = pairItem.second;
        if (representativeRow == nullptr)
        {
            continue;
        }

        HANDLE sourceProcessHandle = openProcessHandleForDuplicate(representativeRow->processId);
        if (sourceProcessHandle == nullptr)
        {
            ++typeQueryFailedCount;
            continue;
        }

        UniqueHandle localHandle;
        if (!duplicateRemoteHandleToLocal(sourceProcessHandle, representativeRow->handleValue, localHandle))
        {
            ++typeQueryFailedCount;
            continue;
        }

        QString typeNameText;
        if (!queryUnicodeTextByNtObject(apiSet, localHandle.get(), kObjectTypeInformationClass, typeNameText))
        {
            ++typeQueryFailedCount;
            continue;
        }
        if (typeNameText.trimmed().isEmpty())
        {
            continue;
        }
        typeNameCache[typeIndex] = typeNameText.toStdString();
    }

    // 第四步：按过滤条件生成最终可见句柄行。
    int nameBudgetRemain = std::max(options.nameResolveBudget, 0);
    std::size_t nameQueryFailedCount = 0;
    std::size_t duplicateFailedCount = 0;
    std::set<QString> typeNameSet;
    result.rows.reserve(rawRecords.size());

    for (const RawHandleRecord& rawRow : rawRecords)
    {
        if (rawRow.processId == 0)
        {
            continue;
        }
        HandleRow row{};
        row.processId = rawRow.processId;
        row.processName = processNameOf(rawRow.processId);
        row.handleValue = rawRow.handleValue;
        row.typeIndex = rawRow.typeIndex;
        row.objectAddress = rawRow.objectAddress;
        row.grantedAccess = rawRow.grantedAccess;
        row.attributes = rawRow.attributes;

        const auto typeIt = typeNameCache.find(rawRow.typeIndex);
        if (typeIt != typeNameCache.end() && !typeIt->second.empty())
        {
            row.typeName = QString::fromStdString(typeIt->second);
            if (options.typeNameMapFromObjectTab.find(rawRow.typeIndex) != options.typeNameMapFromObjectTab.end())
            {
                ++result.objectTypeMappedCount;
            }
        }
        else
        {
            row.typeName = QStringLiteral("Type#%1").arg(rawRow.typeIndex);
        }
        typeNameSet.insert(row.typeName);

        const bool shouldResolveName =
            options.resolveObjectName &&
            nameBudgetRemain > 0 &&
            shouldAttemptNameQuery(row.typeName);
        if (shouldResolveName)
        {
            HANDLE sourceProcessHandle = openProcessHandleForDuplicate(rawRow.processId);
            if (sourceProcessHandle != nullptr)
            {
                UniqueHandle localHandle;
                if (duplicateRemoteHandleToLocal(sourceProcessHandle, rawRow.handleValue, localHandle))
                {
                    OBJECT_BASIC_INFORMATION_NATIVE basicInfo{};
                    if (queryObjectBasicInfo(apiSet, localHandle.get(), basicInfo))
                    {
                        row.handleCount = basicInfo.handleCount;
                        row.pointerCount = basicInfo.pointerCount;
                    }

                    QString objectNameText;
                    if (queryUnicodeTextByNtObject(apiSet, localHandle.get(), kObjectNameInformationClass, objectNameText))
                    {
                        row.objectName = objectNameText.trimmed();
                    }
                    else
                    {
                        ++nameQueryFailedCount;
                    }
                }
                else
                {
                    ++duplicateFailedCount;
                }
            }
            else
            {
                ++duplicateFailedCount;
            }
            --nameBudgetRemain;
        }

        if (!row.objectName.trimmed().isEmpty())
        {
            ++result.resolvedNameCount;
        }
        result.rows.push_back(std::move(row));
    }

    std::sort(
        result.rows.begin(),
        result.rows.end(),
        [](const HandleRow& leftRow, const HandleRow& rightRow)
        {
            if (leftRow.processId != rightRow.processId)
            {
                return leftRow.processId < rightRow.processId;
            }
            if (leftRow.typeIndex != rightRow.typeIndex)
            {
                return leftRow.typeIndex < rightRow.typeIndex;
            }
            return leftRow.handleValue < rightRow.handleValue;
        });

    result.visibleHandleCount = result.rows.size();
    for (const QString& typeNameText : typeNameSet)
    {
        result.availableTypeList.push_back(typeNameText);
    }
    result.updatedTypeNameCacheByIndex = std::move(typeNameCache);

    QStringList diagnosticList;
    if (!queryDiagnosticText.trimmed().isEmpty())
    {
        diagnosticList.push_back(queryDiagnosticText);
    }
    if (typeQueryFailedCount > 0)
    {
        diagnosticList.push_back(QStringLiteral("类型解析失败:%1").arg(typeQueryFailedCount));
    }
    if (duplicateFailedCount > 0)
    {
        diagnosticList.push_back(QStringLiteral("句柄复制失败:%1").arg(duplicateFailedCount));
    }
    if (nameQueryFailedCount > 0)
    {
        diagnosticList.push_back(QStringLiteral("对象名查询失败:%1").arg(nameQueryFailedCount));
    }
    if (options.resolveObjectName && nameBudgetRemain <= 0 && options.nameResolveBudget > 0)
    {
        diagnosticList.push_back(QStringLiteral("对象名解析已达到预算上限"));
    }
    result.diagnosticText = diagnosticList.join(QStringLiteral(" | "));

    result.elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beginTime).count());
    return result;
}

HandleDock::ObjectTypeRefreshResult HandleDock::buildObjectTypeRefreshResult()
{
    ObjectTypeRefreshResult result{};
    const auto beginTime = std::chrono::steady_clock::now();

    QString errorText;
    std::vector<HandleObjectTypeEntry> rows;
    const bool queryOk = runHandleObjectTypeSnapshotTask(rows, errorText);
    result.rows = std::move(rows);
    result.typeNameMapByIndex = buildTypeNameMapFromObjectTypeRows(result.rows);
    result.diagnosticText = errorText;
    result.elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beginTime).count());
    return result;
}

bool HandleDock::closeRemoteHandle(
    const std::uint32_t processId,
    const std::uint64_t handleValue,
    std::string& detailTextOut)
{
    detailTextOut.clear();

    UniqueHandle processHandle(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, processId));
    if (!processHandle.valid())
    {
        detailTextOut = "OpenProcess(PROCESS_DUP_HANDLE) failed, error=" + std::to_string(::GetLastError());
        return false;
    }

    const BOOL closeOk = ::DuplicateHandle(
        processHandle.get(),
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(handleValue)),
        nullptr,
        nullptr,
        0,
        FALSE,
        DUPLICATE_CLOSE_SOURCE);
    if (closeOk == FALSE)
    {
        detailTextOut = "DuplicateHandle(DUPLICATE_CLOSE_SOURCE) failed, error=" + std::to_string(::GetLastError());
        return false;
    }

    detailTextOut = "CloseSource success.";
    return true;
}

QString HandleDock::formatHex(const std::uint64_t value, const int width)
{
    if (width > 0)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), width, 16, QChar('0'))
            .toUpper();
    }
    return QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(value), 0, 16)
        .toUpper();
}

QString HandleDock::formatTypeIndexDisplayText(
    const std::uint16_t typeIndex,
    const QString& typeName)
{
    const QString trimmedTypeName = typeName.trimmed();
    const QString fallbackTypeText = QStringLiteral("Type#%1").arg(typeIndex);
    if (trimmedTypeName.isEmpty() ||
        trimmedTypeName.compare(fallbackTypeText, Qt::CaseInsensitive) == 0 ||
        trimmedTypeName.startsWith(QStringLiteral("<UnknownType_"), Qt::CaseInsensitive))
    {
        return QString::number(typeIndex);
    }

    return QStringLiteral("%1 (%2)")
        .arg(trimmedTypeName)
        .arg(typeIndex);
}

QString HandleDock::formatHandleAttributes(const std::uint32_t attributes)
{
    QStringList flagTextList;
    if ((attributes & 0x00000001U) != 0)
    {
        flagTextList.push_back(QStringLiteral("PROTECT"));
    }
    if ((attributes & 0x00000002U) != 0)
    {
        flagTextList.push_back(QStringLiteral("INHERIT"));
    }
    if ((attributes & 0x00000004U) != 0)
    {
        flagTextList.push_back(QStringLiteral("AUDIT"));
    }
    if (flagTextList.isEmpty())
    {
        return QStringLiteral("None");
    }
    return flagTextList.join('|');
}
