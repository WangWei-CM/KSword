// ============================================================
// MemoryDock.ViewBreakpointUtil.inc
// 作用：
// - 负责内存查看器、断点与书签、通用格式化和解析工具函数。
// - 聚焦“可视化读写 + 调试辅助 + 文本转换”能力。
// ============================================================

bool MemoryDock::eventFilter(QObject* watched, QEvent* event)
{
    // 只接管十六进制表格视口事件，其它对象保持 QWidget 默认分发。
    if (m_hexTable == nullptr || watched != m_hexTable->viewport() || event == nullptr)
    {
        return QWidget::eventFilter(watched, event);
    }

    // 统一的“鼠标坐标 -> 表格索引”解析函数：
    // 当鼠标拖出表格可视区域时，仍然会把坐标钳制到边界单元格，
    // 这样拖拽选择行为会更接近文本编辑器的连续选中体验。
    const auto resolveIndexByMousePos = [this](const QPoint& localPosition) -> QModelIndex {
        QModelIndex index = m_hexTable->indexAt(localPosition);
        if (index.isValid())
        {
            return index;
        }

        if (m_hexTable->rowCount() <= 0 || m_hexTable->columnCount() <= 0 || m_hexTable->model() == nullptr)
        {
            return QModelIndex();
        }

        int row = m_hexTable->rowAt(localPosition.y());
        if (row < 0)
        {
            row = (localPosition.y() < 0) ? 0 : (m_hexTable->rowCount() - 1);
        }

        int column = m_hexTable->columnAt(localPosition.x());
        if (column < 0)
        {
            column = (localPosition.x() < 0) ? 0 : (m_hexTable->columnCount() - 1);
        }

        row = std::clamp(row, 0, m_hexTable->rowCount() - 1);
        column = std::clamp(column, 0, m_hexTable->columnCount() - 1);
        return m_hexTable->model()->index(row, column);
        };

    switch (event->type())
    {
    case QEvent::MouseButtonPress:
    {
        const QMouseEvent* mouseEvent = static_cast<const QMouseEvent*>(event);
        if (mouseEvent->button() != Qt::LeftButton)
        {
            return QWidget::eventFilter(watched, event);
        }

        const QModelIndex anchorIndex = resolveIndexByMousePos(mouseEvent->pos());
        if (!anchorIndex.isValid())
        {
            return true;
        }

        const int anchorColumn = anchorIndex.column();

        // 仅在“十六进制列 / ASCII 列”启用文本式拖拽选择。
        // 地址列仍保留默认行为，不强制接管。
        if (!isHexValueColumn(anchorColumn) && !isAsciiColumn(anchorColumn))
        {
            return QWidget::eventFilter(watched, event);
        }

        // 记录拖拽锚点和模式：
        // - 起点在 ASCII 列：后续仅选中 ASCII 列（跨行）
        // - 起点在 HEX 列：后续仅选中 HEX 16 字节区域（线性）
        m_hexDragSelecting = true;
        m_hexDragStartInAscii = isAsciiColumn(anchorColumn);
        m_hexDragAnchorRow = anchorIndex.row();
        m_hexDragAnchorColumn = anchorColumn;
        applyHexViewerDragSelection(anchorIndex.row(), anchorIndex.column());

        kLogEvent dragSelectStartEvent;
        dbg << dragSelectStartEvent
            << "[MemoryDock] eventFilter: 开始十六进制拖拽选择, anchorRow="
            << m_hexDragAnchorRow
            << ", anchorColumn="
            << m_hexDragAnchorColumn
            << ", mode="
            << (m_hexDragStartInAscii ? "ASCII" : "HEX")
            << eol;
        return true;
    }
    case QEvent::MouseMove:
    {
        if (!m_hexDragSelecting)
        {
            return QWidget::eventFilter(watched, event);
        }

        const QMouseEvent* mouseEvent = static_cast<const QMouseEvent*>(event);
        if ((mouseEvent->buttons() & Qt::LeftButton) == 0)
        {
            // 理论上 MouseButtonRelease 会清理状态；
            // 这里额外兜底，避免极端情况下状态残留。
            m_hexDragSelecting = false;
            return true;
        }

        QModelIndex currentIndex = resolveIndexByMousePos(mouseEvent->pos());
        if (!currentIndex.isValid())
        {
            return true;
        }

        // 根据起点模式修正目标列：
        // ASCII 模式始终锁定 ASCII 列；
        // HEX 模式把非 HEX 列强制夹到 [1,16] 的边界。
        int adjustedColumn = currentIndex.column();
        if (m_hexDragStartInAscii)
        {
            adjustedColumn = kHexBytesPerRow + 1;
        }
        else if (!isHexValueColumn(adjustedColumn))
        {
            adjustedColumn = (adjustedColumn < 1) ? 1 : kHexBytesPerRow;
        }

        applyHexViewerDragSelection(currentIndex.row(), adjustedColumn);
        return true;
    }
    case QEvent::MouseButtonRelease:
    {
        const QMouseEvent* mouseEvent = static_cast<const QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton && m_hexDragSelecting)
        {
            QModelIndex releaseIndex = resolveIndexByMousePos(mouseEvent->pos());
            if (releaseIndex.isValid())
            {
                int releaseColumn = releaseIndex.column();
                if (m_hexDragStartInAscii)
                {
                    releaseColumn = kHexBytesPerRow + 1;
                }
                else if (!isHexValueColumn(releaseColumn))
                {
                    releaseColumn = (releaseColumn < 1) ? 1 : kHexBytesPerRow;
                }
                applyHexViewerDragSelection(releaseIndex.row(), releaseColumn);
            }

            kLogEvent dragSelectEndEvent;
            dbg << dragSelectEndEvent
                << "[MemoryDock] eventFilter: 结束十六进制拖拽选择, mode="
                << (m_hexDragStartInAscii ? "ASCII" : "HEX")
                << eol;

            m_hexDragSelecting = false;
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void MemoryDock::applyHexViewerDragSelection(int currentRow, int currentColumn)
{
    // 保护性检查：无表格/无模型/无选中模型时不执行任何动作。
    if (m_hexTable == nullptr || m_hexTable->model() == nullptr || m_hexTable->selectionModel() == nullptr)
    {
        return;
    }
    if (m_hexTable->rowCount() <= 0 || m_hexDragAnchorRow < 0 || m_hexDragAnchorColumn < 0)
    {
        return;
    }

    // 统一夹取行索引，避免拖拽到边缘外出现越界计算。
    const int maxRowIndex = m_hexTable->rowCount() - 1;
    const int anchorRow = std::clamp(m_hexDragAnchorRow, 0, maxRowIndex);
    const int targetRow = std::clamp(currentRow, 0, maxRowIndex);
    QItemSelectionModel* selectionModel = m_hexTable->selectionModel();
    selectionModel->clearSelection();

    if (m_hexDragStartInAscii)
    {
        // ASCII 模式：只允许选中 ASCII 列，行为像文本编辑器的整列跨行选中。
        const int asciiColumn = kHexBytesPerRow + 1;
        const int startRow = std::min(anchorRow, targetRow);
        const int endRow = std::max(anchorRow, targetRow);
        bool firstIndex = true;
        for (int row = startRow; row <= endRow; ++row)
        {
            const QModelIndex index = m_hexTable->model()->index(row, asciiColumn);
            if (!index.isValid())
            {
                continue;
            }

            QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Select;
            if (firstIndex)
            {
                flags |= QItemSelectionModel::Clear;
                firstIndex = false;
            }
            selectionModel->select(index, flags);
        }

        const QModelIndex currentIndex = m_hexTable->model()->index(targetRow, asciiColumn);
        if (currentIndex.isValid())
        {
            selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
        }
        return;
    }

    // HEX 模式：把锚点和终点映射为线性偏移，再按线性范围逐格选中。
    // 这会彻底避免 Qt 默认“矩形框选”行为。
    const int anchorColumn = std::clamp(m_hexDragAnchorColumn, 1, kHexBytesPerRow);
    const int targetColumn = std::clamp(currentColumn, 1, kHexBytesPerRow);
    const int anchorLinearOffset = anchorRow * kHexBytesPerRow + (anchorColumn - 1);
    const int targetLinearOffset = targetRow * kHexBytesPerRow + (targetColumn - 1);
    const int startLinearOffset = std::min(anchorLinearOffset, targetLinearOffset);
    const int endLinearOffset = std::max(anchorLinearOffset, targetLinearOffset);

    bool firstIndex = true;
    for (int linearOffset = startLinearOffset; linearOffset <= endLinearOffset; ++linearOffset)
    {
        const int row = linearOffset / kHexBytesPerRow;
        const int column = (linearOffset % kHexBytesPerRow) + 1;
        const QModelIndex index = m_hexTable->model()->index(row, column);
        if (!index.isValid())
        {
            continue;
        }

        QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Select;
        if (firstIndex)
        {
            flags |= QItemSelectionModel::Clear;
            firstIndex = false;
        }
        selectionModel->select(index, flags);
    }

    const QModelIndex currentIndex = m_hexTable->model()->index(targetRow, targetColumn);
    if (currentIndex.isValid())
    {
        selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
    }
}

bool MemoryDock::isHexValueColumn(const int column)
{
    // 十六进制值列固定为 [1,16]：0 列是地址，17 列是 ASCII。
    return column >= 1 && column <= kHexBytesPerRow;
}

bool MemoryDock::isAsciiColumn(const int column)
{
    // ASCII 列固定在十六进制 16 列之后，即索引 17。
    return column == (kHexBytesPerRow + 1);
}

void MemoryDock::jumpToAddressFromUi()
{
    // 地址跳转入口日志：记录用户输入原文。
    kLogEvent jumpFromUiEvent;
    info << jumpFromUiEvent
        << "[MemoryDock] jumpToAddressFromUi: 请求跳转, text="
        << m_viewAddressEdit->text().trimmed().toStdString()
        << eol;

    // 地址栏支持十进制或 0x 十六进制。
    std::uint64_t targetAddress = 0;
    if (!parseAddressText(m_viewAddressEdit->text().trimmed(), targetAddress))
    {
        kLogEvent jumpFromUiParseFailEvent;
        warn << jumpFromUiParseFailEvent
            << "[MemoryDock] jumpToAddressFromUi: 地址解析失败。"
            << eol;
        QMessageBox::warning(this, "地址跳转", "地址格式无效。");
        return;
    }
    jumpToAddress(targetAddress);
}

void MemoryDock::jumpToAddress(const std::uint64_t address)
{
    // 跳转日志：记录目标地址并切换页面。
    kLogEvent jumpAddressEvent;
    info << jumpAddressEvent
        << "[MemoryDock] jumpToAddress: 跳转到地址="
        << formatAddress(address).toStdString()
        << eol;

    // 记录当前查看基址，供滚动、编辑、状态栏等多个逻辑复用。
    m_currentViewerAddress = address;
    m_viewAddressEdit->setText(formatAddress(address));

    // 跳转时自动切换到 Tab4，符合“模块/区域/搜索结果双击即查看”的预期。
    m_tabWidget->setCurrentWidget(m_tabViewer);
    reloadMemoryViewerPage();
}

void MemoryDock::reloadMemoryViewerPage()
{
    // 页面重载日志：输出当前查看地址。
    kLogEvent reloadViewerStartEvent;
    dbg << reloadViewerStartEvent
        << "[MemoryDock] reloadMemoryViewerPage: 开始刷新, baseAddress="
        << formatAddress(m_currentViewerAddress).toStdString()
        << eol;

    // 每次重载都重置拖拽选择状态，避免旧状态影响新页面的鼠标选择行为。
    m_hexDragSelecting = false;
    m_hexDragStartInAscii = false;
    m_hexDragAnchorRow = -1;
    m_hexDragAnchorColumn = -1;

    // 没有附加目标进程时，查看器仅显示提示，不尝试读内存。
    if (m_attachedProcessHandle == nullptr)
    {
        m_hexTableProgrammaticUpdate = true;
        m_hexTable->clearContents();
        m_hexTable->setRowCount(kHexRowCount);
        m_hexTableProgrammaticUpdate = false;
        m_viewProtectLabel->setText("保护属性: -");
        m_viewerStatusLabel->setText("未附加进程。");
        kLogEvent reloadViewerNoAttachEvent;
        warn << reloadViewerNoAttachEvent
            << "[MemoryDock] reloadMemoryViewerPage: 未附加进程，结束刷新。"
            << eol;
        return;
    }

    // 每页固定读取 512 字节，兼顾可读性与刷新性能。
    QByteArray pageBytes(static_cast<int>(kHexPageBytes), '\0');
    SIZE_T bytesRead = 0;
    const BOOL readOk = ::ReadProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(m_currentViewerAddress)),
        pageBytes.data(),
        static_cast<SIZE_T>(kHexPageBytes),
        &bytesRead);
    if (readOk == FALSE || bytesRead == 0)
    {
        m_currentViewerPageBytes.clear();
        m_hexTableProgrammaticUpdate = true;
        m_hexTable->clearContents();
        m_hexTable->setRowCount(kHexRowCount);
        m_hexTableProgrammaticUpdate = false;
        m_viewerStatusLabel->setText(
            QString("读取失败：地址=%1，错误码=%2")
            .arg(formatAddress(m_currentViewerAddress))
            .arg(::GetLastError()));
        kLogEvent reloadViewerReadFailEvent;
        err << reloadViewerReadFailEvent
            << "[MemoryDock] reloadMemoryViewerPage: ReadProcessMemory 失败, address="
            << formatAddress(m_currentViewerAddress).toStdString()
            << ", error="
            << ::GetLastError()
            << eol;
        return;
    }

    m_currentViewerPageBytes = pageBytes.left(static_cast<int>(bytesRead));

    // 用 QSignalBlocker + 标志位双保险，避免 setItem 时触发 itemChanged 回写。
    m_hexTableProgrammaticUpdate = true;
    QSignalBlocker signalBlocker(m_hexTable);
    m_hexTable->clearContents();
    m_hexTable->setRowCount(kHexRowCount);

    for (int row = 0; row < kHexRowCount; ++row)
    {
        const std::uint64_t rowBaseAddress =
            m_currentViewerAddress + static_cast<std::uint64_t>(row * kHexBytesPerRow);
        QTableWidgetItem* rowAddressItem = new QTableWidgetItem(formatAddress(rowBaseAddress));
        rowAddressItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_hexTable->setItem(row, 0, rowAddressItem);

        QString asciiText;
        asciiText.reserve(kHexBytesPerRow);

        for (int column = 0; column < kHexBytesPerRow; ++column)
        {
            const int byteIndex = row * kHexBytesPerRow + column;
            QTableWidgetItem* byteItem = nullptr;

            if (byteIndex < m_currentViewerPageBytes.size())
            {
                const unsigned char byteValue = static_cast<unsigned char>(m_currentViewerPageBytes.at(byteIndex));
                byteItem = new QTableWidgetItem(QString("%1").arg(byteValue, 2, 16, QChar('0')).toUpper());

                // 仅在可写句柄场景下允许直接编辑十六进制单元格。
                Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
                if (m_canReadWriteMemory)
                {
                    itemFlags |= Qt::ItemIsEditable;
                }
                byteItem->setFlags(itemFlags);

                // ASCII 区域仅显示可打印字符，不可打印统一替换为 '.'。
                if (byteValue >= 32 && byteValue <= 126)
                {
                    asciiText.push_back(QChar(byteValue));
                }
                else
                {
                    asciiText.push_back('.');
                }
            }
            else
            {
                byteItem = new QTableWidgetItem("--");
                byteItem->setFlags(Qt::ItemIsEnabled);
                asciiText.push_back(' ');
            }

            m_hexTable->setItem(row, column + 1, byteItem);
        }

        QTableWidgetItem* asciiItem = new QTableWidgetItem(asciiText);
        asciiItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_hexTable->setItem(row, kHexBytesPerRow + 1, asciiItem);
    }

    m_hexTableProgrammaticUpdate = false;

    // 更新当前地址保护属性显示，帮助用户判断是否可写/可执行。
    MEMORY_BASIC_INFORMATION mbi{};
    const SIZE_T querySize = ::VirtualQueryEx(
        m_attachedProcessHandle,
        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(m_currentViewerAddress)),
        &mbi,
        sizeof(mbi));
    if (querySize == sizeof(mbi))
    {
        m_viewProtectLabel->setText(
            QString("保护属性: %1 | 状态: %2 | 类型: %3")
            .arg(protectToText(static_cast<std::uint32_t>(mbi.Protect)))
            .arg(stateToText(static_cast<std::uint32_t>(mbi.State)))
            .arg(typeToText(static_cast<std::uint32_t>(mbi.Type))));
    }
    else
    {
        m_viewProtectLabel->setText("保护属性: (查询失败)");
    }

    m_viewerStatusLabel->setText(
        QString("地址 %1 读取 %2 字节。")
        .arg(formatAddress(m_currentViewerAddress))
        .arg(bytesRead));

    // 刷新完成日志：记录本页成功读取字节数。
    kLogEvent reloadViewerFinishEvent;
    dbg << reloadViewerFinishEvent
        << "[MemoryDock] reloadMemoryViewerPage: 刷新完成, bytesRead="
        << bytesRead
        << eol;
}

bool MemoryDock::writeSingleByteAtViewer(
    const std::uint64_t absoluteAddress,
    const std::uint8_t value,
    QString& errorTextOut)
{
    // 单字节写入入口日志：输出目标地址与目标值。
    kLogEvent writeByteStartEvent;
    dbg << writeByteStartEvent
        << "[MemoryDock] writeSingleByteAtViewer: 请求写入, address="
        << formatAddress(absoluteAddress).toStdString()
        << ", value=0x"
        << QString("%1").arg(value, 2, 16, QChar('0')).toUpper().toStdString()
        << eol;

    // 未附加进程或句柄只读时，禁止写入并明确返回失败原因。
    if (m_attachedProcessHandle == nullptr)
    {
        errorTextOut = "当前未附加进程，无法写入。";
        kLogEvent writeByteNoAttachEvent;
        warn << writeByteNoAttachEvent
            << "[MemoryDock] writeSingleByteAtViewer: 未附加进程。"
            << eol;
        return false;
    }
    if (!m_canReadWriteMemory)
    {
        errorTextOut = "当前句柄为只读权限，无法写入内存。";
        kLogEvent writeByteReadonlyEvent;
        warn << writeByteReadonlyEvent
            << "[MemoryDock] writeSingleByteAtViewer: 当前句柄只读，拒绝写入。"
            << eol;
        return false;
    }

    SIZE_T bytesWritten = 0;
    const BOOL writeOk = ::WriteProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(absoluteAddress)),
        &value,
        sizeof(value),
        &bytesWritten);
    if (writeOk == FALSE || bytesWritten != sizeof(value))
    {
        errorTextOut = QString("WriteProcessMemory 失败，错误码=%1").arg(::GetLastError());
        kLogEvent writeByteFailEvent;
        err << writeByteFailEvent
            << "[MemoryDock] writeSingleByteAtViewer: 写入失败, error="
            << ::GetLastError()
            << eol;
        return false;
    }

    kLogEvent writeByteSuccessEvent;
    info << writeByteSuccessEvent
        << "[MemoryDock] writeSingleByteAtViewer: 写入成功, address="
        << formatAddress(absoluteAddress).toStdString()
        << eol;

    return true;
}

bool MemoryDock::addBreakpointByAddress(
    const std::uint64_t address,
    const QString& description,
    QString& errorTextOut)
{
    // 添加断点入口日志：记录地址和描述来源。
    kLogEvent addBreakpointStartEvent;
    info << addBreakpointStartEvent
        << "[MemoryDock] addBreakpointByAddress: 请求添加断点, address="
        << formatAddress(address).toStdString()
        << ", description="
        << description.toStdString()
        << eol;

    // 软件断点依赖 0xCC 写入，因此必须具备写内存权限。
    if (m_attachedProcessHandle == nullptr)
    {
        errorTextOut = "请先附加进程。";
        kLogEvent addBreakpointNoAttachEvent;
        warn << addBreakpointNoAttachEvent
            << "[MemoryDock] addBreakpointByAddress: 未附加进程。"
            << eol;
        return false;
    }
    if (!m_canReadWriteMemory)
    {
        errorTextOut = "当前为只读句柄，无法设置断点。";
        kLogEvent addBreakpointReadonlyEvent;
        warn << addBreakpointReadonlyEvent
            << "[MemoryDock] addBreakpointByAddress: 句柄只读。"
            << eol;
        return false;
    }

    // 避免重复写入相同地址断点，提升断点缓存一致性。
    for (const BreakpointEntry& cachedBp : m_breakpointCache)
    {
        if (cachedBp.address == address)
        {
            errorTextOut = "该地址已存在断点。";
            kLogEvent addBreakpointDuplicateEvent;
            warn << addBreakpointDuplicateEvent
                << "[MemoryDock] addBreakpointByAddress: 重复断点地址。"
                << eol;
            return false;
        }
    }

    std::uint8_t originalByte = 0;
    SIZE_T bytesRead = 0;
    const BOOL readOk = ::ReadProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
        &originalByte,
        sizeof(originalByte),
        &bytesRead);
    if (readOk == FALSE || bytesRead != sizeof(originalByte))
    {
        errorTextOut = QString("读取原始字节失败，错误码=%1").arg(::GetLastError());
        kLogEvent addBreakpointReadFailEvent;
        err << addBreakpointReadFailEvent
            << "[MemoryDock] addBreakpointByAddress: 读取原字节失败, error="
            << ::GetLastError()
            << eol;
        return false;
    }

    const std::uint8_t int3Byte = 0xCC;
    SIZE_T bytesWritten = 0;
    const BOOL writeOk = ::WriteProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(address)),
        &int3Byte,
        sizeof(int3Byte),
        &bytesWritten);
    if (writeOk == FALSE || bytesWritten != sizeof(int3Byte))
    {
        errorTextOut = QString("写入 0xCC 失败，错误码=%1").arg(::GetLastError());
        kLogEvent addBreakpointWriteFailEvent;
        err << addBreakpointWriteFailEvent
            << "[MemoryDock] addBreakpointByAddress: 写入0xCC失败, error="
            << ::GetLastError()
            << eol;
        return false;
    }

    BreakpointEntry bpEntry{};
    bpEntry.address = address;
    bpEntry.originalByte = originalByte;
    bpEntry.enabled = true;
    bpEntry.hitCount = 0;
    bpEntry.description = description;
    m_breakpointCache.push_back(std::move(bpEntry));

    kLogEvent addBreakpointFinishEvent;
    info << addBreakpointFinishEvent
        << "[MemoryDock] addBreakpointByAddress: 添加成功, totalBreakpointCount="
        << m_breakpointCache.size()
        << eol;
    return true;
}

bool MemoryDock::removeBreakpointByRow(const int row)
{
    if (row < 0 || row >= static_cast<int>(m_breakpointCache.size()))
    {
        kLogEvent removeBreakpointInvalidRowEvent;
        warn << removeBreakpointInvalidRowEvent
            << "[MemoryDock] removeBreakpointByRow: row越界, row="
            << row
            << eol;
        return false;
    }

    // 删除前若断点处于启用状态，先恢复原字节，避免目标进程留脏。
    if (m_breakpointCache[static_cast<std::size_t>(row)].enabled)
    {
        if (!setBreakpointEnabledByRow(row, false))
        {
            kLogEvent removeBreakpointDisableFailEvent;
            warn << removeBreakpointDisableFailEvent
                << "[MemoryDock] removeBreakpointByRow: 禁用断点失败, row="
                << row
                << eol;
            return false;
        }
    }

    m_breakpointCache.erase(m_breakpointCache.begin() + row);
    kLogEvent removeBreakpointFinishEvent;
    info << removeBreakpointFinishEvent
        << "[MemoryDock] removeBreakpointByRow: 删除成功, remainCount="
        << m_breakpointCache.size()
        << eol;
    return true;
}

bool MemoryDock::setBreakpointEnabledByRow(const int row, const bool enabled)
{
    if (row < 0 || row >= static_cast<int>(m_breakpointCache.size()))
    {
        kLogEvent setBreakpointInvalidRowEvent;
        warn << setBreakpointInvalidRowEvent
            << "[MemoryDock] setBreakpointEnabledByRow: row越界, row="
            << row
            << eol;
        return false;
    }
    if (m_attachedProcessHandle == nullptr || !m_canReadWriteMemory)
    {
        kLogEvent setBreakpointNoPermissionEvent;
        warn << setBreakpointNoPermissionEvent
            << "[MemoryDock] setBreakpointEnabledByRow: 句柄不可写, row="
            << row
            << eol;
        return false;
    }

    BreakpointEntry& bpEntry = m_breakpointCache[static_cast<std::size_t>(row)];
    if (bpEntry.enabled == enabled)
    {
        kLogEvent setBreakpointNoopEvent;
        dbg << setBreakpointNoopEvent
            << "[MemoryDock] setBreakpointEnabledByRow: 状态未变, row="
            << row
            << ", enabled="
            << (enabled ? "true" : "false")
            << eol;
        return true;
    }

    const std::uint8_t targetByte = enabled ? 0xCC : bpEntry.originalByte;
    SIZE_T bytesWritten = 0;
    const BOOL writeOk = ::WriteProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(bpEntry.address)),
        &targetByte,
        sizeof(targetByte),
        &bytesWritten);
    if (writeOk == FALSE || bytesWritten != sizeof(targetByte))
    {
        kLogEvent setBreakpointWriteFailEvent;
        err << setBreakpointWriteFailEvent
            << "[MemoryDock] setBreakpointEnabledByRow: 写入断点字节失败, row="
            << row
            << ", error="
            << ::GetLastError()
            << eol;
        return false;
    }

    bpEntry.enabled = enabled;
    kLogEvent setBreakpointFinishEvent;
    info << setBreakpointFinishEvent
        << "[MemoryDock] setBreakpointEnabledByRow: 切换成功, row="
        << row
        << ", enabled="
        << (enabled ? "true" : "false")
        << eol;
    return true;
}

void MemoryDock::rebuildBreakpointTable()
{
    // 重建断点表日志：记录当前断点数量。
    kLogEvent rebuildBreakpointEvent;
    dbg << rebuildBreakpointEvent
        << "[MemoryDock] rebuildBreakpointTable: 重建断点表, count="
        << m_breakpointCache.size()
        << eol;

    // 断点表直接由 m_breakpointCache 投影，避免 UI 与业务数据分叉。
    m_breakpointTable->setRowCount(static_cast<int>(m_breakpointCache.size()));
    for (int row = 0; row < static_cast<int>(m_breakpointCache.size()); ++row)
    {
        const BreakpointEntry& entry = m_breakpointCache[static_cast<std::size_t>(row)];
        m_breakpointTable->setItem(row, 0, new QTableWidgetItem(formatAddress(entry.address)));
        m_breakpointTable->setItem(row, 1, new QTableWidgetItem(
            QString("0x%1").arg(entry.originalByte, 2, 16, QChar('0')).toUpper()));
        m_breakpointTable->setItem(row, 2, new QTableWidgetItem(entry.enabled ? "启用" : "禁用"));
        m_breakpointTable->setItem(row, 3, new QTableWidgetItem(QString::number(entry.hitCount)));
        m_breakpointTable->setItem(row, 4, new QTableWidgetItem(entry.description));
    }
}

void MemoryDock::addBookmarkByAddress(const std::uint64_t address, const QString& noteText)
{
    // 添加书签入口日志：记录地址与备注。
    kLogEvent addBookmarkStartEvent;
    info << addBookmarkStartEvent
        << "[MemoryDock] addBookmarkByAddress: 请求添加书签, address="
        << formatAddress(address).toStdString()
        << ", note="
        << noteText.toStdString()
        << eol;

    // 已存在同地址书签时仅更新备注，不重复插入。
    for (BookmarkEntry& bookmark : m_bookmarkCache)
    {
        if (bookmark.address == address)
        {
            bookmark.noteText = noteText;
            kLogEvent addBookmarkUpdateEvent;
            dbg << addBookmarkUpdateEvent
                << "[MemoryDock] addBookmarkByAddress: 已存在地址，更新备注。"
                << eol;
            return;
        }
    }

    BookmarkEntry bookmark{};
    bookmark.address = address;
    bookmark.noteText = noteText;
    bookmark.addTimeText = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    // 添加时尽力读取一份初始值，失败不阻断添加流程。
    if (m_attachedProcessHandle != nullptr)
    {
        QByteArray initialBytes(8, '\0');
        SIZE_T bytesRead = 0;
        if (::ReadProcessMemory(
            m_attachedProcessHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
            initialBytes.data(),
            static_cast<SIZE_T>(initialBytes.size()),
            &bytesRead) != FALSE && bytesRead > 0)
        {
            bookmark.lastValueBytes = initialBytes.left(static_cast<int>(bytesRead));
        }
    }

    m_bookmarkCache.push_back(std::move(bookmark));
    kLogEvent addBookmarkFinishEvent;
    info << addBookmarkFinishEvent
        << "[MemoryDock] addBookmarkByAddress: 添加完成, totalBookmarkCount="
        << m_bookmarkCache.size()
        << eol;
}

void MemoryDock::rebuildBookmarkTable()
{
    // 重建书签表日志：记录当前书签数量。
    kLogEvent rebuildBookmarkEvent;
    dbg << rebuildBookmarkEvent
        << "[MemoryDock] rebuildBookmarkTable: 重建书签表, count="
        << m_bookmarkCache.size()
        << eol;

    // 书签表每次全量重建，逻辑清晰且数量通常不大，维护成本最低。
    m_bookmarkTable->setRowCount(static_cast<int>(m_bookmarkCache.size()));
    for (int row = 0; row < static_cast<int>(m_bookmarkCache.size()); ++row)
    {
        const BookmarkEntry& bookmark = m_bookmarkCache[static_cast<std::size_t>(row)];
        m_bookmarkTable->setItem(row, 0, new QTableWidgetItem(formatAddress(bookmark.address)));

        // 书签“当前值”默认按十六进制字节串展示，通用于未知变量类型。
        const QString valueText = bytesToDisplayString(bookmark.lastValueBytes, SearchValueType::ByteArray);
        m_bookmarkTable->setItem(row, 1, new QTableWidgetItem(valueText));
        m_bookmarkTable->setItem(row, 2, new QTableWidgetItem(bookmark.noteText));
        m_bookmarkTable->setItem(row, 3, new QTableWidgetItem(bookmark.addTimeText));
    }
}

void MemoryDock::refreshBookmarkValues()
{
    // 刷新书签值入口日志：记录当前书签规模。
    kLogEvent refreshBookmarkStartEvent;
    dbg << refreshBookmarkStartEvent
        << "[MemoryDock] refreshBookmarkValues: 开始刷新, bookmarkCount="
        << m_bookmarkCache.size()
        << eol;

    // 未附加进程或没有书签时无需刷新，直接返回避免无意义系统调用。
    if (m_attachedProcessHandle == nullptr || m_bookmarkCache.empty())
    {
        kLogEvent refreshBookmarkSkipEvent;
        dbg << refreshBookmarkSkipEvent
            << "[MemoryDock] refreshBookmarkValues: 跳过刷新（未附加或无书签）。"
            << eol;
        return;
    }

    for (BookmarkEntry& bookmark : m_bookmarkCache)
    {
        const int requestLength = std::max<int>(8, bookmark.lastValueBytes.size());
        QByteArray valueBytes(requestLength, '\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ::ReadProcessMemory(
            m_attachedProcessHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(bookmark.address)),
            valueBytes.data(),
            static_cast<SIZE_T>(valueBytes.size()),
            &bytesRead);
        if (readOk != FALSE && bytesRead > 0)
        {
            bookmark.lastValueBytes = valueBytes.left(static_cast<int>(bytesRead));
        }
    }

    // 刷新完成后重建表格，保证用户看到的是最新读回值。
    rebuildBookmarkTable();

    kLogEvent refreshBookmarkFinishEvent;
    dbg << refreshBookmarkFinishEvent
        << "[MemoryDock] refreshBookmarkValues: 刷新完成。"
        << eol;
}

void MemoryDock::updateStatusBarText()
{
    // 状态栏刷新日志：记录当前 PID 与权限状态。
    kLogEvent statusUpdateEvent;
    dbg << statusUpdateEvent
        << "[MemoryDock] updateStatusBarText: attachedPid="
        << m_attachedPid
        << ", canReadWrite="
        << (m_canReadWriteMemory ? "true" : "false")
        << eol;

    // 状态栏由三段组成：进程名、PID、读写状态，任何状态变化都统一经此函数刷新。
    if (m_attachedPid == 0 || m_attachedProcessHandle == nullptr)
    {
        m_statusProcessLabel->setText("进程: 未附加");
        m_statusPidLabel->setText("PID: -");
        m_statusMemoryIoLabel->setText("内存读写: 未就绪");
        return;
    }

    m_statusProcessLabel->setText(QString("进程: %1").arg(m_attachedProcessName));
    m_statusPidLabel->setText(QString("PID: %1").arg(m_attachedPid));
    m_statusMemoryIoLabel->setText(
        QString("内存读写: %1").arg(m_canReadWriteMemory ? "可读可写" : "只读"));
}

bool MemoryDock::parseAddressText(const QString& text, std::uint64_t& valueOut)
{
    // 地址解析日志：保留输入文本用于定位格式问题。
    kLogEvent parseAddressEvent;
    dbg << parseAddressEvent
        << "[MemoryDock] parseAddressText: text="
        << text.trimmed().toStdString()
        << eol;

    // 地址解析与通用无符号整数解析共享一套规则。
    return parseUnsignedNumber(text, valueOut);
}

bool MemoryDock::parseUnsignedNumber(const QString& text, std::uint64_t& valueOut)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
    {
        return false;
    }

    bool parseOk = false;
    qulonglong parsedValue = 0;

    // 优先处理 0x 前缀（十六进制）形式。
    if (trimmed.startsWith("0x", Qt::CaseInsensitive))
    {
        parsedValue = trimmed.mid(2).toULongLong(&parseOk, 16);
        if (!parseOk)
        {
            return false;
        }
        valueOut = static_cast<std::uint64_t>(parsedValue);
        return true;
    }

    // 再尝试十进制；失败后再尝试“无前缀十六进制”以兼容输入习惯。
    parsedValue = trimmed.toULongLong(&parseOk, 10);
    if (!parseOk)
    {
        parsedValue = trimmed.toULongLong(&parseOk, 16);
        if (!parseOk)
        {
            return false;
        }
    }

    valueOut = static_cast<std::uint64_t>(parsedValue);
    return true;
}

QString MemoryDock::formatAddress(const std::uint64_t address)
{
    // 统一输出 16 位十六进制，便于 32/64 位地址在表格中对齐阅读。
    const QString hexText = QString("%1").arg(
        static_cast<qulonglong>(address),
        16,
        16,
        QChar('0')).toUpper();
    return QString("0x%1").arg(hexText);
}

QString MemoryDock::formatSize(const std::uint64_t sizeBytes)
{
    // 字节数可读化显示：B / KB / MB / GB 自动切换，保留两位小数。
    constexpr double kKB = 1024.0;
    constexpr double kMB = 1024.0 * 1024.0;
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

    const double sizeValue = static_cast<double>(sizeBytes);
    if (sizeValue >= kGB)
    {
        return QString("%1 GB").arg(sizeValue / kGB, 0, 'f', 2);
    }
    if (sizeValue >= kMB)
    {
        return QString("%1 MB").arg(sizeValue / kMB, 0, 'f', 2);
    }
    if (sizeValue >= kKB)
    {
        return QString("%1 KB").arg(sizeValue / kKB, 0, 'f', 2);
    }
    return QString("%1 B").arg(sizeBytes);
}

QString MemoryDock::protectToText(const std::uint32_t protect)
{
    // PAGE_* 主值仅保留低 8 位；高位是 PAGE_GUARD/NOCACHE 等修饰位。
    const std::uint32_t baseProtect = protect & 0xFF;
    QString baseText;
    switch (baseProtect)
    {
    case PAGE_NOACCESS:          baseText = "---"; break;
    case PAGE_READONLY:          baseText = "R--"; break;
    case PAGE_READWRITE:         baseText = "RW-"; break;
    case PAGE_WRITECOPY:         baseText = "RC-"; break;
    case PAGE_EXECUTE:           baseText = "--X"; break;
    case PAGE_EXECUTE_READ:      baseText = "R-X"; break;
    case PAGE_EXECUTE_READWRITE: baseText = "RWX"; break;
    case PAGE_EXECUTE_WRITECOPY: baseText = "RCX"; break;
    default:                     baseText = "UNK"; break;
    }

    // 叠加修饰标记，帮助用户识别 Guard/NoCache/WriteCombine 特性。
    if ((protect & PAGE_GUARD) != 0)
    {
        baseText += "|G";
    }
    if ((protect & PAGE_NOCACHE) != 0)
    {
        baseText += "|NC";
    }
    if ((protect & PAGE_WRITECOMBINE) != 0)
    {
        baseText += "|WC";
    }

    return baseText;
}

QString MemoryDock::stateToText(const std::uint32_t state)
{
    switch (state)
    {
    case MEM_COMMIT:  return "MEM_COMMIT";
    case MEM_RESERVE: return "MEM_RESERVE";
    case MEM_FREE:    return "MEM_FREE";
    default:          return QString("UNKNOWN(0x%1)").arg(state, 0, 16);
    }
}

QString MemoryDock::typeToText(const std::uint32_t type)
{
    switch (type)
    {
    case MEM_IMAGE:   return "IMAGE";
    case MEM_MAPPED:  return "MAPPED";
    case MEM_PRIVATE: return "PRIVATE";
    case 0:           return "-";
    default:          return QString("UNKNOWN(0x%1)").arg(type, 0, 16);
    }
}

QString MemoryDock::bytesToDisplayString(const QByteArray& bytes, const SearchValueType valueType)
{
    // 空字节统一显示短横线，避免表格出现空白造成歧义。
    if (bytes.isEmpty())
    {
        return "-";
    }

    switch (valueType)
    {
    case SearchValueType::Byte:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::uint8_t))) return "-";
        std::uint8_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString("%1 (0x%2)")
            .arg(value)
            .arg(value, 2, 16, QChar('0')).toUpper();
    }
    case SearchValueType::Int16:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::int16_t))) return "-";
        std::int16_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value);
    }
    case SearchValueType::Int32:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::int32_t))) return "-";
        std::int32_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value);
    }
    case SearchValueType::Int64:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::int64_t))) return "-";
        std::int64_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value);
    }
    case SearchValueType::Float32:
    {
        if (bytes.size() < static_cast<int>(sizeof(float))) return "-";
        float value = 0.0f;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value, 'f', 6);
    }
    case SearchValueType::Float64:
    {
        if (bytes.size() < static_cast<int>(sizeof(double))) return "-";
        double value = 0.0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value, 'f', 8);
    }
    case SearchValueType::StringAscii:
    {
        return QString::fromLatin1(bytes);
    }
    case SearchValueType::StringUnicode:
    {
        // UTF-16 字节长度需为偶数，不足时截断最后 1 字节防止越界。
        const int alignedLength = bytes.size() - (bytes.size() % 2);
        if (alignedLength <= 0)
        {
            return "-";
        }
        return QString::fromUtf16(
            reinterpret_cast<const char16_t*>(bytes.constData()),
            alignedLength / 2);
    }
    case SearchValueType::ByteArray:
    default:
    {
        // 默认按十六进制字节串展示，兼容未知类型与书签显示。
        QStringList parts;
        parts.reserve(bytes.size());
        for (int index = 0; index < bytes.size(); ++index)
        {
            const auto byteValue = static_cast<unsigned char>(bytes.at(index));
            parts.push_back(QString("%1").arg(byteValue, 2, 16, QChar('0')).toUpper());
        }
        return parts.join(' ');
    }
    }
}
