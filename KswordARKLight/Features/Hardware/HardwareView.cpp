#include "HardwareView.h"

#include "HardwareEnumerator.h"
#include "HardwareModel.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../Ui/Controls.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Ksword::Features::Hardware {
namespace {
constexpr wchar_t kHardwareViewClass[] = L"KswordARKLight.Hardware.DeviceManagerView";
constexpr int kRefreshButtonId = 61001;
constexpr int kTreeId = 61002;
constexpr int kDetailId = 61003;
constexpr int kHeaderHeight = 32;
constexpr int kGap = 6;
constexpr int kTreeWidth = 420;
constexpr UINT kMenuRefresh = 62001;
constexpr UINT kMenuCopyInstanceId = 62002;

// Width returns a non-negative rectangle width. Input is a RECT; output is the
// usable pixel width used by layout code.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative rectangle height. Input is a RECT; output is the
// usable pixel height used by layout code.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// HardwareViewState owns the child controls and model for one hardware page.
// Inputs arrive through the Win32 window procedure; processing keeps all device
// data local to the Hardware module; no data is shared with other modules.
struct HardwareViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND tree = nullptr;
    HWND detail = nullptr;
    HardwareModel model;
    std::wstring statusText;
};

// AddListColumn inserts one detail list column. Inputs are list HWND, index,
// title and width; processing sends LVM_INSERTCOLUMNW; no return value.
void AddListColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

// SetListText writes one subitem value. Inputs are list HWND, row, column and
// text; processing inserts or updates a list-view item; no return value.
void SetListText(HWND list, int row, int column, const std::wstring& text) {
    if (column == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(text.c_str());
        ListView_InsertItem(list, &item);
        return;
    }
    ListView_SetItemText(list, row, column, const_cast<LPWSTR>(text.c_str()));
}

// AddDetailRow appends one property row to the detail list. Inputs are list HWND,
// row index, property name and property value; no value is returned.
void AddDetailRow(HWND list, int row, const std::wstring& name, const std::wstring& value) {
    SetListText(list, row, 0, name);
    SetListText(list, row, 1, value);
}

// WriteClipboardText copies Unicode text to the clipboard. Inputs are an owner
// window and text; processing transfers a movable CF_UNICODETEXT allocation to
// Windows; output reports whether the clipboard accepted it.
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

// Utf8ToWideLossy 将 ArkDriverClient 的窄字符诊断提升为宽字符。
// 输入：通常是 ASCII/UTF-8 风格 message；处理：逐字节提升用于表格展示；
// 返回：宽字符串。
std::wstring Utf8ToWideLossy(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char ch : text) {
        wide.push_back(static_cast<unsigned char>(ch));
    }
    return wide;
}

// DeviceAuditSummaryText 生成 R0 设备审计摘要。
// 输入：DeviceAuditResult；处理：组合 IO 状态、行数和设备/驱动计数；
// 返回：一行详情文本。
std::wstring DeviceAuditSummaryText(const ksword::ark::DeviceAuditResult& result) {
    std::wostringstream stream;
    stream << (result.io.ok ? L"OK" : (result.unsupported ? L"Unsupported" : L"Unavailable"))
           << L"; rows=" << result.entries.size()
           << L"; returned=" << result.returnedCount << L"/" << result.totalCount
           << L"; drivers=" << result.driverCount
           << L"; devices=" << result.deviceCount
           << L"; win32=" << result.io.win32Error
           << L"; " << Utf8ToWideLossy(result.io.message);
    return stream.str();
}

// SelectedDeviceIndex returns the model index stored on the selected tree item.
// Input is the hardware page state; output is -1 when no valid tree item is
// selected.
int SelectedDeviceIndex(HardwareViewState* state) {
    if (!state || !state->tree) {
        return -1;
    }

    HTREEITEM selected = TreeView_GetSelection(state->tree);
    if (!selected) {
        return -1;
    }
    TVITEMW item{};
    item.mask = TVIF_PARAM;
    item.hItem = selected;
    if (!TreeView_GetItem(state->tree, &item)) {
        return -1;
    }
    return static_cast<int>(item.lParam);
}

// InsertTreeNode recursively inserts a device and its children. Inputs are state,
// device index and parent HTREEITEM; processing stores the model index in lParam;
// output is the inserted tree item, or nullptr when the index is invalid.
HTREEITEM InsertTreeNode(HardwareViewState* state, int index, HTREEITEM parent) {
    const HardwareDeviceNode* node = state ? state->model.deviceAt(index) : nullptr;
    if (!node) {
        return nullptr;
    }

    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    const std::wstring title = CompactDeviceName(*node);
    insert.item.pszText = const_cast<LPWSTR>(title.c_str());
    insert.item.lParam = static_cast<LPARAM>(index);
    HTREEITEM item = TreeView_InsertItem(state->tree, &insert);
    for (int childIndex : node->childIndices) {
        InsertTreeNode(state, childIndex, item);
    }
    return item;
}

// ShowDetail renders the detail pane for one selected device. Inputs are state
// and model index; processing queries live SetupAPI details, falling back to the
// cached model; no value is returned.
void ShowDetail(HardwareViewState* state, int index) {
    if (!state || !state->detail) {
        return;
    }
    ListView_DeleteAllItems(state->detail);
    const HardwareDeviceNode* node = state->model.deviceAt(index);
    if (!node) {
        AddDetailRow(state->detail, 0, L"Selection", L"No device selected");
        return;
    }

    HardwareDeviceDetail detail = QueryDeviceManagerDetails(node->instanceId);
    if (!detail.found) {
        detail = state->model.detailFromNode(*node);
    }

    int row = 0;
    AddDetailRow(state->detail, row++, L"Device", detail.title);
    AddDetailRow(state->detail, row++, L"Audit mode", L"Read-only evidence view; no device state changes are exposed here.");
    const ksword::ark::DriverClient client;
    AddDetailRow(state->detail, row++, L"R0 device stack protocol", DeviceAuditSummaryText(client.queryDeviceStackAudit()));
    AddDetailRow(state->detail, row++, L"R0 input stack protocol", DeviceAuditSummaryText(client.queryInputStackAudit()));
    AddDetailRow(state->detail, row++, L"R0 USB topology protocol", DeviceAuditSummaryText(client.queryUsbTopologyAudit()));
    AddDetailRow(state->detail, row++, L"Input privacy", L"Keyboard, mouse and HID rows are metadata only; no keystroke, movement or report stream is collected.");
    AddDetailRow(state->detail, row++, L"Audit category", HardwareReadOnlyAuditDescription(*node));
    for (const HardwareProperty& property : detail.properties) {
        AddDetailRow(state->detail, row++, property.name, property.value);
    }
}

// SelectFirstRoot selects the first available device after refresh. Input is the
// module state; processing updates the tree selection and detail pane; no return.
void SelectFirstRoot(HardwareViewState* state) {
    if (!state || !state->tree) {
        return;
    }
    HTREEITEM first = TreeView_GetRoot(state->tree);
    if (first) {
        TreeView_SelectItem(state->tree, first);
    } else {
        ShowDetail(state, -1);
    }
}

// PopulateTree rebuilds the tree control from the model. Input is module state;
// processing inserts only device-manager nodes and expands top-level roots; no
// value is returned.
void PopulateTree(HardwareViewState* state) {
    if (!state || !state->tree) {
        return;
    }
    TreeView_DeleteAllItems(state->tree);
    for (int rootIndex : state->model.rootIndices()) {
        HTREEITEM root = InsertTreeNode(state, rootIndex, TVI_ROOT);
        if (root) {
            TreeView_Expand(state->tree, root, TVE_EXPAND);
        }
    }
    SelectFirstRoot(state);
}

// RefreshDevices performs a full SetupAPI/CM enumeration pass. Input is module
// state; processing replaces the model and tree contents; no value is returned.
void RefreshDevices(HardwareViewState* state) {
    if (!state) {
        return;
    }
    HardwareEnumerationResult result = EnumerateDeviceManagerTree();
    if (result.success) {
        state->model.setDevices(std::move(result.devices));
        const HardwareAuditSummary summary = state->model.auditSummary();
        state->statusText = L"Devices: " + std::to_wstring(summary.totalDevices) +
            L" | Input/HID: " + std::to_wstring(summary.inputDevices) + L"/" + std::to_wstring(summary.hidDevices) +
            L" | USB: " + std::to_wstring(summary.usbDevices) +
            L" | PnP PCI/ACPI: " + std::to_wstring(summary.pciDevices) + L"/" + std::to_wstring(summary.acpiDevices) +
            L" | Filters: " + std::to_wstring(summary.filterEvidenceDevices) +
            L" | Attention: " + std::to_wstring(summary.problemDevices);
    } else {
        state->statusText = result.diagnosticText;
        state->model.setDevices({});
    }
    PopulateTree(state);
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// ShowDeviceContextMenu displays read-only evidence actions for the selected
// device. Inputs are module state and a screen point; processing corrects
// selection and offers only refresh/copy operations; no device configuration is
// changed and no value is returned.
void ShowDeviceContextMenu(HardwareViewState* state, POINT screenPoint) {
    if (!state || !state->tree) {
        return;
    }

    POINT clientPoint = screenPoint;
    ::ScreenToClient(state->tree, &clientPoint);
    TVHITTESTINFO hit{};
    hit.pt = clientPoint;
    HTREEITEM hitItem = TreeView_HitTest(state->tree, &hit);
    if (hitItem != nullptr) {
        TreeView_SelectItem(state->tree, hitItem);
    }

    const int index = SelectedDeviceIndex(state);
    const HardwareDeviceNode* node = state->model.deviceAt(index);
    const bool hasDevice = node != nullptr && !node->instanceId.empty();

    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    HMENU refreshMenu = ::CreatePopupMenu();
    if (refreshMenu) {
        ::AppendMenuW(refreshMenu, MF_STRING, kMenuRefresh, L"刷新");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(refreshMenu), L"刷新");
    }
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | (hasDevice ? 0U : MF_GRAYED), kMenuCopyInstanceId, L"复制实例 ID");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        state->hwnd,
        nullptr);
    ::DestroyMenu(menu);

    if (command == 0) {
        return;
    }

    switch (command) {
    case kMenuRefresh:
        RefreshDevices(state);
        return;
    case kMenuCopyInstanceId:
        state->statusText = WriteClipboardText(state->hwnd, node->instanceId) ? L"已复制设备实例 ID。" : L"复制设备实例 ID 失败。";
        ::InvalidateRect(state->hwnd, nullptr, TRUE);
        return;
    default:
        break;
    }
}

// LayoutView positions toolbar, tree, and detail controls. Input is module state;
// processing computes child rectangles from the current client area; no return.
void LayoutView(HardwareViewState* state) {
    if (!state || !state->hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state->hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    ::MoveWindow(state->refreshButton, kGap, kGap, 86, 24, TRUE);
    const int top = kHeaderHeight + kGap;
    const int treeWidth = width > kTreeWidth + 260 ? kTreeWidth : width / 2;
    ::MoveWindow(state->tree, kGap, top, treeWidth - kGap, height - top - kGap, TRUE);
    ::MoveWindow(state->detail, treeWidth + kGap, top, width - treeWidth - (kGap * 2), height - top - kGap, TRUE);
}

// CreateChildControls creates the refresh button, tree, and detail list. Inputs
// are module state and parent HWND; processing initializes columns/fonts; output
// is true when all required controls exist.
bool CreateChildControls(HardwareViewState* state, HWND hwnd) {
    state->refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"Refresh", 0, 0, 80, 24);
    state->tree = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeId)), ::GetModuleHandleW(nullptr), nullptr);
    state->detail = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailId)), ::GetModuleHandleW(nullptr), nullptr);
    if (!state->refreshButton || !state->tree || !state->detail) {
        return false;
    }
    ::SendMessageW(state->tree, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ::SendMessageW(state->detail, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ListView_SetExtendedListViewStyle(state->detail, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    AddListColumn(state->detail, 0, L"Property", 180);
    AddListColumn(state->detail, 1, L"Value", 560);
    return true;
}

// RegisterHardwareViewClass registers the custom hardware page class. There is
// no external input beyond the process module; output is true when the class is
// available for CreateWindowExW.
bool RegisterHardwareViewClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        HardwareViewState* state = reinterpret_cast<HardwareViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<HardwareViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
            return TRUE;
        }
        case WM_CREATE:
            if (state && CreateChildControls(state, hwnd)) {
                LayoutView(state);
                RefreshDevices(state);
            }
            return 0;
        case WM_SIZE:
            LayoutView(state);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kRefreshButtonId) {
                RefreshDevices(state);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* notify = reinterpret_cast<NMHDR*>(lParam);
            if (state && notify && notify->idFrom == kTreeId && notify->code == TVN_SELCHANGEDW) {
                auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
                ShowDetail(state, static_cast<int>(change->itemNew.lParam));
                return 0;
            }
            if (state && notify && notify->idFrom == kTreeId && notify->code == NM_RCLICK) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ShowDeviceContextMenu(state, pt);
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->tree) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->tree, &rc);
                    pt.x = rc.left + 24;
                    pt.y = rc.top + 24;
                }
                ShowDeviceContextMenu(state, pt);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().panelBrush());
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().panelBrush());
            RECT textRc{ 104, 7, rc.right - kGap, kHeaderHeight };
            const std::wstring title = state ? state->statusText : L"Device manager";
            Ksword::Ui::DrawTextLine(dc, title, textRc, Ksword::Ui::AppTheme().mutedTextColor,
                Ksword::Ui::SystemUIFont(), DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
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
    wc.hbrBackground = Ksword::Ui::AppTheme().panelBrush();
    wc.lpszClassName = kHardwareViewClass;
    if (::RegisterClassW(&wc)) {
        return true;
    }
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

HWND CreateHardwareDeviceManagerView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterHardwareViewClass()) {
        return nullptr;
    }
    auto* state = new HardwareViewState();
    HWND hwnd = ::CreateWindowExW(0, kHardwareViewClass, L"Hardware devices",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, Width(bounds), Height(bounds), parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::Hardware
