#include "ProcessDetailPage.h"

#include "ProcessDetailCollector.h"

#include "../../Ui/Controls.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windowsx.h>

#include <cstring>
#include <limits>
#include <vector>
#include <sstream>
#include <utility>

namespace Ksword::Features::ProcessDetail {
namespace {

constexpr wchar_t kProcessDetailPageClass[] = L"KswordARKLight.ProcessDetailPage";
constexpr int kRefreshButtonId = 1001;
constexpr int kTabControlId = 1002;
constexpr int kToolbarHeight = 24;
constexpr int kMargin = 0;
constexpr UINT kThreadMenuCopyCell = 61001;
constexpr UINT kThreadMenuCopyRow = 61002;
constexpr UINT kThreadMenuSuspend = 61003;
constexpr UINT kThreadMenuResume = 61004;
constexpr UINT kThreadMenuTerminate = 61005;
constexpr UINT kThreadMenuStack = 61006;
constexpr UINT kThreadMenuProcessDetail = 61007;
constexpr UINT kModuleMenuCopyCell = 61101;
constexpr UINT kModuleMenuCopyRow = 61102;
constexpr UINT kModuleMenuOpenFolder = 61103;
constexpr UINT kModuleMenuGotoModule = 61104;
constexpr UINT kModuleMenuUnload = 61105;
constexpr UINT kModuleMenuSuspendThread = 61106;
constexpr UINT kModuleMenuResumeThread = 61107;
constexpr UINT kModuleMenuTerminateThread = 61108;

using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);

// Width returns a non-negative RECT width. Input is a RECT in one coordinate
// space; processing clamps negative values; output is pixels.
int Width(const RECT& rect) {
    return rect.right > rect.left ? rect.right - rect.left : 0;
}

// Height returns a non-negative RECT height. Input is a RECT in one coordinate
// space; processing clamps negative values; output is pixels.
int Height(const RECT& rect) {
    return rect.bottom > rect.top ? rect.bottom - rect.top : 0;
}

// RegisterPageClass registers the root page window. Input is none; processing is
// idempotent through RegisterClassW/ERROR_CLASS_ALREADY_EXISTS; output reports
// whether the class can be used for CreateWindowExW.
bool RegisterPageClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = ProcessDetailPage::WindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kProcessDetailPageClass;
    if (::RegisterClassW(&wc)) {
        return true;
    }
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// SetChildFont applies the project UI font to one child control. Input is HWND;
// processing sends WM_SETFONT when non-null; no value is returned.
void SetChildFont(HWND hwnd) {
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
}

// InsertListColumn appends one list-view column. Inputs are control, index,
// title and width; processing sends LVM_INSERTCOLUMNW; no value is returned.
void InsertListColumn(HWND list, int index, const wchar_t* title, int width) {
    if (!list) {
        return;
    }

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ::SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&column));
}

// ClearListRows deletes every row from a list-view. Input is the list HWND;
// processing sends LVM_DELETEALLITEMS; no value is returned.
void ClearListRows(HWND list) {
    if (list) {
        ::SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
    }
}

// InsertListText writes one row into a list-view. Inputs are row values; the
// first value creates the item and remaining values become subitems; no return.
void InsertListText(HWND list, int rowIndex, const std::vector<std::wstring>& values) {
    if (!list || values.empty()) {
        return;
    }

    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = rowIndex;
    item.iSubItem = 0;
    item.pszText = const_cast<LPWSTR>(values[0].c_str());
    ::SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

    for (int column = 1; column < static_cast<int>(values.size()); ++column) {
        LVITEMW subItem{};
        subItem.iSubItem = column;
        subItem.pszText = const_cast<LPWSTR>(values[column].c_str());
        ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(rowIndex), reinterpret_cast<LPARAM>(&subItem));
    }
}

// ListText returns one list-view cell as a string. Inputs are list, row and
// column; processing asks the control for text into a local buffer; output is
// empty for invalid handles or blank cells.
std::wstring ListText(HWND list, int row, int column) {
    if (!list || row < 0 || column < 0) {
        return {};
    }

    std::vector<wchar_t> buffer(4096, L'\0');
    LVITEMW item{};
    item.iSubItem = column;
    item.cchTextMax = static_cast<int>(buffer.size());
    item.pszText = buffer.data();
    ::SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
    return std::wstring(buffer.data());
}

// WriteClipboardText copies Unicode text to the clipboard. Inputs are owner and
// text; processing transfers a GMEM_MOVEABLE block to CF_UNICODETEXT; output
// reports whether the clipboard accepted the value.
bool WriteClipboardText(HWND owner, const std::wstring& text) {
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

// SelectedThreadId extracts the selected TID from the Threads tab. Input is the
// thread list HWND; processing reads column zero and parses decimal text; output
// is zero when no valid TID is selected.
DWORD SelectedThreadId(HWND list) {
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) {
        return 0;
    }
    return static_cast<DWORD>(std::wcstoul(ListText(list, row, 0).c_str(), nullptr, 10));
}

// DecimalText converts an integer to display text. Input is any unsigned value
// accepted by std::to_wstring; output is a decimal string.
template <typename T>
std::wstring DecimalText(T value) {
    return std::to_wstring(static_cast<unsigned long long>(value));
}

// SignedDecimalText converts a signed integer to display text. Input is a
// signed priority or delta value; processing preserves the sign; output is a
// decimal string.
template <typename T>
std::wstring SignedDecimalText(T value) {
    return std::to_wstring(static_cast<long long>(value));
}

// HexAddressText formats a module base address. Input is an integer address;
// processing emits uppercase hexadecimal; output is UI-ready text.
std::wstring HexAddressText(std::uintptr_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase;
    stream.width(sizeof(void*) == 8 ? 16 : 8);
    stream.fill(L'0');
    stream << value;
    return stream.str();
}

// ParseHexAddress converts a ListView address cell back to an integer. Input is
// text such as "0x00007FF..." or plain hex; processing rejects malformed
// characters and overflows; output reports success and writes value.
bool ParseHexAddress(const std::wstring& text, std::uintptr_t& value) {
    value = 0;
    std::size_t offset = 0;
    while (offset < text.size() && iswspace(text[offset])) {
        ++offset;
    }
    if (offset + 2 <= text.size() && text[offset] == L'0' && (text[offset + 1] == L'x' || text[offset + 1] == L'X')) {
        offset += 2;
    }
    if (offset >= text.size()) {
        return false;
    }

    std::uintptr_t parsed = 0;
    bool sawDigit = false;
    for (; offset < text.size(); ++offset) {
        const wchar_t ch = text[offset];
        if (iswspace(ch)) {
            break;
        }
        unsigned digit = 0;
        if (ch >= L'0' && ch <= L'9') {
            digit = static_cast<unsigned>(ch - L'0');
        } else if (ch >= L'a' && ch <= L'f') {
            digit = static_cast<unsigned>(10 + ch - L'a');
        } else if (ch >= L'A' && ch <= L'F') {
            digit = static_cast<unsigned>(10 + ch - L'A');
        } else {
            return false;
        }
        if (parsed > ((std::numeric_limits<std::uintptr_t>::max)() >> 4)) {
            return false;
        }
        parsed = (parsed << 4) | static_cast<std::uintptr_t>(digit);
        sawDigit = true;
    }
    value = parsed;
    return sawDigit;
}

// LeafNameLower returns a lowercase DLL filename for module matching. Input is
// a full module path or filename; processing strips directory components and
// lowercases ASCII/Unicode through towlower; output is used for kernel32 lookup.
std::wstring LeafNameLower(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    for (wchar_t& ch : leaf) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return leaf;
}

// RemoteModuleBase returns a module base inside the target process. Inputs are
// PID and a lowercase module leaf name; processing uses Toolhelp module
// snapshots for both native and WOW64 module lists; output is zero on failure.
std::uintptr_t RemoteModuleBase(DWORD processId, const std::wstring& lowercaseLeafName) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::uintptr_t base = 0;
    if (::Module32FirstW(snapshot, &entry)) {
        do {
            if (LeafNameLower(entry.szModule) == lowercaseLeafName || LeafNameLower(entry.szExePath) == lowercaseLeafName) {
                base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (::Module32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return base;
}

// ResolveRemoteFreeLibrary computes kernel32!FreeLibrary in the target process.
// Inputs are PID; processing computes the local RVA of FreeLibrary from local
// kernel32 and adds it to the remote kernel32 base; output is zero on failure.
std::uintptr_t ResolveRemoteFreeLibrary(DWORD processId) {
    HMODULE localKernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!localKernel32) {
        return 0;
    }
    FARPROC localFreeLibrary = ::GetProcAddress(localKernel32, "FreeLibrary");
    if (!localFreeLibrary) {
        return 0;
    }
    const std::uintptr_t localBase = reinterpret_cast<std::uintptr_t>(localKernel32);
    const std::uintptr_t localAddress = reinterpret_cast<std::uintptr_t>(localFreeLibrary);
    if (localAddress < localBase) {
        return 0;
    }
    const std::uintptr_t remoteKernel32 = RemoteModuleBase(processId, L"kernel32.dll");
    if (remoteKernel32 == 0) {
        return 0;
    }
    return remoteKernel32 + (localAddress - localBase);
}

// IsCurrentProcess64Bit returns the bitness of this executable. There is no
// runtime input; output is true for the intended x64 build and false for any
// future 32-bit build.
bool IsCurrentProcess64Bit() {
#if defined(_WIN64)
    return true;
#else
    return false;
#endif
}

// QueryProcess64Bit determines the target process bitness before using a local
// kernel32 RVA as a remote function address. Input is an open process handle;
// output writes is64Bit and returns false when Windows cannot provide the data.
bool QueryProcess64Bit(HANDLE process, bool& is64Bit) {
    is64Bit = false;
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const auto isWow64Process2 = kernel32
        ? reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(kernel32, "IsWow64Process2"))
        : nullptr;
    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine)) {
            return false;
        }
        is64Bit = processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
            (nativeMachine == IMAGE_FILE_MACHINE_AMD64 || nativeMachine == IMAGE_FILE_MACHINE_ARM64);
        return true;
    }

    BOOL wow64 = FALSE;
    if (!::IsWow64Process(process, &wow64)) {
        return false;
    }
    is64Bit = IsCurrentProcess64Bit() && !wow64;
    return true;
}

// SetTabItem inserts one tab caption. Inputs are the tab control, index and
// title; processing sends TCM_INSERTITEMW; no value is returned.
void SetTabItem(HWND tab, int index, const wchar_t* title) {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(title);
    ::SendMessageW(tab, TCM_INSERTITEMW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
}

// CreateListView creates a report-mode child list-view. Inputs are parent and
// bounds; processing enables full-row selection/grid lines; output is HWND.
HWND CreateListView(HWND parent, const RECT& bounds) {
    HWND list = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        bounds.left,
        bounds.top,
        Width(bounds),
        Height(bounds),
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (list) {
        ::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        SetChildFont(list);
    }
    return list;
}

} // namespace

ProcessDetailPage::ProcessDetailPage(DWORD processId) : processId_(processId) {}

HWND ProcessDetailPage::Create(HWND parent, DWORD processId, const RECT& bounds) {
    if (!RegisterPageClass()) {
        return nullptr;
    }

    auto* page = new ProcessDetailPage(processId);
    HWND hwnd = ::CreateWindowExW(
        0,
        kProcessDetailPageClass,
        L"Process Detail",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        Width(bounds),
        Height(bounds),
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        page);
    if (!hwnd) {
        delete page;
        return nullptr;
    }
    return hwnd;
}

LRESULT CALLBACK ProcessDetailPage::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ProcessDetailPage* page = reinterpret_cast<ProcessDetailPage*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        page = reinterpret_cast<ProcessDetailPage*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(page));
        if (page) {
            page->hwnd_ = hwnd;
        }
    }

    if (!page) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return page->HandleMessage(hwnd, message, wParam, lParam);
}

LRESULT ProcessDetailPage::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        Initialize(hwnd);
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kRefreshButtonId) {
            Refresh();
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (const auto* header = reinterpret_cast<const NMHDR*>(lParam)) {
            if (header->idFrom == kTabControlId && header->code == TCN_SELCHANGE) {
                UpdateVisiblePage();
                return 0;
            }
            if (header->code == NM_RCLICK &&
                (header->hwndFrom == threadsList_ || header->hwndFrom == modulesList_)) {
                POINT point{};
                ::GetCursorPos(&point);
                if (HandleListContextMenu(header->hwndFrom, point)) {
                    return 0;
                }
            }
        }
        break;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == threadsList_ || reinterpret_cast<HWND>(wParam) == modulesList_) {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (point.x == -1 && point.y == -1) {
                RECT rc{};
                ::GetWindowRect(reinterpret_cast<HWND>(wParam), &rc);
                point.x = rc.left + 24;
                point.y = rc.top + 24;
            }
            if (HandleListContextMenu(reinterpret_cast<HWND>(wParam), point)) {
                return 0;
            }
        }
        break;
    case WM_CTLCOLORSTATIC:
        ::SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    case WM_NCDESTROY:
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete this;
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ProcessDetailPage::Initialize(HWND hwnd) {
    hwnd_ = hwnd;
    refreshButton_ = Ksword::Ui::CreateButton(hwnd_, kRefreshButtonId, L"刷新", kMargin, kMargin, 76, 24);
    statusText_ = Ksword::Ui::CreateText(hwnd_, 0, L"Ready", 96, kMargin + 4, 640, 20);

    tab_ = ::CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        kMargin,
        kToolbarHeight,
        400,
        260,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabControlId)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    SetChildFont(tab_);
    SetTabItem(tab_, 0, L"基础");
    SetTabItem(tab_, 1, L"线程");
    SetTabItem(tab_, 2, L"模块");

    RECT listBounds{ 0, 0, 100, 100 };
    basicList_ = CreateListView(tab_, listBounds);
    threadsList_ = CreateListView(tab_, listBounds);
    modulesList_ = CreateListView(tab_, listBounds);

    InsertListColumn(basicList_, 0, L"字段", 160);
    InsertListColumn(basicList_, 1, L"值", 720);

    InsertListColumn(threadsList_, 0, L"TID", 110);
    InsertListColumn(threadsList_, 1, L"Owner PID", 110);
    InsertListColumn(threadsList_, 2, L"Start Address", 150);
    InsertListColumn(threadsList_, 3, L"Base Priority", 120);
    InsertListColumn(threadsList_, 4, L"Delta Priority", 120);
    InsertListColumn(threadsList_, 5, L"Status", 420);

    InsertListColumn(modulesList_, 0, L"Module", 220);
    InsertListColumn(modulesList_, 1, L"Base", 150);
    InsertListColumn(modulesList_, 2, L"Size", 110);
    InsertListColumn(modulesList_, 3, L"Path", 520);
    InsertListColumn(modulesList_, 4, L"ThreadID", 110);
    InsertListColumn(modulesList_, 5, L"Status", 300);

    Layout();
    Refresh();
    return tab_ && basicList_ && threadsList_ && modulesList_;
}

void ProcessDetailPage::Refresh() {
    ProcessDetailCollector collector;
    snapshot_ = collector.Collect(processId_);
    PopulateBasic();
    PopulateThreads();
    PopulateModules();
    UpdateVisiblePage();

    const std::wstring status = L"PID " + DecimalText(processId_) +
        L" | Threads " + DecimalText(snapshot_.threads.size()) +
        L" | Modules " + DecimalText(snapshot_.modules.size()) +
        L" | " + snapshot_.errorText;
    if (statusText_) {
        ::SetWindowTextW(statusText_, status.c_str());
    }
}

void ProcessDetailPage::Layout() {
    if (!hwnd_) {
        return;
    }

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int width = Width(client);
    const int height = Height(client);

    if (refreshButton_) {
        ::MoveWindow(refreshButton_, kMargin, kMargin, 76, 24, TRUE);
    }
    if (statusText_) {
        const int statusWidth = width > 104 ? width - 104 : 0;
        ::MoveWindow(statusText_, 96, kMargin + 4, statusWidth, 20, TRUE);
    }
    if (tab_) {
        const int tabWidth = width > kMargin * 2 ? width - kMargin * 2 : 0;
        const int tabHeight = height > kToolbarHeight + kMargin ? height - kToolbarHeight - kMargin : 0;
        ::MoveWindow(tab_, kMargin, kToolbarHeight, tabWidth, tabHeight, TRUE);
    }

    RECT tabClient{};
    if (tab_) {
        ::GetClientRect(tab_, &tabClient);
        ::SendMessageW(tab_, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&tabClient));
        ::InflateRect(&tabClient, -4, -4);
    }

    const int childWidth = Width(tabClient);
    const int childHeight = Height(tabClient);
    const HWND children[] = { basicList_, threadsList_, modulesList_ };
    for (HWND child : children) {
        if (child) {
            ::MoveWindow(child, tabClient.left, tabClient.top, childWidth, childHeight, TRUE);
        }
    }
}

void ProcessDetailPage::UpdateVisiblePage() {
    const int selected = tab_ ? static_cast<int>(::SendMessageW(tab_, TCM_GETCURSEL, 0, 0)) : 0;
    ::ShowWindow(basicList_, selected == 0 ? SW_SHOW : SW_HIDE);
    ::ShowWindow(threadsList_, selected == 1 ? SW_SHOW : SW_HIDE);
    ::ShowWindow(modulesList_, selected == 2 ? SW_SHOW : SW_HIDE);
}

void ProcessDetailPage::PopulateBasic() {
    ClearListRows(basicList_);
    const ProcessBasicInfo& basic = snapshot_.basic;
    const std::vector<std::pair<std::wstring, std::wstring>> rows{
        { L"PID", DecimalText(basic.processId) },
        { L"PPID", DecimalText(basic.parentProcessId) },
        { L"映像路径", basic.imagePath },
        { L"命令行", basic.commandLine },
        { L"位数", basic.bitness },
        { L"会话", DecimalText(basic.sessionId) },
        { L"用户", basic.userName },
        { L"完整性", basic.integrityLevel },
        { L"状态", basic.statusText }
    };

    int row = 0;
    for (const auto& item : rows) {
        InsertListText(basicList_, row++, { item.first, item.second });
    }
}

void ProcessDetailPage::PopulateThreads() {
    ClearListRows(threadsList_);
    int row = 0;
    for (const ProcessThreadInfo& thread : snapshot_.threads) {
        InsertListText(threadsList_, row++, {
            DecimalText(thread.threadId),
            DecimalText(thread.ownerProcessId),
            HexAddressText(thread.startAddress),
            SignedDecimalText(thread.basePriority),
            SignedDecimalText(thread.deltaPriority),
            thread.statusText
        });
    }
}

void ProcessDetailPage::PopulateModules() {
    ClearListRows(modulesList_);
    int row = 0;
    for (const ProcessModuleInfo& module : snapshot_.modules) {
        InsertListText(modulesList_, row++, {
            module.moduleName,
            HexAddressText(module.baseAddress),
            DecimalText(module.imageSize),
            module.modulePath,
            module.representativeThreadId == 0 ? L"" : DecimalText(module.representativeThreadId),
            module.statusText
        });
    }
}

bool ProcessDetailPage::HandleListContextMenu(HWND list, POINT screenPoint) {
    if (list != threadsList_ && list != modulesList_) {
        return false;
    }

    POINT clientPoint = screenPoint;
    ::ScreenToClient(list, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int row = ListView_HitTest(list, &hit);
    const int column = hit.iSubItem >= 0 ? hit.iSubItem : 0;
    if (list == threadsList_) {
        lastThreadContextColumn_ = column;
    } else if (list == modulesList_) {
        lastModuleContextColumn_ = column;
    }
    if (row >= 0 && (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) == 0) {
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    if (list == threadsList_) {
        ShowThreadContextMenu(screenPoint);
    } else {
        ShowModuleContextMenu(screenPoint);
    }
    return true;
}

void ProcessDetailPage::ShowThreadContextMenu(POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }

    const bool hasThread = SelectedThreadId(threadsList_) != 0;
    const UINT enabled = hasThread ? 0U : MF_GRAYED;
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | enabled, kThreadMenuCopyCell, L"复制当前单元格");
        ::AppendMenuW(copyMenu, MF_STRING | enabled, kThreadMenuCopyRow, L"复制整行");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }
    HMENU detailMenu = ::CreatePopupMenu();
    if (detailMenu) {
        ::AppendMenuW(detailMenu, MF_STRING | enabled, kThreadMenuProcessDetail, L"进程详细信息");
        ::AppendMenuW(detailMenu, MF_STRING | enabled, kThreadMenuStack, L"线程详细信息");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(detailMenu), L"详细信息");
    }
    HMENU threadMenu = ::CreatePopupMenu();
    if (threadMenu) {
        ::AppendMenuW(threadMenu, MF_STRING | enabled, kThreadMenuSuspend, L"挂起线程");
        ::AppendMenuW(threadMenu, MF_STRING | enabled, kThreadMenuResume, L"恢复线程");
        ::AppendMenuW(threadMenu, MF_STRING | enabled, kThreadMenuTerminate, L"终止线程");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(threadMenu), L"线程操作");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        hwnd_,
        nullptr);
    ::DestroyMenu(menu);

    switch (command) {
    case kThreadMenuCopyCell:
        CopySelectedListCell(threadsList_);
        break;
    case kThreadMenuCopyRow:
        CopySelectedListRow(threadsList_, 6);
        break;
    case kThreadMenuProcessDetail:
        SetStatusLine(L"当前已位于 PID " + DecimalText(processId_) + L" 的详细信息页。");
        break;
    case kThreadMenuStack:
        ShowSelectedThreadSummary();
        break;
    case kThreadMenuSuspend:
        SuspendSelectedThread();
        break;
    case kThreadMenuResume:
        ResumeSelectedThread();
        break;
    case kThreadMenuTerminate:
        TerminateSelectedThread();
        break;
    default:
        break;
    }
}

void ProcessDetailPage::ShowModuleContextMenu(POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }

    const bool hasModule = SelectedListIndex(modulesList_) >= 0;
    const UINT enabled = hasModule ? 0U : MF_GRAYED;
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | enabled, kModuleMenuCopyCell, L"复制当前单元格");
        ::AppendMenuW(copyMenu, MF_STRING | enabled, kModuleMenuCopyRow, L"复制整行");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }
    HMENU moduleMenu = ::CreatePopupMenu();
    if (moduleMenu) {
        ::AppendMenuW(moduleMenu, MF_STRING | enabled, kModuleMenuOpenFolder, L"打开所在目录");
        ::AppendMenuW(moduleMenu, MF_STRING | enabled, kModuleMenuGotoModule, L"复制模块路径/定位信息");
        ::AppendMenuW(moduleMenu, MF_STRING | enabled, kModuleMenuUnload, L"卸载模块(FreeLibrary)");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moduleMenu), L"模块操作");
    }
    HMENU threadMenu = ::CreatePopupMenu();
    if (threadMenu) {
        ::AppendMenuW(threadMenu, MF_STRING | enabled, kModuleMenuSuspendThread, L"挂起代表 Thread");
        ::AppendMenuW(threadMenu, MF_STRING | enabled, kModuleMenuResumeThread, L"取消挂起代表 Thread");
        ::AppendMenuW(threadMenu, MF_STRING | enabled, kModuleMenuTerminateThread, L"结束代表 Thread");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(threadMenu), L"线程操作");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        hwnd_,
        nullptr);
    ::DestroyMenu(menu);

    switch (command) {
    case kModuleMenuCopyCell:
        CopySelectedListCell(modulesList_);
        break;
    case kModuleMenuCopyRow:
        CopySelectedListRow(modulesList_, 6);
        break;
    case kModuleMenuOpenFolder:
        OpenSelectedModuleFolder();
        break;
    case kModuleMenuGotoModule:
        FocusSelectedModule();
        break;
    case kModuleMenuUnload:
        UnloadSelectedModule();
        break;
    case kModuleMenuSuspendThread:
        SuspendSelectedModuleThread();
        break;
    case kModuleMenuResumeThread:
        ResumeSelectedModuleThread();
        break;
    case kModuleMenuTerminateThread:
        TerminateSelectedModuleThread();
        break;
    default:
        break;
    }
}

int ProcessDetailPage::SelectedListIndex(HWND list) const {
    return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
}

int ProcessDetailPage::SelectedListColumn(HWND list) const {
    // SelectedListColumn chooses the subitem recorded during the last
    // NM_RCLICK/WM_CONTEXTMENU hit test. Inputs are the source list-view; output
    // is a non-negative column index so keyboard-invoked menus still copy the
    // first column.
    if (list == threadsList_) {
        return lastThreadContextColumn_ > 0 ? lastThreadContextColumn_ : 0;
    }
    if (list == modulesList_) {
        return lastModuleContextColumn_ > 0 ? lastModuleContextColumn_ : 0;
    }
    return 0;
}

void ProcessDetailPage::CopySelectedListCell(HWND list) {
    const int row = SelectedListIndex(list);
    if (row < 0) {
        SetStatusLine(L"没有选中行可复制。");
        return;
    }

    const int column = SelectedListColumn(list);
    const bool copied = WriteClipboardText(hwnd_, ListText(list, row, column));
    SetStatusLine(copied ? L"已复制单元格。" : L"复制单元格失败。");
}

void ProcessDetailPage::CopySelectedListRow(HWND list, int columnCount) {
    const int row = SelectedListIndex(list);
    if (row < 0) {
        SetStatusLine(L"没有选中行可复制。");
        return;
    }

    std::wstring text;
    for (int column = 0; column < columnCount; ++column) {
        if (column != 0) {
            text += L'\t';
        }
        text += ListText(list, row, column);
    }
    const bool copied = WriteClipboardText(hwnd_, text);
    SetStatusLine(copied ? L"已复制行。" : L"复制行失败。");
}

void ProcessDetailPage::SuspendSelectedThread() {
    const DWORD tid = SelectedThreadId(threadsList_);
    if (tid == 0) {
        SetStatusLine(L"没有选中线程。");
        return;
    }

    HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!thread) {
        SetStatusLine(L"OpenThread 失败: " + DecimalText(::GetLastError()));
        return;
    }
    const DWORD result = ::SuspendThread(thread);
    const DWORD error = result == static_cast<DWORD>(-1) ? ::GetLastError() : ERROR_SUCCESS;
    ::CloseHandle(thread);
    SetStatusLine(error == ERROR_SUCCESS ? L"已挂起线程 " + DecimalText(tid) : L"SuspendThread 失败: " + DecimalText(error));
    Refresh();
}

void ProcessDetailPage::ResumeSelectedThread() {
    const DWORD tid = SelectedThreadId(threadsList_);
    if (tid == 0) {
        SetStatusLine(L"没有选中线程。");
        return;
    }

    HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!thread) {
        SetStatusLine(L"OpenThread 失败: " + DecimalText(::GetLastError()));
        return;
    }
    const DWORD result = ::ResumeThread(thread);
    const DWORD error = result == static_cast<DWORD>(-1) ? ::GetLastError() : ERROR_SUCCESS;
    ::CloseHandle(thread);
    SetStatusLine(error == ERROR_SUCCESS ? L"已恢复线程 " + DecimalText(tid) : L"ResumeThread 失败: " + DecimalText(error));
    Refresh();
}

void ProcessDetailPage::TerminateSelectedThread() {
    const DWORD tid = SelectedThreadId(threadsList_);
    if (tid == 0) {
        SetStatusLine(L"没有选中线程。");
        return;
    }

    const int answer = ::MessageBoxW(hwnd_, L"确定要终止选中线程吗？这可能导致目标进程崩溃。", L"终止线程", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        return;
    }

    HANDLE thread = ::OpenThread(THREAD_TERMINATE, FALSE, tid);
    if (!thread) {
        SetStatusLine(L"OpenThread 失败: " + DecimalText(::GetLastError()));
        return;
    }
    const BOOL ok = ::TerminateThread(thread, 1);
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(thread);
    SetStatusLine(ok ? L"已请求终止线程 " + DecimalText(tid) : L"TerminateThread 失败: " + DecimalText(error));
    Refresh();
}

void ProcessDetailPage::ShowSelectedThreadSummary() {
    // ShowSelectedThreadSummary gives the retained thread-detail menu a real
    // Win32 behavior without injecting stack walkers. Inputs are the selected
    // thread row; processing formats TID, owner PID, start address, priority and
    // state from the collector snapshot; no value is returned.
    const int row = SelectedListIndex(threadsList_);
    if (row < 0) {
        SetStatusLine(L"没有选中线程。");
        return;
    }
    std::wostringstream detail;
    detail << L"线程详细信息\r\n\r\n"
           << L"TID: " << ListText(threadsList_, row, 0) << L"\r\n"
           << L"Owner PID: " << ListText(threadsList_, row, 1) << L"\r\n"
           << L"StartAddress: " << ListText(threadsList_, row, 2) << L"\r\n"
           << L"BasePriority: " << ListText(threadsList_, row, 3) << L"\r\n"
           << L"DeltaPriority: " << ListText(threadsList_, row, 4) << L"\r\n"
           << L"状态: " << ListText(threadsList_, row, 5) << L"\r\n\r\n"
           << L"轻量版不执行远程栈展开；当前页保留基础、线程和模块三页。";
    ::MessageBoxW(hwnd_, detail.str().c_str(), L"线程详细信息", MB_OK | MB_ICONINFORMATION);
    SetStatusLine(L"已显示线程 " + ListText(threadsList_, row, 0) + L" 的详细信息。");
}

void ProcessDetailPage::OpenSelectedModuleFolder() {
    const int row = SelectedListIndex(modulesList_);
    if (row < 0) {
        SetStatusLine(L"没有选中模块。");
        return;
    }

    const std::wstring path = ListText(modulesList_, row, 3);
    if (path.empty()) {
        SetStatusLine(L"模块路径为空。");
        return;
    }

    const std::wstring args = L"/select,\"" + path + L"\"";
    const HINSTANCE result = ::ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    SetStatusLine(reinterpret_cast<INT_PTR>(result) > 32 ? L"已打开模块所在目录。" : L"打开模块所在目录失败。");
}

void ProcessDetailPage::FocusSelectedModule() {
    // FocusSelectedModule provides the original "go to module" action as a
    // light-build handoff: copy exact module path and show module identity.
    // Inputs are selected module row; output is clipboard/status feedback.
    const int row = SelectedListIndex(modulesList_);
    if (row < 0) {
        SetStatusLine(L"没有选中模块。");
        return;
    }
    const std::wstring moduleName = ListText(modulesList_, row, 0);
    const std::wstring base = ListText(modulesList_, row, 1);
    const std::wstring path = ListText(modulesList_, row, 3);
    const bool copied = WriteClipboardText(hwnd_, path);
    std::wostringstream detail;
    detail << L"模块定位信息\r\n\r\n"
           << L"模块: " << moduleName << L"\r\n"
           << L"基址: " << base << L"\r\n"
           << L"路径: " << path << L"\r\n\r\n"
           << (copied ? L"模块路径已复制到剪贴板。" : L"模块路径复制失败。");
    ::MessageBoxW(hwnd_, detail.str().c_str(), L"模块定位", MB_OK | MB_ICONINFORMATION);
    SetStatusLine(copied ? L"已复制模块路径: " + moduleName : L"模块路径复制失败。");
}

void ProcessDetailPage::UnloadSelectedModule() {
    // UnloadSelectedModule implements the retained module right-click unload
    // action with explicit confirmation. Inputs are the selected module row and
    // processId_; processing resolves kernel32!FreeLibrary in the remote process
    // and creates a short-lived remote thread; no value is returned.
    const int row = SelectedListIndex(modulesList_);
    if (row < 0) {
        SetStatusLine(L"没有选中模块。");
        return;
    }
    const std::wstring moduleName = ListText(modulesList_, row, 0);
    const std::wstring baseText = ListText(modulesList_, row, 1);
    const std::wstring path = ListText(modulesList_, row, 3);
    std::uintptr_t moduleBase = 0;
    if (!ParseHexAddress(baseText, moduleBase) || moduleBase == 0) {
        SetStatusLine(L"模块基址无效，无法卸载: " + moduleName);
        return;
    }

    std::wostringstream prompt;
    prompt << L"确定要在 PID " << processId_ << L" 中调用 FreeLibrary 卸载该模块吗？\r\n\r\n"
           << L"模块: " << moduleName << L"\r\n"
           << L"基址: " << baseText << L"\r\n"
           << L"路径: " << path << L"\r\n\r\n"
           << L"该操作可能导致目标进程崩溃。";
    if (::MessageBoxW(hwnd_, prompt.str().c_str(), L"确认卸载模块", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        SetStatusLine(L"已取消卸载模块: " + moduleName);
        return;
    }

    HANDLE process = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION, FALSE, processId_);
    if (!process) {
        SetStatusLine(L"OpenProcess 失败: " + DecimalText(::GetLastError()));
        return;
    }

    bool targetIs64Bit = false;
    if (!QueryProcess64Bit(process, targetIs64Bit)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(process);
        SetStatusLine(L"无法确认目标进程位数，已取消卸载。错误: " + DecimalText(error));
        return;
    }
    if (targetIs64Bit != IsCurrentProcess64Bit()) {
        ::CloseHandle(process);
        SetStatusLine(L"目标进程位数与 KswordARKLight 不一致，拒绝跨位数 FreeLibrary。");
        return;
    }

    const std::uintptr_t remoteFreeLibrary = ResolveRemoteFreeLibrary(processId_);
    if (remoteFreeLibrary == 0) {
        ::CloseHandle(process);
        SetStatusLine(L"无法解析远程 kernel32!FreeLibrary。");
        return;
    }

    HANDLE thread = ::CreateRemoteThread(process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteFreeLibrary),
        reinterpret_cast<LPVOID>(moduleBase),
        0,
        nullptr);
    if (!thread) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(process);
        SetStatusLine(L"CreateRemoteThread(FreeLibrary) 失败: " + DecimalText(error));
        return;
    }

    const DWORD wait = ::WaitForSingleObject(thread, 5000);
    DWORD exitCode = 0;
    ::GetExitCodeThread(thread, &exitCode);
    ::CloseHandle(thread);
    ::CloseHandle(process);

    std::wostringstream status;
    status << L"FreeLibrary 请求已发送: " << moduleName
           << L" | wait=" << wait
           << L" | exit=" << exitCode;
    SetStatusLine(status.str());
    Refresh();
}

void ProcessDetailPage::SuspendSelectedModuleThread() {
    // SuspendSelectedModuleThread mirrors the original module context menu's
    // "挂起Thread" action. Input is the selected module row; processing reads
    // its representative ThreadID column and calls SuspendThread; no value is
    // returned.
    const int row = SelectedListIndex(modulesList_);
    if (row < 0) {
        SetStatusLine(L"没有选中模块。");
        return;
    }
    const DWORD tid = static_cast<DWORD>(std::wcstoul(ListText(modulesList_, row, 4).c_str(), nullptr, 10));
    if (tid == 0) {
        SetStatusLine(L"当前模块行没有可用 ThreadID。");
        return;
    }

    HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!thread) {
        SetStatusLine(L"OpenThread 失败: " + DecimalText(::GetLastError()));
        return;
    }
    const DWORD result = ::SuspendThread(thread);
    const DWORD error = result == static_cast<DWORD>(-1) ? ::GetLastError() : ERROR_SUCCESS;
    ::CloseHandle(thread);
    SetStatusLine(error == ERROR_SUCCESS ? L"已挂起 Thread " + DecimalText(tid) : L"SuspendThread 失败: " + DecimalText(error));
    Refresh();
}

void ProcessDetailPage::ResumeSelectedModuleThread() {
    // ResumeSelectedModuleThread mirrors the original module context menu's
    // "取消挂起Thread" action. Input is the selected module row; processing
    // reads its representative ThreadID column and calls ResumeThread; no value
    // is returned.
    const int row = SelectedListIndex(modulesList_);
    if (row < 0) {
        SetStatusLine(L"没有选中模块。");
        return;
    }
    const DWORD tid = static_cast<DWORD>(std::wcstoul(ListText(modulesList_, row, 4).c_str(), nullptr, 10));
    if (tid == 0) {
        SetStatusLine(L"当前模块行没有可用 ThreadID。");
        return;
    }

    HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!thread) {
        SetStatusLine(L"OpenThread 失败: " + DecimalText(::GetLastError()));
        return;
    }
    const DWORD result = ::ResumeThread(thread);
    const DWORD error = result == static_cast<DWORD>(-1) ? ::GetLastError() : ERROR_SUCCESS;
    ::CloseHandle(thread);
    SetStatusLine(error == ERROR_SUCCESS ? L"已恢复 Thread " + DecimalText(tid) : L"ResumeThread 失败: " + DecimalText(error));
    Refresh();
}

void ProcessDetailPage::TerminateSelectedModuleThread() {
    // TerminateSelectedModuleThread mirrors the original module context menu's
    // "结束Thread" action. Input is the selected module row; processing prompts
    // once before calling TerminateThread; no value is returned.
    const int row = SelectedListIndex(modulesList_);
    if (row < 0) {
        SetStatusLine(L"没有选中模块。");
        return;
    }
    const DWORD tid = static_cast<DWORD>(std::wcstoul(ListText(modulesList_, row, 4).c_str(), nullptr, 10));
    if (tid == 0) {
        SetStatusLine(L"当前模块行没有可用 ThreadID。");
        return;
    }

    const int answer = ::MessageBoxW(hwnd_, L"确定要终止该模块关联的代表线程吗？这可能导致目标进程崩溃。", L"结束 Thread", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        return;
    }

    HANDLE thread = ::OpenThread(THREAD_TERMINATE, FALSE, tid);
    if (!thread) {
        SetStatusLine(L"OpenThread 失败: " + DecimalText(::GetLastError()));
        return;
    }
    const BOOL ok = ::TerminateThread(thread, 1);
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(thread);
    SetStatusLine(ok ? L"已请求结束 Thread " + DecimalText(tid) : L"TerminateThread 失败: " + DecimalText(error));
    Refresh();
}

void ProcessDetailPage::SetStatusLine(const std::wstring& text) {
    if (statusText_) {
        ::SetWindowTextW(statusText_, text.c_str());
    }
}

} // namespace Ksword::Features::ProcessDetail
