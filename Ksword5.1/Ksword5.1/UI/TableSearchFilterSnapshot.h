#pragma once

#include <QStringList>
#include <QVector>

namespace ks::ui
{
    // Immutable value of the generic "only show search results" filter. Column
    // numbers address the producer's complete displayed-cell vector. A popup
    // highlight alone does not activate this result filter.
    struct TableSearchResultFilterSpec
    {
        bool active = false;
        QString query;
        QVector<int> columns;
    };

    template<class ReadCell>
    bool MatchTableSearchResultFilterCells(const QString& query,
        const QVector<int>& columns, ReadCell&& readCell)
    {
        for (int column : columns)
            if (column >= 0 && readCell(column).contains(query, Qt::CaseInsensitive)) return true;
        return false;
    }

    // Worker-safe: reads only captured values, never a widget or Qt model.
    inline bool TableSearchResultFilterMatches(const TableSearchResultFilterSpec& spec,
        const QStringList& displayedCells)
    {
        if (!spec.active) return true;
        return MatchTableSearchResultFilterCells(spec.query, spec.columns,
            [&displayedCells](int column) {
                return column < displayedCells.size() ? displayedCells.at(column) : QString();
            });
    }
}
