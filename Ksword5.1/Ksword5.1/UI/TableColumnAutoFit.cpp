#include "TableColumnAutoFit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QVariant>
#include <QWidget>

namespace
{
    // 属性名统一集中，避免和业务控件属性冲突。
    constexpr const char* AutoFitInstalledProperty = "_ksword_table_column_auto_fit_installed";
    constexpr const char* AutoFitScheduledProperty = "_ksword_table_column_auto_fit_scheduled";
    constexpr const char* AutoFitUserAdjustedProperty = "_ksword_table_column_auto_fit_user_adjusted";
    constexpr const char* AutoFitHeaderHookedProperty = "_ksword_table_column_auto_fit_header_hooked";
    constexpr const char* AutoFitResizePendingProperty = "_ksword_table_column_auto_fit_resize_pending";
    constexpr const char* AutoFitResizePressPositionProperty = "_ksword_table_column_auto_fit_resize_press_position";

    // kViewportPadding 作用：
    // - 给视口右边预留少量像素，规避样式边框/网格线取整导致的 1px 横向滚动条；
    // - 不改变滚动条策略，只让列宽总和略小于 viewport 宽度。
    constexpr int kViewportPadding = 2;

    // kPreferredMinimumSectionWidth/kAbsoluteMinimumSectionWidth 作用：
    // - 普通情况下列至少保留 24px，避免内容完全不可见；
    // - 超多列/超窄窗口时允许继续压缩到 8px，尽量达成“默认不出横向滚动条”。
    constexpr int kPreferredMinimumSectionWidth = 24;
    constexpr int kAbsoluteMinimumSectionWidth = 8;

    // kResizeGripMargin 作用：
    // - 识别用户是否在表头列边界附近按下鼠标；
    // - 只有这类动作会被视作手动调列宽，普通点击排序不禁用自动 fit。
    constexpr int kResizeGripMargin = 6;

    struct VisibleSection
    {
        int logicalIndex = -1;  // logicalIndex：QHeaderView 逻辑列号。
        int currentWidth = 0;   // currentWidth：自动 fit 前的当前列宽，用作比例权重。
        int fittedWidth = 0;    // fittedWidth：压缩/扩展后的目标列宽。
    };

    // horizontalHeaderForView 作用：
    // - 从 QTableView/QTreeView 统一取得横向表头；
    // - QTableWidget/QTreeWidget 继承自这两类，因此不需要单独分支。
    // 参数 view：候选表格视图。
    // 返回值：有效横向表头；不支持的视图返回 nullptr。
    QHeaderView* horizontalHeaderForView(QAbstractItemView* view)
    {
        if (view == nullptr || qobject_cast<QHeaderView*>(view) != nullptr)
        {
            return nullptr;
        }

        if (QTableView* tableView = qobject_cast<QTableView*>(view))
        {
            return tableView->horizontalHeader();
        }

        if (QTreeView* treeView = qobject_cast<QTreeView*>(view))
        {
            return treeView->header();
        }

        return nullptr;
    }

    // viewForHorizontalHeader 作用：
    // - 从表头反向取得所属表格/树表；
    // - 用于在用户拖拽表头边界时标记该表已经由用户手动接管列宽。
    // 参数 header：横向表头。
    // 返回值：所属 QAbstractItemView；无法解析时返回 nullptr。
    QAbstractItemView* viewForHorizontalHeader(QHeaderView* header)
    {
        if (header == nullptr || header->orientation() != Qt::Horizontal)
        {
            return nullptr;
        }

        return qobject_cast<QAbstractItemView*>(header->parentWidget());
    }

    // eventObjectToHorizontalHeader 作用：
    // - 把全局事件过滤器收到的 QObject 映射为 QHeaderView；
    // - 鼠标事件通常落在 header viewport 上，因此需要检查 parentWidget。
    // 参数 watchedObject：事件来源对象。
    // 返回值：横向 QHeaderView；不相关对象返回 nullptr。
    QHeaderView* eventObjectToHorizontalHeader(QObject* watchedObject)
    {
        if (QHeaderView* header = qobject_cast<QHeaderView*>(watchedObject))
        {
            return header->orientation() == Qt::Horizontal ? header : nullptr;
        }

        QWidget* watchedWidget = qobject_cast<QWidget*>(watchedObject);
        if (watchedWidget == nullptr)
        {
            return nullptr;
        }

        QHeaderView* parentHeader = qobject_cast<QHeaderView*>(watchedWidget->parentWidget());
        if (parentHeader == nullptr || parentHeader->orientation() != Qt::Horizontal)
        {
            return nullptr;
        }
        return parentHeader;
    }

    // isSupportedTableView 作用：
    // - 判断对象是否是本功能应处理的表格/树表视图；
    // - 排除 QHeaderView 本身，因为它也继承自 QAbstractItemView。
    // 参数 view：候选视图。
    // 返回值：true=支持自动列宽；false=忽略。
    bool isSupportedTableView(QAbstractItemView* view)
    {
        if (view == nullptr || qobject_cast<QHeaderView*>(view) != nullptr)
        {
            return false;
        }
        return qobject_cast<QTableView*>(view) != nullptr || qobject_cast<QTreeView*>(view) != nullptr;
    }

    // collectVisibleSections 作用：
    // - 按当前视觉顺序收集没有隐藏的列；
    // - 同时读取当前列宽作为后续缩放权重。
    // 参数 header：目标横向表头。
    // 参数 minimumWidth：本轮 fit 使用的最小列宽。
    // 返回值：可见列集合；无列时返回空数组。
    std::vector<VisibleSection> collectVisibleSections(QHeaderView* header, const int minimumWidth)
    {
        std::vector<VisibleSection> visibleSections;
        if (header == nullptr)
        {
            return visibleSections;
        }

        const int sectionCount = header->count();
        visibleSections.reserve(static_cast<std::size_t>(std::max(0, sectionCount)));
        for (int visualIndex = 0; visualIndex < sectionCount; ++visualIndex)
        {
            const int logicalIndex = header->logicalIndex(visualIndex);
            if (logicalIndex < 0 || header->isSectionHidden(logicalIndex))
            {
                continue;
            }

            const int sectionWidth = std::max(header->sectionSize(logicalIndex), minimumWidth);
            visibleSections.push_back({ logicalIndex, sectionWidth, sectionWidth });
        }

        return visibleSections;
    }

    // sumFittedWidths 作用：统计当前计算出的目标列宽总和。
    // 参数 sections：可见列集合。
    // 返回值：所有 fittedWidth 的和。
    int sumFittedWidths(const std::vector<VisibleSection>& sections)
    {
        int totalWidth = 0;
        for (const VisibleSection& section : sections)
        {
            totalWidth += section.fittedWidth;
        }
        return totalWidth;
    }

    // countVisibleSections 作用：
    // - 只统计当前没有隐藏的横向列；
    // - 用于根据“实际可见列数”计算本轮最小列宽，避免进程表这类隐藏列很多的表格被过度压缩。
    // 参数 header：目标横向表头。
    // 返回值：可见列数量；无有效表头时返回 0。
    int countVisibleSections(QHeaderView* header)
    {
        if (header == nullptr)
        {
            return 0;
        }

        int visibleSectionCount = 0;
        const int sectionCount = header->count();
        for (int visualIndex = 0; visualIndex < sectionCount; ++visualIndex)
        {
            const int logicalIndex = header->logicalIndex(visualIndex);
            if (logicalIndex >= 0 && !header->isSectionHidden(logicalIndex))
            {
                ++visibleSectionCount;
            }
        }
        return visibleSectionCount;
    }

    // widestSectionIndex 作用：
    // - 找到当前最宽的列；
    // - 多余宽度优先给最宽列，减少大量窄列同时变化带来的视觉抖动。
    // 参数 sections：可见列集合。
    // 返回值：sections 下标；空数组时返回 -1。
    int widestSectionIndex(const std::vector<VisibleSection>& sections)
    {
        if (sections.empty())
        {
            return -1;
        }

        int widestIndex = 0;
        for (int index = 1; index < static_cast<int>(sections.size()); ++index)
        {
            if (sections[static_cast<std::size_t>(index)].fittedWidth >
                sections[static_cast<std::size_t>(widestIndex)].fittedWidth)
            {
                widestIndex = index;
            }
        }
        return widestIndex;
    }

    // fitWidthsToAvailableSpace 作用：
    // - 根据当前列宽比例，把总宽度压缩/扩展到 availableWidth；
    // - 不隐藏列，不改变滚动条策略；
    // - 当列数极多时允许列宽低于常规最小值，尽量避免默认横向滚动条。
    // 参数 sections：可见列集合，会原地写入 fittedWidth。
    // 参数 availableWidth：当前 viewport 可用宽度。
    // 参数 minimumWidth：本轮 fit 使用的最小列宽。
    // 返回值：无。
    void fitWidthsToAvailableSpace(
        std::vector<VisibleSection>& sections,
        const int availableWidth,
        const int minimumWidth)
    {
        if (sections.empty() || availableWidth <= 0)
        {
            return;
        }

        int currentTotalWidth = 0;
        for (const VisibleSection& section : sections)
        {
            currentTotalWidth += section.currentWidth;
        }
        if (currentTotalWidth <= 0)
        {
            currentTotalWidth = static_cast<int>(sections.size()) * minimumWidth;
        }

        if (currentTotalWidth > availableWidth)
        {
            const double scaleRatio = static_cast<double>(availableWidth) /
                static_cast<double>(currentTotalWidth);
            for (VisibleSection& section : sections)
            {
                section.fittedWidth = std::max(
                    minimumWidth,
                    static_cast<int>(static_cast<double>(section.currentWidth) * scaleRatio));
            }
        }

        int fittedTotalWidth = sumFittedWidths(sections);
        while (fittedTotalWidth > availableWidth)
        {
            const int widestIndex = widestSectionIndex(sections);
            if (widestIndex < 0)
            {
                break;
            }

            VisibleSection& widestSection = sections[static_cast<std::size_t>(widestIndex)];
            const int reducibleWidth = widestSection.fittedWidth - minimumWidth;
            if (reducibleWidth <= 0)
            {
                break;
            }

            const int reduceBy = std::min(reducibleWidth, fittedTotalWidth - availableWidth);
            widestSection.fittedWidth -= reduceBy;
            fittedTotalWidth -= reduceBy;
        }

        const int remainingWidth = availableWidth - fittedTotalWidth;
        if (remainingWidth > 0)
        {
            const int widestIndex = widestSectionIndex(sections);
            if (widestIndex >= 0)
            {
                sections[static_cast<std::size_t>(widestIndex)].fittedWidth += remainingWidth;
            }
        }
    }

    // fitViewColumnsToViewport 作用：
    // - 将目标表格当前可见列一次性调整到 viewport 宽度以内；
    // - 统一把列 resize mode 设为 Interactive，保证用户后续可以拖拽调整；
    // - 不设置/隐藏任何滚动条。
    // 参数 view：目标表格/树表视图。
    // 返回值：true=执行过有效 fit；false=当前视图暂不可处理。
    bool fitViewColumnsToViewport(QAbstractItemView* view)
    {
        if (!isSupportedTableView(view) ||
            view->property(AutoFitUserAdjustedProperty).toBool())
        {
            return false;
        }

        QHeaderView* header = horizontalHeaderForView(view);
        if (header == nullptr || header->count() <= 0 || view->viewport() == nullptr)
        {
            return false;
        }

        const int availableWidth = view->viewport()->width() - kViewportPadding;
        if (availableWidth <= 0)
        {
            return false;
        }

        const int visibleSectionCount = countVisibleSections(header);
        if (visibleSectionCount <= 0)
        {
            return false;
        }

        const int dynamicMinimumWidth = std::max(
            kAbsoluteMinimumSectionWidth,
            std::min(kPreferredMinimumSectionWidth, availableWidth / visibleSectionCount));
        if (header->minimumSectionSize() > dynamicMinimumWidth)
        {
            header->setMinimumSectionSize(dynamicMinimumWidth);
        }

        std::vector<VisibleSection> visibleSections =
            collectVisibleSections(header, dynamicMinimumWidth);
        if (visibleSections.empty())
        {
            return false;
        }

        fitWidthsToAvailableSpace(visibleSections, availableWidth, dynamicMinimumWidth);

        const QSignalBlocker headerSignalBlocker(header);
        header->setStretchLastSection(false);
        for (const VisibleSection& section : visibleSections)
        {
            header->setSectionResizeMode(section.logicalIndex, QHeaderView::Interactive);
        }
        for (const VisibleSection& section : visibleSections)
        {
            header->resizeSection(section.logicalIndex, section.fittedWidth);
        }

        return true;
    }

    // scheduleColumnFit 作用：
    // - 把多次 show/resize/layout 事件合并为一次延迟 fit；
    // - 确保 Dock 内部先完成自己的 setColumnWidth/setHeader 配置后，再统一压入 viewport。
    // 参数 view：目标表格/树表视图。
    // 返回值：无。
    void scheduleColumnFit(QAbstractItemView* view)
    {
        if (!isSupportedTableView(view) ||
            view->property(AutoFitScheduledProperty).toBool() ||
            view->property(AutoFitUserAdjustedProperty).toBool())
        {
            return;
        }

        view->setProperty(AutoFitScheduledProperty, true);
        const QPointer<QAbstractItemView> guardedView(view);
        QTimer::singleShot(0, view, [guardedView]()
            {
                if (guardedView.isNull())
                {
                    return;
                }

                guardedView->setProperty(AutoFitScheduledProperty, false);
                fitViewColumnsToViewport(guardedView.data());
            });
    }

    // installHeaderAutoFitHooks 作用：
    // - 给表格横向表头安装一次轻量信号钩子；
    // - 业务代码在 show 后继续 setColumnWidth、隐藏/显示列、替换模型导致列结构变化时，也能再触发一次 fit；
    // - 用户已经手动拖拽过列宽的表格会被 AutoFitUserAdjustedProperty 排除，不会被钩子重新压缩。
    // 参数 view：目标表格/树表视图。
    // 返回值：无。重复调用会由属性去重。
    void installHeaderAutoFitHooks(QAbstractItemView* view)
    {
        if (!isSupportedTableView(view))
        {
            return;
        }

        QHeaderView* header = horizontalHeaderForView(view);
        if (header == nullptr || header->property(AutoFitHeaderHookedProperty).toBool())
        {
            return;
        }

        header->setProperty(AutoFitHeaderHookedProperty, true);
        const QPointer<QAbstractItemView> guardedView(view);

        // sectionResized 覆盖业务 setColumnWidth/resizeSection，也覆盖模型列宽重算。
        QObject::connect(
            header,
            &QHeaderView::sectionResized,
            header,
            [guardedView](int, int, int)
            {
                if (!guardedView.isNull())
                {
                    scheduleColumnFit(guardedView.data());
                }
            });

        // sectionCountChanged 覆盖模型替换、列新增/删除等结构变化。
        QObject::connect(
            header,
            &QHeaderView::sectionCountChanged,
            header,
            [guardedView](int, int)
            {
                if (!guardedView.isNull())
                {
                    scheduleColumnFit(guardedView.data());
                }
            });

        // geometriesChanged 覆盖隐藏列、显示列、表头布局刷新等无 sectionResized 的情况。
        QObject::connect(
            header,
            &QHeaderView::geometriesChanged,
            header,
            [guardedView]()
            {
                if (!guardedView.isNull())
                {
                    scheduleColumnFit(guardedView.data());
                }
            });
    }

    // isNearHeaderResizeBoundary 作用：
    // - 判断鼠标位置是否靠近任意可见列边界；
    // - 仅这种按压会被视为“用户准备拖拽列宽”。
    // 参数 header：目标横向表头。
    // 参数 localPosition：鼠标在 header/viewport 坐标系中的位置。
    // 返回值：true=接近列边界；false=普通表头点击。
    bool isNearHeaderResizeBoundary(QHeaderView* header, const QPoint& localPosition)
    {
        if (header == nullptr || header->count() <= 0)
        {
            return false;
        }

        const int sectionCount = header->count();
        for (int visualIndex = 0; visualIndex < sectionCount; ++visualIndex)
        {
            const int logicalIndex = header->logicalIndex(visualIndex);
            if (logicalIndex < 0 || header->isSectionHidden(logicalIndex))
            {
                continue;
            }

            const int sectionLeft = header->sectionViewportPosition(logicalIndex);
            const int sectionRight = sectionLeft + header->sectionSize(logicalIndex);
            if (std::abs(localPosition.x() - sectionRight) <= kResizeGripMargin)
            {
                return true;
            }
        }

        return false;
    }

    // updateUserColumnResizeState 作用：
    // - 捕获用户在表头列边界附近按下并拖动鼠标的动作；
    // - 只有拖动距离超过 QApplication 的拖拽阈值后，才视作用户手动调列宽；
    // - 一旦用户开始手动调列宽，该表后续不再被全局自动 fit 干预。
    // 参数 watchedObject：全局事件源对象。
    // 参数 eventObject：当前事件。
    // 返回值：无。
    void updateUserColumnResizeState(QObject* watchedObject, QEvent* eventObject)
    {
        if (eventObject == nullptr)
        {
            return;
        }

        QHeaderView* header = eventObjectToHorizontalHeader(watchedObject);
        if (header == nullptr)
        {
            return;
        }

        if (eventObject->type() == QEvent::MouseButtonPress)
        {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(eventObject);
            if (mouseEvent->button() != Qt::LeftButton)
            {
                return;
            }

            const QPoint headerPosition = header->viewport() == watchedObject
                ? mouseEvent->position().toPoint()
                : header->mapFromGlobal(mouseEvent->globalPosition().toPoint());
            if (!isNearHeaderResizeBoundary(header, headerPosition))
            {
                return;
            }

            header->setProperty(AutoFitResizePendingProperty, true);
            header->setProperty(AutoFitResizePressPositionProperty, headerPosition);
            return;
        }

        if (eventObject->type() == QEvent::MouseMove)
        {
            if (!header->property(AutoFitResizePendingProperty).toBool())
            {
                return;
            }

            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(eventObject);
            if ((mouseEvent->buttons() & Qt::LeftButton) == 0)
            {
                header->setProperty(AutoFitResizePendingProperty, false);
                return;
            }

            const QPoint headerPosition = header->viewport() == watchedObject
                ? mouseEvent->position().toPoint()
                : header->mapFromGlobal(mouseEvent->globalPosition().toPoint());
            const QPoint pressPosition =
                header->property(AutoFitResizePressPositionProperty).toPoint();
            if ((headerPosition - pressPosition).manhattanLength() < QApplication::startDragDistance())
            {
                return;
            }

            header->setProperty(AutoFitResizePendingProperty, false);
            if (QAbstractItemView* view = viewForHorizontalHeader(header))
            {
                view->setProperty(AutoFitUserAdjustedProperty, true);
            }
            return;
        }

        if (eventObject->type() == QEvent::MouseButtonRelease)
        {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(eventObject);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                header->setProperty(AutoFitResizePendingProperty, false);
            }
        }
    }

    class GlobalTableColumnAutoFitFilter final : public QObject
    {
    public:
        // 构造函数作用：
        // - 绑定 QApplication 为父对象；
        // - 生命周期跟随应用进程，无需手动释放。
        // 参数 parentObject：通常为 QApplication。
        explicit GlobalTableColumnAutoFitFilter(QObject* parentObject)
            : QObject(parentObject)
        {
        }

    protected:
        // eventFilter 作用：
        // - 全局捕获表格 show/resize/layout 事件并延迟执行列宽 fit；
        // - 全局捕获表头边界鼠标按压并标记用户接管列宽；
        // - 返回 false，不吞掉业务事件。
        // 参数 watchedObject：事件源对象。
        // 参数 eventObject：当前事件。
        // 返回值：始终继续 Qt 默认事件分发。
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            updateUserColumnResizeState(watchedObject, eventObject);

            QAbstractItemView* view = qobject_cast<QAbstractItemView*>(watchedObject);
            if (isSupportedTableView(view) && eventObject != nullptr)
            {
                installHeaderAutoFitHooks(view);

                const QEvent::Type eventType = eventObject->type();
                if (eventType == QEvent::Show ||
                    eventType == QEvent::Resize ||
                    eventType == QEvent::Polish ||
                    eventType == QEvent::LayoutRequest ||
                    eventType == QEvent::StyleChange)
                {
                    scheduleColumnFit(view);
                }
            }

            return QObject::eventFilter(watchedObject, eventObject);
        }
    };
}

namespace ks::ui
{
    void InstallGlobalTableColumnAutoFit(QApplication* appInstance)
    {
        if (appInstance == nullptr ||
            appInstance->property(AutoFitInstalledProperty).toBool())
        {
            return;
        }

        auto* filter = new GlobalTableColumnAutoFitFilter(appInstance);
        appInstance->installEventFilter(filter);
        appInstance->setProperty(AutoFitInstalledProperty, true);
    }

    void RequestTableColumnAutoFit(QAbstractItemView* view)
    {
        scheduleColumnFit(view);
    }
}
