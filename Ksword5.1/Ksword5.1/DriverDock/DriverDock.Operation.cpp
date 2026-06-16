#include "DriverDock.Internal.h"

// 说明：由原聚合式实现迁移为独立 .cpp，成员函数实现保持原样。
using namespace ksword::driver_dock_internal;

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
        ++m_moduleEvidenceQueryTicket;
        m_moduleEvidenceQuerying = false;
        if (m_refreshModuleEvidenceButton != nullptr)
        {
            m_refreshModuleEvidenceButton->setEnabled(true);
        }
        m_loadedModuleCache.clear();
        m_loadedModuleEvidenceCache.clear();
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

    ++m_moduleEvidenceQueryTicket;
    m_moduleEvidenceQuerying = false;
    if (m_refreshModuleEvidenceButton != nullptr)
    {
        m_refreshModuleEvidenceButton->setEnabled(true);
    }

    m_loadedModuleCache = std::move(moduleRecordList);
    m_loadedModuleEvidenceCache.clear();
    m_loadedModuleEvidenceCache.reserve(m_loadedModuleCache.size());
    for (const LoadedKernelModuleRecord& moduleRecord : m_loadedModuleCache)
    {
        m_loadedModuleEvidenceCache.push_back(buildPendingModuleEvidenceRecord(moduleRecord));
    }
    rebuildLoadedModuleTable();

    if (m_overviewStatusLabel != nullptr)
    {
        m_overviewStatusLabel->setText(
            QStringLiteral("状态：驱动服务 %1 条，模块 %2 条")
            .arg(m_driverServiceCache.size())
            .arg(m_loadedModuleCache.size()));
    }
    if (m_moduleEvidenceStatusLabel != nullptr)
    {
        m_moduleEvidenceStatusLabel->setText(QStringLiteral("证据：模块已刷新，等待后台聚合。"));
    }
    if (!m_loadedModuleCache.empty())
    {
        // 模块证据聚合只在已有模块快照时启动：
        // - 输入：当前 EnumDeviceDrivers 枚举出的模块缓存；
        // - 处理：交给后台线程调用现有 ArkDriverClient 只读接口；
        // - 返回：无；空列表直接停留在提示状态，避免刷新函数互相递归。
        refreshLoadedModuleEvidenceAsync();
    }
    else if (m_moduleEvidenceStatusLabel != nullptr)
    {
        m_moduleEvidenceStatusLabel->setText(QStringLiteral("证据：没有可聚合的模块。"));
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
    // - R0 DriverObject 操作分为“仅调用 DriverUnload”和“显式确认后的实验强拆”两档。
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
    QAction* stopServiceAction = contextMenu.addAction(
        QIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("停止驱动服务（SCM 安全路径）"));
    stopServiceAction->setToolTip(QStringLiteral("通过服务控制管理器发送 SERVICE_CONTROL_STOP；不直接调用 DriverObject->DriverUnload。"));
    QAction* forceUnloadAction = contextMenu.addAction(
        QIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("R0 强制卸载 DriverObject"));
    forceUnloadAction->setToolTip(QStringLiteral("仅调用 DriverObject->DriverUnload，不清 dispatch/unload/device；高危后处理需二次确认。"));
    QAction* forceDestructiveUnloadAction = contextMenu.addAction(
        QIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("R0 实验性破坏强拆 DriverObject"));
    forceDestructiveUnloadAction->setToolTip(QStringLiteral("允许中和 DriverObject；目标无 DriverUnload 时才删 DeviceObject。仅用于恶意驱动且可能蓝屏。"));

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
    if (selectedAction == stopServiceAction)
    {
        stopDriverServiceFromServiceRow(selectedRows.front().row());
        return;
    }
    if (selectedAction == forceUnloadAction)
    {
        forceUnloadDriverFromServiceRow(selectedRows.front().row(), false);
        return;
    }
    if (selectedAction == forceDestructiveUnloadAction)
    {
        const QMessageBox::StandardButton confirmResult = QMessageBox::warning(
            this,
            QStringLiteral("R0 实验性破坏强拆"),
            QStringLiteral("该操作会绕过 SCM/PnP 生命周期，允许 R0 中和目标 DriverObject；目标缺少 DriverUnload 时还会尝试删除 DeviceObject 并将 DriverObject 临时化。\n\n这可能立即导致蓝屏，仅建议用于确认恶意驱动且普通强制卸载无效的场景。\n\n是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirmResult == QMessageBox::Yes)
        {
            forceUnloadDriverFromServiceRow(selectedRows.front().row(), true);
        }
        return;
    }
}

void DriverDock::stopDriverServiceFromServiceRow(const int rowIndex)
{
    // 服务列表停驱流程：
    // - 输入：服务表格行号。
    // - 处理：读取 SCM 服务短名，在后台线程调用 ControlService(SERVICE_CONTROL_STOP)；
    // - 返回：无返回值；结果通过操作日志和刷新后的服务/模块表体现。
    // 注意：这里刻意不调用 R0 DriverObject 强卸载。直接调用第三方 DriverUnload
    // 或清理 DriverObject/DeviceObject 会绕过 SCM/PnP 生命周期，目标仍在处理 IRP
    // 时容易导致 bugcheck。
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
        appendOperateLogLine(QStringLiteral("停止服务失败：服务名为空。"));
        return;
    }

    appendOperateLogLine(QStringLiteral("开始停止驱动服务（SCM）：%1").arg(serviceNameText));

    QPointer<DriverDock> guardThis(this);
    const std::wstring serviceNameWide = toWideString(serviceNameText);
    auto* stopTask = QRunnable::create([guardThis, serviceNameText, serviceNameWide]()
        {
            ks::service::ServiceStatus finalStatus{};
            std::string errorText;
            std::uint32_t errorCode = 0U;
            const bool stopOk = ks::service::StopServiceByName(
                serviceNameWide,
                10000U,
                SERVICE_STOPPED,
                &finalStatus,
                &errorText,
                &errorCode);

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, serviceNameText, stopOk, finalStatus, errorText, errorCode]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }

                    if (stopOk)
                    {
                        guardThis->appendOperateLogLine(
                            finalStatus.currentState == SERVICE_STOPPED
                            ? QStringLiteral("停止服务成功：service=%1").arg(serviceNameText)
                            : QStringLiteral("停止服务结束：service=%1，当前状态=%2")
                            .arg(serviceNameText)
                            .arg(guardThis->serviceStateToText(finalStatus.currentState)));
                    }
                    else
                    {
                        guardThis->appendOperateLogLine(
                            QStringLiteral("停止服务失败：service=%1，error=%2，detail=%3")
                            .arg(serviceNameText)
                            .arg(errorCode)
                            .arg(QString::fromUtf8(errorText.c_str())));
                    }
                    guardThis->refreshDriverServiceRecords();
                    guardThis->refreshLoadedKernelModuleRecords();
                },
                Qt::QueuedConnection);
        });
    stopTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(stopTask);
}

void DriverDock::forceUnloadDriverFromServiceRow(const int rowIndex, const bool destructiveCleanup)
{
    // 强制卸载流程：
    // - 使用服务名推导 \Driver\ServiceName；
    // - R0 内部再通过 ObReferenceObjectByName 引用对象；
    // - 默认只调用 DriverUnload，不再清 unload/dispatch，避免失败后把真实驱动留在半清理状态；
    // - 只有 destructiveCleanup 为 true 时，才允许持久中和 DriverObject、删 DeviceObject 和临时化对象。
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
    auto* unloadTask = QRunnable::create([guardThis, driverObjectNameText, driverObjectNameWide, destructiveCleanup]()
        {
            unsigned long cleanupFlags = 0UL;
            if (destructiveCleanup)
            {
                cleanupFlags |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT;
            }
            const ksword::ark::DriverForceUnloadResult result =
                ksword::ark::DriverClient().forceUnloadDriver(
                    driverObjectNameWide,
                    cleanupFlags,
                    3000UL);

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, driverObjectNameText, destructiveCleanup, result]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }

                    const QString resultLine = QStringLiteral(
                        "R0 强制卸载完成：%1 | IO=%2 | Status=%3 | Flags=%4 | Applied=%5 | Deleted=%6 | Last=%7 | Wait=%8 | Object=%9 | Unload=%10 | Name=%11")
                        .arg(driverObjectNameText)
                        .arg(QString::fromStdString(result.io.message))
                        .arg(driverForceUnloadStatusText(result.status))
                        .arg(formatHex32(result.flags))
                        .arg(formatHex32(result.cleanupFlagsApplied))
                        .arg(result.deletedDeviceCount)
                        .arg(formatNtStatusText(result.lastStatus))
                        .arg(formatNtStatusText(result.waitStatus))
                        .arg(formatCompactAddress(result.driverObjectAddress))
                        .arg(formatCompactAddress(result.driverUnloadAddress))
                        .arg(QString::fromStdWString(result.driverName));
                    guardThis->appendOperateLogLine(resultLine);
                    if (destructiveCleanup)
                    {
                        guardThis->appendOperateLogLine(QStringLiteral("已请求实验性高危后处理：允许 DriverObject 中和 / 删 DeviceObject / 临时对象。"));
                    }
                    guardThis->refreshDriverServiceRecords();
                    guardThis->refreshLoadedKernelModuleRecords();
                },
                Qt::QueuedConnection);
        });
    unloadTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(unloadTask);
}

void DriverDock::showModuleTableContextMenu(const QPoint& localPosition)
{
    // 模块表右键入口：
    // - 服务已停止但模块仍残留时，服务名路径可能已经失效；
    // - 这里改用模块基址，让 R0 扫描对象目录反查 DriverObject。
    if (m_moduleTable == nullptr)
    {
        return;
    }

    const QModelIndex clickedIndex = m_moduleTable->indexAt(localPosition);
    if (clickedIndex.isValid())
    {
        m_moduleTable->selectRow(clickedIndex.row());
    }

    const QModelIndexList selectedRows =
        (m_moduleTable->selectionModel() == nullptr)
        ? QModelIndexList()
        : m_moduleTable->selectionModel()->selectedRows(0);
    if (selectedRows.isEmpty())
    {
        return;
    }

    QMenu contextMenu(this);
    contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* refreshEvidenceAction = contextMenu.addAction(
        QIcon(":/Icon/process_refresh.svg"),
        QStringLiteral("刷新模块证据聚合"));
    QAction* copyEvidenceAction = contextMenu.addAction(
        QIcon(":/Icon/process_copy_row.svg"),
        QStringLiteral("复制当前模块证据详情"));
    contextMenu.addSeparator();
    QAction* forceCleanupByBaseAction = contextMenu.addAction(
        QIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("R0 按模块基址强制卸载 DriverObject"));
    forceCleanupByBaseAction->setToolTip(
        QStringLiteral("按模块基址反查 DriverObject，仅调用 DriverUnload；高危后处理需二次确认。"));
    QAction* forceDeepCleanupByBaseAction = contextMenu.addAction(
        QIcon(":/Icon/process_uncritical.svg"),
        QStringLiteral("R0 强力清理模块回调 + DriverObject"));
    forceDeepCleanupByBaseAction->setToolTip(
        QStringLiteral("DriverObject 处理成功后再移除可验证回调；该路径有明显系统不稳定风险。"));

    QAction* selectedAction = contextMenu.exec(m_moduleTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == refreshEvidenceAction)
    {
        refreshLoadedModuleEvidenceAsync();
        return;
    }
    if (selectedAction == copyEvidenceAction)
    {
        showSelectedModuleEvidenceDetail();
        if (m_moduleEvidenceDetailEditor != nullptr && QGuiApplication::clipboard() != nullptr)
        {
            QGuiApplication::clipboard()->setText(m_moduleEvidenceDetailEditor->text());
        }
        return;
    }
    if (selectedAction == forceCleanupByBaseAction)
    {
        // 普通模块基址清理不需要二次确认，直接复用当前选中行。
        forceUnloadDriverFromModuleRow(selectedRows.front().row(), false, false);
        return;
    }
    if (selectedAction == forceDeepCleanupByBaseAction)
    {
        // 强力清理是高风险动作，因此保留二次确认：
        // - 全局 QMessageBox 主题器已不再拦截 Close 事件；
        // - 这里可以恢复使用标准按钮返回值，避免业务层绕过全局弹窗语义。
        const QMessageBox::StandardButton confirmResult = QMessageBox::warning(
            this,
            QStringLiteral("R0 强力清理"),
            QStringLiteral("该操作会按模块基址清理 DriverObject；仅在 DriverObject 处理成功后，才继续移除进程/线程/镜像/Minifilter/WFP 等可验证回调。\n\n不会摘 PsLoadedModuleList，但目标驱动若正在处理请求仍可能导致系统不稳定。\n\n是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirmResult == QMessageBox::Yes)
        {
            forceUnloadDriverFromModuleRow(selectedRows.front().row(), true, true);
        }
        return;
    }
}

void DriverDock::forceUnloadDriverFromModuleRow(
    const int rowIndex,
    const bool removeCallbacksFirst,
    const bool destructiveCleanup)
{
    // 按模块基址清理：
    // - R3 只传模块基址和模块名兜底文本；
    // - R0 先按 DriverStart 反查真实 DriverObject，再执行分级强制卸载；
    // - removeCallbacksFirst 为 true 时请求 R0 在 DriverObject 处理成功后移除可验证回调；
    // - destructiveCleanup 为 false 时只调用 DriverUnload，不改写 dispatch/unload/device；
    // - destructiveCleanup 为 true 时才允许持久中和 DriverObject、删设备对象和临时化对象。
    if (m_moduleTable == nullptr || rowIndex < 0 || rowIndex >= m_moduleTable->rowCount())
    {
        return;
    }

    QTableWidgetItem* moduleNameItem = m_moduleTable->item(rowIndex, 0);
    QTableWidgetItem* moduleBaseItem = m_moduleTable->item(rowIndex, 1);
    if (moduleNameItem == nullptr || moduleBaseItem == nullptr)
    {
        return;
    }

    const QString moduleNameText = moduleNameItem->text().trimmed();
    const std::uint64_t moduleBaseValue = moduleBaseItem->data(Qt::UserRole).toULongLong();
    if (moduleBaseValue == 0U)
    {
        appendOperateLogLine(QStringLiteral("模块基址清理失败：模块基址为空。"));
        return;
    }

    QString fallbackNameText = moduleNameText;
    if (fallbackNameText.endsWith(QStringLiteral(".sys"), Qt::CaseInsensitive))
    {
        fallbackNameText.chop(4);
    }

    appendOperateLogLine(QStringLiteral("开始 R0 按模块基址%1：%2 | base=%3")
        .arg(removeCallbacksFirst ? QStringLiteral("强力清理回调+DriverObject") : QStringLiteral("强制卸载 DriverObject"))
        .arg(moduleNameText, formatCompactAddress(moduleBaseValue)));

    QPointer<DriverDock> guardThis(this);
    const std::wstring fallbackNameWide = fallbackNameText.toStdWString();
    auto* unloadTask = QRunnable::create([guardThis, moduleNameText, moduleBaseValue, fallbackNameWide, removeCallbacksFirst, destructiveCleanup]()
        {
            unsigned long cleanupFlags =
                KSWORD_ARK_DRIVER_UNLOAD_FLAG_TARGET_MODULE_BASE_PRESENT;
            if (destructiveCleanup)
            {
                cleanupFlags |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_ALLOW_DESTRUCTIVE_CLEANUP |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD |
                    KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT;
            }
            if (removeCallbacksFirst)
            {
                cleanupFlags |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_REMOVE_CALLBACKS_BY_MODULE_BASE;
            }
            const ksword::ark::DriverForceUnloadResult result =
                ksword::ark::DriverClient().forceUnloadDriverByModuleBase(
                    moduleBaseValue,
                    fallbackNameWide,
                    cleanupFlags,
                    3000UL);

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, moduleNameText, moduleBaseValue, removeCallbacksFirst, destructiveCleanup, result]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }

                    const QString resultLine = QStringLiteral(
                        "R0 模块基址%1完成：%2 | Base=%3 | IO=%4 | Status=%5 | Flags=%6 | Applied=%7 | Deleted=%8 | Last=%9 | Wait=%10 | Object=%11 | Unload=%12 | Callbacks=%13/%14 fail=%15 last=%16 | Name=%17")
                        .arg(removeCallbacksFirst ? QStringLiteral("强力清理") : QStringLiteral("清理"))
                        .arg(moduleNameText)
                        .arg(formatCompactAddress(moduleBaseValue))
                        .arg(QString::fromStdString(result.io.message))
                        .arg(driverForceUnloadStatusText(result.status))
                        .arg(formatHex32(result.flags))
                        .arg(formatHex32(result.cleanupFlagsApplied))
                        .arg(result.deletedDeviceCount)
                        .arg(formatNtStatusText(result.lastStatus))
                        .arg(formatNtStatusText(result.waitStatus))
                        .arg(formatCompactAddress(result.driverObjectAddress))
                        .arg(formatCompactAddress(result.driverUnloadAddress))
                        .arg(result.callbacksRemoved)
                        .arg(result.callbackCandidates)
                        .arg(result.callbackFailures)
                        .arg(formatNtStatusText(result.callbackLastStatus))
                        .arg(QString::fromStdWString(result.driverName));
                    guardThis->appendOperateLogLine(resultLine);
                    if (destructiveCleanup)
                    {
                        guardThis->appendOperateLogLine(QStringLiteral("已请求实验性高危后处理：允许 DriverObject 中和 / 删 DeviceObject / 临时对象。"));
                    }
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
    for (std::size_t sourceIndex = 0U; sourceIndex < m_loadedModuleCache.size(); ++sourceIndex)
    {
        const LoadedKernelModuleRecord& moduleRecord = m_loadedModuleCache[sourceIndex];
        const int rowIndex = m_moduleTable->rowCount();
        m_moduleTable->insertRow(rowIndex);
        QTableWidgetItem* moduleNameItem = createReadOnlyItem(moduleRecord.moduleName);
        moduleNameItem->setData(
            ModuleRecordIndexRole,
            QVariant::fromValue<qulonglong>(static_cast<qulonglong>(sourceIndex)));
        m_moduleTable->setItem(rowIndex, 0, moduleNameItem);
        QTableWidgetItem* baseItem = createReadOnlyItem(formatAddress(moduleRecord.baseAddress));
        baseItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(
            static_cast<qulonglong>(moduleRecord.baseAddress)));
        m_moduleTable->setItem(rowIndex, 1, baseItem);
        for (int evidenceColumn = 2; evidenceColumn <= 7; ++evidenceColumn)
        {
            m_moduleTable->setItem(rowIndex, evidenceColumn, createReadOnlyItem(QStringLiteral("待扫描")));
        }
        QTableWidgetItem* pathItem = createReadOnlyItem(moduleRecord.imagePath);
        pathItem->setToolTip(moduleRecord.imagePath);
        m_moduleTable->setItem(rowIndex, 8, pathItem);
    }
    if (m_moduleTable->rowCount() > 0)
    {
        m_moduleTable->setCurrentCell(0, 0);
    }
    rebuildLoadedModuleEvidenceViews();
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

    // UI adapter only: the reusable service layer owns SCM open/query/close details.
    ks::service::ServiceStatus statusInfo;
    std::string errorText;
    std::uint32_t errorCode = 0;
    if (!ks::service::QueryServiceStatus(
        toWideString(serviceNameText),
        &statusInfo,
        &errorText,
        &errorCode))
    {
        appendOperateLogLine(QStringLiteral("查询失败：%1").arg(QString::fromUtf8(errorText.c_str())));
        warn << queryEvent
            << "[DriverDock] 查询状态失败, service="
            << serviceNameText.toStdString()
            << ", error=" << errorCode
            << ", detail=" << errorText
            << eol;
        return;
    }

    appendOperateLogLine(
        QStringLiteral("服务 %1 当前状态：%2")
        .arg(serviceNameText)
        .arg(serviceStateToText(statusInfo.currentState)));

    info << queryEvent
        << "[DriverDock] 查询状态成功, service=" << serviceNameText.toStdString()
        << ", state=" << statusInfo.currentState
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

    ks::service::KernelDriverServiceConfig serviceConfig;
    serviceConfig.serviceName = toWideString(serviceNameText);
    serviceConfig.displayName = toWideString(displayNameText);
    serviceConfig.description = toWideString(descriptionText);
    serviceConfig.binaryPath = toWideString(binaryPathText);
    serviceConfig.startType = static_cast<std::uint32_t>(m_startTypeCombo->currentData().toInt());
    serviceConfig.errorControl = static_cast<std::uint32_t>(m_errorControlCombo->currentData().toInt());

    bool created = false;
    std::string errorText;
    std::uint32_t errorCode = 0;
    if (!ks::service::CreateOrUpdateKernelDriverService(
        serviceConfig,
        &created,
        &errorText,
        &errorCode))
    {
        appendOperateLogLine(QStringLiteral("注册/更新失败：%1").arg(QString::fromUtf8(errorText.c_str())));
        err << operationEvent
            << "[DriverDock] 注册/更新失败, service=" << serviceNameText.toStdString()
            << ", error=" << errorCode
            << ", detail=" << errorText
            << eol;
        return;
    }

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

    ks::service::ServiceStatus finalStatus;
    std::string errorText;
    std::uint32_t errorCode = 0;
    if (!ks::service::StartServiceByName(
        toWideString(serviceNameText),
        6000,
        SERVICE_RUNNING,
        &finalStatus,
        &errorText,
        &errorCode))
    {
        if (isDriverSignatureLoadError(static_cast<DWORD>(errorCode)))
        {
            const QString binaryPathText =
                (m_binaryPathEdit == nullptr) ? QString() : m_binaryPathEdit->text().trimmed();
            const QString adviceText =
                buildDriverSignatureLoadAdvice(static_cast<DWORD>(errorCode), serviceNameText, binaryPathText);
            appendOperateLogLine(adviceText);
            err << operationEvent
                << "[DriverDock] 挂载失败：驱动签名/镜像校验失败, service="
                << serviceNameText.toStdString()
                << ", error=" << errorCode
                << ", path=" << binaryPathText.toStdString()
                << eol;
            return;
        }

        appendOperateLogLine(QStringLiteral("挂载失败：%1").arg(QString::fromUtf8(errorText.c_str())));
        err << operationEvent
            << "[DriverDock] 挂载失败, service=" << serviceNameText.toStdString()
            << ", error=" << errorCode
            << ", detail=" << errorText
            << eol;
        return;
    }

    appendOperateLogLine(finalStatus.currentState == SERVICE_RUNNING
        ? QStringLiteral("挂载成功：service=%1").arg(serviceNameText)
        : QStringLiteral("挂载结束：当前状态=%1").arg(serviceStateToText(finalStatus.currentState)));

    info << operationEvent
        << "[DriverDock] 挂载执行完成, service=" << serviceNameText.toStdString()
        << ", finalState=" << finalStatus.currentState
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

    ks::service::ServiceStatus finalStatus;
    std::string errorText;
    std::uint32_t errorCode = 0;
    if (!ks::service::StopServiceByName(
        toWideString(serviceNameText),
        6000,
        SERVICE_STOPPED,
        &finalStatus,
        &errorText,
        &errorCode))
    {
        appendOperateLogLine(QStringLiteral("卸载失败：%1").arg(QString::fromUtf8(errorText.c_str())));
        err << operationEvent
            << "[DriverDock] 卸载失败, service=" << serviceNameText.toStdString()
            << ", error=" << errorCode
            << ", detail=" << errorText
            << eol;
        return;
    }

    appendOperateLogLine(finalStatus.currentState == SERVICE_STOPPED
        ? QStringLiteral("卸载成功：service=%1").arg(serviceNameText)
        : QStringLiteral("卸载结束：当前状态=%1").arg(serviceStateToText(finalStatus.currentState)));

    info << operationEvent
        << "[DriverDock] 卸载执行完成, service=" << serviceNameText.toStdString()
        << ", finalState=" << finalStatus.currentState
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

    std::string errorText;
    std::uint32_t errorCode = 0;
    if (!ks::service::DeleteServiceByName(
        toWideString(serviceNameText),
        true,
        4000,
        &errorText,
        &errorCode))
    {
        appendOperateLogLine(QStringLiteral("删除失败：%1").arg(QString::fromUtf8(errorText.c_str())));
        err << operationEvent
            << "[DriverDock] 删除失败, service=" << serviceNameText.toStdString()
            << ", error=" << errorCode
            << ", detail=" << errorText
            << eol;
        return;
    }

    appendOperateLogLine(QStringLiteral("删除成功（或已标记删除）：service=%1").arg(serviceNameText));
    info << operationEvent << "[DriverDock] 删除执行完成, service=" << serviceNameText.toStdString() << eol;
    refreshDriverServiceRecords();
    refreshLoadedKernelModuleRecords();
}
