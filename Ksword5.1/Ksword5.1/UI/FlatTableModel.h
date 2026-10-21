#pragma once

#include <QAbstractTableModel>
#include <QMetaType>
#include <QList>
#include <QPointer>
#include <optional>
#include <QString>
#include <QVariant>

#include "../Internationalization/LanguageManager.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ks::ui
{
    // FlatTableModel 作用：
    // - 以“行对象数组”为唯一数据源，为只读 QTableView 提供轻量模型；
    // - 适合日志、搜索结果、快照列表等没有层级关系的表格；
    // - 目标是替代 QTableWidget/QTreeWidget 的大量 item 对象开销。
    template<typename RowT>
    class FlatTableModel final : public QAbstractTableModel
    {
    public:
        using RowType = RowT;

        // DataResolver 作用：
        // - 由外部按“行 + 列 + role”统一返回单元格数据；
        // - 允许调用方在一个函数里处理 Display / ToolTip / Background 等角色；
        // - 这比每个单元格维护独立 item 对象更轻量。
        using DataResolver = std::function<QVariant(const RowType& row, int column, int role)>;

        // FlagsResolver 作用：
        // - 允许调用方按行/列动态控制 Qt item flags；
        // - 默认保持只读可选，兼容既有表格；
        // - 进程友好视图会用它让分类标题和应用聚合行不可选，避免误触发进程动作。
        using FlagsResolver = std::function<Qt::ItemFlags(const RowType& row, int column)>;

        // KeyResolver 作用：
        // - 为支持增量更新的长表返回稳定且唯一的行键；
        // - 未提供时保持原有 reset 模式，适配日志这类没有稳定 identity 的快照。
        using KeyResolver = std::function<std::string(const RowType& row)>;

        // 可选精确差异协议。空变更列表表示数据/flags 未变；单个区间的空 roles
        // 沿用 Qt 语义，表示该列区间的所有角色（包括 flags 等未单列角色）。
        struct ColumnChange
        {
            int firstColumn = 0;
            int lastColumn = 0;
            QList<int> roles;
        };
        using RowChangeResolver = std::function<std::vector<ColumnChange>(
            const RowType& oldRow, const RowType& newRow)>;

        struct UpdateStats
        {
            int insertedRowCount = 0;
            int removedRowCount = 0;
            // 精确协议只计真实变化的保留行；兼容模式/reset 沿用旧口径（全部新行）。
            int updatedRowCount = 0;
            bool orderChanged = false;
            bool modelReset = false;
        };

        // ColumnSpec 作用：
        // - 保存横向表头文本和默认对齐方式；
        // - 真正的数据由 DataResolver 提供。
        struct ColumnSpec
        {
            QString headerText;
            Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter;
        };

        // 构造函数作用：
        // - 接收固定列定义与数据解析回调；
        // - 返回：无，模型对象直接交给 QTableView 绑定。
        explicit FlatTableModel(
            std::vector<ColumnSpec> columns,
            DataResolver resolver,
            QObject* parent = nullptr,
            FlagsResolver flagsResolver = FlagsResolver(),
            KeyResolver keyResolver = KeyResolver(),
            RowChangeResolver rowChangeResolver = RowChangeResolver())
            : QAbstractTableModel(parent)
            , m_columns(std::move(columns))
            , m_dataResolver(std::move(resolver))
            , m_flagsResolver(std::move(flagsResolver))
            , m_keyResolver(std::move(keyResolver))
            , m_rowChangeResolver(std::move(rowChangeResolver))
        {
        }

        // setRows 作用：
        // - 用一批新行替换当前快照；
        // - 有稳定键时按删除/插入/layout/dataChanged 增量发布；
        // - 没有稳定键或键重复时回退 beginResetModel/endResetModel。
        // 参数 rows：新的可见行集合。
        UpdateStats setRows(std::vector<RowType> rows)
        {
            return submitRows(std::move(rows), false);
        }

        // 仅影响后续具有稳定键的 setRows；不替换数据，也不主动发通知。
        // resolver 必须覆盖 DataResolver/FlagsResolver 的全部行内依赖；外部状态变化
        // 应另行失效，或暂时清除此 resolver 使用兼容的全表通知。无键/重复键仍 reset。
        void setRowChangeResolver(RowChangeResolver resolver)
        {
            m_rowChangeResolver = std::move(resolver);
        }

        // clearRows 作用：
        // - 清空当前快照；
        // - 供“清空日志/清空结果”这类场景直接调用。
        void clearRows()
        {
            if (m_rows.empty() && !m_updatingRows) return;
            submitRows({}, true);
        }

        // rowAt 作用：
        // - 按行号读取当前快照中的一行；
        // - 越界时返回 nullptr，方便调用方安全判空。
        const RowType* rowAt(const int row) const
        {
            if (row < 0 || row >= static_cast<int>(m_rows.size()))
            {
                return nullptr;
            }
            return &m_rows[static_cast<std::size_t>(row)];
        }

        // rows 作用：返回当前快照的只读引用，供导出/复制逻辑复用。
        const std::vector<RowType>& rows() const
        {
            return m_rows;
        }

        // rowCount 作用：返回当前快照行数；父索引有效时不展开子节点。
        int rowCount(const QModelIndex& parent = QModelIndex()) const override
        {
            return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
        }

        // columnCount 作用：返回固定列数；父索引有效时不展开子节点。
        int columnCount(const QModelIndex& parent = QModelIndex()) const override
        {
            return parent.isValid() ? 0 : static_cast<int>(m_columns.size());
        }

        // data 作用：
        // - 按 role 向视图返回单元格数据；
        // - 对齐角色由列定义统一控制，其余角色交给 DataResolver。
        QVariant data(const QModelIndex& index, const int role) const override
        {
            if (!index.isValid())
            {
                return {};
            }

            const int row = index.row();
            const int column = index.column();
            if (row < 0 || column < 0 ||
                row >= static_cast<int>(m_rows.size()) ||
                column >= static_cast<int>(m_columns.size()))
            {
                return {};
            }

            if (role == Qt::TextAlignmentRole)
            {
                return static_cast<int>(m_columns[static_cast<std::size_t>(column)].alignment);
            }

            if (!m_dataResolver)
            {
                return {};
            }

            const QVariant resolvedData = m_dataResolver(
                m_rows[static_cast<std::size_t>(row)],
                column,
                role);
            if ((role == Qt::DisplayRole || role == Qt::ToolTipRole)
                && resolvedData.metaType().id() == QMetaType::QString)
            {
                return ks::i18n::sourceText(resolvedData.toString());
            }
            return resolvedData;
        }

        // headerData 作用：
        // - 仅为横向表头提供标题文本；
        // - 纵向表头和其它 role 统一返回空值。
        QVariant headerData(const int section, const Qt::Orientation orientation, const int role) const override
        {
            if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            {
                return {};
            }

            if (section < 0 || section >= static_cast<int>(m_columns.size()))
            {
                return {};
            }

            return ks::i18n::displayText(
                m_columns[static_cast<std::size_t>(section)].headerText);
        }

        // flags 作用：
        // - 保持表格只读；
        // - 允许视图进行选择与上下文菜单定位。
        Qt::ItemFlags flags(const QModelIndex& index) const override
        {
            if (!index.isValid())
            {
                return Qt::NoItemFlags;
            }

            const int row = index.row();
            const int column = index.column();
            if (row < 0 || column < 0 ||
                row >= static_cast<int>(m_rows.size()) ||
                column >= static_cast<int>(m_columns.size()))
            {
                return Qt::NoItemFlags;
            }

            if (m_flagsResolver)
            {
                return m_flagsResolver(m_rows[static_cast<std::size_t>(row)], column);
            }

            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        }

    private:
        std::vector<ColumnSpec> m_columns;  // m_columns：固定横向列定义。
        DataResolver m_dataResolver;        // m_dataResolver：单元格角色解析器。
        FlagsResolver m_flagsResolver;      // m_flagsResolver：可选行 flags 解析器，未设置时保持默认可选。
        KeyResolver m_keyResolver;          // m_keyResolver：可选稳定行键，启用按键增量更新。
        RowChangeResolver m_rowChangeResolver; // 可选精确差异；缺省保持全部行/列/roles 通知。
        std::vector<RowType> m_rows;        // m_rows：当前可见行快照。
        // Qt structural notification pairs cannot nest. A synchronous observer
        // may replace the complete UI snapshot; retain only its latest value,
        // finish the current pair and drain it before the outer call returns.
        bool m_updatingRows = false;
        quint64 m_updateGeneration = 0;
        std::optional<std::vector<RowType>> m_pendingRows;
        bool m_pendingClear = false;

        UpdateStats submitRows(std::vector<RowType> rows, bool clear)
        {
            const QPointer<FlatTableModel> alive(this);
            ++m_updateGeneration;
            if (m_updatingRows)
            {
                m_pendingRows = std::move(rows);
                m_pendingClear = clear;
                return {}; // accepted replacement; no nested Qt publication
            }
            m_updatingRows = true;
            UpdateStats firstStats;
            bool first = true;
            for (;;)
            {
                const quint64 generation = m_updateGeneration;
                const auto stats = clear || !m_keyResolver
                    ? resetRows(std::move(rows)) : setRowsIncrementally(std::move(rows), generation);
                if (first) { firstStats = stats; first = false; }
                if (!alive) return firstStats;
                if (!m_pendingRows)
                { m_updatingRows = false; return firstStats; }
                rows = std::move(*m_pendingRows);
                m_pendingRows.reset();
                clear = m_pendingClear;
            }
        }


        UpdateStats resetRows(std::vector<RowType> rows)
        {
            UpdateStats stats;
            stats.updatedRowCount = static_cast<int>(rows.size());
            stats.modelReset = true;
            const QPointer<FlatTableModel> alive(this);
            beginResetModel();
            if (!alive) return stats;
            m_rows = std::move(rows);
            endResetModel();
            return stats;
        }

        bool collectUniqueKeys(
            const std::vector<RowType>& rows,
            std::vector<std::string>& keyList,
            std::unordered_set<std::string>& keySet, const KeyResolver& resolver,
            quint64 generation) const
        {
            const QPointer<const FlatTableModel> alive(this);
            keyList.clear();
            keySet.clear();
            keyList.reserve(rows.size());
            keySet.reserve(rows.size());
            for (const RowType& row : rows)
            {
                std::string key = resolver(row);
                if (!alive || m_updateGeneration != generation) return false;
                if (key.empty() || !keySet.insert(key).second)
                {
                    return false;
                }
                keyList.push_back(std::move(key));
            }
            return true;
        }

        struct DataChangeRectangle
        {
            int firstRow = 0;
            int lastRow = 0;
            ColumnChange columns;
        };

        // 将重叠区间规范为互不相交的列段；roles 排序去重后才能合并相邻行。
        std::vector<ColumnChange> normalizeChanges(std::vector<ColumnChange> changes) const
        {
            const int columnTotal = static_cast<int>(m_columns.size());
            if (columnTotal == 0 || changes.empty()) return {};
            std::vector<bool> changed(static_cast<std::size_t>(columnTotal), false);
            std::vector<bool> allRoles(static_cast<std::size_t>(columnTotal), false);
            std::vector<QList<int>> rolesByColumn(static_cast<std::size_t>(columnTotal));
            for (const ColumnChange& change : changes)
            {
                const bool valid = change.firstColumn >= 0 && change.lastColumn >= change.firstColumn
                    && change.lastColumn < columnTotal
                    && std::all_of(change.roles.cbegin(), change.roles.cend(), [](int role) { return role >= 0; });
                if (!valid)
                {
                    Q_ASSERT_X(valid, "FlatTableModel::normalizeChanges", "invalid column/role delta");
                    // 发布构建保守失效整行，不能因调用者区间错误漏掉显示或 flags 变化。
                    return { { 0, columnTotal - 1, {} } };
                }
                for (int column = change.firstColumn; column <= change.lastColumn; ++column)
                {
                    const std::size_t slot = static_cast<std::size_t>(column);
                    changed[slot] = true;
                    if (change.roles.empty())
                    {
                        allRoles[slot] = true;
                        rolesByColumn[slot].clear();
                    }
                    else if (!allRoles[slot])
                    {
                        rolesByColumn[slot].append(change.roles);
                    }
                }
            }
            std::vector<ColumnChange> normalized;
            for (int column = 0; column < columnTotal; ++column)
            {
                const std::size_t slot = static_cast<std::size_t>(column);
                if (!changed[slot]) continue;
                QList<int>& roles = rolesByColumn[slot];
                std::sort(roles.begin(), roles.end());
                roles.erase(std::unique(roles.begin(), roles.end()), roles.end());
                if (!normalized.empty() && normalized.back().lastColumn + 1 == column
                    && normalized.back().roles == roles)
                {
                    normalized.back().lastColumn = column;
                }
                else
                {
                    normalized.push_back({ column, column, std::move(roles) });
                }
            }
            return normalized;
        }

        // 先比较旧快照，避免 beginRemove/Insert/layout 发布后才去读已替换的旧行。
        // 新键由 rowsInserted 发布完整内容，不需额外 dataChanged；保留键按最终行号通知。
        std::vector<DataChangeRectangle> collectDataChanges(
            const std::vector<RowType>& rows, const std::vector<std::string>& newKeys,
            const std::vector<std::string>& oldKeys, int& updatedRowCount,
            const RowChangeResolver& resolver, quint64 generation) const
        {
            const QPointer<const FlatTableModel> alive(this);
            std::unordered_map<std::string, std::size_t> oldRowByKey;
            oldRowByKey.reserve(oldKeys.size());
            for (std::size_t row = 0; row < oldKeys.size(); ++row) oldRowByKey.emplace(oldKeys[row], row);
            std::vector<DataChangeRectangle> rectangles;
            std::vector<std::size_t> previousRowRectangles;
            for (std::size_t row = 0; row < rows.size(); ++row)
            {
                const auto oldRow = oldRowByKey.find(newKeys[row]);
                std::vector<ColumnChange> changes;
                if (oldRow != oldRowByKey.end())
                {
                    auto raw = resolver(m_rows[oldRow->second], rows[row]);
                    if (!alive || m_updateGeneration != generation) return {};
                    changes = normalizeChanges(raw);
                }
                if (!changes.empty()) ++updatedRowCount;
                std::vector<std::size_t> currentRowRectangles;
                currentRowRectangles.reserve(changes.size());
                for (ColumnChange& change : changes)
                {
                    auto previous = std::find_if(previousRowRectangles.cbegin(), previousRowRectangles.cend(),
                        [&rectangles, &change](std::size_t rectangle)
                        {
                            const ColumnChange& columns = rectangles[rectangle].columns;
                            return columns.firstColumn == change.firstColumn
                                && columns.lastColumn == change.lastColumn && columns.roles == change.roles;
                        });
                    if (previous != previousRowRectangles.cend())
                    {
                        rectangles[*previous].lastRow = static_cast<int>(row);
                        currentRowRectangles.push_back(*previous);
                    }
                    else
                    {
                        currentRowRectangles.push_back(rectangles.size());
                        rectangles.push_back({ static_cast<int>(row), static_cast<int>(row), std::move(change) });
                    }
                }
                previousRowRectangles = std::move(currentRowRectangles);
            }
            return rectangles;
        }

        UpdateStats setRowsIncrementally(std::vector<RowType> rows, quint64 generation)
        {
            const QPointer<FlatTableModel> alive(this);
            const auto current = [&] { return alive && m_updateGeneration == generation; };
            // A callback can replace/destroy its owning std::function. Its local
            // copy remains valid until invocation returns; model rows stay put
            // because nested setRows/clearRows only queue complete snapshots.
            const KeyResolver keyResolver = m_keyResolver;
            const RowChangeResolver changeResolver = m_rowChangeResolver;
            std::vector<std::string> newKeys;
            std::unordered_set<std::string> newKeySet;
            std::vector<std::string> oldKeys;
            std::unordered_set<std::string> oldKeySet;
            const bool validKeys = collectUniqueKeys(rows, newKeys, newKeySet, keyResolver, generation)
                && collectUniqueKeys(m_rows, oldKeys, oldKeySet, keyResolver, generation);
            if (!current()) return {};
            if (!validKeys) return resetRows(std::move(rows));

            UpdateStats stats;
            const bool preciseChanges = static_cast<bool>(changeResolver);
            const std::vector<DataChangeRectangle> dataChanges = preciseChanges
                ? collectDataChanges(rows, newKeys, oldKeys, stats.updatedRowCount, changeResolver, generation)
                : std::vector<DataChangeRectangle>();
            if (!current()) return stats;

            // 先按连续区间删除新快照中已经不存在的键。倒序处理保证剩余行号稳定。
            for (int lastRow = static_cast<int>(m_rows.size()) - 1; lastRow >= 0;)
            {
                const std::string& key = oldKeys[static_cast<std::size_t>(lastRow)];
                if (newKeySet.find(key) != newKeySet.end())
                {
                    --lastRow;
                    continue;
                }

                int firstRow = lastRow;
                while (firstRow > 0 &&
                    newKeySet.find(oldKeys[static_cast<std::size_t>(firstRow - 1)]) == newKeySet.end())
                {
                    --firstRow;
                }
                beginRemoveRows(QModelIndex(), firstRow, lastRow);
                if (!alive) return stats;
                m_rows.erase(m_rows.begin() + firstRow, m_rows.begin() + lastRow + 1);
                oldKeys.erase(oldKeys.begin() + firstRow, oldKeys.begin() + lastRow + 1);
                endRemoveRows();
                if (!current()) return stats;
                stats.removedRowCount += lastRow - firstRow + 1;
                lastRow = firstRow - 1;
            }

            // 新键先追加到末尾，随后通过一次 layout 变更放到最终位置。
            // For each unique new key, membership in the original old set equals
            // membership in the retained set: every removed key is absent from newKeys.
            // Reuse that set; inserted keys need not be tracked because newKeys is unique.
            std::vector<std::size_t> insertedNewRowIndexes;
            for (std::size_t newRow = 0; newRow < newKeys.size(); ++newRow)
            {
                if (oldKeySet.find(newKeys[newRow]) != oldKeySet.end())
                {
                    continue;
                }
                insertedNewRowIndexes.push_back(newRow);
            }
            if (!insertedNewRowIndexes.empty())
            {
                const int firstInsertRow = static_cast<int>(m_rows.size());
                const int lastInsertRow = firstInsertRow + static_cast<int>(insertedNewRowIndexes.size()) - 1;
                beginInsertRows(QModelIndex(), firstInsertRow, lastInsertRow);
                if (!alive) return stats;
                for (const std::size_t newRow : insertedNewRowIndexes)
                {
                    m_rows.push_back(rows[newRow]);
                    oldKeys.push_back(newKeys[newRow]);
                }
                endInsertRows();
                if (!current()) return stats;
                stats.insertedRowCount = static_cast<int>(insertedNewRowIndexes.size());
            }

            stats.orderChanged = (oldKeys != newKeys);
            if (stats.orderChanged)
            {
                std::unordered_map<std::string, int> newRowByKey;
                newRowByKey.reserve(newKeys.size());
                for (int row = 0; row < static_cast<int>(newKeys.size()); ++row)
                {
                    newRowByKey.emplace(newKeys[static_cast<std::size_t>(row)], row);
                }

                emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
                if (!alive) return stats;
                const QModelIndexList oldPersistentIndexes = persistentIndexList();
                QModelIndexList newPersistentIndexes;
                newPersistentIndexes.reserve(oldPersistentIndexes.size());
                for (const QModelIndex& oldIndex : oldPersistentIndexes)
                {
                    if (!oldIndex.isValid() || oldIndex.row() < 0 ||
                        oldIndex.row() >= static_cast<int>(oldKeys.size()))
                    {
                        newPersistentIndexes.push_back(QModelIndex());
                        continue;
                    }
                    const auto targetIt = newRowByKey.find(oldKeys[static_cast<std::size_t>(oldIndex.row())]);
                    newPersistentIndexes.push_back(
                        targetIt == newRowByKey.end()
                        ? QModelIndex()
                        : createIndex(targetIt->second, oldIndex.column()));
                }

                m_rows = std::move(rows);
                changePersistentIndexList(oldPersistentIndexes, newPersistentIndexes);
                emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
                if (!current()) return stats;
            }
            else
            {
                m_rows = std::move(rows);
            }

            if (preciseChanges)
            {
                for (const DataChangeRectangle& change : dataChanges)
                {
                    emit dataChanged(index(change.firstRow, change.columns.firstColumn),
                        index(change.lastRow, change.columns.lastColumn), change.columns.roles);
                    if (!current()) return stats;
                }
            }
            else
            {
                // 未选择精确协议的旧消费者保持原有通知行为与统计口径。
                stats.updatedRowCount = static_cast<int>(m_rows.size());
                if (!m_rows.empty() && !m_columns.empty())
                {
                    emit dataChanged(
                        index(0, 0),
                        index(static_cast<int>(m_rows.size()) - 1, static_cast<int>(m_columns.size()) - 1));
                }
            }
            return stats;
        }
    };
}
