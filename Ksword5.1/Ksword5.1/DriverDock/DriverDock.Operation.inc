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

void DriverDock::fillObjectDriverNameFromSelection()
{
    // 从当前服务行推导 DriverObject 名称：
    // - 大多数服务名与 \Driver\Name 一致；
    // - 如果用户已手工输入内容，本按钮仍明确覆盖，避免隐式猜测。
    if (m_serviceTable == nullptr ||
        m_serviceTable->selectionModel() == nullptr ||
        m_objectDriverNameEdit == nullptr)
    {
        return;
    }

    const QModelIndexList rowList = m_serviceTable->selectionModel()->selectedRows(0);
    if (rowList.isEmpty())
    {
        m_objectInfoStatusLabel->setText(QStringLiteral("状态：请先在驱动服务表选中一行。"));
        return;
    }

    QTableWidgetItem* serviceNameItem = m_serviceTable->item(rowList.front().row(), 0);
    if (serviceNameItem == nullptr)
    {
        return;
    }

    const QString serviceNameText = serviceNameItem->data(Qt::UserRole).toString().trimmed();
    if (serviceNameText.isEmpty())
    {
        return;
    }
    m_objectDriverNameEdit->setText(QStringLiteral("\\Driver\\%1").arg(serviceNameText));
    if (m_tabWidget != nullptr && m_objectInfoPage != nullptr)
    {
        m_tabWidget->setCurrentWidget(m_objectInfoPage);
    }
}

void DriverDock::showServiceTableContextMenu(const QPoint& localPosition)
{
    // 右键菜单入口：
    // - 普通 SCM 操作仍使用既有按钮/函数；
    // - R0 强制卸载仅挂在驱动服务列表，满足“第四点右键菜单形式”要求。
    if (m_serviceTable == nullptr)
    {
        return;
    }

    const QModelIndex clickedIndex = m_serviceTable->indexAt(localPosition);
    if (clickedIndex.isValid())
    {
        m_serviceTable->selectRow(clickedIndex.row());
        syncOperateFormBySelectedService();
    }

    const QModelIndexList selectedRows =
        (m_serviceTable->selectionModel() == nullptr)
        ? QModelIndexList()
        : m_serviceTable->selectionModel()->selectedRows(0);
    if (selectedRows.isEmpty())
    {
        return;
    }

    QMenu contextMenu(this);
    contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* fillObjectNameAction = contextMenu.addAction(
        QIcon(":/Icon/process_details.svg"),
        QStringLiteral("填充 DriverObject 名称"));
    QAction* queryObjectAction = contextMenu.addAction(
        QIcon(":/Icon/process_refresh.svg"),
        QStringLiteral("查询 DriverObject 信息"));
    contextMenu.addSeparator();
    QAction* forceUnloadAction = contextMenu.addAction(
        QIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("R0 强制卸载 DriverObject"));
    forceUnloadAction->setToolTip(QStringLiteral("调用 DriverUnload 后清理 dispatch/unload 指针；无 DriverUnload 时尝试清设备链。高风险操作。"));

    QAction* selectedAction = contextMenu.exec(m_serviceTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == fillObjectNameAction)
    {
        fillObjectDriverNameFromSelection();
        return;
    }
    if (selectedAction == queryObjectAction)
    {
        fillObjectDriverNameFromSelection();
        querySelectedDriverObjectInfo();
        return;
    }
    if (selectedAction == forceUnloadAction)
    {
        forceUnloadDriverFromServiceRow(selectedRows.front().row());
        return;
    }
}

void DriverDock::forceUnloadDriverFromServiceRow(const int rowIndex)
{
    // 强制卸载流程：
    // - 使用服务名推导 \Driver\ServiceName；
    // - R0 内部再通过 ObReferenceObjectByName 引用对象；
    // - 右键入口使用 FORCE_CLEANUP，使行为接近 SKT64 的强制卸载；
    // - UI 不传递 DriverObject 地址，避免地址成为操作凭据。
    if (m_serviceTable == nullptr || rowIndex < 0 || rowIndex >= m_serviceTable->rowCount())
    {
        return;
    }

    QTableWidgetItem* serviceNameItem = m_serviceTable->item(rowIndex, 0);
    if (serviceNameItem == nullptr)
    {
        return;
    }

    const QString serviceNameText = serviceNameItem->data(Qt::UserRole).toString().trimmed();
    if (serviceNameText.isEmpty())
    {
        appendOperateLogLine(QStringLiteral("强制卸载失败：服务名为空。"));
        return;
    }

    const QString driverObjectNameText = QStringLiteral("\\Driver\\%1").arg(serviceNameText);
    appendOperateLogLine(QStringLiteral("开始 R0 强制卸载：%1").arg(driverObjectNameText));

    QPointer<DriverDock> guardThis(this);
    const std::wstring driverObjectNameWide = driverObjectNameText.toStdWString();
    auto* unloadTask = QRunnable::create([guardThis, driverObjectNameText, driverObjectNameWide]()
        {
            const ksword::ark::DriverForceUnloadResult result =
                ksword::ark::DriverClient().forceUnloadDriver(
                    driverObjectNameWide,
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP,
                    3000UL);

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, driverObjectNameText, result]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }

                    const QString resultLine = QStringLiteral(
                        "R0 强制卸载完成：%1 | IO=%2 | Status=%3 | Last=%4 | Wait=%5 | Object=%6 | Unload=%7")
                        .arg(driverObjectNameText)
                        .arg(QString::fromStdString(result.io.message))
                        .arg(driverForceUnloadStatusText(result.status))
                        .arg(formatNtStatusText(result.lastStatus))
                        .arg(formatNtStatusText(result.waitStatus))
                        .arg(formatCompactAddress(result.driverObjectAddress))
                        .arg(formatCompactAddress(result.driverUnloadAddress));
                    guardThis->appendOperateLogLine(resultLine);
                    guardThis->refreshDriverServiceRecords();
                    guardThis->refreshLoadedKernelModuleRecords();
                },
                Qt::QueuedConnection);
        });
    unloadTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(unloadTask);
}

void DriverDock::querySelectedDriverObjectInfo()
{
    // DriverObject 查询：
    // - 只接受对象名，不接受地址；
    // - 后台线程中通过 ArkDriverClient 访问 KswordARK，避免阻塞 UI。
    if (m_objectInfoQuerying || m_objectDriverNameEdit == nullptr)
    {
        return;
    }

    const QString driverNameText = m_objectDriverNameEdit->text().trimmed();
    if (driverNameText.isEmpty())
    {
        if (m_objectInfoStatusLabel != nullptr)
        {
            m_objectInfoStatusLabel->setText(QStringLiteral("状态：DriverObject 名称不能为空。"));
        }
        return;
    }

    m_objectInfoQuerying = true;
    const std::uint64_t ticketValue = ++m_objectInfoQueryTicket;
    if (m_queryObjectInfoButton != nullptr)
    {
        m_queryObjectInfoButton->setEnabled(false);
    }
    if (m_objectInfoStatusLabel != nullptr)
    {
        m_objectInfoStatusLabel->setText(QStringLiteral("状态：正在查询 DriverObject..."));
    }

    QPointer<DriverDock> guardThis(this);
    const std::wstring driverNameWide = driverNameText.toStdWString();
    auto* queryTask = QRunnable::create([guardThis, ticketValue, driverNameWide]()
        {
            const ksword::ark::DriverObjectQueryResult result =
                ksword::ark::DriverClient().queryDriverObject(
                    driverNameWide,
                    KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ALL,
                    KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT,
                    KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT);

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, ticketValue, result]()
                {
                    if (guardThis == nullptr ||
                        guardThis->m_objectInfoQueryTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyDriverObjectQueryResult(result);
                },
                Qt::QueuedConnection);
        });
    queryTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(queryTask);
}

void DriverDock::applyDriverObjectQueryResult(const ksword::ark::DriverObjectQueryResult& result)
{
    // 查询结果回填：
    // - 所有地址都作为诊断文本展示；
    // - 不在 UI 中将地址作为任何二次操作输入。
    m_objectInfoQuerying = false;
    if (m_queryObjectInfoButton != nullptr)
    {
        m_queryObjectInfoButton->setEnabled(true);
    }

    if (m_objectInfoStatusLabel != nullptr)
    {
        m_objectInfoStatusLabel->setText(QStringLiteral("状态：%1 | %2")
            .arg(result.io.ok ? driverObjectQueryStatusText(result.queryStatus) : QStringLiteral("IO failed"))
            .arg(QString::fromStdString(result.io.message)));
    }

    if (m_objectInfoSummaryEdit != nullptr)
    {
        QStringList summaryLines;
        summaryLines << QStringLiteral("[DriverObject]");
        summaryLines << QStringLiteral("IO: %1").arg(QString::fromStdString(result.io.message));
        summaryLines << QStringLiteral("QueryStatus: %1").arg(driverObjectQueryStatusText(result.queryStatus));
        summaryLines << QStringLiteral("LastStatus: %1").arg(formatNtStatusText(result.lastStatus));
        summaryLines << QStringLiteral("DriverName: %1").arg(QString::fromStdWString(result.driverName));
        summaryLines << QStringLiteral("ServiceKey: %1").arg(QString::fromStdWString(result.serviceKeyName));
        summaryLines << QStringLiteral("ImagePath: %1").arg(QString::fromStdWString(result.imagePath));
        summaryLines << QStringLiteral("DriverObject: %1").arg(formatCompactAddress(result.driverObjectAddress));
        summaryLines << QStringLiteral("DriverStart: %1 Size=%2")
            .arg(formatCompactAddress(result.driverStart))
            .arg(formatHex32(result.driverSize));
        summaryLines << QStringLiteral("DriverSection: %1").arg(formatCompactAddress(result.driverSection));
        summaryLines << QStringLiteral("DriverUnload: %1").arg(formatCompactAddress(result.driverUnload));
        summaryLines << QStringLiteral("Flags: %1 FieldFlags=%2")
            .arg(formatHex32(result.driverFlags))
            .arg(formatHex32(result.fieldFlags));
        summaryLines << QStringLiteral("MajorFunctions: %1 DeviceObjects: %2/%3")
            .arg(result.majorFunctions.size())
            .arg(result.devices.size())
            .arg(result.totalDeviceCount);
        m_objectInfoSummaryEdit->setPlainText(summaryLines.join('\n'));
    }

    if (m_majorFunctionTable != nullptr)
    {
        m_majorFunctionTable->setRowCount(0);
        for (const ksword::ark::DriverMajorFunctionEntry& majorEntry : result.majorFunctions)
        {
            const int rowIndex = m_majorFunctionTable->rowCount();
            m_majorFunctionTable->insertRow(rowIndex);
            m_majorFunctionTable->setItem(rowIndex, 0, createReadOnlyItem(driverMajorFunctionName(majorEntry.majorFunction)));
            m_majorFunctionTable->setItem(rowIndex, 1, createReadOnlyItem(formatCompactAddress(majorEntry.dispatchAddress)));
            m_majorFunctionTable->setItem(rowIndex, 2, createReadOnlyItem(QString::fromStdWString(majorEntry.moduleName).isEmpty()
                ? QStringLiteral("-")
                : QString::fromStdWString(majorEntry.moduleName)));
            m_majorFunctionTable->setItem(rowIndex, 3, createReadOnlyItem(formatCompactAddress(majorEntry.moduleBase)));
            m_majorFunctionTable->setItem(rowIndex, 4, createReadOnlyItem(driverDispatchLocationText(majorEntry.flags)));
            if ((majorEntry.flags & 0x00000002U) == 0U)
            {
                for (int columnIndex = 0; columnIndex < m_majorFunctionTable->columnCount(); ++columnIndex)
                {
                    QTableWidgetItem* cellItem = m_majorFunctionTable->item(rowIndex, columnIndex);
                    if (cellItem != nullptr)
                    {
                        cellItem->setToolTip(QStringLiteral("Dispatch 不在 DriverObject 自身镜像范围内。"));
                    }
                }
            }
        }
    }

    if (m_deviceObjectTable != nullptr)
    {
        m_deviceObjectTable->setRowCount(0);
        for (const ksword::ark::DriverDeviceEntry& deviceEntry : result.devices)
        {
            const int rowIndex = m_deviceObjectTable->rowCount();
            m_deviceObjectTable->insertRow(rowIndex);
            m_deviceObjectTable->setItem(rowIndex, 0, createReadOnlyItem(deviceEntry.relationDepth == 0U
                ? QStringLiteral("Root")
                : QStringLiteral("Attached +%1").arg(deviceEntry.relationDepth)));
            m_deviceObjectTable->setItem(rowIndex, 1, createReadOnlyItem(formatCompactAddress(deviceEntry.deviceObjectAddress)));
            QTableWidgetItem* nameItem = createReadOnlyItem(QString::fromStdWString(deviceEntry.deviceName).isEmpty()
                ? QStringLiteral("(unnamed)")
                : QString::fromStdWString(deviceEntry.deviceName));
            nameItem->setToolTip(QStringLiteral("NameStatus=%1").arg(formatNtStatusText(deviceEntry.nameStatus)));
            m_deviceObjectTable->setItem(rowIndex, 2, nameItem);
            m_deviceObjectTable->setItem(rowIndex, 3, createReadOnlyItem(driverDeviceTypeText(deviceEntry.deviceType)));
            m_deviceObjectTable->setItem(rowIndex, 4, createReadOnlyItem(formatHex32(deviceEntry.flags)));
            m_deviceObjectTable->setItem(rowIndex, 5, createReadOnlyItem(formatHex32(deviceEntry.characteristics)));
            m_deviceObjectTable->setItem(rowIndex, 6, createReadOnlyItem(QString::number(deviceEntry.stackSize)));
            m_deviceObjectTable->setItem(rowIndex, 7, createReadOnlyItem(formatCompactAddress(deviceEntry.nextDeviceObjectAddress)));
            m_deviceObjectTable->setItem(rowIndex, 8, createReadOnlyItem(formatCompactAddress(deviceEntry.attachedDeviceObjectAddress)));
            m_deviceObjectTable->setItem(rowIndex, 9, createReadOnlyItem(formatCompactAddress(deviceEntry.driverObjectAddress)));
        }
    }
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
                    cellItem->setBackground(KswordTheme::NewRowBackgroundColor());
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
            // 577 等签名失败需要单独解释：
            // - 用户可能已经开启测试模式；
            // - 但完全未签名的 x64 驱动仍会被系统拒绝。
            if (isDriverSignatureLoadError(lastError))
            {
                const QString binaryPathText =
                    (m_binaryPathEdit == nullptr) ? QString() : m_binaryPathEdit->text().trimmed();
                const QString adviceText =
                    buildDriverSignatureLoadAdvice(lastError, serviceNameText, binaryPathText);
                appendOperateLogLine(adviceText);
                err << operationEvent
                    << "[DriverDock] 挂载失败：驱动签名/镜像校验失败, service="
                    << serviceNameText.toStdString()
                    << ", error="
                    << lastError
                    << ", path="
                    << binaryPathText.toStdString()
                    << eol;
                return;
            }

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

