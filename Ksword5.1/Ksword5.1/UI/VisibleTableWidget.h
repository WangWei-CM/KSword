#pragma once

#include <QAbstractItemModel>
#include <QHeaderView>
#include <QList>
#include <QModelIndex>
#include <QPointer>
#include <QScrollBar>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace ks::ui
{
    namespace visible_table_detail
    {
        inline std::pair<int, int> visibleRowRange(const QTableView* tableView)
        {
            if (tableView == nullptr ||
                tableView->model() == nullptr ||
                tableView->viewport() == nullptr ||
                tableView->verticalHeader() == nullptr ||
                tableView->viewport()->height() <= 0)
            {
                return { -1, -1 };
            }

            const int rowCount = tableView->model()->rowCount();
            if (rowCount <= 0)
            {
                return { -1, -1 };
            }

            const QHeaderView* verticalHeader = tableView->verticalHeader();
            int firstRow = verticalHeader->logicalIndexAt(0);
            if (firstRow < 0)
            {
                firstRow = verticalHeader->logicalIndexAt(1);
            }
            if (firstRow < 0)
            {
                return { -1, -1 };
            }

            int lastRow = verticalHeader->logicalIndexAt(tableView->viewport()->height() - 1);
            if (lastRow < 0)
            {
                // The table is shorter than its viewport. In that case the final model row is
                // visible even though the pixel at the bottom of the viewport is empty.
                const int finalContentPixel = std::min(
                    tableView->viewport()->height() - 1,
                    verticalHeader->length() - 1);
                lastRow = verticalHeader->logicalIndexAt(finalContentPixel);
            }
            if (lastRow < 0)
            {
                lastRow = firstRow;
            }

            if (firstRow > lastRow)
            {
                std::swap(firstRow, lastRow);
            }
            return {
                std::clamp(firstRow, 0, rowCount - 1),
                std::clamp(lastRow, 0, rowCount - 1)
            };
        }

        inline void scheduleVisibleRowHeightRefresh(QTableView* tableView)
        {
            if (tableView == nullptr || tableView->property("kswordVisibleRowHeightRefreshPending").toBool())
            {
                return;
            }

            tableView->setProperty("kswordVisibleRowHeightRefreshPending", true);
            const QPointer<QTableView> guardedTable(tableView);
            QTimer::singleShot(0, tableView, [guardedTable]()
                {
                    if (guardedTable.isNull())
                    {
                        return;
                    }

                    QTableView* table = guardedTable.data();
                    table->setProperty("kswordVisibleRowHeightRefreshPending", false);
                    const auto [firstRow, lastRow] = visibleRowRange(table);
                    if (firstRow < 0 || lastRow < firstRow)
                    {
                        return;
                    }

                    for (int row = firstRow; row <= lastRow; ++row)
                    {
                        if (!table->isRowHidden(row))
                        {
                            table->resizeRowToContents(row);
                        }
                    }
                });
        }
    }

    // VisibleTableWidget keeps the complete QTableWidget model intact. In particular, every
    // off-screen item and sort role still participates in QTableWidget's normal full-data sort.
    // Only QAbstractItemView's repaint notification is clipped to rows intersecting the viewport.
    class VisibleTableWidget final : public QTableWidget
    {
    public:
        using QTableWidget::QTableWidget;

        static constexpr int LongTableRowThreshold = 64;

    protected:
        void dataChanged(
            const QModelIndex& topLeft,
            const QModelIndex& bottomRight,
            const QList<int>& roles = QList<int>()) override
        {
            if (!topLeft.isValid() ||
                !bottomRight.isValid() ||
                topLeft.parent() != bottomRight.parent() ||
                model() == nullptr ||
                model()->rowCount(topLeft.parent()) < LongTableRowThreshold ||
                property("kswordDisableVisibleRefresh").toBool())
            {
                QTableView::dataChanged(topLeft, bottomRight, roles);
                return;
            }

            // Hidden tabs need no repaint. Qt will paint their current model contents normally
            // when they become visible, so skipping this notification cannot leave stale data.
            if (viewport() == nullptr || !viewport()->isVisible())
            {
                return;
            }

            const auto [firstVisibleRow, lastVisibleRow] =
                visible_table_detail::visibleRowRange(this);
            if (firstVisibleRow < 0 ||
                lastVisibleRow < topLeft.row() ||
                firstVisibleRow > bottomRight.row())
            {
                return;
            }

            const int firstChangedVisibleRow = std::max(firstVisibleRow, topLeft.row());
            const int lastChangedVisibleRow = std::min(lastVisibleRow, bottomRight.row());
            const QModelIndex clippedTopLeft = model()->index(
                firstChangedVisibleRow,
                topLeft.column(),
                topLeft.parent());
            const QModelIndex clippedBottomRight = model()->index(
                lastChangedVisibleRow,
                bottomRight.column(),
                bottomRight.parent());
            QTableView::dataChanged(clippedTopLeft, clippedBottomRight, roles);
        }
    };

    // Enables on-demand row-height measurement for variable-height long tables. The model still
    // contains every row; scrolling only measures the rows that have entered the viewport.
    inline void InstallVisibleRowHeightRefresh(QTableView* tableView)
    {
        if (tableView == nullptr || tableView->property("kswordVisibleRowHeightRefreshInstalled").toBool())
        {
            return;
        }

        tableView->setProperty("kswordVisibleRowHeightRefreshInstalled", true);
        if (tableView->verticalHeader() != nullptr)
        {
            tableView->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        }

        QObject::connect(
            tableView->verticalScrollBar(),
            &QScrollBar::valueChanged,
            tableView,
            [tableView](int)
            {
                visible_table_detail::scheduleVisibleRowHeightRefresh(tableView);
            });
        visible_table_detail::scheduleVisibleRowHeightRefresh(tableView);
    }

    inline void RefreshVisibleRowHeights(QTableView* tableView)
    {
        visible_table_detail::scheduleVisibleRowHeightRefresh(tableView);
    }
}
