#include "HardwareEnumerator.h"

#include "../../Core/Common.h"

#include <algorithm>
#include <cfgmgr32.h>
#include <cstring>
#include <map>
#include <setupapi.h>

#ifndef DN_PHANTOM
#define DN_PHANTOM 0x00004000
#endif

namespace Ksword::Features::Hardware {
namespace {

// DevInfoSet owns a SetupAPI HDEVINFO handle. Inputs are handles from
// SetupDiGetClassDevsW or SetupDiCreateDeviceInfoList; processing releases the
// handle at scope exit; get() returns the raw handle without transferring it.
class DevInfoSet final {
public:
    explicit DevInfoSet(HDEVINFO handle) : handle_(handle) {}
    ~DevInfoSet() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::SetupDiDestroyDeviceInfoList(handle_);
        }
    }

    DevInfoSet(const DevInfoSet&) = delete;
    DevInfoSet& operator=(const DevInfoSet&) = delete;

    HDEVINFO get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HDEVINFO handle_;
};

// JoinMultiSz converts a REG_MULTI_SZ buffer into a semicolon-separated string.
// Input is the raw UTF-16 buffer returned by SetupAPI; output is compact display
// text and is empty when the buffer has no strings.
std::wstring JoinMultiSz(const std::vector<wchar_t>& buffer) {
    std::wstring out;
    const wchar_t* cursor = buffer.data();
    const wchar_t* end = buffer.data() + buffer.size();
    while (cursor < end && *cursor) {
        const std::wstring part(cursor);
        if (!out.empty()) {
            out += L"; ";
        }
        out += part;
        cursor += part.size() + 1;
    }
    return out;
}

// RegistryTypeToString formats non-string registry data. Inputs are registry type
// and raw bytes; processing keeps display deterministic; output is readable text.
std::wstring RegistryTypeToString(DWORD type, const std::vector<BYTE>& data) {
    if ((type == REG_DWORD || type == REG_DWORD_LITTLE_ENDIAN) && data.size() >= sizeof(DWORD)) {
        DWORD value = 0;
        std::memcpy(&value, data.data(), sizeof(value));
        return L"0x" + std::to_wstring(value);
    }
    if (type == REG_QWORD && data.size() >= sizeof(ULONGLONG)) {
        ULONGLONG value = 0;
        std::memcpy(&value, data.data(), sizeof(value));
        return std::to_wstring(value);
    }
    return L"(" + std::to_wstring(data.size()) + L" bytes)";
}

// QueryDeviceRegistryProperty reads one SPDRP_* value as display text. Inputs are
// the device set, device info record and property identifier; processing handles
// REG_SZ, REG_EXPAND_SZ, REG_MULTI_SZ and numeric fallbacks; output is empty on
// missing or unsupported values.
std::wstring QueryDeviceRegistryProperty(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD property) {
    DWORD type = 0;
    DWORD needed = 0;
    ::SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type, nullptr, 0, &needed);
    if (needed == 0) {
        return {};
    }

    std::vector<BYTE> bytes(needed + sizeof(wchar_t) * 2, 0);
    if (!::SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type, bytes.data(), needed, nullptr)) {
        return {};
    }

    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        const wchar_t* text = reinterpret_cast<const wchar_t*>(bytes.data());
        return text ? std::wstring(text) : std::wstring();
    }
    if (type == REG_MULTI_SZ) {
        const auto* first = reinterpret_cast<const wchar_t*>(bytes.data());
        const std::size_t count = bytes.size() / sizeof(wchar_t);
        return JoinMultiSz(std::vector<wchar_t>(first, first + count));
    }
    return RegistryTypeToString(type, bytes);
}

// DevInstToInstanceId returns the stable PnP instance ID for one devnode. Input
// is a DEVINST from SetupAPI/CM; processing uses CM_Get_Device_IDW; output is
// empty when the devnode is invalid or was removed while enumerating.
std::wstring DevInstToInstanceId(DEVINST devInst) {
    ULONG length = 0;
    if (::CM_Get_Device_ID_Size(&length, devInst, 0) != CR_SUCCESS) {
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1, L'\0');
    if (::CM_Get_Device_IDW(devInst, buffer.data(), static_cast<ULONG>(buffer.size()), 0) != CR_SUCCESS) {
        return {};
    }
    return std::wstring(buffer.data());
}

// QueryParentInstanceId reads the parent devnode ID through Configuration
// Manager. Input is a child DEVINST; output is empty for root devices or failed
// parent lookups.
std::wstring QueryParentInstanceId(DEVINST devInst) {
    DEVINST parent = 0;
    if (::CM_Get_Parent(&parent, devInst, 0) != CR_SUCCESS) {
        return {};
    }
    return DevInstToInstanceId(parent);
}

// StateFromStatus converts CM status/problem values to a small UI enum. Inputs
// are raw status flags and problem code; output is a HardwareDeviceState used by
// model and view formatting.
HardwareDeviceState StateFromStatus(ULONG status, ULONG problem) {
    if ((status & DN_HAS_PROBLEM) != 0) {
        if (problem == CM_PROB_DISABLED) {
            return HardwareDeviceState::Disabled;
        }
        return HardwareDeviceState::Problem;
    }
    if ((status & DN_STARTED) != 0) {
        return HardwareDeviceState::Started;
    }
    if ((status & DN_PHANTOM) != 0) {
        return HardwareDeviceState::Phantom;
    }
    if (status != 0) {
        return HardwareDeviceState::Stopped;
    }
    return HardwareDeviceState::Unknown;
}

// QueryStatus fills status flags and problem code for one device. Input is a
// DEVINST; processing uses CM_Get_DevNode_Status; output is the derived state.
HardwareDeviceState QueryStatus(DEVINST devInst, ULONG& status, ULONG& problem) {
    status = 0;
    problem = 0;
    if (::CM_Get_DevNode_Status(&status, &problem, devInst, 0) != CR_SUCCESS) {
        return HardwareDeviceState::Unknown;
    }
    return StateFromStatus(status, problem);
}

// AppendProperty adds a live detail row when a value exists. Inputs are mutable
// detail, label and value; processing filters empty values; no return.
void AppendProperty(HardwareDeviceDetail& detail, const std::wstring& name, const std::wstring& value) {
    if (!value.empty()) {
        detail.properties.push_back({ name, value });
    }
}

// PopulateNodeFromDevInfo converts one SP_DEVINFO_DATA into the module model.
// Inputs are a SetupAPI set, info data and index; processing queries SetupAPI and
// CM fields; output is one HardwareDeviceNode with no child links yet.
HardwareDeviceNode PopulateNodeFromDevInfo(HDEVINFO set, SP_DEVINFO_DATA& info, int index) {
    HardwareDeviceNode node;
    node.index = index;
    node.devInst = info.DevInst;
    node.instanceId = DevInstToInstanceId(info.DevInst);
    node.parentInstanceId = QueryParentInstanceId(info.DevInst);
    node.displayName = QueryDeviceRegistryProperty(set, info, SPDRP_FRIENDLYNAME);
    if (node.displayName.empty()) {
        node.displayName = QueryDeviceRegistryProperty(set, info, SPDRP_DEVICEDESC);
    }
    node.className = QueryDeviceRegistryProperty(set, info, SPDRP_CLASS);
    node.manufacturer = QueryDeviceRegistryProperty(set, info, SPDRP_MFG);
    node.serviceName = QueryDeviceRegistryProperty(set, info, SPDRP_SERVICE);
    node.driverKey = QueryDeviceRegistryProperty(set, info, SPDRP_DRIVER);
    node.location = QueryDeviceRegistryProperty(set, info, SPDRP_LOCATION_INFORMATION);
    node.hardwareIds = QueryDeviceRegistryProperty(set, info, SPDRP_HARDWAREID);
    node.state = QueryStatus(info.DevInst, node.statusFlags, node.problemCode);
    return node;
}

// RebuildHierarchy links device nodes using parent instance IDs. Input is a flat
// device vector; processing fills parentIndex, depth, and childIndices fields; no
// value is returned.
void RebuildHierarchy(std::vector<HardwareDeviceNode>& devices) {
    std::map<std::wstring, int> byInstance;
    for (HardwareDeviceNode& node : devices) {
        node.parentIndex = -1;
        node.depth = 0;
        node.childIndices.clear();
        if (!node.instanceId.empty()) {
            byInstance[node.instanceId] = node.index;
        }
    }

    for (HardwareDeviceNode& node : devices) {
        if (node.parentInstanceId.empty()) {
            continue;
        }
        const auto found = byInstance.find(node.parentInstanceId);
        if (found != byInstance.end() && found->second != node.index) {
            node.parentIndex = found->second;
            devices[found->second].childIndices.push_back(node.index);
        }
    }

    for (HardwareDeviceNode& node : devices) {
        int depth = 0;
        int parent = node.parentIndex;
        while (parent >= 0 && parent < static_cast<int>(devices.size()) && depth < 64) {
            ++depth;
            parent = devices[parent].parentIndex;
        }
        node.depth = depth;
    }

    for (HardwareDeviceNode& node : devices) {
        std::sort(node.childIndices.begin(), node.childIndices.end(), [&devices](int left, int right) {
            return CompactDeviceName(devices[left]) < CompactDeviceName(devices[right]);
        });
    }
}

// FindDeviceByInstanceId opens a specific device into an existing set. Inputs are
// an HDEVINFO and instance ID; processing calls SetupDiOpenDeviceInfoW; output is
// true when infoData has been initialized for the matching device.
bool FindDeviceByInstanceId(HDEVINFO set, const std::wstring& instanceId, SP_DEVINFO_DATA& infoData) {
    infoData = {};
    infoData.cbSize = sizeof(infoData);
    return ::SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &infoData) == TRUE;
}

} // namespace

HardwareEnumerationResult EnumerateDeviceManagerTree() {
    HardwareEnumerationResult result;
    DevInfoSet set(::SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
    if (!set.valid()) {
        result.success = false;
        result.diagnosticText = L"SetupDiGetClassDevsW failed: " + Ksword::Core::LastErrorMessage();
        return result;
    }

    for (DWORD ordinal = 0;; ++ordinal) {
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (!::SetupDiEnumDeviceInfo(set.get(), ordinal, &info)) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) {
                break;
            }
            result.success = false;
            result.diagnosticText = L"SetupDiEnumDeviceInfo failed: " + Ksword::Core::LastErrorMessage(error);
            return result;
        }
        result.devices.push_back(PopulateNodeFromDevInfo(set.get(), info, static_cast<int>(result.devices.size())));
    }

    RebuildHierarchy(result.devices);
    std::sort(result.devices.begin(), result.devices.end(), [](const HardwareDeviceNode& left, const HardwareDeviceNode& right) {
        if (left.parentIndex != right.parentIndex) {
            return left.parentIndex < right.parentIndex;
        }
        return CompactDeviceName(left) < CompactDeviceName(right);
    });
    for (int i = 0; i < static_cast<int>(result.devices.size()); ++i) {
        result.devices[i].index = i;
    }
    RebuildHierarchy(result.devices);

    result.success = true;
    result.diagnosticText = L"OK";
    return result;
}

HardwareDeviceDetail QueryDeviceManagerDetails(const std::wstring& instanceId) {
    HardwareDeviceDetail detail;
    detail.instanceId = instanceId;
    if (instanceId.empty()) {
        return detail;
    }

    DevInfoSet set(::SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
    if (!set.valid()) {
        return detail;
    }

    SP_DEVINFO_DATA info{};
    if (!FindDeviceByInstanceId(set.get(), instanceId, info)) {
        return detail;
    }

    HardwareDeviceNode node = PopulateNodeFromDevInfo(set.get(), info, 0);
    detail.found = true;
    detail.title = CompactDeviceName(node);
    detail.instanceId = node.instanceId;
    AppendProperty(detail, L"Display name", CompactDeviceName(node));
    AppendProperty(detail, L"Instance ID", node.instanceId);
    AppendProperty(detail, L"Parent instance ID", node.parentInstanceId);
    AppendProperty(detail, L"Class", node.className);
    AppendProperty(detail, L"Manufacturer", node.manufacturer);
    AppendProperty(detail, L"Service", node.serviceName);
    AppendProperty(detail, L"Driver key", node.driverKey);
    AppendProperty(detail, L"Location", node.location);
    AppendProperty(detail, L"Hardware IDs", node.hardwareIds);
    AppendProperty(detail, L"State", DeviceStateText(node.state, node.problemCode));
    AppendProperty(detail, L"Status flags", L"0x" + std::to_wstring(node.statusFlags));
    AppendProperty(detail, L"Problem code", std::to_wstring(node.problemCode));
    return detail;
}

} // namespace Ksword::Features::Hardware
