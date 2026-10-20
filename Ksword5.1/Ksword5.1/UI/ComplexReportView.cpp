#include "../Framework.h"
#include "./ComplexReportView.h"
#include "./ComplexReportModel.h"
#include "./ComplexReportDelegate.h"
#include "./AsyncUiDispatcher.h"
#include "./CodeEditorWidget.h"
#include "./SegmentedControl.h"
#include "./FlatButtonTheme.h"
#include "./ThemeBinding.h"
#include "./ToolbarMetrics.h"
#include "./PrimaryPageStyle.h"
#include "./TablePresentation.h"
#include "./TableColumnAutoFit.h"
#include "./VisibleTableWidget.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QShowEvent>
#include <QThreadPool>
#include <QTimer>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace
{
    const QString kDiagnostics = QStringLiteral("ksword_report_diagnostics"); // 仅用于导航身份，不显示为业务文本。
    QString text(const QString& source) { return ks::i18n::sourceText(source); }

    // 只在恢复结构或当前搜索结果时遍历模型，不构造每字段控件。
    void visit(const QAbstractItemModel* model, const QModelIndex& parent,
        const std::function<void(const QModelIndex&)>& action)
    {
        const QPointer<QAbstractItemModel> alive(const_cast<QAbstractItemModel*>(model));
        for (int row = 0; alive && row < model->rowCount(parent); ++row)
        {
            const QModelIndex index = model->index(row, 0, parent);
            action(index);
            if (!alive) return;
            visit(model, index, action);
        }
    }


}

namespace ks::ui
{
    struct ComplexReportView::ReadingRestore
    {
        QSet<QString> collapse;
        QSet<QString> expand;
        QSet<QString>::const_iterator collapsePosition;
        QSet<QString>::const_iterator expandPosition;
        QString selected;
        int column = 0;
        int scroll = 0;
        quint64 documentTicket = 0;
        quint64 filterTicket = 0;
    };

    ComplexReportView::ComplexReportView(QWidget* parent)
        : QWidget(parent), m_dispatcher(std::make_shared<AsyncUiDispatcher>(this))
    {
        setObjectName(QStringLiteral("ksword_complex_report"));
        setMinimumSize(0, 0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(6);
        auto* navigation = new QHBoxLayout(); // 两个直接选择器替代摘要跳转和多层折叠路径。
        navigation->setContentsMargins(0, 0, 0, 0);
        m_scopes = new QComboBox(this);
        m_scopes->setObjectName(QStringLiteral("ksword_report_scope"));
        m_types = new QComboBox(this);
        m_types->setObjectName(QStringLiteral("ksword_report_type"));
        for (auto* combo : {m_scopes, m_types})
        {
            combo->setMinimumWidth(0);
            combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(10);
            StylePrimaryCombo(combo);
            StyleStandaloneInput(combo);
        }
        navigation->addWidget(m_scopes, 2);
        navigation->addWidget(m_types, 3);
        m_count = new QLabel(this);
        m_count->setMinimumWidth(0);
        m_count->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        navigation->addWidget(m_count, 1);
        m_diagnostics = new QToolButton(this);
        m_diagnostics->setObjectName(QStringLiteral("ksword_report_diagnostics"));
        ApplyFlatButtonTheme(m_diagnostics, FlatButtonTone::Neutral, FlatButtonAppearance::Flat);
        navigation->addWidget(m_diagnostics);
        m_modes = new QTabBar(this);
        m_modes->setObjectName(QStringLiteral("ksword_report_mode"));
        m_modes->addTab(QString());
        m_modes->addTab(QString());
        m_modes->setExpanding(false);
        m_modes->setDrawBase(false);
        InstallSegmentedTabBar(m_modes);
        navigation->addWidget(m_modes);
        NormalizeToolbarRow(navigation);
        m_layout->addLayout(navigation);

        // 原生三列表承担扫描、选择与复制；操作条为 None，整份文档动作由原投影拥有。
        m_model = new ComplexReportModel(this);
        m_proxy = new ComplexReportFilter(this);
        m_proxy->setPreparedSearchEnabled(true);
        m_proxy->setSourceModel(m_model);
        m_tree = new QTreeView(this);
        m_tree->setObjectName(QStringLiteral("ksword_report_tree"));
        m_tree->setAccessibleName(text(QStringLiteral("报告字段")));
        m_tree->setProperty("ks_i18n_preserve_model_data", true);
        m_tree->setProperty(TableActionBarModeProperty, static_cast<int>(TableActionBarMode::None));
        m_tree->setModel(m_proxy);
        connect(m_tree, &QTreeView::expanded, this, [this](const QModelIndex& index)
        { m_expandedPaths.insert(index.data(ReportPathRole).toString()); });
        connect(m_tree, &QTreeView::collapsed, this, [this](const QModelIndex& index)
        { m_expandedPaths.remove(index.data(ReportPathRole).toString()); });
        SetTableColumnAutoFitEnabled(m_tree, false); // 三列用途明确，不能按长诊断值的权重把名称压成省略号。
        m_tree->setItemDelegate(new ComplexReportDelegate(m_tree));
        m_tree->setUniformRowHeights(true);
        m_tree->setIndentation(14);
        m_tree->setAlternatingRowColors(true);
        m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tree->setExpandsOnDoubleClick(false);
        m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_tree->setWordWrap(false);
        m_tree->setMinimumSize(0, 0);
        m_tree->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_tree->header()->setMinimumSectionSize(60);
        m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
        m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        m_tree->setColumnWidth(0, m_catalogNameWidth);
        m_tree->setColumnWidth(1, 96);
        SetTablePresentationDensity(m_tree, TablePresentationDensity::Compact);
        ApplyTablePresentation(m_tree);
        m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_tree, &QTreeView::customContextMenuRequested, this, &ComplexReportView::showContextMenu);
        m_layout->addWidget(m_tree, 1);
        m_empty = new QLabel(this);
        m_empty->setObjectName(QStringLiteral("ksword_report_empty_state"));
        m_empty->setAccessibleName(text(QStringLiteral("报告空态")));
        m_empty->setAlignment(Qt::AlignCenter);
        m_empty->setWordWrap(true);
        m_layout->addWidget(m_empty, 1);

        // 低频展开与范围复制放在一个菜单，正文不再逐分区重复堆复制按钮。
        m_actions = new QToolButton(this);
        m_actions->setObjectName(QStringLiteral("ksword_report_actions"));
        m_actions->setAccessibleName(text(QStringLiteral("报告操作")));
        m_actions->setPopupMode(QToolButton::InstantPopup);
        ApplyFlatButtonTheme(m_actions, FlatButtonTone::Neutral, FlatButtonAppearance::Flat);
        {
            auto* menu = new QMenu(m_actions); // 菜单在首次点击前安装，避免 InstantPopup 的首次动作丢失。
            menu->setStyleSheet(KswordTheme::ContextMenuStyle());
            auto* expand = menu->addAction(text(QStringLiteral("展开当前范围")));
            auto* collapse = menu->addAction(text(QStringLiteral("收起当前范围")));
            menu->addSeparator();
            auto* copy = menu->addAction(text(QStringLiteral("复制当前范围")));
            connect(expand, &QAction::triggered, this, [this]() { setCurrentExpanded(true); });
            connect(collapse, &QAction::triggered, this, [this]() { setCurrentExpanded(false); });
            connect(copy, &QAction::triggered, this, [this]()
            {
                const QString value = currentScopeText();
                if (auto* clipboard = QApplication::clipboard()) clipboard->setText(value);
            });
            m_actions->setMenu(menu);
        }

        connect(m_scopes, &QComboBox::currentIndexChanged, this, [this]()
        {
            if (m_updatingNavigation) return;
            m_lastTypes.insert(m_scopePath, m_typePath);
            m_scopePath = m_scopes->currentData().toString();
            m_typePath = m_lastTypes.value(m_scopePath);
            updateTypes(!m_lastTypes.contains(m_scopePath));
            applyNavigation();
        });
        connect(m_types, &QComboBox::currentIndexChanged, this, [this]()
        {
            if (m_updatingNavigation) return;
            m_typePath = m_types->currentData().toString();
            m_lastTypes.insert(m_scopePath, m_typePath);
            applyNavigation();
        });
        connect(m_diagnostics, &QToolButton::clicked, this, [this]()
        {
            const int position = m_scopes->findData(kDiagnostics);
            if (position >= 0) m_scopes->setCurrentIndex(position);
        });
        connect(m_modes, &QTabBar::currentChanged, this, [this]()
        {
            // 显隐不改变规范快照/范围/类型；已显示原文继续复用同一编辑器文档和选区。
            updatePresentation();
        });
        connect(m_tree, &QTreeView::clicked, this, [this](const QModelIndex& cell)
        {
            if (cell.column() != 0) return; // 值列单击用于选择和复制，不顺带折叠。
            const QModelIndex index = cell.siblingAtColumn(0);
            const QString path = index.data(ReportPathRole).toString();
            if (static_cast<FieldNode::ReportRole>(index.data(ReportPurposeRole).toInt()) == FieldNode::ReportRole::Collection)
            {
                const int position = m_types->findData(path);
                if (position >= 0 && m_query.isEmpty()) { m_types->setCurrentIndex(position); return; }
            }
            if (m_proxy->rowCount(index) > 0) m_tree->setExpanded(index, !m_tree->isExpanded(index));
        });
        auto* copy = new QShortcut(QKeySequence::Copy, m_tree);
        copy->setContext(Qt::WidgetWithChildrenShortcut);
        connect(copy, &QShortcut::activated, this, [this]() { copyCell(m_tree->currentIndex(), false); });
        auto* copyRecord = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")), m_tree);
        copyRecord->setContext(Qt::WidgetWithChildrenShortcut);
        connect(copyRecord, &QShortcut::activated, this, [this]() { copyCell(m_tree->currentIndex(), true); });
        auto* find = new QShortcut(QKeySequence::Find, m_tree);
        find->setContext(Qt::WidgetWithChildrenShortcut);
        connect(find, &QShortcut::activated, this, [this]() { if (m_search) { m_search->setFocus(); m_search->selectAll(); } });
        const QPointer<ComplexReportView> safe(this);
        BindWidgetTheme(this, [safe]() { if (safe) safe->refreshTheme(); });
        refreshTheme();
    }

    void ComplexReportView::bindSearchField(QLineEdit* search)
    {
        m_search = search;
        updatePresentation();
    }

    void ComplexReportView::changeEvent(QEvent* event)
    {
        const QPointer<ComplexReportView> alive(this);
        QWidget::changeEvent(event);
        if (!alive) return;
        if (event != nullptr && event->type() == QEvent::LanguageChange)
        {
            // 语言是原文版本的一部分；不依赖父投影随后恰好再次提交 document。
            m_rawDirty = true;
            m_language = ks::i18n::LanguageManager::instance().currentLanguageId();
            m_projectionDirty = true;
            ++m_readingGeneration;
            ++m_expansionGeneration;
            ++*m_filterGeneration;
            m_candidate.reset();
            ++*m_prepareGeneration; // Cancel an obsolete language projection before any queued return.
            if (isVisible()) prepareDocument();
            if (!alive) return;
            refreshTheme(); // 当前原文立即更新一次，隐藏原文保留 dirty 到下次显示。
        }
    }

    // 保存稳定路径而非 QModelIndex，文档刷新后仍能恢复对应字段。
    QSet<QString> ComplexReportView::expandedPaths() const
    {
        return m_readingRestore ? m_readingRestore->expand : m_expandedPaths;
    }

    void ComplexReportView::restoreReading(const QSet<QString>& paths, const QString& selected,
        int column, int scroll, bool collapse)
    {
        auto restore = std::shared_ptr<ReadingRestore>(new ReadingRestore, [](ReadingRestore* retired)
        { QThreadPool::globalInstance()->start([retired]() { delete retired; }); });
        if (collapse) restore->collapse = m_expandedPaths;
        restore->expand = paths;
        restore->collapsePosition = restore->collapse.cbegin();
        restore->expandPosition = restore->expand.cbegin();
        restore->selected = selected;
        restore->column = column;
        restore->scroll = scroll;
        restore->documentTicket = m_prepareGeneration->load();
        restore->filterTicket = m_filterGeneration->load();
        // Keep target sets immutable while tree signals build a new current expansion set.
        m_expandedPaths.clear();
        m_readingRestore = std::move(restore);
        continueReadingRestore(++m_readingGeneration);
    }

    void ComplexReportView::continueReadingRestore(quint64 ticket)
    {
        if (ticket != m_readingGeneration || !m_readingRestore || !isVisible() || m_preparing) return;
        const QPointer<ComplexReportView> alive(this);
        const auto request = m_readingRestore;
        const auto documentGeneration = m_prepareGeneration;
        const auto filterGeneration = m_filterGeneration;
        const auto currentRequest = [&]()
        {
            return alive && ticket == m_readingGeneration && documentGeneration->load() == request->documentTicket
                && filterGeneration->load() == request->filterTicket;
        };
        if (!currentRequest()) return;
        QElapsedTimer elapsed;
        elapsed.start();
        int count = 0;
        while (count++ < 128 && elapsed.elapsed() < 4)
        {
            ReadingRestore& restore = *m_readingRestore;
            const bool collapsing = restore.collapsePosition != restore.collapse.cend();
            if (!collapsing && restore.expandPosition == restore.expand.cend()) break;
            const QString path = collapsing ? *restore.collapsePosition++ : *restore.expandPosition++;
            const QModelIndex index = m_proxy->mapFromSource(m_model->indexForPath(path));
            if (index.isValid()) m_tree->setExpanded(index, !collapsing);
            if (!currentRequest() || !m_readingRestore) return;
            if (index.isValid())
            {
                if (collapsing) m_expandedPaths.remove(path);
                else m_expandedPaths.insert(path);
            }
        }
        if (m_readingRestore->collapsePosition != m_readingRestore->collapse.cend()
            || m_readingRestore->expandPosition != m_readingRestore->expand.cend())
        {
            QTimer::singleShot(0, this, [this, ticket]() { continueReadingRestore(ticket); });
            return;
        }
        auto completed = std::move(m_readingRestore);
        const QModelIndex current = m_proxy->mapFromSource(m_model->indexForPath(completed->selected));
        if (current.isValid()) m_tree->setCurrentIndex(current.siblingAtColumn(completed->column));
        if (!currentRequest()) return;
        m_tree->doItemsLayout();
        if (!currentRequest()) return;
        m_tree->verticalScrollBar()->setValue(completed->scroll);
        if (!currentRequest()) return;
        if (m_query.isEmpty() && m_restoreAfterSearch)
        {
            m_beforeSearch.clear();
            m_restoreAfterSearch = false;
        }
        if (!m_query.isEmpty()) expandSearchGroups();
        else if (m_scopePath == kDiagnostics) m_tree->expandToDepth(0);
        if (!currentRequest()) return;
        updatePresentation();
        QThreadPool::globalInstance()->start([retired = std::move(completed)]() {});
    }

    ComplexReportView::~ComplexReportView()
    {
        m_dispatcher->close();
        ++*m_prepareGeneration;
        ++*m_filterGeneration;
        auto pending = std::move(m_requestedDocument);
        auto restore = std::move(m_readingRestore);
        QThreadPool::globalInstance()->start([pending = std::move(pending), restore = std::move(restore)]() {});
    }

    void ComplexReportView::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        if (m_projectionDirty) prepareDocument();
        else if (m_searchPending) scheduleSearch(false);
        else if (m_readingRestore) continueReadingRestore(m_readingGeneration);
        else if (!m_query.isEmpty() && m_searchExpansion && m_expansionOffset < m_searchExpansion->expansionPaths.size())
            continueSearchExpansion(m_expansionGeneration);
    }

    void ComplexReportView::setDocument(const FieldDocument& document)
    {
        ++*m_filterGeneration;
        m_candidate.reset();
        auto retired = std::move(m_requestedDocument);
        m_requestedDocument = document;
        ++m_readingGeneration;
        ++m_expansionGeneration;
        QThreadPool::globalInstance()->start([retired = std::move(retired)]() {});
        m_projectionDirty = true;
        ++*m_prepareGeneration;
        if (isVisible()) prepareDocument();
    }

    void ComplexReportView::prepareDocument()
    {
        const QPointer<ComplexReportView> safe(this);
        const auto generation = m_prepareGeneration;
        const quint64 ticket = ++*generation;
        m_projectionDirty = false;
        m_preparing = true;
        updatePresentation();
        if (!safe || generation->load() != ticket) return;
        const auto translator = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
        const QString language = translator.languageId();
        const auto previous = m_model->snapshot();
        const FieldDocument document = m_requestedDocument;
        const auto dispatcher = m_dispatcher;
        QThreadPool::globalInstance()->start([generation, ticket, translator, language, previous, document, safe, dispatcher]()
        {
            const auto cancelled = [generation, ticket]() { return generation->load() != ticket; };
            auto prepared = ComplexReportModel::prepareDocument(document,
                [translator](const QString& source) { return translator.sourceText(source); }, previous, cancelled);
            if (!prepared.next || cancelled()) return;
            dispatcher->post([safe, generation, ticket, prepared = std::move(prepared), language]() mutable
            {
                if (safe && generation->load() == ticket && !safe->isVisible())
                {
                    safe->m_projectionDirty = true;
                    safe->m_preparing = false; // Hidden pages retain the latest canonical request only.
                }
                if (safe && generation->load() == ticket && safe->isVisible())
                {
                    safe->m_candidate = std::make_shared<ComplexReportModel::PreparedDocument>(std::move(prepared));
                    safe->m_language = language;
                    safe->scheduleSearch(false);
                }
                // The previous large state and request closures are retired outside the GUI thread,
                // including stale results delivered after a newer request or window destruction.
                QThreadPool::globalInstance()->start([retired = std::move(prepared)]() {});
            });
        });
    }

    void ComplexReportView::scheduleSearch(bool debounce)
    {
        const quint64 ticket = ++*m_filterGeneration;
        ++m_expansionGeneration;
        if (m_readingRestore && !m_preparing)
        {
            ++m_readingGeneration;
            auto retired = std::move(m_readingRestore);
            QThreadPool::globalInstance()->start([retired = std::move(retired)]() {});
        }
        m_searchPending = true;
        const QPointer<ComplexReportView> alive(this);
        const auto documentGeneration = m_prepareGeneration;
        const quint64 documentTicket = documentGeneration->load();
        updatePresentation();
        if (!alive || m_filterGeneration->load() != ticket || documentGeneration->load() != documentTicket) return;
        if (m_preparing && !m_candidate) return; // Index preparation will schedule the latest query.
        QTimer::singleShot(debounce ? 80 : 0, this, [this, ticket]()
        {
            if (m_filterGeneration->load() == ticket) prepareSearch();
        });
    }

    void ComplexReportView::prepareSearch()
    {
        if (!isVisible()) return;
        const auto candidate = m_candidate;
        const auto source = candidate ? candidate->next : m_model->snapshot();
        const auto generation = m_filterGeneration;
        const auto documentGeneration = m_prepareGeneration;
        const quint64 ticket = generation->load();
        const quint64 documentTicket = documentGeneration->load();
        const QString query = m_query;
        const QSet<QString> expandedState = expandedPaths();
        QString scope = !m_initialized ? ComplexReportModel::preferredScopePath(source)
            : m_scopePath == kDiagnostics ? QString() : m_scopePath;
        if (!scope.isEmpty() && !ComplexReportModel::containsPath(source, scope)) scope.clear();
        const bool diagnostics = m_scopePath == kDiagnostics;
        const QPointer<ComplexReportView> safe(this);
        const auto dispatcher = m_dispatcher;
        QThreadPool::globalInstance()->start([source, candidate, generation, ticket, documentGeneration,
            documentTicket, query, scope, diagnostics, expandedState, safe, dispatcher]()
        {
            const auto cancelled = [generation, ticket, documentGeneration, documentTicket]()
            { return generation->load() != ticket || documentGeneration->load() != documentTicket; };
            auto prepared = ComplexReportModel::prepareFilter(source, query, scope, diagnostics, cancelled);
            if (!prepared || cancelled()) return;
            auto independentExpansion = std::shared_ptr<QSet<QString>>(new QSet<QString>(expandedState),
                [](QSet<QString>* retired) { QThreadPool::globalInstance()->start([retired]() { delete retired; }); });
            independentExpansion->detach();
            if (cancelled()) return;
            dispatcher->post([safe, prepared = std::move(prepared), candidate,
                generation, ticket, documentGeneration, documentTicket, independentExpansion]()
            {
                auto expectedCandidate = candidate;
                const auto current = [&]()
                {
                    return safe && generation->load() == ticket && documentGeneration->load() == documentTicket
                        && safe->m_candidate == expectedCandidate;
                };
                if (!current()) return;
                if (!safe->isVisible())
                {
                    if (candidate) safe->m_projectionDirty = true;
                    safe->m_preparing = false;
                    safe->m_searchPending = true;
                    return;
                }
                QElapsedTimer elapsed;
                elapsed.start();
                const QString selected = safe->m_readingRestore ? safe->m_readingRestore->selected
                    : safe->m_tree->currentIndex().data(ReportPathRole).toString();
                const int column = safe->m_readingRestore ? safe->m_readingRestore->column
                    : qMax(0, safe->m_tree->currentIndex().column());
                const int scroll = safe->m_readingRestore ? safe->m_readingRestore->scroll
                    : safe->m_tree->verticalScrollBar()->value();
                QSet<QString> expanded;
                const bool wasRestoring = bool(safe->m_readingRestore);
                if (candidate && (candidate->reset || wasRestoring)) expanded = safe->expandedPaths();
                safe->m_expandedPaths.swap(*independentExpansion);
                safe->m_proxy->stagePrepared(prepared);
                if (!current()) return;
                if (candidate)
                {
                    const auto installed = safe->m_model->installPrepared(*candidate);
                    if (!current()) return;
                    if (installed == ComplexReportModel::InstallResult::Rejected)
                    {
                        // A prior reset may have exchanged State after this request captured
                        // its base. It is still the latest canonical input, so retry that input
                        // against the installed State instead of clearing loading as a success.
                        safe->m_candidate.reset();
                        safe->prepareDocument();
                        return;
                    }
                    safe->m_rawDirty = true;
                    safe->updateNavigation(!safe->m_initialized);
                    if (!current()) return;
                }
                safe->m_proxy->applyPrepared(prepared);
                if (!current()) return;
                safe->m_tree->setRootIndex(safe->displayRoot());
                if (!current()) return;
                if (candidate)
                {
                    safe->m_candidate.reset();
                    expectedCandidate.reset();
                }
                safe->m_preparing = false;
                safe->m_searchPending = false;
                bool restoring = false;
                if (safe->m_restoreAfterSearch && safe->m_query.isEmpty())
                {
                    safe->restoreReading(safe->m_beforeSearch, safe->m_beforeSearchSelected,
                        safe->m_beforeSearchColumn, safe->m_beforeSearchScroll, true);
                    restoring = true;
                }
                else if (candidate && (candidate->reset || wasRestoring))
                {
                    safe->restoreReading(expanded, selected, column, scroll, false);
                    restoring = true;
                }
                if (!current()) return;
                if (!restoring && !safe->m_query.isEmpty()) safe->expandSearchGroups();
                else if (!restoring && safe->m_scopePath == kDiagnostics) safe->m_tree->expandToDepth(0);
                if (!current()) return;
                safe->updatePresentation();
                if (!current()) return;
                safe->setProperty("ksword_report_update_us", elapsed.nsecsElapsed() / 1000);
                QThreadPool::globalInstance()->start([retired = independentExpansion]() {});
            });
        });
    }

    // 顶层目录由生产者标记，诊断合为一个入口；默认范围直接承接当前业务对象。
    void ComplexReportView::updateNavigation(bool firstDocument)
    {
        m_updatingNavigation = true;
        const QSignalBlocker blocker(m_scopes);
        m_scopes->clear();
        m_scopes->addItem(text(QStringLiteral("全部报告")), QString());
        QString preferred;
        int catalogCount = 0;
        int diagnostics = 0;
        for (const QModelIndex& section : m_model->sectionIndexes())
        {
            if (static_cast<FieldNode::ReportRole>(section.data(ReportPurposeRole).toInt()) == FieldNode::ReportRole::Catalog)
            {
                const QString path = section.data(ReportPathRole).toString();
                m_scopes->addItem(section.data(ReportSummaryRole).toString(), path);
                m_scopes->setItemData(m_scopes->count() - 1, section.data(ReportNameRole), Qt::ToolTipRole);
                if (section.data(ReportPreferredScopeRole).toBool()) preferred = path;
                ++catalogCount;
            }
            else if (static_cast<FieldNode::Status>(section.data(ReportStatusRole).toInt()) != FieldNode::Status::Neutral) ++diagnostics;
        }
        if (catalogCount > 0)
        {
            m_scopes->addItem(text(QStringLiteral("运行诊断")), kDiagnostics);
            if (firstDocument) m_scopePath = preferred.isEmpty() ? m_scopes->itemData(1).toString() : preferred;
            m_initialized = true;
        }
        int position = m_scopes->findData(m_scopePath);
        if (position < 0) { position = 0; m_scopePath.clear(); }
        m_scopes->setCurrentIndex(position);
        m_diagnostics->setVisible(catalogCount > 0);
        m_diagnostics->setText(text(QStringLiteral("诊断 %1")).arg(diagnostics));
        m_updatingNavigation = false;
        updateTypes(firstDocument);
        m_proxy->setDiagnosticsOnly(m_scopePath == kDiagnostics);
        m_proxy->setScopePath(m_scopePath == kDiagnostics ? QString() : m_scopePath);
    }

    void ComplexReportView::updateTypes(bool selectFirst)
    {
        const QSignalBlocker blocker(m_types);
        m_types->clear();
        m_types->addItem(text(QStringLiteral("全部类型")), QString());
        const QModelIndex section = scopeIndex();
        for (int row = 0; row < m_model->rowCount(section) && section.isValid(); ++row)
        {
            const QModelIndex type = m_model->index(row, 0, section);
            if (static_cast<FieldNode::ReportRole>(type.data(ReportPurposeRole).toInt()) == FieldNode::ReportRole::Collection)
                m_types->addItem(type.data(ReportNameRole).toString(), type.data(ReportPathRole));
        }
        int position = m_types->findData(m_typePath);
        if (selectFirst && m_types->count() > 1) position = 1;
        if (position < 0) position = 0;
        m_types->setCurrentIndex(position);
        m_typePath = m_types->currentData().toString();
        m_types->setVisible(m_types->count() > 1);
    }

    QModelIndex ComplexReportView::scopeIndex() const { return m_model->indexForPath(m_scopePath); }

    QModelIndex ComplexReportView::displayRoot() const
    {
        const QString path = m_query.isEmpty() && !m_typePath.isEmpty() ? m_typePath : m_scopePath;
        return m_proxy->mapFromSource(m_model->indexForPath(path));
    }

    void ComplexReportView::applyNavigation()
    {
        if (!m_query.isEmpty())
        {
            m_beforeSearch.clear(); // 搜索中换范围时不能把上一范围的阅读位置带到新范围。
            m_beforeSearchSelected.clear();
            m_beforeSearchScroll = 0;
        }
        m_tree->scrollToTop();
        m_rawDirty = true;
        scheduleSearch(false);
    }

    // 多词搜索在当前范围所有类型中进行；只展开到字段列表，不把每个命中的属性再全部铺开。
    void ComplexReportView::setSearchText(const QString& query)
    {
        const QString next = query.trimmed();
        if (next == m_query) return;
        if (m_query.isEmpty() && !next.isEmpty() && !m_restoreAfterSearch)
        {
            m_beforeSearch = expandedPaths();
            m_beforeSearchSelected = m_readingRestore ? m_readingRestore->selected
                : m_tree->currentIndex().data(ReportPathRole).toString();
            m_beforeSearchColumn = m_readingRestore ? m_readingRestore->column : qMax(0, m_tree->currentIndex().column());
            m_beforeSearchScroll = m_readingRestore ? m_readingRestore->scroll : m_tree->verticalScrollBar()->value();
        }
        m_restoreAfterSearch = next.isEmpty();
        m_query = next;
        scheduleSearch(!next.isEmpty());
    }

    void ComplexReportView::expandSearchGroups()
    {
        m_searchExpansion = m_proxy->preparedSnapshot();
        m_expansionOffset = 0;
        continueSearchExpansion(++m_expansionGeneration);
    }

    void ComplexReportView::continueSearchExpansion(quint64 ticket)
    {
        if (ticket != m_expansionGeneration || m_query.isEmpty() || !isVisible() || m_preparing || m_searchPending) return;
        const auto expansion = m_searchExpansion;
        if (!expansion) return;
        QElapsedTimer elapsed;
        elapsed.start();
        int count = 0;
        const QPointer<ComplexReportView> alive(this);
        const auto documentGeneration = m_prepareGeneration;
        const auto filterGeneration = m_filterGeneration;
        const quint64 documentTicket = documentGeneration->load();
        const quint64 filterTicket = filterGeneration->load();
        while (m_expansionOffset < expansion->expansionPaths.size() && count++ < 128 && elapsed.elapsed() < 4)
        {
            const QModelIndex index = m_proxy->mapFromSource(m_model->indexForPath(expansion->expansionPaths.at(m_expansionOffset++)));
            if (index.isValid()) m_tree->setExpanded(index, true);
            if (!alive || ticket != m_expansionGeneration || documentGeneration->load() != documentTicket
                || filterGeneration->load() != filterTicket) return;
        }
        if (m_expansionOffset < expansion->expansionPaths.size())
            QTimer::singleShot(0, this, [this, ticket]() { continueSearchExpansion(ticket); });
    }

    void ComplexReportView::setCurrentExpanded(bool expanded)
    {
        visit(m_proxy, displayRoot(), [this, expanded](const QModelIndex& index)
        {
            if (m_proxy->rowCount(index) > 0) m_tree->setExpanded(index, expanded);
        });
    }

    void ComplexReportView::updatePresentation()
    {
        const bool raw = m_modes->currentIndex() == 1;
        const bool empty = m_proxy->rowCount(displayRoot()) == 0;
        m_tree->setEnabled(!m_preparing && !m_searchPending && !m_readingRestore);
        m_scopes->setEnabled(!m_preparing);
        m_actions->setEnabled(!m_preparing && !m_searchPending && !m_readingRestore);
        m_diagnostics->setEnabled(!m_preparing);
        const bool diagnostics = m_scopePath == kDiagnostics;
        if (diagnostics != m_diagnosticColumns)
        {
            // 用户拖动的列宽保存在对应模式，普通主题/数据刷新不会覆盖手工调整。
            if (m_diagnosticColumns) m_diagnosticNameWidth = m_tree->columnWidth(0);
            else m_catalogNameWidth = m_tree->columnWidth(0);
            m_diagnosticColumns = diagnostics;
            m_tree->setColumnHidden(1, diagnostics);
            m_tree->setColumnWidth(0, diagnostics ? m_diagnosticNameWidth : m_catalogNameWidth);
        }
        m_tree->setVisible(!raw && !empty);
        m_empty->setText(text(m_preparing || m_searchPending || m_readingRestore ? QStringLiteral("正在加载...") : m_query.isEmpty() ? QStringLiteral("暂无报告数据") : QStringLiteral("没有匹配的属性")));
        m_empty->setVisible(!raw && empty);
        m_types->setEnabled(!m_preparing && (raw || m_query.isEmpty()));
        m_count->setText(m_preparing || m_searchPending || m_readingRestore ? text(QStringLiteral("正在加载...")) : raw ? text(QStringLiteral("完整原文")) : !m_query.isEmpty()
            ? text(QStringLiteral("%1 项匹配")).arg(m_proxy->matchCount(displayRoot()))
            : text(QStringLiteral("%1 行")).arg(m_proxy->rowCount(displayRoot())));
        if (m_search)
        {
            m_search->setEnabled(!raw);
            m_search->setPlaceholderText(text(raw ? QStringLiteral("原文中使用 Ctrl+F 查找")
                : QStringLiteral("搜索当前范围的字段、偏移或类型")));
        }
        if (raw)
        {
            if (!m_editor)
            {
                m_editor = new CodeEditorWidget(this);
                m_editor->setObjectName(QStringLiteral("ksword_report_raw"));
                m_editor->setProperty("ksword_detail_keep_layout", true);
                m_editor->setReadOnly(true);
                m_editor->setReadOnlyExportEnabled(true);
                m_editor->setMaximumBlockCount(0);
                m_editor->setWordWrapEnabled(false);
                m_layout->addWidget(m_editor, 1);
            }
            const QPointer<ComplexReportView> alive(this);
            if (!m_preparing && !m_readingRestore) updateRawText();
            if (!alive) return;
            m_editor->show();
        }
        else if (m_editor) m_editor->hide();
    }

    // 原文从同一规范快照按范围序列化，不受字段搜索和折叠影响，也不维护另一份业务来源。
    void ComplexReportView::updateRawText()
    {
        if (!m_editor || !m_rawDirty) return;
        const QPointer<ComplexReportView> alive(this);
        const auto documentGeneration = m_prepareGeneration;
        const auto filterGeneration = m_filterGeneration;
        const quint64 documentTicket = documentGeneration->load();
        const quint64 filterTicket = filterGeneration->load();
        m_editor->replaceRawText(currentScopeText(), false);
        if (!alive || documentGeneration->load() != documentTicket || filterGeneration->load() != filterTicket) return;
        m_rawDirty = false;
    }

    QString ComplexReportView::currentScopeText() const
    {
        QString raw;
        if (m_scopePath == kDiagnostics)
        {
            FieldDocument diagnostics;
            for (const FieldNode& node : m_model->document().nodes)
                if (node.reportRole != FieldNode::ReportRole::Catalog) diagnostics.nodes.append(node);
            raw = diagnostics.toPlainText(true);
        }
        else raw = m_model->textForIndex(m_model->indexForPath(m_typePath.isEmpty() ? m_scopePath : m_typePath));
        return raw;
    }

    void ComplexReportView::copyCell(const QModelIndex& index, bool wholeRecord)
    {
        if (!index.isValid()) return;
        const QString value = wholeRecord ? m_model->textForIndex(m_proxy->mapToSource(index)) : index.data().toString();
        if (auto* clipboard = QApplication::clipboard()) clipboard->setText(value);
    }

    void ComplexReportView::showContextMenu(const QPoint& position)
    {
        const QModelIndex index = m_tree->indexAt(position);
        if (!index.isValid()) return;
        const QString cell = index.data().toString();
        const QString record = m_model->textForIndex(m_proxy->mapToSource(index));
        const QPoint global = m_tree->viewport()->mapToGlobal(position);
        QMenu menu;
        menu.setStyleSheet(KswordTheme::ContextMenuStyle());
        connect(this, &QObject::destroyed, &menu, &QMenu::close);
        auto* copy = menu.addAction(text(QStringLiteral("复制单元格")));
        copy->setShortcut(QKeySequence::Copy);
        auto* copyRecord = menu.addAction(text(QStringLiteral("复制完整字段")));
        copyRecord->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
        const QAction* chosen = menu.exec(global);
        if (chosen == copy || chosen == copyRecord)
            if (auto* clipboard = QApplication::clipboard()) clipboard->setText(chosen == copy ? cell : record);
    }

    // 主题只更新共享外观和入口文字；不重建数据、原文或每行图标。
    void ComplexReportView::refreshTheme()
    {
        m_scopes->setAccessibleName(text(QStringLiteral("报告范围")));
        m_scopes->setToolTip(text(QStringLiteral("直接切换报告范围")));
        m_types->setAccessibleName(text(QStringLiteral("结构类型")));
        m_types->setToolTip(text(QStringLiteral("直接选择类型；搜索时自动查找当前范围的所有类型")));
        m_diagnostics->setToolTip(text(QStringLiteral("查看运行状态、身份校验和采样诊断")));
        m_diagnostics->setAccessibleName(text(QStringLiteral("运行诊断")));
        m_modes->setAccessibleName(text(QStringLiteral("报告视图")));
        m_tree->setAccessibleName(text(QStringLiteral("报告字段")));
        m_empty->setAccessibleName(text(QStringLiteral("报告空态")));
        m_actions->setAccessibleName(text(QStringLiteral("报告操作")));
        m_count->setAccessibleName(text(QStringLiteral("报告计数")));
        m_modes->setTabText(0, text(QStringLiteral("字段")));
        m_modes->setTabText(1, text(QStringLiteral("原文")));
        m_modes->setTabToolTip(0, text(QStringLiteral("紧凑字段表；单击字段名展开附加属性，Ctrl+C 复制当前单元格")));
        m_modes->setTabToolTip(1, text(QStringLiteral("查看当前范围完整原文，支持搜索、选择复制和导出")));
        m_actions->setText(text(QStringLiteral("更多")));
        m_actions->setToolTip(text(QStringLiteral("展开、收起与复制当前范围")));
        const auto actions = m_actions->menu()->actions();
        actions.at(0)->setText(text(QStringLiteral("展开当前范围")));
        actions.at(1)->setText(text(QStringLiteral("收起当前范围")));
        actions.at(3)->setText(text(QStringLiteral("复制当前范围")));
        m_actions->menu()->setStyleSheet(KswordTheme::ContextMenuStyle());
        m_count->setToolTip(text(QStringLiteral("当前范围与搜索结果数量；原始总数和来源可在原文中查看")));
        ApplyTablePresentation(m_tree);
        updatePresentation();
    }
}
