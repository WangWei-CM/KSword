#include "startup.h"

#include "../string/string.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#include <Softpub.h>
#include <WinTrust.h>
#include <winsvc.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Wintrust.lib")

namespace
{
    // The backend stores all public strings as UTF-8 and converts to UTF-16 only at Win32 boundaries.
    std::string FromWide(const std::wstring& text)
    {
        return ks::str::Utf16ToUtf8(text);
    }

    // Win32 APIs require UTF-16; empty conversion failures naturally produce empty Win32 strings.
    std::wstring ToWide(const std::string& text)
    {
        return ks::str::Utf8ToUtf16(text);
    }

    // TrimWide mirrors ks::str::TrimCopy for temporary UTF-16 values returned by Win32 APIs.
    std::wstring TrimWide(const std::wstring& text)
    {
        std::size_t first = 0;
        while (first < text.size() && std::iswspace(text[first]))
        {
            ++first;
        }
        std::size_t last = text.size();
        while (last > first && std::iswspace(text[last - 1]))
        {
            --last;
        }
        return text.substr(first, last - first);
    }

    // LowerWideCopy is used for case-insensitive registry and command-line tests.
    std::wstring LowerWideCopy(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return text;
    }

    // LowerAsciiCopy is sufficient for registry catalog roots and de-duplication keys.
    std::string LowerAsciiCopy(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return text;
    }

    // Case-insensitive prefix check for ASCII catalog and JSON field values.
    bool StartsWithI(const std::string& text, const std::string& prefix)
    {
        if (text.size() < prefix.size())
        {
            return false;
        }
        return LowerAsciiCopy(text.substr(0, prefix.size())) == LowerAsciiCopy(prefix);
    }

    // Case-insensitive suffix check for UTF-16 registry subkey filtering.
    bool EndsWithI(const std::wstring& text, const std::wstring& suffix)
    {
        if (text.size() < suffix.size())
        {
            return false;
        }
        return LowerWideCopy(text.substr(text.size() - suffix.size())) == LowerWideCopy(suffix);
    }

    // Replace all ASCII occurrences without changing unrelated non-ASCII display text.
    void ReplaceAll(std::string& text, const std::string& from, const std::string& to)
    {
        if (from.empty())
        {
            return;
        }
        std::size_t offset = 0;
        while ((offset = text.find(from, offset)) != std::string::npos)
        {
            text.replace(offset, from.size(), to);
            offset += to.size();
        }
    }

    // Case-insensitive replace for noisy registry catalog input lines.
    void ReplaceAllI(std::string& text, const std::string& from, const std::string& to)
    {
        if (from.empty())
        {
            return;
        }
        std::string lowerText = LowerAsciiCopy(text);
        const std::string lowerFrom = LowerAsciiCopy(from);
        std::size_t offset = 0;
        while ((offset = lowerText.find(lowerFrom, offset)) != std::string::npos)
        {
            text.replace(offset, from.size(), to);
            lowerText.replace(offset, from.size(), LowerAsciiCopy(to));
            offset += to.size();
        }
    }

    // Convert slash variants to Windows native separators for display and Explorer operations.
    std::string ToNativeSeparators(std::string text)
    {
        std::replace(text.begin(), text.end(), '/', '\\');
        return text;
    }

    // Expand environment variables while preserving the original text on failure.
    std::wstring ExpandEnvironmentWide(const std::wstring& text)
    {
        if (TrimWide(text).empty())
        {
            return std::wstring();
        }
        std::array<wchar_t, 32768> buffer{};
        const DWORD chars = ::ExpandEnvironmentStringsW(text.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (chars == 0 || chars >= buffer.size())
        {
            return TrimWide(text);
        }
        return TrimWide(buffer.data());
    }

    // Query an environment variable as UTF-16 and return an empty string when it is absent.
    std::wstring QueryEnvironmentWide(const wchar_t* name)
    {
        std::array<wchar_t, 32768> buffer{};
        const DWORD chars = ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (chars == 0 || chars >= buffer.size())
        {
            return std::wstring();
        }
        return std::wstring(buffer.data(), chars);
    }

    // Append detail fragments without forcing the UI layer to know how backend details were assembled.
    void AppendDetailPart(std::string& detailText, const std::string& partText)
    {
        const std::string trimmedPart = ks::str::TrimCopy(partText);
        if (trimmedPart.empty())
        {
            return;
        }
        if (!ks::str::TrimCopy(detailText).empty())
        {
            detailText += FromWide(L"\uff1b");
        }
        detailText += trimmedPart;
    }

    // Join helper for registry value dumps and CSV-like action lists.
    std::string JoinStrings(const std::vector<std::string>& values, const std::string& separator)
    {
        std::ostringstream stream;
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            if (index != 0)
            {
                stream << separator;
            }
            stream << values[index];
        }
        return stream.str();
    }

    // FileExists intentionally accepts a command-extracted path; callers decide whether absence is suspicious.
    bool FileExists(const std::string& pathText)
    {
        const std::wstring pathWide = ToWide(pathText);
        if (pathWide.empty())
        {
            return false;
        }
        const DWORD attributes = ::GetFileAttributesW(pathWide.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    // FormatBinaryText keeps binary registry values readable and bounded.
    std::string FormatBinaryText(const std::vector<std::uint8_t>& rawBuffer)
    {
        static constexpr char hexDigits[] = "0123456789ABCDEF";
        const std::size_t displayCount = std::min<std::size_t>(rawBuffer.size(), 16);
        std::string text;
        for (std::size_t index = 0; index < displayCount; ++index)
        {
            if (!text.empty())
            {
                text.push_back(' ');
            }
            text.push_back(hexDigits[(rawBuffer[index] >> 4) & 0x0F]);
            text.push_back(hexDigits[rawBuffer[index] & 0x0F]);
        }
        if (rawBuffer.size() > displayCount)
        {
            text += " ... (" + std::to_string(rawBuffer.size()) + " bytes)";
        }
        return text;
    }

    // RegistryWideStringFromBuffer safely decodes REG_SZ/REG_EXPAND_SZ data.
    // Inputs:
    // - rawBuffer: raw bytes returned by RegQueryValueExW/RegEnumValueW.
    // Processing:
    // - Interpret only complete wchar_t units and trim one trailing NUL if present.
    // - Do not call wcslen because registry strings are not guaranteed to be terminated.
    // Return:
    // - UTF-16 string content without the terminator, or empty text for invalid/empty buffers.
    std::wstring RegistryWideStringFromBuffer(const std::vector<std::uint8_t>& rawBuffer)
    {
        const std::size_t wcharCount = rawBuffer.size() / sizeof(wchar_t);
        if (wcharCount == 0)
        {
            return std::wstring();
        }

        const wchar_t* textBegin = reinterpret_cast<const wchar_t*>(rawBuffer.data());
        std::size_t visibleCount = wcharCount;
        if (visibleCount > 0 && textBegin[visibleCount - 1] == L'\0')
        {
            --visibleCount;
        }
        return std::wstring(textBegin, textBegin + visibleCount);
    }

    // RegistryWideMultiStringFromBuffer safely decodes REG_MULTI_SZ data.
    // Inputs:
    // - rawBuffer: raw bytes returned for a multi-string registry value.
    // Processing:
    // - Walk bounded wchar_t units and split at NUL separators.
    // - Stop at an empty segment, which is the conventional REG_MULTI_SZ terminator.
    // Return:
    // - Non-empty UTF-16 segments; malformed missing double-NUL data is still bounded.
    std::vector<std::wstring> RegistryWideMultiStringFromBuffer(const std::vector<std::uint8_t>& rawBuffer)
    {
        std::vector<std::wstring> values;
        const std::size_t wcharCount = rawBuffer.size() / sizeof(wchar_t);
        if (wcharCount == 0)
        {
            return values;
        }

        const wchar_t* textBegin = reinterpret_cast<const wchar_t*>(rawBuffer.data());
        std::size_t offset = 0;
        while (offset < wcharCount)
        {
            const std::size_t segmentStart = offset;
            while (offset < wcharCount && textBegin[offset] != L'\0')
            {
                ++offset;
            }

            if (offset == segmentStart)
            {
                break;
            }

            values.emplace_back(textBegin + segmentStart, textBegin + offset);
            if (offset < wcharCount)
            {
                ++offset;
            }
        }
        return values;
    }

    // Read CompanyName from VERSIONINFO as the fast publisher fallback before WinVerifyTrust text.
    std::string QueryCompanyNameByVersion(const std::string& filePathText)
    {
        const std::wstring pathWide = ToWide(filePathText);
        if (pathWide.empty())
        {
            return std::string();
        }
        DWORD handleValue = 0;
        const DWORD bytes = ::GetFileVersionInfoSizeW(pathWide.c_str(), &handleValue);
        if (bytes == 0)
        {
            return std::string();
        }
        std::vector<std::uint8_t> buffer(bytes);
        if (::GetFileVersionInfoW(pathWide.c_str(), 0, bytes, buffer.data()) == FALSE)
        {
            return std::string();
        }
        struct LangAndCodePage { WORD language = 0; WORD codePage = 0; };
        LangAndCodePage* translation = nullptr;
        UINT translationBytes = 0;
        if (::VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translation), &translationBytes) == FALSE ||
            translation == nullptr || translationBytes < sizeof(LangAndCodePage))
        {
            return std::string();
        }
        wchar_t queryPath[64] = {};
        _snwprintf_s(queryPath, _countof(queryPath), _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\CompanyName", translation[0].language, translation[0].codePage);
        wchar_t* companyName = nullptr;
        UINT companyChars = 0;
        if (::VerQueryValueW(buffer.data(), queryPath, reinterpret_cast<LPVOID*>(&companyName), &companyChars) == FALSE || companyName == nullptr || companyChars <= 1)
        {
            return std::string();
        }
        return FromWide(TrimWide(companyName));
    }

    // WinVerifyTrust is used in cache-only mode to avoid UI/network prompts from the backend thread.
    bool IsFileTrustedByWindows(const std::string& filePathText)
    {
        const std::wstring pathWide = ToWide(filePathText);
        if (pathWide.empty())
        {
            return false;
        }
        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = pathWide.c_str();
        WINTRUST_DATA trustData{};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        trustData.pFile = &fileInfo;
        GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG result = ::WinVerifyTrust(nullptr, &policyGuid, &trustData);
        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        ::WinVerifyTrust(nullptr, &policyGuid, &trustData);
        return result == ERROR_SUCCESS;
    }
}

namespace
{
    // RegistryValueRecord stores one registry value converted to UTF-8 display text.
    struct RegistryValueRecord
    {
        std::string valueNameText;
        std::string valueDataText;
        DWORD valueType = REG_NONE;
    };

    // RootKeyText maps the limited root keys used by StartupDock-compatible enumerators.
    std::string RootKeyText(HKEY rootKey)
    {
        if (rootKey == HKEY_CURRENT_USER)
        {
            return "HKCU";
        }
        if (rootKey == HKEY_LOCAL_MACHINE)
        {
            return "HKLM";
        }
        if (rootKey == HKEY_CLASSES_ROOT)
        {
            return "HKCR";
        }
        return "UNKNOWN";
    }

    // BuildRegistryLocationText creates the exact location syntax consumed by StartupDock actions.
    std::string BuildRegistryLocationText(HKEY rootKey, const std::wstring& subKeyText)
    {
        return RootKeyText(rootKey) + "\\" + FromWide(subKeyText);
    }

    // RegistryDataToText converts common registry types into compact UTF-8 display strings.
    std::string RegistryDataToText(DWORD valueType, const std::vector<std::uint8_t>& rawBuffer)
    {
        if (rawBuffer.empty())
        {
            return std::string();
        }
        if (valueType == REG_SZ || valueType == REG_EXPAND_SZ)
        {
            std::wstring valueText = RegistryWideStringFromBuffer(rawBuffer);
            if (valueType == REG_EXPAND_SZ)
            {
                valueText = ExpandEnvironmentWide(valueText);
            }
            return FromWide(TrimWide(valueText));
        }
        if (valueType == REG_MULTI_SZ)
        {
            std::vector<std::string> items;
            for (const std::wstring& rawItemText : RegistryWideMultiStringFromBuffer(rawBuffer))
            {
                const std::wstring itemText = ExpandEnvironmentWide(rawItemText);
                if (!TrimWide(itemText).empty())
                {
                    items.push_back(FromWide(itemText));
                }
            }
            return JoinStrings(items, " | ");
        }
        if (valueType == REG_DWORD && rawBuffer.size() >= sizeof(DWORD))
        {
            const DWORD value = *reinterpret_cast<const DWORD*>(rawBuffer.data());
            std::ostringstream stream;
            stream << value << " (0x" << std::uppercase << std::hex;
            stream.width(8);
            stream.fill('0');
            stream << value << ")";
            return stream.str();
        }
        if (valueType == REG_QWORD && rawBuffer.size() >= sizeof(unsigned long long))
        {
            const unsigned long long value = *reinterpret_cast<const unsigned long long*>(rawBuffer.data());
            std::ostringstream stream;
            stream << value << " (0x" << std::uppercase << std::hex;
            stream.width(16);
            stream.fill('0');
            stream << value << ")";
            return stream.str();
        }
        return FormatBinaryText(rawBuffer);
    }

    // QueryRegistryValueRecord reads a named or default value and converts it to a backend value record.
    std::optional<RegistryValueRecord> QueryRegistryValueRecord(HKEY rootKey, const std::wstring& subKeyText, const std::wstring& valueNameText)
    {
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(rootKey, subKeyText.c_str(), 0, KEY_QUERY_VALUE, &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            return std::nullopt;
        }
        DWORD valueType = REG_NONE;
        DWORD bufferBytes = 0;
        const wchar_t* valueNamePointer = valueNameText.empty() ? nullptr : valueNameText.c_str();
        const LONG sizeResult = ::RegQueryValueExW(openedKey, valueNamePointer, nullptr, &valueType, nullptr, &bufferBytes);
        if (sizeResult != ERROR_SUCCESS || bufferBytes == 0)
        {
            ::RegCloseKey(openedKey);
            return std::nullopt;
        }
        std::vector<std::uint8_t> rawBuffer(static_cast<std::size_t>(bufferBytes));
        const LONG dataResult = ::RegQueryValueExW(openedKey, valueNamePointer, nullptr, &valueType, rawBuffer.data(), &bufferBytes);
        ::RegCloseKey(openedKey);
        if (dataResult != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        RegistryValueRecord record;
        record.valueNameText = FromWide(valueNameText);
        record.valueDataText = ks::str::TrimCopy(RegistryDataToText(valueType, rawBuffer));
        record.valueType = valueType;
        return record;
    }

    // EnumerateRegistryValues returns all values under a key; inaccessible keys simply yield no rows.
    std::vector<RegistryValueRecord> EnumerateRegistryValues(HKEY rootKey, const std::wstring& subKeyText)
    {
        std::vector<RegistryValueRecord> records;
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(rootKey, subKeyText.c_str(), 0, KEY_QUERY_VALUE, &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            return records;
        }
        DWORD valueIndex = 0;
        while (true)
        {
            std::array<wchar_t, 1024> valueName{};
            DWORD valueNameChars = static_cast<DWORD>(valueName.size());
            DWORD valueType = REG_NONE;
            DWORD dataBytes = 0;
            const LONG headerResult = ::RegEnumValueW(openedKey, valueIndex, valueName.data(), &valueNameChars, nullptr, &valueType, nullptr, &dataBytes);
            if (headerResult == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (headerResult != ERROR_SUCCESS)
            {
                ++valueIndex;
                continue;
            }
            std::vector<std::uint8_t> rawBuffer(static_cast<std::size_t>(dataBytes == 0 ? 2 : dataBytes));
            valueNameChars = static_cast<DWORD>(valueName.size());
            const LONG dataResult = ::RegEnumValueW(openedKey, valueIndex, valueName.data(), &valueNameChars, nullptr, &valueType, rawBuffer.data(), &dataBytes);
            if (dataResult == ERROR_SUCCESS)
            {
                RegistryValueRecord record;
                record.valueNameText = FromWide(std::wstring(valueName.data(), valueNameChars));
                record.valueDataText = ks::str::TrimCopy(RegistryDataToText(valueType, rawBuffer));
                record.valueType = valueType;
                records.push_back(std::move(record));
            }
            ++valueIndex;
        }
        ::RegCloseKey(openedKey);
        return records;
    }

    // EnumerateRegistrySubKeys lists first-level subkey names for registry persistence families.
    std::vector<std::wstring> EnumerateRegistrySubKeys(HKEY rootKey, const std::wstring& subKeyText)
    {
        std::vector<std::wstring> subKeys;
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(rootKey, subKeyText.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            return subKeys;
        }
        DWORD subKeyIndex = 0;
        while (true)
        {
            std::array<wchar_t, 1024> subKeyName{};
            DWORD subKeyChars = static_cast<DWORD>(subKeyName.size());
            const LONG enumResult = ::RegEnumKeyExW(openedKey, subKeyIndex, subKeyName.data(), &subKeyChars, nullptr, nullptr, nullptr, nullptr);
            if (enumResult == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (enumResult == ERROR_SUCCESS)
            {
                subKeys.emplace_back(subKeyName.data(), subKeyChars);
            }
            ++subKeyIndex;
        }
        ::RegCloseKey(openedKey);
        return subKeys;
    }

    // IsClsidText accepts the same relaxed {GUID} shape used by the previous UI-side backend.
    bool IsClsidText(const std::string& text)
    {
        const std::string trimmed = ks::str::TrimCopy(text);
        return trimmed.size() >= 38 && trimmed.front() == '{' && trimmed.back() == '}';
    }

    // QueryClsidFriendlyName gives COM rows a readable name when HKCR exposes one.
    std::string QueryClsidFriendlyName(const std::string& clsidText)
    {
        if (!IsClsidText(clsidText))
        {
            return std::string();
        }
        const auto record = QueryRegistryValueRecord(HKEY_CLASSES_ROOT, L"CLSID\\" + ToWide(ks::str::TrimCopy(clsidText)), L"");
        return record.has_value() ? record->valueDataText : std::string();
    }

    // QueryClsidServerPath resolves COM server paths from InprocServer32 or LocalServer32.
    std::string QueryClsidServerPath(const std::string& clsidText)
    {
        if (!IsClsidText(clsidText))
        {
            return std::string();
        }
        const std::wstring clsidSubKey = L"CLSID\\" + ToWide(ks::str::TrimCopy(clsidText));
        for (const std::wstring& candidate : { clsidSubKey + L"\\InprocServer32", clsidSubKey + L"\\LocalServer32" })
        {
            const auto record = QueryRegistryValueRecord(HKEY_CLASSES_ROOT, candidate, L"");
            if (record.has_value() && !ks::str::TrimCopy(record->valueDataText).empty())
            {
                return ks::startup::NormalizeFilePathText(record->valueDataText);
            }
        }
        return std::string();
    }

    // FinalizeRegistryEntry fills command, normalized path, publisher, and registry deletion metadata.
    void FinalizeRegistryEntry(
        ks::startup::StartupEntry& entry,
        const std::string& rawCommandText,
        const std::string& fallbackClsidText,
        const std::string& registryValueNameText,
        bool deleteRegistryTree,
        bool resolveClsidFromValueData)
    {
        entry.commandText = ks::str::TrimCopy(rawCommandText);
        entry.registryValueNameText = registryValueNameText;
        entry.deleteRegistryTree = deleteRegistryTree;
        entry.canOpenRegistryLocation = !ks::str::TrimCopy(entry.locationText).empty();
        entry.canDelete = entry.canOpenRegistryLocation;

        std::string resolvedImagePath;
        if (resolveClsidFromValueData && IsClsidText(entry.commandText))
        {
            resolvedImagePath = QueryClsidServerPath(entry.commandText);
            AppendDetailPart(entry.detailText, "CLSID=" + entry.commandText);
        }
        if (ks::str::TrimCopy(resolvedImagePath).empty() && IsClsidText(fallbackClsidText))
        {
            resolvedImagePath = QueryClsidServerPath(fallbackClsidText);
            AppendDetailPart(entry.detailText, "CLSID=" + fallbackClsidText);
        }
        const std::string clsidFriendlyName = IsClsidText(fallbackClsidText)
            ? QueryClsidFriendlyName(fallbackClsidText)
            : (IsClsidText(entry.commandText) ? QueryClsidFriendlyName(entry.commandText) : std::string());
        if (!ks::str::TrimCopy(clsidFriendlyName).empty())
        {
            AppendDetailPart(entry.detailText, FromWide(L"\u7ec4\u4ef6=") + clsidFriendlyName);
        }
        entry.imagePathText = ks::str::TrimCopy(resolvedImagePath).empty()
            ? ks::startup::NormalizeFilePathText(entry.commandText)
            : resolvedImagePath;
        entry.publisherText = ks::startup::QueryPublisherTextByPath(entry.imagePathText);
        entry.canOpenFileLocation = !ks::str::TrimCopy(entry.imagePathText).empty();
        entry.imagePathExists = FileExists(entry.imagePathText);
        entry.enabled = true;
    }

    // ProcessOutput contains captured stdout/stderr from a hidden child process.
    struct ProcessOutput
    {
        bool started = false;
        bool finished = false;
        DWORD exitCode = 0;
        std::string stdoutText;
        std::string stderrText;
    };

    // AppendPipeText drains available pipe bytes without blocking after process completion/timeout.
    void AppendPipeText(HANDLE pipeHandle, std::string& outputText)
    {
        if (pipeHandle == nullptr || pipeHandle == INVALID_HANDLE_VALUE)
        {
            return;
        }
        while (true)
        {
            DWORD availableBytes = 0;
            if (::PeekNamedPipe(pipeHandle, nullptr, 0, nullptr, &availableBytes, nullptr) == FALSE || availableBytes == 0)
            {
                break;
            }
            std::vector<char> buffer(std::min<DWORD>(availableBytes, 8192));
            DWORD readBytes = 0;
            if (::ReadFile(pipeHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &readBytes, nullptr) == FALSE || readBytes == 0)
            {
                break;
            }
            outputText.append(buffer.data(), buffer.data() + readBytes);
        }
    }

    // RunHiddenProcess executes PowerShell/schtasks-compatible commands without UI framework helpers.
    ProcessOutput RunHiddenProcess(const std::wstring& commandLine, DWORD timeoutMs)
    {
        ProcessOutput output;
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;

        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrRead = nullptr;
        HANDLE stderrWrite = nullptr;
        if (::CreatePipe(&stdoutRead, &stdoutWrite, &securityAttributes, 0) == FALSE ||
            ::CreatePipe(&stderrRead, &stderrWrite, &securityAttributes, 0) == FALSE)
        {
            if (stdoutRead != nullptr) ::CloseHandle(stdoutRead);
            if (stdoutWrite != nullptr) ::CloseHandle(stdoutWrite);
            if (stderrRead != nullptr) ::CloseHandle(stderrRead);
            if (stderrWrite != nullptr) ::CloseHandle(stderrWrite);
            return output;
        }
        ::SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;
        startupInfo.hStdOutput = stdoutWrite;
        startupInfo.hStdError = stderrWrite;
        startupInfo.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION processInfo{};
        std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back(L'\0');
        const BOOL createOk = ::CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
        ::CloseHandle(stdoutWrite);
        ::CloseHandle(stderrWrite);
        stdoutWrite = nullptr;
        stderrWrite = nullptr;
        if (createOk == FALSE)
        {
            ::CloseHandle(stdoutRead);
            ::CloseHandle(stderrRead);
            return output;
        }

        output.started = true;
        const DWORD startTick = ::GetTickCount();
        DWORD waitResult = WAIT_TIMEOUT;
        while (true)
        {
            // Drain stdout/stderr while the child is running so large JSON output cannot fill
            // the inherited pipe and deadlock the PowerShell process before it exits.
            AppendPipeText(stdoutRead, output.stdoutText);
            AppendPipeText(stderrRead, output.stderrText);
            waitResult = ::WaitForSingleObject(processInfo.hProcess, 50);
            if (waitResult == WAIT_OBJECT_0)
            {
                break;
            }
            const DWORD elapsedMs = ::GetTickCount() - startTick;
            if (elapsedMs >= timeoutMs)
            {
                break;
            }
        }
        if (waitResult == WAIT_TIMEOUT)
        {
            ::TerminateProcess(processInfo.hProcess, 1);
            ::WaitForSingleObject(processInfo.hProcess, 1500);
        }
        output.finished = waitResult == WAIT_OBJECT_0;
        DWORD exitCode = 0;
        if (::GetExitCodeProcess(processInfo.hProcess, &exitCode) != FALSE)
        {
            output.exitCode = exitCode;
        }
        AppendPipeText(stdoutRead, output.stdoutText);
        AppendPipeText(stderrRead, output.stderrText);
        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);
        ::CloseHandle(stdoutRead);
        ::CloseHandle(stderrRead);
        return output;
    }

    // Base64EncodeWideScript avoids command-line quoting bugs for complex PowerShell scripts.
    std::wstring Base64EncodeWideScript(const std::wstring& scriptText)
    {
        static constexpr wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(scriptText.data());
        const std::size_t byteCount = scriptText.size() * sizeof(wchar_t);
        std::wstring encoded;
        encoded.reserve(((byteCount + 2) / 3) * 4);
        for (std::size_t index = 0; index < byteCount; index += 3)
        {
            const std::uint32_t b0 = bytes[index];
            const std::uint32_t b1 = (index + 1 < byteCount) ? bytes[index + 1] : 0;
            const std::uint32_t b2 = (index + 2 < byteCount) ? bytes[index + 2] : 0;
            encoded.push_back(alphabet[(b0 >> 2) & 0x3F]);
            encoded.push_back(alphabet[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)]);
            encoded.push_back(index + 1 < byteCount ? alphabet[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : L'=');
            encoded.push_back(index + 2 < byteCount ? alphabet[b2 & 0x3F] : L'=');
        }
        return encoded;
    }

    // RunPowerShellScript runs an EncodedCommand script and returns UTF-8 decoded output.
    ProcessOutput RunPowerShellScript(const std::wstring& scriptText, DWORD timeoutMs)
    {
        const std::wstring encodedScript = Base64EncodeWideScript(scriptText);
        const std::wstring commandLine = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand " + encodedScript;
        ProcessOutput output = RunHiddenProcess(commandLine, timeoutMs);
        // PowerShell scripts set OutputEncoding=UTF8; fall back to raw bytes if conversion is unnecessary.
        return output;
    }
}

namespace
{
    // JsonValue is a small JSON DOM sufficient for PowerShell ConvertTo-Json results.
    struct JsonValue
    {
        enum class Type
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        };

        Type type = Type::Null;
        bool boolValue = false;
        double numberValue = 0.0;
        std::string stringValue;
        std::vector<JsonValue> arrayValue;
        std::map<std::string, JsonValue> objectValue;
    };

    // AppendUtf8CodePoint writes one Unicode scalar to a UTF-8 string.
    void AppendUtf8CodePoint(std::string& text, std::uint32_t codePoint)
    {
        if (codePoint <= 0x7F)
        {
            text.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FF)
        {
            text.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint <= 0xFFFF)
        {
            text.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            text.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    // JsonParser implements the small subset of RFC 8259 needed by PowerShell JSON output.
    class JsonParser
    {
    public:
        explicit JsonParser(std::string_view text) : m_text(text) {}

        // Parse reads a single JSON value and ignores trailing whitespace.
        bool Parse(JsonValue& valueOut)
        {
            SkipWhitespace();
            if (!ParseValue(valueOut))
            {
                return false;
            }
            SkipWhitespace();
            return m_offset == m_text.size();
        }

    private:
        // SkipWhitespace advances over JSON insignificant whitespace.
        void SkipWhitespace()
        {
            while (m_offset < m_text.size())
            {
                const unsigned char ch = static_cast<unsigned char>(m_text[m_offset]);
                if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
                {
                    break;
                }
                ++m_offset;
            }
        }

        // Consume checks and advances one expected byte.
        bool Consume(char expected)
        {
            if (m_offset >= m_text.size() || m_text[m_offset] != expected)
            {
                return false;
            }
            ++m_offset;
            return true;
        }

        // ParseValue dispatches by the current leading token.
        bool ParseValue(JsonValue& valueOut)
        {
            SkipWhitespace();
            if (m_offset >= m_text.size())
            {
                return false;
            }
            const char ch = m_text[m_offset];
            if (ch == '"')
            {
                valueOut.type = JsonValue::Type::String;
                return ParseString(valueOut.stringValue);
            }
            if (ch == '{')
            {
                return ParseObject(valueOut);
            }
            if (ch == '[')
            {
                return ParseArray(valueOut);
            }
            if (ch == 't' || ch == 'f')
            {
                return ParseBool(valueOut);
            }
            if (ch == 'n')
            {
                return ParseNull(valueOut);
            }
            return ParseNumber(valueOut);
        }

        // ParseHex4 decodes a JSON \uXXXX escape sequence.
        bool ParseHex4(std::uint32_t& valueOut)
        {
            if (m_offset + 4 > m_text.size())
            {
                return false;
            }
            std::uint32_t value = 0;
            for (int index = 0; index < 4; ++index)
            {
                const char ch = m_text[m_offset++];
                value <<= 4;
                if (ch >= '0' && ch <= '9') value |= static_cast<std::uint32_t>(ch - '0');
                else if (ch >= 'a' && ch <= 'f') value |= static_cast<std::uint32_t>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') value |= static_cast<std::uint32_t>(ch - 'A' + 10);
                else return false;
            }
            valueOut = value;
            return true;
        }

        // ParseString handles ordinary JSON escapes and UTF-16 surrogate pairs.
        bool ParseString(std::string& textOut)
        {
            if (!Consume('"'))
            {
                return false;
            }
            std::string result;
            while (m_offset < m_text.size())
            {
                const char ch = m_text[m_offset++];
                if (ch == '"')
                {
                    textOut = std::move(result);
                    return true;
                }
                if (ch != '\\')
                {
                    result.push_back(ch);
                    continue;
                }
                if (m_offset >= m_text.size())
                {
                    return false;
                }
                const char esc = m_text[m_offset++];
                switch (esc)
                {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u':
                {
                    std::uint32_t codePoint = 0;
                    if (!ParseHex4(codePoint))
                    {
                        return false;
                    }
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
                    {
                        const std::size_t savedOffset = m_offset;
                        if (m_offset + 2 <= m_text.size() && m_text[m_offset] == '\\' && m_text[m_offset + 1] == 'u')
                        {
                            m_offset += 2;
                            std::uint32_t lowSurrogate = 0;
                            if (ParseHex4(lowSurrogate) && lowSurrogate >= 0xDC00 && lowSurrogate <= 0xDFFF)
                            {
                                codePoint = 0x10000 + (((codePoint - 0xD800) << 10) | (lowSurrogate - 0xDC00));
                            }
                            else
                            {
                                m_offset = savedOffset;
                            }
                        }
                    }
                    AppendUtf8CodePoint(result, codePoint);
                    break;
                }
                default:
                    return false;
                }
            }
            return false;
        }

        // ParseObject reads a string-keyed JSON object.
        bool ParseObject(JsonValue& valueOut)
        {
            if (!Consume('{'))
            {
                return false;
            }
            valueOut = JsonValue{};
            valueOut.type = JsonValue::Type::Object;
            SkipWhitespace();
            if (Consume('}'))
            {
                return true;
            }
            while (true)
            {
                SkipWhitespace();
                std::string key;
                if (!ParseString(key))
                {
                    return false;
                }
                SkipWhitespace();
                if (!Consume(':'))
                {
                    return false;
                }
                JsonValue child;
                if (!ParseValue(child))
                {
                    return false;
                }
                valueOut.objectValue.emplace(std::move(key), std::move(child));
                SkipWhitespace();
                if (Consume('}'))
                {
                    return true;
                }
                if (!Consume(','))
                {
                    return false;
                }
            }
        }

        // ParseArray reads an ordered JSON array.
        bool ParseArray(JsonValue& valueOut)
        {
            if (!Consume('['))
            {
                return false;
            }
            valueOut = JsonValue{};
            valueOut.type = JsonValue::Type::Array;
            SkipWhitespace();
            if (Consume(']'))
            {
                return true;
            }
            while (true)
            {
                JsonValue child;
                if (!ParseValue(child))
                {
                    return false;
                }
                valueOut.arrayValue.push_back(std::move(child));
                SkipWhitespace();
                if (Consume(']'))
                {
                    return true;
                }
                if (!Consume(','))
                {
                    return false;
                }
            }
        }

        // ParseBool reads true/false literals.
        bool ParseBool(JsonValue& valueOut)
        {
            if (m_text.substr(m_offset, 4) == "true")
            {
                valueOut = JsonValue{};
                valueOut.type = JsonValue::Type::Bool;
                valueOut.boolValue = true;
                m_offset += 4;
                return true;
            }
            if (m_text.substr(m_offset, 5) == "false")
            {
                valueOut = JsonValue{};
                valueOut.type = JsonValue::Type::Bool;
                valueOut.boolValue = false;
                m_offset += 5;
                return true;
            }
            return false;
        }

        // ParseNull reads the null literal.
        bool ParseNull(JsonValue& valueOut)
        {
            if (m_text.substr(m_offset, 4) != "null")
            {
                return false;
            }
            valueOut = JsonValue{};
            valueOut.type = JsonValue::Type::Null;
            m_offset += 4;
            return true;
        }

        // ParseNumber stores the double form and keeps exact display through JsonValueToText when needed.
        bool ParseNumber(JsonValue& valueOut)
        {
            const std::size_t start = m_offset;
            if (m_offset < m_text.size() && m_text[m_offset] == '-')
            {
                ++m_offset;
            }
            while (m_offset < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_offset])))
            {
                ++m_offset;
            }
            if (m_offset < m_text.size() && m_text[m_offset] == '.')
            {
                ++m_offset;
                while (m_offset < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_offset])))
                {
                    ++m_offset;
                }
            }
            if (m_offset < m_text.size() && (m_text[m_offset] == 'e' || m_text[m_offset] == 'E'))
            {
                ++m_offset;
                if (m_offset < m_text.size() && (m_text[m_offset] == '+' || m_text[m_offset] == '-'))
                {
                    ++m_offset;
                }
                while (m_offset < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_offset])))
                {
                    ++m_offset;
                }
            }
            if (m_offset == start)
            {
                return false;
            }
            valueOut = JsonValue{};
            valueOut.type = JsonValue::Type::Number;
            valueOut.numberValue = std::strtod(std::string(m_text.substr(start, m_offset - start)).c_str(), nullptr);
            return true;
        }

        std::string_view m_text;
        std::size_t m_offset = 0;
    };

    // JsonValueToText produces the display string used by StartupEntry fields.
    std::string JsonValueToText(const JsonValue& value)
    {
        switch (value.type)
        {
        case JsonValue::Type::String:
            return ks::str::TrimCopy(value.stringValue);
        case JsonValue::Type::Number:
        {
            std::ostringstream stream;
            stream << value.numberValue;
            return stream.str();
        }
        case JsonValue::Type::Bool:
            return value.boolValue ? "true" : "false";
        case JsonValue::Type::Null:
            return std::string();
        case JsonValue::Type::Array:
        {
            std::vector<std::string> parts;
            for (const JsonValue& child : value.arrayValue)
            {
                parts.push_back(JsonValueToText(child));
            }
            return JoinStrings(parts, " | ");
        }
        case JsonValue::Type::Object:
            return "<object>";
        }
        return std::string();
    }

    // GetJsonField returns a named object field as display text, or empty for absent fields.
    std::string GetJsonField(const JsonValue& objectValue, const std::string& key)
    {
        if (objectValue.type != JsonValue::Type::Object)
        {
            return std::string();
        }
        const auto fieldIt = objectValue.objectValue.find(key);
        if (fieldIt == objectValue.objectValue.end())
        {
            return std::string();
        }
        return JsonValueToText(fieldIt->second);
    }

    // StripUtf8Bom removes a leading UTF-8 BOM because Windows PowerShell may emit it before JSON.
    std::string StripUtf8Bom(std::string text)
    {
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text.erase(0, 3);
        }
        return text;
    }

    // ParseJsonObjects normalizes a single JSON object or an array of objects into a vector.
    std::vector<JsonValue> ParseJsonObjects(const std::string& jsonText, bool* parseOkOut)
    {
        std::vector<JsonValue> objects;
        JsonValue root;
        JsonParser parser(jsonText);
        const bool parseOk = parser.Parse(root);
        if (parseOkOut != nullptr)
        {
            *parseOkOut = parseOk;
        }
        if (!parseOk)
        {
            return objects;
        }
        if (root.type == JsonValue::Type::Object)
        {
            objects.push_back(std::move(root));
        }
        else if (root.type == JsonValue::Type::Array)
        {
            for (JsonValue& value : root.arrayValue)
            {
                if (value.type == JsonValue::Type::Object)
                {
                    objects.push_back(std::move(value));
                }
            }
        }
        return objects;
    }
}

namespace
{
    // RunKeySpec describes Run/RunOnce style locations whose values are startup commands.
    struct RunKeySpec
    {
        HKEY rootKey = nullptr;
        const wchar_t* subKeyText = L"";
        const char* sourceTypeText = "";
        const wchar_t* userText = L"";
        const wchar_t* detailText = L"";
    };

    // SingleValueSpec describes a fixed key/value startup persistence source.
    struct SingleValueSpec
    {
        HKEY rootKey = nullptr;
        const wchar_t* subKeyText = L"";
        const wchar_t* valueNameText = L"";
        const char* sourceTypeText = "";
        const wchar_t* userText = L"";
        const wchar_t* detailText = L"";
        bool resolveClsidFromValueData = false;
    };

    // ValueEnumSpec describes a key where every value can represent a persistence item.
    struct ValueEnumSpec
    {
        HKEY rootKey = nullptr;
        const wchar_t* subKeyText = L"";
        const char* sourceTypeText = "";
        const wchar_t* userText = L"";
        const wchar_t* detailText = L"";
        bool resolveClsidFromValueData = false;
        bool resolveClsidFromValueName = false;
    };

    // SubKeyValueSpec describes sources where subkeys are enumerated and one value is read from each.
    struct SubKeyValueSpec
    {
        HKEY rootKey = nullptr;
        const wchar_t* subKeyText = L"";
        const wchar_t* valueNameText = L"";
        const char* sourceTypeText = "";
        const wchar_t* userText = L"";
        const wchar_t* detailText = L"";
        bool resolveClsidFromValueData = false;
        bool resolveClsidFromSubKeyName = false;
        bool deleteRegistryTree = false;
    };

    // BuildRunKeySpecList centralizes logon registry coverage.
    const std::array<RunKeySpec, 21>& BuildRunKeySpecList()
    {
        static const std::array<RunKeySpec, 21> specs{ {
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", "Run", L"当前用户", L"用户登录后自动运行" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "RunOnce", L"当前用户", L"当前用户一次性登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices", "RunServices", L"当前用户", L"兼容性 RunServices 登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", "RunServicesOnce", L"当前用户", L"兼容性 RunServicesOnce 登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", "PoliciesRun", L"当前用户", L"策略控制的登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", "WindowsRun", L"当前用户", L"Windows 兼容 Run/Load 位置" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Command Processor", "CommandProcessorAutorun", L"当前用户", L"命令行解释器 Autorun" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", "Run", L"本机", L"系统级登录后自动运行" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "RunOnce", L"本机", L"系统级一次性登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices", "RunServices", L"本机", L"兼容性 RunServices 登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", "RunServicesOnce", L"本机", L"兼容性 RunServicesOnce 登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", "PoliciesRun", L"本机", L"策略控制的登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", "WindowsRun", L"本机", L"Windows 兼容 Run/Load 位置" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Command Processor", "CommandProcessorAutorun", L"本机", L"命令行解释器 Autorun" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Terminal Server\\Install\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", "TerminalServerRun", L"本机", L"终端服务安装模式 Run" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Terminal Server\\Install\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "TerminalServerRunOnce", L"本机", L"终端服务安装模式 RunOnce" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", "Run32", L"本机(32位)", L"32 位视图 Run" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "RunOnce32", L"本机(32位)", L"32 位视图 RunOnce" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunServices", "RunServices32", L"本机(32位)", L"32 位视图 RunServices" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", "RunServicesOnce32", L"本机(32位)", L"32 位视图 RunServicesOnce" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", "PoliciesRun32", L"本机(32位)", L"32 位视图策略 Run" }
        } };
        return specs;
    }

    // BuildSingleValueSpecList centralizes fixed-value advanced registry persistence coverage.
    const std::array<SingleValueSpec, 20>& BuildSingleValueSpecList()
    {
        static const std::array<SingleValueSpec, 20> specs{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Shell", "WinlogonShell", L"本机", L"Winlogon Shell", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Userinit", "WinlogonUserinit", L"本机", L"Winlogon Userinit", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Taskman", "WinlogonTaskman", L"本机", L"Winlogon Taskman", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"VmApplet", "WinlogonVmApplet", L"本机", L"Winlogon VM Applet", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"GinaDLL", "WinlogonGinaDll", L"本机", L"旧式 GINA 登录 DLL", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"AppSetup", "WinlogonAppSetup", L"本机", L"Winlogon AppSetup", false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Shell", "UserWinlogonShell", L"当前用户", L"用户级 Winlogon Shell", false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Userinit", "UserWinlogonUserinit", L"当前用户", L"用户级 Winlogon Userinit", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"AppInit_DLLs", "AppInitDlls", L"本机", L"AppInit DLL 列表", false },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"AppInit_DLLs", "AppInitDlls32", L"本机(32位)", L"32 位 AppInit DLL 列表", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", L"Authentication Packages", "LsaAuthPackages", L"本机", L"LSA 认证包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", L"Security Packages", "LsaSecurityPackages", L"本机", L"LSA 安全包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", L"Notification Packages", "LsaNotificationPackages", L"本机", L"LSA 通知包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa\\OSConfig", L"Security Packages", "LsaOsConfigSecurityPackages", L"本机", L"LSA OSConfig 安全包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager", L"BootExecute", "BootExecute", L"本机", L"会话管理器启动前执行命令", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager", L"SetupExecute", "SetupExecute", L"本机", L"会话管理器 SetupExecute", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager", L"Execute", "SessionManagerExecute", L"本机", L"会话管理器 Execute", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"Shell", "PoliciesSystemShell", L"本机", L"策略指定系统 Shell", false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"Load", "WindowsLoad", L"当前用户", L"Windows 兼容 Load", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"Load", "MachineWindowsLoad", L"本机", L"系统级 Windows Load", false }
        } };
        return specs;
    }

    // BuildValueEnumSpecList centralizes advanced registry keys where all values are inspected.
    const std::array<ValueEnumSpec, 19>& BuildValueEnumSpecList()
    {
        static const std::array<ValueEnumSpec, 19> specs{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellExecuteHooks", "ShellExecuteHooks", L"本机", L"Explorer Shell Execute Hooks", true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellExecuteHooks", "ShellExecuteHooksUser", L"当前用户", L"用户级 Explorer Shell Execute Hooks", true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SharedTaskScheduler", "SharedTaskScheduler", L"本机", L"Explorer Shared Task Scheduler", true, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellServiceObjectDelayLoad", "ShellDelayLoad", L"本机", L"Explorer 延迟加载 COM", true, false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellServiceObjectDelayLoad", "ShellDelayLoadUser", L"当前用户", L"用户级 Explorer 延迟加载 COM", true, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", "ShellExtensionsApproved", L"本机", L"Shell 扩展白名单", false, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", "ShellExtensionsApprovedUser", L"当前用户", L"用户级 Shell 扩展白名单", false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Internet Explorer\\URLSearchHooks", "IEUrlSearchHooks", L"本机", L"Internet Explorer URL Search Hooks", true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\URLSearchHooks", "IEUrlSearchHooksUser", L"当前用户", L"用户级 URL Search Hooks", true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Internet Explorer\\Toolbar", "IEToolbar", L"本机", L"Internet Explorer Toolbar", true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\Toolbar", "IEToolbarUser", L"当前用户", L"用户级 Internet Explorer Toolbar", true, true },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager\\AppCertDlls", "AppCertDlls", L"本机", L"AppCert DLL 注入点", false, false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs", "KnownDlls", L"本机", L"Known DLL 列表", false, false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs32", "KnownDlls32", L"本机", L"32 位 Known DLL 列表", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", "Drivers32", L"本机", L"系统解码器/媒体驱动", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", "Drivers32Wow64", L"本机(32位)", L"32 位解码器/媒体驱动", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", "RunOnceExRoot", L"本机", L"RunOnceEx 根键直接值", false, false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", "RunOnceExRootUser", L"当前用户", L"用户级 RunOnceEx 根键直接值", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", "RunOnceExRoot32", L"本机(32位)", L"32 位 RunOnceEx 根键直接值", false, false }
        } };
        return specs;
    }

    // BuildSubKeyValueSpecList centralizes subkey-driven advanced registry persistence coverage.
    const std::array<SubKeyValueSpec, 15>& BuildSubKeyValueSpecList()
    {
        static const std::array<SubKeyValueSpec, 15> specs{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Active Setup\\Installed Components", L"StubPath", "ActiveSetup", L"本机", L"Active Setup StubPath", true, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", L"Debugger", "IFEO-Debugger", L"本机", L"映像执行调试器劫持", false, false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", L"VerifierDlls", "IFEO-VerifierDlls", L"本机", L"映像验证器 DLL", false, false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\SilentProcessExit", L"MonitorProcess", "SilentProcessExit", L"本机", L"SilentProcessExit 监视进程", false, false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\Notify", L"DLLName", "WinlogonNotify", L"本机", L"Winlogon Notify 包", false, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers", L"", "CredentialProvider", L"本机", L"Credential Provider", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Provider Filters", L"", "CredentialProviderFilter", L"本机", L"Credential Provider Filter", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Authentication\\PLAP Providers", L"", "PlapProvider", L"本机", L"PLAP Provider", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects", L"", "BHO", L"本机", L"Browser Helper Object", false, true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects", L"", "BHO-User", L"当前用户", L"用户级 Browser Helper Object", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Internet Explorer\\Explorer Bars", L"", "IEExplorerBar", L"本机", L"Internet Explorer Explorer Bar", false, true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\Explorer Bars", L"", "IEExplorerBar-User", L"当前用户", L"用户级 Explorer Bar", false, true, true },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Print\\Monitors", L"Driver", "PrintMonitor", L"本机", L"打印监视器驱动", false, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers", L"", "ShellIconOverlay", L"本机", L"Shell 图标覆盖标识符", true, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Sidebar\\Gadgets", L"Path", "SidebarGadget", L"本机", L"Sidebar Gadget 注册", false, false, true }
        } };
        return specs;
    }

    // AppendValueBasedLogonEntry adapts a registry value under a Run-like key into StartupEntry.
    void AppendValueBasedLogonEntry(std::vector<ks::startup::StartupEntry>& entries, const RunKeySpec& spec, const RegistryValueRecord& valueRecord)
    {
        if (ks::str::TrimCopy(valueRecord.valueDataText).empty())
        {
            return;
        }
        const std::wstring subKeyText(spec.subKeyText);
        const std::string locationText = BuildRegistryLocationText(spec.rootKey, subKeyText);
        ks::startup::StartupEntry entry;
        entry.category = ks::startup::StartupCategory::Logon;
        entry.categoryText = ks::startup::CategoryToText(entry.category);
        entry.itemNameText = ks::str::TrimCopy(valueRecord.valueNameText).empty() ? FromWide(L"(\u9ed8\u8ba4\u503c)") : valueRecord.valueNameText;
        entry.locationText = locationText;
        entry.locationGroupText = locationText;
        entry.userText = FromWide(spec.userText);
        entry.sourceTypeText = spec.sourceTypeText;
        entry.detailText = FromWide(spec.detailText);
        entry.uniqueIdText = "REGLOGON|" + locationText + "|" + entry.itemNameText;
        FinalizeRegistryEntry(entry, valueRecord.valueDataText, std::string(), valueRecord.valueNameText, false, false);
        entries.push_back(std::move(entry));
    }

    // AppendRunOnceExEntries handles RunOnceEx subkey/value layout specially.
    void AppendRunOnceExEntries(std::vector<ks::startup::StartupEntry>& entries)
    {
        const std::array<RunKeySpec, 3> specs{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", "RunOnceEx", L"本机", L"RunOnceEx 子键值" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", "RunOnceExUser", L"当前用户", L"用户级 RunOnceEx 子键值" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", "RunOnceEx32", L"本机(32位)", L"32 位 RunOnceEx 子键值" }
        } };
        for (const RunKeySpec& spec : specs)
        {
            const std::wstring rootSubKey(spec.subKeyText);
            const std::string groupLocationText = BuildRegistryLocationText(spec.rootKey, rootSubKey);
            for (const std::wstring& subKeyName : EnumerateRegistrySubKeys(spec.rootKey, rootSubKey))
            {
                const std::wstring itemSubKey = rootSubKey + L"\\" + subKeyName;
                for (const RegistryValueRecord& valueRecord : EnumerateRegistryValues(spec.rootKey, itemSubKey))
                {
                    const std::string valueName = ks::str::TrimCopy(valueRecord.valueNameText);
                    if (ks::str::TrimCopy(valueRecord.valueDataText).empty() || LowerAsciiCopy(valueName) == "flags" || LowerAsciiCopy(valueName) == "title")
                    {
                        continue;
                    }
                    const std::string subKeyNameText = FromWide(subKeyName);
                    ks::startup::StartupEntry entry;
                    entry.category = ks::startup::StartupCategory::Logon;
                    entry.categoryText = ks::startup::CategoryToText(entry.category);
                    entry.itemNameText = valueName.empty()
                        ? subKeyNameText + FromWide(L"\\(\u9ed8\u8ba4\u503c)")
                        : subKeyNameText + "\\" + valueName;
                    entry.locationText = BuildRegistryLocationText(spec.rootKey, itemSubKey);
                    entry.locationGroupText = groupLocationText;
                    entry.userText = FromWide(spec.userText);
                    entry.sourceTypeText = spec.sourceTypeText;
                    entry.detailText = FromWide(spec.detailText) + FromWide(L"\uff1b\u5b50\u952e=") + subKeyNameText;
                    entry.uniqueIdText = "RUNONCEEX|" + entry.locationText + "|" + entry.itemNameText;
                    FinalizeRegistryEntry(entry, valueRecord.valueDataText, std::string(), valueRecord.valueNameText, false, false);
                    entries.push_back(std::move(entry));
                }
            }
        }
    }

    // AppendStartupFolderEntries enumerates per-user and machine Startup folders.
    void AppendStartupFolderEntries(std::vector<ks::startup::StartupEntry>& entries)
    {
        const std::wstring appData = QueryEnvironmentWide(L"APPDATA");
        const std::wstring programData = QueryEnvironmentWide(L"ProgramData");
        const std::array<std::pair<std::wstring, const wchar_t*>, 2> folders{ {
            { appData.empty() ? std::wstring() : appData + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", L"当前用户" },
            { programData.empty() ? std::wstring() : programData + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", L"本机" }
        } };
        for (const auto& folder : folders)
        {
            if (folder.first.empty())
            {
                continue;
            }
            std::error_code ec;
            if (!std::filesystem::exists(folder.first, ec) || !std::filesystem::is_directory(folder.first, ec))
            {
                continue;
            }
            std::vector<std::filesystem::directory_entry> fileEntries;
            for (const auto& dirEntry : std::filesystem::directory_iterator(folder.first, ec))
            {
                if (!ec && dirEntry.is_regular_file(ec))
                {
                    fileEntries.push_back(dirEntry);
                }
            }
            std::sort(fileEntries.begin(), fileEntries.end(), [](const auto& left, const auto& right) {
                return LowerWideCopy(left.path().filename().wstring()) < LowerWideCopy(right.path().filename().wstring());
            });
            for (const auto& fileEntry : fileEntries)
            {
                const std::string filePathText = ToNativeSeparators(FromWide(fileEntry.path().wstring()));
                ks::startup::StartupEntry entry;
                entry.category = ks::startup::StartupCategory::Logon;
                entry.categoryText = ks::startup::CategoryToText(entry.category);
                entry.itemNameText = FromWide(fileEntry.path().filename().wstring());
                entry.commandText = filePathText;
                entry.imagePathText = filePathText;
                entry.publisherText = ks::startup::QueryPublisherTextByPath(entry.imagePathText);
                entry.locationText = ToNativeSeparators(FromWide(folder.first));
                entry.userText = FromWide(folder.second);
                entry.sourceTypeText = "StartupFolder";
                entry.detailText = FromWide(L"开始菜单启动文件夹");
                entry.enabled = true;
                entry.canOpenFileLocation = true;
                entry.canDelete = true;
                entry.imagePathExists = true;
                entry.uniqueIdText = "STARTUPFOLDER|" + filePathText;
                entries.push_back(std::move(entry));
            }
        }
    }
}

namespace
{
    // AppendSingleValueEntries adds fixed key/value advanced registry records.
    void AppendSingleValueEntries(std::vector<ks::startup::StartupEntry>& entries)
    {
        for (const SingleValueSpec& spec : BuildSingleValueSpecList())
        {
            const std::wstring subKeyText(spec.subKeyText);
            const std::wstring valueNameText(spec.valueNameText);
            const auto valueRecord = QueryRegistryValueRecord(spec.rootKey, subKeyText, valueNameText);
            if (!valueRecord.has_value() || ks::str::TrimCopy(valueRecord->valueDataText).empty())
            {
                continue;
            }
            ks::startup::StartupEntry entry;
            entry.category = ks::startup::StartupCategory::Registry;
            entry.categoryText = ks::startup::CategoryToText(entry.category);
            entry.itemNameText = valueNameText.empty() ? FromWide(L"(\u9ed8\u8ba4\u503c)") : FromWide(valueNameText);
            entry.locationText = BuildRegistryLocationText(spec.rootKey, subKeyText);
            entry.locationGroupText = entry.locationText;
            entry.userText = FromWide(spec.userText);
            entry.sourceTypeText = spec.sourceTypeText;
            entry.detailText = FromWide(spec.detailText);
            entry.uniqueIdText = "SINGLE|" + entry.locationText + "|" + entry.itemNameText;
            FinalizeRegistryEntry(entry, valueRecord->valueDataText, std::string(), FromWide(valueNameText), false, spec.resolveClsidFromValueData);
            entries.push_back(std::move(entry));
        }
    }

    // AppendValueEnumEntries adds one record for each non-empty value under known advanced keys.
    void AppendValueEnumEntries(std::vector<ks::startup::StartupEntry>& entries)
    {
        for (const ValueEnumSpec& spec : BuildValueEnumSpecList())
        {
            const std::wstring subKeyText(spec.subKeyText);
            const std::string locationText = BuildRegistryLocationText(spec.rootKey, subKeyText);
            for (const RegistryValueRecord& valueRecord : EnumerateRegistryValues(spec.rootKey, subKeyText))
            {
                if (ks::str::TrimCopy(valueRecord.valueDataText).empty())
                {
                    continue;
                }
                ks::startup::StartupEntry entry;
                entry.category = ks::startup::StartupCategory::Registry;
                entry.categoryText = ks::startup::CategoryToText(entry.category);
                entry.itemNameText = ks::str::TrimCopy(valueRecord.valueNameText).empty() ? FromWide(L"(\u9ed8\u8ba4\u503c)") : valueRecord.valueNameText;
                entry.locationText = locationText;
                entry.locationGroupText = locationText;
                entry.userText = FromWide(spec.userText);
                entry.sourceTypeText = spec.sourceTypeText;
                entry.detailText = FromWide(spec.detailText);
                entry.uniqueIdText = "VALUEENUM|" + locationText + "|" + entry.itemNameText;
                FinalizeRegistryEntry(
                    entry,
                    valueRecord.valueDataText,
                    spec.resolveClsidFromValueName ? valueRecord.valueNameText : std::string(),
                    valueRecord.valueNameText,
                    false,
                    spec.resolveClsidFromValueData);
                entries.push_back(std::move(entry));
            }
        }
    }

    // AppendSubKeyValueEntries adds records for subkey-driven advanced registry families.
    void AppendSubKeyValueEntries(std::vector<ks::startup::StartupEntry>& entries)
    {
        for (const SubKeyValueSpec& spec : BuildSubKeyValueSpecList())
        {
            const std::wstring rootSubKey(spec.subKeyText);
            const std::wstring valueNameText(spec.valueNameText);
            const std::string groupLocationText = BuildRegistryLocationText(spec.rootKey, rootSubKey);
            for (const std::wstring& subKeyName : EnumerateRegistrySubKeys(spec.rootKey, rootSubKey))
            {
                const std::wstring itemSubKey = rootSubKey + L"\\" + subKeyName;
                const auto valueRecord = QueryRegistryValueRecord(spec.rootKey, itemSubKey, valueNameText);
                if (!valueRecord.has_value() && !spec.resolveClsidFromSubKeyName)
                {
                    continue;
                }
                const std::string subKeyNameText = FromWide(subKeyName);
                std::string itemNameText = subKeyNameText;
                const std::string clsidFallbackText = spec.resolveClsidFromSubKeyName ? subKeyNameText : std::string();
                const std::string friendlyNameText = QueryClsidFriendlyName(clsidFallbackText);
                if (!ks::str::TrimCopy(friendlyNameText).empty())
                {
                    itemNameText = friendlyNameText;
                }
                std::string commandText = valueRecord.has_value() ? valueRecord->valueDataText : std::string();
                if (ks::str::TrimCopy(commandText).empty() && spec.resolveClsidFromSubKeyName)
                {
                    commandText = subKeyNameText;
                }
                if (ks::str::TrimCopy(commandText).empty())
                {
                    continue;
                }
                ks::startup::StartupEntry entry;
                entry.category = ks::startup::StartupCategory::Registry;
                entry.categoryText = ks::startup::CategoryToText(entry.category);
                entry.itemNameText = itemNameText;
                entry.locationText = BuildRegistryLocationText(spec.rootKey, itemSubKey);
                entry.locationGroupText = groupLocationText;
                entry.userText = FromWide(spec.userText);
                entry.sourceTypeText = spec.sourceTypeText;
                entry.detailText = FromWide(spec.detailText) + FromWide(L"\uff1b\u5b50\u952e=") + subKeyNameText;
                entry.uniqueIdText = "SUBKEY|" + entry.locationText + "|" + FromWide(valueNameText);
                FinalizeRegistryEntry(
                    entry,
                    commandText,
                    clsidFallbackText,
                    valueRecord.has_value() ? valueRecord->valueNameText : FromWide(valueNameText),
                    spec.deleteRegistryTree,
                    spec.resolveClsidFromValueData);
                entries.push_back(std::move(entry));
            }
        }
    }

    // QueryServiceBinaryPathText extracts the configured image path from QueryServiceConfig.
    std::string QueryServiceBinaryPathText(const QUERY_SERVICE_CONFIGW& serviceConfig)
    {
        if (serviceConfig.lpBinaryPathName == nullptr)
        {
            return std::string();
        }
        return ToNativeSeparators(FromWide(TrimWide(serviceConfig.lpBinaryPathName)));
    }

    // EnumerateScmEntries implements the shared service/driver backend with category-specific filters.
    std::vector<ks::startup::StartupEntry> EnumerateScmEntries(bool drivers)
    {
        std::vector<ks::startup::StartupEntry> entries;
        SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (scmHandle == nullptr)
        {
            return entries;
        }
        DWORD requiredBytes = 0;
        DWORD serviceCount = 0;
        DWORD resumeHandle = 0;
        const DWORD serviceType = drivers ? SERVICE_DRIVER : SERVICE_WIN32;
        ::EnumServicesStatusExW(scmHandle, SC_ENUM_PROCESS_INFO, serviceType, SERVICE_STATE_ALL, nullptr, 0, &requiredBytes, &serviceCount, &resumeHandle, nullptr);
        if (requiredBytes == 0)
        {
            ::CloseServiceHandle(scmHandle);
            return entries;
        }
        std::vector<std::uint8_t> buffer(requiredBytes);
        resumeHandle = 0;
        const BOOL enumOk = ::EnumServicesStatusExW(scmHandle, SC_ENUM_PROCESS_INFO, serviceType, SERVICE_STATE_ALL, buffer.data(), static_cast<DWORD>(buffer.size()), &requiredBytes, &serviceCount, &resumeHandle, nullptr);
        if (enumOk == FALSE)
        {
            ::CloseServiceHandle(scmHandle);
            return entries;
        }
        const auto* serviceArray = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD index = 0; index < serviceCount; ++index)
        {
            const ENUM_SERVICE_STATUS_PROCESSW& serviceItem = serviceArray[index];
            SC_HANDLE serviceHandle = ::OpenServiceW(scmHandle, serviceItem.lpServiceName, SERVICE_QUERY_CONFIG);
            if (serviceHandle == nullptr)
            {
                continue;
            }
            DWORD configBytes = 0;
            ::QueryServiceConfigW(serviceHandle, nullptr, 0, &configBytes);
            if (configBytes == 0)
            {
                ::CloseServiceHandle(serviceHandle);
                continue;
            }
            std::vector<std::uint8_t> configBuffer(configBytes);
            auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
            if (::QueryServiceConfigW(serviceHandle, config, configBytes, &configBytes) == FALSE)
            {
                ::CloseServiceHandle(serviceHandle);
                continue;
            }
            const DWORD startType = config->dwStartType;
            if ((!drivers && startType != SERVICE_AUTO_START) ||
                (drivers && startType != SERVICE_AUTO_START && startType != SERVICE_SYSTEM_START && startType != SERVICE_BOOT_START))
            {
                ::CloseServiceHandle(serviceHandle);
                continue;
            }
            const std::string serviceName = serviceItem.lpServiceName == nullptr ? std::string() : FromWide(serviceItem.lpServiceName);
            const std::string displayName = serviceItem.lpDisplayName == nullptr ? std::string() : FromWide(TrimWide(serviceItem.lpDisplayName));
            const std::string commandText = QueryServiceBinaryPathText(*config);
            ks::startup::StartupEntry entry;
            entry.category = drivers ? ks::startup::StartupCategory::Drivers : ks::startup::StartupCategory::Services;
            entry.categoryText = ks::startup::CategoryToText(entry.category);
            entry.itemNameText = displayName.empty() ? serviceName : displayName;
            entry.imagePathText = ks::startup::NormalizeFilePathText(commandText);
            entry.commandText = commandText;
            entry.publisherText = ks::startup::QueryPublisherTextByPath(entry.imagePathText);
            entry.locationText = std::string(drivers ? "SCM\\Driver\\" : "SCM\\Service\\") + serviceName;
            entry.userText = drivers ? FromWide(L"内核") : (config->lpServiceStartName == nullptr ? "N/A" : FromWide(config->lpServiceStartName));
            entry.enabled = true;
            entry.sourceTypeText = drivers ? "Driver" : "AutoService";
            if (drivers && startType == SERVICE_BOOT_START)
            {
                entry.detailText = FromWide(L"引导启动驱动");
            }
            else if (drivers && startType == SERVICE_SYSTEM_START)
            {
                entry.detailText = FromWide(L"系统启动驱动");
            }
            else
            {
                entry.detailText = drivers ? FromWide(L"自动启动驱动") : FromWide(L"自动启动服务");
            }
            entry.canOpenFileLocation = !entry.imagePathText.empty();
            entry.canDelete = true;
            entry.imagePathExists = FileExists(entry.imagePathText);
            entry.uniqueIdText = std::string(drivers ? "DRIVER|" : "SERVICE|") + serviceName;
            entries.push_back(std::move(entry));
            ::CloseServiceHandle(serviceHandle);
        }
        ::CloseServiceHandle(scmHandle);
        return entries;
    }
}

namespace
{
    // WinsockKeySpec describes one Winsock catalog registry root.
    struct WinsockKeySpec
    {
        HKEY rootKey = nullptr;
        const wchar_t* subKeyText = L"";
        const char* sourceTypeText = "";
        const wchar_t* userText = L"";
        const wchar_t* detailText = L"";
    };

    // BuildWinsockKeySpecList keeps Winsock provider/catalog coverage in one place.
    const std::array<WinsockKeySpec, 4>& BuildWinsockKeySpecList()
    {
        static const std::array<WinsockKeySpec, 4> specs{ {
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\Protocol_Catalog9\\Catalog_Entries", "Winsock-ProtocolCatalog", L"本机", L"Winsock Protocol Catalog" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\Protocol_Catalog9\\Catalog_Entries64", "Winsock-ProtocolCatalog64", L"本机", L"Winsock 64 位 Protocol Catalog" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\NameSpace_Catalog5\\Catalog_Entries", "Winsock-NameSpaceCatalog", L"本机", L"Winsock NameSpace Catalog" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\NameSpace_Catalog5\\Catalog_Entries64", "Winsock-NameSpaceCatalog64", L"本机", L"Winsock 64 位 NameSpace Catalog" }
        } };
        return specs;
    }

    // EnumerateRegistryValueTextList serializes every value in one registry key as name=value text.
    std::vector<std::string> EnumerateRegistryValueTextList(HKEY rootKey, const std::wstring& subKeyText)
    {
        std::vector<std::string> values;
        for (const RegistryValueRecord& record : EnumerateRegistryValues(rootKey, subKeyText))
        {
            const std::string nameText = ks::str::TrimCopy(record.valueNameText).empty() ? FromWide(L"(\u9ed8\u8ba4\u503c)") : record.valueNameText;
            values.push_back(nameText + "=" + record.valueDataText);
        }
        return values;
    }

    // AppendScheduledTaskJsonObject converts one PowerShell task object to StartupEntry.
    void AppendScheduledTaskJsonObject(std::vector<ks::startup::StartupEntry>& entries, const JsonValue& taskObject)
    {
        const std::string actionText = GetJsonField(taskObject, "Actions");
        const std::string taskPathText = GetJsonField(taskObject, "TaskPath");
        const std::string taskNameText = GetJsonField(taskObject, "TaskName");
        if (ks::str::TrimCopy(taskNameText).empty())
        {
            return;
        }
        ks::startup::StartupEntry entry;
        entry.category = ks::startup::StartupCategory::Tasks;
        entry.categoryText = ks::startup::CategoryToText(entry.category);
        entry.itemNameText = taskNameText;
        entry.commandText = actionText;
        entry.imagePathText = ks::startup::NormalizeFilePathText(actionText);
        entry.publisherText = ks::startup::QueryPublisherTextByPath(entry.imagePathText);
        entry.locationText = taskPathText + taskNameText;
        entry.userText = GetJsonField(taskObject, "UserId");
        entry.enabled = LowerAsciiCopy(GetJsonField(taskObject, "State")).find("disabled") == std::string::npos;
        entry.sourceTypeText = "ScheduledTask";
        entry.detailText = FromWide(L"\u72b6\u6001=") + GetJsonField(taskObject, "State")
            + FromWide(L"\uff1b\u89e6\u53d1\u5668=") + GetJsonField(taskObject, "Triggers")
            + FromWide(L"\uff1b\u63cf\u8ff0=") + GetJsonField(taskObject, "Description");
        entry.canOpenFileLocation = !entry.imagePathText.empty();
        entry.canDelete = true;
        entry.imagePathExists = FileExists(entry.imagePathText);
        entry.uniqueIdText = "TASK|" + entry.locationText;
        entries.push_back(std::move(entry));
    }

    // AppendWmiJsonObject converts one PowerShell WMI persistence object to StartupEntry.
    void AppendWmiJsonObject(std::vector<ks::startup::StartupEntry>& entries, const JsonValue& objectValue)
    {
        const std::string typeText = GetJsonField(objectValue, "Type");
        const std::string nameText = GetJsonField(objectValue, "Name");
        const std::string commandText = GetJsonField(objectValue, "Command");
        const std::string imagePathText = ks::startup::NormalizeFilePathText(GetJsonField(objectValue, "Image"));
        const std::string locationText = GetJsonField(objectValue, "Location");
        const std::string detailText = GetJsonField(objectValue, "Detail");
        if (ks::str::TrimCopy(typeText).empty() && ks::str::TrimCopy(nameText).empty() && ks::str::TrimCopy(commandText).empty())
        {
            return;
        }
        ks::startup::StartupEntry entry;
        entry.category = ks::startup::StartupCategory::Wmi;
        entry.categoryText = ks::startup::CategoryToText(entry.category);
        entry.itemNameText = ks::str::TrimCopy(nameText).empty() ? FromWide(L"(\u672a\u547d\u540dWMI\u9879)") : nameText;
        entry.publisherText = ks::startup::QueryPublisherTextByPath(imagePathText);
        entry.imagePathText = imagePathText;
        entry.commandText = commandText;
        entry.locationText = locationText;
        entry.userText = FromWide(L"本机");
        entry.detailText = detailText;
        entry.sourceTypeText = typeText;
        entry.enabled = true;
        entry.canOpenFileLocation = !entry.imagePathText.empty();
        entry.canOpenRegistryLocation = false;
        entry.canDelete = false;
        entry.imagePathExists = FileExists(entry.imagePathText);
        entry.uniqueIdText = "WMI|" + typeText + "|" + entry.itemNameText + "|" + locationText;
        entries.push_back(std::move(entry));
    }

    // BuildTaskPowerShellScript returns JSON for scheduled tasks while staying UI-framework-free.
    std::wstring BuildTaskPowerShellScript()
    {
        return LR"PS(
$ErrorActionPreference='SilentlyContinue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$taskList = @(Get-ScheduledTask | ForEach-Object {
  $actions = ($_.Actions | ForEach-Object { ($_.Execute + ' ' + $_.Arguments).Trim() }) -join ' | '
  $triggers = ($_.Triggers | ForEach-Object { $_.CimClass.CimClassName }) -join ' | '
  [PSCustomObject]@{
    TaskPath = $_.TaskPath
    TaskName = $_.TaskName
    State = [string]$_.State
    Author = $_.Author
    Description = $_.Description
    Actions = $actions
    Triggers = $triggers
    UserId = $_.Principal.UserId
  }
})
if ($taskList.Count -eq 0) { '[]' } else { $taskList | ConvertTo-Json -Depth 5 -Compress }
)PS";
    }

    // BuildWmiPowerShellScript returns JSON for common root\subscription persistence classes.
    std::wstring BuildWmiPowerShellScript()
    {
        return LR"PS(
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$items = @()
Get-CimInstance -Namespace root/subscription -ClassName CommandLineEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{ Type='WMI-CommandLineConsumer'; Name=$_.Name; Command=$_.CommandLineTemplate; Image=$_.ExecutablePath; Location='root\subscription\CommandLineEventConsumer'; Detail=('ExecutablePath=' + $_.ExecutablePath + '; WorkingDirectory=' + $_.WorkingDirectory) }
}
Get-CimInstance -Namespace root/subscription -ClassName ActiveScriptEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{ Type='WMI-ActiveScriptConsumer'; Name=$_.Name; Command=$_.ScriptText; Image=''; Location='root\subscription\ActiveScriptEventConsumer'; Detail=('ScriptingEngine=' + $_.ScriptingEngine) }
}
Get-CimInstance -Namespace root/subscription -ClassName LogFileEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{ Type='WMI-LogFileConsumer'; Name=$_.Name; Command=$_.Filename; Image=''; Location='root\subscription\LogFileEventConsumer'; Detail=('Text=' + $_.Text) }
}
Get-CimInstance -Namespace root/subscription -ClassName NTEventLogEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{ Type='WMI-NTEventLogConsumer'; Name=$_.Name; Command=$_.SourceName; Image=''; Location='root\subscription\NTEventLogEventConsumer'; Detail=('EventId=' + $_.EventID + '; Category=' + $_.Category) }
}
Get-CimInstance -Namespace root/subscription -ClassName __EventFilter | ForEach-Object {
    $items += [PSCustomObject]@{ Type='WMI-EventFilter'; Name=$_.Name; Command=$_.Query; Image=''; Location='root\subscription\__EventFilter'; Detail=('QueryLanguage=' + $_.QueryLanguage + '; EventNamespace=' + $_.EventNamespace) }
}
Get-CimInstance -Namespace root/subscription -ClassName __FilterToConsumerBinding | ForEach-Object {
    $items += [PSCustomObject]@{ Type='WMI-FilterToConsumerBinding'; Name=$_.Consumer; Command=$_.Filter; Image=''; Location='root\subscription\__FilterToConsumerBinding'; Detail=('Consumer=' + $_.Consumer + '; Filter=' + $_.Filter + '; DeliveryQoS=' + $_.DeliveryQoS) }
}
if ($items.Count -eq 0) { '[]' } else { $items | ConvertTo-Json -Compress -Depth 4 }
)PS";
    }
}

namespace ks::startup
{
    std::string CategoryToText(const StartupCategory category)
    {
        switch (category)
        {
        case StartupCategory::All:
            return FromWide(L"\u603b\u89c8");
        case StartupCategory::Logon:
            return FromWide(L"\u767b\u5f55");
        case StartupCategory::Services:
            return FromWide(L"\u670d\u52a1");
        case StartupCategory::Drivers:
            return FromWide(L"\u9a71\u52a8");
        case StartupCategory::Tasks:
            return FromWide(L"\u8ba1\u5212\u4efb\u52a1");
        case StartupCategory::Registry:
            return FromWide(L"\u9ad8\u7ea7\u6ce8\u518c\u8868");
        case StartupCategory::Wmi:
            return "WMI";
        default:
            return FromWide(L"\u672a\u77e5");
        }
    }

    std::string NormalizeFilePathText(const std::string& commandText)
    {
        std::wstring text = TrimWide(ToWide(commandText));
        if (text.empty())
        {
            return std::string();
        }
        text = ExpandEnvironmentWide(text);
        if (text.starts_with(L"\\??\\"))
        {
            text.erase(0, 4);
        }
        if (text.starts_with(L"\\SystemRoot\\"))
        {
            const std::wstring systemRoot = QueryEnvironmentWide(L"SystemRoot");
            if (!systemRoot.empty())
            {
                text = systemRoot + text.substr(std::wstring(L"\\SystemRoot").size());
            }
        }
        if (!text.empty() && text.front() == L'\"')
        {
            const std::size_t endQuote = text.find(L'\"', 1);
            if (endQuote != std::wstring::npos && endQuote > 1)
            {
                return ToNativeSeparators(FromWide(text.substr(1, endQuote - 1)));
            }
        }
        const std::wstring lowerText = LowerWideCopy(text);
        for (const std::wstring& extension : { L".exe", L".dll", L".sys" })
        {
            const std::size_t index = lowerText.find(extension);
            if (index != std::wstring::npos && index > 0)
            {
                return ToNativeSeparators(FromWide(text.substr(0, index + extension.size())));
            }
        }
        const std::size_t spaceIndex = text.find(L' ');
        if (spaceIndex != std::wstring::npos && spaceIndex > 0)
        {
            return ToNativeSeparators(FromWide(text.substr(0, spaceIndex)));
        }
        return ToNativeSeparators(FromWide(text));
    }

    std::string QueryPublisherTextByPath(const std::string& filePathText)
    {
        const std::string trimmedPath = ks::str::TrimCopy(filePathText);
        if (trimmedPath.empty() || !FileExists(trimmedPath))
        {
            return std::string();
        }
        const std::string companyName = QueryCompanyNameByVersion(trimmedPath);
        const bool trusted = IsFileTrustedByWindows(trimmedPath);
        if (!companyName.empty())
        {
            return companyName + (trusted ? " (Trusted)" : " (Untrusted)");
        }
        return trusted ? "Signed (Trusted)" : std::string();
    }

    std::string NormalizeRegistryLocationLine(const std::string& rawLineText)
    {
        std::string text = ks::str::TrimCopy(rawLineText);
        if (text.empty() || (!StartsWithI(text, "HKLM") && !StartsWithI(text, "HKCU") && !StartsWithI(text, "HKCR")))
        {
            return std::string();
        }
        std::replace(text.begin(), text.end(), '/', '\\');
        text = std::regex_replace(
            text,
            std::regex("^(HKLM|HKCU|HKCR)\\s+(SOFTWARE|SYSTEM|Software|System|Classes|Environment|Control Panel)", std::regex_constants::icase),
            "$1\\$2");
        text = std::regex_replace(text, std::regex("\\s*\\\\\\s*"), "\\");
        text = std::regex_replace(text, std::regex("\\\\{2,}"), "\\");
        const std::array<std::pair<const char*, const char*>, 28> replacements{ {
            { "HKLMSOFTWARE", "HKLM\\SOFTWARE" }, { "HKLMSoftware", "HKLM\\Software" }, { "HKLMSystem", "HKLM\\System" }, { "HKLMSYSTEM", "HKLM\\SYSTEM" },
            { "HKCU\\SOFTWAREClasses", "HKCU\\SOFTWARE\\Classes" }, { "HKCU\\SOFTWARE Classes", "HKCU\\SOFTWARE\\Classes" }, { "HKLM\\SOFTWAREWow6432Node", "HKLM\\SOFTWARE\\Wow6432Node" },
            { "SOFTWAREClasses", "SOFTWARE\\Classes" }, { "SOFTWARE Classes", "SOFTWARE\\Classes" }, { "ShelllconOverlayldentifiers", "ShellIconOverlayIdentifiers" },
            { "Catalog Entries64", "Catalog_Entries64" }, { "Folder ShellEx", "Folder\\ShellEx" }, { "Explorer ShellExecuteHooks", "Explorer\\ShellExecuteHooks" },
            { "Explorer ShellServiceObjects", "Explorer\\ShellServiceObjects" }, { "ShellExecute Hooks", "ShellExecuteHooks" }, { "Internet ExplorerExtensions", "Internet Explorer\\Extensions" },
            { "Intemet", "Internet" }, { "Interet", "Internet" }, { "Userlnit", "Userinit" }, { "Scmsave.exe", "Scrnsave.exe" }, { "AutoStartDisconnect", "AutoStartOnDisconnect" },
            { "Appinit Dlls", "AppInit_DLLs" }, { "Appinit_Dlls", "AppInit_DLLs" }, { "Session Manager\\SOInitialCommand", "Session Manager\\S0InitialCommand" },
            { "HKCU\\Software\\Classes\\M\\ShellEx\\ContextMenuHandlers", "HKCU\\Software\\Classes\\*\\ShellEx\\ContextMenuHandlers" },
            { "HKCU\\Software\\Classes\\\\ShellEx\\PropertySheetHandlers", "HKCU\\Software\\Classes\\*\\ShellEx\\PropertySheetHandlers" },
            { "HKLM\\Software\\Classes\\\\ShellEx\\PropertySheetHandlers", "HKLM\\Software\\Classes\\*\\ShellEx\\PropertySheetHandlers" },
            { "HKLM\\Software\\Wow6432Node\\Classes\\\\ShellEx\\PropertySheetHandlers", "HKLM\\Software\\Wow6432Node\\Classes\\*\\ShellEx\\PropertySheetHandlers" }
        } };
        for (const auto& rule : replacements)
        {
            ReplaceAllI(text, rule.first, rule.second);
        }
        text = std::regex_replace(
            text,
            std::regex("\\\\CLSID\\\\\\{?\\(?([0-9A-Fa-f\\-]{36})\\)?\\}?\\\\"),
            "\\\\CLSID\\\\{$1}\\\\");
        if ((StartsWithI(text, "HKLM") || StartsWithI(text, "HKCU") || StartsWithI(text, "HKCR")) && text.size() > 4 && text[4] != '\\')
        {
            text.insert(4, "\\");
        }
        text = std::regex_replace(text, std::regex("\\s*\\\\\\s*"), "\\");
        text = std::regex_replace(text, std::regex("\\\\{2,}"), "\\");
        text = ks::str::TrimCopy(text);
        if (!StartsWithI(text, "HKLM\\") && !StartsWithI(text, "HKCU\\") && !StartsWithI(text, "HKCR\\"))
        {
            return std::string();
        }
        text.replace(0, 4, LowerAsciiCopy(text.substr(0, 4)) == "hklm" ? "HKLM" : (LowerAsciiCopy(text.substr(0, 4)) == "hkcu" ? "HKCU" : "HKCR"));
        ReplaceAllI(text, "}\\)\\InProcServer32", "}\\InProcServer32");
        ReplaceAllI(text, "}\\)\\Instance", "}\\Instance");
        if (LowerAsciiCopy(text).ends_with("\\(default)"))
        {
            text.erase(text.size() - std::string("\\(Default)").size());
        }
        return text;
    }

    std::vector<std::string> BuildKnownStartupRegistryLocationList(const std::vector<std::string>& rawLineList)
    {
        std::vector<std::string> locations;
        std::vector<std::string> dedupeKeys;
        for (const std::string& rawLine : rawLineList)
        {
            const std::string normalized = NormalizeRegistryLocationLine(rawLine);
            if (normalized.empty())
            {
                continue;
            }
            const std::string key = LowerAsciiCopy(normalized);
            if (std::find(dedupeKeys.begin(), dedupeKeys.end(), key) != dedupeKeys.end())
            {
                continue;
            }
            dedupeKeys.push_back(key);
            locations.push_back(normalized);
        }
        return locations;
    }

    std::vector<StartupEntry> EnumerateLogonEntries()
    {
        std::vector<StartupEntry> entries;
        for (const RunKeySpec& spec : BuildRunKeySpecList())
        {
            const std::wstring subKeyText(spec.subKeyText);
            for (const RegistryValueRecord& valueRecord : EnumerateRegistryValues(spec.rootKey, subKeyText))
            {
                if (ks::str::TrimCopy(valueRecord.valueDataText).empty())
                {
                    continue;
                }
                if (EndsWithI(subKeyText, L"\\Windows"))
                {
                    const std::string lowerName = LowerAsciiCopy(ks::str::TrimCopy(valueRecord.valueNameText));
                    if (lowerName != "run" && lowerName != "load")
                    {
                        continue;
                    }
                }
                if (EndsWithI(subKeyText, L"Command Processor") && LowerAsciiCopy(valueRecord.valueNameText) != "autorun")
                {
                    continue;
                }
                AppendValueBasedLogonEntry(entries, spec, valueRecord);
            }
        }
        AppendRunOnceExEntries(entries);
        AppendStartupFolderEntries(entries);
        return entries;
    }

    std::vector<StartupEntry> EnumerateServiceEntries()
    {
        return EnumerateScmEntries(false);
    }

    std::vector<StartupEntry> EnumerateDriverEntries()
    {
        return EnumerateScmEntries(true);
    }

    std::vector<StartupEntry> EnumerateTaskEntries()
    {
        std::vector<StartupEntry> entries;
        const ProcessOutput output = RunPowerShellScript(BuildTaskPowerShellScript(), 20000);
        if (!output.started || !output.finished || output.stdoutText.empty())
        {
            return entries;
        }
        bool parseOk = false;
        const std::vector<JsonValue> taskObjects = ParseJsonObjects(StripUtf8Bom(ks::str::TrimCopy(output.stdoutText)), &parseOk);
        if (!parseOk)
        {
            return entries;
        }
        for (const JsonValue& taskObject : taskObjects)
        {
            AppendScheduledTaskJsonObject(entries, taskObject);
        }
        return entries;
    }

    std::vector<StartupEntry> EnumerateAdvancedRegistryEntries()
    {
        std::vector<StartupEntry> entries;
        AppendSingleValueEntries(entries);
        AppendValueEnumEntries(entries);
        AppendSubKeyValueEntries(entries);
        return entries;
    }

    std::vector<StartupEntry> EnumerateWinsockEntries()
    {
        std::vector<StartupEntry> entries;
        for (const WinsockKeySpec& spec : BuildWinsockKeySpecList())
        {
            const std::wstring rootSubKey(spec.subKeyText);
            const std::string groupLocationText = BuildRegistryLocationText(spec.rootKey, rootSubKey);
            for (const std::wstring& subKeyName : EnumerateRegistrySubKeys(spec.rootKey, rootSubKey))
            {
                const std::wstring itemSubKey = rootSubKey + L"\\" + subKeyName;
                const std::string subKeyNameText = FromWide(subKeyName);
                const std::vector<std::string> valueTexts = EnumerateRegistryValueTextList(spec.rootKey, itemSubKey);
                StartupEntry entry;
                entry.category = StartupCategory::Registry;
                entry.categoryText = CategoryToText(entry.category);
                entry.itemNameText = FromWide(L"Winsock \u9879 ") + subKeyNameText;
                entry.commandText = JoinStrings(valueTexts, FromWide(L"\uff1b"));
                entry.locationText = BuildRegistryLocationText(spec.rootKey, itemSubKey);
                entry.locationGroupText = groupLocationText;
                entry.userText = FromWide(spec.userText);
                entry.detailText = FromWide(spec.detailText) + FromWide(L"\uff1b\u952e\u503c\u6570\u91cf=") + std::to_string(valueTexts.size());
                entry.sourceTypeText = spec.sourceTypeText;
                entry.enabled = true;
                entry.canOpenFileLocation = false;
                entry.canOpenRegistryLocation = true;
                entry.canDelete = false;
                entry.deleteRegistryTree = false;
                entry.uniqueIdText = "WINSOCK|" + entry.locationText;
                entries.push_back(std::move(entry));
            }
        }
        return entries;
    }

    std::vector<StartupEntry> EnumerateWmiEntries()
    {
        std::vector<StartupEntry> entries;
        const ProcessOutput output = RunPowerShellScript(BuildWmiPowerShellScript(), 15000);
        if (!output.started || !output.finished)
        {
            return entries;
        }
        const std::string stdoutText = StripUtf8Bom(ks::str::TrimCopy(output.stdoutText));
        if (stdoutText.empty())
        {
            return entries;
        }
        bool parseOk = false;
        const std::vector<JsonValue> wmiObjects = ParseJsonObjects(stdoutText, &parseOk);
        if (!parseOk)
        {
            StartupEntry errorEntry;
            errorEntry.category = StartupCategory::Wmi;
            errorEntry.categoryText = CategoryToText(errorEntry.category);
            errorEntry.itemNameText = FromWide(L"WMI \u679a\u4e3e\u89e3\u6790\u5931\u8d25");
            errorEntry.commandText = stdoutText;
            errorEntry.locationText = "root\\subscription";
            errorEntry.userText = FromWide(L"本机");
            errorEntry.detailText = FromWide(L"JSON \u89e3\u6790\u5931\u8d25");
            errorEntry.sourceTypeText = "WMI-ParseError";
            errorEntry.enabled = false;
            errorEntry.uniqueIdText = "WMI|ParseError";
            entries.push_back(std::move(errorEntry));
            return entries;
        }
        for (const JsonValue& wmiObject : wmiObjects)
        {
            AppendWmiJsonObject(entries, wmiObject);
        }
        return entries;
    }

    std::vector<StartupEntry> EnumerateAllStartupEntries()
    {
        std::vector<StartupEntry> entries;
        auto append = [&entries](std::vector<StartupEntry> part) {
            entries.insert(entries.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        };
        append(EnumerateLogonEntries());
        append(EnumerateServiceEntries());
        append(EnumerateDriverEntries());
        append(EnumerateTaskEntries());
        append(EnumerateAdvancedRegistryEntries());
        append(EnumerateWinsockEntries());
        append(EnumerateWmiEntries());
        return entries;
    }
}
