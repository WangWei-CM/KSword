#pragma once

#include "AuditStatus.h"
#include "../../Core/Win32Lean.h"

#include <string>
#include <vector>

namespace Ksword::Features::AuditCommon {

// AuditSummaryItem describes one key-value audit summary row. Inputs are a
// property name, display value, normalized status and optional detail; processing
// is performed by SetAuditSummaryItems; output is rendered as a read-only row.
struct AuditSummaryItem {
    std::wstring key;
    std::wstring value;
    AuditStatus status = AuditStatus::Unknown;
    std::wstring detail;
};

// CreateAuditSummaryPanel creates a compact read-only key-value ListView.
// Inputs are parent/id/bounds; processing creates columns Key/Value/Status/Detail
// using the shared table helper; output is the ListView HWND or nullptr.
HWND CreateAuditSummaryPanel(HWND parent, int id, const RECT& bounds);

// SetAuditSummaryItems replaces the summary panel rows. Inputs are the panel
// HWND and summary items; processing renders one row per item; no value returns.
void SetAuditSummaryItems(HWND panel, const std::vector<AuditSummaryItem>& items);

// BuildAuditSummaryTsv serializes the summary panel. Input is a panel HWND;
// processing delegates to the shared table TSV reader; output is TSV text.
std::wstring BuildAuditSummaryTsv(HWND panel);

// AppendAuditSummaryItem adds one item to a vector with compact call-site
// syntax. Inputs are destination vector and row fields; processing appends a
// value object; no return value is needed.
void AppendAuditSummaryItem(
    std::vector<AuditSummaryItem>& items,
    const std::wstring& key,
    const std::wstring& value,
    AuditStatus status,
    const std::wstring& detail = std::wstring());

} // namespace Ksword::Features::AuditCommon
