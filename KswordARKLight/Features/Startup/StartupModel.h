#pragma once

#include "../../../shared/usermode/backend/startup/StartupTypes.h"
#include "../../Core/Win32Lean.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Features::Startup {
using BackendEntry = ks::r3::startup::StartupEntry;
using BackendEnumerationResult = ks::r3::startup::StartupEnumerationResult;

enum class StartupEntryKind : int {
    RegistryRun,
    RegistryRunOnce,
    StartupFolder,
    Service,
    DriverService,
    RegistryOnlyService,
    ScheduledTaskFacade,
};
enum class StartupEntryScope : int { CurrentUser, LocalMachine, AllUsers, Unknown };
enum class StartupEntryState : int { Active, Disabled, Manual, Unknown };
struct StartupProperty { std::wstring name; std::wstring value; };
struct StartupEntry {
    BackendEntry backendEntry;
    StartupEntryKind kind = StartupEntryKind::RegistryRun;
    StartupEntryScope scope = StartupEntryScope::Unknown;
    StartupEntryState state = StartupEntryState::Unknown;
    std::wstring name;
    std::wstring command;
    std::wstring location;
    std::wstring description;
    std::wstring publisher;
    HKEY registryRoot = nullptr;
    DWORD registryView = 0;
    std::wstring registrySubKey;
    std::wstring registryValueName;
    std::wstring disabledRegistrySubKey;
    std::wstring filePath;
    std::wstring disabledFilePath;
    std::wstring serviceName;
    DWORD serviceStartType = 0;
    std::wstring taskPath;
    std::vector<StartupProperty> properties;
};
struct StartupEnumerationResult {
    bool success = false;
    std::wstring diagnosticText;
    std::vector<StartupEntry> entries;
    std::vector<BackendEnumerationResult::SourceError> sourceErrors;
};

class StartupModel final {
public:
    StartupModel();
    void setEntries(std::vector<StartupEntry> entries);
    const std::vector<StartupEntry>& entries() const;
    const StartupEntry* entryAt(int index) const;
    std::wstring textForColumn(const StartupEntry& entry, int column) const;
    std::vector<StartupProperty> propertiesForEntry(const StartupEntry& entry) const;
private:
    std::vector<StartupEntry> entries_;
};

std::wstring StartupKindText(StartupEntryKind kind);
std::wstring StartupScopeText(StartupEntryScope scope);
std::wstring StartupStateText(StartupEntryState state);

} // namespace Ksword::Features::Startup
