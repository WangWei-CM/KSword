#pragma once

#include <QAbstractTableModel>
#include <QMetaType>
#include <QString>
#include <QVariant>

#include "../Internationalization/LanguageManager.h"

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

        struct UpdateStats
        {
            int insertedRowCount = 0;
            int removedRowCount = 0;
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
            KeyResolver keyResolver = KeyResolver())
            : QAbstractTableModel(parent)
            , m_columns(std::move(columns))
            , m_dataResolver(std::move(resolver))
            , m_flagsResolver(std::move(flagsResolver))
            , m_keyResolver(std::move(keyResolver))
        {
        }

        // setRows 作用：
        // - 用一批新行替换当前快照；
        // - 有稳定键时按删除/插入/layout/dataChanged 增量发布；
        // - 没有稳定键或键重复时回退 beginResetModel/endResetModel。
        // 参数 rows：新的可见行集合。
        UpdateStats setRows(std::vector<RowType> rows)
        {
            if (!m_keyResolver)
            {
                return resetRows(std::move(rows));
            }
            return setRowsIncrementally(std::move(rows));
        }

        // clearRows 作用：
        // - 清空当前快照；
        // - 供“清空日志/清空结果”这类场景直接调用。
        void clearRows()
        {
            if (m_rows.empty())
            {
                return;
            }

            beginResetModel();
            m_rows.clear();
            endResetModel();
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
        std::vector<RowType> m_rows;        // m_rows：当前可见行快照。

        UpdateStats resetRows(std::vector<RowType> rows)
        {
            UpdateStats stats;
            stats.updatedRowCount = static_cast<int>(rows.size());
            stats.modelReset = true;
            beginResetModel();
            m_rows = std::move(rows);
            endResetModel();
            return stats;
        }

        bool collectUniqueKeys(
            const std::vector<RowType>& rows,
            std::vector<std::string>& keyList,
            std::unordered_set<std::string>& keySet) const
        {
            keyList.clear();
            keySet.clear();
            keyList.reserve(rows.size());
            keySet.reserve(rows.size());
            for (const RowType& row : rows)
            {
                std::string key = m_keyResolver(row);
                if (key.empty() || !keySet.insert(key).second)
                {
                    return false;
                }
                keyList.push_back(std::move(key));
            }
            return true;
        }

        UpdateStats setRowsIncrementally(std::vector<RowType> rows)
        {
            std::vector<std::string> newKeys;
            std::unordered_set<std::string> newKeySet;
            std::vector<std::string> oldKeys;
            std::unordered_set<std::string> oldKeySet;
            if (!collectUniqueKeys(rows, newKeys, newKeySet) ||
                !collectUniqueKeys(m_rows, oldKeys, oldKeySet))
            {
                return resetRows(std::move(rows));
            }

            UpdateStats stats;

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
                m_rows.erase(m_rows.begin() + firstRow, m_rows.begin() + lastRow + 1);
                oldKeys.erase(oldKeys.begin() + firstRow, oldKeys.begin() + lastRow + 1);
                endRemoveRows();
                stats.removedRowCount += lastRow - firstRow + 1;
                lastRow = firstRow - 1;
            }

            // 新键先追加到末尾，随后通过一次 layout 变更放到最终位置。
            oldKeySet.clear();
            oldKeySet.insert(oldKeys.cbegin(), oldKeys.cend());
            std::vector<std::size_t> insertedNewRowIndexes;
            for (std::size_t newRow = 0; newRow < newKeys.size(); ++newRow)
            {
                if (oldKeySet.find(newKeys[newRow]) != oldKeySet.end())
                {
                    continue;
                }
                insertedNewRowIndexes.push_back(newRow);
                oldKeySet.insert(newKeys[newRow]);
            }
            if (!insertedNewRowIndexes.empty())
            {
                const int firstInsertRow = static_cast<int>(m_rows.size());
                const int lastInsertRow = firstInsertRow + static_cast<int>(insertedNewRowIndexes.size()) - 1;
                beginInsertRows(QModelIndex(), firstInsertRow, lastInsertRow);
                for (const std::size_t newRow : insertedNewRowIndexes)
                {
                    m_rows.push_back(rows[newRow]);
                    oldKeys.push_back(newKeys[newRow]);
                }
                endInsertRows();
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

                emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
                m_rows = std::move(rows);
                changePersistentIndexList(oldPersistentIndexes, newPersistentIndexes);
                emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
            }
            else
            {
                m_rows = std::move(rows);
            }

            stats.updatedRowCount = static_cast<int>(m_rows.size());
            if (!m_rows.empty() && !m_columns.empty())
            {
                emit dataChanged(
                    index(0, 0),
                    index(static_cast<int>(m_rows.size()) - 1, static_cast<int>(m_columns.size()) - 1));
            }
            return stats;
        }
    };
}
