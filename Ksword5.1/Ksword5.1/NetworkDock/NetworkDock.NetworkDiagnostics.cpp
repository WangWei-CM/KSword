#include "NetworkDock.InternalCommon.h"

#include "../UI/CodeEditorWidget.h"
#include <QDir>

using namespace network_dock_detail;
// ============================================================
// NetworkDock.NetworkDiagnostics.cpp
// 作用：
// - ARP 缓存展示与编辑；
// - DNS 缓存展示与编辑；
// - 存活主机发现（ICMP 扫描）。
// - hosts 文件编辑。
// ============================================================

void NetworkDock::initializeArpCacheTab()
{
    m_arpCachePage = new QWidget(this);
    m_arpCacheLayout = new QVBoxLayout(m_arpCachePage);
    m_arpCacheLayout->setContentsMargins(6, 6, 6, 6);
    m_arpCacheLayout->setSpacing(6);

    m_arpCacheControlLayout = new QHBoxLayout();
    m_arpCacheControlLayout->setSpacing(6);

    m_refreshArpButton = new QPushButton(m_arpCachePage);
    m_refreshArpButton->setIcon(QIcon(":/Icon/process_refresh.svg"));
    m_refreshArpButton->setToolTip(QStringLiteral("刷新 ARP 缓存列表"));

    m_addArpButton = new QPushButton(m_arpCachePage);
    m_addArpButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_addArpButton->setToolTip(QStringLiteral("新增静态 ARP 缓存项"));

    m_removeArpButton = new QPushButton(m_arpCachePage);
    m_removeArpButton->setIcon(QIcon(":/Icon/process_uncritical.svg"));
    m_removeArpButton->setToolTip(QStringLiteral("删除选中的 ARP 缓存项"));

    m_flushArpButton = new QPushButton(m_arpCachePage);
    m_flushArpButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_flushArpButton->setToolTip(QStringLiteral("清空全部 ARP 缓存"));

    m_arpStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_arpCachePage);
    m_arpStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_arpCacheControlLayout->addWidget(m_refreshArpButton);
    m_arpCacheControlLayout->addWidget(m_addArpButton);
    m_arpCacheControlLayout->addWidget(m_removeArpButton);
    m_arpCacheControlLayout->addWidget(m_flushArpButton);
    m_arpCacheControlLayout->addWidget(m_arpStatusLabel, 1);
    m_arpCacheLayout->addLayout(m_arpCacheControlLayout);

    m_arpTable = new QTableWidget(m_arpCachePage);
    m_arpTable->setColumnCount(4);
    m_arpTable->setHorizontalHeaderLabels({
        QStringLiteral("IPv4地址"),
        QStringLiteral("MAC地址"),
        QStringLiteral("类型"),
        QStringLiteral("接口索引")
        });
    m_arpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_arpTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_arpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_arpTable->verticalHeader()->setVisible(false);
    m_arpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_arpTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_arpCacheLayout->addWidget(m_arpTable, 1);

    m_sideTabWidget->addTab(m_arpCachePage, QIcon(":/Icon/process_tree.svg"), QStringLiteral("ARP缓存"));
}

void NetworkDock::initializeDnsCacheTab()
{
    m_dnsCachePage = new QWidget(this);
    m_dnsCacheLayout = new QVBoxLayout(m_dnsCachePage);
    m_dnsCacheLayout->setContentsMargins(6, 6, 6, 6);
    m_dnsCacheLayout->setSpacing(6);

    m_dnsCacheControlLayout = new QHBoxLayout();
    m_dnsCacheControlLayout->setSpacing(6);

    m_refreshDnsButton = new QPushButton(m_dnsCachePage);
    m_refreshDnsButton->setIcon(QIcon(":/Icon/process_refresh.svg"));
    m_refreshDnsButton->setToolTip(QStringLiteral("刷新 DNS 缓存列表"));

    m_dnsEntryEdit = new QLineEdit(m_dnsCachePage);
    m_dnsEntryEdit->setPlaceholderText(QStringLiteral("输入要删除的 DNS 域名，或先在表格里选择"));
    m_dnsEntryEdit->setToolTip(QStringLiteral("删除指定域名的 DNS 缓存条目"));

    m_removeDnsButton = new QPushButton(m_dnsCachePage);
    m_removeDnsButton->setIcon(QIcon(":/Icon/process_uncritical.svg"));
    m_removeDnsButton->setToolTip(QStringLiteral("删除指定 DNS 缓存条目"));

    m_flushDnsButton = new QPushButton(m_dnsCachePage);
    m_flushDnsButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_flushDnsButton->setToolTip(QStringLiteral("清空 DNS 缓存"));

    m_dnsStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_dnsCachePage);
    m_dnsStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_dnsCacheControlLayout->addWidget(m_refreshDnsButton);
    m_dnsCacheControlLayout->addWidget(m_dnsEntryEdit, 1);
    m_dnsCacheControlLayout->addWidget(m_removeDnsButton);
    m_dnsCacheControlLayout->addWidget(m_flushDnsButton);
    m_dnsCacheControlLayout->addWidget(m_dnsStatusLabel, 1);
    m_dnsCacheLayout->addLayout(m_dnsCacheControlLayout);

    m_dnsTable = new QTableWidget(m_dnsCachePage);
    m_dnsTable->setColumnCount(3);
    m_dnsTable->setHorizontalHeaderLabels({
        QStringLiteral("域名"),
        QStringLiteral("记录类型"),
        QStringLiteral("标志")
        });
    m_dnsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_dnsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_dnsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dnsTable->verticalHeader()->setVisible(false);
    m_dnsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_dnsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_dnsCacheLayout->addWidget(m_dnsTable, 1);

    m_sideTabWidget->addTab(m_dnsCachePage, QIcon(":/Icon/process_list.svg"), QStringLiteral("DNS缓存"));
}

void NetworkDock::initializeAliveHostScanTab()
{
    m_aliveScanPage = new QWidget(this);
    m_aliveScanLayout = new QVBoxLayout(m_aliveScanPage);
    m_aliveScanLayout->setContentsMargins(6, 6, 6, 6);
    m_aliveScanLayout->setSpacing(6);

    m_aliveScanControlLayout = new QHBoxLayout();
    m_aliveScanControlLayout->setSpacing(6);

    QLabel* startIpLabel = new QLabel(QStringLiteral("起始IP:"), m_aliveScanPage);
    m_aliveScanStartIpEdit = new QLineEdit(m_aliveScanPage);
    m_aliveScanStartIpEdit->setPlaceholderText(QStringLiteral("例如 192.168.1.1"));
    m_aliveScanStartIpEdit->setText(QStringLiteral("192.168.1.1"));

    QLabel* endIpLabel = new QLabel(QStringLiteral("结束IP:"), m_aliveScanPage);
    m_aliveScanEndIpEdit = new QLineEdit(m_aliveScanPage);
    m_aliveScanEndIpEdit->setPlaceholderText(QStringLiteral("例如 192.168.1.254"));
    m_aliveScanEndIpEdit->setText(QStringLiteral("192.168.1.254"));

    QLabel* timeoutLabel = new QLabel(QStringLiteral("超时ms:"), m_aliveScanPage);
    m_aliveScanTimeoutSpin = new QSpinBox(m_aliveScanPage);
    m_aliveScanTimeoutSpin->setRange(50, 3000);
    m_aliveScanTimeoutSpin->setValue(220);

    m_startAliveScanButton = new QPushButton(m_aliveScanPage);
    m_startAliveScanButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_startAliveScanButton->setToolTip(QStringLiteral("开始扫描指定 IP 段的存活主机"));

    m_stopAliveScanButton = new QPushButton(m_aliveScanPage);
    m_stopAliveScanButton->setIcon(QIcon(":/Icon/process_pause.svg"));
    m_stopAliveScanButton->setToolTip(QStringLiteral("停止当前主机扫描任务"));
    m_stopAliveScanButton->setEnabled(false);

    m_aliveScanControlLayout->addWidget(startIpLabel);
    m_aliveScanControlLayout->addWidget(m_aliveScanStartIpEdit);
    m_aliveScanControlLayout->addWidget(endIpLabel);
    m_aliveScanControlLayout->addWidget(m_aliveScanEndIpEdit);
    m_aliveScanControlLayout->addWidget(timeoutLabel);
    m_aliveScanControlLayout->addWidget(m_aliveScanTimeoutSpin);
    m_aliveScanControlLayout->addWidget(m_startAliveScanButton);
    m_aliveScanControlLayout->addWidget(m_stopAliveScanButton);
    m_aliveScanLayout->addLayout(m_aliveScanControlLayout);

    m_aliveScanProgressBar = new QProgressBar(m_aliveScanPage);
    m_aliveScanProgressBar->setRange(0, 100);
    m_aliveScanProgressBar->setValue(0);
    m_aliveScanProgressBar->setFormat(QStringLiteral("0%"));
    m_aliveScanLayout->addWidget(m_aliveScanProgressBar);

    m_aliveScanStatusLabel = new QLabel(QStringLiteral("状态：待机"), m_aliveScanPage);
    m_aliveScanLayout->addWidget(m_aliveScanStatusLabel);

    m_aliveScanTable = new QTableWidget(m_aliveScanPage);
    m_aliveScanTable->setColumnCount(4);
    m_aliveScanTable->setHorizontalHeaderLabels({
        QStringLiteral("IP"),
        QStringLiteral("状态"),
        QStringLiteral("RTT(ms)"),
        QStringLiteral("详情")
        });
    m_aliveScanTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_aliveScanTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_aliveScanTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_aliveScanTable->verticalHeader()->setVisible(false);
    m_aliveScanTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_aliveScanTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_aliveScanLayout->addWidget(m_aliveScanTable, 1);

    m_sideTabWidget->addTab(m_aliveScanPage, QIcon(":/Icon/process_main.svg"), QStringLiteral("存活主机"));
}

void NetworkDock::initializeHostsFileEditorTab()
{
    m_hostsFileEditorPage = new QWidget(this);
    m_hostsFileEditorLayout = new QVBoxLayout(m_hostsFileEditorPage);
    m_hostsFileEditorLayout->setContentsMargins(0, 0, 0, 0);
    m_hostsFileEditorLayout->setSpacing(0);

    m_hostsFileEditor = new CodeEditorWidget(m_hostsFileEditorPage);
    m_hostsFileEditor->setReadOnly(false);
    m_hostsFileEditorLayout->addWidget(m_hostsFileEditor, 1);

    QString windowsDirectory = qEnvironmentVariable("WINDIR").trimmed();
    if (windowsDirectory.isEmpty())
    {
        windowsDirectory = QStringLiteral("C:/Windows");
    }
    const QString hostsFilePath = QDir(windowsDirectory)
        .absoluteFilePath(QStringLiteral("System32/drivers/etc/hosts"));

    if (m_hostsFileEditor->openLocalFile(hostsFilePath))
    {
        kLogEvent openHostsEvent;
        info << openHostsEvent
            << "[NetworkDock] hosts文件编辑页已加载, path="
            << hostsFilePath.toStdString()
            << eol;
    }
    else
    {
        m_hostsFileEditor->setCurrentFilePath(hostsFilePath);
        m_hostsFileEditor->setText(QStringLiteral(
            "# hosts 文件读取失败。\n"
            "# 目标路径: %1\n"
            "# 如需直接保存系统 hosts，请以管理员权限运行程序。").arg(hostsFilePath));

        kLogEvent openHostsFailEvent;
        warn << openHostsFailEvent
            << "[NetworkDock] hosts文件编辑页加载失败, path="
            << hostsFilePath.toStdString()
            << eol;
    }

    m_sideTabWidget->addTab(m_hostsFileEditorPage, QIcon(":/Icon/codeeditor_open.svg"), QStringLiteral("hosts文件编辑"));
}

void NetworkDock::refreshArpCacheTable()
{
    if (m_arpTable == nullptr)
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[NetworkDock] 开始刷新ARP缓存表。"
            << eol;
    }

    ULONG tableSize = 0;
    if (GetIpNetTable(nullptr, &tableSize, FALSE) != ERROR_INSUFFICIENT_BUFFER)
    {
        m_arpStatusLabel->setText(QStringLiteral("状态：读取ARP缓存失败"));
        kLogEvent event;
        err << event
            << "[NetworkDock] ARP缓存刷新失败：首次获取缓冲区大小失败。"
            << eol;
        return;
    }

    std::vector<std::uint8_t> buffer(tableSize, 0);
    PMIB_IPNETTABLE netTable = reinterpret_cast<PMIB_IPNETTABLE>(buffer.data());
    if (GetIpNetTable(netTable, &tableSize, FALSE) != NO_ERROR)
    {
        m_arpStatusLabel->setText(QStringLiteral("状态：读取ARP缓存失败"));
        kLogEvent event;
        err << event
            << "[NetworkDock] ARP缓存刷新失败：GetIpNetTable读取失败。"
            << eol;
        return;
    }

    m_arpTable->setRowCount(0);
    for (DWORD index = 0; index < netTable->dwNumEntries; ++index)
    {
        const MIB_IPNETROW& row = netTable->table[index];
        IN_ADDR ipAddress{};
        ipAddress.S_un.S_addr = row.dwAddr;
        char ipBuffer[32] = {};
        inet_ntop(AF_INET, &ipAddress, ipBuffer, static_cast<int>(sizeof(ipBuffer)));

        QStringList macParts;
        for (DWORD macIndex = 0; macIndex < row.dwPhysAddrLen; ++macIndex)
        {
            macParts.push_back(QString("%1").arg(row.bPhysAddr[macIndex], 2, 16, QChar('0')).toUpper());
        }

        QString typeText = QStringLiteral("Unknown");
        if (row.dwType == MIB_IPNET_TYPE_DYNAMIC) typeText = QStringLiteral("Dynamic");
        else if (row.dwType == MIB_IPNET_TYPE_STATIC) typeText = QStringLiteral("Static");
        else if (row.dwType == MIB_IPNET_TYPE_INVALID) typeText = QStringLiteral("Invalid");

        const int tableRow = m_arpTable->rowCount();
        m_arpTable->insertRow(tableRow);
        m_arpTable->setItem(tableRow, 0, new QTableWidgetItem(QString::fromLatin1(ipBuffer)));
        m_arpTable->setItem(tableRow, 1, new QTableWidgetItem(macParts.join('-')));
        m_arpTable->setItem(tableRow, 2, new QTableWidgetItem(typeText));
        m_arpTable->setItem(tableRow, 3, new QTableWidgetItem(QString::number(row.dwIndex)));
    }

    m_arpStatusLabel->setText(QString("状态：ARP缓存项 %1").arg(m_arpTable->rowCount()));

    kLogEvent event;
    info << event
        << "[NetworkDock] ARP缓存刷新完成, rowCount="
        << m_arpTable->rowCount()
        << eol;
}

void NetworkDock::addArpCacheEntry()
{
    kLogEvent startEvent;
    info << startEvent
        << "[NetworkDock] 用户触发新增ARP缓存项。"
        << eol;

    bool ok = false;
    const QString ipText = QInputDialog::getText(
        this,
        QStringLiteral("新增ARP"),
        QStringLiteral("IPv4地址:"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (!ok || ipText.isEmpty())
    {
        kLogEvent event;
        dbg << event
            << "[NetworkDock] 新增ARP取消：未输入IPv4地址。"
            << eol;
        return;
    }

    const QString macText = QInputDialog::getText(
        this,
        QStringLiteral("新增ARP"),
        QStringLiteral("MAC地址(AA-BB-CC-DD-EE-FF):"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (!ok || macText.isEmpty())
    {
        kLogEvent event;
        dbg << event
            << "[NetworkDock] 新增ARP取消：未输入MAC地址。"
            << eol;
        return;
    }

    const int interfaceIndex = QInputDialog::getInt(
        this,
        QStringLiteral("新增ARP"),
        QStringLiteral("接口索引(ifIndex):"),
        1,
        1,
        INT_MAX,
        1,
        &ok);
    if (!ok)
    {
        kLogEvent event;
        dbg << event
            << "[NetworkDock] 新增ARP取消：未输入接口索引。"
            << eol;
        return;
    }

    std::uint32_t ipHostOrder = 0;
    if (!tryParseIpv4Text(ipText, ipHostOrder))
    {
        kLogEvent event;
        warn << event
            << "[NetworkDock] 新增ARP失败：IPv4格式非法, ip="
            << ipText.toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("新增ARP"), QStringLiteral("IPv4 地址格式不正确。"));
        return;
    }

    QString normalizedMacText = macText;
    normalizedMacText.replace(':', '-');
    const QStringList macSegments = normalizedMacText.split('-', Qt::SkipEmptyParts);
    if (macSegments.size() != 6)
    {
        kLogEvent event;
        warn << event
            << "[NetworkDock] 新增ARP失败：MAC格式非法, mac="
            << macText.toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("新增ARP"), QStringLiteral("MAC 地址格式不正确。"));
        return;
    }

    MIB_IPNETROW row{};
    row.dwIndex = static_cast<DWORD>(interfaceIndex);
    row.dwAddr = htonl(ipHostOrder);
    row.dwType = MIB_IPNET_TYPE_STATIC;
    row.dwPhysAddrLen = 6;
    for (int segmentIndex = 0; segmentIndex < 6; ++segmentIndex)
    {
        bool segmentOk = false;
        const int segmentValue = macSegments[segmentIndex].toInt(&segmentOk, 16);
        if (!segmentOk || segmentValue < 0 || segmentValue > 255)
        {
            kLogEvent event;
            warn << event
                << "[NetworkDock] 新增ARP失败：MAC段非法, mac="
                << macText.toStdString()
                << eol;
            QMessageBox::warning(this, QStringLiteral("新增ARP"), QStringLiteral("MAC 地址段格式不正确。"));
            return;
        }
        row.bPhysAddr[segmentIndex] = static_cast<BYTE>(segmentValue);
    }

    const DWORD createResult = CreateIpNetEntry(&row);
    if (createResult != NO_ERROR)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 新增ARP失败, errorCode="
            << createResult
            << eol;
        QMessageBox::warning(this, QStringLiteral("新增ARP"), QString("CreateIpNetEntry 失败，错误码=%1").arg(createResult));
        return;
    }

    kLogEvent event;
    info << event
        << "[NetworkDock] 新增ARP成功, ip="
        << ipText.toStdString()
        << ", interfaceIndex="
        << interfaceIndex
        << eol;

    refreshArpCacheTable();
}

void NetworkDock::removeSelectedArpCacheEntry()
{
    if (m_arpTable == nullptr || m_arpTable->currentRow() < 0)
    {
        kLogEvent event;
        dbg << event
            << "[NetworkDock] 删除ARP取消：未选中行。"
            << eol;
        return;
    }

    const int selectedRow = m_arpTable->currentRow();
    const QString ipText = m_arpTable->item(selectedRow, 0) != nullptr
        ? m_arpTable->item(selectedRow, 0)->text().trimmed()
        : QString();
    const QString indexText = m_arpTable->item(selectedRow, 3) != nullptr
        ? m_arpTable->item(selectedRow, 3)->text().trimmed()
        : QString();

    std::uint32_t ipHostOrder = 0;
    bool indexOk = false;
    const DWORD interfaceIndex = static_cast<DWORD>(indexText.toUInt(&indexOk));
    if (!tryParseIpv4Text(ipText, ipHostOrder) || !indexOk)
    {
        kLogEvent event;
        warn << event
            << "[NetworkDock] 删除ARP失败：选中行解析失败, ip="
            << ipText.toStdString()
            << ", index="
            << indexText.toStdString()
            << eol;
        return;
    }

    MIB_IPNETROW deleteRow{};
    deleteRow.dwAddr = htonl(ipHostOrder);
    deleteRow.dwIndex = interfaceIndex;
    const DWORD deleteResult = DeleteIpNetEntry(&deleteRow);
    if (deleteResult != NO_ERROR)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 删除ARP失败, ip="
            << ipText.toStdString()
            << ", errorCode="
            << deleteResult
            << eol;
        QMessageBox::warning(this, QStringLiteral("删除ARP"), QString("DeleteIpNetEntry 失败，错误码=%1").arg(deleteResult));
        return;
    }

    kLogEvent event;
    info << event
        << "[NetworkDock] 删除ARP成功, ip="
        << ipText.toStdString()
        << ", interfaceIndex="
        << interfaceIndex
        << eol;
    refreshArpCacheTable();
}

void NetworkDock::flushArpCache()
{
    if (m_arpTable == nullptr)
    {
        return;
    }

    kLogEvent startEvent;
    info << startEvent
        << "[NetworkDock] 开始清空ARP缓存。"
        << eol;

    std::unordered_set<DWORD> interfaceIndexSet;
    for (int row = 0; row < m_arpTable->rowCount(); ++row)
    {
        bool indexOk = false;
        const DWORD interfaceIndex = static_cast<DWORD>(
            m_arpTable->item(row, 3)->text().toUInt(&indexOk));
        if (indexOk)
        {
            interfaceIndexSet.insert(interfaceIndex);
        }
    }

    for (DWORD interfaceIndex : interfaceIndexSet)
    {
        FlushIpNetTable(interfaceIndex);
    }

    kLogEvent event;
    info << event
        << "[NetworkDock] 清空ARP缓存完成, interfaceCount="
        << interfaceIndexSet.size()
        << eol;
    refreshArpCacheTable();
}

void NetworkDock::refreshDnsCacheTable()
{
    if (m_dnsTable == nullptr)
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[NetworkDock] 开始刷新DNS缓存表。"
            << eol;
    }

    using DnsGetCacheDataTableFn = BOOL(WINAPI*)(PVOID);
    struct DnsCacheEntryRecord
    {
        DnsCacheEntryRecord* next = nullptr;
        PWSTR name = nullptr;
        WORD type = 0;
        WORD dataLength = 0;
        DWORD flags = 0;
    };

    HMODULE dnsapiModule = GetModuleHandleW(L"dnsapi.dll");
    if (dnsapiModule == nullptr)
    {
        dnsapiModule = LoadLibraryW(L"dnsapi.dll");
    }
    if (dnsapiModule == nullptr)
    {
        m_dnsStatusLabel->setText(QStringLiteral("状态：dnsapi.dll 不可用"));
        kLogEvent event;
        err << event
            << "[NetworkDock] DNS缓存刷新失败：dnsapi.dll不可用。"
            << eol;
        return;
    }

    auto dnsGetCacheDataTable = reinterpret_cast<DnsGetCacheDataTableFn>(
        GetProcAddress(dnsapiModule, "DnsGetCacheDataTable"));
    if (dnsGetCacheDataTable == nullptr)
    {
        m_dnsStatusLabel->setText(QStringLiteral("状态：DnsGetCacheDataTable 不可用"));
        kLogEvent event;
        err << event
            << "[NetworkDock] DNS缓存刷新失败：DnsGetCacheDataTable不可用。"
            << eol;
        return;
    }

    DnsCacheEntryRecord rootEntry{};
    const BOOL queryOk = dnsGetCacheDataTable(&rootEntry);
    if (queryOk == FALSE)
    {
        m_dnsStatusLabel->setText(QStringLiteral("状态：读取DNS缓存失败"));
        kLogEvent event;
        err << event
            << "[NetworkDock] DNS缓存刷新失败：读取缓存表失败。"
            << eol;
        return;
    }

    m_dnsTable->setRowCount(0);
    int count = 0;
    for (DnsCacheEntryRecord* node = rootEntry.next; node != nullptr; node = node->next)
    {
        const int row = m_dnsTable->rowCount();
        m_dnsTable->insertRow(row);
        const QString nameText = node->name != nullptr ? QString::fromWCharArray(node->name) : QStringLiteral("<null>");
        m_dnsTable->setItem(row, 0, new QTableWidgetItem(nameText));
        m_dnsTable->setItem(row, 1, new QTableWidgetItem(QString::number(node->type)));
        m_dnsTable->setItem(row, 2, new QTableWidgetItem(QString("0x%1").arg(node->flags, 0, 16).toUpper()));
        ++count;
    }

    m_dnsStatusLabel->setText(QString("状态：DNS缓存项 %1").arg(count));

    kLogEvent event;
    info << event
        << "[NetworkDock] DNS缓存刷新完成, rowCount="
        << count
        << eol;
}

void NetworkDock::removeDnsCacheEntry()
{
    QString entryName = m_dnsEntryEdit != nullptr ? m_dnsEntryEdit->text().trimmed() : QString();
    if (entryName.isEmpty() && m_dnsTable != nullptr && m_dnsTable->currentRow() >= 0)
    {
        entryName = m_dnsTable->item(m_dnsTable->currentRow(), 0)->text().trimmed();
    }
    if (entryName.isEmpty())
    {
        kLogEvent event;
        dbg << event
            << "[NetworkDock] 删除DNS缓存取消：未指定域名。"
            << eol;
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[NetworkDock] 尝试删除DNS缓存项, entry="
            << entryName.toStdString()
            << eol;
    }

    using DnsFlushResolverCacheEntryFn = DNS_STATUS(WINAPI*)(PCWSTR);
    HMODULE dnsapiModule = GetModuleHandleW(L"dnsapi.dll");
    if (dnsapiModule == nullptr)
    {
        dnsapiModule = LoadLibraryW(L"dnsapi.dll");
    }
    if (dnsapiModule == nullptr)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 删除DNS缓存失败：dnsapi.dll不可用。"
            << eol;
        QMessageBox::warning(this, QStringLiteral("删除DNS缓存"), QStringLiteral("dnsapi.dll 不可用。"));
        return;
    }

    auto dnsFlushResolverCacheEntry = reinterpret_cast<DnsFlushResolverCacheEntryFn>(
        GetProcAddress(dnsapiModule, "DnsFlushResolverCacheEntry_W"));
    if (dnsFlushResolverCacheEntry == nullptr)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 删除DNS缓存失败：按项删除API不可用。"
            << eol;
        QMessageBox::warning(this, QStringLiteral("删除DNS缓存"), QStringLiteral("当前系统不支持按项删除 DNS 缓存。"));
        return;
    }

    const DNS_STATUS flushStatus = dnsFlushResolverCacheEntry(reinterpret_cast<PCWSTR>(entryName.utf16()));
    if (flushStatus != 0)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 删除DNS缓存失败, entry="
            << entryName.toStdString()
            << ", errorCode="
            << flushStatus
            << eol;
        QMessageBox::warning(this, QStringLiteral("删除DNS缓存"), QString("删除失败，错误码=%1").arg(flushStatus));
        return;
    }

    kLogEvent event;
    info << event
        << "[NetworkDock] 删除DNS缓存成功, entry="
        << entryName.toStdString()
        << eol;
    refreshDnsCacheTable();
}

void NetworkDock::flushDnsCache()
{
    kLogEvent startEvent;
    info << startEvent
        << "[NetworkDock] 尝试清空DNS缓存。"
        << eol;

    using DnsFlushResolverCacheFn = DNS_STATUS(WINAPI*)();
    HMODULE dnsapiModule = GetModuleHandleW(L"dnsapi.dll");
    if (dnsapiModule == nullptr)
    {
        dnsapiModule = LoadLibraryW(L"dnsapi.dll");
    }
    if (dnsapiModule == nullptr)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 清空DNS缓存失败：dnsapi.dll不可用。"
            << eol;
        QMessageBox::warning(this, QStringLiteral("清空DNS缓存"), QStringLiteral("dnsapi.dll 不可用。"));
        return;
    }

    auto dnsFlushResolverCache = reinterpret_cast<DnsFlushResolverCacheFn>(
        GetProcAddress(dnsapiModule, "DnsFlushResolverCache"));
    if (dnsFlushResolverCache == nullptr)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 清空DNS缓存失败：API不可用。"
            << eol;
        QMessageBox::warning(this, QStringLiteral("清空DNS缓存"), QStringLiteral("当前系统不支持清空 DNS 缓存 API。"));
        return;
    }

    const DNS_STATUS flushStatus = dnsFlushResolverCache();
    if (flushStatus != 0)
    {
        kLogEvent event;
        err << event
            << "[NetworkDock] 清空DNS缓存失败, errorCode="
            << flushStatus
            << eol;
        QMessageBox::warning(this, QStringLiteral("清空DNS缓存"), QString("清空失败，错误码=%1").arg(flushStatus));
        return;
    }

    kLogEvent event;
    info << event
        << "[NetworkDock] 清空DNS缓存成功。"
        << eol;
    refreshDnsCacheTable();
}

void NetworkDock::startAliveHostScan()
{
    if (m_aliveScanRunning.load())
    {
        kLogEvent event;
        dbg << event
            << "[NetworkDock] 忽略存活主机扫描启动：扫描已在进行中。"
            << eol;
        return;
    }

    std::uint32_t startIpHostOrder = 0;
    std::uint32_t endIpHostOrder = 0;
    if (!tryParseIpv4Text(m_aliveScanStartIpEdit->text().trimmed(), startIpHostOrder) ||
        !tryParseIpv4Text(m_aliveScanEndIpEdit->text().trimmed(), endIpHostOrder))
    {
        kLogEvent event;
        warn << event
            << "[NetworkDock] 启动存活主机扫描失败：IP地址格式非法, start="
            << m_aliveScanStartIpEdit->text().trimmed().toStdString()
            << ", end="
            << m_aliveScanEndIpEdit->text().trimmed().toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("存活主机扫描"), QStringLiteral("请输入正确的起止 IPv4 地址。"));
        return;
    }
    if (startIpHostOrder > endIpHostOrder)
    {
        std::swap(startIpHostOrder, endIpHostOrder);
    }

    const std::uint64_t hostCount = static_cast<std::uint64_t>(endIpHostOrder) - startIpHostOrder + 1;
    if (hostCount > 4096)
    {
        kLogEvent event;
        warn << event
            << "[NetworkDock] 启动存活主机扫描失败：扫描范围过大, hostCount="
            << hostCount
            << eol;
        QMessageBox::warning(this, QStringLiteral("存活主机扫描"), QStringLiteral("扫描范围过大，请控制在 4096 个主机以内。"));
        return;
    }

    m_aliveScanTable->setRowCount(0);
    m_aliveScanProgressBar->setValue(0);
    m_aliveScanStatusLabel->setText(QString("状态：正在扫描 %1 个主机").arg(hostCount));
    m_startAliveScanButton->setEnabled(false);
    m_stopAliveScanButton->setEnabled(true);

    if (m_aliveScanProgressPid == 0)
    {
        m_aliveScanProgressPid = kPro.add("网络", "存活主机扫描");
    }
    kPro.set(m_aliveScanProgressPid, "开始ICMP探测", 0, 0.0f);

    m_aliveScanRunning.store(true);
    m_aliveScanCancel.store(false);
    const int timeoutMs = m_aliveScanTimeoutSpin->value();

    {
        kLogEvent event;
        info << event
            << "[NetworkDock] 开始存活主机扫描, startIp="
            << formatIpv4HostOrder(startIpHostOrder).toStdString()
            << ", endIp="
            << formatIpv4HostOrder(endIpHostOrder).toStdString()
            << ", hostCount="
            << hostCount
            << ", timeoutMs="
            << timeoutMs
            << eol;
    }

    QPointer<NetworkDock> guardThis(this);
    auto* scanTask = QRunnable::create([guardThis, startIpHostOrder, endIpHostOrder, timeoutMs]()
        {
            const std::uint64_t totalCount =
                static_cast<std::uint64_t>(endIpHostOrder) - startIpHostOrder + 1;
            std::atomic<std::uint64_t> nextIpHostOrder{ startIpHostOrder };
            std::atomic<std::uint64_t> finishedCount{ 0 };
            std::atomic<std::uint64_t> aliveCount{ 0 };
            std::atomic<std::uint32_t> icmpHandleFailCount{ 0 };

            unsigned int workerCount = std::thread::hardware_concurrency();
            if (workerCount == 0)
            {
                workerCount = 8;
            }
            workerCount = std::clamp(workerCount, 2u, 32u);

            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (unsigned int workerIndex = 0; workerIndex < workerCount; ++workerIndex)
            {
                workers.emplace_back([guardThis,
                    endIpHostOrder,
                    timeoutMs,
                    totalCount,
                    &nextIpHostOrder,
                    &finishedCount,
                    &aliveCount,
                    &icmpHandleFailCount]()
                    {
                        HANDLE icmpHandle = IcmpCreateFile();
                        if (icmpHandle == INVALID_HANDLE_VALUE)
                        {
                            ++icmpHandleFailCount;
                            return;
                        }

                        char sendData[] = "KSWORD";
                        while (true)
                        {
                            if (guardThis == nullptr || guardThis->m_aliveScanCancel.load())
                            {
                                break;
                            }

                            const std::uint64_t currentIpTicket = nextIpHostOrder.fetch_add(1);
                            if (currentIpTicket > endIpHostOrder)
                            {
                                break;
                            }
                            const std::uint32_t currentIpHostOrder = static_cast<std::uint32_t>(currentIpTicket);

                            std::uint8_t replyBuffer[sizeof(ICMP_ECHO_REPLY) + 64] = {};
                            const DWORD replyCount = IcmpSendEcho(
                                icmpHandle,
                                htonl(currentIpHostOrder),
                                sendData,
                                static_cast<WORD>(sizeof(sendData)),
                                nullptr,
                                replyBuffer,
                                static_cast<DWORD>(sizeof(replyBuffer)),
                                static_cast<DWORD>(timeoutMs));

                            bool alive = false;
                            std::uint32_t rttMs = 0;
                            QString detailText = QStringLiteral("Timeout");
                            if (replyCount > 0)
                            {
                                const auto* echoReply = reinterpret_cast<const ICMP_ECHO_REPLY*>(replyBuffer);
                                alive = (echoReply->Status == IP_SUCCESS);
                                rttMs = echoReply->RoundTripTime;
                                detailText = alive
                                    ? QString("TTL=%1").arg(echoReply->Options.Ttl)
                                    : QString("Status=%1").arg(echoReply->Status);
                            }

                            const std::uint64_t doneCount = finishedCount.fetch_add(1) + 1;
                            const std::uint64_t aliveNow = alive
                                ? (aliveCount.fetch_add(1) + 1)
                                : aliveCount.load();
                            const QString ipText = formatIpv4HostOrder(currentIpHostOrder);
                            QMetaObject::invokeMethod(
                                guardThis,
                                [guardThis, ipText, alive, rttMs, detailText, doneCount, totalCount, aliveNow]()
                                {
                                    if (guardThis == nullptr)
                                    {
                                        return;
                                    }
                                    // 结果列表只展示存活主机，Down 主机不入表。
                                    if (alive)
                                    {
                                        guardThis->appendAliveHostRow(ipText, true, rttMs, detailText);
                                    }
                                    const int progressValue = totalCount == 0
                                        ? 0
                                        : static_cast<int>((doneCount * 100ULL) / totalCount);
                                    guardThis->m_aliveScanProgressBar->setValue(progressValue);
                                    guardThis->m_aliveScanProgressBar->setFormat(QString("%1%").arg(progressValue));
                                    guardThis->m_aliveScanStatusLabel->setText(
                                        QString("状态：扫描中 %1/%2，存活 %3")
                                        .arg(doneCount)
                                        .arg(totalCount)
                                        .arg(aliveNow));
                                    kPro.set(guardThis->m_aliveScanProgressPid, "ICMP探测中", 0, static_cast<float>(progressValue));
                                },
                                Qt::QueuedConnection);
                        }

                        IcmpCloseHandle(icmpHandle);
                    });
            }

            for (std::thread& workerThread : workers)
            {
                if (workerThread.joinable())
                {
                    workerThread.join();
                }
            }

            const std::uint64_t finishedCountValue = finishedCount.load();
            const std::uint64_t aliveCountValue = aliveCount.load();
            const std::uint32_t icmpHandleFailCountValue = icmpHandleFailCount.load();

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, totalCount, finishedCountValue, aliveCountValue, icmpHandleFailCountValue, workerCount]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->m_aliveScanRunning.store(false);
                    guardThis->m_startAliveScanButton->setEnabled(true);
                    guardThis->m_stopAliveScanButton->setEnabled(false);
                    const bool canceled = guardThis->m_aliveScanCancel.load();
                    const int progressValue = totalCount == 0
                        ? 0
                        : static_cast<int>((finishedCountValue * 100ULL) / totalCount);
                    guardThis->m_aliveScanProgressBar->setValue(progressValue);
                    guardThis->m_aliveScanProgressBar->setFormat(QString("%1%").arg(progressValue));

                    QString statusText;
                    if (icmpHandleFailCountValue >= workerCount)
                    {
                        statusText = QStringLiteral("状态：ICMP句柄创建失败，扫描未执行");
                    }
                    else if (canceled)
                    {
                        statusText = QString("状态：已停止，已探测 %1/%2，存活 %3")
                            .arg(finishedCountValue)
                            .arg(totalCount)
                            .arg(aliveCountValue);
                    }
                    else
                    {
                        statusText = QString("状态：扫描完成，存活 %1 台").arg(aliveCountValue);
                    }
                    guardThis->m_aliveScanStatusLabel->setText(statusText);
                    kPro.set(
                        guardThis->m_aliveScanProgressPid,
                        canceled ? "ICMP扫描已停止" : "ICMP扫描完成",
                        0,
                        canceled ? static_cast<float>(progressValue) : 100.0f);

                    kLogEvent event;
                    info << event
                        << "[NetworkDock] 存活主机扫描结束, resultRowCount="
                        << guardThis->m_aliveScanTable->rowCount()
                        << ", finished="
                        << finishedCountValue
                        << ", total="
                        << totalCount
                        << ", alive="
                        << aliveCountValue
                        << ", icmpHandleFailCount="
                        << icmpHandleFailCountValue
                        << ", canceled="
                        << (guardThis->m_aliveScanCancel.load() ? "true" : "false")
                        << eol;
                },
                Qt::QueuedConnection);
        });
    QThreadPool::globalInstance()->start(scanTask);
}

void NetworkDock::stopAliveHostScan()
{
    m_aliveScanCancel.store(true);
    m_aliveScanStatusLabel->setText(QStringLiteral("状态：正在停止扫描..."));

    kLogEvent event;
    info << event
        << "[NetworkDock] 用户请求停止存活主机扫描。"
        << eol;
}

void NetworkDock::appendAliveHostRow(
    const QString& ipText,
    const bool alive,
    const std::uint32_t rttMs,
    const QString& detailText)
{
    if (m_aliveScanTable == nullptr)
    {
        return;
    }
    if (!alive)
    {
        // 结果表仅保留 Alive 项，Down/Timeout 不写入列表。
        return;
    }

    const int row = m_aliveScanTable->rowCount();
    m_aliveScanTable->insertRow(row);
    m_aliveScanTable->setItem(row, 0, new QTableWidgetItem(ipText));
    auto* stateItem = new QTableWidgetItem(QStringLiteral("Alive"));
    stateItem->setForeground(QColor("#228B22"));
    stateItem->setTextAlignment(Qt::AlignCenter);
    m_aliveScanTable->setItem(row, 1, stateItem);
    m_aliveScanTable->setItem(row, 2, new QTableWidgetItem(QString::number(rttMs)));
    m_aliveScanTable->setItem(row, 3, new QTableWidgetItem(detailText));
}


