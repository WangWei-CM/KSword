#include "FileFeature.h"

#include "FileView.h"

#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <commctrl.h>
#include <fltuser.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "FltLib.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

namespace Ksword::Features::File {
namespace {

constexpr wchar_t kFileHostClass[] = L"KswordARKLight.FileFeatureHost";
constexpr wchar_t kFileAuditClass[] = L"KswordARKLight.FileAuditPage";
constexpr int kHostTabId = 52600;
constexpr int kBrowserTabIndex = 0;
constexpr int kAuditTabIndex = 1;
constexpr int kAuditTargetEditId = 52610;
constexpr int kAuditRefreshButtonId = 52611;
constexpr int kAuditInnerTabId = 52612;
constexpr int kAuditListId = 52613;
constexpr int kAuditStatusId = 52614;
constexpr int kAuditMinifilterTabIndex = 0;
constexpr int kAuditFileObjectTabIndex = 1;
constexpr int kAuditSectionTabIndex = 2;
constexpr int kAuditStorageTabIndex = 3;
constexpr int kAuditBitLockerTabIndex = 4;
constexpr unsigned long kAuditMaxSectionMappings = 256UL;

// AuditRow is the common table model for every read-only file audit subpage.
// Inputs are collected by R3 public APIs or ArkDriverClient; processing only
// formats evidence; the row owns no handles and returns no resources.
struct AuditRow {
    std::wstring item;
    std::wstring source;
    std::wstring status;
    std::wstring value;
    std::wstring detail;
};

// FileFeatureHostState owns the outer File feature tabs. Inputs arrive through
// Win32 messages; processing only sizes and shows child pages; no value is
// returned and ownership is released in WM_NCDESTROY.
struct FileFeatureHostState {
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HWND browserPage = nullptr;
    HWND auditPage = nullptr;
    int currentTab = kBrowserTabIndex;
};

// FileAuditPageState owns the audit target controls and result table. Inputs are
// the current target path plus active subtab; processing rebuilds rows on demand;
// no kernel object is modified and the state is destroyed with the HWND.
struct FileAuditPageState {
    HWND hwnd = nullptr;
    HWND targetEdit = nullptr;
    HWND refreshButton = nullptr;
    HWND tab = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    int currentTab = kAuditMinifilterTabIndex;
    std::vector<AuditRow> rows;
};

// Width returns a non-negative rectangle width. Input is a RECT; output is a
// pixel count suitable for MoveWindow.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative rectangle height. Input is a RECT; output is a
// pixel count suitable for MoveWindow.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// FileTimeToText converts a FILETIME-compatible integer to local display text.
// Input is a 64-bit FILETIME value; processing uses FileTimeToSystemTime after
// local conversion; output is a readable timestamp or an unavailable marker.
std::wstring FileTimeToText(const std::int64_t value) {
    if (value <= 0) {
        return L"<不可用>";
    }
    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(value) & 0xFFFFFFFFULL);
    utc.dwHighDateTime = static_cast<DWORD>((static_cast<std::uint64_t>(value) >> 32) & 0xFFFFFFFFULL);
    FILETIME local{};
    SYSTEMTIME st{};
    if (!::FileTimeToLocalFileTime(&utc, &local) || !::FileTimeToSystemTime(&local, &st)) {
        return L"<时间转换失败>";
    }
    wchar_t buffer[64]{};
    ::swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

// HexText formats a diagnostic integer/address. Input is a 64-bit value; output
// is uppercase hexadecimal text used only as evidence, never as an action key.
std::wstring HexText(const std::uint64_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

// BoolText converts a boolean flag to Chinese display text. Input is a bool;
// output is a stable label for audit rows.
const wchar_t* BoolText(const bool value) {
    return value ? L"是" : L"否";
}

// Utf8ToWide converts ArkDriverClient diagnostics to UTF-16. Input is a UTF-8
// or byte-oriented message; processing tries strict UTF-8 and falls back to a
// byte copy; output is safe for UI status cells.
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (required > 0) {
        std::wstring wide(static_cast<std::size_t>(required), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), wide.data(), required);
        return wide;
    }
    std::wstring fallback;
    fallback.reserve(text.size());
    for (const unsigned char ch : text) {
        fallback.push_back(static_cast<wchar_t>(ch));
    }
    return fallback;
}

// FormatWin32Error converts GetLastError style failures to a compact reason.
// Input is a Win32 error code; processing calls FormatMessage when possible;
// output includes the numeric code for static triage.
std::wstring FormatWin32Error(const DWORD error) {
    wchar_t* message = nullptr;
    const DWORD chars = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);
    std::wstring text = L"Win32=" + std::to_wstring(error);
    if (chars > 0 && message) {
        text += L" ";
        text += message;
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'\t')) {
            text.pop_back();
        }
    }
    if (message) {
        ::LocalFree(message);
    }
    return text;
}

// FormatHresult formats an HRESULT with a likely human reason. Input is an
// HRESULT from Filter Manager or WMI; output is a compact diagnostic string.
std::wstring FormatHresult(const HRESULT hr) {
    std::wostringstream stream;
    stream << L"HRESULT=0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        stream << L" (" << FormatWin32Error(HRESULT_CODE(hr)) << L")";
    }
    return stream.str();
}

// TextFromWindow returns the current text of a child control. Input is an HWND;
// processing reads bounded Win32 text; output is empty if the window is absent.
std::wstring TextFromWindow(HWND hwnd) {
    if (!hwnd) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    if (length > 0) {
        ::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

// TrimmedPath removes surrounding whitespace from a user supplied path. Input
// is raw edit text; processing trims only whitespace and leaves path semantics
// untouched; output may be empty.
std::wstring TrimmedPath(std::wstring text) {
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    std::size_t first = 0;
    while (first < text.size() && (text[first] == L' ' || text[first] == L'\t' || text[first] == L'\r' || text[first] == L'\n')) {
        ++first;
    }
    if (first > 0) {
        text.erase(0, first);
    }
    return text;
}

// BuildDriverNtPath mirrors the existing FileActions path convention for R0
// read-only queries. Input is a Win32/UNC/NT path; processing normalizes slashes
// and prefixes \??\ where needed; output is empty only for empty input.
std::wstring BuildDriverNtPath(const std::wstring& path) {
    std::wstring nativePath = TrimmedPath(path);
    for (wchar_t& ch : nativePath) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    if (nativePath.empty()) {
        return {};
    }
    if (nativePath.rfind(L"\\??\\", 0) == 0 || nativePath.rfind(L"\\Device\\", 0) == 0) {
        return nativePath;
    }
    if (nativePath.rfind(L"\\\\?\\", 0) == 0) {
        return L"\\??\\" + nativePath.substr(4);
    }
    if (nativePath.rfind(L"\\\\", 0) == 0) {
        return L"\\??\\UNC\\" + nativePath.substr(2);
    }
    return L"\\??\\" + nativePath;
}

// DefaultAuditTarget chooses a harmless existing target for initial display.
// There is no input; processing prefers ntdll.dll for FileObject/Section demos
// and falls back to the system drive; output is a Win32 path.
std::wstring DefaultAuditTarget() {
    wchar_t windowsDir[MAX_PATH]{};
    const UINT chars = ::GetWindowsDirectoryW(windowsDir, static_cast<UINT>(std::size(windowsDir)));
    if (chars > 0 && chars < std::size(windowsDir)) {
        std::wstring ntdll = std::wstring(windowsDir) + L"\\System32\\ntdll.dll";
        if (::GetFileAttributesW(ntdll.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return ntdll;
        }
        std::wstring root = std::wstring(windowsDir).substr(0, 3);
        if (root.size() == 3 && root[1] == L':' && (root[2] == L'\\' || root[2] == L'/')) {
            return root;
        }
    }
    return L"C:\\";
}

// ReadUtf16FieldAtOffset extracts a non-NUL-terminated WCHAR field from a Filter
// Manager aggregate buffer. Inputs are the record buffer, byte count, field
// offset and byte length; output is empty when bounds are invalid.
std::wstring ReadUtf16FieldAtOffset(const void* buffer, const std::size_t bytes, const USHORT offset, const USHORT lengthBytes) {
    if (!buffer || lengthBytes == 0U || offset >= bytes || static_cast<std::size_t>(offset) + lengthBytes > bytes) {
        return {};
    }
    const auto* base = static_cast<const unsigned char*>(buffer);
    const auto* text = reinterpret_cast<const wchar_t*>(base + offset);
    return std::wstring(text, text + (lengthBytes / sizeof(wchar_t)));
}

// FieldPresentText renders whether a shared protocol field flag is set. Inputs
// are the response flags and one mask; output states whether the R0 protocol
// populated that field in this query.
std::wstring FieldPresentText(const std::uint32_t flags, const std::uint32_t mask) {
    return (flags & mask) ? L"present" : L"unavailable";
}

// FileInfoStatusText converts KSWORD_ARK_FILE_INFO_STATUS_* into UI text. Input
// is the shared status enum; output is a stable label with degradation meaning.
const wchar_t* FileInfoStatusText(const std::uint32_t value) {
    switch (value) {
    case KSWORD_ARK_FILE_INFO_STATUS_OK: return L"OK";
    case KSWORD_ARK_FILE_INFO_STATUS_PARTIAL: return L"Partial";
    case KSWORD_ARK_FILE_INFO_STATUS_OPEN_FAILED: return L"OpenFailed";
    case KSWORD_ARK_FILE_INFO_STATUS_BASIC_FAILED: return L"BasicFailed";
    case KSWORD_ARK_FILE_INFO_STATUS_STANDARD_FAILED: return L"StandardFailed";
    case KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED: return L"ObjectFailed";
    case KSWORD_ARK_FILE_INFO_STATUS_NAME_FAILED: return L"NameFailed";
    default: return L"Unavailable";
    }
}

// FileSectionStatusText converts KSWORD_ARK_FILE_SECTION_QUERY_STATUS_* into UI
// text. Input is the shared status enum; output is a stable label.
const wchar_t* FileSectionStatusText(const std::uint32_t value) {
    switch (value) {
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_OK: return L"OK";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_PARTIAL: return L"Partial";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_DYNDATA_MISSING: return L"DynDataMissing";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OPEN_FAILED: return L"FileOpenFailed";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OBJECT_FAILED: return L"FileObjectFailed";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_SECTION_POINTERS_MISSING: return L"SectionPointersMissing";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_CONTROL_AREA_MISSING: return L"ControlAreaMissing";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_MAPPING_QUERY_FAILED: return L"MappingQueryFailed";
    case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_BUFFER_TOO_SMALL: return L"BufferTooSmall";
    default: return L"Unavailable";
    }
}

// SectionKindText converts the file-section kind enum to display text. Input is
// KSWORD_ARK_FILE_SECTION_KIND_*; output is Data/Image/Unknown.
const wchar_t* SectionKindText(const std::uint32_t value) {
    switch (value) {
    case KSWORD_ARK_FILE_SECTION_KIND_DATA: return L"Data";
    case KSWORD_ARK_FILE_SECTION_KIND_IMAGE: return L"Image";
    default: return L"Unknown";
    }
}

// ViewMapTypeText converts a mapped-view source enum to display text. Input is
// KSWORD_ARK_SECTION_MAP_TYPE_*; output is used in ControlArea rows.
const wchar_t* ViewMapTypeText(const std::uint32_t value) {
    switch (value) {
    case KSWORD_ARK_SECTION_MAP_TYPE_PROCESS: return L"Process";
    case KSWORD_ARK_SECTION_MAP_TYPE_SESSION: return L"Session";
    case KSWORD_ARK_SECTION_MAP_TYPE_SYSTEM_CACHE: return L"SystemCache";
    default: return L"Unknown";
    }
}

// FltFilesystemText converts FLT_FILESYSTEM_TYPE into a concise name. Input is
// a Filter Manager file-system enum; output is a label or numeric fallback.
std::wstring FltFilesystemText(const FLT_FILESYSTEM_TYPE type) {
    switch (type) {
    case FLT_FSTYPE_UNKNOWN: return L"Unknown";
    case FLT_FSTYPE_RAW: return L"RAW";
    case FLT_FSTYPE_NTFS: return L"NTFS";
    case FLT_FSTYPE_FAT: return L"FAT";
    case FLT_FSTYPE_CDFS: return L"CDFS";
    case FLT_FSTYPE_UDFS: return L"UDFS";
    case FLT_FSTYPE_LANMAN: return L"LANMAN";
    case FLT_FSTYPE_WEBDAV: return L"WebDAV";
    case FLT_FSTYPE_RDPDR: return L"RDPDR";
    case FLT_FSTYPE_NFS: return L"NFS";
    case FLT_FSTYPE_EXFAT: return L"exFAT";
    case FLT_FSTYPE_REFS: return L"ReFS";
    case FLT_FSTYPE_CSVFS: return L"CSVFS";
    case FLT_FSTYPE_CIMFS: return L"CIMFS";
    default: return L"Type=" + std::to_wstring(static_cast<int>(type));
    }
}


// AddRow appends one evidence row. Inputs are the row fields; processing only
// stores display text in the vector; no value is returned.
void AddRow(std::vector<AuditRow>& rows, std::wstring item, std::wstring source, std::wstring status, std::wstring value, std::wstring detail) {
    rows.push_back({ std::move(item), std::move(source), std::move(status), std::move(value), std::move(detail) });
}

// AppendMinifilterRecord parses one FilterFind* aggregate record. Inputs are the
// Filter Manager buffer and byte count; processing extracts only public fields;
// no kernel filter state is modified.
void AppendMinifilterRecord(std::vector<AuditRow>& rows, const void* buffer, const std::size_t bytes) {
    if (!buffer || bytes < sizeof(FILTER_AGGREGATE_STANDARD_INFORMATION)) {
        AddRow(rows, L"Filter", L"FilterMgr API", L"数据不可用", L"<record too small>", L"FilterAggregateStandardInformation 缓冲区过小。");
        return;
    }
    const auto* record = static_cast<const FILTER_AGGREGATE_STANDARD_INFORMATION*>(buffer);
    const bool minifilter = (record->Flags & FLTFL_ASI_IS_MINIFILTER) != 0;
    const std::wstring name = minifilter
        ? ReadUtf16FieldAtOffset(buffer, bytes, record->Type.MiniFilter.FilterNameBufferOffset, record->Type.MiniFilter.FilterNameLength)
        : ReadUtf16FieldAtOffset(buffer, bytes, record->Type.LegacyFilter.FilterNameBufferOffset, record->Type.LegacyFilter.FilterNameLength);
    const std::wstring altitude = minifilter
        ? ReadUtf16FieldAtOffset(buffer, bytes, record->Type.MiniFilter.FilterAltitudeBufferOffset, record->Type.MiniFilter.FilterAltitudeLength)
        : ReadUtf16FieldAtOffset(buffer, bytes, record->Type.LegacyFilter.FilterAltitudeBufferOffset, record->Type.LegacyFilter.FilterAltitudeLength);
    std::wostringstream detail;
    detail << L"Kind=" << (minifilter ? L"Minifilter" : L"LegacyFilter")
           << L", Flags=" << HexText(record->Flags);
    if (minifilter) {
        detail << L", FrameID=" << record->Type.MiniFilter.FrameID
               << L", Instances=" << record->Type.MiniFilter.NumberOfInstances;
    }
    AddRow(rows, name.empty() ? L"<unknown filter>" : name, L"FilterFindFirst/Next", L"OK", altitude.empty() ? L"<altitude unavailable>" : altitude, detail.str());
}

// AppendInstanceRecord parses one FilterInstanceFind* aggregate record. Inputs
// are the public Filter Manager record buffer; processing extracts instance,
// volume, filter and altitude fields; no detach/unload action exists here.
void AppendInstanceRecord(std::vector<AuditRow>& rows, const void* buffer, const std::size_t bytes) {
    if (!buffer || bytes < sizeof(INSTANCE_AGGREGATE_STANDARD_INFORMATION)) {
        AddRow(rows, L"Instance", L"FilterMgr API", L"数据不可用", L"<record too small>", L"InstanceAggregateStandardInformation 缓冲区过小。");
        return;
    }
    const auto* record = static_cast<const INSTANCE_AGGREGATE_STANDARD_INFORMATION*>(buffer);
    const bool minifilter = (record->Flags & FLTFL_IASI_IS_MINIFILTER) != 0;
    std::wstring instanceName;
    std::wstring altitude;
    std::wstring volumeName;
    std::wstring filterName;
    FLT_FILESYSTEM_TYPE fsType = FLT_FSTYPE_UNKNOWN;
    ULONG frameId = 0;
    ULONG flags = 0;
    if (minifilter) {
        instanceName = ReadUtf16FieldAtOffset(buffer, bytes, record->Type.MiniFilter.InstanceNameBufferOffset, record->Type.MiniFilter.InstanceNameLength);
        altitude = ReadUtf16FieldAtOffset(buffer, bytes, record->Type.MiniFilter.AltitudeBufferOffset, record->Type.MiniFilter.AltitudeLength);
        volumeName = ReadUtf16FieldAtOffset(buffer, bytes, record->Type.MiniFilter.VolumeNameBufferOffset, record->Type.MiniFilter.VolumeNameLength);
        filterName = ReadUtf16FieldAtOffset(buffer, bytes, record->Type.MiniFilter.FilterNameBufferOffset, record->Type.MiniFilter.FilterNameLength);
        fsType = record->Type.MiniFilter.VolumeFileSystemType;
        frameId = record->Type.MiniFilter.FrameID;
        flags = record->Type.MiniFilter.Flags;
    } else {
        altitude = ReadUtf16FieldAtOffset(buffer, bytes, record->Type.LegacyFilter.AltitudeBufferOffset, record->Type.LegacyFilter.AltitudeLength);
        volumeName = ReadUtf16FieldAtOffset(buffer, bytes, record->Type.LegacyFilter.VolumeNameBufferOffset, record->Type.LegacyFilter.VolumeNameLength);
        filterName = ReadUtf16FieldAtOffset(buffer, bytes, record->Type.LegacyFilter.FilterNameBufferOffset, record->Type.LegacyFilter.FilterNameLength);
        flags = record->Type.LegacyFilter.Flags;
    }
    std::wostringstream value;
    value << (altitude.empty() ? L"<altitude unavailable>" : altitude)
          << L" @ " << (volumeName.empty() ? L"<volume unavailable>" : volumeName);
    std::wostringstream detail;
    detail << L"Filter=" << (filterName.empty() ? L"<unknown>" : filterName)
           << L", Kind=" << (minifilter ? L"Minifilter" : L"LegacyFilter")
           << L", FrameID=" << frameId
           << L", FS=" << FltFilesystemText(fsType)
           << L", Flags=" << HexText(flags);
    AddRow(rows, instanceName.empty() ? filterName : instanceName, L"FilterInstanceFindFirst/Next", L"OK", value.str(), detail.str());
}

// AppendVolumeRecord parses one FilterVolumeFind* standard record. Inputs are
// the public Filter Manager record buffer; processing extracts volume name,
// frame and file-system type; output is appended table rows only.
void AppendVolumeRecord(std::vector<AuditRow>& rows, const void* buffer, const std::size_t bytes) {
    if (!buffer || bytes < sizeof(FILTER_VOLUME_STANDARD_INFORMATION)) {
        AddRow(rows, L"Volume", L"FilterMgr API", L"数据不可用", L"<record too small>", L"FilterVolumeStandardInformation 缓冲区过小。");
        return;
    }
    const auto* record = static_cast<const FILTER_VOLUME_STANDARD_INFORMATION*>(buffer);
    const std::wstring volumeName = ReadUtf16FieldAtOffset(buffer, bytes, static_cast<USHORT>(offsetof(FILTER_VOLUME_STANDARD_INFORMATION, FilterVolumeName)), record->FilterVolumeNameLength);
    std::wostringstream detail;
    detail << L"FrameID=" << record->FrameID
           << L", FS=" << FltFilesystemText(record->FileSystemType)
           << L", Flags=" << HexText(record->Flags);
    AddRow(rows, volumeName.empty() ? L"<unknown volume>" : volumeName, L"FilterVolumeFindFirst/Next", L"OK", FltFilesystemText(record->FileSystemType), detail.str());
}

// QueryMinifilterRows builds the Minifilter/Instance/Volume audit view. There
// is no target input because Filter Manager exposes global inventory; processing
// uses documented enumeration APIs only; output rows include unsupported and
// permission-denied reasons when enumeration fails.
std::vector<AuditRow> QueryMinifilterRows() {
    std::vector<AuditRow> rows;
    const ksword::ark::DriverClient client;
    const ksword::ark::MinifilterInventoryResult r0Inventory = client.queryMinifilterInventory();
    AddRow(rows, L"R0 cross-view", L"ArkDriverClient::queryMinifilterInventory", r0Inventory.io.ok ? L"OK" : (r0Inventory.unsupported ? L"驱动不支持" : L"驱动不可用/权限不足"), std::to_wstring(r0Inventory.returnedCount) + L"/" + std::to_wstring(r0Inventory.totalCount), Utf8ToWide(r0Inventory.io.message));
    for (const KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY& entry : r0Inventory.entries) {
        std::wostringstream detail;
        detail << L"FilterObject=" << HexText(entry.filterObject)
               << L", VolumeObject=" << HexText(entry.volumeObject)
               << L", Instances=" << entry.instanceCount
               << L", Frame=" << entry.frameId
               << L", Flags=" << HexText(entry.fieldFlags);
        AddRow(rows, entry.filterName[0] ? entry.filterName : L"<R0 unnamed filter>", L"R0 MinifilterInventory", std::to_wstring(entry.status), entry.altitude[0] ? entry.altitude : L"<altitude unavailable>", detail.str());
    }
    AddRow(rows, L"安全边界", L"UI", L"只读", L"无卸载/分离/绕过", L"本页不调用 FilterUnload、FilterDetach、patch、bypass 或任意修改接口。");

    std::vector<unsigned char> buffer(16U * 1024U, 0U);
    DWORD bytesReturned = 0;
    HANDLE findHandle = nullptr;
    HRESULT hr = E_FAIL;
    for (;;) {
        hr = ::FilterFindFirst(FilterAggregateStandardInformation, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, &findHandle);
        if (SUCCEEDED(hr)) {
            break;
        }
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
            buffer.resize(buffer.size() * 2U);
            continue;
        }
        AddRow(rows, L"Filter list", L"FilterFindFirst", L"数据不可用", FormatHresult(hr), L"可能原因：权限不足、fltMgr 不可用或系统不支持该信息类。");
        return rows;
    }

    std::vector<std::wstring> filterNames;
    const auto captureFilterName = [&](const void* data, const std::size_t bytes) {
        if (!data || bytes < sizeof(FILTER_AGGREGATE_STANDARD_INFORMATION)) {
            return;
        }
        const auto* record = static_cast<const FILTER_AGGREGATE_STANDARD_INFORMATION*>(data);
        if ((record->Flags & FLTFL_ASI_IS_MINIFILTER) == 0) {
            return;
        }
        const std::wstring name = ReadUtf16FieldAtOffset(data, bytes, record->Type.MiniFilter.FilterNameBufferOffset, record->Type.MiniFilter.FilterNameLength);
        if (!name.empty()) {
            filterNames.push_back(name);
        }
    };

    AppendMinifilterRecord(rows, buffer.data(), bytesReturned);
    captureFilterName(buffer.data(), bytesReturned);
    for (;;) {
        bytesReturned = 0;
        hr = ::FilterFindNext(findHandle, FilterAggregateStandardInformation, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned);
        if (SUCCEEDED(hr)) {
            AppendMinifilterRecord(rows, buffer.data(), bytesReturned);
            captureFilterName(buffer.data(), bytesReturned);
            continue;
        }
        if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) {
            break;
        }
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
            buffer.resize(buffer.size() * 2U);
            continue;
        }
        AddRow(rows, L"Filter list", L"FilterFindNext", L"部分数据", FormatHresult(hr), L"后续 filter 枚举失败，已保留前面成功记录。");
        break;
    }
    if (findHandle) {
        ::FilterFindClose(findHandle);
    }

    for (const std::wstring& filterName : filterNames) {
        HANDLE instanceHandle = nullptr;
        bytesReturned = 0;
        hr = ::FilterInstanceFindFirst(filterName.c_str(), InstanceAggregateStandardInformation, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, &instanceHandle);
        if (FAILED(hr)) {
            if (hr != HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) {
                AddRow(rows, filterName, L"FilterInstanceFindFirst", L"数据不可用", FormatHresult(hr), L"该 filter 的 instance 信息不可用，保留 filter 基础行。");
            }
            continue;
        }
        AppendInstanceRecord(rows, buffer.data(), bytesReturned);
        for (;;) {
            bytesReturned = 0;
            hr = ::FilterInstanceFindNext(instanceHandle, InstanceAggregateStandardInformation, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned);
            if (SUCCEEDED(hr)) {
                AppendInstanceRecord(rows, buffer.data(), bytesReturned);
                continue;
            }
            if (hr != HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) {
                AddRow(rows, filterName, L"FilterInstanceFindNext", L"部分数据", FormatHresult(hr), L"后续 instance 枚举失败，已保留前面成功记录。");
            }
            break;
        }
        ::FilterInstanceFindClose(instanceHandle);
    }

    HANDLE volumeHandle = nullptr;
    bytesReturned = 0;
    hr = ::FilterVolumeFindFirst(FilterVolumeStandardInformation, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, &volumeHandle);
    if (SUCCEEDED(hr)) {
        AppendVolumeRecord(rows, buffer.data(), bytesReturned);
        for (;;) {
            bytesReturned = 0;
            hr = ::FilterVolumeFindNext(volumeHandle, FilterVolumeStandardInformation, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned);
            if (SUCCEEDED(hr)) {
                AppendVolumeRecord(rows, buffer.data(), bytesReturned);
                continue;
            }
            if (hr != HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) {
                AddRow(rows, L"Volume list", L"FilterVolumeFindNext", L"部分数据", FormatHresult(hr), L"后续 volume 枚举失败，已保留前面成功记录。");
            }
            break;
        }
        ::FilterVolumeFindClose(volumeHandle);
    } else if (hr != HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) {
        AddRow(rows, L"Volume list", L"FilterVolumeFindFirst", L"数据不可用", FormatHresult(hr), L"可能原因：权限不足或 Filter Manager volume 信息不可用。");
    }
    return rows;
}


// QueryFileObjectRows builds the FileObject audit view for one target path.
// Input is a Win32 or NT path; processing calls ArkDriverClient::queryFileInfo
// only and displays per-field availability; output rows include all failures as
// degraded evidence instead of raising UI errors.
std::vector<AuditRow> QueryFileObjectRows(const std::wstring& targetPath) {
    std::vector<AuditRow> rows;
    const std::wstring trimmed = TrimmedPath(targetPath);
    const std::wstring ntPath = BuildDriverNtPath(trimmed);
    AddRow(rows, L"目标路径", L"UI", trimmed.empty() ? L"数据不可用" : L"OK", trimmed.empty() ? L"<empty>" : trimmed, L"可输入 Win32、UNC、\\??\\ 或 \\Device\\ 路径。");
    AddRow(rows, L"NT 路径", L"PathAdapter", ntPath.empty() ? L"数据不可用" : L"OK", ntPath.empty() ? L"<empty>" : ntPath, L"仅用于 R0 只读查询，不作为后续修改凭据。");
    if (ntPath.empty()) {
        AddRow(rows, L"FileObject", L"ArkDriverClient", L"数据不可用", L"<empty path>", L"请输入一个文件或目录路径后刷新。");
        return rows;
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::FileInfoQueryResult query = client.queryFileInfo(ntPath, KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL);
    const std::wstring ioMessage = Utf8ToWide(query.io.message);
    AddRow(rows, L"IOCTL_KSWORD_ARK_QUERY_FILE_INFO", L"ArkDriverClient", query.io.ok ? L"OK" : L"驱动不支持/权限不足", query.io.ok ? L"DeviceIoControl OK" : L"DeviceIoControl failed", ioMessage);
    AddRow(rows, L"QueryStatus", L"R0 FileInfo", FileInfoStatusText(query.queryStatus), std::to_wstring(query.queryStatus), L"open/basic/standard/object/name 均以独立状态返回，失败不会毒化整页。");
    AddRow(rows, L"FieldFlags", L"R0 FileInfo", query.fieldFlags ? L"OK" : L"数据不可用", HexText(query.fieldFlags), L"字段 present/unavailable 依据 shared 协议位显示。");
    AddRow(rows, L"OpenStatus", L"R0 FileInfo", query.openStatus >= 0 ? L"OK" : L"数据不可用", HexText(static_cast<std::uint32_t>(query.openStatus)), L"ZwCreateFile/ObReference 路径状态。");
    AddRow(rows, L"BasicStatus", L"R0 FileInfo", query.basicStatus >= 0 ? L"OK" : L"数据不可用", HexText(static_cast<std::uint32_t>(query.basicStatus)), L"FileBasicInformation 状态。");
    AddRow(rows, L"StandardStatus", L"R0 FileInfo", query.standardStatus >= 0 ? L"OK" : L"数据不可用", HexText(static_cast<std::uint32_t>(query.standardStatus)), L"FileStandardInformation 状态。");
    AddRow(rows, L"ObjectStatus", L"R0 FileInfo", query.objectStatus >= 0 ? L"OK" : L"数据不可用", HexText(static_cast<std::uint32_t>(query.objectStatus)), L"FileObject/SectionObjectPointers 安全读取状态。");
    AddRow(rows, L"NameStatus", L"R0 FileInfo", query.nameStatus >= 0 ? L"OK" : L"数据不可用", HexText(static_cast<std::uint32_t>(query.nameStatus)), L"ObQueryNameString 状态。");
    AddRow(rows, L"FileObject", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_FILE_OBJECT_PRESENT), HexText(query.fileObjectAddress), L"诊断地址，仅展示，不作为 UI 动作输入。");
    AddRow(rows, L"SectionObjectPointers", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_SECTION_POINTERS_PRESENT), HexText(query.sectionObjectPointersAddress), L"_FILE_OBJECT.SectionObjectPointer 诊断地址。");
    AddRow(rows, L"DataSectionObject", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_DATA_SECTION_PRESENT), HexText(query.dataSectionObjectAddress), L"数据段 SectionObject，可能为空。");
    AddRow(rows, L"ImageSectionObject", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_IMAGE_SECTION_PRESENT), HexText(query.imageSectionObjectAddress), L"映像段 SectionObject，非映像文件通常为空。");
    AddRow(rows, L"ObjectName", L"R0 FileInfo", query.objectName.empty() ? L"数据不可用" : L"OK", query.objectName.empty() ? L"<empty>" : query.objectName, L"R0 对象名安全读取结果。");
    AddRow(rows, L"NtPathEcho", L"R0 FileInfo", query.ntPath.empty() ? L"数据不可用" : L"OK", query.ntPath.empty() ? L"<empty>" : query.ntPath, L"驱动回显路径。");
    AddRow(rows, L"Attributes", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_BASIC_PRESENT), HexText(query.fileAttributes), L"FILE_ATTRIBUTE_*。");
    AddRow(rows, L"EndOfFile", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_STANDARD_PRESENT), std::to_wstring(query.endOfFile), L"逻辑文件大小。");
    AddRow(rows, L"AllocationSize", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_STANDARD_PRESENT), std::to_wstring(query.allocationSize), L"分配大小。");
    AddRow(rows, L"CreationTime", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_BASIC_PRESENT), FileTimeToText(query.creationTime), L"FILETIME 本地化显示。");
    AddRow(rows, L"LastWriteTime", L"R0 FileInfo", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_INFO_FIELD_BASIC_PRESENT), FileTimeToText(query.lastWriteTime), L"FILETIME 本地化显示。");
    AddRow(rows, L"降级说明", L"UI", query.io.ok ? L"OK" : L"驱动不支持/权限不足", query.io.ok ? L"R0 FileInfo 可用" : L"R0 FileInfo 不可用", L"若旧驱动未注册 IOCTL 或服务未加载，本页保留路径和错误原因，不执行 fallback 写操作。");
    return rows;
}

// QuerySectionRows builds the Section/ControlArea audit view for one target
// file. Input is a path; processing calls ArkDriverClient::queryFileSectionMappings;
// output rows show data/image ControlArea and bounded mapping entries.
std::vector<AuditRow> QuerySectionRows(const std::wstring& targetPath) {
    std::vector<AuditRow> rows;
    const std::wstring trimmed = TrimmedPath(targetPath);
    const std::wstring ntPath = BuildDriverNtPath(trimmed);
    AddRow(rows, L"目标路径", L"UI", trimmed.empty() ? L"数据不可用" : L"OK", trimmed.empty() ? L"<empty>" : trimmed, L"Section 查询只读打开文件并返回诊断字段。");
    if (ntPath.empty()) {
        AddRow(rows, L"ControlArea", L"ArkDriverClient", L"数据不可用", L"<empty path>", L"请输入一个普通文件路径后刷新。");
        return rows;
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::FileSectionMappingsQueryResult query = client.queryFileSectionMappings(
        ntPath,
        KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL,
        kAuditMaxSectionMappings);
    AddRow(rows, L"IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS", L"ArkDriverClient", query.io.ok ? L"OK" : L"驱动不支持/权限不足", query.io.ok ? L"DeviceIoControl OK" : L"DeviceIoControl failed", Utf8ToWide(query.io.message));
    AddRow(rows, L"QueryStatus", L"R0 Section", FileSectionStatusText(query.queryStatus), std::to_wstring(query.queryStatus), L"DynData/profile 缺失会显示为降级，不导致 UI 失败。");
    AddRow(rows, L"FieldFlags", L"R0 Section", query.fieldFlags ? L"OK" : L"数据不可用", HexText(query.fieldFlags), L"KSWORD_ARK_FILE_SECTION_FIELD_*。");
    AddRow(rows, L"LastStatus", L"R0 Section", query.lastStatus >= 0 ? L"OK" : L"部分数据", HexText(static_cast<std::uint32_t>(query.lastStatus)), L"最近一次 NTSTATUS。");
    AddRow(rows, L"FileObject", L"R0 Section", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_SECTION_FIELD_FILE_OBJECT_PRESENT), HexText(query.fileObjectAddress), L"诊断地址，仅展示。");
    AddRow(rows, L"SectionObjectPointers", L"R0 Section", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_SECTION_FIELD_SECTION_POINTERS_PRESENT), HexText(query.sectionObjectPointersAddress), L"_SECTION_OBJECT_POINTERS 地址。");
    AddRow(rows, L"DataControlArea", L"R0 Section", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_SECTION_FIELD_DATA_CONTROL_AREA_PRESENT), HexText(query.dataControlAreaAddress), L"数据映射 ControlArea。");
    AddRow(rows, L"ImageControlArea", L"R0 Section", FieldPresentText(query.fieldFlags, KSWORD_ARK_FILE_SECTION_FIELD_IMAGE_CONTROL_AREA_PRESENT), HexText(query.imageControlAreaAddress), L"映像映射 ControlArea。");
    AddRow(rows, L"Mappings", L"R0 Section", query.mappings.empty() ? L"数据不可用" : L"OK", std::to_wstring(query.returnedCount) + L"/" + std::to_wstring(query.totalCount), L"最多请求 256 行，R0 仍有硬上限和截断标记。");
    if ((query.fieldFlags & KSWORD_ARK_FILE_SECTION_FIELD_MAPPING_TRUNCATED) != 0U) {
        AddRow(rows, L"MappingTruncated", L"R0 Section", L"部分数据", L"true", L"R0 返回截断标记，表格仅展示已返回行。");
    }
    const std::size_t limit = std::min<std::size_t>(query.mappings.size(), 96U);
    for (std::size_t index = 0; index < limit; ++index) {
        const ksword::ark::FileSectionMappingEntry& row = query.mappings[index];
        std::wostringstream item;
        item << L"Mapping #" << (index + 1U) << L" PID=" << row.processId;
        std::wostringstream value;
        value << SectionKindText(row.sectionKind) << L" " << ViewMapTypeText(row.viewMapType);
        std::wostringstream detail;
        detail << L"VA=" << HexText(row.startVa) << L"-" << HexText(row.endVa)
               << L", ControlArea=" << HexText(row.controlAreaAddress);
        AddRow(rows, item.str(), L"R0 VAD/ControlArea", L"OK", value.str(), detail.str());
    }
    if (query.mappings.size() > limit) {
        AddRow(rows, L"Mapping display cap", L"UI", L"部分数据", std::to_wstring(limit) + L"/" + std::to_wstring(query.mappings.size()), L"UI 限制展示 96 行，避免轻量页卡顿；R0 returnedCount 保留真实数量。");
    }
    AddRow(rows, L"安全边界", L"UI", L"只读", L"无解除映射/关闭句柄", L"本页不接受任意 ControlArea 地址作为操作输入，不解除 VAD、不修改 Section。");
    return rows;
}


// VolumeRootFromPath resolves a path to a Win32 volume root. Input is a file,
// directory, drive or empty string; processing uses GetVolumePathName and safe
// fallbacks; output may be empty when Windows cannot map the path.
std::wstring VolumeRootFromPath(const std::wstring& targetPath) {
    std::wstring path = TrimmedPath(targetPath);
    if (path.empty()) {
        path = DefaultAuditTarget();
    }
    wchar_t volumeRoot[MAX_PATH]{};
    if (::GetVolumePathNameW(path.c_str(), volumeRoot, static_cast<DWORD>(std::size(volumeRoot)))) {
        return volumeRoot;
    }
    if (path.size() >= 2 && path[1] == L':') {
        std::wstring root = path.substr(0, 2) + L"\\";
        return root;
    }
    return {};
}

// QueryVolumeGuid resolves a volume root to its stable Volume GUID path. Input
// is a root such as C:\; processing calls GetVolumeNameForVolumeMountPoint;
// output is empty when the mapping is unavailable.
std::wstring QueryVolumeGuid(const std::wstring& volumeRoot, DWORD* errorOut = nullptr) {
    wchar_t guid[128]{};
    if (::GetVolumeNameForVolumeMountPointW(volumeRoot.c_str(), guid, static_cast<DWORD>(std::size(guid)))) {
        if (errorOut) {
            *errorOut = ERROR_SUCCESS;
        }
        return guid;
    }
    if (errorOut) {
        *errorOut = ::GetLastError();
    }
    return {};
}

// QueryDosDeviceForVolume resolves C: to a \Device\HarddiskVolume* path. Input
// is a volume root; output is empty with an optional error when unavailable.
std::wstring QueryDosDeviceForVolume(const std::wstring& volumeRoot, DWORD* errorOut = nullptr) {
    if (volumeRoot.size() < 2 || volumeRoot[1] != L':') {
        if (errorOut) {
            *errorOut = ERROR_INVALID_PARAMETER;
        }
        return {};
    }
    wchar_t device[1024]{};
    wchar_t driveName[3] = { volumeRoot[0], L':', L'\0' };
    const DWORD chars = ::QueryDosDeviceW(driveName, device, static_cast<DWORD>(std::size(device)));
    if (chars > 0) {
        if (errorOut) {
            *errorOut = ERROR_SUCCESS;
        }
        return device;
    }
    if (errorOut) {
        *errorOut = ::GetLastError();
    }
    return {};
}

// QueryMountPoints returns drive-letter and mount-point names for a Volume GUID.
// Input is a Volume GUID path; processing calls documented Win32 APIs only;
// output is a semicolon separated list or a degradation reason.
std::wstring QueryMountPoints(const std::wstring& volumeGuid) {
    if (volumeGuid.empty()) {
        return L"<Volume GUID unavailable>";
    }
    DWORD needed = 0;
    ::GetVolumePathNamesForVolumeNameW(volumeGuid.c_str(), nullptr, 0, &needed);
    if (needed == 0) {
        return L"<no mount points or query unsupported>";
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(needed) + 2U, L'\0');
    if (!::GetVolumePathNamesForVolumeNameW(volumeGuid.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), &needed)) {
        return L"查询失败: " + FormatWin32Error(::GetLastError());
    }
    std::wstring result;
    const wchar_t* current = buffer.data();
    while (*current) {
        if (!result.empty()) {
            result += L"; ";
        }
        result += current;
        current += std::wcslen(current) + 1U;
    }
    return result.empty() ? L"<no mount points>" : result;
}

// QueryVolumeInfoRows appends Win32 volume metadata. Inputs are a volume root and
// output row vector; processing uses GetVolumeInformation and related read-only
// APIs; no handles or volume state are changed.
void AppendVolumeInfoRows(std::vector<AuditRow>& rows, const std::wstring& volumeRoot) {
    wchar_t label[MAX_PATH]{};
    wchar_t fsName[MAX_PATH]{};
    DWORD serial = 0;
    DWORD maxComponent = 0;
    DWORD flags = 0;
    if (::GetVolumeInformationW(volumeRoot.c_str(), label, static_cast<DWORD>(std::size(label)), &serial, &maxComponent, &flags, fsName, static_cast<DWORD>(std::size(fsName)))) {
        AddRow(rows, L"FileSystem", L"GetVolumeInformation", L"OK", fsName[0] ? fsName : L"<empty>", L"Volume label=" + std::wstring(label) + L", Serial=" + HexText(serial) + L", Flags=" + HexText(flags));
    } else {
        AddRow(rows, L"FileSystem", L"GetVolumeInformation", L"数据不可用", FormatWin32Error(::GetLastError()), L"可能是权限不足、卷离线、网络路径或非本地卷。");
    }
    ULARGE_INTEGER freeBytesAvailable{};
    ULARGE_INTEGER totalBytes{};
    ULARGE_INTEGER totalFree{};
    if (::GetDiskFreeSpaceExW(volumeRoot.c_str(), &freeBytesAvailable, &totalBytes, &totalFree)) {
        AddRow(rows, L"Capacity", L"GetDiskFreeSpaceEx", L"OK", std::to_wstring(totalBytes.QuadPart), L"Free=" + std::to_wstring(totalFree.QuadPart) + L", CallerFree=" + std::to_wstring(freeBytesAvailable.QuadPart));
    } else {
        AddRow(rows, L"Capacity", L"GetDiskFreeSpaceEx", L"数据不可用", FormatWin32Error(::GetLastError()), L"容量信息不可用。");
    }
}

// QueryStorageRows builds the Storage/Volume/MountMgr read-only view. Input is
// the current target path; processing uses ArkDriverClient storage wrappers plus
// documented R3 APIs for cross-view context; output rows are suitable for the
// shared audit grid.
std::vector<AuditRow> QueryStorageRows(const std::wstring& targetPath) {
    std::vector<AuditRow> rows;
    const ksword::ark::DriverClient client;
    const ksword::ark::StorageVolumeStackAuditResult r0Stack = client.queryVolumeStackAudit();
    AddRow(rows, L"R0 VolumeStack", L"ArkDriverClient::queryVolumeStackAudit", r0Stack.io.ok ? L"OK" : (r0Stack.unsupported ? L"驱动不支持" : L"驱动不可用/权限不足"), std::to_wstring(r0Stack.returnedCount) + L"/" + std::to_wstring(r0Stack.totalCount), Utf8ToWide(r0Stack.io.message));
    for (const KSWORD_ARK_VOLUME_STACK_ROW& row : r0Stack.rows) {
        std::wostringstream detail;
        detail << L"Device=" << HexText(row.deviceObjectAddress)
               << L", Driver=" << HexText(row.driverObjectAddress)
               << L", Attached=" << HexText(row.attachedDeviceAddress)
               << L", Risk=" << HexText(row.riskFlags)
               << L", Confidence=" << row.confidence;
        AddRow(rows, row.driverName[0] ? row.driverName : L"<R0 driver>", L"R0 VolumeStack", std::to_wstring(row.stackIndex), row.volumeDeviceName[0] ? row.volumeDeviceName : L"<volume unavailable>", detail.str());
    }
    const ksword::ark::StorageMountMgrMappingAuditResult r0Mount = client.queryMountMgrMappingAudit();
    AddRow(rows, L"R0 MountMgr", L"ArkDriverClient::queryMountMgrMappingAudit", r0Mount.io.ok ? L"OK" : (r0Mount.unsupported ? L"驱动不支持" : L"驱动不可用/权限不足"), std::to_wstring(r0Mount.returnedCount) + L"/" + std::to_wstring(r0Mount.totalCount), Utf8ToWide(r0Mount.io.message));
    for (const KSWORD_ARK_MOUNTMGR_MAPPING_ROW& row : r0Mount.rows) {
        std::wostringstream detail;
        detail << L"Confidence=" << row.confidence
               << L", Risk=" << HexText(row.riskFlags)
               << L", Status=" << HexText(static_cast<std::uint32_t>(row.lastStatus));
        AddRow(rows, row.driveLetter[0] ? row.driveLetter : L"<mount>", L"R0 MountMgr", L"OK", row.volumeGuid[0] ? row.volumeGuid : L"<guid unavailable>", row.ntDevicePath[0] ? std::wstring(row.ntDevicePath) + L"; " + detail.str() : detail.str());
    }
    const ksword::ark::StorageFilesystemIntegrityAuditResult r0Fs = client.queryFilesystemIntegrityAudit();
    AddRow(rows, L"R0 FilesystemIntegrity", L"ArkDriverClient::queryFilesystemIntegrityAudit", r0Fs.io.ok ? L"OK" : (r0Fs.unsupported ? L"驱动不支持" : L"驱动不可用/权限不足"), std::to_wstring(r0Fs.returnedCount) + L"/" + std::to_wstring(r0Fs.totalCount), Utf8ToWide(r0Fs.io.message));
    const std::wstring volumeRoot = VolumeRootFromPath(targetPath);
    AddRow(rows, L"VolumeRoot", L"GetVolumePathName", volumeRoot.empty() ? L"数据不可用" : L"OK", volumeRoot.empty() ? L"<unknown>" : volumeRoot, L"从目标路径推导卷根。");
    if (volumeRoot.empty()) {
        AddRow(rows, L"Storage", L"Win32", L"数据不可用", L"<no volume root>", L"无法定位卷根，后续 MountMgr/Volume 查询降级。");
        return rows;
    }

    DWORD guidError = ERROR_SUCCESS;
    const std::wstring volumeGuid = QueryVolumeGuid(volumeRoot, &guidError);
    AddRow(rows, L"VolumeGuid", L"GetVolumeNameForVolumeMountPoint", volumeGuid.empty() ? L"数据不可用" : L"OK", volumeGuid.empty() ? FormatWin32Error(guidError) : volumeGuid, L"MountMgr 公开视图中的 Volume GUID 路径。");
    DWORD deviceError = ERROR_SUCCESS;
    const std::wstring ntDevice = QueryDosDeviceForVolume(volumeRoot, &deviceError);
    AddRow(rows, L"NtDevicePath", L"QueryDosDevice", ntDevice.empty() ? L"数据不可用" : L"OK", ntDevice.empty() ? FormatWin32Error(deviceError) : ntDevice, L"DOS 盘符到 NT 设备路径的公开映射。");
    AddRow(rows, L"MountPoints", L"GetVolumePathNamesForVolumeName", volumeGuid.empty() ? L"数据不可用" : L"OK", QueryMountPoints(volumeGuid), L"公开 MountMgr cross-view；不删除 stale mapping。");
    AppendVolumeInfoRows(rows, volumeRoot);
    AddRow(rows, L"VolumeStack", L"R3 Win32 cross-view", r0Stack.io.ok ? L"R0 已接入" : L"数据不可用", r0Stack.io.ok ? L"R0 volume stack wrapper available" : L"<R0 volume stack unavailable>", L"R3 Win32 cross-view 仍保留，用于和 R0 结果对照。");
    AddRow(rows, L"安全边界", L"UI", L"只读", L"无卸载/解锁/绕过", L"本页不调用卸载卷、修改挂载点、BitLocker 解锁或密钥导出接口。");
    return rows;
}

// WmiPropertyToText reads one VARIANT property into a display string. Inputs are
// an IWbemClassObject and property name; processing handles common scalar types;
// output is empty when the property is absent.
std::wstring WmiPropertyToText(IWbemClassObject* object, const wchar_t* name) {
    if (!object || !name) {
        return {};
    }
    VARIANT value;
    ::VariantInit(&value);
    const HRESULT hr = object->Get(name, 0, &value, nullptr, nullptr);
    if (FAILED(hr)) {
        ::VariantClear(&value);
        return {};
    }
    std::wstring text;
    switch (value.vt) {
    case VT_BSTR:
        text = value.bstrVal ? value.bstrVal : L"";
        break;
    case VT_I4:
        text = std::to_wstring(value.lVal);
        break;
    case VT_UI4:
        text = std::to_wstring(value.ulVal);
        break;
    case VT_BOOL:
        text = (value.boolVal == VARIANT_TRUE) ? L"true" : L"false";
        break;
    default:
        break;
    }
    ::VariantClear(&value);
    return text;
}

// AppendBitLockerWmiRows queries Win32_EncryptableVolume status labels. Input is
// a drive root such as C:\; processing uses WMI read-only SELECT and only emits
// status fields; no key protector material is requested or exported.
void AppendBitLockerWmiRows(std::vector<AuditRow>& rows, const std::wstring& volumeRoot) {
    if (volumeRoot.size() < 2 || volumeRoot[1] != L':') {
        AddRow(rows, L"BitLocker WMI", L"Win32_EncryptableVolume", L"数据不可用", L"<no drive letter>", L"WMI BitLocker 类按 DriveLetter 查询；无盘符卷降级。");
        return;
    }

    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        AddRow(rows, L"BitLocker WMI", L"CoInitializeEx", L"数据不可用", FormatHresult(init), L"COM 初始化失败。");
        return;
    }

    IWbemLocator* locator = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&locator));
    if (FAILED(hr) || !locator) {
        AddRow(rows, L"BitLocker WMI", L"CoCreateInstance", L"数据不可用", FormatHresult(hr), L"无法创建 IWbemLocator。");
        if (uninit) {
            ::CoUninitialize();
        }
        return;
    }

    IWbemServices* services = nullptr;
    BSTR namespaceName = ::SysAllocString(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
    hr = namespaceName
        ? locator->ConnectServer(namespaceName, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services)
        : E_OUTOFMEMORY;
    if (namespaceName) {
        ::SysFreeString(namespaceName);
    }
    locator->Release();
    if (FAILED(hr) || !services) {
        AddRow(rows, L"BitLocker WMI", L"ConnectServer", L"驱动不支持/权限不足", FormatHresult(hr), L"命名空间不可用通常表示系统版本/权限/服务不支持。");
        if (uninit) {
            ::CoUninitialize();
        }
        return;
    }

    hr = ::CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        AddRow(rows, L"BitLocker WMI", L"CoSetProxyBlanket", L"权限不足", FormatHresult(hr), L"无法设置 WMI 调用安全上下文。");
        services->Release();
        if (uninit) {
            ::CoUninitialize();
        }
        return;
    }

    std::wstring query = L"SELECT DriveLetter,ConversionStatus,ProtectionStatus,LockStatus,EncryptionMethod FROM Win32_EncryptableVolume WHERE DriveLetter='";
    query.push_back(volumeRoot[0]);
    query += L":'";
    IEnumWbemClassObject* enumerator = nullptr;
    BSTR wql = ::SysAllocString(L"WQL");
    BSTR queryText = ::SysAllocString(query.c_str());
    hr = (wql && queryText)
        ? services->ExecQuery(wql, queryText, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator)
        : E_OUTOFMEMORY;
    if (queryText) {
        ::SysFreeString(queryText);
    }
    if (wql) {
        ::SysFreeString(wql);
    }
    services->Release();
    if (FAILED(hr) || !enumerator) {
        AddRow(rows, L"BitLocker WMI", L"ExecQuery", L"数据不可用", FormatHresult(hr), L"只读查询失败；未调用任何解锁或 protector 方法。");
        if (uninit) {
            ::CoUninitialize();
        }
        return;
    }

    ULONG returned = 0;
    IWbemClassObject* object = nullptr;
    hr = enumerator->Next(2500, 1, &object, &returned);
    if (SUCCEEDED(hr) && returned == 1 && object) {
        AddRow(rows, L"DriveLetter", L"Win32_EncryptableVolume", L"OK", WmiPropertyToText(object, L"DriveLetter"), L"BitLocker WMI 可见卷。");
        AddRow(rows, L"ConversionStatus", L"Win32_EncryptableVolume", L"OK", WmiPropertyToText(object, L"ConversionStatus"), L"状态枚举值，UI 不推断密钥材料。");
        AddRow(rows, L"ProtectionStatus", L"Win32_EncryptableVolume", L"OK", WmiPropertyToText(object, L"ProtectionStatus"), L"保护状态枚举值。");
        AddRow(rows, L"LockStatus", L"Win32_EncryptableVolume", L"OK", WmiPropertyToText(object, L"LockStatus"), L"锁定状态枚举值。");
        AddRow(rows, L"EncryptionMethod", L"Win32_EncryptableVolume", L"OK", WmiPropertyToText(object, L"EncryptionMethod"), L"加密方法枚举值；不导出密钥。");
        object->Release();
    } else {
        AddRow(rows, L"BitLocker WMI", L"Win32_EncryptableVolume", L"数据不可用", SUCCEEDED(hr) ? L"<no row>" : FormatHresult(hr), L"该卷可能未启用 BitLocker、无权限或 WMI 未返回 DriveLetter 行。");
    }
    enumerator->Release();
    if (uninit) {
        ::CoUninitialize();
    }
}

// QueryBitLockerRows builds the BitLocker status page. Input is the target path;
// processing resolves the volume and queries WMI/status-only evidence; output
// never contains key material, protector payloads or unlock actions.
std::vector<AuditRow> QueryBitLockerRows(const std::wstring& targetPath) {
    std::vector<AuditRow> rows;
    const ksword::ark::DriverClient client;
    const ksword::ark::StorageBitlockerFveAuditResult r0Fve = client.queryBitlockerFveAudit();
    AddRow(rows, L"R0 FVE audit", L"ArkDriverClient::queryBitlockerFveAudit", r0Fve.io.ok ? L"OK" : (r0Fve.unsupported ? L"驱动不支持" : L"驱动不可用/权限不足"), std::to_wstring(r0Fve.returnedCount) + L"/" + std::to_wstring(r0Fve.totalCount), Utf8ToWide(r0Fve.io.message));
    for (const KSWORD_ARK_BITLOCKER_FVE_ROW& row : r0Fve.rows) {
        std::wostringstream detail;
        detail << L"Fvevol=" << row.fvevolPresent
               << L", StackPos=" << row.fvevolStackPosition
               << L", Protection=" << row.protectionStatus
               << L", Conversion=" << row.conversionStatus
               << L", Lock=" << row.lockStatus
               << L", Confidence=" << row.confidence
               << L", Risk=" << HexText(row.riskFlags);
        AddRow(rows, row.volumeDeviceName[0] ? row.volumeDeviceName : L"<R0 volume>", L"R0 BitLocker/FVE", L"OK", detail.str(), row.detail[0] ? row.detail : L"协议不返回密钥材料。");
    }
    const std::wstring volumeRoot = VolumeRootFromPath(targetPath);
    AddRow(rows, L"VolumeRoot", L"GetVolumePathName", volumeRoot.empty() ? L"数据不可用" : L"OK", volumeRoot.empty() ? L"<unknown>" : volumeRoot, L"BitLocker 状态按卷聚合展示。");
    AppendBitLockerWmiRows(rows, volumeRoot);
    AddRow(rows, L"安全边界", L"UI", L"只读", L"不导出密钥/不解锁/不暂停保护", L"本页只读取 WMI 状态标签，不调用 GetKeyProtectors、UnlockWith*、DisableKeyProtectors 等方法。");
    return rows;
}


// StateFromHostWindow returns the outer host state from GWLP_USERDATA. Input is
// the host HWND; output may be null during creation/destruction.
FileFeatureHostState* StateFromHostWindow(HWND hwnd) {
    return reinterpret_cast<FileFeatureHostState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// StateFromAuditWindow returns the audit page state from GWLP_USERDATA. Input is
// the audit HWND; output may be null during creation/destruction.
FileAuditPageState* StateFromAuditWindow(HWND hwnd) {
    return reinterpret_cast<FileAuditPageState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// SetAuditStatus writes one status line for the audit page. Inputs are state and
// text; processing updates the STATIC control; there is no return value.
void SetAuditStatus(FileAuditPageState& state, const std::wstring& text) {
    if (state.status) {
        ::SetWindowTextW(state.status, text.c_str());
    }
}

// ShowHostPages toggles the outer File browser/audit pages. Input is host state;
// processing hides inactive child HWNDs and shows the selected tab; no return.
void ShowHostPages(FileFeatureHostState& state) {
    if (state.browserPage) {
        ::ShowWindow(state.browserPage, state.currentTab == kBrowserTabIndex ? SW_SHOW : SW_HIDE);
    }
    if (state.auditPage) {
        ::ShowWindow(state.auditPage, state.currentTab == kAuditTabIndex ? SW_SHOW : SW_HIDE);
    }
}

// LayoutHostChildren sizes the outer tab and its two child pages. Input is host
// state; processing uses the current tab display rectangle; no value is returned.
void LayoutHostChildren(FileFeatureHostState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    ::MoveWindow(state.tab, 0, 0, Width(rc), Height(rc), TRUE);
    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    const int pageW = Width(display);
    const int pageH = Height(display);
    if (state.browserPage) {
        ::MoveWindow(state.browserPage, display.left, display.top, pageW, pageH, TRUE);
    }
    if (state.auditPage) {
        ::MoveWindow(state.auditPage, display.left, display.top, pageW, pageH, TRUE);
    }
    ShowHostPages(state);
}

// AddAuditColumns creates the shared audit table columns. Input is a report
// list HWND; processing inserts five stable columns; output is true on success.
bool AddAuditColumns(HWND list) {
    return Ksword::Ui::AddListViewColumns(list, {
        { 0, 210, LVCFMT_LEFT, L"项目" },
        { 1, 190, LVCFMT_LEFT, L"数据来源" },
        { 2, 130, LVCFMT_LEFT, L"状态/降级" },
        { 3, 260, LVCFMT_LEFT, L"值" },
        { 4, 560, LVCFMT_LEFT, L"详情" },
    });
}

// QueryRowsForCurrentTab dispatches the active audit subpage query. Input is
// audit state and target text; processing never calls mutating APIs; output is
// the fresh table model.
std::vector<AuditRow> QueryRowsForCurrentTab(const FileAuditPageState& state, const std::wstring& targetPath) {
    switch (state.currentTab) {
    case kAuditMinifilterTabIndex:
        return QueryMinifilterRows();
    case kAuditFileObjectTabIndex:
        return QueryFileObjectRows(targetPath);
    case kAuditSectionTabIndex:
        return QuerySectionRows(targetPath);
    case kAuditStorageTabIndex:
        return QueryStorageRows(targetPath);
    case kAuditBitLockerTabIndex:
        return QueryBitLockerRows(targetPath);
    default:
        break;
    }
    std::vector<AuditRow> rows;
    AddRow(rows, L"Audit", L"UI", L"数据不可用", L"<unknown tab>", L"未知文件审计页索引。");
    return rows;
}

// PopulateAuditList writes the current row model into the ListView. Input is the
// audit page state; processing deletes old rows and inserts new text rows; no
// value is returned.
void PopulateAuditList(FileAuditPageState& state) {
    Ksword::Ui::ScopedListViewRedrawLock redrawLock(state.list);
    Ksword::Ui::ClearListViewRows(state.list);
    for (const AuditRow& row : state.rows) {
        Ksword::Ui::InsertListViewTextRow(state.list, { row.item, row.source, row.status, row.value, row.detail });
    }
}

// RefreshAuditRows re-runs the active read-only audit query. Input is page
// state; processing updates rows/list/status; no value is returned.
void RefreshAuditRows(FileAuditPageState& state) {
    const std::wstring target = TextFromWindow(state.targetEdit);
    state.rows = QueryRowsForCurrentTab(state, target);
    PopulateAuditList(state);
    std::wstring tabName;
    switch (state.currentTab) {
    case kAuditMinifilterTabIndex: tabName = L"Minifilter"; break;
    case kAuditFileObjectTabIndex: tabName = L"FileObject"; break;
    case kAuditSectionTabIndex: tabName = L"Section / ControlArea"; break;
    case kAuditStorageTabIndex: tabName = L"Storage / Volume / MountMgr"; break;
    case kAuditBitLockerTabIndex: tabName = L"BitLocker"; break;
    default: tabName = L"Audit"; break;
    }
    SetAuditStatus(state, L"状态：" + tabName + L" 只读审计完成，行数=" + std::to_wstring(state.rows.size()) + L"。灰/黄状态表示驱动不支持、数据不可用或权限不足。");
}

// LayoutAuditChildren sizes the audit controls. Input is audit state; processing
// uses the current client rectangle and tab display area; no value is returned.
void LayoutAuditChildren(FileAuditPageState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int margin = 8;
    const int buttonW = 82;
    const int topH = 26;
    const int statusH = 22;
    const int gap = 6;
    const int editW = std::max(160, Width(rc) - margin * 2 - buttonW - gap);
    ::MoveWindow(state.targetEdit, margin, margin, editW, topH, TRUE);
    ::MoveWindow(state.refreshButton, margin + editW + gap, margin, buttonW, topH, TRUE);

    const int tabTop = margin + topH + gap;
    const int tabH = std::max(80, Height(rc) - tabTop - statusH - margin);
    ::MoveWindow(state.tab, margin, tabTop, std::max(80, Width(rc) - margin * 2), tabH, TRUE);
    RECT display = Ksword::Ui::GetTabDisplayRect(state.tab);
    ::MoveWindow(state.list, display.left, display.top, Width(display), Height(display), TRUE);
    ::MoveWindow(state.status, margin, tabTop + tabH + 2, std::max(80, Width(rc) - margin * 2), statusH, TRUE);
}

// CreateAuditControls builds the read-only audit subpage controls. Input is an
// initialized audit state; processing creates edit/button/tabs/list/status and
// seeds the target path; output is true when required HWNDs exist.
bool CreateAuditControls(FileAuditPageState& state) {
    state.targetEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", DefaultAuditTarget().c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAuditTargetEditId)), ::GetModuleHandleW(nullptr), nullptr);
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kAuditRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kAuditInnerTabId, 0, 0, 0, 0);
    state.list = Ksword::Ui::CreateReportListView(state.tab, kAuditListId, 0, 0, 0, 0);
    state.status = Ksword::Ui::CreateText(state.hwnd, kAuditStatusId, L"状态：等待刷新。", 0, 0, 0, 0);
    if (!state.targetEdit || !state.refreshButton || !state.tab || !state.list || !state.status) {
        return false;
    }
    ::SendMessageW(state.targetEdit, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    Ksword::Ui::AddTabPage(state.tab, kAuditMinifilterTabIndex, { L"Minifilter" });
    Ksword::Ui::AddTabPage(state.tab, kAuditFileObjectTabIndex, { L"FileObject" });
    Ksword::Ui::AddTabPage(state.tab, kAuditSectionTabIndex, { L"Section / ControlArea" });
    Ksword::Ui::AddTabPage(state.tab, kAuditStorageTabIndex, { L"Storage / MountMgr" });
    Ksword::Ui::AddTabPage(state.tab, kAuditBitLockerTabIndex, { L"BitLocker" });
    ::SendMessageW(state.tab, TCM_SETCURSEL, static_cast<WPARAM>(kAuditMinifilterTabIndex), 0);
    return AddAuditColumns(state.list);
}

// RegisterAuditClass registers the audit page window class. There is no input;
// processing is idempotent; output is true once the class is usable.
bool RegisterAuditClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        FileAuditPageState* state = StateFromAuditWindow(hwnd);
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<FileAuditPageState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }
        switch (msg) {
        case WM_CREATE:
            if (state) {
                if (!CreateAuditControls(*state)) {
                    delete state;
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    return -1;
                }
                LayoutAuditChildren(*state);
                RefreshAuditRows(*state);
            }
            return 0;
        case WM_SIZE:
            if (state) {
                LayoutAuditChildren(*state);
            }
            return 0;
        case WM_COMMAND:
            if (state && LOWORD(wParam) == kAuditRefreshButtonId) {
                RefreshAuditRows(*state);
                return 0;
            }
            if (state && LOWORD(wParam) == kAuditTargetEditId && HIWORD(wParam) == EN_UPDATE) {
                SetAuditStatus(*state, L"状态：目标路径已修改，点击刷新重新采集只读审计数据。");
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
                    const LRESULT selected = ::SendMessageW(state->tab, TCM_GETCURSEL, 0, 0);
                    if (selected >= 0) {
                        state->currentTab = static_cast<int>(selected);
                    }
                    RefreshAuditRows(*state);
                    return 0;
                }
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kFileAuditClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

// CreateFileAuditPage creates one audit child page. Inputs are parent and bounds;
// processing allocates page state and creates the registered class; output is an
// HWND or nullptr when registration/window creation fails.
HWND CreateFileAuditPage(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterAuditClass()) {
        return nullptr;
    }
    auto* state = new FileAuditPageState();
    HWND hwnd = ::CreateWindowExW(0, kFileAuditClass, L"FileAudit", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, Width(bounds), Height(bounds), parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

// CreateHostControls builds the outer File feature tabs and child pages. Input
// is host state; processing creates the existing browser page plus audit page;
// output is true when both children are available.
bool CreateHostControls(FileFeatureHostState& state) {
    state.tab = Ksword::Ui::CreateTabControl(state.hwnd, kHostTabId, 0, 0, 0, 0);
    if (!state.tab) {
        return false;
    }
    Ksword::Ui::AddTabPage(state.tab, kBrowserTabIndex, { L"文件浏览" });
    Ksword::Ui::AddTabPage(state.tab, kAuditTabIndex, { L"只读审计" });
    ::SendMessageW(state.tab, TCM_SETCURSEL, static_cast<WPARAM>(kBrowserTabIndex), 0);

    RECT pageRect{};
    ::GetClientRect(state.tab, &pageRect);
    pageRect = Ksword::Ui::GetTabDisplayRect(state.tab);
    const RECT childBounds{ 0, 0, std::max(1, Width(pageRect)), std::max(1, Height(pageRect)) };
    state.browserPage = CreateFileViewPage(state.tab, childBounds);
    state.auditPage = CreateFileAuditPage(state.tab, childBounds);
    if (!state.browserPage || !state.auditPage) {
        return false;
    }
    ShowHostPages(state);
    return true;
}

// RegisterHostClass registers the outer File feature host. There is no input;
// processing is idempotent; output reports whether CreateFileFeaturePage can
// instantiate the class.
bool RegisterHostClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        FileFeatureHostState* state = StateFromHostWindow(hwnd);
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<FileFeatureHostState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }
        switch (msg) {
        case WM_CREATE:
            if (state) {
                if (!CreateHostControls(*state)) {
                    delete state;
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    return -1;
                }
                LayoutHostChildren(*state);
            }
            return 0;
        case WM_SIZE:
            if (state) {
                LayoutHostChildren(*state);
            }
            return 0;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
                    const LRESULT selected = ::SendMessageW(state->tab, TCM_GETCURSEL, 0, 0);
                    if (selected >= 0) {
                        state->currentTab = static_cast<int>(selected);
                    }
                    ShowHostPages(*state);
                    return 0;
                }
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kFileHostClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateFileFeaturePage(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterHostClass()) {
        return nullptr;
    }
    auto* state = new FileFeatureHostState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kFileHostClass,
        L"File",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::File
