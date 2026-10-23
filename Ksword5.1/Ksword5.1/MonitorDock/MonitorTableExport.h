#pragma once
#include "MonitorCanonicalExport.h"
#include "MonitorEventFilterProjection.h"
#include "../UI/TableSearchSupport.h"
#include "UI/TableSearchFilterSnapshot.h"
namespace ks::monitor
{
    struct MonitorCachedTableExportSnapshot
    {
        std::vector<std::shared_ptr<const EventFilterRow>> rows;
        std::shared_ptr<const CompiledEventFilter> businessFilter;
        ks::ui::TableSearchResultFilterSpec search;
        ks::i18n::SourceTextSnapshot language;
        QStringList headers;
        std::uint64_t archiveRevision = 0;
        bool wmi = false;
        auto orderedRows() const { return rows; } // Frozen actual table identity order.
        QStringList displayedCells(const std::shared_ptr<const EventFilterRow>& row) const
        {
            auto cells = row->exportCells;
            for (auto& cell : cells) cell = language.displayText(cell);
            return cells;
        }
        bool matches(const std::shared_ptr<const EventFilterRow>& row) const
        {
            const auto cells = displayedCells(row);
            auto filterRow = *row;
            if (wmi) filterRow.fields = cells;
            else if (filterRow.fields.size() > 2) filterRow.fields[2] = cells.value(8);
            return (!businessFilter || businessFilter->matches(filterRow))
                && ks::ui::TableSearchResultFilterMatches(search, cells);
        }
    };
}
