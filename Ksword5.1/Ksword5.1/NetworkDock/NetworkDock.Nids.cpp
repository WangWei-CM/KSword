#include "NetworkDock.InternalCommon.h"

#include <QBrush>

using namespace network_dock_detail;

namespace
{
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
            return QColor(99, 102, 106);
        case ks::network::NidsAlertSeverity::Medium:
            return QColor(202, 138, 4);
        case ks::network::NidsAlertSeverity::High:
            return QColor(220, 38, 38);
        case ks::network::NidsAlertSeverity::Critical:
            return QColor(147, 51, 234);
        default:
            return QColor(99, 102, 106);
        }
    }

    // createNidsCell：创建统一的只读 NIDS 表格单元格。
    QTableWidgetItem* createNidsCell(const QString& cellText)
    {
        QTableWidgetItem* tableItem = new QTableWidgetItem(cellText);
        tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        return tableItem;
    }

    // nidsSeverityIndex：等级转 int，用于过滤和排序。
    int nidsSeverityIndex(const ks::network::NidsAlertSeverity severity)
    {
        return static_cast<int>(severity);
    }
}

void NetworkDock::initializeNidsTab()
{
    m_nidsPage = new QWidget(this);
    m_nidsLayout = new QVBoxLayout(m_nidsPage);
    m_nidsLayout->setContentsMargins(6, 6, 6, 6);
    m_nidsLayout->setSpacing(6);

    m_nidsControlLayout = new QHBoxLayout();
    m_nidsControlLayout->setSpacing(6);

    m_nidsEnableCheck = new QCheckBox(QStringLiteral("实时检测"), m_nidsPage);
    m_nidsEnableCheck->setChecked(true);
    m_nidsEnableCheck->setToolTip(QStringLiteral("启用或暂停 NIDS 实时报文检测"));

    QLabel* severityFilterLabel = new QLabel(QStringLiteral("等级:"), m_nidsPage);
    m_nidsSeverityFilterCombo = new QComboBox(m_nidsPage);
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("全部"), static_cast<int>(ks::network::NidsAlertSeverity::Low));
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("中危+"), static_cast<int>(ks::network::NidsAlertSeverity::Medium));
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("高危+"), static_cast<int>(ks::network::NidsAlertSeverity::High));
    m_nidsSeverityFilterCombo->addItem(QStringLiteral("严重"), static_cast<int>(ks::network::NidsAlertSeverity::Critical));
    m_nidsSeverityFilterCombo->setToolTip(QStringLiteral("按最低告警等级过滤表格"));
    m_nidsSeverityFilterCombo->setMaximumWidth(120);

    m_nidsClearButton = new QPushButton(m_nidsPage);
    m_nidsClearButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_nidsClearButton->setToolTip(QStringLiteral("清空 NIDS 告警和检测窗口"));

    m_nidsStatusLabel = new QLabel(m_nidsPage);
    m_nidsStatusLabel->setWordWrap(true);
    m_nidsStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_nidsControlLayout->addWidget(m_nidsEnableCheck);
    m_nidsControlLayout->addWidget(severityFilterLabel);
    m_nidsControlLayout->addWidget(m_nidsSeverityFilterCombo);
    m_nidsControlLayout->addWidget(m_nidsClearButton);
    m_nidsControlLayout->addWidget(m_nidsStatusLabel, 1);
    m_nidsLayout->addLayout(m_nidsControlLayout);

    m_nidsAlertTable = new QTableWidget(m_nidsPage);
    m_nidsAlertTable->setColumnCount(toNidsAlertColumn(NidsAlertTableColumn::Count));
    m_nidsAlertTable->setHorizontalHeaderLabels({
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
    m_nidsAlertTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nidsAlertTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_nidsAlertTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_nidsAlertTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_nidsAlertTable->verticalHeader()->setVisible(false);
    m_nidsAlertTable->horizontalHeader()->setStretchLastSection(true);
    m_nidsAlertTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_nidsLayout->addWidget(m_nidsAlertTable, 1);

    updateNidsStatusLabel();
    m_sideTabWidget->addTab(m_nidsPage, QIcon(":/Icon/process_critical.svg"), QStringLiteral("NIDS"));

    kLogEvent initNidsEvent;
    info << initNidsEvent << "[NetworkDock] NIDS 页初始化完成。" << eol;
}

void NetworkDock::processNidsPacket(const ks::network::PacketRecord& packetRecord)
{
    if (m_nidsEnableCheck != nullptr && !m_nidsEnableCheck->isChecked())
    {
        return;
    }

    ++m_nidsAnalyzedPacketCount;

    std::vector<ks::network::NidsAlert> alertList = m_nidsEngine.AnalyzePacket(packetRecord);
    if (alertList.empty())
    {
        if ((m_nidsAnalyzedPacketCount % 256) == 0)
        {
            updateNidsStatusLabel();
        }
        return;
    }

    bool trimmed = false;
    for (const ks::network::NidsAlert& alertRecord : alertList)
    {
        m_nidsAlertList.push_back(alertRecord);
        ++m_nidsTotalAlertCount;
        while (m_nidsAlertList.size() > kMaxNidsAlertCount)
        {
            m_nidsAlertList.pop_front();
            trimmed = true;
        }

        if (!trimmed && nidsAlertPassesFilter(alertRecord))
        {
            appendNidsAlertRow(alertRecord);
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

    if (trimmed)
    {
        rebuildNidsAlertTable();
    }
    else if (m_nidsAlertTable != nullptr && m_nidsAlertTable->rowCount() > 0)
    {
        m_nidsAlertTable->scrollToBottom();
    }

    updateNidsStatusLabel();
}

void NetworkDock::appendNidsAlertRow(const ks::network::NidsAlert& alertRecord)
{
    if (m_nidsAlertTable == nullptr)
    {
        return;
    }

    const int newRow = m_nidsAlertTable->rowCount();
    m_nidsAlertTable->setRowCount(newRow + 1);

    const QString timeText = toQString(ks::network::FormatUnixTimestampMs(alertRecord.timestampMs, false));
    const QString severityText = nidsSeverityText(alertRecord.severity);
    const QString protocolText = toQString(ks::network::PacketProtocolToString(alertRecord.protocol));
    const QString directionText = toQString(ks::network::PacketDirectionToString(alertRecord.direction));
    const QString pidText = QString::number(alertRecord.processId);
    const QString processNameText = toQString(alertRecord.processName);
    const QString localEndpointText = formatEndpointText(alertRecord.localAddress, alertRecord.localPort);
    const QString remoteEndpointText = formatEndpointText(alertRecord.remoteAddress, alertRecord.remotePort);
    const QString detailText = QStringLiteral("%1：%2")
        .arg(toQString(alertRecord.title))
        .arg(toQString(alertRecord.detail));

    QTableWidgetItem* timeItem = createNidsCell(timeText);
    timeItem->setData(Qt::UserRole, static_cast<qulonglong>(alertRecord.sequenceId));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Time), timeItem);

    QTableWidgetItem* severityItem = createNidsCell(severityText);
    severityItem->setForeground(QBrush(nidsSeverityColor(alertRecord.severity)));
    severityItem->setData(Qt::UserRole, nidsSeverityIndex(alertRecord.severity));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Severity), severityItem);

    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Category), createNidsCell(toQString(alertRecord.category)));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Rule), createNidsCell(toQString(alertRecord.ruleId)));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Protocol), createNidsCell(protocolText));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Direction), createNidsCell(directionText));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Pid), createNidsCell(pidText));

    QTableWidgetItem* processNameItem = createNidsCell(processNameText);
    processNameItem->setIcon(resolveProcessIconByPid(alertRecord.processId, alertRecord.processName));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::ProcessName), processNameItem);

    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::LocalEndpoint), createNidsCell(localEndpointText));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::RemoteEndpoint), createNidsCell(remoteEndpointText));
    m_nidsAlertTable->setItem(newRow, toNidsAlertColumn(NidsAlertTableColumn::Detail), createNidsCell(detailText));
}

void NetworkDock::rebuildNidsAlertTable()
{
    if (m_nidsAlertTable == nullptr)
    {
        return;
    }

    m_nidsAlertTable->setUpdatesEnabled(false);
    m_nidsAlertTable->setRowCount(0);
    for (const ks::network::NidsAlert& alertRecord : m_nidsAlertList)
    {
        if (!nidsAlertPassesFilter(alertRecord))
        {
            continue;
        }
        appendNidsAlertRow(alertRecord);
    }
    m_nidsAlertTable->setUpdatesEnabled(true);
    if (m_nidsAlertTable->rowCount() > 0)
    {
        m_nidsAlertTable->scrollToBottom();
    }
}

void NetworkDock::clearNidsAlerts()
{
    m_nidsEngine.Reset();
    m_nidsAlertList.clear();
    m_nidsAnalyzedPacketCount = 0;
    m_nidsTotalAlertCount = 0;
    if (m_nidsAlertTable != nullptr)
    {
        m_nidsAlertTable->setRowCount(0);
    }
    updateNidsStatusLabel();

    kLogEvent clearNidsEvent;
    info << clearNidsEvent << "[NetworkDock] NIDS 告警与检测窗口已清空。" << eol;
}

void NetworkDock::updateNidsStatusLabel()
{
    if (m_nidsStatusLabel == nullptr)
    {
        return;
    }

    std::size_t lowCount = 0;
    std::size_t mediumCount = 0;
    std::size_t highCount = 0;
    std::size_t criticalCount = 0;
    for (const ks::network::NidsAlert& alertRecord : m_nidsAlertList)
    {
        switch (alertRecord.severity)
        {
        case ks::network::NidsAlertSeverity::Low:
            ++lowCount;
            break;
        case ks::network::NidsAlertSeverity::Medium:
            ++mediumCount;
            break;
        case ks::network::NidsAlertSeverity::High:
            ++highCount;
            break;
        case ks::network::NidsAlertSeverity::Critical:
            ++criticalCount;
            break;
        default:
            break;
        }
    }

    const bool enabled = (m_nidsEnableCheck == nullptr || m_nidsEnableCheck->isChecked());
    m_nidsStatusLabel->setText(QStringLiteral("状态：%1 | 已分析 %2 包 | 告警 %3（低 %4 / 中 %5 / 高 %6 / 严重 %7）")
        .arg(enabled ? QStringLiteral("实时检测中") : QStringLiteral("已暂停"))
        .arg(static_cast<qulonglong>(m_nidsAnalyzedPacketCount))
        .arg(static_cast<qulonglong>(m_nidsAlertList.size()))
        .arg(static_cast<qulonglong>(lowCount))
        .arg(static_cast<qulonglong>(mediumCount))
        .arg(static_cast<qulonglong>(highCount))
        .arg(static_cast<qulonglong>(criticalCount)));
}

bool NetworkDock::nidsAlertPassesFilter(const ks::network::NidsAlert& alertRecord) const
{
    if (m_nidsSeverityFilterCombo == nullptr)
    {
        return true;
    }

    const int minSeverity = m_nidsSeverityFilterCombo->currentData().toInt();
    return nidsSeverityIndex(alertRecord.severity) >= minSeverity;
}
