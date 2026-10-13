#pragma once
#include "StartupTypes.h"
namespace ks::r3::startup {
StartupEnumerationResult EnumerateStartupSnapshot(
    const StartupEnumerationProgressCallback& progressCallback = {},
    const StartupEnumerationStageResultCallback& stageResultCallback = {});
std::vector<StartupEntry> EnumerateAllStartupEntries();
std::vector<StartupEntry> EnumerateAllStartupEntries(const StartupEnumerationProgressCallback& progressCallback);
std::vector<StartupEntry> EnumerateAllStartupEntries(const StartupEnumerationProgressCallback& progressCallback,
    const StartupEnumerationStageResultCallback& stageResultCallback);
std::vector<StartupEntry> EnumerateLogonEntries();
std::vector<StartupEntry> EnumerateServiceEntries();
std::vector<StartupEntry> EnumerateDriverEntries();
std::vector<StartupEntry> EnumerateTaskEntries();
std::vector<StartupEntry> EnumerateImageHijackEntries();
std::vector<StartupEntry> EnumerateAdvancedRegistryEntries();
std::vector<StartupEntry> EnumerateWinsockEntries();
std::vector<StartupEntry> EnumerateWmiEntries();
std::vector<StartupEntry> EnumerateHiddenEntries();
}
