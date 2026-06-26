#pragma once

#include "../../Core/Win32Lean.h"

#include <cfgmgr32.h>
#include <string>
#include <vector>

namespace Ksword::Features::Hardware {

// HardwareDeviceState is the UI-facing status derived from Configuration Manager.
// Inputs are CM_Get_DevNode_Status flags and problem codes; processing happens in
// HardwareEnumerator; values are consumed by HardwareModel and HardwareView.
enum class HardwareDeviceState {
    Unknown,
    Started,
    Stopped,
    Disabled,
    Problem,
    Phantom
};

// HardwareProperty is one name/value pair shown in the device detail pane. Inputs
// are SetupAPI and Configuration Manager properties; consumers display values as
// already-formatted text and do not parse the value column.
struct HardwareProperty {
    std::wstring name;
    std::wstring value;
};

// HardwareDeviceNode is one device-manager tree item. Inputs are SP_DEVINFO_DATA
// records enriched by CM_Get_Parent and registry properties; processing stores
// parent/child indexes rather than pointers so vector reallocation cannot break
// tree links; return behavior is passive data only.
struct HardwareDeviceNode {
    int index = -1;
    int parentIndex = -1;
    int depth = 0;
    DEVINST devInst = 0;
    ULONG statusFlags = 0;
    ULONG problemCode = 0;
    HardwareDeviceState state = HardwareDeviceState::Unknown;
    std::wstring instanceId;
    std::wstring parentInstanceId;
    std::wstring displayName;
    std::wstring className;
    std::wstring manufacturer;
    std::wstring serviceName;
    std::wstring driverKey;
    std::wstring location;
    std::wstring hardwareIds;
    std::vector<int> childIndices;
};

// HardwareDeviceDetail is the expanded detail view for one device instance. Input
// is an instance ID; HardwareEnumerator queries the matching devnode and fills
// properties; consumers receive a title plus a flat property list.
struct HardwareDeviceDetail {
    bool found = false;
    std::wstring title;
    std::wstring instanceId;
    std::vector<HardwareProperty> properties;
};

// HardwareEnumerationResult contains one device-manager enumeration pass. success
// is false only when SetupAPI cannot create/enumerate the device information set;
// partial property failures are represented by empty strings on individual nodes.
struct HardwareEnumerationResult {
    bool success = false;
    std::wstring diagnosticText;
    std::vector<HardwareDeviceNode> devices;
};

// HardwareModel owns the latest device tree snapshot. Inputs are produced by
// HardwareEnumerator; processing builds root indexes and stable column/detail
// strings; outputs are references valid until the next setDevices call.
class HardwareModel final {
public:
    HardwareModel();

    // setDevices replaces the current device tree. Input is a complete vector of
    // nodes with child indexes already prepared; processing records root nodes;
    // no value is returned.
    void setDevices(std::vector<HardwareDeviceNode> devices);

    // devices returns the raw device rows. There is no input; output is a const
    // reference valid until the model is refreshed.
    const std::vector<HardwareDeviceNode>& devices() const;

    // rootIndices returns indexes for top-level tree nodes. There is no input;
    // output is a const reference valid until the model is refreshed.
    const std::vector<int>& rootIndices() const;

    // deviceAt validates an index. Input is a model index; output is nullptr when
    // the index is outside the current snapshot.
    const HardwareDeviceNode* deviceAt(int index) const;

    // textForColumn returns a list/tree column string. Inputs are node and column
    // number; output is empty for unsupported columns.
    std::wstring textForColumn(const HardwareDeviceNode& node, int column) const;

    // detailFromNode converts the cached node into immediate detail rows. Input
    // is a device node; processing avoids extra SetupAPI calls; output is a detail
    // object suitable as a fallback when live querying fails.
    HardwareDeviceDetail detailFromNode(const HardwareDeviceNode& node) const;

private:
    void rebuildRoots();

private:
    std::vector<HardwareDeviceNode> devices_;
    std::vector<int> rootIndices_;
};

// DeviceStateText formats HardwareDeviceState and problem code into display text.
// Inputs are state and CM problem code; output is a human-readable label.
std::wstring DeviceStateText(HardwareDeviceState state, ULONG problemCode);

// CompactDeviceName returns the safest display label for a device node. Input is
// a node with optional friendly name and instance ID; output is never empty.
std::wstring CompactDeviceName(const HardwareDeviceNode& node);

} // namespace Ksword::Features::Hardware
