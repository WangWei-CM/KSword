void NetworkDock::initializeConnections()
{
    // 启停抓包与清空表格按钮连接。
    connect(m_startMonitorButton, &QPushButton::clicked, this, [this]()
        {
            startTrafficMonitor();
        });
    connect(m_stopMonitorButton, &QPushButton::clicked, this, [this]()
        {
            stopTrafficMonitor();
        });
    connect(m_clearPacketButton, &QPushButton::clicked, this, [this]()
        {
            clearAllPacketRows();
        });

    // 组合过滤控制连接：
    // - 支持 PID/IP段/端口/包长的“同时应用”；
    // - 所有输入框按 Enter 也会触发应用，便于快速筛查。
    connect(m_applyMonitorFilterButton, &QPushButton::clicked, this, [this]()
        {
            applyMonitorFilters();
        });
    connect(m_clearMonitorFilterButton, &QPushButton::clicked, this, [this]()
        {
            clearMonitorFilters();
        });

    connect(m_pidFilterEdit, &QLineEdit::returnPressed, this, [this]()
        {
            applyMonitorFilters();
        });
    connect(m_localIpFilterEdit, &QLineEdit::returnPressed, this, [this]()
        {
            applyMonitorFilters();
        });
    connect(m_remoteIpFilterEdit, &QLineEdit::returnPressed, this, [this]()
        {
            applyMonitorFilters();
        });
    connect(m_localPortFilterEdit, &QLineEdit::returnPressed, this, [this]()
        {
            applyMonitorFilters();
        });
    connect(m_remotePortFilterEdit, &QLineEdit::returnPressed, this, [this]()
        {
            applyMonitorFilters();
        });

    // 限速规则控制连接。
    connect(m_applyRateLimitButton, &QPushButton::clicked, this, [this]()
        {
            applyOrUpdateRateLimitRule();
        });
    connect(m_removeRateLimitButton, &QPushButton::clicked, this, [this]()
        {
            removeSelectedRateLimitRule();
        });
    connect(m_clearRateLimitButton, &QPushButton::clicked, this, [this]()
        {
            clearAllRateLimitRules();
        });

    // 连接管理控制连接。
    connect(m_refreshConnectionButton, &QPushButton::clicked, this, [this]()
        {
            kLogEvent refreshClickEvent;
            info << refreshClickEvent << "[NetworkDock] 用户触发连接快照手动刷新。" << eol;
            refreshConnectionTables();
        });
    connect(m_autoRefreshConnectionButton, &QPushButton::toggled, this, [this](const bool checked)
        {
            if (m_autoRefreshConnectionButton != nullptr)
            {
                m_autoRefreshConnectionButton->setToolTip(
                    checked ? QStringLiteral("自动刷新开关（已开启）")
                    : QStringLiteral("自动刷新开关（已关闭）"));
            }
            if (m_connectionStatusLabel != nullptr)
            {
                m_connectionStatusLabel->setText(
                    checked ? QStringLiteral("状态：自动刷新已开启")
                    : QStringLiteral("状态：自动刷新已关闭"));
            }

            kLogEvent autoRefreshEvent;
            info << autoRefreshEvent
                << "[NetworkDock] 连接自动刷新开关变更, enabled="
                << (checked ? "true" : "false")
                << eol;
        });
    connect(m_terminateTcpButton, &QPushButton::clicked, this, [this]()
        {
            terminateSelectedTcpConnection();
        });

    // 连接表 PID 解析辅助：
    // - 从任意连接表（TCP/UDP）指定列提取 PID；
    // - 失败时统一弹窗并记录日志，减少重复代码。
    const auto parsePidFromConnectionRow = [this](
        QTableWidget* tableWidget,
        const int row,
        const int pidColumn,
        std::uint32_t& pidOut,
        const QString& sourceTag) -> bool
        {
            if (tableWidget == nullptr || row < 0 || row >= tableWidget->rowCount())
            {
                return false;
            }

            QTableWidgetItem* pidItem = tableWidget->item(row, pidColumn);
            if (pidItem == nullptr)
            {
                return false;
            }

            if (!tryParsePidText(pidItem->text(), pidOut))
            {
                QMessageBox::information(
                    this,
                    QStringLiteral("连接管理"),
                    QStringLiteral("当前行 PID 无效，无法执行该操作。"));

                kLogEvent parsePidFailEvent;
                warn << parsePidFailEvent
                    << "[NetworkDock] 连接表 PID 解析失败, source="
                    << sourceTag.toStdString()
                    << ", row=" << row
                    << ", pidText=" << pidItem->text().toStdString()
                    << eol;
                return false;
            }
            return true;
        };

    // 打开进程详情辅助：
    // - 连接表与流量表都复用同一套“按 PID 打开详情窗口”逻辑；
    // - 统一维护日志与错误提示格式。
    const auto openProcessDetailByPid = [this](const std::uint32_t targetPid, const QString& sourceTag) -> void
        {
            ks::process::ProcessRecord processRecord;
            if (!ks::process::QueryProcessStaticDetailByPid(targetPid, processRecord))
            {
                QMessageBox::warning(
                    this,
                    QStringLiteral("进程详情"),
                    QStringLiteral("查询 PID=%1 的进程信息失败。").arg(targetPid));

                kLogEvent processDetailFailEvent;
                warn << processDetailFailEvent
                    << "[NetworkDock] 连接表打开进程详情失败, source="
                    << sourceTag.toStdString()
                    << ", pid=" << targetPid
                    << eol;
                return;
            }

            ProcessDetailWindow* detailWindow = new ProcessDetailWindow(processRecord, nullptr);
            detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
            detailWindow->setWindowFlag(Qt::Window, true);
            detailWindow->show();
            detailWindow->raise();
            detailWindow->activateWindow();

            kLogEvent processDetailEvent;
            info << processDetailEvent
                << "[NetworkDock] 连接表打开进程详情, source=" << sourceTag.toStdString()
                << ", pid=" << targetPid
                << eol;
        };

    // TCP 表右键菜单：
    // - 支持“终止连接、复制行、跟踪此进程、转到进程详细信息”；
    // - “跟踪此进程”语义为写入 PID 过滤并立即应用。
    connect(
        m_tcpConnectionTable,
        &QWidget::customContextMenuRequested,
        this,
        [this, parsePidFromConnectionRow, openProcessDetailByPid](const QPoint& position)
        {
            const QModelIndex index = m_tcpConnectionTable->indexAt(position);
            if (!index.isValid())
            {
                return;
            }

            QMenu contextMenu(this);
            QAction* terminateAction = contextMenu.addAction(QIcon(":/Icon/process_uncritical.svg"), QStringLiteral("终止此 TCP 连接"));
            QAction* copyRowAction = contextMenu.addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制行"));
            QAction* trackProcessAction = contextMenu.addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("跟踪此进程"));
            QAction* gotoProcessDetailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));
            QAction* selectedAction = contextMenu.exec(m_tcpConnectionTable->viewport()->mapToGlobal(position));
            if (selectedAction == terminateAction)
            {
                m_tcpConnectionTable->selectRow(index.row());
                terminateSelectedTcpConnection();
            }
            else if (selectedAction == copyRowAction)
            {
                m_tcpConnectionTable->selectRow(index.row());
                copySelectedConnectionRowToClipboard(m_tcpConnectionTable);
            }
            else if (selectedAction == trackProcessAction)
            {
                m_tcpConnectionTable->selectRow(index.row());

                std::uint32_t targetPid = 0;
                if (!parsePidFromConnectionRow(
                    m_tcpConnectionTable,
                    index.row(),
                    toTcpConnectionColumn(TcpConnectionTableColumn::Pid),
                    targetPid,
                    QStringLiteral("tcp_table")))
                {
                    return;
                }

                if (m_pidFilterEdit != nullptr)
                {
                    m_pidFilterEdit->setText(QString::number(targetPid));
                }
                applyMonitorFilters();

                kLogEvent trackEvent;
                info << trackEvent
                    << "[NetworkDock] TCP 连接右键触发进程跟踪, pid=" << targetPid
                    << eol;
            }
            else if (selectedAction == gotoProcessDetailAction)
            {
                m_tcpConnectionTable->selectRow(index.row());

                std::uint32_t targetPid = 0;
                if (!parsePidFromConnectionRow(
                    m_tcpConnectionTable,
                    index.row(),
                    toTcpConnectionColumn(TcpConnectionTableColumn::Pid),
                    targetPid,
                    QStringLiteral("tcp_table")))
                {
                    return;
                }
                openProcessDetailByPid(targetPid, QStringLiteral("tcp_table"));
            }
        });

    // UDP 表右键菜单：
    // - UDP 无标准“按连接终止”API，因此不提供 terminate；
    // - 仍支持复制行、跟踪此进程、转到进程详情。
    connect(
        m_udpEndpointTable,
        &QWidget::customContextMenuRequested,
        this,
        [this, parsePidFromConnectionRow, openProcessDetailByPid](const QPoint& position)
        {
            const QModelIndex index = m_udpEndpointTable->indexAt(position);
            if (!index.isValid())
            {
                return;
            }

            QMenu contextMenu(this);
            QAction* copyRowAction = contextMenu.addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制行"));
            QAction* trackProcessAction = contextMenu.addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("跟踪此进程"));
            QAction* gotoProcessDetailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));
            QAction* selectedAction = contextMenu.exec(m_udpEndpointTable->viewport()->mapToGlobal(position));
            if (selectedAction == copyRowAction)
            {
                m_udpEndpointTable->selectRow(index.row());
                copySelectedConnectionRowToClipboard(m_udpEndpointTable);
            }
            else if (selectedAction == trackProcessAction)
            {
                m_udpEndpointTable->selectRow(index.row());

                std::uint32_t targetPid = 0;
                if (!parsePidFromConnectionRow(
                    m_udpEndpointTable,
                    index.row(),
                    toUdpEndpointColumn(UdpEndpointTableColumn::Pid),
                    targetPid,
                    QStringLiteral("udp_table")))
                {
                    return;
                }

                if (m_pidFilterEdit != nullptr)
                {
                    m_pidFilterEdit->setText(QString::number(targetPid));
                }
                applyMonitorFilters();

                kLogEvent trackEvent;
                info << trackEvent
                    << "[NetworkDock] UDP 端点右键触发进程跟踪, pid=" << targetPid
                    << eol;
            }
            else if (selectedAction == gotoProcessDetailAction)
            {
                m_udpEndpointTable->selectRow(index.row());

                std::uint32_t targetPid = 0;
                if (!parsePidFromConnectionRow(
                    m_udpEndpointTable,
                    index.row(),
                    toUdpEndpointColumn(UdpEndpointTableColumn::Pid),
                    targetPid,
                    QStringLiteral("udp_table")))
                {
                    return;
                }
                openProcessDetailByPid(targetPid, QStringLiteral("udp_table"));
            }
        });

    // 请求构造控制连接：执行请求、重置表单、模式切换自动调整默认参数。
    connect(m_manualExecuteButton, &QPushButton::clicked, this, [this]()
        {
            executeManualRequest();
        });
    connect(m_manualResetButton, &QPushButton::clicked, this, [this]()
        {
            kLogEvent resetClickEvent;
            info << resetClickEvent << "[NetworkDock] 用户点击请求构造重置按钮。" << eol;
            resetManualRequestForm();
        });
    connect(m_manualApiCombo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](const int /*index*/)
        {
            if (m_manualApiCombo == nullptr)
            {
                return;
            }

            const ks::network::ManualNetworkApiKind apiKind =
                static_cast<ks::network::ManualNetworkApiKind>(m_manualApiCombo->currentData().toInt());

            // 模式切换时只更新推荐默认参数，不强行覆盖用户已勾选的“手动覆盖”语义。
            if (m_manualOverrideSocketParameterCheck != nullptr &&
                m_manualOverrideSocketParameterCheck->isChecked())
            {
                kLogEvent switchApiEvent;
                dbg << switchApiEvent
                    << "[NetworkDock] 请求构造 API 模式切换（保留手工参数）, api="
                    << ks::network::ManualNetworkApiKindToString(apiKind)
                    << eol;
                return;
            }

            if (m_manualSocketTypeEdit == nullptr || m_manualProtocolEdit == nullptr)
            {
                return;
            }

            if (apiKind == ks::network::ManualNetworkApiKind::WinSockTcp)
            {
                m_manualSocketTypeEdit->setText(QStringLiteral("1")); // SOCK_STREAM
                m_manualProtocolEdit->setText(QStringLiteral("6"));   // IPPROTO_TCP
                if (m_manualConnectBeforeSendCheck != nullptr)
                {
                    m_manualConnectBeforeSendCheck->setChecked(true);
                }
            }
            else
            {
                m_manualSocketTypeEdit->setText(QStringLiteral("2")); // SOCK_DGRAM
                m_manualProtocolEdit->setText(QStringLiteral("17"));  // IPPROTO_UDP
            }

            kLogEvent switchApiEvent;
            dbg << switchApiEvent
                << "[NetworkDock] 请求构造 API 模式切换, api="
                << ks::network::ManualNetworkApiKindToString(apiKind)
                << ", overrideSocket="
                << (m_manualOverrideSocketParameterCheck->isChecked() ? "true" : "false")
                << eol;
        });

    // 双击报文行：打开独立详情窗口（非阻塞）。
    connect(m_packetTable, &QTableWidget::cellDoubleClicked, this,
        [this](const int row, const int /*column*/)
        {
            openPacketDetailWindowFromTableRow(m_packetTable, row);
        });

    // 右键菜单：查看详情 / 复制行 / 跟踪此进程 / 转到进程详细信息。
    connect(m_packetTable, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position)
        {
            const QModelIndex index = m_packetTable->indexAt(position);
            if (!index.isValid())
            {
                return;
            }

            QMenu contextMenu(this);
            QAction* detailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("查看报文详情"));
            QAction* copyRowAction = contextMenu.addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制行"));
            QAction* trackProcessAction = contextMenu.addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("跟踪此进程"));
            QAction* gotoProcessDetailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));
            QAction* selectedAction = contextMenu.exec(m_packetTable->viewport()->mapToGlobal(position));
            if (selectedAction == detailAction)
            {
                openPacketDetailWindowFromTableRow(m_packetTable, index.row());
            }
            else if (selectedAction == copyRowAction)
            {
                QStringList rowTextList;
                for (int columnIndex = 0; columnIndex < m_packetTable->columnCount(); ++columnIndex)
                {
                    QTableWidgetItem* item = m_packetTable->item(index.row(), columnIndex);
                    rowTextList.push_back(item == nullptr ? QString() : item->text());
                }
                if (QGuiApplication::clipboard() != nullptr)
                {
                    QGuiApplication::clipboard()->setText(rowTextList.join('\t'));
                }

                kLogEvent copyPacketRowEvent;
                dbg << copyPacketRowEvent
                    << "[NetworkDock] 已复制报文行, row=" << index.row()
                    << ", columnCount=" << m_packetTable->columnCount()
                    << eol;
            }
            else if (selectedAction == trackProcessAction)
            {
                trackProcessByTableRow(index.row());
            }
            else if (selectedAction == gotoProcessDetailAction)
            {
                gotoProcessDetailByTableRow(index.row());
            }
        });

    // 初始化按钮可用状态。
    updateMonitorButtonState();
}

void NetworkDock::startTrafficMonitor()
{
    if (m_trafficService == nullptr)
    {
        return;
    }

    // 每次启动前清零“后台队列丢包计数”，便于观察本次运行状态。
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        m_droppedPacketCount = 0;
    }

    const bool startIssued = m_trafficService->StartCapture();
    m_monitorRunning = startIssued && m_trafficService->IsRunning();
    if (!startIssued)
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：启动失败"));
    }
    else
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：启动中..."));
    }

    updateMonitorButtonState();

    kLogEvent startEvent;
    info << startEvent << "[NetworkDock] 用户触发网络监控启动。" << eol;
}

void NetworkDock::stopTrafficMonitor()
{
    if (m_trafficService == nullptr)
    {
        return;
    }

    m_trafficService->StopCapture();
    m_monitorRunning = false;
    m_monitorStatusLabel->setText(QStringLiteral("状态：已停止"));
    updateMonitorButtonState();

    kLogEvent stopEvent;
    info << stopEvent << "[NetworkDock] 用户触发网络监控停止。" << eol;
}

void NetworkDock::updateMonitorButtonState()
{
    if (m_startMonitorButton != nullptr)
    {
        m_startMonitorButton->setEnabled(!m_monitorRunning);
    }
    if (m_stopMonitorButton != nullptr)
    {
        m_stopMonitorButton->setEnabled(m_monitorRunning);
    }
}

