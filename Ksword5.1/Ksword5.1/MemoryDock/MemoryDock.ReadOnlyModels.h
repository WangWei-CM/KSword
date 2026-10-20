#pragma once
#include "../UI/VisibleTableWidget.h"
#include "../Internationalization/LanguageManager.h"
#include <QAbstractTableModel>
#include <QTreeView>
#include <QPointer>
#include <QThreadPool>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ks::memory_detail {
inline void RetireValue(std::function<void()> destroy)
{
    try { QThreadPool::globalInstance()->start([destroy] { destroy(); }); }
    catch (...) { if (destroy) { destroy(); } }
}
template<class T> std::shared_ptr<T> ValueStorage()
{
    return std::shared_ptr<T>(new T, [](T* value) { RetireValue([value] { delete value; }); });
}
template<class T> std::shared_ptr<T> ValueStorage(T value)
{
    return std::shared_ptr<T>(new T(std::move(value)), [](T* previous) { RetireValue([previous] { delete previous; }); });
}
inline constexpr int CoreIdentityRole = Qt::UserRole + 1903;
inline constexpr int CoreSourceIndexRole = Qt::UserRole + 1904;
class CoreReadOnlyModel final : public QAbstractTableModel {
public:
    using Resolver = std::function<QVariant(std::size_t, int, int)>;
    explicit CoreReadOnlyModel(QObject* parent) : QAbstractTableModel(parent) {}
    std::function<void(int, Qt::SortOrder)> requestSort;
    std::uint64_t revision = 0;
    void setHeaders(QStringList value) {
        const QPointer<CoreReadOnlyModel> alive(this);
        const bool changed = headers.size() != value.size();
        if (changed) { beginResetModel(); if (!alive) { return; } }
        headers = std::move(value);
        if (changed) { endResetModel(); }
        else if (!headers.isEmpty()) { emit headerDataChanged(Qt::Horizontal, 0, headers.size() - 1); }
    }
    void publish(std::shared_ptr<const std::vector<std::size_t>> value, Resolver resolve) {
        const QPointer<CoreReadOnlyModel> alive(this);
        beginResetModel(); if (!alive) { return; }
        rows = std::move(value); resolver = std::move(resolve); ++revision;
        endResetModel();
    }
    void clear() { publish({}, {}); }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() || !rows ? 0 : static_cast<int>(rows->size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : headers.size(); }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        return orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < headers.size()
            ? QVariant(ks::i18n::sourceText(headers[section])) : QVariant();
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || index.column() < 0 || index.column() >= columnCount() || !resolver) { return {}; }
        return resolver((*rows)[static_cast<std::size_t>(index.row())], index.column(), role);
    }
    bool setData(const QModelIndex& index, const QVariant&, int role) override {
        // The icon service already updated its shared GUI cache. Only repaint this identity.
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || role != Qt::DecorationRole) { return false; }
        emit dataChanged(index, index, {Qt::DecorationRole}); return true;
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override { return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags; }
    void sort(int column, Qt::SortOrder order) override { if (requestSort) { requestSort(column, order); } }
    void refreshColors(int first, int last) { if (first >= 0 && last >= first && last < rowCount()) { emit dataChanged(index(first, 0), index(last, columnCount() - 1), {Qt::ForegroundRole}); } }
    void refreshHeaders() { if (!headers.isEmpty()) { emit headerDataChanged(Qt::Horizontal, 0, headers.size() - 1); } }
private:
    QStringList headers;
    std::shared_ptr<const std::vector<std::size_t>> rows;
    Resolver resolver;
};
}

class MemoryTableView final : public ks::ui::TableActionTableView {
public:
    explicit MemoryTableView(QWidget* parent) : TableActionTableView(parent) { readOnlyModel = new ks::memory_detail::CoreReadOnlyModel(this); setModel(readOnlyModel); }
    ks::memory_detail::CoreReadOnlyModel* readOnlyModel;
    void setHorizontalHeaderLabels(QStringList headers) { readOnlyModel->setHeaders(std::move(headers)); }
    int rowCount() const { return readOnlyModel->rowCount(); }
    int columnCount() const { return readOnlyModel->columnCount(); }
    void clearEntries() { readOnlyModel->clear(); }
};
class MemoryTreeView final : public QTreeView {
public:
    explicit MemoryTreeView(QWidget* parent) : QTreeView(parent) { readOnlyModel = new ks::memory_detail::CoreReadOnlyModel(this); setModel(readOnlyModel); setUniformRowHeights(true); }
    ks::memory_detail::CoreReadOnlyModel* readOnlyModel;
    void setHeaderLabels(QStringList headers) { readOnlyModel->setHeaders(std::move(headers)); }
    int columnCount() const { return readOnlyModel->columnCount(); }
    int topLevelItemCount() const { return readOnlyModel->rowCount(); }
    void clear() { readOnlyModel->clear(); }
};
