#pragma once
#include "../UI/ImmutableValueTableModel.h"

namespace ks::window
{
    // Worker-only construction: complete canonical records/search documents; no Qt GUI objects.
    inline ks::ui::ImmutableTableSource makeWindowTableSource(QVector<QStringList> rows,
        const QVector<int>& identityColumns)
    {
        QVector<ks::ui::ImmutableTableRow> values;
        values.reserve(rows.size());
        QHash<QString, int> occurrences;
        for (auto& cells : rows)
        {
            ks::ui::ImmutableTableRow row;
            QString identity;
            for (const int column : identityColumns)
            {
                const auto value = cells.value(column);
                identity += QString::number(value.size()) + QLatin1Char(':') + value;
            }
            // Preserve repeated diagnostic/partial rows too; never trim canonical records.
            row.key = identity + QLatin1Char('#') + QString::number(occurrences[identity]++);
            row.searchText = cells.join(QLatin1Char(' '));
            row.cells = std::move(cells);
            values.push_back(std::move(row));
        }
        return ks::ui::MakeImmutableTableSource(std::move(values));
    }

    template<typename Translator>
    inline ks::ui::ImmutableTableSource formatWindowTableSource(ks::ui::ImmutableTableSource source,
        const Translator& translator)
    {
        if (!source) return {};
        auto formatted = std::make_shared<ks::ui::ImmutableTableValues>();
        formatted->byKey = source->byKey;
        formatted->rows.reserve(source->rows.size());
        for (const auto& original : source->rows)
        {
            auto row = original;
            for (auto& cell : row.cells) cell = translator.displayText(cell);
            row.searchText = row.cells.join(QLatin1Char(' '));
            formatted->rows.push_back(std::move(row));
        }
        return formatted;
    }
}
