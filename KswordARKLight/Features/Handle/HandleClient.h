#pragma once

#include "../../Core/Win32Lean.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Features::Handle {

// HandleEntryView is the page-local snapshot row for one R0 HandleTable entry.
// Inputs are filled from KSWORD_ARK_HANDLE_ENTRY; processing code only formats
// these values for read-only UI; return behavior is value-copy semantics.
struct HandleEntryView {
    std::uint32_t processId = 0;
    std::uint32_t handleValue = 0;
    std::uint32_t fieldFlags = 0;
    std::uint32_t decodeStatus = 0;
    std::uint32_t grantedAccess = 0;
    std::uint32_t attributes = 0;
    std::uint32_t objectTypeIndex = 0;
    std::uint32_t objectHeaderDecodeStatus = 0;
    long objectHeaderReadStatus = 0;
    std::uint32_t grantedAccessDecodeStatus = 0;
    long grantedAccessReadStatus = 0;
    std::uint32_t objectTypeIndexSource = 0;
    std::uint32_t objectTypeNameSource = 0;
    std::uint32_t nameInfoStatus = 0;
    std::uint32_t objectHeaderTypeIndex = 0;
    std::uint32_t objectHeaderInfoMask = 0;
    std::uint32_t objectHeaderFlags = 0;
    std::uint32_t objectHeaderTraceFlags = 0;
    std::int64_t pointerCount = 0;
    std::uint64_t handleCount = 0;
    std::uint64_t objectAddress = 0;
    std::uint64_t objectHeaderAddress = 0;
    std::uint64_t objectTypeAddress = 0;
    std::uint64_t dynDataCapabilityMask = 0;
    std::uint32_t epObjectTableOffset = 0;
    std::uint32_t htHandleContentionEventOffset = 0;
    std::uint32_t obDecodeShift = 0;
    std::uint32_t obAttributesShift = 0;
    std::uint32_t otNameOffset = 0;
    std::uint32_t otIndexOffset = 0;
};

// HandleObjectDetailView is the detail snapshot for a selected handle. Inputs
// come from KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE; processing preserves both
// object identity and ObjectHeader/ObjectType diagnostics; return behavior is
// value-copy semantics.
struct HandleObjectDetailView {
    ksword::ark::IoResult io;
    std::uint32_t version = 0;
    std::uint32_t processId = 0;
    std::uint32_t fieldFlags = 0;
    std::uint64_t handleValue = 0;
    std::uint64_t objectAddress = 0;
    std::uint32_t objectTypeIndex = 0;
    std::uint32_t queryStatus = 0;
    long objectReferenceStatus = 0;
    long typeStatus = 0;
    long nameStatus = 0;
    std::uint32_t proxyStatus = 0;
    long proxyNtStatus = 0;
    std::uint32_t proxyPolicyFlags = 0;
    std::uint32_t requestedAccess = 0;
    std::uint32_t actualGrantedAccess = 0;
    std::uint64_t proxyHandle = 0;
    std::uint64_t dynDataCapabilityMask = 0;
    std::uint32_t otNameOffset = 0;
    std::uint32_t otIndexOffset = 0;
    std::uint32_t objectHeaderDecodeStatus = 0;
    long objectHeaderReadStatus = 0;
    std::uint32_t grantedAccessDecodeStatus = 0;
    long grantedAccessReadStatus = 0;
    std::uint32_t objectTypeIndexSource = 0;
    std::uint32_t objectTypeNameSource = 0;
    std::uint32_t nameInfoStatus = 0;
    std::uint32_t objectHeaderTypeIndex = 0;
    std::uint32_t objectHeaderInfoMask = 0;
    std::uint32_t objectHeaderFlags = 0;
    std::uint32_t objectHeaderTraceFlags = 0;
    std::int64_t pointerCount = 0;
    std::uint64_t handleCount = 0;
    std::uint64_t objectHeaderAddress = 0;
    std::uint64_t objectTypeAddress = 0;
    std::wstring typeName;
    std::wstring objectName;
    bool alpcQueried = false;
    ksword::ark::AlpcPortQueryResult alpc;
};

// HandleEnumView is the page-local response for one process HandleTable query.
// Inputs are populated by HandleAuditClient; processing stores protocol status,
// parsed rows, and transport diagnostics; return behavior is value-copy.
struct HandleEnumView {
    ksword::ark::IoResult io;
    std::uint32_t version = 0;
    std::uint32_t totalCount = 0;
    std::uint32_t returnedCount = 0;
    std::uint32_t processId = 0;
    std::uint32_t overallStatus = 0;
    long lastStatus = 0;
    std::vector<HandleEntryView> entries;
};

// HandleAuditClient is a local R3 adapter for this unregistered Light module.
// It reuses the strong ArkDriverClient handle-query methods, so the page never
// opens the device or sends generic IOCTL packets directly.
class HandleAuditClient final {
public:
    // EnumerateProcessHandles queries one process HandleTable. Input is a PID;
    // processing calls ArkDriverClient::enumerateProcessHandles; output
    // contains typed entries or IO diagnostics.
    HandleEnumView EnumerateProcessHandles(std::uint32_t processId) const;

    // QueryHandleObject requests ObjectHeader/ObjectType/object-name details for
    // one selected handle. Inputs are PID and handle value; processing does not
    // request a user proxy handle; output is a read-only detail snapshot.
    HandleObjectDetailView QueryHandleObject(std::uint32_t processId, std::uint64_t handleValue) const;
};

} // namespace Ksword::Features::Handle
