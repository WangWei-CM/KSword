#include "HandleClient.h"

#include "../../../shared/driver/KswordArkHandleIoctl.h"

#include <algorithm>
#include <cwctype>
#include <sstream>

namespace Ksword::Features::Handle {
namespace {

constexpr long kNtStatusInvalidParameter = -1073741811L;

// MakeInvalidPidResult creates a local validation failure without touching R0.
// Input is the rejected PID; processing mirrors ArkDriverClient status style;
// output is a HandleEnumView with io.ok=false.
HandleEnumView MakeInvalidPidResult(const std::uint32_t processId) {
    HandleEnumView result;
    result.processId = processId;
    result.overallStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_UNAVAILABLE;
    result.lastStatus = kNtStatusInvalidParameter;
    result.io.ok = false;
    result.io.win32Error = ERROR_INVALID_PARAMETER;
    result.io.ntStatus = kNtStatusInvalidParameter;
    result.io.message = "handle audit requires a non-zero target pid";
    return result;
}

// CopyEntry maps the strong ArkDriverClient result into the page-local view.
// New protocol-only diagnostics remain zero/default until they are exposed by
// ArkDriverClient; this keeps the Light UI on the supported public contract.
HandleEntryView CopyEntry(const ksword::ark::HandleEntry& entry) {
    HandleEntryView out;
    out.processId = static_cast<std::uint32_t>(entry.processId);
    out.handleValue = static_cast<std::uint32_t>(entry.handleValue);
    out.fieldFlags = static_cast<std::uint32_t>(entry.fieldFlags);
    out.decodeStatus = static_cast<std::uint32_t>(entry.decodeStatus);
    out.grantedAccess = static_cast<std::uint32_t>(entry.grantedAccess);
    out.attributes = static_cast<std::uint32_t>(entry.attributes);
    out.objectTypeIndex = static_cast<std::uint32_t>(entry.objectTypeIndex);
    out.objectAddress = static_cast<std::uint64_t>(entry.objectAddress);
    out.dynDataCapabilityMask = static_cast<std::uint64_t>(entry.dynDataCapabilityMask);
    out.epObjectTableOffset = static_cast<std::uint32_t>(entry.epObjectTableOffset);
    out.htHandleContentionEventOffset = static_cast<std::uint32_t>(entry.htHandleContentionEventOffset);
    out.obDecodeShift = static_cast<std::uint32_t>(entry.obDecodeShift);
    out.obAttributesShift = static_cast<std::uint32_t>(entry.obAttributesShift);
    out.otNameOffset = static_cast<std::uint32_t>(entry.otNameOffset);
    out.otIndexOffset = static_cast<std::uint32_t>(entry.otIndexOffset);
    return out;
}

// IsAlpcPortType decides whether the selected object can use the dedicated
// ALPC parser. The input is ObjectType text produced by the typed object query;
// output is false for every non-port object, avoiding unrelated extra IOCTLs.
bool IsAlpcPortType(const std::wstring& typeName) {
    std::wstring normalized;
    normalized.reserve(typeName.size());
    for (const wchar_t character : typeName) {
        normalized.push_back(static_cast<wchar_t>(std::towupper(character)));
    }
    return normalized.find(L"ALPC") != std::wstring::npos &&
        normalized.find(L"PORT") != std::wstring::npos;
}

} // namespace

HandleEnumView HandleAuditClient::EnumerateProcessHandles(const std::uint32_t processId) const {
    // Input is one target process id. Processing uses the typed ArkDriverClient
    // handle API. Output keeps partial/transport failures visible to the page.
    if (processId == 0U) {
        return MakeInvalidPidResult(processId);
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::HandleEnumResult typed = client.enumerateProcessHandles(
        processId,
        KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL);
    HandleEnumView result;
    result.io = typed.io;
    result.version = typed.version;
    result.totalCount = typed.totalCount;
    result.returnedCount = typed.returnedCount;
    result.processId = typed.processId;
    result.overallStatus = typed.overallStatus;
    result.lastStatus = typed.lastStatus;
    result.entries.reserve(typed.entries.size());
    for (const ksword::ark::HandleEntry& entry : typed.entries) {
        result.entries.push_back(CopyEntry(entry));
    }

    std::ostringstream stream;
    stream << "version=" << result.version
           << ", pid=" << result.processId
           << ", total=" << result.totalCount
           << ", returned=" << result.returnedCount
           << ", parsed=" << result.entries.size()
           << ", overallStatus=" << result.overallStatus
           << ", bytesReturned=" << result.io.bytesReturned;
    result.io.message = stream.str();
    return result;
}

HandleObjectDetailView HandleAuditClient::QueryHandleObject(
    const std::uint32_t processId,
    const std::uint64_t handleValue) const {
    // Inputs identify one process handle. Processing asks R0 for the metadata
    // currently exposed by the strong client: type name, object name and access
    // diagnostics. It intentionally does not request a proxy handle.
    HandleObjectDetailView result;
    const ksword::ark::DriverClient client;
    const ksword::ark::HandleObjectQueryResult typed = client.queryHandleObject(
        processId,
        handleValue,
        KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL,
        0U);
    result.io = typed.io;
    result.version = typed.version;
    result.processId = typed.processId;
    result.fieldFlags = typed.fieldFlags;
    result.handleValue = typed.handleValue;
    result.objectAddress = typed.objectAddress;
    result.objectTypeIndex = typed.objectTypeIndex;
    result.queryStatus = typed.queryStatus;
    result.objectReferenceStatus = typed.objectReferenceStatus;
    result.typeStatus = typed.typeStatus;
    result.nameStatus = typed.nameStatus;
    result.proxyStatus = typed.proxyStatus;
    result.proxyNtStatus = typed.proxyNtStatus;
    result.proxyPolicyFlags = typed.proxyPolicyFlags;
    result.requestedAccess = typed.requestedAccess;
    result.actualGrantedAccess = typed.actualGrantedAccess;
    result.proxyHandle = typed.proxyHandle;
    result.dynDataCapabilityMask = typed.dynDataCapabilityMask;
    result.otNameOffset = typed.otNameOffset;
    result.otIndexOffset = typed.otIndexOffset;
    result.typeName = typed.typeName;
    result.objectName = typed.objectName;
    if (typed.io.ok && IsAlpcPortType(result.typeName)) {
        result.alpcQueried = true;
        result.alpc = client.queryAlpcPort(processId, handleValue);
    }

    std::ostringstream stream;
    stream << "version=" << result.version
           << ", pid=" << result.processId
           << ", handle=0x" << std::hex << std::uppercase << result.handleValue
           << ", queryStatus=" << std::dec << result.queryStatus
           << ", bytesReturned=" << result.io.bytesReturned;
    result.io.message = stream.str();
    return result;
}

} // namespace Ksword::Features::Handle
