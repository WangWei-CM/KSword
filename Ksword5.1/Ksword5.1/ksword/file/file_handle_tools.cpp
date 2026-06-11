#include "file_handle_tools.h"

#include "../process/process.h"
#include "../string/string.h"
#include "../../ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <TlHelp32.h>
#include <sddl.h>

namespace ks::file
{
    namespace
    {
        // NtQuerySystemInformation/NtQueryObject class values are kept local so this file
        // remains stable across SDK revisions that expose different enum names.
        constexpr ULONG kSystemExtendedHandleInformationClass = 64;
        constexpr ULONG kObjectBasicInformationClass = 0;
        constexpr ULONG kObjectNameInformationClass = 1;
        constexpr ULONG kObjectTypeInformationClass = 2;
        constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004);
        constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005);
        constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023);

        // Native mirror for SystemExtendedHandleInformation rows. Only fields used by
        // FileDock and HandleDock are modeled, and every access is bounded by buffer size.
        struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE
        {
            PVOID objectAddress = nullptr;
            ULONG_PTR uniqueProcessId = 0;
            ULONG_PTR handleValue = 0;
            ULONG grantedAccess = 0;
            USHORT creatorBackTraceIndex = 0;
            USHORT objectTypeIndex = 0;
            ULONG handleAttributes = 0;
            ULONG reserved = 0;
        };

        // Variable-size header returned by SystemExtendedHandleInformation. The handles
        // member is a flexible tail and must never be trusted without external size checks.
        struct SYSTEM_HANDLE_INFORMATION_EX_NATIVE
        {
            ULONG_PTR numberOfHandles = 0;
            ULONG_PTR reserved = 0;
            SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE handles[1] = {};
        };

        // Local layout for ObjectBasicInformation. The public UI only needs handle and
        // pointer counts, but the full prefix is required for NtQueryObject to fill it.
        struct OBJECT_BASIC_INFORMATION_NATIVE
        {
            ULONG attributes = 0;
            ACCESS_MASK grantedAccess = 0;
            ULONG handleCount = 0;
            ULONG pointerCount = 0;
            ULONG pagedPoolUsage = 0;
            ULONG nonPagedPoolUsage = 0;
            ULONG reserved[3] = {};
            ULONG nameInfoSize = 0;
            ULONG typeInfoSize = 0;
            ULONG securityDescriptorSize = 0;
            LARGE_INTEGER creationTime{};
        };

        // UniqueHandle gives backend code exception-safe HANDLE ownership. It accepts both
        // nullptr and INVALID_HANDLE_VALUE as invalid values and closes only valid handles.
        class UniqueHandle final
        {
        public:
            explicit UniqueHandle(HANDLE handleValue = nullptr) : m_handle(handleValue) {}
            ~UniqueHandle() { reset(nullptr); }
            UniqueHandle(const UniqueHandle&) = delete;
            UniqueHandle& operator=(const UniqueHandle&) = delete;
            UniqueHandle(UniqueHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
            UniqueHandle& operator=(UniqueHandle&& other) noexcept
            {
                if (this != &other)
                {
                    reset(nullptr);
                    m_handle = other.m_handle;
                    other.m_handle = nullptr;
                }
                return *this;
            }
            HANDLE get() const { return m_handle; }
            bool valid() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
            void reset(HANDLE newHandle)
            {
                if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(m_handle);
                }
                m_handle = newHandle;
            }
        private:
            HANDLE m_handle = nullptr;
        };

        // CachedObjectSnapshot stores object-level query results so multiple handles to the
        // same kernel object do not repeatedly duplicate/query the same object.
        struct CachedObjectSnapshot
        {
            bool basicInfoAvailable = false;
            std::uint32_t handleCount = 0;
            std::uint32_t pointerCount = 0;
            bool objectNameAvailable = false;
            bool objectNameFromFallback = false;
            std::wstring objectName;
        };

        // HandleIdentityKey is the stable identity used for R3/R0 diff merging.
        struct HandleIdentityKey
        {
            std::uint32_t processId = 0;
            std::uint64_t handleValue = 0;
            bool operator==(const HandleIdentityKey& other) const noexcept
            {
                return processId == other.processId && handleValue == other.handleValue;
            }
        };

        struct HandleIdentityKeyHash
        {
            std::size_t operator()(const HandleIdentityKey& key) const noexcept
            {
                const std::size_t pidHash = std::hash<std::uint32_t>{}(key.processId);
                const std::size_t handleHash = std::hash<std::uint64_t>{}(key.handleValue);
                return pidHash ^ (handleHash + 0x9e3779b97f4a7c15ULL + (pidHash << 6) + (pidHash >> 2));
            }
        };

        // TrimWideCopy removes leading/trailing whitespace while preserving path body bytes.
        std::wstring TrimWideCopy(const std::wstring& text)
        {
            std::size_t beginIndex = 0;
            while (beginIndex < text.size() && std::iswspace(text[beginIndex]) != 0) { ++beginIndex; }
            std::size_t endIndex = text.size();
            while (endIndex > beginIndex && std::iswspace(text[endIndex - 1]) != 0) { --endIndex; }
            return text.substr(beginIndex, endIndex - beginIndex);
        }

        // ToLowerWideCopy implements case-insensitive comparison keys for Windows paths
        // and object type names without bringing in Qt string helpers.
        std::wstring ToLowerWideCopy(std::wstring text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return text;
        }

        bool EqualsInsensitive(const std::wstring& left, const std::wstring& right)
        {
            return ToLowerWideCopy(left) == ToLowerWideCopy(right);
        }

        bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle)
        {
            return needle.empty() || ToLowerWideCopy(haystack).find(ToLowerWideCopy(needle)) != std::wstring::npos;
        }

        bool StartsWithInsensitive(const std::wstring& text, const std::wstring& prefix)
        {
            return prefix.size() <= text.size() && EqualsInsensitive(text.substr(0, prefix.size()), prefix);
        }

        bool EndsWithSlash(const std::wstring& text)
        {
            return !text.empty() && (text.back() == L'\\' || text.back() == L'/');
        }

        // MakeAbsolutePath normalizes relative DOS paths while preserving NT device paths.
        std::wstring MakeAbsolutePath(const std::wstring& rawPath)
        {
            const std::wstring normalizedInput = NormalizeNativePath(rawPath);
            if (normalizedInput.empty() || StartsWithInsensitive(normalizedInput, L"\\Device\\"))
            {
                return normalizedInput;
            }
            const DWORD requiredChars = ::GetFullPathNameW(normalizedInput.c_str(), 0, nullptr, nullptr);
            if (requiredChars == 0)
            {
                return normalizedInput;
            }
            std::vector<wchar_t> buffer(static_cast<std::size_t>(requiredChars) + 1U, L'\0');
            const DWORD writtenChars = ::GetFullPathNameW(normalizedInput.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            return (writtenChars > 0 && writtenChars < buffer.size())
                ? NormalizeNativePath(std::wstring(buffer.data(), writtenChars))
                : normalizedInput;
        }

        bool IsDirectoryPath(const std::wstring& pathText)
        {
            const DWORD attributes = ::GetFileAttributesW(pathText.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }
            return EndsWithSlash(pathText);
        }

        std::wstring JoinDiagnostic(const std::vector<std::wstring>& itemList)
        {
            std::wstring output;
            for (const std::wstring& item : itemList)
            {
                if (TrimWideCopy(item).empty()) { continue; }
                if (!output.empty()) { output += L" | "; }
                output += item;
            }
            return output;
        }

        std::wstring FormatNtStatusHex(const NTSTATUS statusValue)
        {
            std::wostringstream stream;
            stream << L"0x" << std::uppercase << std::hex << static_cast<std::uint32_t>(statusValue);
            return stream.str();
        }

        std::wstring BuildPathRuleText(const std::wstring& sourceText, const bool directoryRule)
        {
            return directoryRule ? sourceText + L"-目录前缀" : sourceText + L"-精确路径";
        }

        std::uint64_t BuildHandleKey(const std::uint32_t processId, const std::uint64_t handleValue)
        {
            return (static_cast<std::uint64_t>(processId) << 32) ^ handleValue;
        }
        std::wstring ProcessNameFallback(const std::uint32_t processId)
        {
            return L"PID_" + std::to_wstring(processId);
        }

        // CollectProcessNameMap builds a reusable PID -> process name cache through Toolhelp.
        std::unordered_map<std::uint32_t, std::wstring> CollectProcessNameMap()
        {
            std::unordered_map<std::uint32_t, std::wstring> processNameMap;
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
                processNameMap[processEntry.th32ProcessID] = processEntry.szExeFile;
                hasItem = ::Process32NextW(snapshotHandle.get(), &processEntry);
            }
            return processNameMap;
        }

        std::wstring ProcessNameOf(
            const std::unordered_map<std::uint32_t, std::wstring>& processNameMap,
            const std::uint32_t processId)
        {
            const auto foundIt = processNameMap.find(processId);
            return (foundIt != processNameMap.end() && !foundIt->second.empty())
                ? foundIt->second
                : ProcessNameFallback(processId);
        }

        // QueryProcessImagePathCached reuses ks::process for process paths while keeping this
        // module free of any UI-framework string or widget dependencies.
        std::wstring QueryProcessImagePathCached(
            const std::uint32_t processId,
            std::unordered_map<std::uint32_t, std::wstring>& cacheMap)
        {
            const auto foundIt = cacheMap.find(processId);
            if (foundIt != cacheMap.end())
            {
                return foundIt->second;
            }
            const std::wstring imagePath = ks::str::Utf8ToUtf16(ks::process::QueryProcessPathByPid(processId));
            cacheMap.insert_or_assign(processId, imagePath);
            return imagePath;
        }

        // OpenProcessHandleForDuplicate caches remote process handles for one snapshot pass.
        HANDLE OpenProcessHandleForDuplicate(
            const std::uint32_t processId,
            std::unordered_map<std::uint32_t, UniqueHandle>& processHandleCache,
            std::unordered_set<std::uint32_t>& failedProcessOpenSet)
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

            UniqueHandle processHandle(::OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
            if (!processHandle.valid())
            {
                processHandle.reset(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, processId));
            }
            if (!processHandle.valid())
            {
                failedProcessOpenSet.insert(processId);
                return nullptr;
            }

            HANDLE rawHandle = processHandle.get();
            processHandleCache.emplace(processId, std::move(processHandle));
            return rawHandle;
        }

        // OpenTargetPathHandle opens the first scan target only to discover the runtime File TypeIndex.
        bool OpenTargetPathHandle(const TargetPathPattern& pattern, UniqueHandle& handleOut)
        {
            handleOut.reset(nullptr);
            const DWORD flags = pattern.directoryMode ? FILE_FLAG_BACKUP_SEMANTICS : 0;
            HANDLE rawHandle = ::CreateFileW(
                pattern.displayPath.c_str(),
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

        bool ResolveFileTypeIndex(
            const HANDLE localTargetHandle,
            const std::vector<RawSystemHandle>& rawRecords,
            std::uint16_t& fileTypeIndexOut)
        {
            fileTypeIndexOut = 0;
            if (localTargetHandle == nullptr || localTargetHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            const DWORD currentProcessId = ::GetCurrentProcessId();
            const ULONG_PTR localHandleValue = reinterpret_cast<ULONG_PTR>(localTargetHandle);
            for (const RawSystemHandle& row : rawRecords)
            {
                if (row.processId == currentProcessId && row.handleValue == static_cast<std::uint64_t>(localHandleValue))
                {
                    fileTypeIndexOut = row.typeIndex;
                    return fileTypeIndexOut != 0;
                }
            }
            return false;
        }

        // ShouldAttemptNameQuery mirrors the existing HandleDock object-name budget policy.
        bool ShouldAttemptNameQuery(const std::wstring& typeNameText)
        {
            const std::wstring normalizedType = ToLowerWideCopy(TrimWideCopy(typeNameText));
            if (normalizedType.empty())
            {
                return false;
            }

            static const std::array<const wchar_t*, 28> kAllowTypeKeyword{
                L"file", L"directory", L"symboliclink", L"key", L"event", L"semaphore", L"mutant",
                L"timer", L"section", L"desktop", L"windowstation", L"port", L"alpc", L"job",
                L"token", L"process", L"thread", L"device", L"driver", L"wmi", L"iocompletion",
                L"filterconnectionport", L"waitcompletionpacket", L"session", L"keyedevent", L"eventpair",
                L"iocompletionreserve", L"partition"
            };
            for (const wchar_t* keywordText : kAllowTypeKeyword)
            {
                if (normalizedType.find(keywordText) != std::wstring::npos)
                {
                    return true;
                }
            }
            return false;
        }

        std::wstring ResolveTypeNameFromCache(
            const std::uint16_t typeIndex,
            const std::unordered_map<std::uint16_t, std::string>& typeNameCache)
        {
            const auto typeIt = typeNameCache.find(typeIndex);
            if (typeIt != typeNameCache.end() && !typeIt->second.empty())
            {
                return ks::str::Utf8ToUtf16(typeIt->second);
            }
            return L"Type#" + std::to_wstring(typeIndex);
        }

        bool QueryFileObjectDisplayName(HANDLE objectHandle, std::wstring& textOut)
        {
            return QueryFinalDosPathByHandle(objectHandle, textOut);
        }

        bool QueryProcessObjectDisplayName(HANDLE objectHandle, std::wstring& textOut)
        {
            textOut.clear();
            const DWORD targetProcessId = ::GetProcessId(objectHandle);
            if (targetProcessId == 0)
            {
                return false;
            }

            DWORD bufferChars = 2048;
            std::vector<wchar_t> pathBuffer(static_cast<std::size_t>(bufferChars), L'\0');
            if (::QueryFullProcessImageNameW(objectHandle, 0, pathBuffer.data(), &bufferChars) != FALSE && bufferChars > 0)
            {
                textOut = L"PID " + std::to_wstring(targetProcessId) + L" | " +
                    TrimWideCopy(std::wstring(pathBuffer.data(), bufferChars));
                return true;
            }
            textOut = L"PID " + std::to_wstring(targetProcessId);
            return true;
        }

        bool QueryThreadObjectDisplayName(HANDLE objectHandle, std::wstring& textOut)
        {
            textOut.clear();
            const DWORD targetThreadId = ::GetThreadId(objectHandle);
            if (targetThreadId == 0)
            {
                return false;
            }
            const DWORD ownerProcessId = ::GetProcessIdOfThread(objectHandle);
            textOut = (ownerProcessId != 0)
                ? L"PID " + std::to_wstring(ownerProcessId) + L" / TID " + std::to_wstring(targetThreadId)
                : L"TID " + std::to_wstring(targetThreadId);
            return true;
        }

        bool QueryTokenObjectDisplayName(HANDLE objectHandle, std::wstring& textOut)
        {
            textOut.clear();
            DWORD requiredLength = 0;
            ::GetTokenInformation(objectHandle, TokenUser, nullptr, 0, &requiredLength);
            if (requiredLength == 0)
            {
                return false;
            }

            std::vector<std::uint8_t> tokenBuffer(static_cast<std::size_t>(requiredLength), 0);
            if (::GetTokenInformation(objectHandle, TokenUser, tokenBuffer.data(), requiredLength, &requiredLength) == FALSE)
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
            if (::LookupAccountSidW(nullptr, tokenUser->User.Sid, accountName, &accountNameLength, domainName, &domainNameLength, &sidType) != FALSE)
            {
                textOut = std::wstring(domainName) + L"\\" + accountName;
                return true;
            }

            LPWSTR sidText = nullptr;
            if (::ConvertSidToStringSidW(tokenUser->User.Sid, &sidText) != FALSE && sidText != nullptr)
            {
                textOut = sidText;
                ::LocalFree(sidText);
                return true;
            }
            return false;
        }

        bool QueryTypeSpecificObjectName(const std::wstring& typeNameText, HANDLE objectHandle, std::wstring& textOut)
        {
            const std::wstring normalizedType = ToLowerWideCopy(TrimWideCopy(typeNameText));
            if (normalizedType == L"file" || normalizedType == L"directory")
            {
                return QueryFileObjectDisplayName(objectHandle, textOut);
            }
            if (normalizedType == L"process")
            {
                return QueryProcessObjectDisplayName(objectHandle, textOut);
            }
            if (normalizedType == L"thread")
            {
                return QueryThreadObjectDisplayName(objectHandle, textOut);
            }
            if (normalizedType == L"token")
            {
                return QueryTokenObjectDisplayName(objectHandle, textOut);
            }
            textOut.clear();
            return false;
        }
    }

    bool NtApiSet::ready() const
    {
        return ntdllModule != nullptr && querySystemInformation != nullptr && queryObject != nullptr;
    }

    std::wstring NormalizeNativePath(const std::wstring& pathText)
    {
        std::wstring pathValue = TrimWideCopy(pathText);
        std::replace(pathValue.begin(), pathValue.end(), L'/', L'\\');
        if (StartsWithInsensitive(pathValue, L"\\\\?\\UNC\\"))
        {
            pathValue = L"\\\\" + pathValue.substr(8);
        }
        else if (StartsWithInsensitive(pathValue, L"\\\\?\\"))
        {
            pathValue = pathValue.substr(4);
        }
        return TrimWideCopy(pathValue);
    }

    std::wstring NormalizePathForCompare(const std::wstring& pathText)
    {
        std::wstring pathValue = NormalizeNativePath(pathText);
        while (pathValue.size() > 3 && pathValue.back() == L'\\')
        {
            pathValue.pop_back();
        }
        return ToLowerWideCopy(pathValue);
    }

    bool BuildNtPathEquivalent(const std::wstring& absolutePath, std::wstring& ntPathOut)
    {
        ntPathOut.clear();
        const std::wstring pathValue = NormalizeNativePath(absolutePath);
        if (pathValue.size() >= 2 && pathValue[1] == L':')
        {
            const std::wstring driveText = pathValue.substr(0, 2);
            wchar_t deviceBuffer[4096] = {};
            const DWORD queryChars = ::QueryDosDeviceW(driveText.c_str(), deviceBuffer, static_cast<DWORD>(std::size(deviceBuffer)));
            if (queryChars == 0 || deviceBuffer[0] == L'\0')
            {
                return false;
            }
            ntPathOut = TrimWideCopy(std::wstring(deviceBuffer)) + pathValue.substr(2);
            return !TrimWideCopy(ntPathOut).empty();
        }
        if (pathValue.rfind(L"\\\\", 0) == 0)
        {
            ntPathOut = L"\\Device\\Mup" + pathValue.substr(1);
            return !TrimWideCopy(ntPathOut).empty();
        }
        return false;
    }

    std::vector<TargetPathPattern> BuildTargetPathPatterns(const std::vector<std::wstring>& absolutePaths)
    {
        std::vector<TargetPathPattern> patternList;
        patternList.reserve(absolutePaths.size() * 2U);
        std::set<std::wstring> normalizedSet;
        for (const std::wstring& rawPath : absolutePaths)
        {
            if (TrimWideCopy(rawPath).empty())
            {
                continue;
            }

            const std::wstring absolutePath = MakeAbsolutePath(rawPath);
            const std::wstring normalizedPath = NormalizePathForCompare(absolutePath);
            if (normalizedPath.empty() || normalizedSet.find(normalizedPath) != normalizedSet.end())
            {
                continue;
            }
            normalizedSet.insert(normalizedPath);

            const bool directoryMode = IsDirectoryPath(absolutePath);
            TargetPathPattern pattern{};
            pattern.displayPath = absolutePath;
            pattern.normalizedPath = normalizedPath;
            pattern.directoryMode = directoryMode;
            patternList.push_back(std::move(pattern));

            std::wstring ntPathText;
            if (BuildNtPathEquivalent(absolutePath, ntPathText))
            {
                const std::wstring normalizedNtPath = NormalizePathForCompare(ntPathText);
                if (!normalizedNtPath.empty() && normalizedSet.find(normalizedNtPath) == normalizedSet.end())
                {
                    normalizedSet.insert(normalizedNtPath);
                    TargetPathPattern ntPattern{};
                    ntPattern.displayPath = absolutePath;
                    ntPattern.normalizedPath = normalizedNtPath;
                    ntPattern.directoryMode = directoryMode;
                    patternList.push_back(std::move(ntPattern));
                }
            }
        }
        return patternList;
    }

    bool MatchTargetPath(
        const std::wstring& normalizedCandidatePath,
        const std::vector<TargetPathPattern>& patternList,
        std::wstring& matchedTargetPathOut,
        bool& matchedByDirectoryRuleOut)
    {
        matchedTargetPathOut.clear();
        matchedByDirectoryRuleOut = false;
        if (TrimWideCopy(normalizedCandidatePath).empty())
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
            if (normalizedCandidatePath == pattern.normalizedPath || normalizedCandidatePath.rfind(pattern.normalizedPath + L"\\", 0) == 0)
            {
                matchedTargetPathOut = pattern.displayPath;
                matchedByDirectoryRuleOut = true;
                return true;
            }
        }
        return false;
    }

    NtApiSet QueryNtApis()
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
        apiSet.querySystemInformation = reinterpret_cast<NtApiSet::NtQuerySystemInformationFn>(
            ::GetProcAddress(apiSet.ntdllModule, "NtQuerySystemInformation"));
        apiSet.queryObject = reinterpret_cast<NtApiSet::NtQueryObjectFn>(
            ::GetProcAddress(apiSet.ntdllModule, "NtQueryObject"));
        return apiSet;
    }

    bool QuerySystemHandles(
        const NtApiSet& apiSet,
        std::vector<RawSystemHandle>& recordsOut,
        std::wstring& diagnosticTextOut)
    {
        recordsOut.clear();
        diagnosticTextOut.clear();
        if (!apiSet.ready())
        {
            diagnosticTextOut = L"Nt API 不可用，无法枚举系统句柄。";
            return false;
        }

        ULONG bufferSize = 1024U * 1024U;
        for (int attemptIndex = 0; attemptIndex < 10; ++attemptIndex)
        {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(bufferSize), 0);
            ULONG returnLength = 0;
            const NTSTATUS status = apiSet.querySystemInformation(kSystemExtendedHandleInformationClass, buffer.data(), bufferSize, &returnLength);
            const bool needGrow = status == kStatusInfoLengthMismatch || status == kStatusBufferOverflow || status == kStatusBufferTooSmall;
            if (needGrow)
            {
                const ULONG recommendedSize = (returnLength > bufferSize) ? returnLength + (256U * 1024U) : bufferSize * 2U;
                bufferSize = std::max<ULONG>(recommendedSize, bufferSize + (256U * 1024U));
                continue;
            }
            if (status < 0)
            {
                diagnosticTextOut = L"NtQuerySystemInformation 失败，status=" + FormatNtStatusHex(status);
                return false;
            }
            if (buffer.size() < sizeof(SYSTEM_HANDLE_INFORMATION_EX_NATIVE))
            {
                diagnosticTextOut = L"句柄快照缓冲区尺寸异常（过小）。";
                return false;
            }

            const auto* handleHeader = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX_NATIVE*>(buffer.data());
            const std::size_t declaredCount = static_cast<std::size_t>(handleHeader->numberOfHandles);
            const std::size_t headerBytes = offsetof(SYSTEM_HANDLE_INFORMATION_EX_NATIVE, handles);
            const std::size_t availableBytes = buffer.size() > headerBytes ? buffer.size() - headerBytes : 0U;
            const std::size_t safeRecordCount = std::min(declaredCount, availableBytes / sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE));
            recordsOut.reserve(safeRecordCount);
            for (std::size_t index = 0; index < safeRecordCount; ++index)
            {
                const SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_NATIVE& source = handleHeader->handles[index];
                RawSystemHandle row{};
                row.processId = static_cast<std::uint32_t>(source.uniqueProcessId);
                row.handleValue = static_cast<std::uint64_t>(source.handleValue);
                row.typeIndex = static_cast<std::uint16_t>(source.objectTypeIndex);
                row.objectAddress = reinterpret_cast<std::uint64_t>(source.objectAddress);
                row.grantedAccess = static_cast<std::uint32_t>(source.grantedAccess);
                row.attributes = static_cast<std::uint32_t>(source.handleAttributes);
                recordsOut.push_back(row);
            }
            if (safeRecordCount < declaredCount)
            {
                diagnosticTextOut = L"句柄记录超出缓冲区，结果已截断。";
            }
            return true;
        }
        diagnosticTextOut = L"句柄快照缓冲区扩容次数已达上限。";
        return false;
    }

    bool QueryNtObjectText(const NtApiSet& apiSet, HANDLE objectHandle, const ULONG informationClass, std::wstring& textOut)
    {
        textOut.clear();
        if (!apiSet.ready() || objectHandle == nullptr || objectHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        ULONG bufferSize = 1024;
        for (int attemptIndex = 0; attemptIndex < 8; ++attemptIndex)
        {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(bufferSize), 0);
            ULONG returnLength = 0;
            const NTSTATUS status = apiSet.queryObject(objectHandle, informationClass, buffer.data(), bufferSize, &returnLength);
            const bool needGrow = status == kStatusInfoLengthMismatch || status == kStatusBufferOverflow || status == kStatusBufferTooSmall;
            if (needGrow)
            {
                const ULONG recommendedSize = (returnLength > bufferSize) ? returnLength + 256U : bufferSize * 2U;
                bufferSize = std::max<ULONG>(recommendedSize, bufferSize + 256U);
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
            textOut.assign(unicodeValue->Buffer, unicodeValue->Length / sizeof(wchar_t));
            return true;
        }
        return false;
    }
    bool QueryObjectBasicInfo(const NtApiSet& apiSet, HANDLE objectHandle, ObjectBasicInfo& basicInfoOut)
    {
        basicInfoOut = ObjectBasicInfo{};
        if (!apiSet.ready() || objectHandle == nullptr || objectHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        OBJECT_BASIC_INFORMATION_NATIVE nativeInfo{};
        const NTSTATUS status = apiSet.queryObject(
            objectHandle,
            kObjectBasicInformationClass,
            &nativeInfo,
            static_cast<ULONG>(sizeof(nativeInfo)),
            nullptr);
        if (status < 0)
        {
            return false;
        }
        basicInfoOut.handleCount = nativeInfo.handleCount;
        basicInfoOut.pointerCount = nativeInfo.pointerCount;
        return true;
    }

    ObjectNameQueryResult ResolveObjectNameText(
        const NtApiSet& apiSet,
        HANDLE objectHandle,
        const std::wstring& typeNameText)
    {
        ObjectNameQueryResult result{};
        std::wstring ntObjectNameText;
        const bool ntQueryOk = QueryNtObjectText(apiSet, objectHandle, kObjectNameInformationClass, ntObjectNameText);
        if (ntQueryOk)
        {
            result.available = true;
            result.objectName = TrimWideCopy(ntObjectNameText);
            if (!result.objectName.empty())
            {
                return result;
            }
        }

        std::wstring fallbackText;
        if (QueryTypeSpecificObjectName(typeNameText, objectHandle, fallbackText))
        {
            result.available = true;
            result.usedFallback = !TrimWideCopy(fallbackText).empty();
            result.objectName = TrimWideCopy(fallbackText);
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

    bool DuplicateRemoteHandleToLocal(HANDLE sourceProcessHandle, const std::uint64_t handleValue, HANDLE& localHandleOut)
    {
        localHandleOut = nullptr;
        if (sourceProcessHandle == nullptr || sourceProcessHandle == INVALID_HANDLE_VALUE)
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
        localHandleOut = duplicatedHandle;
        return true;
    }

    bool QueryFinalDosPathByHandle(HANDLE objectHandle, std::wstring& pathOut)
    {
        pathOut.clear();
        if (objectHandle == nullptr || objectHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

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
            pathOut = NormalizeNativePath(std::wstring(pathBuffer.data(), pathLength));
            return !pathOut.empty();
        }
        return false;
    }

    bool CloseRemoteHandle(const std::uint32_t processId, const std::uint64_t handleValue, std::string& detailTextOut)
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

    HandleSnapshotResult BuildHandleSnapshot(const HandleSnapshotOptions& options)
    {
        HandleSnapshotResult result{};
        const auto beginTime = std::chrono::steady_clock::now();
        if (options.enumMode == HandleEnumMode::KernelHandleTable && !options.hasPidFilter)
        {
            result.diagnosticText = L"Kernel HandleTable 模式需要先输入目标 PID。";
            result.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
            return result;
        }

        const NtApiSet apiSet = QueryNtApis();
        std::vector<RawSystemHandle> rawRecords;
        std::wstring queryDiagnosticText;
        if (!QuerySystemHandles(apiSet, rawRecords, queryDiagnosticText))
        {
            result.diagnosticText = queryDiagnosticText;
            result.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
            return result;
        }
        result.totalHandleCount = rawRecords.size();

        const std::unordered_map<std::uint32_t, std::wstring> processNameMap = CollectProcessNameMap();
        std::unordered_map<std::uint16_t, std::string> typeNameCache = options.typeNameCacheByIndex;
        for (const auto& pairItem : options.typeNameMapFromObjectTab)
        {
            typeNameCache[pairItem.first] = pairItem.second;
        }

        std::unordered_map<std::uint32_t, UniqueHandle> processHandleCache;
        std::unordered_set<std::uint32_t> failedProcessOpenSet;
        std::size_t typeQueryFailedCount = 0;
        std::unordered_map<std::uint16_t, const RawSystemHandle*> typeRepresentativeMap;
        for (const RawSystemHandle& rawRow : rawRecords)
        {
            if (rawRow.processId == 0 || typeNameCache.find(rawRow.typeIndex) != typeNameCache.end())
            {
                continue;
            }
            if (typeRepresentativeMap.find(rawRow.typeIndex) == typeRepresentativeMap.end())
            {
                typeRepresentativeMap[rawRow.typeIndex] = &rawRow;
            }
        }

        for (const auto& pairItem : typeRepresentativeMap)
        {
            const RawSystemHandle* representativeRow = pairItem.second;
            HANDLE sourceProcessHandle = representativeRow == nullptr ? nullptr : OpenProcessHandleForDuplicate(
                representativeRow->processId,
                processHandleCache,
                failedProcessOpenSet);
            if (sourceProcessHandle == nullptr)
            {
                ++typeQueryFailedCount;
                continue;
            }

            HANDLE localRawHandle = nullptr;
            if (!DuplicateRemoteHandleToLocal(sourceProcessHandle, representativeRow->handleValue, localRawHandle))
            {
                ++typeQueryFailedCount;
                continue;
            }
            UniqueHandle localHandle(localRawHandle);
            std::wstring typeNameText;
            if (!QueryNtObjectText(apiSet, localHandle.get(), kObjectTypeInformationClass, typeNameText))
            {
                ++typeQueryFailedCount;
                continue;
            }
            typeNameText = TrimWideCopy(typeNameText);
            if (!typeNameText.empty())
            {
                typeNameCache[pairItem.first] = ks::str::Utf16ToUtf8(typeNameText);
            }
        }

        int nameBudgetRemain = std::max(options.nameResolveBudget, 0);
        std::size_t duplicateFailedCount = 0;
        std::size_t basicInfoFailedCount = 0;
        std::size_t nameQueryFailedCount = 0;
        std::size_t nameBudgetSkippedCount = 0;
        std::unordered_map<std::uint64_t, CachedObjectSnapshot> objectSnapshotCacheByAddress;
        std::set<std::wstring> typeNameSet;
        result.rows.reserve(rawRecords.size());
        const std::wstring typeFilterText = TrimWideCopy(options.typeFilterText);
        const bool hasTypeFilter = !typeFilterText.empty() && typeFilterText != L"全部类型";

        for (const RawSystemHandle& rawRow : rawRecords)
        {
            if (rawRow.processId == 0)
            {
                continue;
            }

            HandleSnapshotRow row{};
            row.processId = rawRow.processId;
            row.processName = ProcessNameOf(processNameMap, rawRow.processId);
            row.handleValue = rawRow.handleValue;
            row.typeIndex = rawRow.typeIndex;
            row.objectAddress = rawRow.objectAddress;
            row.grantedAccess = rawRow.grantedAccess;
            row.attributes = rawRow.attributes;
            row.sourceMode = options.enumMode == HandleEnumMode::UserSnapshot ? HandleEnumMode::UserSnapshot : HandleEnumMode::DuplicateHandle;
            row.decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_OK;
            if (options.enumMode == HandleEnumMode::KernelHandleTable)
            {
                row.diffStatus = HandleDiffStatus::UserOnly;
            }
            row.typeName = ResolveTypeNameFromCache(rawRow.typeIndex, typeNameCache);
            if (options.typeNameMapFromObjectTab.find(rawRow.typeIndex) != options.typeNameMapFromObjectTab.end())
            {
                ++result.objectTypeMappedCount;
            }
            typeNameSet.insert(row.typeName);

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

            const bool pidMatchedForBudget = !options.hasPidFilter || row.processId == options.pidFilter;
            const bool typeMatchedForBudget = !hasTypeFilter || EqualsInsensitive(row.typeName, typeFilterText);
            const bool allowDuplicateQueries = options.enumMode == HandleEnumMode::DuplicateHandle || options.enumMode == HandleEnumMode::KernelHandleTable;
            const bool shouldQueryBasicInfo = allowDuplicateQueries && !row.basicInfoAvailable;
            const bool typeEligibleForNameResolve = allowDuplicateQueries && options.resolveObjectName &&
                pidMatchedForBudget && typeMatchedForBudget && ShouldAttemptNameQuery(row.typeName);
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
                HANDLE sourceProcessHandle = OpenProcessHandleForDuplicate(rawRow.processId, processHandleCache, failedProcessOpenSet);
                if (sourceProcessHandle != nullptr)
                {
                    HANDLE localRawHandle = nullptr;
                    if (DuplicateRemoteHandleToLocal(sourceProcessHandle, rawRow.handleValue, localRawHandle))
                    {
                        UniqueHandle localHandle(localRawHandle);
                        CachedObjectSnapshot* cachedSnapshot = rawRow.objectAddress != 0 ? &objectSnapshotCacheByAddress[rawRow.objectAddress] : nullptr;
                        if (shouldQueryBasicInfo)
                        {
                            ObjectBasicInfo basicInfo{};
                            if (QueryObjectBasicInfo(apiSet, localHandle.get(), basicInfo))
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
                            const ObjectNameQueryResult nameQueryResult = ResolveObjectNameText(apiSet, localHandle.get(), row.typeName);
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
                                if (nameQueryResult.failed) { ++nameQueryFailedCount; }
                            }
                        }
                    }
                    else
                    {
                        ++duplicateFailedCount;
                        if (shouldQueryBasicInfo) { ++basicInfoFailedCount; }
                        if (shouldQueryObjectName) { row.objectNameFailed = true; ++nameQueryFailedCount; }
                    }
                }
                else
                {
                    ++duplicateFailedCount;
                    if (shouldQueryBasicInfo) { ++basicInfoFailedCount; }
                    if (shouldQueryObjectName) { row.objectNameFailed = true; ++nameQueryFailedCount; }
                }
            }

            if (row.basicInfoAvailable) { ++result.basicInfoResolvedCount; }
            if (row.objectNameAvailable && !TrimWideCopy(row.objectName).empty())
            {
                ++result.resolvedNameCount;
                if (row.objectNameFromFallback) { ++result.fallbackNameCount; }
            }
            result.rows.push_back(std::move(row));
        }
        if (options.enumMode == HandleEnumMode::KernelHandleTable)
        {
            ksword::ark::DriverClient driverClient;
            std::size_t kernelDecodeProblemCount = 0;
            std::size_t kernelMappedTypeCount = 0;
            std::unordered_map<HandleIdentityKey, std::size_t, HandleIdentityKeyHash> rowIndexByKey;
            rowIndexByKey.reserve(result.rows.size());
            for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); ++rowIndex)
            {
                const HandleSnapshotRow& row = result.rows[rowIndex];
                rowIndexByKey[HandleIdentityKey{ row.processId, row.handleValue }] = rowIndex;
            }

            const ksword::ark::HandleEnumResult kernelResult = driverClient.enumerateProcessHandles(
                options.pidFilter,
                KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL);
            if (!kernelResult.io.ok)
            {
                queryDiagnosticText = L"R0 HandleTable 枚举失败: " + ks::str::Utf8ToUtf16(kernelResult.io.message);
            }
            else
            {
                result.kernelHandleCount = kernelResult.entries.size();
                for (const ksword::ark::HandleEntry& kernelEntry : kernelResult.entries)
                {
                    const HandleIdentityKey key{ kernelEntry.processId, static_cast<std::uint64_t>(kernelEntry.handleValue) };
                    const auto existingIt = rowIndexByKey.find(key);
                    HandleSnapshotRow* row = nullptr;
                    if (existingIt != rowIndexByKey.end())
                    {
                        row = &result.rows[existingIt->second];
                        row->diffStatus = HandleDiffStatus::Both;
                    }
                    else
                    {
                        HandleSnapshotRow kernelRow{};
                        kernelRow.processId = kernelEntry.processId;
                        kernelRow.processName = ProcessNameOf(processNameMap, kernelEntry.processId);
                        kernelRow.handleValue = static_cast<std::uint64_t>(kernelEntry.handleValue);
                        kernelRow.diffStatus = HandleDiffStatus::KernelOnly;
                        result.rows.push_back(std::move(kernelRow));
                        row = &result.rows.back();
                        rowIndexByKey[key] = result.rows.size() - 1U;
                    }

                    row->sourceMode = HandleEnumMode::KernelHandleTable;
                    row->typeIndex = static_cast<std::uint16_t>(kernelEntry.objectTypeIndex);
                    row->objectAddress = kernelEntry.objectAddress;
                    row->grantedAccess = kernelEntry.grantedAccess;
                    row->attributes = kernelEntry.attributes;
                    row->decodeStatus = kernelEntry.decodeStatus;
                    row->r0FieldFlags = kernelEntry.fieldFlags;
                    row->r0DynDataCapabilityMask = kernelEntry.dynDataCapabilityMask;
                    row->epObjectTableOffset = kernelEntry.epObjectTableOffset;
                    row->htHandleContentionEventOffset = kernelEntry.htHandleContentionEventOffset;
                    row->obDecodeShift = kernelEntry.obDecodeShift;
                    row->obAttributesShift = kernelEntry.obAttributesShift;
                    row->otNameOffset = kernelEntry.otNameOffset;
                    row->otIndexOffset = kernelEntry.otIndexOffset;
                    row->typeName = ResolveTypeNameFromCache(row->typeIndex, typeNameCache);
                    typeNameSet.insert(row->typeName);

                    if ((kernelEntry.fieldFlags & KSWORD_ARK_HANDLE_FIELD_TYPE_INDEX_PRESENT) != 0U)
                    {
                        ++kernelMappedTypeCount;
                    }
                    if (kernelEntry.decodeStatus != KSWORD_ARK_HANDLE_DECODE_STATUS_OK)
                    {
                        ++kernelDecodeProblemCount;
                    }
                }

                for (const HandleSnapshotRow& row : result.rows)
                {
                    if (row.diffStatus == HandleDiffStatus::UserOnly) { ++result.userOnlyCount; }
                    else if (row.diffStatus == HandleDiffStatus::KernelOnly) { ++result.kernelOnlyCount; }
                    else if (row.diffStatus == HandleDiffStatus::Both) { ++result.bothCount; }
                }
                if (kernelResult.totalCount > kernelResult.returnedCount)
                {
                    queryDiagnosticText = JoinDiagnostic({
                        queryDiagnosticText,
                        L"R0 HandleTable 输出截断 total=" + std::to_wstring(kernelResult.totalCount) +
                            L" returned=" + std::to_wstring(kernelResult.returnedCount)
                    });
                }
                if (kernelDecodeProblemCount > 0)
                {
                    queryDiagnosticText = JoinDiagnostic({ queryDiagnosticText, L"R0 解码异常:" + std::to_wstring(kernelDecodeProblemCount) });
                }
                if (kernelMappedTypeCount > 0)
                {
                    queryDiagnosticText = JoinDiagnostic({ queryDiagnosticText, L"R0 类型索引:" + std::to_wstring(kernelMappedTypeCount) });
                }
            }
        }
        else
        {
            result.userOnlyCount = result.rows.size();
        }

        std::sort(result.rows.begin(), result.rows.end(), [](const HandleSnapshotRow& leftRow, const HandleSnapshotRow& rightRow) {
            if (leftRow.processId != rightRow.processId) { return leftRow.processId < rightRow.processId; }
            if (leftRow.typeIndex != rightRow.typeIndex) { return leftRow.typeIndex < rightRow.typeIndex; }
            return leftRow.handleValue < rightRow.handleValue;
        });

        result.visibleHandleCount = result.rows.size();
        result.availableTypeList.assign(typeNameSet.begin(), typeNameSet.end());
        result.updatedTypeNameCacheByIndex = std::move(typeNameCache);

        std::vector<std::wstring> diagnosticList;
        diagnosticList.push_back(queryDiagnosticText);
        if (typeQueryFailedCount > 0) { diagnosticList.push_back(L"类型解析失败:" + std::to_wstring(typeQueryFailedCount)); }
        if (duplicateFailedCount > 0) { diagnosticList.push_back(L"句柄复制失败:" + std::to_wstring(duplicateFailedCount)); }
        if (basicInfoFailedCount > 0) { diagnosticList.push_back(L"对象计数查询失败:" + std::to_wstring(basicInfoFailedCount)); }
        if (nameQueryFailedCount > 0) { diagnosticList.push_back(L"对象名查询失败:" + std::to_wstring(nameQueryFailedCount)); }
        if (result.fallbackNameCount > 0) { diagnosticList.push_back(L"对象名回退命中:" + std::to_wstring(result.fallbackNameCount)); }
        if (nameBudgetSkippedCount > 0) { diagnosticList.push_back(L"对象名预算跳过:" + std::to_wstring(nameBudgetSkippedCount)); }
        if (options.resolveObjectName && nameBudgetSkippedCount > 0 && options.nameResolveBudget > 0)
        {
            diagnosticList.push_back(L"对象名解析已达到预算上限");
        }
        result.diagnosticText = JoinDiagnostic(diagnosticList);
        result.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beginTime).count());
        return result;
    }

    namespace
    {
        // ScanKernelHandleTableOccupancy uses the existing R0 ArkDriverClient path and
        // returns only entries that match target file/directory patterns.
        HandleUsageScanResult ScanKernelHandleTableOccupancy(
            const std::vector<TargetPathPattern>& targetPatterns,
            const std::unordered_map<std::uint32_t, std::wstring>& processNameMap)
        {
            HandleUsageScanResult result{};
            if (targetPatterns.empty())
            {
                result.diagnosticText = L"KernelHandleTable:目标为空";
                return result;
            }

            ksword::ark::DriverClient driverClient;
            const ksword::ark::ProcessEnumResult processResult = driverClient.enumerateProcesses(KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE);
            if (!processResult.io.ok || processResult.entries.empty())
            {
                result.diagnosticText = L"KernelHandleTable不可用:" + ks::str::Utf8ToUtf16(processResult.io.message);
                return result;
            }

            std::unordered_set<std::uint64_t> emittedHandleKeySet;
            std::size_t enumFailedCount = 0;
            std::size_t objectQueryFailedCount = 0;
            std::size_t nonFileSkippedCount = 0;
            for (const ksword::ark::ProcessEntry& processEntry : processResult.entries)
            {
                if (processEntry.processId == 0) { continue; }
                const ksword::ark::HandleEnumResult handleResult = driverClient.enumerateProcessHandles(
                    processEntry.processId,
                    KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL);
                if (!handleResult.io.ok || handleResult.entries.empty())
                {
                    ++enumFailedCount;
                    continue;
                }
                result.totalHandleCount += handleResult.entries.size();
                for (const ksword::ark::HandleEntry& handleEntry : handleResult.entries)
                {
                    const std::uint64_t handleKey = BuildHandleKey(handleEntry.processId, handleEntry.handleValue);
                    if (emittedHandleKeySet.find(handleKey) != emittedHandleKeySet.end()) { continue; }
                    const ksword::ark::HandleObjectQueryResult objectResult = driverClient.queryHandleObject(
                        handleEntry.processId,
                        handleEntry.handleValue,
                        KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL);
                    if (!objectResult.io.ok || objectResult.queryStatus == KSWORD_ARK_OBJECT_QUERY_STATUS_HANDLE_REFERENCE_FAILED)
                    {
                        ++objectQueryFailedCount;
                        continue;
                    }
                    const std::wstring typeName = objectResult.typeName;
                    if (!EqualsInsensitive(typeName, L"File") && !ContainsInsensitive(typeName, L"File"))
                    {
                        ++nonFileSkippedCount;
                        continue;
                    }
                    const std::wstring objectName = objectResult.objectName;
                    if (TrimWideCopy(objectName).empty())
                    {
                        ++objectQueryFailedCount;
                        continue;
                    }

                    std::wstring matchedTargetPath;
                    bool matchedByDirectoryRule = false;
                    if (!MatchTargetPath(NormalizePathForCompare(objectName), targetPatterns, matchedTargetPath, matchedByDirectoryRule))
                    {
                        continue;
                    }

                    HandleUsageEntry entry{};
                    entry.processId = handleEntry.processId;
                    entry.processName = ProcessNameOf(processNameMap, entry.processId);
                    entry.handleValue = handleEntry.handleValue;
                    entry.typeIndex = static_cast<std::uint16_t>(objectResult.objectTypeIndex);
                    entry.typeName = typeName.empty() ? L"File" : typeName;
                    entry.objectName = objectName;
                    entry.grantedAccess = objectResult.actualGrantedAccess != 0 ? objectResult.actualGrantedAccess : handleEntry.grantedAccess;
                    entry.attributes = handleEntry.attributes;
                    entry.matchedTargetPath = matchedTargetPath;
                    entry.matchedByDirectoryRule = matchedByDirectoryRule;
                    entry.matchRuleText = BuildPathRuleText(L"文件句柄", matchedByDirectoryRule);
                    entry.enumerationSource = L"Kernel HandleTable";
                    emittedHandleKeySet.insert(handleKey);
                    result.entries.push_back(std::move(entry));
                }
            }

            result.fileLikeHandleCount = result.entries.size();
            result.matchedHandleCount = result.entries.size();
            result.kernelHandleMatchCount = result.entries.size();
            std::vector<std::wstring> diagnosticList;
            diagnosticList.push_back(L"KernelHandleTable进程:" + std::to_wstring(processResult.entries.size()));
            if (enumFailedCount > 0) { diagnosticList.push_back(L"R0枚举失败进程:" + std::to_wstring(enumFailedCount)); }
            if (objectQueryFailedCount > 0) { diagnosticList.push_back(L"R0对象查询失败:" + std::to_wstring(objectQueryFailedCount)); }
            if (nonFileSkippedCount > 0) { diagnosticList.push_back(L"R0非File跳过:" + std::to_wstring(nonFileSkippedCount)); }
            result.diagnosticText = JoinDiagnostic(diagnosticList);
            return result;
        }
        // ScanFileHandleOccupancyByR3 scans duplicated File handles from the R3 system snapshot.
        HandleUsageScanResult ScanFileHandleOccupancyByR3(
            const std::vector<TargetPathPattern>& targetPatterns,
            const std::unordered_map<std::uint32_t, std::wstring>& processNameMap,
            const ProgressCallback& progressCallback)
        {
            HandleUsageScanResult result{};
            if (targetPatterns.empty())
            {
                result.diagnosticText = L"未提供有效目标路径。";
                return result;
            }

            UniqueHandle helperHandle;
            if (!OpenTargetPathHandle(targetPatterns.front(), helperHandle))
            {
                result.diagnosticText = L"打开目标路径失败，无法解析 File TypeIndex。";
                return result;
            }
            if (progressCallback) { progressCallback("准备抓取系统句柄快照", 10.0f); }

            const NtApiSet apiSet = QueryNtApis();
            std::vector<RawSystemHandle> rawRecords;
            std::wstring snapshotDiagnosticText;
            if (!QuerySystemHandles(apiSet, rawRecords, snapshotDiagnosticText))
            {
                result.diagnosticText = snapshotDiagnosticText;
                return result;
            }
            result.totalHandleCount = rawRecords.size();

            std::uint16_t fileTypeIndex = 0;
            if (!ResolveFileTypeIndex(helperHandle.get(), rawRecords, fileTypeIndex))
            {
                result.diagnosticText = L"动态 File TypeIndex 解析失败。";
                return result;
            }

            const std::uint64_t helperHandleKey = BuildHandleKey(
                ::GetCurrentProcessId(),
                static_cast<std::uint64_t>(reinterpret_cast<ULONG_PTR>(helperHandle.get())));
            std::unordered_set<std::uint64_t> emittedHandleKeySet;
            std::unordered_map<std::uint32_t, UniqueHandle> processHandleCache;
            std::unordered_set<std::uint32_t> failedProcessOpenSet;
            std::unordered_map<std::uint32_t, std::wstring> processImagePathCache;
            std::size_t openProcessFailedCount = 0;
            std::size_t duplicateFailedCount = 0;
            std::size_t pathQueryFailedCount = 0;
            std::size_t nonDiskFileSkippedCount = 0;
            if (progressCallback) { progressCallback("扫描文件句柄", 35.0f); }

            for (const RawSystemHandle& row : rawRecords)
            {
                if (row.typeIndex != fileTypeIndex)
                {
                    continue;
                }
                const std::uint64_t handleKey = BuildHandleKey(row.processId, row.handleValue);
                if (handleKey == helperHandleKey || emittedHandleKeySet.find(handleKey) != emittedHandleKeySet.end())
                {
                    continue;
                }

                HANDLE ownerProcessHandle = OpenProcessHandleForDuplicate(row.processId, processHandleCache, failedProcessOpenSet);
                if (ownerProcessHandle == nullptr)
                {
                    ++openProcessFailedCount;
                    continue;
                }
                HANDLE localRawHandle = nullptr;
                if (!DuplicateRemoteHandleToLocal(ownerProcessHandle, row.handleValue, localRawHandle))
                {
                    ++duplicateFailedCount;
                    continue;
                }
                UniqueHandle localHandle(localRawHandle);
                if (::GetFileType(localHandle.get()) != FILE_TYPE_DISK)
                {
                    ++nonDiskFileSkippedCount;
                    continue;
                }

                std::wstring finalPathText;
                if (!QueryFinalDosPathByHandle(localHandle.get(), finalPathText))
                {
                    std::wstring ntObjectPathText;
                    if (QueryNtObjectText(apiSet, localHandle.get(), kObjectNameInformationClass, ntObjectPathText))
                    {
                        finalPathText = TrimWideCopy(ntObjectPathText);
                    }
                }
                if (TrimWideCopy(finalPathText).empty())
                {
                    ++pathQueryFailedCount;
                    continue;
                }

                std::wstring matchedTargetPath;
                bool matchedByDirectoryRule = false;
                if (!MatchTargetPath(NormalizePathForCompare(finalPathText), targetPatterns, matchedTargetPath, matchedByDirectoryRule))
                {
                    continue;
                }

                HandleUsageEntry entry{};
                entry.processId = row.processId;
                entry.processName = ProcessNameOf(processNameMap, row.processId);
                entry.processImagePath = QueryProcessImagePathCached(row.processId, processImagePathCache);
                entry.handleValue = row.handleValue;
                entry.typeIndex = fileTypeIndex;
                entry.typeName = L"FileHandle";
                entry.objectName = NormalizeNativePath(finalPathText);
                entry.grantedAccess = row.grantedAccess;
                entry.attributes = row.attributes;
                entry.matchedTargetPath = matchedTargetPath;
                entry.matchedByDirectoryRule = matchedByDirectoryRule;
                entry.matchRuleText = BuildPathRuleText(L"文件句柄", matchedByDirectoryRule);
                entry.enumerationSource = L"R3 DuplicateHandle";
                emittedHandleKeySet.insert(handleKey);
                result.entries.push_back(std::move(entry));
            }

            result.fileLikeHandleCount = result.entries.size();
            result.matchedHandleCount = result.entries.size();
            std::vector<std::wstring> diagnosticList;
            diagnosticList.push_back(L"文件TypeIndex:" + std::to_wstring(fileTypeIndex));
            diagnosticList.push_back(snapshotDiagnosticText);
            if (openProcessFailedCount > 0) { diagnosticList.push_back(L"OpenProcess失败:" + std::to_wstring(openProcessFailedCount)); }
            if (duplicateFailedCount > 0) { diagnosticList.push_back(L"DuplicateHandle失败:" + std::to_wstring(duplicateFailedCount)); }
            if (pathQueryFailedCount > 0) { diagnosticList.push_back(L"路径查询失败:" + std::to_wstring(pathQueryFailedCount)); }
            if (nonDiskFileSkippedCount > 0) { diagnosticList.push_back(L"非磁盘File跳过:" + std::to_wstring(nonDiskFileSkippedCount)); }
            result.diagnosticText = JoinDiagnostic(diagnosticList);
            return result;
        }

        // AppendSyntheticOccupancyEntries adds image/module occupancy sources that do not
        // necessarily appear as File handles but still keep files busy on disk.
        void AppendSyntheticOccupancyEntries(
            const std::vector<TargetPathPattern>& targetPatterns,
            std::unordered_map<std::uint32_t, std::wstring>& processImagePathCache,
            const std::unordered_map<std::uint32_t, std::wstring>& processNameMap,
            std::vector<HandleUsageEntry>& entryList,
            std::size_t& processImageMatchCountOut,
            std::size_t& loadedModuleMatchCountOut,
            const ProgressCallback& progressCallback)
        {
            processImageMatchCountOut = 0;
            loadedModuleMatchCountOut = 0;
            if (progressCallback) { progressCallback("扫描进程映像占用", 90.0f); }

            std::set<std::wstring> syntheticDedupeSet;
            for (const auto& processPair : processNameMap)
            {
                const std::uint32_t processId = processPair.first;
                const std::wstring imagePath = QueryProcessImagePathCached(processId, processImagePathCache);
                std::wstring matchedTargetPath;
                bool matchedByDirectoryRule = false;
                if (!MatchTargetPath(NormalizePathForCompare(imagePath), targetPatterns, matchedTargetPath, matchedByDirectoryRule))
                {
                    continue;
                }
                const std::wstring dedupeKey = L"PI|" + std::to_wstring(processId) + L"|" + NormalizePathForCompare(imagePath);
                if (syntheticDedupeSet.find(dedupeKey) != syntheticDedupeSet.end())
                {
                    continue;
                }
                syntheticDedupeSet.insert(dedupeKey);

                HandleUsageEntry entry{};
                entry.processId = processId;
                entry.processName = processPair.second;
                entry.processImagePath = imagePath;
                entry.typeName = L"ProcessImage";
                entry.objectName = imagePath;
                entry.matchedTargetPath = matchedTargetPath;
                entry.matchedByDirectoryRule = matchedByDirectoryRule;
                entry.matchRuleText = BuildPathRuleText(L"进程映像", matchedByDirectoryRule);
                entry.enumerationSource = L"R3 ProcessImage";
                entryList.push_back(std::move(entry));
                ++processImageMatchCountOut;
            }

            if (progressCallback) { progressCallback("扫描模块加载占用", 95.0f); }
            for (const auto& processPair : processNameMap)
            {
                const std::uint32_t processId = processPair.first;
                UniqueHandle moduleSnapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId));
                if (!moduleSnapshot.valid())
                {
                    continue;
                }

                MODULEENTRY32W moduleEntry{};
                moduleEntry.dwSize = sizeof(moduleEntry);
                BOOL hasModule = ::Module32FirstW(moduleSnapshot.get(), &moduleEntry);
                while (hasModule != FALSE)
                {
                    const std::wstring modulePath = moduleEntry.szExePath;
                    std::wstring matchedTargetPath;
                    bool matchedByDirectoryRule = false;
                    if (MatchTargetPath(NormalizePathForCompare(modulePath), targetPatterns, matchedTargetPath, matchedByDirectoryRule))
                    {
                        const std::wstring dedupeKey = L"LM|" + std::to_wstring(processId) + L"|" + NormalizePathForCompare(modulePath);
                        if (syntheticDedupeSet.find(dedupeKey) == syntheticDedupeSet.end())
                        {
                            syntheticDedupeSet.insert(dedupeKey);
                            HandleUsageEntry entry{};
                            entry.processId = processId;
                            entry.processName = processPair.second;
                            entry.processImagePath = QueryProcessImagePathCached(processId, processImagePathCache);
                            entry.typeName = L"LoadedModule";
                            entry.objectName = modulePath;
                            entry.matchedTargetPath = matchedTargetPath;
                            entry.matchedByDirectoryRule = matchedByDirectoryRule;
                            entry.matchRuleText = BuildPathRuleText(L"模块加载", matchedByDirectoryRule);
                            entry.enumerationSource = L"R3 ModuleSnapshot";
                            entryList.push_back(std::move(entry));
                            ++loadedModuleMatchCountOut;
                        }
                    }
                    hasModule = ::Module32NextW(moduleSnapshot.get(), &moduleEntry);
                }
            }
        }
    }

    HandleUsageScanResult ScanHandleUsageByPaths(const std::vector<std::wstring>& absolutePaths, const HandleUsageScanOptions& options)
    {
        HandleUsageScanResult result{};
        const auto beginTime = std::chrono::steady_clock::now();
        const std::vector<TargetPathPattern> targetPatterns = BuildTargetPathPatterns(absolutePaths);
        if (targetPatterns.empty())
        {
            result.diagnosticText = L"未提供有效目标路径。";
            result.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
            return result;
        }

        const std::unordered_map<std::uint32_t, std::wstring> processNameMap = CollectProcessNameMap();
        std::unordered_map<std::uint32_t, std::wstring> processImagePathCache;
        if (options.progressCallback) { options.progressCallback("开始扫描占用来源", 5.0f); }

        HandleUsageScanResult kernelHandleResult{};
        bool kernelUsable = false;
        if (options.tryKernelHandleTable)
        {
            kernelHandleResult = ScanKernelHandleTableOccupancy(targetPatterns, processNameMap);
            kernelUsable = kernelHandleResult.diagnosticText.find(L"KernelHandleTable进程:") != std::wstring::npos;
        }

        const HandleUsageScanResult fileHandleResult = kernelHandleResult.entries.empty()
            ? ScanFileHandleOccupancyByR3(targetPatterns, processNameMap, options.progressCallback)
            : kernelHandleResult;
        result = fileHandleResult;

        AppendSyntheticOccupancyEntries(
            targetPatterns,
            processImagePathCache,
            processNameMap,
            result.entries,
            result.processImageMatchCount,
            result.loadedModuleMatchCount,
            options.progressCallback);

        std::sort(result.entries.begin(), result.entries.end(), [](const HandleUsageEntry& leftEntry, const HandleUsageEntry& rightEntry) {
            if (leftEntry.processId != rightEntry.processId) { return leftEntry.processId < rightEntry.processId; }
            if (leftEntry.handleValue != rightEntry.handleValue) { return leftEntry.handleValue < rightEntry.handleValue; }
            return leftEntry.objectName < rightEntry.objectName;
        });

        result.matchedHandleCount = result.entries.size();
        std::vector<std::wstring> diagnosticList;
        diagnosticList.push_back(result.diagnosticText);
        if (options.tryKernelHandleTable && kernelHandleResult.entries.empty())
        {
            if (!TrimWideCopy(kernelHandleResult.diagnosticText).empty())
            {
                diagnosticList.push_back(L"R0回退原因:" + kernelHandleResult.diagnosticText);
            }
            diagnosticList.push_back(L"文件句柄来源:R3 DuplicateHandle");
        }
        else if (kernelUsable)
        {
            diagnosticList.push_back(L"文件句柄来源:Kernel HandleTable");
        }
        if (result.processImageMatchCount > 0) { diagnosticList.push_back(L"进程映像占用:" + std::to_wstring(result.processImageMatchCount)); }
        if (result.loadedModuleMatchCount > 0) { diagnosticList.push_back(L"模块加载占用:" + std::to_wstring(result.loadedModuleMatchCount)); }
        result.diagnosticText = JoinDiagnostic(diagnosticList);
        if (options.progressCallback) { options.progressCallback("占用扫描完成", 100.0f); }
        result.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beginTime).count());
        return result;
    }
}
