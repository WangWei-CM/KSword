#include "TableHeaderSortingSupport.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QTimer>
#include <QTableView>
#include <QVector>
#include <utility>
#include <QHeaderView>
#include <QList>
#include <QModelIndex>
#include <QPointer>
#include <QTableWidget>
#include <QVariant>

namespace
{
    // 下列动态属性只保存通用表头排序的安装状态和最近一次手动排序状态。
    // 属性挂在表格对象上，避免额外控制器与表格生命周期不同步。
    constexpr char kHeaderSortingInstalledProperty[] =
        "KSWORD_TABLE_HEADER_SORTING_INSTALLED";
    constexpr char kHeaderSortingDisabledProperty[] =
        "KSWORD_TABLE_HEADER_SORTING_DISABLED";
    constexpr char kManualSortActiveProperty[] =
        "KSWORD_TABLE_HEADER_MANUAL_SORT_ACTIVE";
    constexpr char kManualSortColumnProperty[] =
        "KSWORD_TABLE_HEADER_MANUAL_SORT_COLUMN";
    constexpr char kManualSortOrderProperty[] =
        "KSWORD_TABLE_HEADER_MANUAL_SORT_ORDER";

    // clearManualSortState 作用：
    // - 清除通用排序记录，并按需隐藏只属于通用排序的表头箭头；
    // - 原生 sortingEnabled 为 true 时由调用方跳过本函数，避免隐藏 Qt 自己的箭头。
    // 传入 tableWidget：目标表格；hideIndicator：是否隐藏排序指示器；传出：无。
    void clearManualSortState(
        QTableView* tableWidget,
        const bool hideIndicator)
    {
        if (tableWidget == nullptr)
        {
            return;
        }

        tableWidget->setProperty(kManualSortActiveProperty, false);
        tableWidget->setProperty(kManualSortColumnProperty, -1);
        tableWidget->setProperty(
            kManualSortOrderProperty,
            static_cast<int>(Qt::AscendingOrder));

        QHeaderView* const headerView = tableWidget->horizontalHeader();
        if (hideIndicator && headerView != nullptr)
        {
            headerView->setSortIndicatorShown(false);
        }
    }

    // invalidateManualSortAfterDataChange 作用：
    // - 数据源发生增删、重置或改单元格后撤销手动排序标记；
    // - 不自动重排新数据，避免异步分批填表期间搬动尚未填完整的行；
    // - 用户再次点击表头即可按新快照排序。
    void invalidateManualSortAfterDataChange(
        const QPointer<QTableWidget>& guardedTable)
    {
        if (guardedTable.isNull() || guardedTable->isSortingEnabled())
        {
            return;
        }
        if (!guardedTable->property(kManualSortActiveProperty).toBool())
        {
            return;
        }

        clearManualSortState(guardedTable.data(), true);
    }

    // nextSortOrder 作用：
    // - 新列第一次点击使用升序；连续点击同一列时在升序和降序之间切换；
    // - 最近列与顺序由表格动态属性读取，不依赖 QHeaderView 未显示时的默认值。
    // 传入 tableWidget：目标表格；logicalColumn：本次点击列；返回本次排序方向。
    Qt::SortOrder nextSortOrder(
        const QTableView* tableWidget,
        const int logicalColumn)
    {
        const bool manualSortActive =
            tableWidget->property(kManualSortActiveProperty).toBool();
        const int previousColumn =
            tableWidget->property(kManualSortColumnProperty).toInt();
        if (!manualSortActive || previousColumn != logicalColumn)
        {
            return Qt::AscendingOrder;
        }

        const auto previousOrder = static_cast<Qt::SortOrder>(
            tableWidget->property(kManualSortOrderProperty).toInt());
        return previousOrder == Qt::AscendingOrder
            ? Qt::DescendingOrder
            : Qt::AscendingOrder;
    }

    // sortByClickedHeader 作用：
    // - 响应一次水平表头点击并排序 QTableWidget 的完整内部模型；
    // - 原生持续排序开启或业务禁用通用排序时不介入；
    // - 返回：无，排序结果直接体现在目标表格。
    void sortByClickedHeader(
        QTableWidget* tableWidget,
        const int logicalColumn)
    {
        if (tableWidget == nullptr ||
            tableWidget->property(kHeaderSortingDisabledProperty).toBool() ||
            logicalColumn < 0 ||
            logicalColumn >= tableWidget->columnCount())
        {
            return;
        }

        // Qt 原生排序已经连接表头信号，此处只清理旧的手动状态，避免执行两次排序。
        if (tableWidget->isSortingEnabled())
        {
            tableWidget->setProperty(kManualSortActiveProperty, false);
            return;
        }

        const Qt::SortOrder sortOrder = nextSortOrder(tableWidget, logicalColumn);
        tableWidget->setProperty(kManualSortActiveProperty, true);
        tableWidget->setProperty(kManualSortColumnProperty, logicalColumn);
        tableWidget->setProperty(kManualSortOrderProperty, static_cast<int>(sortOrder));

        tableWidget->sortItems(logicalColumn, sortOrder);
        QHeaderView* const headerView = tableWidget->horizontalHeader();
        if (headerView != nullptr)
        {
            headerView->setSortIndicator(logicalColumn, sortOrder);
            headerView->setSortIndicatorShown(true);
        }
    }
    constexpr char kViewSortingStateProperty[] = "KSWORD_TABLE_VIEW_HEADER_SORT_STATE";

    // The view owns this binding. Model replacement is checked at the exact view,
    // at header clicks, and before every old-model callback; no control-tree scan.
    class TableViewHeaderSorting final : public QObject
    {
    public:
        explicit TableViewHeaderSorting(QTableView* view)
            : QObject(view), m_view(view)
        {
            view->setProperty(kViewSortingStateProperty, QVariant::fromValue(static_cast<QObject*>(this)));
            view->installEventFilter(this);
            if (view->viewport() != nullptr) view->viewport()->installEventFilter(this);
            synchronize();
        }
        ~TableViewHeaderSorting() override
        {
            if (!m_view.isNull() && m_view->property(kViewSortingStateProperty).value<QObject*>() == this)
                m_view->setProperty(kViewSortingStateProperty, QVariant());
        }

        void synchronize()
        {
            if (m_view.isNull()) return;
            auto* const header = m_view->horizontalHeader();
            if (m_header != header)
            {
                QObject::disconnect(m_headerConnection);
                m_header = header;
                if (header != nullptr)
                {
                    header->setSectionsClickable(true);
                    m_headerConnection = QObject::connect(header, &QHeaderView::sectionClicked, this,
                        [this, header](int column) { clicked(header, column); });
                }
            }
            auto* const model = m_view->model();
            if (m_model == model) return;
            for (const auto& connection : m_modelConnections) QObject::disconnect(connection);
            m_modelConnections.clear();
            ++m_epoch;
            m_model = model;
            m_resetInFlight = false;
            m_restoreSourceOrder = false;
            m_flushScheduled = false;
            m_pendingClicks.clear();
            if (!m_view->isSortingEnabled()) clearManualSortState(m_view, true);
            if (model == nullptr) return;

            m_modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::modelAboutToBeReset, this,
                [this, model]() {
                    if (!isCurrentModel(model)) return;
                    m_resetInFlight = true;
                    m_restoreSourceOrder = true;
                    m_pendingClicks.clear();
                    if (!m_view->isSortingEnabled()) clearManualSortState(m_view, true);
                }));
            m_modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::modelReset, this,
                [this, model]() {
                    if (!isCurrentModel(model)) return;
                    m_resetInFlight = false;
                    if (!m_view->isSortingEnabled()) clearManualSortState(m_view, true);
                    scheduleFlush();
                }));
            const auto changed = [this, model]() {
                if (isCurrentModel(model) && !m_view->isSortingEnabled()) clearManualSortState(m_view, true);
            };
            m_modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::rowsInserted, this,
                [changed](const QModelIndex&, int, int) { changed(); }));
            m_modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::rowsRemoved, this,
                [changed](const QModelIndex&, int, int) { changed(); }));
            m_modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::columnsInserted, this,
                [changed](const QModelIndex&, int, int) { changed(); }));
            m_modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::columnsRemoved, this,
                [changed](const QModelIndex&, int, int) { changed(); }));
            m_modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::dataChanged, this,
                [changed](const QModelIndex&, const QModelIndex&, const QList<int>&) { changed(); }));
            m_modelConnections.push_back(QObject::connect(model, &QObject::destroyed, this,
                [this]() { synchronize(); }));
        }

    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            switch (event->type())
            {
            case QEvent::Show:
            case QEvent::Polish:
            case QEvent::LayoutRequest:
            case QEvent::UpdateRequest:
            case QEvent::Paint:
            case QEvent::ChildAdded:
            case QEvent::ChildRemoved:
                synchronize();
                break;
            default:
                break;
            }
            return false;
        }

    private:
        bool isCurrentModel(QAbstractItemModel* expected)
        {
            if (m_view.isNull()) return false;
            if (m_view->model() != m_model) synchronize();
            return !m_view.isNull() && m_model == expected && m_view->model() == expected;
        }
        void clicked(QHeaderView* expectedHeader, int column)
        {
            synchronize();
            if (m_view.isNull() || m_model.isNull() || m_header != expectedHeader ||
                m_view->property(kHeaderSortingDisabledProperty).toBool() ||
                column < 0 || column >= m_model->columnCount()) return;
            if (m_view->isSortingEnabled())
            {
                m_view->setProperty(kManualSortActiveProperty, false);
                return;
            }
            if (m_resetInFlight || m_restoreSourceOrder || m_flushScheduled)
            {
                m_pendingClicks.push_back(column);
                if (!m_resetInFlight) scheduleFlush();
                return;
            }
            sortNow(column);
        }
        void sortNow(int column)
        {
            if (m_view.isNull() || m_model.isNull() || m_view->model() != m_model ||
                m_view->property(kHeaderSortingDisabledProperty).toBool() ||
                column < 0 || column >= m_model->columnCount()) return;
            if (m_view->isSortingEnabled())
            {
                m_view->setProperty(kManualSortActiveProperty, false);
                return;
            }
            const auto order = nextSortOrder(m_view, column);
            m_view->setProperty(kManualSortActiveProperty, true);
            m_view->setProperty(kManualSortColumnProperty, column);
            m_view->setProperty(kManualSortOrderProperty, static_cast<int>(order));
            const QPointer<TableViewHeaderSorting> guard(this);
            const auto epoch = m_epoch;
            m_model->sort(column, order);
            if (guard.isNull()) return;
            if (m_view.isNull() || m_epoch != epoch || m_view->model() != m_model ||
                m_resetInFlight || m_restoreSourceOrder) return;
            if (!m_header.isNull())
            {
                const QPointer<QHeaderView> header(m_header);
                const QPointer<QAbstractItemModel> model(m_model);
                header->setSortIndicator(column, order);
                // sortIndicatorChanged is synchronous. A listener may close the
                // owner, replace its header/model, or reset the snapshot.
                if (guard.isNull()) return;
                if (m_view.isNull() || header.isNull() || model.isNull() ||
                    m_epoch != epoch || m_view->model() != model ||
                    m_view->horizontalHeader() != header || m_header != header ||
                    m_resetInFlight || m_restoreSourceOrder) return;
                header->setSortIndicatorShown(true);
            }
        }
        void scheduleFlush()
        {
            if (m_flushScheduled) return;
            m_flushScheduled = true;
            const auto epoch = m_epoch;
            QTimer::singleShot(0, this, [this, epoch]() {
                if (m_epoch != epoch) return;
                m_flushScheduled = false;
                synchronize();
                if (m_epoch != epoch || m_view.isNull() || m_model.isNull() || m_resetInFlight) return;
                if (std::exchange(m_restoreSourceOrder, false) && !m_view->isSortingEnabled())
                {
                    // A proxy may emit modelReset from inside its source-reset stack.
                    // Deferring sort(-1) keeps layout changes outside begin/endResetModel.
                    const QPointer<TableViewHeaderSorting> guard(this);
                    m_model->sort(-1, Qt::AscendingOrder);
                    if (guard.isNull()) return;
                    if (m_epoch != epoch || m_view.isNull() || m_view->model() != m_model ||
                        m_resetInFlight || m_restoreSourceOrder) return;
                }
                auto clicks = std::exchange(m_pendingClicks, {});
                const QPointer<TableViewHeaderSorting> guard(this);
                for (const int column : clicks)
                {
                    sortNow(column);
                    if (guard.isNull()) return;
                    if (m_epoch != epoch || m_resetInFlight || m_restoreSourceOrder) return;
                }
            });
        }

        QPointer<QTableView> m_view;
        QPointer<QHeaderView> m_header;
        QPointer<QAbstractItemModel> m_model;
        QMetaObject::Connection m_headerConnection;
        QList<QMetaObject::Connection> m_modelConnections;
        QVector<int> m_pendingClicks;
        quint64 m_epoch = 0;
        bool m_resetInFlight = false;
        bool m_restoreSourceOrder = false;
        bool m_flushScheduled = false;
    };
}

namespace ks::ui
{
    void InstallTableHeaderClickSorting(QTableWidget* tableWidget)
    {
        if (tableWidget == nullptr ||
            tableWidget->model() == nullptr ||
            tableWidget->property(kHeaderSortingInstalledProperty).toBool())
        {
            return;
        }

        QHeaderView* const headerView = tableWidget->horizontalHeader();
        if (headerView == nullptr)
        {
            return;
        }

        tableWidget->setProperty(kHeaderSortingInstalledProperty, true);
        headerView->setSectionsClickable(true);
        const QPointer<QTableWidget> guardedTable(tableWidget);

        QObject::connect(
            headerView,
            &QHeaderView::sectionClicked,
            tableWidget,
            [guardedTable](const int logicalColumn)
            {
                sortByClickedHeader(guardedTable.data(), logicalColumn);
            });

        // 内部 QTableWidget model 与表格同寿命。任何数据变化都只撤销旧箭头，
        // 不在填充信号中重排，兼容同步和定时器分批刷新两种页面。
        QAbstractItemModel* const tableModel = tableWidget->model();
        QObject::connect(
            tableModel,
            &QAbstractItemModel::modelReset,
            tableWidget,
            [guardedTable]()
            {
                invalidateManualSortAfterDataChange(guardedTable);
            });
        QObject::connect(
            tableModel,
            &QAbstractItemModel::rowsInserted,
            tableWidget,
            [guardedTable](const QModelIndex&, const int, const int)
            {
                invalidateManualSortAfterDataChange(guardedTable);
            });
        QObject::connect(
            tableModel,
            &QAbstractItemModel::rowsRemoved,
            tableWidget,
            [guardedTable](const QModelIndex&, const int, const int)
            {
                invalidateManualSortAfterDataChange(guardedTable);
            });
        QObject::connect(
            tableModel,
            &QAbstractItemModel::dataChanged,
            tableWidget,
            [guardedTable](const QModelIndex&, const QModelIndex&, const QList<int>&)
            {
                invalidateManualSortAfterDataChange(guardedTable);
            });
    }

    void SetTableHeaderClickSortingEnabled(
        QTableWidget* tableWidget,
        const bool enabled)
    {
        if (tableWidget == nullptr)
        {
            return;
        }

        tableWidget->setProperty(kHeaderSortingDisabledProperty, !enabled);
        if (!enabled && !tableWidget->isSortingEnabled())
        {
            clearManualSortState(tableWidget, true);
        }
    }

    void InstallTableHeaderClickSorting(QTableView* tableView)
    {
        if (tableView == nullptr) return;
        if (auto* widget = qobject_cast<QTableWidget*>(tableView))
        {
            InstallTableHeaderClickSorting(widget);
            return;
        }
        auto* state = dynamic_cast<TableViewHeaderSorting*>(
            tableView->property(kViewSortingStateProperty).value<QObject*>());
        if (state != nullptr) state->synchronize();
        else new TableViewHeaderSorting(tableView);
    }

    void SetTableHeaderClickSortingEnabled(QTableView* tableView, const bool enabled)
    {
        if (tableView == nullptr) return;
        if (auto* widget = qobject_cast<QTableWidget*>(tableView))
        {
            SetTableHeaderClickSortingEnabled(widget, enabled);
            return;
        }
        tableView->setProperty(kHeaderSortingDisabledProperty, !enabled);
        if (!enabled && !tableView->isSortingEnabled()) clearManualSortState(tableView, true);
    }
}
