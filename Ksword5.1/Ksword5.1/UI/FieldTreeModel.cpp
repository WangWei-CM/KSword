#include "FieldTreeModel.h"
#include <QSet>
#include <QPointer>
#include <QStringList>
#include <algorithm>

namespace ks::ui
{
    std::shared_ptr<const FieldTreeProjection> PrepareFieldTreeProjection(
        const FieldDocument& document, const std::function<QString(const QString&)>& translate,
        const QString& query, const std::shared_ptr<const FieldTreeProjection>& previous,
        const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        const auto interrupted = [&] { return cancelled && cancelled->load(std::memory_order_relaxed); };
        if (interrupted()) return {};
        auto result = std::make_shared<FieldTreeProjection>();
        result->document = document;
        result->query = query.trimmed();
        result->base = previous;
        const auto localized = [&](const QString& source, bool enabled)
        { return enabled && translate ? translate(source) : source; };
        result->nameHeader = localized(QStringLiteral("属性"), true);
        result->valueHeader = localized(QStringLiteral("值"), true);
        std::function<void(const QVector<FieldNode>&, int, const QString&, bool)> append;
        append = [&](const QVector<FieldNode>& nodes, int parentId, const QString& prefix, bool parentMatches)
        {
            QHash<QString, int> occurrences;
            for (const FieldNode& node : nodes)
            {
                if (interrupted()) return;
                const QString source = QString::number(static_cast<int>(node.kind)) + QLatin1Char(':')
                    + (node.kind == FieldNode::Kind::Note ? node.value : node.name);
                const int occurrence = occurrences[source]++;
                FieldTreeProjection::Entry entry;
                entry.parent = parentId;
                entry.kind = node.kind;
                entry.key = prefix + QString::number(source.size()) + QLatin1Char(':') + source
                    + QLatin1Char(':') + QString::number(occurrence) + QLatin1Char('/');
                entry.name = localized(node.name, node.translateName);
                entry.value = localized(node.value, node.translateValue || node.kind == FieldNode::Kind::Note);
                entry.initiallyExpanded = node.initiallyExpanded && !node.children.isEmpty();
                if (node.kind == FieldNode::Kind::Section)
                    entry.text = QLatin1Char('[') + entry.name + QLatin1Char(']');
                else if (node.kind == FieldNode::Kind::Note)
                { entry.name = entry.value; entry.text = entry.value; }
                else entry.text = entry.name + QStringLiteral(": ") + entry.value;
                const bool matches = result->query.isEmpty() || parentMatches
                    || entry.text.contains(result->query, Qt::CaseInsensitive);
                const int id = static_cast<int>(result->entries.size());
                result->keys.insert(entry.key, id);
                const QString key = entry.key;
                result->entries.append(std::move(entry));
                append(node.children, id, key, matches);
                if (matches || !result->entries.at(id).accepted.isEmpty())
                {
                    QVector<int>& accepted = parentId < 0 ? result->acceptedRoots : result->entries[parentId].accepted;
                    result->entries[id].row = static_cast<int>(accepted.size());
                    accepted.append(id);
                }
                else result->entries[id].row = -1;
            }
        };
        append(document.nodes, -1, {}, false);
        if (interrupted()) return {};
        result->sameStructure = previous && previous->acceptedRoots == result->acceptedRoots
            && previous->entries.size() == result->entries.size();
        QHash<int, QVector<int>> changes;
        if (result->sameStructure)
        {
            for (int id = 0; id < result->entries.size(); ++id)
            {
                if (interrupted()) return {};
                const auto& before = previous->entries.at(id);
                const auto& after = result->entries.at(id);
                if (before.key != after.key || before.parent != after.parent || before.kind != after.kind
                    || before.accepted != after.accepted)
                { result->sameStructure = false; break; }
                if (after.row >= 0 && (before.name != after.name || before.value != after.value
                    || before.text != after.text || before.initiallyExpanded != after.initiallyExpanded))
                    changes[after.parent].append(after.row);
            }
        }
        if (result->sameStructure)
        {
            for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            {
                QVector<int> rows = it.value();
                std::sort(rows.begin(), rows.end());
                for (int offset = 0; offset < rows.size();)
                {
                    const int first = rows.at(offset);
                    int last = first;
                    while (++offset < rows.size() && rows.at(offset) == last + 1) last = rows.at(offset);
                    result->changes.append({it.key(), first, last});
                }
            }
        }
        return result;
    }

    bool FieldTreeSelection::basisContains(const QString& key) const
    {
        if (!m_basis) return false;
        const int id = m_basis->keys.value(key, -1);
        if (id < 0 || m_basis->entries.at(id).row < 0) return false;
        for (int parent = m_basis->entries.at(id).parent; parent >= 0;
            parent = m_basis->entries.at(parent).parent)
        {
            const auto& entry = m_basis->entries.at(parent);
            if (!m_searching && !m_expansion.value(entry.key, entry.initiallyExpanded)) return false;
        }
        return true;
    }

    bool FieldTreeSelection::contains(const QString& key) const
    { return m_explicit.contains(key) || (!m_excluded.contains(key) && basisContains(key)); }

    void FieldTreeSelection::setSelected(const QString& key, bool selected)
    {
        if (key.isEmpty()) return;
        if (selected)
        {
            m_excluded.remove(key);
            if (!basisContains(key)) m_explicit.insert(key);
        }
        else
        {
            m_explicit.remove(key);
            if (basisContains(key)) m_excluded.insert(key);
        }
    }

    void FieldTreeSelection::selectAll(const std::shared_ptr<const FieldTreeProjection>& basis,
        const QHash<QString, bool>& expansion, bool searching)
    {
        if (!basis || basis->acceptedRoots.isEmpty()) return; // Qt selectAll is a no-op for an empty viewItems range.
        clear();
        m_basis = basis;
        m_expansion = expansion;
        m_searching = searching;
    }

    void FieldTreeSelection::clear()
    {
        m_explicit.clear();
        m_excluded.clear();
        m_basis.reset();
        m_expansion.clear();
        m_searching = false;
    }

    void FieldTreeSelection::retain(const FieldTreeProjection& current)
    {
        for (auto it = m_explicit.begin(); it != m_explicit.end();)
            if (!current.keys.contains(*it)) it = m_explicit.erase(it); else ++it;
        if (!m_basis) return;
        bool remaining = false;
        for (const auto& entry : m_basis->entries)
        {
            if (!basisContains(entry.key)) continue;
            if (!current.keys.contains(entry.key)) m_excluded.insert(entry.key);
            else if (!m_excluded.contains(entry.key)) remaining = true;
        }
        if (!remaining)
        {
            m_basis.reset();
            m_expansion.clear();
            m_excluded.clear();
        }
    }

    QString FieldTreeSelection::text(const FieldTreeProjection& current) const
    {
        QStringList lines;
        for (const auto& entry : current.entries)
            if (contains(entry.key)) lines.append(entry.text);
        return lines.join(QLatin1Char('\n'));
    }

    FieldTreeModel::FieldTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

    int FieldTreeModel::entryId(const QModelIndex& value) const
    {
        if (!value.isValid()) return -1;
        if (value.model() != this || !m_snapshot || value.internalId() == 0
            || value.internalId() > static_cast<quintptr>(m_snapshot->entries.size())) return -2;
        return static_cast<int>(value.internalId() - 1);
    }

    const QVector<int>& FieldTreeModel::children(int parentId) const
    {
        static const QVector<int> empty;
        if (!m_snapshot || parentId < -1 || parentId >= m_snapshot->entries.size()) return empty;
        return parentId < 0 ? m_snapshot->acceptedRoots : m_snapshot->entries.at(parentId).accepted;
    }

    QModelIndex FieldTreeModel::index(int row, int column, const QModelIndex& parentValue) const
    {
        if (row < 0 || column < 0 || column >= 2 || (parentValue.isValid() && parentValue.column() != 0)
            || row >= rowCount(parentValue)) return {};
        const int id = children(entryId(parentValue)).at(row);
        return createIndex(row, column, static_cast<quintptr>(id + 1));
    }

    QModelIndex FieldTreeModel::entryIndex(int id, int column) const
    {
        if (!m_snapshot || id < 0 || id >= m_snapshot->entries.size()) return {};
        const auto& entry = m_snapshot->entries.at(id);
        const int loaded = m_loaded.value(entry.parent, std::min(PageSize, static_cast<int>(children(entry.parent).size())));
        if (entry.row < 0 || entry.row >= loaded
            || (entry.parent >= 0 && !entryIndex(entry.parent).isValid())) return {};
        return createIndex(entry.row, column, static_cast<quintptr>(id + 1));
    }

    QModelIndex FieldTreeModel::parent(const QModelIndex& child) const
    {
        const int id = entryId(child);
        return id >= 0 ? entryIndex(m_snapshot->entries.at(id).parent) : QModelIndex();
    }

    int FieldTreeModel::rowCount(const QModelIndex& parentValue) const
    {
        if (parentValue.isValid() && parentValue.column() != 0) return 0;
        const int id = entryId(parentValue);
        const int available = static_cast<int>(children(id).size());
        return std::min(available, m_loaded.value(id, std::min(PageSize, available)));
    }

    bool FieldTreeModel::hasChildren(const QModelIndex& parentValue) const
    { return (!parentValue.isValid() || parentValue.column() == 0) && !children(entryId(parentValue)).isEmpty(); }

    bool FieldTreeModel::canFetchMore(const QModelIndex& parentValue) const
    {
        return (!parentValue.isValid() || parentValue.column() == 0)
            && rowCount(parentValue) < children(entryId(parentValue)).size();
    }

    void FieldTreeModel::fetchMore(const QModelIndex& parentValue)
    {
        if (!canFetchMore(parentValue)) return;
        const int id = entryId(parentValue);
        const int first = rowCount(parentValue);
        const int next = std::min(first + PageSize, static_cast<int>(children(id).size()));
        const QPointer<FieldTreeModel> alive(this);
        beginInsertRows(parentValue, first, next - 1);
        if (!alive) return;
        m_loaded.insert(id, next);
        endInsertRows();
    }

    QVariant FieldTreeModel::data(const QModelIndex& value, int role) const
    {
        const int id = entryId(value);
        if (id < 0) return {};
        const auto& entry = m_snapshot->entries.at(id);
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole || role == Qt::AccessibleTextRole)
            return value.column() == 0 ? entry.name : entry.kind == FieldNode::Kind::Field ? entry.value : QString();
        if (role == FieldTreeKindRole) return static_cast<int>(entry.kind);
        if (role == FieldTreeKeyRole) return entry.key;
        if (role == FieldTreeRowRole) return entry.text;
        if (role == FieldTreeValueRole) return entry.kind == FieldNode::Kind::Field ? entry.value : entry.name;
        if (role == FieldTreeInitiallyExpandedRole) return entry.initiallyExpanded;
        return {};
    }

    QVariant FieldTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (m_snapshot && orientation == Qt::Horizontal && section >= 0 && section < 2
            && (role == Qt::DisplayRole || role == Qt::AccessibleTextRole))
            return section == 0 ? m_snapshot->nameHeader : m_snapshot->valueHeader;
        return QAbstractItemModel::headerData(section, orientation, role);
    }

    Qt::ItemFlags FieldTreeModel::flags(const QModelIndex& value) const
    { return value.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags; }

    QModelIndex FieldTreeModel::indexForKey(const QString& key) const
    { return m_snapshot ? entryIndex(m_snapshot->keys.value(key, -1)) : QModelIndex(); }

    std::shared_ptr<const FieldTreeProjection> FieldTreeModel::applySnapshot(
        std::shared_ptr<const FieldTreeProjection> snapshotValue)
    {
        auto retired = m_snapshot;
        const bool incremental = snapshotValue && snapshotValue->sameStructure
            && snapshotValue->base.lock() == m_snapshot;
        QHash<QString, int> exposed;
        if (!incremental && m_snapshot)
        {
            for (auto it = m_loaded.cbegin(); it != m_loaded.cend(); ++it)
                exposed.insert(it.key() < 0 ? QString() : m_snapshot->entries.at(it.key()).key, it.value());
            if (snapshotValue && m_snapshot->query.isEmpty() && !snapshotValue->query.isEmpty())
                m_beforeSearchLoaded = exposed;
            else if (snapshotValue && !m_snapshot->query.isEmpty() && snapshotValue->query.isEmpty())
                exposed = m_beforeSearchLoaded;
        }
        const QPointer<FieldTreeModel> alive(this);
        if (!incremental)
        {
            beginResetModel();
            if (!alive) return retired;
        }
        m_snapshot = std::move(snapshotValue);
        if (!incremental)
        {
            m_loaded.clear();
            if (m_snapshot)
            {
                for (auto it = exposed.cbegin(); it != exposed.cend(); ++it)
                {
                    const int id = it.key().isEmpty() ? -1 : m_snapshot->keys.value(it.key(), -2);
                    if (id >= -1) m_loaded.insert(id, std::min(it.value(), static_cast<int>(children(id).size())));
                }
            }
            endResetModel();
        }
        else
        {
            const auto changes = m_snapshot->changes;
            for (const auto& change : changes)
            {
                const QModelIndex parentValue = entryIndex(change.parent);
                if (change.parent >= 0 && !parentValue.isValid()) continue;
                const int last = std::min(change.last, rowCount(parentValue) - 1);
                if (change.first <= last)
                    emit dataChanged(index(change.first, 0, parentValue), index(last, 1, parentValue),
                        {Qt::DisplayRole, Qt::ToolTipRole, Qt::AccessibleTextRole, FieldTreeRowRole, FieldTreeValueRole,
                            FieldTreeInitiallyExpandedRole});
                if (!alive) return retired;
            }
        }
        if (alive && incremental && retired && (retired->nameHeader != m_snapshot->nameHeader
            || retired->valueHeader != m_snapshot->valueHeader)) emit headerDataChanged(Qt::Horizontal, 0, 1);
        return retired; // caller releases potentially large old projection on a worker
    }
}
