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
        // 这里把批量上限下调，优先保障主线程交互流畅性（按钮、拖拽、切页不卡顿）。
        constexpr std::size_t kMaxConsumePerTick = 96;
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

    // 仅在限速页可见时立即刷新表格，避免隐藏页事件触发不必要重绘。
    if (m_sideTabWidget != nullptr && m_sideTabWidget->currentWidget() == m_rateLimitPage)
    {
        refreshRateLimitTable();
    }

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

    // 追加场景改用“直接扩容 rowCount”而非 insertRow：
    // - insertRow 会触发后续行移动；
    // - setRowCount 在尾部追加时更轻量，适合高频写入场景。
    const int newRow = m_packetTable->rowCount();
    m_packetTable->setRowCount(newRow + 1);
    const QIcon processIcon = resolveProcessIconByPid(packetRecord.processId, packetRecord.processName);
    populatePacketRow(m_packetTable, newRow, packetRecord, packetRecord.sequenceId, processIcon);
}

void NetworkDock::rebuildMonitorTableByFilter()
{
    if (m_packetTable == nullptr)
    {
        return;
    }

    // 先收集通过过滤的记录列表，再一次性 setRowCount，减少反复 insertRow 开销。
    std::vector<const ks::network::PacketRecord*> visibleRecordList;
    visibleRecordList.reserve(m_packetSequenceOrder.size());
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
        visibleRecordList.push_back(&packetRecord);
    }

    m_packetTable->setUpdatesEnabled(false);
    m_packetTable->setRowCount(static_cast<int>(visibleRecordList.size()));

    int writeRow = 0;
    for (const ks::network::PacketRecord* packetRecordPtr : visibleRecordList)
    {
        if (packetRecordPtr == nullptr)
        {
            continue;
        }

        const ks::network::PacketRecord& packetRecord = *packetRecordPtr;
        const QIcon processIcon = resolveProcessIconByPid(packetRecord.processId, packetRecord.processName);
        populatePacketRow(m_packetTable, writeRow, packetRecord, packetRecord.sequenceId, processIcon);
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

    // 批量裁剪策略：
    // - 避免“每来一包就 removeRow(0)”导致持续 O(n) 行移动；
    // - 改为累计到阈值后一次性移除一批，显著降低 UI 抖动和卡顿。
    constexpr std::size_t kTrimBatchCount = 320;
    const std::size_t overflowCount = m_packetSequenceOrder.size() - kMaxPacketCacheCount;

    // 只有当“溢出量达到批处理阈值”时才执行批量裁剪，
    // 避免刚超 1 条就频繁触发重排。
    if (overflowCount < kTrimBatchCount &&
        m_packetSequenceOrder.size() < (kMaxPacketCacheCount + kTrimBatchCount))
    {
        return;
    }

    const std::size_t trimCount = std::max<std::size_t>(overflowCount, kTrimBatchCount);

    std::size_t visibleTrimCount = 0;
    for (std::size_t trimIndex = 0; trimIndex < trimCount; ++trimIndex)
    {
        if (m_packetSequenceOrder.empty())
        {
            break;
        }

        const std::uint64_t oldestSequenceId = m_packetSequenceOrder.front();
        m_packetSequenceOrder.pop_front();

        const auto oldestIterator = m_packetBySequence.find(oldestSequenceId);
        if (oldestIterator == m_packetBySequence.end())
        {
            continue;
        }

        if (packetPassesMonitorFilter(oldestIterator->second))
        {
            ++visibleTrimCount;
        }
        m_packetBySequence.erase(oldestIterator);
    }

    // 仅在当前表确有可见裁剪时才删除行，减少不必要的 UI 操作。
    if (visibleTrimCount > 0 && m_packetTable != nullptr && m_packetTable->rowCount() > 0)
    {
        const int removeRows = std::min<int>(static_cast<int>(visibleTrimCount), m_packetTable->rowCount());
        for (int removeIndex = 0; removeIndex < removeRows; ++removeIndex)
        {
            m_packetTable->removeRow(0);
        }

        kLogEvent trimEvent;
        dbg << trimEvent
            << "[NetworkDock] 报文缓存批量裁剪, trimCount=" << trimCount
            << ", visibleTrimCount=" << visibleTrimCount
            << ", remainCache=" << m_packetSequenceOrder.size()
            << eol;
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

