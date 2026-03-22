void DriverDock::refreshDriverServiceRecords()
{
    kLogEvent refreshEvent;
    info << refreshEvent << "[DriverDock] 开始刷新驱动服务列表。" << eol;

    std::vector<DriverServiceRecord> serviceRecordList;
    std::string errorText;
    if (!queryDriverServiceRecords(serviceRecordList, &errorText))
    {
        m_driverServiceCache.clear();
        rebuildDriverServiceTableByFilter();
        appendOperateLogLine(QStringLiteral("刷新服务失败：%1").arg(QString::fromUtf8(errorText.c_str())));
        if (m_overviewStatusLabel != nullptr)
        {
            m_overviewStatusLabel->setText(
                QStringLiteral("状态：服务刷新失败（%1）").arg(QString::fromUtf8(errorText.c_str())));
        }
        warn << refreshEvent << "[DriverDock] 刷新服务失败, detail=" << errorText << eol;
        return;
    }

    m_driverServiceCache = std::move(serviceRecordList);
    rebuildDriverServiceTableByFilter();

    if (m_overviewStatusLabel != nullptr)
    {
        m_overviewStatusLabel->setText(
            QStringLiteral("状态：驱动服务 %1 条，模块 %2 条")
            .arg(m_driverServiceCache.size())
            .arg(m_loadedModuleCache.size()));
    }

    info << refreshEvent << "[DriverDock] 刷新服务完成, count=" << m_driverServiceCache.size() << eol;
}

void DriverDock::refreshLoadedKernelModuleRecords()
{
    kLogEvent refreshEvent;
    info << refreshEvent << "[DriverDock] 开始刷新已加载模块列表。" << eol;

    std::vector<LoadedKernelModuleRecord> moduleRecordList;
    std::string errorText;
    if (!queryLoadedKernelModuleRecords(moduleRecordList, &errorText))
    {
        m_loadedModuleCache.clear();
        rebuildLoadedModuleTable();
        appendOperateLogLine(QStringLiteral("刷新模块失败：%1").arg(QString::fromUtf8(errorText.c_str())));
        if (m_overviewStatusLabel != nullptr)
        {
            m_overviewStatusLabel->setText(
                QStringLiteral("状态：模块刷新失败（%1）").arg(QString::fromUtf8(errorText.c_str())));
        }
        warn << refreshEvent << "[DriverDock] 刷新模块失败, detail=" << errorText << eol;
        return;
    }

    m_loadedModuleCache = std::move(moduleRecordList);
    rebuildLoadedModuleTable();

    if (m_overviewStatusLabel != nullptr)
    {
        m_overviewStatusLabel->setText(
            QStringLiteral("状态：驱动服务 %1 条，模块 %2 条")
            .arg(m_driverServiceCache.size())
            .arg(m_loadedModuleCache.size()));
    }

    info << refreshEvent << "[DriverDock] 刷新模块完成, count=" << m_loadedModuleCache.size() << eol;
}

void DriverDock::rebuildDriverServiceTableByFilter()
{
    if (m_serviceTable == nullptr)
    {
        return;
    }

    const QString filterText = (m_serviceFilterEdit == nullptr)
        ? QString()
        : m_serviceFilterEdit->text().trimmed();

    m_serviceTable->setRowCount(0);
    int visibleCount = 0;
    for (const DriverServiceRecord& serviceRecord : m_driverServiceCache)
    {
        const bool matchFilter =
            filterText.isEmpty() ||
            serviceRecord.serviceName.contains(filterText, Qt::CaseInsensitive) ||
            serviceRecord.displayName.contains(filterText, Qt::CaseInsensitive) ||
            serviceRecord.binaryPath.contains(filterText, Qt::CaseInsensitive) ||
            serviceRecord.description.contains(filterText, Qt::CaseInsensitive);
        if (!matchFilter)
        {
            continue;
        }

        const int rowIndex = m_serviceTable->rowCount();
        m_serviceTable->insertRow(rowIndex);

        QTableWidgetItem* serviceNameItem = createReadOnlyItem(serviceRecord.serviceName);
        serviceNameItem->setData(Qt::UserRole, serviceRecord.serviceName);
        m_serviceTable->setItem(rowIndex, 0, serviceNameItem);
        m_serviceTable->setItem(rowIndex, 1, createReadOnlyItem(serviceRecord.displayName));
        m_serviceTable->setItem(rowIndex, 2, createReadOnlyItem(serviceStateToText(serviceRecord.currentState)));
        m_serviceTable->setItem(rowIndex, 3, createReadOnlyItem(startTypeToText(serviceRecord.startType)));
        m_serviceTable->setItem(rowIndex, 4, createReadOnlyItem(errorControlToText(serviceRecord.errorControl)));

        QTableWidgetItem* pathItem = createReadOnlyItem(serviceRecord.binaryPath);
        pathItem->setToolTip(serviceRecord.binaryPath);
        m_serviceTable->setItem(rowIndex, 5, pathItem);

        QTableWidgetItem* descriptionItem = createReadOnlyItem(serviceRecord.description);
        descriptionItem->setToolTip(serviceRecord.description);
        m_serviceTable->setItem(rowIndex, 6, descriptionItem);

        if (serviceRecord.currentState == SERVICE_RUNNING)
        {
            for (int columnIndex = 0; columnIndex < m_serviceTable->columnCount(); ++columnIndex)
            {
                QTableWidgetItem* cellItem = m_serviceTable->item(rowIndex, columnIndex);
                if (cellItem != nullptr)
                {
                    cellItem->setBackground(QColor(226, 255, 232));
                }
            }
        }
        ++visibleCount;
    }

    if (m_overviewStatusLabel != nullptr)
    {
        m_overviewStatusLabel->setText(
            QStringLiteral("状态：驱动服务 %1 条（显示 %2 条），模块 %3 条")
            .arg(m_driverServiceCache.size())
            .arg(visibleCount)
            .arg(m_loadedModuleCache.size()));
    }
}

void DriverDock::rebuildLoadedModuleTable()
{
    if (m_moduleTable == nullptr)
    {
        return;
    }

    m_moduleTable->setRowCount(0);
    for (const LoadedKernelModuleRecord& moduleRecord : m_loadedModuleCache)
    {
        const int rowIndex = m_moduleTable->rowCount();
        m_moduleTable->insertRow(rowIndex);
        m_moduleTable->setItem(rowIndex, 0, createReadOnlyItem(moduleRecord.moduleName));
        m_moduleTable->setItem(rowIndex, 1, createReadOnlyItem(formatAddress(moduleRecord.baseAddress)));
        QTableWidgetItem* pathItem = createReadOnlyItem(moduleRecord.imagePath);
        pathItem->setToolTip(moduleRecord.imagePath);
        m_moduleTable->setItem(rowIndex, 2, pathItem);
    }
}

void DriverDock::syncOperateFormBySelectedService()
{
    if (m_serviceTable == nullptr || m_serviceTable->selectionModel() == nullptr)
    {
        return;
    }

    const QModelIndexList rowList = m_serviceTable->selectionModel()->selectedRows(0);
    if (rowList.isEmpty())
    {
        return;
    }

    const int rowIndex = rowList.front().row();
    QTableWidgetItem* serviceNameItem = m_serviceTable->item(rowIndex, 0);
    if (serviceNameItem == nullptr)
    {
        return;
    }

    const QString serviceNameText = serviceNameItem->data(Qt::UserRole).toString();
    auto iterator = std::find_if(
        m_driverServiceCache.begin(),
        m_driverServiceCache.end(),
        [&serviceNameText](const DriverServiceRecord& record)
        {
            return record.serviceName.compare(serviceNameText, Qt::CaseInsensitive) == 0;
        });
    if (iterator == m_driverServiceCache.end())
    {
        return;
    }

    if (m_serviceNameEdit != nullptr)
    {
        m_serviceNameEdit->setText(iterator->serviceName);
    }
    if (m_displayNameEdit != nullptr)
    {
        m_displayNameEdit->setText(iterator->displayName);
    }
    if (m_binaryPathEdit != nullptr)
    {
        m_binaryPathEdit->setText(trimQuotedText(iterator->binaryPath));
    }
    if (m_descriptionEdit != nullptr)
    {
        m_descriptionEdit->setText(iterator->description);
    }

    if (m_startTypeCombo != nullptr)
    {
        const int startTypeIndex = m_startTypeCombo->findData(static_cast<int>(iterator->startType));
        if (startTypeIndex >= 0)
        {
            m_startTypeCombo->setCurrentIndex(startTypeIndex);
        }
    }
    if (m_errorControlCombo != nullptr)
    {
        const int errorControlIndex = m_errorControlCombo->findData(static_cast<int>(iterator->errorControl));
        if (errorControlIndex >= 0)
        {
            m_errorControlCombo->setCurrentIndex(errorControlIndex);
        }
    }
}

void DriverDock::refreshSelectedServiceStateToForm()
{
    if (m_serviceNameEdit == nullptr)
    {
        return;
    }

    kLogEvent queryEvent;
    const QString serviceNameText = m_serviceNameEdit->text().trimmed();
    if (serviceNameText.isEmpty())
    {
        appendOperateLogLine(QStringLiteral("查询失败：服务名不能为空。"));
        warn << queryEvent << "[DriverDock] 查询状态失败：服务名为空。" << eol;
        return;
    }

    ServiceHandleGuard scmHandle(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scmHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("查询失败：OpenSCManagerW 失败（%1）").arg(formatWin32ErrorText(lastError)));
        warn << queryEvent << "[DriverDock] 查询状态失败：OpenSCManagerW 失败, error=" << lastError << eol;
        return;
    }

    const std::wstring serviceNameWide = toWideString(serviceNameText);
    ServiceHandleGuard serviceHandle(::OpenServiceW(
        scmHandle.get(),
        serviceNameWide.c_str(),
        SERVICE_QUERY_STATUS));
    if (!serviceHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("查询失败：OpenServiceW 失败（%1）").arg(formatWin32ErrorText(lastError)));
        warn << queryEvent
            << "[DriverDock] 查询状态失败：OpenServiceW 失败, service="
            << serviceNameText.toStdString()
            << ", error=" << lastError
            << eol;
        return;
    }

    SERVICE_STATUS_PROCESS statusInfo{};
    DWORD bytesNeeded = 0;
    if (!::QueryServiceStatusEx(
        serviceHandle.get(),
        SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&statusInfo),
        static_cast<DWORD>(sizeof(statusInfo)),
        &bytesNeeded))
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("查询失败：QueryServiceStatusEx 失败（%1）").arg(formatWin32ErrorText(lastError)));
        warn << queryEvent << "[DriverDock] 查询状态失败：QueryServiceStatusEx 失败, error=" << lastError << eol;
        return;
    }

    appendOperateLogLine(
        QStringLiteral("服务 %1 当前状态：%2")
        .arg(serviceNameText)
        .arg(serviceStateToText(statusInfo.dwCurrentState)));

    info << queryEvent
        << "[DriverDock] 查询状态成功, service=" << serviceNameText.toStdString()
        << ", state=" << statusInfo.dwCurrentState
        << eol;
}

void DriverDock::registerOrUpdateDriverService()
{
    if (m_serviceNameEdit == nullptr ||
        m_binaryPathEdit == nullptr ||
        m_startTypeCombo == nullptr ||
        m_errorControlCombo == nullptr)
    {
        return;
    }

    kLogEvent operationEvent;
    const QString serviceNameText = m_serviceNameEdit->text().trimmed();
    const QString displayNameText = (m_displayNameEdit == nullptr)
        ? QString()
        : m_displayNameEdit->text().trimmed();
    const QString descriptionText = (m_descriptionEdit == nullptr)
        ? QString()
        : m_descriptionEdit->text().trimmed();
    const QString binaryPathText = normalizeDriverBinaryPath(m_binaryPathEdit->text().trimmed());

    if (serviceNameText.isEmpty())
    {
        appendOperateLogLine(QStringLiteral("注册/更新失败：服务名不能为空。"));
        warn << operationEvent << "[DriverDock] 注册/更新失败：服务名为空。" << eol;
        return;
    }
    if (binaryPathText.isEmpty())
    {
        appendOperateLogLine(QStringLiteral("注册/更新失败：驱动路径不能为空。"));
        warn << operationEvent << "[DriverDock] 注册/更新失败：路径为空。" << eol;
        return;
    }

    const QString unquotedPathText = trimQuotedText(binaryPathText);
    if (!QFileInfo::exists(unquotedPathText) &&
        !unquotedPathText.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive) &&
        !unquotedPathText.startsWith(QStringLiteral("%SystemRoot%"), Qt::CaseInsensitive))
    {
        appendOperateLogLine(QStringLiteral("警告：驱动路径当前不可访问，仍将尝试注册。"));
    }

    const int startTypeValue = m_startTypeCombo->currentData().toInt();
    const int errorControlValue = m_errorControlCombo->currentData().toInt();
    const std::wstring serviceNameWide = toWideString(serviceNameText);
    const std::wstring displayNameWide = toWideString(displayNameText);
    const std::wstring descriptionWide = toWideString(descriptionText);
    const std::wstring binaryPathWide = toWideString(binaryPathText);

    ServiceHandleGuard scmHandle(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scmHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("注册/更新失败：OpenSCManagerW 失败（%1）").arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 注册/更新失败：OpenSCManagerW 失败, error=" << lastError << eol;
        return;
    }

    constexpr DWORD kDesiredAccess =
        SERVICE_QUERY_STATUS |
        SERVICE_QUERY_CONFIG |
        SERVICE_CHANGE_CONFIG |
        SERVICE_START |
        SERVICE_STOP |
        DELETE;

    ServiceHandleGuard serviceHandle(::OpenServiceW(
        scmHandle.get(),
        serviceNameWide.c_str(),
        kDesiredAccess));

    bool created = false;
    if (!serviceHandle.valid())
    {
        const DWORD openServiceError = ::GetLastError();
        if (openServiceError != ERROR_SERVICE_DOES_NOT_EXIST)
        {
            appendOperateLogLine(
                QStringLiteral("注册/更新失败：OpenServiceW 失败（%1）")
                .arg(formatWin32ErrorText(openServiceError)));
            err << operationEvent
                << "[DriverDock] 注册/更新失败：OpenServiceW 失败, error="
                << openServiceError
                << eol;
            return;
        }

        SC_HANDLE createdService = ::CreateServiceW(
            scmHandle.get(),
            serviceNameWide.c_str(),
            displayNameWide.empty() ? serviceNameWide.c_str() : displayNameWide.c_str(),
            kDesiredAccess,
            SERVICE_KERNEL_DRIVER,
            static_cast<DWORD>(startTypeValue),
            static_cast<DWORD>(errorControlValue),
            binaryPathWide.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (createdService == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            appendOperateLogLine(QStringLiteral("注册失败：CreateServiceW 失败（%1）").arg(formatWin32ErrorText(lastError)));
            err << operationEvent << "[DriverDock] 注册失败：CreateServiceW 失败, error=" << lastError << eol;
            return;
        }
        serviceHandle.reset(createdService);
        created = true;
    }

    if (!::ChangeServiceConfigW(
        serviceHandle.get(),
        SERVICE_KERNEL_DRIVER,
        static_cast<DWORD>(startTypeValue),
        static_cast<DWORD>(errorControlValue),
        binaryPathWide.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        displayNameWide.empty() ? nullptr : displayNameWide.c_str()))
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("更新失败：ChangeServiceConfigW 失败（%1）").arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 更新失败：ChangeServiceConfigW 失败, error=" << lastError << eol;
        return;
    }

    std::wstring mutableDescription = descriptionWide;
    SERVICE_DESCRIPTIONW serviceDescription{};
    serviceDescription.lpDescription = mutableDescription.empty() ? nullptr : mutableDescription.data();
    (void)::ChangeServiceConfig2W(
        serviceHandle.get(),
        SERVICE_CONFIG_DESCRIPTION,
        reinterpret_cast<LPBYTE>(&serviceDescription));

    appendOperateLogLine(QStringLiteral("%1成功：service=%2")
        .arg(created ? QStringLiteral("注册") : QStringLiteral("更新"))
        .arg(serviceNameText));

    info << operationEvent
        << "[DriverDock] 注册/更新成功, created=" << (created ? "true" : "false")
        << ", service=" << serviceNameText.toStdString()
        << eol;

    refreshDriverServiceRecords();
}

void DriverDock::loadSelectedDriverService()
{
    if (m_serviceNameEdit == nullptr)
    {
        return;
    }

    kLogEvent operationEvent;
    const QString serviceNameText = m_serviceNameEdit->text().trimmed();
    if (serviceNameText.isEmpty())
    {
        appendOperateLogLine(QStringLiteral("挂载失败：服务名不能为空。"));
        warn << operationEvent << "[DriverDock] 挂载失败：服务名为空。" << eol;
        return;
    }

    ServiceHandleGuard scmHandle(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scmHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(
            QStringLiteral("挂载失败：OpenSCManagerW 失败（%1）")
            .arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 挂载失败：OpenSCManagerW 失败, error=" << lastError << eol;
        return;
    }

    const std::wstring serviceNameWide = toWideString(serviceNameText);
    ServiceHandleGuard serviceHandle(::OpenServiceW(
        scmHandle.get(),
        serviceNameWide.c_str(),
        SERVICE_START | SERVICE_QUERY_STATUS));
    if (!serviceHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("挂载失败：OpenServiceW 失败（%1）").arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 挂载失败：OpenServiceW 失败, error=" << lastError << eol;
        return;
    }

    if (!::StartServiceW(serviceHandle.get(), 0, nullptr))
    {
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_SERVICE_ALREADY_RUNNING)
        {
            appendOperateLogLine(QStringLiteral("挂载失败：StartServiceW 失败（%1）").arg(formatWin32ErrorText(lastError)));
            err << operationEvent << "[DriverDock] 挂载失败：StartServiceW 失败, error=" << lastError << eol;
            return;
        }

        info << operationEvent
            << "[DriverDock] 挂载请求命中“已运行”状态, service="
            << serviceNameText.toStdString()
            << eol;
    }

    DWORD currentState = 0;
    const bool started = waitServiceState(serviceHandle.get(), SERVICE_RUNNING, 6000, &currentState);
    appendOperateLogLine(started
        ? QStringLiteral("挂载成功：service=%1").arg(serviceNameText)
        : QStringLiteral("挂载结束：当前状态=%1").arg(serviceStateToText(currentState)));

    info << operationEvent
        << "[DriverDock] 挂载执行完成, service=" << serviceNameText.toStdString()
        << ", finalState=" << currentState
        << eol;

    refreshDriverServiceRecords();
    refreshLoadedKernelModuleRecords();
}

void DriverDock::unloadSelectedDriverService()
{
    if (m_serviceNameEdit == nullptr)
    {
        return;
    }

    kLogEvent operationEvent;
    const QString serviceNameText = m_serviceNameEdit->text().trimmed();
    if (serviceNameText.isEmpty())
    {
        appendOperateLogLine(QStringLiteral("卸载失败：服务名不能为空。"));
        warn << operationEvent << "[DriverDock] 卸载失败：服务名为空。" << eol;
        return;
    }

    ServiceHandleGuard scmHandle(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scmHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(
            QStringLiteral("卸载失败：OpenSCManagerW 失败（%1）")
            .arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 卸载失败：OpenSCManagerW 失败, error=" << lastError << eol;
        return;
    }

    const std::wstring serviceNameWide = toWideString(serviceNameText);
    ServiceHandleGuard serviceHandle(::OpenServiceW(
        scmHandle.get(),
        serviceNameWide.c_str(),
        SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!serviceHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("卸载失败：OpenServiceW 失败（%1）").arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 卸载失败：OpenServiceW 失败, error=" << lastError << eol;
        return;
    }

    SERVICE_STATUS serviceStatus{};
    if (!::ControlService(serviceHandle.get(), SERVICE_CONTROL_STOP, &serviceStatus))
    {
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_SERVICE_NOT_ACTIVE)
        {
            appendOperateLogLine(QStringLiteral("卸载失败：ControlService 失败（%1）").arg(formatWin32ErrorText(lastError)));
            err << operationEvent << "[DriverDock] 卸载失败：ControlService 失败, error=" << lastError << eol;
            return;
        }

        info << operationEvent
            << "[DriverDock] 卸载请求命中“未运行”状态, service="
            << serviceNameText.toStdString()
            << eol;
    }

    DWORD currentState = 0;
    const bool stopped = waitServiceState(serviceHandle.get(), SERVICE_STOPPED, 6000, &currentState);
    appendOperateLogLine(stopped
        ? QStringLiteral("卸载成功：service=%1").arg(serviceNameText)
        : QStringLiteral("卸载结束：当前状态=%1").arg(serviceStateToText(currentState)));

    info << operationEvent
        << "[DriverDock] 卸载执行完成, service=" << serviceNameText.toStdString()
        << ", finalState=" << currentState
        << eol;

    refreshDriverServiceRecords();
    refreshLoadedKernelModuleRecords();
}

void DriverDock::deleteSelectedDriverService()
{
    if (m_serviceNameEdit == nullptr)
    {
        return;
    }

    kLogEvent operationEvent;
    const QString serviceNameText = m_serviceNameEdit->text().trimmed();
    if (serviceNameText.isEmpty())
    {
        appendOperateLogLine(QStringLiteral("删除失败：服务名不能为空。"));
        warn << operationEvent << "[DriverDock] 删除失败：服务名为空。" << eol;
        return;
    }

    ServiceHandleGuard scmHandle(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scmHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(
            QStringLiteral("删除失败：OpenSCManagerW 失败（%1）")
            .arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 删除失败：OpenSCManagerW 失败, error=" << lastError << eol;
        return;
    }

    const std::wstring serviceNameWide = toWideString(serviceNameText);
    ServiceHandleGuard serviceHandle(::OpenServiceW(
        scmHandle.get(),
        serviceNameWide.c_str(),
        DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!serviceHandle.valid())
    {
        const DWORD lastError = ::GetLastError();
        appendOperateLogLine(QStringLiteral("删除失败：OpenServiceW 失败（%1）").arg(formatWin32ErrorText(lastError)));
        err << operationEvent << "[DriverDock] 删除失败：OpenServiceW 失败, error=" << lastError << eol;
        return;
    }

    SERVICE_STATUS_PROCESS statusInfo{};
    DWORD bytesNeeded = 0;
    if (::QueryServiceStatusEx(
        serviceHandle.get(),
        SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&statusInfo),
        static_cast<DWORD>(sizeof(statusInfo)),
        &bytesNeeded))
    {
        if (statusInfo.dwCurrentState != SERVICE_STOPPED)
        {
            SERVICE_STATUS stopStatus{};
            (void)::ControlService(serviceHandle.get(), SERVICE_CONTROL_STOP, &stopStatus);
            (void)waitServiceState(serviceHandle.get(), SERVICE_STOPPED, 4000, nullptr);
        }
    }

    if (!::DeleteService(serviceHandle.get()))
    {
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_SERVICE_MARKED_FOR_DELETE)
        {
            appendOperateLogLine(QStringLiteral("删除失败：DeleteService 失败（%1）").arg(formatWin32ErrorText(lastError)));
            err << operationEvent << "[DriverDock] 删除失败：DeleteService 失败, error=" << lastError << eol;
            return;
        }

        warn << operationEvent
            << "[DriverDock] 删除返回“已标记删除”, service="
            << serviceNameText.toStdString()
            << eol;
    }

    appendOperateLogLine(QStringLiteral("删除成功（或已标记删除）：service=%1").arg(serviceNameText));
    info << operationEvent << "[DriverDock] 删除执行完成, service=" << serviceNameText.toStdString() << eol;
    refreshDriverServiceRecords();
    refreshLoadedKernelModuleRecords();
}

