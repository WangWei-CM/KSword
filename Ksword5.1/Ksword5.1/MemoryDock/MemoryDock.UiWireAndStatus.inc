// ============================================================
// MemoryDock.UiWireAndStatus.inc
// 作用：承载信号槽连接与状态栏/定时器初始化代码。
// ============================================================

void MemoryDock::initializeConnections()
{
    // 信号槽连接日志：用于确认所有交互通道都已挂接。
    kLogEvent connectInitEvent;
    info << connectInitEvent
        << "[MemoryDock] initializeConnections: 开始连接全部信号槽。"
        << eol;

    // ========================================================
    // 工具栏逻辑
    // ========================================================

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        // 刷新动作按当前 Tab 路由，减少无关页面刷新开销。
        const int tabIndex = m_tabWidget->currentIndex();
        kLogEvent refreshClickEvent;
        info << refreshClickEvent
            << "[MemoryDock] 点击刷新按钮, 当前Tab索引="
            << tabIndex
            << ", attachedPid="
            << m_attachedPid
            << eol;
        if (tabIndex == 0)
        {
            refreshProcessList(true);
            if (m_attachedPid != 0)
            {
                refreshModuleListForPid(m_attachedPid);
            }
            return;
        }
        if (tabIndex == 1)
        {
            refreshMemoryRegionList(true);
            return;
        }
        if (tabIndex == 2)
        {
            refreshMemoryRegionList(true);
            return;
        }
        if (tabIndex == 3)
        {
            reloadMemoryViewerPage();
            return;
        }
        if (tabIndex == 4)
        {
            refreshBookmarkValues();
        }
        });

    connect(m_processCombo, &QComboBox::currentIndexChanged, this, [this](int indexValue) {
        // 顶部进程切换时只刷新模块预览，不自动附加。
        if (indexValue < 0)
        {
            return;
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_processCombo->itemData(indexValue, Qt::UserRole).toUInt());
        kLogEvent processComboEvent;
        dbg << processComboEvent
            << "[MemoryDock] 进程下拉框切换, index="
            << indexValue
            << ", pid="
            << pid
            << eol;
        refreshModuleListForPid(pid);
        });

    connect(m_attachButton, &QPushButton::clicked, this, [this]() {
        const int comboIndex = m_processCombo->currentIndex();
        if (comboIndex < 0)
        {
            kLogEvent noSelectionEvent;
            warn << noSelectionEvent
                << "[MemoryDock] 点击附加但未选择进程。"
                << eol;
            QMessageBox::warning(this, "附加进程", "请先选择进程。");
            return;
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt());
        const QString processName = m_processCombo->itemData(comboIndex, Qt::UserRole + 1).toString();
        kLogEvent attachClickEvent;
        info << attachClickEvent
            << "[MemoryDock] 点击附加按钮, pid="
            << pid
            << ", processName="
            << processName.toStdString()
            << eol;
        attachToProcess(pid, processName, true);
        });

    connect(m_detachButton, &QPushButton::clicked, this, [this]() {
        kLogEvent detachClickEvent;
        info << detachClickEvent
            << "[MemoryDock] 点击分离按钮, currentPid="
            << m_attachedPid
            << eol;
        detachProcess();
        QMessageBox::information(this, "分离进程", "已分离当前进程。");
        });

    connect(m_settingsButton, &QPushButton::clicked, this, [this]() {
        kLogEvent settingsOpenEvent;
        info << settingsOpenEvent
            << "[MemoryDock] 打开扫描设置对话框, 当前线程数="
            << m_scanThreadCount
            << ", 当前块大小KB="
            << m_scanChunkSizeKB
            << eol;

        // 设置页面：允许调整扫描线程数和块大小（缓存大小近似）。
        QDialog dialog(this);
        dialog.setWindowTitle("内存扫描设置");
        QFormLayout* formLayout = new QFormLayout(&dialog);

        QSpinBox* threadSpin = new QSpinBox(&dialog);
        threadSpin->setRange(1, 32);
        threadSpin->setValue(static_cast<int>(m_scanThreadCount));

        QSpinBox* chunkSpin = new QSpinBox(&dialog);
        chunkSpin->setRange(64, 16384);
        chunkSpin->setValue(static_cast<int>(m_scanChunkSizeKB));
        chunkSpin->setSuffix(" KB");

        QPushButton* okButton = new QPushButton("确定", &dialog);
        QPushButton* cancelButton = new QPushButton("取消", &dialog);
        okButton->setStyleSheet(buildBlueButtonStyle());
        cancelButton->setStyleSheet(buildBlueButtonStyle());
        QHBoxLayout* buttonLayout = new QHBoxLayout();
        buttonLayout->addWidget(okButton);
        buttonLayout->addWidget(cancelButton);

        formLayout->addRow("扫描线程数", threadSpin);
        formLayout->addRow("读取块大小", chunkSpin);
        formLayout->addRow(buttonLayout);

        connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
        connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);

        if (dialog.exec() == QDialog::Accepted)
        {
            m_scanThreadCount = static_cast<std::uint32_t>(threadSpin->value());
            m_scanChunkSizeKB = static_cast<std::uint32_t>(chunkSpin->value());
            m_scanStatusLabel->setText(
                QString("设置已更新：线程=%1, 块=%2KB")
                .arg(m_scanThreadCount)
                .arg(m_scanChunkSizeKB));

            kLogEvent settingsApplyEvent;
            info << settingsApplyEvent
                << "[MemoryDock] 扫描设置已更新, 线程数="
                << m_scanThreadCount
                << ", 块大小KB="
                << m_scanChunkSizeKB
                << eol;
        }
        else
        {
            kLogEvent settingsCancelEvent;
            dbg << settingsCancelEvent
                << "[MemoryDock] 扫描设置对话框取消。"
                << eol;
        }
        });

    // ========================================================
    // Tab1：进程与模块
    // ========================================================

    connect(m_processTable, &QTableWidget::cellClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0)
        {
            return;
        }

        // 注意：表格允许排序，row 可能不再对应 m_processCache 原始下标。
        // 因此这里改为“从单元格文本/数据反解 PID + 进程名”，保证绑定准确。
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
            kLogEvent processClickParseFailEvent;
            warn << processClickParseFailEvent
                << "[MemoryDock] 进程表单击后 PID 解析失败, row="
                << row
                << eol;
            return;
        }

        kLogEvent processClickEvent;
        dbg << processClickEvent
            << "[MemoryDock] 进程表单击, row="
            << row
            << ", pid="
            << pid
            << eol;

        for (int index = 0; index < m_processCombo->count(); ++index)
        {
            if (m_processCombo->itemData(index, Qt::UserRole).toUInt() == pid)
            {
                m_processCombo->setCurrentIndex(index);
                break;
            }
        }
        refreshModuleListForPid(pid);
        });

    connect(m_processTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0)
        {
            return;
        }
        // 双击附加同样不能直接拿 row 索引缓存，需从当前表格行动态取值。
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
            kLogEvent processDoubleClickParseFailEvent;
            warn << processDoubleClickParseFailEvent
                << "[MemoryDock] 进程表双击后 PID 解析失败, row="
                << row
                << eol;
            return;
        }
        kLogEvent processDoubleClickEvent;
        info << processDoubleClickEvent
            << "[MemoryDock] 进程表双击触发附加, row="
            << row
            << ", pid="
            << pid
            << ", processName="
            << nameItem->text().toStdString()
            << eol;
        attachToProcess(pid, nameItem->text(), true);
        });

    connect(m_processTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showProcessTableContextMenu(localPosition);
        });

    connect(m_moduleRefreshButton, &QPushButton::clicked, this, [this]() {
        // 模块刷新按钮：按当前下拉 PID 触发模块重载。
        const int comboIndex = m_processCombo->currentIndex();
        if (comboIndex < 0)
        {
            return;
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt());
        kLogEvent moduleRefreshClickEvent;
        info << moduleRefreshClickEvent
            << "[MemoryDock] 点击模块刷新按钮, pid="
            << pid
            << eol;
        refreshModuleListForPid(pid);
        });

    connect(m_moduleSignatureCheck, &QCheckBox::toggled, this, [this](bool checked) {
        // 切换签名校验后立即重刷，确保列表状态与选项一致。
        const int comboIndex = m_processCombo->currentIndex();
        if (comboIndex < 0)
        {
            return;
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt());
        kLogEvent moduleSignatureToggleEvent;
        info << moduleSignatureToggleEvent
            << "[MemoryDock] 模块签名校验选项变更, checked="
            << (checked ? "true" : "false")
            << ", pid="
            << pid
            << eol;
        refreshModuleListForPid(pid);
        });

    connect(m_moduleFilterEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        // 过滤输入改为“只重绘缓存”，避免每敲一个字都重新做模块枚举和签名校验。
        kLogEvent moduleFilterEvent;
        dbg << moduleFilterEvent
            << "[MemoryDock] 模块过滤条件变化, filter="
            << m_moduleFilterEdit->text().trimmed().toStdString()
            << ", cacheCount="
            << m_moduleCache.size()
            << eol;

        rebuildModuleTableFromCache();
        if (m_moduleStatusLabel != nullptr)
        {
            m_moduleStatusLabel->setText(
                QString("● 模块:%1 显示:%2 (过滤)")
                .arg(m_moduleCache.size())
                .arg(m_moduleTable->topLevelItemCount()));
            if (!m_moduleRefreshInProgress.load())
            {
                m_moduleStatusLabel->setStyleSheet("color:#2F7D32; font-weight:600;");
            }
        }
        });

    connect(m_moduleTable, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        // 双击模块任意列时，按模块基址跳转到内存查看器。
        Q_UNUSED(column);
        if (item == nullptr)
        {
            return;
        }
        const std::uint64_t baseAddress = item->data(
            toModuleTreeColumnIndex(ModuleTreeColumn::Path),
            Qt::UserRole).toULongLong();
        kLogEvent moduleJumpEvent;
        info << moduleJumpEvent
            << "[MemoryDock] 模块表双击跳转查看器, address="
            << formatAddress(baseAddress).toStdString()
            << eol;
        jumpToAddress(baseAddress);
        });

    connect(m_moduleTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        QTreeWidgetItem* clickedItem = m_moduleTable->itemAt(localPosition);
        if (clickedItem == nullptr)
        {
            return;
        }
        m_moduleTable->setCurrentItem(clickedItem);

        const std::uint64_t baseAddress = clickedItem->data(
            toModuleTreeColumnIndex(ModuleTreeColumn::Path),
            Qt::UserRole).toULongLong();
        const QString baseText = formatAddress(baseAddress);

        QMenu menu(this);
        QAction* copyBaseAction = menu.addAction("复制基址");
        QAction* jumpViewerAction = menu.addAction("跳转到内存查看器");
        QAction* selectedAction = menu.exec(m_moduleTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        if (selectedAction == copyBaseAction)
        {
            QApplication::clipboard()->setText(baseText);
            kLogEvent copyBaseEvent;
            dbg << copyBaseEvent
                << "[MemoryDock] 模块表右键复制基址, text="
                << baseText.toStdString()
                << eol;
            return;
        }
        if (selectedAction == jumpViewerAction)
        {
            kLogEvent contextJumpEvent;
            info << contextJumpEvent
                << "[MemoryDock] 模块表右键跳转查看器, address="
                << baseText.toStdString()
                << eol;
            jumpToAddress(baseAddress);
        }
        });

    // ========================================================
    // Tab2：内存区域
    // ========================================================

    const auto applyRegionFilter = [this]() {
        kLogEvent regionFilterToggleEvent;
        dbg << regionFilterToggleEvent
            << "[MemoryDock] 区域过滤条件变化, committedOnly="
            << (m_regionCommittedOnlyCheck->isChecked() ? "true" : "false")
            << ", imageOnly="
            << (m_regionImageOnlyCheck->isChecked() ? "true" : "false")
            << ", readableOnly="
            << (m_regionReadableOnlyCheck->isChecked() ? "true" : "false")
            << eol;
        applyRegionFilterAndRebuildTable();
        };
    connect(m_regionCommittedOnlyCheck, &QCheckBox::toggled, this, applyRegionFilter);
    connect(m_regionImageOnlyCheck, &QCheckBox::toggled, this, applyRegionFilter);
    connect(m_regionReadableOnlyCheck, &QCheckBox::toggled, this, applyRegionFilter);

    connect(m_regionTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0)
        {
            return;
        }
        const QString baseText = m_regionTable->item(row, 0) != nullptr
            ? m_regionTable->item(row, 0)->text()
            : QString();
        std::uint64_t baseAddress = 0;
        if (parseAddressText(baseText, baseAddress))
        {
            kLogEvent regionDoubleClickEvent;
            info << regionDoubleClickEvent
                << "[MemoryDock] 区域表双击跳转查看器, base="
                << formatAddress(baseAddress).toStdString()
                << eol;
            jumpToAddress(baseAddress);
        }
        });

    connect(m_regionTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        QTableWidgetItem* clickedItem = m_regionTable->itemAt(localPosition);
        if (clickedItem == nullptr)
        {
            return;
        }

        const int row = clickedItem->row();
        const QString baseText = m_regionTable->item(row, 0) != nullptr
            ? m_regionTable->item(row, 0)->text()
            : QString();

        QMenu menu(this);
        QAction* viewAction = menu.addAction("查看此区域");
        QAction* copyAction = menu.addAction("复制基址");
        QAction* searchAction = menu.addAction("搜索此区域");
        QAction* selectedAction = menu.exec(m_regionTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        if (selectedAction == copyAction)
        {
            QApplication::clipboard()->setText(baseText);
            kLogEvent regionCopyEvent;
            dbg << regionCopyEvent
                << "[MemoryDock] 区域表右键复制基址, text="
                << baseText.toStdString()
                << eol;
            return;
        }

        std::uint64_t baseAddress = 0;
        if (!parseAddressText(baseText, baseAddress))
        {
            return;
        }

        if (selectedAction == viewAction)
        {
            kLogEvent regionViewEvent;
            info << regionViewEvent
                << "[MemoryDock] 区域表右键查看区域, base="
                << formatAddress(baseAddress).toStdString()
                << eol;
            jumpToAddress(baseAddress);
            return;
        }
        if (selectedAction == searchAction)
        {
            // 搜索此区域：切换到 Tab3 并填充起止地址。
            // 注意：区域表启用了排序，row 与 m_regionCache 下标不再等价。
            // 因此基于表格 item 的 UserRole 中缓存的 base/size 进行计算。
            const QTableWidgetItem* baseItem = m_regionTable->item(row, 0);
            const QTableWidgetItem* sizeItem = m_regionTable->item(row, 1);
            if (baseItem != nullptr && sizeItem != nullptr)
            {
                const std::uint64_t regionBase = baseItem->data(Qt::UserRole).toULongLong();
                const std::uint64_t regionSize = sizeItem->data(Qt::UserRole).toULongLong();
                if (regionSize > 0)
                {
                    const std::uint64_t regionEnd = regionBase + regionSize - 1;
                    kLogEvent regionSearchEvent;
                    info << regionSearchEvent
                        << "[MemoryDock] 区域表右键搜索区域, start="
                        << formatAddress(regionBase).toStdString()
                        << ", end="
                        << formatAddress(regionEnd).toStdString()
                        << eol;
                    m_searchRangeCombo->setCurrentIndex(1);
                    m_searchRangeStartEdit->setText(formatAddress(regionBase));
                    m_searchRangeEndEdit->setText(formatAddress(regionEnd));
                    m_tabWidget->setCurrentWidget(m_tabSearch);
                }
            }
        }
        });

    // ========================================================
    // Tab3：搜索
    // ========================================================

    connect(m_searchRangeCombo, &QComboBox::currentIndexChanged, this, [this](int indexValue) {
        const bool customRange = (indexValue == 1);
        m_searchRangeStartEdit->setEnabled(customRange);
        m_searchRangeEndEdit->setEnabled(customRange);
        kLogEvent rangeModeEvent;
        dbg << rangeModeEvent
            << "[MemoryDock] 搜索范围模式切换, index="
            << indexValue
            << ", customRange="
            << (customRange ? "true" : "false")
            << eol;
        });

    connect(m_nextScanCompareCombo, &QComboBox::currentIndexChanged, this, [this](int indexValue) {
        const auto compareMode = static_cast<SearchCompareMode>(
            m_nextScanCompareCombo->itemData(indexValue).toInt());
        const bool needValueInput = !(
            compareMode == SearchCompareMode::Changed ||
            compareMode == SearchCompareMode::Unchanged ||
            compareMode == SearchCompareMode::Increased ||
            compareMode == SearchCompareMode::Decreased);
        const bool needSecondValue = (compareMode == SearchCompareMode::Between);
        m_nextScanValueEdit->setEnabled(needValueInput);
        m_nextScanValueBEdit->setVisible(needSecondValue);
        kLogEvent compareModeEvent;
        dbg << compareModeEvent
            << "[MemoryDock] 再次扫描条件切换, mode="
            << static_cast<int>(compareMode)
            << ", needValue="
            << (needValueInput ? "true" : "false")
            << ", needValueB="
            << (needSecondValue ? "true" : "false")
            << eol;
        });

    connect(m_firstScanButton, &QPushButton::clicked, this, [this]() {
        kLogEvent firstScanClickEvent;
        info << firstScanClickEvent
            << "[MemoryDock] 点击首次扫描按钮。"
            << eol;
        startFirstScan();
        });
    connect(m_nextScanButton, &QPushButton::clicked, this, [this]() {
        kLogEvent nextScanClickEvent;
        info << nextScanClickEvent
            << "[MemoryDock] 点击再次扫描按钮。"
            << eol;
        startNextScan();
        });
    connect(m_resetScanButton, &QPushButton::clicked, this, [this]() {
        kLogEvent resetScanClickEvent;
        info << resetScanClickEvent
            << "[MemoryDock] 点击重置扫描按钮。"
            << eol;
        resetScanState();
        });
    connect(m_cancelScanButton, &QPushButton::clicked, this, [this]() {
        kLogEvent cancelScanClickEvent;
        warn << cancelScanClickEvent
            << "[MemoryDock] 点击取消扫描按钮。"
            << eol;
        cancelCurrentScan();
        });

    connect(m_searchResultTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0 || row >= static_cast<int>(m_searchResultVisibleCount))
        {
            return;
        }
        kLogEvent resultDoubleClickEvent;
        info << resultDoubleClickEvent
            << "[MemoryDock] 搜索结果双击跳转, row="
            << row
            << eol;
        jumpToAddress(m_searchResultCache[static_cast<std::size_t>(row)].address);
        });

    connect(m_searchResultTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        QTableWidgetItem* clickedItem = m_searchResultTable->itemAt(localPosition);
        if (clickedItem == nullptr)
        {
            return;
        }

        const int row = clickedItem->row();
        if (row < 0 || row >= static_cast<int>(m_searchResultVisibleCount))
        {
            return;
        }

        QMenu menu(this);
        QAction* viewAction = menu.addAction("查看此地址");
        QAction* addBookmarkAction = menu.addAction("添加到书签");
        QAction* copyAddressAction = menu.addAction("复制地址");
        QAction* copyValueAction = menu.addAction("复制值");
        QAction* selectedAction = menu.exec(m_searchResultTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        const SearchResultEntry& entry = m_searchResultCache[static_cast<std::size_t>(row)];
        if (selectedAction == viewAction)
        {
            kLogEvent resultViewActionEvent;
            info << resultViewActionEvent
                << "[MemoryDock] 搜索结果右键查看地址, address="
                << formatAddress(entry.address).toStdString()
                << eol;
            jumpToAddress(entry.address);
            return;
        }
        if (selectedAction == addBookmarkAction)
        {
            kLogEvent resultBookmarkActionEvent;
            info << resultBookmarkActionEvent
                << "[MemoryDock] 搜索结果右键添加书签, address="
                << formatAddress(entry.address).toStdString()
                << eol;
            addBookmarkByAddress(entry.address, "来自内存搜索结果");
            rebuildBookmarkTable();
            return;
        }
        if (selectedAction == copyAddressAction)
        {
            QApplication::clipboard()->setText(formatAddress(entry.address));
            kLogEvent resultCopyAddressEvent;
            dbg << resultCopyAddressEvent
                << "[MemoryDock] 搜索结果右键复制地址, address="
                << formatAddress(entry.address).toStdString()
                << eol;
            return;
        }
        if (selectedAction == copyValueAction)
        {
            QApplication::clipboard()->setText(
                bytesToDisplayString(entry.currentValueBytes, m_lastSearchValueType));
            kLogEvent resultCopyValueEvent;
            dbg << resultCopyValueEvent
                << "[MemoryDock] 搜索结果右键复制值, row="
                << row
                << eol;
        }
        });

    // ========================================================
    // Tab4：查看器
    // ========================================================

    connect(m_viewJumpButton, &QPushButton::clicked, this, [this]() {
        kLogEvent viewerJumpClickEvent;
        info << viewerJumpClickEvent
            << "[MemoryDock] 查看器点击跳转按钮, text="
            << m_viewAddressEdit->text().trimmed().toStdString()
            << eol;
        jumpToAddressFromUi();
        });
    connect(m_viewAddressEdit, &QLineEdit::returnPressed, this, [this]() {
        kLogEvent viewerEnterJumpEvent;
        info << viewerEnterJumpEvent
            << "[MemoryDock] 查看器地址框回车跳转, text="
            << m_viewAddressEdit->text().trimmed().toStdString()
            << eol;
        jumpToAddressFromUi();
        });

    connect(m_hexEditorWidget, &HexEditorWidget::byteEdited, this,
        [this](const std::uint64_t absoluteAddress, const std::uint8_t oldValue, const std::uint8_t newValue) {
            QString errorText;
            if (!writeSingleByteAtViewer(absoluteAddress, newValue, errorText))
            {
                kLogEvent viewerWriteFailEvent;
                err << viewerWriteFailEvent
                    << "[MemoryDock] 十六进制编辑写入失败, address="
                    << formatAddress(absoluteAddress).toStdString()
                    << ", error="
                    << errorText.toStdString()
                    << eol;

                // 写入失败时把控件数据回滚到旧值，保证显示与目标进程一致。
                m_hexEditorWidget->setByteAtAbsoluteAddress(absoluteAddress, oldValue, true);
                QMessageBox::warning(this, "内存编辑", errorText);
                return;
            }

            // 本地缓存同步更新，避免后续断点/书签逻辑读到旧页缓存。
            if (absoluteAddress >= m_currentViewerAddress)
            {
                const std::uint64_t offset = absoluteAddress - m_currentViewerAddress;
                if (offset < static_cast<std::uint64_t>(m_currentViewerPageBytes.size()))
                {
                    m_currentViewerPageBytes[static_cast<int>(offset)] = static_cast<char>(newValue);
                }
            }

            kLogEvent viewerWriteSuccessEvent;
            info << viewerWriteSuccessEvent
                << "[MemoryDock] 十六进制编辑写入成功, address="
                << formatAddress(absoluteAddress).toStdString()
                << ", value=0x"
                << QString("%1").arg(newValue, 2, 16, QChar('0')).toUpper().toStdString()
                << eol;
        });

    connect(m_hexEditorWidget, &HexEditorWidget::aboutToShowContextMenu, this,
        [this](QMenu* menu, const std::uint64_t absoluteAddress, const bool hasByte) {
            if (menu == nullptr || !hasByte)
            {
                return;
            }

            menu->addSeparator();
            QAction* addBookmarkAction = menu->addAction("添加书签");
            QAction* addBreakpointAction = menu->addAction("添加断点");

            connect(addBookmarkAction, &QAction::triggered, this, [this, absoluteAddress]() {
                kLogEvent hexAddBookmarkEvent;
                info << hexAddBookmarkEvent
                    << "[MemoryDock] 查看器右键添加书签, address="
                    << formatAddress(absoluteAddress).toStdString()
                    << eol;
                addBookmarkByAddress(absoluteAddress, "来自内存查看器");
                rebuildBookmarkTable();
                });

            connect(addBreakpointAction, &QAction::triggered, this, [this, absoluteAddress]() {
                QString errorText;
                if (!addBreakpointByAddress(absoluteAddress, "来自内存查看器", errorText))
                {
                    kLogEvent hexAddBreakpointFailEvent;
                    err << hexAddBreakpointFailEvent
                        << "[MemoryDock] 查看器右键添加断点失败, address="
                        << formatAddress(absoluteAddress).toStdString()
                        << ", error="
                        << errorText.toStdString()
                        << eol;
                    QMessageBox::warning(this, "添加断点", errorText);
                }
                else
                {
                    kLogEvent hexAddBreakpointSuccessEvent;
                    info << hexAddBreakpointSuccessEvent
                        << "[MemoryDock] 查看器右键添加断点成功, address="
                        << formatAddress(absoluteAddress).toStdString()
                        << eol;
                }
                rebuildBreakpointTable();
                });
        });

    // ========================================================
    // Tab5：断点与书签
    // ========================================================

    connect(m_addBreakpointButton, &QPushButton::clicked, this, [this]() {
        bool inputOk = false;
        const QString addressText = QInputDialog::getText(
            this,
            "添加断点",
            "输入断点地址",
            QLineEdit::Normal,
            "0x0",
            &inputOk);
        if (!inputOk)
        {
            kLogEvent addBreakpointCancelEvent;
            dbg << addBreakpointCancelEvent
                << "[MemoryDock] 添加断点操作已取消。"
                << eol;
            return;
        }

        std::uint64_t address = 0;
        if (!parseAddressText(addressText, address))
        {
            kLogEvent addBreakpointParseFailEvent;
            warn << addBreakpointParseFailEvent
                << "[MemoryDock] 添加断点地址解析失败, text="
                << addressText.toStdString()
                << eol;
            QMessageBox::warning(this, "添加断点", "地址格式无效。");
            return;
        }

        QString errorText;
        if (!addBreakpointByAddress(address, "手动添加", errorText))
        {
            kLogEvent addBreakpointFailEvent;
            err << addBreakpointFailEvent
                << "[MemoryDock] 添加断点失败, address="
                << formatAddress(address).toStdString()
                << ", error="
                << errorText.toStdString()
                << eol;
            QMessageBox::warning(this, "添加断点", errorText);
            return;
        }
        kLogEvent addBreakpointSuccessEvent;
        info << addBreakpointSuccessEvent
            << "[MemoryDock] 添加断点成功, address="
            << formatAddress(address).toStdString()
            << eol;
        rebuildBreakpointTable();
        });

    connect(m_removeBreakpointButton, &QPushButton::clicked, this, [this]() {
        const int row = m_breakpointTable->currentRow();
        if (row < 0)
        {
            return;
        }
        kLogEvent removeBreakpointEvent;
        info << removeBreakpointEvent
            << "[MemoryDock] 删除断点, row="
            << row
            << eol;
        removeBreakpointByRow(row);
        rebuildBreakpointTable();
        });

    connect(m_toggleBreakpointButton, &QPushButton::clicked, this, [this]() {
        const int row = m_breakpointTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_breakpointCache.size()))
        {
            return;
        }
        const bool nextState = !m_breakpointCache[static_cast<std::size_t>(row)].enabled;
        if (!setBreakpointEnabledByRow(row, nextState))
        {
            kLogEvent toggleBreakpointFailEvent;
            warn << toggleBreakpointFailEvent
                << "[MemoryDock] 断点状态切换失败, row="
                << row
                << ", targetEnabled="
                << (nextState ? "true" : "false")
                << eol;
            QMessageBox::warning(this, "断点切换", "切换失败，请检查权限或进程状态。");
        }
        else
        {
            kLogEvent toggleBreakpointSuccessEvent;
            info << toggleBreakpointSuccessEvent
                << "[MemoryDock] 断点状态切换成功, row="
                << row
                << ", targetEnabled="
                << (nextState ? "true" : "false")
                << eol;
        }
        rebuildBreakpointTable();
        });

    connect(m_addBookmarkButton, &QPushButton::clicked, this, [this]() {
        bool inputOk = false;
        const QString addressText = QInputDialog::getText(
            this,
            "添加书签",
            "输入书签地址",
            QLineEdit::Normal,
            "0x0",
            &inputOk);
        if (!inputOk)
        {
            kLogEvent addBookmarkCancelEvent;
            dbg << addBookmarkCancelEvent
                << "[MemoryDock] 添加书签操作已取消。"
                << eol;
            return;
        }
        std::uint64_t address = 0;
        if (!parseAddressText(addressText, address))
        {
            kLogEvent addBookmarkParseFailEvent;
            warn << addBookmarkParseFailEvent
                << "[MemoryDock] 添加书签地址解析失败, text="
                << addressText.toStdString()
                << eol;
            QMessageBox::warning(this, "添加书签", "地址格式无效。");
            return;
        }
        kLogEvent addBookmarkEvent;
        info << addBookmarkEvent
            << "[MemoryDock] 添加书签, address="
            << formatAddress(address).toStdString()
            << eol;
        addBookmarkByAddress(address, "手动添加");
        rebuildBookmarkTable();
        });

    connect(m_removeBookmarkButton, &QPushButton::clicked, this, [this]() {
        const int row = m_bookmarkTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_bookmarkCache.size()))
        {
            return;
        }
        kLogEvent removeBookmarkEvent;
        info << removeBookmarkEvent
            << "[MemoryDock] 删除书签, row="
            << row
            << ", address="
            << formatAddress(m_bookmarkCache[static_cast<std::size_t>(row)].address).toStdString()
            << eol;
        m_bookmarkCache.erase(m_bookmarkCache.begin() + row);
        rebuildBookmarkTable();
        });

    connect(m_refreshBookmarkButton, &QPushButton::clicked, this, [this]() {
        kLogEvent refreshBookmarkClickEvent;
        dbg << refreshBookmarkClickEvent
            << "[MemoryDock] 点击手动刷新书签值。"
            << eol;
        refreshBookmarkValues();
        });

    connect(m_jumpBookmarkButton, &QPushButton::clicked, this, [this]() {
        const int row = m_bookmarkTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_bookmarkCache.size()))
        {
            return;
        }
        kLogEvent jumpBookmarkEvent;
        info << jumpBookmarkEvent
            << "[MemoryDock] 跳转书签, row="
            << row
            << ", address="
            << formatAddress(m_bookmarkCache[static_cast<std::size_t>(row)].address).toStdString()
            << eol;
        jumpToAddress(m_bookmarkCache[static_cast<std::size_t>(row)].address);
        });
}

void MemoryDock::initializeStatusBar()
{
    // 初始化状态栏时输出日志，便于确认底部状态组件创建成功。
    kLogEvent statusBarInitEvent;
    info << statusBarInitEvent
        << "[MemoryDock] initializeStatusBar: 创建状态栏标签。"
        << eol;

    // 底部状态栏统一显示进程名、PID、读写能力。
    m_statusBar = new QStatusBar(this);
    m_statusBar->setSizeGripEnabled(false);

    m_statusProcessLabel = new QLabel("进程: 未附加", m_statusBar);
    m_statusPidLabel = new QLabel("PID: -", m_statusBar);
    m_statusMemoryIoLabel = new QLabel("读写状态: 未就绪", m_statusBar);
    m_statusBar->addPermanentWidget(m_statusProcessLabel, 1);
    m_statusBar->addPermanentWidget(m_statusPidLabel);
    m_statusBar->addPermanentWidget(m_statusMemoryIoLabel, 1);

    m_rootLayout->addWidget(m_statusBar);
}

void MemoryDock::initializeBookmarkRefreshTimer()
{
    // 初始化书签刷新定时器时输出日志，便于定位自动刷新触发源。
    kLogEvent timerInitEvent;
    info << timerInitEvent
        << "[MemoryDock] initializeBookmarkRefreshTimer: 启动 1 秒周期刷新。"
        << eol;

    // 书签值默认每秒刷新一次，便于观察变量变化。
    m_bookmarkRefreshTimer = new QTimer(this);
    m_bookmarkRefreshTimer->setInterval(1000);
    connect(m_bookmarkRefreshTimer, &QTimer::timeout, this, [this]() {
        // 定时器日志使用 Debug 级别，避免影响 Info 流可读性。
        kLogEvent timerTickEvent;
        dbg << timerTickEvent
            << "[MemoryDock] 书签刷新定时器触发。"
            << eol;
        refreshBookmarkValues();
        });
    m_bookmarkRefreshTimer->start();
}

