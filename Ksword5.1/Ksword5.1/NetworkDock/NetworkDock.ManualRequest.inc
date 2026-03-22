void NetworkDock::executeManualRequest()
{
    // 并发门控：
    // - 同一时刻只允许 1 个手动请求在后台运行；
    // - 防止重复点击导致多个阻塞请求同时堆积。
    static std::atomic_bool s_manualRequestInFlight = false;
    if (s_manualRequestInFlight.exchange(true))
    {
        appendManualRequestLogLine(QStringLiteral("已有请求正在执行，请稍候。"));

        kLogEvent duplicateRequestEvent;
        warn << duplicateRequestEvent << "[NetworkDock] 忽略重复执行请求：上一条请求尚未完成。" << eol;
        return;
    }

    // 保护性检查：若请求构造页控件尚未初始化，则直接返回。
    if (m_manualApiCombo == nullptr ||
        m_manualOverrideSocketParameterCheck == nullptr ||
        m_manualAddressFamilyEdit == nullptr ||
        m_manualSocketTypeEdit == nullptr ||
        m_manualProtocolEdit == nullptr ||
        m_manualSocketFlagsEdit == nullptr ||
        m_manualEnableBindCheck == nullptr ||
        m_manualLocalAddressEdit == nullptr ||
        m_manualLocalPortSpin == nullptr ||
        m_manualRemoteAddressEdit == nullptr ||
        m_manualRemotePortSpin == nullptr ||
        m_manualConnectBeforeSendCheck == nullptr ||
        m_manualReuseAddressCheck == nullptr ||
        m_manualNoDelayCheck == nullptr ||
        m_manualSendTimeoutSpin == nullptr ||
        m_manualRecvTimeoutSpin == nullptr ||
        m_manualPayloadFormatCombo == nullptr ||
        m_manualPayloadEditor == nullptr ||
        m_manualSendFlagsEdit == nullptr ||
        m_manualReceiveFlagsEdit == nullptr ||
        m_manualReceiveAfterSendCheck == nullptr ||
        m_manualReceiveMaxBytesSpin == nullptr ||
        m_manualShutdownSendCheck == nullptr)
    {
        s_manualRequestInFlight.store(false);
        return;
    }

    // 统一输入错误处理：
    // - 先给用户弹窗提示；
    // - 再写入日志，便于后续定位失败字段。
    const auto reportInputError = [this](const QString& errorText)
        {
            QMessageBox::warning(this, QStringLiteral("请求构造"), errorText);

            kLogEvent inputErrorEvent;
            warn << inputErrorEvent
                << "[NetworkDock] 请求构造参数校验失败, detail="
                << errorText.toStdString()
                << eol;

            // 参数错误属于快速失败路径，需要释放执行门控。
            s_manualRequestInFlight.store(false);
        };

    // 1) 组装请求对象。
    ks::network::ManualNetworkRequest request;
    request.apiKind = static_cast<ks::network::ManualNetworkApiKind>(m_manualApiCombo->currentData().toInt());
    request.overrideSocketParameters = m_manualOverrideSocketParameterCheck->isChecked();

    if (request.overrideSocketParameters)
    {
        std::uint32_t parsedValue = 0;
        if (!tryParseUnsignedIntegerText(m_manualAddressFamilyEdit->text(), parsedValue))
        {
            reportInputError(QStringLiteral("addressFamily 格式无效。"));
            return;
        }
        request.addressFamily = static_cast<int>(parsedValue);

        if (!tryParseUnsignedIntegerText(m_manualSocketTypeEdit->text(), parsedValue))
        {
            reportInputError(QStringLiteral("socketType 格式无效。"));
            return;
        }
        request.socketType = static_cast<int>(parsedValue);

        if (!tryParseUnsignedIntegerText(m_manualProtocolEdit->text(), parsedValue))
        {
            reportInputError(QStringLiteral("protocol 格式无效。"));
            return;
        }
        request.protocol = static_cast<int>(parsedValue);

        if (!tryParseUnsignedIntegerText(m_manualSocketFlagsEdit->text(), parsedValue))
        {
            reportInputError(QStringLiteral("socketFlags 格式无效。"));
            return;
        }
        request.socketFlags = static_cast<DWORD>(parsedValue);
    }

    request.enableLocalBind = m_manualEnableBindCheck->isChecked();
    request.localAddress = m_manualLocalAddressEdit->text().trimmed().toStdString();
    request.localPort = static_cast<std::uint16_t>(m_manualLocalPortSpin->value());
    request.remoteAddress = m_manualRemoteAddressEdit->text().trimmed().toStdString();
    request.remotePort = static_cast<std::uint16_t>(m_manualRemotePortSpin->value());
    request.connectBeforeSend = m_manualConnectBeforeSendCheck->isChecked();
    request.enableReuseAddress = m_manualReuseAddressCheck->isChecked();
    request.enableNoDelay = m_manualNoDelayCheck->isChecked();
    request.sendTimeoutMs = static_cast<std::uint32_t>(m_manualSendTimeoutSpin->value());
    request.receiveTimeoutMs = static_cast<std::uint32_t>(m_manualRecvTimeoutSpin->value());

    std::uint32_t parsedFlagValue = 0;
    if (!tryParseUnsignedIntegerText(m_manualSendFlagsEdit->text(), parsedFlagValue))
    {
        reportInputError(QStringLiteral("sendFlags 格式无效。"));
        return;
    }
    request.sendFlags = static_cast<int>(parsedFlagValue);

    if (!tryParseUnsignedIntegerText(m_manualReceiveFlagsEdit->text(), parsedFlagValue))
    {
        reportInputError(QStringLiteral("recvFlags 格式无效。"));
        return;
    }
    request.receiveFlags = static_cast<int>(parsedFlagValue);

    request.receiveAfterSend = m_manualReceiveAfterSendCheck->isChecked();
    request.receiveMaxBytes = static_cast<std::size_t>(m_manualReceiveMaxBytesSpin->value());
    request.shutdownSendAfterWrite = m_manualShutdownSendCheck->isChecked();
    request.payloadFormat = static_cast<ks::network::ManualPayloadFormat>(m_manualPayloadFormatCombo->currentData().toInt());
    request.payloadText = m_manualPayloadEditor->toPlainText().toStdString();

    // 2) 输出请求开始日志（UI + 框架日志双通道）。
    appendManualRequestLogLine(QStringLiteral("开始执行请求：api=%1, remote=%2:%3, payloadLength=%4")
        .arg(toQString(ks::network::ManualNetworkApiKindToString(request.apiKind)))
        .arg(toQString(request.remoteAddress))
        .arg(request.remotePort)
        .arg(static_cast<qulonglong>(request.payloadText.size())));
    appendManualRequestLogLine(QStringLiteral("请求已投递到后台线程，UI 保持可响应。"));

    kLogEvent requestStartEvent;
    info << requestStartEvent
        << "[NetworkDock] 执行手动网络请求, api=" << ks::network::ManualNetworkApiKindToString(request.apiKind)
        << ", remote=" << request.remoteAddress << ":" << request.remotePort
        << ", connectBeforeSend=" << (request.connectBeforeSend ? "true" : "false")
        << ", payloadFormat=" << ks::network::ManualPayloadFormatToString(request.payloadFormat)
        << eol;

    // UI 反馈：后台执行期间禁用按钮，避免重复触发同一请求。
    if (m_manualExecuteButton != nullptr)
    {
        m_manualExecuteButton->setEnabled(false);
        m_manualExecuteButton->setToolTip(QStringLiteral("请求执行中，请稍候..."));
    }

    // 3) 后台线程执行网络请求：
    // - 彻底避免 send/recv 超时等待阻塞 UI 事件循环；
    // - 执行完成后再通过 QueuedConnection 把结果回传 UI 线程。
    QPointer<NetworkDock> dockGuard(this);
    try
    {
        std::thread([dockGuard, request]()
            {
                ks::network::ManualNetworkResult requestResult;
                const bool executeOk = ks::network::ExecuteManualNetworkRequest(request, &requestResult);

                NetworkDock* dockPointer = dockGuard.data();
                if (dockPointer == nullptr)
                {
                    s_manualRequestInFlight.store(false);
                    return;
                }

                QMetaObject::invokeMethod(
                    dockPointer,
                    [dockGuard, executeOk, requestResult = std::move(requestResult)]() mutable
                    {
                        NetworkDock* uiDock = dockGuard.data();
                        if (uiDock == nullptr)
                        {
                            s_manualRequestInFlight.store(false);
                            return;
                        }

                        // 4) 输出结果摘要（此处已回到 UI 线程）。
                        uiDock->appendManualRequestLogLine(QStringLiteral("执行结果：ok=%1, sent=%2, recv=%3, detail=%4")
                            .arg((executeOk && requestResult.succeeded) ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(static_cast<qulonglong>(requestResult.bytesSent))
                            .arg(static_cast<qulonglong>(requestResult.bytesReceived))
                            .arg(toQString(requestResult.detailText)));

                        if (!requestResult.responseBytes.empty())
                        {
                            uiDock->appendManualRequestLogLine(QStringLiteral("响应HEX预览：%1")
                                .arg(formatBytesToHexText(requestResult.responseBytes, 256)));
                        }

                        if (executeOk && requestResult.succeeded)
                        {
                            kLogEvent requestSuccessEvent;
                            info << requestSuccessEvent
                                << "[NetworkDock] 手动网络请求成功, sent=" << requestResult.bytesSent
                                << ", recv=" << requestResult.bytesReceived
                                << ", detail=" << requestResult.detailText
                                << eol;
                        }
                        else
                        {
                            kLogEvent requestFailEvent;
                            warn << requestFailEvent
                                << "[NetworkDock] 手动网络请求失败, wsaError=" << requestResult.wsaErrorCode
                                << ", detail=" << requestResult.detailText
                                << eol;
                        }

                        if (uiDock->m_manualExecuteButton != nullptr)
                        {
                            uiDock->m_manualExecuteButton->setEnabled(true);
                            uiDock->m_manualExecuteButton->setToolTip(QStringLiteral("按上方参数立即执行一次网络请求。"));
                        }

                        s_manualRequestInFlight.store(false);
                    },
                    Qt::QueuedConnection);
            }).detach();
    }
    catch (...)
    {
        // 极端情况下线程创建失败，立即恢复 UI 状态并写日志。
        if (m_manualExecuteButton != nullptr)
        {
            m_manualExecuteButton->setEnabled(true);
            m_manualExecuteButton->setToolTip(QStringLiteral("按上方参数立即执行一次网络请求。"));
        }
        s_manualRequestInFlight.store(false);

        appendManualRequestLogLine(QStringLiteral("后台线程创建失败，请稍后重试。"));
        kLogEvent threadCreateFailEvent;
        err << threadCreateFailEvent << "[NetworkDock] 手动请求后台线程创建失败。" << eol;
    }
}

void NetworkDock::replayPacketToManualRequestByTableRow(const int row)
{
    // replayEvent 用途：贯穿本次“抓包回放草稿生成”全流程的统一日志事件链。
    kLogEvent replayEvent;

    // 基础行校验：右键目标行必须有效且仍在当前表格范围内。
    if (m_packetTable == nullptr || row < 0 || row >= m_packetTable->rowCount())
    {
        warn << replayEvent
            << "[NetworkDock] 报文重放失败：目标行无效, row=" << row
            << eol;
        return;
    }

    // 从时间列 UserRole 取出 sequenceId，用于回查完整报文实体。
    QTableWidgetItem* timeItem = m_packetTable->item(row, toPacketColumn(PacketTableColumn::Time));
    if (timeItem == nullptr)
    {
        warn << replayEvent
            << "[NetworkDock] 报文重放失败：时间列为空, row=" << row
            << eol;
        return;
    }

    const QVariant sequenceVariant = timeItem->data(Qt::UserRole);
    if (!sequenceVariant.isValid())
    {
        warn << replayEvent
            << "[NetworkDock] 报文重放失败：sequenceId 缺失, row=" << row
            << eol;
        return;
    }

    const std::uint64_t sequenceId = static_cast<std::uint64_t>(sequenceVariant.toULongLong());
    const auto packetIterator = m_packetBySequence.find(sequenceId);
    if (packetIterator == m_packetBySequence.end())
    {
        appendManualRequestLogLine(QStringLiteral("回放失败：该报文已被清理，无法生成重放草稿。"));

        warn << replayEvent
            << "[NetworkDock] 报文重放失败：缓存中不存在该序号, sequenceId=" << sequenceId
            << eol;
        return;
    }

    // 请求构造页关键控件校验：缺失任一关键控件都无法完成自动回填。
    if (m_manualApiCombo == nullptr ||
        m_manualOverrideSocketParameterCheck == nullptr ||
        m_manualEnableBindCheck == nullptr ||
        m_manualLocalAddressEdit == nullptr ||
        m_manualLocalPortSpin == nullptr ||
        m_manualRemoteAddressEdit == nullptr ||
        m_manualRemotePortSpin == nullptr ||
        m_manualConnectBeforeSendCheck == nullptr ||
        m_manualPayloadFormatCombo == nullptr ||
        m_manualPayloadEditor == nullptr)
    {
        warn << replayEvent
            << "[NetworkDock] 报文重放失败：请求构造页控件未就绪。"
            << eol;
        return;
    }

    // packetRecord 用途：承载本次要回放的抓包实体，后续统一从该对象提取参数。
    const ks::network::PacketRecord& packetRecord = packetIterator->second;

    // 当前请求构造执行器仅支持 IPv4；IPv6 报文给出明确提示，避免用户误操作。
    std::uint32_t remoteIpv4HostOrder = 0;
    if (!tryParseIpv4Text(toQString(packetRecord.remoteAddress), remoteIpv4HostOrder))
    {
        appendManualRequestLogLine(
            QStringLiteral("回放失败：当前快速重放仅支持 IPv4 报文，IPv6 请在请求构造页手工填写。"));

        warn << replayEvent
            << "[NetworkDock] 报文重放失败：远端地址不是 IPv4, sequenceId=" << sequenceId
            << ", remote=" << packetRecord.remoteAddress
            << eol;
        return;
    }

    // 端口 0 无法作为常规目标端点，提前拦截并提示。
    if (packetRecord.remotePort == 0)
    {
        appendManualRequestLogLine(QStringLiteral("回放失败：该报文远端端口为 0，无法自动生成目标端点。"));

        warn << replayEvent
            << "[NetworkDock] 报文重放失败：远端端口为 0, sequenceId=" << sequenceId
            << eol;
        return;
    }

    // replayApiKind 用途：按抓包协议映射请求构造模式（TCP/UDP）。
    const ks::network::ManualNetworkApiKind replayApiKind =
        (packetRecord.protocol == ks::network::PacketTransportProtocol::Tcp)
        ? ks::network::ManualNetworkApiKind::WinSockTcp
        : ks::network::ManualNetworkApiKind::WinSockUdp;
    const int replayApiComboIndex = m_manualApiCombo->findData(static_cast<int>(replayApiKind));

    // payloadHexText 用途：把 payload 原始字节转为 HEX 文本，供请求构造直接发送。
    const QString payloadHexText = buildPayloadHexFullText(packetRecord);
    // payloadRange 用途：记录本次可回放的 payload 字节范围，便于日志输出与提示。
    const auto payloadRange = buildPayloadByteRange(packetRecord);

    // 统一回填请求构造页：重放先生成“可编辑草稿”，不直接自动执行。
    m_manualOverrideSocketParameterCheck->setChecked(false);
    if (replayApiComboIndex >= 0)
    {
        m_manualApiCombo->setCurrentIndex(replayApiComboIndex);
    }
    m_manualEnableBindCheck->setChecked(false);
    m_manualLocalAddressEdit->setText(QStringLiteral("0.0.0.0"));
    m_manualLocalPortSpin->setValue(0);
    m_manualRemoteAddressEdit->setText(toQString(packetRecord.remoteAddress));
    m_manualRemotePortSpin->setValue(static_cast<int>(packetRecord.remotePort));
    m_manualConnectBeforeSendCheck->setChecked(true);

    const int hexFormatIndex = m_manualPayloadFormatCombo->findData(
        static_cast<int>(ks::network::ManualPayloadFormat::HexBytes));
    if (hexFormatIndex >= 0)
    {
        m_manualPayloadFormatCombo->setCurrentIndex(hexFormatIndex);
    }
    m_manualPayloadEditor->setPlainText(payloadHexText);

    // 自动切到请求构造页，方便用户立刻审阅并二次调整。
    if (m_sideTabWidget != nullptr && m_manualRequestPage != nullptr)
    {
        m_sideTabWidget->setCurrentWidget(m_manualRequestPage);
    }

    appendManualRequestLogLine(QStringLiteral(
        "已载入回放草稿：seq=%1, 协议=%2, 目标=%3:%4, payloadBytes=%5。")
        .arg(static_cast<qulonglong>(packetRecord.sequenceId))
        .arg(toQString(ks::network::PacketProtocolToString(packetRecord.protocol)))
        .arg(toQString(packetRecord.remoteAddress))
        .arg(packetRecord.remotePort)
        .arg(static_cast<qulonglong>(payloadRange.second)));
    appendManualRequestLogLine(QStringLiteral("提示：已自动切换到 HEX 载荷模式，点击“执行请求”即可发送。"));

    if (packetRecord.direction == ks::network::PacketDirection::Inbound)
    {
        appendManualRequestLogLine(QStringLiteral("提示：该报文是入站包，重放时将由本机主动发往远端。"));
    }
    if (packetRecord.packetBytesTruncated)
    {
        appendManualRequestLogLine(QStringLiteral("注意：该报文抓包内容被截断，重放仅使用已保留字节。"));
    }
    if (payloadRange.second == 0)
    {
        appendManualRequestLogLine(QStringLiteral("提示：该报文 payload 为空，执行时将发送空载荷。"));
    }

    info << replayEvent
        << "[NetworkDock] 已生成报文重放草稿, sequenceId=" << sequenceId
        << ", api=" << ks::network::ManualNetworkApiKindToString(replayApiKind)
        << ", remote=" << packetRecord.remoteAddress << ":" << packetRecord.remotePort
        << ", payloadBytes=" << payloadRange.second
        << ", truncated=" << (packetRecord.packetBytesTruncated ? "true" : "false")
        << eol;
}

void NetworkDock::resetManualRequestForm()
{
    // 保护性检查：若页面尚未初始化则直接返回。
    if (m_manualApiCombo == nullptr ||
        m_manualOverrideSocketParameterCheck == nullptr ||
        m_manualAddressFamilyEdit == nullptr ||
        m_manualSocketTypeEdit == nullptr ||
        m_manualProtocolEdit == nullptr ||
        m_manualSocketFlagsEdit == nullptr ||
        m_manualEnableBindCheck == nullptr ||
        m_manualLocalAddressEdit == nullptr ||
        m_manualLocalPortSpin == nullptr ||
        m_manualRemoteAddressEdit == nullptr ||
        m_manualRemotePortSpin == nullptr ||
        m_manualConnectBeforeSendCheck == nullptr ||
        m_manualReuseAddressCheck == nullptr ||
        m_manualNoDelayCheck == nullptr ||
        m_manualSendTimeoutSpin == nullptr ||
        m_manualRecvTimeoutSpin == nullptr ||
        m_manualPayloadFormatCombo == nullptr ||
        m_manualPayloadEditor == nullptr ||
        m_manualSendFlagsEdit == nullptr ||
        m_manualReceiveFlagsEdit == nullptr ||
        m_manualReceiveAfterSendCheck == nullptr ||
        m_manualReceiveMaxBytesSpin == nullptr ||
        m_manualShutdownSendCheck == nullptr)
    {
        return;
    }

    // 默认参数侧重“可直接验证链路”的 TCP 文本请求。
    m_manualApiCombo->setCurrentIndex(0);
    m_manualOverrideSocketParameterCheck->setChecked(false);
    m_manualAddressFamilyEdit->setText(QStringLiteral("2")); // AF_INET
    m_manualSocketTypeEdit->setText(QStringLiteral("1"));    // SOCK_STREAM
    m_manualProtocolEdit->setText(QStringLiteral("6"));      // IPPROTO_TCP
    m_manualSocketFlagsEdit->setText(QStringLiteral("0x1")); // WSA_FLAG_OVERLAPPED
    m_manualEnableBindCheck->setChecked(false);
    m_manualLocalAddressEdit->setText(QStringLiteral("0.0.0.0"));
    m_manualLocalPortSpin->setValue(0);
    m_manualRemoteAddressEdit->setText(QStringLiteral("127.0.0.1"));
    m_manualRemotePortSpin->setValue(80);
    m_manualConnectBeforeSendCheck->setChecked(true);
    m_manualReuseAddressCheck->setChecked(false);
    m_manualNoDelayCheck->setChecked(false);
    m_manualSendTimeoutSpin->setValue(3000);
    m_manualRecvTimeoutSpin->setValue(3000);
    m_manualPayloadFormatCombo->setCurrentIndex(0);
    m_manualPayloadEditor->setPlainText(QStringLiteral("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"));
    m_manualSendFlagsEdit->setText(QStringLiteral("0"));
    m_manualReceiveFlagsEdit->setText(QStringLiteral("0"));
    m_manualReceiveAfterSendCheck->setChecked(true);
    m_manualReceiveMaxBytesSpin->setValue(4096);
    m_manualShutdownSendCheck->setChecked(false);

    appendManualRequestLogLine(QStringLiteral("已恢复请求构造页默认参数。"));

    kLogEvent resetRequestEvent;
    info << resetRequestEvent << "[NetworkDock] 请求构造页参数已恢复默认值。" << eol;
}

void NetworkDock::appendManualRequestLogLine(const QString& logLine)
{
    if (m_manualResultOutput == nullptr)
    {
        return;
    }

    const QString timePrefix = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_manualResultOutput->appendPlainText(QStringLiteral("[%1] %2").arg(timePrefix, logLine));
}

