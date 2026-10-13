#pragma once
#include "CommandRegistry.h"
#include "../shared/usermode/backend/startup/StartupEnumerator.h"
namespace ks::cli::startup {
struct Identity { std::wstring sid; std::uint32_t win32Error = 0; };
Identity identity();
std::wstring utf8(const std::string& text);
std::wstring id(const ks::r3::startup::StartupEntry& entry, const std::wstring& sid);
const wchar_t* category(ks::r3::startup::StartupCategory value);
const wchar_t* kind(const ks::r3::startup::StartupEntry& entry);
const wchar_t* stage(ks::r3::startup::StartupCategory value);
const wchar_t* state(const ks::r3::startup::StartupEntry& entry);
const wchar_t* scope(const ks::r3::startup::StartupEntry& entry);
Json row(const ks::r3::startup::StartupEntry& entry, const std::wstring& sid);
Json errors(const ks::r3::startup::StartupEnumerationResult& snapshot);
}
