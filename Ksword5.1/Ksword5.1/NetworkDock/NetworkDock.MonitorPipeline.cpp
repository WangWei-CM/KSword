#include "NetworkDock.InternalCommon.h"

using namespace network_dock_detail;

namespace
{
    // kUnixMsTo100ns：
    // - Unix 毫秒时间戳转换到 100ns 时间轴单位的倍率；
    // - 网络抓包使用 Unix ms，ETW 同款时间轴使用 100ns，所以需要统一换算。
    constexpr std::uint64_t kUnixMsTo100ns = 10000ULL;

    // kFallbackTimelineRange100ns：
    // - 没有报文或起止相同的时候给时间轴提供 1 秒非零跨度；
    // - 避免时间轴控件内部坐标除零，也让初始空状态仍可见。
    constexpr std::uint64_t kFallbackTimelineRange100ns = 1ULL * 1000ULL * 1000ULL * 10ULL;

    // kTimelineSecond100ns：
    // - 1 秒在 100ns 时间轴单位下的宽度；
    // - 用于把报文聚合到“每秒上传/下载速率”桶。
    constexpr std::uint64_t kTimelineSecond100ns = 1ULL * 1000ULL * 1000ULL * 10ULL;

    // unixMsToTimeline100ns：
    // - 作用：把 Unix ms 时长或时间差转换成时间轴使用的 100ns；
    // - 参数 unixMsValue：毫秒数；
    // - 返回：100ns 时间值。
    std::uint64_t unixMsToTimeline100ns(const std::uint64_t unixMsValue)
    {
        return unixMsValue * kUnixMsTo100ns;
    }

    // packetTimelineTypeText：
    // - 作用：把 TCP/UDP 与出入站方向归一化成时间轴颜色/泳道可理解的类型；
    // - 参数 packetRecord：待映射报文；
    // - 返回：时间轴点 typeText，当前统一归入“网络”泳道。
    QString packetTimelineTypeText(const ks::network::PacketRecord& packetRecord)
    {
        (void)packetRecord;
        return QStringLiteral("网络");
    }
}

void NetworkDock::onPacketCaptured(const ks::network::PacketRecord& packetRecord)
{
    // timelineTime100ns 是“只统计监控开启时间”的压缩时间：
    // - 停止监控后的等待时长不再占用横轴；
    // - 后续表格筛选、事件点、速率折线全部使用同一压缩时间。
    const std::uint64_t timelineTime100ns = packetTimelineTimeForRecord(packetRecord);

    // 缓存报文实体：用于详情窗口通过序号回查完整字节内容。
    m_packetSequenceOrder.push_back(packetRecord.sequenceId);
    m_packetBySequence[packetRecord.sequenceId] = packetRecord;
    m_packetTimelineTimeBySequence[packetRecord.sequenceId] = timelineTime100ns;

    // 同步写入时间轴轻量点：
    // - 点列表只保存时间和类型，不保存表格行号；
    // - 裁剪时与报文序号缓存同步删除，避免时间轴显示已经不存在的历史点。
    ProcessTraceTimelineEventPoint timelinePoint;
    timelinePoint.time100ns = timelineTime100ns;
    timelinePoint.typeText = packetTimelineTypeText(packetRecord);
    m_packetTimelineEventPoints.push_back(std::move(timelinePoint));
    addPacketTimelineRateSample(packetRecord, timelineTime100ns);

    // 当前报文若通过“组合过滤条件”才追加到主表（过滤关闭时默认全部通过）。
    if (packetPassesMonitorFilter(packetRecord.sequenceId, packetRecord))
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
    // timelineIdleRefreshNeeded 用途：即使后台队列为空，只要监控还在运行，也周期推进压缩时间轴。
    const bool timelineIdleRefreshNeeded = m_packetTimelineSessionActive && m_monitorRunning;
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        if (m_pendingPacketQueue.empty() && m_droppedPacketCount == 0 && !timelineIdleRefreshNeeded)
        {
            return;
        }

        // 单次最多处理固定数量，避免 UI 一帧执行过久。
        // kMaxConsumePerTick 用途：在“不卡 UI”与“少丢包”之间折中；这里提高上限以减少高流量积压。
        constexpr std::size_t kMaxConsumePerTick = 960;
        const std::size_t consumeCount = std::min<std::size_t>(kMaxConsumePerTick, m_pendingPacketQueue.size());
        packetBatch.reserve(consumeCount);
        for (std::size_t index = 0; index < consumeCount; ++index)
        {
            packetBatch.push_back(std::move(m_pendingPacketQueue.front()));
            m_pendingPacketQueue.pop_front();
        }

        droppedCountSnapshot = m_droppedPacketCount;
    }

    // 空流量心跳：
    // - 用户开启监控但这一秒没有任何包时，也要推进时间轴右边界；
    // - 同时补一个 0 B/s 速率桶，让折线在空闲秒回落到基线。
    const bool shouldRefreshIdleTimeline = packetBatch.empty()
        && m_packetTimelineSessionActive
        && m_monitorRunning;
    if (shouldRefreshIdleTimeline)
    {
        const std::uint64_t timelineEnd100ns = currentPacketTimelineEnd100ns();
        const std::uint64_t heartbeatSecond = timelineEnd100ns / kTimelineSecond100ns;
        m_packetTimelineRangeStart100ns = 0;
        m_packetTimelineRangeEnd100ns = std::max(timelineEnd100ns, kFallbackTimelineRange100ns);
        if (heartbeatSecond != m_packetTimelineLastHeartbeatSecond)
        {
            m_packetTimelineLastHeartbeatSecond = heartbeatSecond;
            m_packetTimelineRateBucketBySecond.try_emplace(heartbeatSecond, PacketTimelineRateBucket{});
            refreshPacketTimelineRange();
            refreshPacketTimelineRatePoints();
        }
        else if (m_packetTimelineWidget != nullptr
            && m_packetTimelineRangeEnd100ns > m_packetTimelineWidget->selectionEnd100ns())
        {
            refreshPacketTimelineRange();
        }
    }

    // 批量更新表格期间临时关闭重绘，减少 UI 抖动与卡顿。
    if (!packetBatch.empty() && m_packetTable != nullptr)
    {
        m_packetTable->setUpdatesEnabled(false);
    }

    // tableUpdateDisabled 用途：保证后续提前返回或异常路径也能恢复表格刷新开关。
    const bool tableUpdateDisabled = !packetBatch.empty() && m_packetTable != nullptr;
    for (const ks::network::PacketRecord& packetRecord : packetBatch)
    {
        onPacketCaptured(packetRecord);
    }

    if (tableUpdateDisabled)
    {
        m_packetTable->setUpdatesEnabled(true);
        m_packetTable->scrollToBottom();
    }

    // 时间轴刷新放在批量表格刷新之后：
    // - 一次 tick 只重绘一次时间轴，避免高频逐包 update；
    // - 若用户已经框选时间窗口，新包仍会进入点缓存，但表格按外层时间过滤显示。
    if (!packetBatch.empty())
    {
        refreshPacketTimelineRange();
        refreshPacketTimelinePoints();
        refreshPacketTimelineRatePoints();
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
    const bool wasMonitorRunning = m_monitorRunning;
    if (m_trafficService != nullptr)
    {
        m_monitorRunning = m_trafficService->IsRunning();
    }
    if (wasMonitorRunning && !m_monitorRunning && m_packetTimelineSessionActive)
    {
        // 后台线程若因错误或外部条件自行退出，也要关闭时间轴会话；
        // 否则后续等待时间会被误计入“监控开启时长”。
        endPacketTimelineMonitorSession();
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
        if (!packetPassesMonitorFilter(sequenceId, packetRecord))
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

void NetworkDock::applyPacketTimelineSelection(
    const std::uint64_t start100ns,
    const std::uint64_t end100ns)
{
    // 时间轴控件负责把鼠标框选转换成时间戳；NetworkDock 只保存范围并重建表格。
    m_packetTimelineSelectionStart100ns = std::min(start100ns, end100ns);
    m_packetTimelineSelectionEnd100ns = std::max(start100ns, end100ns);
    m_packetTimelineUserSelectionActive = true;

    updateMonitorFilterStateLabel();
    rebuildMonitorTableByFilter();

    kLogEvent timelineEvent;
    dbg << timelineEvent
        << "[NetworkDock] 流量时间轴选区变更, start100ns="
        << m_packetTimelineSelectionStart100ns
        << ", end100ns="
        << m_packetTimelineSelectionEnd100ns
        << eol;
}

void NetworkDock::resetPacketTimelineToCurrentRange()
{
    // 清空/清筛选操作后让时间轴恢复默认全范围，避免旧选区继续影响下一轮抓包。
    m_packetTimelineUserSelectionActive = false;
    m_packetTimelineSelectionStart100ns = 0;
    m_packetTimelineSelectionEnd100ns = 0;
    m_packetTimelineRangeStart100ns = 0;
    m_packetTimelineRangeEnd100ns = currentPacketTimelineEnd100ns();
    if (m_packetTimelineRangeEnd100ns == 0)
    {
        m_packetTimelineRangeEnd100ns = kFallbackTimelineRange100ns;
    }

    if (m_packetTimelineWidget != nullptr)
    {
        m_packetTimelineWidget->resetTimeline(0);
        m_packetTimelineWidget->setCaptureRange(
            m_packetTimelineRangeStart100ns,
            m_packetTimelineRangeEnd100ns);
        m_packetTimelineSelectionStart100ns = m_packetTimelineWidget->selectionStart100ns();
        m_packetTimelineSelectionEnd100ns = m_packetTimelineWidget->selectionEnd100ns();
        m_packetTimelineWidget->setEventPoints(m_packetTimelineEventPoints);
    }
    refreshPacketTimelineRatePoints();
}

void NetworkDock::refreshPacketTimelineRange()
{
    if (m_packetTimelineWidget == nullptr)
    {
        return;
    }

    std::uint64_t rangeStart100ns = 0;
    std::uint64_t rangeEnd100ns = currentPacketTimelineEnd100ns();
    if (!m_packetSequenceOrder.empty())
    {
        // 报文序号缓存按采集顺序排列，但时间轴左边界固定为 0：
        // - 这样旧包裁剪后，横轴仍代表“本轮监控已运行多久”；
        // - 右边界使用累计监控时长，不把未监控的停机间隔算进去。
        for (const std::uint64_t sequenceId : m_packetSequenceOrder)
        {
            const auto iterator = m_packetBySequence.find(sequenceId);
            if (iterator == m_packetBySequence.end())
            {
                continue;
            }

            const std::uint64_t timestamp100ns = packetTimelineTimeForSequence(sequenceId, iterator->second);
            if (timestamp100ns == 0)
            {
                continue;
            }

            rangeEnd100ns = std::max(rangeEnd100ns, timestamp100ns);
        }
    }

    if (rangeEnd100ns == 0)
    {
        // 没有任何报文且未开始监控时，使用 0~1 秒作为空时间轴。
        rangeEnd100ns = kFallbackTimelineRange100ns;
    }

    if (rangeEnd100ns <= rangeStart100ns)
    {
        rangeEnd100ns = rangeStart100ns + kFallbackTimelineRange100ns;
    }

    m_packetTimelineRangeStart100ns = rangeStart100ns;
    m_packetTimelineRangeEnd100ns = rangeEnd100ns;
    m_packetTimelineWidget->setCaptureRange(rangeStart100ns, rangeEnd100ns);
    m_packetTimelineSelectionStart100ns = m_packetTimelineWidget->selectionStart100ns();
    m_packetTimelineSelectionEnd100ns = m_packetTimelineWidget->selectionEnd100ns();
}

void NetworkDock::refreshPacketTimelinePoints()
{
    if (m_packetTimelineWidget == nullptr)
    {
        return;
    }

    m_packetTimelineWidget->setEventPoints(m_packetTimelineEventPoints);
}

void NetworkDock::refreshPacketTimelineRatePoints()
{
    if (m_packetTimelineWidget == nullptr)
    {
        return;
    }
    if (m_packetTimelineRangeEnd100ns <= m_packetTimelineRangeStart100ns)
    {
        m_packetTimelineRangeStart100ns = 0;
        m_packetTimelineRangeEnd100ns = std::max(
            currentPacketTimelineEnd100ns(),
            kFallbackTimelineRange100ns);
    }

    // 将 unordered_map 快照整理为按秒升序的折线点：
    // - X 轴使用秒桶起点；
    // - Y 值直接为该秒累计字节，等价于 B/s。
    const std::uint64_t visibleEndSecond = m_packetTimelineRangeEnd100ns / kTimelineSecond100ns;
    std::vector<std::uint64_t> secondIndexList;
    secondIndexList.reserve(m_packetTimelineRateBucketBySecond.size() + 2);
    for (const auto& bucketPair : m_packetTimelineRateBucketBySecond)
    {
        secondIndexList.push_back(bucketPair.first);
    }
    secondIndexList.push_back(0);
    secondIndexList.push_back(visibleEndSecond);
    std::sort(secondIndexList.begin(), secondIndexList.end());
    secondIndexList.erase(
        std::unique(secondIndexList.begin(), secondIndexList.end()),
        secondIndexList.end());

    std::vector<ProcessTraceTimelineRatePoint> ratePointList;
    ratePointList.reserve(secondIndexList.size());
    for (const std::uint64_t secondIndex : secondIndexList)
    {
        const auto iterator = m_packetTimelineRateBucketBySecond.find(secondIndex);

        ProcessTraceTimelineRatePoint ratePoint;
        ratePoint.time100ns = secondIndex * kTimelineSecond100ns;
        if (iterator != m_packetTimelineRateBucketBySecond.end())
        {
            ratePoint.uploadBytesPerSecond = static_cast<double>(iterator->second.uploadBytes);
            ratePoint.downloadBytesPerSecond = static_cast<double>(iterator->second.downloadBytes);
        }
        ratePointList.push_back(ratePoint);
    }

    m_packetTimelineWidget->setRateOverlayPoints(ratePointList);
}

bool NetworkDock::isPacketTimelineFilterActive() const
{
    if (!m_packetTimelineUserSelectionActive
        || m_packetTimelineSelectionEnd100ns == 0
        || m_packetTimelineSelectionEnd100ns <= m_packetTimelineSelectionStart100ns)
    {
        return false;
    }

    if (m_packetTimelineRangeEnd100ns <= m_packetTimelineRangeStart100ns)
    {
        return false;
    }

    // 全范围选区不视为过滤；这能让实时抓包默认持续显示新包。
    return m_packetTimelineSelectionStart100ns > m_packetTimelineRangeStart100ns
        || m_packetTimelineSelectionEnd100ns < m_packetTimelineRangeEnd100ns;
}

bool NetworkDock::packetPassesTimelineFilter(const ks::network::PacketRecord& packetRecord) const
{
    if (!isPacketTimelineFilterActive())
    {
        return true;
    }

    const std::uint64_t packetTime100ns = packetTimelineTimeForRecord(packetRecord);
    return packetTime100ns >= m_packetTimelineSelectionStart100ns
        && packetTime100ns <= m_packetTimelineSelectionEnd100ns;
}

bool NetworkDock::packetPassesTimelineFilter(
    const std::uint64_t sequenceId,
    const ks::network::PacketRecord& packetRecord) const
{
    if (!isPacketTimelineFilterActive())
    {
        return true;
    }

    const std::uint64_t packetTime100ns = packetTimelineTimeForSequence(sequenceId, packetRecord);
    return packetTime100ns >= m_packetTimelineSelectionStart100ns
        && packetTime100ns <= m_packetTimelineSelectionEnd100ns;
}

void NetworkDock::beginPacketTimelineMonitorSession()
{
    // 已存在活动会话时不重复登记，避免双击开始按钮导致累计时间错位。
    if (m_packetTimelineSessionActive)
    {
        return;
    }

    // 如果上一个会话还处于“未闭合”状态，先用当前时间闭合它；
    // 这样即使存在异常状态回调顺序，也不会让停机间隔混入下一段监控时长。
    if (!m_packetTimelineSessionList.empty()
        && m_packetTimelineSessionList.back().endUnixMs == 0)
    {
        PacketTimelineCaptureSession& previousSession = m_packetTimelineSessionList.back();
        previousSession.endUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        const std::uint64_t previousDurationMs = previousSession.endUnixMs > previousSession.startUnixMs
            ? (previousSession.endUnixMs - previousSession.startUnixMs)
            : 0;
        m_packetTimelineAccumulatedActive100ns =
            previousSession.baseStart100ns + unixMsToTimeline100ns(previousDurationMs);
    }

    PacketTimelineCaptureSession session;
    session.startUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    session.endUnixMs = 0;
    session.baseStart100ns = m_packetTimelineAccumulatedActive100ns;
    m_packetTimelineSessionList.push_back(session);
    m_packetTimelineSessionActive = true;
    m_packetTimelineLastHeartbeatSecond = session.baseStart100ns / kTimelineSecond100ns;

    refreshPacketTimelineRange();
    refreshPacketTimelineRatePoints();
}

void NetworkDock::endPacketTimelineMonitorSession()
{
    if (!m_packetTimelineSessionActive || m_packetTimelineSessionList.empty())
    {
        return;
    }

    PacketTimelineCaptureSession& session = m_packetTimelineSessionList.back();
    if (session.endUnixMs == 0)
    {
        session.endUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    }

    const std::uint64_t sessionDurationMs = session.endUnixMs > session.startUnixMs
        ? (session.endUnixMs - session.startUnixMs)
        : 0;
    m_packetTimelineAccumulatedActive100ns =
        session.baseStart100ns + unixMsToTimeline100ns(sessionDurationMs);
    m_packetTimelineSessionActive = false;
    m_packetTimelineLastHeartbeatSecond = m_packetTimelineAccumulatedActive100ns / kTimelineSecond100ns;

    refreshPacketTimelineRange();
    refreshPacketTimelineRatePoints();
}

void NetworkDock::resetPacketTimelineClockForCurrentState()
{
    // wasRunning 用途：在清理会话列表前捕获当前运行状态，避免下面清空状态后误判。
    const bool wasRunning = m_monitorRunning
        || (m_trafficService != nullptr && m_trafficService->IsRunning());

    // 清空报文意味着开始一条新的逻辑时间轴：
    // - 历史会话与报文都丢弃；
    // - 如果用户当前仍在监控，则立即从 0 秒重新登记活动会话。
    m_packetTimelineSessionList.clear();
    m_packetTimelineTimeBySequence.clear();
    m_packetTimelineRateBucketBySecond.clear();
    m_packetTimelineAccumulatedActive100ns = 0;
    m_packetTimelineLastHeartbeatSecond = 0;
    m_packetTimelineSessionActive = false;

    if (wasRunning)
    {
        beginPacketTimelineMonitorSession();
    }
}

std::uint64_t NetworkDock::packetTimelineTimeForRecord(const ks::network::PacketRecord& packetRecord) const
{
    if (packetRecord.captureTimestampMs == 0)
    {
        return currentPacketTimelineEnd100ns();
    }

    if (m_packetTimelineSessionList.empty()
        && m_packetTimelineSessionActive
        && m_packetTimelineAccumulatedActive100ns == 0)
    {
        // 理论上 beginPacketTimelineMonitorSession 会先登记会话；
        // 若异常顺序导致列表为空，则把报文放在 0 秒位置，仍然不使用真实系统时间。
        return 0;
    }

    for (const PacketTimelineCaptureSession& session : m_packetTimelineSessionList)
    {
        const std::uint64_t sessionEndUnixMs = session.endUnixMs == 0
            ? std::max<std::uint64_t>(
                packetRecord.captureTimestampMs,
                static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()))
            : session.endUnixMs;
        if (packetRecord.captureTimestampMs < session.startUnixMs
            || packetRecord.captureTimestampMs > sessionEndUnixMs)
        {
            continue;
        }

        const std::uint64_t elapsedMs = packetRecord.captureTimestampMs > session.startUnixMs
            ? (packetRecord.captureTimestampMs - session.startUnixMs)
            : 0;
        return session.baseStart100ns + unixMsToTimeline100ns(elapsedMs);
    }

    if (m_packetTimelineSessionActive && !m_packetTimelineSessionList.empty())
    {
        // 极少数情况下报文可能早于 UI 登记会话（线程启动竞争）：
        // - 归入当前活动会话起点，而不是当前时间；
        // - 这样第一批报文不会被压到右边界造成瞬间速率尖峰。
        return m_packetTimelineSessionList.back().baseStart100ns;
    }

    // 停止后才到达的滞后报文归入累计末尾，确保不会落到真实系统时间造成横轴跳变。
    return currentPacketTimelineEnd100ns();
}

std::uint64_t NetworkDock::packetTimelineTimeForSequence(
    const std::uint64_t sequenceId,
    const ks::network::PacketRecord& packetRecord) const
{
    const auto iterator = m_packetTimelineTimeBySequence.find(sequenceId);
    if (iterator != m_packetTimelineTimeBySequence.end())
    {
        return iterator->second;
    }
    return packetTimelineTimeForRecord(packetRecord);
}

std::uint64_t NetworkDock::currentPacketTimelineEnd100ns() const
{
    if (m_packetTimelineSessionActive && !m_packetTimelineSessionList.empty())
    {
        const PacketTimelineCaptureSession& session = m_packetTimelineSessionList.back();
        const std::uint64_t nowUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        const std::uint64_t elapsedMs = nowUnixMs > session.startUnixMs
            ? (nowUnixMs - session.startUnixMs)
            : 0;
        return session.baseStart100ns + unixMsToTimeline100ns(elapsedMs);
    }

    return m_packetTimelineAccumulatedActive100ns;
}

void NetworkDock::addPacketTimelineRateSample(
    const ks::network::PacketRecord& packetRecord,
    const std::uint64_t timelineTime100ns)
{
    const std::uint64_t secondIndex = timelineTime100ns / kTimelineSecond100ns;
    PacketTimelineRateBucket& bucket = m_packetTimelineRateBucketBySecond[secondIndex];

    if (packetRecord.direction == ks::network::PacketDirection::Outbound)
    {
        bucket.uploadBytes += packetRecord.totalPacketSize;
    }
    else if (packetRecord.direction == ks::network::PacketDirection::Inbound)
    {
        bucket.downloadBytes += packetRecord.totalPacketSize;
    }
}

void NetworkDock::removePacketTimelineRateSample(
    const ks::network::PacketRecord& packetRecord,
    const std::uint64_t timelineTime100ns)
{
    const std::uint64_t secondIndex = timelineTime100ns / kTimelineSecond100ns;
    const auto iterator = m_packetTimelineRateBucketBySecond.find(secondIndex);
    if (iterator == m_packetTimelineRateBucketBySecond.end())
    {
        return;
    }

    PacketTimelineRateBucket& bucket = iterator->second;
    if (packetRecord.direction == ks::network::PacketDirection::Outbound)
    {
        bucket.uploadBytes = bucket.uploadBytes > packetRecord.totalPacketSize
            ? bucket.uploadBytes - packetRecord.totalPacketSize
            : 0;
    }
    else if (packetRecord.direction == ks::network::PacketDirection::Inbound)
    {
        bucket.downloadBytes = bucket.downloadBytes > packetRecord.totalPacketSize
            ? bucket.downloadBytes - packetRecord.totalPacketSize
            : 0;
    }

    if (bucket.uploadBytes == 0 && bucket.downloadBytes == 0 && !m_packetTimelineSessionActive)
    {
        m_packetTimelineRateBucketBySecond.erase(iterator);
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
            m_packetTimelineTimeBySequence.erase(oldestSequenceId);
            continue;
        }

        if (!m_packetTimelineEventPoints.empty())
        {
            // 时间轴点与 m_packetSequenceOrder 同步按采集顺序追加，因此裁剪最老报文时移除最老点。
            m_packetTimelineEventPoints.erase(m_packetTimelineEventPoints.begin());
        }

        const std::uint64_t timelineTime100ns =
            packetTimelineTimeForSequence(oldestSequenceId, oldestIterator->second);
        removePacketTimelineRateSample(oldestIterator->second, timelineTime100ns);
        m_packetTimelineTimeBySequence.erase(oldestSequenceId);

        if (packetPassesMonitorFilter(oldestSequenceId, oldestIterator->second))
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

    refreshPacketTimelineRange();
    refreshPacketTimelinePoints();
    refreshPacketTimelineRatePoints();
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
    m_packetTimelineEventPoints.clear();
    resetPacketTimelineClockForCurrentState();
    resetPacketTimelineToCurrentRange();
    refreshPacketTimelinePoints();
    refreshPacketTimelineRatePoints();
    updateMonitorFilterStateLabel();

    if (m_packetTable != nullptr)
    {
        m_packetTable->setRowCount(0);
    }

    kLogEvent clearEvent;
    info << clearEvent << "[NetworkDock] 用户清空了网络报文列表。" << eol;
}



