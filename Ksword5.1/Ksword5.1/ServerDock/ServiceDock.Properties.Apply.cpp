#include "ServiceDock.Internal.h"

#include <QSignalBlocker>
#include "../UI/AsyncOperation.h"
#include "../Internationalization/LanguageManager.h"
#include "../Framework/PrivilegeElevationPrompt.h"

using namespace service_dock_detail;

namespace
{
    // splitRecoveryCommandText 作用：
    // - 把 FailureActions 的 command 文本拆成“程序 + 参数”；
    // - 同时识别是否附带 /fail=%1% 结尾参数。
    void splitRecoveryCommandText(
        const QString& commandText,
        QString* programPathOut,
        QString* argumentsOut,
        bool* appendFailureCountOut)
    {
        if (programPathOut != nullptr)
        {
            *programPathOut = QString();
        }
        if (argumentsOut != nullptr)
        {
            *argumentsOut = QString();
        }
        if (appendFailureCountOut != nullptr)
        {
            *appendFailureCountOut = false;
        }

        QString workingText = commandText.trimmed();
        if (workingText.isEmpty())
        {
            return;
        }

        const QString failTokenText = QStringLiteral("/fail=%1%");
        if (workingText.contains(failTokenText, Qt::CaseInsensitive))
        {
            workingText.replace(failTokenText, QString(), Qt::CaseInsensitive);
            workingText = workingText.trimmed();
            if (appendFailureCountOut != nullptr)
            {
                *appendFailureCountOut = true;
            }
        }

        if (workingText.startsWith('\"'))
        {
            const int endQuoteIndex = workingText.indexOf('\"', 1);
            if (endQuoteIndex > 1)
            {
                if (programPathOut != nullptr)
                {
                    *programPathOut = workingText.mid(1, endQuoteIndex - 1).trimmed();
                }
                if (argumentsOut != nullptr)
                {
                    *argumentsOut = workingText.mid(endQuoteIndex + 1).trimmed();
                }
                return;
            }
        }

        const int firstSpaceIndex = workingText.indexOf(' ');
        if (firstSpaceIndex > 0)
        {
            if (programPathOut != nullptr)
            {
                *programPathOut = workingText.left(firstSpaceIndex).trimmed();
            }
            if (argumentsOut != nullptr)
            {
                *argumentsOut = workingText.mid(firstSpaceIndex + 1).trimmed();
            }
            return;
        }

        if (programPathOut != nullptr)
        {
            *programPathOut = workingText;
        }
    }

    // composeRecoveryCommandText 作用：
    // - 按“程序 + 参数 + 可选 fail token”拼装恢复命令；
    // - 保存恢复配置时复用，避免字符串规则分散。
    QString composeRecoveryCommandText(
        const QString& programPathText,
        const QString& argumentsText,
        const bool appendFailureCount)
    {
        QStringList segmentList;
        const QString normalizedProgramPath = programPathText.trimmed();
        if (!normalizedProgramPath.isEmpty())
        {
            segmentList.push_back(normalizedProgramPath.contains(' ')
                ? QStringLiteral("\"%1\"").arg(normalizedProgramPath)
                : normalizedProgramPath);
        }

        const QString normalizedArguments = argumentsText.trimmed();
        if (!normalizedArguments.isEmpty())
        {
            segmentList.push_back(normalizedArguments);
        }
        if (appendFailureCount)
        {
            segmentList.push_back(QStringLiteral("/fail=%1%"));
        }

        return segmentList.join(QStringLiteral(" ")).trimmed();
    }
}

bool ServiceDock::queryServiceFailureSettings(
    const QString& serviceNameText,
    ServiceRecoverySettings* settingsOut,
    QString* errorTextOut,
    bool* completeOut)
{
    if (completeOut) *completeOut = false;
    if (settingsOut == nullptr || serviceNameText.trimmed().isEmpty())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("恢复配置读取参数无效");
        }
        return false;
    }

    *settingsOut = ServiceRecoverySettings{};

    ks::service::FailureSettings failureSettings;
    std::string errorText;
    if (!ks::service::QueryServiceFailureSettings(
        serviceNameText.trimmed().toStdWString(),
        &failureSettings,
        &errorText))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QString::fromUtf8(errorText.c_str());
        }
        return false;
    }

    settingsOut->resetPeriodDays = static_cast<int>(failureSettings.resetPeriodSeconds / (24u * 60u * 60u));
    settingsOut->rebootMessageText = QString::fromStdWString(failureSettings.rebootMessage).trimmed();

    const QString commandText = QString::fromStdWString(failureSettings.command).trimmed();
    splitRecoveryCommandText(
        commandText,
        &settingsOut->programPathText,
        &settingsOut->programArgumentsText,
        &settingsOut->appendFailureCount);


    if (!failureSettings.actions.empty())
    {
        settingsOut->firstActionType = static_cast<SC_ACTION_TYPE>(failureSettings.actions[0].type);
        if (settingsOut->firstActionType == SC_ACTION_RESTART)
        {
            settingsOut->restartDelayMinutes = static_cast<int>(failureSettings.actions[0].delayMs / (60u * 1000u));
        }
    }
    if (failureSettings.actions.size() > 1)
    {
        settingsOut->secondActionType = static_cast<SC_ACTION_TYPE>(failureSettings.actions[1].type);
        if (settingsOut->secondActionType == SC_ACTION_RESTART)
        {
            settingsOut->restartDelayMinutes = static_cast<int>(failureSettings.actions[1].delayMs / (60u * 1000u));
        }
    }
    if (failureSettings.actions.size() > 2)
    {
        settingsOut->subsequentActionType = static_cast<SC_ACTION_TYPE>(failureSettings.actions[2].type);
        if (settingsOut->subsequentActionType == SC_ACTION_RESTART)
        {
            settingsOut->restartDelayMinutes = static_cast<int>(failureSettings.actions[2].delayMs / (60u * 1000u));
        }
    }
    settingsOut->failureActionsFlag = failureSettings.failureActionsOnNonCrash;
    if (completeOut) *completeOut = failureSettings.hasFailureActions && failureSettings.hasFailureActionsFlag;
    return true;
}


bool ServiceDock::applyServiceFailureSettings(
    const QString& serviceNameText,
    const ServiceRecoverySettings& settings,
    QString* errorTextOut)
{
    if (serviceNameText.trimmed().isEmpty())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("恢复配置写入参数无效");
        }
        return false;
    }

    const QString commandText = composeRecoveryCommandText(
        settings.programPathText,
        settings.programArgumentsText,
        settings.appendFailureCount);
    const DWORD restartDelayMs = static_cast<DWORD>(settings.restartDelayMinutes * 60 * 1000);
    const SC_ACTION_TYPE actionTypeArray[3]{
        settings.firstActionType,
        settings.secondActionType,
        settings.subsequentActionType
    };

    ks::service::FailureSettings failureSettings;
    failureSettings.resetPeriodSeconds = static_cast<std::uint32_t>(settings.resetPeriodDays * 24 * 60 * 60);
    failureSettings.rebootMessage = settings.rebootMessageText.toStdWString();
    failureSettings.command = commandText.toStdWString();
    failureSettings.failureActionsOnNonCrash = settings.failureActionsFlag;
    failureSettings.actions.reserve(3);
    for (int actionIndex = 0; actionIndex < 3; ++actionIndex)
    {
        ks::service::FailureAction action;
        action.type = static_cast<std::uint32_t>(actionTypeArray[actionIndex]);
        action.delayMs = (actionTypeArray[actionIndex] == SC_ACTION_RESTART) ? restartDelayMs : 0u;
        failureSettings.actions.push_back(action);
    }

    std::string errorText;
    if (!ks::service::ApplyServiceFailureSettings(
        serviceNameText.trimmed().toStdWString(),
        failureSettings,
        &errorText))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QString::fromUtf8(errorText.c_str());
        }
        return false;
    }
    return true;
}


void ServiceDock::applyGeneralTabChanges()
{
    reconcileConfigReceipt();
    if (m_configOperation->isInFlight()) return;
    const QString serviceNameText = selectedServiceName();
    const int serviceIndex = findServiceIndexByName(serviceNameText);
    if (serviceIndex < 0 || serviceIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const ServiceEntry selectedEntry = m_serviceList[static_cast<std::size_t>(serviceIndex)];
    const auto detailRevision = m_detailRevision;
    const DWORD targetStartType = static_cast<DWORD>(m_generalStartTypeCombo->currentData().toULongLong());
    const bool targetDelayedAutoStart = m_generalDelayedAutoCheck->isChecked();
    const QString displayNameText = m_generalDisplayNameEdit->text().trimmed();
    const QString descriptionText = m_generalDescriptionEdit->toPlainText().trimmed();

    if (displayNameText.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("显示名不能为空。"));
        return;
    }
    if (targetStartType == SERVICE_DISABLED)
    {
        const QPointer<ServiceDock> guarded(this);
        const QMessageBox::StandardButton confirmButton = QMessageBox::warning(
            this,
            QStringLiteral("高风险动作确认"),
            QStringLiteral("确认将服务“%1”设置为禁用吗？").arg(serviceNameText),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (!guarded || confirmButton != QMessageBox::Yes)
        {
            return;
        }
    }

    ConfigWriteRequest request;
    request.entry = selectedEntry;
    request.detailRevision = detailRevision;
    request.entry.serviceNameText = serviceNameText;
    request.kind = ConfigWriteKind::General;
    request.payload = std::make_shared<ConfigWritePayload>();
    auto& update = request.payload->update;
    update.changeStartType = true;
    update.startType = targetStartType;
    update.changeDisplayName = true;
    update.displayName = displayNameText.toStdWString();
    request.description = descriptionText;
    request.delayedAuto = targetStartType == SERVICE_AUTO_START && targetDelayedAutoStart;
    request.failureText = ks::i18n::sourceText(QStringLiteral("修改常规属性失败：\n%1"));
    submitConfigWrite(std::move(request));
}


void ServiceDock::applyLogonTabChanges()
{
    reconcileConfigReceipt();
    if (m_configOperation->isInFlight()) return;
    const QString serviceNameText = selectedServiceName();
    const int serviceIndex = findServiceIndexByName(serviceNameText);
    if (serviceIndex < 0 || serviceIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const ServiceEntry& selectedEntry = m_serviceList[static_cast<std::size_t>(serviceIndex)];
    const bool useLocalSystem = m_logonLocalSystemRadio->isChecked();
    const QString accountText = m_logonAccountEdit->text().trimmed();
    const QString passwordText = m_logonPasswordEdit->text();
    const QString confirmPasswordText = m_logonConfirmPasswordEdit->text();

    if (!useLocalSystem && accountText.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("登录帐户不能为空。"));
        return;
    }
    if (!useLocalSystem && passwordText != confirmPasswordText)
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("两次输入的密码不一致。"));
        return;
    }

    const DWORD baseServiceType = selectedEntry.serviceTypeValue & (~SERVICE_INTERACTIVE_PROCESS);
    const DWORD targetServiceType = (useLocalSystem && m_logonDesktopInteractCheck->isChecked())
        ? (baseServiceType | SERVICE_INTERACTIVE_PROCESS)
        : baseServiceType;

    ConfigWriteRequest request;
    request.entry = selectedEntry;
    request.detailRevision = m_detailRevision;
    request.kind = ConfigWriteKind::Logon;
    request.payload = std::make_shared<ConfigWritePayload>();
    auto& update = request.payload->update;
    update.changeServiceType = true;
    update.serviceType = targetServiceType;
    update.changeAccount = true;
    update.accountName = useLocalSystem ? std::wstring(L"LocalSystem") : accountText.toStdWString();
    if (!useLocalSystem && (!passwordText.isEmpty() || QString::compare(accountText, selectedEntry.accountText, Qt::CaseInsensitive) != 0))
    {
        update.changePassword = true;
        update.password = passwordText.toStdWString();
    }
    request.failureText = ks::i18n::sourceText(QStringLiteral("修改登录属性失败：\n%1"));
    if (submitConfigWrite(std::move(request)))
    {
        m_logonPasswordEdit->clear();
        m_logonConfirmPasswordEdit->clear();
    }
}


void ServiceDock::applyRecoveryTabChanges()
{
    reconcileConfigReceipt();
    if (m_configOperation->isInFlight()) return;
    const QString serviceNameText = selectedServiceName();
    if (serviceNameText.isEmpty())
    {
        return;
    }

    ServiceRecoverySettings settings;
    settings.firstActionType = static_cast<SC_ACTION_TYPE>(m_recoveryFirstActionCombo->currentData().toInt());
    settings.secondActionType = static_cast<SC_ACTION_TYPE>(m_recoverySecondActionCombo->currentData().toInt());
    settings.subsequentActionType = static_cast<SC_ACTION_TYPE>(m_recoverySubsequentActionCombo->currentData().toInt());
    settings.resetPeriodDays = m_recoveryResetDaysSpin->value();
    settings.restartDelayMinutes = m_recoveryRestartMinutesSpin->value();
    settings.failureActionsFlag = m_recoveryFailureActionsFlagCheck->isChecked();
    settings.rebootMessageText = m_recoveryRebootMessageEdit->text().trimmed();
    settings.programPathText = m_recoveryProgramEdit->text().trimmed();
    settings.programArgumentsText = m_recoveryArgumentsEdit->text().trimmed();
    settings.appendFailureCount = m_recoveryAppendFailCountCheck->isChecked();

    const int index = findServiceIndexByName(serviceNameText);
    if (index < 0 || index >= static_cast<int>(m_serviceList.size())) return;
    ConfigWriteRequest request;
    request.entry = m_serviceList[static_cast<std::size_t>(index)];
    request.detailRevision = m_detailRevision;
    request.kind = ConfigWriteKind::Recovery;
    request.recovery = settings;
    request.failureText = ks::i18n::sourceText(QStringLiteral("修改恢复属性失败：\n%1"));
    submitConfigWrite(std::move(request));
}

ServiceDock::ConfigWritePayload::~ConfigWritePayload()
{
    if (!update.password.empty()) SecureZeroMemory(update.password.data(), update.password.size() * sizeof(wchar_t));
}

ServiceDock::ConfigWriteResult ServiceDock::executeConfigWrite(const ConfigWriteRequest& input)
{
    ConfigWriteResult result;
    const std::wstring serviceName = input.entry.serviceNameText.toStdWString();
    try
    {
        if (input.kind == ConfigWriteKind::Recovery)
        {
            result.writeOk = applyServiceFailureSettings(input.entry.serviceNameText, input.recovery, &result.writeError);
        }
        else
        {
            std::string error;
            const bool primaryOk = ks::service::ChangeServiceConfiguration(serviceName, input.payload->update, &error, &result.errorCode);
            result.writeOk = primaryOk;
            result.writeError = QString::fromUtf8(error.c_str());
            if (primaryOk && input.kind == ConfigWriteKind::General)
            {
                error.clear();
                if (!ks::service::SetServiceDescription(serviceName, input.description.toStdWString(), &error))
                {
                    result.descriptionError = QString::fromUtf8(error.c_str());
                    result.writeOk = false;
                }
            }
            if (primaryOk && (input.kind == ConfigWriteKind::General || input.kind == ConfigWriteKind::StartType))
            {
                error.clear();
                if (!ks::service::SetDelayedAutoStart(serviceName, input.delayedAuto, &error, &result.delayedErrorCode))
                {
                    result.delayedError = QString::fromUtf8(error.c_str());
                    result.writeOk = false;
                }
            }
        }
    }
    catch (...)
    {
        result.writeOk = false;
        result.writeError = QStringLiteral("服务配置写入异常，可能已部分生效。");
    }
    if (input.payload)
    {
        auto& password = input.payload->update.password;
        if (!password.empty()) SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
        password.clear();
    }
    // Always read after the one accepted write sequence; optional fields retain their actual availability.
    try
    {
        ks::service::ServiceRecord nativeReadback;
        result.snapshotOk = querySingleServiceSnapshot(input.entry.serviceNameText,
            input.entry.scmRecordPresent, input.entry.registryScanCompleted, &result.readback, &result.readbackError, &nativeReadback);
        result.readbackOk = result.snapshotOk && nativeReadback.hasConfig;
        if (input.kind == ConfigWriteKind::General)
            result.readbackOk = result.readbackOk && nativeReadback.hasDescription && nativeReadback.config.hasDelayedAutoStart;
        else if (input.kind == ConfigWriteKind::StartType)
            result.readbackOk = result.readbackOk && nativeReadback.config.hasDelayedAutoStart;
        if (result.snapshotOk && !result.readbackOk) result.readbackError = QStringLiteral("最终配置回读缺少所需字段。");
    }
    catch (...) { result.readbackError = QStringLiteral("最终配置回读发生异常。"); }
    if (input.kind == ConfigWriteKind::Recovery)
    {
        try
        {
            bool complete = false;
            result.recoveryReadbackOk = queryServiceFailureSettings(input.entry.serviceNameText, &result.recovery, &result.recoveryReadbackError, &complete) && complete;
            if (!result.recoveryReadbackOk && result.recoveryReadbackError.isEmpty()) result.recoveryReadbackError = QStringLiteral("最终配置回读缺少所需字段。");
        }
        catch (...) { result.recoveryReadbackError = QStringLiteral("最终配置回读发生异常。"); }
    }
    return result;
}

void ServiceDock::updateConfigUiState()
{
    if (!m_generalTabPage) return;
    const bool busy = m_configOperation->isInFlight();
    m_generalTabPage->setEnabled(!busy);
    m_logonTabPage->setEnabled(!busy);
    m_recoveryTabPage->setEnabled(!busy);
    syncToolbarStateWithSelection(true);
    const int index = findServiceIndexByName(selectedServiceName());
    const bool available = index >= 0 && index < static_cast<int>(m_serviceList.size()) && m_serviceList[static_cast<std::size_t>(index)].scmRecordPresent && !busy;
    m_applyStartTypeButton->setEnabled(available && !isServiceStatePending(index >= 0 ? m_serviceList[static_cast<std::size_t>(index)].currentState : 0));
    m_generalApplyButton->setEnabled(available);
    m_logonApplyButton->setEnabled(available);
    const auto& recovery = m_detailResults[static_cast<int>(DetailKind::Recovery)];
    m_recoveryApplyButton->setEnabled(available && recovery && recovery->revision == m_detailRevision);
}

void ServiceDock::reconcileConfigReceipt()
{
    if (m_configPendingReceipt && !m_configOperation->isInFlight())
    {
        m_configPendingReceipt = false;
        if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("服务配置未返回有效回执，请重载以核实当前配置；不会自动重试写入。")));
    }
    updateConfigUiState();
}

bool ServiceDock::submitConfigWrite(ConfigWriteRequest request)
{
    reconcileConfigReceipt();
    if (m_configOperation->isInFlight()) return false;
    const auto kind = request.kind;
    const auto revision = request.detailRevision;
    const QString serviceName = request.entry.serviceNameText;
    const QString failureText = request.failureText;
    const int progressPid = request.progressPid;
    const QString finishedText = ks::i18n::sourceText(QStringLiteral("服务配置写入及回读已结束"));
    m_configPendingReceipt = true;
    const auto ticket = m_configOperation->submitTransaction<ConfigWriteRequest, ConfigWriteResult>(std::move(request),
        [](const ConfigWriteRequest& input) { return executeConfigWrite(input); },
        [this, kind, revision, serviceName, failureText, progressPid](const ConfigWriteResult& result, std::uint64_t) {
            m_configPendingReceipt = false;
            const QPointer<ServiceDock> guarded(this);
            const bool sameSelection = serviceName.compare(selectedServiceName(), Qt::CaseInsensitive) == 0;
            const bool applyOwnDetail = sameSelection && revision == m_detailRevision;
            if (result.snapshotOk)
            {
                applyServiceUpdateToCache(result.readback);
                rebuildServiceTable(!applyOwnDetail);
                if (!guarded) return true;
                if (kind == ConfigWriteKind::Recovery && result.recoveryReadbackOk && serviceName.compare(selectedServiceName(), Qt::CaseInsensitive) == 0 && applyOwnDetail)
                {
                    auto detail = std::make_shared<ServiceDetailResult>();
                    detail->serviceName = serviceName;
                    detail->revision = m_detailRevision;
                    detail->kind = DetailKind::Recovery;
                    detail->queryOk = true;
                    detail->recovery = result.recovery;
                    m_detailResults[static_cast<int>(DetailKind::Recovery)] = detail;
                    applyCurrentDetail(*detail);
                }
            }
            QStringList errors;
            if (!result.writeError.isEmpty()) errors.append(ks::i18n::sourceText(result.writeError));
            if (!result.descriptionError.isEmpty()) errors.append(ks::i18n::sourceText(QStringLiteral("描述写入失败：%1")).arg(result.descriptionError));
            if (!result.delayedError.isEmpty()) errors.append(ks::i18n::sourceText(QStringLiteral("延迟启动写入失败：%1")).arg(result.delayedError));
            if (!result.readbackOk) errors.append(ks::i18n::sourceText(QStringLiteral("最终配置回读失败：%1")).arg(ks::i18n::sourceText(result.readbackError)));
            if (kind == ConfigWriteKind::Recovery && !result.recoveryReadbackOk) errors.append(ks::i18n::sourceText(QStringLiteral("恢复配置回读失败：%1")).arg(ks::i18n::sourceText(result.recoveryReadbackError)));
            updateConfigUiState();
            if (progressPid) kPro.finish(progressPid, errors.isEmpty() && result.writeOk ? kProgressState::Success : kProgressState::Failure, "配置写入及回读结束");
            if (m_summaryLabel) m_summaryLabel->setText(errors.isEmpty() && result.writeOk
                ? ks::i18n::sourceText(QStringLiteral("服务配置已写入并完成最终回读。"))
                : ks::i18n::sourceText(QStringLiteral("服务配置写入或回读未完整成功，请核对当前配置。")));
            if (kind == ConfigWriteKind::StartType && result.errorCode)
            {
                const bool prompted = ks::ui::promptForPrivilegeFailure(this, QStringLiteral("修改服务启动类型"), result.errorCode);
                if (!guarded) return true;
                if (prompted) return true;
            }
            if (kind == ConfigWriteKind::StartType && result.delayedErrorCode)
            {
                (void)ks::ui::promptForPrivilegeFailure(this, QStringLiteral("修改服务延迟启动"), result.delayedErrorCode);
                if (!guarded) return true;
            }
            if (!errors.isEmpty() || !result.writeOk)
            {
                const QString message = result.writeOk
                    ? ks::i18n::sourceText(QStringLiteral("服务配置已写入，但最终回读未完整成功：\n%1")).arg(errors.join(QLatin1Char('\n')))
                    : failureText.arg(errors.join(QLatin1Char('\n')));
                QMessageBox::warning(this, QStringLiteral("服务管理"), message);
            }
            return true;
        },
        [this, progressPid](std::exception_ptr) {
            m_configPendingReceipt = false;
            updateConfigUiState();
            if (progressPid) kPro.finish(progressPid, kProgressState::Failure, "配置结果未知");
            if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("服务配置未返回有效回执，请重载以核实当前配置；不会自动重试写入。")));
        },
        [progressPid, finishedText](ks::ui::AsyncOperationOutcome outcome) {
            if (progressPid && outcome != ks::ui::AsyncOperationOutcome::Applied) kPro.set(progressPid, finishedText.toUtf8().constData(), 0, 100.0f);
        });
    if (!ticket)
    {
        m_configPendingReceipt = false;
        updateConfigUiState();
        if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("宿主正在关闭或已有配置事务，服务配置未提交。")));
        return false;
    }
    if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("正在写入服务配置并准备最终回读...")));
    return true;
}
