#include "../UI/StructuredFieldView.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/SegmentedControl.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/VisibleTableWidget.h"
// ============================================================
// MinidumpDock.Tables.cpp
// 作用：
// - 实现转储解析结果的全部渲染逻辑：
//   诊断结论/肇事模块/概览/异常/调用栈/寄存器/流目录/模块/线程/内存/
//   句柄/已卸载模块共十二张表 + 全文报告；
// - 表格控件在 MinidumpDock.cpp 预创建，这里只负责清空重填与挂载页签，
//   语言切换时重进本文件即可完成重译；
// - 宽表通过 createStructuredTablePage 提供互补 A/B/C 列组预设与
//   表头右键逐列显隐（遵循项目 A/B 列组规范）。
// ============================================================

#include "MinidumpDock.h"
#include "../UI/ThemeBinding.h"

#include "DumpAnalyzer.h"
#include "DumpByteView.h"
#include "DumpMemoryView.h"
#include "DumpSymbolResolver.h"
#include "Internationalization/LanguageManager.h"
#include "MinidumpFormat.h"
#include "UI/CodeEditorWidget.h"
#include "UI/AsyncOperation.h"
#include "UI/MemoryWorkbench/SnapshotWorkbenchWidget.h"
#include "UI/PerfTrace.h"
#include "UI/TableHeaderSortingSupport.h"
#include "theme.h"

#include <QAction>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QElapsedTimer>
#include <QTimer>
#include "UI/UiCommitCoordinator.h"
#include "UI/TableColumnAutoFit.h"
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <vector>

struct MinidumpTableProjectionState
{
    QPointer<QTableWidget> table;
    std::function<void(int)> fillRow;
    std::uint64_t generation = 0;
    int totalRows = 0;
    int nextRow = 0;
    int nextColumn = 0;
    int nextHeightRow = 0;
    int originalPrecision = 0;
    bool fitRows = false;
};

namespace
{
    // kReportMemoryRowLimit：全文报告里内存区域最多列出的行数。
    constexpr std::size_t kReportMemoryRowLimit = 2000;

    // hexText 作用：把数值格式化成 0x 大写十六进制（渲染层通用）。
    QString hexText(const std::uint64_t value)
    {
        return QStringLiteral("0x%1").arg(QString::number(value, 16).toUpper());
    }

    // makeItem 作用：创建一个只读表格单元格。
    // 传入 text 单元格文本；返回新建的 QTableWidgetItem。
    QTableWidgetItem* makeItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    // beginFill 作用：批量填充前关闭刷新与排序并清空内容。
    // 注意 endFill 只恢复刷新，不重新打开排序——本页所有表都以解析产出的
    // 原始顺序呈现（调用栈的帧序、模块的加载顺序本身就是信息），
    // 让用户点表头打乱它没有意义。
    void beginFill(QTableWidget* const table)
    {
        table->setUpdatesEnabled(false);
        table->setSortingEnabled(false);
        table->clearContents();
        table->setRowCount(0);
        // 采集：记录填充起点，供 endFill 计算单表填充总耗时。
        table->setProperty("perf_fill_start_ms",
            QVariant::fromValue<qlonglong>(ks::ui::PerfTraceNowMs()));
    }


}

QTableWidget* MinidumpDock::createReadOnlyTable(QWidget* parent) const
{
    // table：统一风格的只读表格；与 ScannerDock 保持一致的交互配置。
    auto* table = new QTableWidget(parent);
    // 这里创建的调用栈、模块、线程及内存表都来自一次静态转储，不需要现场快照条。
    ks::ui::SetTableActionBarMode(table, ks::ui::TableActionBarMode::Compact);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideMiddle);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionsMovable(true);
    table->horizontalHeader()->setStretchLastSection(true);
    // 表头左对齐：末列被拉伸后会很宽，居中的表头文字会飘到列中央，
    // 和左对齐的数据完全对不上。数据表的惯例本来也是左对齐。
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // 转储解析表的帧序、加载序和采集序本身属于证据，禁用通用表头排序。
    ks::ui::SetTableHeaderClickSortingEnabled(table, false);
    return table;
}

void MinidumpDock::clearResultTabs()
{
    // 只摘下页签不销毁控件：全部表格/编辑器都是预创建的复用成员。
    // removeTab 不会改变父子关系，被摘下的控件仍是 m_resultTabs 的子控件；
    // 若不显式隐藏，它会作为普通子控件浮在当前页之上，看起来就是"表格重叠"。
    while (m_resultTabs->count() > 0)
    {
        QWidget* const detachedPage = m_resultTabs->widget(0);
        m_resultTabs->removeTab(0);
        if (detachedPage != nullptr)
        {
            detachedPage->hide();
        }
    }
    // 原始字节块页持有前一次转储的复制数据，换结果时销毁这些子页。
    while (m_rawMemoryTabs->count() > 0)
    {
        QWidget* const blockPage = m_rawMemoryTabs->widget(0);
        m_rawMemoryTabs->removeTab(0);
        delete blockPage;
    }
}

QWidget* MinidumpDock::createStructuredTablePage(
    QTableWidget* table,
    const int columnCount) const
{
    // 五列以内无需拆组；返回原表可避免增加无意义的控制条。
    if (table == nullptr || columnCount <= 5)
    {
        return table;
    }

    // page/pageLayout：宽表页由紧贴的 A/B/C 控件和原只读表组成。
    auto* page = new QWidget(m_resultTabs); // page：本结构表的外层页面。
    auto* pageLayout = new QVBoxLayout(page); // pageLayout：纵向排列列组按钮与数据表。
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(4);
    auto* buttonLayout = new QHBoxLayout(); // buttonLayout：无间距排列 A/B/C 按钮。
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(0);

    // groupCount：最多三组；第一列作为识别键在各组保留，其余列平均分配。
    const int remainingColumns = columnCount - 1; // remainingColumns：除识别列外的字段数。
    const int groupCount = std::clamp((remainingColumns + 3) / 4, 2, 3); // groupCount：实际列组数。
    const int columnsPerGroup = (remainingColumns + groupCount - 1) / groupCount; // columnsPerGroup：每组字段上限。
    auto groups = std::make_shared<std::vector<std::vector<int>>>(); // groups：每个预设显示的列索引。
    auto buttons = std::make_shared<std::vector<QPointer<QPushButton>>>(); // buttons：可安全失效的列组按钮。

    // 动态宽表列组共用分段底面，业务仅登记选择状态，不保存旧主题样式快照。
    for (int groupIndex = 0; groupIndex < groupCount; ++groupIndex)
    {
        // columns：每组保留第 0 列，并加入属于本组的互补字段。
        std::vector<int> columns{ 0 }; // columns：当前 A/B/C 预设的可见列。
        const int firstColumn = 1 + groupIndex * columnsPerGroup; // firstColumn：当前分组首列。
        const int endColumn = std::min(columnCount, firstColumn + columnsPerGroup); // endColumn：当前分组尾后列。
        for (int column = firstColumn; column < endColumn; ++column)
        {
            columns.push_back(column);
        }
        groups->push_back(std::move(columns));

        // groupButton：A/B/C 采用紧贴布局，并通过悬停说明当前行为。
        const char16_t groupLetter = static_cast<char16_t>(u'A' + groupIndex); // groupLetter：当前组的字母标识。
        auto* groupButton = new QPushButton(
            QString(QChar(groupLetter)),
            page); // groupButton：切换到当前互补字段组。
        groupButton->setMinimumWidth(36);
        groupButton->setToolTip(
            translated(
                "minidump.column_view.tooltip",
                "列组 %1：显示一组互补字段；可在表头右键自定义列。")
                .arg(groupButton->text()));
        ks::ui::SetSegmentedButtonSelected(groupButton, groupIndex == 0);
        buttons->push_back(groupButton);
        buttonLayout->addWidget(groupButton);
    }
    // 此工厂覆盖所有动态宽表；原按钮、提示、列显隐与右键自定义语义保持一致。
    QList<QAbstractButton*> segments;
    for (const QPointer<QPushButton>& button : *buttons)
    {
        segments.push_back(button.data());
    }
    ks::ui::NormalizeToolbarRow(buttonLayout, 0);
    ks::ui::InstallSegmentedButtons(buttonLayout, segments);
    buttonLayout->addStretch(1);
    pageLayout->addLayout(buttonLayout);
    pageLayout->addWidget(table, 1);

    // currentGroup：当前生效的预设下标。本函数在 buildUi 阶段被调用，
    // 那时表格还没有任何列，applyGroup 里的 setColumnHidden 全是空操作
    //（Qt 对越界的 section 直接忽略）。于是 A 按钮被高亮成“当前视图”，
    // 表格却在渲染后显示了全部列——高亮状态与实际不符。
    // 记下当前下标，等列数真正就位时再重放一次。
    auto currentGroup = std::make_shared<int>(0);

    // applyGroup：切换预设时先隐藏所有列，再仅显示目标组并更新按钮主题。
    const auto applyGroup =
        [table, groups, buttons, currentGroup](const int groupIndex)
        {
            *currentGroup = groupIndex;
            for (int column = 0; column < table->columnCount(); ++column)
            {
                table->setColumnHidden(column, true);
            }
            for (const int column : groups->at(static_cast<std::size_t>(groupIndex)))
            {
                table->setColumnHidden(column, false);
            }
            for (int buttonIndex = 0; buttonIndex < static_cast<int>(buttons->size()); ++buttonIndex)
            {
                if (buttons->at(static_cast<std::size_t>(buttonIndex)))
                {
                    ks::ui::SetSegmentedButtonSelected(
                        buttons->at(static_cast<std::size_t>(buttonIndex)), buttonIndex == groupIndex);
                }
            }
        };

    for (int groupIndex = 0; groupIndex < static_cast<int>(buttons->size()); ++groupIndex)
    {
        // groupButton：当前按钮连接到自己的预设索引，点击不会改变表内数据。
        QPushButton* groupButton = buttons->at(static_cast<std::size_t>(groupIndex)); // groupButton：待连接按钮。
        connect(
            groupButton,
            &QPushButton::clicked,
            page,
            [applyGroup, groupIndex]() { applyGroup(groupIndex); });
    }
    applyGroup(0);

    // 列数从 0 变成实际值时（renderResult 里的 setColumnCount）重放当前预设。
    // 只有这时候 setColumnHidden 才真正生效，否则按钮高亮与表格内容会长期不一致。
    // 后续重复解析时列数不变、信号不发，用户手动选择的列组因此得以保留。
    connect(
        table->horizontalHeader(),
        &QHeaderView::sectionCountChanged,
        page,
        [applyGroup, currentGroup](const int oldCount, const int newCount)
        {
            if (oldCount == 0 && newCount > 0)
            {
                applyGroup(*currentGroup);
            }
        });

    // 表头右键菜单允许逐列显隐；每次手动调整后取消 A/B/C 激活色，表示自定义布局。
    // QMenu 按项目规范显式设置背景/文字/选中态/禁用态，避免透明继承导致黑底黑字。
    QHeaderView* tableHeader = table->horizontalHeader(); // tableHeader：承载列菜单的表头。
    tableHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        tableHeader,
        &QHeaderView::customContextMenuRequested,
        page,
        [this, table, tableHeader, buttons](const QPoint& position)
        {
            QMenu menu(tableHeader); // menu：仅在当前右键操作期间存在的列显隐菜单。
            menu.setStyleSheet(QStringLiteral(
                "QMenu { background:%1; color:%2; border:1px solid %3; }"
                "QMenu::item { padding:5px 22px; }"
                "QMenu::item:selected { background:%4; color:%5; }"
                "QMenu::item:disabled { color:%6; }")
                .arg(
                    KswordTheme::SurfaceHex(),
                    KswordTheme::TextPrimaryHex(),
                    KswordTheme::BorderHex(),
                    KswordTheme::AccentHex(KswordTheme::AccentRole::Blue),
                    KswordTheme::OnAccentDynamicHex(),
                    KswordTheme::TextDisabledColorHex()));

            // columnAction：勾选状态直接反映当前列可见性。
            for (int column = 0; column < table->columnCount(); ++column)
            {
                QAction* columnAction = menu.addAction( // columnAction：控制单列显隐的菜单项。
                    table->horizontalHeaderItem(column) != nullptr
                        ? table->horizontalHeaderItem(column)->text()
                        : translated("minidump.column.unnamed", "未命名列"));
                columnAction->setCheckable(true);
                columnAction->setChecked(!table->isColumnHidden(column));
                connect(
                    columnAction,
                    &QAction::toggled,
                    &menu,
                    [table, column, buttons](const bool visible)
                    {
                        table->setColumnHidden(column, !visible);
                        for (const QPointer<QPushButton>& button : *buttons)
                        {
                            if (button)
                            {
                                ks::ui::SetSegmentedButtonSelected(button, false);
                            }
                        }
                    });
            }
            menu.exec(tableHeader->mapToGlobal(position));
        });
    return page;
}

void MinidumpDock::registerResultTab(QWidget* page, const char* key, const char* sourceTitle)
{
    page->setProperty("ksword_navigation_descriptor_id", QString::fromLatin1(key));
    const QString title = translated(key, sourceTitle);
    const int index = m_resultTabs->indexOf(page);
    if (index < 0) m_resultTabs->addTab(page, title);
    else m_resultTabs->setTabText(index, title);
}

void MinidumpDock::renderResult(const ks::minidump::DumpParseResult& result)
{
    ks::ui::PerfScope renderPerf("perf_dump_render", QStringLiteral("render_result"));
    const QSignalBlocker tabSignalsBlocker(m_resultTabs);
    m_buildingResultCatalog = true;
    cancelResultTableFills();
    if (m_reportBuildOperation) m_reportBuildOperation->cancel();
    m_cachedReportSource.reset();
    m_cachedReportDocument.reset();
    m_reportEditor->setDocument({});
    m_reportEditor->setEnabled(true);
    clearResultTabs();
    m_memoryView->clearData();
    m_projectedResultPages.clear();
    m_projectedRawMemoryBlocks.clear();
    projectResult(result, nullptr);
    m_buildingResultCatalog = false;
    renderCurrentResultPage();
}

void MinidumpDock::renderCurrentResultPage()
{
    if (m_buildingResultCatalog || !m_lastResult || !isVisible()) return;
    QWidget* const page = m_resultTabs->currentWidget();
    if (page == nullptr) return;
    if (m_projectedResultPages.contains(page))
    {
        if (page == m_rawMemoryTabs) renderCurrentRawMemoryPage();
        scheduleResultTableFill();
        return;
    }
    if (auto* coordinator = ks::ui::UiCommitCoordinator::forApplication())
    {
        QTableWidget* const table = currentResultTable();
        if (table != nullptr && coordinator->deferIfBlocked(this,
                QStringLiteral("minidump_result_projection"), {table},
                [this]() { renderCurrentResultPage(); })) return;
    }
    m_projectedResultPages.insert(page);
    projectResult(*m_lastResult, page);
}

void MinidumpDock::retranslateResultPages()
{
    if (!m_lastResult) return;
    // These controls own canonical source data and their own LanguageChange behavior.
    // Keep the user's current address, raw editor state and full report document.
    QSet<QWidget*> persistentPages;
    for (QWidget* const page : {static_cast<QWidget*>(m_rawMemoryTabs),
            static_cast<QWidget*>(m_memoryView), static_cast<QWidget*>(m_reportEditor)})
    {
        if (m_projectedResultPages.contains(page)) persistentPages.insert(page);
    }
    cancelResultTableFills();
    m_projectedResultPages.swap(persistentPages);
    const QSignalBlocker tabSignalsBlocker(m_resultTabs);
    m_buildingResultCatalog = true;
    projectResult(*m_lastResult, nullptr);
    m_buildingResultCatalog = false;
    for (int index = 0; index < m_rawMemoryTabs->count(); ++index)
    {
        const QString title = QStringLiteral("%1 %2")
            .arg(translated("minidump.raw.block", "数据块")).arg(index + 1);
        m_rawMemoryTabs->setTabText(index, title);
        // The original metadata title was rendered in the active language. Refresh only
        // this small document; retain the professional editor and its reading state.
        for (auto* metadata : m_rawMemoryTabs->widget(index)
                ->findChildren<ks::ui::StructuredFieldView*>(QString(), Qt::FindDirectChildrenOnly))
        {
            auto document = metadata->document();
            document.title = title;
            metadata->setDocument(document);
        }
    }
    renderCurrentResultPage();
}

void MinidumpDock::renderCurrentRawMemoryPage()
{
    if (m_buildingResultCatalog || m_buildingRawMemoryCatalog || !m_lastResult ||
        !isVisible() || m_resultTabs->currentWidget() != m_rawMemoryTabs) return;
    const int current = m_rawMemoryTabs->currentIndex();
    if (current < 0 || m_projectedRawMemoryBlocks.contains(current) ||
        static_cast<std::size_t>(current) >= m_lastResult->byteBlocks.size()) return;
    m_projectedRawMemoryBlocks.insert(current);
    const auto& result = *m_lastResult;
    const auto index = static_cast<std::size_t>(current);
    QWidget* const page = m_rawMemoryTabs->widget(current);
    const ks::minidump::DumpByteBlock& block = result.byteBlocks[index];
    const std::uint64_t previewBytes = block.previewBytes.size();
    const std::uint64_t omittedBytes = block.capturedBytes > previewBytes
        ? block.capturedBytes - previewBytes : 0;
    const QString blockTitle = QStringLiteral("%1 %2")
        .arg(translated("minidump.raw.block", "数据块")).arg(index + 1);
    ks::ui::FieldDocument metadataDocument;
    metadataDocument.title = blockTitle;
    metadataDocument.field(QStringLiteral("来源"), block.source, true);
    metadataDocument.field(QStringLiteral("虚拟地址"), block.hasVirtualAddress ? hexText(block.address) : QStringLiteral("不适用"));
    metadataDocument.field(QStringLiteral("文件偏移"), hexText(block.fileOffset));
    metadataDocument.field(QStringLiteral("完整捕获大小（字节）"), QString::number(block.capturedBytes));
    metadataDocument.field(QStringLiteral("预览大小（字节）"), QString::number(previewBytes));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* metadata = new ks::ui::StructuredFieldView(page);
    metadata->setValueModelProjection();
    metadata->setHiddenProjectionDeferred();
    metadata->setMaximumHeight(145);
    metadata->setDocument(metadataDocument);
    layout->addWidget(metadata);
    if (block.hasVirtualAddress)
    {
        auto* editor = new ks::ui::SnapshotWorkbenchWidget(page);
        editor->setEditable(false);
        const QByteArray bytes(block.previewBytes.empty() ? nullptr
            : reinterpret_cast<const char*>(block.previewBytes.data()),
            static_cast<qsizetype>(previewBytes));
        const auto architecture = result.pointerSize == 4
            ? ks::ui::DisassemblyArchitecture::X86 : ks::ui::DisassemblyArchitecture::X64;
        const auto anchor = result.faultingAddress >= block.address
            && result.faultingAddress - block.address < previewBytes
            ? result.faultingAddress : block.address;
        editor->setSnapshot(bytes, block.address, architecture, anchor,
            QStringLiteral("dump-block:%1:%2:%3:%4")
                .arg(result.filePath).arg(result.fileSize)
                .arg(result.fileLastModifiedUtcMs).arg(block.fileOffset));
        layout->addWidget(editor, 1);
    }
    else
    {
        const QString rawText = ks::minidump::FormatDumpBytes(block.fileOffset,
            block.previewBytes.empty() ? nullptr : block.previewBytes.data(),
            previewBytes, omittedBytes);
        auto* report = new CodeEditorWidget(page);
        report->setReadOnly(true);
        report->setRawText(rawText);
        layout->addWidget(report, 1);
    }
}

QTableWidget* MinidumpDock::currentResultTable() const
{
    QWidget* const page = m_resultTabs->currentWidget();
    if (page == m_modulePage) return m_moduleTable;
    if (page == m_threadPage) return m_threadTable;
    if (page == m_memoryPage) return m_memoryTable;
    if (page == m_handlePage) return m_handleTable;
    if (page == m_stackPage) return m_stackTable;
    return qobject_cast<QTableWidget*>(page);
}

void MinidumpDock::startResultTableFill(
    QTableWidget* table, int totalRows, std::function<void(int)> fillRow)
{
    auto state = std::make_shared<MinidumpTableProjectionState>();
    state->table = table;
    state->fillRow = std::move(fillRow);
    state->generation = m_resultProjectionGeneration;
    state->totalRows = totalRows;
    state->fitRows = table == m_exceptionTable;
    state->originalPrecision = table->horizontalHeader()->resizeContentsPrecision();
    // Bound Qt's own per-column sample; never request an all-row measurement here.
    table->horizontalHeader()->setResizeContentsPrecision(
        state->originalPrecision < 0 ? 1000 : std::min(state->originalPrecision, 1000));
    ks::ui::SetTableColumnAutoFitEnabled(table, false); // This projection owns native sizing.
    table->setEnabled(false); // Its shared action bar is a child and cannot export partial rows.
    m_resultTableProjections[table] = std::move(state);
    scheduleResultTableFill();
}

void MinidumpDock::restoreResultProjectionStatus()
{
    if (m_statusKey != QStringLiteral("minidump.status.projecting") ||
        m_projectionPreviousStatusKey.isEmpty()) return;
    m_statusKey = m_projectionPreviousStatusKey;
    m_statusFallback = m_projectionPreviousStatusFallback;
    m_statusArguments = m_projectionPreviousStatusArguments;
    refreshStatus();
}

void MinidumpDock::cancelResultTableFills()
{
    ++m_resultProjectionGeneration;
    m_resultTableTickPending = false;
    for (const auto& entry : m_resultTableProjections)
    {
        const auto& state = entry.second;
        if (state->table != nullptr)
        {
            state->table->horizontalHeader()->setResizeContentsPrecision(state->originalPrecision);
            state->table->setUpdatesEnabled(true);
            state->table->setEnabled(true);
        }
    }
    m_resultTableProjections.clear();
    restoreResultProjectionStatus();
}

void MinidumpDock::scheduleResultTableFill()
{
    if (!isVisible() || m_resultTableTickPending) return;
    const auto found = m_resultTableProjections.find(currentResultTable());
    if (found == m_resultTableProjections.end())
    {
        restoreResultProjectionStatus();
        return;
    }
    m_resultTableTickPending = true;
    const auto generation = m_resultProjectionGeneration;
    QTimer::singleShot(0, this, [this, generation]()
        {
            if (generation != m_resultProjectionGeneration) return;
            m_resultTableTickPending = false;
            continueResultTableFill();
        });
}

void MinidumpDock::continueResultTableFill()
{
    if (!isVisible()) return;
    const auto found = m_resultTableProjections.find(currentResultTable());
    if (found == m_resultTableProjections.end())
    {
        restoreResultProjectionStatus();
        return;
    }
    const auto state = found->second;
    QTableWidget* const table = state->table.data();
    if (table == nullptr || state->generation != m_resultProjectionGeneration) return;
    if (m_statusKey != QStringLiteral("minidump.status.projecting"))
    {
        m_projectionPreviousStatusKey = m_statusKey;
        m_projectionPreviousStatusFallback = m_statusFallback;
        m_projectionPreviousStatusArguments = m_statusArguments;
    }
    setStatus("minidump.status.projecting", "正在更新结果视图：%1 / %2",
        {QString::number(state->nextRow), QString::number(state->totalRows)});
    QElapsedTimer timer;
    timer.start();
    if (state->nextRow < state->totalRows)
    {
        const int end = std::min(state->nextRow + 16, state->totalRows);
        table->setRowCount(end);
        do
        {
            state->fillRow(state->nextRow++);
        } while (state->nextRow < end && timer.elapsed() < 4);
    }
    else if (state->nextColumn < table->columnCount())
    {
        // At most one bounded native column measurement per turn, including hidden widths.
        const int column = state->nextColumn++;
        table->resizeColumnToContents(column);
        if (!table->isColumnHidden(column))
            table->setColumnWidth(column, std::min(table->columnWidth(column), 420));
    }
    else if (state->fitRows && state->nextHeightRow < state->totalRows)
    {
        int count = 0;
        do
        {
            table->resizeRowToContents(state->nextHeightRow++);
        } while (state->nextHeightRow < state->totalRows && ++count < 16 && timer.elapsed() < 4);
    }
    else
    {
        table->horizontalHeader()->setResizeContentsPrecision(state->originalPrecision);
        table->setUpdatesEnabled(true);
        table->setEnabled(true);
        const qint64 started = table->property("perf_fill_start_ms").toLongLong();
        ks::ui::RecordPerfSpanWithCount("perf_dump_table_fill", table->objectName(),
            ks::ui::PerfTraceNowMs() - started, table->rowCount());
        m_resultTableProjections.erase(found);
        restoreResultProjectionStatus();
        return;
    }
    scheduleResultTableFill();
}

void MinidumpDock::projectResult(
    const ks::minidump::DumpParseResult& result, QWidget* targetPage)
{
    const auto& analysis = result.analysis;
    // ===================== 诊断结论页（放在最前，是本页的主产出） =====================
    if (result.success && !analysis.headline.isEmpty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_analysisView, "minidump.tab.analysis", "诊断结论");
        }
        else if (targetPage == m_analysisView)
        {
            // 结论页按"先说结论，再给证据，最后给动作"的顺序排版。
            // 可信度用带底色的徽章而不是一行文字：它决定了这份结论该被多认真地对待，
            // 必须一眼看到，不能和其它属性混在一起。
            const QString accentHex = KswordTheme::AccentHex(KswordTheme::AccentRole::Blue);
            const QString textHex = KswordTheme::TextPrimaryHex();
            const QString mutedHex = KswordTheme::TextDisabledColorHex();
            const QString borderHex = KswordTheme::BorderHex();
            const QString surfaceAltHex = KswordTheme::SurfaceAltHex();

            // confidenceColor：可信度越低越要显眼地降调，避免低可信结论被当成定论。
            QColor confidenceColor = KswordTheme::TextDisabledColor();
            switch (analysis.confidence)
            {
            case ks::minidump::AnalysisConfidence::High:
                confidenceColor = KswordTheme::AccentColor(KswordTheme::AccentRole::Red);
                break;
            case ks::minidump::AnalysisConfidence::Medium:
                confidenceColor = KswordTheme::AccentColor(KswordTheme::AccentRole::Orange);
                break;
            case ks::minidump::AnalysisConfidence::Low:
                confidenceColor = KswordTheme::AccentColor(KswordTheme::AccentRole::Yellow);
                break;
            case ks::minidump::AnalysisConfidence::None:
            default:
                break;
            }
            const QString confidenceHex = KswordTheme::ThemeColorName(confidenceColor);
            // confidenceTextHex：徽章前景按徽章自己的底色反算，不能套 OnAccentHex()——
            // 后者只对蓝色主强调色做过对比度校正，压在 Yellow（琥珀）和 None 的灰蓝上
            // 白字只有 2:1，而这两档恰恰最需要用户看清"别把这个结论当定论"。
            const QString confidenceTextHex = KswordTheme::ThemeColorName(
                KswordTheme::EnsureTextContrast(
                    KswordTheme::TextPrimaryColor(),
                    confidenceColor));

            // escape：结论文本里可能出现 <> &，必须转义后再拼进 HTML。
            const auto escape = [](const QString& text) { return text.toHtmlEscaped(); };

            QString html;
            html.reserve(2048);
            html += QStringLiteral("<div style='color:%1;'>").arg(textHex);

            // 一句话结论：整页最大的字号，单独成块。
            html += QStringLiteral(
                "<div style='font-size:15pt; font-weight:600; line-height:150%%; margin:2px 0 10px 0;'>%1</div>")
                .arg(escape(ks::i18n::sourceText(analysis.headline)));

            // 徽章行：可信度 + 故障归类。
            html += QStringLiteral("<div style='margin-bottom:14px;'>");
            html += QStringLiteral(
                "<span style='background:%1; color:%2; padding:3px 10px; "
                "border-radius:4px; font-weight:600;'>%3 %4</span>")
                .arg(confidenceHex)
                .arg(confidenceTextHex)
                .arg(translated("minidump.analysis.confidence", "可信度"))
                .arg(escape(ks::i18n::sourceText(
                    ks::minidump::AnalysisConfidenceText(analysis.confidence))));
            if (!analysis.category.isEmpty())
            {
                html += QStringLiteral(
                    "&nbsp;&nbsp;<span style='background:%1; color:%2; padding:3px 10px; "
                    "border-radius:4px; border:1px solid %3;'>%4</span>")
                    .arg(surfaceAltHex)
                    .arg(textHex)
                    .arg(borderHex)
                    .arg(escape(ks::i18n::sourceText(analysis.category)));
            }
            html += QStringLiteral("</div>");

            // sectionHtml：统一渲染"小标题 + 条目列表"。
            const auto sectionHtml =
                [&escape, &accentHex, &textHex](const QString& title, const QStringList& items)
                {
                    if (items.isEmpty())
                    {
                        return QString();
                    }
                    QString block = QStringLiteral(
                        "<div style='color:%1; font-weight:600; font-size:11pt; "
                        "margin:0 0 6px 0;'>%2</div>")
                        .arg(accentHex)
                        .arg(escape(title));
                    block += QStringLiteral("<div style='margin:0 0 16px 0;'>");
                    for (const QString& item : items)
                    {
                        // 用悬挂缩进的行而不是 <ul>：Qt 富文本对列表的边距控制很有限，
                        // 长条目换行后会顶到项目符号下面，读起来更乱。
                        block += QStringLiteral(
                            "<div style='color:%1; line-height:160%%; margin-bottom:5px;'>"
                            "<span style='color:%2;'>▸</span>&nbsp;%3</div>")
                            .arg(textHex)
                            .arg(accentHex)
                            .arg(escape(item));
                    }
                    block += QStringLiteral("</div>");
                    return block;
                };

            QStringList localizedFindings;
            for (const QString& finding : analysis.findings)
            {
                localizedFindings.append(ks::i18n::sourceText(finding));
            }
            QStringList localizedSuggestions;
            for (const QString& suggestion : analysis.suggestions)
            {
                localizedSuggestions.append(ks::i18n::sourceText(suggestion));
            }

            // 崩溃点：整份报告里最该被记住的一个地址，单独成块并用等宽字体，
            // 免得在证据条目里跟其它文字混成一片。
            if (result.faultingAddress != 0)
            {
                // faultSymbol：优先用肇事候选里与崩溃地址同模块的那条，给出"模块+偏移"。
                QString faultSymbol;
                for (const ks::minidump::BlameEntry& blame : analysis.blame)
                {
                    if (blame.address == result.faultingAddress && !blame.moduleName.isEmpty())
                    {
                        faultSymbol = QStringLiteral("%1+0x%2")
                            .arg(blame.moduleName)
                            .arg(QString::number(blame.offset, 16).toUpper());
                        break;
                    }
                }
                html += QStringLiteral(
                    "<div style='background:%1; border:1px solid %2; border-radius:6px; "
                    "padding:8px 12px; margin-bottom:16px;'>"
                    "<span style='color:%3;'>%4</span>&nbsp;&nbsp;"
                    "<span style='font-family:Consolas,monospace; font-size:11pt; color:%5;'>%6</span>")
                    .arg(surfaceAltHex)
                    .arg(borderHex)
                    .arg(mutedHex)
                    .arg(translated("minidump.analysis.fault_address", "崩溃点"))
                    .arg(textHex)
                    .arg(hexText(result.faultingAddress));
                if (!faultSymbol.isEmpty())
                {
                    html += QStringLiteral(
                        "&nbsp;&nbsp;<span style='color:%1; font-weight:600;'>%2</span>")
                        .arg(accentHex)
                        .arg(escape(faultSymbol));
                }
                html += QStringLiteral("</div>");
            }

            // 肇事模块候选直接嵌进本页：它是结论的直接依据，让用户为了看第一嫌疑
            // 再跳一个页签没有道理。权重画成条形，相对高低一眼可见。
            if (!analysis.blame.empty())
            {
                html += QStringLiteral(
                    "<div style='color:%1; font-weight:600; font-size:11pt; margin:0 0 8px 0;'>%2</div>")
                    .arg(accentHex)
                    .arg(escape(translated("minidump.analysis.blame_title", "肇事模块候选")));

                // maxWeight：条形长度的基准。权重全为 0 时退化为等长，避免除零。
                int maxWeight = 1;
                for (const ks::minidump::BlameEntry& blame : analysis.blame)
                {
                    maxWeight = std::max(maxWeight, blame.weight);
                }

                // 只列前若干条：候选表尾部通常是权重极低的噪音，完整列表在专页里。
                constexpr std::size_t kInlineBlameLimit = 5;
                const std::size_t shownCount =
                    std::min(kInlineBlameLimit, analysis.blame.size());
                html += QStringLiteral("<table cellspacing='0' cellpadding='0' width='100%'>");
                for (std::size_t blameIndex = 0; blameIndex < shownCount; ++blameIndex)
                {
                    const ks::minidump::BlameEntry& blame = analysis.blame[blameIndex];
                    const int barPercent =
                        std::clamp(blame.weight * 100 / maxWeight, 3, 100);
                    // 第一名用强调色，其余用中性色：排序本身已经表达了先后，
                    // 全部上色反而让"第一名"失去意义。
                    const QString barColor = (blameIndex == 0) ? confidenceHex : borderHex;
                    const QString nameText = blame.unloadedModule
                        ? QStringLiteral("%1 [%2]")
                            .arg(blame.moduleName, translated("minidump.blame.unloaded", "已卸载"))
                        : blame.moduleName;
                    html += QStringLiteral(
                        "<tr>"
                        "<td width='34%%' style='padding:3px 8px 3px 0; color:%1;'>%2</td>"
                        "<td width='50%%' style='padding:3px 0;'>"
                        "<table cellspacing='0' cellpadding='0' width='100%%'><tr>"
                        "<td width='%3%%' style='background:%4;'>&nbsp;</td>"
                        "<td>&nbsp;</td></tr></table></td>"
                        "<td width='16%%' style='padding:3px 0 3px 8px; color:%5;'>%6 %7</td>"
                        "</tr>")
                        .arg(textHex)
                        .arg(escape(nameText))
                        .arg(barPercent)
                        .arg(barColor)
                        .arg(mutedHex)
                        .arg(translated("minidump.column.weight", "证据权重"))
                        .arg(blame.weight);
                }
                html += QStringLiteral("</table>");
                if (analysis.blame.size() > shownCount)
                {
                    html += QStringLiteral(
                        "<div style='color:%1; margin:4px 0 0 0;'>%2</div>")
                        .arg(mutedHex)
                        .arg(escape(translated(
                            "minidump.analysis.blame_more",
                            "更多候选与逐条证据见“肇事模块”页。")));
                }
                html += QStringLiteral("<div style='margin-bottom:16px;'></div>");
            }

            html += sectionHtml(
                translated("minidump.analysis.findings_title", "证据"),
                localizedFindings);
            html += sectionHtml(
                translated("minidump.analysis.suggestions_title", "接下来怎么做"),
                localizedSuggestions);

            html += QStringLiteral("</div>");
            m_analysisView->setHtml(html);
        }
    }

    // ===================== 肇事模块候选页 =====================
    if (!analysis.blame.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_blameTable, "minidump.tab.blame", "肇事模块");
        }
        else if (targetPage == m_blameTable)
        {
            beginFill(m_blameTable);
            m_blameTable->setColumnCount(6);
            m_blameTable->setHorizontalHeaderLabels({
                translated("minidump.column.suspect_module", "模块"),
                translated("minidump.column.suspect_function", "函数"),
                translated("minidump.column.hit_address", "命中地址"),
                translated("minidump.column.module_offset", "模块内偏移"),
                translated("minidump.column.weight", "证据权重"),
                translated("minidump.column.evidence", "证据") });
            startResultTableFill(m_blameTable, static_cast<int>(analysis.blame.size()),
                [this, snapshot = m_lastResult](int blameRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::BlameEntry& blame = result.analysis.blame[static_cast<std::size_t>(blameRow)];
                // moduleText：已卸载模块单独标注，它是最强的单条证据。
                const QString moduleText = blame.unloadedModule
                    ? QStringLiteral("%1 [%2]")
                        .arg(blame.moduleName,
                             translated("minidump.blame.unloaded", "已卸载"))
                    : blame.moduleName;
                m_blameTable->setItem(blameRow, 0, makeItem(moduleText));
                // 有符号时把函数名单列出来：归因到模块只能说明"哪个驱动"，
                // 归因到函数才能直接落到代码上，这是本页最有价值的一列。
                m_blameTable->setItem(blameRow, 1, makeItem(blame.functionText));
                m_blameTable->setItem(blameRow, 2, makeItem(hexText(blame.address)));
                m_blameTable->setItem(blameRow, 3, makeItem(hexText(blame.offset)));
                m_blameTable->setItem(blameRow, 4, makeItem(QString::number(blame.weight)));
                // evidence：多条证据合成一行，用分号分隔，避免行数爆炸。
                QStringList localizedEvidence;
                for (const QString& evidence : blame.evidence)
                {
                    localizedEvidence.append(ks::i18n::sourceText(evidence));
                }
                m_blameTable->setItem(blameRow, 5,
                    makeItem(localizedEvidence.join(QStringLiteral("；"))));
                });
        }
    }

    // ===================== 概览页（恒存在） =====================
    if (true)
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_overviewTable, "minidump.tab.overview", "概览");
        }
        else if (targetPage == m_overviewTable)
        {
        beginFill(m_overviewTable);
        m_overviewTable->setColumnCount(2);
        m_overviewTable->setHorizontalHeaderLabels({
            translated("minidump.column.property", "属性"),
            translated("minidump.column.value", "值") });
        // overviewRows：概览属性 + 解析告警合并展示，一次性设定行数。
        const int overviewRowCount =
            static_cast<int>(result.overview.size()) +
            static_cast<int>(result.diagnostics.size()) +
            (result.memoryRegionShown < result.memoryRegionTotal ? 1 : 0);
        startResultTableFill(m_overviewTable, overviewRowCount,
            [this, snapshot = m_lastResult](int overviewRow)
            {
                const auto& result = *snapshot;
                const auto properties = static_cast<int>(result.overview.size());
                if (overviewRow < properties)
                {
                    const auto& property = result.overview[static_cast<std::size_t>(overviewRow)];
                    m_overviewTable->setItem(overviewRow, 0, makeItem(ks::i18n::sourceText(property.name)));
                    m_overviewTable->setItem(overviewRow, 1, makeItem(ks::i18n::sourceText(property.value)));
                }
                else
                {
                    m_overviewTable->setItem(overviewRow, 0,
                        makeItem(translated("minidump.overview.warning", "解析告警")));
                    const bool truncated = result.memoryRegionShown < result.memoryRegionTotal;
                    if (truncated && overviewRow == properties)
                    {
                        m_overviewTable->setItem(overviewRow, 1,
                            makeItem(translated("minidump.overview.memory_truncated",
                                "内存区域过多，仅展示前 %1 / %2 条。")
                                .arg(result.memoryRegionShown).arg(result.memoryRegionTotal)));
                    }
                    else
                    {
                        const int diagnostic = overviewRow - properties - (truncated ? 1 : 0);
                        m_overviewTable->setItem(overviewRow, 1,
                            makeItem(ks::i18n::sourceText(result.diagnostics.at(diagnostic))));
                    }
                }
            });
        }
    }

    // ===================== 异常/停止码页 =====================
    if (!result.exceptionInfo.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_exceptionTable, "minidump.tab.exception", "异常信息");
        }
        else if (targetPage == m_exceptionTable)
        {
            beginFill(m_exceptionTable);
            m_exceptionTable->setColumnCount(2);
            m_exceptionTable->setHorizontalHeaderLabels({
                translated("minidump.column.property", "属性"),
                translated("minidump.column.value", "值") });
            m_exceptionTable->setWordWrap(true);
            startResultTableFill(m_exceptionTable, static_cast<int>(result.exceptionInfo.size()),
                [this, snapshot = m_lastResult](int exceptionRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::DumpProperty& property = result.exceptionInfo[static_cast<std::size_t>(exceptionRow)];
                m_exceptionTable->setItem(exceptionRow, 0,
                    makeItem(ks::i18n::sourceText(property.name)));
                m_exceptionTable->setItem(exceptionRow, 1,
                    makeItem(ks::i18n::sourceText(property.value)));
                });
        }
    }

    // ===================== 崩溃现场页 =====================
    if (!result.executionContext.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_executionContextTable, "minidump.tab.execution_context", "崩溃现场");
        }
        else if (targetPage == m_executionContextTable)
        {
            beginFill(m_executionContextTable);
            m_executionContextTable->setColumnCount(2);
            m_executionContextTable->setHorizontalHeaderLabels({
                translated("minidump.column.property", "属性"),
                translated("minidump.column.value", "值") });
            startResultTableFill(m_executionContextTable, static_cast<int>(result.executionContext.size()),
                [this, snapshot = m_lastResult](int contextRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::DumpProperty& property = result.executionContext[static_cast<std::size_t>(contextRow)];
                m_executionContextTable->setItem(contextRow, 0,
                    makeItem(ks::i18n::sourceText(property.name)));
                m_executionContextTable->setItem(contextRow, 1,
                    makeItem(ks::i18n::sourceText(property.value)));
                });
        }
    }

    // ===================== 调用栈页 =====================
    if (!result.stackFrames.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_stackPage, "minidump.tab.stack", "调用栈");
        }
        else if (targetPage == m_stackPage)
        {
            beginFill(m_stackTable);
            m_stackTable->setColumnCount(7);
            m_stackTable->setHorizontalHeaderLabels({
                translated("minidump.column.thread_id", "线程 ID"),
                translated("minidump.column.frame_index", "帧"),
                translated("minidump.column.symbol", "符号"),
                translated("minidump.column.source_location", "源码位置"),
                translated("minidump.column.frame_address", "返回地址"),
                translated("minidump.column.stack_address", "栈地址"),
                translated("minidump.column.frame_source", "来源") });
            startResultTableFill(m_stackTable, static_cast<int>(result.stackFrames.size()),
                [this, snapshot = m_lastResult](int stackRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::StackFrameEntry& frame = result.stackFrames[static_cast<std::size_t>(stackRow)];
                m_stackTable->setItem(stackRow, 0,
                    makeItem(frame.threadId != 0
                        ? QString::number(frame.threadId)
                        : translated("minidump.stack.crash_thread", "崩溃线程")));
                m_stackTable->setItem(stackRow, 1, makeItem(QString::number(frame.index)));
                // 符号列优先给"模块!函数+偏移"，没有符号才退回"模块+偏移"。
                // 合成一列而不是新开一列，是为了不把这张表撑到需要列组预设——
                // 一旦分组，"来源"列可能被隐藏，那条误报提示就没地方说了。
                const QString baseSymbol = frame.functionText.isEmpty()
                    ? frame.symbolText
                    : frame.functionText;
                const QString symbolText = frame.unloadedModule
                    ? QStringLiteral("%1 [%2]")
                        .arg(baseSymbol,
                             translated("minidump.blame.unloaded", "已卸载"))
                    : baseSymbol;
                m_stackTable->setItem(stackRow, 2, makeItem(symbolText));
                // 源码位置只在映像与 PDB 都匹配时才有值，解析器已经把不可信的滤掉了。
                m_stackTable->setItem(stackRow, 3, makeItem(frame.sourceText));
                m_stackTable->setItem(stackRow, 4, makeItem(hexText(frame.address)));
                m_stackTable->setItem(stackRow, 5,
                    makeItem(frame.stackAddress != 0 ? hexText(frame.stackAddress) : QString()));
                // 来源必须如实标注：只有第 0 帧取自 CONTEXT，其余都是扫描猜测。
                m_stackTable->setItem(stackRow, 6,
                    makeItem(frame.fromContext
                        ? translated("minidump.stack.from_context", "上下文（可信）")
                        : translated("minidump.stack.from_scan", "栈扫描（可能误报）")));
                });
        }
    }

    // ===================== 崩溃历史页 =====================
    if (!result.crashHistory.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_crashHistoryTable, "minidump.tab.crash_history", "崩溃历史");
        }
        else if (targetPage == m_crashHistoryTable)
        {
            beginFill(m_crashHistoryTable);
            m_crashHistoryTable->setColumnCount(4);
            m_crashHistoryTable->setHorizontalHeaderLabels({
                translated("minidump.column.event_time", "时间"),
                translated("minidump.column.event_kind", "性质"),
                translated("minidump.column.event_summary", "摘要"),
                translated("minidump.column.detail", "说明") });
            startResultTableFill(m_crashHistoryTable, static_cast<int>(result.crashHistory.size()),
                [this, snapshot = m_lastResult](int historyRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::CrashHistoryEntry& entry = result.crashHistory[static_cast<std::size_t>(historyRow)];
                m_crashHistoryTable->setItem(historyRow, 0,
                    makeItem(entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
                m_crashHistoryTable->setItem(historyRow, 1,
                    makeItem(ks::i18n::sourceText(entry.kindText)));
                m_crashHistoryTable->setItem(historyRow, 2,
                    makeItem(ks::i18n::sourceText(entry.summary)));
                m_crashHistoryTable->setItem(historyRow, 3,
                    makeItem(ks::i18n::sourceText(entry.detail)));
                });
        }
    }

    // ===================== 池标记页 =====================
    if (!result.poolTags.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_poolTagTable, "minidump.tab.pool_tags", "池标记");
        }
        else if (targetPage == m_poolTagTable)
        {
            beginFill(m_poolTagTable);
            m_poolTagTable->setColumnCount(4);
            m_poolTagTable->setHorizontalHeaderLabels({
                translated("minidump.column.pool_tag", "池标记"),
                translated("minidump.column.pool_tag_source", "来源"),
                translated("minidump.column.pool_tag_owner", "映像中出现该标记的模块"),
                translated("minidump.column.pool_tag_purpose", "已知用途") });
            startResultTableFill(m_poolTagTable, static_cast<int>(result.poolTags.size()),
                [this, snapshot = m_lastResult](int poolRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::PoolTagCandidate& candidate = result.poolTags[static_cast<std::size_t>(poolRow)];
                m_poolTagTable->setItem(poolRow, 0, makeItem(candidate.tagText));
                m_poolTagTable->setItem(poolRow, 1,
                    makeItem(ks::i18n::sourceText(candidate.source)));
                m_poolTagTable->setItem(poolRow, 2,
                    makeItem(candidate.ownerModules.join(QStringLiteral("、"))));
                m_poolTagTable->setItem(poolRow, 3, makeItem(candidate.knownPurpose));
                });
        }
    }

    // ===================== 符号状态页 =====================
    if (!result.symbolStatus.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_symbolTable, "minidump.tab.symbols", "符号");
        }
        else if (targetPage == m_symbolTable)
        {
            beginFill(m_symbolTable);
            m_symbolTable->setColumnCount(5);
            m_symbolTable->setHorizontalHeaderLabels({
                translated("minidump.column.module", "模块"),
                translated("minidump.column.symbol_state", "符号状态"),
                translated("minidump.column.image_path", "映像路径"),
                translated("minidump.column.pdb_path", "PDB 路径"),
                translated("minidump.column.detail", "说明") });
            startResultTableFill(m_symbolTable, static_cast<int>(result.symbolStatus.size()),
                [this, snapshot = m_lastResult](int symbolRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::ModuleSymbolStatus& status = result.symbolStatus[static_cast<std::size_t>(symbolRow)];
                m_symbolTable->setItem(symbolRow, 0, makeItem(status.moduleName));
                m_symbolTable->setItem(symbolRow, 1,
                    makeItem(ks::i18n::sourceText(
                        ks::minidump::SymbolMatchStateText(status.state))));
                m_symbolTable->setItem(symbolRow, 2, makeItem(status.imagePath));
                m_symbolTable->setItem(symbolRow, 3, makeItem(status.pdbPath));
                m_symbolTable->setItem(symbolRow, 4,
                    makeItem(ks::i18n::sourceText(status.detail)));
                });
        }
    }

    // ===================== 寄存器页 =====================
    if (!result.registers.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_registerTable, "minidump.tab.registers", "寄存器");
        }
        else if (targetPage == m_registerTable)
        {
            beginFill(m_registerTable);
            m_registerTable->setColumnCount(3);
            m_registerTable->setHorizontalHeaderLabels({
                translated("minidump.column.register", "寄存器"),
                translated("minidump.column.value", "值"),
                translated("minidump.column.interpretation", "解读") });
            startResultTableFill(m_registerTable, static_cast<int>(result.registers.size()),
                [this, snapshot = m_lastResult](int registerRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::RegisterEntry& registerEntry = result.registers[static_cast<std::size_t>(registerRow)];
                m_registerTable->setItem(registerRow, 0, makeItem(registerEntry.name));
                m_registerTable->setItem(registerRow, 1, makeItem(hexText(registerEntry.value)));
                m_registerTable->setItem(registerRow, 2,
                    makeItem(ks::i18n::sourceText(registerEntry.note)));
                });
        }
    }

    // ===================== 流目录/数据布局页 =====================
    if (!result.streams.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_streamTable,
                result.kind == ks::minidump::DumpKind::UserMinidump ? "minidump.tab.streams" : "minidump.tab.layout",
                result.kind == ks::minidump::DumpKind::UserMinidump ? "流目录" : "数据布局");
        }
        else if (targetPage == m_streamTable)
        {
            beginFill(m_streamTable);
            m_streamTable->setColumnCount(5);
            m_streamTable->setHorizontalHeaderLabels({
                translated("minidump.column.stream_type", "类型编号"),
                translated("minidump.column.stream_name", "类型名"),
                translated("minidump.column.offset", "文件偏移"),
                translated("minidump.column.size", "大小"),
                translated("minidump.column.note", "说明") });
            startResultTableFill(m_streamTable, static_cast<int>(result.streams.size()),
                [this, snapshot = m_lastResult](int streamRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::StreamEntry& stream = result.streams[static_cast<std::size_t>(streamRow)];
                m_streamTable->setItem(streamRow, 0, makeItem(QString::number(stream.type)));
                m_streamTable->setItem(streamRow, 1, makeItem(stream.typeName));
                m_streamTable->setItem(streamRow, 2, makeItem(hexText(stream.rva)));
                m_streamTable->setItem(streamRow, 3, makeItem(QString::number(stream.size)));
                m_streamTable->setItem(streamRow, 4,
                    makeItem(ks::i18n::sourceText(stream.note)));
                });
        }
    }

    // ===================== 模块/驱动页 =====================
    if (!result.modules.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_modulePage,
                result.kind == ks::minidump::DumpKind::UserMinidump ? "minidump.tab.modules" : "minidump.tab.drivers",
                result.kind == ks::minidump::DumpKind::UserMinidump ? "模块" : "驱动");
        }
        else if (targetPage == m_modulePage)
        {
            beginFill(m_moduleTable);
            m_moduleTable->setColumnCount(8);
            m_moduleTable->setHorizontalHeaderLabels({
                translated("minidump.column.module_name", "名称"),
                translated("minidump.column.base", "基址"),
                translated("minidump.column.size", "大小"),
                translated("minidump.column.timestamp", "时间戳"),
                translated("minidump.column.version", "版本"),
                translated("minidump.column.pdb_file", "PDB 文件"),
                translated("minidump.column.pdb_guid", "PDB GUID/Age"),
                translated("minidump.column.checksum", "校验和") });
            startResultTableFill(m_moduleTable, static_cast<int>(result.modules.size()),
                [this, snapshot = m_lastResult](int moduleRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::ModuleEntry& module = result.modules[static_cast<std::size_t>(moduleRow)];
                m_moduleTable->setItem(moduleRow, 0, makeItem(module.name));
                m_moduleTable->setItem(moduleRow, 1, makeItem(hexText(module.base)));
                m_moduleTable->setItem(moduleRow, 2, makeItem(hexText(module.size)));
                m_moduleTable->setItem(moduleRow, 3, makeItem(module.timestampText));
                m_moduleTable->setItem(moduleRow, 4, makeItem(module.version));
                m_moduleTable->setItem(moduleRow, 5, makeItem(module.pdbName));
                m_moduleTable->setItem(moduleRow, 6, makeItem(module.pdbGuidAge));
                m_moduleTable->setItem(moduleRow, 7,
                    makeItem(module.checksum != 0 ? hexText(module.checksum) : QString()));
                });
        }
    }

    // ===================== 线程页 =====================
    if (!result.threads.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_threadPage, "minidump.tab.threads", "线程");
        }
        else if (targetPage == m_threadPage)
        {
            beginFill(m_threadTable);
            m_threadTable->setColumnCount(13);
            m_threadTable->setHorizontalHeaderLabels({
                translated("minidump.column.thread_id", "线程 ID"),
                translated("minidump.column.thread_state", "状态"),
                translated("minidump.column.thread_name", "名称"),
                translated("minidump.column.instruction_pointer", "指令指针"),
                translated("minidump.column.ip_module", "指令指针所属模块"),
                translated("minidump.column.start_address", "起始地址"),
                translated("minidump.column.start_module", "起始地址所属模块"),
                translated("minidump.column.cpu_time", "CPU 时间(用户/内核)"),
                translated("minidump.column.teb", "TEB"),
                translated("minidump.column.stack_base", "栈基址"),
                translated("minidump.column.stack_size", "栈大小"),
                translated("minidump.column.suspend_count", "挂起计数"),
                translated("minidump.column.priority", "优先级") });
            startResultTableFill(m_threadTable, static_cast<int>(result.threads.size()),
                [this, snapshot = m_lastResult](int threadRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::ThreadEntry& thread = result.threads[static_cast<std::size_t>(threadRow)];
                m_threadTable->setItem(threadRow, 0, makeItem(QString::number(thread.threadId)));
                m_threadTable->setItem(threadRow, 1,
                    makeItem(thread.faulting
                        ? translated("minidump.thread.faulting", "崩溃线程")
                        : QString()));
                m_threadTable->setItem(threadRow, 2, makeItem(thread.name));
                m_threadTable->setItem(threadRow, 3,
                    makeItem(thread.instructionPointer != 0
                        ? hexText(thread.instructionPointer)
                        : QString()));
                m_threadTable->setItem(threadRow, 4, makeItem(thread.ipSymbolText));
                m_threadTable->setItem(threadRow, 5,
                    makeItem(thread.startAddress != 0 ? hexText(thread.startAddress) : QString()));
                m_threadTable->setItem(threadRow, 6, makeItem(thread.startSymbolText));
                m_threadTable->setItem(threadRow, 7, makeItem(thread.cpuTimeText));
                m_threadTable->setItem(threadRow, 8, makeItem(hexText(thread.teb)));
                m_threadTable->setItem(threadRow, 9, makeItem(hexText(thread.stackBase)));
                m_threadTable->setItem(threadRow, 10, makeItem(QString::number(thread.stackSize)));
                m_threadTable->setItem(threadRow, 11, makeItem(QString::number(thread.suspendCount)));
                m_threadTable->setItem(threadRow, 12,
                    makeItem(QStringLiteral("%1 / %2")
                        .arg(thread.priorityClass)
                        .arg(thread.priority)));
                });
        }
    }

    // ===================== 内存区域页 =====================
    if (!result.memoryRegions.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_memoryPage, "minidump.tab.memory", "内存区域");
        }
        else if (targetPage == m_memoryPage)
        {
            beginFill(m_memoryTable);
            m_memoryTable->setColumnCount(6);
            m_memoryTable->setHorizontalHeaderLabels({
                translated("minidump.column.base", "基址"),
                translated("minidump.column.size", "大小"),
                translated("minidump.column.mem_state", "状态"),
                translated("minidump.column.mem_protect", "保护"),
                translated("minidump.column.mem_type", "类型"),
                translated("minidump.column.mem_source", "来源") });
            startResultTableFill(m_memoryTable, static_cast<int>(result.memoryRegions.size()),
                [this, snapshot = m_lastResult](int memoryRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::MemoryRegionEntry& region = result.memoryRegions[static_cast<std::size_t>(memoryRow)];
                m_memoryTable->setItem(memoryRow, 0, makeItem(hexText(region.base)));
                m_memoryTable->setItem(memoryRow, 1, makeItem(hexText(region.size)));
                m_memoryTable->setItem(memoryRow, 2, makeItem(region.state));
                m_memoryTable->setItem(memoryRow, 3, makeItem(region.protect));
                m_memoryTable->setItem(memoryRow, 4, makeItem(region.type));
                m_memoryTable->setItem(memoryRow, 5,
                    makeItem(ks::i18n::sourceText(region.source)));
                });
        }
    }

    // ===================== 原始内存预览页 =====================
    if (!result.byteBlocks.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_rawMemoryTabs, "minidump.tab.raw_memory", "原始内存");
        }
        else if (targetPage == m_rawMemoryTabs)
        {
            const QSignalBlocker rawSignals(m_rawMemoryTabs);
            m_buildingRawMemoryCatalog = true;
            for (std::size_t index = 0; index < result.byteBlocks.size(); ++index)
            {
                auto* page = new QWidget(m_rawMemoryTabs);
                page->setProperty("ksword_navigation_descriptor_id", rawMemoryNavigationId(index));
                m_rawMemoryTabs->addTab(page, rawMemoryNavigationTitle(index));
            }
            m_buildingRawMemoryCatalog = false;
            renderCurrentRawMemoryPage();
        }
    }

    // ===================== 内存查看器页 =====================
    if (!result.capturedMemoryRanges.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_memoryView, "minidump.tab.memory_view", "内存查看器");
        }
        else if (targetPage == m_memoryView)
        {
            m_memoryView->setDumpData(result);
        }
    }

    // ===================== 句柄页 =====================
    if (!result.handles.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_handlePage, "minidump.tab.handles", "句柄");
        }
        else if (targetPage == m_handlePage)
        {
            beginFill(m_handleTable);
            m_handleTable->setColumnCount(7);
            m_handleTable->setHorizontalHeaderLabels({
                translated("minidump.column.handle_value", "句柄值"),
                translated("minidump.column.handle_type", "类型"),
                translated("minidump.column.object_name", "对象名"),
                translated("minidump.column.attributes", "属性"),
                translated("minidump.column.granted_access", "访问掩码"),
                translated("minidump.column.handle_count", "句柄计数"),
                translated("minidump.column.pointer_count", "指针计数") });
            startResultTableFill(m_handleTable, static_cast<int>(result.handles.size()),
                [this, snapshot = m_lastResult](int handleRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::HandleEntry& handle = result.handles[static_cast<std::size_t>(handleRow)];
                m_handleTable->setItem(handleRow, 0, makeItem(hexText(handle.handleValue)));
                m_handleTable->setItem(handleRow, 1, makeItem(handle.typeName));
                m_handleTable->setItem(handleRow, 2, makeItem(handle.objectName));
                m_handleTable->setItem(handleRow, 3, makeItem(hexText(handle.attributes)));
                m_handleTable->setItem(handleRow, 4, makeItem(hexText(handle.grantedAccess)));
                m_handleTable->setItem(handleRow, 5, makeItem(QString::number(handle.handleCount)));
                m_handleTable->setItem(handleRow, 6, makeItem(QString::number(handle.pointerCount)));
                });
        }
    }

    // ===================== 已卸载模块页 =====================
    if (!result.unloadedModules.empty())
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_unloadedTable, "minidump.tab.unloaded", "已卸载模块");
        }
        else if (targetPage == m_unloadedTable)
        {
            beginFill(m_unloadedTable);
            m_unloadedTable->setColumnCount(5);
            m_unloadedTable->setHorizontalHeaderLabels({
                translated("minidump.column.module_name", "名称"),
                translated("minidump.column.base", "基址"),
                translated("minidump.column.size", "大小"),
                translated("minidump.column.timestamp", "时间戳"),
                translated("minidump.column.checksum", "校验和") });
            startResultTableFill(m_unloadedTable, static_cast<int>(result.unloadedModules.size()),
                [this, snapshot = m_lastResult](int unloadedRow)
                {
                    const auto& result = *snapshot;
                    const ks::minidump::UnloadedModuleEntry& module = result.unloadedModules[static_cast<std::size_t>(unloadedRow)];
                m_unloadedTable->setItem(unloadedRow, 0, makeItem(module.name));
                m_unloadedTable->setItem(unloadedRow, 1, makeItem(hexText(module.base)));
                m_unloadedTable->setItem(unloadedRow, 2, makeItem(hexText(module.size)));
                m_unloadedTable->setItem(unloadedRow, 3, makeItem(module.timestampText));
                m_unloadedTable->setItem(unloadedRow, 4,
                    makeItem(module.checksum != 0 ? hexText(module.checksum) : QString()));
                });
        }
    }

    // ===================== 全文报告页 =====================
    if (result.success)
    {
        if (targetPage == nullptr)
        {
            registerResultTab(m_reportEditor, "minidump.tab.report", "报告");
        }
        else if (targetPage == m_reportEditor)
        {
            // 报告用中文规范文本生成，只读编辑器按当前语言即时渲染。
            requestReportDocument();
        }
    }

}

ks::ui::FieldDocument MinidumpDock::buildReportText(const ks::minidump::DumpParseResult& result)
{
    ks::ui::FieldDocument document;
    document.title = QStringLiteral("KSword 转储解析报告");
    document.field(QStringLiteral("文件"), result.filePath);
    const auto appendProperties = [&document](const QString& title, const auto& properties) {
        document.section(title);
        for (const auto& property : properties) document.field(property.name, property.value);
    };
    const auto& analysis = result.analysis;
    if (!analysis.headline.isEmpty()) {
        document.section(QStringLiteral("诊断结论"));
        document.field(QStringLiteral("结论"), analysis.headline, true);
        document.field(QStringLiteral("可信度"), ks::minidump::AnalysisConfidenceText(analysis.confidence), true);
        if (!analysis.category.isEmpty()) document.field(QStringLiteral("故障归类"), analysis.category, true);
        for (const auto& finding : analysis.findings) document.field(QStringLiteral("发现"), finding, true);
        for (const auto& suggestion : analysis.suggestions) document.field(QStringLiteral("建议"), suggestion, true);
    }
    if (!analysis.blame.empty()) {
        document.section(QStringLiteral("肇事模块候选"));
        document.note(QStringLiteral("按证据权重降序"));
        for (const auto& blame : analysis.blame) {
            document.section(blame.moduleName);
            document.field(QStringLiteral("权重"), QString::number(blame.weight));
            document.field(QStringLiteral("命中"), hexText(blame.address));
            document.field(QStringLiteral("偏移"), hexText(blame.offset));
            document.field(QStringLiteral("已卸载模块"), blame.unloadedModule ? QStringLiteral("是") : QStringLiteral("否"), true);
            for (const auto& evidence : blame.evidence) document.field(QStringLiteral("证据"), evidence, true);
        }
    }
    appendProperties(QStringLiteral("概览"), result.overview);
    if (!result.exceptionInfo.empty()) appendProperties(QStringLiteral("异常信息"), result.exceptionInfo);
    if (!result.executionContext.empty()) appendProperties(QStringLiteral("崩溃现场"), result.executionContext);
    if (!result.registers.empty()) {
        document.section(QStringLiteral("崩溃点寄存器"));
        for (const auto& entry : result.registers) {
            document.field(entry.name, hexText(entry.value));
            if (!entry.note.isEmpty()) document.note(entry.note);
        }
    }
    if (!result.stackFrames.empty()) {
        document.section(QStringLiteral("疑似调用栈"));
        document.note(QStringLiteral("由栈内存扫描重建，无符号；顺序为近似值，可能含残留帧"));
        for (const auto& frame : result.stackFrames) {
            document.section(QStringLiteral("TID %1 / #%2").arg(frame.threadId).arg(frame.index));
            document.field(QStringLiteral("地址"), hexText(frame.address));
            document.field(QStringLiteral("符号"), frame.symbolText);
            document.field(QStringLiteral("来源"), frame.fromContext ? QStringLiteral("上下文") : QStringLiteral("栈扫描"), true);
        }
    }
    if (!result.streams.empty()) {
        document.section(QStringLiteral("数据流"));
        for (const auto& stream : result.streams) {
            document.section(stream.typeName);
            document.field(QStringLiteral("类型"), QString::number(stream.type));
            document.field(QStringLiteral("偏移"), hexText(stream.rva));
            document.field(QStringLiteral("大小"), QString::number(stream.size));
        }
    }
    if (!result.modules.empty()) {
        document.section(QStringLiteral("模块"));
        document.field(QStringLiteral("数量"), QString::number(result.modules.size()));
        for (const auto& module : result.modules) {
            document.section(module.name);
            document.field(QStringLiteral("基址"), hexText(module.base));
            document.field(QStringLiteral("大小"), hexText(module.size));
            if (!module.version.isEmpty()) document.field(QStringLiteral("版本"), module.version);
            if (!module.timestampText.isEmpty()) document.field(QStringLiteral("时间戳"), module.timestampText);
            if (!module.pdbName.isEmpty()) document.field(QStringLiteral("PDB"), module.pdbName);
        }
    }
    if (!result.threads.empty()) {
        document.section(QStringLiteral("线程"));
        document.field(QStringLiteral("数量"), QString::number(result.threads.size()));
        for (const auto& thread : result.threads) {
            document.section(QStringLiteral("TID %1").arg(thread.threadId));
            document.field(QStringLiteral("崩溃线程"), thread.faulting ? QStringLiteral("是") : QStringLiteral("否"), true);
            if (!thread.name.isEmpty()) document.field(QStringLiteral("名称"), thread.name);
            if (thread.instructionPointer != 0) document.field(QStringLiteral("IP"), hexText(thread.instructionPointer));
            document.field(QStringLiteral("TEB"), hexText(thread.teb));
            document.field(QStringLiteral("栈"), hexText(thread.stackBase));
            document.field(QStringLiteral("栈大小"), QString::number(thread.stackSize));
        }
    }
    if (!result.memoryRegions.empty()) {
        document.section(QStringLiteral("内存区域"));
        document.field(QStringLiteral("总数"), QString::number(result.memoryRegionTotal));
        const std::size_t reportRows = std::min(result.memoryRegions.size(), kReportMemoryRowLimit);
        for (std::size_t index = 0; index < reportRows; ++index) {
            const auto& region = result.memoryRegions[index];
            document.section(hexText(region.base));
            document.field(QStringLiteral("大小"), hexText(region.size));
            if (!region.state.isEmpty()) document.field(QStringLiteral("状态"), region.state, true);
            if (!region.protect.isEmpty()) document.field(QStringLiteral("保护"), region.protect, true);
            if (!region.type.isEmpty()) document.field(QStringLiteral("类型"), region.type, true);
        }
        if (result.memoryRegions.size() > reportRows) document.note(QStringLiteral("(其余 %1 条内存区域未列入报告)").arg(result.memoryRegions.size() - reportRows));
    }
    if (!result.handles.empty()) {
        document.section(QStringLiteral("句柄"));
        for (const auto& handle : result.handles) {
            document.section(hexText(handle.handleValue));
            document.field(QStringLiteral("类型"), handle.typeName);
            if (!handle.objectName.isEmpty()) document.field(QStringLiteral("对象名称"), handle.objectName);
        }
    }
    if (!result.unloadedModules.empty()) {
        document.section(QStringLiteral("已卸载模块"));
        for (const auto& module : result.unloadedModules) {
            document.section(module.name);
            document.field(QStringLiteral("基址"), hexText(module.base));
            document.field(QStringLiteral("大小"), hexText(module.size));
        }
    }
    if (!result.diagnostics.isEmpty()) {
        document.section(QStringLiteral("解析告警"));
        for (const auto& diagnostic : result.diagnostics) document.note(diagnostic);
    }
    return document;
}

QString MinidumpDock::rawMemoryNavigationId(const std::size_t index) const
{
    // 同一已接受结果代次内稳定；重新装载结果后旧block ID不能指向另一份dump。
    return QStringLiteral("minidump.raw:%1:%2").arg(m_resultProjectionGeneration).arg(index);
}
QString MinidumpDock::rawMemoryNavigationTitle(const std::size_t index) const
{
    return QStringLiteral("%1 %2").arg(translated("minidump.raw.block", "数据块")).arg(index + 1);
}
ks::ui::PageNavigationCatalog MinidumpDock::navigationCatalog(const MinidumpDock* existing)
{
    ks::ui::PageNavigationCatalog catalog;
    // 无解析结果就没有结果Tab，不能为目录创建/解析一份转储。
    if (!existing || !existing->m_resultTabs) return catalog;
    const QString rawId = QStringLiteral("minidump.tab.raw_memory");
    bool hasRaw = false;
    for (int index = 0; index < existing->m_resultTabs->count(); ++index)
    {
        QWidget* page = existing->m_resultTabs->widget(index);
        const QString id = page->property("ksword_navigation_descriptor_id").toString();
        if (id.isEmpty()) return {{}}; // 不完整迁移保持central真实回退，不能漏实际目录。
        catalog.append({id, {}, existing->m_resultTabs->tabText(index), {},
            existing->m_resultTabs->isTabVisible(index), existing->m_resultTabs->isTabEnabled(index),
            existing->m_resultTabs->currentIndex() == index});
        if (id == rawId) hasRaw = true;
    }
    if (hasRaw && existing->m_lastResult)
    {
        for (std::size_t index = 0; index < existing->m_lastResult->byteBlocks.size(); ++index)
        {
            const QString id = existing->rawMemoryNavigationId(index);
            ks::ui::PageNavigationDescriptor descriptor{id, rawId, existing->rawMemoryNavigationTitle(index), {}, true, true, index == 0};
            if (existing->m_rawMemoryTabs && index < static_cast<std::size_t>(existing->m_rawMemoryTabs->count()))
            {
                const int tab = static_cast<int>(index);
                QWidget* page = existing->m_rawMemoryTabs->widget(tab);
                if (page && page->property("ksword_navigation_descriptor_id").toString() == id)
                {
                    descriptor.title = existing->m_rawMemoryTabs->tabText(tab);
                    descriptor.visible = existing->m_rawMemoryTabs->isTabVisible(tab);
                    descriptor.enabled = existing->m_rawMemoryTabs->isTabEnabled(tab);
                    descriptor.current = existing->m_rawMemoryTabs->currentIndex() == tab;
                }
            }
            catalog.append(std::move(descriptor));
        }
    }
    return catalog;
}
QWidget* MinidumpDock::activateNavigationDescriptor(const QString& id)
{
    const QPointer<MinidumpDock> self(this);
    const auto permitted = [self, &id]() -> std::optional<QString> {
        if (!self) return std::nullopt;
        const auto resolved = ks::ui::ResolvePageNavigationCatalog(navigationCatalog(self), QString());
        if (!resolved) return std::nullopt;
        for (const auto& entry : *resolved)
            if (entry.page.id == id && entry.page.visible && entry.page.enabled) return entry.page.parentId;
        return std::nullopt;
    };
    const auto parent = permitted();
    if (!parent) return nullptr;
    const auto findPage = [](QTabWidget* tabs, const QString& key) -> QWidget* {
        if (!tabs) return nullptr;
        for (int index = 0; index < tabs->count(); ++index)
            if (QWidget* page = tabs->widget(index); page && page->property("ksword_navigation_descriptor_id").toString() == key)
                return page;
        return nullptr;
    };
    const auto selectPage = [](QPointer<QTabWidget> tabs, QPointer<QWidget> page) -> bool {
        if (!tabs || !page) return false;
        const auto valid = [&]() { const int index = tabs && page ? tabs->indexOf(page) : -1;
            return index >= 0 && tabs->isTabVisible(index) && tabs->isTabEnabled(index); };
        if (!valid()) return false;
        ks::ui::PrepareWidgetThemeBindingsForShow(page);
        if (!valid()) return false;
        tabs->setCurrentWidget(page);
        if (!valid() || tabs->currentWidget() != page) return false;
        ks::ui::PrepareWidgetThemeBindingsForShow(page);
        return valid() && tabs->currentWidget() == page;
    };
    QPointer<QTabWidget> rootTabs = m_resultTabs;
    QPointer<QWidget> rootPage = findPage(rootTabs, parent->isEmpty() ? id : *parent);
    if (!selectPage(rootTabs, rootPage) || !self) return nullptr;
    renderCurrentResultPage(); // 只接受具体目标后，沿原lazy投影入口准备目标结果壳。
    if (!self || !rootTabs || !rootPage || rootTabs->currentWidget() != rootPage || !permitted()) return nullptr;
    QPointer<QWidget> target = rootPage;
    QPointer<QTabWidget> rawTabs;
    if (!parent->isEmpty())
    {
        rawTabs = m_rawMemoryTabs;
        target = findPage(rawTabs, id);
        if (!selectPage(rawTabs, target) || !self) return nullptr;
    }
    if (!self || !rootTabs || !rootPage || !target || rootTabs->currentWidget() != rootPage
        || (!parent->isEmpty() && (!rawTabs || rawTabs->currentWidget() != target)) || !permitted()) return nullptr;
    return target.data();
}
namespace { const bool kMinidumpNavigationRegistered =
    ks::ui::RegisterTypedPageNavigationCatalog<MinidumpDock>(QStringLiteral("misc.minidump")); }
