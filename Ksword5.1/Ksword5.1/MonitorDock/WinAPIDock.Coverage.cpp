#include "WinAPIDock.h"
#include "../UI/ThemeStatusRole.h"
#include "../UI/TableInteractionSupport.h"
#include "../Internationalization/LanguageManager.h"
#include <QAbstractTableModel>
#include <QFileSystemWatcher>
#include <QDir>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QApplication>
#include <algorithm>
#include <functional>
#include <QComboBox>
#include <QCollator>
#include <QHeaderView>
#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTableView>
#include <QTabWidget>

namespace
{
    // The source packet snapshot remains canonical; search only replaces these row identities.
    std::vector<std::size_t> coverageMatchingRows(
        const std::vector<WinAPIDock::CoverageDisplayRow>& rows, const QString& filter,
        int sortColumn, Qt::SortOrder sortOrder, const ks::i18n::SourceTextSnapshot& language)
    {
        std::vector<std::size_t> matching;
        matching.reserve(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (filter.isEmpty() || rows[i].searchText.contains(filter, Qt::CaseInsensitive)) matching.push_back(i);
        if (sortColumn >= 0 && sortColumn < 7)
        {
            std::vector<std::pair<std::size_t, QString>> keys;
            keys.reserve(matching.size());
            for (const auto row : matching) keys.emplace_back(row, language.displayText(rows[row].columns.value(sortColumn)));
            const QCollator collator;
            std::stable_sort(keys.begin(), keys.end(), [&](const auto& left, const auto& right) {
                const int order = collator.compare(left.second, right.second);
                return sortOrder == Qt::AscendingOrder ? order < 0 : order > 0;
            });
            matching.clear();
            for (const auto& key : keys) matching.push_back(key.first);
        }
        return matching;
    }

    class CoverageTableModel final : public QAbstractTableModel
    {
    public:
        explicit CoverageTableModel(QObject* parent, std::function<void()> request)
            : QAbstractTableModel(parent), m_request(std::move(request)) {}
        void sort(int column, Qt::SortOrder order) override
        { m_requestedSortColumn = column; m_requestedSortOrder = order; m_request(); }
        int requestedSortColumn() const { return m_requestedSortColumn; }
        Qt::SortOrder requestedSortOrder() const { return m_requestedSortOrder; }
        int rowCount(const QModelIndex& parent = {}) const override
        { return parent.isValid() || !m_indexes ? 0 : static_cast<int>(m_indexes->size()); }
        int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 7; }
        QVariant data(const QModelIndex& index, const int role) const override
        {
            if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()
                || index.column() < 0 || index.column() >= columnCount() || !m_rows) return {};
            const auto source = (*m_indexes)[static_cast<std::size_t>(index.row())];
            if (source >= m_rows->size()) return {};
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
                return ks::i18n::sourceText((*m_rows)[source].columns[index.column()]);
            if (role == Qt::TextAlignmentRole) return int(Qt::AlignLeft | Qt::AlignVCenter);
            return {};
        }
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override
        {
            if (role != Qt::DisplayRole) return {};
            if (orientation == Qt::Vertical) return section + 1;
            static const QStringList headers{QStringLiteral("PID"), QStringLiteral("API ID"), QStringLiteral("Hook 类型"),
                QStringLiteral("API"), QStringLiteral("覆盖状态"), QStringLiteral("入口地址"), QStringLiteral("原因")};
            return section >= 0 && section < headers.size() ? QVariant(ks::i18n::sourceText(headers[section])) : QVariant();
        }
        bool matches(const std::shared_ptr<const std::vector<WinAPIDock::CoverageDisplayRow>>& rows, const QString& filter, int column, Qt::SortOrder order) const
        { return m_rows == rows && m_filter == filter && m_appliedSortColumn == column && m_appliedSortOrder == order; }
        void replace(std::shared_ptr<const std::vector<WinAPIDock::CoverageDisplayRow>> rows,
            std::shared_ptr<const std::vector<std::size_t>> indexes, QString filter = {},
            int sortColumn = -1, Qt::SortOrder sortOrder = Qt::AscendingOrder)
        {
            const bool sameRows = m_rows == rows && m_filter == filter && m_indexes && indexes;
            if (sameRows) emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
            else beginResetModel();
            // Selection models create their persistent indexes in the signal.
            const auto previousPersistent = sameRows ? persistentIndexList() : QModelIndexList{};
            auto previousRows = std::move(m_rows);
            auto previousIndexes = std::move(m_indexes);
            m_rows = std::move(rows); m_indexes = std::move(indexes); m_filter = std::move(filter);
            m_appliedSortColumn = sortColumn; m_appliedSortOrder = sortOrder;
            if (sameRows) {
                std::vector<int> destination(m_rows->size(), -1);
                for (std::size_t row = 0; row < m_indexes->size(); ++row) destination[(*m_indexes)[row]] = static_cast<int>(row);
                QModelIndexList nextPersistent; nextPersistent.reserve(previousPersistent.size());
                for (const auto& index : previousPersistent) {
                    const auto source = (*previousIndexes)[static_cast<std::size_t>(index.row())];
                    nextPersistent.push_back(source < destination.size() && destination[source] >= 0 ? this->index(destination[source], index.column()) : QModelIndex{});
                }
                changePersistentIndexList(previousPersistent, nextPersistent);
                emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
            } else endResetModel();
            // Large immutable snapshots can become last-owned here; release their strings off GUI.
            QThreadPool::globalInstance()->start(QRunnable::create(
                [previousRows = std::move(previousRows), previousIndexes = std::move(previousIndexes)]() {}));
        }
    private:
        QString m_filter;
        std::function<void()> m_request;
        int m_requestedSortColumn = -1, m_appliedSortColumn = -1;
        Qt::SortOrder m_requestedSortOrder = Qt::AscendingOrder, m_appliedSortOrder = Qt::AscendingOrder;
        std::shared_ptr<const std::vector<WinAPIDock::CoverageDisplayRow>> m_rows;
        std::shared_ptr<const std::vector<std::size_t>> m_indexes;
    };

    QString coverageText(const wchar_t* text, std::size_t capacity)
    { return QString::fromWCharArray(text, static_cast<int>(wcsnlen_s(text, capacity))); }
    QString coverageStateText(std::uint32_t state)
    {
        using ks::winapi_monitor::CoverageState;
        switch (static_cast<CoverageState>(state))
        {
        case CoverageState::Installed: return QStringLiteral("已安装");
        case CoverageState::SharedEntry: return QStringLiteral("共享入口");
        case CoverageState::CategoryDisabled: return QStringLiteral("分类关闭");
        case CoverageState::WaitingModule: return QStringLiteral("等待模块");
        case CoverageState::ExportMissing: return QStringLiteral("导出缺失");
        case CoverageState::RuleExcluded: return QStringLiteral("规则排除");
        case CoverageState::RetryableFailure: return QStringLiteral("可重试失败");
        case CoverageState::Unsupported: return QStringLiteral("不支持");
        case CoverageState::Removed: return QStringLiteral("已移除");
        }
        return QStringLiteral("未知");
    }
    QString coverageKindText(std::uint32_t kind)
    {
        using ks::winapi_monitor::HookKind;
        switch (static_cast<HookKind>(kind))
        {
        case HookKind::Strong: return QStringLiteral("强类型");
        case HookKind::Raw: return QString::fromLatin1("Raw");
        case HookKind::Fake: return QString::fromLatin1("Fake");
        case HookKind::DynamicExtension: return QStringLiteral("动态扩展");
        }
        return QStringLiteral("未知");
    }
}

void WinAPISessionBackend::queueCoverageSnapshot(std::uint32_t pid, std::uint64_t session, std::uint64_t revision,
    std::vector<ks::winapi_monitor::ApiMonitorEventPacket> rows, bool stale, std::uint64_t generation)
{
    std::shared_ptr<const std::vector<CoverageDisplayRow>> displayRows;
    if (!stale)
    {
        auto prepared = std::make_shared<std::vector<CoverageDisplayRow>>();
        prepared->reserve(rows.size());
        for (const auto& packet : rows)
        {
            const QString api = coverageText(packet.moduleName, std::size(packet.moduleName)) + QString::fromLatin1("!")
                + coverageText(packet.apiName, std::size(packet.apiName));
            const QString state = coverageStateText(packet.coverageState);
            const QString kind = coverageKindText(packet.hookKind);
            const QString detail = coverageText(packet.detailText, std::size(packet.detailText));
            prepared->push_back({{QString::number(pid), QString::number(packet.apiId), kind, api, state,
                QString::fromLatin1("0x%1").arg(static_cast<qulonglong>(packet.hookAddress), 0, 16), detail}, api + state + kind + detail});
        }
        displayRows = std::move(prepared);
    }
    const std::lock_guard<std::mutex> lock(pendingMutex);
    if (generation != config.generation) return;
    auto& pending = pendingCoverage[pid];
    if (pending.session && (pending.session != session || pending.revision > revision)) return;
    pending.session = session;
    pending.revision = revision;
    pending.stale = stale;
    if (!stale) { pending.rows = std::move(rows); pending.displayRows = std::move(displayRows); pending.complete = true; }
}

void WinAPIDock::flushCoverageUpdates()
{
    // A canceled GUI delivery only publishes a value flag. This existing tick
    // reconciles it and recomputes the latest replaceable projection/hash.
    if (m_coverageHashDeliveryDropped && m_coverageHashDeliveryDropped->exchange(false)) {
        m_coverageHashPending = false; refreshCoverageDefinitionHash();
    }
    if (m_coverageFilterDeliveryDropped && m_coverageFilterDeliveryDropped->exchange(false)) {
        m_coverageFilterRunning = false; m_coverageDirty = true; updateCoverageView();
    }
    std::unordered_map<std::uint32_t, CoverageViewState> updates;
    {
        const std::lock_guard<std::mutex> lock(m_pendingMutex);
        updates.swap(m_pendingCoverage);
    }
    bool changed = false;
    for (auto& [pid, update] : updates)
    {
        auto& visible = m_coverageViews[pid];
        if (visible.session && (visible.session != update.session || visible.revision > update.revision)) continue;
        visible.session = update.session;
        if (update.complete) { visible.revision = update.revision; visible.rows = std::move(update.rows); visible.displayRows = std::move(update.displayRows); visible.complete = true; }
        visible.stale = update.stale;
        if (pid == m_currentSessionPid && !visible.rows.empty())
        {
            m_definitionMismatch = m_coverageDefinitionHash.isEmpty()
                || m_coverageDefinitionHash != QByteArray(visible.rows.front().definitionSha256, 64);
        }
        changed = true;
    }
    if (!changed) return;
    m_coverageDirty = true;
    const QSignalBlocker blocker(m_coveragePidCombo);
    const auto previousPid = m_coveragePidCombo->currentData().toUInt();
    m_coveragePidCombo->clear();
    std::vector<std::uint32_t> pids;
    for (const auto& [pid, view] : m_coverageViews) pids.push_back(pid);
    std::sort(pids.begin(), pids.end());
    for (const auto pid : pids) m_coveragePidCombo->addItem(QString::number(pid), pid);
    const int selected = m_coveragePidCombo->findData(previousPid);
    if (selected >= 0) m_coveragePidCombo->setCurrentIndex(selected);
    if (m_resultTabs->currentIndex() == 1) updateCoverageView();
}

void WinAPIDock::initializeCoverageTable()
{
    m_coverageDispatcher = std::make_shared<ks::ui::AsyncUiDispatcher>(this);
    m_coverageModel = new CoverageTableModel(m_coverageTable, [this]() { updateCoverageView(); });
    m_coverageTable->setModel(m_coverageModel);
    auto* watcher = new QFileSystemWatcher(this);
    const QString directory = QApplication::applicationDirPath() + QStringLiteral("/profiles");
    watcher->addPath(directory);
    watcher->addPath(directory + QStringLiteral("/api_monitor_definitions.json"));
    connect(watcher, &QFileSystemWatcher::directoryChanged, this, [this, watcher, directory]() {
        // Atomic replacement invalidates a file watch. The directory watch reconnects it.
        if (watcher->files().isEmpty()) watcher->addPath(directory + QStringLiteral("/api_monitor_definitions.json"));
        refreshCoverageDefinitionHash();
    });
    connect(watcher, &QFileSystemWatcher::fileChanged, this, [this]() { refreshCoverageDefinitionHash(); });
    refreshCoverageDefinitionHash();
}

void WinAPIDock::refreshCoverageDefinitionHash()
{
    m_coverageHashDirty = true;
    if (m_coverageHashPending || !m_coverageDispatcher) return;
    m_coverageHashPending = true;
    const auto deliveryDropped = m_coverageHashDeliveryDropped = std::make_shared<std::atomic_bool>(false);
    m_coverageHashDirty = false;
    const QString path = QApplication::applicationDirPath() + QStringLiteral("/profiles/api_monitor_definitions.json");
    const auto dispatcher = m_coverageDispatcher;
    QThreadPool::globalInstance()->start(QRunnable::create([this, dispatcher, path, deliveryDropped]() {
        QFile file(path);
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        const QByteArray hash = file.open(QIODevice::ReadOnly) && hasher.addData(&file)
            && file.error() == QFileDevice::NoError ? hasher.result().toHex() : QByteArray();
        dispatcher->post([this, hash]() {
            m_coverageHashPending = false;
            const bool changed = m_coverageDefinitionHash != hash;
            m_coverageDefinitionHash = hash;
            if (changed)
            {
                const auto root = m_coverageViews.find(m_currentSessionPid);
                if (root != m_coverageViews.end() && !root->second.rows.empty())
                    m_definitionMismatch = hash.isEmpty() || hash != QByteArray(root->second.rows.front().definitionSha256, 64);
                m_coverageDirty = true;
                updateCoverageView();
                updateStatusLabel();
            }
            if (m_coverageHashDirty) refreshCoverageDefinitionHash();
        }, [deliveryDropped]() { deliveryDropped->store(true); });
    }));
}

void WinAPIDock::updateCoverageView()
{
    if (!m_coverageTable || !m_coveragePidCombo || !m_coverageStatusLabel || !m_coverageDispatcher) return;
    m_coverageDirty = true;
    const auto ticket = ++m_coverageFilterTicket;
    // Hidden coverage keeps its accepted canonical snapshot; only its display projection waits.
    if (!m_coverageTable->isVisible()) return;
    if (!m_coverageDebounceDue)
    {
        if (!m_coverageDebouncePending)
        {
            m_coverageDebouncePending = true;
            QTimer::singleShot(180, this, [this]() {
                m_coverageDebouncePending = false;
                m_coverageDebounceDue = true;
                updateCoverageView();
            });
        }
        return;
    }
    m_coverageDebounceDue = false;
    const auto pid = m_coveragePidCombo->currentData().toUInt();
    const auto iterator = m_coverageViews.find(pid);
    if (iterator == m_coverageViews.end())
    {
        static_cast<CoverageTableModel*>(m_coverageModel)->replace({}, {});
        m_coverageStatusLabel->setText(QStringLiteral("覆盖快照：等待 Agent 完整快照"));
        m_coverageDirty = false;
        return;
    }
    const auto& view = iterator->second;
    const auto remoteHash = view.rows.empty() ? QByteArray() : QByteArray(view.rows.front().definitionSha256, 64);
    const bool mismatch = m_coverageDefinitionHash.isEmpty() || remoteHash != m_coverageDefinitionHash;
    if (pid == m_currentSessionPid) m_definitionMismatch = mismatch;
    QString status = view.stale ? QStringLiteral("覆盖快照：已过期或不完整") : QStringLiteral("覆盖快照：完整");
    status += QStringLiteral(" | PID=%1，修订=%2，API=%3").arg(pid).arg(static_cast<qulonglong>(view.revision)).arg(view.rows.size());
    if (mismatch) status += QStringLiteral(" | API 定义不匹配或缺失");
    m_coverageStatusLabel->setText(status);
    ks::ui::ApplyStatusRole(m_coverageStatusLabel, view.stale || mismatch ? ks::ui::StatusRole::Warning : ks::ui::StatusRole::Info);
    const auto rows = view.displayRows;
    const QString filter = m_coverageFilterEdit->text().trimmed();
    const auto model = static_cast<CoverageTableModel*>(m_coverageModel);
    const int sortColumn = model->requestedSortColumn();
    const auto sortOrder = model->requestedSortOrder();
    const auto language = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
    if (model->matches(rows, filter, sortColumn, sortOrder))
    { m_coverageDirty = false; return; }
    const auto dispatcher = m_coverageDispatcher;
    // One timer samples the latest request; one worker scans it without touching the model.
    if (m_coverageFilterRunning) return;
    m_coverageFilterRunning = true;
    const auto deliveryDropped = m_coverageFilterDeliveryDropped = std::make_shared<std::atomic_bool>(false);
        QThreadPool::globalInstance()->start(QRunnable::create([this, ticket, rows, filter, sortColumn, sortOrder, language, dispatcher, deliveryDropped]() {
            const auto matching = std::make_shared<const std::vector<std::size_t>>(
                rows ? coverageMatchingRows(*rows, filter, sortColumn, sortOrder, language) : std::vector<std::size_t>());
            dispatcher->post([this, ticket, rows, matching, filter, sortColumn, sortOrder]() {
                m_coverageFilterRunning = false;
                if (ticket != m_coverageFilterTicket) { updateCoverageView(); return; }
                if (!m_coverageTable->isVisible()) return;
                auto commit = [this, ticket, rows, matching, filter, sortColumn, sortOrder]() {
                    if (ticket != m_coverageFilterTicket || !m_coverageTable->isVisible()) return;
                    static_cast<CoverageTableModel*>(m_coverageModel)->replace(rows, matching, filter, sortColumn, sortOrder);
                    m_coverageDirty = false;
                };
                if (!ks::ui::DeferTableUiCommitIfContextMenuOpen(this,
                    QStringLiteral("winapi-coverage-projection-apply"), {m_coverageTable}, commit)) commit();
            }, [deliveryDropped]() { deliveryDropped->store(true); });
        }));
}

void WinAPIDock::resetCoverageView()
{
    {
        const std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingCoverage.clear();
    }
    m_coverageViews.clear();
    refreshCoverageDefinitionHash();
    m_coveragePidCombo->clear();
    m_coverageDirty = true;
    updateCoverageView();
}

void WinAPIDock::markCoverageStale()
{
    flushCoverageUpdates();
    for (auto& [pid, view] : m_coverageViews) view.stale = true;
    m_coverageDirty = true;
    if (m_resultTabs->currentIndex() == 1) updateCoverageView();
}
