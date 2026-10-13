#include "StartupEnumerator.h"
#include "../../../shared/usermode/backend/startup/StartupEnumerator.h"
#include <Windows.h>
#include <algorithm>
#include <cwctype>
namespace Ksword::Features::Startup {
namespace {
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), needed) == needed ? result : std::wstring{};
}
std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}
StartupEntryKind Kind(const BackendEntry& entry) {
    if (entry.category == ks::r3::startup::StartupCategory::Services) return StartupEntryKind::Service;
    if (entry.category == ks::r3::startup::StartupCategory::Drivers) return StartupEntryKind::DriverService;
    if (entry.category == ks::r3::startup::StartupCategory::Tasks) return StartupEntryKind::ScheduledTaskFacade;
    if (entry.category == ks::r3::startup::StartupCategory::Logon) {
        const auto type = Lower(Wide(entry.sourceTypeText));
        if (type.find(L"once") != std::wstring::npos) return StartupEntryKind::RegistryRunOnce;
        if (type.find(L"folder") != std::wstring::npos) return StartupEntryKind::StartupFolder;
        return StartupEntryKind::RegistryRun;
    }
    return StartupEntryKind::RegistryRun;
}
StartupEntryScope Scope(const BackendEntry& entry) {
    const auto location = Lower(Wide(entry.locationText));
    if (location.rfind(L"hkcu\\", 0) == 0) return StartupEntryScope::CurrentUser;
    if (location.rfind(L"hklm\\", 0) == 0 || location.rfind(L"scm\\", 0) == 0) return StartupEntryScope::LocalMachine;
    const auto user = Lower(Wide(entry.userText));
    if (user.find(L"all") != std::wstring::npos || user.find(L"公共") != std::wstring::npos) return StartupEntryScope::AllUsers;
    if (user.find(L"current") != std::wstring::npos || user.find(L"当前") != std::wstring::npos) return StartupEntryScope::CurrentUser;
    return StartupEntryScope::Unknown;
}
StartupEntryState State(const BackendEntry& entry) {
    if (entry.category == ks::r3::startup::StartupCategory::Services && entry.actionLocator.serviceStartMode == ks::r3::startup::StartupScmStartMode::Manual) return StartupEntryState::Manual;
    return entry.enabled ? StartupEntryState::Active : StartupEntryState::Disabled;
}
StartupEntry Convert(const BackendEntry& backend) {
    StartupEntry entry{};
    entry.backendEntry = backend;
    entry.kind = Kind(backend);
    entry.scope = Scope(backend);
    entry.state = State(backend);
    entry.name = Wide(backend.itemNameText);
    entry.command = Wide(backend.commandText);
    entry.location = Wide(backend.locationText);
    entry.description = Wide(backend.detailText);
    entry.publisher = Wide(backend.publisherText);
    entry.registryValueName = Wide(backend.registryValueNameText);
    entry.serviceName = Wide(backend.actionLocator.serviceNameText);
    entry.filePath = Wide(backend.actionLocator.originalFilePathText.empty() ? backend.imagePathText : backend.actionLocator.originalFilePathText);
    entry.disabledFilePath = Wide(backend.actionLocator.parkedFilePathText);
    entry.taskPath = Wide(backend.actionLocator.taskPathText);
    if (backend.actionLocator.registryRoot == ks::r3::startup::StartupRegistryRoot::CurrentUser) entry.registryRoot = HKEY_CURRENT_USER;
    if (backend.actionLocator.registryRoot == ks::r3::startup::StartupRegistryRoot::LocalMachine) entry.registryRoot = HKEY_LOCAL_MACHINE;
    entry.registryView = backend.actionLocator.registryRoot == ks::r3::startup::StartupRegistryRoot::None ? 0 : KEY_WOW64_64KEY;
    entry.registrySubKey = Wide(backend.actionLocator.registrySubKeyText);
    entry.disabledRegistrySubKey = Wide(backend.actionLocator.registrySubKeyText);
    entry.serviceStartType = backend.actionLocator.serviceStartType;
    entry.properties.push_back({L"来源类型", Wide(backend.sourceTypeText)});
    entry.properties.push_back({L"风险", Wide(backend.riskReasonText)});
    entry.properties.push_back({L"唯一标识", Wide(backend.uniqueIdText)});
    return entry;
}
}
StartupEnumerationResult EnumerateStartupEntries() {
    const auto backend = ks::r3::startup::EnumerateStartupSnapshot();
    StartupEnumerationResult result{};
    result.success = backend.success;
    result.diagnosticText = Wide(backend.diagnosticText);
    result.sourceErrors = backend.sourceErrors;
    result.entries.reserve(backend.entries.size());
    for (const auto& entry : backend.entries) result.entries.push_back(Convert(entry));
    if (!result.success && result.diagnosticText.empty()) result.diagnosticText = L"启动项枚举失败。";
    if (!result.sourceErrors.empty() && result.diagnosticText.empty()) result.diagnosticText = L"部分启动来源读取失败。";
    return result;
}
} // namespace Ksword::Features::Startup
