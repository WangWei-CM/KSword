#pragma once
#include "StartupModel.h"
#include <cstdint>
namespace Ksword::Features::Startup {
struct StartupActionResult {
    bool success = false;
    std::wstring message;
    std::uint32_t errorCode = 0;
    bool unsupported = false;
};
StartupActionResult EnableStartupEntry(const StartupEntry& entry);
StartupActionResult DisableStartupEntry(const StartupEntry& entry);
StartupActionResult DeleteStartupEntry(const StartupEntry& entry);
StartupActionResult OpenStartupEntryLocation(const StartupEntry& entry);
}
