#include "TableSearchSupport.h"
#include "TableRowFilterSupport.h"
#include "TableFreezeSupport.h"
#include "./FlatButtonTheme.h"
#include "ThemeBinding.h"

#include "GlobalUiSearch.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QAbstractItemModel>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>

#include <QEvent>
#include <QElapsedTimer>
#include <QDynamicPropertyChangeEvent>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHash>
#include <QSet>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMargins>
#include <QPoint>
#include <QScrollBar>

#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVariant>

#include <algorithm>
#include <functional>

namespace
{
    constexpr char kSearchSupportObjectName[] = "KSWORD_TABLE_SEARCH_SUPPORT";
    constexpr char kSearchSupportPropertyName[] = "KSWORD_TABLE_SEARCH_SUPPORT_OBJECT";
    constexpr char kTableActionBarObjectName[] = "KSWORD_TABLE_INTERACTION_ACTION_BAR";
    constexpr char kGenericSearchControlPropertyName[] = "ksword_generic_table_search_control";
    constexpr char kGlobalSearchInputPropertyName[] = "ksword_global_ui_search_input";
    constexpr char kExplicitTableNamePropertyName[] = "ksword_table_search_name";
    constexpr int kCollapsedSearchWidth = 28;
    constexpr int kSearchControlHeight = 24;
    constexpr int kSearchOuterMargin = 4;

    // textLooksLikeSearchControl：用控件元数据识别页面已有的搜索/筛选输入框。
    bool textLooksLikeSearchControl(const QString& sourceText)
    {
        const QString normalizedText = sourceText.trimmed().toLower();
        return normalizedText.contains(QStringLiteral("搜索"))
            || normalizedText.contains(QStringLiteral("筛选"))
            || normalizedText.contains(QStringLiteral("过滤"))
            || normalizedText.contains(QStringLiteral("search"))
            || normalizedText.contains(QStringLiteral("filter"))
            || normalizedText.contains(QStringLiteral("find"));
    }

    // nearestPageRoot：返回表格所在的最近页签页面；找不到时回退到顶层窗口内容根。
    QWidget* nearestPageRoot(QWidget* childWidget)
    {
        QWidget* fallbackRoot = childWidget;
        for (QWidget* cursorWidget = childWidget;
             cursorWidget != nullptr;
             cursorWidget = cursorWidget->parentWidget())
        {
            fallbackRoot = cursorWidget;
            QWidget* parentWidget = cursorWidget->parentWidget();
            if (qobject_cast<QStackedWidget*>(parentWidget) != nullptr)
            {
                return cursorWidget;
            }
            if (cursorWidget->isWindow())
            {
                break;
            }
        }
        return fallbackRoot;
    }

    // One discovery cache per page; table geometry refreshes do not rediscover the
    // entire QObject tree. Structural changes rebuild once and share the result.
    class DedicatedSearchPageCache final : public QObject
    {
    public:
        explicit DedicatedSearchPageCache(QWidget* page) : QObject(page), m_page(page) {}

        bool hasDedicatedSearchControl(QTableView* table)
        {
            if (m_page.isNull() || table == nullptr) return false;
            if (m_structureDirty) rebuildCandidates();
            observeGeometryPath(table);

            // Placeholder/accessibility setters have no shared Qt notify signal.
            // Compare only known input metadata to catch these changes exactly.
            for (Candidate& candidate : m_candidates)
            {
                QLineEdit* edit = candidate.edit.data();
                if (edit == nullptr) continue;
                const QString metadata = edit->objectName() + QLatin1Char(' ')
                    + edit->placeholderText() + QLatin1Char(' ') + edit->toolTip()
                    + QLatin1Char(' ') + edit->accessibleName();
                const bool excluded = edit->property(kGenericSearchControlPropertyName).toBool()
                    || edit->property(kGlobalSearchInputPropertyName).toBool();
                if (candidate.metadata != metadata || candidate.excluded != excluded)
                {
                    candidate.metadata = metadata;
                    candidate.excluded = excluded;
                    candidate.isSearch = !excluded && textLooksLikeSearchControl(metadata);
                    m_decisions.clear();
                }
            }
            const auto known = m_decisions.constFind(table);
            if (known != m_decisions.cend()) return known.value();

            QWidget* pageRoot = m_page.data();
            QTableView* tableView = table;
            const QPoint tableTopLeft = table->mapTo(pageRoot, QPoint(0, 0));
            bool dedicatedSearchFound = false;
            for (const Candidate& candidate : m_candidates)
            {
                QLineEdit* lineEdit = candidate.edit.data();
                if (lineEdit == nullptr || !candidate.isSearch) continue;
            const QPoint editTopLeft = lineEdit->mapTo(pageRoot, QPoint(0, 0));
            const bool directlyAboveTable = editTopLeft.y() <= tableTopLeft.y()
                && tableTopLeft.y() - editTopLeft.y() <= 180;
            const bool sharesImmediateContainer = lineEdit->parentWidget() == tableView->parentWidget();
            if (directlyAboveTable || sharesImmediateContainer)
            {
                dedicatedSearchFound = true;
                // 沿现有专用表格搜索识别路径接入，跳过数值/下拉内部编辑器与扫描值表单。
                // 只改已经承担文本过滤的字段，不接管所有 QLineEdit，也不修改过滤连接。
                bool belongsToTextSearch = false; // 编辑器查找/替换保持自己的输入语义和主题。
                for (QWidget* owner = lineEdit->parentWidget(); owner != nullptr; owner = owner->parentWidget())
                {
                    if (owner->objectName() == QStringLiteral("code_editor")
                        || owner->objectName() == QStringLiteral("KswordTextSearchBar"))
                    {
                        belongsToTextSearch = true;
                        break;
                    }
                    if (owner->isWindow())
                    {
                        break;
                    }
                }
                const QString placeholderSource = ks::i18n::LanguageManager::instance().sourceForRenderedText(
                    lineEdit->placeholderText()); // 比较规范源文本，中文和译文都保持相同排除边界。
                if (qobject_cast<QAbstractSpinBox*>(lineEdit->parentWidget()) == nullptr
                    && qobject_cast<QComboBox*>(lineEdit->parentWidget()) == nullptr
                    && !lineEdit->isReadOnly()
                    && !belongsToTextSearch
                    && placeholderSource != QStringLiteral("输入搜索值")
                    && placeholderSource != QStringLiteral("搜索：AA BB ?? 或 ASCII/UTF-16 文本"))
                {
                    if (lineEdit->placeholderText().isEmpty())
                    {
                        lineEdit->setPlaceholderText(ks::i18n::sourceText(QStringLiteral("搜索")));
                    }
                    ks::ui::BindSearchFieldTheme(lineEdit);
                }
            }
        }
            m_decisions.insert(table, dedicatedSearchFound);
            if (!m_tables.contains(table))
            {
                m_tables.insert(table);
                connect(table, &QObject::destroyed, this, [this, table]() {
                    m_decisions.remove(table);
                    m_tables.remove(table);
                    m_refreshCallbacks.remove(table);
                });
            }
            return dedicatedSearchFound;
        }

        void observeTable(QTableView* table, QObject* receiver, std::function<void()> refresh)
        {
            // QPointer protects a replaced/deleted search host until the next refresh.
            m_refreshCallbacks.insert(table, [guard = QPointer<QObject>(receiver), refresh]() {
                if (!guard.isNull()) refresh();
            });
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event == nullptr) return QObject::eventFilter(watched, event);
            if ((event->type() == QEvent::Move || event->type() == QEvent::Resize)
                && !m_geometryNodes.contains(watched))
            {
                return QObject::eventFilter(watched, event);
            }
            if (event->type() == QEvent::DynamicPropertyChange)
            {
                const QByteArray name = static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();
                if (qobject_cast<QLineEdit*>(watched) == nullptr
                    || (name != kGenericSearchControlPropertyName && name != kGlobalSearchInputPropertyName))
                {
                    return QObject::eventFilter(watched, event);
                }
            }
            if (event->type() == QEvent::ToolTipChange && qobject_cast<QLineEdit*>(watched) == nullptr)
            {
                return QObject::eventFilter(watched, event);
            }
            switch (event->type())
            {
            case QEvent::ChildAdded:
            case QEvent::ChildRemoved:
            case QEvent::ParentChange:
                m_structureDirty = true;
                [[fallthrough]];
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::LanguageChange:
            case QEvent::DynamicPropertyChange:
            case QEvent::ToolTipChange:
                m_decisions.clear();
                scheduleRefresh();
                break;
            default:
                break;
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        struct Candidate
        {
            QPointer<QLineEdit> edit;
            QString metadata;
            bool isSearch = false;
            bool excluded = false;
        };

        void observeGeometryPath(QWidget* widget)
        {
            for (QWidget* cursor = widget; cursor != nullptr; cursor = cursor->parentWidget())
            {
                m_geometryNodes.insert(cursor);
                if (cursor == m_page.data()) break;
            }
        }

        void rebuildCandidates()
        {
            m_structureDirty = false;
            m_candidates.clear();
            m_decisions.clear();
            const auto widgets = m_page->findChildren<QWidget*>();
            QSet<QObject*> current;
            current.insert(m_page.data());
            for (QWidget* widget : widgets) current.insert(widget);
            for (auto it = m_observed.begin(); it != m_observed.end();)
            {
                if (!current.contains(*it))
                {
                    (*it)->removeEventFilter(this);
                    it = m_observed.erase(it);
                }
                else ++it;
            }
            m_geometryNodes.clear();
            for (auto it = m_tables.begin(); it != m_tables.end();)
            {
                QTableView* table = *it;
                if (nearestPageRoot(table) != m_page.data())
                {
                    m_decisions.remove(table);
                    m_refreshCallbacks.remove(table);
                    it = m_tables.erase(it);
                }
                else { observeGeometryPath(table); ++it; }
            }
            auto observe = [this](QWidget* widget) {
                if (widget == nullptr || m_observed.contains(widget)) return;
                m_observed.insert(widget);
                widget->installEventFilter(this);
                connect(widget, &QObject::destroyed, this, [this, widget]() {
                    m_observed.remove(widget);
                    m_geometryNodes.remove(widget);
                });
            };
            observe(m_page.data());
            for (QWidget* widget : widgets)
            {
                observe(widget);
                if (auto* edit = qobject_cast<QLineEdit*>(widget))
                {
                    Candidate candidate;
                    candidate.edit = edit;
                    m_candidates.push_back(candidate);
                    observeGeometryPath(edit);
                }
            }
        }

        void scheduleRefresh()
        {
            if (m_refreshPending) return;
            m_refreshPending = true;
            QTimer::singleShot(0, this, [this]() {
                m_refreshPending = false;
                const auto callbacks = m_refreshCallbacks;
                for (auto it = callbacks.cbegin(); it != callbacks.cend(); ++it) it.value()();
            });
        }

        QPointer<QWidget> m_page;
        QVector<Candidate> m_candidates;
        QSet<QObject*> m_observed;
        QSet<QObject*> m_geometryNodes;
        QSet<QTableView*> m_tables;
        QHash<QTableView*, bool> m_decisions;
        QHash<QTableView*, std::function<void()>> m_refreshCallbacks;
        bool m_structureDirty = true;
        bool m_refreshPending = false;
    };

    DedicatedSearchPageCache* dedicatedSearchCache(QTableView* tableView)
    {
        QWidget* page = nearestPageRoot(tableView);
        if (page == nullptr) return nullptr;
        constexpr const char* property = "_ksword_dedicated_search_page_cache";
        auto* cache = dynamic_cast<DedicatedSearchPageCache*>(page->property(property).value<QObject*>());
        if (cache == nullptr)
        {
            cache = new DedicatedSearchPageCache(page);
            page->setProperty(property, QVariant::fromValue<QObject*>(cache));
        }
        return cache;
    }

    bool hasDedicatedSearchControl(QTableView* tableView)
    {
        auto* cache = dedicatedSearchCache(tableView);
        return cache != nullptr && cache->hasDedicatedSearchControl(tableView);
    }

    // tableNeedsSearchAccess：只有模型内容实际超过当前垂直可视页时才需要入口。
    bool tableNeedsSearchAccess(QTableView* tableView)
    {
        return tableView != nullptr
            && tableView->model() != nullptr
            && tableView->model()->rowCount() > 0
            && tableView->verticalScrollBar() != nullptr
            && tableView->verticalScrollBar()->maximum()
                > tableView->verticalScrollBar()->minimum();
    }

    // resolveSearchHost：优先使用现有表格操作条，否则使用水平表头尾部空白区。
    QWidget* resolveSearchHost(QTableView* tableView)
    {
        if (tableView == nullptr)
        {
            return nullptr;
        }
        QWidget* actionBar = tableView->findChild<QWidget*>(
            QString::fromLatin1(kTableActionBarObjectName),
            Qt::FindDirectChildrenOnly);
        return actionBar != nullptr ? actionBar : tableView->horizontalHeader();
    }

    // trailingHeaderSpace：计算水平表头最右侧没有被可见列占用的像素宽度。
    int trailingHeaderSpace(QTableView* tableView)
    {
        QHeaderView* headerView = tableView != nullptr ? tableView->horizontalHeader() : nullptr;
        if (headerView == nullptr)
        {
            return 0;
        }

        int occupiedRight = 0;
        for (int logicalIndex = 0; logicalIndex < headerView->count(); ++logicalIndex)
        {
            if (headerView->isSectionHidden(logicalIndex))
            {
                continue;
            }
            const int sectionLeft = headerView->sectionViewportPosition(logicalIndex);
            const int sectionRight = sectionLeft + headerView->sectionSize(logicalIndex);
            occupiedRight = std::max(occupiedRight, sectionRight);
        }
        return std::max(0, headerView->width() - occupiedRight);
    }

    // TableSearchAccessWidget：管理单个表格的搜索按钮与宿主空间预留。
    class TableSearchAccessWidget final : public QFrame
    {
    public:
        explicit TableSearchAccessWidget(QTableView* tableView)
            : QFrame(resolveSearchHost(tableView))
            , m_tableView(tableView)
            , m_hostWidget(resolveSearchHost(tableView))
        {
            setObjectName(QString::fromLatin1(kSearchSupportObjectName));
            setFrameShape(QFrame::NoFrame);
            setAttribute(Qt::WA_StyledBackground, true);

            auto* rootLayout = new QHBoxLayout(this);
            rootLayout->setContentsMargins(0, 0, 0, 0);
            rootLayout->setSpacing(0);

            m_searchButton = new QToolButton(this);
            m_searchButton->setProperty(kGenericSearchControlPropertyName, true);
            m_searchButton->setAutoRaise(true);
            m_searchButton->setIcon(QIcon(QStringLiteral(":/Icon/file_find.svg")));
            m_searchButton->setIconSize(QSize(16, 16));
            m_searchButton->setFixedSize(kCollapsedSearchWidth, kSearchControlHeight);
            rootLayout->addWidget(m_searchButton);

            connect(m_searchButton, &QToolButton::clicked, this, [this]() {
                if (!m_tableView.isNull())
                {
                    ks::ui::ActivateGlobalUiSearchForTable(
                        m_tableView.data(),
                        QString(),
                        true);
                }
            });
            if (m_hostWidget != nullptr && m_hostWidget->layout() != nullptr)
            {
                m_originalHostMargins = m_hostWidget->layout()->contentsMargins();
            }
            installObservers();
            refreshPresentation();
        }

        ~TableSearchAccessWidget() override
        {
            clearResultFilter();
            restoreHostMargins();
        }

        bool isGenericSearchEligible() const
        {
            return !m_tableView.isNull()
                && m_tableView->model() != nullptr
                && !hasDedicatedSearchControl(m_tableView.data());
        }

        bool applyResultFilter(const QString& queryText)
        {
            const QString normalizedQuery = queryText.trimmed();
            if (!isGenericSearchEligible() || normalizedQuery.isEmpty())
            {
                clearResultFilter();
                return false;
            }

            QAbstractItemModel* currentModel = m_tableView->model();
            observeFilterModel(currentModel);
            if (!m_resultFilterActive)
            {
                m_keepSearchAccessVisible = tableNeedsSearchAccess(
                    m_tableView.data());
                captureBaselineHiddenRows();
            }
            m_resultFilterActive = true;
            m_resultFilterQuery = normalizedQuery;
            refreshSearchColumns();
            scheduleFilterReapply(true);

            scheduleRefresh();
            return true;
        }

        void clearResultFilter()
        {
            // 清空后重新启用过滤也属于新轮次，旧 queued 回投不能重建新基线。
            ++m_filterGeneration;
            m_filterRefreshPending = false;
            const bool stateChanged = m_resultFilterActive;
            if (m_resultFilterActive)
            {
                restoreBaselineHiddenRows();
            }
            m_resultFilterActive = false;
            m_modelMutationPrepared = false;
            m_keepSearchAccessVisible = false;
            m_resultFilterQuery.clear();
            m_filterRows.clear();
            m_rowLookup.clear();
            m_dirtyRows.clear();
            m_allRowsDirty = false;
            m_filterCursor = 0;

            if (stateChanged)
            {
                scheduleRefresh();
            }
        }


        // 模型切换前恢复原先隐藏状态；过滤词留给新模型换绑后继续使用。
        void prepareModelChange()
        {
            if (m_resultFilterActive)
            {
                restoreBaselineHiddenRows();
            }
        }

        // 旧模型继续更新时不能影响新表；observeFilterModel 会断开全部旧信号。
        void refreshModelBinding()
        {
            QAbstractItemModel* const model = m_tableView.isNull() ? nullptr : m_tableView->model();
            if (m_filterModel == model && m_filterRoot == m_tableView->rootIndex())
            {
                return;
            }
            // 断开 signal 不能取消已排队 singleShot，因此另外隔离模型轮次。
            ++m_filterGeneration;
            m_filterRefreshPending = false;
            const QString query = m_resultFilterQuery;
            const bool wasActive = m_resultFilterActive;
            observeFilterModel(model);
            if (wasActive && model != nullptr && !query.isEmpty())
            {
                applyResultFilter(query);
            }
        }

        void refreshPresentation()
        {
            if (m_tableView.isNull() || m_hostWidget.isNull())
            {
                hide();
                return;
            }

            refreshModelBinding();
            QTableView* tableView = m_tableView.data();
            refreshSearchColumns();
            if (auto* cache = dedicatedSearchCache(tableView))
            {
                cache->observeTable(tableView, this, [this]() { scheduleRefresh(); });
            }
            const QString tableName = ks::ui::ResolveTableSearchDisplayName(tableView);
            m_searchButton->setToolTip(
                ks::i18n::sourceText(QStringLiteral("搜索当前表格：%1")).arg(tableName));

            if (hasDedicatedSearchControl(tableView))
            {
                clearResultFilter();
                applyPresentation(0);
                return;
            }
            if (!tableNeedsSearchAccess(tableView)
                && !(m_resultFilterActive && m_keepSearchAccessVisible))
            {
                applyPresentation(0);
                return;
            }

            QWidget* hostWidget = m_hostWidget.data();
            const bool hostedByActionBar = hostWidget != tableView->horizontalHeader();
            int availableWidth = trailingHeaderSpace(tableView);
            if (hostedByActionBar)
            {
                const int minimumContentWidth = hostWidget->layout() != nullptr
                    ? hostWidget->layout()->minimumSize().width()
                    : 0;
                // minimumSize 已包含当前搜索预留；加回该宽度后再决定是否升/降级，
                // 避免每次刷新先撤销再添加边距形成 LayoutRequest 循环。
                availableWidth = std::max(
                    0,
                    hostWidget->width() - minimumContentWidth + m_reservedHostWidth);
            }

            if (availableWidth >= kCollapsedSearchWidth + kSearchOuterMargin)
            {
                applyPresentation(kCollapsedSearchWidth);
            }
            else
            {
                applyPresentation(0);
            }
        }

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (eventObject != nullptr && watchedObject == m_tableView.data()
                && eventObject->type() == QEvent::Show)
            {
                scheduleFilterReapply(false);
            }
            if (eventObject != nullptr
                && (eventObject->type() == QEvent::Resize
                    || eventObject->type() == QEvent::LayoutRequest
                    || eventObject->type() == QEvent::Show
                    || eventObject->type() == QEvent::StyleChange
                    || eventObject->type() == QEvent::LanguageChange))
            {
                scheduleRefresh();
            }
            return QFrame::eventFilter(watchedObject, eventObject);
        }

    private:
        void observeFilterModel(QAbstractItemModel* model)
        {
            const QModelIndex root = m_tableView.isNull() ? QModelIndex() : m_tableView->rootIndex();
            if (m_filterModel == model && m_filterRoot == root) return;
            m_resultFilterActive = false;
            m_modelMutationPrepared = false;
            m_filterRows.clear();
            m_rowLookup.clear();
            m_dirtyRows.clear();
            m_allRowsDirty = false;
            m_filterCursor = 0;
            if (!m_filterModel.isNull()) disconnect(m_filterModel.data(), nullptr, this, nullptr);
            m_filterModel = model;
            m_filterRoot = root;
            if (model == nullptr) return;

            connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
                    if (!m_resultFilterActive || first.parent() != m_filterRoot
                        || (!roles.isEmpty() && !roles.contains(Qt::DisplayRole))) return;
                    bool relevant = false;
                    for (int column = first.column(); column <= last.column(); ++column)
                    {
                        if (m_searchColumns.contains(column)) { relevant = true; break; }
                    }
                    if (relevant) markRowsDirty(first.row(), last.row());
                });
            connect(model, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex& parent, int first, int last) {
                    if (!m_resultFilterActive || parent != m_filterRoot) return;
                    for (int row = first; row <= last; ++row)
                    {
                        FilterRowState state;
                        state.key = ++m_nextRowKey;
                        state.identity = m_filterModel->index(row, 0, m_filterRoot);
                        m_filterRows.insert(row, state);
                    }
                    rebuildRowLookup(first);
                    if (m_allRowsDirty && first < m_filterCursor) m_filterCursor += last - first + 1;
                    markRowsDirty(first, last);
                });
            connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex& parent, int first, int last) {
                    if (!m_resultFilterActive || parent != m_filterRoot) return;
                    if (m_allRowsDirty && first < m_filterCursor)
                    {
                        m_filterCursor -= std::min(last + 1, m_filterCursor) - first;
                    }
                    for (int row = first; row <= last; ++row)
                    {
                        m_rowLookup.remove(m_filterRows.at(row).key);
                        m_dirtyRows.remove(m_filterRows.at(row).key);
                    }
                    m_filterRows.remove(first, last - first + 1);
                    rebuildRowLookup(first);
                });
            connect(model, &QAbstractItemModel::rowsRemoved, this,
                [this]() { scheduleFilterReapply(false); });
            const auto reordered = [this]() {
                if (!m_resultFilterActive) return;
                // Persistent indexes follow model sorting/moves, so baseline visibility
                // remains attached to the business row rather than its previous number.
                if (!m_filterRows.isEmpty() && m_filterRows.first().identity.isValid())
                {
                    std::sort(m_filterRows.begin(), m_filterRows.end(),
                        [](const FilterRowState& a, const FilterRowState& b) {
                            return a.identity.row() < b.identity.row();
                        });
                }
                rebuildRowLookup();
                for (int row = 0; row < m_filterRows.size(); ++row) applyRowVisibility(row);
                if (m_allRowsDirty) { m_filterCursor = 0; }
                scheduleFilterReapply(false);
            };
            connect(model, &QAbstractItemModel::layoutChanged, this, reordered);
            connect(model, &QAbstractItemModel::rowsMoved, this,
                [this, reordered](const QModelIndex& source, int first, int last,
                    const QModelIndex& destination, int destinationRow) {
                    if (!m_resultFilterActive) return;
                    if (source == m_filterRoot && destination == m_filterRoot) { reordered(); return; }
                    if (source == m_filterRoot)
                    {
                        for (int row = first; row <= last; ++row)
                        {
                            m_rowLookup.remove(m_filterRows.at(row).key);
                            m_dirtyRows.remove(m_filterRows.at(row).key);
                        }
                        m_filterRows.remove(first, last - first + 1);
                        rebuildRowLookup(first);
                        if (m_allRowsDirty) m_filterCursor = 0;
                    }
                    else if (destination == m_filterRoot)
                    {
                        for (int row = destinationRow; row < destinationRow + last - first + 1; ++row)
                        {
                            FilterRowState state;
                            state.key = ++m_nextRowKey;
                            state.identity = m_filterModel->index(row, 0, m_filterRoot);
                            m_filterRows.insert(row, state);
                        }
                        rebuildRowLookup(destinationRow);
                        if (m_allRowsDirty && destinationRow < m_filterCursor)
                            m_filterCursor += last - first + 1;
                        markRowsDirty(destinationRow, destinationRow + last - first);
                    }
                    scheduleFilterReapply(false);
                });
            const auto columnsChanged = [this]() {
                if (!m_resultFilterActive) return;
                for (int row = 0; row < m_filterRows.size(); ++row)
                {
                    m_filterRows[row].identity = m_filterModel->index(row, 0, m_filterRoot);
                }
                refreshSearchColumns();
                scheduleFilterReapply(true);
            };
            connect(model, &QAbstractItemModel::columnsInserted, this, columnsChanged);
            connect(model, &QAbstractItemModel::columnsRemoved, this, columnsChanged);
            connect(model, &QAbstractItemModel::columnsMoved, this, columnsChanged);
            connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
                ++m_filterGeneration;
                m_filterRefreshPending = false;
                m_filterRows.clear();
                m_rowLookup.clear();
                m_dirtyRows.clear();
                m_allRowsDirty = false;
                m_filterCursor = 0;
            });
            connect(model, &QAbstractItemModel::modelReset, this, [this]() {
                if (!m_resultFilterActive) return;
                m_filterRoot = m_tableView->rootIndex();
                captureBaselineHiddenRows(true);
                refreshSearchColumns();
                scheduleFilterReapply(true);
            });
        }

        void refreshSearchColumns()
        {
            if (m_tableView.isNull() || m_tableView->model() == nullptr) return;
            QVector<int> columns;
            for (int column = 0; column < m_tableView->model()->columnCount(m_tableView->rootIndex()); ++column)
            {
                if (!m_tableView->isColumnHidden(column)
                    || ks::ui::isColumnHiddenByFreeze(m_tableView.data(), column)) columns.push_back(column);
            }
            if (columns != m_searchColumns)
            {
                m_searchColumns = columns;
                if (m_resultFilterActive) scheduleFilterReapply(true);
            }
        }

        void rebuildRowLookup(const int first = 0)
        {
            for (int row = first; row < m_filterRows.size(); ++row)
                m_rowLookup.insert(m_filterRows.at(row).key, row);
        }

        void markRowsDirty(int first, int last)
        {
            if (!m_resultFilterActive || m_filterModel.isNull()) return;
            first = std::max(0, first);
            last = std::min(last, static_cast<int>(m_filterRows.size()) - 1);
            if (first == 0 && last + 1 == m_filterRows.size())
            {
                scheduleFilterReapply(true);
                return;
            }
            for (int row = first; row <= last; ++row)
            {
                m_dirtyRows.insert(m_filterRows.at(row).key);
            }
            scheduleFilterReapply(false);
        }

        void scheduleFilterReapply(const bool allRows)
        {
            if (!m_resultFilterActive) return;
            if (allRows)
            {
                m_allRowsDirty = true;
                m_filterCursor = 0;
                m_dirtyRows.clear();
            }
            if (m_filterRefreshPending || m_tableView.isNull() || !m_tableView->isVisible()
                || (!m_allRowsDirty && m_dirtyRows.isEmpty())) return;
            m_filterRefreshPending = true;
            const quint64 generation = m_filterGeneration;
            QTimer::singleShot(0, this, [this, generation]() {
                if (generation != m_filterGeneration) return;
                m_filterRefreshPending = false;
                applyResultFilterRows();
            });
        }

        void captureBaselineHiddenRows(const bool deferBaseline = false)
        {
            m_filterRows.clear();
            m_rowLookup.clear();
            if (m_tableView.isNull() || m_tableView->model() == nullptr) return;
            const int rowCount = m_tableView->model()->rowCount(m_tableView->rootIndex());
            m_filterRows.reserve(rowCount);
            for (int row = 0; row < rowCount; ++row)
            {
                FilterRowState state;
                state.key = ++m_nextRowKey;
                state.baselinePending = deferBaseline;
                state.identity = m_tableView->model()->index(row, 0, m_tableView->rootIndex());
                if (!deferBaseline)
                    state.baselineHidden = ks::ui::IsTableRowHiddenByLegacyFilter(m_tableView.data(), row);
                m_filterRows.push_back(state);
                m_rowLookup.insert(state.key, row);
            }
        }

        void restoreBaselineHiddenRows()
        {
            if (m_tableView.isNull() || m_filterModel != m_tableView->model()
                || m_filterRoot != m_tableView->rootIndex()) return;
            ks::ui::ClearTableSearchRowFilter(m_tableView.data());
        }

        bool rowMatchesQuery(const int row) const
        {
            if (m_filterModel.isNull()) return false;
            for (int column : m_searchColumns)
            {
                if (m_filterModel->index(row, column, m_filterRoot).data(Qt::DisplayRole)
                    .toString().contains(m_resultFilterQuery, Qt::CaseInsensitive)) return true;
            }
            return false;
        }

        void applyRowVisibility(const int row)
        {
            const auto& state = m_filterRows.at(row);
            if (state.baselinePending) return;
            ks::ui::SetTableSearchRowExcluded(m_tableView.data(), row,
                state.baselineHidden || !state.matches);
        }

        void applyResultFilterRows()
        {
            if (!m_resultFilterActive || m_tableView.isNull() || m_filterModel.isNull()
                || m_filterModel != m_tableView->model() || !m_tableView->isVisible()) return;
            QElapsedTimer slice;
            slice.start();
            int processed = 0;
            while (m_allRowsDirty || !m_dirtyRows.isEmpty())
            {
                int row = -1;
                if (m_allRowsDirty)
                {
                    if (m_filterCursor >= m_filterRows.size())
                    {
                        m_allRowsDirty = false;
                        m_filterCursor = 0;
                        continue;
                    }
                    row = m_filterCursor++;
                    m_dirtyRows.remove(m_filterRows.at(row).key);
                }
                else
                {
                    const quint64 key = *m_dirtyRows.cbegin();
                    m_dirtyRows.remove(key);
                    row = m_rowLookup.value(key, -1);
                }
                if (row < 0 || row >= m_filterRows.size()) continue;
                auto& state = m_filterRows[row];
                if (state.baselinePending)
                {
                    // Business code commonly hides rows immediately after endInsertRows/
                    // endResetModel. Capture only after those synchronous handlers finish.
                    state.baselineHidden = ks::ui::IsTableRowHiddenByLegacyFilter(m_tableView.data(), row);
                    state.baselinePending = false;
                }
                if (!state.baselineHidden) state.matches = rowMatchesQuery(row);
                applyRowVisibility(row);
                if ((++processed % 32) == 0 && slice.elapsed() >= 4)
                {
                    scheduleFilterReapply(false);
                    return;
                }
            }
            scheduleRefresh();
        }

        void installObservers()
        {
            if (!m_tableView.isNull())
            {
                m_tableView->installEventFilter(this);
                if (m_tableView->verticalScrollBar() != nullptr)
                {
                    connect(
                        m_tableView->verticalScrollBar(),
                        &QScrollBar::rangeChanged,
                        this,
                        [this](int, int) { scheduleRefresh(); });
                }
                if (m_tableView->horizontalHeader() != nullptr)
                {
                    m_tableView->horizontalHeader()->installEventFilter(this);
                    connect(
                        m_tableView->horizontalHeader(),
                        &QHeaderView::geometriesChanged,
                        this,
                        [this]() { refreshSearchColumns(); scheduleRefresh(); });
                }
            }
            if (!m_hostWidget.isNull())
            {
                m_hostWidget->installEventFilter(this);
            }
        }

        void scheduleRefresh()
        {
            if (m_refreshPending)
            {
                return;
            }
            m_refreshPending = true;
            QTimer::singleShot(0, this, [this]() {
                m_refreshPending = false;
                refreshPresentation();
            });
        }

        void restoreHostMargins()
        {
            if (!m_hostWidget.isNull()
                && m_hostWidget->layout() != nullptr
                && !m_tableView.isNull()
                && m_hostWidget.data() != m_tableView->horizontalHeader()
                && m_reservedHostWidth > 0)
            {
                m_hostWidget->layout()->setContentsMargins(m_originalHostMargins);
                m_reservedHostWidth = 0;
            }
        }

        void applyPresentation(const int requestedWidth)
        {
            if (requestedWidth <= 0 || m_hostWidget.isNull())
            {
                restoreHostMargins();
                hide();
                return;
            }

            QWidget* hostWidget = m_hostWidget.data();
            m_searchButton->setVisible(true);

            if (hostWidget->layout() != nullptr
                && hostWidget != m_tableView->horizontalHeader())
            {
                const int requestedReservation = requestedWidth + kSearchOuterMargin;
                if (m_reservedHostWidth != requestedReservation)
                {
                    QMargins reservedMargins = m_originalHostMargins;
                    reservedMargins.setRight(
                        reservedMargins.right() + requestedReservation);
                    hostWidget->layout()->setContentsMargins(reservedMargins);
                    m_reservedHostWidth = requestedReservation;
                }
            }

            const int controlTop = std::max(0, (hostWidget->height() - kSearchControlHeight) / 2);
            const int controlLeft = std::max(
                0,
                hostWidget->width() - requestedWidth - kSearchOuterMargin);
            setGeometry(
                controlLeft,
                controlTop,
                requestedWidth,
                kSearchControlHeight);
            const QString presentationStyle = QStringLiteral(
                "QFrame#KSWORD_TABLE_SEARCH_SUPPORT{background:transparent;}"
                "QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:3px;padding:0 6px;}"
                "QLineEdit:focus{border-color:%4;}"

                "QToolButton{border-radius:3px;}" )
                .arg(KswordTheme::SurfaceHex(), KswordTheme::TextPrimaryHex(),
                    KswordTheme::BorderStrongHex(), KswordTheme::PrimaryBlueHex)
                + ks::ui::BuildFlatButtonStyle(ks::ui::FlatButtonTone::Neutral);
            if (styleSheet() != presentationStyle) setStyleSheet(presentationStyle);
            show();
            raise();
        }

        QPointer<QTableView> m_tableView;    // m_tableView：搜索入口对应的原始表格。
        QPointer<QWidget> m_hostWidget;      // m_hostWidget：操作条或水平表头宿主。
        QToolButton* m_searchButton = nullptr; // m_searchButton：激活标题栏表格搜索的图标按钮。
        QMargins m_originalHostMargins;      // m_originalHostMargins：操作条原始布局边距。
        QPointer<QAbstractItemModel> m_filterModel; // m_filterModel：过滤启用时观察的当前模型。
        struct FilterRowState
        {
            quint64 key = 0;
            QPersistentModelIndex identity;
            bool baselineHidden = false;
            bool baselinePending = true;
            bool matches = true;
        };
        QVector<FilterRowState> m_filterRows; // Business baseline and query acceptance have separate owners.
        QPersistentModelIndex m_filterRoot;
        QSet<quint64> m_dirtyRows;
        QHash<quint64, int> m_rowLookup;
        quint64 m_nextRowKey = 0;
        QVector<int> m_searchColumns;
        bool m_allRowsDirty = false;
        int m_filterCursor = 0;
        QString m_resultFilterQuery;         // m_resultFilterQuery：当前结果专显关键词。
        int m_reservedHostWidth = 0;         // m_reservedHostWidth：当前为搜索入口预留的右侧宽度。
        bool m_resultFilterActive = false;   // m_resultFilterActive：是否已附加通用行过滤。
        bool m_keepSearchAccessVisible = false; // m_keepSearchAccessVisible：过滤前入口是否满足超页显示条件。
        bool m_modelMutationPrepared = false; // m_modelMutationPrepared：结构变更前是否已恢复基线。
        bool m_filterRefreshPending = false; // m_filterRefreshPending：合并模型变化触发的过滤刷新。
        quint64 m_filterGeneration = 0;      // 隔离换模型/清空过滤前排队的旧回投。
        bool m_refreshPending = false;       // m_refreshPending：合并同一事件循环内的刷新请求。
    };

    // searchSupportForTable：读取表格已安装的搜索入口对象。
    TableSearchAccessWidget* searchSupportForTable(QTableView* tableView)
    {
        if (tableView == nullptr)
        {
            return nullptr;
        }
        QObject* supportObject = tableView->property(kSearchSupportPropertyName).value<QObject*>();
        return dynamic_cast<TableSearchAccessWidget*>(supportObject);
    }

    // normalizedMatchRank：计算单元格命中的排序优先级。
    int normalizedMatchRank(const QString& cellText, const QString& queryText)
    {
        if (cellText.compare(queryText, Qt::CaseInsensitive) == 0)
        {
            return 0;
        }
        return cellText.startsWith(queryText, Qt::CaseInsensitive) ? 1 : 2;
    }
}

namespace ks::ui
{
    void InstallTableSearchSupport(QTableView* tableView)
    {
        if (tableView == nullptr || searchSupportForTable(tableView) != nullptr)
        {
            return;
        }
        auto* searchSupport = new TableSearchAccessWidget(tableView);
        tableView->setProperty(
            kSearchSupportPropertyName,
            QVariant::fromValue<QObject*>(searchSupport));
    }

    void RefreshTableSearchSupport(QTableView* tableView)
    {
        if (TableSearchAccessWidget* searchSupport = searchSupportForTable(tableView))
        {
            searchSupport->refreshPresentation();
        }
    }

    // 两个入口只委托存量搜索控制器，不另外维护过滤状态或隐藏快照。
    void PrepareTableSearchModelChange(QTableView* tableView)
    {
        if (TableSearchAccessWidget* searchSupport = searchSupportForTable(tableView))
        {
            searchSupport->prepareModelChange();
        }
    }

    void RefreshTableSearchModelBinding(QTableView* tableView)
    {
        if (TableSearchAccessWidget* searchSupport = searchSupportForTable(tableView))
        {
            searchSupport->refreshModelBinding();
        }
    }

    bool IsGenericTableSearchEligible(QTableView* tableView)
    {
        if (TableSearchAccessWidget* searchSupport = searchSupportForTable(tableView))
        {
            return searchSupport->isGenericSearchEligible();
        }
        return false;
    }

    bool ApplyTableSearchResultFilter(
        QTableView* tableView,
        const QString& queryText)
    {
        if (TableSearchAccessWidget* searchSupport = searchSupportForTable(tableView))
        {
            return searchSupport->applyResultFilter(queryText);
        }
        return false;
    }

    void ClearTableSearchResultFilter(QTableView* tableView)
    {
        if (TableSearchAccessWidget* searchSupport = searchSupportForTable(tableView))
        {
            searchSupport->clearResultFilter();
        }
    }


    QString ResolveTableSearchDisplayName(const QTableView* tableView)
    {
        if (tableView == nullptr)
        {
            return ks::i18n::sourceText(QStringLiteral("表格"));
        }

        const QString explicitName = tableView->property(kExplicitTableNamePropertyName)
            .toString()
            .trimmed();
        if (!explicitName.isEmpty())
        {
            return explicitName;
        }
        if (!tableView->accessibleName().trimmed().isEmpty())
        {
            return tableView->accessibleName().trimmed();
        }

        for (QWidget* cursorWidget = tableView->parentWidget();
             cursorWidget != nullptr;
             cursorWidget = cursorWidget->parentWidget())
        {
            if (QGroupBox* groupBox = qobject_cast<QGroupBox*>(cursorWidget))
            {
                if (!groupBox->title().trimmed().isEmpty())
                {
                    return groupBox->title().trimmed();
                }
            }
            QWidget* parentWidget = cursorWidget->parentWidget();
            if (QStackedWidget* stackedWidget = qobject_cast<QStackedWidget*>(parentWidget))
            {
                if (QTabWidget* tabWidget = qobject_cast<QTabWidget*>(stackedWidget->parentWidget()))
                {
                    const int tabIndex = tabWidget->indexOf(cursorWidget);
                    if (tabIndex >= 0 && !tabWidget->tabText(tabIndex).trimmed().isEmpty())
                    {
                        return tabWidget->tabText(tabIndex).trimmed();
                    }
                }
            }
        }

        QString objectName = tableView->objectName().trimmed();
        if (objectName.startsWith(QStringLiteral("m_"), Qt::CaseInsensitive))
        {
            objectName.remove(0, 2);
        }
        const QStringList knownSuffixList = {
            QStringLiteral("tableWidget"),
            QStringLiteral("tableView"),
            QStringLiteral("table")
        };
        for (const QString& knownSuffix : knownSuffixList)
        {
            if (objectName.endsWith(knownSuffix, Qt::CaseInsensitive))
            {
                objectName.chop(knownSuffix.size());
                break;
            }
        }
        if (!objectName.isEmpty())
        {
            return objectName;
        }
        return ks::i18n::sourceText(QStringLiteral("表格"));
    }

    QVector<TableCellSearchMatch> CollectTableCellSearchMatches(
        QTableView* tableView,
        const QString& queryText,
        const int maxHitCount)
    {
        QVector<TableCellSearchMatch> resultList;
        if (tableView == nullptr
            || tableView->model() == nullptr
            || queryText.trimmed().isEmpty()
            || maxHitCount <= 0)
        {
            return resultList;
        }

        QAbstractItemModel* itemModel = tableView->model();
        const QString normalizedQuery = queryText.trimmed();
        const QString tableName = ResolveTableSearchDisplayName(tableView);
        const int columnCount = itemModel->columnCount();
        for (int columnIndex = 0;
             columnIndex < columnCount && resultList.size() < maxHitCount;
             ++columnIndex)
        {
            if ((tableView->isColumnHidden(columnIndex) && !isColumnHiddenByFreeze(tableView, columnIndex))
                || itemModel->rowCount() <= 0)
            {
                continue;
            }

            // Preserve Qt's default column/depth-first matching order while
            // applying business exclusions before counting a hit. A rejected
            // prefix must not consume the hit cap or allocate all model matches.
            struct SearchFrame { QPersistentModelIndex parent; int row; int rows; };
            QVector<SearchFrame> frames;
            frames.push_back({{}, 0, itemModel->rowCount()});
            QModelIndexList matchedIndexList;
            const int remainingHits = maxHitCount - resultList.size();
            while (!frames.isEmpty() && matchedIndexList.size() < remainingHits)
            {
                auto& frame = frames.last();
                if (frame.row >= frame.rows) { frames.removeLast(); continue; }
                const QModelIndex index = itemModel->index(frame.row++, columnIndex, frame.parent);
                if (!index.isValid() || IsTableCellExcludedByBusinessFilter(tableView, index)) continue;
                if (index.data(Qt::DisplayRole).toString().contains(normalizedQuery, Qt::CaseInsensitive))
                    matchedIndexList.push_back(index);
                const QModelIndex child = columnIndex == 0 ? index : index.siblingAtColumn(0);
                if (itemModel->hasChildren(child))
                    frames.push_back({QPersistentModelIndex(child), 0, itemModel->rowCount(child)});
            }
            const QString columnName = itemModel
                ->headerData(columnIndex, Qt::Horizontal, Qt::DisplayRole)
                .toString()
                .trimmed();
            for (const QModelIndex& matchedIndex : matchedIndexList)
            {
                if (IsTableCellExcludedByBusinessFilter(tableView, matchedIndex)) continue;
                const QString matchedText = matchedIndex.data(Qt::DisplayRole).toString().trimmed();
                if (matchedText.isEmpty())
                {
                    continue;
                }

                TableCellSearchMatch searchMatch;
                searchMatch.tableView = tableView;
                searchMatch.modelIndex = QPersistentModelIndex(matchedIndex);
                searchMatch.matchedText = matchedText;
                searchMatch.locationText = columnName.isEmpty()
                    ? ks::i18n::sourceText(QStringLiteral("%1，第 %2 行"))
                        .arg(tableName)
                        .arg(matchedIndex.row() + 1)
                    : ks::i18n::sourceText(QStringLiteral("%1，第 %2 行，%3 列"))
                        .arg(tableName)
                        .arg(matchedIndex.row() + 1)
                        .arg(columnName);
                searchMatch.matchRank = normalizedMatchRank(matchedText, normalizedQuery);
                resultList.push_back(searchMatch);
                if (resultList.size() >= maxHitCount)
                {
                    break;
                }
            }
        }
        return resultList;
    }

    void RevealTableCellSearchMatch(const TableCellSearchMatch& searchMatch)
    {
        QTableView* tableView = searchMatch.tableView.data();
        if (tableView == nullptr || !searchMatch.modelIndex.isValid())
        {
            return;
        }

        const QModelIndex modelIndex = searchMatch.modelIndex;
        if (modelIndex.model() != tableView->model()
            || IsTableCellExcludedByBusinessFilter(tableView, modelIndex)) return;
        tableView->scrollTo(modelIndex, QAbstractItemView::PositionAtCenter);
        tableView->setCurrentIndex(modelIndex);
        if (tableView->selectionModel() != nullptr)
        {
            tableView->selectionModel()->select(
                modelIndex,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        tableView->setFocus(Qt::OtherFocusReason);
    }
}
