void NetworkDock::executeManualRequest()
{
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

    kLogEvent requestStartEvent;
    info << requestStartEvent
        << "[NetworkDock] 执行手动网络请求, api=" << ks::network::ManualNetworkApiKindToString(request.apiKind)
        << ", remote=" << request.remoteAddress << ":" << request.remotePort
        << ", connectBeforeSend=" << (request.connectBeforeSend ? "true" : "false")
        << ", payloadFormat=" << ks::network::ManualPayloadFormatToString(request.payloadFormat)
        << eol;

    // 3) 调用底层工具执行请求。
    ks::network::ManualNetworkResult requestResult;
    const bool executeOk = ks::network::ExecuteManualNetworkRequest(request, &requestResult);

    // 4) 输出结果摘要。
    appendManualRequestLogLine(QStringLiteral("执行结果：ok=%1, sent=%2, recv=%3, detail=%4")
        .arg((executeOk && requestResult.succeeded) ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(static_cast<qulonglong>(requestResult.bytesSent))
        .arg(static_cast<qulonglong>(requestResult.bytesReceived))
        .arg(toQString(requestResult.detailText)));

    if (!requestResult.responseBytes.empty())
    {
        appendManualRequestLogLine(QStringLiteral("响应HEX预览：%1")
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

