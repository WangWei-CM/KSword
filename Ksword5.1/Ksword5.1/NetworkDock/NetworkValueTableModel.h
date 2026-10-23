#pragma once

#include "../UI/FlatTableModel.h"
#include "../UI/CompleteExport.h"
#include <QIdentityProxyModel>
#include <QItemSelectionModel>
#include <numeric>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace network_value_detail
{
    template<class Value> std::shared_ptr<Value> retainMutable(std::shared_ptr<Value> value)
    {
        const auto retained = ks::ui::RetainCompleteExportSnapshot(std::shared_ptr<const Value>(value));
        return value ? std::shared_ptr<Value>(retained, value.get()) : std::shared_ptr<Value>{};
    }
    // Sorting is part of the immutable worker projection. The permanent identity
    // proxy only forwards Qt's structural/persistent-index protocol; it never
    // performs a second comparison sort while rows are being published.
    class SortProxy final : public QIdentityProxyModel
    {
    public:
        using QIdentityProxyModel::QIdentityProxyModel;
        std::function<void()> request;
        int sortColumn() const { return m_column; }
        Qt::SortOrder sortOrder() const { return m_order; }
        quint64 sortRevision() const { return m_revision; }
        void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override
        {
            if (m_column == column && m_order == order) return;
            m_column = column; m_order = order; ++m_revision;
            if (request) request();
        }
    private:
        int m_column = -1;
        Qt::SortOrder m_order = Qt::AscendingOrder;
        quint64 m_revision = 0;
    };

    template<class Row> class Model final : public QAbstractTableModel
    {
        using Engine = ks::ui::FlatTableModel<Row>;
    public:
        using RowType = Row;
        using ColumnSpec = typename Engine::ColumnSpec;
        using ColumnChange = typename Engine::ColumnChange;
        using DataResolver = typename Engine::DataResolver;
        using FlagsResolver = typename Engine::FlagsResolver;
        using KeyResolver = typename Engine::KeyResolver;
        using RowChangeResolver = typename Engine::RowChangeResolver;
        using Rows = std::vector<Row>;
        using RowsHandle = std::shared_ptr<const Rows>;
    private:
    public:
        struct PreparedData
        {
            typename Engine::PreparedRows delta;
            RowsHandle rows, reordered, inserted;
            std::vector<int> oldTargetRows;
            int kept = 0;
            bool reorderOld = false, changed = false;
            int firstRow = 0, lastRow = 0, firstColumn = 0, lastColumn = 0;
        };
        struct PreparedRows
        {
            quint64 baseRevision = 0;
            std::shared_ptr<const PreparedData> value;
        };
        Model(std::vector<ColumnSpec> columns, DataResolver resolve, QObject* parent,
            FlagsResolver flags = {}, KeyResolver = {}, RowChangeResolver = {})
            : QAbstractTableModel(parent), m_columns(std::move(columns)), m_resolve(std::move(resolve)), m_flags(std::move(flags))
        {
            m_rows = ks::ui::RetainCompleteExportSnapshot(std::make_shared<const Rows>());
            m_current = m_rows;
        }
        quint64 rowsRevision() const { return m_revision; }
        bool canAcceptPreparedRows(quint64 revision) const { return !m_updating && revision == m_revision; }
        const Rows& rows() const { return *m_rows; }
        RowsHandle rowsHandle() const { return m_rows; }
        static PreparedRows prepareRows(const RowsHandle& oldRows, Rows rows, int columns,
            KeyResolver key, RowChangeResolver changes, quint64 revision)
        {
            auto data = std::make_shared<PreparedData>();
            data->delta = Engine::prepareRows(*oldRows, std::move(rows), columns, std::move(key), std::move(changes), revision);
            data->rows = ks::ui::RetainCompleteExportSnapshot(std::make_shared<const Rows>(std::move(data->delta.rows)));
            if (!data->delta.stats.modelReset)
            {
                std::vector<bool> removed(oldRows->size(), false);
                for (const auto& range : data->delta.removals)
                    for (int row = range.first; row <= range.second; ++row) removed[static_cast<std::size_t>(row)] = true;
                std::vector<int> order; order.reserve(oldRows->size());
                for (std::size_t i = 0; i < oldRows->size(); ++i) if (!removed[i]) order.push_back(static_cast<int>(i));
                data->kept = static_cast<int>(order.size());
                for (std::size_t i = 0; i < oldRows->size(); ++i) if (removed[i]) order.push_back(static_cast<int>(i));
                data->oldTargetRows.resize(order.size());
                for (std::size_t i = 0; i < order.size(); ++i)
                {
                    data->oldTargetRows[static_cast<std::size_t>(order[i])] = static_cast<int>(i);
                    if (order[i] != static_cast<int>(i)) data->reorderOld = true;
                }
                data->reordered = oldRows;
                if (data->reorderOld)
                {
                    Rows values; values.reserve(order.size());
                    for (const int row : order) values.push_back((*oldRows)[static_cast<std::size_t>(row)]);
                    data->reordered = ks::ui::RetainCompleteExportSnapshot(std::make_shared<const Rows>(std::move(values)));
                }
                data->inserted = data->reordered;
                if (!data->delta.insertions.empty())
                {
                    if (!data->kept) data->inserted = data->rows;
                    else
                    {
                        Rows values; values.reserve(static_cast<std::size_t>(data->kept) + data->delta.insertions.size());
                        values.insert(values.end(), data->reordered->begin(), data->reordered->begin() + data->kept);
                        for (const auto row : data->delta.insertions) values.push_back((*data->rows)[row]);
                        data->inserted = ks::ui::RetainCompleteExportSnapshot(std::make_shared<const Rows>(std::move(values)));
                    }
                }
                // One conservative rectangle covers all real field/flag changes.
                // Current values remain exact; unchanged cells inside it are safe.
                for (const auto& change : data->delta.changes)
                {
                    if (!data->changed)
                    {
                        data->firstRow = change.firstRow; data->lastRow = change.lastRow;
                        data->firstColumn = change.columns.firstColumn; data->lastColumn = change.columns.lastColumn;
                    }
                    else
                    {
                        data->firstRow = std::min(data->firstRow, change.firstRow); data->lastRow = std::max(data->lastRow, change.lastRow);
                        data->firstColumn = std::min(data->firstColumn, change.columns.firstColumn);
                        data->lastColumn = std::max(data->lastColumn, change.columns.lastColumn);
                    }
                    data->changed = true;
                }
            }
            return {revision, ks::ui::RetainCompleteExportSnapshot(std::shared_ptr<const PreparedData>(std::move(data)))};
        }
        bool setPreparedRows(PreparedRows prepared)
        {
            if (!prepared.value || !canAcceptPreparedRows(prepared.baseRevision)) return false;
            const auto value = std::move(prepared.value);
            const auto& delta = value->delta;
            const QPointer<Model> alive(this);
            m_updating = true; ++m_revision;
            if (delta.stats.modelReset)
            {
                beginResetModel(); if (!alive) return true;
                m_current = m_rows = value->rows; m_count = static_cast<int>(m_rows->size());
                endResetModel(); if (!alive) return true;
            }
            else
            {
                if (value->reorderOld && !publishLayout(value->reordered, rowCount(), value->oldTargetRows, false)) return true;
                if (rowCount() > value->kept)
                {
                    beginRemoveRows({}, value->kept, rowCount() - 1); if (!alive) return true;
                    m_current = value->reordered; m_count = value->kept;
                    endRemoveRows(); if (!alive) return true;
                }
                if (!delta.insertions.empty())
                {
                    const int first = rowCount();
                    beginInsertRows({}, first, first + static_cast<int>(delta.insertions.size()) - 1); if (!alive) return true;
                    m_current = value->inserted; m_count += static_cast<int>(delta.insertions.size());
                    endInsertRows(); if (!alive) return true;
                }
                if (delta.stats.orderChanged)
                {
                    if (!publishLayout(value->rows, static_cast<int>(value->rows->size()), delta.targetRows, true)) return true;
                }
                else { m_current = m_rows = value->rows; m_count = static_cast<int>(m_rows->size()); }
                if (value->changed)
                {
                    emit dataChanged(index(value->firstRow, value->firstColumn), index(value->lastRow, value->lastColumn), {});
                    if (!alive) return true;
                }
            }
            m_updating = false;
            return true;
        }
        const Row* rowAt(int row) const
        { return row >= 0 && row < m_count ? &(*m_current)[static_cast<std::size_t>(row)] : nullptr; }
        int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_count; }
        int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(m_columns.size()); }
        QVariant data(const QModelIndex& index, int role) const override
        {
            const auto* row = rowAt(index.row());
            if (!index.isValid() || !row || index.column() < 0 || index.column() >= columnCount()) return {};
            if (role == Qt::TextAlignmentRole) return static_cast<int>(m_columns[static_cast<std::size_t>(index.column())].alignment);
            const auto result = m_resolve ? m_resolve(*row, index.column(), role) : QVariant{};
            return (role == Qt::DisplayRole || role == Qt::ToolTipRole) && result.metaType().id() == QMetaType::QString
                ? ks::i18n::sourceText(result.toString()) : result;
        }
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override
        {
            return orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < columnCount()
                ? QVariant(ks::i18n::displayText(m_columns[static_cast<std::size_t>(section)].headerText)) : QVariant{};
        }
        Qt::ItemFlags flags(const QModelIndex& index) const override
        {
            const auto* row = rowAt(index.row());
            return index.isValid() && row ? (m_flags ? m_flags(*row, index.column()) : Qt::ItemIsEnabled | Qt::ItemIsSelectable) : Qt::NoItemFlags;
        }
    private:
        std::vector<ColumnSpec> m_columns;
        DataResolver m_resolve;
        FlagsResolver m_flags;
        RowsHandle m_rows, m_current;
        int m_count = 0;
        bool publishLayout(RowsHandle rows, int count, const std::vector<int>& mapping, bool final)
        {
            const QPointer<Model> alive(this);
            emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint); if (!alive) return false;
            const auto before = persistentIndexList();
            QModelIndexList after; after.reserve(before.size());
            for (const auto& item : before)
                after.push_back(item.isValid() && item.row() >= 0 && item.row() < static_cast<int>(mapping.size())
                    ? createIndex(mapping[static_cast<std::size_t>(item.row())], item.column()) : QModelIndex());
            m_current = std::move(rows); m_count = count;
            if (final) m_rows = m_current;
            changePersistentIndexList(before, after);
            emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
            return !alive.isNull();
        }
        quint64 m_revision = 0;
        bool m_updating = false;
    };

    template<class Rows, class Text> void sortRows(Rows& rows, int column, Qt::SortOrder order,
        const ks::i18n::SourceTextSnapshot& language, Text text, const Rows* previous = nullptr,
        typename ks::ui::FlatTableModel<typename Rows::value_type>::KeyResolver key = {})
    {
        if (column < 0) return;
        // Qt's stable proxy sort retains the previous displayed tie order when
        // switching columns. New rows follow retained rows. Invalid/duplicate
        // keys use the original reset path and canonical tie order instead.
        std::unordered_map<std::string, std::size_t> rank;
        bool stableTies = previous && static_cast<bool>(key);
        if (stableTies)
        {
            rank.reserve(previous->size());
            for (std::size_t i = 0; i < previous->size(); ++i)
            {
                const auto identity = key((*previous)[i]);
                if (identity.empty() || !rank.emplace(identity, i).second) { stableTies = false; break; }
            }
            std::unordered_set<std::string> seen;
            if (stableTies) for (const auto& row : rows)
            {
                const auto identity = key(row);
                if (identity.empty() || !seen.insert(identity).second) { stableTies = false; break; }
            }
        }
        std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
            const auto left = language.sourceText(text(a, column));
            const auto right = language.sourceText(text(b, column));
            const int compare = QString::localeAwareCompare(left, right);
            if (compare != 0) return order == Qt::AscendingOrder ? compare < 0 : compare > 0;
            if (!stableTies) return false;
            const auto leftRank = rank.find(key(a)), rightRank = rank.find(key(b));
            const auto missing = previous->size();
            return (leftRank == rank.end() ? missing : leftRank->second)
                < (rightRank == rank.end() ? missing : rightRank->second);
        });
    }

    // selectedRows() asks Qt to check every column of every selected row. These
    // views select whole rows, so selection ranges already carry that fact.
    inline std::vector<std::pair<int, int>> selectedRanges(QItemSelectionModel* selection)
    {
        std::vector<std::pair<int, int>> ranges;
        if (!selection) return ranges;
        for (const auto& range : selection->selection()) ranges.emplace_back(range.top(), range.bottom());
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<int, int>> result;
        for (const auto& range : ranges)
            if (!result.empty() && range.first <= result.back().second + 1) result.back().second = std::max(result.back().second, range.second);
            else result.push_back(range);
        return result;
    }
    inline QModelIndexList selectedRows(QItemSelectionModel* selection,
        int limit = std::numeric_limits<int>::max(), const std::function<bool(int)>& include = {})
    {
        QModelIndexList rows;
        if (!selection || !selection->model()) return rows;
        for (const auto& range : selectedRanges(selection))
            for (int row = range.first; row <= range.second; ++row)
                if (!include || include(row))
                {
                    rows.push_back(selection->model()->index(row, 0));
                    if (rows.size() >= limit) return rows;
                }
        return rows;
    }
}
