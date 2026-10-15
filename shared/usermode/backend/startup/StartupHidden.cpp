// ============================================================
// ksword/startup/startup_hidden.cpp
// Namespace: ks::startup (public entry point) plus one file-local helper namespace.
// Purpose:
// - Surface autostart persistence that the ordinary enumerators structurally cannot see.
// - Every finding in this file is produced by comparing two views of the same object:
//     * the native NT registry view against the Win32 registry view (NUL-masked names),
//     * the registry service database against the SCM service list (unregistered services),
//     * the Task Scheduler registry cache against the Task Scheduler COM API (ghost tasks),
//     * the configured Startup folder against the shell default (folder redirection),
//     * per-user HKCU class registrations against machine HKLM ones (COM hijacking).
// - Findings are reported, never silently mutated: the hidden object is by definition not
//   something the normal action locators can revalidate, so actions stay disabled.
// ============================================================

#include "StartupInternal.h"

#include "StartupText.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <sddl.h>      // ConvertSidToStringSidW: names the trustee of a deny ACE.
#include <taskschd.h>  // ITaskService: the API view used to detect ghost scheduled tasks.
#include <winsvc.h>    // EnumServicesStatusExW: the SCM view used to detect hidden services.
#include <winternl.h>  // UNICODE_STRING / OBJECT_ATTRIBUTES / NTSTATUS for the native registry view.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "taskschd.lib")

namespace
{
    // The shared registry/entry plumbing lives in ks::r3::startup::detail; use it unqualified here
    // so this file reads like the other enumerator blocks in startup.cpp.
    using namespace ks::r3::startup::detail;

    // ===================== Native (NT) registry view =====================

    // kStatusNoMoreEntries / kStatusBufferTooSmall / kStatusBufferOverflow are the only NTSTATUS
    // values these loops branch on; everything else aborts the current enumeration.
    constexpr NTSTATUS kStatusNoMoreEntries = static_cast<NTSTATUS>(0x8000001AL);
    constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
    constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);

    // kSweepKeyBudget bounds the recursive native sweep: a crafted key tree must not hang the scan.
    constexpr std::size_t kSweepKeyBudget = 40000;

    // kEvidencePreviewLimit keeps evidence strings readable when a masked name is deliberately long.
    constexpr std::size_t kEvidencePreviewLimit = 200;

    // KsKeyInformationClass mirrors the subset of KEY_INFORMATION_CLASS this file needs.
    // A private enum avoids clashing with any future SDK definition in this translation unit.
    enum KsKeyInformationClass : int
    {
        KsKeyBasicInformation = 0
    };

    // KsKeyValueInformationClass mirrors the subset of KEY_VALUE_INFORMATION_CLASS this file needs.
    enum KsKeyValueInformationClass : int
    {
        KsKeyValueFullInformation = 1
    };

    // KsKeyBasicInformationData is the NtEnumerateKey KeyBasicInformation layout.
    // Name is a counted (not NUL-terminated) buffer, which is exactly why it can carry NUL bytes.
    struct KsKeyBasicInformationData
    {
        LARGE_INTEGER LastWriteTime;
        ULONG TitleIndex;
        ULONG NameLength;
        WCHAR Name[1];
    };

    // KsKeyValueFullInformationData is the NtEnumerateValueKey KeyValueFullInformation layout.
    // DataOffset is relative to the start of this structure.
    struct KsKeyValueFullInformationData
    {
        ULONG TitleIndex;
        ULONG Type;
        ULONG DataOffset;
        ULONG DataLength;
        ULONG NameLength;
        WCHAR Name[1];
    };

    using NtEnumerateKeyProc = NTSTATUS(NTAPI*)(HANDLE, ULONG, KsKeyInformationClass, PVOID, ULONG, PULONG);
    using NtEnumerateValueKeyProc = NTSTATUS(NTAPI*)(HANDLE, ULONG, KsKeyValueInformationClass, PVOID, ULONG, PULONG);
    using NtOpenKeyProc = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using NtCloseProc = NTSTATUS(NTAPI*)(HANDLE);

    // NativeRegistryApi holds the ntdll entry points that expose the unfiltered registry view.
    struct NativeRegistryApi
    {
        NtEnumerateKeyProc enumerateKey = nullptr;
        NtEnumerateValueKeyProc enumerateValueKey = nullptr;
        NtOpenKeyProc openKey = nullptr;
        NtCloseProc closeHandle = nullptr;
        bool available = false;
    };

    // NativeApi resolves ntdll exports once; ntdll is always loaded, so no LoadLibrary is needed.
    const NativeRegistryApi& NativeApi()
    {
        static const NativeRegistryApi api = []() -> NativeRegistryApi {
            NativeRegistryApi resolved;
            const HMODULE ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
            if (ntdllModule == nullptr)
            {
                return resolved;
            }
            resolved.enumerateKey = reinterpret_cast<NtEnumerateKeyProc>(
                reinterpret_cast<void*>(::GetProcAddress(ntdllModule, "NtEnumerateKey")));
            resolved.enumerateValueKey = reinterpret_cast<NtEnumerateValueKeyProc>(
                reinterpret_cast<void*>(::GetProcAddress(ntdllModule, "NtEnumerateValueKey")));
            resolved.openKey = reinterpret_cast<NtOpenKeyProc>(
                reinterpret_cast<void*>(::GetProcAddress(ntdllModule, "NtOpenKey")));
            resolved.closeHandle = reinterpret_cast<NtCloseProc>(
                reinterpret_cast<void*>(::GetProcAddress(ntdllModule, "NtClose")));
            resolved.available = resolved.enumerateKey != nullptr
                && resolved.enumerateValueKey != nullptr
                && resolved.openKey != nullptr
                && resolved.closeHandle != nullptr;
            return resolved;
        }();
        return api;
    }

    // IsNtSuccess keeps the NTSTATUS sign test in one place instead of repeating the macro shape.
    bool IsNtSuccess(const NTSTATUS status)
    {
        return status >= 0;
    }

    // NativeValueRecord is one value as the kernel sees it, including a name that may embed NUL.
    struct NativeValueRecord
    {
        std::wstring nameText;                 // Full counted name; may contain NUL or control characters.
        DWORD valueType = REG_NONE;            // Original REG_* type.
        std::vector<std::uint8_t> rawData;     // Raw payload bytes for display conversion.
    };

    // NativeSubKeyNames lists subkey names through NtEnumerateKey so masked names survive intact.
    // Returns an empty list when the native API is unavailable or the handle cannot be enumerated.
    std::vector<std::wstring> NativeSubKeyNames(const HANDLE keyHandle)
    {
        std::vector<std::wstring> names;
        const NativeRegistryApi& api = NativeApi();
        if (!api.available || keyHandle == nullptr)
        {
            return names;
        }
        // 2 KiB covers every realistic key name; the loop grows the buffer when the kernel asks.
        std::vector<std::uint8_t> buffer(2048);
        for (ULONG index = 0; index < 65536; ++index)
        {
            ULONG resultLength = 0;
            NTSTATUS status = api.enumerateKey(
                keyHandle,
                index,
                KsKeyBasicInformation,
                buffer.data(),
                static_cast<ULONG>(buffer.size()),
                &resultLength);
            if (status == kStatusBufferTooSmall || status == kStatusBufferOverflow)
            {
                buffer.assign(static_cast<std::size_t>(resultLength) + 64, 0);
                status = api.enumerateKey(
                    keyHandle,
                    index,
                    KsKeyBasicInformation,
                    buffer.data(),
                    static_cast<ULONG>(buffer.size()),
                    &resultLength);
            }
            if (!IsNtSuccess(status))
            {
                // STATUS_NO_MORE_ENTRIES ends the walk; any other failure aborts it to avoid spinning.
                break;
            }
            const auto* information = reinterpret_cast<const KsKeyBasicInformationData*>(buffer.data());
            const std::size_t nameChars = static_cast<std::size_t>(information->NameLength) / sizeof(wchar_t);
            names.emplace_back(information->Name, nameChars);
        }
        return names;
    }

    // NativeValueRecords lists values through NtEnumerateValueKey, preserving masked names and raw data.
    std::vector<NativeValueRecord> NativeValueRecords(const HANDLE keyHandle)
    {
        std::vector<NativeValueRecord> records;
        const NativeRegistryApi& api = NativeApi();
        if (!api.available || keyHandle == nullptr)
        {
            return records;
        }
        // 4 KiB is enough for most values; oversized payloads trigger one resize per value.
        std::vector<std::uint8_t> buffer(4096);
        for (ULONG index = 0; index < 65536; ++index)
        {
            ULONG resultLength = 0;
            NTSTATUS status = api.enumerateValueKey(
                keyHandle,
                index,
                KsKeyValueFullInformation,
                buffer.data(),
                static_cast<ULONG>(buffer.size()),
                &resultLength);
            if (status == kStatusBufferTooSmall || status == kStatusBufferOverflow)
            {
                buffer.assign(static_cast<std::size_t>(resultLength) + 64, 0);
                status = api.enumerateValueKey(
                    keyHandle,
                    index,
                    KsKeyValueFullInformation,
                    buffer.data(),
                    static_cast<ULONG>(buffer.size()),
                    &resultLength);
            }
            if (!IsNtSuccess(status))
            {
                break;
            }
            const auto* information = reinterpret_cast<const KsKeyValueFullInformationData*>(buffer.data());
            NativeValueRecord record;
            record.nameText.assign(
                information->Name,
                static_cast<std::size_t>(information->NameLength) / sizeof(wchar_t));
            record.valueType = information->Type;
            // DataOffset is measured from the structure start; clamp against the reported buffer size.
            const std::size_t dataOffset = static_cast<std::size_t>(information->DataOffset);
            const std::size_t dataLength = static_cast<std::size_t>(information->DataLength);
            if (dataOffset <= buffer.size() && dataLength <= buffer.size() - dataOffset)
            {
                record.rawData.assign(
                    buffer.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                    buffer.begin() + static_cast<std::ptrdiff_t>(dataOffset + dataLength));
            }
            records.push_back(std::move(record));
        }
        return records;
    }

    // OpenNativeChildKey opens a subkey whose name the Win32 API cannot express (embedded NUL).
    // parentHandle stays owned by the caller; the returned handle must be closed with NtClose.
    HANDLE OpenNativeChildKey(const HANDLE parentHandle, const std::wstring& childNameText)
    {
        const NativeRegistryApi& api = NativeApi();
        if (!api.available || parentHandle == nullptr || childNameText.empty())
        {
            return nullptr;
        }
        // UNICODE_STRING is length-counted, which is the whole reason a NUL-bearing name works here.
        UNICODE_STRING objectName{};
        objectName.Buffer = const_cast<PWSTR>(childNameText.c_str());
        objectName.Length = static_cast<USHORT>(childNameText.size() * sizeof(wchar_t));
        objectName.MaximumLength = objectName.Length;
        OBJECT_ATTRIBUTES objectAttributes{};
        InitializeObjectAttributes(&objectAttributes, &objectName, OBJ_CASE_INSENSITIVE, parentHandle, nullptr);
        HANDLE childHandle = nullptr;
        const NTSTATUS status = api.openKey(&childHandle, KEY_READ, &objectAttributes);
        return IsNtSuccess(status) ? childHandle : nullptr;
    }

    // CloseNativeKey releases a handle produced by OpenNativeChildKey.
    void CloseNativeKey(const HANDLE keyHandle)
    {
        const NativeRegistryApi& api = NativeApi();
        if (api.available && keyHandle != nullptr)
        {
            api.closeHandle(keyHandle);
        }
    }

    // ===================== Masked-name analysis =====================

    // NameMaskKind classifies why a registry name cannot be reached through the Win32 API.
    enum class NameMaskKind : int
    {
        None = 0,        // Ordinary name; Win32 sees exactly what the kernel stores.
        EmbeddedNull,    // Name contains U+0000: Win32 truncates it, so query/delete by name fails.
        ControlCharacter,// Name contains other C0 control characters that break display and matching.
        Win32Invisible   // Name is well formed yet absent from the Win32 enumeration result.
    };

    // ClassifyNativeName reports whether a kernel-visible name is reachable through Win32.
    // win32NameSet holds the lowercased names the Win32 enumeration returned for the same key,
    // plus whether that enumeration finished; an aborted walk suppresses the weakest verdict.
    NameMaskKind ClassifyNativeName(
        const std::wstring& nativeNameText,
        const std::unordered_set<std::wstring>& win32NameSet,
        const bool win32EnumerationComplete)
    {
        if (nativeNameText.find(L'\0') != std::wstring::npos)
        {
            return NameMaskKind::EmbeddedNull;
        }
        for (const wchar_t nameChar : nativeNameText)
        {
            if (nameChar < 0x20)
            {
                return NameMaskKind::ControlCharacter;
            }
        }
        if (win32EnumerationComplete
            && !nativeNameText.empty()
            && win32NameSet.find(LowerWideCopy(nativeNameText)) == win32NameSet.end())
        {
            return NameMaskKind::Win32Invisible;
        }
        return NameMaskKind::None;
    }

    // MaskKindText renders the classification as the Chinese evidence text shown in the table.
    std::string MaskKindText(const NameMaskKind kind)
    {
        switch (kind)
        {
        case NameMaskKind::EmbeddedNull:
            return FromWide(L"名称内嵌 NUL 字符：注册表编辑器与 Win32 API 只能看到截断后的名称，按名查询/删除都会失败");
        case NameMaskKind::ControlCharacter:
            return FromWide(L"名称包含控制字符：常规工具显示与匹配都会失真");
        case NameMaskKind::Win32Invisible:
            return FromWide(L"内核枚举可见但 Win32 枚举不返回该名称");
        case NameMaskKind::None:
            break;
        }
        return std::string();
    }

    // EscapeNameForDisplay renders a masked name unambiguously by escaping every unsafe code unit.
    std::string EscapeNameForDisplay(const std::wstring& nameText)
    {
        std::wstring escaped;
        escaped.reserve(nameText.size() + 8);
        for (const wchar_t nameChar : nameText)
        {
            if (nameChar == L'\0')
            {
                escaped += L"\\0";
                continue;
            }
            if (nameChar < 0x20)
            {
                // Render remaining control characters as \xNN so the evidence stays copy-pasteable.
                const wchar_t hexDigits[] = L"0123456789ABCDEF";
                escaped += L"\\x";
                escaped += hexDigits[(nameChar >> 4) & 0x0F];
                escaped += hexDigits[nameChar & 0x0F];
                continue;
            }
            escaped.push_back(nameChar);
        }
        if (escaped.size() > kEvidencePreviewLimit)
        {
            escaped.resize(kEvidencePreviewLimit);
            escaped += L"...";
        }
        return FromWide(escaped);
    }

    // ===================== Shared entry construction =====================

    // HiddenFinding carries everything one Hidden-category row needs before it becomes a StartupEntry.
    struct HiddenFinding
    {
        std::string uniqueSuffixText;  // Appended to the HIDDEN| prefix to build a stable identity.
        std::string sourceTypeText;    // Stable machine-readable subtype, e.g. Hidden-MaskedRegistryValue.
        std::string itemNameText;      // Display name of the hidden object.
        std::string commandText;       // Payload: command line, image path or raw data preview.
        std::string imagePathText;     // Resolved image path when one can be inferred.
        std::string locationText;      // Registry key, file path or SCM name.
        std::string locationGroupText; // Grouping location used by the registry tree view.
        std::string userText;          // Scope text: 本机 / 当前用户 / 服务账户 …
        std::string detailText;        // Evidence: which two views disagreed and how.
        std::string riskReasonCode;    // Stable i18n code for the warning banner.
        std::string riskReasonText;    // Human readable reason the row cannot be acted on.
        bool canOpenRegistryLocation = false;
    };

    // AppendHiddenEntry converts one finding into the unified backend record.
    // Hidden rows are report-only: the object is by definition not addressable through the normal
    // locators, so every action stays disabled and the risk level is always Critical.
    void AppendHiddenEntry(std::vector<ks::r3::startup::StartupEntry>& entries, HiddenFinding finding)
    {
        ks::r3::startup::StartupEntry entry;
        entry.category = ks::r3::startup::StartupCategory::Hidden;
        entry.categoryText = ks::r3::startup::CategoryToText(entry.category);
        entry.itemNameText = std::move(finding.itemNameText);
        entry.commandText = std::move(finding.commandText);
        entry.imagePathText = std::move(finding.imagePathText);
        entry.locationText = std::move(finding.locationText);
        entry.locationGroupText = finding.locationGroupText.empty()
            ? entry.locationText
            : std::move(finding.locationGroupText);
        entry.userText = std::move(finding.userText);
        entry.detailText = std::move(finding.detailText);
        entry.sourceTypeText = std::move(finding.sourceTypeText);
        entry.uniqueIdText = "HIDDEN|" + finding.uniqueSuffixText;
        entry.enabled = true;
        // Publisher and existence are resolved for whatever image path could be inferred, so an
        // unsigned binary in a hidden slot is visible without opening the detail dialog.
        if (!ks::r3::startup::detail::TrimCopy(entry.imagePathText).empty())
        {
            entry.publisherText = ks::r3::startup::QueryPublisherTextByPath(entry.imagePathText);
            entry.imagePathExists = FileExists(entry.imagePathText);
            entry.canOpenFileLocation = entry.imagePathExists;
        }
        MarkEntryActionUnavailable(
            entry,
            ks::r3::startup::StartupRiskLevel::Critical,
            std::move(finding.riskReasonCode),
            std::move(finding.riskReasonText));
        entry.canOpenRegistryLocation = finding.canOpenRegistryLocation;
        entries.push_back(std::move(entry));
    }

    // ===================== 1) Native vs Win32 registry sweep =====================

    // SweepRootSpec describes one persistence subtree that gets compared kernel-view vs Win32-view.
    // maxDepth 0 inspects only the root key itself; 1 also inspects its immediate children, and so on.
    struct SweepRootSpec
    {
        HKEY rootKey = nullptr;
        const wchar_t* subKeyText = L"";
        int maxDepth = 0;
        const wchar_t* userText = L"";
        const wchar_t* detailText = L"";
    };

    // BuildSweepRootList centralizes which persistence subtrees are worth a full native comparison.
    // The list intentionally covers the load points an attacker would hide something in, not the
    // whole hive: a full-registry native walk would cost minutes and drown the result table.
    const std::vector<SweepRootSpec>& BuildSweepRootList()
    {
        static const std::vector<SweepRootSpec> specs{
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 1, L"当前用户", L"用户登录运行键" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 1, L"当前用户", L"用户一次性运行键" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", 2, L"当前用户", L"用户 RunOnceEx" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", 1, L"当前用户", L"用户策略运行键" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, L"当前用户", L"用户 Windows Load/Run" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 1, L"当前用户", L"用户 Winlogon" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Command Processor", 0, L"当前用户", L"命令处理器 Autorun" },
            { HKEY_CURRENT_USER, L"Environment", 0, L"当前用户", L"用户环境变量（UserInitMprLogonScript 等）" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", 0, L"当前用户", L"用户外壳文件夹重定向" },
            { HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", 0, L"当前用户", L"用户级 COM 类注册根" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 1, L"本机", L"系统登录运行键" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 1, L"本机", L"系统一次性运行键" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", 2, L"本机", L"系统 RunOnceEx" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", 1, L"本机", L"系统策略运行键" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", 1, L"本机(32位)", L"32 位视图运行键" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 1, L"本机(32位)", L"32 位视图一次性运行键" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 2, L"本机", L"Winlogon 及其 Notify 包" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, L"本机", L"AppInit_DLLs 所在键" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", 1, L"本机", L"映像劫持 IFEO" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\SilentProcessExit", 1, L"本机", L"静默退出监视" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Active Setup\\Installed Components", 1, L"本机", L"Active Setup" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", 0, L"本机", L"公共外壳文件夹重定向" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager", 1, L"本机", L"会话管理器" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", 1, L"本机", L"LSA 包" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\SafeBoot", 1, L"本机", L"安全模式配置" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services", 1, L"本机", L"服务与驱动定义" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tasks", 1, L"本机", L"计划任务缓存" }
        };
        return specs;
    }

    // SweepContext threads the shared budget and output list through the recursive walk.
    struct SweepContext
    {
        std::vector<ks::r3::startup::StartupEntry>* entries = nullptr;
        std::size_t visitedKeyCount = 0;
        bool budgetExhausted = false;
    };

    // Win32NameSet is a Win32 enumeration result plus whether that enumeration ran to completion.
    // The completeness flag matters: an aborted Win32 walk would make every remaining native name
    // look "invisible", so the comparison must be suppressed instead of producing false findings.
    struct Win32NameSet
    {
        std::unordered_set<std::wstring> names;
        bool complete = false;
    };

    // Win32ValueNameSet returns the lowercased value names the Win32 API reports for an open key.
    // The buffer is sized for the 16383-character registry limit so a long name is not misread as hidden.
    Win32NameSet Win32ValueNameSet(const HKEY keyHandle)
    {
        Win32NameSet result;
        std::vector<wchar_t> valueName(16400, L'\0');
        for (DWORD valueIndex = 0;; ++valueIndex)
        {
            DWORD valueNameChars = static_cast<DWORD>(valueName.size());
            const LONG enumResult = ::RegEnumValueW(
                keyHandle, valueIndex, valueName.data(), &valueNameChars, nullptr, nullptr, nullptr, nullptr);
            if (enumResult == ERROR_NO_MORE_ITEMS)
            {
                result.complete = true;
                break;
            }
            if (enumResult == ERROR_MORE_DATA)
            {
                // Only the data was too large; the name is still valid, so keep walking.
                result.names.insert(LowerWideCopy(std::wstring(valueName.data(), valueNameChars)));
                continue;
            }
            if (enumResult != ERROR_SUCCESS)
            {
                break;
            }
            result.names.insert(LowerWideCopy(std::wstring(valueName.data(), valueNameChars)));
        }
        return result;
    }

    // Win32SubKeyNameSet returns the lowercased subkey names the Win32 API reports for an open key.
    Win32NameSet Win32SubKeyNameSet(const HKEY keyHandle)
    {
        Win32NameSet result;
        std::vector<wchar_t> subKeyName(1024, L'\0');
        for (DWORD subKeyIndex = 0;; ++subKeyIndex)
        {
            DWORD subKeyChars = static_cast<DWORD>(subKeyName.size());
            const LONG enumResult = ::RegEnumKeyExW(
                keyHandle, subKeyIndex, subKeyName.data(), &subKeyChars, nullptr, nullptr, nullptr, nullptr);
            if (enumResult == ERROR_NO_MORE_ITEMS)
            {
                result.complete = true;
                break;
            }
            if (enumResult != ERROR_SUCCESS)
            {
                break;
            }
            result.names.insert(LowerWideCopy(std::wstring(subKeyName.data(), subKeyChars)));
        }
        return result;
    }

    // DescribeMaskedChildKey lists the values inside a masked subkey so the payload is visible.
    // The masked key can only be opened natively, which is exactly what makes it evidence.
    std::string DescribeMaskedChildKey(const HANDLE parentHandle, const std::wstring& childNameText)
    {
        const HANDLE childHandle = OpenNativeChildKey(parentHandle, childNameText);
        if (childHandle == nullptr)
        {
            return FromWide(L"（无法打开该隐藏子键）");
        }
        std::vector<std::string> parts;
        for (const NativeValueRecord& record : NativeValueRecords(childHandle))
        {
            const std::string nameText = record.nameText.empty()
                ? FromWide(L"(默认)")
                : EscapeNameForDisplay(record.nameText);
            parts.push_back(nameText + "=" + RegistryDataToText(record.valueType, record.rawData));
            if (parts.size() >= 8)
            {
                parts.push_back("...");
                break;
            }
        }
        CloseNativeKey(childHandle);
        return parts.empty() ? FromWide(L"（隐藏子键为空）") : JoinStrings(parts, " | ");
    }

    // SweepKeyForMaskedNames compares one key's kernel view against its Win32 view and recurses.
    // rootKey/subKeyText identify the key for display; depthRemaining bounds the recursion.
    void SweepKeyForMaskedNames(
        SweepContext& context,
        const HKEY rootKey,
        const std::wstring& subKeyText,
        const int depthRemaining,
        const SweepRootSpec& spec)
    {
        if (context.visitedKeyCount >= kSweepKeyBudget)
        {
            context.budgetExhausted = true;
            return;
        }
        ++context.visitedKeyCount;

        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(rootKey, subKeyText.c_str(), 0, KEY_READ, &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            // Access problems are reported by the dedicated ACL scanner, not here.
            return;
        }

        const std::string locationText = BuildRegistryLocationText(rootKey, subKeyText);
        const Win32NameSet win32Values = Win32ValueNameSet(openedKey);
        const Win32NameSet win32SubKeys = Win32SubKeyNameSet(openedKey);

        // Values first: a masked value under an ordinary Run key is the cheapest hiding trick there is.
        for (const NativeValueRecord& record : NativeValueRecords(openedKey))
        {
            const NameMaskKind maskKind = ClassifyNativeName(record.nameText, win32Values.names, win32Values.complete);
            if (maskKind == NameMaskKind::None)
            {
                continue;
            }
            const std::string dataText = RegistryDataToText(record.valueType, record.rawData);
            HiddenFinding finding;
            finding.uniqueSuffixText = "MASKEDVALUE|" + locationText + "|" + EscapeNameForDisplay(record.nameText);
            finding.sourceTypeText = "Hidden-MaskedRegistryValue";
            finding.itemNameText = EscapeNameForDisplay(record.nameText);
            finding.commandText = dataText;
            finding.imagePathText = ks::r3::startup::NormalizeFilePathText(dataText);
            finding.locationText = locationText;
            finding.locationGroupText = BuildRegistryLocationText(rootKey, std::wstring(spec.subKeyText));
            finding.userText = FromWide(spec.userText);
            finding.detailText = FromWide(spec.detailText) + FromWide(L"；") + MaskKindText(maskKind);
            finding.riskReasonCode = "hidden_registry_value";
            finding.riskReasonText = FromWide(
                L"该值只能通过内核接口看到，常规注册表 API 无法按名定位；KSword 只报告不修改，"
                L"请用注册表页的原生视图或离线工具处理。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(*context.entries, std::move(finding));
        }

        // Subkeys second: a masked subkey can hold an entire RunOnceEx-style command group.
        const std::vector<std::wstring> nativeSubKeys = NativeSubKeyNames(openedKey);
        for (const std::wstring& nativeName : nativeSubKeys)
        {
            const NameMaskKind maskKind = ClassifyNativeName(nativeName, win32SubKeys.names, win32SubKeys.complete);
            if (maskKind == NameMaskKind::None)
            {
                continue;
            }
            HiddenFinding finding;
            finding.uniqueSuffixText = "MASKEDKEY|" + locationText + "|" + EscapeNameForDisplay(nativeName);
            finding.sourceTypeText = "Hidden-MaskedRegistryKey";
            finding.itemNameText = EscapeNameForDisplay(nativeName);
            finding.commandText = DescribeMaskedChildKey(openedKey, nativeName);
            finding.locationText = locationText;
            finding.locationGroupText = BuildRegistryLocationText(rootKey, std::wstring(spec.subKeyText));
            finding.userText = FromWide(spec.userText);
            finding.detailText = FromWide(spec.detailText) + FromWide(L"；") + MaskKindText(maskKind);
            finding.riskReasonCode = "hidden_registry_key";
            finding.riskReasonText = FromWide(
                L"该子键只能通过内核接口枚举，注册表编辑器通常会在打开父键时报错或直接跳过；"
                L"KSword 只报告不删除。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(*context.entries, std::move(finding));
        }

        ::RegCloseKey(openedKey);

        if (depthRemaining <= 0)
        {
            return;
        }
        // Recursion uses the Win32 names only: a masked child cannot be reopened by path anyway,
        // and its contents were already captured inline above.
        for (const std::wstring& childName : EnumerateRegistrySubKeys(rootKey, subKeyText))
        {
            if (context.visitedKeyCount >= kSweepKeyBudget)
            {
                context.budgetExhausted = true;
                return;
            }
            SweepKeyForMaskedNames(context, rootKey, subKeyText + L"\\" + childName, depthRemaining - 1, spec);
        }
    }

    // AppendMaskedRegistryEntries runs the native-vs-Win32 comparison over every sweep root.
    void AppendMaskedRegistryEntries(std::vector<ks::r3::startup::StartupEntry>& entries)
    {
        if (!NativeApi().available)
        {
            // Without ntdll exports there is no second view to compare against; say so explicitly
            // instead of silently reporting "no hidden items found".
            HiddenFinding finding;
            finding.uniqueSuffixText = "NATIVEAPI|Unavailable";
            finding.sourceTypeText = "Hidden-ScannerUnavailable";
            finding.itemNameText = FromWide(L"原生注册表视图不可用");
            finding.locationText = "ntdll.dll";
            finding.userText = FromWide(L"本机");
            finding.detailText = FromWide(L"未能解析 NtEnumerateKey/NtEnumerateValueKey/NtOpenKey；内嵌 NUL 名称的隐藏项无法检测。");
            finding.riskReasonCode = "hidden_scanner_unavailable";
            finding.riskReasonText = FromWide(L"这是扫描能力缺失的诊断记录，不是启动项本身。");
            AppendHiddenEntry(entries, std::move(finding));
            return;
        }

        SweepContext context;
        context.entries = &entries;
        for (const SweepRootSpec& spec : BuildSweepRootList())
        {
            SweepKeyForMaskedNames(context, spec.rootKey, std::wstring(spec.subKeyText), spec.maxDepth, spec);
        }
        if (context.budgetExhausted)
        {
            // A truncated sweep must never look like a clean sweep.
            HiddenFinding finding;
            finding.uniqueSuffixText = "SWEEP|BudgetExhausted";
            finding.sourceTypeText = "Hidden-ScannerTruncated";
            finding.itemNameText = FromWide(L"隐藏项扫描已达键数上限");
            finding.locationText = FromWide(L"注册表原生对比扫描");
            finding.userText = FromWide(L"本机");
            finding.detailText = FromWide(L"已检查 ") + std::to_string(context.visitedKeyCount)
                + FromWide(L" 个键后停止，部分持久化子树未完成对比。");
            finding.riskReasonCode = "hidden_scanner_truncated";
            finding.riskReasonText = FromWide(L"这是扫描覆盖度的诊断记录，不是启动项本身。");
            AppendHiddenEntry(entries, std::move(finding));
        }
    }

    // ===================== 2) ACL-blocked persistence keys =====================

    // TrusteeTextFromSid renders an ACE trustee as an account name, falling back to the SID string.
    std::string TrusteeTextFromSid(PSID sidValue)
    {
        if (sidValue == nullptr || ::IsValidSid(sidValue) == FALSE)
        {
            return FromWide(L"(无效 SID)");
        }
        std::array<wchar_t, 256> accountName{};
        std::array<wchar_t, 256> domainName{};
        DWORD accountChars = static_cast<DWORD>(accountName.size());
        DWORD domainChars = static_cast<DWORD>(domainName.size());
        SID_NAME_USE nameUse = SidTypeUnknown;
        if (::LookupAccountSidW(nullptr, sidValue, accountName.data(), &accountChars, domainName.data(), &domainChars, &nameUse) != FALSE)
        {
            const std::wstring domainText(domainName.data());
            const std::wstring accountText(accountName.data());
            return FromWide(domainText.empty() ? accountText : domainText + L"\\" + accountText);
        }
        LPWSTR sidText = nullptr;
        if (::ConvertSidToStringSidW(sidValue, &sidText) != FALSE && sidText != nullptr)
        {
            const std::string result = FromWide(sidText);
            ::LocalFree(sidText);
            return result;
        }
        return FromWide(L"(未知账户)");
    }

    // DescribeDenyAces returns a description of every ACCESS_DENIED ACE on a key, or empty when none.
    // A deny ACE on a persistence key is the standard way to hide a payload from an administrator.
    std::string DescribeDenyAces(const HKEY rootKey, const std::wstring& subKeyText)
    {
        HKEY openedKey = nullptr;
        if (::RegOpenKeyExW(rootKey, subKeyText.c_str(), 0, READ_CONTROL, &openedKey) != ERROR_SUCCESS)
        {
            return std::string();
        }
        DWORD descriptorBytes = 0;
        ::RegGetKeySecurity(openedKey, DACL_SECURITY_INFORMATION, nullptr, &descriptorBytes);
        if (descriptorBytes == 0)
        {
            ::RegCloseKey(openedKey);
            return std::string();
        }
        std::vector<std::uint8_t> descriptorBuffer(descriptorBytes);
        const LONG securityResult = ::RegGetKeySecurity(
            openedKey,
            DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptorBuffer.data()),
            &descriptorBytes);
        ::RegCloseKey(openedKey);
        if (securityResult != ERROR_SUCCESS)
        {
            return std::string();
        }
        BOOL daclPresent = FALSE;
        BOOL daclDefaulted = FALSE;
        PACL daclPointer = nullptr;
        if (::GetSecurityDescriptorDacl(
                reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptorBuffer.data()),
                &daclPresent,
                &daclPointer,
                &daclDefaulted) == FALSE
            || daclPresent == FALSE
            || daclPointer == nullptr)
        {
            return std::string();
        }
        std::vector<std::string> denyParts;
        for (WORD aceIndex = 0; aceIndex < daclPointer->AceCount; ++aceIndex)
        {
            LPVOID acePointer = nullptr;
            if (::GetAce(daclPointer, aceIndex, &acePointer) == FALSE || acePointer == nullptr)
            {
                continue;
            }
            const auto* aceHeader = static_cast<const ACE_HEADER*>(acePointer);
            if (aceHeader->AceType != ACCESS_DENIED_ACE_TYPE)
            {
                continue;
            }
            // ACCESS_DENIED_ACE stores the trustee SID inline right after the access mask.
            const auto* deniedAce = static_cast<const ACCESS_DENIED_ACE*>(acePointer);
            PSID trusteeSid = const_cast<PSID>(static_cast<const void*>(&deniedAce->SidStart));
            denyParts.push_back(TrusteeTextFromSid(trusteeSid));
        }
        return denyParts.empty() ? std::string() : JoinStrings(denyParts, ", ");
    }

    // AppendAclBlockedRegistryEntries reports persistence keys that refuse a plain read or carry deny ACEs.
    void AppendAclBlockedRegistryEntries(std::vector<ks::r3::startup::StartupEntry>& entries)
    {
        for (const SweepRootSpec& spec : BuildSweepRootList())
        {
            const std::wstring subKeyText(spec.subKeyText);
            HKEY openedKey = nullptr;
            const LONG openResult = ::RegOpenKeyExW(spec.rootKey, subKeyText.c_str(), 0, KEY_READ, &openedKey);
            const std::string locationText = BuildRegistryLocationText(spec.rootKey, subKeyText);
            if (openResult == ERROR_ACCESS_DENIED)
            {
                // The key exists (otherwise the error would be FILE_NOT_FOUND) but denies reading.
                HiddenFinding finding;
                finding.uniqueSuffixText = "ACLDENIED|" + locationText;
                finding.sourceTypeText = "Hidden-AclBlockedKey";
                finding.itemNameText = FromWide(L"读取被拒绝：") + FromWide(subKeyText);
                finding.locationText = locationText;
                finding.userText = FromWide(spec.userText);
                finding.detailText = FromWide(spec.detailText)
                    + FromWide(L"；当前进程无法读取该持久化键，内容对本次扫描完全不可见。");
                finding.riskReasonCode = "hidden_registry_acl";
                finding.riskReasonText = FromWide(
                    L"以管理员身份重新扫描；若仍被拒绝，说明该键的 DACL 被显式改写，属于典型的隐藏手法。");
                finding.canOpenRegistryLocation = true;
                AppendHiddenEntry(entries, std::move(finding));
                continue;
            }
            if (openResult != ERROR_SUCCESS)
            {
                continue;
            }
            ::RegCloseKey(openedKey);
            const std::string denyText = DescribeDenyAces(spec.rootKey, subKeyText);
            if (denyText.empty())
            {
                continue;
            }
            // A readable key can still carry deny ACEs aimed at other principals or other rights.
            HiddenFinding finding;
            finding.uniqueSuffixText = "ACLDENYACE|" + locationText;
            finding.sourceTypeText = "Hidden-DenyAceOnKey";
            finding.itemNameText = FromWide(L"存在拒绝 ACE：") + FromWide(subKeyText);
            finding.commandText = denyText;
            finding.locationText = locationText;
            finding.userText = FromWide(spec.userText);
            finding.detailText = FromWide(spec.detailText) + FromWide(L"；被拒绝的账户：") + denyText;
            finding.riskReasonCode = "hidden_registry_deny_ace";
            finding.riskReasonText = FromWide(
                L"Windows 默认不会在这些持久化键上放置拒绝 ACE；请确认该 ACE 的来源。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(entries, std::move(finding));
        }
    }

    // ===================== 3) Registry services the SCM does not list =====================

    // ScmServiceNameSet returns the lowercased names of every service and driver the SCM reports.
    // An empty set means the SCM could not be queried, and the caller must skip the comparison.
    std::unordered_set<std::wstring> ScmServiceNameSet(bool& queriedOk)
    {
        queriedOk = false;
        std::unordered_set<std::wstring> names;
        const SC_HANDLE managerHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (managerHandle == nullptr)
        {
            return names;
        }
        DWORD bytesNeeded = 0;
        DWORD servicesReturned = 0;
        DWORD resumeHandle = 0;
        std::vector<std::uint8_t> buffer(64 * 1024);
        while (true)
        {
            bytesNeeded = 0;
            servicesReturned = 0;
            const BOOL enumResult = ::EnumServicesStatusExW(
                managerHandle,
                SC_ENUM_PROCESS_INFO,
                SERVICE_WIN32 | SERVICE_DRIVER,
                SERVICE_STATE_ALL,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesNeeded,
                &servicesReturned,
                &resumeHandle,
                nullptr);
            const DWORD lastError = (enumResult == FALSE) ? ::GetLastError() : ERROR_SUCCESS;
            if (enumResult == FALSE && lastError != ERROR_MORE_DATA)
            {
                // 名单不完整就别声称查成功，让上层走兜底，否则残缺名单会把正常服务判成隐藏项。
                ::CloseServiceHandle(managerHandle);
                return names;
            }

            // 必须先消费再扩容：ERROR_MORE_DATA 只表示还没枚举完，SCM 已经往缓冲里写了
            // servicesReturned 条并把 resumeHandle 前移，先扩容重试会让这一批永远取不回来。
            const auto* statusArray = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
            for (DWORD serviceIndex = 0; serviceIndex < servicesReturned; ++serviceIndex)
            {
                if (statusArray[serviceIndex].lpServiceName != nullptr)
                {
                    names.insert(LowerWideCopy(statusArray[serviceIndex].lpServiceName));
                }
            }
            if (enumResult != FALSE)
            {
                break;
            }
            if (bytesNeeded > buffer.size())
            {
                // Grow to the size the SCM asked for and continue from the advanced resume position.
                buffer.assign(static_cast<std::size_t>(bytesNeeded) + 4096, 0);
            }
            else if (servicesReturned == 0)
            {
                // 既没进展也不要求扩容，再转下去就是死循环；当作查询失败交给兜底。
                ::CloseServiceHandle(managerHandle);
                return names;
            }
        }
        ::CloseServiceHandle(managerHandle);
        queriedOk = true;
        return names;
    }

    // IsRealServiceType keeps configuration-only subkeys out of the comparison.
    // Only the four types the SCM itself enumerates can be legitimately missing from its list.
    bool IsRealServiceType(const DWORD serviceType)
    {
        return serviceType == SERVICE_KERNEL_DRIVER
            || serviceType == SERVICE_FILE_SYSTEM_DRIVER
            || serviceType == SERVICE_WIN32_OWN_PROCESS
            || serviceType == SERVICE_WIN32_SHARE_PROCESS;
    }

    // AppendUnregisteredServiceEntries reports service definitions present in the registry but not in the SCM.
    // This is the classic "the rootkit wrote the key and unlinked the SCM record" shape.
    void AppendUnregisteredServiceEntries(std::vector<ks::r3::startup::StartupEntry>& entries)
    {
        bool scmQueriedOk = false;
        const std::unordered_set<std::wstring> scmNames = ScmServiceNameSet(scmQueriedOk);
        if (!scmQueriedOk)
        {
            // Without a trustworthy SCM list every registry key would look hidden; report the gap instead.
            HiddenFinding finding;
            finding.uniqueSuffixText = "SCM|EnumFailed";
            finding.sourceTypeText = "Hidden-ScannerUnavailable";
            finding.itemNameText = FromWide(L"SCM 服务列表不可用");
            finding.locationText = FromWide(L"服务控制管理器");
            finding.userText = FromWide(L"本机");
            finding.detailText = FromWide(L"EnumServicesStatusEx 失败，无法与注册表服务定义做交叉比对。");
            finding.riskReasonCode = "hidden_scanner_unavailable";
            finding.riskReasonText = FromWide(L"这是扫描能力缺失的诊断记录，不是启动项本身。");
            AppendHiddenEntry(entries, std::move(finding));
            return;
        }

        static const std::wstring servicesRoot = L"System\\CurrentControlSet\\Services";
        for (const std::wstring& serviceName : EnumerateRegistrySubKeys(HKEY_LOCAL_MACHINE, servicesRoot))
        {
            if (scmNames.find(LowerWideCopy(serviceName)) != scmNames.end())
            {
                continue;
            }
            const std::wstring serviceSubKey = servicesRoot + L"\\" + serviceName;
            const auto typeRecord = QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, serviceSubKey, L"Type");
            if (!typeRecord.has_value() || typeRecord->rawData.size() < sizeof(DWORD))
            {
                // Keys without a Type value are configuration containers, not service definitions.
                continue;
            }
            DWORD serviceType = 0;
            std::memcpy(&serviceType, typeRecord->rawData.data(), sizeof(DWORD));
            if (!IsRealServiceType(serviceType))
            {
                continue;
            }
            const auto imageRecord = QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, serviceSubKey, L"ImagePath");
            const auto startRecord = QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, serviceSubKey, L"Start");
            DWORD startType = SERVICE_DEMAND_START;
            if (startRecord.has_value() && startRecord->rawData.size() >= sizeof(DWORD))
            {
                std::memcpy(&startType, startRecord->rawData.data(), sizeof(DWORD));
            }
            const std::string imagePathText = imageRecord.has_value()
                ? ks::r3::startup::NormalizeFilePathText(imageRecord->valueDataText)
                : std::string();

            HiddenFinding finding;
            finding.uniqueSuffixText = "SCMMISSING|" + FromWide(serviceName);
            finding.sourceTypeText = "Hidden-UnregisteredService";
            finding.itemNameText = FromWide(serviceName);
            finding.commandText = imageRecord.has_value() ? imageRecord->valueDataText : std::string();
            finding.imagePathText = imagePathText;
            finding.locationText = BuildRegistryLocationText(HKEY_LOCAL_MACHINE, serviceSubKey);
            finding.locationGroupText = BuildRegistryLocationText(HKEY_LOCAL_MACHINE, servicesRoot);
            finding.userText = FromWide(L"本机");
            finding.detailText = FromWide(L"注册表存在服务定义（Type=") + std::to_string(serviceType)
                + FromWide(L"，Start=") + std::to_string(startType)
                + FromWide(L"），但服务控制管理器的枚举结果里没有它。");
            finding.riskReasonCode = "hidden_service";
            finding.riskReasonText = FromWide(
                L"SCM 未列出该服务，服务页也就看不到它；可能是被卸载残留的键，也可能是刻意脱管的加载点。"
                L"确认用途前不要直接删除，驱动类型的键删除后可能导致无法启动。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(entries, std::move(finding));
        }
    }

    // ===================== 4) Ghost scheduled tasks =====================

    // kTaskCacheRoot is where the Task Scheduler service keeps its authoritative task index.
    constexpr wchar_t kTaskCacheRoot[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache";

    // TaskTreeRecord is one leaf of TaskCache\Tree: the visible task path and the GUID it points at.
    struct TaskTreeRecord
    {
        std::wstring taskPathText;   // Full task path such as \Microsoft\Windows\Foo\Bar.
        std::wstring taskIdText;     // The {GUID} stored in the leaf's Id value, empty when absent.
        std::wstring treeSubKeyText; // The Tree leaf key itself; SD lives here, not under Tasks\{GUID}.
    };

    // CollectTaskTreeRecords walks TaskCache\Tree recursively and returns every leaf that carries an Id.
    // depthRemaining bounds a crafted deep tree; the real tree is at most a handful of levels.
    void CollectTaskTreeRecords(
        const std::wstring& registrySubKey,
        const std::wstring& taskPathText,
        const int depthRemaining,
        std::vector<TaskTreeRecord>& recordsOut)
    {
        if (depthRemaining < 0 || recordsOut.size() > 20000)
        {
            return;
        }
        const auto idRecord = QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, registrySubKey, L"Id");
        if (idRecord.has_value() && !ks::r3::startup::detail::TrimCopy(idRecord->valueDataText).empty())
        {
            TaskTreeRecord record;
            record.taskPathText = taskPathText;
            record.taskIdText = ToWide(ks::r3::startup::detail::TrimCopy(idRecord->valueDataText));
            record.treeSubKeyText = registrySubKey;
            recordsOut.push_back(std::move(record));
        }
        for (const std::wstring& childName : EnumerateRegistrySubKeys(HKEY_LOCAL_MACHINE, registrySubKey))
        {
            CollectTaskTreeRecords(
                registrySubKey + L"\\" + childName,
                taskPathText + L"\\" + childName,
                depthRemaining - 1,
                recordsOut);
        }
    }

    // ApiTaskPathSet returns the lowercased task paths the Task Scheduler API is willing to enumerate.
    // queriedOk stays false when COM or the service is unavailable, so the caller can skip comparing.
    void CollectApiTaskPaths(ITaskFolder* folderPointer, std::unordered_set<std::wstring>& pathsOut, int depthRemaining)
    {
        if (folderPointer == nullptr || depthRemaining < 0)
        {
            return;
        }
        IRegisteredTaskCollection* taskCollection = nullptr;
        if (SUCCEEDED(folderPointer->GetTasks(TASK_ENUM_HIDDEN, &taskCollection)) && taskCollection != nullptr)
        {
            LONG taskCount = 0;
            taskCollection->get_Count(&taskCount);
            for (LONG taskIndex = 1; taskIndex <= taskCount; ++taskIndex)
            {
                IRegisteredTask* registeredTask = nullptr;
                VARIANT indexVariant{};
                indexVariant.vt = VT_I4;
                indexVariant.lVal = taskIndex;
                if (SUCCEEDED(taskCollection->get_Item(indexVariant, &registeredTask)) && registeredTask != nullptr)
                {
                    BSTR pathText = nullptr;
                    if (SUCCEEDED(registeredTask->get_Path(&pathText)) && pathText != nullptr)
                    {
                        pathsOut.insert(LowerWideCopy(std::wstring(pathText)));
                        ::SysFreeString(pathText);
                    }
                    registeredTask->Release();
                }
            }
            taskCollection->Release();
        }
        ITaskFolderCollection* folderCollection = nullptr;
        if (SUCCEEDED(folderPointer->GetFolders(0, &folderCollection)) && folderCollection != nullptr)
        {
            LONG folderCount = 0;
            folderCollection->get_Count(&folderCount);
            for (LONG folderIndex = 1; folderIndex <= folderCount; ++folderIndex)
            {
                ITaskFolder* childFolder = nullptr;
                VARIANT indexVariant{};
                indexVariant.vt = VT_I4;
                indexVariant.lVal = folderIndex;
                if (SUCCEEDED(folderCollection->get_Item(indexVariant, &childFolder)) && childFolder != nullptr)
                {
                    CollectApiTaskPaths(childFolder, pathsOut, depthRemaining - 1);
                    childFolder->Release();
                }
            }
            folderCollection->Release();
        }
    }

    // ApiTaskPathSet connects to the Task Scheduler service and returns every path it exposes.
    std::unordered_set<std::wstring> ApiTaskPathSet(bool& queriedOk)
    {
        queriedOk = false;
        std::unordered_set<std::wstring> paths;
        // The enumeration thread may or may not already have COM initialized; both cases are fine,
        // and only an initialization we performed ourselves may be undone.
        const HRESULT initResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ownsComInitialization = SUCCEEDED(initResult);
        if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
        {
            return paths;
        }
        ITaskService* taskService = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(&taskService)))
            && taskService != nullptr)
        {
            VARIANT emptyVariant{};
            emptyVariant.vt = VT_EMPTY;
            if (SUCCEEDED(taskService->Connect(emptyVariant, emptyVariant, emptyVariant, emptyVariant)))
            {
                ITaskFolder* rootFolder = nullptr;
                BSTR rootPath = ::SysAllocString(L"\\");
                if (rootPath != nullptr)
                {
                    if (SUCCEEDED(taskService->GetFolder(rootPath, &rootFolder)) && rootFolder != nullptr)
                    {
                        CollectApiTaskPaths(rootFolder, paths, 24);
                        rootFolder->Release();
                        queriedOk = true;
                    }
                    ::SysFreeString(rootPath);
                }
            }
            taskService->Release();
        }
        if (ownsComInitialization)
        {
            ::CoUninitialize();
        }
        return paths;
    }

    // AppendGhostScheduledTaskEntries compares the TaskCache registry against the Task Scheduler API.
    // Three shapes are reported: a task the API refuses to list, a task whose security descriptor was
    // deleted (the documented way to make a task invisible), and a Tasks entry with no Tree leaf.
    void AppendGhostScheduledTaskEntries(std::vector<ks::r3::startup::StartupEntry>& entries)
    {
        const std::wstring treeRoot = std::wstring(kTaskCacheRoot) + L"\\Tree";
        const std::wstring tasksRoot = std::wstring(kTaskCacheRoot) + L"\\Tasks";
        HKEY probeKey = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, treeRoot.c_str(), 0, KEY_READ, &probeKey) != ERROR_SUCCESS)
        {
            // TaskCache needs administrative rights; without it the whole comparison is impossible.
            HiddenFinding finding;
            finding.uniqueSuffixText = "TASKCACHE|Unreadable";
            finding.sourceTypeText = "Hidden-ScannerUnavailable";
            finding.itemNameText = FromWide(L"计划任务缓存不可读");
            finding.locationText = BuildRegistryLocationText(HKEY_LOCAL_MACHINE, treeRoot);
            finding.userText = FromWide(L"本机");
            finding.detailText = FromWide(L"无法读取 TaskCache\\Tree，幽灵计划任务检测被跳过；请以管理员身份重新扫描。");
            finding.riskReasonCode = "hidden_scanner_unavailable";
            finding.riskReasonText = FromWide(L"这是扫描能力缺失的诊断记录，不是启动项本身。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(entries, std::move(finding));
            return;
        }
        ::RegCloseKey(probeKey);

        std::vector<TaskTreeRecord> treeRecords;
        CollectTaskTreeRecords(treeRoot, std::wstring(), 24, treeRecords);

        bool apiQueriedOk = false;
        const std::unordered_set<std::wstring> apiPaths = ApiTaskPathSet(apiQueriedOk);

        // referencedIds lets the second pass find Tasks entries that no Tree leaf points at.
        std::unordered_set<std::wstring> referencedIds;
        for (const TaskTreeRecord& record : treeRecords)
        {
            referencedIds.insert(LowerWideCopy(record.taskIdText));
            const std::wstring taskSubKey = tasksRoot + L"\\" + record.taskIdText;
            // SD 存在 TaskCache\Tree 的叶子上，不在 Tasks\{GUID} 下；从 Tasks 读会恒缺失，
            // 把每一个正常任务都判成幽灵任务，同时真正的删 SD 手法反而检不出来。
            const auto securityRecord =
                QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, record.treeSubKeyText, L"SD");
            const auto actionsRecord = QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, taskSubKey, L"Actions");
            const auto pathRecord = QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, taskSubKey, L"Path");
            const bool apiVisible = !apiQueriedOk
                || apiPaths.find(LowerWideCopy(record.taskPathText)) != apiPaths.end();
            const bool securityDescriptorMissing = !securityRecord.has_value();
            const bool definitionMissing = !actionsRecord.has_value() && !pathRecord.has_value();
            if (apiVisible && !securityDescriptorMissing && !definitionMissing)
            {
                continue;
            }

            // The task XML mirrors the registry definition and is the friendliest evidence to show.
            const std::wstring systemRoot = QueryEnvironmentWide(L"SystemRoot");
            const std::wstring xmlPath = systemRoot.empty()
                ? std::wstring()
                : systemRoot + L"\\System32\\Tasks" + record.taskPathText;
            std::vector<std::string> reasonParts;
            if (securityDescriptorMissing)
            {
                reasonParts.push_back(FromWide(L"TaskCache\\Tree 叶子缺少 SD 安全描述符（任务计划程序界面与 Get-ScheduledTask 都会跳过该任务，但服务仍会执行）"));
            }
            if (!apiVisible)
            {
                reasonParts.push_back(FromWide(L"任务计划程序 API 枚举不返回该路径"));
            }
            if (definitionMissing)
            {
                reasonParts.push_back(FromWide(L"TaskCache\\Tasks 下缺少 Actions/Path 定义"));
            }

            HiddenFinding finding;
            finding.uniqueSuffixText = "GHOSTTASK|" + FromWide(record.taskPathText);
            finding.sourceTypeText = "Hidden-GhostScheduledTask";
            finding.itemNameText = FromWide(record.taskPathText);
            finding.commandText = FromWide(xmlPath);
            // 缺 SD 时把用户直接带到出问题的 Tree 叶子，而不是仍然指向 Tasks\{GUID}。
            finding.locationText = securityDescriptorMissing
                ? BuildRegistryLocationText(HKEY_LOCAL_MACHINE, record.treeSubKeyText)
                : BuildRegistryLocationText(HKEY_LOCAL_MACHINE, tasksRoot + L"\\" + record.taskIdText);
            finding.locationGroupText = BuildRegistryLocationText(HKEY_LOCAL_MACHINE, tasksRoot);
            finding.userText = FromWide(L"本机");
            finding.detailText = JoinStrings(reasonParts, FromWide(L"；"));
            finding.riskReasonCode = "hidden_scheduled_task";
            finding.riskReasonText = FromWide(
                L"这类任务对任务计划程序界面不可见，KSword 的计划任务页同样看不到它，因此这里只报告不操作；"
                L"处置需要同时清理 TaskCache\\Tree、TaskCache\\Tasks 与 System32\\Tasks 下的 XML。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(entries, std::move(finding));
        }

        // Second pass: a Tasks GUID that no Tree leaf references never shows up in any UI.
        for (const std::wstring& taskId : EnumerateRegistrySubKeys(HKEY_LOCAL_MACHINE, tasksRoot))
        {
            if (referencedIds.find(LowerWideCopy(taskId)) != referencedIds.end())
            {
                continue;
            }
            const std::wstring taskSubKey = tasksRoot + L"\\" + taskId;
            const auto pathRecord = QueryRegistryValueRecord(HKEY_LOCAL_MACHINE, taskSubKey, L"Path");
            HiddenFinding finding;
            finding.uniqueSuffixText = "ORPHANTASK|" + FromWide(taskId);
            finding.sourceTypeText = "Hidden-OrphanTaskCacheEntry";
            finding.itemNameText = pathRecord.has_value() && !pathRecord->valueDataText.empty()
                ? pathRecord->valueDataText
                : FromWide(taskId);
            finding.commandText = FromWide(taskId);
            finding.locationText = BuildRegistryLocationText(HKEY_LOCAL_MACHINE, taskSubKey);
            finding.locationGroupText = BuildRegistryLocationText(HKEY_LOCAL_MACHINE, tasksRoot);
            finding.userText = FromWide(L"本机");
            finding.detailText = FromWide(L"TaskCache\\Tasks 下存在该任务定义，但 TaskCache\\Tree 中没有任何叶子指向它。");
            finding.riskReasonCode = "hidden_scheduled_task";
            finding.riskReasonText = FromWide(
                L"没有 Tree 叶子的任务定义不会出现在任何任务列表里；可能是卸载残留，也可能是刻意脱链的持久化项。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(entries, std::move(finding));
        }
    }

    // ===================== 5) Startup folder redirection =====================

    // StartupFolderSpec pairs a shell folder default with the registry value that can redirect it.
    struct StartupFolderSpec
    {
        HKEY rootKey = nullptr;
        const wchar_t* subKeyText = L"";
        const wchar_t* valueNameText = L"";
        const KNOWNFOLDERID* folderId = nullptr;
        const wchar_t* userText = L"";
        const wchar_t* detailText = L"";
    };

    // StripTrailingSeparator removes a trailing backslash so path comparison ignores cosmetic differences.
    std::wstring StripTrailingSeparator(std::wstring pathText)
    {
        while (!pathText.empty() && (pathText.back() == L'\\' || pathText.back() == L'/'))
        {
            pathText.pop_back();
        }
        return pathText;
    }

    // AppendRedirectedStartupFolderEntries reports Startup folders that no longer point at their default.
    // A redirect moves the real autostart directory somewhere the normal folder enumerator never looks.
    void AppendRedirectedStartupFolderEntries(std::vector<ks::r3::startup::StartupEntry>& entries)
    {
        static const std::array<StartupFolderSpec, 4> specs{ {
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", L"Startup", &FOLDERID_Startup, L"当前用户", L"用户启动文件夹（User Shell Folders）" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", L"Startup", &FOLDERID_Startup, L"当前用户", L"用户启动文件夹（Shell Folders 缓存）" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", L"Common Startup", &FOLDERID_CommonStartup, L"本机", L"公共启动文件夹（User Shell Folders）" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", L"Common Startup", &FOLDERID_CommonStartup, L"本机", L"公共启动文件夹（Shell Folders 缓存）" }
        } };

        for (const StartupFolderSpec& spec : specs)
        {
            const std::wstring subKeyText(spec.subKeyText);
            const auto valueRecord = QueryRegistryValueRecord(spec.rootKey, subKeyText, spec.valueNameText);
            if (!valueRecord.has_value() || ks::r3::startup::detail::TrimCopy(valueRecord->valueDataText).empty())
            {
                continue;
            }
            // The value data is already environment-expanded by RegistryDataToText for REG_EXPAND_SZ.
            const std::wstring configuredPath = StripTrailingSeparator(
                TrimWide(ExpandEnvironmentWide(ToWide(valueRecord->valueDataText))));
            const std::wstring defaultPath = StripTrailingSeparator(KnownFolderPath(*spec.folderId));
            if (configuredPath.empty() || defaultPath.empty())
            {
                continue;
            }
            // Only a genuinely different target counts; casing and a trailing slash are not a redirect.
            if (EqualWideI(configuredPath, defaultPath))
            {
                continue;
            }

            const std::string locationText = BuildRegistryLocationText(spec.rootKey, subKeyText);
            HiddenFinding finding;
            finding.uniqueSuffixText = "STARTUPREDIRECT|" + locationText + "|" + FromWide(spec.valueNameText);
            finding.sourceTypeText = "Hidden-StartupFolderRedirect";
            finding.itemNameText = FromWide(spec.valueNameText) + FromWide(L" 指向非默认目录");
            finding.commandText = ToNativeSeparators(FromWide(configuredPath));
            finding.locationText = locationText;
            finding.userText = FromWide(spec.userText);
            finding.detailText = FromWide(spec.detailText) + FromWide(L"；默认目录=")
                + ToNativeSeparators(FromWide(defaultPath))
                + FromWide(L"；实际目录=") + ToNativeSeparators(FromWide(configuredPath));
            finding.riskReasonCode = "hidden_startup_redirect";
            finding.riskReasonText = FromWide(
                L"启动文件夹被重定向后，常规的启动文件夹枚举不会看到真正生效的目录；"
                L"恢复默认值前请确认这不是企业策略下发的配置。");
            finding.canOpenRegistryLocation = true;
            AppendHiddenEntry(entries, std::move(finding));

            // The files inside the redirected directory are the actual autostart payloads.
            std::error_code errorCode;
            if (!std::filesystem::is_directory(configuredPath, errorCode))
            {
                continue;
            }
            std::size_t reportedFileCount = 0;
            for (const auto& directoryEntry : std::filesystem::directory_iterator(configuredPath, errorCode))
            {
                if (errorCode || reportedFileCount >= 64)
                {
                    break;
                }
                const std::filesystem::file_status linkStatus = directoryEntry.symlink_status(errorCode);
                if (errorCode || std::filesystem::is_directory(linkStatus))
                {
                    continue;
                }
                ++reportedFileCount;
                const std::string filePathText = ToNativeSeparators(FromWide(directoryEntry.path().wstring()));
                HiddenFinding fileFinding;
                fileFinding.uniqueSuffixText = "STARTUPREDIRECTFILE|" + filePathText;
                fileFinding.sourceTypeText = "Hidden-RedirectedStartupFile";
                fileFinding.itemNameText = FromWide(directoryEntry.path().filename().wstring());
                fileFinding.commandText = filePathText;
                fileFinding.imagePathText = filePathText;
                fileFinding.locationText = ToNativeSeparators(FromWide(configuredPath));
                fileFinding.locationGroupText = locationText;
                fileFinding.userText = FromWide(spec.userText);
                fileFinding.detailText = FromWide(L"位于被重定向的启动目录中，登录时会被执行，但不在系统默认启动文件夹内。");
                fileFinding.riskReasonCode = "hidden_startup_redirect";
                fileFinding.riskReasonText = FromWide(
                    L"该文件所在目录来自注册表重定向；处理前先确认重定向本身是否合法。");
                AppendHiddenEntry(entries, std::move(fileFinding));
            }
        }
    }

    // ===================== 6) Per-user COM hijacking =====================

    // ComServerRecord holds the server registration of one CLSID in one hive.
    struct ComServerRecord
    {
        bool found = false;
        std::string serverPathText;   // Raw default value of InprocServer32/LocalServer32.
        std::string serverKindText;   // Which server subkey provided the value.
    };

    // QueryComServerRecord reads the first available server registration under a class root.
    // classesSubKey is the hive-relative path of the Classes key, e.g. Software\Classes.
    ComServerRecord QueryComServerRecord(
        const HKEY rootKey,
        const std::wstring& classesSubKey,
        const std::wstring& clsidText)
    {
        ComServerRecord record;
        static const std::array<const wchar_t*, 3> serverKinds{ { L"InprocServer32", L"LocalServer32", L"TreatAs" } };
        for (const wchar_t* serverKind : serverKinds)
        {
            const std::wstring serverSubKey = classesSubKey + L"\\CLSID\\" + clsidText + L"\\" + serverKind;
            const auto valueRecord = QueryRegistryValueRecord(rootKey, serverSubKey, L"");
            if (valueRecord.has_value() && !ks::r3::startup::detail::TrimCopy(valueRecord->valueDataText).empty())
            {
                record.found = true;
                record.serverPathText = valueRecord->valueDataText;
                record.serverKindText = FromWide(serverKind);
                return record;
            }
        }
        return record;
    }

    // AppendComHijackEntries reports HKCU class registrations that shadow a machine-wide CLSID.
    // Per-user registration wins over HKLM for every in-process COM activation in that session,
    // which is why shadowing an already-registered CLSID is a standard persistence primitive.
    void AppendComHijackEntries(std::vector<ks::r3::startup::StartupEntry>& entries)
    {
        // Both the native and the WOW64 class views are checked; the 32-bit view is a common blind spot.
        static const std::array<std::pair<const wchar_t*, const wchar_t*>, 2> classViews{ {
            { L"Software\\Classes", L"Software\\Classes" },
            { L"Software\\Classes\\Wow6432Node", L"Software\\WOW6432Node\\Classes" }
        } };

        for (const auto& classView : classViews)
        {
            const std::wstring userClassesSubKey(classView.first);
            const std::wstring machineClassesSubKey(classView.second);
            const std::wstring userClsidRoot = userClassesSubKey + L"\\CLSID";
            std::size_t inspectedCount = 0;
            for (const std::wstring& clsidName : EnumerateRegistrySubKeys(HKEY_CURRENT_USER, userClsidRoot))
            {
                // A crafted hive with tens of thousands of classes must not stall the scan.
                if (++inspectedCount > 20000)
                {
                    break;
                }
                const ComServerRecord userRecord = QueryComServerRecord(HKEY_CURRENT_USER, userClassesSubKey, clsidName);
                if (!userRecord.found)
                {
                    continue;
                }
                const ComServerRecord machineRecord = QueryComServerRecord(HKEY_LOCAL_MACHINE, machineClassesSubKey, clsidName);
                const bool treatAsHijack = LowerAsciiCopy(userRecord.serverKindText) == "treatas";
                if (!machineRecord.found && !treatAsHijack)
                {
                    // A user-only CLSID is ordinary per-user software registration, not a hijack.
                    continue;
                }
                const std::string userServerPath = ks::r3::startup::NormalizeFilePathText(userRecord.serverPathText);
                const std::string machineServerPath = ks::r3::startup::NormalizeFilePathText(machineRecord.serverPathText);
                if (machineRecord.found
                    && !treatAsHijack
                    && LowerAsciiCopy(userServerPath) == LowerAsciiCopy(machineServerPath))
                {
                    // Same target on both sides: a redundant registration, not a redirection.
                    continue;
                }

                const std::wstring userServerSubKey =
                    userClassesSubKey + L"\\CLSID\\" + clsidName + L"\\" + ToWide(userRecord.serverKindText);
                HiddenFinding finding;
                finding.uniqueSuffixText = "COMHIJACK|" + FromWide(userClsidRoot) + "|" + FromWide(clsidName);
                finding.sourceTypeText = "Hidden-ComHijack";
                const std::string friendlyName = QueryClsidFriendlyName(FromWide(clsidName));
                finding.itemNameText = friendlyName.empty()
                    ? FromWide(clsidName)
                    : friendlyName + " " + FromWide(clsidName);
                finding.commandText = userRecord.serverPathText;
                finding.imagePathText = userServerPath;
                finding.locationText = BuildRegistryLocationText(HKEY_CURRENT_USER, userServerSubKey);
                finding.locationGroupText = BuildRegistryLocationText(HKEY_CURRENT_USER, userClsidRoot);
                finding.userText = FromWide(L"当前用户");
                finding.detailText = treatAsHijack
                    ? FromWide(L"用户级 TreatAs 把该 CLSID 整体重定向到了另一个组件")
                    : (FromWide(L"用户级 ") + userRecord.serverKindText
                        + FromWide(L" 覆盖了机器级注册；机器级目标=") + machineServerPath);
                finding.riskReasonCode = "hidden_com_hijack";
                finding.riskReasonText = FromWide(
                    L"进程内 COM 激活优先使用 HKCU 注册，因此该项会在机器级组件之前被加载；"
                    L"确认它不是软件的正常按用户安装后再处理。");
                finding.canOpenRegistryLocation = true;
                AppendHiddenEntry(entries, std::move(finding));
            }
        }
    }
}

namespace ks::r3::startup
{
    std::vector<StartupEntry> EnumerateHiddenEntries()
    {
        // Order mirrors the cost of each comparison: cheap registry diffs first, COM last.
        std::vector<StartupEntry> entries;
        AppendMaskedRegistryEntries(entries);
        AppendAclBlockedRegistryEntries(entries);
        AppendUnregisteredServiceEntries(entries);
        AppendRedirectedStartupFolderEntries(entries);
        AppendComHijackEntries(entries);
        AppendGhostScheduledTaskEntries(entries);
        return entries;
    }
}
