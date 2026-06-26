#include "DriverMemoryClient.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>

namespace Ksword::Features::Memory {
namespace {

// MemoryReadStatusText maps the shared driver read status into stable UI text.
// Input is KSWORD_ARK_MEMORY_READ_STATUS_* from shared/driver; output is a short
// label that supplements ArkDriverClient's transport diagnostic message.
const wchar_t* MemoryReadStatusText(const std::uint32_t status) {
    switch (status) {
    case KSWORD_ARK_MEMORY_READ_STATUS_OK: return L"OK";
    case KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY: return L"Partial copy";
    case KSWORD_ARK_MEMORY_READ_STATUS_PROCESS_LOOKUP_FAILED: return L"Process lookup failed";
    case KSWORD_ARK_MEMORY_READ_STATUS_COPY_FAILED: return L"Copy failed";
    case KSWORD_ARK_MEMORY_READ_STATUS_RANGE_REJECTED: return L"Range rejected";
    case KSWORD_ARK_MEMORY_READ_STATUS_BUFFER_TOO_SMALL: return L"Buffer too small";
    case KSWORD_ARK_MEMORY_READ_STATUS_ZERO_FILLED: return L"Zero-filled unreadable";
    case KSWORD_ARK_MEMORY_READ_STATUS_UNAVAILABLE:
    default:
        return L"Unavailable";
    }
}

// MemoryWriteStatusText maps the shared driver write status into stable UI
// text. Input is KSWORD_ARK_MEMORY_WRITE_STATUS_*; output is a short label.
const wchar_t* MemoryWriteStatusText(const std::uint32_t status) {
    switch (status) {
    case KSWORD_ARK_MEMORY_WRITE_STATUS_OK: return L"OK";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_PARTIAL_COPY: return L"Partial copy";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_PROCESS_LOOKUP_FAILED: return L"Process lookup failed";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_COPY_FAILED: return L"Copy failed";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_RANGE_REJECTED: return L"Range rejected";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_BUFFER_TOO_SMALL: return L"Buffer too small";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_ACCESS_DENIED: return L"Access denied";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_FORCE_REQUIRED: return L"Force required";
    case KSWORD_ARK_MEMORY_WRITE_STATUS_UNAVAILABLE:
    default:
        return L"Unavailable";
    }
}

// NarrowToWide converts ArkDriverClient's narrow diagnostic messages into the
// Win32-light UI's UTF-16 status text. Input is ASCII/UTF-8-like diagnostic
// text; processing widens byte-for-byte because current client messages are
// English diagnostics; output is displayable UTF-16 text.
std::wstring NarrowToWide(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const unsigned char ch : text) {
        wide.push_back(static_cast<wchar_t>(ch));
    }
    return wide;
}

// FormatReadStatus creates the final read status line. Inputs are the original
// request and ArkDriverClient result; processing combines Win32, NT, protocol
// and byte-count fields; output is shown in the memory page status box.
std::wstring FormatReadStatus(
    const DriverMemoryReadRequest& request,
    const ksword::ark::VirtualMemoryReadResult& driverResult) {
    std::wostringstream stream;
    stream << L"R0 read "
           << (driverResult.io.ok ? L"transport OK" : L"transport failed")
           << L"; protocol=" << MemoryReadStatusText(driverResult.readStatus)
           << L"; pid=" << request.processId
           << L"; address=0x" << std::hex << std::uppercase << request.address
           << std::dec
           << L"; requested=" << request.length
           << L"; bytesRead=" << driverResult.bytesRead
           << L"; win32=" << driverResult.io.win32Error
           << L"; nt=0x" << std::hex << static_cast<unsigned long>(driverResult.copyStatus)
           << L"; fields=0x" << driverResult.fieldFlags
           << L"; " << NarrowToWide(driverResult.io.message);
    return stream.str();
}

// FormatWriteStatus creates the final write status line. Inputs are the request
// and ArkDriverClient result; processing combines transport/protocol/byte
// counts; output is shown in the memory page status box.
std::wstring FormatWriteStatus(
    const DriverMemoryWriteRequest& request,
    const ksword::ark::VirtualMemoryWriteResult& driverResult) {
    std::wostringstream stream;
    stream << L"R0 write "
           << (driverResult.io.ok ? L"transport OK" : L"transport failed")
           << L"; protocol=" << MemoryWriteStatusText(driverResult.writeStatus)
           << L"; pid=" << request.processId
           << L"; address=0x" << std::hex << std::uppercase << request.address
           << std::dec
           << L"; requested=" << request.bytes.size()
           << L"; bytesWritten=" << driverResult.bytesWritten
           << L"; win32=" << driverResult.io.win32Error
           << L"; nt=0x" << std::hex << static_cast<unsigned long>(driverResult.copyStatus)
           << L"; fields=0x" << driverResult.fieldFlags
           << L"; " << NarrowToWide(driverResult.io.message);
    return stream.str();
}

// MakeReadValidationError returns a failed local result before any IOCTL is
// sent. Input is the user request and message; output is a consistent UI result.
DriverMemoryReadResult MakeReadValidationError(const DriverMemoryReadRequest& request, const std::wstring& message) {
    DriverMemoryReadResult result;
    result.success = false;
    result.win32Error = ERROR_INVALID_PARAMETER;
    result.statusText = message + L" Requested " + std::to_wstring(request.length) + L" byte(s).";
    return result;
}

// MakeWriteValidationError returns a failed local result before any IOCTL is
// sent. Input is the user request and message; output is a consistent UI result.
DriverMemoryWriteResult MakeWriteValidationError(const DriverMemoryWriteRequest& request, const std::wstring& message) {
    DriverMemoryWriteResult result;
    result.success = false;
    result.win32Error = ERROR_INVALID_PARAMETER;
    result.bytesWritten = 0;
    result.statusText = message + L" Payload " + std::to_wstring(request.bytes.size()) + L" byte(s).";
    return result;
}

} // namespace

DriverMemoryClient::DriverMemoryClient() = default;

DriverMemoryClient::~DriverMemoryClient() = default;

DriverMemoryReadResult DriverMemoryClient::ReadMemory(const DriverMemoryReadRequest& request) {
    if (request.processId == 0 || request.length == 0) {
        return MakeReadValidationError(request, L"PID and length must be non-zero.");
    }
    if (request.length > KSWORD_ARK_MEMORY_READ_MAX_BYTES) {
        return MakeReadValidationError(request, L"Read length exceeds shared driver limit.");
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::VirtualMemoryReadResult driverResult = client.readVirtualMemory(
        static_cast<std::uint32_t>(request.processId),
        request.address,
        static_cast<std::uint32_t>(request.length),
        0);

    DriverMemoryReadResult result;
    result.success = driverResult.io.ok &&
        (driverResult.readStatus == KSWORD_ARK_MEMORY_READ_STATUS_OK ||
            driverResult.readStatus == KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY) &&
        !driverResult.data.empty();
    result.win32Error = driverResult.io.win32Error;
    result.bytes = driverResult.data;
    result.statusText = FormatReadStatus(request, driverResult);
    return result;
}

DriverMemoryWriteResult DriverMemoryClient::WriteMemory(const DriverMemoryWriteRequest& request) {
    if (request.processId == 0 || request.bytes.empty()) {
        return MakeWriteValidationError(request, L"PID and payload must be non-zero.");
    }
    if (request.bytes.size() > KSWORD_ARK_MEMORY_WRITE_MAX_BYTES) {
        return MakeWriteValidationError(request, L"Write payload exceeds shared driver limit.");
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::VirtualMemoryWriteResult driverResult = client.writeVirtualMemory(
        static_cast<std::uint32_t>(request.processId),
        request.address,
        request.bytes,
        KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED);

    DriverMemoryWriteResult result;
    result.success = driverResult.io.ok &&
        (driverResult.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_OK ||
            driverResult.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_PARTIAL_COPY) &&
        driverResult.bytesWritten > 0;
    result.win32Error = driverResult.io.win32Error;
    result.bytesWritten = driverResult.bytesWritten;
    result.statusText = FormatWriteStatus(request, driverResult);
    return result;
}

} // namespace Ksword::Features::Memory
