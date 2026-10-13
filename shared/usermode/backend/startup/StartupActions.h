#pragma once
#include "StartupTypes.h"
namespace ks::r3::startup {
ActionResult SetStartupEntryEnabled(const StartupEntry& entry, bool enabled);
ActionResult DeleteStartupEntry(const StartupEntry& entry);
}
