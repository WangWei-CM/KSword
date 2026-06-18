#include "KernelSymbolicLinkWorker.h"

// ============================================================
// KernelSymbolicLinkWorker.cpp
// 作用说明：
// 1) 在 R3 中枚举常见 Object Manager 目录下的 SymbolicLink；
// 2) 对每个 SymbolicLink 解析目标路径并尝试映射 DOS 路径候选；
// 3) 保留目录打开失败和链接解析失败行，方便排查权限/Session 差异。
// ============================================================

#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <limits>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winternl.h>

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
    constexpr ULONG kInitialDirectoryQueryBuffer = 16U * 1024U;
    constexpr ULONG kInitialSymbolicLinkChars = 1024U;
    constexpr std::size_t kMaxEntriesPerDirectory = 8192U;
    constexpr ACCESS_MASK kDirectoryAccess = DIRECTORY_QUERY | DIRECTORY_TRAVERSE;
    constexpr ACCESS_MASK kSymbolicLinkAccess = SYMBOLIC_LINK_QUERY;

    using NtOpenDirectoryObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtQueryDirectoryObjectFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
    using NtOpenSymbolicLinkObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtQuerySymbolicLinkObjectFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);

    struct NtSymbolicLinkApi
    {
        HMODULE ntdllModule = nullptr;
        NtOpenDirectoryObjectFn openDirectoryObject = nullptr;
        NtQueryDirectoryObjectFn queryDirectoryObject = nullptr;
        NtOpenSymbolicLinkObjectFn openSymbolicLinkObject = nullptr;
        NtQuerySymbolicLinkObjectFn querySymbolicLinkObject = nullptr;
    };

    struct ScopedHandle
    {
        HANDLE handle = nullptr;

        ~ScopedHandle()
        {
            if (handle != nullptr)
            {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        }

        ScopedHandle() = default;
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
    };

    struct DirectoryRecord
    {
        QString objectName;
        QString objectType;
    };

    struct KsObjectDirectoryInformation
    {
        UNICODE_STRING Name;
        UNICODE_STRING TypeName;
    };

    QString normalizeObjectPath(QString pathText)
    {
        pathText = pathText.trimmed();
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

    QString unicodeStringToQString(const UNICODE_STRING& text)
    {
        if (text.Buffer == nullptr || text.Length == 0)
        {
            return QString();
        }
        return QString::fromWCharArray(
            text.Buffer,
            text.Length / static_cast<USHORT>(sizeof(wchar_t)));
    }

    void initializeUnicodeString(UNICODE_STRING& textOut, const std::wstring& sourceText)
    {
        textOut.Buffer = const_cast<PWSTR>(sourceText.c_str());
        textOut.Length = static_cast<USHORT>(std::min<std::size_t>(
            sourceText.size() * sizeof(wchar_t),
            static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));
        textOut.MaximumLength = static_cast<USHORT>(textOut.Length + sizeof(wchar_t));
    }

    bool isNeedGrowStatus(const NTSTATUS status)
    {
        return status == kStatusInfoLengthMismatch
            || status == kStatusBufferOverflow
            || status == kStatusBufferTooSmall;
    }

    QString ntStatusToText(const HMODULE ntdllModule, const NTSTATUS status)
    {
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(status), 8, 16, QChar('0'))
            .toUpper();

        std::array<wchar_t, 256> messageBuffer{};
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            ntdllModule,
            static_cast<DWORD>(status),
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

    bool loadNtApi(NtSymbolicLinkApi& apiOut, QString& errorTextOut)
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
            errorTextOut = QStringLiteral("解析 NtOpen/NtQuery Directory 或 SymbolicLink API 失败。");
            return false;
        }
        return true;
    }

    NTSTATUS openDirectoryObject(
        const NtSymbolicLinkApi& api,
        const QString& directoryPathText,
        HANDLE& handleOut)
    {
        handleOut = nullptr;
        const std::wstring pathText = normalizeObjectPath(directoryPathText).toStdWString();
        UNICODE_STRING objectName{};
        initializeUnicodeString(objectName, pathText);

        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(&attributes, &objectName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        return api.openDirectoryObject(&handleOut, kDirectoryAccess, &attributes);
    }

    NTSTATUS openSymbolicLinkObject(
        const NtSymbolicLinkApi& api,
        const QString& symbolicLinkPathText,
        HANDLE& handleOut)
    {
        handleOut = nullptr;
        const std::wstring pathText = normalizeObjectPath(symbolicLinkPathText).toStdWString();
        UNICODE_STRING objectName{};
        initializeUnicodeString(objectName, pathText);

        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(&attributes, &objectName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        return api.openSymbolicLinkObject(&handleOut, kSymbolicLinkAccess, &attributes);
    }

    bool enumerateDirectory(
        const NtSymbolicLinkApi& api,
        const QString& directoryPathText,
        std::vector<DirectoryRecord>& recordsOut,
        NTSTATUS& statusOut,
        bool& truncatedOut)
    {
        recordsOut.clear();
        statusOut = kStatusUnsuccessful;
        truncatedOut = false;

        ScopedHandle directoryHandle;
        statusOut = openDirectoryObject(api, directoryPathText, directoryHandle.handle);
        if (!NT_SUCCESS(statusOut))
        {
            return false;
        }

        ULONG context = 0;
        BOOLEAN restartScan = TRUE;
        ULONG queryBufferSize = kInitialDirectoryQueryBuffer;
        for (std::size_t index = 0; index < kMaxEntriesPerDirectory; ++index)
        {
            std::vector<std::uint8_t> queryBuffer(queryBufferSize);
            ULONG returnLength = 0;
            NTSTATUS queryStatus = api.queryDirectoryObject(
                directoryHandle.handle,
                queryBuffer.data(),
                queryBufferSize,
                TRUE,
                restartScan,
                &context,
                &returnLength);
            restartScan = FALSE;

            if (queryStatus == kStatusNoMoreEntries)
            {
                statusOut = kStatusSuccess;
                return true;
            }
            if (isNeedGrowStatus(queryStatus))
            {
                queryBufferSize = std::max(queryBufferSize * 2U, returnLength + 512U);
                --index;
                continue;
            }
            if (!NT_SUCCESS(queryStatus))
            {
                statusOut = queryStatus;
                return false;
            }
            if (queryBuffer.size() < sizeof(KsObjectDirectoryInformation))
            {
                statusOut = kStatusBufferTooSmall;
                return false;
            }

            const auto* recordInfo = reinterpret_cast<const KsObjectDirectoryInformation*>(queryBuffer.data());
            DirectoryRecord record;
            record.objectName = unicodeStringToQString(recordInfo->Name).trimmed();
            record.objectType = unicodeStringToQString(recordInfo->TypeName).trimmed();
            if (!record.objectName.isEmpty())
            {
                recordsOut.push_back(std::move(record));
            }
        }

        statusOut = kStatusSuccess;
        truncatedOut = true;
        return true;
    }

    bool querySymbolicLinkTargetInternal(
        const NtSymbolicLinkApi& api,
        const QString& symbolicLinkPathText,
        QString& targetTextOut,
        QString& statusTextOut)
    {
        targetTextOut.clear();
        statusTextOut.clear();

        ScopedHandle symbolicLinkHandle;
        const NTSTATUS openStatus = openSymbolicLinkObject(api, symbolicLinkPathText, symbolicLinkHandle.handle);
        if (!NT_SUCCESS(openStatus))
        {
            statusTextOut = QStringLiteral("NtOpenSymbolicLinkObject: %1")
                .arg(ntStatusToText(api.ntdllModule, openStatus));
            return false;
        }

        ULONG targetChars = kInitialSymbolicLinkChars;
        for (int retry = 0; retry < 6; ++retry)
        {
            std::vector<wchar_t> targetBuffer(targetChars, L'\0');
            UNICODE_STRING targetText{};
            targetText.Buffer = targetBuffer.data();
            targetText.Length = 0;
            targetText.MaximumLength = static_cast<USHORT>(std::min<std::size_t>(
                targetChars * sizeof(wchar_t),
                static_cast<std::size_t>(std::numeric_limits<USHORT>::max() - sizeof(wchar_t))));

            ULONG returnLength = 0;
            const NTSTATUS queryStatus = api.querySymbolicLinkObject(
                symbolicLinkHandle.handle,
                &targetText,
                &returnLength);
            if (NT_SUCCESS(queryStatus))
            {
                targetTextOut = QString::fromWCharArray(
                    targetText.Buffer,
                    targetText.Length / static_cast<USHORT>(sizeof(wchar_t)));
                statusTextOut = QStringLiteral("OK: %1").arg(ntStatusToText(api.ntdllModule, queryStatus));
                return true;
            }
            if (!isNeedGrowStatus(queryStatus))
            {
                statusTextOut = QStringLiteral("NtQuerySymbolicLinkObject: %1")
                    .arg(ntStatusToText(api.ntdllModule, queryStatus));
                return false;
            }
            targetChars = std::max<ULONG>(
                targetChars * 2U,
                static_cast<ULONG>(returnLength / sizeof(wchar_t) + 8U));
        }

        statusTextOut = QStringLiteral("NtQuerySymbolicLinkObject: 缓冲区重试达到上限。");
        return false;
    }

    bool isNumericText(const QString& text)
    {
        if (text.trimmed().isEmpty())
        {
            return false;
        }
        for (const QChar ch : text)
        {
            if (!ch.isDigit())
            {
                return false;
            }
        }
        return true;
    }

    QStringList buildSourceDirectories(const NtSymbolicLinkApi& api)
    {
        QStringList directories{
            QStringLiteral("\\GLOBAL??"),
            QStringLiteral("\\??"),
            QStringLiteral("\\Device"),
            QStringLiteral("\\BaseNamedObjects")
        };

        std::vector<DirectoryRecord> sessionRecords;
        NTSTATUS sessionStatus = kStatusUnsuccessful;
        bool truncated = false;
        if (enumerateDirectory(api, QStringLiteral("\\Sessions"), sessionRecords, sessionStatus, truncated))
        {
            for (const DirectoryRecord& record : sessionRecords)
            {
                if (record.objectType.compare(QStringLiteral("Directory"), Qt::CaseInsensitive) == 0
                    && isNumericText(record.objectName))
                {
                    directories.push_back(QStringLiteral("\\Sessions\\%1\\DosDevices").arg(record.objectName));
                    directories.push_back(QStringLiteral("\\Sessions\\%1\\BaseNamedObjects").arg(record.objectName));
                }
            }
        }

        DWORD currentSessionId = 0;
        if (::ProcessIdToSessionId(::GetCurrentProcessId(), &currentSessionId) != FALSE)
        {
            directories.push_back(QStringLiteral("\\Sessions\\%1\\DosDevices").arg(currentSessionId));
            directories.push_back(QStringLiteral("\\Sessions\\%1\\BaseNamedObjects").arg(currentSessionId));
        }

        directories.removeDuplicates();
        return directories;
    }

    QString firstDosCandidate(const QString& ntPathText)
    {
        const std::vector<QString> candidates = queryKernelSymbolicLinkDosPathCandidates(ntPathText);
        if (candidates.empty())
        {
            return QString();
        }
        QStringList candidateTextList;
        for (const QString& candidateText : candidates)
        {
            candidateTextList.push_back(candidateText);
        }
        return candidateTextList.join(QStringLiteral(" | "));
    }

    void appendDirectoryFailureRow(
        const NtSymbolicLinkApi& api,
        const QString& directoryPathText,
        const NTSTATUS status,
        std::vector<KernelSymbolicLinkEntry>& rows)
    {
        KernelSymbolicLinkEntry entry;
        entry.sourceDirectory = normalizeObjectPath(directoryPathText);
        entry.linkName = QStringLiteral("<目录打开失败>");
        entry.fullPath = entry.sourceDirectory;
        entry.statusText = QStringLiteral("NtOpenDirectoryObject: %1")
            .arg(ntStatusToText(api.ntdllModule, status));
        rows.push_back(std::move(entry));
    }
}

bool runKernelSymbolicLinkSnapshotTask(
    std::vector<KernelSymbolicLinkEntry>& rowsOut,
    QString& errorTextOut)
{
    rowsOut.clear();
    errorTextOut.clear();

    NtSymbolicLinkApi api;
    if (!loadNtApi(api, errorTextOut))
    {
        return false;
    }

    const QStringList sourceDirectories = buildSourceDirectories(api);
    std::vector<KernelSymbolicLinkEntry> rows;
    rows.reserve(512);

    for (const QString& directoryPath : sourceDirectories)
    {
        std::vector<DirectoryRecord> records;
        NTSTATUS directoryStatus = kStatusUnsuccessful;
        bool truncated = false;
        if (!enumerateDirectory(api, directoryPath, records, directoryStatus, truncated))
        {
            appendDirectoryFailureRow(api, directoryPath, directoryStatus, rows);
            continue;
        }

        for (const DirectoryRecord& record : records)
        {
            if (record.objectType.compare(QStringLiteral("SymbolicLink"), Qt::CaseInsensitive) != 0)
            {
                continue;
            }

            KernelSymbolicLinkEntry entry;
            entry.sourceDirectory = normalizeObjectPath(directoryPath);
            entry.linkName = record.objectName;
            entry.fullPath = joinObjectPath(directoryPath, record.objectName);

            QString targetText;
            QString statusText;
            if (querySymbolicLinkTargetInternal(api, entry.fullPath, targetText, statusText))
            {
                entry.targetPath = targetText;
                entry.dosCandidate = firstDosCandidate(targetText);
                entry.statusText = statusText;
            }
            else
            {
                entry.statusText = statusText;
            }
            rows.push_back(std::move(entry));
        }

        if (truncated)
        {
            KernelSymbolicLinkEntry entry;
            entry.sourceDirectory = normalizeObjectPath(directoryPath);
            entry.linkName = QStringLiteral("<目录枚举截断>");
            entry.fullPath = entry.sourceDirectory;
            entry.statusText = QStringLiteral("目录项超过本页 R3 安全上限 %1。").arg(kMaxEntriesPerDirectory);
            rows.push_back(std::move(entry));
        }
    }

    std::sort(rows.begin(), rows.end(), [](const KernelSymbolicLinkEntry& left, const KernelSymbolicLinkEntry& right) {
        const int directoryCompare = QString::compare(left.sourceDirectory, right.sourceDirectory, Qt::CaseInsensitive);
        if (directoryCompare != 0)
        {
            return directoryCompare < 0;
        }
        return QString::compare(left.linkName, right.linkName, Qt::CaseInsensitive) < 0;
    });

    rowsOut = std::move(rows);
    return true;
}

bool queryKernelSymbolicLinkTarget(
    const QString& symbolicLinkPathText,
    QString& targetTextOut,
    QString& statusTextOut)
{
    targetTextOut.clear();
    statusTextOut.clear();

    NtSymbolicLinkApi api;
    QString errorText;
    if (!loadNtApi(api, errorText))
    {
        statusTextOut = errorText;
        return false;
    }
    return querySymbolicLinkTargetInternal(api, symbolicLinkPathText, targetTextOut, statusTextOut);
}

std::vector<QString> queryKernelSymbolicLinkDosPathCandidates(const QString& ntPathText)
{
    std::vector<QString> result;
    const QString normalizedNtPath = normalizeObjectPath(ntPathText);
    if (!normalizedNtPath.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
    {
        return result;
    }

    QStringList uniqueCandidates;
    for (wchar_t driveLetter = L'A'; driveLetter <= L'Z'; ++driveLetter)
    {
        wchar_t driveName[3]{ driveLetter, L':', L'\0' };
        wchar_t mappingBuffer[4096]{};
        const DWORD mappingLength = ::QueryDosDeviceW(
            driveName,
            mappingBuffer,
            static_cast<DWORD>(std::size(mappingBuffer)));
        if (mappingLength == 0)
        {
            continue;
        }

        const wchar_t* cursor = mappingBuffer;
        while (*cursor != L'\0')
        {
            const QString mappingText = QString::fromWCharArray(cursor).trimmed();
            if (!mappingText.isEmpty()
                && normalizedNtPath.startsWith(mappingText, Qt::CaseInsensitive))
            {
                const QString suffixText = normalizedNtPath.mid(mappingText.size());
                QString candidateText = QString::fromWCharArray(driveName);
                candidateText += suffixText.isEmpty() ? QStringLiteral("\\") : suffixText;
                if (!uniqueCandidates.contains(candidateText, Qt::CaseInsensitive))
                {
                    uniqueCandidates.push_back(candidateText);
                }
            }
            cursor += (wcslen(cursor) + 1);
        }
    }

    for (const QString& candidate : uniqueCandidates)
    {
        result.push_back(candidate);
    }
    return result;
}
