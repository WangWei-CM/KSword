#include "HardwareView.h"

#include "HardwareEnumerator.h"
#include "HardwareModel.h"
#include "../../Ui/Controls.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <setupapi.h>
#include <windowsx.h>

#include <cstring>
#include <memory>
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
constexpr UINT kMenuEnableDevice = 62003;
constexpr UINT kMenuDisableDevice = 62004;
constexpr UINT kMenuUninstallDevice = 62005;
constexpr UINT kMenuScanHardware = 62006;

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

// DevInfoSet owns an HDEVINFO handle used by one SetupAPI operation. Inputs are
// handles returned from SetupDiGetClassDevsW; processing releases the handle in
// the destructor; output is access through get()/valid().
class DevInfoSet final {
public:
    explicit DevInfoSet(HDEVINFO handle) : handle_(handle) {}
    ~DevInfoSet() {
        if (valid()) {
            ::SetupDiDestroyDeviceInfoList(handle_);
        }
    }
    DevInfoSet(const DevInfoSet&) = delete;
    DevInfoSet& operator=(const DevInfoSet&) = delete;
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    HDEVINFO get() const { return handle_; }

private:
    HDEVINFO handle_ = INVALID_HANDLE_VALUE;
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

// OpenDeviceInfo opens one device instance into a temporary SetupAPI device set.
// Inputs are a device instance ID and output SP_DEVINFO_DATA; processing uses an
// empty device-info set and SetupDiOpenDeviceInfoW; output is the owning set.
std::unique_ptr<DevInfoSet> OpenDeviceInfo(const std::wstring& instanceId, SP_DEVINFO_DATA& data, std::wstring& errorText) {
    data = SP_DEVINFO_DATA{};
    data.cbSize = sizeof(data);
    auto set = std::make_unique<DevInfoSet>(::SetupDiCreateDeviceInfoList(nullptr, nullptr));
    if (!set->valid()) {
        errorText = L"SetupDiCreateDeviceInfoList failed: " + std::to_wstring(::GetLastError());
        return nullptr;
    }
    if (!::SetupDiOpenDeviceInfoW(set->get(), instanceId.c_str(), nullptr, 0, &data)) {
        errorText = L"SetupDiOpenDeviceInfoW failed: " + std::to_wstring(::GetLastError());
        return nullptr;
    }
    return set;
}

// ChangeDeviceState enables or disables a device through DIF_PROPERTYCHANGE.
// Inputs are a device instance ID and target state; processing routes through
// SetupAPI class installers; output is a concise result message.
std::wstring ChangeDeviceState(const std::wstring& instanceId, bool enable) {
    SP_DEVINFO_DATA data{};
    std::wstring error;
    std::unique_ptr<DevInfoSet> set = OpenDeviceInfo(instanceId, data, error);
    if (!set) {
        return error;
    }

    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
    params.Scope = DICS_FLAG_GLOBAL;
    params.HwProfile = 0;
    if (!::SetupDiSetClassInstallParamsW(set->get(),
            &data,
            &params.ClassInstallHeader,
            sizeof(params))) {
        return L"SetupDiSetClassInstallParamsW failed: " + std::to_wstring(::GetLastError());
    }
    if (!::SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set->get(), &data)) {
        return L"SetupDiCallClassInstaller(DIF_PROPERTYCHANGE) failed: " + std::to_wstring(::GetLastError());
    }
    return enable ? L"设备启用请求已提交。" : L"设备禁用请求已提交。";
}

// UninstallDevice removes one device through the standard SetupAPI class
// installer path. Input is a device instance ID; output is a user-facing result
// string. The caller is responsible for confirmation.
std::wstring UninstallDevice(const std::wstring& instanceId) {
    SP_DEVINFO_DATA data{};
    std::wstring error;
    std::unique_ptr<DevInfoSet> set = OpenDeviceInfo(instanceId, data, error);
    if (!set) {
        return error;
    }
    if (!::SetupDiCallClassInstaller(DIF_REMOVE, set->get(), &data)) {
        return L"SetupDiCallClassInstaller(DIF_REMOVE) failed: " + std::to_wstring(::GetLastError());
    }
    return L"设备卸载请求已提交。";
}

// RescanHardware asks Configuration Manager to re-enumerate the local root
// devnode. Inputs are none; output is a human-readable result string.
std::wstring RescanHardware() {
    DEVINST root = 0;
    CONFIGRET status = ::CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL);
    if (status != CR_SUCCESS) {
        return L"CM_Locate_DevNodeW failed: " + std::to_wstring(status);
    }
    status = ::CM_Reenumerate_DevNode(root, CM_REENUMERATE_NORMAL);
    if (status != CR_SUCCESS) {
        return L"CM_Reenumerate_DevNode failed: " + std::to_wstring(status);
    }
    return L"已请求重新扫描硬件。";
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
        state->statusText = L"Devices: " + std::to_wstring(result.devices.size());
        state->model.setDevices(std::move(result.devices));
    } else {
        state->statusText = result.diagnosticText;
        state->model.setDevices({});
    }
    PopulateTree(state);
    ::InvalidateRect(state->hwnd, nullptr, TRUE);
}

// ShowDeviceContextMenu displays Device Manager actions for the selected device.
// Inputs are module state and a screen point; processing corrects selection,
// groups refresh/copy/device actions into submenus, executes the chosen
// SetupAPI/CM action, and refreshes the tree after mutating actions; no value is
// returned.
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
        ::AppendMenuW(refreshMenu, MF_STRING, kMenuScanHardware, L"扫描检测硬件改动");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(refreshMenu), L"刷新");
    }
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | (hasDevice ? 0U : MF_GRAYED), kMenuCopyInstanceId, L"复制实例 ID");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }
    HMENU deviceMenu = ::CreatePopupMenu();
    if (deviceMenu) {
        ::AppendMenuW(deviceMenu, MF_STRING | (hasDevice ? 0U : MF_GRAYED), kMenuEnableDevice, L"启用设备");
        ::AppendMenuW(deviceMenu, MF_STRING | (hasDevice ? 0U : MF_GRAYED), kMenuDisableDevice, L"禁用设备");
        ::AppendMenuW(deviceMenu, MF_STRING | (hasDevice ? 0U : MF_GRAYED), kMenuUninstallDevice, L"卸载设备");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(deviceMenu), L"设备操作");
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

    std::wstring result;
    switch (command) {
    case kMenuRefresh:
        RefreshDevices(state);
        return;
    case kMenuScanHardware:
        result = RescanHardware();
        state->statusText = result;
        RefreshDevices(state);
        return;
    case kMenuCopyInstanceId:
        state->statusText = WriteClipboardText(state->hwnd, node->instanceId) ? L"已复制设备实例 ID。" : L"复制设备实例 ID 失败。";
        ::InvalidateRect(state->hwnd, nullptr, TRUE);
        return;
    case kMenuEnableDevice:
        result = ChangeDeviceState(node->instanceId, true);
        state->statusText = result;
        RefreshDevices(state);
        return;
    case kMenuDisableDevice:
        if (::MessageBoxW(state->hwnd, L"确定要禁用选中设备吗？", L"禁用设备", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            result = ChangeDeviceState(node->instanceId, false);
            state->statusText = result;
            RefreshDevices(state);
        }
        return;
    case kMenuUninstallDevice:
        if (::MessageBoxW(state->hwnd, L"确定要卸载选中设备吗？该操作可能需要重启或重新扫描硬件。", L"卸载设备", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            result = UninstallDevice(node->instanceId);
            state->statusText = result;
            RefreshDevices(state);
        }
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
