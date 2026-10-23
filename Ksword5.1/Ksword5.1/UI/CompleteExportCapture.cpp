#include "CompleteExportCapture.h"
#include "CompleteExport.h"
#include <QAbstractItemView>
#include <QTreeView>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QRunnable>
#include <QTableView>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace ks::ui
{
    QString CompleteExportCapturedCell::text() const
    {
        QString result;
        for (const auto& fragment : fragments) result += fragment;
        return result;
    }
    const CompleteExportCapturedCell& CompleteExportCapturedTable::cell(const std::size_t row, const std::size_t column) const
    {
        if (row >= sourceRows.size() || column >= sourceColumns.size()) throw std::out_of_range("captured table cell");
        return cells.at(row * sourceColumns.size() + column);
    }
    bool CompleteExportCaptureStatus::terminal() const
    {
        return phase == CompleteExportCapturePhase::Completed || phase == CompleteExportCapturePhase::Canceled
            || phase == CompleteExportCapturePhase::Invalidated || phase == CompleteExportCapturePhase::Failed
            || phase == CompleteExportCapturePhase::Closed;
    }

    namespace
    {
        struct ModelPort final : QObject
        {
            QPointer<QAbstractItemView> view;
            QPointer<QItemSelectionModel> selection;
            QPersistentModelIndex root;
            QPointer<QAbstractItemModel> model;
            std::uint64_t revision = 0;
            std::uint64_t lastModel = 0, lastScope = 0, serial = 0;
            explicit ModelPort(QAbstractItemView* source, const CompleteExportCaptureViewWatch watch)
                : view(source), selection(source ? source->selectionModel() : nullptr),
                  root(source ? source->rootIndex() : QModelIndex{}), model(source ? source->model() : nullptr)
            {
                const auto changed = [this]() { ++revision; };
                if (model)
                {
                    QObject::connect(model, &QAbstractItemModel::dataChanged, this, changed);
                    QObject::connect(model, &QAbstractItemModel::headerDataChanged, this, changed);
                    QObject::connect(model, &QAbstractItemModel::modelAboutToBeReset, this, changed);
                    QObject::connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, changed);
                    QObject::connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this, changed);
                    QObject::connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, changed);
                    QObject::connect(model, &QAbstractItemModel::rowsAboutToBeMoved, this, changed);
                    QObject::connect(model, &QAbstractItemModel::columnsAboutToBeInserted, this, changed);
                    QObject::connect(model, &QAbstractItemModel::columnsAboutToBeRemoved, this, changed);
                    QObject::connect(model, &QAbstractItemModel::columnsAboutToBeMoved, this, changed);
                }
                if (view)
                {
                    // Scoped to the one captured view. LanguageChange also
                    // catches same-id pack reload/retranslation epochs.
                    view->installEventFilter(this);
                    if (watch.geometry)
                    {
                        QList<QHeaderView*> headers;
                        if (auto* table = qobject_cast<QTableView*>(view.data()))
                            headers = { table->horizontalHeader(), table->verticalHeader() };
                        else if (auto* tree = qobject_cast<QTreeView*>(view.data())) headers = { tree->header() };
                        for (auto* header : headers)
                        {
                            QObject::connect(header, &QHeaderView::geometriesChanged, this, changed);
                            QObject::connect(header, &QHeaderView::sectionMoved, this, changed);
                            QObject::connect(header, &QHeaderView::sectionResized, this, changed);
                        }
                    }
                    if (auto* tree = qobject_cast<QTreeView*>(view.data()))
                    {
                        QObject::connect(tree, &QTreeView::expanded, this, changed);
                        QObject::connect(tree, &QTreeView::collapsed, this, changed);
                    }
                    if (watch.selection && view->selectionModel())
                    {
                        QObject::connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, changed);
                        QObject::connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this, changed);
                    }
                }
            }
            bool valid() const { return view && model && view->model() == model
                && view->selectionModel() == selection && view->rootIndex() == root; }
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (watched == view && event && event->type() == QEvent::LanguageChange) ++revision;
                return false;
            }
        };

        QString currentException()
        {
            try { throw; }
            catch (const std::exception& error) { return QString::fromUtf8(error.what()); }
            catch (...) { return {}; }
        }
    }

    CompleteExportCapturePort MakeCompleteExportItemViewCapturePort(QAbstractItemView* view,
        QString sourceIdentity, const std::uint64_t sourceVersion,
        std::function<bool(int)> rowIncluded, std::function<bool(int)> columnIncluded,
        std::function<std::uint64_t()> scopeRevision, std::function<QString()> languageId,
        const CompleteExportCaptureViewWatch watch)
    {
        CompleteExportCapturePort port;
        port.source = view;
        port.sourceIdentity = std::move(sourceIdentity);
        port.sourceVersion = sourceVersion;
        if (!view || !view->model()) return port;
        const auto state = std::make_shared<ModelPort>(view, watch);
        port.valid = [state]() { return state->valid(); };
        // Keep model and scope counters separate through an injective pair
        // check, rather than an XOR/hash that can miss opposing mutations.
        state->lastModel = state->revision;
        port.revision = [state, scopeRevision = std::move(scopeRevision)]()
        {
            const auto scope = scopeRevision ? scopeRevision() : 0;
            if (state->revision != state->lastModel || scope != state->lastScope)
            {
                state->lastModel = state->revision; state->lastScope = scope; ++state->serial;
            }
            return state->serial;
        };
        port.languageId = std::move(languageId);
        port.rowCount = [state]() { return state->model ? state->model->rowCount(state->root) : -1; };
        port.columnCount = [state]() { return state->model ? state->model->columnCount(state->root) : -1; };
        port.rowIncluded = std::move(rowIncluded);
        port.columnIncluded = std::move(columnIncluded);
        port.header = [state](const int column)
        { return state->model ? state->model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString() : QString{}; };
        port.cell = [state](const int row, const int column)
        {
            const auto model = state->model;
            if (!model) return QString{};
            const auto index = model->index(row, column, state->root);
            return model ? model->data(index, Qt::DisplayRole).toString() : QString{};
        };
        return port;
    }

    CompleteExportCapturePort MakeCompleteExportTableViewCapturePort(QTableView* view,
        QString sourceIdentity, const std::uint64_t sourceVersion,
        std::function<bool(int)> rowIncluded, std::function<bool(int)> columnIncluded,
        std::function<std::uint64_t()> scopeRevision, std::function<QString()> languageId,
        const CompleteExportCaptureViewWatch watch)
    {
        return MakeCompleteExportItemViewCapturePort(view, std::move(sourceIdentity), sourceVersion,
            std::move(rowIncluded), std::move(columnIncluded), std::move(scopeRevision), std::move(languageId), watch);
    }

    struct CompleteExportCapture::State
    {
        CompleteExportCaptureStatus status;
        std::shared_ptr<const CompleteExportCapturePort> port;
        CompleteExportCaptureBudget budget;
        Completion completion;
        std::function<void(const CompleteExportCaptureStatus&)> progress;
        std::shared_ptr<CompleteExportCapturedTable> table;
        std::uint64_t nextGeneration = 0;
        std::uint64_t revision = 0;
        int rowCount = 0, columnCount = 0, nextColumn = 0;
        std::uint64_t nextRow = 0;
        std::size_t rowColumn = 0;
        QString language;
        QString pendingText;
        CompleteExportCapturedCell pendingCell;
        qsizetype pendingOffset = 0;
        enum class Pending { None, Header, Cell } pending = Pending::None;
        bool rowStarted = false, initialized = false, queued = false, busy = false, closed = false;
    };

    CompleteExportCapture::CompleteExportCapture(QObject* owner) : QObject(owner), m_state(std::make_unique<State>())
    {
        if (owner) QObject::connect(owner, &QObject::destroyed, this, [this]() { close(); });
    }
    CompleteExportCapture::~CompleteExportCapture() { close(); }

    std::uint64_t CompleteExportCapture::start(CompleteExportCapturePort port, Completion completion, CompleteExportCaptureBudget budget)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (m_state->closed || m_state->busy || !port.source || !port.revision || !port.rowCount
            || !port.columnCount || !port.header || (!port.scopeOnly && !port.cell) || budget.milliseconds <= 0
            || budget.maximumUnits <= 0 || budget.utf16UnitsPerFragment <= 1
            || m_state->nextGeneration == std::numeric_limits<std::uint64_t>::max()) return 0;
        m_state->status = {};
        m_state->status.generation = ++m_state->nextGeneration;
        m_state->budget = budget;
        m_state->port = std::make_shared<const CompleteExportCapturePort>(std::move(port));
        m_state->completion = std::move(completion);
        const auto mutableTable = std::make_shared<CompleteExportCapturedTable>();
        const auto retainedTable = RetainCompleteExportSnapshot(std::shared_ptr<const CompleteExportCapturedTable>(mutableTable));
        m_state->table = std::shared_ptr<CompleteExportCapturedTable>(retainedTable, mutableTable.get());
        m_state->table->sourceIdentity = m_state->port->sourceIdentity;
        m_state->table->sourceVersion = m_state->port->sourceVersion;
        m_state->nextColumn = 0; m_state->nextRow = 0;
        m_state->rowColumn = 0;
        m_state->rowStarted = m_state->initialized = false;
        m_state->busy = true;
        // Freeze the accepted epoch now, before the first queued slice. A sort,
        // same-size replacement or language change in that gap must invalidate
        // the whole preparation, not become a newer baseline for older values.
        const QPointer<CompleteExportCapture> guard(this);
        const auto generation = m_state->status.generation;
        const auto retainedPort = m_state->port;
        const auto current = [&]() { return guard && m_state->busy && m_state->status.generation == generation; };
        try
        {
            const bool valid = retainedPort->source && (!retainedPort->valid || retainedPort->valid());
            if (!current()) return generation;
            if (!valid) { finish(CompleteExportCapturePhase::Invalidated, CompleteExportCaptureError::SourceChanged); return generation; }
            const auto revision = retainedPort->revision();
            if (!current()) return generation;
            const auto language = retainedPort->languageId ? retainedPort->languageId() : QString{};
            if (!current()) return generation;
            const auto rows = retainedPort->rowCount();
            if (!current()) return generation;
            const auto columns = retainedPort->columnCount();
            if (!current()) return generation;
            if (rows < 0 || columns < 0)
            { finish(CompleteExportCapturePhase::Failed, CompleteExportCaptureError::InvalidPort); return generation; }
            m_state->revision = revision; m_state->language = language;
            m_state->rowCount = rows; m_state->columnCount = columns;
            m_state->table->languageId = language; m_state->status.sourceRows = rows;
            m_state->initialized = true;
            queue();
        }
        catch (...)
        {
            if (current()) finish(CompleteExportCapturePhase::Failed, CompleteExportCaptureError::ReadFailed, currentException());
        }
        return generation;
    }

    void CompleteExportCapture::queue()
    {
        if (m_state->queued || !m_state->busy || m_state->closed) return;
        m_state->queued = true;
        const auto generation = m_state->status.generation;
        QTimer::singleShot(0, this, [this, generation]()
        {
            // An older queued slice must not consume a new preparation's slot.
            if (m_state->status.generation != generation) return;
            m_state->queued = false;
            step();
        });
    }

    bool CompleteExportCapture::unchanged()
    {
        // Copies keep each GUI callable alive across a nested cancel/delete.
        const auto port = m_state->port;
        const QPointer<CompleteExportCapture> guard(this);
        const auto generation = m_state->status.generation;
        const auto initialized = m_state->initialized;
        const auto revision = m_state->revision;
        const auto language = m_state->language;
        const auto rows = m_state->rowCount, columns = m_state->columnCount;
        const auto current = [&]() { return guard && m_state->busy && m_state->status.generation == generation; };
        if (!port->source || (port->valid && !port->valid())) return false;
        if (!current()) return false;
        if (!initialized) return true;
        const auto nowRevision = port->revision();
        if (!current() || nowRevision != revision) return false;
        const auto nowLanguage = port->languageId ? port->languageId() : QString{};
        if (!current() || nowLanguage != language) return false;
        const auto nowRows = port->rowCount();
        if (!current() || nowRows != rows) return false;
        const auto nowColumns = port->columnCount();
        return current() && nowColumns == columns;
    }

    void CompleteExportCapture::step()
    {
        if (!m_state->busy || m_state->closed) return;
        const QPointer<CompleteExportCapture> guard(this);
        const auto generation = m_state->status.generation;
        const auto stillCurrent = [&]() { return guard && m_state->busy && m_state->status.generation == generation; };
        try
        {
            const bool initiallyUnchanged = unchanged();
            if (!stillCurrent()) return;
            if (!initiallyUnchanged) { finish(CompleteExportCapturePhase::Invalidated, CompleteExportCaptureError::SourceChanged); return; }
            auto& state = *m_state;
            state.status.phase = CompleteExportCapturePhase::Capturing;
            QElapsedTimer elapsed; elapsed.start();
            for (int unit = 0; unit < state.budget.maximumUnits && elapsed.elapsed() < state.budget.milliseconds; ++unit)
            {
                const bool currentVersion = unchanged();
                if (!stillCurrent()) return;
                if (!currentVersion) { finish(CompleteExportCapturePhase::Invalidated, CompleteExportCaptureError::SourceChanged); return; }
                if (state.pending != State::Pending::None)
                {
                    auto length = std::min<qsizetype>(state.budget.utf16UnitsPerFragment, state.pendingText.size() - state.pendingOffset);
                    if (state.pendingOffset + length < state.pendingText.size() && length > 0
                        && state.pendingText[state.pendingOffset + length - 1].isHighSurrogate()
                        && state.pendingText[state.pendingOffset + length].isLowSurrogate()) --length;
                    // Constructing from characters deliberately deep-copies a
                    // bounded fragment, including a QString::fromRawData value.
                    if (length > 0) state.pendingCell.fragments.emplace_back(state.pendingText.constData() + state.pendingOffset, length);
                    state.pendingOffset += length;
                    if (state.pendingOffset == state.pendingText.size())
                    {
                        state.pendingText.clear(); state.pendingOffset = 0;
                        if (state.pending == State::Pending::Header)
                        {
                            state.table->sourceColumns.push_back(state.nextColumn++);
                            state.table->headers.push_back(std::move(state.pendingCell));
                        }
                        else
                        {
                            state.table->cells.push_back(std::move(state.pendingCell));
                            ++state.status.capturedCells; ++state.rowColumn;
                        }
                        state.pendingCell = {}; state.pending = State::Pending::None;
                    }
                    continue;
                }
                if (state.nextColumn < state.columnCount)
                {
                    const auto port = state.port;
                    const bool included = !port->columnIncluded || port->columnIncluded(state.nextColumn);
                    if (!stillCurrent()) return;
                    if (included)
                    {
                        auto text = port->header(state.nextColumn);
                        if (!stillCurrent()) return;
                        state.pendingText = std::move(text);
                        state.pending = State::Pending::Header;
                    }
                    else ++state.nextColumn;
                    continue;
                }
                if ((!state.port->rowAvailable && state.nextRow >= static_cast<std::uint64_t>(state.rowCount))
                    || state.table->sourceColumns.empty())
                { finish(CompleteExportCapturePhase::Completed); return; }
                if (!state.rowStarted)
                {
                    const auto port = state.port;
                    if (port->rowAvailable)
                    {
                        const auto available = port->rowAvailable(state.nextRow);
                        if (!stillCurrent()) return;
                        if (!available) { finish(CompleteExportCapturePhase::Completed); return; }
                    }
                    const bool included = !port->rowIncluded || port->rowIncluded(static_cast<int>(state.nextRow));
                    if (!stillCurrent()) return;
                    if (!included) { ++state.nextRow; ++state.status.visitedRows; continue; }
                    if (port->rowCaptured)
                    {
                        port->rowCaptured(static_cast<int>(state.nextRow));
                        if (!stillCurrent()) return;
                    }
                    state.rowStarted = true; state.rowColumn = 0;
                }
                if (state.port->scopeOnly || state.rowColumn == state.table->sourceColumns.size())
                {
                    state.table->sourceRows.push_back(state.nextRow++);
                    ++state.status.capturedRows; ++state.status.visitedRows;
                    state.rowStarted = false;
                }
                else
                {
                    const auto port = state.port;
                    auto text = port->cell(static_cast<int>(state.nextRow), state.table->sourceColumns.at(state.rowColumn));
                    if (!stillCurrent()) return;
                    state.pendingText = std::move(text);
                    state.pending = State::Pending::Cell;
                }
            }
            if (!stillCurrent()) return;
            // Verify once more after the final read/fragment, before progress
            // or completion can hand off a complete stable value version.
            const bool finalVersion = unchanged();
            if (!stillCurrent()) return;
            if (!finalVersion) { finish(CompleteExportCapturePhase::Invalidated, CompleteExportCaptureError::SourceChanged); return; }
            const auto callback = state.progress;
            const auto status = state.status;
            if (callback) callback(status);
            if (stillCurrent()) queue();
        }
        catch (...)
        {
            if (stillCurrent()) finish(CompleteExportCapturePhase::Failed, CompleteExportCaptureError::ReadFailed, currentException());
        }
    }

    void CompleteExportCapture::finish(const CompleteExportCapturePhase phase, const CompleteExportCaptureError error, QString detail)
    {
        if (!m_state->busy) return;
        m_state->status.phase = phase; m_state->status.error = error; m_state->status.nativeError = std::move(detail);
        const auto status = m_state->status;
        const auto completion = std::move(m_state->completion);
        std::shared_ptr<const CompleteExportCapturedTable> table;
        if (phase == CompleteExportCapturePhase::Completed) table = std::move(m_state->table);
        else
        {
            // A canceled multi-megabyte cell can itself hold many owned
            // fragments. Move its tail into the abandoned value so those
            // fragment destructors also run in the queued value deleter.
            if (m_state->table)
            {
                m_state->pendingCell.fragments.push_back(std::move(m_state->pendingText));
                m_state->table->cells.push_back(std::move(m_state->pendingCell));
            }
            m_state->table.reset();
        }
        m_state->port = {};
        m_state->pendingText.clear(); m_state->pendingCell = {}; m_state->pendingOffset = 0;
        m_state->pending = State::Pending::None;
        m_state->busy = false; m_state->queued = false;
        if (completion) { try { completion(status, std::move(table)); } catch (...) {} }
    }
    void CompleteExportCapture::cancel() { finish(CompleteExportCapturePhase::Canceled); }
    void CompleteExportCapture::close()
    {
        if (m_state->closed) return;
        m_state->closed = true; m_state->completion = {}; m_state->progress = {};
        if (m_state->busy) finish(CompleteExportCapturePhase::Closed);
    }
    bool CompleteExportCapture::isBusy() const { return m_state->busy; }
    CompleteExportCaptureStatus CompleteExportCapture::status() const { return m_state->status; }
    void CompleteExportCapture::setProgressCallback(std::function<void(const CompleteExportCaptureStatus&)> callback)
    { m_state->progress = std::move(callback); }
}
