#include "StartupActions.h"
#include "../../../shared/usermode/backend/startup/StartupActions.h"
#include <shellapi.h>
namespace Ksword::Features::Startup {
namespace {
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), needed) == needed ? result : std::wstring{};
}
StartupActionResult Convert(const ks::r3::startup::ActionResult& result) {
    StartupActionResult output{};
    output.success = result.success;
    output.message = Wide(result.messageText);
    output.errorCode = result.errorCode;
    output.unsupported = result.status == ks::r3::startup::StartupActionStatus::NotSupported;
    return output;
}
StartupActionResult OpenLocation(const StartupEntry& entry) {
    if (entry.kind == StartupEntryKind::DriverService || entry.kind == StartupEntryKind::RegistryOnlyService) {
        return {false, L"该启动项仅供只读调查，不能打开位置。", ERROR_NOT_SUPPORTED, true};
    }
    const std::wstring path = !entry.filePath.empty() ? entry.filePath : entry.location;
    if (path.empty()) return {false, L"启动项位置为空。", ERROR_FILE_NOT_FOUND, false};
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) return {false, L"无法打开启动项位置。", static_cast<std::uint32_t>(reinterpret_cast<INT_PTR>(result)), false};
    return {true, L"已打开启动项位置。", ERROR_SUCCESS, false};
}
}
StartupActionResult EnableStartupEntry(const StartupEntry& entry) { return Convert(ks::r3::startup::SetStartupEntryEnabled(entry.backendEntry, true)); }
StartupActionResult DisableStartupEntry(const StartupEntry& entry) { return Convert(ks::r3::startup::SetStartupEntryEnabled(entry.backendEntry, false)); }
StartupActionResult DeleteStartupEntry(const StartupEntry& entry) { return Convert(ks::r3::startup::DeleteStartupEntry(entry.backendEntry)); }
StartupActionResult OpenStartupEntryLocation(const StartupEntry& entry) { return OpenLocation(entry); }
} // namespace Ksword::Features::Startup
