#include "ServiceDock.Internal.h"

#include <Softpub.h>
#include <wintrust.h>

using namespace service_dock_detail;

#pragma comment(lib, "Wintrust.lib")

namespace
{
    // isFileTrustedByWindows 作用：判断文件签名是否被系统信任。
    bool isFileTrustedByWindows(const QString& filePathText)
    {
        if (filePathText.trimmed().isEmpty())
        {
            return false;
        }

        const std::wstring utf16Path = filePathText.toStdWString();
        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = utf16Path.c_str();

        WINTRUST_DATA trustData{};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        trustData.pFile = &fileInfo;

        GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG verifyResult = ::WinVerifyTrust(nullptr, &policyGuid, &trustData);

        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        ::WinVerifyTrust(nullptr, &policyGuid, &trustData);
        return verifyResult == ERROR_SUCCESS;
    }

    // queryFailureCommandTextByServiceName 作用：读取 FailureActions 配置中的命令文本。
    QString queryFailureCommandTextByServiceName(const QString& serviceNameText)
    {
        SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scmHandle == nullptr)
        {
            return QString();
        }

        SC_HANDLE serviceHandle = ::OpenServiceW(
            scmHandle,
            reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
            SERVICE_QUERY_CONFIG);
        if (serviceHandle == nullptr)
        {
            ::CloseServiceHandle(scmHandle);
            return QString();
        }

        DWORD requiredBytes = 0;
        ::QueryServiceConfig2W(serviceHandle, SERVICE_CONFIG_FAILURE_ACTIONS, nullptr, 0, &requiredBytes);
        if (requiredBytes == 0)
        {
            ::CloseServiceHandle(serviceHandle);
            ::CloseServiceHandle(scmHandle);
            return QString();
        }

        std::vector<std::uint8_t> failureBuffer(requiredBytes);
        const BOOL queryOk = ::QueryServiceConfig2W(
            serviceHandle,
            SERVICE_CONFIG_FAILURE_ACTIONS,
            failureBuffer.data(),
            requiredBytes,
            &requiredBytes);
        if (queryOk == FALSE)
        {
            ::CloseServiceHandle(serviceHandle);
            ::CloseServiceHandle(scmHandle);
            return QString();
        }

        const SERVICE_FAILURE_ACTIONSW* failurePointer =
            reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(failureBuffer.data());
        const QString commandText = (failurePointer->lpCommand != nullptr)
            ? QString::fromWCharArray(failurePointer->lpCommand).trimmed()
            : QString();

        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        return commandText;
    }

    // queryServiceDllPathFromRegistry 作用：读取服务 Parameters\ServiceDll 路径。
    QString queryServiceDllPathFromRegistry(const QString& serviceNameText)
    {
        const QString registryPathText = QStringLiteral(
            "SYSTEM\\CurrentControlSet\\Services\\%1\\Parameters").arg(serviceNameText.trimmed());
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

    // evaluateRiskTagList 作用：生成服务风险标签列表，供主表风险列与过滤复用。
    QStringList evaluateRiskTagList(const ServiceDock::ServiceEntry& entry)
    {
        QStringList riskTagList;

        const QFileInfo imageFileInfo(entry.imagePathText);
        if (entry.imagePathText.trimmed().isEmpty() || !imageFileInfo.exists())
        {
            riskTagList.push_back(QStringLiteral("文件不存在"));
        }
        else if (!isFileTrustedByWindows(entry.imagePathText))
        {
            riskTagList.push_back(QStringLiteral("无签名"));
        }

        if ((entry.serviceTypeValue & SERVICE_WIN32_SHARE_PROCESS) != 0)
        {
            if (entry.serviceDllPathText.trimmed().isEmpty())
            {
                riskTagList.push_back(QStringLiteral("ServiceDll缺失"));
            }
            else if (!QFileInfo::exists(entry.serviceDllPathText))
            {
                riskTagList.push_back(QStringLiteral("ServiceDll不存在"));
            }
        }

        const QString accountLowerText = entry.accountText.trimmed().toLower();
        if (!accountLowerText.isEmpty()
            && accountLowerText != QStringLiteral("localsystem")
            && accountLowerText != QStringLiteral("nt authority\\localsystem")
            && accountLowerText != QStringLiteral("nt authority\\localservice")
            && accountLowerText != QStringLiteral("localservice")
            && accountLowerText != QStringLiteral("nt authority\\networkservice")
            && accountLowerText != QStringLiteral("networkservice")
            && accountLowerText != QStringLiteral("n/a"))
        {
            riskTagList.push_back(QStringLiteral("异常账户"));
        }

        if (entry.startTypeValue == SERVICE_AUTO_START)
        {
            const QString imagePathLowerText = QDir::toNativeSeparators(entry.imagePathText).toLower();
            const bool underWindowsDirectory =
                imagePathLowerText.startsWith(QStringLiteral("c:\\windows\\"))
                || imagePathLowerText.startsWith(QStringLiteral("\\windows\\"));
            if (!underWindowsDirectory)
            {
                riskTagList.push_back(QStringLiteral("非微软自动启动"));
            }
        }

        if (entry.descriptionText.trimmed().isEmpty())
        {
            riskTagList.push_back(QStringLiteral("配置缺失"));
        }

        const QString failureCommandText = queryFailureCommandTextByServiceName(entry.serviceNameText).toLower();
        if (failureCommandText.contains(QStringLiteral("powershell"))
            || failureCommandText.contains(QStringLiteral("cmd.exe"))
            || failureCommandText.contains(QStringLiteral("wscript"))
            || failureCommandText.contains(QStringLiteral("cscript"))
            || failureCommandText.contains(QStringLiteral("rundll32")))
        {
            riskTagList.push_back(QStringLiteral("可疑失败命令"));
        }

        riskTagList.removeDuplicates();
        return riskTagList;
    }

    // queryServiceStatusProcess 作用：读取服务实时状态（含 PID/控制位）。
    bool queryServiceStatusProcess(
        SC_HANDLE serviceHandle,
        SERVICE_STATUS_PROCESS* statusOut,
        QString* errorTextOut)
    {
        if (serviceHandle == nullptr || statusOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("queryServiceStatusProcess 参数无效");
            }
            return false;
        }

        DWORD requiredBytes = 0;
        SERVICE_STATUS_PROCESS statusValue{};
        const BOOL queryOk = ::QueryServiceStatusEx(
            serviceHandle,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&statusValue),
            sizeof(statusValue),
            &requiredBytes);
        if (queryOk == FALSE)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(::GetLastError());
            }
            return false;
        }

        *statusOut = statusValue;
        return true;
    }

    // queryServiceConfigBuffer 作用：查询 QueryServiceConfigW 并返回配置缓冲区。
    bool queryServiceConfigBuffer(
        SC_HANDLE serviceHandle,
        std::vector<std::uint8_t>* configBufferOut,
        QUERY_SERVICE_CONFIGW** configOut,
        QString* errorTextOut)
    {
        if (serviceHandle == nullptr || configBufferOut == nullptr || configOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("queryServiceConfigBuffer 参数无效");
            }
            return false;
        }

        DWORD requiredBytes = 0;
        ::QueryServiceConfigW(serviceHandle, nullptr, 0, &requiredBytes);
        if (requiredBytes == 0)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(::GetLastError());
            }
            return false;
        }

        configBufferOut->assign(requiredBytes, 0);
        QUERY_SERVICE_CONFIGW* configPointer =
            reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBufferOut->data());
        const BOOL queryOk = ::QueryServiceConfigW(
            serviceHandle,
            configPointer,
            requiredBytes,
            &requiredBytes);
        if (queryOk == FALSE)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(::GetLastError());
            }
            return false;
        }

        *configOut = configPointer;
        return true;
    }

    // queryServiceDescriptionText 作用：读取服务描述字符串。
    QString queryServiceDescriptionText(SC_HANDLE serviceHandle)
    {
        if (serviceHandle == nullptr)
        {
            return QString();
        }

        DWORD requiredBytes = 0;
        ::QueryServiceConfig2W(serviceHandle, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &requiredBytes);
        if (requiredBytes == 0)
        {
            return QString();
        }

        std::vector<std::uint8_t> descriptionBuffer(requiredBytes);
        const BOOL queryOk = ::QueryServiceConfig2W(
            serviceHandle,
            SERVICE_CONFIG_DESCRIPTION,
            descriptionBuffer.data(),
            requiredBytes,
            &requiredBytes);
        if (queryOk == FALSE)
        {
            return QString();
        }

        SERVICE_DESCRIPTIONW* descriptionPointer =
            reinterpret_cast<SERVICE_DESCRIPTIONW*>(descriptionBuffer.data());
        if (descriptionPointer->lpDescription == nullptr)
        {
            return QString();
        }
        return QString::fromWCharArray(descriptionPointer->lpDescription).trimmed();
    }

    // queryDelayedAutoStartFlag 作用：读取延迟自动启动开关值。
    bool queryDelayedAutoStartFlag(SC_HANDLE serviceHandle)
    {
        if (serviceHandle == nullptr)
        {
            return false;
        }

        SERVICE_DELAYED_AUTO_START_INFO delayedInfo{};
        DWORD requiredBytes = 0;
        const BOOL queryOk = ::QueryServiceConfig2W(
            serviceHandle,
            SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
            reinterpret_cast<LPBYTE>(&delayedInfo),
            sizeof(delayedInfo),
            &requiredBytes);
        if (queryOk == FALSE)
        {
            return false;
        }
        return delayedInfo.fDelayedAutostart != FALSE;
    }

    // buildServiceEntryByName 作用：按服务名构建完整 ServiceEntry 结构。
    bool buildServiceEntryByName(
        SC_HANDLE scmHandle,
        const QString& serviceNameText,
        const QString& displayNameHintText,
        const SERVICE_STATUS_PROCESS* statusHint,
        ServiceDock::ServiceEntry* entryOut,
        QString* errorTextOut)
    {
        if (scmHandle == nullptr || entryOut == nullptr || serviceNameText.trimmed().isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("buildServiceEntryByName 参数无效");
            }
            return false;
        }

        SC_HANDLE serviceHandle = ::OpenServiceW(
            scmHandle,
            reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
        if (serviceHandle == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(::GetLastError());
            }
            return false;
        }

        SERVICE_STATUS_PROCESS statusValue{};
        if (statusHint != nullptr)
        {
            statusValue = *statusHint;
        }
        else
        {
            QString statusErrorText;
            if (!queryServiceStatusProcess(serviceHandle, &statusValue, &statusErrorText))
            {
                ::CloseServiceHandle(serviceHandle);
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = statusErrorText;
                }
                return false;
            }
        }

        std::vector<std::uint8_t> configBuffer;
        QUERY_SERVICE_CONFIGW* configPointer = nullptr;
        QString configErrorText;
        if (!queryServiceConfigBuffer(serviceHandle, &configBuffer, &configPointer, &configErrorText))
        {
            ::CloseServiceHandle(serviceHandle);
            if (errorTextOut != nullptr)
            {
                *errorTextOut = configErrorText;
            }
            return false;
        }

        entryOut->serviceNameText = serviceNameText;
        entryOut->displayNameText = displayNameHintText.trimmed().isEmpty()
            ? serviceNameText
            : displayNameHintText;
        entryOut->descriptionText = queryServiceDescriptionText(serviceHandle);
        entryOut->commandLineText = (configPointer->lpBinaryPathName != nullptr)
            ? QString::fromWCharArray(configPointer->lpBinaryPathName).trimmed()
            : QString();
        entryOut->imagePathText = normalizeServiceImagePath(entryOut->commandLineText);
        entryOut->accountText = (configPointer->lpServiceStartName != nullptr)
            ? QString::fromWCharArray(configPointer->lpServiceStartName).trimmed()
            : QStringLiteral("N/A");
        entryOut->currentState = statusValue.dwCurrentState;
        entryOut->stateText = serviceStateToText(statusValue.dwCurrentState);
        entryOut->controlsAccepted = statusValue.dwControlsAccepted;
        entryOut->processId = statusValue.dwProcessId;
        entryOut->startTypeValue = configPointer->dwStartType;
        entryOut->serviceTypeValue = configPointer->dwServiceType;
        entryOut->errorControlValue = configPointer->dwErrorControl;
        entryOut->serviceDllPathText = queryServiceDllPathFromRegistry(serviceNameText);
        entryOut->delayedAutoStart = (configPointer->dwStartType == SERVICE_AUTO_START)
            ? queryDelayedAutoStartFlag(serviceHandle)
            : false;
        entryOut->startTypeText = startTypeToText(entryOut->startTypeValue, entryOut->delayedAutoStart);
        entryOut->serviceTypeText = serviceTypeToText(entryOut->serviceTypeValue);
        entryOut->errorControlText = errorControlToText(entryOut->errorControlValue);
        entryOut->riskTagList = evaluateRiskTagList(*entryOut);
        entryOut->riskSummaryText = entryOut->riskTagList.isEmpty()
            ? QStringLiteral("低")
            : entryOut->riskTagList.join(QStringLiteral(" | "));
        entryOut->hasRisk = !entryOut->riskTagList.isEmpty();

        ::CloseServiceHandle(serviceHandle);
        return true;
    }
}

void ServiceDock::enumerateServiceList(
    std::vector<ServiceEntry>* serviceListOut,
    QString* errorTextOut) const
{
    if (serviceListOut == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("输出容器为空");
        }
        return;
    }

    serviceListOut->clear();

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("OpenSCManagerW 失败：%1").arg(winErrorText(::GetLastError()));
        }
        return;
    }

    DWORD bytesNeeded = 0;
    DWORD servicesReturned = 0;
    DWORD resumeHandle = 0;
    ::EnumServicesStatusExW(
        scmHandle,
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS,
        SERVICE_STATE_ALL,
        nullptr,
        0,
        &bytesNeeded,
        &servicesReturned,
        &resumeHandle,
        nullptr);

    DWORD lastErrorValue = ::GetLastError();
    if (bytesNeeded == 0 && lastErrorValue != ERROR_SUCCESS)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("EnumServicesStatusExW 初始化失败：%1").arg(winErrorText(lastErrorValue));
        }
        ::CloseServiceHandle(scmHandle);
        return;
    }

    std::vector<std::uint8_t> enumBuffer(bytesNeeded > 0 ? bytesNeeded : 64 * 1024);
    resumeHandle = 0;
    do
    {
        const BOOL enumOk = ::EnumServicesStatusExW(
            scmHandle,
            SC_ENUM_PROCESS_INFO,
            SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS,
            SERVICE_STATE_ALL,
            enumBuffer.data(),
            static_cast<DWORD>(enumBuffer.size()),
            &bytesNeeded,
            &servicesReturned,
            &resumeHandle,
            nullptr);
        if (enumOk == FALSE)
        {
            const DWORD enumError = ::GetLastError();
            if (enumError == ERROR_MORE_DATA && bytesNeeded > enumBuffer.size())
            {
                enumBuffer.resize(bytesNeeded);
                continue;
            }

            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("EnumServicesStatusExW 失败：%1").arg(winErrorText(enumError));
            }
            ::CloseServiceHandle(scmHandle);
            return;
        }

        const ENUM_SERVICE_STATUS_PROCESSW* serviceArray =
            reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(enumBuffer.data());
        for (DWORD serviceIndex = 0; serviceIndex < servicesReturned; ++serviceIndex)
        {
            const ENUM_SERVICE_STATUS_PROCESSW& serviceItem = serviceArray[serviceIndex];
            const QString serviceNameText = QString::fromWCharArray(serviceItem.lpServiceName).trimmed();
            const QString displayNameText = QString::fromWCharArray(serviceItem.lpDisplayName).trimmed();

            ServiceEntry serviceEntry;
            QString queryErrorText;
            const bool queryOk = buildServiceEntryByName(
                scmHandle,
                serviceNameText,
                displayNameText,
                &serviceItem.ServiceStatusProcess,
                &serviceEntry,
                &queryErrorText);
            if (!queryOk)
            {
                // 兜底策略：
                // - 即使读取详细配置失败，也保留主列表可见性；
                // - 把错误塞到描述字段便于用户定位权限问题。
                serviceEntry.serviceNameText = serviceNameText;
                serviceEntry.displayNameText = displayNameText.isEmpty() ? serviceNameText : displayNameText;
                serviceEntry.descriptionText = QStringLiteral("读取服务配置失败：%1").arg(queryErrorText);
                serviceEntry.currentState = serviceItem.ServiceStatusProcess.dwCurrentState;
                serviceEntry.stateText = serviceStateToText(serviceItem.ServiceStatusProcess.dwCurrentState);
                serviceEntry.controlsAccepted = serviceItem.ServiceStatusProcess.dwControlsAccepted;
                serviceEntry.processId = serviceItem.ServiceStatusProcess.dwProcessId;
                serviceEntry.startTypeValue = 0;
                serviceEntry.serviceTypeValue = serviceItem.ServiceStatusProcess.dwServiceType;
                serviceEntry.errorControlValue = 0;
                serviceEntry.delayedAutoStart = false;
                serviceEntry.startTypeText = QStringLiteral("未知");
                serviceEntry.serviceTypeText = serviceTypeToText(serviceEntry.serviceTypeValue);
                serviceEntry.errorControlText = QStringLiteral("未知");
                serviceEntry.accountText = QStringLiteral("N/A");
                serviceEntry.serviceDllPathText = queryServiceDllPathFromRegistry(serviceNameText);
                serviceEntry.riskTagList = evaluateRiskTagList(serviceEntry);
                serviceEntry.riskSummaryText = serviceEntry.riskTagList.isEmpty()
                    ? QStringLiteral("低")
                    : serviceEntry.riskTagList.join(QStringLiteral(" | "));
                serviceEntry.hasRisk = !serviceEntry.riskTagList.isEmpty();
            }

            serviceListOut->push_back(std::move(serviceEntry));
        }
    } while (resumeHandle != 0);

    ::CloseServiceHandle(scmHandle);
}

bool ServiceDock::querySingleServiceByName(
    const QString& serviceNameText,
    ServiceEntry* entryOut,
    QString* errorTextOut) const
{
    if (entryOut == nullptr || serviceNameText.trimmed().isEmpty())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("querySingleServiceByName 参数无效");
        }
        return false;
    }

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("OpenSCManagerW 失败：%1").arg(winErrorText(::GetLastError()));
        }
        return false;
    }

    const bool queryOk = buildServiceEntryByName(
        scmHandle,
        serviceNameText.trimmed(),
        QString(),
        nullptr,
        entryOut,
        errorTextOut);
    ::CloseServiceHandle(scmHandle);
    return queryOk;
}
