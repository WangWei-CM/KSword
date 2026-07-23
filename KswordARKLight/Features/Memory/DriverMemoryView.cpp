#include "DriverMemoryView.h"

#include "DriverMemoryClient.h"
#include "DriverMemoryModel.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <commctrl.h>
#include <psapi.h>
#include <windowsx.h>

namespace Ksword::Features::Memory {
namespace {

constexpr wchar_t kDriverMemoryViewClass[] = L"KswordARKLight.DriverMemoryView";
constexpr int kPidEditId = 51001;
constexpr int kAddressEditId = 51002;
constexpr int kLengthEditId = 51003;
constexpr int kReadButtonId = 51004;
constexpr int kWriteButtonId = 51005;
constexpr int kHexEditId = 51006;
constexpr int kStatusEditId = 51007;
constexpr UINT kMemoryMenuRead = 51501;
constexpr UINT kMemoryMenuWrite = 51502;
constexpr UINT kMemoryMenuCopyHex = 51503;
constexpr UINT kMemoryMenuPasteHex = 51504;
constexpr UINT kMemoryMenuClearHex = 51505;
constexpr UINT kMemoryMenuNormalizeHex = 51506;
constexpr UINT kMemoryMenuSelectAll = 51507;
constexpr UINT kMemoryMenuCopyStatus = 51508;
constexpr UINT kMsgMemoryOperationCompleted = WM_APP + 598;

struct MemoryOperationSnapshot {
    bool readOperation = false;
    DriverMemoryReadResult readResult;
    DriverMemoryWriteResult writeResult;
};

// DriverMemoryViewState owns child HWNDs and the driver facade for one page.
// Inputs arrive through window messages; processing validates edit-control text
// and calls DriverMemoryClient; return values are produced by WndProc message
// handling rather than by this state object.
struct DriverMemoryViewState {
    HWND hwnd = nullptr;
    HWND pidEdit = nullptr;
    HWND addressEdit = nullptr;
    HWND lengthEdit = nullptr;
    HWND hexEdit = nullptr;
    HWND statusEdit = nullptr;
    HWND readButton = nullptr;
    HWND writeButton = nullptr;
    bool operationInProgress = false;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<MemoryOperationSnapshot>> operationTask;
};

// GetWindowTextString copies text from a Win32 edit control. Input is the child
// HWND; processing queries length and copies the text; output is an empty string
// for null handles or controls without text.
std::wstring GetWindowTextString(HWND hwnd) {
    if (!hwnd) {
        return std::wstring();
    }
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    ::GetWindowTextW(hwnd, &text[0], length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

// SetStatus writes a human-readable status message. Inputs are page state and
// text; processing updates the multiline status edit; no value is returned.
void SetStatus(DriverMemoryViewState& state, const std::wstring& text) {
    if (state.statusEdit) {
        ::SetWindowTextW(state.statusEdit, text.c_str());
    }
}

// CopyTextToClipboard writes Unicode text from the memory page to the clipboard.
// Inputs are owner HWND and text; processing allocates CF_UNICODETEXT and hands
// it to Windows; output reports whether clipboard ownership succeeded.
bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!::OpenClipboard(owner)) {
        return false;
    }
    ::EmptyClipboard();
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
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

// TextFromClipboard reads CF_UNICODETEXT for paste into the hex buffer. Input is
// an owner HWND; processing copies the global clipboard text before unlocking;
// output is empty when the clipboard does not contain Unicode text.
std::wstring TextFromClipboard(HWND owner) {
    if (!::OpenClipboard(owner)) {
        return {};
    }
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        ::CloseClipboard();
        return {};
    }
    const wchar_t* text = static_cast<const wchar_t*>(::GlobalLock(data));
    std::wstring output = text ? std::wstring(text) : std::wstring();
    if (text) {
        ::GlobalUnlock(data);
    }
    ::CloseClipboard();
    return output;
}

// ReplaceEditSelection inserts text into an edit control. Inputs are edit HWND
// and text; processing uses EM_REPLACESEL so paste respects the current
// selection/caret; no value is returned.
void ReplaceEditSelection(HWND edit, const std::wstring& text) {
    if (edit) {
        ::SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

// SelectAllEditText selects all text in a target edit control. Input is edit
// HWND; processing sends EM_SETSEL; no value is returned.
void SelectAllEditText(HWND edit) {
    if (edit) {
        ::SendMessageW(edit, EM_SETSEL, 0, -1);
        ::SetFocus(edit);
    }
}

// CreateEdit creates a single-line or multiline edit control with the project
// UI font. Inputs are parent/id/geometry/style flags; processing calls
// CreateWindowExW; output is the child HWND.
HWND CreateEdit(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, DWORD extraStyle) {
    HWND hwnd = ::CreateWindowExW(WS_EX_CLIENTEDGE,
        L"EDIT",
        text ? text : L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    return hwnd;
}

// LayoutChildren positions all controls inside the page. Input is the page
// state and current client rectangle; processing computes a simple two-panel
// layout; no value is returned.
void LayoutChildren(DriverMemoryViewState& state, const RECT& rc) {
    const int margin = 12;
    const int labelWidth = 58;
    const int editHeight = 24;
    const int buttonWidth = 88;
    const int gap = 8;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int rowTop = margin + 26;
    const int inputWidth = 130;

    ::MoveWindow(state.pidEdit, margin + labelWidth, rowTop, inputWidth, editHeight, TRUE);
    ::MoveWindow(state.addressEdit, margin + labelWidth + inputWidth + 72, rowTop, inputWidth + 70, editHeight, TRUE);
    ::MoveWindow(state.lengthEdit, margin + labelWidth + inputWidth + 72 + inputWidth + 70 + 72, rowTop, inputWidth, editHeight, TRUE);

    const int buttonTop = rowTop + editHeight + gap;
    ::MoveWindow(state.readButton, margin, buttonTop, buttonWidth, editHeight + 2, TRUE);
    ::MoveWindow(state.writeButton, margin + buttonWidth + gap, buttonTop, buttonWidth, editHeight + 2, TRUE);

    const int hexTop = buttonTop + editHeight + 34;
    const int statusHeight = 58;
    const int hexHeight = std::max(80, height - hexTop - statusHeight - (margin * 2));
    const int contentWidth = std::max(100, width - margin * 2);
    ::MoveWindow(state.hexEdit, margin, hexTop, contentWidth, hexHeight, TRUE);
    ::MoveWindow(state.statusEdit, margin, hexTop + hexHeight + gap, contentWidth, statusHeight, TRUE);
}

// PaintLabels draws static labels directly on the page to keep the child window
// count small. Input is page HWND and paint DC; processing draws title and field
// labels using the shared theme; no value is returned.
void PaintLabels(HWND hwnd, HDC dc) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());

    const COLORREF text = Ksword::Ui::AppTheme().textColor;
    const COLORREF muted = Ksword::Ui::AppTheme().mutedTextColor;
    RECT title{ 12, 8, rc.right - 12, 28 };
    Ksword::Ui::DrawTextLine(dc, L"Driver Memory Read / Write", title, text, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT pid{ 12, 38, 70, 62 };
    RECT address{ 200, 38, 272, 62 };
    RECT length{ 472, 38, 544, 62 };
    RECT hex{ 12, 96, rc.right - 12, 118 };
    Ksword::Ui::DrawTextLine(dc, L"PID", pid, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"Address", address, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"Length", length, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"Hex input / output", hex, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// StateFromWindow returns the state pointer stored on the page HWND. Input is a
// page HWND; processing reads GWLP_USERDATA; output is null before WM_NCCREATE
// finishes or after WM_NCDESTROY clears the pointer.
DriverMemoryViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<DriverMemoryViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// HandleRead validates read fields and invokes the driver facade. Input is page
// state; processing parses PID/address/length and calls DriverMemoryClient;
// output is reflected in the hex and status edit controls.
void HandleRead(DriverMemoryViewState& state) {
    DriverMemoryReadRequest request;
    std::wstring error;
    if (!ParseReadRequest(GetWindowTextString(state.pidEdit),
            GetWindowTextString(state.addressEdit),
            GetWindowTextString(state.lengthEdit),
            request,
            error)) {
        SetStatus(state, error);
        return;
    }

    if (state.operationInProgress || !state.operationTask) {
        SetStatus(state, L"内存操作正在执行。");
        return;
    }
    state.operationInProgress = true;
    ::EnableWindow(state.readButton, FALSE);
    ::EnableWindow(state.writeButton, FALSE);
    SetStatus(state, L"正在后台执行 R0 内存读取…");
    state.operationTask->request(
        [request] {
            MemoryOperationSnapshot snapshot{};
            snapshot.readOperation = true;
            DriverMemoryClient client;
            snapshot.readResult = client.ReadMemory(request);
            return snapshot;
        },
        [&state](std::uint64_t, std::optional<MemoryOperationSnapshot>&& snapshot, std::exception_ptr error) {
            state.operationInProgress = false;
            ::EnableWindow(state.readButton, TRUE);
            ::EnableWindow(state.writeButton, TRUE);
            if (error || !snapshot.has_value()) {
                SetStatus(state, L"R0 内存读取异常结束。");
                return;
            }
            if (snapshot->readResult.success) {
                ::SetWindowTextW(state.hexEdit, FormatHexBytesForDisplay(snapshot->readResult.bytes).c_str());
            }
            SetStatus(state, snapshot->readResult.statusText);
        });
}

// HandleWrite validates write fields and invokes the driver facade. Input is
// page state; processing parses PID/address/hex bytes and calls
// DriverMemoryClient; output is reflected in the status edit control.
void HandleWrite(DriverMemoryViewState& state) {
    DriverMemoryWriteRequest request;
    std::wstring error;
    if (!ParseWriteRequest(GetWindowTextString(state.pidEdit),
            GetWindowTextString(state.addressEdit),
            GetWindowTextString(state.hexEdit),
            request,
            error)) {
        SetStatus(state, error);
        return;
    }

    if (state.operationInProgress || !state.operationTask) {
        SetStatus(state, L"内存操作正在执行。");
        return;
    }
    state.operationInProgress = true;
    ::EnableWindow(state.readButton, FALSE);
    ::EnableWindow(state.writeButton, FALSE);
    SetStatus(state, L"正在后台执行 R0 内存写入…");
    state.operationTask->request(
        [request = std::move(request)] {
            MemoryOperationSnapshot snapshot{};
            DriverMemoryClient client;
            snapshot.writeResult = client.WriteMemory(request);
            return snapshot;
        },
        [&state](std::uint64_t, std::optional<MemoryOperationSnapshot>&& snapshot, std::exception_ptr error) {
            state.operationInProgress = false;
            ::EnableWindow(state.readButton, TRUE);
            ::EnableWindow(state.writeButton, TRUE);
            SetStatus(state, error || !snapshot.has_value() ? L"R0 内存写入异常结束。" : snapshot->writeResult.statusText);
        });
}

// NormalizeHexBuffer parses and rewrites the hex edit as canonical two-digit
// byte text. Input is page state; processing never performs driver I/O; output
// is reflected in the edit control and status line.
void NormalizeHexBuffer(DriverMemoryViewState& state) {
    std::vector<std::uint8_t> bytes;
    std::wstring error;
    if (!ParseHexBytes(GetWindowTextString(state.hexEdit), bytes, error)) {
        SetStatus(state, error);
        return;
    }
    ::SetWindowTextW(state.hexEdit, FormatHexBytesForDisplay(bytes).c_str());
    SetStatus(state, L"Hex buffer normalized.");
}

// ShowMemoryContextMenu displays compact driver-memory actions. Inputs are page
// state, target child window and screen point; processing groups read/write,
// Hex-buffer, and status commands into submenus before dispatching the selected
// command; no value is returned.
void ShowMemoryContextMenu(DriverMemoryViewState& state, HWND target, POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    const bool textTarget = target == state.hexEdit || target == state.statusEdit;
    const bool hexTarget = target == state.hexEdit || target == state.hwnd;
    HMENU driverMenu = ::CreatePopupMenu();
    if (driverMenu) {
        ::AppendMenuW(driverMenu, MF_STRING, kMemoryMenuRead, L"读取");
        ::AppendMenuW(driverMenu, MF_STRING, kMemoryMenuWrite, L"写入");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(driverMenu), L"驱动内存");
    }
    HMENU hexMenu = ::CreatePopupMenu();
    if (hexMenu) {
        ::AppendMenuW(hexMenu, MF_STRING | (textTarget ? 0U : MF_GRAYED), kMemoryMenuSelectAll, L"全选");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuCopyHex, L"复制 Hex");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuPasteHex, L"粘贴 Hex");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuClearHex, L"清空 Hex");
        ::AppendMenuW(hexMenu, MF_STRING | (hexTarget ? 0U : MF_GRAYED), kMemoryMenuNormalizeHex, L"格式化 Hex");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hexMenu), L"Hex");
    }
    HMENU statusMenu = ::CreatePopupMenu();
    if (statusMenu) {
        ::AppendMenuW(statusMenu, MF_STRING, kMemoryMenuCopyStatus, L"复制状态");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(statusMenu), L"状态");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        state.hwnd,
        nullptr);
    ::DestroyMenu(menu);

    switch (command) {
    case kMemoryMenuRead:
        HandleRead(state);
        break;
    case kMemoryMenuWrite:
        HandleWrite(state);
        break;
    case kMemoryMenuSelectAll:
        SelectAllEditText(textTarget ? target : state.hexEdit);
        break;
    case kMemoryMenuCopyHex:
        SetStatus(state, CopyTextToClipboard(state.hwnd, GetWindowTextString(state.hexEdit)) ? L"Hex copied." : L"Copy Hex failed.");
        break;
    case kMemoryMenuPasteHex:
        ReplaceEditSelection(state.hexEdit, TextFromClipboard(state.hwnd));
        SetStatus(state, L"Hex paste requested.");
        break;
    case kMemoryMenuClearHex:
        ::SetWindowTextW(state.hexEdit, L"");
        SetStatus(state, L"Hex buffer cleared.");
        break;
    case kMemoryMenuNormalizeHex:
        NormalizeHexBuffer(state);
        break;
    case kMemoryMenuCopyStatus:
        SetStatus(state, CopyTextToClipboard(state.hwnd, GetWindowTextString(state.statusEdit)) ? L"Status copied." : L"Copy status failed.");
        break;
    default:
        break;
    }
}

// CreateChildControls creates every input/output control for the page. Input is
// page state with hwnd set; processing creates edit controls and buttons; no
// value is returned because missing children are handled by normal HWND checks.
void CreateChildControls(DriverMemoryViewState& state) {
    state.pidEdit = CreateEdit(state.hwnd, kPidEditId, L"", 0, 0, 0, 0, 0);
    state.addressEdit = CreateEdit(state.hwnd, kAddressEditId, L"0x0", 0, 0, 0, 0, 0);
    state.lengthEdit = CreateEdit(state.hwnd, kLengthEditId, L"16", 0, 0, 0, 0, 0);
    state.readButton = Ksword::Ui::CreateButton(state.hwnd, kReadButtonId, L"Read", 0, 0, 0, 0);
    state.writeButton = Ksword::Ui::CreateButton(state.hwnd, kWriteButtonId, L"Write", 0, 0, 0, 0);
    state.hexEdit = CreateEdit(state.hwnd,
        kHexEditId,
        L"",
        0,
        0,
        0,
        0,
        ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
    state.statusEdit = CreateEdit(state.hwnd,
        kStatusEditId,
        L"Driver-only memory read/write surface. Requests are sent through ArkDriverClient and the shared memory IOCTL protocol.",
        0,
        0,
        0,
        0,
        ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY);
}

// DriverMemoryViewWndProc dispatches page window messages. Inputs are standard
// Win32 message parameters; processing owns state lifetime, child layout and
// button clicks; output is an LRESULT for DefWindowProcW-compatible handling.
LRESULT CALLBACK DriverMemoryViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto state = std::make_unique<DriverMemoryViewState>();
        state->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.release()));
    }

    DriverMemoryViewState* state = StateFromWindow(hwnd);
    switch (msg) {
    case WM_CREATE:
        if (state) {
            CreateChildControls(*state);
            state->operationTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<MemoryOperationSnapshot>>(hwnd, kMsgMemoryOperationCompleted);
        }
        return 0;
    case WM_SIZE:
        if (state) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            LayoutChildren(*state, rc);
        }
        return 0;
    case WM_COMMAND:
        if (state && HIWORD(wParam) == BN_CLICKED) {
            const int id = LOWORD(wParam);
            if (id == kReadButtonId) {
                HandleRead(*state);
                return 0;
            }
            if (id == kWriteButtonId) {
                HandleWrite(*state);
                return 0;
            }
        }
        break;
    case kMsgMemoryOperationCompleted:
        if (state && state->operationTask && state->operationTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case WM_CONTEXTMENU:
        if (state) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                ::GetWindowRect(state->hexEdit ? state->hexEdit : hwnd, &rc);
                pt = { rc.left + 16, rc.top + 16 };
            }
            HWND target = reinterpret_cast<HWND>(wParam);
            if (target != state->hexEdit && target != state->statusEdit) {
                target = state->hwnd;
            }
            ShowMemoryContextMenu(*state, target, pt);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        PaintLabels(hwnd, dc);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        if (state && state->operationTask) {
            state->operationTask->cancel();
        }
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// RegisterDriverMemoryViewClass installs the page WNDCLASS once. Input is none;
// processing calls RegisterClassW and accepts already-registered classes; output
// is true when CreateWindowExW can use the class.
bool RegisterDriverMemoryViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = DriverMemoryViewWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kDriverMemoryViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

constexpr wchar_t kProcessMemoryEvidenceViewClass[] = L"KswordARKLight.ProcessMemoryEvidenceView";
constexpr int kEvidencePidEditId = 51601;
constexpr int kEvidenceStartEditId = 51602;
constexpr int kEvidenceLimitEditId = 51603;
constexpr int kEvidenceRefreshButtonId = 51604;
constexpr int kEvidenceListId = 51605;
constexpr int kEvidenceStatusEditId = 51606;

// ProcessMemoryEvidenceRow is the R3-only audit model for one VA region. Inputs
// come from VirtualQueryEx and, when available, QueryWorkingSetEx; processing
// derives risk columns without reading bytes or changing mappings; return
// behavior is value storage for ListView rendering.
struct ProcessMemoryEvidenceRow {
    std::uint64_t baseAddress = 0;
    std::uint64_t allocationBase = 0;
    std::uint64_t regionSize = 0;
    DWORD state = 0;
    DWORD protect = 0;
    DWORD type = 0;
    bool workingSetKnown = false;
    bool workingSetValid = false;
    bool workingSetShared = false;
    bool writableExecutable = false;
    bool privateExecutable = false;
    bool largePageUnknown = true;
    bool badPage = false;
    bool mappedFile = false;
};

// ProcessMemoryEvidenceViewState owns the R3 fallback evidence controls. Inputs
// arrive through Win32 messages; processing performs read-only process queries;
// no destructor-owned handles are kept after each refresh.
struct ProcessMemoryEvidenceViewState {
    HWND hwnd = nullptr;
    HWND pidEdit = nullptr;
    HWND startEdit = nullptr;
    HWND limitEdit = nullptr;
    HWND refreshButton = nullptr;
    HWND list = nullptr;
    HWND statusEdit = nullptr;
};

using QueryWorkingSetExFn = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);

// Hex64 formats an integer as uppercase 0x-prefixed text. Input is the numeric
// value; processing uses iostream formatting; output is stable ListView text.
std::wstring Hex64(const std::uint64_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

// YesNo returns a localized boolean marker. Input is a boolean observation;
// output is short text for risk/evidence columns.
const wchar_t* YesNo(const bool value) {
    return value ? L"是" : L"否";
}

// ProtectionIsExecutable checks whether a Win32 page protection allows execute.
// Input is MEMORY_BASIC_INFORMATION.Protect or AllocationProtect; processing
// strips guard/nocache modifiers and tests execute variants; output is true for
// executable user-mode protections.
bool ProtectionIsExecutable(const DWORD protect) {
    const DWORD base = protect & 0xFFU;
    return base == PAGE_EXECUTE ||
        base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE ||
        base == PAGE_EXECUTE_WRITECOPY;
}

// ProtectionIsWritable checks whether a Win32 page protection allows writes or
// copy-on-write writes. Input is a protection mask; output is true for writable
// mappings used by the WX risk heuristic.
bool ProtectionIsWritable(const DWORD protect) {
    const DWORD base = protect & 0xFFU;
    return base == PAGE_READWRITE ||
        base == PAGE_WRITECOPY ||
        base == PAGE_EXECUTE_READWRITE ||
        base == PAGE_EXECUTE_WRITECOPY;
}

// ProtectionText converts Win32 protection bits into a compact display string.
// Input is a protection mask; processing preserves guard/nocache/writecombine
// suffixes; output is human-readable and never used as an authority decision.
std::wstring ProtectionText(const DWORD protect) {
    const DWORD base = protect & 0xFFU;
    std::wstring text;
    switch (base) {
    case PAGE_NOACCESS: text = L"NOACCESS"; break;
    case PAGE_READONLY: text = L"R"; break;
    case PAGE_READWRITE: text = L"RW"; break;
    case PAGE_WRITECOPY: text = L"WC"; break;
    case PAGE_EXECUTE: text = L"X"; break;
    case PAGE_EXECUTE_READ: text = L"RX"; break;
    case PAGE_EXECUTE_READWRITE: text = L"RWX"; break;
    case PAGE_EXECUTE_WRITECOPY: text = L"XWC"; break;
    default: text = L"0x" + Hex64(base).substr(2); break;
    }
    if ((protect & PAGE_GUARD) != 0) {
        text += L"|GUARD";
    }
    if ((protect & PAGE_NOCACHE) != 0) {
        text += L"|NOCACHE";
    }
    if ((protect & PAGE_WRITECOMBINE) != 0) {
        text += L"|WRITECOMBINE";
    }
    return text;
}

// StateText converts MEM_* state to display text. Input is mbi.State; output is
// a short table value.
std::wstring StateText(const DWORD state) {
    switch (state) {
    case MEM_COMMIT: return L"COMMIT";
    case MEM_RESERVE: return L"RESERVE";
    case MEM_FREE: return L"FREE";
    default: return Hex64(state);
    }
}

// TypeText converts MEM_PRIVATE/MAPPED/IMAGE values to display text. Input is
// mbi.Type; output is a short table value.
std::wstring TypeText(const DWORD type) {
    switch (type) {
    case MEM_PRIVATE: return L"PRIVATE";
    case MEM_MAPPED: return L"MAPPED";
    case MEM_IMAGE: return L"IMAGE";
    case 0: return L"-";
    default: return Hex64(type);
    }
}

// EvidenceColumns returns the fixed report columns for the process VA evidence
// page. There is no input; output is consumed by the shared ListView helper.
std::vector<Ksword::Ui::ListViewColumn> EvidenceColumns() {
    return {
        { 0, 130, LVCFMT_LEFT, L"Base" },
        { 1, 130, LVCFMT_LEFT, L"AllocBase" },
        { 2, 92, LVCFMT_RIGHT, L"Size" },
        { 3, 74, LVCFMT_LEFT, L"State" },
        { 4, 92, LVCFMT_LEFT, L"Protect" },
        { 5, 80, LVCFMT_LEFT, L"Type" },
        { 6, 70, LVCFMT_LEFT, L"WX" },
        { 7, 92, LVCFMT_LEFT, L"PrivateExec" },
        { 8, 86, LVCFMT_LEFT, L"LargePage" },
        { 9, 74, LVCFMT_LEFT, L"BadPage" },
        { 10, 82, LVCFMT_LEFT, L"MappedFile" },
        { 11, 98, LVCFMT_LEFT, L"WorkingSet" },
    };
}

// QueryWorkingSetExDynamic calls K32QueryWorkingSetEx when the API is present.
// Inputs are a process handle and one address; processing dynamically resolves
// kernel32 export to avoid adding project dependencies; output reports whether
// working-set evidence was available and fills validity/shared flags.
bool QueryWorkingSetExDynamic(HANDLE process, void* address, bool& valid, bool& shared) {
    valid = false;
    shared = false;
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        return false;
    }
    const auto query = reinterpret_cast<QueryWorkingSetExFn>(::GetProcAddress(kernel32, "K32QueryWorkingSetEx"));
    if (!query) {
        return false;
    }
    PSAPI_WORKING_SET_EX_INFORMATION info{};
    info.VirtualAddress = address;
    if (!query(process, &info, static_cast<DWORD>(sizeof(info)))) {
        return false;
    }
    valid = info.VirtualAttributes.Valid != 0;
    shared = info.VirtualAttributes.Shared != 0;
    return true;
}

// BuildEvidenceRow derives display/risk facts from one MEMORY_BASIC_INFORMATION
// record. Inputs are mbi plus optional working-set facts; processing never reads
// target bytes; output is a row object for the report table.
ProcessMemoryEvidenceRow BuildEvidenceRow(
    const MEMORY_BASIC_INFORMATION& mbi,
    const bool workingSetKnown,
    const bool workingSetValid,
    const bool workingSetShared) {
    ProcessMemoryEvidenceRow row;
    row.baseAddress = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
    row.allocationBase = reinterpret_cast<std::uint64_t>(mbi.AllocationBase);
    row.regionSize = static_cast<std::uint64_t>(mbi.RegionSize);
    row.state = mbi.State;
    row.protect = mbi.Protect;
    row.type = mbi.Type;
    row.workingSetKnown = workingSetKnown;
    row.workingSetValid = workingSetValid;
    row.workingSetShared = workingSetShared;
    row.writableExecutable = mbi.State == MEM_COMMIT && ProtectionIsExecutable(mbi.Protect) && ProtectionIsWritable(mbi.Protect);
    row.privateExecutable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && ProtectionIsExecutable(mbi.Protect);
    row.badPage = mbi.State == MEM_COMMIT && ((mbi.Protect & PAGE_GUARD) != 0 || ((mbi.Protect & 0xFFU) == PAGE_NOACCESS));
    row.mappedFile = mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED;
    return row;
}

// InsertEvidenceRow appends one evidence row to the ListView. Inputs are a list
// HWND and model row; processing converts each field to text; output is the
// inserted row index from the shared helper.
int InsertEvidenceRow(HWND list, const ProcessMemoryEvidenceRow& row) {
    std::wstring workingSetText = L"未知";
    if (row.workingSetKnown) {
        workingSetText = row.workingSetValid ? (row.workingSetShared ? L"Valid/Shared" : L"Valid/Private") : L"Not resident";
    }
    return Ksword::Ui::InsertListViewTextRow(list, {
        Hex64(row.baseAddress),
        Hex64(row.allocationBase),
        std::to_wstring(row.regionSize),
        StateText(row.state),
        ProtectionText(row.protect),
        TypeText(row.type),
        YesNo(row.writableExecutable),
        YesNo(row.privateExecutable),
        row.largePageUnknown ? L"R3未知" : L"否",
        YesNo(row.badPage),
        YesNo(row.mappedFile),
        workingSetText,
    });
}

// SetEvidenceStatus writes a status summary to the read-only status box. Inputs
// are page state and text; processing updates the edit control; no value is
// returned.
void SetEvidenceStatus(ProcessMemoryEvidenceViewState& state, const std::wstring& text) {
    if (state.statusEdit) {
        ::SetWindowTextW(state.statusEdit, text.c_str());
    }
}

// EnumerateProcessMemoryEvidence walks a target process VA map with R3 APIs.
// Inputs are PID/start/limit from UI; processing uses VirtualQueryEx and an
// optional QueryWorkingSetEx sample for each region; output is a vector of
// read-only evidence rows plus status text.
std::vector<ProcessMemoryEvidenceRow> EnumerateProcessMemoryEvidence(
    const DWORD processId,
    const std::uint64_t startAddress,
    const std::uint64_t maxRegions,
    std::wstring& statusText) {
    std::vector<ProcessMemoryEvidenceRow> rows;
    if (processId == 0 || maxRegions == 0) {
        statusText = L"PID 和 MaxRegions 必须非零。";
        return rows;
    }

    HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!process) {
        statusText = L"OpenProcess 查询失败，win32=" + std::to_wstring(::GetLastError()) + L"。";
        return rows;
    }

    std::uint64_t address = startAddress;
    std::uint64_t queried = 0;
    std::uint64_t workingSetKnown = 0;
    while (queried < maxRegions) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T returned = ::VirtualQueryEx(
            process,
            reinterpret_cast<LPCVOID>(address),
            &mbi,
            sizeof(mbi));
        if (returned == 0) {
            break;
        }

        bool wsValid = false;
        bool wsShared = false;
        const bool wsKnown = QueryWorkingSetExDynamic(process, mbi.BaseAddress, wsValid, wsShared);
        if (wsKnown) {
            ++workingSetKnown;
        }
        rows.push_back(BuildEvidenceRow(mbi, wsKnown, wsValid, wsShared));
        ++queried;

        const std::uint64_t base = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
        const std::uint64_t size = static_cast<std::uint64_t>(mbi.RegionSize);
        if (size == 0 || base + size <= address) {
            break;
        }
        address = base + size;
        if (address < base) {
            break;
        }
    }

    ::CloseHandle(process);
    statusText = L"R3 fallback evidence: VirtualQueryEx regions=" + std::to_wstring(rows.size()) +
        L"，WorkingSetEx rows=" + std::to_wstring(workingSetKnown) +
        L"。LargePage/PTE/物理地址字段需要 R0 PTE/VA translation 支持，当前页只读降级显示未知。";
    return rows;
}

// RefreshProcessEvidence validates UI input and rebuilds the ListView. Input is
// the page state; processing does only read-only R3 queries; output is visible
// table/status state.
void RefreshProcessEvidence(ProcessMemoryEvidenceViewState& state) {
    std::uint64_t pidValue = 0;
    std::uint64_t startValue = 0;
    std::uint64_t limitValue = 0;
    std::wstring error;
    if (!ParseUnsignedInteger(GetWindowTextString(state.pidEdit), std::numeric_limits<DWORD>::max(), L"PID", pidValue, error) ||
        !ParseUnsignedInteger(GetWindowTextString(state.startEdit), std::numeric_limits<std::uint64_t>::max(), L"Start", startValue, error) ||
        !ParseUnsignedInteger(GetWindowTextString(state.limitEdit), 100000ULL, L"MaxRegions", limitValue, error)) {
        SetEvidenceStatus(state, error);
        return;
    }

    std::wstring status;
    const std::vector<ProcessMemoryEvidenceRow> rows = EnumerateProcessMemoryEvidence(
        static_cast<DWORD>(pidValue),
        startValue,
        limitValue,
        status);

    Ksword::Ui::ScopedListViewRedrawLock redrawLock(state.list);
    Ksword::Ui::ClearListViewRows(state.list);
    for (const ProcessMemoryEvidenceRow& row : rows) {
        InsertEvidenceRow(state.list, row);
    }
    SetEvidenceStatus(state, status);
}

// LayoutEvidenceChildren positions every control in the process VA page. Inputs
// are page state and current client rect; processing computes a simple report
// layout; no value is returned.
void LayoutEvidenceChildren(ProcessMemoryEvidenceViewState& state, const RECT& rc) {
    const int margin = 12;
    const int editHeight = 24;
    const int width = std::max(0, static_cast<int>(rc.right - rc.left));
    const int height = std::max(0, static_cast<int>(rc.bottom - rc.top));
    ::MoveWindow(state.pidEdit, 64, 36, 110, editHeight, TRUE);
    ::MoveWindow(state.startEdit, 238, 36, 160, editHeight, TRUE);
    ::MoveWindow(state.limitEdit, 488, 36, 100, editHeight, TRUE);
    ::MoveWindow(state.refreshButton, 606, 35, 86, editHeight + 2, TRUE);
    const int statusHeight = 56;
    const int listTop = 74;
    const int listHeight = std::max(80, height - listTop - statusHeight - margin);
    ::MoveWindow(state.list, margin, listTop, std::max(100, width - margin * 2), listHeight, TRUE);
    ::MoveWindow(state.statusEdit, margin, listTop + listHeight + 8, std::max(100, width - margin * 2), statusHeight, TRUE);
}

// PaintEvidenceLabels draws static labels for the R3 evidence page. Inputs are
// page HWND and paint DC; processing draws title and field labels; no value is
// returned.
void PaintEvidenceLabels(HWND hwnd, HDC dc) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());
    const COLORREF text = Ksword::Ui::AppTheme().textColor;
    const COLORREF muted = Ksword::Ui::AppTheme().mutedTextColor;
    Ksword::Ui::DrawTextLine(dc, L"Process VA / PTE Evidence (R3 read-only fallback)", { 12, 8, rc.right - 12, 30 }, text, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"PID", { 12, 36, 58, 60 }, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"Start", { 190, 36, 232, 60 }, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Ksword::Ui::DrawTextLine(dc, L"MaxRegions", { 410, 36, 486, 60 }, muted, Ksword::Ui::SystemUIFont(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// StateFromEvidenceWindow returns state stored on the evidence page HWND. Input
// is a page HWND; processing reads GWLP_USERDATA; output is null before create
// or after destroy.
ProcessMemoryEvidenceViewState* StateFromEvidenceWindow(HWND hwnd) {
    return reinterpret_cast<ProcessMemoryEvidenceViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// CreateEvidenceChildControls creates all R3 evidence controls. Input is page
// state with hwnd set; processing creates edit/button/list/status controls and
// columns; no direct value is returned.
void CreateEvidenceChildControls(ProcessMemoryEvidenceViewState& state) {
    state.pidEdit = CreateEdit(state.hwnd, kEvidencePidEditId, std::to_wstring(::GetCurrentProcessId()).c_str(), 0, 0, 0, 0, 0);
    state.startEdit = CreateEdit(state.hwnd, kEvidenceStartEditId, L"0x0", 0, 0, 0, 0, 0);
    state.limitEdit = CreateEdit(state.hwnd, kEvidenceLimitEditId, L"512", 0, 0, 0, 0, 0);
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kEvidenceRefreshButtonId, L"Refresh", 0, 0, 0, 0);
    state.list = Ksword::Ui::CreateReportListView(state.hwnd, kEvidenceListId, 0, 0, 0, 0, LVS_SHOWSELALWAYS);
    Ksword::Ui::AddListViewColumns(state.list, EvidenceColumns());
    state.statusEdit = CreateEdit(state.hwnd,
        kEvidenceStatusEditId,
        L"只读 R3 fallback：VirtualQueryEx / QueryWorkingSetEx。R0 PTE/VA translation 不可用时 LargePage、物理地址、PTE 位显示为未知。",
        0,
        0,
        0,
        0,
        ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY);
}

// ProcessMemoryEvidenceWndProc dispatches page messages. Inputs are standard
// Win32 message parameters; processing owns state lifetime, layout and refresh
// button behavior; output is a DefWindowProc-compatible LRESULT.
LRESULT CALLBACK ProcessMemoryEvidenceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto state = std::make_unique<ProcessMemoryEvidenceViewState>();
        state->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.release()));
    }

    ProcessMemoryEvidenceViewState* state = StateFromEvidenceWindow(hwnd);
    switch (msg) {
    case WM_CREATE:
        if (state) {
            CreateEvidenceChildControls(*state);
        }
        return 0;
    case WM_SIZE:
        if (state) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            LayoutEvidenceChildren(*state, rc);
        }
        return 0;
    case WM_COMMAND:
        if (state && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kEvidenceRefreshButtonId) {
            RefreshProcessEvidence(*state);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        PaintEvidenceLabels(hwnd, dc);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// RegisterProcessMemoryEvidenceViewClass installs the R3 evidence page class.
// There is no input; processing is idempotent; output is true when the class can
// be used by CreateWindowExW.
bool RegisterProcessMemoryEvidenceViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = ProcessMemoryEvidenceWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kProcessMemoryEvidenceViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateDriverMemoryView(HWND parent, const RECT& bounds) {
    if (!RegisterDriverMemoryViewClass()) {
        return nullptr;
    }

    return ::CreateWindowExW(0,
        kDriverMemoryViewClass,
        L"Driver Memory",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);
}

HWND CreateProcessMemoryEvidenceView(HWND parent, const RECT& bounds) {
    if (!RegisterProcessMemoryEvidenceViewClass()) {
        return nullptr;
    }

    return ::CreateWindowExW(0,
        kProcessMemoryEvidenceViewClass,
        L"Process Memory Evidence",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);
}

} // namespace Ksword::Features::Memory
