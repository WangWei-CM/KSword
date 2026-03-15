void NetworkDock::refreshConnectionTables()
{
    // 统一入口：同一时刻刷新 TCP 与 UDP，保证两个表看到的是同一时间段快照。
    refreshTcpConnectionTable();
    refreshUdpEndpointTable();

    if (m_connectionStatusLabel != nullptr)
    {
        const QString nowText = QDateTime::currentDateTime().toString("HH:mm:ss");
        m_connectionStatusLabel->setText(
            QStringLiteral("状态：TCP=%1 条, UDP=%2 条, 刷新于 %3")
            .arg(static_cast<int>(m_tcpConnectionCache.size()))
            .arg(static_cast<int>(m_udpEndpointCache.size()))
            .arg(nowText));
    }

    // 刷新频率较高，这里使用 dbg 级别避免 info 日志过于密集。
    kLogEvent refreshEvent;
    dbg << refreshEvent
        << "[NetworkDock] 刷新连接快照完成, tcpCount=" << m_tcpConnectionCache.size()
        << ", udpCount=" << m_udpEndpointCache.size()
        << eol;
}

void NetworkDock::refreshTcpConnectionTable()
{
    if (m_tcpConnectionTable == nullptr)
    {
        return;
    }

    std::vector<ks::network::TcpConnectionRecord> tcpSnapshot;
    std::string errorText;
    if (!ks::network::EnumerateTcpConnectionRecords(tcpSnapshot, &errorText))
    {
        if (m_connectionStatusLabel != nullptr)
        {
            m_connectionStatusLabel->setText(QStringLiteral("状态：TCP 刷新失败"));
        }

        kLogEvent refreshFailEvent;
        warn << refreshFailEvent
            << "[NetworkDock] 枚举 TCP 连接失败, detail=" << errorText
            << eol;
        return;
    }

    // 刷新缓存：终止连接时会通过行索引回查该缓存。
    m_tcpConnectionCache = std::move(tcpSnapshot);

    m_tcpConnectionTable->setUpdatesEnabled(false);
    m_tcpConnectionTable->setRowCount(0);

    int rowIndex = 0;
    for (const ks::network::TcpConnectionRecord& connectionRecord : m_tcpConnectionCache)
    {
        m_tcpConnectionTable->insertRow(rowIndex);

        const QString stateText = toQString(connectionRecord.tcpStateText);
        const QString pidText = QString::number(connectionRecord.processId);
        const QString processNameText = toQString(connectionRecord.processName);
        const QString localEndpointText = formatEndpointText(connectionRecord.localAddressText, connectionRecord.localPort);
        const QString remoteEndpointText = formatEndpointText(connectionRecord.remoteAddressText, connectionRecord.remotePort);

        m_tcpConnectionTable->setItem(rowIndex, toTcpConnectionColumn(TcpConnectionTableColumn::State), createPacketCell(stateText));
        m_tcpConnectionTable->setItem(rowIndex, toTcpConnectionColumn(TcpConnectionTableColumn::Pid), createPacketCell(pidText));
        QTableWidgetItem* processItem = createPacketCell(processNameText);
        processItem->setIcon(resolveProcessIconByPid(connectionRecord.processId, connectionRecord.processName));
        m_tcpConnectionTable->setItem(rowIndex, toTcpConnectionColumn(TcpConnectionTableColumn::ProcessName), processItem);
        m_tcpConnectionTable->setItem(rowIndex, toTcpConnectionColumn(TcpConnectionTableColumn::LocalEndpoint), createPacketCell(localEndpointText));
        m_tcpConnectionTable->setItem(rowIndex, toTcpConnectionColumn(TcpConnectionTableColumn::RemoteEndpoint), createPacketCell(remoteEndpointText));
        ++rowIndex;
    }

    m_tcpConnectionTable->setUpdatesEnabled(true);
}

void NetworkDock::refreshUdpEndpointTable()
{
    if (m_udpEndpointTable == nullptr)
    {
        return;
    }

    std::vector<ks::network::UdpEndpointRecord> udpSnapshot;
    std::string errorText;
    if (!ks::network::EnumerateUdpEndpointRecords(udpSnapshot, &errorText))
    {
        if (m_connectionStatusLabel != nullptr)
        {
            m_connectionStatusLabel->setText(QStringLiteral("状态：UDP 刷新失败"));
        }

        kLogEvent refreshFailEvent;
        warn << refreshFailEvent
            << "[NetworkDock] 枚举 UDP 端点失败, detail=" << errorText
            << eol;
        return;
    }

    m_udpEndpointCache = std::move(udpSnapshot);

    m_udpEndpointTable->setUpdatesEnabled(false);
    m_udpEndpointTable->setRowCount(0);

    int rowIndex = 0;
    for (const ks::network::UdpEndpointRecord& endpointRecord : m_udpEndpointCache)
    {
        m_udpEndpointTable->insertRow(rowIndex);

        const QString pidText = QString::number(endpointRecord.processId);
        const QString processNameText = toQString(endpointRecord.processName);
        const QString localEndpointText = formatEndpointText(endpointRecord.localAddressText, endpointRecord.localPort);

        m_udpEndpointTable->setItem(rowIndex, toUdpEndpointColumn(UdpEndpointTableColumn::Pid), createPacketCell(pidText));
        QTableWidgetItem* processItem = createPacketCell(processNameText);
        processItem->setIcon(resolveProcessIconByPid(endpointRecord.processId, endpointRecord.processName));
        m_udpEndpointTable->setItem(rowIndex, toUdpEndpointColumn(UdpEndpointTableColumn::ProcessName), processItem);
        m_udpEndpointTable->setItem(rowIndex, toUdpEndpointColumn(UdpEndpointTableColumn::LocalEndpoint), createPacketCell(localEndpointText));
        ++rowIndex;
    }

    m_udpEndpointTable->setUpdatesEnabled(true);
}

void NetworkDock::terminateSelectedTcpConnection()
{
    if (m_tcpConnectionTable == nullptr)
    {
        return;
    }

    const int selectedRow = m_tcpConnectionTable->currentRow();
    if (selectedRow < 0 || selectedRow >= static_cast<int>(m_tcpConnectionCache.size()))
    {
        QMessageBox::information(this, QStringLiteral("连接管理"), QStringLiteral("请先选中一条 TCP 连接。"));
        return;
    }

    const ks::network::TcpConnectionRecord& targetConnection = m_tcpConnectionCache[static_cast<std::size_t>(selectedRow)];
    const int userChoice = QMessageBox::question(
        this,
        QStringLiteral("终止 TCP 连接"),
        QStringLiteral("确认终止连接？\nPID=%1\n本地=%2:%3\n远端=%4:%5")
        .arg(targetConnection.processId)
        .arg(toQString(targetConnection.localAddressText))
        .arg(targetConnection.localPort)
        .arg(toQString(targetConnection.remoteAddressText))
        .arg(targetConnection.remotePort));
    if (userChoice != QMessageBox::Yes)
    {
        return;
    }

    std::string detailText;
    const bool terminateOk = ks::network::TerminateTcpConnectionByRecord(targetConnection, &detailText);
    if (terminateOk)
    {
        QMessageBox::information(this, QStringLiteral("连接管理"), QStringLiteral("连接终止请求已提交。"));

        kLogEvent terminateEvent;
        info << terminateEvent
            << "[NetworkDock] 终止 TCP 连接成功, pid=" << targetConnection.processId
            << ", local=" << targetConnection.localAddressText << ":" << targetConnection.localPort
            << ", remote=" << targetConnection.remoteAddressText << ":" << targetConnection.remotePort
            << ", detail=" << detailText
            << eol;
    }
    else
    {
        QMessageBox::warning(this, QStringLiteral("连接管理"),
            QStringLiteral("终止连接失败：%1").arg(toQString(detailText)));

        kLogEvent terminateFailEvent;
        warn << terminateFailEvent
            << "[NetworkDock] 终止 TCP 连接失败, pid=" << targetConnection.processId
            << ", detail=" << detailText
            << eol;
    }

    refreshConnectionTables();
}

void NetworkDock::copySelectedConnectionRowToClipboard(QTableWidget* tableWidget)
{
    if (tableWidget == nullptr)
    {
        return;
    }

    const int selectedRow = tableWidget->currentRow();
    if (selectedRow < 0)
    {
        return;
    }

    QStringList rowTextList;
    rowTextList.reserve(tableWidget->columnCount());
    for (int columnIndex = 0; columnIndex < tableWidget->columnCount(); ++columnIndex)
    {
        QTableWidgetItem* item = tableWidget->item(selectedRow, columnIndex);
        rowTextList.push_back(item == nullptr ? QString() : item->text());
    }

    if (QGuiApplication::clipboard() != nullptr)
    {
        QGuiApplication::clipboard()->setText(rowTextList.join('\t'));
    }

    kLogEvent copyRowEvent;
    dbg << copyRowEvent
        << "[NetworkDock] 已复制连接表行到剪贴板, tableColumns=" << tableWidget->columnCount()
        << ", row=" << selectedRow
        << eol;
}

