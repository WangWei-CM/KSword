#include "TableCompleteExport.h"
#include "TableInteractionSupport.h"
#include "CompleteExportCapture.h"
#include "TableFreezeSupport.h"
#include "TableRowFilterSupport.h"
#include "UiCommitCoordinator.h"
#include "../Framework.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHash>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTableView>
#include <QTreeView>

#include <algorithm>
#include <limits>
#include <utility>

namespace ks::ui
{
    namespace
    {
        QString text(const char* source) { return ks::i18n::sourceText(QString::fromUtf8(source)); }
        constexpr char kProducerName[] = "ksword_complete_export_producer";
        constexpr char kControllerName[] = "ksword_complete_export_controller";
        struct Producer final : QObject
        {
            CompleteExportTableProducer factory;
            explicit Producer(QTableView* view) : QObject(view) { setObjectName(QLatin1String(kProducerName)); }
        };
        template<class Object> Object* namedChild(QObject* owner, const char* name)
        {
            if (owner) for (auto* child : owner->children())
                if (child->objectName() == QLatin1String(name)) return static_cast<Object*>(child);
            return nullptr;
        }

        QVector<QPersistentModelIndex> selectedTreeRowsInVisualOrder(QTreeView* tree)
        {
            struct Row { QPersistentModelIndex index; QVector<int> path; };
            QVector<Row> rows;
            if (!tree || !tree->model() || !tree->selectionModel()) return {};
            const QModelIndex root = tree->rootIndex();
            QHash<QPersistentModelIndex, QVector<std::pair<int, int>>> rangesByParent;
            for (const auto& range : tree->selectionModel()->selection())
                if (range.isValid()) rangesByParent[QPersistentModelIndex(range.parent())].push_back({range.top(), range.bottom()});
            for (auto it = rangesByParent.begin(); it != rangesByParent.end(); ++it)
            {
                auto& intervals = it.value();
                std::sort(intervals.begin(), intervals.end());
                int emittedThrough = -1;
                for (const auto& interval : intervals)
                {
                    for (int row = std::max(interval.first, emittedThrough + 1); row <= interval.second; ++row)
                    {
                        const QModelIndex index = tree->model()->index(row, 0, it.key());
                        QModelIndex current = index;
                        QVector<int> path;
                        bool visible = index.isValid();
                        while (visible && current != root)
                        {
                            if (!current.isValid() || tree->isRowHidden(current.row(), current.parent())) { visible = false; break; }
                            path.push_back(current.row());
                            const auto parent = current.parent();
                            if (parent != root && (!parent.isValid() || !tree->isExpanded(parent))) { visible = false; break; }
                            current = parent;
                        }
                        if (visible && current == root)
                        {
                            std::reverse(path.begin(), path.end());
                            rows.push_back({QPersistentModelIndex(index), std::move(path)});
                        }
                    }
                    emittedThrough = std::max(emittedThrough, interval.second);
                }
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
            { return std::lexicographical_compare(a.path.cbegin(), a.path.cend(), b.path.cbegin(), b.path.cend()); });
            QVector<QPersistentModelIndex> result;
            result.reserve(rows.size());
            for (auto& row : rows) result.push_back(std::move(row.index));
            return result;
        }

        struct ExportValues
        {
            std::shared_ptr<const CompleteExportCapturedTable> scope;
            CompleteExportPublishedTable published;
            bool includeHeader = false;
            bool appendNewline = false;
        };
        bool normalizedText(CompleteExportSink& sink, QStringView value)
        {
            constexpr qsizetype chunk = 32768;
            qsizetype offset = 0;
            while (offset < value.size())
            {
                auto count = std::min(chunk, value.size() - offset);
                if (offset + count < value.size() && value[offset + count - 1].isHighSurrogate()
                    && value[offset + count].isLowSurrogate()) --count;
                auto part = value.sliced(offset, count).toString();
                for (auto& character : part)
                    if (character == QLatin1Char('\t') || character == QLatin1Char('\r') || character == QLatin1Char('\n'))
                        character = QLatin1Char(' ');
                if (!sink.writeText(part)) return false;
                offset += count;
            }
            return sink.good();
        }
        bool normalizedCell(CompleteExportSink& sink, const CompleteExportCapturedCell& value)
        {
            for (const auto& fragment : value.fragments) if (!normalizedText(sink, fragment)) return false;
            return sink.good();
        }
        bool serialize(const ExportValues& value, CompleteExportSink& sink, const ks::i18n::SourceTextSnapshot&)
        {
            if (!value.scope || value.scope->sourceColumns.empty()) return false;
            const auto& scope = *value.scope;
            bool hasLine = false;
            if (value.includeHeader)
            {
                if (scope.headers.size() != scope.sourceColumns.size()) return false;
                for (std::size_t column = 0; column < scope.headers.size(); ++column)
                {
                    if (column && !sink.writeText(u"\t")) return false;
                    if (!normalizedCell(sink, scope.headers[column])) return false;
                }
                hasLine = true;
            }
            for (std::size_t row = 0; row < scope.sourceRows.size(); ++row)
            {
                if (hasLine && !sink.writeText(u"\n")) return false;
                for (std::size_t column = 0; column < scope.sourceColumns.size(); ++column)
                {
                    if (column && !sink.writeText(u"\t")) return false;
                    if (value.published.values)
                    {
                        const auto sourceRow = scope.sourceRows[row];
                        const auto sourceColumn = scope.sourceColumns[column];
                        if (!value.published.cell || sourceRow >= static_cast<std::uint64_t>(value.published.rows)
                            || sourceColumn < 0 || sourceColumn >= value.published.columns) return false;
                        const auto cell = value.published.cell(value.published.values.get(), static_cast<int>(sourceRow), sourceColumn);
                        if (!normalizedText(sink, cell)) return false;
                    }
                    else if (!normalizedCell(sink, scope.cell(row, column))) return false;
                }
                if (!sink.rowsCompleted()) return false;
                hasLine = true;
            }
            return !value.appendNewline || !hasLine || sink.writeText(u"\n");
        }

        class Controller final : public QObject
        {
        public:
            explicit Controller(QAbstractItemView* view)
                : QObject(view), m_view(view), m_capture(this), m_operation(this)
            {
                setObjectName(QLatin1String(kControllerName));
                view->installEventFilter(this);
                if (view->viewport()) view->viewport()->installEventFilter(this);
                m_operation.setStateChangedCallback([this]() { updateBusy(); });
                m_capture.setProgressCallback([this](const CompleteExportCaptureStatus& status)
                {
                    if (m_progress) kPro.set(m_progress, text("正在准备完整复制/导出（Esc 取消准备）").toStdString(), 0,
                        status.sourceRows > 0 ? 0.8f * static_cast<float>(status.visitedRows) / static_cast<float>(status.sourceRows) : 0.0f);
                });
            }
            ~Controller() override
            {
                m_closed = true;
                m_capture.close(); m_operation.close(); releaseLease();
                // Producer roots and captured scope use worker-retired handles.
                m_published = {};
            }
            void start(CompleteTableScope scope, bool file, const QString& prefix, CompleteTableColumns columns = CompleteTableColumns::Visible)
            {
                const QPointer<Controller> alive(this);
                try { startImpl(scope, file, prefix, columns); }
                catch (...) { if (alive) failedPreparation(); }
            }
            void startImpl(CompleteTableScope scope, bool file, const QString& prefix, CompleteTableColumns columns)
            {
                const QPointer<Controller> alive(this);
                m_operation.reconcile();
                if (!alive || m_closed || !m_view || !m_view->model()) return;
                if (IsTablePresentationPreparing(qobject_cast<QTableView*>(m_view.data()))) return;
                if (busy())
                {
                    QMessageBox::information(m_view, text("导出 TSV"), text("当前复制/导出尚未完成，请稍候。"));
                    return;
                }
                QString path;
                if (file)
                {
                    m_dialog = true; updateBusy();
                    if (!alive) return;
                    path = QFileDialog::getSaveFileName(m_view, text("导出 TSV"),
                        prefix + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")) + QStringLiteral(".tsv"),
                        text("TSV 文件 (*.tsv)"));
                    if (!alive) return;
                    m_dialog = false; updateBusy();
                    if (!alive) return;
                    if (m_closed || !m_view || !m_view->model() || path.trimmed().isEmpty()) return;
                    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".tsv");
                    if (busy()) return; // Native dialogs can reenter, close or admit another request.
                    if (IsTablePresentationPreparing(qobject_cast<QTableView*>(m_view.data()))) return;
                }
                m_scope = scope; m_columns = columns; m_file = file; m_path = std::move(path); m_fallback = false;
                m_translator = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
                m_current = m_view->currentIndex();
                m_coordinator = UiCommitCoordinator::forApplication();
                if (m_coordinator) { m_lease = true; m_coordinator->beginValueCapture(m_view); }
                if (!alive || !m_view) return;
                m_published = {};
                if (auto* table = qobject_cast<QTableView*>(m_view.data()))
                    if (const auto* producer = namedChild<Producer>(table, kProducerName))
                    {
                        const auto factory = producer->factory;
                        if (factory) m_published = factory();
                        if (!alive) return;
                        if (!m_view || !m_view->model() || !m_published.values || !m_published.cell
                            || m_published.rows != m_view->model()->rowCount(m_view->rootIndex())
                            || m_published.columns != m_view->model()->columnCount(m_view->rootIndex()))
                        {
                            endPreparation(kProgressState::Failure);
                            if (!alive) return;
                            QMessageBox::warning(m_view, text("导出 TSV"), text("当前表格尚无可用的完整数据版本，请刷新后重试。"));
                            return;
                        }
                    }
                m_progress = kPro.add(this, text("复制/导出完整表格").toStdString(), text("正在准备完整复制/导出（Esc 取消准备）").toStdString());
                beginCapture();
            }
        protected:
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (m_view && (watched == m_view || watched == m_view->viewport()) && event && event->type() == QEvent::KeyPress
                    && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && m_capture.isBusy())
                {
                    m_capture.cancel(); return true;
                }
                return QObject::eventFilter(watched, event);
            }
        private:
            bool busy() const { return m_dialog || m_capture.isBusy() || m_operation.isBusy(); }
            void updateBusy()
            {
                if (!m_view || m_closed) return;
                // Set preparation first so the single busy notification observes
                // the correct cancel semantics when toolbar roles refresh.
                const auto view = m_view;
                const bool preparing = m_capture.isBusy(), active = busy();
                view->setProperty(CompleteExportPreparingProperty, preparing);
                if (view) view->setProperty(CompleteExportBusyProperty, active);
            }
            void releaseLease()
            {
                if (!m_lease) return;
                m_lease = false;
                if (m_coordinator) m_coordinator->endValueCapture(m_view);
            }
            void endPreparation(kProgressState state)
            {
                const QPointer<Controller> alive(this);
                m_published = {};
                if (m_progress) kPro.finish(std::exchange(m_progress, 0), state, text("复制/导出准备结束").toStdString());
                releaseLease();
                if (alive) updateBusy();
            }
            void beginCapture()
            {
                if (!m_view || !m_view->model()) { endPreparation(kProgressState::Failure); return; }
                const QPointer<Controller> alive(this);
                auto* table = qobject_cast<QTableView*>(m_view.data());
                auto* tree = qobject_cast<QTreeView*>(m_view.data());
                const auto selected = m_scope == CompleteTableScope::Selected;
                const auto fallback = m_fallback;
                const auto current = m_current;
                auto port = MakeCompleteExportItemViewCapturePort(m_view,
                    m_published.values ? m_published.sourceIdentity : QStringLiteral("view/%1").arg(reinterpret_cast<quintptr>(m_view.data()), 0, 16),
                    m_published.sourceVersion,
                    [alive, table, selected, fallback, current](int row)
                    {
                        if (!alive || !table || !table->selectionModel() || IsTableRowExcludedByFilter(table, row)) return false;
                        if (fallback) return current.isValid() && row == current.row();
                        return !selected || table->selectionModel()->rowIntersectsSelection(row, table->rootIndex());
                    },
                    [table, columns = m_columns](int column) { return table && (columns == CompleteTableColumns::All
                        || !table->isColumnHidden(column) || isColumnHiddenByFreeze(table, column)); },
                    [table]() { return table ? TableRowFilterIdentityRevision(table) : 0; },
                    []() { return ks::i18n::LanguageManager::instance().currentLanguageId(); }, {true, selected});
                port.scopeOnly = static_cast<bool>(m_published.values);
                port.rowCaptured = m_published.prepareRow;
                if (tree)
                {
                    struct Cursor { QPersistentModelIndex index; };
                    const auto cursor = std::make_shared<Cursor>();
                    const auto selectedRows = std::make_shared<const QVector<QPersistentModelIndex>>(
                        selectedTreeRowsInVisualOrder(tree));
                    port.rowAvailable = [cursor, selectedRows, fallback, current](std::uint64_t ordinal)
                    {
                        if (fallback) { cursor->index = current; return ordinal == 0 && current.isValid(); }
                        if (ordinal >= static_cast<std::uint64_t>(selectedRows->size())) return false;
                        cursor->index = selectedRows->at(static_cast<qsizetype>(ordinal));
                        return cursor->index.isValid();
                    };
                    port.rowIncluded = [](int) { return true; };
                    port.columnIncluded = [tree](int position)
                    { const int column = tree->header() ? tree->header()->logicalIndex(position) : position;
                      return column >= 0 && !tree->isColumnHidden(column); };
                    port.header = [tree](int position)
                    { const int column = tree->header() ? tree->header()->logicalIndex(position) : position;
                      return tree->model()->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(); };
                    port.cell = [tree, cursor](int, int position)
                    { const int column = tree->header() ? tree->header()->logicalIndex(position) : position;
                      return tree->model()->data(cursor->index.sibling(cursor->index.row(), column), Qt::DisplayRole).toString(); };
                }
                const auto started = m_capture.start(std::move(port), [alive](const CompleteExportCaptureStatus& status,
                    std::shared_ptr<const CompleteExportCapturedTable> captured)
                { if (alive) alive->captured(status, std::move(captured)); });
                if (!alive) return;
                if (!started) { endPreparation(kProgressState::Failure); return; }
                updateBusy();
            }
            void failedPreparation()
            {
                const QPointer<Controller> alive(this);
                endPreparation(kProgressState::Failure);
                if (alive && m_view) QMessageBox::warning(m_view, text("导出 TSV"), text("复制/导出准备失败，请重试。"));
            }
            void captured(const CompleteExportCaptureStatus& status, std::shared_ptr<const CompleteExportCapturedTable> captured)
            {
                const QPointer<Controller> alive(this);
                try { capturedImpl(status, std::move(captured)); }
                catch (...) { if (alive) failedPreparation(); }
            }
            void capturedImpl(const CompleteExportCaptureStatus& status, std::shared_ptr<const CompleteExportCapturedTable> captured)
            {
                const QPointer<Controller> alive(this);
                if (status.phase != CompleteExportCapturePhase::Completed || !captured)
                {
                    const bool canceled = status.phase == CompleteExportCapturePhase::Canceled || status.phase == CompleteExportCapturePhase::Closed;
                    endPreparation(canceled ? kProgressState::Canceled : kProgressState::Failure);
                    if (alive && !canceled && m_view)
                        QMessageBox::warning(m_view, text("导出 TSV"), status.phase == CompleteExportCapturePhase::Invalidated
                            ? text("复制/导出准备期间数据或范围已变化，请重试。") : text("复制/导出准备失败，请重试。"));
                    return;
                }
                if (captured->sourceRows.empty() && m_scope == CompleteTableScope::Selected && !m_fallback && m_current.isValid())
                { m_fallback = true; beginCapture(); return; }
                if (captured->sourceRows.empty() || captured->sourceColumns.empty())
                {
                    const bool file = m_file;
                    endPreparation(kProgressState::Success);
                    if (alive && file && m_view) QMessageBox::information(m_view, text("导出 TSV"), text("没有可导出的行。"));
                    return;
                }
                m_published.prepareRow = {}; // Every GUI callable is destroyed before the pure value worker.
                auto input = MakeCompleteTableExportInput(captured, std::move(m_published), m_file, m_file, m_translator);
                releaseLease(); // Complete pure values now survive every subsequent live update.
                if (!alive) return;
                if (m_progress) kPro.set(m_progress, text("正在处理，请稍候…").toStdString(), 0, 0.9f);
                const auto completed = [alive](const CompleteExportReceipt& receipt)
                {
                    if (!alive) return;
                    if (alive->m_progress) kPro.finish(std::exchange(alive->m_progress, 0),
                        receipt.succeeded() ? kProgressState::Success : kProgressState::Failure,
                        text("复制/导出准备结束").toStdString());
                    alive->updateBusy();
                    if (!receipt.succeeded() && alive->m_view)
                        QMessageBox::warning(alive->m_view, text("导出 TSV"), text("导出失败：%1").arg(receipt.nativeError));
                };
                std::uint64_t id;
                if (m_file) id = m_operation.startFile(m_path, std::move(input), completed);
                else
                {
                    const bool suppressEmpty = qobject_cast<QTableView*>(m_view.data()) != nullptr;
                    id = m_operation.startCopy(std::move(input), [suppressEmpty](const QString& copied)
                    {
                        if (suppressEmpty && copied.isEmpty()) return true; // Preserve the table's legacy empty-text no-op.
                        auto* clipboard = QApplication::clipboard();
                        if (!clipboard) return false;
                        clipboard->setText(copied); return true;
                    }, completed);
                }
                if (!alive) return;
                if (!id) endPreparation(kProgressState::Failure);
                else updateBusy();
            }
            QPointer<QAbstractItemView> m_view;
            QPointer<UiCommitCoordinator> m_coordinator;
            CompleteExportCapture m_capture;
            CompleteExportOperation m_operation;
            CompleteExportPublishedTable m_published;
            ks::i18n::SourceTextSnapshot m_translator;
            QPersistentModelIndex m_current;
            QString m_path;
            CompleteTableScope m_scope = CompleteTableScope::All;
            CompleteTableColumns m_columns = CompleteTableColumns::Visible;
            int m_progress = 0;
            bool m_file = false, m_fallback = false, m_dialog = false, m_lease = false, m_closed = false;
        };
        Controller* controller(QAbstractItemView* view)
        {
            if (!view) return nullptr;
            auto* existing = namedChild<Controller>(view, kControllerName);
            return existing ? existing : new Controller(view);
        }
    }

    CompleteExportInput MakeCompleteTableExportInput(std::shared_ptr<const CompleteExportCapturedTable> scope,
        CompleteExportPublishedTable published, bool includeHeader, bool appendNewline,
        ks::i18n::SourceTextSnapshot translator)
    {
        if (!scope || published.prepareRow) return {};
        const auto identity = published.values ? published.sourceIdentity : scope->sourceIdentity;
        const auto version = published.values ? published.sourceVersion : scope->sourceVersion;
        const auto rows = static_cast<std::uint64_t>(scope->sourceRows.size());
        const auto values = std::make_shared<const ExportValues>(ExportValues{ std::move(scope), std::move(published), includeHeader, appendNewline });
        return MakeCompleteExportInput(values, identity, version, rows, std::move(translator), &serialize);
    }
    void RegisterCompleteExportTableProducer(QTableView* view, CompleteExportTableProducer factory)
    {
        if (!view) return;
        auto* producer = namedChild<Producer>(view, kProducerName);
        if (!producer) producer = new Producer(view);
        producer->factory = std::move(factory);
    }
    void CopyCompleteTableRows(QTableView* view, CompleteTableScope scope, CompleteTableColumns columns)
    { if (auto* value = controller(view)) value->start(scope, false, {}, columns); }
    void ExportCompleteTableRows(QTableView* view, CompleteTableScope scope, const QString& prefix, CompleteTableColumns columns)
    { if (auto* value = controller(view)) value->start(scope, true, prefix, columns); }
    void CopyCompleteTreeRows(QTreeView* view)
    { if (auto* value = controller(view)) value->start(CompleteTableScope::Selected, false, {}); }
}
