#pragma once

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QEvent>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>
#include <utility>

namespace ks::ui
{
    // QtCore-only identity ledger. View access is confined to the supplied GUI
    // callbacks; tests use a row-visibility vector instead of creating widgets.
    class TableRowFilterIdentityState final : public QObject
    {
    public:
        using ReadRow = std::function<bool(int)>;
        using WriteRow = std::function<void(int, bool)>;

        TableRowFilterIdentityState(QAbstractItemModel* model,
            const QModelIndex& root, ReadRow hidden, ReadRow frozen,
            WriteRow write, std::function<void()> changed,
            std::function<QSet<int>()> frozenRows = {}, QObject* parent = nullptr,
            bool exclusiveBusinessOwnership = false)
            : QObject(parent), m_model(model), m_root(root), m_hidden(std::move(hidden)),
              m_frozen(std::move(frozen)), m_write(std::move(write)),
              m_changed(std::move(changed)), m_frozenRows(std::move(frozenRows)),
              m_exclusiveBusinessOwnership(exclusiveBusinessOwnership)
        {
            if (!m_exclusiveBusinessOwnership)
            {
                captureRows(0,count()-1);
                capturePending();
            }
            connect(model, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex& parent, int first, int last) {
                    if (parent == m_root)
                    {
                        if (!m_exclusiveBusinessOwnership) { captureRows(first,last); scheduleCapture(); }
                        notifyChanged();
                    }
                });
            connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() { notifyChanged(); });
            // A first-column removal must not invalidate the business row anchor.
            // Reanchor before Qt invalidates removed cells; normalize again after
            // insert/remove/move so column zero remains the public row identity.
            connect(model, &QAbstractItemModel::columnsAboutToBeRemoved, this,
                [this](const QModelIndex& parent, int first, int last) {
                    if (parent != m_root || first != 0) return;
                    const int replacement = last + 1;
                    if (replacement >= m_model->columnCount(m_root)) return;
                    for (auto* ids : { &m_business, &m_search, &m_legacy, &m_pending, &m_projectionRows })
                    {
                        QSet<QPersistentModelIndex> remapped;
                        for (const auto& id : *ids)
                            if (id.isValid()) remapped.insert(QModelIndex(id).siblingAtColumn(replacement));
                        *ids = std::move(remapped);
                    }
                });
            const auto columnsChanged = [this]() {
                for (auto* ids : { &m_business, &m_search, &m_legacy, &m_pending, &m_projectionRows })
                {
                    QSet<QPersistentModelIndex> normalized;
                    for (const auto& id : *ids)
                        if (id.isValid()) normalized.insert(identity(id));
                    *ids = std::move(normalized);
                }
                notifyChanged();
            };
            connect(model, &QAbstractItemModel::columnsInserted, this, columnsChanged);
            connect(model, &QAbstractItemModel::columnsRemoved, this, columnsChanged);
            connect(model, &QAbstractItemModel::columnsMoved, this, columnsChanged);
            const auto beforeReorder = [this]() {
                capturePending();
                m_oldRows = affectedRows();
            };
            const auto afterReorder = [this]() {
                QSet<int> rows = affectedRows();
                rows.unite(m_oldRows);
                m_oldRows.clear();
                if (m_incrementalBusiness)
                {
                    // Reordering changes logical header positions as well as
                    // persistent identities. Reconcile both sets through the
                    // same bounded queue instead of a synchronous full replay.
                    for (int row : rows)
                    {
                        const auto id = identity(row);
                        if (id.isValid()) m_projectionRows.insert(id);
                    }
                    ++m_projectionEpoch;
                    notifyChanged();
                    scheduleProjection();
                    return;
                }
                const QPointer<TableRowFilterIdentityState> alive(this);
                for (int row : rows) { apply(row); if (!alive) return; }
                notifyChanged();
            };
            connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, beforeReorder);
            connect(model, &QAbstractItemModel::layoutChanged, this, afterReorder);
            connect(model, &QAbstractItemModel::rowsAboutToBeMoved, this, beforeReorder);
            connect(model, &QAbstractItemModel::rowsMoved, this, afterReorder);
            connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
                capturePending();
                m_resetOwnedRows = affectedRows();
                m_resetOwnedRows.unite(m_oldRows);
                m_resetSentinel = identity(0);
                m_resetBaselinePending = true;
                for (const auto& id : m_legacy)
                    if (id.isValid()) m_resetOwnedRows.remove(id.row());
                // A freeze controller may run later in this same aboutReset
                // signal. Cancel canonical exclusions first so its real
                // unfreezeAllRows cannot restore old business/search bits.
                m_business.clear(); m_search.clear(); m_projectionRows.clear();
                ++m_projectionEpoch;
                notifyChanged();
                if (!m_exclusiveBusinessOwnership)
                {
                    // A legacy producer may call setRowHidden(true) immediately
                    // after endResetModel at the same old hidden position. Qt
                    // emits nothing for an already-hidden section, so only an
                    // explicit exclusive-ownership contract permits lazy repair.
                    const QPointer<TableRowFilterIdentityState> alive(this);
                    const auto rows = m_resetOwnedRows;
                    for (int row : rows)
                    {
                        if (row >= 0 && row < count() && m_hidden(row)) writeProjectionRow(row,false);
                        if (!alive) return;
                    }
                    m_resetOwnedRows.clear();
                }
            });
            connect(model, &QAbstractItemModel::modelReset, this, [this]() {
                // Earlier modelReset producers may already have accepted this
                // generation through setBusiness/setSearch. Never clear it.
                prepareResetGeneration(true);
                if (!m_exclusiveBusinessOwnership)
                {
                    // A producer may have consumed the first pending baseline
                    // before another direct legacy reset handler ran. Capture
                    // again after the complete synchronous signal chain.
                    captureRows(0,count()-1);
                    scheduleCapture();
                }
                notifyChanged();
                scheduleProjection();
            });
        }

        quint64 revision() const { return m_revision; }

        bool isBoundTo(QAbstractItemModel* model, const QModelIndex& root) const
        { return m_model == model && m_root == root; }

        bool businessExcludes(const QModelIndex& index) const
        { return m_business.contains(identity(index)); }

        bool excludes(int row) const
        {
            const auto id = identity(row);
            return id.isValid() && (m_business.contains(id) || m_search.contains(id)
                || m_legacy.contains(id));
        }

        bool legacyExcludes(int row)
        { capturePending(); return m_legacy.contains(identity(row)); }

        // The header emits resize before its hidden bit changes. Only queue
        // this explicit row, and ignore writes made by our own projection.
        void rowVisibilityChanged(int row)
        {
            if (m_writingProjection || m_exclusiveBusinessOwnership) return;
            const auto id = identity(row);
            if (!id.isValid()) return;
            m_pending.insert(id);
            notifyChanged();
            scheduleCapture();
        }

        bool projectionPending() const { return !m_projectionRows.isEmpty(); }
        void setExclusiveBusinessOwnership(bool exclusive)
        {
            if (!prepareResetGeneration()) return;
            if (exclusive == m_exclusiveBusinessOwnership) return;
            capturePending();
            if (exclusive)
            {
                // Preserve the current exclusions while the producer switches
                // from legacy bits to its first complete canonical submission.
                m_business.unite(std::exchange(m_legacy, {}));
                m_exclusiveBusinessOwnership = true;
                notifyChanged();
            }
            else
            {
                // Settle our old bits before allowing new legacy writers.
                const QPointer<TableRowFilterIdentityState> alive(this);
                flushProjection();
                if (!alive) return;
                m_exclusiveBusinessOwnership = false;
                captureRows(0,count()-1);
                scheduleCapture();
            }
        }
        void setProjectionDeferrer(std::function<bool(std::function<void()>)> defer)
        { m_deferProjection = std::move(defer); }

        void setBusiness(const QSet<QPersistentModelIndex>& excluded, bool incremental = false)
        {
            if (!prepareResetGeneration()) return;
            capturePending();
            QSet<QPersistentModelIndex> next;
            next.reserve(excluded.size());
            for (const auto& index : excluded)
            {
                const auto id = identity(index);
                if (id.isValid()) next.insert(id);
            }
            m_incrementalBusiness = incremental;
            if (next == m_business)
            {
                if (!incremental) flushProjection();
                return;
            }
            QSet<QPersistentModelIndex> changed = next - m_business;
            changed.unite(m_business - next);
            m_business = std::move(next);
            m_projectionRows.unite(changed);
            ++m_projectionEpoch;
            notifyChanged(); // canonical identity commits before visual projection
            if (incremental) scheduleProjection(); else flushProjection();
        }

        // Producer-computed differences avoid rebuilding/normalizing the full
        // canonical exclusion set on every append or small condition delta.
        // Added exclusions win if both inputs normalize to the same row.
        // The caller validates its source snapshot generation before invoking.
        void setBusinessDelta(const QSet<QPersistentModelIndex>& excluded,
            const QSet<QPersistentModelIndex>& included)
        {
            if (!prepareResetGeneration()) return;
            capturePending();
            m_incrementalBusiness = true;
            QSet<QPersistentModelIndex> added, removed;
            added.reserve(excluded.size()); removed.reserve(included.size());
            for (const auto& index : excluded)
            {
                const auto id = identity(index);
                if (id.isValid()) added.insert(id);
            }
            for (const auto& index : included)
            {
                const auto id = identity(index);
                if (id.isValid() && !added.contains(id)) removed.insert(id);
            }
            bool changed = false;
            for (const auto& id : removed)
            {
                if (m_business.remove(id))
                { m_projectionRows.insert(id); changed = true; }
            }
            for (const auto& id : added)
            {
                if (!m_business.contains(id))
                { m_business.insert(id); m_projectionRows.insert(id); changed = true; }
            }
            if (!changed) return;
            ++m_projectionEpoch;
            notifyChanged(); // complete canonical delta before visibility writes
            scheduleProjection();
        }

        void setSearch(int row, bool excluded)
        {
            if (!prepareResetGeneration()) return;
            capturePending();
            const auto id = identity(row);
            if (!id.isValid() || m_search.contains(id) == excluded) return;
            if (excluded) m_search.insert(id); else m_search.remove(id);
            const QPointer<TableRowFilterIdentityState> alive(this);
            apply(row);
            if (alive) notifyChanged();
        }

        void clearSearch()
        {
            if (!prepareResetGeneration()) return;
            capturePending();
            if (m_search.isEmpty()) return;
            const auto previous = std::exchange(m_search, {});
            const QPointer<TableRowFilterIdentityState> alive(this);
            for (const auto& id : previous)
                if (id.isValid()) { apply(id.row()); if (!alive) return; }
            notifyChanged();
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event && event->type() == QEvent::Show) scheduleProjection();
            return QObject::eventFilter(watched, event);
        }

    private:
        int count() const { return m_model ? m_model->rowCount(m_root) : 0; }
        QPersistentModelIndex identity(int row) const
        {
            return m_model && row >= 0 && row < count()
                ? QPersistentModelIndex(m_model->index(row, 0, m_root)) : QPersistentModelIndex();
        }
        QPersistentModelIndex identity(QModelIndex index) const
        {
            if (!index.isValid() || index.model() != m_model) return {};
            while (index.isValid() && index.parent() != m_root) index = index.parent();
            return index.isValid() ? QPersistentModelIndex(index.siblingAtColumn(0))
                : QPersistentModelIndex();
        }
        bool prepareResetGeneration(bool resetSignal = false)
        {
            if (!m_resetBaselinePending) return true;
            // Persistent indexes are invalidated before modelReset is emitted.
            // Reject mutations still running in the closing aboutReset phase.
            // For an originally empty model, a valid new cell identifies an
            // early modelReset producer; the final slot also handles empty data.
            if (!resetSignal && (m_resetSentinel.isValid() || !identity(0).isValid())) return false;
            m_resetBaselinePending = false;
            m_resetSentinel = {};
            m_legacy.clear(); m_pending.clear(); m_oldRows.clear();
            for (int row : m_resetOwnedRows)
            {
                const auto id = identity(row);
                if (id.isValid()) m_projectionRows.insert(id);
            }
            m_resetOwnedRows.clear();
            if (!m_exclusiveBusinessOwnership) { captureRows(0,count()-1); scheduleCapture(); }
            notifyChanged();
            scheduleProjection();
            return true;
        }
        void captureRows(int first, int last)
        { for (int row = first; row <= last; ++row) m_pending.insert(identity(row)); }
        void capturePending()
        {
            if (!prepareResetGeneration()) return;
            const auto pending = std::exchange(m_pending, {});
            bool changed = false;
            for (const auto& id : pending)
            {
                if (!id.isValid()) continue;
                if (!m_hidden(id.row())) { changed |= m_legacy.remove(id); continue; }
                if (!m_frozen(id.row()) && !m_business.contains(id) && !m_search.contains(id)
                    && !m_projectionRows.contains(id) && !m_legacy.contains(id))
                { m_legacy.insert(id); changed = true; }
            }
            if (changed) notifyChanged();
        }
        void scheduleCapture()
        {
            if (m_capturePending) return;
            m_capturePending = true;
            QTimer::singleShot(0, this, [this]() { m_capturePending = false; capturePending(); });
        }
        QSet<int> affectedRows() const
        {
            QSet<int> rows = m_frozenRows ? m_frozenRows() : QSet<int>();
            for (const auto* ids : { &m_business, &m_search, &m_legacy, &m_projectionRows })
                for (const auto& id : *ids) if (id.isValid()) rows.insert(id.row());
            return rows;
        }
        void writeProjectionRow(int row, bool hidden)
        {
            const QPointer<TableRowFilterIdentityState> alive(this);
            const bool previous = m_writingProjection;
            m_writingProjection = true;
            m_write(row,hidden);
            if (alive) m_writingProjection = previous;
        }
        void apply(int row)
        {
            if (row < 0 || row >= count()) return;
            const bool hidden = excludes(row) || m_frozen(row);
            if (m_hidden(row) != hidden) writeProjectionRow(row,hidden);
        }
        void flushProjection()
        {
            const bool hadPending = projectionPending();
            const QPointer<TableRowFilterIdentityState> alive(this);
            while (!m_projectionRows.isEmpty())
            {
                auto it = m_projectionRows.begin();
                const QPersistentModelIndex id = *it;
                m_projectionRows.erase(it);
                if (id.isValid()) { apply(id.row()); if (!alive) return; }
            }
            if (hadPending) notifyChanged();
        }
        void scheduleProjection()
        {
            if (m_projectionScheduled || !projectionPending()) return;
            m_projectionScheduled = true;
            const quint64 epoch = m_projectionEpoch;
            QTimer::singleShot(0, this, [this, epoch]() {
                m_projectionScheduled = false;
                if (epoch != m_projectionEpoch) { scheduleProjection(); return; }
                const QPointer<TableRowFilterIdentityState> alive(this);
                if (m_deferProjection && m_deferProjection([alive]() {
                    if (alive) alive->scheduleProjection();
                })) return;
                if (!alive) return;
                QElapsedTimer budget;
                budget.start();
                int work = 0;
                while (!m_projectionRows.isEmpty() && work < 256 && budget.elapsed() < 4)
                {
                    auto it = m_projectionRows.begin();
                    const QPersistentModelIndex id = *it;
                    m_projectionRows.erase(it);
                    ++work;
                    if (id.isValid()) { apply(id.row()); if (!alive) return; }
                    if (epoch != m_projectionEpoch) { scheduleProjection(); return; }
                }
                if (projectionPending()) scheduleProjection(); else notifyChanged();
            });
        }
        void notifyChanged()
        {
            ++m_revision;
            if (m_notificationPending) return;
            m_notificationPending = true;
            QTimer::singleShot(0, this, [this]() {
                m_notificationPending = false;
                m_changed();
            });
        }

        QPointer<QAbstractItemModel> m_model;
        QPersistentModelIndex m_root;
        ReadRow m_hidden, m_frozen;
        WriteRow m_write;
        std::function<void()> m_changed;
        std::function<QSet<int>()> m_frozenRows;
        QSet<QPersistentModelIndex> m_business, m_search, m_legacy, m_pending;
        QSet<int> m_oldRows, m_resetOwnedRows;
        bool m_exclusiveBusinessOwnership = false;
        bool m_writingProjection = false;
        bool m_resetBaselinePending = false;
        QPersistentModelIndex m_resetSentinel;
        QSet<QPersistentModelIndex> m_projectionRows;
        std::function<bool(std::function<void()>)> m_deferProjection;
        quint64 m_projectionEpoch = 0;
        bool m_projectionScheduled = false;
        bool m_incrementalBusiness = false;
        quint64 m_revision = 0;
        bool m_capturePending = false;
        bool m_notificationPending = false;
    };
}
