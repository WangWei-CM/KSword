#include "ServiceDock.Internal.h"

#include <cwchar>
#include <sddl.h>

using namespace service_dock_detail;

namespace
{
    // parseMultiSzText 作用：解析 Win32 MULTI_SZ 文本到 QStringList。
    QStringList parseMultiSzText(const wchar_t* multiSzPointer)
    {
        QStringList resultList;
        if (multiSzPointer == nullptr)
        {
            return resultList;
        }

        const wchar_t* cursorPointer = multiSzPointer;
        while (*cursorPointer != L'\0')
        {
            const QString itemText = QString::fromWCharArray(cursorPointer).trimmed();
            if (!itemText.isEmpty())
            {
                resultList.push_back(itemText);
            }
            cursorPointer += wcslen(cursorPointer) + 1;
        }
        return resultList;
    }

    // queryServiceConfigBufferByHandle 作用：读取 QueryServiceConfigW 配置缓冲区。
    bool queryServiceConfigBufferByHandle(
        SC_HANDLE serviceHandle,
        std::vector<std::uint8_t>* configBufferOut,
        QUERY_SERVICE_CONFIGW** configOut)
    {
        if (serviceHandle == nullptr || configBufferOut == nullptr || configOut == nullptr)
        {
            return false;
        }

        DWORD requiredBytes = 0;
        ::QueryServiceConfigW(serviceHandle, nullptr, 0, &requiredBytes);
        if (requiredBytes == 0)
        {
            return false;
        }

        configBufferOut->assign(requiredBytes, 0);
        QUERY_SERVICE_CONFIGW* configPointer = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBufferOut->data());
        const BOOL queryOk = ::QueryServiceConfigW(
            serviceHandle,
            configPointer,
            requiredBytes,
            &requiredBytes);
        if (queryOk == FALSE)
        {
            return false;
        }

        *configOut = configPointer;
        return true;
    }

    // queryConfig2BufferByHandle 作用：读取 QueryServiceConfig2W 可变长度结构缓冲区。
    bool queryConfig2BufferByHandle(
        SC_HANDLE serviceHandle,
        const DWORD infoLevel,
        std::vector<std::uint8_t>* dataBufferOut)
    {
        if (serviceHandle == nullptr || dataBufferOut == nullptr)
        {
            return false;
        }

        DWORD requiredBytes = 0;
        ::QueryServiceConfig2W(serviceHandle, infoLevel, nullptr, 0, &requiredBytes);
        if (requiredBytes == 0)
        {
            return false;
        }

        dataBufferOut->assign(requiredBytes, 0);
        const BOOL queryOk = ::QueryServiceConfig2W(
            serviceHandle,
            infoLevel,
            dataBufferOut->data(),
            requiredBytes,
            &requiredBytes);
        return queryOk != FALSE;
    }

    // openServiceForRead 作用：打开服务句柄用于读取高级属性。
    SC_HANDLE openServiceForRead(const QString& serviceNameText)
    {
        SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scmHandle == nullptr)
        {
            return nullptr;
        }

        SC_HANDLE serviceHandle = ::OpenServiceW(
            scmHandle,
            reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | READ_CONTROL);
        ::CloseServiceHandle(scmHandle);
        return serviceHandle;
    }

    // scActionTypeToText 作用：把失败动作类型值转成可读文本。
    QString scActionTypeToText(const SC_ACTION_TYPE actionTypeValue)
    {
        switch (actionTypeValue)
        {
        case SC_ACTION_NONE:
            return QStringLiteral("无动作");
        case SC_ACTION_RESTART:
            return QStringLiteral("重启服务");
        case SC_ACTION_REBOOT:
            return QStringLiteral("重启系统");
        case SC_ACTION_RUN_COMMAND:
            return QStringLiteral("执行命令");
        default:
            return QStringLiteral("未知");
        }
    }

    // triggerTypeToText 作用：把触发器类型值转换为友好文本。
    QString triggerTypeToText(const DWORD triggerTypeValue)
    {
        switch (triggerTypeValue)
        {
        case SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL:
            return QStringLiteral("设备接口到达");
        case SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY:
            return QStringLiteral("IP 地址可用性变化");
        case SERVICE_TRIGGER_TYPE_DOMAIN_JOIN:
            return QStringLiteral("域加入/退出");
        case SERVICE_TRIGGER_TYPE_FIREWALL_PORT_EVENT:
            return QStringLiteral("防火墙端口事件");
        case SERVICE_TRIGGER_TYPE_GROUP_POLICY:
            return QStringLiteral("组策略变更");
        case SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT:
            return QStringLiteral("网络端点");
        case SERVICE_TRIGGER_TYPE_CUSTOM_SYSTEM_STATE_CHANGE:
            return QStringLiteral("自定义系统状态变化");
        case SERVICE_TRIGGER_TYPE_CUSTOM:
            return QStringLiteral("自定义触发器");
        default:
            return QStringLiteral("未知类型");
        }
    }

    // triggerActionToText 作用：把触发器动作值转换为友好文本。
    QString triggerActionToText(const DWORD triggerActionValue)
    {
        switch (triggerActionValue)
        {
        case SERVICE_TRIGGER_ACTION_SERVICE_START:
            return QStringLiteral("启动服务");
        case SERVICE_TRIGGER_ACTION_SERVICE_STOP:
            return QStringLiteral("停止服务");
        default:
            return QStringLiteral("未知动作");
        }
    }

    // guidToText 作用：把 GUID 转成标准字符串。
    QString guidToText(const GUID& guidValue)
    {
        wchar_t guidBuffer[64] = {};
        if (::StringFromGUID2(guidValue, guidBuffer, static_cast<int>(std::size(guidBuffer))) <= 0)
        {
            return QStringLiteral("<invalid-guid>");
        }
        return QString::fromWCharArray(guidBuffer);
    }

    // queryServicePermissionVisible 作用：判断当前令牌是否具备指定服务访问权限。
    bool queryServicePermissionVisible(const QString& serviceNameText, const DWORD desiredAccess)
    {
        SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scmHandle == nullptr)
        {
            return false;
        }

        SC_HANDLE serviceHandle = ::OpenServiceW(
            scmHandle,
            reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
            desiredAccess);
        if (serviceHandle == nullptr)
        {
            ::CloseServiceHandle(scmHandle);
            return false;
        }

        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        return true;
    }
}

QString ServiceDock::queryServiceDllPathByName(const QString& serviceNameText) const
{
    const QString normalizedServiceNameText = serviceNameText.trimmed();
    if (normalizedServiceNameText.isEmpty())
    {
        return QString();
    }

    const QString registryPathText = QStringLiteral(
        "SYSTEM\\CurrentControlSet\\Services\\%1\\Parameters").arg(normalizedServiceNameText);
    HKEY openedKey = nullptr;
    const LONG openResult = ::RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        reinterpret_cast<LPCWSTR>(registryPathText.utf16()),
        0,
        KEY_READ,
        &openedKey);
    if (openResult != ERROR_SUCCESS || openedKey == nullptr)
    {
        return QString();
    }

    DWORD valueType = 0;
    DWORD requiredBytes = 0;
    LONG queryResult = ::RegQueryValueExW(openedKey, L"ServiceDll", nullptr, &valueType, nullptr, &requiredBytes);
    if (queryResult != ERROR_SUCCESS || requiredBytes == 0 || (valueType != REG_EXPAND_SZ && valueType != REG_SZ))
    {
        ::RegCloseKey(openedKey);
        return QString();
    }

    std::vector<wchar_t> valueBuffer((requiredBytes / sizeof(wchar_t)) + 2, L'\0');
    queryResult = ::RegQueryValueExW(
        openedKey,
        L"ServiceDll",
        nullptr,
        &valueType,
        reinterpret_cast<LPBYTE>(valueBuffer.data()),
        &requiredBytes);
    ::RegCloseKey(openedKey);
    if (queryResult != ERROR_SUCCESS)
    {
        return QString();
    }

    QString rawPathText = QString::fromWCharArray(valueBuffer.data()).trimmed();
    if (rawPathText.isEmpty())
    {
        return QString();
    }

    wchar_t expandedPathBuffer[MAX_PATH * 4] = {};
    const DWORD expandedLength = ::ExpandEnvironmentStringsW(
        reinterpret_cast<LPCWSTR>(rawPathText.utf16()),
        expandedPathBuffer,
        static_cast<DWORD>(std::size(expandedPathBuffer)));
    if (expandedLength > 0 && expandedLength < std::size(expandedPathBuffer))
    {
        rawPathText = QString::fromWCharArray(expandedPathBuffer).trimmed();
    }
    return QDir::toNativeSeparators(rawPathText);
}

bool ServiceDock::isServiceFilePresent(const QString& filePathText) const
{
    const QFileInfo fileInfo(filePathText.trimmed());
    return fileInfo.exists() && fileInfo.isFile();
}

QString ServiceDock::buildProcessLinkDetailText(const ServiceEntry& entry) const
{
    QStringList detailLineList;
    detailLineList.push_back(QStringLiteral("服务名：%1").arg(entry.serviceNameText));
    detailLineList.push_back(QStringLiteral("PID：%1").arg(entry.processId == 0 ? QStringLiteral("-") : QString::number(entry.processId)));
    detailLineList.push_back(QStringLiteral("状态：%1").arg(entry.stateText));

    if (entry.processId != 0)
    {
        const std::string processPathText = ks::process::QueryProcessPathByPid(entry.processId);
        const QString hostProcessPathText = QString::fromStdString(processPathText);
        detailLineList.push_back(QStringLiteral("宿主进程路径：%1").arg(hostProcessPathText));
        detailLineList.push_back(QStringLiteral("宿主类型：%1").arg(
            hostProcessPathText.contains(QStringLiteral("svchost.exe"), Qt::CaseInsensitive)
            ? QStringLiteral("svchost 共享宿主")
            : QStringLiteral("独立宿主")));

        QStringList sameHostServiceList;
        for (const ServiceEntry& loopEntry : m_serviceList)
        {
            if (loopEntry.processId == entry.processId && loopEntry.currentState == SERVICE_RUNNING)
            {
                sameHostServiceList.push_back(loopEntry.serviceNameText);
            }
        }
        sameHostServiceList.removeDuplicates();
        detailLineList.push_back(QStringLiteral("同宿主服务数量：%1").arg(sameHostServiceList.size()));
        if (!sameHostServiceList.isEmpty())
        {
            detailLineList.push_back(QStringLiteral("同宿主服务：%1").arg(sameHostServiceList.join(QStringLiteral(", "))));
        }
    }
    else
    {
        detailLineList.push_back(QStringLiteral("运行关联：无"));
    }

    return detailLineList.join(QStringLiteral("\n"));
}

QString ServiceDock::buildRegistryFileDetailText(const ServiceEntry& entry) const
{
    const QString registryPathText = QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services\\%1").arg(entry.serviceNameText);
    QStringList detailLineList;
    detailLineList.push_back(QStringLiteral("注册表路径：%1").arg(registryPathText));
    detailLineList.push_back(QStringLiteral("BinaryPath：%1").arg(entry.commandLineText));
    detailLineList.push_back(QStringLiteral("BinaryPath存在：%1").arg(isServiceFilePresent(entry.imagePathText) ? QStringLiteral("是") : QStringLiteral("否")));
    detailLineList.push_back(QStringLiteral("ServiceDll：%1").arg(entry.serviceDllPathText.isEmpty() ? QStringLiteral("未配置") : entry.serviceDllPathText));
    if (!entry.serviceDllPathText.isEmpty())
    {
        detailLineList.push_back(QStringLiteral("ServiceDll存在：%1").arg(isServiceFilePresent(entry.serviceDllPathText) ? QStringLiteral("是") : QStringLiteral("否")));
    }
    return detailLineList.join(QStringLiteral("\n"));
}

QString ServiceDock::buildDependencyDetailText(const ServiceEntry& entry) const
{
    SC_HANDLE serviceHandle = openServiceForRead(entry.serviceNameText);
    if (serviceHandle == nullptr)
    {
        return QStringLiteral("读取依赖失败：无法打开服务句柄。");
    }

    QStringList forwardServiceList;
    QStringList forwardGroupList;
    std::vector<std::uint8_t> configBuffer;
    QUERY_SERVICE_CONFIGW* configPointer = nullptr;
    if (queryServiceConfigBufferByHandle(serviceHandle, &configBuffer, &configPointer))
    {
        const QStringList rawDependencyList = parseMultiSzText(configPointer->lpDependencies);
        for (const QString& dependencyText : rawDependencyList)
        {
            if (dependencyText.startsWith('+'))
            {
                forwardGroupList.push_back(dependencyText.mid(1));
            }
            else
            {
                forwardServiceList.push_back(dependencyText);
            }
        }
    }

    QStringList reverseServiceList;
    DWORD requiredBytes = 0;
    DWORD dependentCount = 0;
    ::EnumDependentServicesW(serviceHandle, SERVICE_STATE_ALL, nullptr, 0, &requiredBytes, &dependentCount);
    if (requiredBytes > 0)
    {
        std::vector<std::uint8_t> dependentBuffer(requiredBytes);
        const BOOL enumOk = ::EnumDependentServicesW(
            serviceHandle,
            SERVICE_STATE_ALL,
            reinterpret_cast<LPENUM_SERVICE_STATUSW>(dependentBuffer.data()),
            requiredBytes,
            &requiredBytes,
            &dependentCount);
        if (enumOk != FALSE)
        {
            const ENUM_SERVICE_STATUSW* dependentArray =
                reinterpret_cast<const ENUM_SERVICE_STATUSW*>(dependentBuffer.data());
            for (DWORD dependentIndex = 0; dependentIndex < dependentCount; ++dependentIndex)
            {
                reverseServiceList.push_back(QString::fromWCharArray(dependentArray[dependentIndex].lpServiceName));
            }
        }
    }
    ::CloseServiceHandle(serviceHandle);

    QStringList detailLineList;
    detailLineList.push_back(QStringLiteral("依赖树（文本）"));
    detailLineList.push_back(QStringLiteral("├─ 当前服务：%1").arg(entry.serviceNameText));
    if (forwardServiceList.isEmpty() && forwardGroupList.isEmpty())
    {
        detailLineList.push_back(QStringLiteral("├─ 正向依赖：无"));
    }
    else
    {
        for (const QString& serviceDependencyText : forwardServiceList)
        {
            const int targetIndex = findServiceIndexByName(serviceDependencyText);
            const QString runningMarkText =
                (targetIndex >= 0 && m_serviceList[static_cast<std::size_t>(targetIndex)].currentState == SERVICE_RUNNING)
                ? QStringLiteral("运行中")
                : QStringLiteral("未运行/缺失");
            detailLineList.push_back(QStringLiteral("├─ 依赖服务：%1 [%2]").arg(serviceDependencyText).arg(runningMarkText));
        }
        for (const QString& groupDependencyText : forwardGroupList)
        {
            detailLineList.push_back(QStringLiteral("├─ 依赖组：%1").arg(groupDependencyText));
        }
    }
    if (reverseServiceList.isEmpty())
    {
        detailLineList.push_back(QStringLiteral("└─ 反向依赖：无"));
    }
    else
    {
        for (const QString& reverseServiceText : reverseServiceList)
        {
            detailLineList.push_back(QStringLiteral("└─ 被依赖：%1").arg(reverseServiceText));
        }
    }
    detailLineList.push_back(QStringLiteral("正向依赖数量：%1").arg(forwardServiceList.size() + forwardGroupList.size()));
    detailLineList.push_back(QStringLiteral("反向依赖数量：%1").arg(reverseServiceList.size()));
    return detailLineList.join(QStringLiteral("\n"));
}

QString ServiceDock::buildFailureActionDetailText(const ServiceEntry& entry) const
{
    SC_HANDLE serviceHandle = openServiceForRead(entry.serviceNameText);
    if (serviceHandle == nullptr)
    {
        return QStringLiteral("读取失败动作失败：无法打开服务句柄。");
    }

    QStringList detailLineList;
    std::vector<std::uint8_t> failureBuffer;
    if (queryConfig2BufferByHandle(serviceHandle, SERVICE_CONFIG_FAILURE_ACTIONS, &failureBuffer))
    {
        const SERVICE_FAILURE_ACTIONSW* failurePointer =
            reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(failureBuffer.data());
        detailLineList.push_back(QStringLiteral("ResetPeriod：%1 秒").arg(failurePointer->dwResetPeriod));
        detailLineList.push_back(QStringLiteral("RebootMessage：%1").arg(
            (failurePointer->lpRebootMsg != nullptr && wcslen(failurePointer->lpRebootMsg) > 0)
            ? QString::fromWCharArray(failurePointer->lpRebootMsg)
            : QStringLiteral("未配置")));
        detailLineList.push_back(QStringLiteral("Command：%1").arg(
            (failurePointer->lpCommand != nullptr && wcslen(failurePointer->lpCommand) > 0)
            ? QString::fromWCharArray(failurePointer->lpCommand)
            : QStringLiteral("未配置")));

        for (DWORD actionIndex = 0; actionIndex < failurePointer->cActions; ++actionIndex)
        {
            const SC_ACTION& actionItem = failurePointer->lpsaActions[actionIndex];
            const QString actionNameText =
                (actionIndex == 0) ? QStringLiteral("第1次失败")
                : ((actionIndex == 1) ? QStringLiteral("第2次失败") : QStringLiteral("后续失败"));
            detailLineList.push_back(
                QStringLiteral("%1：%2，延迟 %3 ms")
                .arg(actionNameText)
                .arg(scActionTypeToText(actionItem.Type))
                .arg(actionItem.Delay));
        }
    }
    else
    {
        detailLineList.push_back(QStringLiteral("FailureActions：未配置或读取失败"));
    }

    std::vector<std::uint8_t> failureFlagBuffer;
    if (queryConfig2BufferByHandle(serviceHandle, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlagBuffer))
    {
        const SERVICE_FAILURE_ACTIONS_FLAG* flagPointer =
            reinterpret_cast<const SERVICE_FAILURE_ACTIONS_FLAG*>(failureFlagBuffer.data());
        detailLineList.push_back(QStringLiteral("FailureActionsFlag：%1").arg(flagPointer->fFailureActionsOnNonCrashFailures ? QStringLiteral("启用") : QStringLiteral("禁用")));
    }

    ::CloseServiceHandle(serviceHandle);
    return detailLineList.join(QStringLiteral("\n"));
}

QString ServiceDock::buildTriggerDetailText(const ServiceEntry& entry) const
{
    SC_HANDLE serviceHandle = openServiceForRead(entry.serviceNameText);
    if (serviceHandle == nullptr)
    {
        return QStringLiteral("读取触发器失败：无法打开服务句柄。");
    }

    QStringList detailLineList;
    std::vector<std::uint8_t> triggerBuffer;
    if (!queryConfig2BufferByHandle(serviceHandle, SERVICE_CONFIG_TRIGGER_INFO, &triggerBuffer))
    {
        ::CloseServiceHandle(serviceHandle);
        return QStringLiteral("触发器：未配置或当前系统不支持读取");
    }

    const SERVICE_TRIGGER_INFO* triggerInfoPointer =
        reinterpret_cast<const SERVICE_TRIGGER_INFO*>(triggerBuffer.data());
    detailLineList.push_back(QStringLiteral("触发器数量：%1").arg(triggerInfoPointer->cTriggers));
    for (DWORD triggerIndex = 0; triggerIndex < triggerInfoPointer->cTriggers; ++triggerIndex)
    {
        const SERVICE_TRIGGER& triggerItem = triggerInfoPointer->pTriggers[triggerIndex];
        detailLineList.push_back(QStringLiteral("---- Trigger #%1 ----").arg(triggerIndex + 1));
        detailLineList.push_back(QStringLiteral("类型：%1 (%2)")
            .arg(triggerTypeToText(triggerItem.dwTriggerType))
            .arg(triggerItem.dwTriggerType));
        detailLineList.push_back(QStringLiteral("动作：%1 (%2)")
            .arg(triggerActionToText(triggerItem.dwAction))
            .arg(triggerItem.dwAction));
        detailLineList.push_back(QStringLiteral("子类型GUID：%1")
            .arg((triggerItem.pTriggerSubtype != nullptr)
                ? guidToText(*triggerItem.pTriggerSubtype)
                : QStringLiteral("未提供")));
        detailLineList.push_back(QStringLiteral("数据项数量：%1").arg(triggerItem.cDataItems));

        for (DWORD dataIndex = 0; dataIndex < triggerItem.cDataItems; ++dataIndex)
        {
            const SERVICE_TRIGGER_SPECIFIC_DATA_ITEM& dataItem = triggerItem.pDataItems[dataIndex];
            QString dataPreviewText;
            if (dataItem.cbData == 0 || dataItem.pData == nullptr)
            {
                dataPreviewText = QStringLiteral("空数据");
            }
            else if (dataItem.dwDataType == SERVICE_TRIGGER_DATA_TYPE_STRING
                || dataItem.dwDataType == SERVICE_TRIGGER_DATA_TYPE_LEVEL
                || dataItem.dwDataType == SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ANY
                || dataItem.dwDataType == SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ALL)
            {
                dataPreviewText = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(dataItem.pData));
            }
            else
            {
                QByteArray rawBytes(reinterpret_cast<const char*>(dataItem.pData), static_cast<int>(dataItem.cbData));
                dataPreviewText = QString::fromLatin1(rawBytes.toHex(' '));
            }

            detailLineList.push_back(QStringLiteral("  数据项[%1] type=%2 size=%3 value=%4")
                .arg(dataIndex)
                .arg(dataItem.dwDataType)
                .arg(dataItem.cbData)
                .arg(dataPreviewText));
        }
    }

    ::CloseServiceHandle(serviceHandle);
    return detailLineList.join(QStringLiteral("\n"));
}

QString ServiceDock::buildSecurityDetailText(const ServiceEntry& entry) const
{
    SC_HANDLE serviceHandle = openServiceForRead(entry.serviceNameText);
    if (serviceHandle == nullptr)
    {
        return QStringLiteral("读取安全信息失败：无法打开服务句柄。");
    }

    QStringList detailLineList;

    std::vector<std::uint8_t> sidTypeBuffer;
    if (queryConfig2BufferByHandle(serviceHandle, SERVICE_CONFIG_SERVICE_SID_INFO, &sidTypeBuffer))
    {
        const SERVICE_SID_INFO* sidInfoPointer = reinterpret_cast<const SERVICE_SID_INFO*>(sidTypeBuffer.data());
        detailLineList.push_back(QStringLiteral("ServiceSidType：%1").arg(sidInfoPointer->dwServiceSidType));
    }

    std::vector<std::uint8_t> privilegeBuffer;
    if (queryConfig2BufferByHandle(serviceHandle, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &privilegeBuffer))
    {
        const SERVICE_REQUIRED_PRIVILEGES_INFOW* privilegeInfoPointer =
            reinterpret_cast<const SERVICE_REQUIRED_PRIVILEGES_INFOW*>(privilegeBuffer.data());
        const QStringList privilegeList = parseMultiSzText(privilegeInfoPointer->pmszRequiredPrivileges);
        detailLineList.push_back(QStringLiteral("RequiredPrivileges：%1").arg(
            privilegeList.isEmpty() ? QStringLiteral("未声明") : privilegeList.join(QStringLiteral(", "))));
    }

    std::vector<std::uint8_t> launchProtectedBuffer;
    if (queryConfig2BufferByHandle(serviceHandle, SERVICE_CONFIG_LAUNCH_PROTECTED, &launchProtectedBuffer))
    {
        const SERVICE_LAUNCH_PROTECTED_INFO* launchProtectedPointer =
            reinterpret_cast<const SERVICE_LAUNCH_PROTECTED_INFO*>(launchProtectedBuffer.data());
        detailLineList.push_back(QStringLiteral("LaunchProtected：%1").arg(launchProtectedPointer->dwLaunchProtected));
    }

    DWORD securityDescriptorBytes = 0;
    ::QueryServiceObjectSecurity(serviceHandle, DACL_SECURITY_INFORMATION, nullptr, 0, &securityDescriptorBytes);
    if (securityDescriptorBytes > 0)
    {
        std::vector<std::uint8_t> securityDescriptorBuffer(securityDescriptorBytes);
        if (::QueryServiceObjectSecurity(
            serviceHandle,
            DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDescriptorBuffer.data()),
            securityDescriptorBytes,
            &securityDescriptorBytes) != FALSE)
        {
            LPWSTR sddlPointer = nullptr;
            if (::ConvertSecurityDescriptorToStringSecurityDescriptorW(
                reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDescriptorBuffer.data()),
                SDDL_REVISION_1,
                DACL_SECURITY_INFORMATION,
                &sddlPointer,
                nullptr) != FALSE
                && sddlPointer != nullptr)
            {
                detailLineList.push_back(QStringLiteral("SDDL：%1").arg(QString::fromWCharArray(sddlPointer)));
                ::LocalFree(sddlPointer);
            }
        }
    }

    ::CloseServiceHandle(serviceHandle);

    detailLineList.push_back(QStringLiteral("权限可见化："));
    detailLineList.push_back(QStringLiteral("  Start：%1").arg(queryServicePermissionVisible(entry.serviceNameText, SERVICE_START) ? QStringLiteral("可用") : QStringLiteral("不可用")));
    detailLineList.push_back(QStringLiteral("  Stop：%1").arg(queryServicePermissionVisible(entry.serviceNameText, SERVICE_STOP) ? QStringLiteral("可用") : QStringLiteral("不可用")));
    detailLineList.push_back(QStringLiteral("  ChangeConfig：%1").arg(queryServicePermissionVisible(entry.serviceNameText, SERVICE_CHANGE_CONFIG) ? QStringLiteral("可用") : QStringLiteral("不可用")));
    detailLineList.push_back(QStringLiteral("  Delete：%1").arg(queryServicePermissionVisible(entry.serviceNameText, DELETE) ? QStringLiteral("可用") : QStringLiteral("不可用")));
    return detailLineList.join(QStringLiteral("\n"));
}

QString ServiceDock::buildRiskDetailText(const ServiceEntry& entry) const
{
    QStringList detailLineList;
    detailLineList.push_back(QStringLiteral("风险摘要：%1").arg(entry.riskSummaryText));
    if (entry.riskTagList.isEmpty())
    {
        detailLineList.push_back(QStringLiteral("未命中风险标签。"));
    }
    else
    {
        for (const QString& riskTagText : entry.riskTagList)
        {
            detailLineList.push_back(QStringLiteral(" - %1").arg(riskTagText));
        }
    }
    return detailLineList.join(QStringLiteral("\n"));
}

QString ServiceDock::buildExportDetailText(const ServiceEntry& entry) const
{
    QStringList detailLineList;
    detailLineList.push_back(QStringLiteral("当前服务：%1").arg(entry.serviceNameText));
    detailLineList.push_back(QStringLiteral("当前可见服务数：%1").arg(m_serviceTable == nullptr ? 0 : m_serviceTable->rowCount()));
    detailLineList.push_back(QStringLiteral("导出列表：支持 TSV（当前筛选结果）"));
    detailLineList.push_back(QStringLiteral("导出单服务：支持 JSON（完整配置快照）"));
    detailLineList.push_back(QStringLiteral("刷新策略：支持“刷新当前服务”与“刷新全部服务”分层更新"));
    return detailLineList.join(QStringLiteral("\n"));
}
