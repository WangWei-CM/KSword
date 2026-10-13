#pragma once

// ============================================================
// ksword/startup/startup_internal.h
// Namespace: ks::r3::startup::detail
// Purpose:
// - Share the registry/entry plumbing that startup.cpp already implements.
// - Let sibling translation units (startup_hidden.cpp) build StartupEntry records
//   with exactly the same location text, risk wording and action locators.
// - This header is backend-internal: no UI layer may include it.
// ============================================================

#include "StartupTypes.h" // ks::r3::startup::StartupEntry and the public enums it carries.

#ifndef NOMINMAX
#define NOMINMAX // Windows.h must not define the min/max macros; <algorithm> users depend on that.
#endif
#include <Windows.h>      // HKEY, DWORD and the registry API types used across the helpers.
#include <ShTypes.h>      // KNOWNFOLDERID: the GUID alias KnownFolders.h constants are typed with.
#include <KnownFolders.h> // FOLDERID_*: identifies shell folders without trusting %ENV%.

#include <cstdint> // std::uint8_t: raw registry data bytes.
#include <optional> // std::optional: "value may be absent" registry reads.
#include <string>  // std::string / std::wstring: UTF-8 storage and Win32 boundary text.
#include <vector>  // std::vector: value/subkey collections.

namespace ks::r3::startup::detail
{
    // RegistryValueRecord:
    // - Purpose: one registry value converted to UTF-8 display text plus its raw bytes.
    // - valueNameText: UTF-8 value name; empty means the key default value.
    // - valueDataText: display text produced by RegistryDataToText.
    // - valueType: original REG_* type, preserved so a disable/restore keeps fidelity.
    // - rawData: untouched bytes, used as the rollback snapshot for reversible actions.
    struct RegistryValueRecord
    {
        std::string valueNameText;
        std::string valueDataText;
        DWORD valueType = REG_NONE;
        std::vector<std::uint8_t> rawData;
    };

    // ===================== Text helpers =====================

    // FromWide: converts a UTF-16 Win32 string into the UTF-8 text the backend stores.
    std::string FromWide(const std::wstring& text);

    // ToWide: converts backend UTF-8 text back into UTF-16 for Win32 calls.
    std::wstring ToWide(const std::string& text);

    // TrimWide: removes leading/trailing whitespace from UTF-16 text.
    std::wstring TrimWide(const std::wstring& text);

    // LowerWideCopy: lowercases UTF-16 text for case-insensitive comparison keys.
    std::wstring LowerWideCopy(std::wstring text);

    // LowerAsciiCopy: lowercases ASCII ranges only, leaving UTF-8 multibyte sequences untouched.
    std::string LowerAsciiCopy(std::string text);

    // StartsWithI: case-insensitive ASCII prefix test.
    bool StartsWithI(const std::string& text, const std::string& prefix);

    // EndsWithI: case-insensitive UTF-16 suffix test.
    bool EndsWithI(const std::wstring& text, const std::wstring& suffix);

    // ToNativeSeparators: normalizes forward slashes into backslashes for display paths.
    std::string ToNativeSeparators(std::string text);

    // ExpandEnvironmentWide: expands %VAR% references; returns the input when expansion fails.
    std::wstring ExpandEnvironmentWide(const std::wstring& text);

    // QueryEnvironmentWide: reads one environment variable; returns empty when unset.
    std::wstring QueryEnvironmentWide(const wchar_t* name);

    // KnownFolderPath: resolves a shell folder without trusting caller-controlled environment text.
    std::wstring KnownFolderPath(const KNOWNFOLDERID& folderId);

    // AppendDetailPart: appends one diagnostics fragment to detailText using the shared separator.
    void AppendDetailPart(std::string& detailText, const std::string& partText);

    // JoinStrings: joins values with separator; used for multi-string registry data.
    std::string JoinStrings(const std::vector<std::string>& values, const std::string& separator);

    // FileExists: reports whether the path resolves to an existing file system object.
    bool FileExists(const std::string& pathText);

    // FormatBinaryText: renders REG_BINARY style payloads as a bounded hex preview.
    std::string FormatBinaryText(const std::vector<std::uint8_t>& rawBuffer);

    // ===================== Registry helpers =====================

    // RootKeyText: maps a supported hive handle to its HKLM/HKCU/HKCR display prefix.
    std::string RootKeyText(HKEY rootKey);

    // BuildRegistryLocationText: builds the exact "HKLM\Sub\Key" syntax the UI actions parse.
    std::string BuildRegistryLocationText(HKEY rootKey, const std::wstring& subKeyText);

    // EqualWideI: ordinal case-insensitive comparison used for locator equality, not display.
    bool EqualWideI(const std::wstring& left, const std::wstring& right);

    // RegistryWideStringFromBuffer: decodes REG_SZ/REG_EXPAND_SZ bytes without trusting termination.
    std::wstring RegistryWideStringFromBuffer(const std::vector<std::uint8_t>& rawBuffer);

    // RegistryDataToText: converts any common REG_* payload into compact UTF-8 display text.
    std::string RegistryDataToText(DWORD valueType, const std::vector<std::uint8_t>& rawBuffer);

    // QueryRegistryValueRecord: reads one named (or default) value; nullopt when absent or unreadable.
    std::optional<RegistryValueRecord> QueryRegistryValueRecord(
        HKEY rootKey,
        const std::wstring& subKeyText,
        const std::wstring& valueNameText);

    // EnumerateRegistryValues: lists every value under a key; unreadable keys simply yield no rows.
    std::vector<RegistryValueRecord> EnumerateRegistryValues(HKEY rootKey, const std::wstring& subKeyText);

    // EnumerateRegistrySubKeys: lists first-level subkey names under a key.
    std::vector<std::wstring> EnumerateRegistrySubKeys(HKEY rootKey, const std::wstring& subKeyText);

    // ===================== COM helpers =====================

    // IsClsidText: relaxed "{GUID}" shape test shared by every COM-backed persistence family.
    bool IsClsidText(const std::string& text);

    // QueryClsidFriendlyName: reads the HKCR CLSID default value, or empty when unnamed.
    std::string QueryClsidFriendlyName(const std::string& clsidText);

    // QueryClsidServerPath: resolves InprocServer32/LocalServer32 into a normalized image path.
    std::string QueryClsidServerPath(const std::string& clsidText);

    // ===================== StartupEntry configuration =====================

    // MarkEntryActionUnavailable: clears every action locator so synthetic rows cannot be mutated.
    void MarkEntryActionUnavailable(
        StartupEntry& entry,
        StartupRiskLevel riskLevel,
        const std::string& reasonCode,
        const std::string& reasonText);

    // ConfigureRegistryValueAction: wires the reversible "disable this registry value" action.
    void ConfigureRegistryValueAction(
        StartupEntry& entry,
        HKEY rootKey,
        const std::wstring& subKeyText,
        const RegistryValueRecord& valueRecord,
        StartupRiskLevel riskLevel,
        const std::string& reasonCode,
        const std::string& reasonText);

    // ConfigureRegistryTreeDeletion: wires the "delete the whole subkey" action with a stale-check snapshot.
    void ConfigureRegistryTreeDeletion(
        StartupEntry& entry,
        HKEY rootKey,
        const std::wstring& subKeyText);

    // FinalizeRegistryEntry: fills command text, resolved image path, publisher and deletion metadata.
    void FinalizeRegistryEntry(
        StartupEntry& entry,
        const std::string& rawCommandText,
        const std::string& fallbackClsidText,
        const std::string& registryValueNameText,
        bool deleteRegistryTree,
        bool resolveClsidFromValueData);
}

