#pragma once

#include "StructuredFieldView.h"
#include <QAbstractItemModel>
#include <QSet>
#include <atomic>
#include <memory>

namespace ks::ui
{
    enum FieldTreeRole
    {
        FieldTreeKindRole = Qt::UserRole + 176,
        FieldTreeKeyRole, FieldTreeRowRole, FieldTreeValueRole,
        FieldTreeInitiallyExpandedRole
    };

    // Immutable values only. Preparation, comparison and search may run on a worker.
    // The canonical document is implicitly shared; no widget/model is retained here.
    struct FieldTreeProjection
    {
        struct Entry
        {
            int parent = -1;
            int row = 0;
            FieldNode::Kind kind = FieldNode::Kind::Field;
            QString key, name, value, text;
            QVector<int> accepted;
            bool initiallyExpanded = false;
        };
        struct Change { int parent = -1, first = 0, last = 0; };
        FieldDocument document;
        QVector<Entry> entries;
        QVector<int> acceptedRoots;
        QHash<QString, int> keys;
        QString query;
        QString nameHeader, valueHeader;
        std::weak_ptr<const FieldTreeProjection> base;
        QVector<Change> changes;
        bool sameStructure = false;
    };

    std::shared_ptr<const FieldTreeProjection> PrepareFieldTreeProjection(
        const FieldDocument& document, const std::function<QString(const QString&)>& translate,
        const QString& query, const std::shared_ptr<const FieldTreeProjection>& previous = {},
        const std::shared_ptr<std::atomic_bool>& cancelled = {});

    // Selection belongs to canonical identities, independent of paging/filtering.
    // Bulk selection stays compact: an immutable basis plus explicit exceptions.
    // This is a pure value type and may be copied/pruned on the preparation worker.
    class FieldTreeSelection final
    {
    public:
        bool contains(const QString& key) const;
        void setSelected(const QString& key, bool selected);
        void selectAll(const std::shared_ptr<const FieldTreeProjection>& basis,
            const QHash<QString, bool>& expansion, bool searching);
        void clear();
        void retain(const FieldTreeProjection& current);
        QString text(const FieldTreeProjection& current) const;
    private:
        bool basisContains(const QString& key) const;
        QSet<QString> m_explicit;
        QSet<QString> m_excluded;
        std::shared_ptr<const FieldTreeProjection> m_basis;
        QHash<QString, bool> m_expansion;
        bool m_searching = false;
    };

    // A paged read-only projection. Qt only lays out exposed rows; fetchMore keeps
    // the complete canonical source reachable without preallocating item objects.
    class FieldTreeModel final : public QAbstractItemModel
    {
    public:
        explicit FieldTreeModel(QObject* parent = nullptr);
        std::shared_ptr<const FieldTreeProjection> snapshot() const { return m_snapshot; }
        std::shared_ptr<const FieldTreeProjection> takeSnapshotForDisposal() { return std::move(m_snapshot); }
        std::shared_ptr<const FieldTreeProjection> applySnapshot(
            std::shared_ptr<const FieldTreeProjection> snapshot);
        QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
        QModelIndex parent(const QModelIndex& child) const override;
        int rowCount(const QModelIndex& parent = {}) const override;
        int columnCount(const QModelIndex& = {}) const override { return 2; }
        bool hasChildren(const QModelIndex& parent = {}) const override;
        bool canFetchMore(const QModelIndex& parent) const override;
        void fetchMore(const QModelIndex& parent) override;
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
        Qt::ItemFlags flags(const QModelIndex& index) const override;
        QModelIndex indexForKey(const QString& key) const;
    private:
        int entryId(const QModelIndex& index) const;
        const QVector<int>& children(int parentId) const;
        QModelIndex entryIndex(int id, int column = 0) const;
        static constexpr int PageSize = 128;
        std::shared_ptr<const FieldTreeProjection> m_snapshot;
        QHash<QString, int> m_beforeSearchLoaded;
        QHash<int, int> m_loaded; // exposed child counts, keyed by canonical preorder identity
    };
}
