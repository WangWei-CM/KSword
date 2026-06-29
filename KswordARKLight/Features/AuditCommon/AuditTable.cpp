#include "AuditTable.h"

#include "AuditFormatting.h"
#include "../../Ui/Controls.h"

#include <commctrl.h>
#include <utility>

namespace Ksword::Features::AuditCommon {
namespace {

constexpr UINT kCopyCellCommand = 72001;
constexpr UINT kCopyRowCommand = 72002;
constexpr UINT kCopyAllCommand = 72003;

// Width returns a non-negative rectangle width. Input is a RECT; output is the
// width used for CreateWindow/MoveWindow calls.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative rectangle height. Input is a RECT; output is the
// height used for CreateWindow/MoveWindow calls.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// ColumnCount returns the current ListView report column count. Input is a
// ListView HWND; processing reads the header control; output is zero when the
// table is invalid or has no header.
int ColumnCount(HWND listView) {
    HWND header = listView ? ListView_GetHeader(listView) : nullptr;
    return header ? Header_GetItemCount(header) : 0;
}

// HeaderText returns one report column title. Inputs are a ListView and column
// index; processing reads HDITEMW text; output is empty if the column is invalid.
std::wstring HeaderText(HWND listView, int column) {
    HWND header = listView ? ListView_GetHeader(listView) : nullptr;
    if (!header || column < 0) {
        return {};
    }

    wchar_t buffer[256]{};
    HDITEMW item{};
    item.mask = HDI_TEXT;
    item.pszText = buffer;
    item.cchTextMax = static_cast<int>(sizeof(buffer) / sizeof(buffer[0]));
    if (!Header_GetItem(header, column, &item)) {
        return {};
    }
    return buffer;
}

// HitTestCell selects the row under a context-menu point and returns row/column.
// Inputs are a ListView and screen point; processing maps to client coordinates
// and performs LVM_SUBITEMHITTEST; output is true when a row was hit.
bool HitTestCell(HWND listView, POINT screenPoint, int* rowOut, int* columnOut) {
    if (!listView) {
        return false;
    }

    POINT clientPoint = screenPoint;
    ::ScreenToClient(listView, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int row = ListView_SubItemHitTest(listView, &hit);
    if (row >= 0) {
        ListView_SetItemState(listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(listView, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (rowOut) {
        *rowOut = row;
    }
    if (columnOut) {
        *columnOut = hit.iSubItem;
    }
    return row >= 0;
}

} // namespace

HWND CreateReadOnlyAuditTable(
    HWND parent,
    const int id,
    const RECT& bounds,
    const std::vector<AuditTableColumn>& columns) {
    HWND listView = Ksword::Ui::CreateReportListView(
        parent,
        id,
        bounds.left,
        bounds.top,
        Width(bounds),
        Height(bounds));
    if (!listView) {
        return nullptr;
    }
    ConfigureReadOnlyAuditTable(listView);
    SetAuditTableColumns(listView, columns);
    return listView;
}

bool ConfigureReadOnlyAuditTable(HWND listView) {
    if (!listView) {
        return false;
    }

    ::SendMessageW(listView, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ListView_SetExtendedListViewStyleEx(
        listView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
    return true;
}

bool SetAuditTableColumns(HWND listView, const std::vector<AuditTableColumn>& columns) {
    if (!listView) {
        return false;
    }

    Ksword::Ui::ClearListViewColumns(listView);
    std::vector<Ksword::Ui::ListViewColumn> uiColumns;
    uiColumns.reserve(columns.size());
    for (std::size_t index = 0; index < columns.size(); ++index) {
        uiColumns.push_back({
            static_cast<int>(index),
            columns[index].width,
            columns[index].format,
            columns[index].title
        });
    }
    return Ksword::Ui::AddListViewColumns(listView, uiColumns);
}

void ReplaceAuditTableRows(HWND listView, const std::vector<std::vector<std::wstring>>& rows) {
    if (!listView) {
        return;
    }

    Ksword::Ui::ScopedListViewRedrawLock redrawLock(listView);
    ListView_DeleteAllItems(listView);
    for (const std::vector<std::wstring>& row : rows) {
        Ksword::Ui::InsertListViewTextRow(listView, row);
    }
}

std::wstring GetAuditTableCellText(HWND listView, const int row, const int column) {
    if (!listView || row < 0 || column < 0) {
        return {};
    }

    std::wstring buffer(256, L'\0');
    for (;;) {
        LVITEMW item{};
        item.iSubItem = column;
        item.cchTextMax = static_cast<int>(buffer.size());
        item.pszText = buffer.data();
        const int copied = static_cast<int>(::SendMessageW(
            listView,
            LVM_GETITEMTEXTW,
            static_cast<WPARAM>(row),
            reinterpret_cast<LPARAM>(&item)));
        if (copied < static_cast<int>(buffer.size()) - 1) {
            buffer.resize(static_cast<std::size_t>(copied));
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768) {
            buffer.resize(static_cast<std::size_t>(copied));
            return buffer;
        }
    }
}

std::wstring GetSelectedAuditTableRowText(HWND listView) {
    if (!listView) {
        return {};
    }

    const int selected = ListView_GetNextItem(listView, -1, LVNI_SELECTED);
    if (selected < 0) {
        return {};
    }

    std::vector<std::wstring> cells;
    const int columns = ColumnCount(listView);
    cells.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        cells.push_back(GetAuditTableCellText(listView, selected, column));
    }
    return BuildTsv({}, { cells });
}

std::wstring BuildAuditTableTsv(HWND listView) {
    if (!listView) {
        return {};
    }

    const int columns = ColumnCount(listView);
    const int rows = ListView_GetItemCount(listView);
    std::vector<std::wstring> headers;
    headers.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        headers.push_back(HeaderText(listView, column));
    }

    std::vector<std::vector<std::wstring>> tableRows;
    tableRows.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        std::vector<std::wstring> cells;
        cells.reserve(static_cast<std::size_t>(columns));
        for (int column = 0; column < columns; ++column) {
            cells.push_back(GetAuditTableCellText(listView, row, column));
        }
        tableRows.push_back(std::move(cells));
    }
    return BuildTsv(headers, tableRows);
}

void ShowAuditTableContextMenu(HWND owner, HWND listView, POINT screenPoint) {
    if (!owner || !listView) {
        return;
    }

    int row = -1;
    int column = 0;
    const bool hasHit = HitTestCell(listView, screenPoint, &row, &column);
    const bool hasSelection = hasHit || ListView_GetNextItem(listView, -1, LVNI_SELECTED) >= 0;

    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kCopyCellCommand, L"复制单元格");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kCopyRowCommand, L"复制整行");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCopyAllCommand, L"复制全部 TSV");

    const UINT command = ::TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        owner,
        nullptr);
    ::DestroyMenu(menu);

    if (command == kCopyCellCommand) {
        CopyTextToClipboard(owner, GetAuditTableCellText(listView, row, column));
    } else if (command == kCopyRowCommand) {
        CopyTextToClipboard(owner, GetSelectedAuditTableRowText(listView));
    } else if (command == kCopyAllCommand) {
        CopyTextToClipboard(owner, BuildAuditTableTsv(listView));
    }
}

} // namespace Ksword::Features::AuditCommon
