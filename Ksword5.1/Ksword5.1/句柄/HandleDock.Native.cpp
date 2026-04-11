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

    // CachedObjectSnapshot：
    // - 按 objectAddress 缓存“对象级别”的成功查询结果；
    // - 让多条句柄指向同一对象时复用 BasicInformation 与对象名；
    // - 仅缓存成功结果，避免错误负缓存误伤后续可查询句柄。
    struct CachedObjectSnapshot
    {
        bool basicInfoAvailable = false;       // basicInfoAvailable：是否已有成功的 BasicInformation 结果。
        std::uint32_t handleCount = 0;         // handleCount：缓存的 HandleCount。
        std::uint32_t pointerCount = 0;        // pointerCount：缓存的 PointerCount。
        bool objectNameAvailable = false;      // objectNameAvailable：是否已有成功的对象名结果。
        bool objectNameFromFallback = false;   // objectNameFromFallback：对象名是否来自类型专用回退。
        QString objectName;                    // objectName：缓存的对象名文本，允许为空表示对象确实无名称。
    };

    // ObjectNameQueryResult：
    // - 表示单次对象名解析的结果状态；
    // - 区分“成功但为空”“查询失败”“回退命中”等状态。
    struct ObjectNameQueryResult
    {
        bool available = false;                // available：对象名查询成功完成，允许 objectName 为空。
        bool failed = false;                   // failed：对象名查询已尝试但失败。
        bool usedFallback = false;             // usedFallback：对象名来自类型专用回退。
        QString objectName;                    // objectName：对象名文本。
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

    // normalizeFinalPathText 作用：
    // - 统一裁剪 GetFinalPathNameByHandleW 返回的 \\?\ 前缀；
    // - 让文件对象名在 UI 中更接近日常 Windows 路径格式。
    QString normalizeFinalPathText(QString pathText)
    {
        if (pathText.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive))
        {
            pathText = QStringLiteral("\\\\") + pathText.mid(8);
        }
        else if (pathText.startsWith(QStringLiteral("\\\\?\\"), Qt::CaseInsensitive))
        {
            pathText = pathText.mid(4);
        }
        return pathText.trimmed();
    }

    // queryFileObjectDisplayName 作用：
    // - 用 Win32 文件路径接口补 File/Directory 类型对象名；
    // - 作为 NtQueryObject(ObjectNameInformation) 的回退路径。
    bool queryFileObjectDisplayName(HANDLE objectHandle, QString& textOut)
    {
        textOut.clear();
        DWORD bufferChars = 512;
        for (int attemptIndex = 0; attemptIndex < 6; ++attemptIndex)
        {
            std::vector<wchar_t> pathBuffer(static_cast<std::size_t>(bufferChars), L'\0');
            const DWORD pathLength = ::GetFinalPathNameByHandleW(
                objectHandle,
                pathBuffer.data(),
                bufferChars,
                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (pathLength == 0)
            {
                return false;
            }
            if (pathLength >= bufferChars)
            {
                bufferChars = pathLength + 1;
                continue;
            }

            textOut = normalizeFinalPathText(QString::fromWCharArray(pathBuffer.data(), static_cast<int>(pathLength)));
            return !textOut.isEmpty();
        }
        return false;
    }

    // queryProcessObjectDisplayName 作用：
    // - 为 Process 句柄生成“PID + 路径”可读文本；
    // - 解决进程对象常常没有标准对象名的问题。
    bool queryProcessObjectDisplayName(HANDLE objectHandle, QString& textOut)
    {
        textOut.clear();
        const DWORD targetProcessId = ::GetProcessId(objectHandle);
        if (targetProcessId == 0)
        {
            return false;
        }

        std::vector<wchar_t> pathBuffer(2048, L'\0');
        DWORD bufferChars = static_cast<DWORD>(pathBuffer.size());
        if (::QueryFullProcessImageNameW(objectHandle, 0, pathBuffer.data(), &bufferChars) != FALSE &&
            bufferChars > 0)
        {
            textOut = QStringLiteral("PID %1 | %2")
                .arg(targetProcessId)
                .arg(QString::fromWCharArray(pathBuffer.data(), static_cast<int>(bufferChars)).trimmed());
            return true;
        }

        textOut = QStringLiteral("PID %1").arg(targetProcessId);
        return true;
    }

    // queryThreadObjectDisplayName 作用：
    // - 为 Thread 句柄生成“PID/TID”身份文本；
    // - 让无对象名的线程句柄也能有稳定识别信息。
    bool queryThreadObjectDisplayName(HANDLE objectHandle, QString& textOut)
    {
        textOut.clear();
        const DWORD targetThreadId = ::GetThreadId(objectHandle);
        if (targetThreadId == 0)
        {
            return false;
        }

        const DWORD ownerProcessId = ::GetProcessIdOfThread(objectHandle);
        if (ownerProcessId != 0)
        {
            textOut = QStringLiteral("PID %1 / TID %2")
                .arg(ownerProcessId)
                .arg(targetThreadId);
            return true;
        }

        textOut = QStringLiteral("TID %1").arg(targetThreadId);
        return true;
    }

    // queryTokenObjectDisplayName 作用：
    // - 为 Token 句柄生成“域\\账户”或 SID 文本；
    // - 让常见令牌句柄在对象名列中也能快速识别归属用户。
    bool queryTokenObjectDisplayName(HANDLE objectHandle, QString& textOut)
    {
        textOut.clear();

        DWORD requiredLength = 0;
        ::GetTokenInformation(objectHandle, TokenUser, nullptr, 0, &requiredLength);
        if (requiredLength == 0)
        {
            return false;
        }

        std::vector<std::uint8_t> tokenBuffer(static_cast<std::size_t>(requiredLength), 0);
        if (::GetTokenInformation(
            objectHandle,
            TokenUser,
            tokenBuffer.data(),
            requiredLength,
            &requiredLength) == FALSE)
        {
            return false;
        }

        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
        if (tokenUser == nullptr || tokenUser->User.Sid == nullptr)
        {
            return false;
        }

        wchar_t accountName[256] = {};
        wchar_t domainName[256] = {};
        DWORD accountNameLength = static_cast<DWORD>(std::size(accountName));
        DWORD domainNameLength = static_cast<DWORD>(std::size(domainName));
        SID_NAME_USE sidType = SidTypeUnknown;
        if (::LookupAccountSidW(
            nullptr,
            tokenUser->User.Sid,
            accountName,
            &accountNameLength,
            domainName,
            &domainNameLength,
            &sidType) != FALSE)
        {
            textOut = QStringLiteral("%1\\%2")
                .arg(QString::fromWCharArray(domainName))
                .arg(QString::fromWCharArray(accountName));
            return true;
        }

        LPWSTR sidText = nullptr;
        if (::ConvertSidToStringSidW(tokenUser->User.Sid, &sidText) != FALSE && sidText != nullptr)
        {
            textOut = QString::fromWCharArray(sidText);
            ::LocalFree(sidText);
            return true;
        }
        return false;
    }

    // queryTypeSpecificObjectName 作用：
    // - 按对象类型选择更合适的回退查询方案；
    // - 用于提升 File/Process/Thread/Token 等类型的对象名覆盖率。
    bool queryTypeSpecificObjectName(
        const QString& typeNameText,
        HANDLE objectHandle,
        QString& textOut)
    {
        const QString normalizedType = typeNameText.trimmed().toLower();
        if (normalizedType == QStringLiteral("file") || normalizedType == QStringLiteral("directory"))
        {
            return queryFileObjectDisplayName(objectHandle, textOut);
        }
        if (normalizedType == QStringLiteral("process"))
        {
            return queryProcessObjectDisplayName(objectHandle, textOut);
        }
        if (normalizedType == QStringLiteral("thread"))
        {
            return queryThreadObjectDisplayName(objectHandle, textOut);
        }
        if (normalizedType == QStringLiteral("token"))
        {
            return queryTokenObjectDisplayName(objectHandle, textOut);
        }
        textOut.clear();
        return false;
    }

    // resolveObjectNameText 作用：
    // - 先尝试 NtQueryObject(ObjectNameInformation)；
    // - 若失败或结果为空，再走类型专用回退；
    // - 最终把成功/失败/回退状态一次性返回给调用方。
    ObjectNameQueryResult resolveObjectNameText(
        const NtApiSet& apiSet,
        HANDLE objectHandle,
        const QString& typeNameText)
    {
        ObjectNameQueryResult result{};

        QString ntObjectNameText;
        const bool ntQueryOk = queryUnicodeTextByNtObject(
            apiSet,
            objectHandle,
            kObjectNameInformationClass,
            ntObjectNameText);
        if (ntQueryOk)
        {
            result.available = true;
            result.objectName = ntObjectNameText.trimmed();
            if (!result.objectName.isEmpty())
            {
                return result;
            }
        }

        QString fallbackText;
        if (queryTypeSpecificObjectName(typeNameText, objectHandle, fallbackText))
        {
            result.available = true;
            result.usedFallback = !fallbackText.trimmed().isEmpty();
            result.objectName = fallbackText.trimmed();
            return result;
        }

        if (ntQueryOk)
        {
            result.objectName.clear();
            return result;
        }

        result.failed = true;
        return result;
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
        static const std::array<const char*, 28> kAllowTypeKeyword{
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
            "filterconnectionport",
            "waitcompletionpacket",
            "session",
            "keyedevent",
            "eventpair",
            "iocompletionreserve",
            "partition"
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
    std::size_t basicInfoFailedCount = 0;
    std::size_t nameQueryFailedCount = 0;
    std::size_t duplicateFailedCount = 0;
    std::size_t nameBudgetSkippedCount = 0;
    std::unordered_map<std::uint64_t, CachedObjectSnapshot> objectSnapshotCacheByAddress;
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

        // 对象级缓存优先复用成功结果，减少同一对象的重复复制与重复查询。
        if (rawRow.objectAddress != 0)
        {
            const auto cachedObjectIt = objectSnapshotCacheByAddress.find(rawRow.objectAddress);
            if (cachedObjectIt != objectSnapshotCacheByAddress.end())
            {
                const CachedObjectSnapshot& cachedSnapshot = cachedObjectIt->second;
                if (cachedSnapshot.basicInfoAvailable)
                {
                    row.basicInfoAvailable = true;
                    row.handleCount = cachedSnapshot.handleCount;
                    row.pointerCount = cachedSnapshot.pointerCount;
                }
                if (cachedSnapshot.objectNameAvailable)
                {
                    row.objectNameAvailable = true;
                    row.objectNameFromFallback = cachedSnapshot.objectNameFromFallback;
                    row.objectName = cachedSnapshot.objectName;
                }
            }
        }

        // 预算聚焦策略：
        // - 若用户指定 PID/类型过滤，则优先把预算用于该范围；
        // - 保持 m_allRows 仍是完整快照，不破坏本地过滤能力。
        const bool pidMatchedForBudget =
            !options.hasPidFilter ||
            row.processId == options.pidFilter;
        const QString typeFilterText = options.typeFilterText.trimmed();
        const bool hasTypeFilter =
            !typeFilterText.isEmpty() &&
            typeFilterText != QStringLiteral("全部类型");
        const bool typeMatchedForBudget =
            !hasTypeFilter ||
            row.typeName.compare(typeFilterText, Qt::CaseInsensitive) == 0;
        const bool candidateForNameResolve = pidMatchedForBudget && typeMatchedForBudget;

        // BasicInformation 与对象名查询彻底解耦：
        // - 只要 DuplicateHandle 成功，就尝试独立读取 HandleCount/PointerCount；
        // - 对象名仍受预算与类型白名单控制，避免名称查询无限膨胀。
        const bool shouldQueryBasicInfo = !row.basicInfoAvailable;
        const bool typeEligibleForNameResolve =
            options.resolveObjectName &&
            candidateForNameResolve &&
            shouldAttemptNameQuery(row.typeName);
        bool shouldQueryObjectName = false;
        if (typeEligibleForNameResolve && !row.objectNameAvailable)
        {
            if (nameBudgetRemain > 0)
            {
                shouldQueryObjectName = true;
                --nameBudgetRemain;
            }
            else
            {
                ++nameBudgetSkippedCount;
            }
        }

        if (shouldQueryBasicInfo || shouldQueryObjectName)
        {
            HANDLE sourceProcessHandle = openProcessHandleForDuplicate(rawRow.processId);
            if (sourceProcessHandle != nullptr)
            {
                UniqueHandle localHandle;
                if (duplicateRemoteHandleToLocal(sourceProcessHandle, rawRow.handleValue, localHandle))
                {
                    CachedObjectSnapshot* cachedSnapshot = nullptr;
                    if (rawRow.objectAddress != 0)
                    {
                        cachedSnapshot = &objectSnapshotCacheByAddress[rawRow.objectAddress];
                    }

                    if (shouldQueryBasicInfo)
                    {
                        OBJECT_BASIC_INFORMATION_NATIVE basicInfo{};
                        if (queryObjectBasicInfo(apiSet, localHandle.get(), basicInfo))
                        {
                            row.basicInfoAvailable = true;
                            row.handleCount = basicInfo.handleCount;
                            row.pointerCount = basicInfo.pointerCount;
                            if (cachedSnapshot != nullptr)
                            {
                                cachedSnapshot->basicInfoAvailable = true;
                                cachedSnapshot->handleCount = basicInfo.handleCount;
                                cachedSnapshot->pointerCount = basicInfo.pointerCount;
                            }
                        }
                        else
                        {
                            ++basicInfoFailedCount;
                        }
                    }

                    if (shouldQueryObjectName)
                    {
                        const ObjectNameQueryResult nameQueryResult =
                            resolveObjectNameText(apiSet, localHandle.get(), row.typeName);
                        if (nameQueryResult.available)
                        {
                            row.objectNameAvailable = true;
                            row.objectNameFailed = false;
                            row.objectNameFromFallback = nameQueryResult.usedFallback;
                            row.objectName = nameQueryResult.objectName;
                            if (cachedSnapshot != nullptr)
                            {
                                cachedSnapshot->objectNameAvailable = true;
                                cachedSnapshot->objectNameFromFallback = nameQueryResult.usedFallback;
                                cachedSnapshot->objectName = nameQueryResult.objectName;
                            }
                        }
                        else
                        {
                            row.objectNameFailed = nameQueryResult.failed;
                            if (nameQueryResult.failed)
                            {
                                ++nameQueryFailedCount;
                            }
                        }
                    }
                }
                else
                {
                    ++duplicateFailedCount;
                    if (shouldQueryBasicInfo)
                    {
                        ++basicInfoFailedCount;
                    }
                    if (shouldQueryObjectName)
                    {
                        row.objectNameFailed = true;
                        ++nameQueryFailedCount;
                    }
                }
            }
            else
            {
                ++duplicateFailedCount;
                if (shouldQueryBasicInfo)
                {
                    ++basicInfoFailedCount;
                }
                if (shouldQueryObjectName)
                {
                    row.objectNameFailed = true;
                    ++nameQueryFailedCount;
                }
            }
        }

        if (row.basicInfoAvailable)
        {
            ++result.basicInfoResolvedCount;
        }
        if (row.objectNameAvailable && !row.objectName.trimmed().isEmpty())
        {
            ++result.resolvedNameCount;
            if (row.objectNameFromFallback)
            {
                ++result.fallbackNameCount;
            }
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
    if (basicInfoFailedCount > 0)
    {
        diagnosticList.push_back(QStringLiteral("对象计数查询失败:%1").arg(basicInfoFailedCount));
    }
    if (nameQueryFailedCount > 0)
    {
        diagnosticList.push_back(QStringLiteral("对象名查询失败:%1").arg(nameQueryFailedCount));
    }
    if (result.fallbackNameCount > 0)
    {
        diagnosticList.push_back(QStringLiteral("对象名回退命中:%1").arg(result.fallbackNameCount));
    }
    if (nameBudgetSkippedCount > 0)
    {
        diagnosticList.push_back(QStringLiteral("对象名预算跳过:%1").arg(nameBudgetSkippedCount));
    }
    if (options.resolveObjectName && nameBudgetSkippedCount > 0 && options.nameResolveBudget > 0)
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

QString HandleDock::formatOptionalObjectCount(
    const std::uint32_t countValue,
    const bool countAvailable)
{
    if (!countAvailable)
    {
        return QStringLiteral("未查到");
    }
    return QString::number(countValue);
}

QString HandleDock::formatObjectNameDisplayText(const HandleRow& row)
{
    if (row.objectNameAvailable)
    {
        if (row.objectName.trimmed().isEmpty())
        {
            return QStringLiteral("无名称");
        }
        return row.objectName;
    }

    if (row.objectNameFailed)
    {
        return QStringLiteral("未查到");
    }

    return QStringLiteral("未查询");
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
