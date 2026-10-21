#pragma once

#include "../UI/VisibleTableWidget.h"
#include "../UI/TableCompleteExport.h"
#include "../UI/TableHeaderSortingSupport.h"
#include "../Internationalization/LanguageManager.h"
#include "../ksword/network/network.h"
#include <QAbstractTableModel>
#include <QIcon>
#include <QHash>
#include <functional>
#include <memory>
#include <vector>

// NetworkDock-local value projection. Canonical packet bytes stay owned by the
// Dock; each published display version is complete and independently retained.
struct PacketMonitorRecord
{
    std::shared_ptr<const ks::network::PacketRecord> packet;
    QString domain;
    std::uint64_t timeline100ns = 0;
};
struct PacketMonitorRow
{
    std::shared_ptr<const PacketMonitorRecord> record;
    QStringList cells;
};
struct PacketMonitorValues
{
    using Row = std::shared_ptr<const PacketMonitorRow>;
    std::vector<Row> canonical, rows;
    QHash<qulonglong, int> positions;
    std::uint64_t version = 0;
};
struct PacketMonitorCommit
{
    std::shared_ptr<const PacketMonitorValues> previous, reordered, survivors, appended, target;
    bool reorderOld = false, reorderTarget = false, fieldsChanged = false;
    std::vector<std::uint64_t> metadata;
};

class PacketMonitorTable final : public ks::ui::TableActionTableView
{
    class Model final : public QAbstractTableModel
    {
    public:
        using QAbstractTableModel::QAbstractTableModel;
        std::shared_ptr<const PacketMonitorValues> values;
        QStringList headers;
        std::function<void(int, Qt::SortOrder)> sortRequest;
        std::function<QIcon(quint32)> icon;
        std::function<void(const ks::network::PacketRecord&)> metadata;
        int rowCount(const QModelIndex& parent = {}) const override
        { return parent.isValid() || !values ? 0 : static_cast<int>(values->rows.size()); }
        int columnCount(const QModelIndex& parent = {}) const override
        { return parent.isValid() ? 0 : static_cast<int>(headers.size()); }
        QVariant data(const QModelIndex& index, int role) const override
        {
            if (!index.isValid() || !values || index.row() < 0 || index.row() >= rowCount()
                || index.column() < 0 || index.column() >= columnCount()) return {};
            const auto& row = *values->rows[index.row()];
            if (metadata && ((role == Qt::DisplayRole && index.column() == 8 && row.record->domain.isEmpty())
                || (role == Qt::DecorationRole && index.column() == 5))) metadata(*row.record->packet);
            if (role == Qt::DisplayRole) return ks::i18n::displayText(row.cells.value(index.column()));
            if (role == Qt::UserRole && index.column() == 0) return QVariant::fromValue<qulonglong>(row.record->packet->sequenceId);
            if (role == Qt::UserRole && index.column() == 8) return QString::fromUtf8(row.record->packet->remoteAddress.c_str());
            if (role == Qt::DecorationRole && index.column() == 5 && icon) return icon(row.record->packet->processId);
            return {};
        }
        Qt::ItemFlags flags(const QModelIndex& index) const override
        { return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled : Qt::NoItemFlags; }
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override
        { return orientation == Qt::Horizontal && role == Qt::DisplayRole ? QVariant(ks::i18n::displayText(headers.value(section))) : QVariant{}; }
        void beginHeaders(const QStringList& labels)
        {
            if (headers.isEmpty() && !labels.isEmpty()) { beginInsertColumns({}, 0, static_cast<int>(labels.size()) - 1); headers = labels; endInsertColumns(); }
            else { headers = labels; emit headerDataChanged(Qt::Horizontal, 0, static_cast<int>(labels.size()) - 1); }
        }
        void sort(int column, Qt::SortOrder order) override { if (sortRequest) sortRequest(column, order); }
        // All permutations/maps are worker values. Only Qt's required atomic
        // notifications and its actual persistent indices remain on the GUI.
        bool rearrange(std::shared_ptr<const PacketMonitorValues> next)
        {
            const QPointer<Model> alive(this);
            emit layoutAboutToBeChanged();
            if (!alive) return false;
            const auto oldIndices = persistentIndexList();
            QModelIndexList mapped;
            mapped.reserve(oldIndices.size());
            for (const auto& index : oldIndices)
            {
                const auto sequence = values->rows[index.row()]->record->packet->sequenceId;
                const int row = next->positions.value(sequence, -1);
                mapped.push_back(row < 0 ? QModelIndex{} : createIndex(row, index.column()));
            }
            values = std::move(next);
            changePersistentIndexList(oldIndices, mapped);
            emit layoutChanged();
            return !alive.isNull();
        }
        bool publish(PacketMonitorCommit commit)
        {
            if (values != commit.previous) return false;
            const QPointer<Model> alive(this);
            if (commit.reorderOld && !rearrange(commit.reordered)) return false;
            const int kept = static_cast<int>(commit.survivors->rows.size());
            if (rowCount() > kept)
            {
                beginRemoveRows({}, kept, rowCount() - 1);
                if (!alive) return false;
                values = commit.survivors;
                endRemoveRows();
                if (!alive) return false;
            }
            const int appended = static_cast<int>(commit.appended->rows.size());
            if (appended > kept)
            {
                beginInsertRows({}, kept, appended - 1);
                if (!alive) return false;
                values = commit.appended;
                endInsertRows();
                if (!alive) return false;
            }
            if (commit.reorderTarget)
            {
                if (!rearrange(commit.target)) return false;
            }
            else values = commit.target;
            if (commit.fieldsChanged && rowCount() > 0)
                emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
            return !alive.isNull();
        }
    };
    Model* m_values;
public:
    std::function<void(int, Qt::SortOrder)> sortRequested;
    std::function<QIcon(quint32)> processIcon;
    std::function<void(const ks::network::PacketRecord&)> requestMetadata;
    explicit PacketMonitorTable(QWidget* parent) : TableActionTableView(parent), m_values(new Model(this))
    {
        setModel(m_values);
        setProperty("ks_i18n_preserve_model_data", true);
        m_values->sortRequest = [this](int column, Qt::SortOrder order) { if (sortRequested) sortRequested(column, order); };
        m_values->icon = [this](quint32 pid) { return processIcon ? processIcon(pid) : QIcon{}; };
        m_values->metadata = [this](const auto& packet) { if (requestMetadata) requestMetadata(packet); };
        ks::ui::InstallTableHeaderClickSorting(this);
        ks::ui::RegisterCompleteExportTableProducer(this, [this]()
        {
            ks::ui::CompleteExportPublishedTable published;
            published.values = m_values->values;
            published.sourceIdentity = QStringLiteral("network-packets/%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
            published.sourceVersion = m_values->values ? m_values->values->version : 0;
            published.rows = rowCount(); published.columns = columnCount();
            published.cell = [](const void* value, int row, int column)
            { return static_cast<const PacketMonitorValues*>(value)->rows[row]->cells.value(column); };
            return published;
        });
    }
    void setHorizontalHeaderLabels(const QStringList& headers)
    { m_values->beginHeaders(headers); }
    int rowCount() const { return m_values->rowCount(); }
    int columnCount() const { return m_values->columnCount(); }
    QVariant cell(int row, int column, int role = Qt::DisplayRole) const { return m_values->data(m_values->index(row, column), role); }
    void setCurrentCell(int row, int column) { setCurrentIndex(m_values->index(row, column)); }
    std::shared_ptr<const PacketMonitorValues> values() const { return m_values->values; }
    bool publish(PacketMonitorCommit commit) { return m_values->publish(std::move(commit)); }
    void refreshProcessIcons()
    { if (rowCount()) emit m_values->dataChanged(m_values->index(0, 5), m_values->index(rowCount() - 1, 5), {Qt::DecorationRole}); }
};
