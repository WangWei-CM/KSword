#pragma once

#include "../../Core/Win32Lean.h"

#include "DriverEnumerator.h"
#include "DriverModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Features::Driver {

// DriverActionResult carries the outcome of a light-module action. Inputs are a
// model update or text utility request; processing stays inside the Driver
// feature surface; output is a short status string for the page footer.
struct DriverActionResult {
    bool success = false;
    std::wstring statusText;
};

// DriverActions owns the module-local action facade. Inputs are view requests
// such as refresh or clipboard export; processing delegates enumeration to the
// R3 driver enumerator and keeps UI code free of direct driver calls; output is
// a compact result structure or a formatted helper string.
class DriverActions final {
public:
    // RefreshModel performs one snapshot refresh and updates the supplied
    // DriverModel. Input is a live model instance; processing queries the R3
    // driver/object enumerator; output is a summary suitable for the status bar.
    static DriverActionResult RefreshModel(DriverModel& model);

    // CopyTextToClipboard writes Unicode text to the shell clipboard. Inputs are
    // owner and text; processing allocates a CF_UNICODETEXT block; output is
    // true when the clipboard accepted the data.
    static bool CopyTextToClipboard(HWND owner, const std::wstring& text);

    // BuildTsv serializes a rectangular text table. Inputs are headers and
    // already-filtered rows; processing escapes tabs and newlines with spaces;
    // output is a TSV payload that callers can copy or save later.
    static std::wstring BuildTsv(
        const std::vector<std::wstring>& headers,
        const std::vector<std::vector<std::wstring>>& rows);

    // BuildDriverObjectDetailText queries the shared ArkDriverClient
    // DriverObject IOCTL and formats the response for a popup. Input is a
    // DriverObject name such as \Driver\ACPI; processing never calls
    // DeviceIoControl directly; output is a detailed diagnostic string.
    static DriverActionResult BuildDriverObjectDetailText(const std::wstring& driverObjectName);

    // ForceUnloadDriverObject requests the R0 force-unload IOCTL by
    // DriverObject name. Inputs are an object name and protocol flags;
    // processing routes through ArkDriverClient; output summarizes the R0
    // response and whether the aggregate unload status is successful.
    static DriverActionResult ForceUnloadDriverObject(const std::wstring& driverObjectName, unsigned long flags = 0UL);

    // ForceUnloadDriverByModuleBase requests the R0 force-unload IOCTL by
    // kernel module base. Inputs are the base address, optional fallback name
    // and flags; processing routes through ArkDriverClient; output summarizes
    // the R0 response.
    static DriverActionResult ForceUnloadDriverByModuleBase(
        std::uint64_t moduleBase,
        const std::wstring& fallbackDriverName,
        unsigned long flags = 0UL);
};

} // namespace Ksword::Features::Driver
