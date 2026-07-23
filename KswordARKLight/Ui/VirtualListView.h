#pragma once

#include "../Core/Win32Lean.h"
#include "ListViewUtil.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Ui {

// VirtualListRow holds one immutable display row. The ListView asks for text
// only when it is visible, avoiding the per-row HWND message storm caused by
// eager ListView_InsertItem calls on large snapshots.
struct VirtualListRow {
    std::wstring stableKey;
    std::vector<std::wstring> cells;
    LPARAM itemData = 0;
    int imageIndex = -1;
    COLORREF textColor = CLR_DEFAULT;
    COLORREF backgroundColor = CLR_DEFAULT;
};

// VirtualListView owns a report ListView configured with LVS_OWNERDATA and an
// immutable backing snapshot. The parent forwards LVN_GETDISPINFO and
// NM_CUSTOMDRAW notifications to handleNotify(). Filtering can be calculated
// off-thread with FilterRowIndexes and installed on the UI thread with
// setVisibleIndexes().
class VirtualListView final {
public:
    VirtualListView() = default;
    ~VirtualListView() = default;

    VirtualListView(const VirtualListView&) = delete;
    VirtualListView& operator=(const VirtualListView&) = delete;

    bool create(HWND parent, int id, int x, int y, int width, int height, DWORD extraStyle = 0);
    HWND hwnd() const noexcept;
    bool addColumns(const std::vector<ListViewColumn>& columns);

    void setRows(std::vector<VirtualListRow> rows);
    const std::vector<VirtualListRow>& rows() const noexcept;
    const std::vector<std::size_t>& visibleIndexes() const noexcept;
    void setVisibleIndexes(std::vector<std::size_t> indexes);
    void resetVisibleIndexes();

    // FilterRowIndexes is data-only and can be called from a worker thread.
    // The query is matched case-insensitively against every cell and stable key.
    static std::vector<std::size_t> FilterRowIndexes(const std::vector<VirtualListRow>& rows, const std::wstring& query);

    // handleNotify returns true when the notification was handled.
    bool handleNotify(const NMHDR& header, LRESULT& result);
    std::size_t rowCount() const noexcept;

private:
    const VirtualListRow* rowAtVisibleIndex(int visibleIndex) const noexcept;
    void updateItemCount();

private:
    HWND hwnd_ = nullptr;
    std::vector<VirtualListRow> rows_;
    std::vector<std::size_t> visibleIndexes_;
};

} // namespace Ksword::Ui
