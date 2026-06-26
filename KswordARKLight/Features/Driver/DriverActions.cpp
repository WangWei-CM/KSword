#include "DriverActions.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Driver {
namespace {

// AppendTsvRow joins one row with tabs and a trailing CRLF. Inputs are an
// output string and column cells; processing sanitizes each cell first; no value
// is returned because the result is accumulated into the output buffer.
void AppendTsvRow(std::wstring& output, const std::vector<std::wstring>& cells) {
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (index != 0) {
            output.push_back(L'\t');
        }
        output += DriverModel::sanitizeTsvCell(cells[index]);
    }
    output += L"\r\n";
}

// Utf8ToWide converts ArkDriverClient's narrow diagnostic messages into UI
// text. Input is a UTF-8 or byte-oriented string; processing tries strict UTF-8
// first and falls back byte-by-byte; output is always safe UTF-16 text.
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int required = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (required > 0) {
        std::wstring wide(static_cast<std::size_t>(required), L'\0');
        ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.c_str(),
            static_cast<int>(text.size()),
            wide.data(),
            required);
        return wide;
    }

    std::wstring fallback;
    fallback.reserve(text.size());
    for (const unsigned char ch : text) {
        fallback.push_back(static_cast<wchar_t>(ch));
    }
    return fallback;
}

// HexText formats protocol addresses and bitfields. Input is an integer value;
// processing uses uppercase hexadecimal without locale-specific formatting;
// output is compact diagnostic text.
std::wstring HexText(const std::uint64_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << value;
    return stream.str();
}

// NtStatusText formats signed NTSTATUS values while preserving their low
// 32-bit representation. Input is a signed long from the shared protocol;
// output is a diagnostic hexadecimal string.
std::wstring NtStatusText(const long status) {
    return HexText(static_cast<std::uint32_t>(status));
}

// DriverObjectQueryStatusText maps the shared DriverObject query status to a
// short label. Input is a KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_* value; output
// is display text for the detail popup.
const wchar_t* DriverObjectQueryStatusText(const std::uint32_t status) {
    switch (status) {
    case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK: return L"OK";
    case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL: return L"Partial";
    case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NAME_INVALID: return L"Name invalid";
    case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NOT_FOUND: return L"Not found";
    case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_REFERENCE_FAILED: return L"Reference failed";
    case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_BUFFER_TOO_SMALL: return L"Buffer too small";
    case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_QUERY_FAILED: return L"Query failed";
    default: return L"Unavailable";
    }
}

// DriverUnloadStatusText maps the shared force-unload aggregate status to a
// concise label. Input is a KSWORD_ARK_DRIVER_UNLOAD_STATUS_* value; output is
// display text for confirmation/results.
const wchar_t* DriverUnloadStatusText(const std::uint32_t status) {
    switch (status) {
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED: return L"Unloaded";
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOAD_ROUTINE_MISSING: return L"Unload routine missing";
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_REFERENCE_FAILED: return L"Reference failed";
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_THREAD_FAILED: return L"Thread failed";
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_WAIT_TIMEOUT: return L"Wait timeout";
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED: return L"Operation failed";
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP: return L"Forced cleanup";
    case KSWORD_ARK_DRIVER_UNLOAD_STATUS_CLEANUP_FAILED: return L"Cleanup failed";
    default: return L"Unknown";
    }
}

// DriverUnloadSucceeded reports whether the R0 response represents a completed
// unload path. Input is the aggregate status; output controls the result's
// success flag without hiding the detailed status text.
bool DriverUnloadSucceeded(const std::uint32_t status) {
    return status == KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED ||
        status == KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP;
}

// MajorFunctionName returns a standard IRP_MJ_* name for one dispatch slot.
// Input is a major-function index; output falls back to a numeric label when
// the value is not in the common Windows range.
std::wstring MajorFunctionName(const std::uint32_t value) {
    switch (value) {
    case 0x00: return L"IRP_MJ_CREATE";
    case 0x01: return L"IRP_MJ_CREATE_NAMED_PIPE";
    case 0x02: return L"IRP_MJ_CLOSE";
    case 0x03: return L"IRP_MJ_READ";
    case 0x04: return L"IRP_MJ_WRITE";
    case 0x05: return L"IRP_MJ_QUERY_INFORMATION";
    case 0x06: return L"IRP_MJ_SET_INFORMATION";
    case 0x07: return L"IRP_MJ_QUERY_EA";
    case 0x08: return L"IRP_MJ_SET_EA";
    case 0x09: return L"IRP_MJ_FLUSH_BUFFERS";
    case 0x0A: return L"IRP_MJ_QUERY_VOLUME_INFORMATION";
    case 0x0B: return L"IRP_MJ_SET_VOLUME_INFORMATION";
    case 0x0C: return L"IRP_MJ_DIRECTORY_CONTROL";
    case 0x0D: return L"IRP_MJ_FILE_SYSTEM_CONTROL";
    case 0x0E: return L"IRP_MJ_DEVICE_CONTROL";
    case 0x0F: return L"IRP_MJ_INTERNAL_DEVICE_CONTROL";
    case 0x10: return L"IRP_MJ_SHUTDOWN";
    case 0x11: return L"IRP_MJ_LOCK_CONTROL";
    case 0x12: return L"IRP_MJ_CLEANUP";
    case 0x13: return L"IRP_MJ_CREATE_MAILSLOT";
    case 0x14: return L"IRP_MJ_QUERY_SECURITY";
    case 0x15: return L"IRP_MJ_SET_SECURITY";
    case 0x16: return L"IRP_MJ_POWER";
    case 0x17: return L"IRP_MJ_SYSTEM_CONTROL";
    case 0x18: return L"IRP_MJ_DEVICE_CHANGE";
    case 0x19: return L"IRP_MJ_QUERY_QUOTA";
    case 0x1A: return L"IRP_MJ_SET_QUOTA";
    case 0x1B: return L"IRP_MJ_PNP";
    default: return L"IRP_MJ_" + std::to_wstring(value);
    }
}

// AppendLine appends one labelled line to a detail string. Inputs are output,
// label and value; processing keeps CRLF formatting consistent; no value is
// returned because the output buffer is mutated in place.
void AppendLine(std::wstring& text, const std::wstring& label, const std::wstring& value) {
    text += label;
    text += L": ";
    text += value;
    text += L"\r\n";
}

// BuildUnloadResultText formats a DriverForceUnloadResult. Input is the parsed
// ArkDriverClient response; processing preserves IO, NTSTATUS and cleanup
// diagnostics; output is suitable for a MessageBox and status bar.
std::wstring BuildUnloadResultText(const ksword::ark::DriverForceUnloadResult& unload) {
    std::wstring text;
    AppendLine(text, L"IO", unload.io.ok ? L"OK" : L"FAIL");
    AppendLine(text, L"Win32", std::to_wstring(unload.io.win32Error));
    AppendLine(text, L"BytesReturned", std::to_wstring(unload.io.bytesReturned));
    AppendLine(text, L"Status", std::wstring(DriverUnloadStatusText(unload.status)) + L" (" + std::to_wstring(unload.status) + L")");
    AppendLine(text, L"DriverName", unload.driverName);
    AppendLine(text, L"DriverObject", HexText(unload.driverObjectAddress));
    AppendLine(text, L"DriverUnload", HexText(unload.driverUnloadAddress));
    AppendLine(text, L"Flags", HexText(unload.flags));
    AppendLine(text, L"CleanupFlagsApplied", HexText(unload.cleanupFlagsApplied));
    AppendLine(text, L"DeletedDeviceCount", std::to_wstring(unload.deletedDeviceCount));
    AppendLine(text, L"LastStatus", NtStatusText(unload.lastStatus));
    AppendLine(text, L"WaitStatus", NtStatusText(unload.waitStatus));
    AppendLine(text, L"CallbackCandidates", std::to_wstring(unload.callbackCandidates));
    AppendLine(text, L"CallbacksRemoved", std::to_wstring(unload.callbacksRemoved));
    AppendLine(text, L"CallbackFailures", std::to_wstring(unload.callbackFailures));
    AppendLine(text, L"CallbackLastStatus", NtStatusText(unload.callbackLastStatus));
    AppendLine(text, L"Message", Utf8ToWide(unload.io.message));
    return text;
}

} // namespace

DriverActionResult DriverActions::RefreshModel(DriverModel& model) {
    const DriverEnumerationResult result = EnumerateDriverSnapshot();
    model.setOverviewRows(result.overviewRows);
    model.setObjectRows(result.objectRows);

    DriverActionResult actionResult;
    actionResult.success = result.success;
    actionResult.statusText = result.diagnosticText;
    return actionResult;
}

bool DriverActions::CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
        return false;
    }

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }

    void* target = ::GlobalLock(memory);
    if (!target) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }

    std::memcpy(target, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

std::wstring DriverActions::BuildTsv(
    const std::vector<std::wstring>& headers,
    const std::vector<std::vector<std::wstring>>& rows) {
    std::wstring output;
    if (!headers.empty()) {
        AppendTsvRow(output, headers);
    }
    for (const auto& row : rows) {
        AppendTsvRow(output, row);
    }
    return output;
}

DriverActionResult DriverActions::BuildDriverObjectDetailText(const std::wstring& driverObjectName) {
    DriverActionResult result;
    if (driverObjectName.empty()) {
        result.statusText = L"DriverObject 名称为空，无法调用 R0 详情查询。";
        return result;
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::DriverObjectQueryResult query = client.queryDriverObject(
        driverObjectName,
        KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ALL,
        KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT,
        KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT);

    result.success = query.io.ok &&
        (query.queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK ||
            query.queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL);

    std::wstring text;
    AppendLine(text, L"Request", driverObjectName);
    AppendLine(text, L"IO", query.io.ok ? L"OK" : L"FAIL");
    AppendLine(text, L"Win32", std::to_wstring(query.io.win32Error));
    AppendLine(text, L"BytesReturned", std::to_wstring(query.io.bytesReturned));
    AppendLine(text, L"QueryStatus", std::wstring(DriverObjectQueryStatusText(query.queryStatus)) + L" (" + std::to_wstring(query.queryStatus) + L")");
    AppendLine(text, L"LastStatus", NtStatusText(query.lastStatus));
    AppendLine(text, L"FieldFlags", HexText(query.fieldFlags));
    AppendLine(text, L"DriverName", query.driverName);
    AppendLine(text, L"ServiceKey", query.serviceKeyName);
    AppendLine(text, L"ImagePath", query.imagePath);
    AppendLine(text, L"DriverObject", HexText(query.driverObjectAddress));
    AppendLine(text, L"DriverStart", HexText(query.driverStart));
    AppendLine(text, L"DriverSection", HexText(query.driverSection));
    AppendLine(text, L"DriverUnload", HexText(query.driverUnload));
    AppendLine(text, L"DriverFlags", HexText(query.driverFlags));
    AppendLine(text, L"DriverSize", HexText(query.driverSize));
    AppendLine(text, L"MajorFunctionCount", std::to_wstring(query.majorFunctionCount));
    AppendLine(text, L"TotalDeviceCount", std::to_wstring(query.totalDeviceCount));
    AppendLine(text, L"ReturnedDeviceCount", std::to_wstring(query.returnedDeviceCount));
    AppendLine(text, L"Message", Utf8ToWide(query.io.message));

    if (!query.majorFunctions.empty()) {
        text += L"\r\nMajorFunctions:\r\n";
        for (const ksword::ark::DriverMajorFunctionEntry& entry : query.majorFunctions) {
            text += L"  ";
            text += MajorFunctionName(entry.majorFunction);
            text += L" Dispatch=" + HexText(entry.dispatchAddress);
            text += L" Module=" + entry.moduleName;
            text += L" ModuleBase=" + HexText(entry.moduleBase);
            text += L" Flags=" + HexText(entry.flags);
            text += L"\r\n";
        }
    }

    if (!query.devices.empty()) {
        text += L"\r\nDeviceObjects:\r\n";
        for (const ksword::ark::DriverDeviceEntry& entry : query.devices) {
            text += L"  ";
            text += entry.deviceName.empty() ? L"<unnamed>" : entry.deviceName;
            text += L" Device=" + HexText(entry.deviceObjectAddress);
            text += L" Attached=" + HexText(entry.attachedDeviceObjectAddress);
            text += L" Next=" + HexText(entry.nextDeviceObjectAddress);
            text += L" Type=" + HexText(entry.deviceType);
            text += L" Flags=" + HexText(entry.flags);
            text += L" Depth=" + std::to_wstring(entry.relationDepth);
            text += L"\r\n";
        }
    }

    result.statusText = text;
    return result;
}

DriverActionResult DriverActions::ForceUnloadDriverObject(const std::wstring& driverObjectName, const unsigned long flags) {
    DriverActionResult result;
    if (driverObjectName.empty()) {
        result.statusText = L"DriverObject 名称为空，无法调用 R0 卸载。";
        return result;
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::DriverForceUnloadResult unload = client.forceUnloadDriver(driverObjectName, flags, 3000UL);
    result.success = unload.io.ok && DriverUnloadSucceeded(unload.status);
    result.statusText = BuildUnloadResultText(unload);
    return result;
}

DriverActionResult DriverActions::ForceUnloadDriverByModuleBase(
    const std::uint64_t moduleBase,
    const std::wstring& fallbackDriverName,
    const unsigned long flags) {
    DriverActionResult result;
    if (moduleBase == 0 && fallbackDriverName.empty()) {
        result.statusText = L"模块基址和 DriverObject 名称均为空，无法调用 R0 卸载。";
        return result;
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::DriverForceUnloadResult unload = moduleBase != 0
        ? client.forceUnloadDriverByModuleBase(moduleBase, fallbackDriverName, flags, 3000UL)
        : client.forceUnloadDriver(fallbackDriverName, flags, 3000UL);
    result.success = unload.io.ok && DriverUnloadSucceeded(unload.status);
    result.statusText = BuildUnloadResultText(unload);
    return result;
}

} // namespace Ksword::Features::Driver
