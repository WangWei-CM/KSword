#include "VirtualListView.h"

#include <algorithm>
#include <cwctype>

namespace Ksword::Ui {
namespace {

bool ContainsCaseInsensitive(const std::wstring& value, const std::wstring& query) {
    if (query.empty()) {
        return true;
    }
    const auto iterator = std::search(value.begin(), value.end(), query.begin(), query.end(), [](const wchar_t left, const wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    });
    return iterator != value.end();
}

} // namespace

bool VirtualListView::create(HWND parent, const int id, const int x, const int y, const int width, const int height, const DWORD extraStyle) {
    hwnd_ = CreateReportListView(parent, id, x, y, width, height, extraStyle | LVS_OWNERDATA);
    return hwnd_ != nullptr;
}

HWND VirtualListView::hwnd() const noexcept {
    return hwnd_;
}

void VirtualListView::detach() noexcept {
    hwnd_ = nullptr;
}

bool VirtualListView::addColumns(const std::vector<ListViewColumn>& columns) {
    return AddListViewColumns(hwnd_, columns);
}

void VirtualListView::setRows(std::vector<VirtualListRow> rows) {
    rows_ = std::make_shared<const std::vector<VirtualListRow>>(std::move(rows));
    resetVisibleIndexes();
}

void VirtualListView::setSharedRows(std::shared_ptr<const std::vector<VirtualListRow>> rows) {
    rows_ = std::move(rows);
    resetVisibleIndexes();
}

const std::vector<VirtualListRow>& VirtualListView::rows() const noexcept {
    static const std::vector<VirtualListRow> empty;
    return rows_ ? *rows_ : empty;
}

const std::vector<std::size_t>& VirtualListView::visibleIndexes() const noexcept {
    return visibleIndexes_;
}

void VirtualListView::setVisibleIndexes(std::vector<std::size_t> indexes) {
    const std::size_t rowCount = rows_ ? rows_->size() : 0;
    indexes.erase(std::remove_if(indexes.begin(), indexes.end(), [rowCount](const std::size_t index) { return index >= rowCount; }), indexes.end());
    visibleIndexes_ = std::move(indexes);
    updateItemCount();
}

void VirtualListView::resetVisibleIndexes() {
    visibleIndexes_.resize(rows_ ? rows_->size() : 0);
    for (std::size_t index = 0; index < visibleIndexes_.size(); ++index) {
        visibleIndexes_[index] = index;
    }
    updateItemCount();
}

std::vector<std::size_t> VirtualListView::FilterRowIndexes(const std::vector<VirtualListRow>& rows, const std::wstring& query) {
    std::vector<std::size_t> indexes;
    indexes.reserve(rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const VirtualListRow& row = rows[index];
        bool matched = ContainsCaseInsensitive(row.stableKey, query);
        for (const std::wstring& cell : row.cells) {
            matched = matched || ContainsCaseInsensitive(cell, query);
        }
        if (matched) {
            indexes.push_back(index);
        }
    }
    return indexes;
}

bool VirtualListView::handleNotify(const NMHDR& header, LRESULT& result) {
    if (!hwnd_ || header.hwndFrom != hwnd_) {
        return false;
    }
    if (header.code == LVN_GETDISPINFOW) {
        auto* displayInfo = reinterpret_cast<NMLVDISPINFOW*>(const_cast<NMHDR*>(&header));
        const VirtualListRow* row = rowAtVisibleIndex(displayInfo->item.iItem);
        if (!row) {
            return true;
        }
        if ((displayInfo->item.mask & LVIF_TEXT) != 0) {
            const int column = displayInfo->item.iSubItem;
            const std::wstring empty;
            const std::wstring& text = column >= 0 && static_cast<std::size_t>(column) < row->cells.size() ? row->cells[static_cast<std::size_t>(column)] : empty;
            displayInfo->item.pszText = const_cast<wchar_t*>(text.c_str());
        }
        if ((displayInfo->item.mask & LVIF_PARAM) != 0) {
            displayInfo->item.lParam = row->itemData;
        }
        if ((displayInfo->item.mask & LVIF_IMAGE) != 0 && row->imageIndex >= 0) {
            displayInfo->item.iImage = row->imageIndex;
        }
        result = 0;
        return true;
    }
    if (header.code == NM_CUSTOMDRAW) {
        auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(&header));
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
            result = CDRF_NOTIFYITEMDRAW;
            return true;
        }
        if ((draw->nmcd.dwDrawStage & CDDS_ITEMPREPAINT) != 0) {
            const VirtualListRow* row = rowAtVisibleIndex(static_cast<int>(draw->nmcd.dwItemSpec));
            if (row) {
                if (row->textColor != CLR_DEFAULT) {
                    draw->clrText = row->textColor;
                }
                if (row->backgroundColor != CLR_DEFAULT) {
                    draw->clrTextBk = row->backgroundColor;
                }
            }
            result = CDRF_NEWFONT;
            return true;
        }
    }
    return false;
}

std::size_t VirtualListView::rowCount() const noexcept {
    return visibleIndexes_.size();
}

const VirtualListRow* VirtualListView::rowAtVisibleIndex(const int visibleIndex) const noexcept {
    if (visibleIndex < 0 || static_cast<std::size_t>(visibleIndex) >= visibleIndexes_.size()) {
        return nullptr;
    }
    const std::size_t rowIndex = visibleIndexes_[static_cast<std::size_t>(visibleIndex)];
    return rows_ && rowIndex < rows_->size() ? &(*rows_)[rowIndex] : nullptr;
}

void VirtualListView::updateItemCount() {
    if (!hwnd_) {
        return;
    }
    const int topIndex = ListView_GetTopIndex(hwnd_);
    ListView_SetItemCountEx(hwnd_, static_cast<int>(visibleIndexes_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (topIndex >= 0 && static_cast<std::size_t>(topIndex) < visibleIndexes_.size()) {
        ListView_EnsureVisible(hwnd_, topIndex, FALSE);
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

} // namespace Ksword::Ui
