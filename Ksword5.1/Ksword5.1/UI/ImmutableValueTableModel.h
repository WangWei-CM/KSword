#pragma once
#include <QAbstractTableModel>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace ks::ui
{
    struct ImmutableTableRow
    {
        QString key;
        QStringList cells;
        QHash<int, QVariant> roles;
        bool rolesInAllColumns = false;
    };
    struct ImmutableTableValues
    {
        QVector<ImmutableTableRow> rows;
        QHash<QString, int> byKey;
    };
    using ImmutableTableSource = std::shared_ptr<const ImmutableTableValues>;

    inline ImmutableTableSource MakeImmutableTableSource(QVector<ImmutableTableRow> rows)
    {
        auto source = std::make_shared<ImmutableTableValues>();
        source->rows = std::move(rows);
        for (int row = 0; row < source->rows.size(); ++row)
        {
            const auto& key = source->rows[row].key;
            if (key.isEmpty() || source->byKey.contains(key)) return {};
            source->byKey.insert(key, row);
        }
        return source;
    }

    struct ImmutableTableProjection
    {
        ImmutableTableSource source;
        QVector<int> rows;
        int currentRow = -1;
    };
    using ImmutableTableViewValues = std::shared_ptr<const ImmutableTableProjection>;

    // Pure worker preparation; canonical rows/roles are never trimmed by presentation filters.
    inline ImmutableTableViewValues ProjectImmutableTable(ImmutableTableSource source, const QString& query,
        int sortColumn, Qt::SortOrder sortOrder, const QString& currentKey, const std::function<bool()>& cancelled)
    {
        auto projection = std::make_shared<ImmutableTableProjection>();
        projection->source = std::move(source);
        if (!projection->source) return projection;
        for (int row = 0; row < projection->source->rows.size(); ++row)
        {
            if ((row & 127) == 0 && cancelled()) return {};
            const auto& value = projection->source->rows[row];
            bool matches = query.isEmpty();
            for (const auto& cell : value.cells)
                if (matches || cell.contains(query, Qt::CaseInsensitive)) { matches = true; break; }
            if (matches) projection->rows.push_back(row);
        }
        if (sortColumn >= 0)
        {
            std::stable_sort(projection->rows.begin(), projection->rows.end(), [&](int a, int b) {
                const int result = QString::localeAwareCompare(projection->source->rows[a].cells.value(sortColumn),
                    projection->source->rows[b].cells.value(sortColumn));
                return sortOrder == Qt::AscendingOrder ? result < 0 : result > 0;
            });
            if (cancelled()) return {};
        }
        for (int row = 0; row < projection->rows.size(); ++row)
            if (projection->source->rows[projection->rows[row]].key == currentKey) { projection->currentRow = row; break; }
        return projection;
    }

    class ImmutableValueTableModel final : public QAbstractTableModel
    {
    public:
        explicit ImmutableValueTableModel(int columns, std::function<QString(int)> header, QObject* parent = nullptr)
            : QAbstractTableModel(parent), m_columns(columns), m_header(std::move(header)) {}
        // Owner retires the returned previous snapshot off-thread.
        ImmutableTableViewValues replace(ImmutableTableViewValues projection)
        {
            const QPointer<ImmutableValueTableModel> alive(this);
            beginResetModel();
            if (!alive) return projection;
            auto previous = std::move(m_projection);
            m_projection = std::move(projection);
            endResetModel();
            return previous;
        }
        ImmutableTableViewValues take() { return std::move(m_projection); }
        const ImmutableTableRow* rowAt(int row) const
        {
            if (!m_projection || !m_projection->source || row < 0 || row >= m_projection->rows.size()) return nullptr;
            const int sourceRow = m_projection->rows[row];
            return sourceRow >= 0 && sourceRow < m_projection->source->rows.size() ? &m_projection->source->rows[sourceRow] : nullptr;
        }
        int rowCount(const QModelIndex& parent = {}) const override
        { return parent.isValid() || !m_projection ? 0 : static_cast<int>(m_projection->rows.size()); }
        int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_columns; }
        QVariant data(const QModelIndex& index, int role) const override
        {
            const auto* row = rowAt(index.row());
            if (!index.isValid() || !row || index.column() < 0 || index.column() >= m_columns) return {};
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return row->cells.value(index.column());
            return index.column() == 0 || row->rolesInAllColumns ? row->roles.value(role) : QVariant{};
        }
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override
        { return orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < m_columns ? m_header(section) : QVariant{}; }
        void sort(int column, Qt::SortOrder order) override { if (sortRequested) sortRequested(column, order); }
        void languageChanged() { if (m_columns) emit headerDataChanged(Qt::Horizontal, 0, m_columns - 1); }
        std::function<void(int, Qt::SortOrder)> sortRequested;
    private:
        int m_columns;
        std::function<QString(int)> m_header;
        ImmutableTableViewValues m_projection;
    };
}
