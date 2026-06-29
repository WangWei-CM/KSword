#include "HardwareModel.h"

#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <utility>

namespace Ksword::Features::Hardware {
namespace {

// AddProperty appends a detail row only when the value exists. Inputs are target
// detail, label and value; processing keeps the detail pane compact; no return.
void AddProperty(HardwareDeviceDetail& detail, const std::wstring& name, const std::wstring& value) {
    if (!value.empty()) {
        detail.properties.push_back({ name, value });
    }
}

// ToLower returns a case-folded copy for simple substring classification. Input
// is a display or instance string; processing lowercases ASCII/Unicode wchar_t
// values through towlower; output preserves the original string length.
std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

// ContainsAny checks whether a haystack contains one of the supplied markers.
// Inputs are already-normalized text and marker literals; output is true on the
// first substring match.
bool ContainsAny(const std::wstring& haystack, const std::initializer_list<const wchar_t*> markers) {
    for (const wchar_t* marker : markers) {
        if (haystack.find(marker) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// NodeSearchText builds a normalized evidence string from stable devnode fields.
// Input is a HardwareDeviceNode; processing concatenates class, service,
// instance, hardware and compatible IDs; output is used only for classification.
std::wstring NodeSearchText(const HardwareDeviceNode& node) {
    return ToLower(node.instanceId + L" " +
        node.parentInstanceId + L" " +
        node.displayName + L" " +
        node.className + L" " +
        node.classGuid + L" " +
        node.serviceName + L" " +
        node.location + L" " +
        node.locationPaths + L" " +
        node.hardwareIds + L" " +
        node.compatibleIds);
}

// HasFilterEvidence reports whether any registry filter source is present for a
// devnode. Input is a cached node; output is true when device/class upper/lower
// filter rows can be shown as read-only evidence.
bool HasFilterEvidence(const HardwareDeviceNode& node) {
    return !node.upperFilters.empty() ||
        !node.lowerFilters.empty() ||
        !node.classUpperFilters.empty() ||
        !node.classLowerFilters.empty();
}

// IsInputDevice classifies keyboard, mouse and HID-family devnodes. Input is a
// node; processing uses only SetupAPI/CM class/service/ID text; output does not
// imply live input capture.
bool IsInputDevice(const HardwareDeviceNode& node) {
    const std::wstring text = NodeSearchText(node);
    return ContainsAny(text, { L"keyboard", L"kbd", L"mouse", L"mou", L"hidclass", L"hidusb", L"hid\\", L"hid_device" });
}

// IsHidDevice classifies generic HID-family rows. Input is a node; output is
// true for HID class/service/ID evidence and false otherwise.
bool IsHidDevice(const HardwareDeviceNode& node) {
    const std::wstring text = NodeSearchText(node);
    return ContainsAny(text, { L"hidclass", L"hidusb", L"hid\\", L"hid_device", L"hid-compliant" });
}

// IsUsbDevice classifies USB controller, hub, composite and interface rows.
// Input is a node; output is derived from instance/service/location evidence.
bool IsUsbDevice(const HardwareDeviceNode& node) {
    const std::wstring text = NodeSearchText(node);
    return ContainsAny(text, { L"usb\\", L"usbstor", L"usbccgp", L"usbhub", L"usbxhci", L"ucx", L"vid_", L"pid_", L"mi_" });
}

// IsPciDevice classifies PCI-backed rows. Input is a node; output is true for
// PCI instance IDs, location paths and common PCI identifier fields.
bool IsPciDevice(const HardwareDeviceNode& node) {
    const std::wstring text = NodeSearchText(node);
    return ContainsAny(text, { L"pci\\", L"pciroot", L"ven_", L"dev_", L"subsys_" });
}

// IsAcpiDevice classifies ACPI and power-management rows. Input is a node;
// output is true for ACPI instance IDs, services and location-path evidence.
bool IsAcpiDevice(const HardwareDeviceNode& node) {
    const std::wstring text = NodeSearchText(node);
    return ContainsAny(text, { L"acpi\\", L"acpi(", L"processor", L"intelpep", L"processr", L"pdc" });
}

} // namespace

HardwareModel::HardwareModel() = default;

void HardwareModel::setDevices(std::vector<HardwareDeviceNode> devices) {
    devices_ = std::move(devices);
    rebuildRoots();
}

const std::vector<HardwareDeviceNode>& HardwareModel::devices() const {
    return devices_;
}

const std::vector<int>& HardwareModel::rootIndices() const {
    return rootIndices_;
}

const HardwareDeviceNode* HardwareModel::deviceAt(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) {
        return nullptr;
    }
    return &devices_[index];
}

std::wstring HardwareModel::textForColumn(const HardwareDeviceNode& node, int column) const {
    switch (column) {
    case 0:
        return CompactDeviceName(node);
    case 1:
        return node.className;
    case 2:
        return DeviceStateText(node.state, node.problemCode);
    case 3:
        return node.manufacturer;
    case 4:
        return node.serviceName;
    default:
        return {};
    }
}

HardwareDeviceDetail HardwareModel::detailFromNode(const HardwareDeviceNode& node) const {
    HardwareDeviceDetail detail;
    detail.found = true;
    detail.title = CompactDeviceName(node);
    detail.instanceId = node.instanceId;
    AddProperty(detail, L"Display name", CompactDeviceName(node));
    AddProperty(detail, L"Instance ID", node.instanceId);
    AddProperty(detail, L"Class", node.className);
    AddProperty(detail, L"Class GUID", node.classGuid);
    AddProperty(detail, L"Manufacturer", node.manufacturer);
    AddProperty(detail, L"Service", node.serviceName);
    AddProperty(detail, L"Driver key", node.driverKey);
    AddProperty(detail, L"Location", node.location);
    AddProperty(detail, L"Location paths", node.locationPaths);
    AddProperty(detail, L"Hardware IDs", node.hardwareIds);
    AddProperty(detail, L"Compatible IDs", node.compatibleIds);
    AddProperty(detail, L"Device UpperFilters", node.upperFilters);
    AddProperty(detail, L"Device LowerFilters", node.lowerFilters);
    AddProperty(detail, L"Class UpperFilters", node.classUpperFilters);
    AddProperty(detail, L"Class LowerFilters", node.classLowerFilters);
    AddProperty(detail, L"Read-only audit", HardwareReadOnlyAuditDescription(node));
    AddProperty(detail, L"State", DeviceStateText(node.state, node.problemCode));
    return detail;
}

HardwareAuditSummary HardwareModel::auditSummary() const {
    HardwareAuditSummary summary;
    summary.totalDevices = devices_.size();
    for (const HardwareDeviceNode& node : devices_) {
        if (IsInputDevice(node)) {
            ++summary.inputDevices;
        }
        if (IsHidDevice(node)) {
            ++summary.hidDevices;
        }
        if (IsUsbDevice(node)) {
            ++summary.usbDevices;
        }
        if (IsPciDevice(node)) {
            ++summary.pciDevices;
        }
        if (IsAcpiDevice(node)) {
            ++summary.acpiDevices;
        }
        if (HasFilterEvidence(node)) {
            ++summary.filterEvidenceDevices;
        }
        if (node.state == HardwareDeviceState::Problem ||
            node.state == HardwareDeviceState::Disabled ||
            node.state == HardwareDeviceState::Phantom) {
            ++summary.problemDevices;
        }
    }
    return summary;
}

void HardwareModel::rebuildRoots() {
    rootIndices_.clear();
    for (const HardwareDeviceNode& node : devices_) {
        if (node.parentIndex < 0) {
            rootIndices_.push_back(node.index);
        }
    }
}

std::wstring DeviceStateText(HardwareDeviceState state, ULONG problemCode) {
    switch (state) {
    case HardwareDeviceState::Started:
        return L"Started";
    case HardwareDeviceState::Stopped:
        return L"Stopped";
    case HardwareDeviceState::Disabled:
        return L"Disabled";
    case HardwareDeviceState::Problem:
        return L"Problem " + std::to_wstring(problemCode);
    case HardwareDeviceState::Phantom:
        return L"Not present";
    default:
        break;
    }
    return L"Unknown";
}

std::wstring CompactDeviceName(const HardwareDeviceNode& node) {
    if (!node.displayName.empty()) {
        return node.displayName;
    }
    if (!node.className.empty()) {
        return node.className;
    }
    if (!node.instanceId.empty()) {
        return node.instanceId;
    }
    return L"Unnamed device";
}

std::wstring HardwareReadOnlyAuditDescription(const HardwareDeviceNode& node) {
    std::vector<std::wstring> labels;
    if (IsInputDevice(node)) {
        labels.push_back(L"Input chain");
    }
    if (IsHidDevice(node)) {
        labels.push_back(L"HID");
    }
    if (IsUsbDevice(node)) {
        labels.push_back(L"USB topology");
    }
    if (IsPciDevice(node)) {
        labels.push_back(L"PCI/PnP");
    }
    if (IsAcpiDevice(node)) {
        labels.push_back(L"ACPI/PnP");
    }
    if (HasFilterEvidence(node)) {
        labels.push_back(L"Filter registry evidence");
    }
    if (labels.empty()) {
        return L"Device stack/PnP row from SetupAPI and Configuration Manager";
    }

    std::wstring out;
    for (const std::wstring& label : labels) {
        if (!out.empty()) {
            out += L"; ";
        }
        out += label;
    }
    return out;
}

} // namespace Ksword::Features::Hardware
