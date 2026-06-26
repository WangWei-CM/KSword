#pragma once

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverTypes.h"
#include "KernelModel.h"

#include <string>
#include <vector>

namespace Ksword::Features::Kernel {

// DynDataProfileMatch describes one local ark_dyndata_pack match for the
// currently loaded kernel image. Inputs are produced by matching the R0
// ntoskrnl identity against local JSON packs; processing later can either render
// diagnostics or call ArkDriverClient apply APIs; return behavior is value-only.
struct DynDataProfileMatch {
    bool scanned = false;
    bool matched = false;
    bool valid = false;
    bool preferExApply = false;
    std::wstring path;
    std::wstring message;
    std::uint32_t existingPackCount = 0;
    std::uint32_t profileCount = 0;
    std::uint32_t scannedProfileCount = 0;
    std::uint32_t fieldCount = 0;
    std::uint32_t typedItemCount = 0;
    std::uint32_t callbackItemCount = 0;
    double coveragePercent = -1.0;
    ksword::ark::DynDataProfileApplyInput profile;
    ksword::ark::DynDataProfileApplyExInput profileEx;
};

// FindMatchingDynDataProfile searches the runtime profile locations for a pack
// entry matching the current ntoskrnl identity. Input is the R0 identity packet;
// processing reads JSON locally and does not contact the driver; output contains
// diagnostics plus ready-to-apply ArkDriverClient input models when matched.
DynDataProfileMatch FindMatchingDynDataProfile(const ksword::ark::ArkDynModuleIdentity& identity);

// AppendDynDataProfileRows appends human-readable profile match details into a
// generic KernelOperationResult. Inputs are the match result and display filter;
// processing only creates rows; no value is returned.
void AppendDynDataProfileRows(KernelOperationResult& result, const DynDataProfileMatch& match, const std::wstring& filterText);

} // namespace Ksword::Features::Kernel
