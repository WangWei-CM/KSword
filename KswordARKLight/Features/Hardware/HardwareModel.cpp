#include "HardwareModel.h"

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
    AddProperty(detail, L"Manufacturer", node.manufacturer);
    AddProperty(detail, L"Service", node.serviceName);
    AddProperty(detail, L"Driver key", node.driverKey);
    AddProperty(detail, L"Location", node.location);
    AddProperty(detail, L"Hardware IDs", node.hardwareIds);
    AddProperty(detail, L"State", DeviceStateText(node.state, node.problemCode));
    return detail;
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

} // namespace Ksword::Features::Hardware
