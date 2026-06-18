#include "KernelBaseNamedObjectsWorker.h"

// ============================================================
// KernelBaseNamedObjectsWorker.cpp
// 作用：
// 1) R3 只读枚举 BaseNamedObjects 相关对象目录；
// 2) 聚合 Global、当前 Session、可发现 Session 的一层命名对象；
// 3) 对 SymbolicLink 额外解析目标，对 Directory 仅标记可继续枚举。
// ============================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef DIRECTORY_QUERY
#define DIRECTORY_QUERY 0x0001
#endif
#ifndef DIRECTORY_TRAVERSE
#define DIRECTORY_TRAVERSE 0x0002
#endif
#ifndef SYMBOLIC_LINK_QUERY
#define SYMBOLIC_LINK_QUERY 0x0001
#endif

namespace
{
    constexpr NTSTATUS kStatusSuccess = static_cast<NTSTATUS>(0x00000000L);
    constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001L);
    constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
    constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);
    constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
    constexpr NTSTATUS kStatusNoMoreEntries = static_cast<NTSTATUS>(0x8000001AL);
    constexpr unsigned long kGlobalScopeSessionSentinel = std::numeric_limits<unsigned long>::max();
    constexpr std::size_t kMaxEntriesPerDirectory = 8192;
    constexpr ULONG kInitialDirectoryQueryBytes = 16 * 1024U;
    constexpr ULONG kInitialSymbolicLinkChars = 1024U;
    constexpr ACCESS_MASK kDirectoryQueryAccess = DIRECTORY_QUERY | DIRECTORY_TRAVERSE;
    constexpr ACCESS_MASK kSymbolicLinkQueryAccess = SYMBOLIC_LINK_QUERY;

    using NtOpenDirectoryObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtQueryDirectoryObjectFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
    using NtOpenSymbolicLinkObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtQuerySymbolicLinkObjectFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);

    // NtBaseNamedObjectsApi：
    // - 输入：无，由 loadNtApi 动态填充；
    // - 处理：保存本 worker 需要的 ntdll 入口；
    // - 返回行为：纯结构体，无函数返回。
    struct NtBaseNamedObjectsApi
    {
        HMODULE ntdllModule = nullptr;
        NtOpenDirectoryObjectFn openDirectoryObject = nullptr;
        NtQueryDirectoryObjectFn queryDirectoryObject = nullptr;
        NtOpenSymbolicLinkObjectFn openSymbolicLinkObject = nullptr;
        NtQuerySymbolicLinkObjectFn querySymbolicLinkObject = nullptr;
    };

    // ScopedNtHandle：
    // - 输入：Nt* 打开的 HANDLE；
    // - 处理：析构时 CloseHandle；
    // - 返回行为：不返回值，只负责生命周期。
    class ScopedNtHandle final
    {
    public:
        ~ScopedNtHandle()
        {
            if (m_handle != nullptr)
            {
                ::CloseHandle(m_handle);
                m_handle = nullptr;
            }
        }

        HANDLE* put()
        {
            return &m_handle;
        }

        HANDLE get() const
        {
            return m_handle;
        }

        ScopedNtHandle() = default;
        ScopedNtHandle(const ScopedNtHandle&) = delete;
        ScopedNtHandle& operator=(const ScopedNtHandle&) = delete;

    private:
        HANDLE m_handle = nullptr;
    };

    struct DirectoryRecord
    {
        QString nameText;
        QString typeText;
    };

    struct DirectorySpec
    {
        QString scopeText;
        QString pathText;
        unsigned long sessionId = kGlobalScopeSessionSentinel;
        bool hasSessionId = false;
    };

    struct ObjectDirectoryInformation
    {
        UNICODE_STRING Name;
        UNICODE_STRING TypeName;
    };

    QString normalizeObjectPath(const QString& rawPathText)
    {
        QString pathText = rawPathText.trimmed();
        pathText.replace('/', '\\');
        while (pathText.contains(QStringLiteral("\\\\")))
        {
            pathText.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }
        if (pathText.isEmpty())
        {
            return QStringLiteral("\\");
        }
        if (!pathText.startsWith('\\'))
        {
            pathText.prepend('\\');
        }
        return pathText;
    }

    QString joinObjectPath(const QString& directoryPathText, const QString& objectNameText)
    {
        const QString directoryText = normalizeObjectPath(directoryPathText);
        if (directoryText == QStringLiteral("\\"))
        {
            return QStringLiteral("\\%1").arg(objectNameText);
        }
        return directoryText.endsWith('\\')
            ? directoryText + objectNameText
            : directoryText + QStringLiteral("\\") + objectNameText;
    }

    void initializeUnicodeString(UNICODE_STRING& unicodeOut, const std::wstring& sourceText)
    {
        unicodeOut.Buffer = const_cast<PWSTR>(sourceText.c_str());
        unicodeOut.Length = static_cast<USHORT>(
            std::min<std::size_t>(
                sourceText.size() * sizeof(wchar_t),
                static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));
        unicodeOut.MaximumLength = static_cast<USHORT>(unicodeOut.Length + sizeof(wchar_t));
    }

    QString unicodeStringToQString(const UNICODE_STRING& unicodeText)
    {
        if (unicodeText.Buffer == nullptr || unicodeText.Length == 0)
        {
            return QString();
        }
        return QString::fromWCharArray(
            unicodeText.Buffer,
            unicodeText.Length / static_cast<USHORT>(sizeof(wchar_t)));
    }

    bool shouldGrowBuffer(const NTSTATUS statusCode)
    {
        return statusCode == kStatusInfoLengthMismatch
            || statusCode == kStatusBufferOverflow
            || statusCode == kStatusBufferTooSmall;
    }

    QString ntStatusToText(const HMODULE ntdllModule, const NTSTATUS statusCode)
    {
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(statusCode), 8, 16, QChar('0'))
            .toUpper();

        std::array<wchar_t, 256> messageBuffer{};
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            ntdllModule,
            static_cast<DWORD>(statusCode),
            0,
            messageBuffer.data(),
            static_cast<DWORD>(messageBuffer.size()),
            nullptr);
        if (messageLength == 0)
        {
            return hexText;
        }
        return QStringLiteral("%1 %2")
            .arg(hexText, QString::fromWCharArray(messageBuffer.data()).trimmed());
    }

    bool loadNtApi(NtBaseNamedObjectsApi& apiOut, QString& errorTextOut)
    {
        errorTextOut.clear();
        apiOut.ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (apiOut.ntdllModule == nullptr)
        {
            apiOut.ntdllModule = ::LoadLibraryW(L"ntdll.dll");
        }
        if (apiOut.ntdllModule == nullptr)
        {
            errorTextOut = QStringLiteral("加载 ntdll.dll 失败。");
            return false;
        }

        apiOut.openDirectoryObject = reinterpret_cast<NtOpenDirectoryObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtOpenDirectoryObject"));
        apiOut.queryDirectoryObject = reinterpret_cast<NtQueryDirectoryObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtQueryDirectoryObject"));
        apiOut.openSymbolicLinkObject = reinterpret_cast<NtOpenSymbolicLinkObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtOpenSymbolicLinkObject"));
        apiOut.querySymbolicLinkObject = reinterpret_cast<NtQuerySymbolicLinkObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtQuerySymbolicLinkObject"));

        if (apiOut.openDirectoryObject == nullptr
            || apiOut.queryDirectoryObject == nullptr
            || apiOut.openSymbolicLinkObject == nullptr
            || apiOut.querySymbolicLinkObject == nullptr)
        {
            errorTextOut = QStringLiteral("解析对象目录 Nt API 失败。");
            return false;
        }
        return true;
    }

    NTSTATUS openDirectory(
        const NtBaseNamedObjectsApi& api,
        const QString& directoryPathText,
        ScopedNtHandle& handleOut)
    {
        const std::wstring pathWideText = normalizeObjectPath(directoryPathText).toStdWString();
        UNICODE_STRING objectName{};
        initializeUnicodeString(objectName, pathWideText);

        OBJECT_ATTRIBUTES objectAttributes{};
        InitializeObjectAttributes(&objectAttributes, &objectName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        return api.openDirectoryObject(handleOut.put(), kDirectoryQueryAccess, &objectAttributes);
    }

    NTSTATUS openSymbolicLink(
        const NtBaseNamedObjectsApi& api,
        const QString& symbolicPathText,
        ScopedNtHandle& handleOut)
    {
        const std::wstring pathWideText = normalizeObjectPath(symbolicPathText).toStdWString();
        UNICODE_STRING objectName{};
        initializeUnicodeString(objectName, pathWideText);

        OBJECT_ATTRIBUTES objectAttributes{};
        InitializeObjectAttributes(&objectAttributes, &objectName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        return api.openSymbolicLinkObject(handleOut.put(), kSymbolicLinkQueryAccess, &objectAttributes);
    }

    bool enumerateDirectory(
        const NtBaseNamedObjectsApi& api,
        const QString& directoryPathText,
        std::vector<DirectoryRecord>& recordsOut,
        QString& statusTextOut)
    {
        recordsOut.clear();
        statusTextOut.clear();

        ScopedNtHandle directoryHandle;
        NTSTATUS statusCode = openDirectory(api, directoryPathText, directoryHandle);
        if (!NT_SUCCESS(statusCode))
        {
            statusTextOut = ntStatusToText(api.ntdllModule, statusCode);
            return false;
        }

        ULONG queryContext = 0;
        BOOLEAN restartScan = TRUE;
        ULONG queryBufferBytes = kInitialDirectoryQueryBytes;

        for (std::size_t entryIndex = 0; entryIndex < kMaxEntriesPerDirectory; ++entryIndex)
        {
            std::vector<std::uint8_t> queryBuffer;
            NTSTATUS queryStatus = kStatusUnsuccessful;
            ULONG returnLength = 0;
            bool queryCompleted = false;

            for (int retryIndex = 0; retryIndex < 6; ++retryIndex)
            {
                queryBuffer.assign(queryBufferBytes, 0);
                queryStatus = api.queryDirectoryObject(
                    directoryHandle.get(),
                    queryBuffer.data(),
                    queryBufferBytes,
                    TRUE,
                    restartScan,
                    &queryContext,
                    &returnLength);

                if (queryStatus == kStatusNoMoreEntries)
                {
                    statusTextOut = ntStatusToText(api.ntdllModule, kStatusSuccess);
                    return true;
                }
                if (shouldGrowBuffer(queryStatus))
                {
                    queryBufferBytes = std::max(queryBufferBytes * 2U, returnLength + 512U);
                    continue;
                }

                queryCompleted = true;
                break;
            }

            restartScan = FALSE;
            if (!queryCompleted)
            {
                statusTextOut = ntStatusToText(api.ntdllModule, queryStatus);
                return false;
            }
            if (!NT_SUCCESS(queryStatus))
            {
                statusTextOut = ntStatusToText(api.ntdllModule, queryStatus);
                return false;
            }
            if (queryBuffer.size() < sizeof(ObjectDirectoryInformation))
            {
                statusTextOut = ntStatusToText(api.ntdllModule, kStatusBufferTooSmall);
                return false;
            }

            const auto* info = reinterpret_cast<const ObjectDirectoryInformation*>(queryBuffer.data());
            DirectoryRecord record;
            record.nameText = unicodeStringToQString(info->Name).trimmed();
            record.typeText = unicodeStringToQString(info->TypeName).trimmed();
            if (!record.nameText.isEmpty())
            {
                recordsOut.push_back(std::move(record));
            }
        }

        statusTextOut = QStringLiteral("目录项超过上限，结果已截断。");
        return true;
    }

    bool querySymbolicLinkTarget(
        const NtBaseNamedObjectsApi& api,
        const QString& symbolicPathText,
        QString& targetTextOut,
        QString& statusTextOut)
    {
        targetTextOut.clear();
        statusTextOut.clear();

        ScopedNtHandle linkHandle;
        NTSTATUS statusCode = openSymbolicLink(api, symbolicPathText, linkHandle);
        if (!NT_SUCCESS(statusCode))
        {
            statusTextOut = ntStatusToText(api.ntdllModule, statusCode);
            return false;
        }

        ULONG targetChars = kInitialSymbolicLinkChars;
        for (int retryIndex = 0; retryIndex < 6; ++retryIndex)
        {
            std::vector<wchar_t> targetBuffer(targetChars, L'\0');
            UNICODE_STRING targetUnicode{};
            targetUnicode.Buffer = targetBuffer.data();
            targetUnicode.Length = 0;
            targetUnicode.MaximumLength = static_cast<USHORT>(
                std::min<std::size_t>(
                    targetChars * sizeof(wchar_t),
                    static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));

            ULONG returnLength = 0;
            statusCode = api.querySymbolicLinkObject(linkHandle.get(), &targetUnicode, &returnLength);
            if (NT_SUCCESS(statusCode))
            {
                targetTextOut = QString::fromWCharArray(
                    targetUnicode.Buffer,
                    targetUnicode.Length / static_cast<USHORT>(sizeof(wchar_t)));
                statusTextOut = ntStatusToText(api.ntdllModule, statusCode);
                return true;
            }
            if (!shouldGrowBuffer(statusCode))
            {
                statusTextOut = ntStatusToText(api.ntdllModule, statusCode);
                return false;
            }

            targetChars = std::max<ULONG>(
                targetChars * 2U,
                static_cast<ULONG>(returnLength / sizeof(wchar_t) + 8U));
        }

        statusTextOut = QStringLiteral("符号链接目标解析重试达到上限。");
        return false;
    }

    bool isNumericText(const QString& textValue)
    {
        const QString trimmedText = textValue.trimmed();
        if (trimmedText.isEmpty())
        {
            return false;
        }
        for (const QChar singleChar : trimmedText)
        {
            if (!singleChar.isDigit())
            {
                return false;
            }
        }
        return true;
    }

    QString classifyObjectType(const QString& objectTypeText)
    {
        const QString typeText = objectTypeText.trimmed();
        if (typeText.compare(QStringLiteral("Event"), Qt::CaseInsensitive) == 0) return QStringLiteral("Event");
        if (typeText.compare(QStringLiteral("Mutant"), Qt::CaseInsensitive) == 0) return QStringLiteral("Mutant");
        if (typeText.compare(QStringLiteral("Semaphore"), Qt::CaseInsensitive) == 0) return QStringLiteral("Semaphore");
        if (typeText.compare(QStringLiteral("Section"), Qt::CaseInsensitive) == 0) return QStringLiteral("Section");
        if (typeText.compare(QStringLiteral("Timer"), Qt::CaseInsensitive) == 0) return QStringLiteral("Timer");
        if (typeText.compare(QStringLiteral("Job"), Qt::CaseInsensitive) == 0) return QStringLiteral("Job");
        if (typeText.compare(QStringLiteral("Directory"), Qt::CaseInsensitive) == 0) return QStringLiteral("Directory");
        if (typeText.compare(QStringLiteral("SymbolicLink"), Qt::CaseInsensitive) == 0) return QStringLiteral("SymbolicLink");
        return QStringLiteral("Other");
    }

    std::vector<unsigned long> discoverSessionIds(const NtBaseNamedObjectsApi& api)
    {
        std::set<unsigned long> sessionIds;

        DWORD currentSessionId = 0;
        if (::ProcessIdToSessionId(::GetCurrentProcessId(), &currentSessionId) != FALSE)
        {
            sessionIds.insert(static_cast<unsigned long>(currentSessionId));
        }

        std::vector<DirectoryRecord> sessionRecords;
        QString statusText;
        if (enumerateDirectory(api, QStringLiteral("\\Sessions"), sessionRecords, statusText))
        {
            for (const DirectoryRecord& record : sessionRecords)
            {
                if (record.typeText.compare(QStringLiteral("Directory"), Qt::CaseInsensitive) != 0)
                {
                    continue;
                }
                if (!isNumericText(record.nameText))
                {
                    continue;
                }

                bool parseOk = false;
                const unsigned long sessionId = record.nameText.toULong(&parseOk);
                if (parseOk)
                {
                    sessionIds.insert(sessionId);
                }
            }
        }

        return std::vector<unsigned long>(sessionIds.begin(), sessionIds.end());
    }

    std::vector<DirectorySpec> buildDirectorySpecs(const NtBaseNamedObjectsApi& api)
    {
        std::vector<DirectorySpec> specs;
        specs.push_back(DirectorySpec{
            QStringLiteral("Global"),
            QStringLiteral("\\BaseNamedObjects"),
            kGlobalScopeSessionSentinel,
            false
            });

        DWORD currentSessionId = kGlobalScopeSessionSentinel;
        const bool hasCurrentSession =
            ::ProcessIdToSessionId(::GetCurrentProcessId(), &currentSessionId) != FALSE;

        const std::vector<unsigned long> sessionIds = discoverSessionIds(api);
        for (const unsigned long sessionId : sessionIds)
        {
            const bool isCurrentSession = hasCurrentSession && sessionId == currentSessionId;
            specs.push_back(DirectorySpec{
                isCurrentSession
                    ? QStringLiteral("Current Session %1").arg(sessionId)
                    : QStringLiteral("Session %1").arg(sessionId),
                QStringLiteral("\\Sessions\\%1\\BaseNamedObjects").arg(sessionId),
                sessionId,
                true
                });
        }

        return specs;
    }
}

bool runBaseNamedObjectsSnapshotTask(
    std::vector<KernelBaseNamedObjectEntry>& rowsOut,
    QString& errorTextOut)
{
    rowsOut.clear();
    errorTextOut.clear();

    NtBaseNamedObjectsApi api;
    if (!loadNtApi(api, errorTextOut))
    {
        return false;
    }

    std::vector<KernelBaseNamedObjectEntry> resultRows;
    for (const DirectorySpec& spec : buildDirectorySpecs(api))
    {
        std::vector<DirectoryRecord> records;
        QString directoryStatusText;
        const bool enumOk = enumerateDirectory(api, spec.pathText, records, directoryStatusText);
        if (!enumOk)
        {
            KernelBaseNamedObjectEntry failureEntry;
            failureEntry.scopeText = spec.scopeText;
            failureEntry.directoryPathText = spec.pathText;
            failureEntry.objectTypeText = QStringLiteral("Directory");
            failureEntry.typeCategoryText = QStringLiteral("Directory");
            failureEntry.fullPathText = spec.pathText;
            failureEntry.statusText = QStringLiteral("目录枚举失败：%1").arg(directoryStatusText);
            failureEntry.sessionId = spec.sessionId;
            failureEntry.hasSessionId = spec.hasSessionId;
            resultRows.push_back(std::move(failureEntry));
            continue;
        }

        for (const DirectoryRecord& record : records)
        {
            KernelBaseNamedObjectEntry entry;
            entry.scopeText = spec.scopeText;
            entry.directoryPathText = spec.pathText;
            entry.objectNameText = record.nameText;
            entry.objectTypeText = record.typeText;
            entry.typeCategoryText = classifyObjectType(record.typeText);
            entry.fullPathText = joinObjectPath(spec.pathText, record.nameText);
            entry.statusText = directoryStatusText;
            entry.sessionId = spec.sessionId;
            entry.hasSessionId = spec.hasSessionId;
            entry.canEnumerate = entry.typeCategoryText == QStringLiteral("Directory");

            if (entry.typeCategoryText == QStringLiteral("Directory"))
            {
                entry.statusText = QStringLiteral("%1；Directory，可继续枚举").arg(directoryStatusText);
            }
            else if (entry.typeCategoryText == QStringLiteral("SymbolicLink"))
            {
                QString linkStatusText;
                querySymbolicLinkTarget(api, entry.fullPathText, entry.symbolicTargetText, linkStatusText);
                entry.statusText = QStringLiteral("%1；SymbolicLink=%2")
                    .arg(directoryStatusText, linkStatusText);
            }

            resultRows.push_back(std::move(entry));
        }
    }

    std::sort(resultRows.begin(), resultRows.end(), [](const KernelBaseNamedObjectEntry& left, const KernelBaseNamedObjectEntry& right) {
        if (left.scopeText != right.scopeText) return left.scopeText < right.scopeText;
        if (left.typeCategoryText != right.typeCategoryText) return left.typeCategoryText < right.typeCategoryText;
        return QString::compare(left.objectNameText, right.objectNameText, Qt::CaseInsensitive) < 0;
    });

    rowsOut = std::move(resultRows);
    return true;
}
