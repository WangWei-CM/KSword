#pragma once

#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QCollator>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QRunnable>
#include <QThreadPool>
#include <QVariant>
#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>

namespace ks::fileui
{
    // Complete display evidence is prepared by the provider, never by GUI item allocation.
    struct PropertyTableRow
    {
        QString key;
        QStringList columns;
        QHash<int, QVariant> roles; // Exact identity roles belong to column zero.
        QSet<int> translatedColumns; // Only provider-declared status text; paths and raw data stay exact.
    };
    struct PropertyTableSnapshot
    {
        QVector<PropertyTableRow> rows;
        QSet<QString> translationKeys; // Only finite provider status enums are registered here.
        void append(PropertyTableRow row)
        {
            for (int column : row.translatedColumns) translationKeys.insert(row.columns.value(column));
            rows.push_back(std::move(row));
        }
    };
    inline std::shared_ptr<PropertyTableSnapshot> makePropertyTableSnapshot()
    {
        return { new PropertyTableSnapshot(), [](PropertyTableSnapshot* rows)
            { QThreadPool::globalInstance()->start(QRunnable::create([rows]() { delete rows; })); } };
    }
    inline QString propertyRowKey(const QStringList& parts)
    {
        QString result;
        for (const QString& part : parts)
            result += QString::number(part.size()) + QLatin1Char(':') + part;
        return result;
    }
    struct PropertyTableOrder
    {
        QVector<int> rows;
        QHash<QString, int> indexByKey;
    };
    inline std::shared_ptr<PropertyTableOrder> orderPropertyTable(
        const PropertyTableSnapshot& snapshot, int column, Qt::SortOrder order,
        const std::shared_ptr<std::atomic_bool>& cancel, const QHash<QString, QString>& translations = {})
    {
        auto result = std::shared_ptr<PropertyTableOrder>(new PropertyTableOrder(), [](PropertyTableOrder* value)
            { QThreadPool::globalInstance()->start(QRunnable::create([value]() { delete value; })); });
        result->rows.resize(snapshot.rows.size());
        std::iota(result->rows.begin(), result->rows.end(), 0);
        struct Cancelled {};
        qsizetype comparisons = 0;
        const QCollator collator;
        try
        {
            if (cancel->load()) return {};
            if (column >= 0)
                std::stable_sort(result->rows.begin(), result->rows.end(), [&](int left, int right)
                    {
                        if (((++comparisons) % 256) == 0 && cancel->load()) throw Cancelled{};
                        const auto value = [&](int rowIndex)
                            {
                                const auto& row = snapshot.rows.at(rowIndex);
                                const QString raw = row.columns.value(column);
                                return row.translatedColumns.contains(column) ? translations.value(raw, raw) : raw;
                            };
                        const int comparison = collator.compare(value(left), value(right));
                        return order == Qt::AscendingOrder ? comparison < 0 : comparison > 0;
                    });
        }
        catch (const Cancelled&) { return {}; }
        if (cancel->load()) return {};
        result->indexByKey.reserve(result->rows.size());
        for (int index = 0; index < result->rows.size(); ++index)
        {
            if ((index % 256) == 0 && cancel->load()) return {};
            const QString& key = snapshot.rows.at(result->rows.at(index)).key;
            if (!result->indexByKey.contains(key)) result->indexByKey.insert(key, index);
        }
        return result;
    }

    class PropertyTableModel final : public QAbstractTableModel
    {
    public:
        static constexpr int StableKeyRole = Qt::UserRole + 917;
        using CommitGate = std::function<void(std::function<void()>)>;
        explicit PropertyTableModel(QStringList headers, QObject* parent,
            std::function<QString(const QString&)> translate = {})
            : QAbstractTableModel(parent), m_headers(std::move(headers)), m_translate(std::move(translate))
        {
            if (parent != nullptr) parent->installEventFilter(this);
        }
        ~PropertyTableModel() override { if (m_cancel) m_cancel->store(true); }
        void setCommitGate(CommitGate gate) { m_gate = std::move(gate); }
        void setSnapshot(std::shared_ptr<const PropertyTableSnapshot> snapshot, std::function<void()> installed = {})
        {
            m_requested = std::move(snapshot);
            refreshTranslations();
            m_installed = std::move(installed);
            requestOrder();
        }
        void invalidateRequest()
        {
            ++m_generation;
            if (m_cancel) m_cancel->store(true);
            m_requested.reset();
            m_installed = {};
        }
        void sort(int column, Qt::SortOrder order) override
        {
            if (m_sortColumn == column && m_sortOrder == order) return;
            m_sortColumn = column; m_sortOrder = order;
            if (!m_requested) m_requested = m_snapshot;
            requestOrder();
        }
        int rowCount(const QModelIndex& parent = {}) const override
            { return parent.isValid() || !m_order ? 0 : static_cast<int>(m_order->rows.size()); }
        int columnCount(const QModelIndex& parent = {}) const override
            { return parent.isValid() ? 0 : static_cast<int>(m_headers.size()); }
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
        {
            if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
                index.column() < 0 || index.column() >= columnCount()) return {};
            const auto& row = m_snapshot->rows.at(m_order->rows.at(index.row()));
            if (role == StableKeyRole) return row.key;
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            {
                const QString value = row.columns.value(index.column());
                return m_translate && row.translatedColumns.contains(index.column()) ? m_translate(value) : value;
            }
            return index.column() == 0 ? row.roles.value(role) : QVariant();
        }
        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
        {
            if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < columnCount())
                return m_translate ? m_translate(m_headers.at(section)) : m_headers.at(section);
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        int rowForKey(const QString& key) const { return m_order ? m_order->indexByKey.value(key, -1) : -1; }
        std::function<void()> beforeInstall;
        std::function<void()> afterInstall;
    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event && event->type() == QEvent::LanguageChange && columnCount() > 0)
            {
                const QPointer<PropertyTableModel> lifetime(this);
                refreshTranslations();
                emit headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
                if (!lifetime) return false;
                if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole, Qt::ToolTipRole});
                if (!lifetime) return false;
                if (m_requested) requestOrder();
            }
            return QAbstractTableModel::eventFilter(watched, event);
        }
    private:
        void refreshTranslations()
        {
            m_sortTranslations.clear();
            if (m_requested && m_translate)
                for (const QString& key : m_requested->translationKeys) m_sortTranslations.insert(key, m_translate(key));
        }
        void requestOrder()
        {
            if (m_cancel) m_cancel->store(true);
            const quint64 generation = ++m_generation;
            if (!m_requested) return;
            const auto snapshot = m_requested;
            const auto cancel = m_cancel = std::make_shared<std::atomic_bool>(false);
            const QPointer<PropertyTableModel> guard(this);
            const int column = m_sortColumn;
            const auto order = m_sortOrder;
            const auto translations = m_sortTranslations;
            static QThreadPool* pool = []() { auto* value = new QThreadPool(QCoreApplication::instance());
                value->setMaxThreadCount(2); return value; }();
            pool->start(QRunnable::create([snapshot, cancel, guard, column, order, generation, translations]()
                {
                    const auto projection = orderPropertyTable(*snapshot, column, order, cancel, translations);
                    if (!projection || cancel->load()) return;
                    QMetaObject::invokeMethod(QCoreApplication::instance(), [snapshot, projection, cancel, guard, generation]()
                        {
                            if (!guard || cancel->load() || generation != guard->m_generation) return;
                            const auto commit = [snapshot, projection, cancel, guard, generation]()
                                {
                                    if (!guard || cancel->load() || generation != guard->m_generation) return;
                                    auto installed = guard->m_installed;
                                    if (guard->beforeInstall) guard->beforeInstall();
                                    if (!guard || cancel->load() || generation != guard->m_generation) return;
                                    guard->beginResetModel();
                                    if (!guard) return;
                                    if (cancel->load() || generation != guard->m_generation) { guard->endResetModel(); return; }
                                    guard->m_snapshot = snapshot; guard->m_order = projection;
                                    guard->endResetModel();
                                    if (!guard) return;
                                    if (cancel->load() || generation != guard->m_generation) return;
                                    guard->m_installed = {};
                                    if (installed) installed();
                                    if (!guard || cancel->load() || generation != guard->m_generation) return;
                                    if (guard->afterInstall) guard->afterInstall();
                                };
                            const auto gate = guard->m_gate;
                            if (gate) gate(commit); else commit();
                        }, Qt::QueuedConnection);
                }));
        }
        QStringList m_headers;
        std::function<QString(const QString&)> m_translate;
        std::shared_ptr<const PropertyTableSnapshot> m_requested, m_snapshot;
        std::shared_ptr<const PropertyTableOrder> m_order;
        std::shared_ptr<std::atomic_bool> m_cancel;
        QHash<QString, QString> m_sortTranslations;
        CommitGate m_gate;
        std::function<void()> m_installed;
        quint64 m_generation = 0;
        int m_sortColumn = -1;
        Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    };
}

#include <QTableView>
#include <QHeaderView>
#include <QScrollBar>
#include <QTimer>
#include "../UI/TableInteractionSupport.h"

namespace ks::fileui
{
    // The existing table keeps its model and action host throughout its lifetime.
    class PropertyTableCommitHost final : public QObject
    {
    public:
        PropertyTableCommitHost(QTableView* table, PropertyTableModel* model, QObject* owner, QString key)
            : QObject(table), m_table(table), m_model(model), m_owner(owner), m_key(std::move(key))
        {
            table->installEventFilter(this);
            const QPointer<PropertyTableCommitHost> guard(this);
            model->setCommitGate([guard](std::function<void()> commit) { if (guard) guard->submit(std::move(commit)); });
            model->beforeInstall = [guard]() { if (guard) guard->saveSelection(); };
            model->afterInstall = [guard]() { if (guard) guard->restoreSelection(); };
        }
    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event && event->type() == QEvent::Show && m_pending)
            {
                const QPointer<PropertyTableCommitHost> guard(this);
                QTimer::singleShot(0, this, [guard]() { if (guard && guard->m_pending)
                    guard->submit(std::move(guard->m_pending)); });
            }
            return QObject::eventFilter(watched, event);
        }
    private:
        void submit(std::function<void()> commit)
        {
            if (!m_table || !m_model || !m_owner) return;
            if (!m_table->isVisible()) { m_pending = std::move(commit); return; }
            m_pending = {};
            const QPointer<PropertyTableCommitHost> guard(this);
            auto guarded = [guard, commit = std::move(commit)]() mutable
                {
                    if (!guard || !guard->m_table || !guard->m_model || !guard->m_owner) return;
                    if (!guard->m_table->isVisible()) { guard->m_pending = std::move(commit); return; }
                    commit();
                };
            if (!ks::ui::DeferTableUiCommitIfContextMenuOpen(m_owner, m_key, {m_table.data()}, guarded)) guarded();
        }
        void saveSelection()
        {
            m_selected.clear(); m_current.clear();
            if (!m_table || !m_model) return;
            m_scroll = m_table->verticalScrollBar()->value();
            if (m_table->currentIndex().isValid())
                m_current = m_table->currentIndex().data(PropertyTableModel::StableKeyRole).toString();
            if (m_table->selectionModel())
                for (const auto& index : m_table->selectionModel()->selectedRows())
                    m_selected.push_back(index.data(PropertyTableModel::StableKeyRole).toString());
        }
        void restoreSelection()
        {
            if (!m_table || !m_model || !m_table->selectionModel()) return;
            QItemSelection selection;
            for (const QString& key : m_selected)
            {
                const int row = m_model->rowForKey(key);
                if (row >= 0) selection.select(m_model->index(row, 0), m_model->index(row, m_model->columnCount() - 1));
            }
            const QPointer<PropertyTableCommitHost> lifetime(this);
            m_table->selectionModel()->select(selection, QItemSelectionModel::Select);
            if (!lifetime || !m_table || !m_model) return;
            const int current = m_model->rowForKey(m_current);
            if (current >= 0) m_table->selectionModel()->setCurrentIndex(m_model->index(current, 0), QItemSelectionModel::NoUpdate);
            else if (m_current.isEmpty() && m_model->rowCount() > 0) m_table->setCurrentIndex(m_model->index(0, 0));
            if (lifetime && m_table) m_table->verticalScrollBar()->setValue(m_scroll);
        }
        QPointer<QTableView> m_table;
        QPointer<PropertyTableModel> m_model;
        QPointer<QObject> m_owner;
        QString m_key, m_current;
        QStringList m_selected;
        int m_scroll = 0;
        std::function<void()> m_pending;
    };
}
