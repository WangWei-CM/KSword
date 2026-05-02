#include "NetworkDock.InternalCommon.h"
#include "../theme.h"

#include <string>

using namespace network_dock_detail;
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

    // 流量时间轴连接：
    // - ProcessTraceTimelineWidget 内部复用了 ETW 页的框选、拖拽和滚轮缩放工具；
    // - 这里仅接收最终时间范围，并把它叠加到现有规则组过滤与表格重建流程。
    if (m_packetTimelineWidget != nullptr)
    {
        m_packetTimelineWidget->setSelectionChangedCallback(
            [this](const std::uint64_t start100ns, const std::uint64_t end100ns)
            {
                applyPacketTimelineSelection(start100ns, end100ns);
            });
    }

    // 组合过滤控制连接：
    // - 漏斗按钮控制折叠面板显隐；
    // - 规则组支持新增、应用、导入、导出、默认保存、一键清空。
    connect(m_monitorFilterToggleButton, &QPushButton::toggled, this, [this](const bool checked)
        {
            if (m_monitorFilterPanel != nullptr)
            {
                m_monitorFilterPanel->setVisible(checked);
            }
        });

    connect(m_addMonitorFilterGroupButton, &QPushButton::clicked, this, [this]()
        {
            addMonitorFilterRuleGroup();
        });

    connect(m_applyMonitorFilterButton, &QPushButton::clicked, this, [this]()
        {
            applyMonitorFilters();
        });
    connect(m_clearMonitorFilterButton, &QPushButton::clicked, this, [this]()
        {
            clearAllMonitorFilterConfigurations();
        });
    connect(m_saveMonitorFilterButton, &QPushButton::clicked, this, [this]()
        {
            saveMonitorFilterConfigToDefaultPath();
        });
    connect(m_importMonitorFilterButton, &QPushButton::clicked, this, [this]()
        {
            importMonitorFilterConfigFromUserSelectedPath();
        });
    connect(m_exportMonitorFilterButton, &QPushButton::clicked, this, [this]()
        {
            exportMonitorFilterConfigToUserSelectedPath();
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

    // HTTPS 解析控制连接。
    connect(m_httpsStartProxyButton, &QPushButton::clicked, this, [this]()
        {
            startHttpsProxyService();
        });
    connect(m_httpsStopProxyButton, &QPushButton::clicked, this, [this]()
        {
            stopHttpsProxyService();
        });
    connect(m_httpsTrustCertButton, &QPushButton::clicked, this, [this]()
        {
            ensureHttpsRootCertificateTrusted();
        });
    connect(m_httpsApplyProxyButton, &QPushButton::clicked, this, [this]()
        {
            applyHttpsSystemProxy();
        });
    connect(m_httpsClearProxyButton, &QPushButton::clicked, this, [this]()
        {
            clearHttpsSystemProxy();
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
            // 连接表跳转必须避免同步完整静态查询：
            // - QueryProcessStaticDetailByPid 内部默认包含签名校验；
            // - 详情窗口负责后台补齐字段并懒加载高级页面。
            ks::process::ProcessRecord processRecord;
            processRecord.pid = targetPid;
            processRecord.processName = ks::process::GetProcessNameByPID(targetPid);
            if (processRecord.processName.empty())
            {
                processRecord.processName = "PID_" + std::to_string(targetPid);
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
            contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
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

                addOrTrackProcessPid(targetPid);

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
            contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
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

                addOrTrackProcessPid(targetPid);

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

    // 多线程下载页连接：开始下载、选择目录、URL回车触发。
    connect(m_multiDownloadStartButton, &QPushButton::clicked, this, [this]()
        {
            startMultiThreadDownloadTask();
        });
    connect(m_multiDownloadBrowseDirButton, &QPushButton::clicked, this, [this]()
        {
            browseMultiThreadDownloadDirectory();
        });
    connect(m_multiDownloadUrlEdit, &QLineEdit::returnPressed, this, [this]()
        {
            startMultiThreadDownloadTask();
        });

    // 下载捕获设置连接：开关/后缀变化后写入 JSON。
    connect(m_multiDownloadAutoCaptureClipboardCheck, &QCheckBox::toggled, this, [this](const bool checked)
        {
            m_multiDownloadAutoCaptureClipboardEnabled = checked;
            saveMultiThreadDownloadCaptureSettings();
        });
    connect(m_multiDownloadCaptureSuffixEdit, &QLineEdit::editingFinished, this, [this]()
        {
            saveMultiThreadDownloadCaptureSettings();
        });
    connect(m_multiDownloadSaveCaptureSettingsButton, &QPushButton::clicked, this, [this]()
        {
            saveMultiThreadDownloadCaptureSettings();
        });

    // 剪贴板监听连接：
    // - 仅处理主剪贴板文本变化；
    // - 自动捕获开关关闭时，检测函数会快速返回。
    QClipboard* clipboardObject = QGuiApplication::clipboard(); // clipboardObject：系统主剪贴板对象。
    if (clipboardObject != nullptr)
    {
        connect(clipboardObject, &QClipboard::changed, this, [this](const QClipboard::Mode mode)
            {
                if (mode != QClipboard::Clipboard)
                {
                    return;
                }
                onMultiThreadDownloadClipboardChanged();
            });
    }

    // 多线程下载任务选中变化：切换右侧分段详情与总进度条绑定任务。
    connect(m_multiDownloadTaskTable, &QTableWidget::itemSelectionChanged, this, [this]()
        {
            if (m_multiDownloadTaskTable == nullptr)
            {
                return;
            }

            const QList<QTableWidgetItem*> selectedItemList = m_multiDownloadTaskTable->selectedItems();
            if (selectedItemList.isEmpty())
            {
                m_multiDownloadSelectedTaskId = 0;
                refreshMultiThreadDownloadUi();
                return;
            }

            const int selectedRow = selectedItemList.first()->row();
            QTableWidgetItem* idItem = m_multiDownloadTaskTable->item(selectedRow, 0);
            if (idItem == nullptr)
            {
                m_multiDownloadSelectedTaskId = 0;
                refreshMultiThreadDownloadUi();
                return;
            }

            bool parseOk = false;
            const int selectedTaskId = idItem->text().toInt(&parseOk, 10);
            m_multiDownloadSelectedTaskId = parseOk ? selectedTaskId : 0;
            refreshMultiThreadDownloadUi();
        });

    // 双击报文行：打开独立详情窗口（非阻塞）。
    connect(m_packetTable, &QTableWidget::cellDoubleClicked, this,
        [this](const int row, const int /*column*/)
        {
            openPacketDetailWindowFromTableRow(m_packetTable, row);
        });

    // 右键菜单：查看详情 / 复制行 / 批量复制ASCII/HEX / 重放到请求构造 / 跟踪此进程 / 转到进程详细信息。
    connect(m_packetTable, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position)
        {
            if (m_packetTable == nullptr)
            {
                return;
            }

            const QModelIndex index = m_packetTable->indexAt(position);
            const bool hasSelection =
                (m_packetTable->selectionModel() != nullptr && m_packetTable->selectionModel()->hasSelection());
            if (!index.isValid() && !hasSelection)
            {
                return;
            }

            // collectTargetRows 作用：
            // - 收集本次右键动作要处理的行号集合；
            // - 若已有多选则优先用多选结果；否则回退到当前右键行。
            const auto collectTargetRows = [this, index]() -> std::vector<int>
                {
                    std::vector<int> rowList;
                    if (m_packetTable != nullptr && m_packetTable->selectionModel() != nullptr)
                    {
                        const QModelIndexList selectedRowIndexList =
                            m_packetTable->selectionModel()->selectedRows(toPacketColumn(PacketTableColumn::Time));
                        rowList.reserve(static_cast<std::size_t>(selectedRowIndexList.size()));
                        for (const QModelIndex& selectedRowIndex : selectedRowIndexList)
                        {
                            rowList.push_back(selectedRowIndex.row());
                        }
                    }
                    if (rowList.empty() && index.isValid())
                    {
                        rowList.push_back(index.row());
                    }

                    std::sort(rowList.begin(), rowList.end());
                    rowList.erase(std::unique(rowList.begin(), rowList.end()), rowList.end());
                    return rowList;
                };

            // collectSequenceListByRows 作用：
            // - 按行号提取报文 sequenceId（存于“时间列 UserRole”）；
            // - 后续复制 ASCII/HEX 与打开详情都依赖该序号回查缓存实体。
            const auto collectSequenceListByRows = [this](const std::vector<int>& rowList) -> std::vector<std::uint64_t>
                {
                    std::vector<std::uint64_t> sequenceList;
                    sequenceList.reserve(rowList.size());
                    for (const int row : rowList)
                    {
                        if (m_packetTable == nullptr || row < 0 || row >= m_packetTable->rowCount())
                        {
                            continue;
                        }

                        QTableWidgetItem* timeItem = m_packetTable->item(row, toPacketColumn(PacketTableColumn::Time));
                        if (timeItem == nullptr)
                        {
                            continue;
                        }

                        const QVariant sequenceVariant = timeItem->data(Qt::UserRole);
                        if (!sequenceVariant.isValid())
                        {
                            continue;
                        }
                        sequenceList.push_back(static_cast<std::uint64_t>(sequenceVariant.toULongLong()));
                    }
                    std::sort(sequenceList.begin(), sequenceList.end());
                    sequenceList.erase(std::unique(sequenceList.begin(), sequenceList.end()), sequenceList.end());
                    return sequenceList;
                };

            QMenu contextMenu(this);
            contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
            QAction* detailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("查看报文详情"));
            QAction* copyRowAction = contextMenu.addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制行"));
            QAction* copyAsciiAction = contextMenu.addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制选中报文ASCII"));
            // 仅正文复制动作：不拼接报文头部元信息，只输出 payload 的 ASCII 文本。
            QAction* copyPayloadAsciiOnlyAction = contextMenu.addAction(
                QIcon(":/Icon/process_copy_row.svg"),
                QStringLiteral("复制选中payload ASCII（仅正文）"));
            QAction* copyHexAction = contextMenu.addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制选中报文16进制"));
            // 报文重放动作：把单条报文自动填充到“请求构造”页，用户可二次编辑再执行。
            QAction* replayToManualRequestAction = contextMenu.addAction(
                QIcon(":/Icon/process_refresh.svg"),
                QStringLiteral("重放到请求构造"));
            replayToManualRequestAction->setToolTip(QStringLiteral("将当前报文填充到请求构造页，便于快速重放。"));
            contextMenu.addSeparator();
            QAction* trackProcessAction = contextMenu.addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("跟踪此进程"));
            QAction* gotoProcessDetailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));

            QAction* selectedAction = contextMenu.exec(m_packetTable->viewport()->mapToGlobal(position));
            if (selectedAction == nullptr)
            {
                return;
            }

            const std::vector<int> targetRows = collectTargetRows();
            const int anchorRow = index.isValid() ? index.row() : (targetRows.empty() ? -1 : targetRows.front());
            if (selectedAction == detailAction)
            {
                if (anchorRow >= 0)
                {
                    openPacketDetailWindowFromTableRow(m_packetTable, anchorRow);
                }
            }
            else if (selectedAction == copyRowAction)
            {
                if (anchorRow < 0)
                {
                    return;
                }

                QStringList rowTextList;
                rowTextList.reserve(m_packetTable->columnCount());
                for (int columnIndex = 0; columnIndex < m_packetTable->columnCount(); ++columnIndex)
                {
                    QTableWidgetItem* item = m_packetTable->item(anchorRow, columnIndex);
                    rowTextList.push_back(item == nullptr ? QString() : item->text());
                }
                if (QGuiApplication::clipboard() != nullptr)
                {
                    QGuiApplication::clipboard()->setText(rowTextList.join('\t'));
                }

                kLogEvent copyPacketRowEvent;
                dbg << copyPacketRowEvent
                    << "[NetworkDock] 已复制报文行, row=" << anchorRow
                    << ", columnCount=" << m_packetTable->columnCount()
                    << eol;
            }

            else if (
                selectedAction == copyAsciiAction ||
                selectedAction == copyPayloadAsciiOnlyAction ||
                selectedAction == copyHexAction)
            {
                const std::vector<std::uint64_t> sequenceList = collectSequenceListByRows(targetRows);
                if (sequenceList.empty())
                {
                    return;
                }

                // copyAsciiWithHeaderMode 用途：是否复制“含报文头元信息”的 ASCII 模式。
                const bool copyAsciiWithHeaderMode = (selectedAction == copyAsciiAction);
                // copyPayloadAsciiOnlyMode 用途：是否复制“仅 payload 正文”的 ASCII 模式。
                const bool copyPayloadAsciiOnlyMode = (selectedAction == copyPayloadAsciiOnlyAction);
                // copyHexMode 用途：是否复制十六进制模式。
                const bool copyHexMode = (selectedAction == copyHexAction);

                // blockTextList 用途：按“每个报文一个文本块”拼接复制结果。
                QStringList blockTextList;
                blockTextList.reserve(static_cast<int>(sequenceList.size()));
                for (const std::uint64_t sequenceId : sequenceList)
                {
                    const auto iterator = m_packetBySequence.find(sequenceId);
                    if (iterator == m_packetBySequence.end())
                    {
                        continue;
                    }

                    const ks::network::PacketRecord& packetRecord = iterator->second;
                    if (copyHexMode)
                    {
                        // 十六进制模式：始终保留报文头元信息，方便回溯上下文。
                        blockTextList.push_back(QStringLiteral("%1\n%2")
                            .arg(buildPacketCopyHeaderLine(packetRecord))
                            .arg(buildPacketHexAsciiDumpText(packetRecord)));
                        continue;
                    }

                    const QString payloadAsciiText = buildPayloadAsciiFullText(packetRecord);
                    if (copyPayloadAsciiOnlyMode)
                    {
                        // 仅正文模式：不拼接任何报文头字段，只保留 payload ASCII。
                        blockTextList.push_back(payloadAsciiText);
                        continue;
                    }

                    if (copyAsciiWithHeaderMode)
                    {
                        // ASCII 标准模式：保留报文头元信息 + payload ASCII 正文。
                        blockTextList.push_back(QStringLiteral("%1\n%2")
                            .arg(buildPacketCopyHeaderLine(packetRecord))
                            .arg(payloadAsciiText));
                    }
                }

                if (blockTextList.isEmpty())
                {
                    return;
                }

                // blockJoinSeparator 用途：统一多报文块分隔符，避免复制后粘连难读。
                const QString blockJoinSeparator = copyPayloadAsciiOnlyMode
                    ? QStringLiteral("\n\n")
                    : QStringLiteral("\n\n============================================================\n\n");
                const QString finalText = blockTextList.join(blockJoinSeparator);
                if (QGuiApplication::clipboard() != nullptr)
                {
                    QGuiApplication::clipboard()->setText(finalText);
                }

                // copyModeText 用途：日志输出的模式标识，便于后续问题定位。
                const std::string copyModeText = copyHexMode
                    ? "hex"
                    : (copyPayloadAsciiOnlyMode ? "ascii_payload_only" : "ascii");

                kLogEvent copyPacketBatchEvent;
                info << copyPacketBatchEvent
                    << "[NetworkDock] 批量复制报文内容, mode=" << copyModeText
                    << ", packetCount=" << sequenceList.size()
                    << ", outputChars=" << finalText.size()
                    << eol;
            }
            else if (selectedAction == replayToManualRequestAction)
            {
                if (anchorRow >= 0)
                {
                    replayPacketToManualRequestByTableRow(anchorRow);
                }
            }
            else if (selectedAction == trackProcessAction)
            {
                if (anchorRow >= 0)
                {
                    trackProcessByTableRow(anchorRow);
                }
            }
            else if (selectedAction == gotoProcessDetailAction)
            {
                if (anchorRow >= 0)
                {
                    gotoProcessDetailByTableRow(anchorRow);
                }
            }
        });

    // ARP 缓存页控制连接。
    connect(m_refreshArpButton, &QPushButton::clicked, this, [this]()
        {
            refreshArpCacheTable();
        });
    connect(m_addArpButton, &QPushButton::clicked, this, [this]()
        {
            addArpCacheEntry();
        });
    connect(m_removeArpButton, &QPushButton::clicked, this, [this]()
        {
            removeSelectedArpCacheEntry();
        });
    connect(m_flushArpButton, &QPushButton::clicked, this, [this]()
        {
            flushArpCache();
        });

    // DNS 缓存页控制连接。
    connect(m_refreshDnsButton, &QPushButton::clicked, this, [this]()
        {
            refreshDnsCacheTable();
        });
    connect(m_removeDnsButton, &QPushButton::clicked, this, [this]()
        {
            removeDnsCacheEntry();
        });
    connect(m_flushDnsButton, &QPushButton::clicked, this, [this]()
        {
            flushDnsCache();
        });
    connect(m_dnsTable, &QTableWidget::cellClicked, this, [this](const int row, const int /*column*/)
        {
            if (m_dnsEntryEdit == nullptr || m_dnsTable == nullptr || row < 0 || row >= m_dnsTable->rowCount())
            {
                return;
            }
            QTableWidgetItem* hostItem = m_dnsTable->item(row, 0);
            if (hostItem != nullptr)
            {
                m_dnsEntryEdit->setText(hostItem->text());
            }
        });

    // 存活主机扫描页连接。
    connect(m_startAliveScanButton, &QPushButton::clicked, this, [this]()
        {
            startAliveHostScan();
        });
    connect(m_stopAliveScanButton, &QPushButton::clicked, this, [this]()
        {
            stopAliveHostScan();
        });

    // 初始化按钮可用状态。
    updateMonitorButtonState();

    // 初始化新增页首屏数据。
    refreshMultiThreadDownloadUi();
    refreshArpCacheTable();
    refreshDnsCacheTable();
}

void NetworkDock::startTrafficMonitor()
{
    if (m_trafficService == nullptr)
    {
        return;
    }

    // 正在停止时不允许立刻重启，避免服务线程状态在“停/启”间抖动。
    if (m_monitorStopInProgress.load())
    {
        if (m_monitorStatusLabel != nullptr)
        {
            m_monitorStatusLabel->setText(QStringLiteral("状态：停止中，请稍候..."));
        }
        return;
    }

    // 若上次停止线程对象仍残留（通常已结束），这里做一次回收，避免线程句柄泄漏。
    if (m_monitorStopThread != nullptr && m_monitorStopThread->joinable())
    {
        m_monitorStopThread->join();
    }
    m_monitorStopThread.reset();

    // 每次启动前清零“后台队列丢包计数”，便于观察本次运行状态。
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        m_droppedPacketCount = 0;
    }

    const bool startIssued = m_trafficService->StartCapture();
    m_monitorRunning = startIssued && m_trafficService->IsRunning();
    if (!startIssued || !m_monitorRunning)
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：启动失败"));
    }
    else
    {
        // 监控成功启动后才登记时间轴会话：
        // - 未启动监控的等待时间不进入横轴；
        // - 多次启动/停止会按活动时长连续拼接。
        beginPacketTimelineMonitorSession();
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

    // 线程正在停机时直接返回，避免重复点击导致多个 stop 线程并发。
    if (m_monitorStopInProgress.exchange(true))
    {
        return;
    }

    // UI 立即切换到“停止中”，给用户及时反馈，避免误判“按钮没反应”。
    m_monitorRunning = false;
    if (m_monitorStatusLabel != nullptr)
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：停止中..."));
    }
    updateMonitorButtonState();

    // 先回收上一次 stop 线程对象，确保本轮只保留一个 stop worker。
    if (m_monitorStopThread != nullptr && m_monitorStopThread->joinable())
    {
        m_monitorStopThread->join();
    }
    m_monitorStopThread.reset();

    // 把 StopCapture 放到后台线程，避免主线程 join 导致界面卡顿。
    QPointer<NetworkDock> guardThis(this);
    ks::network::TrafficMonitorService* trafficServicePtr = m_trafficService.get();
    m_monitorStopThread = std::make_unique<std::thread>([guardThis, trafficServicePtr]() {
        if (trafficServicePtr != nullptr)
        {
            trafficServicePtr->StopCapture();
        }

        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }

            // stop 线程退出后再 join，保证不会在 UI 线程长时间阻塞。
            if (guardThis->m_monitorStopThread != nullptr && guardThis->m_monitorStopThread->joinable())
            {
                guardThis->m_monitorStopThread->join();
            }
            guardThis->m_monitorStopThread.reset();
            guardThis->m_monitorStopInProgress.store(false);
            guardThis->m_monitorRunning = false;
            guardThis->endPacketTimelineMonitorSession();
            if (guardThis->m_monitorStatusLabel != nullptr)
            {
                guardThis->m_monitorStatusLabel->setText(QStringLiteral("状态：已停止"));
            }
            guardThis->updateMonitorButtonState();

            kLogEvent stopFinishedEvent;
            info << stopFinishedEvent << "[NetworkDock] 后台停止流程完成，抓包线程已退出。" << eol;
        }, Qt::QueuedConnection);
    });

    kLogEvent stopEvent;
    info << stopEvent << "[NetworkDock] 用户触发网络监控停止（异步）。" << eol;
}

void NetworkDock::updateMonitorButtonState()
{
    const bool stopping = m_monitorStopInProgress.load();
    if (m_startMonitorButton != nullptr)
    {
        m_startMonitorButton->setEnabled(!m_monitorRunning && !stopping);
    }
    if (m_stopMonitorButton != nullptr)
    {
        m_stopMonitorButton->setEnabled(m_monitorRunning && !stopping);
    }
}



