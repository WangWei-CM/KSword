// ============================================================
// MemoryDock.SearchFlow.inc
// 作用：承载首次/再次扫描流程与后台扫描执行逻辑。
// ============================================================

void MemoryDock::startFirstScan()
{
    // 首次扫描入口日志：记录当前附加 PID 与扫描参数。
    kLogEvent firstScanStartEvent;
    info << firstScanStartEvent
        << "[MemoryDock] startFirstScan: 请求开始首次扫描, attachedPid="
        << m_attachedPid
        << ", scanThreadCount="
        << m_scanThreadCount
        << ", scanChunkSizeKB="
        << m_scanChunkSizeKB
        << eol;

    // 正在扫描时不允许重复发起，避免多个后台线程并发读同一状态。
    if (m_scanInProgress.load())
    {
        kLogEvent firstScanBusyEvent;
        warn << firstScanBusyEvent
            << "[MemoryDock] startFirstScan: 已有扫描在执行，拒绝重复启动。"
            << eol;
        QMessageBox::information(this, "内存扫描", "当前已有扫描任务正在执行。");
        return;
    }

    // 首次扫描前必须附加进程。
    if (m_attachedProcessHandle == nullptr || m_attachedPid == 0)
    {
        kLogEvent firstScanNoAttachEvent;
        warn << firstScanNoAttachEvent
            << "[MemoryDock] startFirstScan: 未附加进程，无法扫描。"
            << eol;
        QMessageBox::warning(this, "内存扫描", "请先附加目标进程。");
        return;
    }

    // 先解析输入值，再收集扫描区域，两步都可能失败并反馈可读错误。
    ParsedSearchPattern pattern{};
    QString parseError;
    if (!parseSearchPatternFromUi(pattern, parseError))
    {
        kLogEvent firstScanParseFailEvent;
        warn << firstScanParseFailEvent
            << "[MemoryDock] startFirstScan: 解析扫描值失败, error="
            << parseError.toStdString()
            << eol;
        QMessageBox::warning(this, "内存扫描", parseError);
        return;
    }

    std::vector<RegionEntry> scanRegions;
    QString regionError;
    if (!collectSearchRegionsFromUi(scanRegions, regionError))
    {
        kLogEvent firstScanCollectFailEvent;
        warn << firstScanCollectFailEvent
            << "[MemoryDock] startFirstScan: 收集扫描区域失败, error="
            << regionError.toStdString()
            << eol;
        QMessageBox::warning(this, "内存扫描", regionError);
        return;
    }

    // 首次扫描会重置上一轮结果，并把“最近扫描类型”更新为当前下拉框类型。
    m_lastSearchValueType = pattern.valueType;
    m_searchResultCache.clear();
    rebuildSearchResultTable();

    // UI 状态切换：禁用发起按钮，启用取消按钮，展示准备中的提示文本。
    m_scanInProgress.store(true);
    m_scanCancelRequested.store(false);
    m_firstScanButton->setEnabled(false);
    m_nextScanButton->setEnabled(false);
    m_resetScanButton->setEnabled(false);
    m_cancelScanButton->setEnabled(true);
    m_scanProgressBar->setValue(0);
    m_scanStatusLabel->setText(
        QString("扫描中：区域=%1，线程=%2")
        .arg(scanRegions.size())
        .arg(m_scanThreadCount));

    // 首次扫描准备就绪日志：记录模式字节长度与区域总数。
    kLogEvent firstScanPreparedEvent;
    info << firstScanPreparedEvent
        << "[MemoryDock] startFirstScan: 准备完成, patternBytes="
        << pattern.exactBytes.size()
        << ", regionCount="
        << scanRegions.size()
        << eol;

    // 进入后台扫描线程，避免 ReadProcessMemory 循环阻塞主界面。
    scanMemoryRegionsInBackground(scanRegions, pattern);
}

void MemoryDock::startNextScan()
{
    // 再次扫描入口日志：输出当前结果条数与比较条件索引。
    kLogEvent nextScanStartEvent;
    info << nextScanStartEvent
        << "[MemoryDock] startNextScan: 开始再次扫描, cachedResultCount="
        << m_searchResultCache.size()
        << ", compareIndex="
        << m_nextScanCompareCombo->currentIndex()
        << eol;

    // 再次扫描依赖首次扫描结果；无结果时直接提示，不执行无意义循环。
    if (m_scanInProgress.load())
    {
        kLogEvent nextScanBusyEvent;
        warn << nextScanBusyEvent
            << "[MemoryDock] startNextScan: 扫描忙碌中，拒绝重复请求。"
            << eol;
        QMessageBox::information(this, "再次扫描", "当前正在执行扫描，请稍候。");
        return;
    }
    if (m_attachedProcessHandle == nullptr || m_searchResultCache.empty())
    {
        kLogEvent nextScanPrerequisiteFailEvent;
        warn << nextScanPrerequisiteFailEvent
            << "[MemoryDock] startNextScan: 前置条件不满足, attachedPid="
            << m_attachedPid
            << ", cachedResultCount="
            << m_searchResultCache.size()
            << eol;
        QMessageBox::warning(this, "再次扫描", "请先完成首次扫描并保留结果。");
        return;
    }

    // 将常用“字节转数值”逻辑内联为局部 lambda，便于多条件比较复用。
    const auto bytesToDouble = [](const QByteArray& sourceBytes, const SearchValueType valueType, double& numberOut) -> bool
        {
            if (sourceBytes.isEmpty())
            {
                return false;
            }

            switch (valueType)
            {
            case SearchValueType::Byte:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::uint8_t))) return false;
                std::uint8_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Int16:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::int16_t))) return false;
                std::int16_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Int32:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::int32_t))) return false;
                std::int32_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Int64:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::int64_t))) return false;
                std::int64_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Float32:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(float))) return false;
                float value = 0.0f;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Float64:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(double))) return false;
                double value = 0.0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = value;
                return true;
            }
            default:
                return false;
            }
        };

    // 当前比较模式决定是否需要输入数值，变化类条件不需要额外输入。
    const auto compareMode = static_cast<SearchCompareMode>(
        m_nextScanCompareCombo->itemData(m_nextScanCompareCombo->currentIndex()).toInt());
    const bool needNumericInput = (
        compareMode == SearchCompareMode::Equal ||
        compareMode == SearchCompareMode::Greater ||
        compareMode == SearchCompareMode::Less ||
        compareMode == SearchCompareMode::Between);

    const bool isNumericType = (
        m_lastSearchValueType == SearchValueType::Byte ||
        m_lastSearchValueType == SearchValueType::Int16 ||
        m_lastSearchValueType == SearchValueType::Int32 ||
        m_lastSearchValueType == SearchValueType::Int64 ||
        m_lastSearchValueType == SearchValueType::Float32 ||
        m_lastSearchValueType == SearchValueType::Float64);

    // 非数值类型不支持 > < between 增减 比较，避免语义混乱。
    if (!isNumericType && compareMode != SearchCompareMode::Equal &&
        compareMode != SearchCompareMode::Changed &&
        compareMode != SearchCompareMode::Unchanged)
    {
        kLogEvent nextScanCompareUnsupportedEvent;
        warn << nextScanCompareUnsupportedEvent
            << "[MemoryDock] startNextScan: 当前数据类型不支持比较模式, valueType="
            << static_cast<int>(m_lastSearchValueType)
            << ", compareMode="
            << static_cast<int>(compareMode)
            << eol;
        QMessageBox::warning(this, "再次扫描", "当前数据类型仅支持 等于/变化/未变化 条件。");
        return;
    }

    double compareA = 0.0;
    double compareB = 0.0;
    if (needNumericInput && isNumericType)
    {
        bool parseOkA = false;
        compareA = m_nextScanValueEdit->text().trimmed().toDouble(&parseOkA);
        if (!parseOkA)
        {
            kLogEvent nextScanValueAParseFailEvent;
            warn << nextScanValueAParseFailEvent
                << "[MemoryDock] startNextScan: 条件值A解析失败, text="
                << m_nextScanValueEdit->text().trimmed().toStdString()
                << eol;
            QMessageBox::warning(this, "再次扫描", "条件值A解析失败。");
            return;
        }

        if (compareMode == SearchCompareMode::Between)
        {
            bool parseOkB = false;
            compareB = m_nextScanValueBEdit->text().trimmed().toDouble(&parseOkB);
            if (!parseOkB)
            {
                kLogEvent nextScanValueBParseFailEvent;
                warn << nextScanValueBParseFailEvent
                    << "[MemoryDock] startNextScan: 条件值B解析失败, text="
                    << m_nextScanValueBEdit->text().trimmed().toStdString()
                    << eol;
                QMessageBox::warning(this, "再次扫描", "条件值B解析失败。");
                return;
            }
            if (compareA > compareB)
            {
                std::swap(compareA, compareB);
            }
        }
    }

    // 非数值类型在“等于”条件下按用户输入文本重新构造比较字节。
    QByteArray nonNumericCompareBytes;
    QByteArray nonNumericWildcardMask;
    if (!isNumericType && compareMode == SearchCompareMode::Equal)
    {
        const QString compareText = m_nextScanValueEdit->text().trimmed();
        if (compareText.isEmpty())
        {
            kLogEvent nextScanTextEmptyEvent;
            warn << nextScanTextEmptyEvent
                << "[MemoryDock] startNextScan: 非数值等于比较值为空。"
                << eol;
            QMessageBox::warning(this, "再次扫描", "请输入用于“等于”比较的目标值。");
            return;
        }

        if (m_lastSearchValueType == SearchValueType::StringAscii)
        {
            nonNumericCompareBytes = compareText.toLatin1();
        }
        else if (m_lastSearchValueType == SearchValueType::StringUnicode)
        {
            const auto* utf16Data = reinterpret_cast<const char*>(compareText.utf16());
            nonNumericCompareBytes = QByteArray(
                utf16Data,
                compareText.size() * static_cast<int>(sizeof(char16_t)));
        }
        else
        {
            // ByteArray 模式支持 “AA ?? BB” 语法，mask=0 表示该字节位置忽略。
            QString normalizedText = compareText;
            normalizedText.replace(',', ' ');
            normalizedText.replace(';', ' ');
            const QStringList tokens = normalizedText.split(' ', Qt::SkipEmptyParts);
            if (tokens.isEmpty())
            {
                kLogEvent nextScanByteArrayEmptyEvent;
                warn << nextScanByteArrayEmptyEvent
                    << "[MemoryDock] startNextScan: ByteArray 比较 token 为空。"
                    << eol;
                QMessageBox::warning(this, "再次扫描", "字节数组比较值无效。");
                return;
            }

            for (const QString& tokenText : tokens)
            {
                const QString token = tokenText.trimmed().toUpper();
                if (token == "??")
                {
                    nonNumericCompareBytes.push_back('\0');
                    nonNumericWildcardMask.push_back('\0');
                    continue;
                }

                std::uint8_t parsedByte = 0;
                if (!parseHexByte(token, parsedByte))
                {
                    kLogEvent nextScanByteArrayTokenFailEvent;
                    warn << nextScanByteArrayTokenFailEvent
                        << "[MemoryDock] startNextScan: ByteArray token 无效, token="
                        << tokenText.toStdString()
                        << eol;
                    QMessageBox::warning(this, "再次扫描", QString("字节数组项无效：%1").arg(tokenText));
                    return;
                }
                nonNumericCompareBytes.push_back(static_cast<char>(parsedByte));
                nonNumericWildcardMask.push_back('\1');
            }
        }
    }

    // 再次扫描只对上一轮命中地址逐个复查，因此通常速度比首次扫描更快。
    std::vector<SearchResultEntry> nextResultCache;
    nextResultCache.reserve(m_searchResultCache.size());

    const auto scanStartTime = std::chrono::steady_clock::now();
    const int totalCount = static_cast<int>(m_searchResultCache.size());
    int processedCount = 0;
    bool cancelled = false;

    // 再次扫描也统一使用“扫描中”状态，确保取消按钮和顶部按钮行为一致。
    m_scanInProgress.store(true);
    m_scanCancelRequested.store(false);
    m_firstScanButton->setEnabled(false);
    m_nextScanButton->setEnabled(false);
    m_resetScanButton->setEnabled(false);
    m_cancelScanButton->setEnabled(true);

    m_scanProgressBar->setValue(0);
    m_scanStatusLabel->setText(QString("再次扫描中：总计 %1 条").arg(totalCount));

    for (const SearchResultEntry& oldEntry : m_searchResultCache)
    {
        // 用户点击取消后尽快退出循环，保留当前缓存不被部分结果覆盖。
        if (m_scanCancelRequested.load())
        {
            cancelled = true;
            break;
        }

        // 每条结果的读取长度遵循“上一轮值字节长度”，确保比较口径一致。
        const std::size_t readLength = std::max<std::size_t>(1, static_cast<std::size_t>(oldEntry.currentValueBytes.size()));
        QByteArray currentBytes(static_cast<int>(readLength), '\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ::ReadProcessMemory(
            m_attachedProcessHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(oldEntry.address)),
            currentBytes.data(),
            static_cast<SIZE_T>(readLength),
            &bytesRead);
        if (readOk == FALSE || bytesRead != readLength)
        {
            ++processedCount;
            if ((processedCount % 256) == 0 || processedCount == totalCount)
            {
                const int progressPercent = (totalCount <= 0) ? 100 : (processedCount * 100 / totalCount);
                m_scanProgressBar->setValue(progressPercent);
                QApplication::processEvents(QEventLoop::AllEvents, 1);
            }
            continue;
        }

        bool keepThisEntry = false;
        switch (compareMode)
        {
        case SearchCompareMode::Equal:
        {
            if (isNumericType)
            {
                double currentNumber = 0.0;
                if (bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber))
                {
                    keepThisEntry = (std::fabs(currentNumber - compareA) <= 0.000001);
                }
            }
            else
            {
                // 字节数组/字符串等于比较按用户输入字节执行，支持 ByteArray 通配掩码。
                if (currentBytes.size() != nonNumericCompareBytes.size())
                {
                    keepThisEntry = false;
                }
                else if (nonNumericWildcardMask.size() == nonNumericCompareBytes.size())
                {
                    keepThisEntry = true;
                    for (int byteIndex = 0; byteIndex < nonNumericCompareBytes.size(); ++byteIndex)
                    {
                        if (nonNumericWildcardMask.at(byteIndex) == '\0')
                        {
                            continue;
                        }
                        if (currentBytes.at(byteIndex) != nonNumericCompareBytes.at(byteIndex))
                        {
                            keepThisEntry = false;
                            break;
                        }
                    }
                }
                else
                {
                    keepThisEntry = (currentBytes == nonNumericCompareBytes);
                }
            }
            break;
        }
        case SearchCompareMode::Greater:
        {
            double currentNumber = 0.0;
            keepThisEntry = bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                (currentNumber > compareA);
            break;
        }
        case SearchCompareMode::Less:
        {
            double currentNumber = 0.0;
            keepThisEntry = bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                (currentNumber < compareA);
            break;
        }
        case SearchCompareMode::Between:
        {
            double currentNumber = 0.0;
            keepThisEntry = bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                (currentNumber >= compareA && currentNumber <= compareB);
            break;
        }
        case SearchCompareMode::Changed:
            keepThisEntry = (currentBytes != oldEntry.currentValueBytes);
            break;
        case SearchCompareMode::Unchanged:
            keepThisEntry = (currentBytes == oldEntry.currentValueBytes);
            break;
        case SearchCompareMode::Increased:
        {
            double currentNumber = 0.0;
            double previousNumber = 0.0;
            keepThisEntry =
                bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                bytesToDouble(oldEntry.currentValueBytes, m_lastSearchValueType, previousNumber) &&
                (currentNumber > previousNumber);
            break;
        }
        case SearchCompareMode::Decreased:
        {
            double currentNumber = 0.0;
            double previousNumber = 0.0;
            keepThisEntry =
                bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                bytesToDouble(oldEntry.currentValueBytes, m_lastSearchValueType, previousNumber) &&
                (currentNumber < previousNumber);
            break;
        }
        default:
            keepThisEntry = false;
            break;
        }

        if (keepThisEntry)
        {
            SearchResultEntry nextEntry = oldEntry;
            nextEntry.previousValueBytes = oldEntry.currentValueBytes;
            nextEntry.currentValueBytes = currentBytes;
            nextResultCache.push_back(std::move(nextEntry));
        }

        // 进度更新改为分批触发，降低 UI 线程重绘开销并保持界面响应。
        ++processedCount;
        if ((processedCount % 256) == 0 || processedCount == totalCount)
        {
            const int progressPercent = (totalCount <= 0) ? 100 : (processedCount * 100 / totalCount);
            m_scanProgressBar->setValue(progressPercent);
            QApplication::processEvents(QEventLoop::AllEvents, 1);
        }
    }

    // 再次扫描统一收尾：恢复按钮状态并清理扫描标记。
    m_scanInProgress.store(false);
    m_scanCancelRequested.store(false);
    m_firstScanButton->setEnabled(true);
    m_resetScanButton->setEnabled(true);
    m_cancelScanButton->setEnabled(false);

    // 取消时保留上一轮结果，不覆盖缓存。
    if (!cancelled)
    {
        m_searchResultCache = std::move(nextResultCache);
    }
    rebuildSearchResultTable();

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scanStartTime).count();
    QString finishText;
    if (cancelled)
    {
        finishText = QString("再次扫描已取消：保留上一轮 %1 项，耗时 %2 ms")
            .arg(m_searchResultCache.size())
            .arg(elapsedMs);
    }
    else
    {
        finishText = QString("再次扫描完成：保留 %1 项，耗时 %2 ms")
            .arg(m_searchResultCache.size())
            .arg(elapsedMs);
    }
    if (m_searchResultCache.size() > m_searchResultVisibleCount)
    {
        finishText += QString("（仅显示前 %1 项）").arg(m_searchResultVisibleCount);
    }
    m_scanStatusLabel->setText(finishText);

    // 再次扫描完成日志：输出保留数量与耗时。
    kLogEvent nextScanFinishEvent;
    info << nextScanFinishEvent
        << "[MemoryDock] startNextScan: 完成, remainingCount="
        << m_searchResultCache.size()
        << ", elapsedMs="
        << elapsedMs
        << ", cancelled="
        << (cancelled ? "true" : "false")
        << eol;
}

void MemoryDock::resetScanState()
{
    // 重置入口日志：用于追踪用户主动清空扫描状态。
    kLogEvent resetScanEvent;
    info << resetScanEvent
        << "[MemoryDock] resetScanState: 清空扫描缓存与状态。"
        << eol;

    // 重置动作会先发起取消请求，避免后台线程晚到结果覆盖清空状态。
    cancelCurrentScan();

    m_searchResultCache.clear();
    m_lastSearchValueType = SearchValueType::Byte;
    rebuildSearchResultTable();

    // 恢复按钮状态，并把进度条与状态文本还原到“就绪”。
    m_scanProgressBar->setValue(0);
    m_scanStatusLabel->setText("已重置，等待首次扫描。");
    m_firstScanButton->setEnabled(true);
    m_nextScanButton->setEnabled(false);
    m_resetScanButton->setEnabled(true);
    m_cancelScanButton->setEnabled(false);
}

void MemoryDock::cancelCurrentScan()
{
    // 仅当扫描进行中才设置取消标志；空闲状态下无需修改。
    if (m_scanInProgress.load())
    {
        m_scanCancelRequested.store(true);
        m_scanStatusLabel->setText("正在请求取消扫描...");
        kLogEvent cancelScanEvent;
        warn << cancelScanEvent
            << "[MemoryDock] cancelCurrentScan: 已设置取消标志。"
            << eol;
    }
}

void MemoryDock::rebuildSearchResultTable()
{
    // 重建结果表入口日志：记录结果规模与显示类型。
    kLogEvent rebuildResultEvent;
    dbg << rebuildResultEvent
        << "[MemoryDock] rebuildSearchResultTable: 结果重建, count="
        << m_searchResultCache.size()
        << ", valueType="
        << static_cast<int>(m_lastSearchValueType)
        << eol;

    // 为避免超大结果集导致界面长时间冻结，UI 层仅展示前 N 条。
    constexpr std::size_t kSearchResultDisplayLimit = 50000;
    const std::size_t totalCount = m_searchResultCache.size();
    const std::size_t visibleCount = std::min<std::size_t>(totalCount, kSearchResultDisplayLimit);
    const std::size_t hiddenCount = totalCount - visibleCount;
    m_searchResultVisibleCount = visibleCount;

    // 表格重建前先关闭排序与绘制，降低批量 setItem 开销。
    m_searchResultTable->setSortingEnabled(false);
    m_searchResultTable->setUpdatesEnabled(false);
    m_searchResultTable->clearContents();
    m_searchResultTable->setRowCount(static_cast<int>(visibleCount));

    for (int row = 0; row < static_cast<int>(visibleCount); ++row)
    {
        const SearchResultEntry& entry = m_searchResultCache[static_cast<std::size_t>(row)];

        QTableWidgetItem* addressItem = new QTableWidgetItem(formatAddress(entry.address));
        addressItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(entry.address)));
        m_searchResultTable->setItem(row, 0, addressItem);

        m_searchResultTable->setItem(
            row,
            1,
            new QTableWidgetItem(bytesToDisplayString(entry.currentValueBytes, m_lastSearchValueType)));

        m_searchResultTable->setItem(
            row,
            2,
            new QTableWidgetItem(bytesToDisplayString(entry.previousValueBytes, m_lastSearchValueType)));

        m_searchResultTable->setItem(row, 3, new QTableWidgetItem(entry.noteText));
    }
    m_searchResultTable->setUpdatesEnabled(true);

    // 当结果被截断显示时，在工具提示中给出明确说明。
    if (hiddenCount > 0)
    {
        m_searchResultTable->setToolTip(
            QString("结果总数 %1，当前仅显示前 %2 条以保持界面响应。")
            .arg(totalCount)
            .arg(visibleCount));
    }
    else
    {
        m_searchResultTable->setToolTip(QString());
    }

    // 有结果时允许再次扫描；无结果时禁用该按钮避免误操作。
    m_nextScanButton->setEnabled(!m_searchResultCache.empty() && !m_scanInProgress.load());

    kLogEvent rebuildResultFinishEvent;
    dbg << rebuildResultFinishEvent
        << "[MemoryDock] rebuildSearchResultTable: 重建完成, nextScanEnabled="
        << (m_nextScanButton->isEnabled() ? "true" : "false")
        << ", visibleCount="
        << visibleCount
        << ", hiddenCount="
        << hiddenCount
        << eol;
}

void MemoryDock::scanMemoryRegionsInBackground(
    const std::vector<RegionEntry>& scanRegions,
    const ParsedSearchPattern& pattern)
{
    // 后台扫描入口日志：记录区域总数、线程数和匹配模式字节数。
    kLogEvent scanBackgroundStartEvent;
    info << scanBackgroundStartEvent
        << "[MemoryDock] scanMemoryRegionsInBackground: 启动后台扫描, regionCount="
        << scanRegions.size()
        << ", threadCount="
        << m_scanThreadCount
        << ", patternBytes="
        << pattern.exactBytes.size()
        << eol;

    // 使用 QPointer 保护 this 指针，防止窗口销毁后回调访问悬空对象。
    const QPointer<MemoryDock> selfGuard(this);

    // 扫描参数固定为值捕获，避免 UI 后续修改导致后台线程读到变化中的对象。
    const HANDLE processHandle = m_attachedProcessHandle;
    const std::vector<RegionEntry> regions = scanRegions;
    const ParsedSearchPattern scanPattern = pattern;
    const std::uint32_t threadCount = std::max<std::uint32_t>(1, m_scanThreadCount);
    const std::size_t chunkSize = static_cast<std::size_t>(std::max<std::uint32_t>(64, m_scanChunkSizeKB) * 1024u);
    const auto startTime = std::chrono::steady_clock::now();

    // 后台主线程只负责拉起 worker 并汇总结果，不直接操作 UI 控件。
    std::thread([selfGuard, processHandle, regions, scanPattern, threadCount, chunkSize, startTime]() {
        if (selfGuard == nullptr || processHandle == nullptr)
        {
            kLogEvent scanBackgroundGuardFailEvent;
            warn << scanBackgroundGuardFailEvent
                << "[MemoryDock] scanMemoryRegionsInBackground: selfGuard 或 processHandle 无效，线程直接返回。"
                << eol;
            return;
        }

        std::vector<SearchResultEntry> mergedResults;
        mergedResults.reserve(1024);
        std::mutex resultMutex;
        std::atomic<std::size_t> finishedRegionCount{ 0 };

        const std::size_t patternLength = static_cast<std::size_t>(scanPattern.exactBytes.size());
        if (patternLength == 0)
        {
            kLogEvent scanBackgroundPatternEmptyEvent;
            err << scanBackgroundPatternEmptyEvent
                << "[MemoryDock] scanMemoryRegionsInBackground: patternLength 为 0，终止扫描。"
                << eol;
            QMetaObject::invokeMethod(selfGuard.data(), [selfGuard]() {
                if (selfGuard == nullptr) return;
                selfGuard->m_scanInProgress.store(false);
                selfGuard->m_scanCancelRequested.store(false);
                selfGuard->m_firstScanButton->setEnabled(true);
                selfGuard->m_resetScanButton->setEnabled(true);
                selfGuard->m_cancelScanButton->setEnabled(false);
                selfGuard->m_scanStatusLabel->setText("扫描失败：匹配模式为空。");
                }, Qt::QueuedConnection);
            return;
        }

        // 单点匹配函数：支持精确匹配与 ByteArray 的通配掩码匹配。
        const auto matchesPatternAt = [&scanPattern, patternLength](const char* dataPtr) -> bool
            {
                for (std::size_t byteIndex = 0; byteIndex < patternLength; ++byteIndex)
                {
                    const bool hasMask = (scanPattern.wildcardMask.size() == static_cast<int>(patternLength));
                    if (hasMask && scanPattern.wildcardMask.at(static_cast<int>(byteIndex)) == '\0')
                    {
                        continue;
                    }
                    if (dataPtr[byteIndex] != scanPattern.exactBytes.at(static_cast<int>(byteIndex)))
                    {
                        return false;
                    }
                }
                return true;
            };

        // 每个 worker 处理“下标同余”的区域切片，实现低成本负载均衡。
        const auto workerBody = [&](const std::uint32_t workerId) {
            // 单个 worker 的命中计数，仅用于日志统计。
            std::vector<SearchResultEntry> localResults;
            localResults.reserve(256);
            std::size_t workerHitCount = 0;

            for (std::size_t regionIndex = workerId; regionIndex < regions.size(); regionIndex += threadCount)
            {
                if (selfGuard->m_scanCancelRequested.load())
                {
                    break;
                }

                const RegionEntry& region = regions[regionIndex];
                if (region.regionSize == 0)
                {
                    ++finishedRegionCount;
                    continue;
                }

                std::uint64_t cursor = region.baseAddress;
                std::uint64_t remainBytes = region.regionSize;
                QByteArray carryBytes;

                while (remainBytes > 0 && !selfGuard->m_scanCancelRequested.load())
                {
                    const std::size_t requestSize = static_cast<std::size_t>(
                        std::min<std::uint64_t>(remainBytes, static_cast<std::uint64_t>(chunkSize)));
                    QByteArray readBuffer(static_cast<int>(requestSize), '\0');
                    SIZE_T bytesRead = 0;
                    const BOOL readOk = ::ReadProcessMemory(
                        processHandle,
                        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(cursor)),
                        readBuffer.data(),
                        static_cast<SIZE_T>(requestSize),
                        &bytesRead);
                    if (readOk == FALSE || bytesRead == 0)
                    {
                        // 某块读取失败时跳过该块继续扫描，保证任务具备容错能力。
                        cursor += requestSize;
                        remainBytes -= requestSize;
                        carryBytes.clear();
                        continue;
                    }

                    // 合并“上一块尾巴 + 当前块”处理边界命中，避免漏掉跨块模式。
                    const int validReadLength = static_cast<int>(bytesRead);
                    QByteArray mergedBuffer = carryBytes + readBuffer.left(validReadLength);
                    const std::size_t carryLength = static_cast<std::size_t>(carryBytes.size());
                    const std::uint64_t mergedBaseAddress = cursor - carryLength;

                    // 起始偏移计算：只复查“上块尾部不足一个模式长度”的窗口。
                    const std::size_t overlap = (patternLength > 0) ? (patternLength - 1) : 0;
                    const std::size_t startOffset = (carryLength > overlap) ? (carryLength - overlap) : 0;

                    if (mergedBuffer.size() >= static_cast<int>(patternLength))
                    {
                        const std::size_t maxOffset =
                            static_cast<std::size_t>(mergedBuffer.size()) - patternLength;
                        for (std::size_t offset = startOffset; offset <= maxOffset; ++offset)
                        {
                            const char* candidatePtr = mergedBuffer.constData() + static_cast<int>(offset);
                            if (!matchesPatternAt(candidatePtr))
                            {
                                continue;
                            }

                            SearchResultEntry resultEntry{};
                            resultEntry.address = mergedBaseAddress + offset;
                            resultEntry.currentValueBytes = QByteArray(candidatePtr, static_cast<int>(patternLength));
                            localResults.push_back(std::move(resultEntry));
                            ++workerHitCount;
                        }
                    }

                    // 保留末尾 patternLength-1 字节供下一块拼接，不足时全量保留。
                    if (patternLength > 1)
                    {
                        const int tailCount = std::min<int>(
                            static_cast<int>(patternLength - 1),
                            mergedBuffer.size());
                        carryBytes = mergedBuffer.right(tailCount);
                    }
                    else
                    {
                        carryBytes.clear();
                    }

                    cursor += requestSize;
                    remainBytes -= requestSize;
                }

                // 分区扫描完成后统一并入总结果，减少锁竞争次数。
                {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    mergedResults.insert(
                        mergedResults.end(),
                        std::make_move_iterator(localResults.begin()),
                        std::make_move_iterator(localResults.end()));
                }
                localResults.clear();

                const std::size_t finishedCount = ++finishedRegionCount;
                if ((finishedCount % 16 == 0) || finishedCount == regions.size())
                {
                    QMetaObject::invokeMethod(selfGuard.data(), [selfGuard, finishedCount, totalCount = regions.size()]() {
                        if (selfGuard == nullptr || totalCount == 0)
                        {
                            return;
                        }
                        const int progress = static_cast<int>((finishedCount * 100) / totalCount);
                        selfGuard->m_scanProgressBar->setValue(progress);
                        selfGuard->m_scanStatusLabel->setText(
                            QString("扫描中：%1 / %2 区域")
                            .arg(finishedCount)
                            .arg(totalCount));
                        }, Qt::QueuedConnection);
                }
            }

            // worker 收尾日志：记录该 worker 命中总数，便于观察负载均衡。
            kLogEvent workerFinishEvent;
            dbg << workerFinishEvent
                << "[MemoryDock] scanMemoryRegionsInBackground: worker完成, workerId="
                << workerId
                << ", hitCount="
                << workerHitCount
                << eol;
            };

        // 拉起 worker 线程组；线程数不会超过区域数，避免空转。
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        const std::uint32_t realThreadCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(regions.size(), static_cast<std::size_t>(threadCount)));
        for (std::uint32_t workerId = 0; workerId < realThreadCount; ++workerId)
        {
            workers.emplace_back(workerBody, workerId);
        }

        for (std::thread& worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        // 统一按地址升序排列，便于用户浏览和“再次扫描”时的稳定结果顺序。
        std::sort(mergedResults.begin(), mergedResults.end(), [](const SearchResultEntry& left, const SearchResultEntry& right) {
            return left.address < right.address;
            });

        const bool cancelled = selfGuard->m_scanCancelRequested.load();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();

        // 后台线程结束日志：区分取消与正常完成。
        kLogEvent scanBackgroundThreadFinishEvent;
        info << scanBackgroundThreadFinishEvent
            << "[MemoryDock] scanMemoryRegionsInBackground: 后台线程结束, cancelled="
            << (cancelled ? "true" : "false")
            << ", mergedCount="
            << mergedResults.size()
            << ", elapsedMs="
            << elapsedMs
            << eol;

        // 所有 UI 修改统一切回主线程执行。
        QMetaObject::invokeMethod(selfGuard.data(), [selfGuard, cancelled, elapsedMs, finalResults = std::move(mergedResults)]() mutable {
            if (selfGuard == nullptr)
            {
                return;
            }

            selfGuard->m_scanInProgress.store(false);
            selfGuard->m_scanCancelRequested.store(false);
            selfGuard->m_firstScanButton->setEnabled(true);
            selfGuard->m_resetScanButton->setEnabled(true);
            selfGuard->m_cancelScanButton->setEnabled(false);

            if (cancelled)
            {
                selfGuard->m_scanStatusLabel->setText("扫描已取消。");
                kLogEvent scanBackgroundCancelledUiEvent;
                warn << scanBackgroundCancelledUiEvent
                    << "[MemoryDock] scanMemoryRegionsInBackground: 主线程收到取消结果。"
                    << eol;
                return;
            }

            selfGuard->m_scanProgressBar->setValue(100);
            selfGuard->m_searchResultCache = std::move(finalResults);
            selfGuard->rebuildSearchResultTable();
            QString finishText = QString("首次扫描完成：命中 %1 项，耗时 %2 ms")
                .arg(selfGuard->m_searchResultCache.size())
                .arg(elapsedMs);
            if (selfGuard->m_searchResultCache.size() > selfGuard->m_searchResultVisibleCount)
            {
                finishText += QString("（仅显示前 %1 项）").arg(selfGuard->m_searchResultVisibleCount);
            }
            selfGuard->m_scanStatusLabel->setText(finishText);

            // UI 提交完成日志：确认结果已落地到缓存与表格。
            kLogEvent scanBackgroundUiFinishEvent;
            info << scanBackgroundUiFinishEvent
                << "[MemoryDock] scanMemoryRegionsInBackground: 主线程提交完成, resultCount="
                << selfGuard->m_searchResultCache.size()
                << ", elapsedMs="
                << elapsedMs
                << eol;
            }, Qt::QueuedConnection);
        }).detach();
}

