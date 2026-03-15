void NetworkDock::openPacketDetailWindowFromTableRow(QTableWidget* tableWidget, const int row)
{
    if (tableWidget == nullptr || row < 0)
    {
        return;
    }

    QTableWidgetItem* firstItem = tableWidget->item(row, toPacketColumn(PacketTableColumn::Time));
    if (firstItem == nullptr)
    {
        return;
    }

    const QVariant sequenceVariant = firstItem->data(Qt::UserRole);
    if (!sequenceVariant.isValid())
    {
        return;
    }

    const std::uint64_t sequenceId = static_cast<std::uint64_t>(sequenceVariant.toULongLong());
    openPacketDetailWindowBySequenceId(sequenceId);
}

void NetworkDock::openPacketDetailWindowBySequenceId(const std::uint64_t sequenceId)
{
    const auto iterator = m_packetBySequence.find(sequenceId);
    if (iterator == m_packetBySequence.end())
    {
        QMessageBox::information(this, QStringLiteral("报文详情"), QStringLiteral("该报文已被清理，无法查看详情。"));
        return;
    }

    // 详情窗口使用 show() 非模态弹出，不阻塞主 UI。
    // 独立窗口：不挂在 Dock 下，避免主窗口布局变化影响详情窗口。
    PacketDetailWindow* detailWindow = new PacketDetailWindow(iterator->second, nullptr);
    detailWindow->setWindowFlag(Qt::Window, true);
    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();

    kLogEvent detailWindowEvent;
    info << detailWindowEvent
        << "[NetworkDock] 打开报文详情窗口, sequenceId=" << sequenceId
        << ", pid=" << iterator->second.processId
        << eol;
}

QIcon NetworkDock::resolveProcessIconByPid(const std::uint32_t processId, const std::string& processName)
{
    if (processId == 0)
    {
        return QIcon(":/Icon/process_main.svg");
    }

    const quint32 pidKey = static_cast<quint32>(processId);
    const auto cacheIterator = m_processIconCacheByPid.constFind(pidKey);
    if (cacheIterator != m_processIconCacheByPid.constEnd())
    {
        return cacheIterator.value();
    }

    QIcon processIcon;

    // 先尝试按可执行路径提取系统文件图标。
    const std::string processPath = ks::process::QueryProcessPathByPid(processId);
    if (!processPath.empty())
    {
        static QFileIconProvider fileIconProvider;
        const QString processPathText = QString::fromUtf8(processPath.c_str());
        processIcon = fileIconProvider.icon(QFileInfo(processPathText));
    }

    // 如果路径不可用，则回退到统一图标，保证列内始终有可视图标。
    if (processIcon.isNull())
    {
        processIcon = QIcon(":/Icon/process_main.svg");
    }

    m_processIconCacheByPid.insert(pidKey, processIcon);

    // 对于名称为空的异常条目，也在日志记录一次，方便定位来源。
    if (processName.empty())
    {
        kLogEvent iconEvent;
        warn << iconEvent << "[NetworkDock] 进程名为空，使用默认图标, pid=" << processId << eol;
    }
    return processIcon;
}

bool NetworkDock::packetPassesMonitorFilter(const ks::network::PacketRecord& packetRecord) const
{
    // 1) PID 精确过滤。
    if (m_activePidFilter.has_value() && packetRecord.processId != m_activePidFilter.value())
    {
        return false;
    }

    // 2) 本地 IP 段过滤：只在该条件启用时才解析文本地址，避免无谓开销。
    if (m_activeLocalIpv4RangeFilter.has_value())
    {
        std::uint32_t localIpv4HostOrder = 0;
        if (!tryParseIpv4Text(toQString(packetRecord.localAddress), localIpv4HostOrder))
        {
            // 地址不可解析时保守处理为“不通过”，避免误放行。
            return false;
        }

        const UInt32Range& localIpRange = m_activeLocalIpv4RangeFilter.value();
        if (localIpv4HostOrder < localIpRange.first || localIpv4HostOrder > localIpRange.second)
        {
            return false;
        }
    }

    // 3) 远端 IP 段过滤。
    if (m_activeRemoteIpv4RangeFilter.has_value())
    {
        std::uint32_t remoteIpv4HostOrder = 0;
        if (!tryParseIpv4Text(toQString(packetRecord.remoteAddress), remoteIpv4HostOrder))
        {
            return false;
        }

        const UInt32Range& remoteIpRange = m_activeRemoteIpv4RangeFilter.value();
        if (remoteIpv4HostOrder < remoteIpRange.first || remoteIpv4HostOrder > remoteIpRange.second)
        {
            return false;
        }
    }

    // 4) 本地端口范围过滤。
    if (m_activeLocalPortRangeFilter.has_value())
    {
        const UInt16Range& localPortRange = m_activeLocalPortRangeFilter.value();
        if (packetRecord.localPort < localPortRange.first || packetRecord.localPort > localPortRange.second)
        {
            return false;
        }
    }

    // 5) 远端端口范围过滤。
    if (m_activeRemotePortRangeFilter.has_value())
    {
        const UInt16Range& remotePortRange = m_activeRemotePortRangeFilter.value();
        if (packetRecord.remotePort < remotePortRange.first || packetRecord.remotePort > remotePortRange.second)
        {
            return false;
        }
    }

    // 6) 报文总长度过滤（总长度字段，单位字节）。
    if (m_activePacketSizeRangeFilter.has_value())
    {
        const UInt32Range& packetSizeRange = m_activePacketSizeRangeFilter.value();
        if (packetRecord.totalPacketSize < packetSizeRange.first ||
            packetRecord.totalPacketSize > packetSizeRange.second)
        {
            return false;
        }
    }

    // 所有已启用条件均通过，才允许显示该报文。
    return true;
}

int NetworkDock::toPacketColumn(const PacketTableColumn column)
{
    return static_cast<int>(column);
}

int NetworkDock::toRateLimitColumn(const RateLimitTableColumn column)
{
    return static_cast<int>(column);
}

int NetworkDock::toTcpConnectionColumn(const TcpConnectionTableColumn column)
{
    return static_cast<int>(column);
}

int NetworkDock::toUdpEndpointColumn(const UdpEndpointTableColumn column)
{
    return static_cast<int>(column);
}

bool NetworkDock::tryParsePidText(const QString& pidText, std::uint32_t& pidOut)
{
    bool parseOk = false;
    const unsigned long pidValue = pidText.trimmed().toULong(&parseOk, 10);
    if (!parseOk || pidValue == 0 || pidValue > 0xFFFFFFFFUL)
    {
        return false;
    }

    pidOut = static_cast<std::uint32_t>(pidValue);
    return true;
}

bool NetworkDock::tryParseUnsignedIntegerText(const QString& integerText, std::uint32_t& valueOut)
{
    const QString trimmedText = integerText.trimmed();
    if (trimmedText.isEmpty())
    {
        return false;
    }

    bool parseOk = false;
    qulonglong parsedValue = 0;

    // 优先识别 0x 前缀十六进制输入。
    if (trimmedText.startsWith("0x", Qt::CaseInsensitive))
    {
        parsedValue = trimmedText.mid(2).toULongLong(&parseOk, 16);
    }
    else
    {
        // 无前缀时先尝试十进制，失败再尝试十六进制。
        parsedValue = trimmedText.toULongLong(&parseOk, 10);
        if (!parseOk)
        {
            parsedValue = trimmedText.toULongLong(&parseOk, 16);
        }
    }

    if (!parseOk || parsedValue > static_cast<qulonglong>(0xFFFFFFFFULL))
    {
        return false;
    }

    valueOut = static_cast<std::uint32_t>(parsedValue);
    return true;
}
