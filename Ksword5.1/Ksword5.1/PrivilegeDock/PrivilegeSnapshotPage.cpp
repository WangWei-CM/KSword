#include "PrivilegeSnapshotPage.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "PrivilegeSnapshotModel.h"
#include "PrivilegeAccountPages.h"
#include "PrivilegeTokenPages.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ValueTableSelection.h"
#include "../UI/TableRowFilterSupport.h"
#include "../theme.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QEvent>
#include <QShowEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QTimer>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPromise>
#include <QPushButton>
#include <QPointer>
#include <QRunnable>
#include <QSaveFile>
#include <QSpinBox>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <climits>
#include <functional>
#include <tuple>
#include <memory>

#include <Windows.h>

namespace ks::privilege
{
    namespace
    {
        QString snapshotText(const QString& source) { return ks::i18n::sourceText(source); }

        QJsonObject capture(DWORD pid, bool includeToken, const ks::i18n::SourceTextSnapshot& translator)
        {
            wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
            DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
            const bool knownComputer = GetComputerNameW(computer, &length) != FALSE;
            const DWORD computerError = knownComputer ? ERROR_SUCCESS : GetLastError();
            QJsonObject policy = captureAccountPolicySnapshot(&translator);
            if (!knownComputer)
            {
                QJsonArray errors = policy.value(QStringLiteral("errors")).toArray();
                errors.append(QStringLiteral("GetComputerNameW: %1").arg(computerError));
                policy.insert(QStringLiteral("errors"), errors);
            }
            QJsonObject token{{QStringLiteral("entries"), QJsonArray{}}, {QStringLiteral("errors"), QJsonArray{}}};
            if (includeToken) token = captureTokenSnapshot(pid);
            return QJsonObject{
                {QStringLiteral("schema"), QStringLiteral("ksword.permission.snapshot")},
                {QStringLiteral("version"), 1},
                {QStringLiteral("computer"), knownComputer ? QString::fromWCharArray(computer) : QStringLiteral("unknown")},
                {QStringLiteral("capturedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                {QStringLiteral("tokenIncluded"), includeToken},
                {QStringLiteral("policy"), policy}, {QStringLiteral("token"), token}};
        }

        using Prepared = std::shared_ptr<const PreparedPermissionSnapshot>;
        using Differences = std::shared_ptr<const QVector<SnapshotDifference>>;

        template<typename... Values>
        void retireValues(Values... values)
        {
            // Destruction of a previous 200k-entry snapshot/index belongs off the GUI thread too.
            auto retired = std::make_tuple(std::move(values)...);
            QThreadPool::globalInstance()->start(QRunnable::create([retired = std::move(retired)] {}));
        }

        QString kindSource(SnapshotDifference::Kind kind)
        {
            switch (kind)
            {
            case SnapshotDifference::Kind::Added: return QStringLiteral("新增");
            case SnapshotDifference::Kind::Removed: return QStringLiteral("移除");
            case SnapshotDifference::Kind::Modified: return QStringLiteral("修改");
            case SnapshotDifference::Kind::Enabled: return QStringLiteral("启用");
            case SnapshotDifference::Kind::Disabled: return QStringLiteral("禁用");
            case SnapshotDifference::Kind::Uncertain: return QStringLiteral("无法确认");
            }
            return {};
        }

        struct DifferenceProjection
        {
            Differences source;
            QVector<int> rows;
            int currentRow = -1;
            ks::ui::ValueTableSelection::Keys keys, allKeys;
            ks::ui::ValueTableSelection::Selected selected;
            QVector<ks::ui::ValueTableSelection::Range> selectedRanges;
        };
        using Projection = std::shared_ptr<const DifferenceProjection>;

        Projection projectDifferences(const Differences& source, const QString& query,
            const QStringList& kinds, const QString& currentKey, const ks::ui::ValueTableSelection& selection,
            int sortColumn, Qt::SortOrder sortOrder, const std::function<bool()>& cancelled)
        {
            auto projection = std::make_shared<DifferenceProjection>();
            projection->source = source;
            projection->selected = selection.resolve(cancelled);
            if (!projection->selected) return {};
            auto keys = std::make_shared<QVector<QString>>();
            auto allKeys = std::make_shared<QVector<QString>>();
            if (source) for (int row = 0; row < source->size(); ++row)
            {
                if ((row & 127) == 0 && cancelled()) return {};
                const auto& change = (*source)[row];
                allKeys->push_back(change.key);
                if (query.isEmpty() || kinds.value(static_cast<int>(change.kind)).contains(query, Qt::CaseInsensitive)
                    || change.key.contains(query, Qt::CaseInsensitive) || change.before.contains(query, Qt::CaseInsensitive)
                    || change.after.contains(query, Qt::CaseInsensitive)) projection->rows.push_back(row);
            }
            if (source && sortColumn >= 0 && sortColumn < 4)
            {
                const auto text = [&source, &kinds, sortColumn](int row) -> QString {
                    const auto& value = (*source)[row];
                    switch (sortColumn)
                    {
                    case 0: return kinds.value(static_cast<int>(value.kind));
                    case 1: return value.key;
                    case 2: return value.before;
                    default: return value.after;
                    }
                };
                std::stable_sort(projection->rows.begin(), projection->rows.end(), [&text, sortOrder](int left, int right) {
                    const int result = QString::localeAwareCompare(text(left), text(right));
                    return sortOrder == Qt::AscendingOrder ? result < 0 : result > 0;
                });
            }
            if (cancelled()) return {};
            for (int row = 0; row < projection->rows.size(); ++row)
            {
                if ((row & 127) == 0 && cancelled()) return {};
                const auto& change = (*source)[projection->rows[row]];
                if (change.key == currentKey) projection->currentRow = row;
                if (projection->selected->contains(change.key))
                {
                    auto& ranges = projection->selectedRanges;
                    if (!ranges.isEmpty() && ranges.last().last + 1 == row) ranges.last().last = row;
                    else ranges.push_back({row, row});
                }
                keys->push_back(change.key);
            }
            projection->keys = std::move(keys);
            projection->allKeys = std::move(allKeys);
            return projection;
        }

        class DifferenceModel final : public QAbstractTableModel
        {
        public:
            explicit DifferenceModel(QObject* parent) : QAbstractTableModel(parent) {}
            std::function<void(int, Qt::SortOrder)> sortRequested;
            void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override
            { if (sortRequested) { const auto callback = sortRequested; callback(column, order); } }
            ~DifferenceModel() override { retireValues(std::move(m_projection)); }
            void setProjection(Projection projection)
            {
                const QPointer<DifferenceModel> alive(this);
                beginResetModel();
                if (!alive) { retireValues(std::move(projection)); return; }
                auto retired = std::move(m_projection);
                m_projection = std::move(projection);
                endResetModel();
                // endResetModel may synchronously close the owning page/model.
                retireValues(std::move(retired));
            }
            int rowCount(const QModelIndex& parent = {}) const override
            { return parent.isValid() || !m_projection ? 0 : static_cast<int>(m_projection->rows.size()); }
            int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 4; }
            const SnapshotDifference* rowAt(int row) const
            {
                if (!m_projection || row < 0 || row >= m_projection->rows.size()) return nullptr;
                return &(*m_projection->source)[m_projection->rows[row]];
            }
            QVariant data(const QModelIndex& index, int role) const override
            {
                const auto* row = rowAt(index.row());
                if (!index.isValid() || !row || index.column() < 0 || index.column() >= 4
                    || (role != Qt::DisplayRole && role != Qt::ToolTipRole)) return {};
                switch (index.column())
                {
                case 0: return snapshotText(kindSource(row->kind));
                case 1: return row->key;
                case 2: return row->before;
                case 3: return row->after;
                }
                return {};
            }
            QVariant headerData(int section, Qt::Orientation orientation, int role) const override
            {
                if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
                const QStringList headers{QStringLiteral("变化"), QStringLiteral("条目"), QStringLiteral("原值"), QStringLiteral("现值")};
                return section >= 0 && section < headers.size() ? snapshotText(headers[section]) : QVariant{};
            }
            void languageChanged() { emit headerDataChanged(Qt::Horizontal, 0, 3); }
        private:
            Projection m_projection;
        };

        Prepared loadPreparedSnapshot(const QString& path, QString* error)
        {
            QFile file(path);
            constexpr qint64 maximumBytes = 64 * 1024 * 1024;
            if (!file.open(QIODevice::ReadOnly) || file.size() > maximumBytes)
            { *error = QStringLiteral("快照无法读取或超过 64 MiB。"); return {}; }
            const auto bytes = file.read(maximumBytes + 1);
            if (bytes.size() > maximumBytes || file.error() != QFileDevice::NoError)
            { *error = QStringLiteral("快照无法读取或超过 64 MiB。"); return {}; }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
            { *error = QStringLiteral("快照 JSON 格式无效。"); return {}; }
            return preparePermissionSnapshot(document.object(), error);
        }

        struct SnapshotResult
        {
            Prepared baseline, current;
            Differences differences;
            QString error, diagnostics;
            bool accepted = false;
        };
        using OperationResult = std::shared_ptr<const SnapshotResult>;

        OperationResult prepareSnapshotResult(const std::function<Prepared(QString*)>& producer, bool baseline,
            const Prepared& oldBaseline, const Prepared& oldCurrent)
        {
            auto result = std::make_shared<SnapshotResult>();
            try
            {
                auto prepared = producer(&result->error);
                if (prepared)
                {
                    result->accepted = true;
                    result->baseline = baseline ? std::move(prepared) : oldBaseline;
                    result->current = baseline ? oldCurrent : std::move(prepared);
                    auto changes = std::make_shared<QVector<SnapshotDifference>>();
                    if (result->baseline && result->current)
                        *changes = comparePreparedPermissionSnapshots(*result->baseline, *result->current, &result->error);
                    result->differences = std::move(changes);
                    QStringList diagnostics;
                    if (result->baseline) diagnostics += result->baseline->diagnostics();
                    if (result->current) diagnostics += result->current->diagnostics();
                    diagnostics.removeDuplicates();
                    result->diagnostics = diagnostics.join(QLatin1Char('\n'));
                }
            }
            catch (const std::exception& exception)
            { result->error = QStringLiteral("权限快照采集失败：%1").arg(QString::fromUtf8(exception.what())); }
            catch (...) { result->error = QStringLiteral("快照 JSON 格式无效。"); }
            return result;
        }

        QString savePreparedSnapshot(const Prepared& frozen, const QString& path)
        {
            QString error;
            try
            {
                const auto bytes = QJsonDocument(frozen->document()).toJson(QJsonDocument::Indented);
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                    error = file.errorString();
            }
            catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
            catch (...) { error = QStringLiteral("快照 JSON 格式无效。"); }
            return error;
        }

        class DifferenceTable final : public ks::ui::TableActionTableView
        {
        public:
            using TableActionTableView::TableActionTableView;
            std::function<void()> clearRequested, beforeSelectAll, afterSelectAll;
            void selectAll() override
            {
                if (beforeSelectAll) beforeSelectAll();
                TableActionTableView::selectAll();
                if (afterSelectAll) afterSelectAll();
            }
        protected:
            QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex& index, const QEvent* event = nullptr) const override
            {
                const auto command = TableActionTableView::selectionCommand(index, event);
                if (command.testFlag(QItemSelectionModel::Clear) && clearRequested) clearRequested();
                return command;
            }
        };

        class SnapshotPage final : public QWidget
        {
        public:
            explicit SnapshotPage(QWidget* parent) : QWidget(parent)
            {
                setObjectName(QStringLiteral("privilege_snapshot_page"));
                auto* layout = new QVBoxLayout(this);
                auto* selectors = new QHBoxLayout;
                m_includeToken = new QCheckBox(snapshotText(QStringLiteral("包含所选进程令牌")), this);
                m_includeToken->setChecked(true);
                m_pid = new QSpinBox(this);
                // 快照 PID 是独立参数，尺寸与数值前景一起交给共享输入主题。
                ks::ui::StyleStandaloneInput(m_pid);
                m_pid->setRange(1, INT_MAX);
                m_pid->setValue(static_cast<int>(GetCurrentProcessId()));
                selectors->addWidget(m_includeToken);
                selectors->addWidget(new QLabel(QStringLiteral("PID"), this));
                selectors->addWidget(m_pid);
                selectors->addStretch();
                ks::ui::NormalizeToolbarRow(selectors);
                layout->addLayout(selectors);
                auto* toolbar = new QHBoxLayout;
                addButton(toolbar, snapshotText(QStringLiteral("采集设为基线")), [this] { startCapture(true); });
                addButton(toolbar, snapshotText(QStringLiteral("采集当前并对比")), [this] { startCapture(false); });
                addButton(toolbar, snapshotText(QStringLiteral("加载基线")), [this] { load(true); });
                addButton(toolbar, snapshotText(QStringLiteral("加载当前")), [this] { load(false); });
                toolbar->addStretch();
                ks::ui::NormalizeToolbarRow(toolbar);
                layout->addLayout(toolbar);
                auto* saveBar = new QHBoxLayout;
                addButton(saveBar, snapshotText(QStringLiteral("保存基线")), [this] { save(m_baseline); });
                addButton(saveBar, snapshotText(QStringLiteral("保存当前")), [this] { save(m_current); });
                addButton(saveBar, snapshotText(QStringLiteral("复制当前差异")), [this] { copyCurrentRow(); });
                m_search = new QLineEdit(this);
                m_search->setPlaceholderText(snapshotText(QStringLiteral("筛选条目、原值或现值")));
                ks::ui::StyleSearchField(m_search);
                saveBar->addWidget(m_search, 1);
                ks::ui::NormalizeToolbarRow(saveBar);
                layout->addLayout(saveBar);
                auto* explanation = new QLabel(snapshotText(QStringLiteral("手动采集账号状态、组成员、直接权限分配与令牌字段。采集不完整时，缺失条目标为无法确认；快照仅用于比较。")), this);
                explanation->setWordWrap(true);
                layout->addWidget(explanation);
                m_status = new QLabel(snapshotText(QStringLiteral("尚未设置基线。")), this);
                m_status->setWordWrap(true);
                layout->addWidget(m_status);
                m_table = new DifferenceTable(this);
                m_model = new DifferenceModel(m_table);
                m_table->setModel(m_model);
                ks::i18n::LanguageManager::instance().setRuntimeTranslationManaged(m_table);
                ks::ui::SetTableRowFilterBusinessOwnership(m_table, ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
                m_table->beforeSelectAll = [this] {
                    if (m_applyingProjection || !m_allKeys || m_allKeys->isEmpty()) return;
                    flushSelectionClear();
                    retireValues(std::move(m_selection));
                    m_selection = {};
                    m_selection.append(m_allKeys, {{0, static_cast<int>(m_allKeys->size()) - 1}}, {});
                    m_bulkSelecting = true;
                    m_applyingProjection = true;
                };
                m_table->afterSelectAll = [this] {
                    if (m_bulkSelecting) { m_bulkSelecting = false; m_applyingProjection = false; }
                };
                m_table->clearRequested = [this] {
                    if (m_applyingProjection) return;
                    m_clearSelectionPending = true;
                    QTimer::singleShot(0, this, [this] { flushSelectionClear(); });
                };
                // 页面已有完整基线/当前比较和保存功能，不再叠加通用快照操作栏。
                ks::ui::SetTableActionBarMode(m_table, ks::ui::TableActionBarMode::None);
                m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
                m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
                m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
                m_table->horizontalHeader()->setStretchLastSection(true);
                m_model->sortRequested = [this](int column, Qt::SortOrder order) {
                    if (column < -1 || column >= 4 || (m_sortColumn == column && m_sortOrder == order)) return;
                    m_sortColumn = column;
                    m_sortOrder = order;
                    requestFilter();
                };
                // Keep initial canonical order, then let native header clicks request worker projections.
                m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
                m_table->setSortingEnabled(true);
                layout->addWidget(m_table, 1);
                connect(m_search, &QLineEdit::textChanged, this, [this] { requestFilter(); });
                connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this,
                    [this](const QModelIndex& current) {
                        if (!m_applyingProjection)
                            if (const auto* row = m_model->rowAt(current.row())) { m_currentKey = row->key; m_currentColumn = current.column(); }
                    });
                connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                    [this](const QItemSelection& selected, const QItemSelection& deselected) {
                        if (m_applyingProjection) return;
                        if (m_clearSelectionPending) { flushSelectionClear(); return; }
                        m_selection.append(m_projectedKeys, selectionRanges(selected), selectionRanges(deselected));
                    });
            }

            ~SnapshotPage() override
            {
                ++*m_filterRevision;
                if (m_operationWatcher)
                {
                    disconnect(m_operationWatcher, nullptr, this, nullptr);
                    if (m_operationWatcher->isFinished() && m_operationWatcher->future().resultCount())
                        retireValues(m_operationWatcher->result());
                    m_operationWatcher->setFuture({});
                }
                if (m_filterWatcher)
                {
                    disconnect(m_filterWatcher, nullptr, this, nullptr);
                    if (m_filterWatcher->isFinished() && m_filterWatcher->future().resultCount())
                        retireValues(m_filterWatcher->result());
                    m_filterWatcher->setFuture({});
                }
                retireValues(std::move(m_baseline), std::move(m_current), std::move(m_differences), std::move(m_selection), std::move(m_projectedKeys), std::move(m_allKeys));
            }

        protected:
            void showEvent(QShowEvent* event) override
            {
                const QPointer<SnapshotPage> alive(this);
                QWidget::showEvent(event);
                if (alive && m_filterDirty) launchFilter();
            }
            void changeEvent(QEvent* event) override
            {
                const QPointer<SnapshotPage> changeOwner(this);
                QWidget::changeEvent(event);
                if (!changeOwner) return;
                if (event->type() == QEvent::LanguageChange)
                {
                    const QPointer<SnapshotPage> alive(this);
                    m_model->languageChanged();
                    if (!alive) return;
                    if (!m_busy) updateSummary();
                    if (alive) requestFilter();
                }
            }

        private:
            template<typename Callback>
            void addButton(QHBoxLayout* bar, const QString& text, Callback callback)
            {
                auto* button = new QPushButton(text, this);
                button->setStyleSheet(KswordTheme::ThemedButtonStyle());
                bar->addWidget(button);
                m_buttons.push_back(button);
                connect(button, &QPushButton::clicked, this, callback);
            }

            bool setBusy(bool busy)
            {
                const QPointer<SnapshotPage> alive(this);
                m_busy = busy;
                for (auto* button : m_buttons)
                {
                    button->setEnabled(!busy);
                    if (!alive) return false;
                    if (m_busy != busy) return true; // a newer operation owns enabled state
                }
                m_pid->setEnabled(!busy);
                if (!alive) return false;
                if (m_busy != busy) return true;
                m_includeToken->setEnabled(!busy);
                return !alive.isNull();
            }

            void startCapture(bool baseline)
            {
                if (m_busy) return;
                const DWORD pid = static_cast<DWORD>(m_pid->value());
                const bool includeToken = m_includeToken->isChecked();
                const auto translator = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
                const QPointer<SnapshotPage> alive(this);
                m_status->setText(snapshotText(QStringLiteral("正在采集权限快照…")));
                if (!alive) return;
                startSnapshotOperation([pid, includeToken, translator](QString* error) {
                    return preparePermissionSnapshot(capture(pid, includeToken, translator), error);
                }, baseline);
            }

            void startSnapshotOperation(std::function<Prepared(QString*)> producer, bool baseline)
            {
                if (!setBusy(true)) return;
                const auto oldBaseline = m_baseline, oldCurrent = m_current;
                auto promise = std::make_shared<QPromise<OperationResult>>();
                promise->start();
                auto* watcher = new QFutureWatcher<OperationResult>(this);
                m_operationWatcher = watcher;
                connect(watcher, &QFutureWatcher<OperationResult>::finished, this, [this, watcher] {
                    disconnect(watcher, nullptr, this, nullptr);
                    auto result = watcher->result();
                    watcher->setFuture({});
                    watcher->deleteLater();
                    m_operationWatcher = nullptr;
                    const QPointer<SnapshotPage> completionOwner(this);
                    if (result->accepted)
                    {
                        retireValues(std::move(m_baseline), std::move(m_current), std::move(m_differences), std::move(m_selection), std::move(m_projectedKeys), std::move(m_allKeys));
                        m_baseline = result->baseline;
                        m_current = result->current;
                        m_differences = result->differences;
                        m_comparisonError = result->error;
                        m_diagnostics = result->diagnostics;
                        m_selection = {};
                        m_projectedKeys.reset();
                        m_allKeys.reset();
                        m_currentKey.clear();
                        const QPointer<SnapshotPage> alive(this);
                        const quint64 resetRevision = ++*m_filterRevision;
                        m_restoreRevision = resetRevision;
                        m_applyingProjection = true;
                        // A nested event loop can complete another operation inside reset signals.
                        // Keep canonical acceptance, but serialize all model publications.
                        if (!m_modelPublishing)
                        {
                            m_modelPublishing = true;
                            m_model->setProjection({});
                            if (!alive) { retireValues(std::move(result)); return; }
                            m_modelPublishing = false;
                        }
                        if (m_restoreRevision == resetRevision) m_applyingProjection = false;
                        updateSummary();
                        if (alive) requestFilter();
                    }
                    else m_status->setText(snapshotText(result->error));
                    retireValues(std::move(result));
                    if (completionOwner) setBusy(false);
                });
                watcher->setFuture(promise->future());
                QThreadPool::globalInstance()->start(QRunnable::create([promise, producer = std::move(producer), oldBaseline, oldCurrent, baseline] {
                    promise->addResult(prepareSnapshotResult(producer, baseline, oldBaseline, oldCurrent));
                    promise->finish();
                }));
            }

            void updateSummary()
            {
                QString status;
                if (!m_comparisonError.isEmpty()) status = snapshotText(m_comparisonError);
                else if (!m_baseline) status = snapshotText(QStringLiteral("尚未设置基线。"));
                else if (!m_current) status = snapshotText(QStringLiteral("基线已设置，请采集或加载当前快照。"));
                else status = snapshotText(QStringLiteral("共 %1 项差异。基线：%2；当前：%3"))
                    .arg(m_differences ? m_differences->size() : 0)
                    .arg(m_baseline->document().value(QStringLiteral("capturedUtc")).toString())
                    .arg(m_current->document().value(QStringLiteral("capturedUtc")).toString());
                if (!m_diagnostics.isEmpty() && m_comparisonError.isEmpty()) status += QLatin1Char('\n') + m_diagnostics;
                m_status->setText(status);
            }

            void requestFilter()
            {
                flushSelectionClear();
                ++*m_filterRevision;
                m_filterDirty = true;
                const QPointer<SnapshotPage> alive(this);
                m_table->setEnabled(false);
                if (alive && isVisible()) launchFilter();
            }

            void launchFilter()
            {
                if (m_filterRunning || m_modelPublishing || !m_filterDirty || !isVisible()) return;
                m_filterRunning = true;
                m_filterDirty = false;
                const auto revision = m_filterRevision->load();
                const auto ticket = m_filterRevision;
                const auto source = m_differences;
                const auto query = m_search->text();
                const auto currentKey = m_currentKey;
                const auto selection = m_selection;
                const int sortColumn = m_sortColumn;
                const auto sortOrder = m_sortOrder;
                QStringList kinds;
                for (int kind = 0; kind < 6; ++kind)
                    kinds.push_back(snapshotText(kindSource(static_cast<SnapshotDifference::Kind>(kind))));
                auto promise = std::make_shared<QPromise<Projection>>();
                promise->start();
                auto* watcher = new QFutureWatcher<Projection>(this);
                m_filterWatcher = watcher;
                connect(watcher, &QFutureWatcher<Projection>::finished, this, [this, watcher, revision] {
                    disconnect(watcher, nullptr, this, nullptr);
                    auto projection = watcher->result();
                    watcher->setFuture({});
                    watcher->deleteLater();
                    m_filterWatcher = nullptr;
                    m_filterRunning = false;
                    if (revision == m_filterRevision->load() && projection)
                    {
                        if (isVisible() && !m_modelPublishing)
                        {
                            m_restoreRevision = revision;
                            m_applyingProjection = true;
                            retireValues(std::move(m_selection), std::move(m_projectedKeys), std::move(m_allKeys));
                            m_selection = {};
                            m_selection.setResolved(projection->selected);
                            m_projectedKeys = projection->keys;
                            m_allKeys = projection->allKeys;
                            const QPointer<SnapshotPage> alive(this);
                            m_modelPublishing = true;
                            m_model->setProjection(projection);
                            if (!alive) { retireValues(std::move(projection)); return; }
                            m_modelPublishing = false;
                            if (revision != m_filterRevision->load() || m_restoreRevision != revision)
                            {
                                if (m_restoreRevision == revision) m_applyingProjection = false;
                                retireValues(std::move(projection));
                                if (m_filterDirty && isVisible()) launchFilter();
                                return;
                            }
                            if (projection->currentRow >= 0)
                                m_table->selectionModel()->setCurrentIndex(m_model->index(projection->currentRow, m_currentColumn), QItemSelectionModel::NoUpdate);
                            if (!alive) { retireValues(std::move(projection)); return; }
                            if (revision == m_filterRevision->load() && m_restoreRevision == revision)
                                restoreSelection(projection, revision, 0);
                            else if (m_restoreRevision == revision) m_applyingProjection = false;
                            if (!alive) { retireValues(std::move(projection)); return; }
                        }
                        else m_filterDirty = true;
                    }
                    retireValues(std::move(projection));
                    if (m_filterDirty && isVisible()) launchFilter();
                });
                watcher->setFuture(promise->future());
                QThreadPool::globalInstance()->start(QRunnable::create([promise, source, query, kinds, revision, ticket, currentKey, selection, sortColumn, sortOrder] {
                    promise->addResult(projectDifferences(source, query, kinds, currentKey, selection, sortColumn, sortOrder,
                        [ticket, revision] { return ticket->load() != revision; }));
                    promise->finish();
                }));
            }

            static QVector<ks::ui::ValueTableSelection::Range> selectionRanges(const QItemSelection& selection)
            {
                QVector<ks::ui::ValueTableSelection::Range> ranges;
                for (const auto& range : selection) ranges.push_back({range.top(), range.bottom()});
                return ranges;
            }

            void flushSelectionClear()
            {
                if (!m_clearSelectionPending || m_applyingProjection) return;
                m_clearSelectionPending = false;
                retireValues(std::move(m_selection));
                m_selection = {};
                m_selection.append(m_projectedKeys, selectionRanges(m_table->selectionModel()->selection()), {});
            }

            void restoreSelection(Projection projection, quint64 revision, qsizetype position)
            {
                if (revision != m_filterRevision->load())
                { if (m_restoreRevision == revision) m_applyingProjection = false; return; }
                if (!isVisible())
                {
                    m_applyingProjection = false;
                    ++*m_filterRevision;
                    m_filterDirty = true;
                    return;
                }
                QElapsedTimer budget;
                budget.start();
                QItemSelection selection;
                int work = 0;
                while (position < projection->selectedRanges.size() && work++ < 128 && budget.elapsed() < 4)
                {
                    const auto range = projection->selectedRanges[position++];
                    selection.select(m_model->index(range.first, 0), m_model->index(range.last, 3));
                }
                const QPointer<SnapshotPage> alive(this);
                if (!selection.isEmpty()) m_table->selectionModel()->select(selection, QItemSelectionModel::Select);
                if (!alive) { retireValues(std::move(projection)); return; }
                if (revision != m_filterRevision->load() || m_restoreRevision != revision)
                { if (m_restoreRevision == revision) m_applyingProjection = false; return; }
                if (position < projection->selectedRanges.size())
                    QTimer::singleShot(0, this, [this, projection, revision, position] { restoreSelection(projection, revision, position); });
                else { m_applyingProjection = false; m_table->setEnabled(true); }
            }

            void save(const Prepared& snapshot)
            {
                if (m_busy) return;
                if (!snapshot) { m_status->setText(snapshotText(QStringLiteral("请先采集或加载快照。"))); return; }
                // Freeze document identity before entering a native modal dialog.
                const auto frozen = snapshot;
                const QPointer<QWidget> pageGuard(this);
                const QString path = QFileDialog::getSaveFileName(this, snapshotText(QStringLiteral("保存权限快照")), QString(), QStringLiteral("JSON (*.json)"));
                if (!pageGuard || path.isEmpty() || m_busy) return;
                m_busy = true;
                auto promise = std::make_shared<QPromise<QString>>();
                promise->start();
                auto* watcher = new QFutureWatcher<QString>(this);
                connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path] {
                    const auto error = watcher->result();
                    watcher->deleteLater();
                    const QPointer<SnapshotPage> alive(this);
                    m_status->setText(error.isEmpty() ? snapshotText(QStringLiteral("快照已保存：%1")).arg(path)
                        : snapshotText(QStringLiteral("保存失败：%1")).arg(error));
                    if (alive) setBusy(false);
                });
                watcher->setFuture(promise->future());
                QThreadPool::globalInstance()->start(QRunnable::create([promise, frozen, path] {
                    promise->addResult(savePreparedSnapshot(frozen, path));
                    promise->finish();
                }));
                (void)setBusy(true);
            }

            void load(bool baseline)
            {
                if (m_busy) return;
                const QPointer<QWidget> pageGuard(this);
                const QString path = QFileDialog::getOpenFileName(this, snapshotText(QStringLiteral("加载权限快照")), QString(), QStringLiteral("JSON (*.json)"));
                if (!pageGuard || path.isEmpty() || m_busy) return;
                startSnapshotOperation([path](QString* error) { return loadPreparedSnapshot(path, error); }, baseline);
            }

            void copyCurrentRow()
            {
                if (!m_differences || m_currentKey.isEmpty()) return;
                const auto row = std::lower_bound(m_differences->cbegin(), m_differences->cend(), m_currentKey,
                    [](const SnapshotDifference& value, const QString& key) { return value.key < key; });
                if (row == m_differences->cend() || row->key != m_currentKey) return;
                QApplication::clipboard()->setText(QStringList{snapshotText(kindSource(row->kind)), row->key, row->before, row->after}.join(QLatin1Char('\t')));
            }

            Prepared m_baseline, m_current;
            Differences m_differences;
            QString m_comparisonError, m_diagnostics, m_currentKey;
            std::shared_ptr<std::atomic<quint64>> m_filterRevision = std::make_shared<std::atomic<quint64>>(0);
            QPointer<QFutureWatcher<OperationResult>> m_operationWatcher;
            QPointer<QFutureWatcher<Projection>> m_filterWatcher;
            bool m_busy = false, m_filterRunning = false, m_filterDirty = false, m_applyingProjection = false, m_modelPublishing = false;
            QVector<QPushButton*> m_buttons;
            QCheckBox* m_includeToken = nullptr;
            QSpinBox* m_pid = nullptr;
            QLineEdit* m_search = nullptr;
            QLabel* m_status = nullptr;
            ks::ui::ValueTableSelection m_selection;
            ks::ui::ValueTableSelection::Keys m_projectedKeys, m_allKeys;
            quint64 m_restoreRevision = 0;
            int m_currentColumn = 0;
            int m_sortColumn = -1;
            Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
            bool m_clearSelectionPending = false, m_bulkSelecting = false;
            DifferenceTable* m_table = nullptr;
            DifferenceModel* m_model = nullptr;
        };
    }

    QWidget* createPermissionSnapshotPage(QWidget* parent) { return new SnapshotPage(parent); }
}
