#pragma once

#include "../Core/Win32Lean.h"

#include <commctrl.h>
#include <string>
#include <vector>

namespace Ksword::Ui {

// ListViewColumn describes one report-mode ListView column. Inputs are plain
// Win32 column attributes; processing happens in AddListViewColumn(s); there is
// no owned resource and no destructor behavior.
struct ListViewColumn {
    int index = 0;                 // Zero-based target column index.
    int width = 120;               // Initial width in logical pixels.
    int format = LVCFMT_LEFT;      // LVCFMT_* alignment value.
    std::wstring title;            // Header text shown by the common control.
};

// ScopedWindowRedrawLock temporarily disables redraw on one HWND. Input is a
// target HWND plus an optional invalidate-on-exit flag; processing sends
// WM_SETREDRAW(FALSE) in the constructor and restores redraw in the destructor;
// there is no return value because the lifetime itself is the operation.
class ScopedWindowRedrawLock final {
public:
    explicit ScopedWindowRedrawLock(HWND hwnd = nullptr, bool invalidateOnExit = true) noexcept;
    ~ScopedWindowRedrawLock();

    ScopedWindowRedrawLock(const ScopedWindowRedrawLock&) = delete;
    ScopedWindowRedrawLock& operator=(const ScopedWindowRedrawLock&) = delete;

private:
    HWND hwnd_ = nullptr;
    bool invalidateOnExit_ = true;
};

// ScopedListViewRedrawLock disables redraw for both a report ListView and its
// header. Input is the ListView HWND; processing holds two window redraw locks
// so bulk insert/delete/update paths avoid per-row painting; there is no return
// value.
class ScopedListViewRedrawLock final {
public:
    explicit ScopedListViewRedrawLock(HWND listView) noexcept;
    ~ScopedListViewRedrawLock() = default;

    ScopedListViewRedrawLock(const ScopedListViewRedrawLock&) = delete;
    ScopedListViewRedrawLock& operator=(const ScopedListViewRedrawLock&) = delete;

private:
    ScopedWindowRedrawLock listViewLock_;
    ScopedWindowRedrawLock headerLock_;
};

// CreateReportListView creates a child WC_LISTVIEWW in report mode. Inputs are
// parent, id and geometry; processing applies the system UI font and common
// extended styles; output is the created HWND or nullptr on failure.
HWND CreateReportListView(HWND parent, int id, int x, int y, int width, int height, DWORD extraStyle = 0);

// AddListViewColumn inserts or replaces one ListView column. Inputs are a
// ListView HWND and a column descriptor; processing sends LVM_INSERTCOLUMNW;
// output is true when the column was inserted.
bool AddListViewColumn(HWND listView, const ListViewColumn& column);

// AddListViewColumns inserts multiple report columns in order. Inputs are a
// ListView HWND and descriptors; processing stops only after trying each entry;
// output is true when every column insert succeeded.
bool AddListViewColumns(HWND listView, const std::vector<ListViewColumn>& columns);

// SetListViewColumnWidth updates one column width. Inputs are a ListView HWND,
// column index and pixel width; processing sends LVM_SETCOLUMNWIDTH; output is
// true when the common control accepted the change.
bool SetListViewColumnWidth(HWND listView, int columnIndex, int width);

// ClearListViewColumns removes all current report columns. Input is a ListView
// HWND; processing deletes columns from right to left; no value is returned.
void ClearListViewColumns(HWND listView);

// InsertListViewTextRow appends one row and optional subitems. Inputs are the
// target ListView and text values; processing fills LVITEMW records; output is
// the inserted row index or -1 on failure.
int InsertListViewTextRow(HWND listView, const std::vector<std::wstring>& cells, LPARAM itemData = 0);

// ClearListViewRows removes all items from the ListView. Input is a ListView
// HWND; processing sends LVM_DELETEALLITEMS; no value is returned.
void ClearListViewRows(HWND listView);

} // namespace Ksword::Ui
