#pragma once

#include "TableRowFilterSupport.h"

#include "TableSnapshotCompare.h"

#include <QAbstractTableModel>
#include <QList>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>

class QEvent;
class QTableView;

namespace ks::ui
{
    // TablePausedSnapshotModel 作用：
    // - 把停止刷新时捕获的 TableSnapshot 暴露为只读表格模型；
    // - 模型完全独立于实时业务模型，后台刷新不会改变当前显示内容。
    class TablePausedSnapshotModel final : public QAbstractTableModel
    {
    public:
        explicit TablePausedSnapshotModel(
            TableSnapshot snapshot,
            QObject* parent = nullptr);

        const TableSnapshot& snapshot() const;

        int rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        QVariant headerData(
            int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;
        Qt::ItemFlags flags(const QModelIndex& index) const override;

    private:
        TableSnapshot m_snapshot;
    };

    // TableFrozenPaneController 作用：
    // - “冻结行列”：把选中的行钉在列表头正下方、把选中的列固定在行表头右侧；
    // - 只冻结选中的那几行/那一列本身，不会把它上方或左侧的内容一并冻结；
    // - 被冻结的行列从滚动区中移出并单独占位（通过 TableActionBarHost 把视口切小），
    //   其余内容——包括冻结点之前的行列——仍可完整滚动，不存在重影或滚不到的区域；
    // - 冻结项用持久模型索引跟踪，行列增删或排序后仍钉住同一行数据；模型整体更换时自动解除；
    // - 冻结区合计不超过可用尺寸的一半，超出预算的行列不接受冻结；
    // - 目标表格必须实现 TableActionBarHost（VisibleTableWidget / TableActionTableView 等），
    //   普通 QTableView 无法预留视口，canFreeze() 会返回 false。
    class TableFrozenPaneController final : public QObject
    {
    public:
        explicit TableFrozenPaneController(QObject* parent = nullptr);
        ~TableFrozenPaneController() override;

        void setTargetTable(QTableView* tableView);
        QTableView* targetTable() const;

        // canFreeze 作用：目标表格是否具备视口预留能力，决定冻结菜单是否可用。
        bool canFreeze() const;

        // freezeRows 作用：冻结给定逻辑行（通常为选中行）；返回本次新冻结的行数，
        // 0 表示行无效、已冻结或冻结预算不足，未做任何改动。
        int freezeRows(const QList<int>& logicalRows);
        int freezeColumns(const QList<int>& logicalColumns);

        void clearFrozenRows();
        void clearFrozenColumns();
        void clearFrozenPanes();

        int frozenRowCount() const;
        int frozenColumnCount() const;

        void refreshGeometry();

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override;

    private:
        // FrozenLine 作用：一条被冻结的行或列。
        struct FrozenLine
        {
            QPersistentModelIndex index; // 行冻结存 (row,0)，列冻结存 (0,column)；随模型增删/排序自动平移。
            int extent = 0;              // 冻结时捕获的行高/列宽；源表隐藏该行列后原尺寸读不到了。
            // section 是最后一次已知的逻辑行号/列号。持久索引锚在单元格上，
            // 表格 setRowCount(0) 重填后就会失效，而列的隐藏位不会随之清除；
            // 没有它就既无法还原隐藏、也无法把冻结重新锚回去。
            int section = -1;
        };

        void disconnectTarget();
        void connectTarget();
        void scheduleRefresh();
        void releaseTarget();
        // 把当前冻结的行号/列号发布到目标表格属性上，供取数侧区分冻结与筛选。
        void publishFrozenSections();
        void destroyPanes();
        void ensurePanes();
        QTableView* createPane(QPointer<QTableView>& guardedPane);
        void configurePane(QTableView* pane, bool showHorizontalHeader, bool showVerticalHeader);
        void bridgeFrozenHeader(QTableView* pane);
        void mirrorColumnLayout(QTableView* pane);
        void mirrorRowLayout(QTableView* pane, int firstLogicalRow, int lastLogicalRow);
        void mirrorVisibleRowLayout(QTableView* pane);
        void applyFrozenRowFilter(QTableView* pane);
        void applyFrozenColumnFilter(QTableView* pane);
        void enforceFrozenState();
        void unfreezeAllRows();
        void unfreezeAllColumns();
        void layoutPanes();
        void syncPanes();
        void setPaneVerticalToSource(QTableView* pane);
        void setPaneHorizontalToSource(QTableView* pane);

        int totalFrozenRowsHeight() const;
        int totalFrozenColumnsWidth() const;
        int frozenRowsBudget() const;
        int frozenColumnsBudget() const;

        QPointer<QTableView> m_targetTable;
        QPointer<QAbstractItemModel> m_targetModel;
        QPointer<QTableView> m_topPane;
        QPointer<QTableView> m_leftPane;
        QPointer<QTableView> m_cornerPane;
        QList<FrozenLine> m_frozenRowLines;
        QList<FrozenLine> m_frozenColumnLines;
        int m_appliedFrozenWidth = 0;
        int m_appliedFrozenHeight = 0;
        bool m_refreshing = false;
        bool m_refreshScheduled = false;
        bool m_mirroringSections = false;
    };

    /*
     * 冻结是用 setRowHidden/setColumnHidden 把行列从主表挪到冻结窗格里的，
     * 在 QTableView 上和"被筛选掉"共用同一个状态位。凡是按"可见行/可见列"
     * 取数的地方（复制、导出、快照比对）都必须先问一下这两个函数，
     * 否则用户特意钉住的行列恰恰不会出现在导出结果里。
     */
    bool isRowHiddenByFreeze(const QTableView* tableView, int row);
    bool isColumnHiddenByFreeze(const QTableView* tableView, int column);
}
