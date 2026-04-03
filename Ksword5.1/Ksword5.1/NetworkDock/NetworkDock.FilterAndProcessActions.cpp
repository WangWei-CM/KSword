#include "NetworkDock.InternalCommon.h"

using namespace network_dock_detail;
void NetworkDock::applyMonitorFilters()
{
    // 保护性检查：过滤控件尚未初始化时直接返回，避免空指针访问。
    if (m_pidFilterEdit == nullptr ||
        m_localIpFilterEdit == nullptr ||
        m_remoteIpFilterEdit == nullptr ||
        m_localPortFilterEdit == nullptr ||
        m_remotePortFilterEdit == nullptr ||
        m_packetSizeMinSpin == nullptr ||
        m_packetSizeMaxSpin == nullptr)
    {
        return;
    }

    // 先解析到局部变量：
    // - 只有全部解析成功才整体提交到 m_active* 成员；
    // - 避免“半成功状态”造成用户感知混乱。
    std::optional<std::uint32_t> parsedPidFilter;
    std::optional<UInt32Range> parsedLocalIpv4RangeFilter;
    std::optional<UInt32Range> parsedRemoteIpv4RangeFilter;
    std::optional<UInt16Range> parsedLocalPortRangeFilter;
    std::optional<UInt16Range> parsedRemotePortRangeFilter;
    std::optional<UInt32Range> parsedPacketSizeRangeFilter;

    // 输入校验失败统一处理：弹窗提示 + 日志记录。
    const auto reportFilterValidationError = [this](const QString& errorText)
        {
            QMessageBox::warning(this, QStringLiteral("流量筛选"), errorText);

            kLogEvent filterValidateFailEvent;
            warn << filterValidateFailEvent
                << "[NetworkDock] 流量筛选参数无效, detail="
                << errorText.toStdString()
                << eol;
        };

    // PID 条件：空输入代表“不启用 PID 过滤”。
    const QString pidText = m_pidFilterEdit->text().trimmed();
    if (!pidText.isEmpty())
    {
        std::uint32_t pidValue = 0;
        if (!tryParsePidText(pidText, pidValue))
        {
            reportFilterValidationError(QStringLiteral("PID 过滤输入无效，请输入十进制 PID。"));
            return;
        }
        parsedPidFilter = pidValue;
    }

    // 本地 IP 段条件：支持 CIDR/范围/单 IP。
    const QString localIpRangeText = m_localIpFilterEdit->text().trimmed();
    if (!localIpRangeText.isEmpty())
    {
        UInt32Range localIpv4Range{};
        QString normalizedLocalIpv4Text;
        if (!tryParseIpv4RangeText(localIpRangeText, localIpv4Range, normalizedLocalIpv4Text))
        {
            reportFilterValidationError(QStringLiteral("本地IP段格式无效。\n支持：192.168.1.0/24、192.168.1.10-192.168.1.50、192.168.1.8"));
            return;
        }
        parsedLocalIpv4RangeFilter = localIpv4Range;
        m_localIpFilterEdit->setText(normalizedLocalIpv4Text);
    }

    // 远端 IP 段条件：支持 CIDR/范围/单 IP。
    const QString remoteIpRangeText = m_remoteIpFilterEdit->text().trimmed();
    if (!remoteIpRangeText.isEmpty())
    {
        UInt32Range remoteIpv4Range{};
        QString normalizedRemoteIpv4Text;
        if (!tryParseIpv4RangeText(remoteIpRangeText, remoteIpv4Range, normalizedRemoteIpv4Text))
        {
            reportFilterValidationError(QStringLiteral("远端IP段格式无效。\n支持：8.8.8.8、10.0.0.0/8、1.1.1.1-1.1.1.9"));
            return;
        }
        parsedRemoteIpv4RangeFilter = remoteIpv4Range;
        m_remoteIpFilterEdit->setText(normalizedRemoteIpv4Text);
    }

    // 本地端口条件：支持单值或范围。
    const QString localPortRangeText = m_localPortFilterEdit->text().trimmed();
    if (!localPortRangeText.isEmpty())
    {
        UInt16Range localPortRange{};
        QString normalizedLocalPortText;
        if (!tryParsePortRangeText(localPortRangeText, localPortRange, normalizedLocalPortText))
        {
            reportFilterValidationError(QStringLiteral("本地端口格式无效，示例：80 或 1000-2000。"));
            return;
        }
        parsedLocalPortRangeFilter = localPortRange;
        m_localPortFilterEdit->setText(normalizedLocalPortText);
    }

    // 远端端口条件：支持单值或范围。
    const QString remotePortRangeText = m_remotePortFilterEdit->text().trimmed();
    if (!remotePortRangeText.isEmpty())
    {
        UInt16Range remotePortRange{};
        QString normalizedRemotePortText;
        if (!tryParsePortRangeText(remotePortRangeText, remotePortRange, normalizedRemotePortText))
        {
            reportFilterValidationError(QStringLiteral("远端端口格式无效，示例：443 或 30000-32000。"));
            return;
        }
        parsedRemotePortRangeFilter = remotePortRange;
        m_remotePortFilterEdit->setText(normalizedRemotePortText);
    }

    // 报文总长度条件：
    // - min=0 且 max=0 => 不启用包长过滤；
    // - max=0 且 min>0 => 仅限制下界（上界不限）。
    const std::uint32_t minPacketSize = static_cast<std::uint32_t>(m_packetSizeMinSpin->value());
    const std::uint32_t maxPacketSize = static_cast<std::uint32_t>(m_packetSizeMaxSpin->value());
    if (minPacketSize > 0 || maxPacketSize > 0)
    {
        if (maxPacketSize > 0 && minPacketSize > maxPacketSize)
        {
            reportFilterValidationError(QStringLiteral("包长范围无效：最小值不能大于最大值。"));
            return;
        }

        const std::uint32_t effectiveMaxPacketSize = (maxPacketSize > 0)
            ? maxPacketSize
            : std::numeric_limits<std::uint32_t>::max();
        parsedPacketSizeRangeFilter = UInt32Range{ minPacketSize, effectiveMaxPacketSize };
    }

    // 全部条件解析成功后一次性提交，保证过滤状态原子切换。
    m_activePidFilter = parsedPidFilter;
    m_activeLocalIpv4RangeFilter = parsedLocalIpv4RangeFilter;
    m_activeRemoteIpv4RangeFilter = parsedRemoteIpv4RangeFilter;
    m_activeLocalPortRangeFilter = parsedLocalPortRangeFilter;
    m_activeRemotePortRangeFilter = parsedRemotePortRangeFilter;
    m_activePacketSizeRangeFilter = parsedPacketSizeRangeFilter;

    updateMonitorFilterStateLabel();
    rebuildMonitorTableByFilter();

    // 记录一次组合过滤应用日志，便于追踪筛选行为。
    kLogEvent filterEvent;
    info << filterEvent
        << "[NetworkDock] 应用组合过滤, pidEnabled=" << (m_activePidFilter.has_value() ? "true" : "false")
        << ", localIpEnabled=" << (m_activeLocalIpv4RangeFilter.has_value() ? "true" : "false")
        << ", remoteIpEnabled=" << (m_activeRemoteIpv4RangeFilter.has_value() ? "true" : "false")
        << ", localPortEnabled=" << (m_activeLocalPortRangeFilter.has_value() ? "true" : "false")
        << ", remotePortEnabled=" << (m_activeRemotePortRangeFilter.has_value() ? "true" : "false")
        << ", packetSizeEnabled=" << (m_activePacketSizeRangeFilter.has_value() ? "true" : "false")
        << eol;
}

void NetworkDock::clearMonitorFilters()
{
    // UI 输入与已应用过滤状态都需要同步清空，避免界面与实际条件不一致。
    if (m_pidFilterEdit != nullptr) { m_pidFilterEdit->clear(); }
    if (m_localIpFilterEdit != nullptr) { m_localIpFilterEdit->clear(); }
    if (m_remoteIpFilterEdit != nullptr) { m_remoteIpFilterEdit->clear(); }
    if (m_localPortFilterEdit != nullptr) { m_localPortFilterEdit->clear(); }
    if (m_remotePortFilterEdit != nullptr) { m_remotePortFilterEdit->clear(); }
    if (m_packetSizeMinSpin != nullptr) { m_packetSizeMinSpin->setValue(0); }
    if (m_packetSizeMaxSpin != nullptr) { m_packetSizeMaxSpin->setValue(0); }

    m_activePidFilter.reset();
    m_activeLocalIpv4RangeFilter.reset();
    m_activeRemoteIpv4RangeFilter.reset();
    m_activeLocalPortRangeFilter.reset();
    m_activeRemotePortRangeFilter.reset();
    m_activePacketSizeRangeFilter.reset();

    updateMonitorFilterStateLabel();
    rebuildMonitorTableByFilter();

    kLogEvent clearFilterEvent;
    info << clearFilterEvent << "[NetworkDock] 已清空全部流量筛选条件。" << eol;
}

void NetworkDock::updateMonitorFilterStateLabel()
{
    if (m_monitorFilterStateLabel == nullptr)
    {
        return;
    }

    // 状态文本会把所有“已启用条件”拼接为一行，便于用户确认当前筛选上下文。
    QStringList activeFilterTextList;

    if (m_activePidFilter.has_value())
    {
        activeFilterTextList.push_back(QString("PID=%1").arg(m_activePidFilter.value()));
    }

    if (m_activeLocalIpv4RangeFilter.has_value())
    {
        const UInt32Range& ipRange = m_activeLocalIpv4RangeFilter.value();
        if (ipRange.first == ipRange.second)
        {
            activeFilterTextList.push_back(QString("本地IP=%1").arg(formatIpv4HostOrder(ipRange.first)));
        }
        else
        {
            activeFilterTextList.push_back(QString("本地IP=%1~%2")
                .arg(formatIpv4HostOrder(ipRange.first))
                .arg(formatIpv4HostOrder(ipRange.second)));
        }
    }

    if (m_activeRemoteIpv4RangeFilter.has_value())
    {
        const UInt32Range& ipRange = m_activeRemoteIpv4RangeFilter.value();
        if (ipRange.first == ipRange.second)
        {
            activeFilterTextList.push_back(QString("远端IP=%1").arg(formatIpv4HostOrder(ipRange.first)));
        }
        else
        {
            activeFilterTextList.push_back(QString("远端IP=%1~%2")
                .arg(formatIpv4HostOrder(ipRange.first))
                .arg(formatIpv4HostOrder(ipRange.second)));
        }
    }

    if (m_activeLocalPortRangeFilter.has_value())
    {
        const UInt16Range& portRange = m_activeLocalPortRangeFilter.value();
        if (portRange.first == portRange.second)
        {
            activeFilterTextList.push_back(QString("本地端口=%1").arg(portRange.first));
        }
        else
        {
            activeFilterTextList.push_back(QString("本地端口=%1-%2").arg(portRange.first).arg(portRange.second));
        }
    }

    if (m_activeRemotePortRangeFilter.has_value())
    {
        const UInt16Range& portRange = m_activeRemotePortRangeFilter.value();
        if (portRange.first == portRange.second)
        {
            activeFilterTextList.push_back(QString("远端端口=%1").arg(portRange.first));
        }
        else
        {
            activeFilterTextList.push_back(QString("远端端口=%1-%2").arg(portRange.first).arg(portRange.second));
        }
    }

    if (m_activePacketSizeRangeFilter.has_value())
    {
        const UInt32Range& packetSizeRange = m_activePacketSizeRangeFilter.value();
        if (packetSizeRange.first == 0 &&
            packetSizeRange.second != std::numeric_limits<std::uint32_t>::max())
        {
            activeFilterTextList.push_back(QString("包长<=%1").arg(packetSizeRange.second));
        }
        else if (packetSizeRange.second == std::numeric_limits<std::uint32_t>::max())
        {
            activeFilterTextList.push_back(QString("包长>=%1").arg(packetSizeRange.first));
        }
        else if (packetSizeRange.first == packetSizeRange.second)
        {
            activeFilterTextList.push_back(QString("包长=%1").arg(packetSizeRange.first));
        }
        else
        {
            activeFilterTextList.push_back(
                QString("包长=%1-%2").arg(packetSizeRange.first).arg(packetSizeRange.second));
        }
    }

    if (activeFilterTextList.empty())
    {
        m_monitorFilterStateLabel->setText(QStringLiteral("当前过滤：无"));
    }
    else
    {
        m_monitorFilterStateLabel->setText(QStringLiteral("当前过滤：%1").arg(activeFilterTextList.join(" | ")));
    }
}

void NetworkDock::trackProcessByTableRow(const int row)
{
    if (m_packetTable == nullptr || row < 0 || row >= m_packetTable->rowCount())
    {
        return;
    }

    QTableWidgetItem* pidItem = m_packetTable->item(row, toPacketColumn(PacketTableColumn::Pid));
    if (pidItem == nullptr)
    {
        return;
    }

    std::uint32_t targetPid = 0;
    if (!tryParsePidText(pidItem->text(), targetPid))
    {
        QMessageBox::information(this, QStringLiteral("跟踪进程"), QStringLiteral("该行 PID 无效，无法跟踪。"));
        return;
    }

    if (m_pidFilterEdit != nullptr)
    {
        m_pidFilterEdit->setText(QString::number(targetPid));
    }
    applyMonitorFilters();

    kLogEvent trackEvent;
    info << trackEvent << "[NetworkDock] 跟踪此进程触发, pid=" << targetPid << eol;
}

void NetworkDock::gotoProcessDetailByTableRow(const int row)
{
    if (m_packetTable == nullptr || row < 0 || row >= m_packetTable->rowCount())
    {
        return;
    }

    QTableWidgetItem* pidItem = m_packetTable->item(row, toPacketColumn(PacketTableColumn::Pid));
    if (pidItem == nullptr)
    {
        return;
    }

    std::uint32_t targetPid = 0;
    if (!tryParsePidText(pidItem->text(), targetPid))
    {
        QMessageBox::information(this, QStringLiteral("进程详情"), QStringLiteral("该行 PID 无效，无法打开进程详情。"));
        return;
    }

    ks::process::ProcessRecord processRecord;
    if (!ks::process::QueryProcessStaticDetailByPid(targetPid, processRecord))
    {
        QMessageBox::warning(this, QStringLiteral("进程详情"),
            QStringLiteral("查询 PID=%1 的进程信息失败。").arg(targetPid));
        return;
    }

    // 使用独立窗口打开进程详情，不阻塞当前网络页面。
    ProcessDetailWindow* detailWindow = new ProcessDetailWindow(processRecord, nullptr);
    detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    detailWindow->setWindowFlag(Qt::Window, true);
    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();

    kLogEvent processDetailEvent;
    info << processDetailEvent
        << "[NetworkDock] 打开进程详情窗口, pid=" << targetPid
        << eol;
}



