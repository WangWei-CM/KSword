void NetworkDock::onPacketCaptured(const ks::network::PacketRecord& packetRecord)
{
    // 缓存报文实体：用于详情窗口通过序号回查完整字节内容。
    m_packetSequenceOrder.push_back(packetRecord.sequenceId);
    m_packetBySequence[packetRecord.sequenceId] = packetRecord;

    // 当前报文若通过“组合过滤条件”才追加到主表（过滤关闭时默认全部通过）。
    if (packetPassesMonitorFilter(packetRecord))
    {
        appendPacketToMonitorTable(packetRecord);
    }
    trimOldestPacketWhenNeeded();
}

void NetworkDock::flushPendingPacketsToUi()
{
    // 先在锁内把待处理报文批量转移到局部容器，缩短锁持有时间。
    std::vector<ks::network::PacketRecord> packetBatch;
    std::uint64_t droppedCountSnapshot = 0;
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        if (m_pendingPacketQueue.empty() && m_droppedPacketCount == 0)
        {
            return;
        }

        // 单次最多处理固定数量，避免 UI 一帧执行过久。
        constexpr std::size_t kMaxConsumePerTick = 240;
        const std::size_t consumeCount = std::min<std::size_t>(kMaxConsumePerTick, m_pendingPacketQueue.size());
        packetBatch.reserve(consumeCount);
        for (std::size_t index = 0; index < consumeCount; ++index)
        {
            packetBatch.push_back(std::move(m_pendingPacketQueue.front()));
            m_pendingPacketQueue.pop_front();
        }

        droppedCountSnapshot = m_droppedPacketCount;
    }

    // 批量更新表格期间临时关闭重绘，减少 UI 抖动与卡顿。
    if (m_packetTable != nullptr)
    {
        m_packetTable->setUpdatesEnabled(false);
    }
    for (const ks::network::PacketRecord& packetRecord : packetBatch)
    {
        onPacketCaptured(packetRecord);
    }

    if (m_packetTable != nullptr)
    {
        m_packetTable->setUpdatesEnabled(true);
        if (!packetBatch.empty())
        {
            m_packetTable->scrollToBottom();
        }
    }
    // 丢包计数只在 UI 线程展示，不在每个回调里刷屏。
    if (droppedCountSnapshot > 0 && m_monitorStatusLabel != nullptr)
    {
        const std::size_t pendingCount = [&]()
            {
                std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
                return m_pendingPacketQueue.size();
            }();
        m_monitorStatusLabel->setText(QStringLiteral("状态：高负载（已丢弃%1条，队列%2）")
            .arg(static_cast<qulonglong>(droppedCountSnapshot))
            .arg(static_cast<qulonglong>(pendingCount)));

        // 丢包状态至少记录一次日志，帮助用户排查“为何表格看起来少包”。
        // 这里按计数变化输出，避免每个刷新 tick 都刷屏。
        static std::uint64_t s_lastLoggedDroppedCount = 0;
        if (droppedCountSnapshot != s_lastLoggedDroppedCount)
        {
            s_lastLoggedDroppedCount = droppedCountSnapshot;

            kLogEvent droppedPacketEvent;
            warn << droppedPacketEvent
                << "[NetworkDock] 抓包队列高负载丢包, dropped=" << droppedCountSnapshot
                << ", pending=" << pendingCount
                << eol;
        }
    }
}

void NetworkDock::onStatusMessageArrived(const std::string& statusText)
{
    const QString statusQString = toQString(statusText);
    if (m_monitorStatusLabel != nullptr)
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：%1").arg(statusQString));
    }

    // 状态回调可能表示启动失败/线程退出，因此同步刷新按钮状态。
    if (m_trafficService != nullptr)
    {
        m_monitorRunning = m_trafficService->IsRunning();
    }
    updateMonitorButtonState();

    kLogEvent statusEvent;
    info << statusEvent << "[NetworkDock] 抓包状态: " << statusText << eol;
}

void NetworkDock::onRateLimitActionArrived(const ks::network::RateLimitActionEvent& actionEvent)
{
    // 把限速动作以时间行追加到日志框，便于排查“为何某进程被挂起/恢复”。
    const QString timeText = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(actionEvent.timestampMs)).toString("HH:mm:ss.zzz");
    const QString actionText = toQString(ks::network::RateLimitActionTypeToString(actionEvent.actionType));
    const QString resultText = actionEvent.actionSucceeded ? QStringLiteral("成功") : QStringLiteral("失败");
    const QString logLine = QStringLiteral("[%1] PID=%2, 动作=%3, 结果=%4, 详情=%5")
        .arg(timeText)
        .arg(actionEvent.processId)
        .arg(actionText)
        .arg(resultText)
        .arg(toQString(actionEvent.detailText));
    appendRateLimitActionLogLine(logLine);

    refreshRateLimitTable();

    // 动作事件使用 warn 级别，方便在日志面板中显眼查看。
    kLogEvent actionEventLog;
    warn << actionEventLog
        << "[NetworkDock] 限速动作, pid=" << actionEvent.processId
        << ", action=" << ks::network::RateLimitActionTypeToString(actionEvent.actionType)
        << ", ok=" << (actionEvent.actionSucceeded ? "true" : "false")
        << ", detail=" << actionEvent.detailText
        << eol;
}

void NetworkDock::appendPacketToMonitorTable(const ks::network::PacketRecord& packetRecord)
{
    if (m_packetTable == nullptr)
    {
        return;
    }

    const int newRow = m_packetTable->rowCount();
    m_packetTable->insertRow(newRow);
    const QIcon processIcon = resolveProcessIconByPid(packetRecord.processId, packetRecord.processName);
    populatePacketRow(m_packetTable, newRow, packetRecord, packetRecord.sequenceId, processIcon);
}

void NetworkDock::rebuildMonitorTableByFilter()
{
    if (m_packetTable == nullptr)
    {
        return;
    }

    m_packetTable->setUpdatesEnabled(false);
    m_packetTable->setRowCount(0);

    int writeRow = 0;
    for (const std::uint64_t sequenceId : m_packetSequenceOrder)
    {
        const auto iterator = m_packetBySequence.find(sequenceId);
        if (iterator == m_packetBySequence.end())
        {
            continue;
        }
        const ks::network::PacketRecord& packetRecord = iterator->second;
        if (!packetPassesMonitorFilter(packetRecord))
        {
            continue;
        }

        m_packetTable->insertRow(writeRow);
        const QIcon processIcon = resolveProcessIconByPid(packetRecord.processId, packetRecord.processName);
        populatePacketRow(m_packetTable, writeRow, packetRecord, sequenceId, processIcon);
        ++writeRow;
    }

    m_packetTable->setUpdatesEnabled(true);
    if (writeRow > 0)
    {
        m_packetTable->scrollToBottom();
    }
}

void NetworkDock::trimOldestPacketWhenNeeded()
{
    if (m_packetSequenceOrder.size() <= kMaxPacketCacheCount)
    {
        return;
    }

    // 删除最老序号对应的缓存。
    const std::uint64_t oldestSequenceId = m_packetSequenceOrder.front();
    m_packetSequenceOrder.pop_front();
    const auto oldestIterator = m_packetBySequence.find(oldestSequenceId);
    if (oldestIterator == m_packetBySequence.end())
    {
        return;
    }

    const bool oldestWasVisible = packetPassesMonitorFilter(oldestIterator->second);
    m_packetBySequence.erase(oldestIterator);

    // 表格保持插入顺序，因此可见最老项对应第一行，直接删除首行即可。
    if (oldestWasVisible && m_packetTable != nullptr && m_packetTable->rowCount() > 0)
    {
        m_packetTable->removeRow(0);
    }
}

void NetworkDock::clearAllPacketRows()
{
    // 同时清空后台待刷新队列，避免“刚清空又回填旧数据”。
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        m_pendingPacketQueue.clear();
        m_droppedPacketCount = 0;
    }

    m_packetSequenceOrder.clear();
    m_packetBySequence.clear();

    if (m_packetTable != nullptr)
    {
        m_packetTable->setRowCount(0);
    }

    kLogEvent clearEvent;
    info << clearEvent << "[NetworkDock] 用户清空了网络报文列表。" << eol;
}

