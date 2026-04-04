#include "StartupDock.Internal.h"

#include <winsvc.h>

#pragma comment(lib, "Advapi32.lib")

using namespace startup_dock_detail;

namespace
{
    // queryServiceBinaryPathText 作用：
    // - 从服务配置结构里提取可执行/驱动镜像路径。
    QString queryServiceBinaryPathText(const QUERY_SERVICE_CONFIGW& serviceConfig)
    {
        if (serviceConfig.lpBinaryPathName == nullptr)
        {
            return QString();
        }
        return QDir::toNativeSeparators(QString::fromWCharArray(serviceConfig.lpBinaryPathName).trimmed());
    }
}

void StartupDock::appendServiceEntries(std::vector<StartupEntry>* entryListOut)
{
    if (entryListOut == nullptr)
    {
        return;
    }

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scmHandle == nullptr)
    {
        return;
    }

    DWORD requiredBytes = 0;
    DWORD serviceCount = 0;
    DWORD resumeHandle = 0;
    ::EnumServicesStatusExW(
        scmHandle,
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32,
        SERVICE_STATE_ALL,
        nullptr,
        0,
        &requiredBytes,
        &serviceCount,
        &resumeHandle,
        nullptr);
    if (requiredBytes == 0)
    {
        ::CloseServiceHandle(scmHandle);
        return;
    }

    std::vector<std::uint8_t> buffer(requiredBytes);
    const BOOL enumOk = ::EnumServicesStatusExW(
        scmHandle,
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32,
        SERVICE_STATE_ALL,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &requiredBytes,
        &serviceCount,
        &resumeHandle,
        nullptr);
    if (enumOk == FALSE)
    {
        ::CloseServiceHandle(scmHandle);
        return;
    }

    const ENUM_SERVICE_STATUS_PROCESSW* serviceArray =
        reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD serviceIndex = 0; serviceIndex < serviceCount; ++serviceIndex)
    {
        const ENUM_SERVICE_STATUS_PROCESSW& serviceItem = serviceArray[serviceIndex];
        SC_HANDLE serviceHandle = ::OpenServiceW(
            scmHandle,
            serviceItem.lpServiceName,
            SERVICE_QUERY_CONFIG);
        if (serviceHandle == nullptr)
        {
            continue;
        }

        DWORD configBytes = 0;
        ::QueryServiceConfigW(serviceHandle, nullptr, 0, &configBytes);
        if (configBytes == 0)
        {
            ::CloseServiceHandle(serviceHandle);
            continue;
        }

        std::vector<std::uint8_t> configBuffer(configBytes);
        QUERY_SERVICE_CONFIGW* configPointer =
            reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
        if (::QueryServiceConfigW(serviceHandle, configPointer, configBytes, &configBytes) == FALSE)
        {
            ::CloseServiceHandle(serviceHandle);
            continue;
        }

        const DWORD startTypeValue = configPointer->dwStartType;
        if (startTypeValue != SERVICE_AUTO_START)
        {
            ::CloseServiceHandle(serviceHandle);
            continue;
        }

        StartupEntry entry;
        entry.category = StartupCategory::Services;
        entry.categoryText = categoryToText(entry.category);
        entry.itemNameText = QString::fromWCharArray(serviceItem.lpDisplayName).trimmed();
        if (entry.itemNameText.isEmpty())
        {
            entry.itemNameText = QString::fromWCharArray(serviceItem.lpServiceName);
        }
        entry.imagePathText = normalizeFilePathText(queryServiceBinaryPathText(*configPointer));
        entry.commandText = queryServiceBinaryPathText(*configPointer);
        entry.publisherText = queryPublisherTextByPath(entry.imagePathText);
        entry.locationText = QStringLiteral("SCM\\Service\\%1").arg(QString::fromWCharArray(serviceItem.lpServiceName));
        entry.userText = (configPointer->lpServiceStartName != nullptr)
            ? QString::fromWCharArray(configPointer->lpServiceStartName)
            : QStringLiteral("N/A");
        entry.enabled = true;
        entry.sourceTypeText = QStringLiteral("AutoService");
        entry.detailText = QStringLiteral("自动启动服务");
        entry.canOpenFileLocation = !entry.imagePathText.isEmpty();
        entry.canDelete = true;
        entry.uniqueIdText = QStringLiteral("SERVICE|%1").arg(QString::fromWCharArray(serviceItem.lpServiceName));
        entryListOut->push_back(entry);

        ::CloseServiceHandle(serviceHandle);
    }

    ::CloseServiceHandle(scmHandle);
}

void StartupDock::appendDriverEntries(std::vector<StartupEntry>* entryListOut)
{
    if (entryListOut == nullptr)
    {
        return;
    }

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scmHandle == nullptr)
    {
        return;
    }

    DWORD requiredBytes = 0;
    DWORD serviceCount = 0;
    DWORD resumeHandle = 0;
    ::EnumServicesStatusExW(
        scmHandle,
        SC_ENUM_PROCESS_INFO,
        SERVICE_DRIVER,
        SERVICE_STATE_ALL,
        nullptr,
        0,
        &requiredBytes,
        &serviceCount,
        &resumeHandle,
        nullptr);
    if (requiredBytes == 0)
    {
        ::CloseServiceHandle(scmHandle);
        return;
    }

    std::vector<std::uint8_t> buffer(requiredBytes);
    const BOOL enumOk = ::EnumServicesStatusExW(
        scmHandle,
        SC_ENUM_PROCESS_INFO,
        SERVICE_DRIVER,
        SERVICE_STATE_ALL,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &requiredBytes,
        &serviceCount,
        &resumeHandle,
        nullptr);
    if (enumOk == FALSE)
    {
        ::CloseServiceHandle(scmHandle);
        return;
    }

    const ENUM_SERVICE_STATUS_PROCESSW* driverArray =
        reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD driverIndex = 0; driverIndex < serviceCount; ++driverIndex)
    {
        const ENUM_SERVICE_STATUS_PROCESSW& driverItem = driverArray[driverIndex];
        SC_HANDLE serviceHandle = ::OpenServiceW(
            scmHandle,
            driverItem.lpServiceName,
            SERVICE_QUERY_CONFIG);
        if (serviceHandle == nullptr)
        {
            continue;
        }

        DWORD configBytes = 0;
        ::QueryServiceConfigW(serviceHandle, nullptr, 0, &configBytes);
        if (configBytes == 0)
        {
            ::CloseServiceHandle(serviceHandle);
            continue;
        }

        std::vector<std::uint8_t> configBuffer(configBytes);
        QUERY_SERVICE_CONFIGW* configPointer =
            reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
        if (::QueryServiceConfigW(serviceHandle, configPointer, configBytes, &configBytes) == FALSE)
        {
            ::CloseServiceHandle(serviceHandle);
            continue;
        }

        const DWORD startTypeValue = configPointer->dwStartType;
        if (startTypeValue != SERVICE_AUTO_START
            && startTypeValue != SERVICE_SYSTEM_START
            && startTypeValue != SERVICE_BOOT_START)
        {
            ::CloseServiceHandle(serviceHandle);
            continue;
        }

        StartupEntry entry;
        entry.category = StartupCategory::Drivers;
        entry.categoryText = categoryToText(entry.category);
        entry.itemNameText = QString::fromWCharArray(driverItem.lpDisplayName).trimmed();
        if (entry.itemNameText.isEmpty())
        {
            entry.itemNameText = QString::fromWCharArray(driverItem.lpServiceName);
        }
        entry.imagePathText = normalizeFilePathText(queryServiceBinaryPathText(*configPointer));
        entry.commandText = queryServiceBinaryPathText(*configPointer);
        entry.publisherText = queryPublisherTextByPath(entry.imagePathText);
        entry.locationText = QStringLiteral("SCM\\Driver\\%1").arg(QString::fromWCharArray(driverItem.lpServiceName));
        entry.userText = QStringLiteral("内核");
        entry.enabled = true;
        entry.sourceTypeText = QStringLiteral("Driver");
        if (startTypeValue == SERVICE_BOOT_START)
        {
            entry.detailText = QStringLiteral("引导启动驱动");
        }
        else if (startTypeValue == SERVICE_SYSTEM_START)
        {
            entry.detailText = QStringLiteral("系统启动驱动");
        }
        else
        {
            entry.detailText = QStringLiteral("自动启动驱动");
        }
        entry.canOpenFileLocation = !entry.imagePathText.isEmpty();
        entry.canDelete = true;
        entry.uniqueIdText = QStringLiteral("DRIVER|%1").arg(QString::fromWCharArray(driverItem.lpServiceName));
        entryListOut->push_back(entry);

        ::CloseServiceHandle(serviceHandle);
    }

    ::CloseServiceHandle(scmHandle);
}
