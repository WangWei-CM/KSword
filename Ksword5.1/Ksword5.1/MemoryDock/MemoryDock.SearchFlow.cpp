#include "MemoryDock.Internal.h"
#include "../UI/TableInteractionSupport.h"
#include "../Internationalization/LanguageManager.h"

#include <memory>

// 说明：由原聚合式实现迁移为独立 .cpp，成员函数实现保持原样。
using namespace ksword::memory_dock_internal;

// ============================================================
// MemoryDock.SearchFlow.cpp
// 作用：承载首次/再次扫描流程与后台扫描执行逻辑。
// ============================================================

struct MemoryDock::FirstScanPreparation {
    ParsedSearchPattern pattern;
    FirstScanOptions options;
    std::shared_ptr<const std::vector<RegionEntry>> source;
    std::vector<RegionEntry> filtered;
    std::atomic_bool cancel{false}, done{false};
    bool waiting = true, success = false;
    QString error;
};

void MemoryDock::startFirstScan()
{
    if (m_scanInProgress.load()) { QMessageBox::information(this, "内存扫描", "当前已有扫描任务正在执行。"); return; }
    if (!m_attachedProcessHandle || m_attachedPid == 0) { QMessageBox::warning(this, "内存扫描", "请先附加目标进程。"); return; }
    auto preparation = ks::memory_detail::ValueStorage<FirstScanPreparation>();
    QString error;
    if (!parseSearchPatternFromUi(preparation->pattern, error) || !captureFirstScanOptions(preparation->options, error)) { QMessageBox::warning(this, "内存扫描", error); return; }
    m_firstScanPreparation = preparation; m_scanInProgress.store(true); m_scanCancelRequested.store(false);
    const QPointer<MemoryDock> owner(this);
    for (auto* button : {m_firstScanButton, m_nextScanButton, m_resetScanButton}) { if (button) { button->setEnabled(false); } if (!owner) { return; } }
    m_cancelScanButton->setEnabled(true); if (!owner) { return; }
    m_scanProgressBar->setValue(0); if (!owner) { return; }
    m_scanStatusLabel->setText(ks::i18n::packedSourceText(QStringLiteral("Preparing scan regions..."))); if (!owner) { return; }
    m_coreProjectionPoll->start(); requestFirstScanRegions();
}

void MemoryDock::acceptFirstScanRegionSnapshot(const std::shared_ptr<const std::vector<RegionEntry>>& rows, const std::uint32_t error)
{
    const auto preparation = m_firstScanPreparation;
    if (!preparation || !preparation->waiting) { return; }
    if (preparation->options.generation != m_processAttachmentGeneration.load() || preparation->options.pid != m_attachedPid || preparation->cancel.load()) { cancelFirstScanPreparation(); return; }
    if (error != 0) { finishFirstScanPreparation(ks::i18n::packedSourceText(QStringLiteral("Region enumeration is partial (Win32: %1)")).arg(error)); return; }
    if (!rows || rows->empty()) { finishFirstScanPreparation(QStringLiteral("当前没有可用的内存区域，请先刷新区域列表。")); return; }
    preparation->waiting = false; preparation->source = rows;
    try { QThreadPool::globalInstance()->start([preparation] {
        try { preparation->success = filterFirstScanRegions(preparation->options, *preparation->source, preparation->filtered, preparation->error, [preparation] { return preparation->cancel.load(); }); }
        catch (...) { preparation->error = QStringLiteral("Memory table projection failed"); }
        preparation->done.store(true);
    }); } catch (...) { preparation->error = QStringLiteral("Memory table projection failed"); preparation->done.store(true); }
}

void MemoryDock::finishFirstScanPreparation(const QString& error, const bool cancelled)
{
    m_firstScanPreparation.reset(); m_scanInProgress.store(false); m_scanCancelRequested.store(false);
    const QPointer<MemoryDock> owner(this);
    for (auto* button : {m_firstScanButton, m_resetScanButton}) { if (button) { button->setEnabled(true); } if (!owner) { return; } }
    m_nextScanButton->setEnabled(!m_searchResultCache->empty()); if (!owner) { return; }
    m_cancelScanButton->setEnabled(false); if (!owner) { return; }
    m_scanStatusLabel->setText(cancelled ? ks::i18n::packedSourceText(QStringLiteral("扫描已取消。")) : ks::i18n::sourceText(error));
}

void MemoryDock::cancelFirstScanPreparation()
{
    if (!m_firstScanPreparation) { return; }
    m_firstScanPreparation->cancel.store(true); finishFirstScanPreparation({}, true);
}

void MemoryDock::pollFirstScanPreparation()
{
    const auto preparation = m_firstScanPreparation;
    if (!preparation) { return; }
    if (preparation->options.generation != m_processAttachmentGeneration.load() || preparation->options.pid != m_attachedPid || preparation->cancel.load()) { cancelFirstScanPreparation(); return; }
    if (!preparation->done.load()) { return; }
    if (!preparation->success) { finishFirstScanPreparation(preparation->error); return; }
    m_firstScanPreparation.reset();
    m_lastSearchValueType = preparation->pattern.valueType; m_searchResultCache = ks::memory_detail::ValueStorage<std::vector<SearchResultEntry>>();
    const QPointer<MemoryDock> owner(this);
    rebuildSearchResultTable(); if (!owner) { return; }
    if (preparation->options.generation != m_processAttachmentGeneration.load() || preparation->options.pid != m_attachedPid || m_scanCancelRequested.load()) { finishFirstScanPreparation({}, true); return; }
    m_scanStatusLabel->setText(QString("扫描中：区域=%1，线程=%2").arg(preparation->filtered.size()).arg(preparation->options.threads)); if (!owner) { return; }
    scanMemoryRegionsInBackground(preparation->filtered, preparation->pattern, preparation->options);
}

void MemoryDock::startNextScan()
{
    // 再次扫描入口日志：输出当前结果条数与比较条件索引。
    kLogEvent nextScanStartEvent;
    info << nextScanStartEvent
        << "[MemoryDock] startNextScan: 开始再次扫描, cachedResultCount="
        << m_searchResultCache->size()
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
    if (m_attachedProcessHandle == nullptr || m_searchResultCache->empty())
    {
        kLogEvent nextScanPrerequisiteFailEvent;
        warn << nextScanPrerequisiteFailEvent
            << "[MemoryDock] startNextScan: 前置条件不满足, attachedPid="
            << m_attachedPid
            << ", cachedResultCount="
            << m_searchResultCache->size()
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

    // 再次扫描只对上一轮命中地址逐个复查。读取目标进程内存必须在后台完成，
    // 否则命中数量较大时会长期占用 GUI 线程，且 processEvents 会引入重入。
    const HANDLE processHandle = m_attachedProcessHandle;
    const SearchValueType valueType = m_lastSearchValueType;
    const auto previousResultCache = m_searchResultCache;
    const int totalCount = static_cast<int>(previousResultCache->size());
    const QPointer<MemoryDock> selfGuard(this);
    const std::shared_ptr<MemoryScanTaskState> taskState = m_scanTaskState;

    // 与首次扫描同理：后端与 DDMA 会话在 UI 线程按值快照，worker 不读活配置。
    const ksword::memory_backend::MemoryAccessBackend scanBackend = currentSearchBackend();
    const ksword::memory_backend::DdmaSession scanDdmaSession = currentDdmaSession();
    const std::uint32_t scanTargetPid = m_attachedPid;
    const QString scanBackendName = ks::i18n::sourceText(
        ksword::memory_backend::backendDisplayName(scanBackend));
    const QString finishedMessage = ks::i18n::sourceText(QStringLiteral(
        "再次扫描完成（%1）：保留 %2 项，耗时 %3 ms；跳过 %4 字节（%5 个条目）。"));
    const QString cancelledMessage = ks::i18n::sourceText(QStringLiteral(
        "再次扫描已取消（%1）：保留上一轮 %2 项，耗时 %3 ms；跳过 %4 字节（%5 个条目）。"));
    const QString readProblemMessage = ks::i18n::sourceText(QStringLiteral(
        "首个读取问题：%1"));
    const QString incompleteMessage = ks::i18n::sourceText(QStringLiteral(
        "读取结果不完整，已跳过该范围。"));

    // 再次扫描也统一使用“扫描中”状态，确保取消按钮和顶部按钮行为一致。
    m_scanInProgress.store(true);
    m_scanCancelRequested.store(false);
    m_firstScanButton->setEnabled(false);
    m_nextScanButton->setEnabled(false);
    m_resetScanButton->setEnabled(false);
    m_cancelScanButton->setEnabled(true);

    m_scanProgressBar->setValue(0);
    m_scanStatusLabel->setText(QString("再次扫描中：总计 %1 条").arg(totalCount));

    // 在线程启动前登记任务，关闭/分离路径据此等待，避免只靠 QPointer 判断生命周期。
    {
        std::lock_guard<std::mutex> lock(taskState->mutex);
        ++taskState->activeTaskCount;
    }

    std::thread([selfGuard,
        taskState,
        processHandle,
        valueType,
        previousResultCache = std::move(previousResultCache),
        compareMode,
        isNumericType,
        compareA,
        compareB,
        nonNumericCompareBytes,
        nonNumericWildcardMask,
        bytesToDouble,
        scanBackend,
        scanDdmaSession,
        scanTargetPid,
        scanBackendName,
        finishedMessage,
        cancelledMessage,
        readProblemMessage,
        incompleteMessage]() mutable
    {
        const auto finishScanTask = [taskState]()
        {
            std::lock_guard<std::mutex> lock(taskState->mutex);
            if (taskState->activeTaskCount > 0)
            {
                --taskState->activeTaskCount;
                if (taskState->activeTaskCount == 0)
                {
                    taskState->completion.notify_all();
                }
            }
        };

        if (selfGuard == nullptr || processHandle == nullptr)
        {
            finishScanTask();
            return;
        }

        const auto scanStartTime = std::chrono::steady_clock::now();
        const int workerTotalCount = static_cast<int>(previousResultCache->size());
        std::vector<SearchResultEntry> nextResultCache;
        nextResultCache.reserve(previousResultCache->size());
        int processedCount = 0;
        bool cancelled = false;
        std::uint64_t skippedBytes = 0;
        std::size_t skippedEntries = 0;
        QString firstReadFailure;

        for (const SearchResultEntry& oldEntry : *previousResultCache)
        {
            if (selfGuard->m_scanCancelRequested.load())
            {
                cancelled = true;
                break;
            }

            const std::size_t readLength = std::max<std::size_t>(
                1,
                static_cast<std::size_t>(oldEntry.currentValueBytes.size()));
            const auto readOutcome = ksword::memory_backend::readVirtual(
                scanBackend, scanDdmaSession, scanTargetPid, oldEntry.address,
                static_cast<std::uint64_t>(readLength));
            const bool readSucceeded = readOutcome.ok && !readOutcome.partial &&
                static_cast<std::size_t>(readOutcome.data.size()) == readLength;
            QByteArray currentBytes;
            if (readSucceeded)
            {
                currentBytes = readOutcome.data;
                if (firstReadFailure.isEmpty() && !readOutcome.failureText.isEmpty())
                {
                    firstReadFailure = readOutcome.failureText;
                }
            }
            else
            {
                skippedBytes += static_cast<std::uint64_t>(readLength);
                ++skippedEntries;
                if (firstReadFailure.isEmpty())
                {
                    firstReadFailure = readOutcome.failureText.isEmpty()
                        ? incompleteMessage : readOutcome.failureText;
                }
            }

            bool keepThisEntry = false;
            if (readSucceeded)
            {
                switch (compareMode)
                {
                case SearchCompareMode::Equal:
                    if (isNumericType)
                    {
                        double currentNumber = 0.0;
                        keepThisEntry = bytesToDouble(currentBytes, valueType, currentNumber) &&
                            (std::fabs(currentNumber - compareA) <= 0.000001);
                    }
                    else if (currentBytes.size() == nonNumericCompareBytes.size())
                    {
                        if (nonNumericWildcardMask.size() == nonNumericCompareBytes.size())
                        {
                            keepThisEntry = true;
                            for (int byteIndex = 0; byteIndex < nonNumericCompareBytes.size(); ++byteIndex)
                            {
                                if (nonNumericWildcardMask.at(byteIndex) != '\0' &&
                                    currentBytes.at(byteIndex) != nonNumericCompareBytes.at(byteIndex))
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
                case SearchCompareMode::Greater:
                {
                    double currentNumber = 0.0;
                    keepThisEntry = bytesToDouble(currentBytes, valueType, currentNumber) && currentNumber > compareA;
                    break;
                }
                case SearchCompareMode::Less:
                {
                    double currentNumber = 0.0;
                    keepThisEntry = bytesToDouble(currentBytes, valueType, currentNumber) && currentNumber < compareA;
                    break;
                }
                case SearchCompareMode::Between:
                {
                    double currentNumber = 0.0;
                    keepThisEntry = bytesToDouble(currentBytes, valueType, currentNumber) &&
                        currentNumber >= compareA && currentNumber <= compareB;
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
                    keepThisEntry = bytesToDouble(currentBytes, valueType, currentNumber) &&
                        bytesToDouble(oldEntry.currentValueBytes, valueType, previousNumber) &&
                        currentNumber > previousNumber;
                    break;
                }
                case SearchCompareMode::Decreased:
                {
                    double currentNumber = 0.0;
                    double previousNumber = 0.0;
                    keepThisEntry = bytesToDouble(currentBytes, valueType, currentNumber) &&
                        bytesToDouble(oldEntry.currentValueBytes, valueType, previousNumber) &&
                        currentNumber < previousNumber;
                    break;
                }
                default:
                    break;
                }
            }

            if (keepThisEntry)
            {
                SearchResultEntry nextEntry = oldEntry;
                nextEntry.previousValueBytes = oldEntry.currentValueBytes;
                nextEntry.currentValueBytes = std::move(currentBytes);
                nextResultCache.push_back(std::move(nextEntry));
            }

            ++processedCount;
            if ((processedCount % 1024) == 0 || processedCount == workerTotalCount)
            {
                const int progressPercent = workerTotalCount <= 0
                    ? 100
                    : (processedCount * 100 / workerTotalCount);
                QMetaObject::invokeMethod(selfGuard.data(), [selfGuard, progressPercent, processedCount, workerTotalCount]()
                {
                    if (selfGuard == nullptr)
                    {
                        return;
                    }
                    selfGuard->m_scanProgressBar->setValue(progressPercent);
                    selfGuard->m_scanStatusLabel->setText(
                        QString("再次扫描中：%1 / %2 条").arg(processedCount).arg(workerTotalCount));
                }, Qt::QueuedConnection);
            }
        }

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scanStartTime).count();
        const auto resultsSnapshot = ks::memory_detail::ValueStorage<std::vector<SearchResultEntry>>(std::move(nextResultCache));
        QMetaObject::invokeMethod(selfGuard.data(), [selfGuard,
            cancelled,
            elapsedMs,
            skippedBytes,
            skippedEntries,
            firstReadFailure,
            scanBackendName,
            finishedMessage,
            cancelledMessage,
            readProblemMessage,
            resultsSnapshot]()
        {
            if (selfGuard == nullptr)
            {
                return;
            }

            auto commitSnapshot = [selfGuard, cancelled, elapsedMs, resultsSnapshot,
                skippedBytes, skippedEntries, firstReadFailure, scanBackendName,
                finishedMessage, cancelledMessage, readProblemMessage]() mutable
            {
                if (selfGuard == nullptr)
                {
                    return;
                }

                // 事件可能因表格菜单冻结而延后，提交前再次吸收期间到达的取消请求。
                const bool effectiveCancelled =
                    cancelled || selfGuard->m_scanCancelRequested.load();
                selfGuard->m_scanInProgress.store(false);
                selfGuard->m_scanCancelRequested.store(false);
                selfGuard->m_firstScanButton->setEnabled(true);
                selfGuard->m_resetScanButton->setEnabled(true);
                selfGuard->m_cancelScanButton->setEnabled(false);

                if (!effectiveCancelled)
                {
                    selfGuard->m_searchResultCache = resultsSnapshot;
                    selfGuard->rebuildSearchResultTable();
                }

                selfGuard->m_nextScanButton->setEnabled(!selfGuard->m_searchResultCache->empty());
                QString finishText = (effectiveCancelled ? cancelledMessage : finishedMessage)
                    .arg(scanBackendName).arg(selfGuard->m_searchResultCache->size())
                    .arg(elapsedMs).arg(skippedBytes).arg(skippedEntries);
                if (!firstReadFailure.isEmpty())
                {
                    finishText += QLatin1Char(' ') + readProblemMessage.arg(
                        ks::i18n::sourceText(firstReadFailure));
                }
                if (selfGuard->m_searchResultCache->size() > selfGuard->m_searchResultVisibleCount)
                {
                    finishText += QString("（仅显示前 %1 项）").arg(selfGuard->m_searchResultVisibleCount);
                }
                selfGuard->m_scanStatusLabel->setText(finishText);

                kLogEvent nextScanFinishEvent;
                info << nextScanFinishEvent
                    << "[MemoryDock] startNextScan: 完成, remainingCount="
                    << selfGuard->m_searchResultCache->size()
                    << ", elapsedMs="
                    << elapsedMs
                    << ", cancelled="
                    << (effectiveCancelled ? "true" : "false")
                    << eol;
            };

            if (ks::ui::DeferItemViewUiCommitIfContextMenuOpen(
                selfGuard.data(),
                QStringLiteral("memory-search-next-scan"),
                { selfGuard->m_searchResultTable },
                commitSnapshot))
            {
                return;
            }
            commitSnapshot();
        }, Qt::QueuedConnection);
        finishScanTask();
    }).detach();
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

    m_searchResultCache = ks::memory_detail::ValueStorage<std::vector<SearchResultEntry>>();
    m_lastSearchValueType = SearchValueType::Byte;
    rebuildSearchResultTable();

    if (!m_searchResultTable) { return; }
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
    if (m_firstScanPreparation) { cancelFirstScanPreparation(); return; }
    // 仅当扫描进行中才设置取消标志；空闲状态下无需修改。
    if (m_scanInProgress.load())
    {
        m_scanCancelRequested.store(true);
        if (m_scanStatusLabel) { m_scanStatusLabel->setText("正在请求取消扫描..."); }
        kLogEvent cancelScanEvent;
        warn << cancelScanEvent
            << "[MemoryDock] cancelCurrentScan: 已设置取消标志。"
            << eol;
    }
}

void MemoryDock::cancelAndWaitForMemoryScanTasks()
{
    // 关闭句柄或析构前必须等待协调线程退出，不能让 detached worker 继续访问控件。
    cancelCurrentScan();
    m_scanCancelRequested.store(true);

    const std::shared_ptr<MemoryScanTaskState> taskState = m_scanTaskState;
    std::unique_lock<std::mutex> lock(taskState->mutex);
    taskState->completion.wait(lock, [taskState]()
    {
        return taskState->activeTaskCount == 0;
    });
}

void MemoryDock::rebuildSearchResultTable()
{
    if (!m_searchResultTable) { return; }
    constexpr std::size_t displayLimit = 10000;
    m_searchResultVisibleCount = (std::min)(m_searchResultCache->size(), displayLimit);
    const auto hidden = m_searchResultCache->size() - m_searchResultVisibleCount;
    m_searchResultTable->setToolTip(hidden ? QString("结果总数 %1，当前仅显示前 %2 条以保持界面响应。").arg(m_searchResultCache->size()).arg(m_searchResultVisibleCount) : QString());
    requestCoreProjection(CoreKind::SearchResults, false);
    m_nextScanButton->setEnabled(!m_searchResultCache->empty() && !m_scanInProgress.load());
}

void MemoryDock::captureSearchResultProjection(CoreProjectionData& data) const
{
    const auto source = m_searchResultCache;
    data.extraSource = source; data.extraCount = (std::min)(source->size(), std::size_t{10000}); data.extraDiagnosticRow = false;
    data.extraMatches = [](std::size_t) { return true; };
    data.extraKey = [source](std::size_t row) { return QString::number((*source)[row].address); };
    data.extraCell = [source, type = m_lastSearchValueType, language = data.language](std::size_t row, int column, int role) -> QVariant {
        const auto& entry = (*source)[row];
        if (column == 0 && role == Qt::UserRole) { return QVariant::fromValue<qulonglong>(entry.address); }
        const bool numeric = role == ks::ui::NumericSortRole;
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole && !numeric) { return {}; }
        switch (column) {
        case 0: return numeric ? QVariant::fromValue<qulonglong>(entry.address) : QVariant(formatAddress(entry.address));
        case 1: return bytesToDisplayString(entry.currentValueBytes, type);
        case 2: return bytesToDisplayString(entry.previousValueBytes, type);
        case 3: return language.sourceText(entry.noteText);
        default: return {};
        }
    };
}

void MemoryDock::scanMemoryRegionsInBackground(
    const std::vector<RegionEntry>& scanRegions,
    const ParsedSearchPattern& pattern, const FirstScanOptions& options)
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
    const std::uint32_t threadCount = std::max<std::uint32_t>(1, options.threads);
    // 所有后端使用协议允许的完整分片，避免 R0 因扫描配置超出单次上限而全程跳过。
    const std::size_t chunkSize = static_cast<std::size_t>(
        (std::min)(static_cast<std::uint64_t>((std::max)(64U, options.chunkKB)) * 1024ULL,
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_READ_MAX_BYTES)));
    const auto startTime = std::chrono::steady_clock::now();
    const std::shared_ptr<MemoryScanTaskState> taskState = m_scanTaskState;

    // 后端选择与 DDMA 会话必须在 UI 线程上按值快照下来：worker 跑起来之后
    // 用户随时可能改这两处配置，后台线程读一个变化中的会话会得到半新半旧的参数。
    const ksword::memory_backend::MemoryAccessBackend scanBackend = options.backend;
    const ksword::memory_backend::DdmaSession scanDdmaSession = options.ddma;
    const std::uint32_t scanTargetPid = options.pid;
    const QString scanBackendName = ks::i18n::sourceText(
        ksword::memory_backend::backendDisplayName(scanBackend));
    const QString finishedMessage = ks::i18n::sourceText(QStringLiteral(
        "首次扫描完成（%1）：命中 %2 项，耗时 %3 ms；跳过 %4 字节（%5 个读取请求）。"));
    const QString cancelledMessage = ks::i18n::sourceText(QStringLiteral(
        "扫描已取消（%1）；跳过 %2 字节（%3 个读取请求）。"));
    const QString readProblemMessage = ks::i18n::sourceText(QStringLiteral(
        "首个读取问题：%1"));
    const QString incompleteMessage = ks::i18n::sourceText(QStringLiteral(
        "读取结果不完整，已跳过该范围。"));

    // 外层协调线程会等待其内部 worker，因此登记这一层即可覆盖整轮首次扫描。
    {
        std::lock_guard<std::mutex> lock(taskState->mutex);
        ++taskState->activeTaskCount;
    }

    // 后台主线程只负责拉起 worker 并汇总结果，不直接操作 UI 控件。
    std::thread([selfGuard, taskState, processHandle, regions, scanPattern, threadCount, chunkSize, startTime,
                 scanBackend, scanDdmaSession, scanTargetPid, scanBackendName,
                 finishedMessage, cancelledMessage, readProblemMessage, incompleteMessage]() {
        const auto finishScanTask = [taskState]()
        {
            std::lock_guard<std::mutex> lock(taskState->mutex);
            if (taskState->activeTaskCount > 0)
            {
                --taskState->activeTaskCount;
                if (taskState->activeTaskCount == 0)
                {
                    taskState->completion.notify_all();
                }
            }
        };

        if (selfGuard == nullptr || processHandle == nullptr)
        {
            kLogEvent scanBackgroundGuardFailEvent;
            warn << scanBackgroundGuardFailEvent
                << "[MemoryDock] scanMemoryRegionsInBackground: selfGuard 或 processHandle 无效，线程直接返回。"
                << eol;
            finishScanTask();
            return;
        }

        std::vector<SearchResultEntry> mergedResults;
        mergedResults.reserve(1024);
        std::mutex resultMutex;
        std::atomic<std::size_t> finishedRegionCount{ 0 };
        std::atomic<std::uint64_t> skippedBytes{ 0 };
        std::atomic<std::size_t> skippedRequests{ 0 };
        QString firstReadFailure;

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
            finishScanTask();
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
                    const auto readOutcome = ksword::memory_backend::readVirtual(
                        scanBackend, scanDdmaSession, scanTargetPid, cursor,
                        static_cast<std::uint64_t>(requestSize));
                    if (!readOutcome.ok || readOutcome.partial ||
                        static_cast<std::size_t>(readOutcome.data.size()) != requestSize)
                    {
                        // 未完整读取的区域不参与匹配，尤其不能把 R0/DDMA 补零当作真值。
                        skippedBytes.fetch_add(static_cast<std::uint64_t>(requestSize),
                            std::memory_order_relaxed);
                        skippedRequests.fetch_add(1, std::memory_order_relaxed);
                        {
                            std::lock_guard<std::mutex> lock(resultMutex);
                            if (firstReadFailure.isEmpty())
                            {
                                firstReadFailure = readOutcome.failureText.isEmpty()
                                    ? incompleteMessage : readOutcome.failureText;
                            }
                        }
                        cursor += requestSize;
                        remainBytes -= requestSize;
                        carryBytes.clear();
                        continue;
                    }
                    if (!readOutcome.failureText.isEmpty())
                    {
                        std::lock_guard<std::mutex> lock(resultMutex);
                        if (firstReadFailure.isEmpty())
                        {
                            firstReadFailure = readOutcome.failureText;
                        }
                    }

                    // 合并“上一块尾巴 + 当前块”处理边界命中，避免漏掉跨块模式。
                    QByteArray mergedBuffer = carryBytes + readOutcome.data;
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

        const auto resultsSnapshot = ks::memory_detail::ValueStorage<std::vector<SearchResultEntry>>(std::move(mergedResults));
        // 所有 UI 修改统一切回主线程执行。
        QMetaObject::invokeMethod(selfGuard.data(), [selfGuard, cancelled, elapsedMs,
            skippedByteCount = skippedBytes.load(std::memory_order_relaxed),
            skippedRequestCount = skippedRequests.load(std::memory_order_relaxed),
            firstReadFailure, scanBackendName, finishedMessage, cancelledMessage,
            readProblemMessage, resultsSnapshot]() {
            if (selfGuard == nullptr)
            {
                return;
            }

            auto commitSnapshot = [selfGuard, cancelled, elapsedMs, resultsSnapshot,
                skippedByteCount, skippedRequestCount, firstReadFailure, scanBackendName,
                finishedMessage, cancelledMessage, readProblemMessage]() mutable
            {
                if (selfGuard == nullptr)
                {
                    return;
                }

                // 延迟提交期间的取消同样有效，避免旧扫描结果覆盖用户当前状态。
                const bool effectiveCancelled =
                    cancelled || selfGuard->m_scanCancelRequested.load();
                selfGuard->m_scanInProgress.store(false);
                selfGuard->m_scanCancelRequested.store(false);
                selfGuard->m_firstScanButton->setEnabled(true);
                selfGuard->m_resetScanButton->setEnabled(true);
                selfGuard->m_cancelScanButton->setEnabled(false);

                if (effectiveCancelled)
                {
                    QString cancelledText = cancelledMessage.arg(scanBackendName)
                        .arg(skippedByteCount).arg(skippedRequestCount);
                    if (!firstReadFailure.isEmpty())
                    {
                        cancelledText += QLatin1Char(' ') + readProblemMessage.arg(
                            ks::i18n::sourceText(firstReadFailure));
                    }
                    selfGuard->m_scanStatusLabel->setText(cancelledText);
                    kLogEvent scanBackgroundCancelledUiEvent;
                    warn << scanBackgroundCancelledUiEvent
                        << "[MemoryDock] scanMemoryRegionsInBackground: 主线程收到取消结果。"
                        << eol;
                    return;
                }

                selfGuard->m_scanProgressBar->setValue(100);
                selfGuard->m_searchResultCache = resultsSnapshot;
                selfGuard->rebuildSearchResultTable();
                QString finishText = finishedMessage.arg(scanBackendName)
                    .arg(selfGuard->m_searchResultCache->size()).arg(elapsedMs)
                    .arg(skippedByteCount).arg(skippedRequestCount);
                if (!firstReadFailure.isEmpty())
                {
                    finishText += QLatin1Char(' ') + readProblemMessage.arg(
                        ks::i18n::sourceText(firstReadFailure));
                }
                if (selfGuard->m_searchResultCache->size() > selfGuard->m_searchResultVisibleCount)
                {
                    finishText += QString("（仅显示前 %1 项）").arg(selfGuard->m_searchResultVisibleCount);
                }
                selfGuard->m_scanStatusLabel->setText(finishText);

                // UI 提交完成日志：确认结果已落地到缓存与表格。
                kLogEvent scanBackgroundUiFinishEvent;
                info << scanBackgroundUiFinishEvent
                    << "[MemoryDock] scanMemoryRegionsInBackground: 主线程提交完成, resultCount="
                    << selfGuard->m_searchResultCache->size()
                    << ", elapsedMs="
                    << elapsedMs
                    << eol;
            };

            if (ks::ui::DeferItemViewUiCommitIfContextMenuOpen(
                selfGuard.data(),
                QStringLiteral("memory-search-first-scan"),
                { selfGuard->m_searchResultTable },
                commitSnapshot))
            {
                return;
            }
            commitSnapshot();
            }, Qt::QueuedConnection);
        finishScanTask();
    }).detach();
}

