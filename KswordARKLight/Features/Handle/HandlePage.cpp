#include "HandlePage.h"

#include "HandleClient.h"

#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/TabUtil.h"
#include "../../Ui/Theme.h"

#include "../../../shared/driver/KswordArkHandleIoctl.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Handle {

constexpr wchar_t kHandlePageClass[] = L"KswordARKLight.HandlePage";
constexpr int kPidEditId = 57001;
constexpr int kRefreshButtonId = 57002;
constexpr int kStatusTextId = 57003;
constexpr int kTabId = 57004;
constexpr int kHandleListId = 57005;
constexpr int kDetailListId = 57006;
constexpr int kHandleTabIndex = 0;
constexpr int kDetailTabIndex = 1;

// HandlePageState stores query snapshots separately from HWND fields so the
// public header remains compact. Inputs are written by Refresh/PopulateDetail;
// output behavior is value ownership for page lifetime.
struct HandlePageState {
    HandleAuditClient client;
    HandleEnumView snapshot;
    HandleObjectDetailView detail;
};

namespace {

// Width returns a non-negative RECT width. Input is any RECT; output is pixels.
int Width(const RECT& rect) {
    return rect.right > rect.left ? rect.right - rect.left : 0;
}

// Height returns a non-negative RECT height. Input is any RECT; output is pixels.
int Height(const RECT& rect) {
    return rect.bottom > rect.top ? rect.bottom - rect.top : 0;
}

// Hex32 formats 32-bit diagnostics. Input is an integer; output is uppercase
// hexadecimal with 0x prefix for table display.
std::wstring Hex32(const std::uint32_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << value;
    return stream.str();
}

// Hex64 formats pointer-sized or 64-bit diagnostics. Input is an integer;
// output is uppercase hexadecimal with fixed 16-digit width.
std::wstring Hex64(const std::uint64_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return stream.str();
}

// NtStatusText formats signed NTSTATUS values as unsigned hex. Input is the
// status integer; output keeps success/failure codes readable.
std::wstring NtStatusText(const long status) {
    return Hex32(static_cast<std::uint32_t>(status));
}

// Utf8ToWide converts ArkDriverClient diagnostic strings to UTF-16. Input is a
// narrow string that is normally ASCII; processing uses byte-preserving widening;
// output is safe for Win32 controls.
std::wstring Utf8ToWide(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const unsigned char ch : text) {
        wide.push_back(static_cast<wchar_t>(ch));
    }
    return wide;
}

// BoolText returns a Chinese yes/no label. Input is a boolean; output is a
// stable UI string.
const wchar_t* BoolText(const bool value) {
    return value ? L"是" : L"否";
}

// DecodeStatusText maps handle-table decode status to readable labels. Input is
// KSWORD_ARK_HANDLE_DECODE_STATUS_*; output is a static UI label.
const wchar_t* DecodeStatusText(const std::uint32_t status) {
    switch (status) {
    case KSWORD_ARK_HANDLE_DECODE_STATUS_OK: return L"OK";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_PARTIAL: return L"Partial";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_DYNDATA_MISSING: return L"DynData缺失";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_LOOKUP_FAILED: return L"进程查找失败";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_EXITING: return L"进程退出中";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_HANDLE_TABLE_MISSING: return L"HandleTable缺失";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_OBJECT_DECODE_FAILED: return L"对象解码失败";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_TYPE_DECODE_FAILED: return L"类型解码失败";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_READ_FAILED: return L"读取失败";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_BUFFER_TOO_SMALL: return L"缓冲区不足";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_HEADER_DYNDATA_MISSING: return L"ObjectHeader偏移缺失";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_HEADER_READ_FAILED: return L"ObjectHeader读取失败";
    case KSWORD_ARK_HANDLE_DECODE_STATUS_ACCESS_DECODE_FAILED: return L"访问掩码解码失败";
    default: return L"Unavailable";
    }
}

// QueryStatusText maps object-query status to readable labels. Input is
// KSWORD_ARK_OBJECT_QUERY_STATUS_*; output is a static UI label.
const wchar_t* QueryStatusText(const std::uint32_t status) {
    switch (status) {
    case KSWORD_ARK_OBJECT_QUERY_STATUS_OK: return L"OK";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_PARTIAL: return L"Partial";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_DYNDATA_MISSING: return L"DynData缺失";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_PROCESS_LOOKUP_FAILED: return L"进程查找失败";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_HANDLE_REFERENCE_FAILED: return L"句柄引用失败";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_TYPE_QUERY_FAILED: return L"类型查询失败";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_QUERY_FAILED: return L"名称查询失败";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_TRUNCATED: return L"名称截断";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_HEADER_DYNDATA_MISSING: return L"ObjectHeader偏移缺失";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_HEADER_QUERY_FAILED: return L"ObjectHeader查询失败";
    case KSWORD_ARK_OBJECT_QUERY_STATUS_ACCESS_DECODE_FAILED: return L"访问掩码解码失败";
    default: return L"Unavailable";
    }
}

// TypeSourceText describes where the type index came from. Input is
// KSWORD_ARK_OBJECT_TYPE_SOURCE_*; output is a readable source label.
const wchar_t* TypeSourceText(const std::uint32_t source) {
    switch (source) {
    case KSWORD_ARK_OBJECT_TYPE_SOURCE_OBJECT_TYPE_INDEX: return L"OBJECT_TYPE.Index";
    case KSWORD_ARK_OBJECT_TYPE_SOURCE_OBJECT_HEADER: return L"OBJECT_HEADER.TypeIndex";
    case KSWORD_ARK_OBJECT_TYPE_SOURCE_BOTH_MATCH: return L"双源一致";
    case KSWORD_ARK_OBJECT_TYPE_SOURCE_BOTH_MISMATCH: return L"双源不一致";
    default: return L"None";
    }
}

// NameInfoStatusText describes object-name query status. Input is
// KSWORD_ARK_OBJECT_NAME_INFO_STATUS_*; output is a readable label.
const wchar_t* NameInfoStatusText(const std::uint32_t status) {
    switch (status) {
    case KSWORD_ARK_OBJECT_NAME_INFO_STATUS_NOT_REQUESTED: return L"未请求";
    case KSWORD_ARK_OBJECT_NAME_INFO_STATUS_HEADER_MASK_EMPTY: return L"InfoMask空";
    case KSWORD_ARK_OBJECT_NAME_INFO_STATUS_HEADER_MASK_NONZERO: return L"InfoMask非零";
    case KSWORD_ARK_OBJECT_NAME_INFO_STATUS_QUERY_OK: return L"查询成功";
    case KSWORD_ARK_OBJECT_NAME_INFO_STATUS_QUERY_FAILED: return L"查询失败";
    case KSWORD_ARK_OBJECT_NAME_INFO_STATUS_QUERY_TRUNCATED: return L"查询截断";
    default: return L"未知";
    }
}

// FieldPresent checks one protocol field flag. Inputs are a flag bitmap and one
// KSWORD_ARK_HANDLE_FIELD_* bit; output says whether the field is present.
bool FieldPresent(const std::uint32_t flags, const std::uint32_t bit) {
    return (flags & bit) != 0U;
}

// DetailFieldPresent checks one object-query field flag. Inputs are a flag
// bitmap and one KSWORD_ARK_OBJECT_INFO_FIELD_* bit; output says field presence.
bool DetailFieldPresent(const std::uint32_t flags, const std::uint32_t bit) {
    return (flags & bit) != 0U;
}

// FieldText renders a present/missing marker. Input is a boolean; output is a
// short Chinese status value.
std::wstring FieldText(const bool present) {
    return present ? L"Present" : L"Missing";
}

// AnomalyText derives read-only risk labels from row diagnostics. Input is a
// parsed handle entry; output is a semicolon-separated label list for UI.
std::wstring AnomalyText(const HandleEntryView& entry) {
    std::vector<std::wstring> labels;
    if (entry.decodeStatus != KSWORD_ARK_HANDLE_DECODE_STATUS_OK) {
        labels.push_back(DecodeStatusText(entry.decodeStatus));
    }
    if (entry.objectTypeIndexSource == KSWORD_ARK_OBJECT_TYPE_SOURCE_BOTH_MISMATCH) {
        labels.push_back(L"ObjectType索引不一致");
    }
    if (entry.objectAddress == 0 && FieldPresent(entry.fieldFlags, KSWORD_ARK_HANDLE_FIELD_OBJECT_PRESENT)) {
        labels.push_back(L"对象地址为空");
    }
    if (entry.objectHeaderAddress == 0 &&
        FieldPresent(entry.fieldFlags, KSWORD_ARK_HANDLE_FIELD_OBJECT_HEADER_PRESENT)) {
        labels.push_back(L"ObjectHeader为空");
    }
    if (entry.grantedAccessDecodeStatus == KSWORD_ARK_HANDLE_DECODE_STATUS_ACCESS_DECODE_FAILED) {
        labels.push_back(L"GrantedAccess解码失败");
    }
    if (entry.pointerCount < 0) {
        labels.push_back(L"PointerCount异常");
    }
    if (labels.empty()) {
        return L"";
    }
    std::wstring text;
    for (const std::wstring& label : labels) {
        if (!text.empty()) {
            text += L"; ";
        }
        text += label;
    }
    return text;
}

// ReadPidEdit parses the PID edit box. Input is the edit HWND; processing
// accepts decimal text only; output is zero when parsing fails.
std::uint32_t ReadPidEdit(HWND edit) {
    wchar_t buffer[64]{};
    if (!edit || ::GetWindowTextW(edit, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0]))) <= 0) {
        return 0;
    }
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(buffer, &end, 10);
    while (end && *end != L'\0' && iswspace(*end)) {
        ++end;
    }
    if (!end || *end != L'\0' || value == 0UL || value > 0xFFFFFFFFUL) {
        return 0;
    }
    return static_cast<std::uint32_t>(value);
}

// RegisterHandlePageClass registers the window class once. There is no input;
// processing is idempotent; output reports whether CreateWindowExW may use it.
bool RegisterHandlePageClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = HandlePage::WindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kHandlePageClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

// AddDetailRow appends one key/value/status row. Inputs are the ListView and
// display cells; processing appends a report row; no value is returned.
void AddDetailRow(HWND list, const std::wstring& name, const std::wstring& value, const std::wstring& status) {
    Ksword::Ui::InsertListViewTextRow(list, { name, value, status });
}

// InsertHandleColumns initializes the handle table columns. Input is the
// ListView HWND; processing inserts fixed report columns; no value is returned.
void InsertHandleColumns(HWND list) {
    Ksword::Ui::AddListViewColumns(list, {
        { 0, 80, LVCFMT_RIGHT, L"PID" },
        { 1, 90, LVCFMT_RIGHT, L"Handle" },
        { 2, 165, LVCFMT_LEFT, L"Object" },
        { 3, 165, LVCFMT_LEFT, L"ObjectHeader" },
        { 4, 165, LVCFMT_LEFT, L"ObjectType" },
        { 5, 70, LVCFMT_RIGHT, L"TypeIdx" },
        { 6, 110, LVCFMT_RIGHT, L"GrantedAccess" },
        { 7, 100, LVCFMT_RIGHT, L"Attributes" },
        { 8, 100, LVCFMT_RIGHT, L"PtrCount" },
        { 9, 100, LVCFMT_RIGHT, L"HandleCount" },
        { 10, 150, LVCFMT_LEFT, L"Decode" },
        { 11, 220, LVCFMT_LEFT, L"异常句柄标记" },
    });
}

// InsertDetailColumns initializes the detail table columns. Input is the
// ListView HWND; processing inserts name/value/status columns; no return.
void InsertDetailColumns(HWND list) {
    Ksword::Ui::AddListViewColumns(list, {
        { 0, 210, LVCFMT_LEFT, L"字段" },
        { 1, 420, LVCFMT_LEFT, L"值" },
        { 2, 220, LVCFMT_LEFT, L"状态/来源" },
    });
}

} // namespace

HWND HandlePage::Create(HWND parent, const RECT& bounds) {
    // Inputs are the dock parent and initial geometry. Processing registers the
    // class and allocates a page instance transferred to WM_NCDESTROY; output is
    // the child HWND or nullptr when registration/window creation fails.
    if (!parent || !RegisterHandlePageClass()) {
        return nullptr;
    }

    auto* page = new HandlePage();
    HWND hwnd = ::CreateWindowExW(
        0,
        kHandlePageClass,
        L"Handle",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        page);
    if (!hwnd) {
        delete page;
    }
    return hwnd;
}

LRESULT CALLBACK HandlePage::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Input is the raw Win32 message. Processing stores the page pointer during
    // WM_NCCREATE and delegates all later messages; output is an LRESULT for the
    // window manager.
    HandlePage* page = reinterpret_cast<HandlePage*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        page = create ? static_cast<HandlePage*>(create->lpCreateParams) : nullptr;
        if (page) {
            page->hwnd_ = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(page));
        }
    }
    if (page) {
        return page->HandleMessage(hwnd, message, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool HandlePage::Initialize(HWND hwnd) {
    // Input is the newly created root HWND. Processing creates all controls once
    // and retains both tab pages for the page lifetime; output is false if a
    // critical control cannot be created.
    hwnd_ = hwnd;
    state_ = new HandlePageState();

    pidEdit_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPidEditId)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    refreshButton_ = Ksword::Ui::CreateButton(hwnd_, kRefreshButtonId, L"枚举PID句柄", 0, 0, 0, 0);
    statusText_ = Ksword::Ui::CreateText(hwnd_, kStatusTextId, L"输入 PID 后刷新；页面只读展示句柄证据。", 0, 0, 0, 0);
    tab_ = Ksword::Ui::CreateTabControl(hwnd_, kTabId, 0, 0, 0, 0);
    if (!pidEdit_ || !refreshButton_ || !statusText_ || !tab_) {
        return false;
    }

    ::SendMessageW(pidEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    Ksword::Ui::AddTabPage(tab_, kHandleTabIndex, { L"句柄表 cross-view" });
    Ksword::Ui::AddTabPage(tab_, kDetailTabIndex, { L"ObjectHeader / ObjectType" });
    ::SendMessageW(tab_, TCM_SETCURSEL, static_cast<WPARAM>(kHandleTabIndex), 0);
    currentTab_ = kHandleTabIndex;

    handleList_ = Ksword::Ui::CreateReportListView(tab_, kHandleListId, 0, 0, 0, 0, 0);
    detailList_ = Ksword::Ui::CreateReportListView(tab_, kDetailListId, 0, 0, 0, 0, 0);
    if (!handleList_ || !detailList_) {
        return false;
    }
    InsertHandleColumns(handleList_);
    InsertDetailColumns(detailList_);

    AddDetailRow(detailList_, L"页面边界", L"只读审计", L"不提供关闭句柄/复制句柄/patch 操作");
    AddDetailRow(detailList_, L"输入", L"PID + 选中句柄行", L"R0 重新引用句柄，不信任对象地址作为操作凭据");
    AddDetailRow(detailList_, L"字段范围", L"ObjectHeader/ObjectType/GrantedAccess/Attributes/异常标记", L"来自现有 Handle IOCTL");

    Ksword::Ui::SetWindowFontRecursive(hwnd_);
    Layout();
    return true;
}

void HandlePage::Layout() {
    // Input is the current root client area. Processing positions the toolbar,
    // tab control and retained child pages; no value is returned.
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int width = std::max(100, Width(rc));
    const int height = std::max(100, Height(rc));
    const int margin = 8;
    const int toolbarHeight = 30;

    ::MoveWindow(pidEdit_, margin, margin, 120, 24, TRUE);
    ::MoveWindow(refreshButton_, margin + 128, margin, 112, 24, TRUE);
    ::MoveWindow(statusText_, margin + 252, margin + 3, std::max(100, width - margin - 252), 20, TRUE);

    const int tabTop = margin + toolbarHeight;
    ::MoveWindow(tab_, margin, tabTop, width - margin * 2, height - tabTop - margin, TRUE);
    RECT display = Ksword::Ui::GetTabDisplayRect(tab_);
    ::MoveWindow(handleList_, display.left, display.top, Width(display), Height(display), TRUE);
    ::MoveWindow(detailList_, display.left, display.top, Width(display), Height(display), TRUE);
    ::ShowWindow(handleList_, currentTab_ == kHandleTabIndex ? SW_SHOW : SW_HIDE);
    ::ShowWindow(detailList_, currentTab_ == kDetailTabIndex ? SW_SHOW : SW_HIDE);
}

void HandlePage::Refresh() {
    // Input is the PID text box. Processing calls the local ArkDriverClient
    // adapter, repopulates the retained list view, and keeps diagnostics visible.
    if (!state_) {
        return;
    }
    const std::uint32_t processId = ReadPidEdit(pidEdit_);
    if (processId == 0U) {
        SetStatus(L"请输入有效的十进制 PID。");
        return;
    }

    SetStatus(L"正在只读枚举 PID " + std::to_wstring(processId) + L" 的 HandleTable...");
    state_->snapshot = state_->client.EnumerateProcessHandles(processId);
    PopulateList();
    const std::wstring message = Utf8ToWide(state_->snapshot.io.message);
    SetStatus(state_->snapshot.io.ok
        ? L"句柄枚举完成：返回 " + std::to_wstring(state_->snapshot.entries.size()) + L" 行；" + message
        : L"句柄枚举失败：" + message);
}

void HandlePage::PopulateList() {
    // Input is state_->snapshot. Processing clears only rows and inserts stable
    // display values; no return value is produced.
    if (!state_ || !handleList_) {
        return;
    }

    Ksword::Ui::ScopedListViewRedrawLock lock(handleList_);
    ListView_DeleteAllItems(handleList_);
    for (std::size_t index = 0; index < state_->snapshot.entries.size(); ++index) {
        const HandleEntryView& entry = state_->snapshot.entries[index];
        Ksword::Ui::InsertListViewTextRow(handleList_, {
            std::to_wstring(entry.processId),
            Hex32(entry.handleValue),
            Hex64(entry.objectAddress),
            Hex64(entry.objectHeaderAddress),
            Hex64(entry.objectTypeAddress),
            std::to_wstring(entry.objectTypeIndex),
            Hex32(entry.grantedAccess),
            Hex32(entry.attributes),
            std::to_wstring(entry.pointerCount),
            std::to_wstring(entry.handleCount),
            DecodeStatusText(entry.decodeStatus),
            AnomalyText(entry),
        }, static_cast<LPARAM>(index));
    }
}

void HandlePage::PopulateDetail(const int rowIndex) {
    // Input is a selected row index. Processing reads the cached row, performs a
    // read-only object query by PID/handle, and renders ObjectHeader/ObjectType
    // fields. No close/duplicate/patch action is exposed.
    if (!state_ || !detailList_ || rowIndex < 0 ||
        rowIndex >= static_cast<int>(state_->snapshot.entries.size())) {
        return;
    }

    const HandleEntryView& entry = state_->snapshot.entries[static_cast<std::size_t>(rowIndex)];
    state_->detail = state_->client.QueryHandleObject(entry.processId, entry.handleValue);

    Ksword::Ui::ScopedListViewRedrawLock lock(detailList_);
    ListView_DeleteAllItems(detailList_);

    const HandleObjectDetailView& detail = state_->detail;
    AddDetailRow(detailList_, L"Transport", detail.io.ok ? L"OK" : L"FAIL", Utf8ToWide(detail.io.message));
    AddDetailRow(detailList_, L"PID", std::to_wstring(entry.processId), L"查询输入");
    AddDetailRow(detailList_, L"Handle", Hex32(entry.handleValue), L"查询输入");
    AddDetailRow(detailList_, L"QueryStatus", QueryStatusText(detail.queryStatus), std::to_wstring(detail.queryStatus));
    AddDetailRow(detailList_, L"ObjectName", detail.objectName, NameInfoStatusText(detail.nameInfoStatus));
    AddDetailRow(detailList_, L"TypeName", detail.typeName, FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_TYPE_NAME_PRESENT)));
    AddDetailRow(detailList_, L"Object", Hex64(detail.objectAddress), FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_PRESENT)));
    AddDetailRow(detailList_, L"ObjectHeader", Hex64(detail.objectHeaderAddress), FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_HEADER_PRESENT)));
    AddDetailRow(detailList_, L"ObjectType", Hex64(detail.objectTypeAddress), FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_TYPE_PRESENT)));
    AddDetailRow(detailList_, L"ObjectTypeIndex", std::to_wstring(detail.objectTypeIndex), TypeSourceText(detail.objectTypeIndexSource));
    AddDetailRow(detailList_, L"HeaderTypeIndex", std::to_wstring(detail.objectHeaderTypeIndex), FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_HEADER_TYPE_INDEX_PRESENT)));
    AddDetailRow(detailList_, L"HeaderInfoMask", Hex32(detail.objectHeaderInfoMask), FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_INFO_MASK_PRESENT)));
    AddDetailRow(detailList_, L"HeaderFlags", Hex32(detail.objectHeaderFlags), L"OBJECT_HEADER.Flags");
    AddDetailRow(detailList_, L"HeaderTraceFlags", Hex32(detail.objectHeaderTraceFlags), L"OBJECT_HEADER.TraceFlags");
    AddDetailRow(detailList_, L"PointerCount", std::to_wstring(detail.pointerCount), FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_POINTER_COUNT_PRESENT)));
    AddDetailRow(detailList_, L"HandleCount", std::to_wstring(detail.handleCount), FieldText(DetailFieldPresent(detail.fieldFlags, KSWORD_ARK_OBJECT_INFO_FIELD_HANDLE_COUNT_PRESENT)));
    AddDetailRow(detailList_, L"GrantedAccess(enum)", Hex32(entry.grantedAccess), FieldText(FieldPresent(entry.fieldFlags, KSWORD_ARK_HANDLE_FIELD_GRANTED_ACCESS_PRESENT)));
    AddDetailRow(detailList_, L"ActualGrantedAccess(query)", Hex32(detail.actualGrantedAccess), L"不请求 proxy handle");
    AddDetailRow(detailList_, L"HandleAttributes", Hex32(entry.attributes), FieldText(FieldPresent(entry.fieldFlags, KSWORD_ARK_HANDLE_FIELD_ATTRIBUTES_PRESENT)));
    AddDetailRow(detailList_, L"FieldFlags", Hex32(detail.fieldFlags), L"KSWORD_ARK_OBJECT_INFO_FIELD_*");
    AddDetailRow(detailList_, L"DynDataCapabilityMask", Hex64(detail.dynDataCapabilityMask), L"capability gated");
    AddDetailRow(detailList_, L"OtNameOffset", Hex32(detail.otNameOffset), L"_OBJECT_TYPE.Name");
    AddDetailRow(detailList_, L"OtIndexOffset", Hex32(detail.otIndexOffset), L"_OBJECT_TYPE.Index");
    AddDetailRow(detailList_, L"ObjectHeaderDecodeStatus", DecodeStatusText(detail.objectHeaderDecodeStatus), NtStatusText(detail.objectHeaderReadStatus));
    AddDetailRow(detailList_, L"GrantedAccessDecodeStatus", DecodeStatusText(detail.grantedAccessDecodeStatus), NtStatusText(detail.grantedAccessReadStatus));
    AddDetailRow(detailList_, L"ObjectReferenceStatus", NtStatusText(detail.objectReferenceStatus), L"ObReferenceObjectByHandle 路径");
    AddDetailRow(detailList_, L"TypeStatus", NtStatusText(detail.typeStatus), L"类型查询");
    AddDetailRow(detailList_, L"NameStatus", NtStatusText(detail.nameStatus), L"名称查询");
    AddDetailRow(detailList_, L"ProxyStatus", std::to_wstring(detail.proxyStatus), L"本页未请求 proxy handle");
    AddDetailRow(detailList_, L"ProxyHandleReturned", BoolText(detail.proxyHandle != 0), Hex64(detail.proxyHandle));
    AddDetailRow(detailList_, L"异常句柄标记", AnomalyText(entry), L"只读提示，不做关闭/复制/patch");

    ::SendMessageW(tab_, TCM_SETCURSEL, static_cast<WPARAM>(kDetailTabIndex), 0);
    currentTab_ = kDetailTabIndex;
    Layout();
    SetStatus(L"已读取句柄 " + Hex32(entry.handleValue) + L" 的 ObjectHeader/ObjectType 详情。");
}

void HandlePage::SetStatus(const std::wstring& text) {
    // Input is a status message. Processing updates only the STATIC text control
    // and returns no value.
    if (statusText_) {
        ::SetWindowTextW(statusText_, text.c_str());
    }
}

LRESULT HandlePage::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        if (!Initialize(hwnd)) {
            return -1;
        }
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kRefreshButtonId) {
            Refresh();
            return 0;
        }
        if (LOWORD(wParam) == kPidEditId && HIWORD(wParam) == EN_CHANGE) {
            SetStatus(L"PID 已更新，点击“枚举PID句柄”刷新。");
            return 0;
        }
        break;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header && header->hwndFrom == tab_ && header->code == TCN_SELCHANGE) {
            const LRESULT selected = ::SendMessageW(tab_, TCM_GETCURSEL, 0, 0);
            if (selected >= 0) {
                currentTab_ = static_cast<int>(selected);
            }
            Layout();
            return 0;
        }
        if (header && header->hwndFrom == handleList_ &&
            (header->code == NM_DBLCLK || header->code == LVN_ITEMCHANGED)) {
            if (header->code == NM_DBLCLK) {
                const int row = ListView_GetNextItem(handleList_, -1, LVNI_SELECTED);
                PopulateDetail(row);
                return 0;
            }
            const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lParam);
            if (changed && (changed->uNewState & LVIS_SELECTED) != 0 &&
                (changed->uOldState & LVIS_SELECTED) == 0) {
                SetStatus(L"已选择句柄；双击行查看 ObjectHeader/ObjectType 详情。");
            }
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        delete state_;
        state_ = nullptr;
        delete this;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace Ksword::Features::Handle
