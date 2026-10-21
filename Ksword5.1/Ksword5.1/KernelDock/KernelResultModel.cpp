#include "KernelResultModel.h"
#include "../UI/AsyncOperation.h"
#include "../UI/TableCompleteExport.h"
#include "../UI/TableRowFilterSupport.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TableHeaderSortingSupport.h"
#include "../theme.h"
#include "../UI/UiCommitCoordinator.h"
#include <QItemSelectionModel>
#include <QScrollBar>
#include <QLineEdit>
#include <algorithm>

ksword::kernel_dock_internal::ResultColors ksword::kernel_dock_internal::ResultColors::capture()
{
    return {KswordTheme::SuccessColor(), KswordTheme::WarningColor(), KswordTheme::ErrorColor(),
        KswordTheme::TextSecondaryColor(), KswordTheme::SurfaceMutedColor(), KswordTheme::TextPrimaryColor(), KswordTheme::WarningAccentColor()};
}
ksword::kernel_dock_internal::ResultColors ksword::kernel_dock_internal::ResultColors::current()
{ return resultColors ? *resultColors : capture(); }
namespace
{
    struct ResultColorScope
    {
        const ksword::kernel_dock_internal::ResultColors* previous;
        explicit ResultColorScope(const ksword::kernel_dock_internal::ResultColors& colors)
            : previous(ksword::kernel_dock_internal::resultColors) { ksword::kernel_dock_internal::resultColors = &colors; }
        ~ResultColorScope() { ksword::kernel_dock_internal::resultColors = previous; }
    };
}

class KernelResultTable::Model final : public QAbstractTableModel
{
public:
    using QAbstractTableModel::QAbstractTableModel;
    std::shared_ptr<const KernelResultProjection> values;
    std::vector<std::unique_ptr<QTableWidgetItem>> headers;
    std::function<void(int, Qt::SortOrder)> sortRequest;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() || !values ? 0 : static_cast<int>(values->order.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(headers.size()); }
    QTableWidgetItem* item(int row, int column) const
    {
        if (!values || row < 0 || row >= values->order.size() || column < 0 || column >= values->source->columns) return nullptr;
        return values->source->rows[values->order[row]][column].get();
    }
    QVariant data(const QModelIndex& index, int role) const override
    { const auto* cell = item(index.row(), index.column()); return index.isValid() && cell ? cell->data(role) : QVariant{}; }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    { const auto* cell = item(index.row(), index.column()); return cell ? cell->flags() : Qt::NoItemFlags; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    { return orientation == Qt::Horizontal && section >= 0 && section < static_cast<int>(headers.size()) && headers[section] ? (role == Qt::DisplayRole ? QVariant(ks::i18n::displayText(headers[section]->text())) : headers[section]->data(role)) : QVariant{}; }
    void resizeColumns(int count) { beginResetModel(); headers.resize(count); endResetModel(); }
    void sort(int column, Qt::SortOrder order) override { if (sortRequest) sortRequest(column, order); }
    void replace(std::shared_ptr<const KernelResultProjection> next, bool newSource)
    {
        const QPointer<Model> alive(this);
        if (newSource || !values || values->order.size() != next->order.size())
        {
            beginResetModel();
            if (!alive) return;
            values = std::move(next);
            endResetModel();
            return;
        }
        const bool fieldsChanged = values->source != next->source;
        emit layoutAboutToBeChanged();
        if (!alive) return;
        const auto persistent = persistentIndexList();
        QModelIndexList mapped;
        mapped.reserve(persistent.size());
        for (const auto& index : persistent)
            mapped.push_back(this->index(next->inverse[values->order[index.row()]], index.column()));
        values = std::move(next);
        changePersistentIndexList(persistent, mapped);
        emit layoutChanged();
        if (alive && fieldsChanged && rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    }
};
struct KernelResultTable::State
{
    ks::ui::AsyncOperation* operation = nullptr;
    Factory factory;
    std::shared_ptr<const KernelResultValues> source;
    std::function<void()> ready;
    std::size_t count = 0;
    QString query;
    QPointer<QLineEdit> filterEdit;
    int searchRole = Qt::UserRole + 171, sortColumn = -1;
    Qt::SortOrder sortOrder = Qt::AscendingOrder;
    quint64 revision = 0, content = 0, acceptedContent = 0;
    bool dirty = true, forceDisabled = false, locked = false;
};
KernelResultTable::KernelResultTable(QWidget* parent) : TableActionTableView(parent), m_values(new Model(this)), m_state(std::make_unique<State>())
{
    setModel(m_values);
    m_state->operation = new ks::ui::AsyncOperation(this);
    m_values->sortRequest = [this](int column, Qt::SortOrder order) {
        m_state->sortColumn = column; m_state->sortOrder = order; request();
    };
    ks::ui::SetTableRowFilterBusinessOwnership(this, ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
    ks::ui::InstallTableHeaderClickSorting(this);
    ks::ui::RegisterCompleteExportTableProducer(this, [this] {
        ks::ui::CompleteExportPublishedTable published;
        published.values = m_values->values;
        published.sourceIdentity = QStringLiteral("kernel-result/%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
        published.sourceVersion = m_state->acceptedContent;
        published.rows = rowCount(); published.columns = columnCount();
        published.cell = [](const void* value, int row, int column) {
            const auto& projection = *static_cast<const KernelResultProjection*>(value);
            const auto* cell = projection.source->rows[projection.order[row]][column].get();
            return cell ? cell->text() : QString();
        };
        return published;
    });
}
KernelResultTable::~KernelResultTable() { m_state->operation->close(); }
void KernelResultTable::setColumnCount(int count) { m_values->resizeColumns(count); }
void KernelResultTable::setHorizontalHeaderLabels(const QStringList& labels)
{ for (int i = 0; i < labels.size() && i < columnCount(); ++i) m_values->headers[i] = std::make_unique<QTableWidgetItem>(labels[i]); emit m_values->headerDataChanged(Qt::Horizontal, 0, columnCount() - 1); }
QTableWidgetItem* KernelResultTable::horizontalHeaderItem(int column) const { return column >= 0 && column < columnCount() ? m_values->headers[column].get() : nullptr; }
int KernelResultTable::rowCount() const { return m_values->rowCount(); }
int KernelResultTable::columnCount() const { return m_values->columnCount(); }
QTableWidgetItem* KernelResultTable::item(int row, int column) const { return m_values->item(row, column); }
int KernelResultTable::separatorRow() const
{ return m_values->values && m_values->values->source->separator >= 0 ? m_values->values->inverse[m_values->values->source->separator] : -1; }
void KernelResultTable::prepare(std::size_t rows, Factory factory, QString query, int searchRole, std::function<void()> ready, bool replaceSource)
{
    m_state->count = rows; m_state->factory = std::move(factory); m_state->query = std::move(query);
    m_state->searchRole = searchRole; m_state->ready = std::move(ready); m_state->dirty = true;
    if (replaceSource)
    {
        ++m_state->content;
        if (property("ksword_kernel_keep_unsorted").toBool()) m_state->sortColumn = -1;
        if (!property("ksword_kernel_keep_unsorted").toBool() && !isSortingEnabled())
        {
            setSortingEnabled(true);
            return; // Enabling invokes Model::sort and submits this complete source.
        }
    }
    request();
}
void KernelResultTable::setFilterEdit(QLineEdit* edit) { m_state->filterEdit = edit; }
void KernelResultTable::filter(QString query) { m_state->query = std::move(query); request(); }
void KernelResultTable::request()
{
    auto& state = *m_state;
    if (!state.factory && !state.source) return;
    const QPointer<KernelResultTable> alive(this);
    if (!state.locked) { state.forceDisabled = testAttribute(Qt::WA_ForceDisabled); state.locked = true; }
    setProperty(ks::ui::TablePresentationPreparingProperty, true);
    if (!alive) return;
    setEnabled(false);
    if (!alive) return;
    const auto revision = ++state.revision;
    struct Request
    {
        Factory factory;
        std::shared_ptr<const KernelResultValues> source;
        ks::i18n::SourceTextSnapshot texts;
        ksword::kernel_dock_internal::ResultColors colors;
        std::size_t count;
        int columns, role, column;
        Qt::SortOrder order;
        QString query;
        std::shared_ptr<const KernelResultProjection> previous;
    };
    Request input{state.dirty ? state.factory : Factory{}, state.source,
        ks::i18n::LanguageManager::instance().sourceTextSnapshot(), ksword::kernel_dock_internal::ResultColors::capture(), state.count, columnCount(), state.searchRole,
        state.sortColumn, state.sortOrder, state.query,
        state.content == state.acceptedContent ? m_values->values : std::shared_ptr<const KernelResultProjection>{}};
    state.operation->submit<Request, std::shared_ptr<const KernelResultProjection>>(std::move(input),
        [](const Request& input, const ks::ui::AsyncOperationToken& token) {
            ksword::kernel_dock_internal::ResultTextScope scope(input.texts);
            ResultColorScope colorScope(input.colors);
            auto source = input.source;
            if (input.factory)
            {
                auto values = std::make_shared<KernelResultValues>(input.count, input.columns);
                input.factory(*values);
                source = ks::ui::RetainCompleteExportSnapshot<KernelResultValues>(values);
            }
            if (token.isCanceled()) return std::shared_ptr<const KernelResultProjection>{};
            auto result = std::make_shared<KernelResultProjection>(); result->source = std::move(source);
            result->language = input.texts.languageId();
            result->order.reserve(static_cast<qsizetype>(result->source->rows.size()));
            bool deferredMatch = false;
            for (int row = 0; row < static_cast<int>(result->source->rows.size()); ++row)
            {
                if ((row & 127) == 0 && token.isCanceled()) return std::shared_ptr<const KernelResultProjection>{};
                result->order.push_back(row);
                if (row == result->source->separator) continue;
                const auto* first = result->source->rows[row][0].get();
                bool match = input.query.isEmpty();
                if (first && !match) for (const auto& field : first->data(input.role).toStringList())
                    if (field.contains(input.query, Qt::CaseInsensitive)) { match = true; break; }
                if (!match) result->excluded.insert(row);
                else if (result->source->separator >= 0 && row > result->source->separator) deferredMatch = true;
            }
            if (result->source->separator >= 0 && !deferredMatch) result->excluded.insert(result->source->separator);
            if (input.column >= 0 && input.previous && input.previous->order.size() == result->order.size()) result->order = input.previous->order;
            if (input.column >= 0 && input.column < input.columns)
                std::stable_sort(result->order.begin(), result->order.end(), [&](int a, int b) {
                    const auto* left = result->source->rows[a][input.column].get();
                    const auto* right = result->source->rows[b][input.column].get();
                    // Qt's native table ordering puts missing items last in both directions.
                    if (!left || !right) return left && !right;
                    return input.order == Qt::AscendingOrder ? *left < *right : *right < *left;
                });
            result->inverse.resize(result->order.size());
            for (int row = 0; row < result->order.size(); ++row) result->inverse[result->order[row]] = row;
            return ks::ui::RetainCompleteExportSnapshot<KernelResultProjection>(result);
        }, [this, revision](const std::shared_ptr<const KernelResultProjection>& result, std::uint64_t) {
            if (result) publish(result, revision);
            return true;
        }, [this](std::exception_ptr) {
            const QPointer<KernelResultTable> alive(this);
            setProperty(ks::ui::TablePresentationPreparingProperty, false);
            if (!alive) return;
            const bool enabled = !m_state->forceDisabled; m_state->locked = false;
            setEnabled(enabled);
            if (alive && preparationFailed) preparationFailed();
        });
}
void KernelResultTable::publish(std::shared_ptr<const KernelResultProjection> result, quint64 revision)
{
    if (revision != m_state->revision) return;
    if (m_state->filterEdit && m_state->filterEdit->text().trimmed() != m_state->query)
    { m_state->query = m_state->filterEdit->text().trimmed(); request(); return; }
    if (result->language != ks::i18n::LanguageManager::instance().currentLanguageId())
    { m_state->dirty = true; request(); return; }
    const QPointer<KernelResultTable> alive(this);
    if (auto* coordinator = ks::ui::UiCommitCoordinator::forApplication(); coordinator && coordinator->deferPresentationIfBlocked(this,
        QStringLiteral("kernel_result_values"), {this}, [alive, result, revision] { if (alive) alive->publish(result, revision); })) return;
    const bool newSource = m_state->content != m_state->acceptedContent;
    const int vertical = verticalScrollBar()->value(), horizontal = horizontalScrollBar()->value();
    m_state->source = result->source; m_state->dirty = false; m_state->acceptedContent = m_state->content;
    clearSpans();
    if (!alive || revision != m_state->revision) return;
    m_values->replace(result, newSource);
    if (!alive || revision != m_state->revision) return;
    if (result->source->separator >= 0 && result->source->separatorColumn >= 0)
        setSpan(separatorRow(), result->source->separatorColumn, 1, columnCount() - result->source->separatorColumn);
    ks::ui::ApplyTableRowFilterPredicateIncrementally(this, [result](const QModelIndex& index) {
        return index.isValid() && index.row() < result->order.size() && result->excluded.contains(result->order[index.row()]);
    });
    if (!alive || revision != m_state->revision) return;
    if (!newSource)
    {
        verticalScrollBar()->setValue(vertical);
        if (!alive || revision != m_state->revision) return;
        horizontalScrollBar()->setValue(horizontal);
        if (!alive || revision != m_state->revision) return;
    }
    if (selectionModel())
    {
        QItemSelection deselect;
        for (const auto& index : selectionModel()->selectedRows()) if (result->excluded.contains(result->order[index.row()]))
            deselect.select(index, index.siblingAtColumn(columnCount() - 1));
        if (!deselect.isEmpty()) selectionModel()->select(deselect, QItemSelectionModel::Deselect);
        if (!alive || revision != m_state->revision) return;
        if (currentRow() >= 0 && result->excluded.contains(result->order[currentRow()])) setCurrentIndex({});
    }
    if (!alive || revision != m_state->revision) return;
    setProperty("ksword_kernel_search_language", result->language);
    if (!alive || revision != m_state->revision) return;
    setProperty(ks::ui::TablePresentationPreparingProperty, false);
    if (!alive || revision != m_state->revision) return;
    const bool enabled = !m_state->forceDisabled; m_state->locked = false;
    setEnabled(enabled);
    if (!alive || revision != m_state->revision) return;
    auto ready = std::move(m_state->ready);
    if (ready) ready();
}

class KernelMinifilterTree::Model final : public QAbstractItemModel
{
public:
    using QAbstractItemModel::QAbstractItemModel;
    std::shared_ptr<const KernelMinifilterValues> values;
    QStringList headers;
    int rowCount(const QModelIndex& parent = {}) const override
    { return !values || parent.column() > 0 ? 0 : parent.isValid() ? static_cast<int>(values->nodes[parent.internalId() - 1].children.size()) : static_cast<int>(values->roots.size()); }
    int columnCount(const QModelIndex& = {}) const override { return static_cast<int>(headers.size()); }
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override
    {
        if (!hasIndex(row, column, parent)) return {};
        const int node = parent.isValid() ? values->nodes[parent.internalId() - 1].children[row] : values->roots[row];
        return createIndex(row, column, static_cast<quintptr>(node + 1));
    }
    QModelIndex parent(const QModelIndex& index) const override
    {
        if (!values || !index.isValid()) return {};
        const int parent = values->nodes[index.internalId() - 1].parent;
        return parent < 0 ? QModelIndex{} : createIndex(values->nodes[parent].position, 0, static_cast<quintptr>(parent + 1));
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!values || !index.isValid()) return {};
        const auto& node = values->nodes[index.internalId() - 1];
        const auto column = node.roles.constFind(index.column());
        if (column != node.roles.cend()) { const auto value = column->constFind(role); if (value != column->cend()) return *value; }
        return role == Qt::DisplayRole ? node.cells.value(index.column()) : QVariant{};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    { return orientation == Qt::Horizontal && role == Qt::DisplayRole ? QVariant(ks::i18n::displayText(headers.value(section))) : QVariant{}; }
    QModelIndex nodeIndex(int node, int column = 0) const
    { return values && node >= 0 && node < values->nodes.size() && values->nodes[node].accepted
        ? createIndex(values->nodes[node].position, column, static_cast<quintptr>(node + 1)) : QModelIndex{}; }
    void resizeColumns(int count) { beginResetModel(); headers.resize(count); endResetModel(); }
    void replace(std::shared_ptr<const KernelMinifilterValues> next, bool newSource)
    {
        const QPointer<Model> alive(this);
        if (!values || newSource) { beginResetModel(); if (!alive) return; values = std::move(next); endResetModel(); return; }
        emit layoutAboutToBeChanged();
        if (!alive) return;
        const auto persistent = persistentIndexList();
        values = std::move(next);
        QModelIndexList mapped; mapped.reserve(persistent.size());
        for (const auto& index : persistent) mapped.push_back(nodeIndex(static_cast<int>(index.internalId() - 1), index.column()));
        changePersistentIndexList(persistent, mapped);
        emit layoutChanged();
    }
};
struct KernelMinifilterTree::State { ks::ui::AsyncOperation* operation; quint64 revision = 0, sourceVersion = 0; QSet<qulonglong> collapsed; bool applying = false; };
KernelMinifilterTree::KernelMinifilterTree(QWidget* parent) : QTreeView(parent), m_values(new Model(this)), m_state(std::make_unique<State>())
{
    setModel(m_values); m_state->operation = new ks::ui::AsyncOperation(this);
    connect(this, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        if (!m_state->applying) m_state->collapsed.insert(index.data(Qt::UserRole).toULongLong());
    });
    connect(this, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        if (!m_state->applying) m_state->collapsed.remove(index.data(Qt::UserRole).toULongLong());
    });
}
KernelMinifilterTree::~KernelMinifilterTree() { m_state->operation->close(); }
void KernelMinifilterTree::setColumnCount(int count) { m_values->resizeColumns(count); }
void KernelMinifilterTree::setHeaderLabels(const QStringList& labels) { m_values->headers = labels; emit m_values->headerDataChanged(Qt::Horizontal, 0, static_cast<int>(labels.size()) - 1); }
void KernelMinifilterTree::prepare(std::function<std::shared_ptr<KernelMinifilterValues>()> factory, std::function<void()> ready, quint64 sourceVersion, std::function<bool()> accepts)
{
    const auto revision = ++m_state->revision;
    const auto colors = ksword::kernel_dock_internal::ResultColors::capture();
    const bool newSource = !m_values->values || sourceVersion != m_state->sourceVersion;
    if (newSource && !m_state->collapsed.isEmpty())
    {
        auto retired = ks::ui::RetainCompleteExportSnapshot<QSet<qulonglong>>(std::make_shared<const QSet<qulonglong>>(std::move(m_state->collapsed)));
        m_state->collapsed = {};
        (void)retired;
    }
    auto coloredFactory = [factory = std::move(factory), colors, previous = m_values->values,
        collapsed = m_state->collapsed, newSource] {
        ResultColorScope scope(colors);
        auto values = factory();
        QSet<int> previousRoots;
        if (previous && !newSource) for (const int root : previous->roots) previousRoots.insert(root);
        for (const int root : values->roots)
            if (!previousRoots.contains(root) && !collapsed.contains(values->nodes[root].roles.value(0).value(Qt::UserRole).toULongLong()))
                values->expandRoots.push_back(root);
        return ks::ui::RetainCompleteExportSnapshot<KernelMinifilterValues>(values);
    };
    m_state->operation->submit<std::function<std::shared_ptr<const KernelMinifilterValues>()>, std::shared_ptr<const KernelMinifilterValues>>(std::move(coloredFactory),
        [](const auto& build, const ks::ui::AsyncOperationToken&) { return build(); },
        [this, revision, sourceVersion, accepts = std::move(accepts), ready = std::move(ready)](const std::shared_ptr<const KernelMinifilterValues>& result, std::uint64_t) {
            const QPointer<KernelMinifilterTree> alive(this);
            const auto apply = [alive, result, revision, sourceVersion, accepts, ready] {
                if (!alive || revision != alive->m_state->revision || (accepts && !accepts())) return;
                const bool newSource = !alive->m_values->values || sourceVersion != alive->m_state->sourceVersion;
                alive->m_state->sourceVersion = sourceVersion;
                const int horizontal = alive->horizontalScrollBar()->value(), vertical = alive->verticalScrollBar()->value();
                alive->m_state->applying = true;
                alive->m_values->replace(result, newSource);
                if (!alive) return;
                if (revision != alive->m_state->revision) { alive->m_state->applying = false; return; }
                // Only newly exposed branches need expansion. Existing collapsed
                // branches retain their source identity across filtered layouts.
                for (const int root : result->expandRoots)
                {
                    alive->setExpanded(alive->m_values->nodeIndex(root), true);
                    if (!alive || revision != alive->m_state->revision) return;
                }
                alive->m_state->applying = false;
                if (!alive || revision != alive->m_state->revision) return;
                alive->horizontalScrollBar()->setValue(horizontal);
                if (!alive || revision != alive->m_state->revision) return;
                alive->verticalScrollBar()->setValue(vertical);
                if (!alive || revision != alive->m_state->revision) return;
                if (ready) ready();
            };
            if (auto* coordinator = ks::ui::UiCommitCoordinator::forApplication(); coordinator && coordinator->deferPresentationIfBlocked(this,
                QStringLiteral("kernel_minifilter_values"), {this}, apply)) return true;
            apply();
            return true;
        }, [](std::exception_ptr) {});
}
