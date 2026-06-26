#include "DriverMemoryView.h"

#include "DriverMemoryClient.h"
#include "DriverMemoryModel.h"
#include "../../Ui/Controls.h"
#include "../../Ui/Theme.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
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
    DriverMemoryClient client;
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

    const DriverMemoryReadResult result = state.client.ReadMemory(request);
    if (result.success) {
        ::SetWindowTextW(state.hexEdit, FormatHexBytesForDisplay(result.bytes).c_str());
    }
    SetStatus(state, result.statusText);
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

    const DriverMemoryWriteResult result = state.client.WriteMemory(request);
    SetStatus(state, result.statusText);
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

} // namespace Ksword::Features::Memory
