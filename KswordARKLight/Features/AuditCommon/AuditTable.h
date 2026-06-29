#pragma once

#include "../../Core/Win32Lean.h"
#include "../../Ui/ListViewUtil.h"

#include <string>
#include <vector>

namespace Ksword::Features::AuditCommon {

// AuditTableColumn describes one read-only audit table column. Inputs are title,
// width and alignment; processing maps the descriptor to a Win32 ListView
// column; there is no owned resource.
struct AuditTableColumn {
    std::wstring title;
    int width = 120;
    int format = LVCFMT_LEFT;
};

// CreateReadOnlyAuditTable creates a report ListView configured for audit
// evidence display. Inputs are parent/id/geometry and column descriptors;
// processing creates a non-editing ListView with full-row selection and columns;
// output is the table HWND or nullptr on failure.
HWND CreateReadOnlyAuditTable(
    HWND parent,
    int id,
    const RECT& bounds,
    const std::vector<AuditTableColumn>& columns);

// ConfigureReadOnlyAuditTable applies common read-only styles to an existing
// ListView. Input is a ListView HWND; processing sets extended styles and font;
// output reports whether the HWND was usable.
bool ConfigureReadOnlyAuditTable(HWND listView);

// SetAuditTableColumns replaces all columns on a ListView. Inputs are a ListView
// and column descriptors; processing clears old columns then inserts new ones;
// output is true when every insertion succeeds.
bool SetAuditTableColumns(HWND listView, const std::vector<AuditTableColumn>& columns);

// ReplaceAuditTableRows replaces all rows in one table. Inputs are a ListView
// and rows of already-rendered strings; processing preserves the current column
// set and inserts rows under a redraw lock; no value is returned.
void ReplaceAuditTableRows(HWND listView, const std::vector<std::vector<std::wstring>>& rows);

// GetAuditTableCellText reads one visible table cell. Inputs are ListView HWND,
// row index and column index; processing asks the common control for text with a
// growing buffer; output is empty when the cell is invalid or blank.
std::wstring GetAuditTableCellText(HWND listView, int row, int column);

// GetSelectedAuditTableRowText serializes the selected row. Input is a ListView
// HWND; processing reads all visible columns in the selected item; output is a
// TSV-compatible row string or empty when nothing is selected.
std::wstring GetSelectedAuditTableRowText(HWND listView);

// BuildAuditTableTsv serializes the current table. Input is a ListView HWND;
// processing reads header text and visible rows; output is TSV text suitable for
// clipboard/export operations.
std::wstring BuildAuditTableTsv(HWND listView);

// ShowAuditTableContextMenu displays a read-only copy/export context menu for
// one audit table. Inputs are owner, ListView and screen point; processing
// allows copy cell, copy row, and copy all TSV only; no mutation commands are
// created and no value is returned.
void ShowAuditTableContextMenu(HWND owner, HWND listView, POINT screenPoint);

} // namespace Ksword::Features::AuditCommon
