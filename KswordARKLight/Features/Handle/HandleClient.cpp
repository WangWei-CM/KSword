#include "HandleClient.h"

#include "../../../shared/driver/KswordArkHandleIoctl.h"

#include <algorithm>
#include <sstream>

namespace Ksword::Features::Handle {
namespace {

constexpr long kNtStatusInvalidParameter = -1073741811L;

// FixedWideString copies a bounded UTF-16 protocol buffer. Inputs are the fixed
// array pointer and maximum character count; processing stops at the first NUL
// or maxChars; output is a safe std::wstring for UI rendering.
std::wstring FixedWideString(const wchar_t* text, const std::size_t maxChars) {
    if (!text || maxChars == 0U) {
        return {};
    }
    std::size_t length = 0U;
    while (length < maxChars && text[length] != L'\0') {
        ++length;
    }
    return std::wstring(text, text + length);
}

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

// CopyEntry maps one shared-protocol entry into the page-local view model.
// Input is a KSWORD_ARK_HANDLE_ENTRY pointer already bounds-checked by caller;
// output is a HandleEntryView preserving all currently defined audit fields.
HandleEntryView CopyEntry(const KSWORD_ARK_HANDLE_ENTRY& entry) {
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
    out.objectHeaderDecodeStatus = static_cast<std::uint32_t>(entry.objectHeaderDecodeStatus);
    out.objectHeaderReadStatus = static_cast<long>(entry.objectHeaderReadStatus);
    out.grantedAccessDecodeStatus = static_cast<std::uint32_t>(entry.grantedAccessDecodeStatus);
    out.grantedAccessReadStatus = static_cast<long>(entry.grantedAccessReadStatus);
    out.objectTypeIndexSource = static_cast<std::uint32_t>(entry.objectTypeIndexSource);
    out.objectTypeNameSource = static_cast<std::uint32_t>(entry.objectTypeNameSource);
    out.nameInfoStatus = static_cast<std::uint32_t>(entry.nameInfoStatus);
    out.objectHeaderTypeIndex = static_cast<std::uint32_t>(entry.objectHeaderTypeIndex);
    out.objectHeaderInfoMask = static_cast<std::uint32_t>(entry.objectHeaderInfoMask);
    out.objectHeaderFlags = static_cast<std::uint32_t>(entry.objectHeaderFlags);
    out.objectHeaderTraceFlags = static_cast<std::uint32_t>(entry.objectHeaderTraceFlags);
    out.pointerCount = static_cast<std::int64_t>(entry.pointerCount);
    out.handleCount = static_cast<std::uint64_t>(entry.handleCount);
    out.objectHeaderAddress = static_cast<std::uint64_t>(entry.objectHeaderAddress);
    out.objectTypeAddress = static_cast<std::uint64_t>(entry.objectTypeAddress);
    return out;
}

} // namespace

HandleEnumView HandleAuditClient::EnumerateProcessHandles(const std::uint32_t processId) const {
    // Input is one target process id. Processing uses the central ArkDriverClient
    // IOCTL helper and parses the versioned variable-length response defensively.
    // Output keeps partial/transport failures visible to the page.
    if (processId == 0U) {
        return MakeInvalidPidResult(processId);
    }

    KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST request{};
    request.flags = KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL;
    request.processId = processId;

    std::vector<std::uint8_t> responseBuffer(1024U * 1024U, 0U);
    HandleEnumView result;
    const ksword::ark::DriverClient client;
    ksword::ark::DriverHandle driverHandle = client.open(GENERIC_READ);
    result.io = client.deviceIoControl(
        IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES,
        &request,
        static_cast<unsigned long>(sizeof(request)),
        responseBuffer.data(),
        static_cast<unsigned long>(responseBuffer.size()),
        &driverHandle);
    if (!result.io.ok) {
        result.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES) failed, error=" +
            std::to_string(result.io.win32Error);
        return result;
    }

    constexpr std::size_t headerSize =
        sizeof(KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE) - sizeof(KSWORD_ARK_HANDLE_ENTRY);
    if (result.io.bytesReturned < headerSize) {
        result.io.ok = false;
        result.io.message = "enum-process-handles response too small, bytesReturned=" +
            std::to_string(result.io.bytesReturned);
        return result;
    }

    const auto* header =
        reinterpret_cast<const KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE*>(responseBuffer.data());
    if (header->entrySize < sizeof(KSWORD_ARK_HANDLE_ENTRY)) {
        result.io.ok = false;
        result.io.message = "enum-process-handles entry size invalid, entrySize=" +
            std::to_string(header->entrySize);
        return result;
    }

    result.version = static_cast<std::uint32_t>(header->version);
    result.totalCount = static_cast<std::uint32_t>(header->totalCount);
    result.returnedCount = static_cast<std::uint32_t>(header->returnedCount);
    result.processId = static_cast<std::uint32_t>(header->processId);
    result.overallStatus = static_cast<std::uint32_t>(header->overallStatus);
    result.lastStatus = static_cast<long>(header->lastStatus);

    const std::size_t availableCount =
        (result.io.bytesReturned - headerSize) / static_cast<std::size_t>(header->entrySize);
    const std::size_t parsedCount =
        std::min<std::size_t>(static_cast<std::size_t>(header->returnedCount), availableCount);
    result.entries.reserve(parsedCount);
    for (std::size_t index = 0; index < parsedCount; ++index) {
        const std::size_t entryOffset = headerSize + (index * static_cast<std::size_t>(header->entrySize));
        const auto* entry = reinterpret_cast<const KSWORD_ARK_HANDLE_ENTRY*>(responseBuffer.data() + entryOffset);
        result.entries.push_back(CopyEntry(*entry));
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
    // Inputs identify one process handle. Processing asks R0 for metadata only:
    // type name, object name, ObjectHeader counters, and access diagnostics. It
    // intentionally does not request a proxy handle so the UI remains read-only.
    // Output is the parsed fixed-size response plus IO status.
    HandleObjectDetailView result;
    KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST request{};
    KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE response{};
    request.flags = KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL;
    request.processId = processId;
    request.handleValue = handleValue;
    request.requestedAccess = 0U;

    const ksword::ark::DriverClient client;
    ksword::ark::DriverHandle driverHandle = client.open(GENERIC_READ);
    result.io = client.deviceIoControl(
        IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT,
        &request,
        static_cast<unsigned long>(sizeof(request)),
        &response,
        static_cast<unsigned long>(sizeof(response)),
        &driverHandle);
    if (!result.io.ok) {
        result.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT) failed, error=" +
            std::to_string(result.io.win32Error);
        return result;
    }
    if (result.io.bytesReturned < sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE)) {
        result.io.ok = false;
        result.io.message = "query-handle-object response too small, bytesReturned=" +
            std::to_string(result.io.bytesReturned);
        return result;
    }

    result.version = static_cast<std::uint32_t>(response.version);
    result.processId = static_cast<std::uint32_t>(response.processId);
    result.fieldFlags = static_cast<std::uint32_t>(response.fieldFlags);
    result.handleValue = static_cast<std::uint64_t>(response.handleValue);
    result.objectAddress = static_cast<std::uint64_t>(response.objectAddress);
    result.objectTypeIndex = static_cast<std::uint32_t>(response.objectTypeIndex);
    result.queryStatus = static_cast<std::uint32_t>(response.queryStatus);
    result.objectReferenceStatus = static_cast<long>(response.objectReferenceStatus);
    result.typeStatus = static_cast<long>(response.typeStatus);
    result.nameStatus = static_cast<long>(response.nameStatus);
    result.proxyStatus = static_cast<std::uint32_t>(response.proxyStatus);
    result.proxyNtStatus = static_cast<long>(response.proxyNtStatus);
    result.proxyPolicyFlags = static_cast<std::uint32_t>(response.proxyPolicyFlags);
    result.requestedAccess = static_cast<std::uint32_t>(response.requestedAccess);
    result.actualGrantedAccess = static_cast<std::uint32_t>(response.actualGrantedAccess);
    result.proxyHandle = static_cast<std::uint64_t>(response.proxyHandle);
    result.dynDataCapabilityMask = static_cast<std::uint64_t>(response.dynDataCapabilityMask);
    result.otNameOffset = static_cast<std::uint32_t>(response.otNameOffset);
    result.otIndexOffset = static_cast<std::uint32_t>(response.otIndexOffset);
    result.objectHeaderDecodeStatus = static_cast<std::uint32_t>(response.objectHeaderDecodeStatus);
    result.objectHeaderReadStatus = static_cast<long>(response.objectHeaderReadStatus);
    result.grantedAccessDecodeStatus = static_cast<std::uint32_t>(response.grantedAccessDecodeStatus);
    result.grantedAccessReadStatus = static_cast<long>(response.grantedAccessReadStatus);
    result.objectTypeIndexSource = static_cast<std::uint32_t>(response.objectTypeIndexSource);
    result.objectTypeNameSource = static_cast<std::uint32_t>(response.objectTypeNameSource);
    result.nameInfoStatus = static_cast<std::uint32_t>(response.nameInfoStatus);
    result.objectHeaderTypeIndex = static_cast<std::uint32_t>(response.objectHeaderTypeIndex);
    result.objectHeaderInfoMask = static_cast<std::uint32_t>(response.objectHeaderInfoMask);
    result.objectHeaderFlags = static_cast<std::uint32_t>(response.objectHeaderFlags);
    result.objectHeaderTraceFlags = static_cast<std::uint32_t>(response.objectHeaderTraceFlags);
    result.pointerCount = static_cast<std::int64_t>(response.pointerCount);
    result.handleCount = static_cast<std::uint64_t>(response.handleCount);
    result.objectHeaderAddress = static_cast<std::uint64_t>(response.objectHeaderAddress);
    result.objectTypeAddress = static_cast<std::uint64_t>(response.objectTypeAddress);
    result.typeName = FixedWideString(response.typeName, KSWORD_ARK_OBJECT_TYPE_NAME_CHARS);
    result.objectName = FixedWideString(response.objectName, KSWORD_ARK_OBJECT_NAME_CHARS);

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
