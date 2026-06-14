#include "TableColumnAutoFit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QModelIndex>
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

    // kHeaderHorizontalPadding/kCellHorizontalPadding 作用：
    // - 用字体度量估算列宽时补偿 item margin、网格线、排序箭头和图标留白；
    // - 只影响自动计算的默认宽度，不改变实际绘制样式。
    constexpr int kHeaderHorizontalPadding = 30;
    constexpr int kCellHorizontalPadding = 22;

    // kContentSampleRowLimit 作用：
    // - 自动列宽只抽样少量行，避免大表在 UI 线程做完整 resizeToContents 扫描；
    // - 表头和前若干可见/根行通常足以识别“PID/CPU/状态”等短列和“路径/命令行”等长列。
    constexpr int kContentSampleRowLimit = 48;

    // kMeasuredTextCharacterLimit 作用：
    // - 极长路径/命令行只取前一段估算，避免单列权重无限放大；
    // - 列仍会被识别为长列，并在压缩/分配剩余空间时获得更高权重。
    constexpr int kMeasuredTextCharacterLimit = 180;

    // kLongColumnExpansionThreshold 作用：
    // - 首选宽度达到该阈值的列才参与“填满剩余宽度”；
    // - 短文本列即使表格很宽，也不会被均分成很宽的一列。
    constexpr int kLongColumnExpansionThreshold = 120;

    // kResizeGripMargin 作用：
    // - 识别用户是否在表头列边界附近按下鼠标；
    // - 只有这类动作会被视作手动调列宽，普通点击排序不禁用自动 fit。
    constexpr int kResizeGripMargin = 6;

    struct VisibleSection
    {
        int logicalIndex = -1;  // logicalIndex：QHeaderView 逻辑列号。
        int currentWidth = 0;   // currentWidth：自动 fit 前的当前列宽快照，保留给调试与后续扩展。
        int preferredWidth = 0; // preferredWidth：根据表头和抽样内容估算出的首选列宽。
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

    // measuredTextWidth 作用：
    // - 用指定字体度量一段显示文本所需宽度；
    // - 对换行文本取单行化后的前缀，避免超长内容把某列权重无限放大；
    // - 返回值只包含文字像素宽度，不包含 item/header padding。
    int measuredTextWidth(const QFontMetrics& fontMetrics, const QString& sourceText)
    {
        QString measuredText = sourceText.simplified();
        if (measuredText.isEmpty())
        {
            return 0;
        }

        if (measuredText.size() > kMeasuredTextCharacterLimit)
        {
            measuredText = measuredText.left(kMeasuredTextCharacterLimit);
        }

        return std::max(0, fontMetrics.horizontalAdvance(measuredText));
    }

    // headerPreferredWidth 作用：
    // - 根据模型横向表头文本估算某列表头所需宽度；
    // - 若该列显示排序箭头，则额外补偿箭头占位；
    // - 返回值包含表头 padding，可直接参与列宽首选值计算。
    int headerPreferredWidth(
        QAbstractItemView* view,
        QHeaderView* header,
        const int logicalIndex)
    {
        if (view == nullptr || header == nullptr || logicalIndex < 0)
        {
            return 0;
        }

        QAbstractItemModel* model = view->model();
        const QVariant headerValue = model != nullptr
            ? model->headerData(logicalIndex, Qt::Horizontal, Qt::DisplayRole)
            : QVariant();
        const int textWidth = measuredTextWidth(header->fontMetrics(), headerValue.toString());
        const int sortIndicatorReserve = header->isSortIndicatorShown()
            && header->sortIndicatorSection() == logicalIndex
            ? 18
            : 0;
        return textWidth + kHeaderHorizontalPadding + sortIndicatorReserve;
    }

    // sampleIndexPreferredWidth 作用：
    // - 估算单个模型索引的显示内容所需宽度；
    // - DisplayRole 负责文字，DecorationRole/CheckStateRole 只按固定占位补偿；
    // - treeIndentWidth 用于 QTreeView 第 0 列，补偿展开层级缩进。
    int sampleIndexPreferredWidth(
        const QModelIndex& index,
        const QFontMetrics& fontMetrics,
        const int treeIndentWidth)
    {
        if (!index.isValid())
        {
            return 0;
        }

        int widthValue = measuredTextWidth(fontMetrics, index.data(Qt::DisplayRole).toString());
        if (index.data(Qt::DecorationRole).isValid())
        {
            widthValue += 20;
        }
        if (index.data(Qt::CheckStateRole).isValid())
        {
            widthValue += 18;
        }

        return widthValue + treeIndentWidth + kCellHorizontalPadding;
    }

    // contentPreferredWidth 作用：
    // - 抽样模型前若干行，估算某列内容首选宽度；
    // - QTreeView 会在已展开节点内继续浅层采样，避免树表子项内容完全被忽略；
    // - 返回值包含单元格 padding，可直接参与列宽首选值计算。
    int contentPreferredWidth(
        QAbstractItemView* view,
        const int logicalIndex)
    {
        if (view == nullptr || logicalIndex < 0 || view->model() == nullptr)
        {
            return 0;
        }

        QAbstractItemModel* model = view->model();
        QTreeView* treeView = qobject_cast<QTreeView*>(view);
        const QFontMetrics cellFontMetrics(view->font());
        const QModelIndex rootIndex = view->rootIndex();
        int sampledRowCount = 0;
        int preferredWidth = 0;

        // sampleParent 作用：
        // - 从 parentIndex 下按顺序采样若干行；
        // - 对展开的树节点递归采样，直到达到全局抽样上限；
        // - 返回行为：无返回值，通过 sampledRowCount/preferredWidth 累积结果。
        auto sampleParent =
            [&](const QModelIndex& parentIndex, const int depthValue, auto&& sampleParentRef) -> void
            {
                if (sampledRowCount >= kContentSampleRowLimit)
                {
                    return;
                }

                const int rowCount = model->rowCount(parentIndex);
                for (int rowIndex = 0;
                    rowIndex < rowCount && sampledRowCount < kContentSampleRowLimit;
                    ++rowIndex)
                {
                    const QModelIndex cellIndex = model->index(rowIndex, logicalIndex, parentIndex);
                    if (cellIndex.isValid())
                    {
                        const int treeIndentWidth =
                            treeView != nullptr && logicalIndex == 0
                            ? std::max(0, depthValue) * treeView->indentation()
                            : 0;
                        preferredWidth = std::max(
                            preferredWidth,
                            sampleIndexPreferredWidth(cellIndex, cellFontMetrics, treeIndentWidth));
                        ++sampledRowCount;
                    }

                    if (treeView == nullptr || sampledRowCount >= kContentSampleRowLimit)
                    {
                        continue;
                    }

                    const QModelIndex treeIndex = model->index(rowIndex, 0, parentIndex);
                    if (treeIndex.isValid() && treeView->isExpanded(treeIndex))
                    {
                        sampleParentRef(treeIndex, depthValue + 1, sampleParentRef);
                    }
                }
            };

        sampleParent(rootIndex, 0, sampleParent);
        return preferredWidth;
    }

    // sectionPreferredWidth 作用：
    // - 综合表头、抽样内容、当前最小列宽，得到某列的内容感知首选宽度；
    // - 返回值不会低于 minimumWidth，但可能高于 viewport，后续 fit 阶段会统一压缩。
    int sectionPreferredWidth(
        QAbstractItemView* view,
        QHeaderView* header,
        const int logicalIndex,
        const int minimumWidth)
    {
        const int preferredWidth = std::max(
            headerPreferredWidth(view, header, logicalIndex),
            contentPreferredWidth(view, logicalIndex));
        return std::max(minimumWidth, preferredWidth);
    }

    // collectVisibleSections 作用：
    // - 按当前视觉顺序收集没有隐藏的列；
    // - 同时读取当前列宽与内容感知首选宽度，后续再整体压入 viewport。
    // 参数 view：目标表格/树表视图。
    // 参数 header：目标横向表头。
    // 参数 minimumWidth：本轮 fit 使用的最小列宽。
    // 返回值：可见列集合；无列时返回空数组。
    std::vector<VisibleSection> collectVisibleSections(
        QAbstractItemView* view,
        QHeaderView* header,
        const int minimumWidth)
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
            const int preferredWidth = sectionPreferredWidth(
                view,
                header,
                logicalIndex,
                minimumWidth);
            visibleSections.push_back({
                logicalIndex,
                sectionWidth,
                preferredWidth,
                preferredWidth });
        }

        return visibleSections;
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

    // fitWidthsToAvailableSpace 作用：
    // - 先按内容感知 preferredWidth 排列列宽；
    // - 若总宽超过 viewport，则按“首选宽度 - 最小宽度”的权重压缩到 availableWidth 内；
    // - 若总宽小于 viewport，则只把剩余空间分给长内容列，短字段列不会被强行均分放大；
    // - 不隐藏列，不改变滚动条策略。
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

        const int sectionCount = static_cast<int>(sections.size());
        for (VisibleSection& section : sections)
        {
            section.fittedWidth = std::max(minimumWidth, section.preferredWidth);
        }

        const auto totalFittedWidth =
            [&sections]() -> int
            {
                int totalWidth = 0;
                for (const VisibleSection& section : sections)
                {
                    totalWidth += section.fittedWidth;
                }
                return totalWidth;
            };

        int fittedTotalWidth = totalFittedWidth();
        if (fittedTotalWidth == availableWidth)
        {
            return;
        }

        if (fittedTotalWidth < availableWidth)
        {
            int remainingWidth = availableWidth - fittedTotalWidth;
            int totalExpansionWeight = 0;
            for (const VisibleSection& section : sections)
            {
                if (section.preferredWidth >= kLongColumnExpansionThreshold)
                {
                    totalExpansionWeight += std::max(
                        1,
                        section.preferredWidth - kLongColumnExpansionThreshold);
                }
            }

            // 没有明显长内容列时保留右侧空白，而不是把所有短列平均拉宽。
            if (totalExpansionWeight <= 0)
            {
                return;
            }

            for (VisibleSection& section : sections)
            {
                if (section.preferredWidth < kLongColumnExpansionThreshold)
                {
                    continue;
                }

                const int expansionWeight = std::max(
                    1,
                    section.preferredWidth - kLongColumnExpansionThreshold);
                const int extraWidth =
                    remainingWidth * expansionWeight / totalExpansionWeight;
                section.fittedWidth += extraWidth;
            }

            int finalTotalWidth = totalFittedWidth();
            for (VisibleSection& section : sections)
            {
                if (finalTotalWidth >= availableWidth)
                {
                    break;
                }
                if (section.preferredWidth < kLongColumnExpansionThreshold)
                {
                    continue;
                }
                ++section.fittedWidth;
                ++finalTotalWidth;
            }
            return;
        }

        const int minimumTotalWidth = sectionCount * minimumWidth;
        if (minimumTotalWidth >= availableWidth)
        {
            const int baseWidth = std::max(1, availableWidth / sectionCount);
            int remainingWidth = availableWidth - baseWidth * sectionCount;
            for (VisibleSection& section : sections)
            {
                section.fittedWidth = baseWidth;
                if (remainingWidth > 0)
                {
                    ++section.fittedWidth;
                    --remainingWidth;
                }
            }
            return;
        }

        int remainingAssignableWidth = availableWidth - minimumTotalWidth;
        int totalContentWeight = 0;
        for (const VisibleSection& section : sections)
        {
            totalContentWeight += std::max(0, section.preferredWidth - minimumWidth);
        }

        for (VisibleSection& section : sections)
        {
            section.fittedWidth = minimumWidth;
        }

        if (totalContentWeight <= 0)
        {
            for (VisibleSection& section : sections)
            {
                if (remainingAssignableWidth <= 0)
                {
                    break;
                }
                ++section.fittedWidth;
                --remainingAssignableWidth;
            }
            return;
        }

        for (VisibleSection& section : sections)
        {
            const int contentWeight = std::max(0, section.preferredWidth - minimumWidth);
            const int extraWidth =
                remainingAssignableWidth * contentWeight / totalContentWeight;
            section.fittedWidth += extraWidth;
        }

        int finalTotalWidth = totalFittedWidth();
        while (finalTotalWidth < availableWidth)
        {
            auto targetIt = std::max_element(
                sections.begin(),
                sections.end(),
                [](const VisibleSection& leftSection, const VisibleSection& rightSection)
                {
                    return (leftSection.preferredWidth - leftSection.fittedWidth)
                        < (rightSection.preferredWidth - rightSection.fittedWidth);
                });
            if (targetIt == sections.end())
            {
                break;
            }
            ++targetIt->fittedWidth;
            ++finalTotalWidth;
        }

        while (finalTotalWidth > availableWidth)
        {
            auto targetIt = std::max_element(
                sections.begin(),
                sections.end(),
                [](const VisibleSection& leftSection, const VisibleSection& rightSection)
                {
                    return leftSection.fittedWidth < rightSection.fittedWidth;
                });
            if (targetIt == sections.end() || targetIt->fittedWidth <= minimumWidth)
            {
                break;
            }
            --targetIt->fittedWidth;
            --finalTotalWidth;
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
            collectVisibleSections(view, header, dynamicMinimumWidth);
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
