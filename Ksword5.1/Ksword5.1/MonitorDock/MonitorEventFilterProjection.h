#pragma once

#include "../UI/AsyncOperation.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TableRowFilterSupport.h"

#include <QDynamicPropertyChangeEvent>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSet>
#include <QTableWidget>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <limits>

namespace ks::monitor
{
    struct EventFilterRow
    {
        QStringList fields;
        QString type;
        quint64 time100ns = 0;
    };

    struct EventFilterSpec
    {
        QStringList patterns;
        std::vector<int> fieldIndexes; // -1 means the full fields joined by one space (WMI).
        QString type;
        bool regex = false, invert = false, timeline = false;
        Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
        quint64 start100ns = 0, end100ns = 0;
        bool operator==(const EventFilterSpec& other) const
        {
            return patterns == other.patterns && fieldIndexes == other.fieldIndexes && type == other.type
                && regex == other.regex && invert == other.invert && timeline == other.timeline
                && sensitivity == other.sensitivity && start100ns == other.start100ns && end100ns == other.end100ns;
        }
        bool matchesAll() const
        {
            if (invert || timeline || !type.isEmpty()) return false;
            for (const auto& pattern : patterns) if (!pattern.trimmed().isEmpty()) return false;
            return true;
        }
    };

    // One immutable condition revision. The first value worker compiles each nonempty regex
    // once, and all later append batches share it. No regex compilation occurs on the GUI.
    class CompiledEventFilter final
    {
    public:
        explicit CompiledEventFilter(EventFilterSpec spec) : spec(std::move(spec)) {}
        const EventFilterSpec spec;
        mutable std::atomic<unsigned> compilationCount{0};

        bool matches(const EventFilterRow& row) const
        {
            std::call_once(m_once, [this]() {
                m_regexes.resize(static_cast<std::size_t>(spec.patterns.size()));
                if (!spec.regex) return;
                const auto options = spec.sensitivity == Qt::CaseInsensitive
                    ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption;
                for (qsizetype i = 0; i < spec.patterns.size(); ++i)
                {
                    if (spec.patterns[i].trimmed().isEmpty()) continue;
                    auto& expression = m_regexes[static_cast<std::size_t>(i)];
                    expression = QRegularExpression(spec.patterns[i], options);
                    expression.optimize();
                    compilationCount.fetch_add(1);
                }
            });
            bool matched = spec.type.isEmpty() || row.type.compare(spec.type, Qt::CaseInsensitive) == 0;
            for (qsizetype i = 0; matched && i < spec.patterns.size(); ++i)
            {
                const auto& pattern = spec.patterns[i];
                if (pattern.trimmed().isEmpty()) continue;
                const int field = static_cast<std::size_t>(i) < spec.fieldIndexes.size() ? spec.fieldIndexes[static_cast<std::size_t>(i)] : (std::numeric_limits<int>::max)();
                const QString text = field < 0 ? row.fields.join(QChar(' ')) : row.fields.value(field);
                if (spec.regex)
                {
                    const auto& expression = m_regexes[static_cast<std::size_t>(i)];
                    matched = expression.isValid() && expression.match(text).hasMatch();
                }
                else matched = text.contains(pattern, spec.sensitivity);
            }
            if (spec.invert) matched = !matched;
            // Time selection remains outside inversion, including unknown/zero timestamps.
            return matched && (!spec.timeline || (row.time100ns >= spec.start100ns && row.time100ns <= spec.end100ns));
        }

    private:
        mutable std::once_flag m_once;
        mutable std::vector<QRegularExpression> m_regexes;
    };

    // A display projection only: accepted monitor/archive events retain their original owner.
    // GUI caches immutable field values once per row; workers never receive QModelIndex/QObject.
    class EventFilterProjection final : public QObject
    {
    public:
        using Resolver = std::function<EventFilterRow(int)>;
        using Status = std::function<void(int, int, bool)>;

        EventFilterProjection(QTableWidget* table, Resolver resolver, Status status)
            : QObject(table), m_table(table), m_resolver(std::move(resolver)), m_status(std::move(status))
            , m_operation(new ks::ui::AsyncOperation(this))
        {
            // This local controller is installed only on the three event tables
            // whose complete business hide/reset/clear paths use canonical identities.
            ks::ui::SetTableRowFilterBusinessOwnership(table,
                ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
            m_suspended = !table->isVisible();
            m_timer.setSingleShot(true); m_timer.setInterval(16);
            connect(&m_timer, &QTimer::timeout, this, [this]() { launch(); });
            m_operation->setStateChangedCallback([this]() {
                if (!m_closed && !m_suspended && !m_operation->isBusy() && !m_operation->isInFlight()
                    && m_filter && !m_filter->spec.matchesAll() && (m_allDirty || !m_pending.isEmpty())) schedule();
                notify();
            });
            table->installEventFilter(this);
            connect(table->model(), &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex& parent, int first, int last) {
                    if (parent != m_table->rootIndex()) return;
                    for (int row = first; row <= last; ++row)
                    {
                        const QPersistentModelIndex index(m_table->model()->index(row, 0, parent));
                        const auto found = m_ids.find(index);
                        if (found == m_ids.end()) continue;
                        m_records.erase(found.value()); m_versions.erase(found.value()); m_pending.remove(found.value());
                        m_indexes.erase(found.value()); m_excluded.remove(index); m_ids.erase(found);
                    }
                });
            connect(table->model(), &QAbstractItemModel::rowsRemoved, this, [this]() { notify(); });
            connect(table->model(), &QAbstractItemModel::modelReset, this, [this]() {
                ++m_revision; m_timer.stop(); m_ids.clear(); m_indexes.clear(); m_records.clear(); m_versions.clear();
                m_pending.clear(); m_excluded.clear(); m_allDirty = false; m_operation->cancel(); notify();
            });
            connect(table->model(), &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
                    if (!roles.isEmpty() && !roles.contains(Qt::DisplayRole) && !roles.contains(Qt::EditRole)) return;
                    for (int row = first.row(); row <= last.row(); ++row)
                    {
                        const QPersistentModelIndex index(m_table->model()->index(row, 0, m_table->rootIndex()));
                        if (m_ids.contains(index)) trackRow(row);
                    }
                });
            for (int row = 0; row < table->rowCount(); ++row) trackRow(row);
        }

        ~EventFilterProjection() override { close(); }
        void close()
        {
            if (m_closed) return;
            m_closed = true; ++m_revision; m_timer.stop(); m_operation->close();
        }

        // GUI-only publication of a pure immutable condition. A null value means
        // the controller has not yet received a condition and excludes no row.
        std::shared_ptr<const CompiledEventFilter> currentCompiledFilter() const { return m_filter; }

        // The display owner controls this independently from capture/archive.
        // In-flight/deferred old results cannot commit after suspension. Values
        // and the latest condition remain available to canonical export workers.
        void setSuspended(const bool suspended)
        {
            if (m_closed || suspended == m_suspended) return;
            m_suspended = suspended; ++m_revision;
            if (m_suspended)
            {
                m_timer.stop();
                if (m_filter) m_allDirty = true;
                m_operation->cancel();
            }
            else if (m_filter && m_filter->spec.matchesAll())
            {
                m_pending.clear(); m_allDirty = false; m_excluded.clear();
                ks::ui::ApplyTableRowFilterIncrementally(m_table, m_excluded);
            }
            else if (m_filter) { m_allDirty = true; schedule(); }
            notify();
        }
        bool isSuspended() const { return m_suspended; }

        void setFilter(EventFilterSpec spec)
        {
            if (m_closed) return;
            if (m_filter && m_filter->spec == spec) { notify(); return; }
            m_filter = std::make_shared<CompiledEventFilter>(std::move(spec));
            ++m_revision;
            if (m_suspended) { m_allDirty = true; notify(); return; }
            if (m_filter->spec.matchesAll())
            {
                m_operation->cancel(); m_pending.clear(); m_allDirty = false; m_excluded.clear();
                ks::ui::ApplyTableRowFilterIncrementally(m_table, m_excluded);
            }
            else { m_allDirty = true; schedule(); }
            notify();
        }

        void trackRow(const int row)
        {
            if (m_closed || row < 0 || row >= m_table->rowCount()) return;
            const QPersistentModelIndex index(m_table->model()->index(row, 0, m_table->rootIndex()));
            auto found = m_ids.find(index);
            quint64 id = found == m_ids.end() ? ++m_nextId : found.value();
            m_ids.insert(index, id); m_indexes[id] = index;
            m_records[id] = std::make_shared<const EventFilterRow>(m_resolver(row));
            ++m_versions[id];
            if (m_filter && !m_filter->spec.matchesAll()) { m_pending.insert(id); schedule(); }
        }

        bool isPending() const
        {
            return m_allDirty || !m_pending.isEmpty() || m_operation->isBusy()
                || ks::ui::IsTableRowFilterProjectionPending(m_table);
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == m_table && (event->type() == QEvent::Show || event->type() == QEvent::Hide))
                setSuspended(!m_table->isVisible());
            if (event->type() == QEvent::DynamicPropertyChange
                && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == ks::ui::TableRowFilterRevisionProperty) notify();
            return QObject::eventFilter(watched, event);
        }

    private:
        struct Work
        {
            quint64 revision = 0;
            std::shared_ptr<const CompiledEventFilter> filter;
            std::vector<std::tuple<quint64, quint64, std::shared_ptr<const EventFilterRow>>> rows;
        };
        struct Matches { quint64 revision = 0; std::vector<std::tuple<quint64, quint64, bool>> rows; };

        void schedule() { if (!m_closed && !m_suspended && !m_timer.isActive()) m_timer.start(); }
        void notify() { if (m_status) m_status(static_cast<int>(static_cast<int>(m_records.size()) - static_cast<int>(m_excluded.size())), static_cast<int>(m_records.size()), isPending()); }

        void launch()
        {
            if (m_closed || m_suspended || !m_filter || m_filter->spec.matchesAll()) return;
            Work work; work.revision = m_revision; work.filter = m_filter;
            if (m_allDirty)
            {
                work.rows.reserve(m_records.size());
                for (const auto& [id, row] : m_records) { work.rows.emplace_back(id, m_versions[id], row); m_pending.insert(id); }
                m_allDirty = false;
            }
            else
            {
                work.rows.reserve(static_cast<std::size_t>(m_pending.size()));
                for (const auto id : m_pending)
                {
                    const auto found = m_records.find(id);
                    if (found != m_records.end()) work.rows.emplace_back(id, m_versions[id], found->second);
                }
            }
            if (work.rows.empty()) { notify(); return; }
            m_operation->submit<Work, Matches>(std::move(work),
                [](const Work& request, const ks::ui::AsyncOperationToken& token) {
                    Matches result; result.revision = request.revision; result.rows.reserve(request.rows.size());
                    for (const auto& [id, version, row] : request.rows)
                    {
                        if (token.isCanceled()) break;
                        result.rows.emplace_back(id, version, request.filter->matches(*row));
                    }
                    return result;
                },
                [this](const Matches& result, const std::uint64_t ticket) {
                    if (m_closed || m_suspended || result.revision != m_revision) return true;
                    const auto values = std::make_shared<const Matches>(result);
                    auto deferred = [this, ticket, values]() {
                        if (!m_closed && !m_suspended && m_operation->isCurrent(ticket) && values->revision == m_revision) apply(*values);
                        m_operation->completeDeferred(ticket);
                    };
                    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this,
                        QStringLiteral("monitor-event-filter-apply"), {m_table}, deferred)) return false;
                    apply(result); return true;
                },
                [this](std::exception_ptr) { if (!m_closed) { m_allDirty = true; notify(); } });
        }

        void apply(const Matches& result)
        {
            if (m_closed || m_suspended || result.revision != m_revision) return;
            for (const auto& [id, version, matched] : result.rows)
            {
                const auto found = m_indexes.find(id);
                if (found == m_indexes.end() || !found->second.isValid() || m_versions[id] != version) continue;
                if (matched) m_excluded.remove(found->second); else m_excluded.insert(found->second);
                m_pending.remove(id);
            }
            ks::ui::ApplyTableRowFilterIncrementally(m_table, m_excluded);
            if (!m_pending.isEmpty()) schedule();
            notify();
        }

        QTableWidget* m_table;
        Resolver m_resolver;
        Status m_status;
        ks::ui::AsyncOperation* m_operation;
        QTimer m_timer;
        std::shared_ptr<const CompiledEventFilter> m_filter;
        quint64 m_revision = 0, m_nextId = 0;
        bool m_allDirty = false, m_suspended = false, m_closed = false;
        QHash<QPersistentModelIndex, quint64> m_ids;
        std::unordered_map<quint64, QPersistentModelIndex> m_indexes;
        std::unordered_map<quint64, std::shared_ptr<const EventFilterRow>> m_records;
        std::unordered_map<quint64, quint64> m_versions;
        QSet<quint64> m_pending;
        QSet<QPersistentModelIndex> m_excluded;
    };
}
