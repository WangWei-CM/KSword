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
        // Persistent range tree. Deleting a range splits only its boundary paths;
        // leaves borrow immutable row arrays. No Row copy/erase/shift runs in a
        // begin/end pair. Final publication returns to one contiguous leaf.
        struct Node;
        using Root = std::shared_ptr<const Node>;
        struct Node
        {
            Root left, right;
            RowsHandle rows;
            int offset = 0, count = 0, height = 1;
        };
        static Root leaf(RowsHandle rows, int offset, int count)
        { return count ? std::make_shared<const Node>(Node{{}, {}, std::move(rows), offset, count}) : Root{}; }
        static int size(const Root& root) { return root ? root->count : 0; }
        static int height(const Root& root) { return root ? root->height : 0; }
        static Root branch(Root left, Root right)
        {
            const int count = size(left) + size(right), depth = std::max(height(left), height(right)) + 1;
            return std::make_shared<const Node>(Node{std::move(left), std::move(right), {}, 0, count, depth});
        }
        static Root balanced(Root left, Root right)
        {
            if (height(left) > height(right) + 1)
            {
                if (height(left->left) >= height(left->right)) return branch(left->left, branch(left->right, right));
                return branch(branch(left->left, left->right->left), branch(left->right->right, right));
            }
            if (height(right) > height(left) + 1)
            {
                if (height(right->right) >= height(right->left)) return branch(branch(left, right->left), right->right);
                return branch(branch(left, right->left->left), branch(right->left->right, right->right));
            }
            return branch(std::move(left), std::move(right));
        }
        static Root join(Root left, Root right)
        {
            if (!left) return right;
            if (!right) return left;
            if (height(left) > height(right) + 1) return balanced(left->left, join(left->right, std::move(right)));
            if (height(right) > height(left) + 1) return balanced(join(std::move(left), right->left), right->right);
            return branch(std::move(left), std::move(right));
        }
        static std::pair<Root, Root> split(const Root& root, int row)
        {
            if (!root || row <= 0) return {{}, root};
            if (row >= size(root)) return {root, {}};
            if (root->rows) return {leaf(root->rows, root->offset, row),
                leaf(root->rows, root->offset + row, root->count - row)};
            const int leftCount = size(root->left);
            if (row < leftCount)
            { auto pair = split(root->left, row); return {pair.first, join(pair.second, root->right)}; }
            auto pair = split(root->right, row - leftCount);
            return {join(root->left, pair.first), pair.second};
        }
        static Root retained(Root root) { return ks::ui::RetainCompleteExportSnapshot(root); }
    public:
        struct PreparedData
        {
            typename Engine::PreparedRows delta;
            RowsHandle rows;
            std::vector<Root> removalRoots;
            Root insertedRoot, finalRoot;
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
            Root root = leaf(oldRows, 0, static_cast<int>(oldRows->size()));
            for (const auto& range : data->delta.removals)
            {
                auto after = split(root, range.second + 1);
                auto before = split(after.first, range.first);
                root = join(before.first, after.second);
                data->removalRoots.push_back(retained(root));
            }
            // Inserted values form runs into the final array; build a balanced
            // tree on the worker rather than push_back/copying rows on GUI.
            std::vector<Root> runs;
            for (std::size_t i = 0; i < data->delta.insertions.size();)
            {
                const auto first = data->delta.insertions[i]; std::size_t end = i + 1;
                while (end < data->delta.insertions.size() && data->delta.insertions[end] == first + end - i) ++end;
                runs.push_back(leaf(data->rows, static_cast<int>(first), static_cast<int>(end - i))); i = end;
            }
            while (runs.size() > 1)
            {
                std::vector<Root> next;
                for (std::size_t i = 0; i < runs.size(); i += 2)
                    next.push_back(i + 1 < runs.size() ? join(runs[i], runs[i + 1]) : runs[i]);
                runs = std::move(next);
            }
            data->insertedRoot = retained(join(root, runs.empty() ? Root{} : runs.front()));
            data->finalRoot = retained(leaf(data->rows, 0, static_cast<int>(data->rows->size())));
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
                // Preserve only the pre-existing invalid/duplicate-key fallback.
                beginResetModel(); if (!alive) return true;
                m_root = value->finalRoot; m_rows = value->rows;
                endResetModel(); if (!alive) return true;
            }
            else
            {
                for (std::size_t i = 0; i < delta.removals.size(); ++i)
                {
                    const auto& range = delta.removals[i];
                    beginRemoveRows({}, range.first, range.second); if (!alive) return true;
                    m_root = value->removalRoots[i];
                    endRemoveRows(); if (!alive) return true;
                }
                if (!delta.insertions.empty())
                {
                    const int first = rowCount();
                    beginInsertRows({}, first, first + static_cast<int>(delta.insertions.size()) - 1); if (!alive) return true;
                    m_root = value->insertedRoot;
                    endInsertRows(); if (!alive) return true;
                }
                if (delta.stats.orderChanged)
                {
                    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint); if (!alive) return true;
                    const auto before = persistentIndexList();
                    QModelIndexList after; after.reserve(before.size());
                    for (const auto& item : before)
                        after.push_back(item.isValid() && item.row() >= 0 && item.row() < static_cast<int>(delta.targetRows.size())
                            ? createIndex(delta.targetRows[static_cast<std::size_t>(item.row())], item.column()) : QModelIndex());
                    m_root = value->finalRoot; m_rows = value->rows;
                    changePersistentIndexList(before, after);
                    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint); if (!alive) return true;
                }
                else { m_root = value->finalRoot; m_rows = value->rows; }
                for (const auto& change : delta.changes)
                {
                    emit dataChanged(index(change.firstRow, change.columns.firstColumn),
                        index(change.lastRow, change.columns.lastColumn), change.columns.roles);
                    if (!alive) return true;
                }
            }
            m_updating = false;
            return true;
        }
        const Row* rowAt(int row) const
        {
            auto* node = m_root.get();
            if (row < 0 || row >= size(m_root)) return nullptr;
            while (node && !node->rows)
            {
                const int count = size(node->left);
                if (row < count) node = node->left.get();
                else { row -= count; node = node->right.get(); }
            }
            return node ? &(*node->rows)[static_cast<std::size_t>(node->offset + row)] : nullptr;
        }
        int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : size(m_root); }
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
        RowsHandle m_rows;
        Root m_root;
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
