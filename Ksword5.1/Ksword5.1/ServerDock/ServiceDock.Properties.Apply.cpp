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

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = winErrorText(::GetLastError());
        }
        return false;
    }

    SC_HANDLE serviceHandle = ::OpenServiceW(
        scmHandle,
        reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
        SERVICE_QUERY_CONFIG);
    if (serviceHandle == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = winErrorText(::GetLastError());
        }
        ::CloseServiceHandle(scmHandle);
        return false;
    }

    DWORD requiredBytes = 0;
    ::QueryServiceConfig2W(serviceHandle, SERVICE_CONFIG_FAILURE_ACTIONS, nullptr, 0, &requiredBytes);
    if (requiredBytes > 0)
    {
        std::vector<std::uint8_t> failureBuffer(requiredBytes);
        const BOOL queryOk = ::QueryServiceConfig2W(
            serviceHandle,
            SERVICE_CONFIG_FAILURE_ACTIONS,
            failureBuffer.data(),
            requiredBytes,
            &requiredBytes);
        if (queryOk != FALSE)
        {
            const SERVICE_FAILURE_ACTIONSW* failurePointer =
                reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(failureBuffer.data());
            settingsOut->resetPeriodDays = static_cast<int>(failurePointer->dwResetPeriod / (24u * 60u * 60u));
            settingsOut->rebootMessageText = (failurePointer->lpRebootMsg != nullptr)
                ? QString::fromWCharArray(failurePointer->lpRebootMsg).trimmed()
                : QString();

            const QString commandText = (failurePointer->lpCommand != nullptr)
                ? QString::fromWCharArray(failurePointer->lpCommand).trimmed()
                : QString();
            splitRecoveryCommandText(
                commandText,
                &settingsOut->programPathText,
                &settingsOut->programArgumentsText,
                &settingsOut->appendFailureCount);

            if (failurePointer->cActions > 0)
            {
                settingsOut->firstActionType = failurePointer->lpsaActions[0].Type;
                if (failurePointer->lpsaActions[0].Type == SC_ACTION_RESTART)
                {
                    settingsOut->restartDelayMinutes =
                        static_cast<int>(failurePointer->lpsaActions[0].Delay / (60u * 1000u));
                }
            }
            if (failurePointer->cActions > 1)
            {
                settingsOut->secondActionType = failurePointer->lpsaActions[1].Type;
                if (failurePointer->lpsaActions[1].Type == SC_ACTION_RESTART)
                {
                    settingsOut->restartDelayMinutes =
                        static_cast<int>(failurePointer->lpsaActions[1].Delay / (60u * 1000u));
                }
            }
            if (failurePointer->cActions > 2)
            {
                settingsOut->subsequentActionType = failurePointer->lpsaActions[2].Type;
                if (failurePointer->lpsaActions[2].Type == SC_ACTION_RESTART)
                {
                    settingsOut->restartDelayMinutes =
                        static_cast<int>(failurePointer->lpsaActions[2].Delay / (60u * 1000u));
                }
            }
        }
    }

    requiredBytes = 0;
    ::QueryServiceConfig2W(serviceHandle, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, nullptr, 0, &requiredBytes);
    if (requiredBytes > 0)
    {
        std::vector<std::uint8_t> flagBuffer(requiredBytes);
        if (::QueryServiceConfig2W(
            serviceHandle,
            SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
            flagBuffer.data(),
            requiredBytes,
            &requiredBytes) != FALSE)
        {
            const SERVICE_FAILURE_ACTIONS_FLAG* flagPointer =
                reinterpret_cast<const SERVICE_FAILURE_ACTIONS_FLAG*>(flagBuffer.data());
            settingsOut->failureActionsFlag = flagPointer->fFailureActionsOnNonCrashFailures != FALSE;
        }
    }

    ::CloseServiceHandle(serviceHandle);
    ::CloseServiceHandle(scmHandle);
    return true;
}

bool ServiceDock::applyServiceFailureSettings(
    const QString& serviceNameText,
    const ServiceRecoverySettings& settings,
    QString* errorTextOut) const
{
    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = winErrorText(::GetLastError());
        }
        return false;
    }

    SC_HANDLE serviceHandle = ::OpenServiceW(
        scmHandle,
        reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
        SERVICE_CHANGE_CONFIG);
    if (serviceHandle == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = winErrorText(::GetLastError());
        }
        ::CloseServiceHandle(scmHandle);
        return false;
    }

    const QString commandText = composeRecoveryCommandText(
        settings.programPathText,
        settings.programArgumentsText,
        settings.appendFailureCount);
    const DWORD restartDelayMs = static_cast<DWORD>(settings.restartDelayMinutes * 60 * 1000);
    SC_ACTION actionArray[3]{};
    const SC_ACTION_TYPE actionTypeArray[3]{
        settings.firstActionType,
        settings.secondActionType,
        settings.subsequentActionType
    };

    for (int actionIndex = 0; actionIndex < 3; ++actionIndex)
    {
        actionArray[actionIndex].Type = actionTypeArray[actionIndex];
        actionArray[actionIndex].Delay =
            (actionTypeArray[actionIndex] == SC_ACTION_RESTART) ? restartDelayMs : 0u;
    }

    std::wstring rebootMessageText = settings.rebootMessageText.toStdWString();
    std::wstring commandWideText = commandText.toStdWString();

    SERVICE_FAILURE_ACTIONSW failureSettings{};
    failureSettings.dwResetPeriod = static_cast<DWORD>(settings.resetPeriodDays * 24 * 60 * 60);
    failureSettings.lpRebootMsg = rebootMessageText.empty() ? nullptr : rebootMessageText.data();
    failureSettings.lpCommand = commandWideText.empty() ? nullptr : commandWideText.data();
    failureSettings.cActions = 3;
    failureSettings.lpsaActions = actionArray;

    if (::ChangeServiceConfig2W(
        serviceHandle,
        SERVICE_CONFIG_FAILURE_ACTIONS,
        reinterpret_cast<LPBYTE>(&failureSettings)) == FALSE)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = winErrorText(::GetLastError());
        }
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        return false;
    }

    SERVICE_FAILURE_ACTIONS_FLAG failureFlag{};
    failureFlag.fFailureActionsOnNonCrashFailures = settings.failureActionsFlag ? TRUE : FALSE;
    if (::ChangeServiceConfig2W(
        serviceHandle,
        SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
        reinterpret_cast<LPBYTE>(&failureFlag)) == FALSE)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = winErrorText(::GetLastError());
        }
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        return false;
    }

    ::CloseServiceHandle(serviceHandle);
    ::CloseServiceHandle(scmHandle);
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

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开 SCM 失败：\n%1").arg(winErrorText(::GetLastError())));
        return;
    }

    SC_HANDLE serviceHandle = ::OpenServiceW(
        scmHandle,
        reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
        SERVICE_CHANGE_CONFIG);
    if (serviceHandle == nullptr)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(scmHandle);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开服务失败：\n%1").arg(errorText));
        return;
    }

    const BOOL changeConfigOk = ::ChangeServiceConfigW(
        serviceHandle,
        SERVICE_NO_CHANGE,
        targetStartType,
        SERVICE_NO_CHANGE,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        reinterpret_cast<LPCWSTR>(displayNameText.utf16()));
    if (changeConfigOk == FALSE)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("修改常规属性失败：\n%1").arg(errorText));
        return;
    }

    SERVICE_DESCRIPTIONW descriptionInfo{};
    std::wstring descriptionWideText = descriptionText.toStdWString();
    descriptionInfo.lpDescription = descriptionWideText.empty() ? nullptr : descriptionWideText.data();
    ::ChangeServiceConfig2W(serviceHandle, SERVICE_CONFIG_DESCRIPTION, reinterpret_cast<LPBYTE>(&descriptionInfo));

    SERVICE_DELAYED_AUTO_START_INFO delayedInfo{};
    delayedInfo.fDelayedAutostart = (targetStartType == SERVICE_AUTO_START && targetDelayedAutoStart) ? TRUE : FALSE;
    ::ChangeServiceConfig2W(
        serviceHandle,
        SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
        reinterpret_cast<LPBYTE>(&delayedInfo));

    ::CloseServiceHandle(serviceHandle);
    ::CloseServiceHandle(scmHandle);
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

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开 SCM 失败：\n%1").arg(winErrorText(::GetLastError())));
        return;
    }

    SC_HANDLE serviceHandle = ::OpenServiceW(
        scmHandle,
        reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
        SERVICE_CHANGE_CONFIG);
    if (serviceHandle == nullptr)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(scmHandle);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开服务失败：\n%1").arg(errorText));
        return;
    }

    LPCWSTR startNamePointer = useLocalSystem
        ? L"LocalSystem"
        : reinterpret_cast<LPCWSTR>(accountText.utf16());
    LPCWSTR passwordPointer = nullptr;
    std::wstring passwordWideText;
    if (!useLocalSystem)
    {
        if (!passwordText.isEmpty() || QString::compare(accountText, selectedEntry.accountText, Qt::CaseInsensitive) != 0)
        {
            passwordWideText = passwordText.toStdWString();
            passwordPointer = passwordWideText.c_str();
        }
    }

    const BOOL changeConfigOk = ::ChangeServiceConfigW(
        serviceHandle,
        targetServiceType,
        SERVICE_NO_CHANGE,
        SERVICE_NO_CHANGE,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        startNamePointer,
        passwordPointer,
        nullptr);
    if (changeConfigOk == FALSE)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("修改登录属性失败：\n%1").arg(errorText));
        return;
    }

    ::CloseServiceHandle(serviceHandle);
    ::CloseServiceHandle(scmHandle);
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
