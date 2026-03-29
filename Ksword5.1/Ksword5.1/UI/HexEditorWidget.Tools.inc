// ============================================================
// HexEditorWidget.Tools.inc
// 作用：承载查找解析、异步搜索、复制导出等工具能力。
// ============================================================

bool HexEditorWidget::parseAddressNumber(const QString& text, std::uint64_t& valueOut) const
{
    const QString trimmedText = text.trimmed();
    if (trimmedText.isEmpty())
    {
        return false;
    }

    bool parseOk = false;
    qulonglong parsedValue = 0;

    if (trimmedText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    {
        parsedValue = trimmedText.mid(2).toULongLong(&parseOk, 16);
        if (!parseOk)
        {
            return false;
        }
        valueOut = static_cast<std::uint64_t>(parsedValue);
        return true;
    }

    parsedValue = trimmedText.toULongLong(&parseOk, 10);
    if (!parseOk)
    {
        parsedValue = trimmedText.toULongLong(&parseOk, 16);
        if (!parseOk)
        {
            return false;
        }
    }

    valueOut = static_cast<std::uint64_t>(parsedValue);
    return true;
}

bool HexEditorWidget::parseSearchPattern(
    const SearchMode mode,
    const QString& text,
    SearchPattern& patternOut,
    QString& errorTextOut) const
{
    if (mode == SearchMode::HexBytes)
    {
        return parseHexPattern(text, patternOut, errorTextOut);
    }
    return parseAsciiPattern(mode, text, patternOut, errorTextOut);
}

bool HexEditorWidget::parseHexPattern(
    const QString& text,
    SearchPattern& patternOut,
    QString& errorTextOut) const
{
    const QString normalizedText = text.trimmed().toUpper();
    if (normalizedText.isEmpty())
    {
        errorTextOut = QStringLiteral("HEX 查找输入为空。");
        return false;
    }

    const QStringList tokenList = normalizedText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (tokenList.isEmpty())
    {
        errorTextOut = QStringLiteral("HEX 查找输入为空。");
        return false;
    }

    QByteArray patternBytes;
    QByteArray maskBytes;
    patternBytes.reserve(tokenList.size());
    maskBytes.reserve(tokenList.size());

    for (const QString& token : tokenList)
    {
        if (token == QStringLiteral("?") || token == QStringLiteral("??"))
        {
            patternBytes.push_back('\0');
            maskBytes.push_back('\0');
            continue;
        }

        bool parseOk = false;
        const int byteValue = token.toInt(&parseOk, 16);
        if (!parseOk || byteValue < 0 || byteValue > 0xFF)
        {
            errorTextOut = QStringLiteral("HEX token 无效: %1").arg(token);
            return false;
        }

        patternBytes.push_back(static_cast<char>(byteValue));
        maskBytes.push_back('\1');
    }

    patternOut.patternBytes = patternBytes;
    patternOut.maskBytes = maskBytes;
    patternOut.normalizeText = tokenList.join(' ');
    return true;
}

bool HexEditorWidget::parseAsciiPattern(
    const SearchMode mode,
    const QString& text,
    SearchPattern& patternOut,
    QString& errorTextOut) const
{
    const QString normalizedText = text;
    if (normalizedText.isEmpty())
    {
        errorTextOut = QStringLiteral("文本查找输入为空。");
        return false;
    }

    QByteArray patternBytes;
    if (mode == SearchMode::AsciiText)
    {
        patternBytes = normalizedText.toUtf8();
    }
    else
    {
        patternBytes.resize(normalizedText.size() * static_cast<int>(sizeof(char16_t)));
        std::memcpy(patternBytes.data(), normalizedText.utf16(), static_cast<std::size_t>(patternBytes.size()));
    }

    if (patternBytes.isEmpty())
    {
        errorTextOut = QStringLiteral("文本查找输入为空。");
        return false;
    }

    QByteArray maskBytes(patternBytes.size(), '\1');
    patternOut.patternBytes = patternBytes;
    patternOut.maskBytes = maskBytes;
    patternOut.normalizeText = normalizedText;
    return true;
}

bool HexEditorWidget::ensureSearchResultReady(
    const NavigateDirection direction,
    const std::uint64_t startOffset)
{
    if (m_findEdit == nullptr || m_findModeCombo == nullptr)
    {
        return false;
    }

    if (m_buffer.isEmpty())
    {
        updateStatusLabel(QStringLiteral("查找失败：当前无数据。"));
        return false;
    }

    const SearchMode mode = static_cast<SearchMode>(m_findModeCombo->currentData().toInt());
    const QString searchText = m_findEdit->text();

    SearchPattern searchPattern;
    QString errorText;
    if (!parseSearchPattern(mode, searchText, searchPattern, errorText))
    {
        updateStatusLabel(QStringLiteral("查找失败：%1").arg(errorText));
        return false;
    }

    const bool samePattern =
        (mode == m_lastSearchMode) &&
        (searchText == m_lastSearchText) &&
        (m_searchReadyRevision == m_bufferRevision);

    if (samePattern)
    {
        return true;
    }

    startSearchAsync(searchPattern, mode, searchText, direction, startOffset);
    return false;
}

void HexEditorWidget::startSearchAsync(
    const SearchPattern& pattern,
    const SearchMode mode,
    const QString& searchText,
    const NavigateDirection direction,
    const std::uint64_t startOffset)
{
    if (pattern.patternBytes.isEmpty())
    {
        updateStatusLabel(QStringLiteral("查找失败：查找模板为空。"));
        return;
    }

    const QByteArray dataSnapshot = m_buffer;
    const QByteArray patternSnapshot = pattern.patternBytes;
    const QByteArray maskSnapshot = pattern.maskBytes;
    const QString normalizedText = pattern.normalizeText;
    const std::uint64_t ticket = ++m_searchTicket;
    const std::uint64_t dataRevision = m_bufferRevision;

    m_searchRunning = true;
    m_pendingDirection = direction;
    m_pendingStartOffset = startOffset;

    m_findResultLabel->setText(QStringLiteral("命中: 搜索中..."));
    updateStatusLabel(QStringLiteral("查找中：正在后台扫描，不阻塞界面。"));

    // 异步扫描：复制快照后在线程中搜索，完成后回主线程提交。
    QPointer<HexEditorWidget> guardThis(this);
    std::thread([guardThis,
        ticket,
        dataSnapshot,
        patternSnapshot,
        maskSnapshot,
        mode,
        searchText,
        normalizedText,
        dataRevision]()
        {
            std::vector<std::uint64_t> matchOffsets;
            const int patternLength = patternSnapshot.size();

            if (patternLength > 0 && dataSnapshot.size() >= patternLength)
            {
                const std::size_t maxStart =
                    static_cast<std::size_t>(dataSnapshot.size() - patternLength);

                for (std::size_t startIndex = 0; startIndex <= maxStart; ++startIndex)
                {
                    bool matched = true;
                    for (int patternIndex = 0; patternIndex < patternLength; ++patternIndex)
                    {
                        if (maskSnapshot.at(patternIndex) == '\0')
                        {
                            continue;
                        }

                        const char dataByte = dataSnapshot.at(static_cast<int>(startIndex) + patternIndex);
                        const char patternByte = patternSnapshot.at(patternIndex);
                        if (dataByte != patternByte)
                        {
                            matched = false;
                            break;
                        }
                    }

                    if (matched)
                    {
                        matchOffsets.push_back(static_cast<std::uint64_t>(startIndex));
                    }
                }
            }

            if (guardThis == nullptr)
            {
                return;
            }

            QMetaObject::invokeMethod(
                guardThis.data(),
                [guardThis,
                    ticket,
                    matchOffsets,
                    patternLength,
                    mode,
                    searchText,
                    normalizedText,
                    dataRevision]() mutable
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }

                    guardThis->applySearchResult(
                        ticket,
                        matchOffsets,
                        patternLength,
                        mode,
                        searchText,
                        normalizedText,
                        dataRevision);
                },
                Qt::QueuedConnection);
        }).detach();
}

void HexEditorWidget::applySearchResult(
    const std::uint64_t ticket,
    const std::vector<std::uint64_t>& matchOffsets,
    const int matchLength,
    const SearchMode mode,
    const QString& searchText,
    const QString& normalizedText,
    const std::uint64_t dataRevision)
{
    // 只接受最新 ticket 结果，防止旧线程回写覆盖新状态。
    if (ticket != m_searchTicket.load())
    {
        return;
    }

    m_searchRunning = false;
    m_matchOffsets = matchOffsets;
    m_currentMatchLength = std::max(1, matchLength);
    m_currentMatchIndex = m_matchOffsets.empty() ? -1 : 0;

    m_lastSearchMode = mode;
    m_lastSearchText = searchText;
    m_lastSearchNormalizeText = normalizedText;
    m_searchReadyRevision = dataRevision;

    // 构建命中掩码，加速单元格高亮判断。
    m_matchMask = QByteArray(m_buffer.size(), '\0');
    for (const std::uint64_t matchOffset : m_matchOffsets)
    {
        for (int index = 0; index < m_currentMatchLength; ++index)
        {
            const std::uint64_t byteOffset = matchOffset + static_cast<std::uint64_t>(index);
            if (byteOffset >= static_cast<std::uint64_t>(m_matchMask.size()))
            {
                break;
            }
            m_matchMask[static_cast<int>(byteOffset)] = '\1';
        }
    }

    // 重绘高亮并执行待处理跳转。
    rebuildTable();

    m_findResultLabel->setText(
        QStringLiteral("命中: %1").arg(static_cast<qulonglong>(m_matchOffsets.size())));

    if (m_matchOffsets.empty())
    {
        updateStatusLabel(QStringLiteral("查找完成：未找到匹配项。"));
        return;
    }

    gotoMatchByDirection(m_pendingDirection, m_pendingStartOffset);
}

void HexEditorWidget::clearSearchState()
{
    m_matchMask = QByteArray(m_buffer.size(), '\0');
    m_matchOffsets.clear();
    m_currentMatchIndex = -1;
    m_currentMatchLength = 0;

    m_lastSearchText.clear();
    m_lastSearchNormalizeText.clear();
    m_lastSearchMode = SearchMode::HexBytes;
    m_searchReadyRevision = 0;

    m_pendingDirection = NavigateDirection::None;
    m_pendingStartOffset = 0;

    if (m_findResultLabel != nullptr)
    {
        m_findResultLabel->setText(QStringLiteral("命中: -"));
    }
}

void HexEditorWidget::gotoMatchByDirection(
    const NavigateDirection direction,
    const std::uint64_t startOffset)
{
    if (m_matchOffsets.empty())
    {
        return;
    }

    m_pendingDirection = NavigateDirection::None;

    int targetIndex = 0;
    if (direction == NavigateDirection::Previous)
    {
        targetIndex = static_cast<int>(m_matchOffsets.size()) - 1;
        for (int index = static_cast<int>(m_matchOffsets.size()) - 1; index >= 0; --index)
        {
            if (m_matchOffsets[static_cast<std::size_t>(index)] <= startOffset)
            {
                targetIndex = index;
                break;
            }
        }
    }
    else
    {
        targetIndex = 0;
        for (int index = 0; index < static_cast<int>(m_matchOffsets.size()); ++index)
        {
            if (m_matchOffsets[static_cast<std::size_t>(index)] >= startOffset)
            {
                targetIndex = index;
                break;
            }
        }
    }

    setCurrentMatchIndex(targetIndex);
}

void HexEditorWidget::setCurrentMatchIndex(const int matchIndex)
{
    if (matchIndex < 0 || matchIndex >= static_cast<int>(m_matchOffsets.size()))
    {
        return;
    }

    m_currentMatchIndex = matchIndex;

    const std::uint64_t matchOffset = m_matchOffsets[static_cast<std::size_t>(m_currentMatchIndex)];
    selectRangeByOffset(matchOffset, std::max(1, m_currentMatchLength), true);

    // 刷新当前命中强调色。
    const int rowIndex = static_cast<int>(matchOffset / static_cast<std::uint64_t>(m_bytesPerRow));
    updateRowHighlightByRow(rowIndex);

    m_findResultLabel->setText(
        QStringLiteral("命中: %1 / %2")
        .arg(m_currentMatchIndex + 1)
        .arg(static_cast<int>(m_matchOffsets.size())));

    updateStatusLabel(
        QStringLiteral("已定位命中 %1/%2，地址=%3")
        .arg(m_currentMatchIndex + 1)
        .arg(static_cast<int>(m_matchOffsets.size()))
        .arg(QStringLiteral("0x%1").arg(static_cast<qulonglong>(m_baseAddress + matchOffset), 16, 16, QChar('0')).toUpper()));
}

void HexEditorWidget::selectRangeByOffset(
    const std::uint64_t offset,
    const int length,
    const bool scrollToCenter)
{
    if (m_hexTable == nullptr || length <= 0)
    {
        return;
    }

    m_hexTable->clearSelection();

    bool firstCell = true;
    for (int index = 0; index < length; ++index)
    {
        const std::uint64_t currentOffset = offset + static_cast<std::uint64_t>(index);
        int row = 0;
        int column = 0;
        if (!offsetToRowColumn(currentOffset, row, column))
        {
            break;
        }

        QTableWidgetItem* item = m_hexTable->item(row, column);
        if (item == nullptr)
        {
            continue;
        }

        item->setSelected(true);
        if (firstCell)
        {
            m_hexTable->setCurrentCell(row, column);
            if (scrollToCenter)
            {
                m_hexTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            }
            firstCell = false;
        }
    }

    // 记录锚点，供 Shift + 鼠标继续扩展文本式选区。
    m_linearSelectAnchorOffset = offset;
    m_linearSelectAnchorValid = true;
}

void HexEditorWidget::selectLinearRange(
    const std::uint64_t anchorOffset,
    const std::uint64_t currentOffset,
    const bool scrollToCurrent)
{
    if (m_hexTable == nullptr || m_buffer.isEmpty())
    {
        return;
    }

    const std::uint64_t maxOffset = static_cast<std::uint64_t>(m_buffer.size() - 1);
    const std::uint64_t safeAnchorOffset = std::min(anchorOffset, maxOffset);
    const std::uint64_t safeCurrentOffset = std::min(currentOffset, maxOffset);
    const std::uint64_t rangeStartOffset = std::min(safeAnchorOffset, safeCurrentOffset);
    const std::uint64_t rangeEndOffset = std::max(safeAnchorOffset, safeCurrentOffset);
    const int startRowIndex = static_cast<int>(rangeStartOffset / static_cast<std::uint64_t>(m_bytesPerRow));
    const int endRowIndex = static_cast<int>(rangeEndOffset / static_cast<std::uint64_t>(m_bytesPerRow));

    m_hexTable->clearSelection();

    for (std::uint64_t byteOffset = rangeStartOffset; byteOffset <= rangeEndOffset; ++byteOffset)
    {
        int rowIndex = 0;
        int columnIndex = 0;
        if (!offsetToRowColumn(byteOffset, rowIndex, columnIndex))
        {
            continue;
        }

        QTableWidgetItem* byteItem = m_hexTable->item(rowIndex, columnIndex);
        if (byteItem != nullptr)
        {
            byteItem->setSelected(true);
        }

        if (byteOffset == safeCurrentOffset)
        {
            m_hexTable->setCurrentCell(rowIndex, columnIndex);
            if (scrollToCurrent && byteItem != nullptr)
            {
                m_hexTable->scrollToItem(byteItem, QAbstractItemView::EnsureVisible);
            }
        }
    }

    // 同步选中 ASCII 列对应行，确保从 ASCII 区域拖拽时有可见反馈。
    for (int rowIndex = startRowIndex; rowIndex <= endRowIndex; ++rowIndex)
    {
        QTableWidgetItem* asciiItem = m_hexTable->item(rowIndex, m_bytesPerRow + 1);
        if (asciiItem != nullptr)
        {
            asciiItem->setSelected(true);
        }
    }

    m_linearSelectAnchorOffset = safeAnchorOffset;
    m_linearSelectAnchorValid = true;
}

bool HexEditorWidget::rowColumnToOffset(
    const int row,
    const int column,
    std::uint64_t& offsetOut) const
{
    if (row < 0 || column <= 0)
    {
        return false;
    }

    // ASCII 列统一映射到“该行首字节”，避免右键/当前单元格在 ASCII 列时失效。
    int normalizedColumn = column;
    if (normalizedColumn == m_bytesPerRow + 1)
    {
        normalizedColumn = 1;
    }
    if (normalizedColumn > m_bytesPerRow)
    {
        return false;
    }

    const std::uint64_t offset =
        static_cast<std::uint64_t>(row) * static_cast<std::uint64_t>(m_bytesPerRow) +
        static_cast<std::uint64_t>(normalizedColumn - 1);

    if (offset >= static_cast<std::uint64_t>(m_buffer.size()))
    {
        return false;
    }

    offsetOut = offset;
    return true;
}

bool HexEditorWidget::offsetToRowColumn(
    const std::uint64_t offset,
    int& rowOut,
    int& columnOut) const
{
    if (offset >= static_cast<std::uint64_t>(m_buffer.size()))
    {
        return false;
    }

    rowOut = static_cast<int>(offset / static_cast<std::uint64_t>(m_bytesPerRow));
    columnOut = static_cast<int>(offset % static_cast<std::uint64_t>(m_bytesPerRow)) + 1;
    return true;
}

bool HexEditorWidget::viewportPosToOffset(
    const QPoint& viewportPos,
    std::uint64_t& offsetOut) const
{
    if (m_hexTable == nullptr || m_buffer.isEmpty())
    {
        return false;
    }

    const int rowCount = m_hexTable->rowCount();
    if (rowCount <= 0)
    {
        return false;
    }

    int rowIndex = m_hexTable->rowAt(viewportPos.y());
    if (rowIndex < 0)
    {
        rowIndex = (viewportPos.y() < 0) ? 0 : (rowCount - 1);
    }
    rowIndex = std::clamp(rowIndex, 0, rowCount - 1);

    int columnIndex = m_hexTable->columnAt(viewportPos.x());
    if (columnIndex < 1)
    {
        columnIndex = 1;
    }

    // ASCII 列按字符位置映射到具体字节，修复“ASCII 区域无法跨行连续选择”。
    if (columnIndex == m_bytesPerRow + 1)
    {
        const std::uint64_t rowStartOffset =
            static_cast<std::uint64_t>(rowIndex) * static_cast<std::uint64_t>(m_bytesPerRow);
        const std::uint64_t remainBytes =
            static_cast<std::uint64_t>(m_buffer.size()) > rowStartOffset
            ? static_cast<std::uint64_t>(m_buffer.size()) - rowStartOffset
            : 0;
        const int validBytesInRow = static_cast<int>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(m_bytesPerRow), remainBytes));
        if (validBytesInRow <= 0)
        {
            return false;
        }

        const QModelIndex asciiModelIndex = m_hexTable->model()->index(rowIndex, m_bytesPerRow + 1);
        const QRect asciiRect = m_hexTable->visualRect(asciiModelIndex);
        const QFontMetrics fontMetrics(m_hexTable->font());
        const int charWidth = std::max(1, fontMetrics.horizontalAdvance(QChar('M')));
        const int relativeX = std::max(0, viewportPos.x() - asciiRect.left());
        int charIndex = relativeX / charWidth;
        charIndex = std::clamp(charIndex, 0, validBytesInRow - 1);

        offsetOut = rowStartOffset + static_cast<std::uint64_t>(charIndex);
        return true;
    }

    if (columnIndex > m_bytesPerRow)
    {
        columnIndex = m_bytesPerRow;
    }

    const std::uint64_t rawOffset =
        static_cast<std::uint64_t>(rowIndex) * static_cast<std::uint64_t>(m_bytesPerRow)
        + static_cast<std::uint64_t>(columnIndex - 1);
    const std::uint64_t maxOffset = static_cast<std::uint64_t>(m_buffer.size() - 1);
    offsetOut = std::min(rawOffset, maxOffset);
    return true;
}

std::vector<std::uint64_t> HexEditorWidget::collectSelectedOffsets() const
{
    std::vector<std::uint64_t> offsetList;
    if (m_hexTable == nullptr)
    {
        return offsetList;
    }

    // 直接读取 selectedIndexes，避免 selectedRanges 的矩形合并带来额外字节。
    const QModelIndexList selectedIndexList = m_hexTable->selectionModel()->selectedIndexes();
    for (const QModelIndex& modelIndex : selectedIndexList)
    {
        if (!modelIndex.isValid())
        {
            continue;
        }

        std::uint64_t offset = 0;
        if (!rowColumnToOffset(modelIndex.row(), modelIndex.column(), offset))
        {
            continue;
        }
        offsetList.push_back(offset);
    }

    // 未选择时回退当前单元格。
    if (offsetList.empty())
    {
        const int currentRow = m_hexTable->currentRow();
        const int currentColumn = m_hexTable->currentColumn();
        std::uint64_t currentOffset = 0;
        if (rowColumnToOffset(currentRow, currentColumn, currentOffset))
        {
            offsetList.push_back(currentOffset);
        }
    }

    std::sort(offsetList.begin(), offsetList.end());
    offsetList.erase(std::unique(offsetList.begin(), offsetList.end()), offsetList.end());
    return offsetList;
}

void HexEditorWidget::copySelectedAsHex()
{
    const std::vector<std::uint64_t> offsetList = collectSelectedOffsets();
    if (offsetList.empty())
    {
        updateStatusLabel(QStringLiteral("复制失败：未选中有效字节。"));
        return;
    }

    QStringList byteTextList;
    byteTextList.reserve(static_cast<int>(offsetList.size()));
    for (const std::uint64_t offset : offsetList)
    {
        const std::uint8_t byteValue = static_cast<std::uint8_t>(m_buffer.at(static_cast<int>(offset)));
        byteTextList.push_back(byteToHexText(byteValue));
    }

    if (QApplication::clipboard() != nullptr)
    {
        QApplication::clipboard()->setText(byteTextList.join(' '));
    }

    updateStatusLabel(
        QStringLiteral("已复制 %1 字节 HEX。")
        .arg(static_cast<qulonglong>(offsetList.size())));
}

void HexEditorWidget::copySelectedAsAscii()
{
    const std::vector<std::uint64_t> offsetList = collectSelectedOffsets();
    if (offsetList.empty())
    {
        updateStatusLabel(QStringLiteral("复制失败：未选中有效字节。"));
        return;
    }

    QString asciiText;
    asciiText.reserve(static_cast<int>(offsetList.size()));
    for (const std::uint64_t offset : offsetList)
    {
        const std::uint8_t byteValue = static_cast<std::uint8_t>(m_buffer.at(static_cast<int>(offset)));
        const bool printable = (byteValue >= 32 && byteValue <= 126);
        asciiText.push_back(printable ? QChar(byteValue) : QChar('.'));
    }

    if (QApplication::clipboard() != nullptr)
    {
        QApplication::clipboard()->setText(asciiText);
    }

    updateStatusLabel(
        QStringLiteral("已复制 %1 字节 ASCII。")
        .arg(static_cast<qulonglong>(offsetList.size())));
}

void HexEditorWidget::copyCurrentAddress()
{
    const std::uint64_t absoluteAddress = selectedAbsoluteAddress();
    const QString addressText = QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(absoluteAddress), 16, 16, QChar('0')).toUpper();

    if (QApplication::clipboard() != nullptr)
    {
        QApplication::clipboard()->setText(addressText);
    }

    updateStatusLabel(QStringLiteral("已复制地址：%1").arg(addressText));
}

void HexEditorWidget::copyCurrentRowDump()
{
    if (m_hexTable == nullptr)
    {
        return;
    }

    const int currentRow = m_hexTable->currentRow();
    if (currentRow < 0)
    {
        updateStatusLabel(QStringLiteral("复制失败：未定位到当前行。"));
        return;
    }

    const QString rowDumpText = buildRowDumpText(currentRow);
    if (QApplication::clipboard() != nullptr)
    {
        QApplication::clipboard()->setText(rowDumpText);
    }

    updateStatusLabel(QStringLiteral("已复制当前行转储文本。"));
}

void HexEditorWidget::exportBinaryFile()
{
    if (m_buffer.isEmpty())
    {
        updateStatusLabel(QStringLiteral("导出失败：当前无数据。"));
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出二进制"),
        QStringLiteral("hex_export.bin"),
        QStringLiteral("Binary Files (*.bin);;All Files (*.*)"));
    if (filePath.trimmed().isEmpty())
    {
        return;
    }

    QFile outputFile(filePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法写入文件：%1").arg(filePath));
        return;
    }

    outputFile.write(m_buffer);
    outputFile.close();

    updateStatusLabel(
        QStringLiteral("导出完成：%1 字节 -> %2")
        .arg(static_cast<qulonglong>(m_buffer.size()))
        .arg(filePath));
}

void HexEditorWidget::exportHexTextFile()
{
    if (m_buffer.isEmpty())
    {
        updateStatusLabel(QStringLiteral("导出失败：当前无数据。"));
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出 HEX 文本"),
        QStringLiteral("hex_export.txt"),
        QStringLiteral("Text Files (*.txt);;All Files (*.*)"));
    if (filePath.trimmed().isEmpty())
    {
        return;
    }

    QFile outputFile(filePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法写入文件：%1").arg(filePath));
        return;
    }

    QTextStream outputStream(&outputFile);
    outputStream.setEncoding(QStringConverter::Utf8);
    outputStream << buildFullDumpText();
    outputFile.close();

    updateStatusLabel(
        QStringLiteral("导出完成：HEX 文本 -> %1").arg(filePath));
}

QString HexEditorWidget::buildRowDumpText(const int rowIndex) const
{
    if (rowIndex < 0)
    {
        return QString();
    }

    const std::uint64_t rowOffset = static_cast<std::uint64_t>(rowIndex) * static_cast<std::uint64_t>(m_bytesPerRow);
    QStringList byteList;
    byteList.reserve(m_bytesPerRow);

    QString asciiText;
    asciiText.reserve(m_bytesPerRow);

    for (int byteColumn = 0; byteColumn < m_bytesPerRow; ++byteColumn)
    {
        const std::uint64_t byteOffset = rowOffset + static_cast<std::uint64_t>(byteColumn);
        if (byteOffset < static_cast<std::uint64_t>(m_buffer.size()))
        {
            const std::uint8_t byteValue = static_cast<std::uint8_t>(m_buffer.at(static_cast<int>(byteOffset)));
            byteList.push_back(byteToHexText(byteValue));
            const bool printable = (byteValue >= 32 && byteValue <= 126);
            asciiText.push_back(printable ? QChar(byteValue) : QChar('.'));
        }
        else
        {
            byteList.push_back(QStringLiteral("--"));
            asciiText.push_back(' ');
        }
    }

    return QStringLiteral("%1  %2  |%3|")
        .arg(QStringLiteral("0x%1").arg(static_cast<qulonglong>(m_baseAddress + rowOffset), 16, 16, QChar('0')).toUpper())
        .arg(byteList.join(' '))
        .arg(asciiText);
}

QString HexEditorWidget::buildFullDumpText() const
{
    if (m_buffer.isEmpty())
    {
        return QStringLiteral("<empty>");
    }

    QStringList rowTextList;
    const int rowCount = (m_buffer.size() + m_bytesPerRow - 1) / m_bytesPerRow;
    rowTextList.reserve(rowCount);

    for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex)
    {
        rowTextList.push_back(buildRowDumpText(rowIndex));
    }
    return rowTextList.join('\n');
}
