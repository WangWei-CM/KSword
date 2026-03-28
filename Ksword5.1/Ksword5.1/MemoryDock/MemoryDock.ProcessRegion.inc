// ============================================================
// MemoryDock.ProcessRegion.inc
// 作用：
// - 负责进程列表、模块列表、内存区域枚举与过滤展示。
// - 聚焦“附加目标与地址空间概览”能力。
// ============================================================

void MemoryDock::refreshProcessList(const bool keepSelection)
{
    // 输出刷新入口日志，记录是否尝试保留上次选择。
    kLogEvent refreshProcessStartEvent;
    info << refreshProcessStartEvent
        << "[MemoryDock] refreshProcessList: 开始刷新进程列表, keepSelection="
        << (keepSelection ? "true" : "false")
        << eol;

    // 刷新前记录当前选择 PID，刷新后尽量恢复体验。
    std::uint32_t previousPid = 0;
    if (keepSelection)
    {
        const int index = m_processCombo->currentIndex();
        if (index >= 0)
        {
            previousPid = static_cast<std::uint32_t>(m_processCombo->itemData(index, Qt::UserRole).toUInt());
        }
    }

    m_processCache.clear();

    // 进程枚举按需求使用 Toolhelp 快照接口。
    HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshotHandle == INVALID_HANDLE_VALUE)
    {
        kLogEvent snapshotFailEvent;
        err << snapshotFailEvent
            << "[MemoryDock] refreshProcessList: CreateToolhelp32Snapshot 失败, error="
            << ::GetLastError()
            << eol;
        QMessageBox::warning(this, "进程刷新", "CreateToolhelp32Snapshot 失败。");
        return;
    }

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);
    if (::Process32FirstW(snapshotHandle, &processEntry) == FALSE)
    {
        kLogEvent processFirstFailEvent;
        err << processFirstFailEvent
            << "[MemoryDock] refreshProcessList: Process32FirstW 失败, error="
            << ::GetLastError()
            << eol;
        ::CloseHandle(snapshotHandle);
        return;
    }

    do
    {
        ProcessEntry entry{};
        entry.pid = static_cast<std::uint32_t>(processEntry.th32ProcessID);
        entry.processName = QString::fromWCharArray(processEntry.szExeFile);
        entry.cpuPercent = 0.0; // CPU 为可选字段，当前版本保留 0。

        DWORD sessionId = 0;
        if (::ProcessIdToSessionId(toDwordPid(entry.pid), &sessionId) != FALSE)
        {
            entry.sessionId = static_cast<std::uint32_t>(sessionId);
        }

        // 尝试读取工作集大小：失败则保持 0，不影响主流程。
        HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE,
            toDwordPid(entry.pid));
        if (processHandle != nullptr)
        {
            PROCESS_MEMORY_COUNTERS_EX memoryCounter{};
            if (::GetProcessMemoryInfo(
                processHandle,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryCounter),
                sizeof(memoryCounter)) != FALSE)
            {
                entry.workingSetMB = static_cast<double>(memoryCounter.WorkingSetSize) / (1024.0 * 1024.0);
            }
            ::CloseHandle(processHandle);
        }
        m_processCache.push_back(std::move(entry));
    } while (::Process32NextW(snapshotHandle, &processEntry) != FALSE);
    ::CloseHandle(snapshotHandle);

    // 为了稳定展示顺序，这里按 PID 升序排序。
    std::sort(m_processCache.begin(), m_processCache.end(), [](const ProcessEntry& left, const ProcessEntry& right) {
        return left.pid < right.pid;
        });

    // 先重建进程表。
    m_processTable->setSortingEnabled(false);
    m_processTable->setRowCount(static_cast<int>(m_processCache.size()));
    static QHash<QString, QIcon> processIconCache;
    for (int row = 0; row < static_cast<int>(m_processCache.size()); ++row)
    {
        const ProcessEntry& entry = m_processCache[static_cast<std::size_t>(row)];

        // 进程名前附加图标：路径可得时使用 EXE 图标，否则回退默认图标。
        const QString imagePath = QString::fromStdString(ks::process::QueryProcessPathByPid(entry.pid));
        QTableWidgetItem* processNameItem = new QTableWidgetItem(entry.processName);
        processNameItem->setIcon(resolveIconByPath(imagePath, processIconCache));
        m_processTable->setItem(row, 0, processNameItem);

        m_processTable->setItem(row, 1, new QTableWidgetItem(QString::number(entry.pid)));
        m_processTable->setItem(row, 2, new QTableWidgetItem(QString::number(entry.sessionId)));
        m_processTable->setItem(row, 3, new QTableWidgetItem(QString::number(entry.cpuPercent, 'f', 2) + "%"));
        m_processTable->setItem(row, 4, new QTableWidgetItem(QString("%1 MB").arg(entry.workingSetMB, 0, 'f', 1)));
    }
    m_processTable->setSortingEnabled(true);

    // 再同步重建顶部下拉框。
    updateProcessComboFromCache();

    // 尝试恢复之前选中 PID。
    if (previousPid != 0)
    {
        for (int comboIndex = 0; comboIndex < m_processCombo->count(); ++comboIndex)
        {
            if (m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt() == previousPid)
            {
                m_processCombo->setCurrentIndex(comboIndex);
                break;
            }
        }
    }

    // 输出刷新结束日志，记录本轮进程总数与恢复 PID。
    kLogEvent refreshProcessFinishEvent;
    info << refreshProcessFinishEvent
        << "[MemoryDock] refreshProcessList: 刷新完成, processCount="
        << m_processCache.size()
        << ", previousPid="
        << previousPid
        << eol;
}

void MemoryDock::updateProcessComboFromCache()
{
    // 下拉框重建入口日志：记录缓存规模。
    kLogEvent comboUpdateEvent;
    dbg << comboUpdateEvent
        << "[MemoryDock] updateProcessComboFromCache: 重建下拉框, cacheSize="
        << m_processCache.size()
        << eol;

    // 下拉框重建期间阻断信号，防止反复触发模块刷新。
    QSignalBlocker blocker(m_processCombo);
    m_processCombo->clear();

    for (const ProcessEntry& entry : m_processCache)
    {
        const QString text = QString("%1 [PID:%2]").arg(entry.processName).arg(entry.pid);
        m_processCombo->addItem(text, QVariant::fromValue(static_cast<uint>(entry.pid)));
        const int row = m_processCombo->count() - 1;
        m_processCombo->setItemData(row, entry.processName, Qt::UserRole + 1);
    }

    // 当前附加 PID 还存在时，默认选中它。
    if (m_attachedPid != 0)
    {
        for (int comboIndex = 0; comboIndex < m_processCombo->count(); ++comboIndex)
        {
            if (m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt() == m_attachedPid)
            {
                m_processCombo->setCurrentIndex(comboIndex);
                return;
            }
        }
    }
    if (m_processCombo->count() > 0)
    {
        m_processCombo->setCurrentIndex(0);
    }
}

void MemoryDock::rebuildModuleTableFromCache()
{
    // 仅用缓存重绘模块表：该路径不会触发 Win32 枚举，适合输入过滤时高频调用。
    const QString filterText = (m_moduleFilterEdit == nullptr) ? QString() : m_moduleFilterEdit->text().trimmed();
    std::vector<const ModuleEntry*> filteredModules;
    filteredModules.reserve(m_moduleCache.size());
    for (const ModuleEntry& entry : m_moduleCache)
    {
        if (!filterText.isEmpty() &&
            !entry.moduleName.contains(filterText, Qt::CaseInsensitive) &&
            !entry.fullPath.contains(filterText, Qt::CaseInsensitive))
        {
            continue;
        }
        filteredModules.push_back(&entry);
    }

    // 批量更新期间关闭绘制和信号，减少重排开销并避免多余回调。
    m_moduleTable->setUpdatesEnabled(false);
    const QSignalBlocker tableBlocker(m_moduleTable);
    m_moduleTable->clear();

    // 图标按路径缓存，避免重建过滤时重复触发磁盘图标查询。
    static QHash<QString, QIcon> moduleIconCache;
    for (const ModuleEntry* moduleEntryPtr : filteredModules)
    {
        const ModuleEntry& entry = *moduleEntryPtr;
        QTreeWidgetItem* rowItem = new QTreeWidgetItem();
        rowItem->setText(toModuleTreeColumnIndex(ModuleTreeColumn::Path), entry.fullPath);
        rowItem->setText(toModuleTreeColumnIndex(ModuleTreeColumn::Size), formatSize(entry.sizeBytes));
        rowItem->setText(toModuleTreeColumnIndex(ModuleTreeColumn::Signature), entry.signatureState);
        rowItem->setText(
            toModuleTreeColumnIndex(ModuleTreeColumn::EntryOffset),
            QString("0x%1").arg(entry.entryPointOffset, 0, 16).toUpper());
        rowItem->setText(toModuleTreeColumnIndex(ModuleTreeColumn::State), entry.runningState);
        rowItem->setText(toModuleTreeColumnIndex(ModuleTreeColumn::ThreadId), entry.threadIdText);
        rowItem->setIcon(
            toModuleTreeColumnIndex(ModuleTreeColumn::Path),
            resolveIconByPath(entry.fullPath, moduleIconCache));

        // 保存基址与线程 ID，供双击跳转和右键动作反查。
        rowItem->setData(
            toModuleTreeColumnIndex(ModuleTreeColumn::Path),
            Qt::UserRole,
            QVariant::fromValue<qulonglong>(entry.baseAddress));
        rowItem->setData(
            toModuleTreeColumnIndex(ModuleTreeColumn::Path),
            Qt::UserRole + 1,
            QVariant::fromValue(entry.representativeThreadId));

        // 签名列颜色策略：可信绿、未知灰、不可信红。
        if (entry.signatureTrusted)
        {
            rowItem->setForeground(toModuleTreeColumnIndex(ModuleTreeColumn::Signature), QColor(34, 139, 34));
        }
        else if (entry.signatureState.compare("Pending", Qt::CaseInsensitive) == 0 ||
            entry.signatureState.compare("Unknown", Qt::CaseInsensitive) == 0)
        {
            rowItem->setForeground(toModuleTreeColumnIndex(ModuleTreeColumn::Signature), QColor(120, 120, 120));
        }
        else
        {
            rowItem->setForeground(toModuleTreeColumnIndex(ModuleTreeColumn::Signature), QColor(220, 50, 47));
        }

        m_moduleTable->addTopLevelItem(rowItem);
    }

    // 排序仍按路径列执行，和原先交互习惯保持一致。
    m_moduleTable->sortItems(toModuleTreeColumnIndex(ModuleTreeColumn::Path), Qt::AscendingOrder);
    m_moduleTable->setUpdatesEnabled(true);

    // 重绘结束日志：用于确认过滤后可见数量。
    kLogEvent rebuildModuleTableEvent;
    dbg << rebuildModuleTableEvent
        << "[MemoryDock] rebuildModuleTableFromCache: 完成, cacheCount="
        << m_moduleCache.size()
        << ", visibleCount="
        << filteredModules.size()
        << ", filterText="
        << filterText.toStdString()
        << eol;
}

bool MemoryDock::refreshModuleListForPid(const std::uint32_t pid)
{
    // 模块刷新改为异步，避免签名校验和模块枚举阻塞主线程。
    kLogEvent refreshModuleStartEvent;
    info << refreshModuleStartEvent
        << "[MemoryDock] refreshModuleListForPid: 请求刷新模块, pid="
        << pid
        << eol;

    if (pid == 0)
    {
        // pid 为 0 表示当前无有效目标，直接清空缓存和表格。
        m_moduleCache.clear();
        rebuildModuleTableFromCache();
        if (m_moduleStatusLabel != nullptr)
        {
            m_moduleStatusLabel->setText("● 未选择有效进程");
            m_moduleStatusLabel->setStyleSheet("color:#DC322F; font-weight:700;");
        }
        return false;
    }

    // 每次请求生成一个递增 ticket；旧任务回调时会被丢弃。
    const std::uint64_t refreshTicket = m_moduleRefreshTicket.fetch_add(1) + 1;
    const bool includeSignatureCheck =
        (m_moduleSignatureCheck != nullptr) && m_moduleSignatureCheck->isChecked();
    m_moduleRefreshInProgress.store(true);

    // 刷新期间更新状态栏并临时禁用按钮，避免用户误以为点击无效而反复触发。
    if (m_moduleRefreshButton != nullptr)
    {
        m_moduleRefreshButton->setEnabled(false);
    }
    if (m_moduleStatusLabel != nullptr)
    {
        m_moduleStatusLabel->setText(
            QString("● 正在刷新模块(PID=%1, 签名校验=%2)...")
            .arg(pid)
            .arg(includeSignatureCheck ? "开启" : "关闭"));
        m_moduleStatusLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }

    // 使用 QPointer 守护 this，避免窗口销毁后异步回调访问悬空对象。
    const QPointer<MemoryDock> selfGuard(this);
    std::thread([selfGuard, pid, includeSignatureCheck, refreshTicket]() {
        if (selfGuard == nullptr)
        {
            return;
        }

        // 后台线程仅执行耗时枚举和数据转换，不直接操作任何 Qt 控件。
        const auto refreshStartTime = std::chrono::steady_clock::now();
        const ks::process::ProcessModuleSnapshot moduleSnapshot =
            ks::process::EnumerateProcessModulesAndThreads(pid, includeSignatureCheck);

        std::vector<ModuleEntry> moduleCache;
        moduleCache.reserve(moduleSnapshot.modules.size());
        for (const ks::process::ProcessModuleRecord& moduleRecord : moduleSnapshot.modules)
        {
            ModuleEntry entry{};
            entry.fullPath = QString::fromStdString(moduleRecord.modulePath);
            entry.moduleName = QFileInfo(entry.fullPath).fileName();
            entry.baseAddress = moduleRecord.moduleBaseAddress;
            entry.sizeBytes = moduleRecord.moduleSizeBytes;
            entry.signatureState = QString::fromStdString(moduleRecord.signatureState);
            entry.signatureTrusted = moduleRecord.signatureTrusted;
            entry.entryPointOffset = moduleRecord.entryPointRva;
            entry.runningState = QString::fromStdString(moduleRecord.runningState);
            entry.threadIdText = QString::fromStdString(moduleRecord.threadIdText);
            entry.representativeThreadId = moduleRecord.representativeThreadId;
            moduleCache.push_back(std::move(entry));
        }
        std::sort(moduleCache.begin(), moduleCache.end(), [](const ModuleEntry& left, const ModuleEntry& right) {
            return left.baseAddress < right.baseAddress;
            });

        const std::uint64_t elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - refreshStartTime).count());
        const std::size_t moduleCount = moduleSnapshot.modules.size();
        const std::size_t threadCount = moduleSnapshot.threads.size();
        const QString diagnosticText = QString::fromStdString(moduleSnapshot.diagnosticText).trimmed();

        // 结果回到主线程落地，确保 Qt 视图更新线程安全。
        QMetaObject::invokeMethod(selfGuard.data(), [selfGuard,
            pid,
            includeSignatureCheck,
            refreshTicket,
            elapsedMs,
            moduleCount,
            threadCount,
            diagnosticText,
            moduleCache = std::move(moduleCache)]() mutable {
                if (selfGuard == nullptr)
                {
                    return;
                }

                // 如果 ticket 已过期，说明用户又发起了新刷新，旧结果直接丢弃。
                if (refreshTicket < selfGuard->m_moduleRefreshTicket.load())
                {
                    kLogEvent staleModuleEvent;
                    dbg << staleModuleEvent
                        << "[MemoryDock] refreshModuleListForPid: 丢弃过期模块结果, ticket="
                        << refreshTicket
                        << ", latestTicket="
                        << selfGuard->m_moduleRefreshTicket.load()
                        << eol;
                    return;
                }

                // 先覆盖缓存，再按当前过滤条件重建可见列表。
                selfGuard->m_moduleCache = std::move(moduleCache);
                selfGuard->rebuildModuleTableFromCache();

                // 刷新收尾：恢复按钮并更新状态标签。
                selfGuard->m_moduleRefreshInProgress.store(false);
                if (selfGuard->m_moduleRefreshButton != nullptr)
                {
                    selfGuard->m_moduleRefreshButton->setEnabled(true);
                }

                if (selfGuard->m_moduleStatusLabel != nullptr)
                {
                    QString statusText = QString("● %1 ms | 模块:%2 线程:%3 显示:%4")
                        .arg(elapsedMs)
                        .arg(moduleCount)
                        .arg(threadCount)
                        .arg(selfGuard->m_moduleTable->topLevelItemCount());
                    if (!diagnosticText.isEmpty())
                    {
                        statusText += QString(" | %1").arg(diagnosticText);
                    }
                    selfGuard->m_moduleStatusLabel->setText(statusText);
                    selfGuard->m_moduleStatusLabel->setStyleSheet(moduleCount == 0
                        ? "color:#DC322F; font-weight:700;"
                        : "color:#2F7D32; font-weight:600;");
                }

                // 刷新完成日志：包含耗时、总数、过滤后数量和诊断文本。
                kLogEvent refreshModuleFinishEvent;
                info << refreshModuleFinishEvent
                    << "[MemoryDock] refreshModuleListForPid: 刷新完成, pid="
                    << pid
                    << ", elapsedMs="
                    << elapsedMs
                    << ", moduleCount="
                    << moduleCount
                    << ", threadCount="
                    << threadCount
                    << ", visibleCount="
                    << selfGuard->m_moduleTable->topLevelItemCount()
                    << ", includeSignatureCheck="
                    << (includeSignatureCheck ? "true" : "false")
                    << ", diagnostic="
                    << diagnosticText.toStdString()
                    << eol;
            }, Qt::QueuedConnection);
        }).detach();

    return true;
}

bool MemoryDock::attachToProcess(
    const std::uint32_t pid,
    const QString& processName,
    const bool showMessage)
{
    // 附加入口日志：记录目标 PID 与是否弹窗反馈。
    kLogEvent attachStartEvent;
    info << attachStartEvent
        << "[MemoryDock] attachToProcess: 开始附加, pid="
        << pid
        << ", processName="
        << processName.toStdString()
        << ", showMessage="
        << (showMessage ? "true" : "false")
        << eol;

    // 附加前先清理旧进程上下文，避免残留句柄。
    detachProcess();

    // 首选读写权限，满足写内存/断点能力。
    HANDLE processHandle = ::OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE,
        toDwordPid(pid));
    if (processHandle == nullptr)
    {
        // 失败时回退只读权限，至少保证浏览能力。
        processHandle = ::OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
            FALSE,
            toDwordPid(pid));
        if (processHandle == nullptr)
        {
            kLogEvent attachOpenFailEvent;
            err << attachOpenFailEvent
                << "[MemoryDock] attachToProcess: 读写+只读权限均失败, pid="
                << pid
                << ", error="
                << ::GetLastError()
                << eol;
            if (showMessage)
            {
                QMessageBox::warning(
                    this,
                    "附加失败",
                    "OpenProcess 失败，可能权限不足。\n请尝试以管理员身份运行。");
            }
            updateStatusBarText();
            return false;
        }
        m_canReadWriteMemory = false;
        kLogEvent attachReadonlyEvent;
        warn << attachReadonlyEvent
            << "[MemoryDock] attachToProcess: 退化为只读句柄, pid="
            << pid
            << eol;
    }
    else
    {
        m_canReadWriteMemory = true;
        kLogEvent attachReadWriteEvent;
        info << attachReadWriteEvent
            << "[MemoryDock] attachToProcess: 获取到可读写句柄, pid="
            << pid
            << eol;
    }

    m_attachedProcessHandle = processHandle;
    m_attachedPid = pid;
    m_attachedProcessName = processName;
    updateStatusBarText();

    // 附加后立即刷新模块与区域，减少下一步等待。
    refreshModuleListForPid(pid);
    refreshMemoryRegionList(true);

    if (showMessage)
    {
        QMessageBox::information(
            this,
            "附加成功",
            QString("已附加到 %1 (PID=%2)\n读写状态: %3")
            .arg(processName)
            .arg(pid)
            .arg(m_canReadWriteMemory ? "可读可写" : "只读"));
    }

    // 附加结束日志：记录最终状态。
    kLogEvent attachFinishEvent;
    info << attachFinishEvent
        << "[MemoryDock] attachToProcess: 附加完成, pid="
        << pid
        << ", canReadWrite="
        << (m_canReadWriteMemory ? "true" : "false")
        << eol;
    return true;
}

void MemoryDock::detachProcess()
{
    // 分离入口日志：记录旧 PID 便于追踪。
    kLogEvent detachStartEvent;
    info << detachStartEvent
        << "[MemoryDock] detachProcess: 开始分离, oldPid="
        << m_attachedPid
        << eol;

    // 先请求扫描取消，防止后台继续使用旧句柄。
    cancelCurrentScan();

    // 同步抬高模块刷新 ticket，确保旧的异步模块回调不会覆盖“已分离”状态。
    m_moduleRefreshTicket.fetch_add(1);
    m_moduleRefreshInProgress.store(false);
    if (m_moduleRefreshButton != nullptr)
    {
        m_moduleRefreshButton->setEnabled(true);
    }

    if (m_attachedProcessHandle != nullptr)
    {
        ::CloseHandle(m_attachedProcessHandle);
        m_attachedProcessHandle = nullptr;
    }

    m_attachedPid = 0;
    m_attachedProcessName.clear();
    m_canReadWriteMemory = false;

    // 分离时清理依赖上下文的数据缓存。
    m_moduleCache.clear();
    m_regionCache.clear();
    m_searchResultCache.clear();
    m_searchResultVisibleCount = 0;

    // 清理表格展示，避免用户误操作旧数据。
    m_moduleTable->clear();
    if (m_moduleStatusLabel != nullptr)
    {
        m_moduleStatusLabel->setText("● 未附加进程");
        m_moduleStatusLabel->setStyleSheet("color:#5F5F5F; font-weight:600;");
    }
    m_regionTable->setRowCount(0);
    m_searchResultTable->setRowCount(0);
    if (m_hexEditorWidget != nullptr)
    {
        m_hexEditorWidget->setEditable(false);
        m_hexEditorWidget->clearData();
    }
    m_viewerStatusLabel->setText("未附加进程。");

    updateStatusBarText();

    // 分离结束日志：确认缓存已清空。
    kLogEvent detachFinishEvent;
    info << detachFinishEvent
        << "[MemoryDock] detachProcess: 分离完成，缓存与表格已重置。"
        << eol;
}

HANDLE MemoryDock::openProcessHandleForRead(const std::uint32_t pid, QString* const errorTextOut) const
{
    // 读取句柄日志：用于诊断权限不足问题。
    kLogEvent openReadHandleEvent;
    dbg << openReadHandleEvent
        << "[MemoryDock] openProcessHandleForRead: pid="
        << pid
        << eol;

    HANDLE processHandle = ::OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        toDwordPid(pid));
    if (processHandle == nullptr && errorTextOut != nullptr)
    {
        *errorTextOut = QString("OpenProcess 失败，错误码=%1").arg(::GetLastError());
        kLogEvent openReadHandleFailEvent;
        warn << openReadHandleFailEvent
            << "[MemoryDock] openProcessHandleForRead: 打开失败, pid="
            << pid
            << ", error="
            << ::GetLastError()
            << eol;
    }
    return processHandle;
}

void MemoryDock::showProcessTableContextMenu(const QPoint& localPosition)
{
    // 右键入口日志：记录触发位置，便于诊断菜单行为。
    kLogEvent contextMenuEvent;
    dbg << contextMenuEvent
        << "[MemoryDock] showProcessTableContextMenu: x="
        << localPosition.x()
        << ", y="
        << localPosition.y()
        << eol;

    if (m_processTable == nullptr)
    {
        return;
    }

    const QModelIndex index = m_processTable->indexAt(localPosition);
    if (!index.isValid())
    {
        return;
    }

    const int row = index.row();
    const QTableWidgetItem* pidItem = m_processTable->item(row, 1);
    const QTableWidgetItem* nameItem = m_processTable->item(row, 0);
    if (pidItem == nullptr || nameItem == nullptr)
    {
        return;
    }

    bool pidOk = false;
    const std::uint32_t pid = pidItem->text().toUInt(&pidOk);
    if (!pidOk || pid == 0)
    {
        return;
    }
    const QString processName = nameItem->text().trimmed();

    QMenu contextMenu(this);
    QAction* attachAction = contextMenu.addAction(
        QIcon(":/Icon/process_start.svg"),
        QStringLiteral("附加进程"));
    QAction* dumpAction = contextMenu.addAction(
        QIcon(":/Icon/process_details.svg"),
        QStringLiteral("Dump内存到文件"));

    QAction* selectedAction = contextMenu.exec(m_processTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == attachAction)
    {
        attachToProcess(pid, processName, true);
        return;
    }

    if (selectedAction == dumpAction)
    {
        requestDumpProcessMemoryByPid(pid, processName);
    }
}

void MemoryDock::requestDumpProcessMemoryByPid(const std::uint32_t pid, const QString& processName)
{
    // Dump 请求日志：记录 PID 与进程名，便于后续审计。
    kLogEvent dumpRequestEvent;
    info << dumpRequestEvent
        << "[MemoryDock] requestDumpProcessMemoryByPid: pid="
        << pid
        << ", processName="
        << processName.toStdString()
        << eol;

    const QString defaultFileName = QStringLiteral("%1_pid%2_%3.kmdump")
        .arg(processName.isEmpty() ? QStringLiteral("process") : processName)
        .arg(pid)
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    const QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("保存进程内存Dump"),
        defaultFileName,
        QStringLiteral("Ksword Memory Dump (*.kmdump);;All Files (*.*)"));
    if (outputPath.trimmed().isEmpty())
    {
        return;
    }

    if (m_dumpMemoryProgressPid == 0)
    {
        m_dumpMemoryProgressPid = kPro.add("内存", "Dump进程内存");
    }
    kPro.set(m_dumpMemoryProgressPid, "准备读取内存区域", 0, 5.0f);

    QPointer<MemoryDock> guardThis(this);
    std::thread([guardThis, pid, processName, outputPath]() {
        if (guardThis == nullptr)
        {
            return;
        }

        QString errorText;
        const bool dumpOk = guardThis->dumpProcessMemoryToFile(pid, outputPath, errorText);
        QMetaObject::invokeMethod(qApp, [guardThis, dumpOk, pid, processName, outputPath, errorText]() {
            if (guardThis == nullptr)
            {
                return;
            }

            if (dumpOk)
            {
                kPro.set(guardThis->m_dumpMemoryProgressPid, "Dump完成", 0, 100.0f);

                kLogEvent dumpFinishEvent;
                info << dumpFinishEvent
                    << "[MemoryDock] Dump完成, pid="
                    << pid
                    << ", path="
                    << outputPath.toStdString()
                    << eol;

                QMessageBox::information(
                    guardThis,
                    QStringLiteral("Dump完成"),
                    QStringLiteral("进程 %1 (PID=%2) 内存已保存到：\n%3")
                    .arg(processName)
                    .arg(pid)
                    .arg(outputPath));
            }
            else
            {
                kPro.set(guardThis->m_dumpMemoryProgressPid, "Dump失败", 0, 100.0f);

                kLogEvent dumpFailEvent;
                err << dumpFailEvent
                    << "[MemoryDock] Dump失败, pid="
                    << pid
                    << ", error="
                    << errorText.toStdString()
                    << eol;

                QMessageBox::warning(
                    guardThis,
                    QStringLiteral("Dump失败"),
                    QStringLiteral("进程 %1 (PID=%2) Dump失败：\n%3")
                    .arg(processName)
                    .arg(pid)
                    .arg(errorText));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

bool MemoryDock::dumpProcessMemoryToFile(
    const std::uint32_t pid,
    const QString& dumpFilePath,
    QString& errorTextOut)
{
    errorTextOut.clear();

    HANDLE processHandle = ::OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        toDwordPid(pid));
    if (processHandle == nullptr)
    {
        errorTextOut = QString("OpenProcess 失败, error=%1").arg(::GetLastError());
        return false;
    }

    QFile outputFile(dumpFilePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        ::CloseHandle(processHandle);
        errorTextOut = QString("无法写入文件: %1").arg(dumpFilePath);
        return false;
    }

    // Dump 文件头结构：
    // - magic：文件签名；
    // - version：版本号（方便后续升级格式）；
    // - pid：目标进程 PID；
    // - timestamp100ns：创建时间戳；
    // - regionCount：成功写入的区域数量。
    struct DumpFileHeader final
    {
        char magic[8];
        std::uint32_t version = 1;
        std::uint32_t pid = 0;
        std::uint64_t timestamp100ns = 0;
        std::uint32_t regionCount = 0;
        std::uint32_t reserved = 0;
    };

    struct DumpRegionHeader final
    {
        std::uint64_t baseAddress = 0;
        std::uint64_t declaredRegionSize = 0;
        std::uint64_t dumpedBytes = 0;
        std::uint32_t protect = 0;
        std::uint32_t state = 0;
        std::uint32_t type = 0;
        std::uint32_t reserved = 0;
    };

    FILETIME fileTime{};
    ::GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER fileTimeValue{};
    fileTimeValue.LowPart = fileTime.dwLowDateTime;
    fileTimeValue.HighPart = fileTime.dwHighDateTime;

    DumpFileHeader fileHeader{};
    std::memcpy(fileHeader.magic, "KMDUMP1", 7);
    fileHeader.magic[7] = '\0';
    fileHeader.pid = pid;
    fileHeader.timestamp100ns = fileTimeValue.QuadPart;

    outputFile.write(reinterpret_cast<const char*>(&fileHeader), static_cast<qint64>(sizeof(fileHeader)));

    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    const std::uint64_t minAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uint64_t maxAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);

    constexpr SIZE_T kChunkSize = 256 * 1024;
    std::vector<std::uint8_t> chunkBuffer(kChunkSize, 0);

    std::uint64_t currentAddress = minAddress;
    std::uint32_t dumpedRegionCount = 0;
    while (currentAddress < maxAddress)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T querySize = ::VirtualQueryEx(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(currentAddress)),
            &mbi,
            sizeof(mbi));
        if (querySize != sizeof(mbi))
        {
            break;
        }

        const std::uint64_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uint64_t regionSize = static_cast<std::uint64_t>(mbi.RegionSize);

        if (mbi.State == MEM_COMMIT && isReadableProtect(static_cast<std::uint32_t>(mbi.Protect)))
        {
            DumpRegionHeader regionHeader{};
            regionHeader.baseAddress = regionBase;
            regionHeader.declaredRegionSize = regionSize;
            regionHeader.protect = static_cast<std::uint32_t>(mbi.Protect);
            regionHeader.state = static_cast<std::uint32_t>(mbi.State);
            regionHeader.type = static_cast<std::uint32_t>(mbi.Type);

            const qint64 headerPosition = outputFile.pos();
            outputFile.write(reinterpret_cast<const char*>(&regionHeader), static_cast<qint64>(sizeof(regionHeader)));

            std::uint64_t offsetInRegion = 0;
            std::uint64_t dumpedBytes = 0;
            while (offsetInRegion < regionSize)
            {
                const SIZE_T remainSize = static_cast<SIZE_T>(std::min<std::uint64_t>(regionSize - offsetInRegion, kChunkSize));
                SIZE_T bytesRead = 0;
                const BOOL readOk = ::ReadProcessMemory(
                    processHandle,
                    reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(regionBase + offsetInRegion)),
                    chunkBuffer.data(),
                    remainSize,
                    &bytesRead);

                if (readOk == FALSE || bytesRead == 0)
                {
                    break;
                }

                outputFile.write(reinterpret_cast<const char*>(chunkBuffer.data()), static_cast<qint64>(bytesRead));
                dumpedBytes += static_cast<std::uint64_t>(bytesRead);
                offsetInRegion += static_cast<std::uint64_t>(bytesRead);
            }

            regionHeader.dumpedBytes = dumpedBytes;
            const qint64 endPosition = outputFile.pos();
            outputFile.seek(headerPosition);
            outputFile.write(reinterpret_cast<const char*>(&regionHeader), static_cast<qint64>(sizeof(regionHeader)));
            outputFile.seek(endPosition);

            if (dumpedBytes > 0)
            {
                ++dumpedRegionCount;
            }
        }

        const std::uint64_t totalRange = maxAddress - minAddress;
        if (totalRange > 0)
        {
            const float progressValue = static_cast<float>(
                std::min<std::uint64_t>(
                    95,
                    ((regionBase - minAddress) * 95ULL) / totalRange));
            kPro.set(m_dumpMemoryProgressPid, "读取并写入内存区域中", 0, progressValue);
        }

        const std::uint64_t nextAddress = regionBase + regionSize;
        if (nextAddress <= currentAddress)
        {
            break;
        }
        currentAddress = nextAddress;
    }

    // 回写区域数量到文件头。
    fileHeader.regionCount = dumpedRegionCount;
    outputFile.seek(0);
    outputFile.write(reinterpret_cast<const char*>(&fileHeader), static_cast<qint64>(sizeof(fileHeader)));
    outputFile.close();

    ::CloseHandle(processHandle);

    if (dumpedRegionCount == 0)
    {
        errorTextOut = "未读取到可导出的有效区域。";
        return false;
    }
    return true;
}

void MemoryDock::refreshMemoryRegionList(const bool forceRequery)
{
    // 区域刷新入口日志：记录是否强制重查。
    kLogEvent regionRefreshStartEvent;
    info << regionRefreshStartEvent
        << "[MemoryDock] refreshMemoryRegionList: 开始刷新区域, forceRequery="
        << (forceRequery ? "true" : "false")
        << ", attachedPid="
        << m_attachedPid
        << eol;

    if (m_attachedProcessHandle == nullptr)
    {
        m_regionCache.clear();
        m_regionTable->setRowCount(0);
        return;
    }

    if (forceRequery || m_regionCache.empty())
    {
        QString errorText;
        std::vector<RegionEntry> regionList;
        if (!enumerateMemoryRegionsByVirtualQuery(m_attachedProcessHandle, regionList, &errorText))
        {
            kLogEvent regionEnumerateFailEvent;
            err << regionEnumerateFailEvent
                << "[MemoryDock] refreshMemoryRegionList: 枚举区域失败, error="
                << errorText.toStdString()
                << eol;
            QMessageBox::warning(this, "区域刷新", errorText);
            return;
        }
        m_regionCache = std::move(regionList);
    }

    applyRegionFilterAndRebuildTable();

    // 区域刷新结束日志：记录缓存数量。
    kLogEvent regionRefreshFinishEvent;
    info << regionRefreshFinishEvent
        << "[MemoryDock] refreshMemoryRegionList: 刷新完成, regionCount="
        << m_regionCache.size()
        << eol;
}

bool MemoryDock::enumerateMemoryRegionsByVirtualQuery(
    HANDLE processHandle,
    std::vector<RegionEntry>& regionsOut,
    QString* const errorTextOut) const
{
    // VirtualQueryEx 枚举入口日志：输出句柄有效性说明。
    kLogEvent enumerateRegionStartEvent;
    dbg << enumerateRegionStartEvent
        << "[MemoryDock] enumerateMemoryRegionsByVirtualQuery: 开始遍历虚拟内存区域。"
        << eol;

    regionsOut.clear();

    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    const std::uint64_t minAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uint64_t maxAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);

    std::uint64_t currentAddress = minAddress;
    while (currentAddress < maxAddress)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T querySize = ::VirtualQueryEx(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(currentAddress)),
            &mbi,
            sizeof(mbi));
        if (querySize != sizeof(mbi))
        {
            break;
        }

        RegionEntry entry{};
        entry.baseAddress = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        entry.regionSize = static_cast<std::uint64_t>(mbi.RegionSize);
        entry.protect = static_cast<std::uint32_t>(mbi.Protect);
        entry.state = static_cast<std::uint32_t>(mbi.State);
        entry.type = static_cast<std::uint32_t>(mbi.Type);

        // IMAGE/MAPPED 区域尽量解析映射文件路径，便于用户定位模块。
        if (entry.state == MEM_COMMIT && (entry.type == MEM_IMAGE || entry.type == MEM_MAPPED))
        {
            wchar_t mappedPath[MAX_PATH] = {};
            const DWORD length = ::GetMappedFileNameW(
                processHandle,
                mbi.BaseAddress,
                mappedPath,
                static_cast<DWORD>(std::size(mappedPath)));
            if (length > 0)
            {
                entry.mappedFilePath = QString::fromWCharArray(mappedPath, static_cast<int>(length));
            }
        }

        regionsOut.push_back(std::move(entry));
        if (mbi.RegionSize == 0)
        {
            break;
        }

        const std::uint64_t nextAddress =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
            static_cast<std::uint64_t>(mbi.RegionSize);
        if (nextAddress <= currentAddress)
        {
            break;
        }
        currentAddress = nextAddress;
    }

    if (regionsOut.empty())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = "VirtualQueryEx 未返回有效区域。";
        }
        kLogEvent enumerateRegionEmptyEvent;
        warn << enumerateRegionEmptyEvent
            << "[MemoryDock] enumerateMemoryRegionsByVirtualQuery: 枚举结果为空。"
            << eol;
        return false;
    }

    // 枚举结束日志：用于衡量目标进程地址空间复杂度。
    kLogEvent enumerateRegionFinishEvent;
    dbg << enumerateRegionFinishEvent
        << "[MemoryDock] enumerateMemoryRegionsByVirtualQuery: 完成, count="
        << regionsOut.size()
        << eol;
    return true;
}

void MemoryDock::applyRegionFilterAndRebuildTable()
{
    // 过滤入口日志：记录当前缓存规模与过滤开关状态。
    kLogEvent regionFilterStartEvent;
    dbg << regionFilterStartEvent
        << "[MemoryDock] applyRegionFilterAndRebuildTable: 开始过滤, cacheSize="
        << m_regionCache.size()
        << ", committedOnly="
        << (m_regionCommittedOnlyCheck->isChecked() ? "true" : "false")
        << ", imageOnly="
        << (m_regionImageOnlyCheck->isChecked() ? "true" : "false")
        << ", readableOnly="
        << (m_regionReadableOnlyCheck->isChecked() ? "true" : "false")
        << eol;

    // 过滤条件组合：已提交 / IMAGE / 可读。
    std::vector<const RegionEntry*> filteredRegions;
    filteredRegions.reserve(m_regionCache.size());
    for (const RegionEntry& entry : m_regionCache)
    {
        if (m_regionCommittedOnlyCheck->isChecked() && entry.state != MEM_COMMIT)
        {
            continue;
        }
        if (m_regionImageOnlyCheck->isChecked() && entry.type != MEM_IMAGE)
        {
            continue;
        }
        if (m_regionReadableOnlyCheck->isChecked() && !isReadableProtect(entry.protect))
        {
            continue;
        }
        filteredRegions.push_back(&entry);
    }

    m_regionTable->setSortingEnabled(false);
    m_regionTable->setRowCount(static_cast<int>(filteredRegions.size()));
    for (int row = 0; row < static_cast<int>(filteredRegions.size()); ++row)
    {
        const RegionEntry& entry = *filteredRegions[static_cast<std::size_t>(row)];
        // 列 0 和列 1 附带 UserRole 原始值，便于排序/右键动作时可靠反查。
        QTableWidgetItem* baseItem = new QTableWidgetItem(formatAddress(entry.baseAddress));
        baseItem->setData(Qt::EditRole, QVariant::fromValue<qulonglong>(entry.baseAddress));
        baseItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(entry.baseAddress)));
        m_regionTable->setItem(row, 0, baseItem);

        QTableWidgetItem* sizeItem = new QTableWidgetItem(formatSize(entry.regionSize));
        sizeItem->setData(Qt::EditRole, QVariant::fromValue<qulonglong>(entry.regionSize));
        sizeItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(entry.regionSize)));
        m_regionTable->setItem(row, 1, sizeItem);

        m_regionTable->setItem(row, 2, new QTableWidgetItem(protectToText(entry.protect)));
        m_regionTable->setItem(row, 3, new QTableWidgetItem(stateToText(entry.state)));
        m_regionTable->setItem(row, 4, new QTableWidgetItem(typeToText(entry.type)));
        m_regionTable->setItem(row, 5, new QTableWidgetItem(entry.mappedFilePath));
    }
    m_regionTable->setSortingEnabled(true);

    // 过滤结束日志：记录最终展示条目数。
    kLogEvent regionFilterFinishEvent;
    info << regionFilterFinishEvent
        << "[MemoryDock] applyRegionFilterAndRebuildTable: 完成, visibleCount="
        << filteredRegions.size()
        << eol;
}

