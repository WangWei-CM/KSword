#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <functional>
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
        explicit FlatTableModel(std::vector<ColumnSpec> columns, DataResolver resolver, QObject* parent = nullptr)
            : QAbstractTableModel(parent)
            , m_columns(std::move(columns))
            , m_dataResolver(std::move(resolver))
        {
        }

        // setRows 作用：
        // - 用一批新行替换当前快照；
        // - 调用 beginResetModel/endResetModel 让视图一次性刷新。
        // 参数 rows：新的可见行集合。
        void setRows(std::vector<RowType> rows)
        {
            beginResetModel();
            m_rows = std::move(rows);
            endResetModel();
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

            return m_dataResolver(m_rows[static_cast<std::size_t>(row)], column, role);
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

            return m_columns[static_cast<std::size_t>(section)].headerText;
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

            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        }

    private:
        std::vector<ColumnSpec> m_columns;  // m_columns：固定横向列定义。
        DataResolver m_dataResolver;        // m_dataResolver：单元格角色解析器。
        std::vector<RowType> m_rows;        // m_rows：当前可见行快照。
    };
}
