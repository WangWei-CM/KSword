#include "ServiceDock.Internal.h"

#include <Softpub.h>
#include <wintrust.h>

using namespace service_dock_detail;

#pragma comment(lib, "Wintrust.lib")

namespace
{
    // isFileTrustedByWindows checks a file signature with WinVerifyTrust.
    // Input: filePathText is a UI QString path.
    // Processing: converts only at the WinTrust boundary and keeps UI policy local.
    // Return: true when Windows trusts the file, otherwise false.
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

    // queryFailureCommandTextByServiceName reads failure-action command via ks::service.
    // Input: serviceNameText is the SCM short name from the UI cache.
    // Processing: the reusable layer owns QueryServiceConfig2W and string ownership.
    // Return: command text, or empty when missing/unreadable.
    QString queryFailureCommandTextByServiceName(const QString& serviceNameText)
    {
        ks::service::FailureSettings settings;
        if (!ks::service::QueryServiceFailureSettings(serviceNameText.toStdWString(), &settings))
        {
            return QString();
        }
        return QString::fromStdWString(settings.command).trimmed();
    }

    // queryServiceDllPathFromRegistry reads Parameters\ServiceDll.
    // Input: serviceNameText is the SCM short name.
    // Processing: this remains registry access, not SCM access, so it stays in UI helper scope.
    // Return: expanded native path or an empty string when absent.
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
        const DWORD expandedPathBufferCount =
            static_cast<DWORD>(sizeof(expandedPathBuffer) / sizeof(expandedPathBuffer[0]));
        const DWORD expandedLength = ::ExpandEnvironmentStringsW(
            reinterpret_cast<LPCWSTR>(rawPathText.utf16()),
            expandedPathBuffer,
            expandedPathBufferCount);
        if (expandedLength > 0 && expandedLength < expandedPathBufferCount)
        {
            rawPathText = QString::fromWCharArray(expandedPathBuffer).trimmed();
        }

        return QDir::toNativeSeparators(rawPathText);
    }

    // evaluateRiskTagList generates UI risk tags from an already-built ServiceEntry.
    // Input: entry contains normalized path/account/config fields.
    // Processing: combines file signature, ServiceDll, account, autostart, description and failure command checks.
    // Return: de-duplicated risk labels for the table and detail panes.
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

    // buildServiceEntryFromRecord converts a Qt-free ks::service record into the UI cache row.
    // Input: serviceRecord is produced by ks::service enumeration/query.
    // Processing: this function only formats text and derives UI-only risk fields.
    // Return: true when a usable row is produced; strict mode fails if config is missing.
    bool buildServiceEntryFromRecord(
        const ks::service::ServiceRecord& serviceRecord,
        ServiceDock::ServiceEntry* entryOut,
        QString* errorTextOut,
        const bool strictConfig)
    {
        if (entryOut == nullptr || serviceRecord.serviceName.empty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("服务记录参数无效");
            }
            return false;
        }

        ServiceDock::ServiceEntry entry;
        entry.serviceNameText = QString::fromStdWString(serviceRecord.serviceName).trimmed();
        entry.displayNameText = QString::fromStdWString(serviceRecord.displayName).trimmed();
        if (entry.displayNameText.isEmpty())
        {
            entry.displayNameText = entry.serviceNameText;
        }

        entry.currentState = serviceRecord.status.currentState;
        entry.stateText = serviceStateToText(entry.currentState);
        entry.controlsAccepted = serviceRecord.status.controlsAccepted;
        entry.processId = serviceRecord.status.processId;
        entry.serviceTypeValue = serviceRecord.status.serviceType;
        entry.serviceTypeText = serviceTypeToText(entry.serviceTypeValue);
        entry.accountText = QStringLiteral("N/A");
        entry.errorControlText = QStringLiteral("未知");
        entry.startTypeText = QStringLiteral("未知");

        if (!serviceRecord.hasConfig)
        {
            const QString errorText = QString::fromUtf8(serviceRecord.configErrorText.c_str());
            if (strictConfig)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = errorText.isEmpty() ? QStringLiteral("读取服务配置失败") : errorText;
                }
                return false;
            }

            entry.descriptionText = QStringLiteral("读取服务配置失败：%1")
                .arg(errorText.isEmpty() ? QStringLiteral("未知错误") : errorText);
            entry.serviceDllPathText = queryServiceDllPathFromRegistry(entry.serviceNameText);
            entry.riskTagList = evaluateRiskTagList(entry);
            entry.riskSummaryText = entry.riskTagList.isEmpty()
                ? QStringLiteral("低")
                : entry.riskTagList.join(QStringLiteral(" | "));
            entry.hasRisk = !entry.riskTagList.isEmpty();
            *entryOut = std::move(entry);
            return true;
        }

        entry.descriptionText = QString::fromStdWString(serviceRecord.description).trimmed();
        entry.commandLineText = QString::fromStdWString(serviceRecord.config.binaryPath).trimmed();
        entry.imagePathText = normalizeServiceImagePath(entry.commandLineText);
        entry.accountText = serviceRecord.config.accountName.empty()
            ? QStringLiteral("N/A")
            : QString::fromStdWString(serviceRecord.config.accountName).trimmed();
        entry.startTypeValue = serviceRecord.config.startType;
        entry.serviceTypeValue = serviceRecord.config.serviceType;
        entry.errorControlValue = serviceRecord.config.errorControl;
        entry.serviceTypeText = serviceTypeToText(entry.serviceTypeValue);
        entry.errorControlText = errorControlToText(entry.errorControlValue);
        entry.serviceDllPathText = queryServiceDllPathFromRegistry(entry.serviceNameText);
        entry.delayedAutoStart = (entry.startTypeValue == SERVICE_AUTO_START) && serviceRecord.config.delayedAutoStart;
        entry.startTypeText = startTypeToText(entry.startTypeValue, entry.delayedAutoStart);
        entry.riskTagList = evaluateRiskTagList(entry);
        entry.riskSummaryText = entry.riskTagList.isEmpty()
            ? QStringLiteral("低")
            : entry.riskTagList.join(QStringLiteral(" | "));
        entry.hasRisk = !entry.riskTagList.isEmpty();

        *entryOut = std::move(entry);
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

    std::vector<ks::service::ServiceRecord> serviceRecordList;
    std::string errorText;
    if (!ks::service::EnumerateServiceRecords(
        SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS,
        SERVICE_STATE_ALL,
        &serviceRecordList,
        &errorText))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("枚举服务失败：%1").arg(QString::fromUtf8(errorText.c_str()));
        }
        return;
    }

    serviceListOut->reserve(serviceRecordList.size());
    for (const ks::service::ServiceRecord& serviceRecord : serviceRecordList)
    {
        ServiceEntry serviceEntry;
        QString buildErrorText;
        if (buildServiceEntryFromRecord(serviceRecord, &serviceEntry, &buildErrorText, false))
        {
            serviceListOut->push_back(std::move(serviceEntry));
        }
    }
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

    ks::service::ServiceRecord serviceRecord;
    std::string errorText;
    if (!ks::service::QueryServiceRecord(
        serviceNameText.trimmed().toStdWString(),
        &serviceRecord,
        &errorText))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("查询服务失败：%1").arg(QString::fromUtf8(errorText.c_str()));
        }
        return false;
    }

    return buildServiceEntryFromRecord(serviceRecord, entryOut, errorTextOut, true);
}
