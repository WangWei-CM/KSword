#include "ServiceDock.Internal.h"

#include <QSignalBlocker>

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
    QString* errorTextOut) const
{
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
    return true;
}


bool ServiceDock::applyServiceFailureSettings(
    const QString& serviceNameText,
    const ServiceRecoverySettings& settings,
    QString* errorTextOut) const
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
    const QString serviceNameText = selectedServiceName();
    const int serviceIndex = findServiceIndexByName(serviceNameText);
    if (serviceIndex < 0 || serviceIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

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
        const QMessageBox::StandardButton confirmButton = QMessageBox::warning(
            this,
            QStringLiteral("高风险动作确认"),
            QStringLiteral("确认将服务“%1”设置为禁用吗？").arg(serviceNameText),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirmButton != QMessageBox::Yes)
        {
            return;
        }
    }

    ks::service::ServiceConfigUpdate update;
    update.changeStartType = true;
    update.startType = targetStartType;
    update.changeDisplayName = true;
    update.displayName = displayNameText.toStdWString();

    std::string errorText;
    if (!ks::service::ChangeServiceConfiguration(
        serviceNameText.toStdWString(),
        update,
        &errorText))
    {
        QMessageBox::warning(
            this,
            QStringLiteral("服务管理"),
            QStringLiteral("修改常规属性失败：\n%1").arg(QString::fromUtf8(errorText.c_str())));
        return;
    }

    // Description and delayed-auto writes remain best-effort to preserve prior UI behavior.
    (void)ks::service::SetServiceDescription(serviceNameText.toStdWString(), descriptionText.toStdWString());
    (void)ks::service::SetDelayedAutoStart(
        serviceNameText.toStdWString(),
        targetStartType == SERVICE_AUTO_START && targetDelayedAutoStart);

    refreshSelectedService();
}


void ServiceDock::applyLogonTabChanges()
{
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

    ks::service::ServiceConfigUpdate update;
    update.changeServiceType = true;
    update.serviceType = targetServiceType;
    update.changeAccount = true;
    update.accountName = useLocalSystem ? std::wstring(L"LocalSystem") : accountText.toStdWString();
    if (!useLocalSystem && (!passwordText.isEmpty() || QString::compare(accountText, selectedEntry.accountText, Qt::CaseInsensitive) != 0))
    {
        update.changePassword = true;
        update.password = passwordText.toStdWString();
    }

    std::string errorText;
    if (!ks::service::ChangeServiceConfiguration(
        serviceNameText.toStdWString(),
        update,
        &errorText))
    {
        QMessageBox::warning(
            this,
            QStringLiteral("服务管理"),
            QStringLiteral("修改登录属性失败：\n%1").arg(QString::fromUtf8(errorText.c_str())));
        return;
    }

    refreshSelectedService();
}


void ServiceDock::applyRecoveryTabChanges()
{
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

    QString errorText;
    if (!applyServiceFailureSettings(serviceNameText, settings, &errorText))
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("修改恢复属性失败：\n%1").arg(errorText));
        return;
    }

    refreshSelectedService();
}
