// ============================================================
// MemoryDock.ViewBreakpointUtil.inc
// 作用：
// - 负责内存查看器、断点与书签、通用格式化和解析工具函数。
// - 聚焦“可视化读写 + 调试辅助 + 文本转换”能力。
// ============================================================

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

    // 没有附加目标进程时，查看器仅显示提示，不尝试读内存。
    if (m_attachedProcessHandle == nullptr)
    {
        m_currentViewerPageBytes.clear();
        if (m_hexEditorWidget != nullptr)
        {
            m_hexEditorWidget->setEditable(false);
            m_hexEditorWidget->clearData();
        }
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
        if (m_hexEditorWidget != nullptr)
        {
            m_hexEditorWidget->setEditable(false);
            m_hexEditorWidget->clearData();
        }
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

    // 保存当前页字节并投影到统一十六进制组件。
    m_currentViewerPageBytes = pageBytes.left(static_cast<int>(bytesRead));
    if (m_hexEditorWidget != nullptr)
    {
        m_hexEditorWidget->setEditable(m_canReadWriteMemory);
        m_hexEditorWidget->setBytesPerRow(16);
        m_hexEditorWidget->setRegionData(
            m_currentViewerPageBytes.constData(),
            static_cast<std::size_t>(m_currentViewerPageBytes.size()),
            m_currentViewerAddress);
    }

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
