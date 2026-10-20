#include "NetworkDock.NidsAnalysis.h"
#include "../UI/AsyncOperation.h"
#include <QTimer>
#include "NetworkDock.NavigationData.h"
#include "NetworkDock.InternalCommon.h"
#include "../UI/TableContextMenu.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/FlatButtonTheme.h"
#include "NetworkFirewallPage.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/TableInteractionSupport.h"

#include "../OnlineScan/SandboxUploadActions.h"
#include "../theme.h"

#include <QApplication>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QTableView>
#include "../Internationalization/LanguageManager.h"
#include <QBrush>
#include <QImage>
#include <QPixmap>
#include <objbase.h>
#include <Shellapi.h>

using namespace network_dock_detail;

namespace
{
    struct NidsIconImage { QString path; QImage image; };
    struct NidsIconComScope
    {
        bool initialized = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        ~NidsIconComScope() { if (initialized) ::CoUninitialize(); }
    };
    struct NidsNativeIcon
    {
        HICON value = nullptr;
        ~NidsNativeIcon() { if (value) ::DestroyIcon(value); }
    };
    QImage decodeNidsNativeIcon(HICON icon) { return QImage::fromHICON(icon); }
    std::vector<NidsIconImage> loadNidsIconImages(const QStringList& paths, const ks::ui::AsyncOperationToken& token)
    {
        NidsIconComScope com;
        std::vector<NidsIconImage> result;
        result.reserve(static_cast<std::size_t>(paths.size()));
        for (const auto& path : paths)
        {
            if (token.isCanceled()) break;
            SHFILEINFOW info{};
            const auto queried = ::SHGetFileInfoW(reinterpret_cast<const wchar_t*>(path.utf16()), 0, &info,
                sizeof(info), SHGFI_ICON | SHGFI_SMALLICON);
            NidsNativeIcon icon{info.hIcon};
            result.push_back({path, queried != 0 && icon.value ? decodeNidsNativeIcon(icon.value) : QImage()});
        }
        return result;
    }

    std::shared_ptr<NidsAnalysisSession> makeNidsAnalysisSession()
    {
        // The last GUI job may release a retired epoch after its worker. A custom
        // value deleter keeps large engine/packet destruction off the GUI then too.
        return std::shared_ptr<NidsAnalysisSession>(new NidsAnalysisSession, [](NidsAnalysisSession* retired) noexcept
        {
            std::unique_ptr<NidsAnalysisSession> value(retired);
            try { QThreadPool::globalInstance()->start([value = std::move(value)]() mutable { value.reset(); }); }
            catch (...) {} // The local/task unique owner still releases if scheduling fails or a queued task is dropped.
        });
    }

    // nidsSeverityText：把 NIDS 等级转为界面文本。
    QString nidsSeverityText(const ks::network::NidsAlertSeverity severity)
    {
        switch (severity)
        {
        case ks::network::NidsAlertSeverity::Low:
            return QStringLiteral("低");
        case ks::network::NidsAlertSeverity::Medium:
            return QStringLiteral("中");
        case ks::network::NidsAlertSeverity::High:
            return QStringLiteral("高");
        case ks::network::NidsAlertSeverity::Critical:
            return QStringLiteral("严重");
        default:
            return QStringLiteral("未知");
        }
    }

    // nidsSeverityColor：按等级返回表格强调色。
    QColor nidsSeverityColor(const ks::network::NidsAlertSeverity severity)
    {
        switch (severity)
        {
        case ks::network::NidsAlertSeverity::Low:
            return KswordTheme::TextSecondaryColor();
        case ks::network::NidsAlertSeverity::Medium:
            return KswordTheme::WarningColor();
        case ks::network::NidsAlertSeverity::High:
            return KswordTheme::ErrorColor();
        case ks::network::NidsAlertSeverity::Critical:
            return KswordTheme::AccentColor(KswordTheme::AccentRole::Purple);
        default:
            return KswordTheme::TextSecondaryColor();
        }
    }

    // nidsSeverityIndex：等级转 int，用于过滤和排序。
    int nidsSeverityIndex(const ks::network::NidsAlertSeverity severity)
    {
        return static_cast<int>(severity);
    }

    // nidsTableRowText：
    // - 输入：NIDS 表格指针和行号；
    // - 处理：按当前列顺序读取整行，使用 TSV 拼接；
    // - 返回：可直接复制到剪贴板或表格工具的文本。
    QString nidsTableRowText(QTableView* table, const int rowIndex)
    {
        if (table == nullptr || rowIndex < 0 || rowIndex >= table->model()->rowCount())
        {
            return QString();
        }

        QStringList fields;
        fields.reserve(table->model()->columnCount());
        for (int columnIndex = 0; columnIndex < table->model()->columnCount(); ++columnIndex)
        {
            fields.push_back(table->model()->index(rowIndex, columnIndex).data().toString());
        }
        return fields.join(QLatin1Char('\t'));
    }

    // nidsSelectedRows：
    // - 输入：NIDS 表格指针；
    // - 处理：收集选择模型中的行号并去重排序，空选择时回退当前行；
    // - 返回：需要复制的行号列表。
    std::vector<int> nidsSelectedRows(QTableView* table)
    {
        std::set<int> rowSet;
        if (table != nullptr)
        {
            for (const QModelIndex& index : table->selectionModel()->selectedRows())
            {
                rowSet.insert(index.row());
            }
            if (rowSet.empty() && table->currentIndex().row() >= 0)
            {
                rowSet.insert(table->currentIndex().row());
            }
        }
        return std::vector<int>(rowSet.begin(), rowSet.end());
    }

    // copyNidsRowsToClipboard：
    // - 输入：NIDS 表格和待复制行号；
    // - 处理：逐行生成 TSV，多行使用换行分隔；
    // - 返回：无，复制失败时静默保持 UI 稳定。
    void copyNidsRowsToClipboard(QTableView* table, const std::vector<int>& rowList)
    {
        if (table == nullptr || QApplication::clipboard() == nullptr || rowList.empty())
        {
            return;
        }

        QStringList rowTexts;
        rowTexts.reserve(static_cast<int>(rowList.size()));
        for (const int rowIndex : rowList)
        {
            const QString rowText = nidsTableRowText(table, rowIndex);
            if (!rowText.isEmpty())
            {
                rowTexts.push_back(rowText);
            }
        }
        if (!rowTexts.isEmpty())
        {
            QApplication::clipboard()->setText(rowTexts.join(QChar('\n')));
        }
    }

    // nidsAlertSequenceForRow：
    // - 输入：NIDS 表格指针、目标行号和时间列索引（列枚举是 NetworkDock 的类内成员，
    //   匿名命名空间取不到，故由调用方换算好列号传入）；
    // - 处理：从时间列的 Qt::UserRole 取出该告警关联的报文序号；
    // - 返回：取到非零序号时为 true，行号越界、单元格缺失或序号无效时为 false。
    bool nidsAlertSequenceForRow(
        QTableView* table,
        const int row,
        const int timeColumn,
        std::uint64_t& sequenceIdOut)
    {
        if (table == nullptr || row < 0 || row >= table->model()->rowCount())
        {
            return false;
        }

        const QVariant sequenceVariant = table->model()->index(row, timeColumn).data(Qt::UserRole);
        if (!sequenceVariant.isValid())
        {
            return false;
        }
        sequenceIdOut = static_cast<std::uint64_t>(sequenceVariant.toULongLong());
        return sequenceIdOut != 0;
    }

    // captureNidsProcessIdentity：
    // - 作用：在 NIDS 告警生成的同一时段冻结应用级处置所需的进程身份；
    // - 处理：创建时间前后各读取一次，只有两次一致且镜像路径可得才接受快照；
    // - 返回：失败时保持字段为空，后续只能生成端点级阻断规则。
    void captureNidsProcessIdentity(ks::network::NidsAlert& alertRecord)
    {
        if (alertRecord.processId == 0U)
        {
            return;
        }

        std::uint64_t creationTimeBefore = 0U;
        if (!ks::process::QueryProcessCreationTimeByPid(
                alertRecord.processId,
                &creationTimeBefore,
                nullptr) ||
            !NidsAnalysisSession::identityPredatesPacket(creationTimeBefore, alertRecord.timestampMs))
        {
            return;
        }

        const std::string imagePath = ks::process::QueryProcessPathByPid(alertRecord.processId);
        if (imagePath.empty())
        {
            return;
        }

        std::uint64_t creationTimeAfter = 0U;
        if (!ks::process::QueryProcessCreationTimeByPid(
                alertRecord.processId,
                &creationTimeAfter,
                nullptr) ||
            creationTimeAfter != creationTimeBefore)
        {
            return;
        }

        alertRecord.processCreationTime100ns = creationTimeAfter;
        alertRecord.processImagePath = imagePath;
    }
}

class NetworkDock::NidsAlertModel final : public QAbstractTableModel
{
public:
    using Alert = std::shared_ptr<const ks::network::NidsAlert>;
    explicit NidsAlertModel(NetworkDock* owner, QStringList headers)
        : QAbstractTableModel(owner), m_owner(owner), m_headers(std::move(headers)) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(m_rows.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_headers.size(); }
    Alert alertAt(int row) const { return row >= 0 && row < rowCount() ? m_rows[static_cast<std::size_t>(row)] : Alert(); }
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < m_headers.size())
            return ks::i18n::displayText(m_headers[section]);
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        const auto value = alertAt(index.row());
        if (!index.isValid() || !value || index.column() < 0 || index.column() >= columnCount()) return {};
        const auto& a = *value;
        const auto column = static_cast<NidsAlertTableColumn>(index.column());
        if (role == Qt::UserRole)
        {
            if (column == NidsAlertTableColumn::Time) return QVariant::fromValue(static_cast<qulonglong>(a.sequenceId));
            if (column == NidsAlertTableColumn::Severity) return nidsSeverityIndex(a.severity);
            return {};
        }
        if (role == Qt::ForegroundRole && column == NidsAlertTableColumn::Severity) return QBrush(nidsSeverityColor(a.severity));
        if (role == Qt::DecorationRole && column == NidsAlertTableColumn::ProcessName && m_owner)
            return m_owner->resolveNidsProcessIcon(a);
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        switch (column)
        {
        case NidsAlertTableColumn::Time: return toQString(ks::network::FormatUnixTimestampMs(a.timestampMs, false));
        case NidsAlertTableColumn::Severity: return ks::i18n::sourceText(nidsSeverityText(a.severity));
        case NidsAlertTableColumn::Category: return toQString(a.category);
        case NidsAlertTableColumn::Rule: return toQString(a.ruleId);
        case NidsAlertTableColumn::Protocol: return toQString(ks::network::PacketProtocolToString(a.protocol));
        case NidsAlertTableColumn::Direction: return toQString(ks::network::PacketDirectionToString(a.direction));
        case NidsAlertTableColumn::Pid: return QString::number(a.processId);
        case NidsAlertTableColumn::ProcessName: return toQString(a.processName);
        case NidsAlertTableColumn::LocalEndpoint: return formatEndpointText(a.localAddress, a.localPort);
        case NidsAlertTableColumn::RemoteEndpoint: return formatEndpointText(a.remoteAddress, a.remotePort);
        case NidsAlertTableColumn::Detail: return QStringLiteral("%1：%2").arg(toQString(a.title)).arg(toQString(a.detail));
        default: return {};
        }
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    { return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags; }
    bool containsPath(const QString& path) const { return m_paths.contains(path); }
    void refreshIcons()
    {
        if (!m_rows.empty()) emit dataChanged(index(0, toNidsAlertColumn(NidsAlertTableColumn::ProcessName)),
            index(rowCount() - 1, toNidsAlertColumn(NidsAlertTableColumn::ProcessName)), {Qt::DecorationRole});
    }
    bool synchronize(std::vector<Alert> incoming)
    {
        const QPointer<NidsAlertModel> guard(this);
        std::size_t removed = 0;
        if (incoming.empty()) removed = m_rows.size();
        else while (removed < m_rows.size() && m_rows[removed] != incoming.front()) ++removed;
        if (removed != 0)
        {
            beginRemoveRows({}, 0, static_cast<int>(removed) - 1);
            if (!guard) return false;
            for (std::size_t i = 0; i < removed; ++i)
            {
                const auto path = QString::fromUtf8(m_rows[i]->processImagePath.c_str());
                auto found = m_paths.find(path);
                if (found != m_paths.end() && --found.value() == 0) m_paths.erase(found);
            }
            m_rows.erase(m_rows.begin(), m_rows.begin() + static_cast<std::ptrdiff_t>(removed));
            endRemoveRows();
            if (!guard) return false;
        }
        const auto kept = m_rows.size();
        if (incoming.size() > kept)
        {
            // Reserve before beginning a Qt insert; canonical values remain shared,
            // and data() formats only requested cells instead of building 11 items.
            m_rows.reserve(incoming.size());
            beginInsertRows({}, static_cast<int>(kept), static_cast<int>(incoming.size()) - 1);
            if (!guard) return false;
            for (std::size_t i = kept; i < incoming.size(); ++i)
            {
                ++m_paths[QString::fromUtf8(incoming[i]->processImagePath.c_str())];
                m_rows.push_back(std::move(incoming[i]));
            }
            endInsertRows();
            if (!guard) return false;
        }
        return true;
    }
private:
    QPointer<NetworkDock> m_owner;
    QStringList m_headers;
    std::vector<Alert> m_rows;
    QHash<QString, int> m_paths; // Rendering membership only; no process/identity lookup.
};

class NetworkDock::NidsAlertProxy final : public QSortFilterProxyModel
{
public:
    explicit NidsAlertProxy(QObject* parent) : QSortFilterProxyModel(parent) { setDynamicSortFilter(false); }
    void setMinimumSeverity(int value)
    {
        if (m_minimumSeverity == value) return;
        m_minimumSeverity = value;
        invalidateFilter();
    }
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        return sourceModel()->index(row, static_cast<int>(NidsAlertTableColumn::Severity), parent)
            .data(Qt::UserRole).toInt() >= m_minimumSeverity;
    }
private:
    int m_minimumSeverity = 0;
};


void NetworkDock::initializeNidsTab()
{
    m_nidsPage = new QWidget(this);
    m_nidsLayout = new QVBoxLayout(m_nidsPage);
    m_nidsLayout->setContentsMargins(6, 6, 6, 6);
    m_nidsLayout->setSpacing(6);

    // 实时开关、告警级别与清理动作保持同一行，用轻工具底区分下方告警列表。
    auto* alertToolbar = new QWidget(m_nidsPage);
    ks::ui::StylePrimaryToolbar(alertToolbar);
    m_nidsControlLayout = new QHBoxLayout(alertToolbar);
    m_nidsControlLayout->setContentsMargins(8, 4, 8, 4);
    m_nidsControlLayout->setSpacing(6);

    m_nidsEnableCheck = new QCheckBox(QStringLiteral("实时检测"), m_nidsPage);
    m_nidsEnableCheck->setChecked(true);
    m_nidsEnableCheck->setToolTip(QStringLiteral("启用或暂停 NIDS 实时报文检测"));

    QLabel* severityFilterLabel = new QLabel(QStringLiteral("等级:"), m_nidsPage);
    m_nidsSeverityFilterCombo = new QComboBox(m_nidsPage);
    ks::ui::StylePrimaryCombo(m_nidsSeverityFilterCombo);
    ks::ui::StyleStandaloneInput(m_nidsSeverityFilterCombo);
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("全部"), static_cast<int>(ks::network::NidsAlertSeverity::Low));
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("中危+"), static_cast<int>(ks::network::NidsAlertSeverity::Medium));
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("高危+"), static_cast<int>(ks::network::NidsAlertSeverity::High));
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("严重"), static_cast<int>(ks::network::NidsAlertSeverity::Critical));
    m_nidsSeverityFilterCombo->setToolTip(QStringLiteral("按最低告警等级过滤表格"));
    m_nidsSeverityFilterCombo->setMaximumWidth(120);

    m_nidsClearButton = new QPushButton(m_nidsPage);
    // 此动作使用纯色主题，保留页面原有图标、状态和业务连接。
    ks::ui::ApplyFlatButtonTheme(m_nidsClearButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);
    m_nidsClearButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    KswordTheme::ApplyCompactIconButtonMetrics(m_nidsClearButton);
    m_nidsClearButton->setToolTip(QStringLiteral("清空 NIDS 告警和检测窗口"));

    m_nidsStatusLabel = new QLabel(m_nidsPage);
    m_nidsStatusLabel->setWordWrap(true);
    m_nidsStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_nidsControlLayout->addWidget(m_nidsEnableCheck);
    m_nidsControlLayout->addWidget(severityFilterLabel);
    m_nidsControlLayout->addWidget(m_nidsSeverityFilterCombo);
    m_nidsControlLayout->addWidget(m_nidsClearButton);
    m_nidsControlLayout->addWidget(m_nidsStatusLabel, 1);
    m_nidsLayout->addWidget(alertToolbar);
    ks::ui::NormalizeToolbarRow(m_nidsControlLayout);

    m_nidsAlertTable = new ks::ui::TableActionTableView(m_nidsPage);
    ks::ui::SetTableActionBarMode(m_nidsAlertTable, ks::ui::TableActionBarMode::Full);
    m_nidsAlertModel = new NidsAlertModel(this, {
        QStringLiteral("时间"),
        QStringLiteral("等级"),
        QStringLiteral("分类"),
        QStringLiteral("规则"),
        QStringLiteral("协议"),
        QStringLiteral("方向"),
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点"),
        QStringLiteral("远端端点"),
        QStringLiteral("详情")
        });
    m_nidsAlertProxy = new NidsAlertProxy(this);
    m_nidsAlertProxy->setSourceModel(m_nidsAlertModel);
    m_nidsAlertTable->setModel(m_nidsAlertProxy);
    m_nidsAlertTable->setProperty("ks_i18n_preserve_model_data", true);
    m_nidsAlertTable->setSortingEnabled(true);
    m_nidsAlertProxy->sort(-1); // Preserve collection order until an explicit header sort.
    m_nidsAlertTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nidsAlertTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_nidsAlertTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_nidsAlertTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_nidsAlertTable->verticalHeader()->setVisible(false);
    m_nidsAlertTable->horizontalHeader()->setStretchLastSection(true);
    m_nidsAlertTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_nidsLayout->addWidget(m_nidsAlertTable, 1);

    connect(m_nidsAlertTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& localPosition)
    {
        // 右键菜单：
        // - 输入：用户在 NIDS 告警表中的点击位置；
        // - 处理：定位当前行，提供关联报文详情，以及单元格/当前行/多选行复制；
        // - 返回：无，菜单只查看和复制审计证据，不修改规则或连接。
        // 唯一性说明：本表的右键菜单只在这里注册。initializeConnections() 曾重复注册过一份
        // “详情类”菜单，两个槽各自 exec() 会让一次右键先后弹出两个内容不同的菜单。
        if (m_nidsAlertTable == nullptr)
        {
            return;
        }

        const QModelIndex clickedIndex = m_nidsAlertTable->indexAt(localPosition);
        if (clickedIndex.isValid())
        {
            m_nidsAlertTable->setCurrentIndex(clickedIndex);
        }

        QMenu menu(m_nidsAlertTable);
        menu.setStyleSheet(KswordTheme::ContextMenuStyle());
        // 关联报文详情：告警行在时间列的 UserRole 里记录了触发它的报文序号。
        std::uint64_t relatedPacketSequenceId = 0;
        const bool hasRelatedPacket = nidsAlertSequenceForRow(
            m_nidsAlertTable,
            m_nidsAlertTable->currentIndex().row(),
            toNidsAlertColumn(NidsAlertTableColumn::Time),
            relatedPacketSequenceId);
        QAction* viewPacketDetailAction = menu.addAction(
            QIcon(QStringLiteral(":/Icon/process_details.svg")),
            QStringLiteral("查看关联报文详情"));
        viewPacketDetailAction->setEnabled(hasRelatedPacket);
        menu.addSeparator();
        QAction* copyCellAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_copy.svg")), QStringLiteral("复制单元格"));
        // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
        ks::ui::SetTableContextActionRole(copyCellAction, ks::ui::TableContextActionRole::CopyCell);
        QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制当前行"));
        // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
        ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
        QAction* copySelectedRowsAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_clipboard.svg")), QStringLiteral("复制选中行"));
        // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
        ks::ui::SetTableContextActionRole(copySelectedRowsAction, ks::ui::TableContextActionRole::CopyRows);
        const int currentRow = m_nidsAlertTable->currentIndex().row();
        const QModelIndex processIdIndex = m_nidsAlertTable->model()->index(currentRow, toNidsAlertColumn(NidsAlertTableColumn::Pid));
        std::uint32_t processId = 0;
        const bool hasProcessId = processIdIndex.isValid() &&
            ks::online_scan::tryParsePidFromText(processIdIndex.data().toString(), &processId) &&
            processId != 0U;
        QAction* openProcessDetailAction = menu.addAction(
            QIcon(QStringLiteral(":/Icon/process_details.svg")),
            QStringLiteral("转到进程详细信息"));
        openProcessDetailAction->setEnabled(hasProcessId);
        std::optional<ks::network::NidsAlert> blockRuleEvidence;
        if (m_nidsAlertModel != nullptr && m_nidsAlertProxy != nullptr)
        {
            const auto evidence = m_nidsAlertModel->alertAt(m_nidsAlertProxy->mapToSource(
                m_nidsAlertTable->currentIndex()).row());
            if (evidence) blockRuleEvidence = *evidence; // Exact selected alert, including multi-rule packets.
        }
        QAction* addBlockRuleAction = menu.addAction(
            QIcon(QStringLiteral(":/Icon/process_terminate.svg")),
            QStringLiteral("预填阻断规则"));
        addBlockRuleAction->setEnabled(blockRuleEvidence.has_value() && m_firewallPage != nullptr);
        menu.addSeparator();
        QAction* uploadVirusTotalAction = ks::online_scan::addVirusTotalSandboxMenu(
            &menu,
            this,
            [this]() -> ks::online_scan::SandboxUploadTarget
            {
                // 输入：NIDS 告警表当前行。
                // 处理：读取 PID 列并解析发起进程 EXE 路径。
                // 返回：待上传路径和来源说明；PID 缺失或进程已退出时返回 errorText。
                ks::online_scan::SandboxUploadTarget uploadTarget;
                const int rowIndex = m_nidsAlertTable != nullptr ? m_nidsAlertTable->currentIndex().row() : -1;
                const QModelIndex pidIndex = m_nidsAlertTable != nullptr
                    ? m_nidsAlertTable->model()->index(rowIndex, toNidsAlertColumn(NidsAlertTableColumn::Pid)) : QModelIndex();
                std::uint32_t targetPid = 0;
                if (!pidIndex.isValid() || !ks::online_scan::tryParsePidFromText(pidIndex.data().toString(), &targetPid))
                {
                    uploadTarget.errorText = QStringLiteral("当前 NIDS 告警没有可解析 PID。");
                    return uploadTarget;
                }

                uploadTarget.filePath = QString::fromStdString(ks::process::QueryProcessPathByPid(targetPid));
                uploadTarget.sourceText = QStringLiteral("NIDS 告警 PID=%1").arg(targetPid);
                return uploadTarget;
            });
        const bool hasCurrentCell = m_nidsAlertTable->currentIndex().row() >= 0 && m_nidsAlertTable->currentIndex().column() >= 0;
        const bool hasCurrentRow = m_nidsAlertTable->currentIndex().row() >= 0;
        copyCellAction->setEnabled(hasCurrentCell);
        copyRowAction->setEnabled(hasCurrentRow);
        copySelectedRowsAction->setEnabled(!nidsSelectedRows(m_nidsAlertTable).empty());
        if (uploadVirusTotalAction != nullptr)
        {
            uploadVirusTotalAction->setEnabled(hasCurrentRow);
        }

        const QAction* selectedAction = menu.exec(m_nidsAlertTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == viewPacketDetailAction)
        {
            openPacketDetailWindowBySequenceId(relatedPacketSequenceId);
        }
        else if (selectedAction == addBlockRuleAction)
        {
            if (blockRuleEvidence.has_value() && m_firewallPage != nullptr)
            {
                const auto& evidence = *blockRuleEvidence;
                m_firewallPage->addBlockRuleFromEvidence(
                    QString::fromUtf8(evidence.remoteAddress.c_str()),
                    QString::number(evidence.remotePort),
                    toQString(ks::network::PacketProtocolToString(evidence.protocol)),
                    evidence.direction == ks::network::PacketDirection::Inbound
                        ? QStringLiteral("Inbound") : QStringLiteral("Outbound"),
                    QStringLiteral("NIDS"),
                    evidence.processId,
                    evidence.processCreationTime100ns,
                    QString::fromUtf8(evidence.processImagePath.c_str()));
            }
        }
        else if (selectedAction == copyCellAction)
        {
            const QModelIndex index = m_nidsAlertTable->currentIndex();
            if (index.isValid() && QApplication::clipboard() != nullptr)
                QApplication::clipboard()->setText(index.data().toString());
        }
        else if (selectedAction == copyRowAction)
        {
            copyNidsRowsToClipboard(m_nidsAlertTable, std::vector<int>{ m_nidsAlertTable->currentIndex().row() });
        }
        else if (selectedAction == copySelectedRowsAction)
        {
            copyNidsRowsToClipboard(m_nidsAlertTable, nidsSelectedRows(m_nidsAlertTable));
        }
        else if (selectedAction == openProcessDetailAction)
        {
            ks::ui::OpenProcessDetailByPid(processId);
        }
        else if (selectedAction == uploadVirusTotalAction)
        {
            return;
        }
    });

    updateNidsStatusLabel();
    network_navigation_detail::AddTab(m_sideTabWidget, m_nidsPage, QStringLiteral("nids"), QIcon(":/Icon/process_critical.svg"));

    kLogEvent initNidsEvent;
    info << initNidsEvent << "[NetworkDock] NIDS 页初始化完成。" << eol;
}

void NetworkDock::synchronizeVisibleNidsProjection()
{
    if (m_nidsPage == nullptr || !m_nidsPage->isVisible()) return;
    if (m_nidsProjectionNeedsRebuild) rebuildNidsAlertTable();
    updateNidsStatusLabel();
    scheduleNidsIconWork();
}

void NetworkDock::processNidsPacket(const ks::network::PacketRecord& packetRecord)
{
    if (m_nidsEnableCheck != nullptr && !m_nidsEnableCheck->isChecked()) return;
    if (!m_nidsAnalysisSession) m_nidsAnalysisSession = makeNidsAnalysisSession();
    m_nidsAnalysisSession->enqueue(packetRecord); // Full FIFO values; never latest-only packet replacement.
    scheduleNidsAnalysis();
}

void NetworkDock::scheduleNidsAnalysis()
{
    if (m_nidsAnalysisKickScheduled || m_nidsAnalysisFailed) return;
    m_nidsAnalysisKickScheduled = true;
    const QPointer<NetworkDock> guard(this);
    QTimer::singleShot(0, this, [guard]()
    {
        if (!guard) return;
        guard->m_nidsAnalysisKickScheduled = false;
        guard->launchNidsAnalysisBatch();
    });
}

void NetworkDock::launchNidsAnalysisBatch()
{
    if (m_nidsAnalysisFailed || !m_nidsAnalysisSession || m_nidsAnalysisSession->pendingCount() == 0) return;
    if (m_nidsAnalysisOperation == nullptr)
    {
        m_nidsAnalysisOperation = new ks::ui::AsyncOperation(this);
        const QPointer<NetworkDock> guard(this);
        m_nidsAnalysisOperation->setStateChangedCallback([guard]()
        {
            if (guard && !guard->m_nidsAnalysisOperation->isBusy()) guard->scheduleNidsAnalysis();
        });
    }
    if (m_nidsAnalysisOperation->isBusy()) return;
    const auto session = m_nidsAnalysisSession;
    const QPointer<NetworkDock> guard(this);
    // One actual worker owns the engine; only a later UI kick can submit again.
    m_nidsAnalysisOperation->submit<std::shared_ptr<NidsAnalysisSession>, std::shared_ptr<NidsAnalysisBatch>>(
        session,
        [](const auto& state, const ks::ui::AsyncOperationToken& token) -> std::shared_ptr<NidsAnalysisBatch>
        {
            if (token.isCanceled()) return {};
            return state->analyzeNextBatch(captureNidsProcessIdentity);
        },
        [guard, session](const auto& batch, std::uint64_t)
        {
            if (!guard || guard->m_nidsAnalysisSession != session || !batch) return true;
            guard->m_nidsAnalysisFailed = batch->failed;
            guard->applyNidsAnalysisBatch(batch);
            if (guard && guard->m_nidsAnalysisSession == session) session->acknowledge(batch);
            return true;
        },
        [guard, session](std::exception_ptr)
        {
            if (!guard || guard->m_nidsAnalysisSession != session) return;
            // A failed launch/allocation never silently advances the accepted FIFO.
            guard->m_nidsAnalysisFailed = true;
            guard->updateNidsStatusLabel();
        });
}

void NetworkDock::retireNidsAnalysisSession()
{
    m_nidsAnalysisSession.reset(); // Custom deleter schedules the last release off-thread.
}

void NetworkDock::applyNidsAnalysisBatch(const std::shared_ptr<NidsAnalysisBatch>& batch)
{
    m_nidsAnalyzedPacketCount += batch->analyzedPackets;
    if (batch->alerts.empty()) { updateNidsStatusLabel(); return; }
    const auto& alertList = batch->alerts;
    m_nidsProjectionNeedsRebuild = true;
    for (const ks::network::NidsAlert& alertRecord : alertList)
    {
        m_nidsAlertList.push_back(std::make_shared<const ks::network::NidsAlert>(alertRecord));
        const int appendedSeverity = nidsSeverityIndex(alertRecord.severity);
        if (appendedSeverity >= 0 && appendedSeverity < static_cast<int>(m_nidsRetainedSeverityCounts.size()))
            ++m_nidsRetainedSeverityCounts[static_cast<std::size_t>(appendedSeverity)];
        ++m_nidsTotalAlertCount;
        while (m_nidsAlertList.size() > kMaxNidsAlertCount)
        {
            const int removedSeverity = nidsSeverityIndex(m_nidsAlertList.front()->severity);
            if (removedSeverity >= 0 && removedSeverity < static_cast<int>(m_nidsRetainedSeverityCounts.size()))
                --m_nidsRetainedSeverityCounts[static_cast<std::size_t>(removedSeverity)];
            m_nidsAlertList.pop_front();
        }

        kLogEvent nidsAlertEvent;
        warn << nidsAlertEvent
            << "[NetworkDock] NIDS 告警, severity="
            << ks::network::NidsAlertSeverityToString(alertRecord.severity)
            << ", rule=" << alertRecord.ruleId
            << ", pid=" << alertRecord.processId
            << ", detail=" << alertRecord.detail
            << eol;
    }

    if (m_nidsProjectionNeedsRebuild && m_nidsPage != nullptr && m_nidsPage->isVisible())
    {
        rebuildNidsAlertTable();
    }

    updateNidsStatusLabel();
}

void NetworkDock::rebuildNidsAlertTable()
{
    m_nidsProjectionNeedsRebuild = true;
    if (m_nidsProjectionInProgress) return;
    if (m_nidsPage == nullptr || !m_nidsPage->isVisible()) return;
    if (m_nidsAlertTable == nullptr)
    {
        return;
    }

    const QPointer<NetworkDock> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("network-nids-filter-rebuild"),
        {m_nidsAlertTable},
        [safeThis]()
        {
            if (!safeThis.isNull())
            {
                safeThis->rebuildNidsAlertTable();
            }
        }))
    {
        return;
    }

    const int minSeverity = m_nidsSeverityFilterCombo != nullptr ? m_nidsSeverityFilterCombo->currentData().toInt() : 0;
    std::vector<NidsAlertModel::Alert> projection(m_nidsAlertList.cbegin(), m_nidsAlertList.cend());
    m_nidsProjectionNeedsRebuild = false;
    m_nidsProjectionInProgress = true;
    if (!m_nidsAlertModel->synchronize(std::move(projection)) || !safeThis) return;
    m_nidsAlertProxy->setMinimumSeverity(minSeverity);
    if (!safeThis) return;
    m_nidsProjectionInProgress = false;
    if (m_nidsProjectionNeedsRebuild)
        QTimer::singleShot(0, this, [safeThis]() { if (safeThis) safeThis->synchronizeVisibleNidsProjection(); });
    else if (m_nidsAlertProxy->rowCount() > 0) m_nidsAlertTable->scrollToBottom();

}

void NetworkDock::clearNidsAlerts()
{
    const QPointer<NetworkDock> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("network-nids-clear"),
        {m_nidsAlertTable},
        [safeThis]()
        {
            if (!safeThis.isNull())
            {
                safeThis->clearNidsAlerts();
            }
        }))
    {
        return;
    }

    // A new epoch owns a fresh engine. Old workers only retain the retired value
    // session; no GUI reset/destruction races their engine or accepted inputs.
    auto freshSession = makeNidsAnalysisSession();
    retireNidsAnalysisSession();
    m_nidsAnalysisSession = std::move(freshSession);
    m_nidsAnalysisFailed = false;
    m_nidsIconActivePaths.clear();
    m_nidsIconQueue.clear();
    m_nidsIconPendingPaths.clear();
    m_nidsIconCache.clear();
    if (m_nidsIconOperation != nullptr) m_nidsIconOperation->cancel();
    if (m_nidsAnalysisOperation != nullptr) m_nidsAnalysisOperation->cancel();
    m_nidsProjectionNeedsRebuild = false;
    m_nidsAlertList.clear();
    m_nidsRetainedSeverityCounts.fill(0);
    m_nidsAnalyzedPacketCount = 0;
    m_nidsTotalAlertCount = 0;
    if (m_nidsAlertModel != nullptr && !m_nidsProjectionInProgress)
    {
        m_nidsProjectionInProgress = true;
        if (!m_nidsAlertModel->synchronize({}) || !safeThis) return;
        m_nidsProjectionInProgress = false;
    }
    else m_nidsProjectionNeedsRebuild = true;
    updateNidsStatusLabel();

    kLogEvent clearNidsEvent;
    info << clearNidsEvent << "[NetworkDock] NIDS 告警与检测窗口已清空。" << eol;
}

void NetworkDock::updateNidsStatusLabel()
{
    if (m_nidsPage != nullptr && !m_nidsPage->isVisible()) return;
    if (m_nidsStatusLabel == nullptr)
    {
        return;
    }

    const auto lowCount = m_nidsRetainedSeverityCounts[0];
    const auto mediumCount = m_nidsRetainedSeverityCounts[1];
    const auto highCount = m_nidsRetainedSeverityCounts[2];
    const auto criticalCount = m_nidsRetainedSeverityCounts[3];

    const bool enabled = (m_nidsEnableCheck == nullptr || m_nidsEnableCheck->isChecked());
    m_nidsStatusLabel->setText(QStringLiteral("状态：%1 | 已分析 %2 包 | 告警 %3（低 %4 / 中 %5 / 高 %6 / 严重 %7）")
        .arg(m_nidsAnalysisFailed ? QStringLiteral("检测失败，请清空后重试")
             : (enabled ? QStringLiteral("实时检测中") : QStringLiteral("已暂停")))
        .arg(static_cast<qulonglong>(m_nidsAnalyzedPacketCount))
        .arg(static_cast<qulonglong>(m_nidsAlertList.size()))
        .arg(static_cast<qulonglong>(lowCount))
        .arg(static_cast<qulonglong>(mediumCount))
        .arg(static_cast<qulonglong>(highCount))
        .arg(static_cast<qulonglong>(criticalCount)));
    const auto pending = m_nidsAnalysisSession ? m_nidsAnalysisSession->pendingPacketCount() : 0;
    if (pending > 0) m_nidsStatusLabel->setText(m_nidsStatusLabel->text()
        + QStringLiteral(" | 等待分析 %1 包").arg(static_cast<qulonglong>(pending)));
}

QIcon NetworkDock::resolveNidsProcessIcon(const ks::network::NidsAlert& alert)
{
    const QString path = QString::fromUtf8(alert.processImagePath.c_str());
    if (path.isEmpty()) return QIcon(QStringLiteral(":/Icon/process_main.svg"));
    if (const auto* icon = m_nidsIconCache.object(path)) return *icon;
    if (!m_nidsIconPendingPaths.contains(path))
    {
        m_nidsIconPendingPaths.insert(path);
        m_nidsIconQueue.push_back(path);
        scheduleNidsIconWork();
    }
    return QIcon(QStringLiteral(":/Icon/process_main.svg"));
}

void NetworkDock::scheduleNidsIconWork()
{
    if (m_nidsIconKickScheduled || m_nidsIconQueue.empty()) return;
    m_nidsIconKickScheduled = true;
    const QPointer<NetworkDock> guard(this);
    QTimer::singleShot(0, this, [guard]()
    {
        if (!guard) return;
        guard->m_nidsIconKickScheduled = false;
        guard->launchNidsIconWork();
    });
}

void NetworkDock::launchNidsIconWork()
{
    if (m_nidsPage == nullptr || !m_nidsPage->isVisible()) return;
    if (m_nidsIconOperation == nullptr)
    {
        m_nidsIconOperation = new ks::ui::AsyncOperation(this);
        const QPointer<NetworkDock> guard(this);
        m_nidsIconOperation->setStateChangedCallback([guard]()
        {
            if (guard && !guard->m_nidsIconOperation->isBusy())
            {
                // A dropped result does not invoke apply/failure. Requeue only
                // selected paths still pending; successful paths were removed.
                for (const auto& path : guard->m_nidsIconActivePaths)
                    if (guard->m_nidsIconPendingPaths.contains(path)) guard->m_nidsIconQueue.push_back(path);
                guard->m_nidsIconActivePaths.clear();
                guard->scheduleNidsIconWork();
            }
        });
    }
    if (m_nidsIconOperation->isBusy()) return;
    QStringList paths;
    while (!m_nidsIconQueue.empty() && paths.size() < 32)
    {
        const auto path = m_nidsIconQueue.front();
        m_nidsIconQueue.pop_front();
        if (m_nidsAlertModel == nullptr || !m_nidsAlertModel->containsPath(path)) { m_nidsIconPendingPaths.remove(path); continue; }
        paths.push_back(path);
    }
    if (paths.empty()) return;
    m_nidsIconActivePaths = paths;
    const QPointer<NetworkDock> guard(this);
    m_nidsIconOperation->submit<QStringList, std::vector<NidsIconImage>>(
        paths, loadNidsIconImages,
        [guard, paths](const auto& images, std::uint64_t)
        {
            if (!guard) return true;
            for (const auto& path : paths) guard->m_nidsIconPendingPaths.remove(path);
            bool changed = false;
            for (const auto& image : images)
            {
                if (!guard) return true;
                if (guard->m_nidsAlertModel == nullptr || !guard->m_nidsAlertModel->containsPath(image.path)) continue;
                const QIcon icon = image.image.isNull() ? QIcon(QStringLiteral(":/Icon/process_main.svg"))
                    : QIcon(QPixmap::fromImage(image.image)); // GUI only
                guard->m_nidsIconCache.insert(image.path, new QIcon(icon));
                if (guard->m_nidsPage == nullptr || !guard->m_nidsPage->isVisible())
                { guard->m_nidsProjectionNeedsRebuild = true; continue; }
                changed = true;
            }
            if (changed && guard && guard->m_nidsAlertModel) guard->m_nidsAlertModel->refreshIcons();
            return true;
        },
        [guard, paths](std::exception_ptr)
        {
            if (guard) for (const auto& path : paths) guard->m_nidsIconPendingPaths.remove(path);
        });
}
