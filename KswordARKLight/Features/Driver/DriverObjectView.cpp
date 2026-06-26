#include "DriverObjectView.h"

#include "DriverActions.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ListViewUtil.h"

#include <commctrl.h>
#include <windowsx.h>

#include <memory>
#include <string>
#include <vector>

namespace Ksword::Features::Driver {
namespace {

constexpr wchar_t kDriverObjectClass[] = L"KswordARKLight.DriverObjectView";
constexpr int kObjectListId = 64011;
constexpr UINT kObjectMenuDetail = 64201;
constexpr UINT kObjectMenuUnload = 64202;
constexpr UINT kObjectMenuCopyRow = 64203;
constexpr UINT kObjectMenuCopyName = 64204;
constexpr UINT kObjectMenuCopyPath = 64205;
constexpr UINT kObjectMenuCopyTarget = 64206;

// DriverObjectViewState owns the object report list and model pointer. Inputs
// arrive through Win32 messages; processing renders the current snapshot into
// the child list; no model ownership is transferred here.
struct DriverObjectViewState {
    HWND hwnd = nullptr;
    HWND listView = nullptr;
    DriverModel* model = nullptr;
};

// StateFromWindow returns the view state from the HWND. Input is the child
// window; processing reads GWLP_USERDATA; output is null before creation or
// after WM_NCDESTROY clears the pointer.
DriverObjectViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<DriverObjectViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// SetChildFont applies the project UI font to one child window. Input is a
// child HWND; processing sends WM_SETFONT; no value is returned.
void SetChildFont(HWND hwnd) {
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
}

// SelectedObjectRow copies the currently selected object row from the attached
// model. Input is view state and optional output pointer; processing maps the
// selected report row to model storage; output is true when a row exists.
bool SelectedObjectRow(const DriverObjectViewState& state, DriverObjectRow* rowOut) {
    if (!state.listView || !state.model) {
        return false;
    }
    const int selected = ListView_GetNextItem(state.listView, -1, LVNI_SELECTED);
    const auto& rows = state.model->objectRows();
    if (selected < 0 || selected >= static_cast<int>(rows.size())) {
        return false;
    }
    if (rowOut) {
        *rowOut = rows[static_cast<std::size_t>(selected)];
    }
    return true;
}

// DriverObjectNameForRow resolves the best DriverObject name for an object-grid
// row. Input may be a native \Driver row, an R0 summary row, or an R0 child
// MajorFunction/DeviceObject row; output is the \Driver\Name string required by
// ArkDriverClient DriverObject IOCTLs.
std::wstring DriverObjectNameForRow(const DriverObjectRow& row) {
    if (!row.fullPathText.empty() &&
        (row.fullPathText.rfind(L"\\Driver\\", 0) == 0 ||
            row.directoryPathText == L"R0 DriverObject" ||
            row.directoryPathText == L"R0 MajorFunction" ||
            row.directoryPathText == L"R0 DeviceObject")) {
        return row.fullPathText;
    }
    if (row.directoryPathText == L"\\Driver" && row.objectTypeText == L"Driver" && !row.objectNameText.empty()) {
        return L"\\Driver\\" + row.objectNameText;
    }
    return {};
}

// ShowActionResult displays an action result as a modal diagnostic. Inputs are
// owner/title/result; processing uses MessageBoxW and does not mutate the model.
void ShowActionResult(HWND owner, const wchar_t* title, const DriverActionResult& result) {
    ::MessageBoxW(
        owner,
        result.statusText.empty() ? L"<no details>" : result.statusText.c_str(),
        title,
        MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONWARNING));
}

// CopyObjectText writes selected object text to the clipboard. Inputs are view
// state and payload; processing routes through DriverActions; no value returns.
void CopyObjectText(const DriverObjectViewState& state, const std::wstring& text, const wchar_t* title) {
    const bool ok = DriverActions::CopyTextToClipboard(state.hwnd, text);
    ::MessageBoxW(state.hwnd, ok ? L"已复制到剪贴板。" : L"复制失败。", title, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

// BuildObjectRowText serializes one driver object row. Input is the selected
// model row; output is a TSV-compatible string for the copy-row menu command.
std::wstring BuildObjectRowText(const DriverObjectRow& row) {
    return row.directoryPathText + L"\t" +
        row.objectNameText + L"\t" +
        row.objectTypeText + L"\t" +
        row.fullPathText + L"\t" +
        row.targetPathText + L"\t" +
        row.referenceCountText + L"\t" +
        row.handleCountText + L"\t" +
        row.statusText + L"\t" +
        row.capabilityHint;
}

// ShowObjectDetail displays row-local columns and, for DriverObject rows, the
// R0 DriverObject query output. Input is view state; processing calls
// DriverActions only; no value is returned.
void ShowObjectDetail(const DriverObjectViewState& state) {
    DriverObjectRow row;
    if (!SelectedObjectRow(state, &row)) {
        return;
    }

    std::wstring detail =
        L"Directory: " + row.directoryPathText + L"\r\n" +
        L"ObjectName: " + row.objectNameText + L"\r\n" +
        L"Type: " + row.objectTypeText + L"\r\n" +
        L"FullPath: " + row.fullPathText + L"\r\n" +
        L"TargetPath: " + row.targetPathText + L"\r\n" +
        L"ReferenceCount: " + row.referenceCountText + L"\r\n" +
        L"HandleCount: " + row.handleCountText + L"\r\n" +
        L"Status: " + row.statusText + L"\r\n" +
        L"Hint: " + row.capabilityHint + L"\r\n";

    const std::wstring driverObjectName = DriverObjectNameForRow(row);
    if (!driverObjectName.empty()) {
        const DriverActionResult r0Detail = DriverActions::BuildDriverObjectDetailText(driverObjectName);
        detail += L"\r\nR0 DriverObject query (" + driverObjectName + L"):\r\n";
        detail += r0Detail.statusText;
    }

    ::MessageBoxW(state.hwnd, detail.c_str(), L"对象详细信息", MB_OK | MB_ICONINFORMATION);
}

// ForceUnloadObjectDriver confirms and sends the R0 force-unload request for a
// selected DriverObject row. Input is view state; processing routes to
// DriverActions/ArkDriverClient; no value is returned.
void ForceUnloadObjectDriver(DriverObjectViewState& state) {
    DriverObjectRow row;
    if (!SelectedObjectRow(state, &row)) {
        return;
    }
    const std::wstring driverObjectName = DriverObjectNameForRow(row);
    if (driverObjectName.empty()) {
        ::MessageBoxW(state.hwnd, L"选中行不是可卸载的 DriverObject。", L"R0 卸载驱动", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring prompt =
        L"确认通过 R0 IOCTL 请求卸载此 DriverObject？\r\n\r\n" +
        driverObjectName + L"\r\n" +
        row.statusText + L"\r\n" +
        row.capabilityHint;
    if (::MessageBoxW(state.hwnd, prompt.c_str(), L"确认 R0 卸载驱动", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    const DriverActionResult result = DriverActions::ForceUnloadDriverObject(driverObjectName, 0UL);
    ShowActionResult(state.hwnd, L"R0 卸载驱动结果", result);
}

// ShowObjectContextMenu displays the object-info right-click menu and dispatches
// retained commands. Inputs are view state and screen point; processing selects
// the clicked row before executing grouped copy/detail/R0 actions; no value is
// returned.
void ShowObjectContextMenu(DriverObjectViewState& state, POINT screenPoint) {
    POINT clientPoint = screenPoint;
    ::ScreenToClient(state.listView, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int item = ListView_HitTest(state.listView, &hit);
    if (item >= 0) {
        ListView_SetItemState(state.listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(state.listView, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    DriverObjectRow row;
    const bool hasSelection = SelectedObjectRow(state, &row);
    const bool canUnload = hasSelection && !DriverObjectNameForRow(row).empty();
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kObjectMenuCopyRow, L"复制当前行");
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kObjectMenuCopyName, L"复制对象名");
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kObjectMenuCopyPath, L"复制完整路径");
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kObjectMenuCopyTarget, L"复制目标路径");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }
    HMENU detailMenu = ::CreatePopupMenu();
    if (detailMenu) {
        ::AppendMenuW(detailMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kObjectMenuDetail, L"详细信息/R0 DriverObject");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(detailMenu), L"详细信息");
    }
    HMENU r0Menu = ::CreatePopupMenu();
    if (r0Menu) {
        ::AppendMenuW(r0Menu, MF_STRING | (canUnload ? 0U : MF_GRAYED), kObjectMenuUnload, L"R0 卸载驱动");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(r0Menu), L"R0 操作");
    }

    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == kObjectMenuDetail) {
        ShowObjectDetail(state);
    } else if (command == kObjectMenuUnload) {
        ForceUnloadObjectDriver(state);
    } else if (command == kObjectMenuCopyRow ||
        command == kObjectMenuCopyName ||
        command == kObjectMenuCopyPath ||
        command == kObjectMenuCopyTarget) {
        if (!SelectedObjectRow(state, &row)) {
            return;
        }
        if (command == kObjectMenuCopyRow) {
            CopyObjectText(state, BuildObjectRowText(row), L"复制对象行");
        } else if (command == kObjectMenuCopyName) {
            CopyObjectText(state, row.objectNameText, L"复制对象名");
        } else if (command == kObjectMenuCopyPath) {
            CopyObjectText(state, row.fullPathText, L"复制完整路径");
        } else {
            CopyObjectText(state, row.targetPathText, L"复制目标路径");
        }
    }
}

// ObjectColumns returns the report columns shown by the object page. There is
// no input; processing returns static descriptors; output is consumed by the
// ListView helper.
std::vector<Ksword::Ui::ListViewColumn> ObjectColumns() {
    return {
        { 0, 140, LVCFMT_LEFT, L"目录" },
        { 1, 180, LVCFMT_LEFT, L"对象名" },
        { 2, 140, LVCFMT_LEFT, L"类型" },
        { 3, 340, LVCFMT_LEFT, L"完整路径" },
        { 4, 320, LVCFMT_LEFT, L"目标路径" },
        { 5, 120, LVCFMT_RIGHT, L"引用计数" },
        { 6, 120, LVCFMT_RIGHT, L"句柄计数" },
        { 7, 220, LVCFMT_LEFT, L"状态" },
        { 8, 300, LVCFMT_LEFT, L"能力提示" },
    };
}

// PopulateObjectList rebuilds the list-view from the model snapshot. Input is
// the view state; processing copies each object row into the report control;
// no value is returned.
void PopulateObjectList(DriverObjectViewState& state) {
    if (!state.listView) {
        return;
    }

    Ksword::Ui::ClearListViewRows(state.listView);
    const std::vector<DriverObjectRow>* rows = state.model ? &state.model->objectRows() : nullptr;
    if (!rows) {
        return;
    }

    for (const DriverObjectRow& row : *rows) {
        Ksword::Ui::InsertListViewTextRow(state.listView, {
            row.directoryPathText,
            row.objectNameText,
            row.objectTypeText,
            row.fullPathText,
            row.targetPathText,
            row.referenceCountText,
            row.handleCountText,
            row.statusText,
            row.capabilityHint
        });
    }
}

// BuildObjectTsv converts the current object rows into TSV text. Input is the
// view state; processing reads the model snapshot and serializes the same
// columns displayed in the report control; output is clipboard/file ready text.
std::wstring BuildObjectTsv(const DriverObjectViewState& state) {
    std::vector<std::vector<std::wstring>> rows;
    if (state.model) {
        for (const DriverObjectRow& row : state.model->objectRows()) {
            rows.push_back({
                row.directoryPathText,
                row.objectNameText,
                row.objectTypeText,
                row.fullPathText,
                row.targetPathText,
                row.referenceCountText,
                row.handleCountText,
                row.statusText,
                row.capabilityHint
            });
        }
    }
    return DriverActions::BuildTsv(
        { L"目录", L"对象名", L"类型", L"完整路径", L"目标路径", L"引用计数", L"句柄计数", L"状态", L"能力提示" },
        rows);
}

// RegisterDriverObjectClass installs the child window class once. There is no
// input; processing is idempotent; output is true when CreateWindowExW can use
// the class name.
bool RegisterDriverObjectClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        DriverObjectViewState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<DriverObjectViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }

        switch (msg) {
        case WM_CREATE:
            if (state) {
                state->listView = Ksword::Ui::CreateReportListView(hwnd, kObjectListId, 0, 0, 0, 0);
                SetChildFont(state->listView);
                Ksword::Ui::AddListViewColumns(state->listView, ObjectColumns());
                PopulateObjectList(*state);
            }
            return 0;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->listView && header->code == NM_RCLICK) {
                    POINT pt{};
                    ::GetCursorPos(&pt);
                    ShowObjectContextMenu(*state, pt);
                    return 0;
                }
            }
            break;
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->listView) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->listView, &rc);
                    pt = { rc.left + 24, rc.top + 24 };
                }
                ShowObjectContextMenu(*state, pt);
                return 0;
            }
            break;
        case WM_SIZE:
            if (state && state->listView) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                ::MoveWindow(state->listView, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
            }
            return 0;
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kDriverObjectClass;
    if (::RegisterClassW(&wc)) {
        registered = true;
    } else if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateDriverObjectView(HWND parent, const RECT& bounds, DriverModel* model) {
    if (!parent || !RegisterDriverObjectClass()) {
        return nullptr;
    }

    auto* state = new DriverObjectViewState();
    state->model = model;
    HWND hwnd = ::CreateWindowExW(
        0,
        kDriverObjectClass,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE,
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

void ResizeDriverObjectView(HWND view, const RECT& bounds) {
    if (view) {
        ::MoveWindow(view,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            TRUE);
    }
}

void RefreshDriverObjectView(HWND view) {
    DriverObjectViewState* state = StateFromWindow(view);
    if (state) {
        PopulateObjectList(*state);
    }
}

std::wstring ExportDriverObjectViewTsv(HWND view) {
    const DriverObjectViewState* state = StateFromWindow(view);
    return state ? BuildObjectTsv(*state) : std::wstring();
}

} // namespace Ksword::Features::Driver
