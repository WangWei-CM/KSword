#include "FileView.h"

#include "FileActions.h"
#include "FileSystemEnumerator.h"
#include "PathNavigator.h"

#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ksword::Features::File {
namespace {

constexpr wchar_t kFileViewClass[] = L"KswordARKLight.FileView";
constexpr int kButtonBackId = 52001;
constexpr int kButtonForwardId = 52002;
constexpr int kButtonUpId = 52003;
constexpr int kButtonRefreshId = 52004;
constexpr int kPathEditId = 52005;
constexpr int kButtonGoId = 52006;
constexpr int kListId = 52007;
constexpr int kStatusId = 52008;
constexpr UINT kColumnMenuBaseId = 52500;

// FileColumnSpec describes one report column. Inputs are static id/title/width
// values; processing uses the same table for initial creation and user column
// visibility toggles; no runtime ownership is stored here.
struct FileColumnSpec {
    int index;
    int defaultWidth;
    const wchar_t* title;
};

constexpr FileColumnSpec kFileColumns[] = {
    { 0, 260, L"名称" },
    { 1, 90, L"类型" },
    { 2, 110, L"大小" },
    { 3, 150, L"修改时间" },
    { 4, 80, L"标志" },
    { 5, 420, L"完整路径" },
};

// FileViewState owns every child HWND and model object used by one File page.
// Inputs are created during WM_CREATE; processing in FileViewProc updates this
// state in response to navigation, list notifications and menu commands; the
// object is deleted on WM_NCDESTROY and has no independent return behavior.
struct FileViewState {
    HWND hwnd = nullptr;
    HWND backButton = nullptr;
    HWND forwardButton = nullptr;
    HWND upButton = nullptr;
    HWND refreshButton = nullptr;
    HWND pathEdit = nullptr;
    HWND goButton = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    HIMAGELIST imageList = nullptr;
    PathNavigator navigator;
    FileSystemEnumerator enumerator;
    std::vector<FileEntry> entries;
    std::unordered_map<std::wstring, int> iconCache;
    bool columnVisible[std::size(kFileColumns)] = { true, true, true, true, true, true };
};

// RegisterFileViewClass registers the native page class once per process. Input
// is the module instance; processing installs a normal Win32 WNDCLASS; output is
// true when the class is available or was already registered.
bool RegisterFileViewClass(HINSTANCE instance);

// FileViewProc dispatches the custom page messages. Inputs are Win32 window
// procedure arguments; processing routes to stateful helpers; output is a Win32
// LRESULT.
LRESULT CALLBACK FileViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// StateFromWindow returns the per-page state pointer stored in GWLP_USERDATA.
// Input is a page HWND; output may be null before WM_CREATE finishes.
FileViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<FileViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// SetStatus writes a short status line for user feedback. Inputs are state and
// text; processing updates the child STATIC; no value is returned.
void SetStatus(FileViewState& state, const std::wstring& text) {
    if (state.status) {
        ::SetWindowTextW(state.status, text.c_str());
    }
}

// CreateFileImageList creates the small-icon image list used by the report
// control. There is no input; processing asks comctl32 for system small-icon
// dimensions; output is owned by FileViewState and destroyed on WM_NCDESTROY.
HIMAGELIST CreateFileImageList() {
    return ImageList_Create(
        ::GetSystemMetrics(SM_CXSMICON),
        ::GetSystemMetrics(SM_CYSMICON),
        ILC_COLOR32 | ILC_MASK,
        32,
        32);
}

// FallbackIconQueryPath chooses a stable synthetic path for generic shell
// icons. Input is a FileEntry kind/name; processing preserves file extensions
// when possible; output is used only with SHGFI_USEFILEATTRIBUTES.
std::wstring FallbackIconQueryPath(const FileEntry& entry) {
    if (entry.kind == FileEntryKind::Directory) {
        return L"folder";
    }
    if (entry.kind == FileEntryKind::Drive) {
        return entry.fullPath.empty() ? L"C:\\" : entry.fullPath;
    }

    const std::size_t dot = entry.name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < entry.name.size()) {
        return std::wstring(L"file") + entry.name.substr(dot);
    }
    return L"file";
}

// FileIconCacheKey builds a deterministic cache key for the shell icon. Input
// is one enumerated file row; processing uses the real path for drives,
// directories and reparse points, and extension-based keys for normal files;
// output indexes FileViewState::iconCache.
std::wstring FileIconCacheKey(const FileEntry& entry) {
    std::wstring key = std::to_wstring(static_cast<int>(entry.kind)) + L"|";
    if (entry.kind != FileEntryKind::File || entry.reparsePoint) {
        return key + entry.fullPath;
    }

    const std::size_t dot = entry.name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        key += entry.name.substr(dot);
    } else {
        key += L"<no-extension>";
    }
    key += L"|" + std::to_wstring(entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    return key;
}

// AddFileIconFromShell resolves one icon through SHGetFileInfoW and copies it
// into the page image list. Inputs are the image list and file row; processing
// first tries the real path, then a generic attributes-based fallback; output is
// the image index or -1 when the shell cannot provide an icon.
int AddFileIconFromShell(HIMAGELIST imageList, const FileEntry& entry) {
    if (!imageList) {
        return -1;
    }

    SHFILEINFOW info{};
    DWORD attributes = entry.attributes;
    if (entry.kind == FileEntryKind::Directory) {
        attributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (!entry.fullPath.empty() &&
        ::SHGetFileInfoW(entry.fullPath.c_str(), attributes, &info, sizeof(info), flags) &&
        info.hIcon) {
        const int index = ImageList_AddIcon(imageList, info.hIcon);
        ::DestroyIcon(info.hIcon);
        return index;
    }

    const std::wstring fallback = FallbackIconQueryPath(entry);
    flags |= SHGFI_USEFILEATTRIBUTES;
    if (::SHGetFileInfoW(fallback.c_str(), attributes, &info, sizeof(info), flags) && info.hIcon) {
        const int index = ImageList_AddIcon(imageList, info.hIcon);
        ::DestroyIcon(info.hIcon);
        return index;
    }
    return -1;
}

// IconIndexForEntry returns a cached small icon index for one file row. Inputs
// are page state and FileEntry; processing extracts/caches through Shell APIs;
// output is a ListView image index or -1 when no icon is available.
int IconIndexForEntry(FileViewState& state, const FileEntry& entry) {
    const std::wstring key = FileIconCacheKey(entry);
    const auto found = state.iconCache.find(key);
    if (found != state.iconCache.end()) {
        return found->second;
    }

    const int index = AddFileIconFromShell(state.imageList, entry);
    if (index >= 0) {
        state.iconCache.emplace(key, index);
    }
    return index;
}

// CreateChildControls creates the toolbar, path edit, report list and status
// controls. Input is the initialized state; output is true when the core list
// and edit controls are available.
bool CreateChildControls(FileViewState& state) {
    state.backButton = Ksword::Ui::CreateButton(state.hwnd, kButtonBackId, L"后退", 0, 0, 64, 24);
    state.forwardButton = Ksword::Ui::CreateButton(state.hwnd, kButtonForwardId, L"前进", 0, 0, 64, 24);
    state.upButton = Ksword::Ui::CreateButton(state.hwnd, kButtonUpId, L"向上", 0, 0, 64, 24);
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kButtonRefreshId, L"刷新", 0, 0, 64, 24);
    state.goButton = Ksword::Ui::CreateButton(state.hwnd, kButtonGoId, L"转到", 0, 0, 64, 24);
    state.pathEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 24, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)), ::GetModuleHandleW(nullptr), nullptr);
    state.list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 400, 300, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), ::GetModuleHandleW(nullptr), nullptr);
    state.status = Ksword::Ui::CreateText(state.hwnd, kStatusId, L"", 0, 0, 300, 20);

    if (state.pathEdit) {
        ::SendMessageW(state.pathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    if (!state.pathEdit || !state.list) {
        return false;
    }

    ::SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ListView_SetExtendedListViewStyle(state.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    state.imageList = CreateFileImageList();
    if (state.imageList) {
        ListView_SetImageList(state.list, state.imageList, LVSIL_SMALL);
    }

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (const FileColumnSpec& spec : kFileColumns) {
        column.pszText = const_cast<LPWSTR>(spec.title);
        column.cx = spec.defaultWidth;
        column.iSubItem = spec.index;
        ListView_InsertColumn(state.list, spec.index, &column);
    }

    return true;
}

// LayoutFileView positions all child controls within the page client rect.
// Input is state plus current client size; processing calls MoveWindow; no value
// is returned.
void LayoutFileView(FileViewState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int margin = 8;
    const int buttonW = 64;
    const int buttonH = 26;
    const int gap = 6;
    const int statusH = 22;
    int x = margin;
    int y = margin;

    ::MoveWindow(state.backButton, x, y, buttonW, buttonH, TRUE);
    x += buttonW + gap;
    ::MoveWindow(state.forwardButton, x, y, buttonW, buttonH, TRUE);
    x += buttonW + gap;
    ::MoveWindow(state.upButton, x, y, buttonW, buttonH, TRUE);
    x += buttonW + gap;
    ::MoveWindow(state.refreshButton, x, y, buttonW, buttonH, TRUE);
    x += buttonW + gap;

    const int goW = 64;
    const int editW = std::max(120, static_cast<int>(rc.right - x - goW - gap - margin));
    ::MoveWindow(state.pathEdit, x, y, editW, buttonH, TRUE);
    x += editW + gap;
    ::MoveWindow(state.goButton, x, y, goW, buttonH, TRUE);

    const int listTop = y + buttonH + margin;
    const int listH = std::max(80, static_cast<int>(rc.bottom - listTop - statusH - margin));
    ::MoveWindow(state.list, margin, listTop, std::max(80, static_cast<int>(rc.right - margin * 2)), listH, TRUE);
    ::MoveWindow(state.status, margin, listTop + listH + 2, std::max(80, static_cast<int>(rc.right - margin * 2)), statusH, TRUE);
}

// EntryTypeText converts a model kind into a Chinese display string. Input is a
// FileEntry; output is a static text label for the list view.
const wchar_t* EntryTypeText(const FileEntry& entry) {
    if (entry.kind == FileEntryKind::Drive) {
        return L"驱动器";
    }
    if (entry.kind == FileEntryKind::Directory) {
        return entry.reparsePoint ? L"目录/链接" : L"目录";
    }
    return entry.reparsePoint ? L"文件/链接" : L"文件";
}

// SetListText writes one subitem in the report list. Inputs are list HWND, row,
// column and text; processing uses ListView_SetItemText; no value is returned.
void SetListText(HWND list, int row, int column, const std::wstring& text) {
    ListView_SetItemText(list, row, column, const_cast<LPWSTR>(text.c_str()));
}

// ApplyColumnVisibility adjusts report-column widths without rebuilding rows.
// Input is the FileView state; processing keeps column zero visible and sets
// hidden columns to zero width; no value is returned.
void ApplyColumnVisibility(FileViewState& state) {
    if (!state.list) {
        return;
    }
    for (std::size_t index = 0; index < std::size(kFileColumns); ++index) {
        const FileColumnSpec& spec = kFileColumns[index];
        const bool visible = index == 0 || state.columnVisible[index];
        ListView_SetColumnWidth(state.list, spec.index, visible ? spec.defaultWidth : 0);
    }
}

// ShowColumnMenu displays the retained "选择列" action as a compact Win32
// checkable popup. Inputs are state and screen point; processing toggles one
// column at a time; no value is returned.
void ShowColumnMenu(FileViewState& state, POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    for (std::size_t index = 0; index < std::size(kFileColumns); ++index) {
        const UINT flags = MF_STRING |
            (state.columnVisible[index] ? MF_CHECKED : MF_UNCHECKED) |
            (index == 0 ? MF_GRAYED : 0U);
        ::AppendMenuW(menu, flags, kColumnMenuBaseId + static_cast<UINT>(index), kFileColumns[index].title);
    }
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command >= kColumnMenuBaseId && command < kColumnMenuBaseId + std::size(kFileColumns)) {
        const std::size_t index = static_cast<std::size_t>(command - kColumnMenuBaseId);
        if (index != 0) {
            state.columnVisible[index] = !state.columnVisible[index];
            ApplyColumnVisibility(state);
            SetStatus(state, L"已更新文件列表列显示。");
        }
    }
}

// PopulateList replaces the visible rows with the latest enumeration result.
// Input is mutable state and an enumeration result; processing stores the row
// model and fills list-view subitems; no value is returned.
void PopulateList(FileViewState& state, DirectoryEnumerationResult result) {
    state.entries = std::move(result.entries);
    Ksword::Ui::ScopedListViewRedrawLock redrawLock(state.list);
    ListView_DeleteAllItems(state.list);

    for (int index = 0; index < static_cast<int>(state.entries.size()); ++index) {
        const FileEntry& entry = state.entries[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        const int iconIndex = IconIndexForEntry(state, entry);
        if (iconIndex >= 0) {
            item.mask |= LVIF_IMAGE;
            item.iImage = iconIndex;
        }
        item.iItem = index;
        item.pszText = const_cast<LPWSTR>(entry.name.c_str());
        item.lParam = index;
        ListView_InsertItem(state.list, &item);

        SetListText(state.list, index, 1, EntryTypeText(entry));
        SetListText(state.list, index, 2, FileSystemEnumerator::formatSize(entry.size, entry.kind));
        SetListText(state.list, index, 3, FileSystemEnumerator::formatLastWriteTime(entry.lastWriteTime));
        SetListText(state.list, index, 4, FileSystemEnumerator::formatAttributes(entry.attributes));
        SetListText(state.list, index, 5, entry.fullPath);
    }

    SetStatus(state, result.statusText);
    ApplyColumnVisibility(state);
    ::EnableWindow(state.backButton, state.navigator.canNavigateBack());
    ::EnableWindow(state.forwardButton, state.navigator.canNavigateForward());
}

// RefreshCurrentPath enumerates the navigator's active path. Input is the page
// state; processing updates the path edit and list rows; no value is returned.
void RefreshCurrentPath(FileViewState& state) {
    const std::wstring current = state.navigator.currentPath();
    ::SetWindowTextW(state.pathEdit, current.empty() ? L"此电脑" : current.c_str());
    PopulateList(state, state.enumerator.enumerate(current));
}

// NavigateTo requests a new directory. Inputs are state and raw path; processing
// normalizes the path through PathNavigator and refreshes rows; no value is
// returned.
void NavigateTo(FileViewState& state, const std::wstring& path) {
    state.navigator.navigateTo(path);
    RefreshCurrentPath(state);
}

// TextFromWindow reads a child window's Unicode text. Input is an HWND; output
// is the current text content or empty when the HWND is null.
std::wstring TextFromWindow(HWND hwnd) {
    if (!hwnd) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        ::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    }
    text.resize(static_cast<size_t>(length));
    return text;
}

// SelectedEntry copies the currently selected list row into an output model.
// Inputs are state and output pointer; output is true when a row is selected.
bool SelectedEntry(const FileViewState& state, FileEntry* entry) {
    const int selected = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(state.entries.size())) {
        return false;
    }
    if (entry) {
        *entry = state.entries[selected];
    }
    return true;
}

// OpenSelectedEntry either navigates into directories/drives or shell-opens
// files. Input is state; processing uses local Win32 ShellExecuteW only for file
// open behavior; no value is returned.
void OpenSelectedEntry(FileViewState& state) {
    FileEntry entry;
    if (!SelectedEntry(state, &entry)) {
        return;
    }
    if (entry.kind == FileEntryKind::Drive || entry.kind == FileEntryKind::Directory) {
        NavigateTo(state, entry.fullPath);
        return;
    }
    FileActionContext context;
    context.owner = state.hwnd;
    context.currentDirectory = state.navigator.currentPath();
    context.selectedEntry = entry;
    context.hasSelection = true;
    const FileActionResult result = FileActions::execute(FileActionId::OpenRun, context);
    SetStatus(state, result.statusText);
}

// ShowContextMenu builds a context object, displays the FileActions menu, then
// executes the selected command. Inputs are state and screen coordinate; no
// value is returned.
void ShowContextMenu(FileViewState& state, POINT screenPoint) {
    FileActionContext context;
    context.owner = state.hwnd;
    context.currentDirectory = state.navigator.currentPath();
    context.hasSelection = SelectedEntry(state, &context.selectedEntry);
    const FileActionId action = FileActions::showContextMenu(state.hwnd, context, screenPoint);
    if (action == FileActionId::SelectColumns) {
        ShowColumnMenu(state, screenPoint);
        return;
    }
    const FileActionResult result = FileActions::execute(action, context);
    if (result.navigateRequested) {
        NavigateTo(state, result.navigatePath);
        return;
    }
    if (result.refreshRequested) {
        RefreshCurrentPath(state);
    }
    if (!result.statusText.empty()) {
        SetStatus(state, result.statusText);
    }
}

// HandleCommand processes toolbar button commands. Inputs are state and command
// id; output is true only when the command belongs to FileView.
bool HandleCommand(FileViewState& state, int commandId) {
    switch (commandId) {
    case kButtonBackId:
        if (state.navigator.navigateBack()) {
            RefreshCurrentPath(state);
        }
        return true;
    case kButtonForwardId:
        if (state.navigator.navigateForward()) {
            RefreshCurrentPath(state);
        }
        return true;
    case kButtonUpId:
        state.navigator.navigateUp();
        RefreshCurrentPath(state);
        return true;
    case kButtonRefreshId:
        RefreshCurrentPath(state);
        return true;
    case kButtonGoId:
        NavigateTo(state, TextFromWindow(state.pathEdit));
        return true;
    default:
        return false;
    }
}

// HandleNotify processes list-view notifications. Inputs are state and NMHDR;
// output is true only when the notification was consumed.
bool HandleNotify(FileViewState& state, const NMHDR* hdr) {
    if (!hdr || hdr->hwndFrom != state.list) {
        return false;
    }
    if (hdr->code == NM_DBLCLK) {
        OpenSelectedEntry(state);
        return true;
    }
    if (hdr->code == NM_RCLICK) {
        POINT pt{};
        ::GetCursorPos(&pt);
        ShowContextMenu(state, pt);
        return true;
    }
    return false;
}

bool RegisterFileViewClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = FileViewProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kFileViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
        return true;
    }
    return false;
}

LRESULT CALLBACK FileViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FileViewState* state = StateFromWindow(hwnd);
    switch (msg) {
    case WM_CREATE: {
        auto ownedState = std::make_unique<FileViewState>();
        ownedState->hwnd = hwnd;
        state = ownedState.get();
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (!CreateChildControls(*state)) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return -1;
        }
        ownedState.release();
        RefreshCurrentPath(*state);
        return 0;
    }
    case WM_SIZE:
        if (state) {
            LayoutFileView(*state);
        }
        return 0;
    case WM_COMMAND:
        if (state && HandleCommand(*state, LOWORD(wParam))) {
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (state && HandleNotify(*state, reinterpret_cast<NMHDR*>(lParam))) {
            return 0;
        }
        break;
    case WM_CONTEXTMENU:
        if (state && reinterpret_cast<HWND>(wParam) == state->list) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                ::GetWindowRect(state->list, &rc);
                pt = { rc.left + 20, rc.top + 20 };
            }
            ShowContextMenu(*state, pt);
            return 0;
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
        if (state && state->imageList) {
            ImageList_Destroy(state->imageList);
            state->imageList = nullptr;
        }
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

HWND CreateFileViewPage(HWND parent, const RECT& bounds) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!RegisterFileViewClass(instance)) {
        return nullptr;
    }
    return ::CreateWindowExW(0, kFileViewClass, L"文件", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        instance,
        nullptr);
}

} // namespace Ksword::Features::File
