#include "NetworkDock.InternalCommon.h"

using namespace network_dock_detail;
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

    // PID -> 进程名静态缓存：
    // - 降低定时刷新时重复 QueryProcessNameByPID 的调用成本；
    // - 规则通常跟随 PID 长时间存在，缓存命中率高。
    static std::unordered_map<std::uint32_t, std::string> s_processNameCacheByPid;

    m_rateLimitTable->setUpdatesEnabled(false);
    m_rateLimitTable->setRowCount(static_cast<int>(snapshots.size()));
    int rowIndex = 0;
    for (const ks::network::ProcessRateLimitSnapshot& snapshot : snapshots)
    {
        const std::uint32_t processId = snapshot.rule.processId;
        const auto cacheIterator = s_processNameCacheByPid.find(processId);
        std::string processName;
        if (cacheIterator != s_processNameCacheByPid.end())
        {
            processName = cacheIterator->second;
        }
        else
        {
            processName = ks::process::GetProcessNameByPID(processId);
            s_processNameCacheByPid.insert({ processId, processName });
        }
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

    m_rateLimitTable->setUpdatesEnabled(true);
    m_rateLimitTable->viewport()->update();
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



