void NetworkDock::applyOrUpdateRateLimitRule()
{
    if (m_trafficService == nullptr || m_rateLimitPidEdit == nullptr ||
        m_rateLimitKBpsSpin == nullptr || m_rateLimitSuspendMsSpin == nullptr)
    {
        return;
    }

    std::uint32_t targetPid = 0;
    if (!tryParsePidText(m_rateLimitPidEdit->text(), targetPid))
    {
        QMessageBox::warning(this, QStringLiteral("进程限速"), QStringLiteral("请输入有效的 PID。"));
        return;
    }

    ks::network::ProcessRateLimitRule limitRule;
    limitRule.processId = targetPid;
    limitRule.bytesPerSecond = static_cast<std::uint64_t>(m_rateLimitKBpsSpin->value()) * 1024ULL;
    limitRule.suspendDurationMs = static_cast<std::uint32_t>(m_rateLimitSuspendMsSpin->value());
    limitRule.enabled = true;

    m_trafficService->UpsertRateLimitRule(limitRule);
    refreshRateLimitTable();

    appendRateLimitActionLogLine(QStringLiteral("更新限速规则：PID=%1, %2 KB/s, suspend=%3 ms")
        .arg(targetPid)
        .arg(m_rateLimitKBpsSpin->value())
        .arg(m_rateLimitSuspendMsSpin->value()));

    kLogEvent limitEvent;
    info << limitEvent
        << "[NetworkDock] 设置限速规则, pid=" << targetPid
        << ", bytesPerSecond=" << limitRule.bytesPerSecond
        << ", suspendMs=" << limitRule.suspendDurationMs
        << eol;
}

void NetworkDock::removeSelectedRateLimitRule()
{
    if (m_trafficService == nullptr || m_rateLimitTable == nullptr)
    {
        return;
    }

    const int selectedRow = m_rateLimitTable->currentRow();
    if (selectedRow < 0)
    {
        QMessageBox::information(this, QStringLiteral("进程限速"), QStringLiteral("请先选中一条规则。"));
        return;
    }

    QTableWidgetItem* pidItem = m_rateLimitTable->item(selectedRow, toRateLimitColumn(RateLimitTableColumn::Pid));
    if (pidItem == nullptr)
    {
        return;
    }

    std::uint32_t targetPid = 0;
    if (!tryParsePidText(pidItem->text(), targetPid))
    {
        return;
    }

    m_trafficService->RemoveRateLimitRule(targetPid);
    refreshRateLimitTable();
    appendRateLimitActionLogLine(QStringLiteral("删除限速规则：PID=%1").arg(targetPid));
}

void NetworkDock::clearAllRateLimitRules()
{
    if (m_trafficService == nullptr)
    {
        return;
    }

    const int userChoice = QMessageBox::question(
        this,
        QStringLiteral("进程限速"),
        QStringLiteral("确认清空全部限速规则吗？"));
    if (userChoice != QMessageBox::Yes)
    {
        return;
    }

    m_trafficService->ClearRateLimitRules();
    refreshRateLimitTable();
    appendRateLimitActionLogLine(QStringLiteral("已清空全部限速规则。"));
}

void NetworkDock::refreshRateLimitTable()
{
    if (m_rateLimitTable == nullptr || m_trafficService == nullptr)
    {
        return;
    }

    const std::vector<ks::network::ProcessRateLimitSnapshot> snapshots =
        m_trafficService->SnapshotRateLimitRules();

    m_rateLimitTable->setRowCount(0);
    int rowIndex = 0;
    for (const ks::network::ProcessRateLimitSnapshot& snapshot : snapshots)
    {
        m_rateLimitTable->insertRow(rowIndex);

        const std::uint32_t processId = snapshot.rule.processId;
        const std::string processName = ks::process::GetProcessNameByPID(processId);
        const QString stateText = snapshot.currentlySuspended ? QStringLiteral("已挂起") : QStringLiteral("运行中");

        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::Pid),
            createPacketCell(QString::number(processId)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::ProcessName),
            createPacketCell(toQString(processName)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::LimitKBps),
            createPacketCell(QString::number(snapshot.rule.bytesPerSecond / 1024ULL)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::SuspendMs),
            createPacketCell(QString::number(snapshot.rule.suspendDurationMs)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::TriggerCount),
            createPacketCell(QString::number(snapshot.triggerCount)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::CurrentWindowBytes),
            createPacketCell(QString::number(snapshot.currentWindowBytes)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::State),
            createPacketCell(stateText));
        ++rowIndex;
    }
}

void NetworkDock::appendRateLimitActionLogLine(const QString& logLine)
{
    if (m_rateLimitLogOutput == nullptr)
    {
        return;
    }

    const QString timePrefix = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_rateLimitLogOutput->appendPlainText(QStringLiteral("[%1] %2").arg(timePrefix, logLine));
}

