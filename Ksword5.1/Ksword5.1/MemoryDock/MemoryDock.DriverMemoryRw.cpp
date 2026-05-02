#include "MemoryDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/HexEditorWidget.h"

#include <QByteArray>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

// ============================================================
// MemoryDock.DriverMemoryRw.cpp
// 作用：
// - 负责“驱动内存读写”页的 R0 读取、R3 缓存编辑和差异写入；
// - 作为独立编译单元维护，避免新增功能继续依赖聚合包含。
// ============================================================
void MemoryDock::updateDriverMemoryBaseComboFromProcessCache()
{
    // 控件未初始化时直接返回；构造早期和析构后期都可能走到这里。
    if (m_driverMemoryBaseCombo == nullptr)
    {
        return;
    }

    // 刷新前保存用户输入文本和当前 PID，避免进程列表刷新破坏正在编辑的基址。
    const QString previousText = m_driverMemoryBaseCombo->currentText().trimmed();
    const int previousIndex = m_driverMemoryBaseCombo->currentIndex();
    const std::uint32_t previousPid = (previousIndex >= 0) ?
        static_cast<std::uint32_t>(m_driverMemoryBaseCombo->itemData(previousIndex, Qt::UserRole).toUInt()) :
        0U;

    // 重建下拉期间阻断信号；该控件用于筛选和读取，不应在刷新进程列表时误触发读取。
    QSignalBlocker blocker(m_driverMemoryBaseCombo);
    m_driverMemoryBaseCombo->clear();
    m_driverMemoryBaseCombo->addItem("0", QVariant::fromValue(static_cast<uint>(0U)));
    m_driverMemoryBaseCombo->setItemData(0, QString(), Qt::UserRole + 1);

    // 下拉项保存 PID 与进程名；显示文本沿用顶部进程框格式，方便直接按 PID 识别。
    for (const ProcessEntry& entry : m_processCache)
    {
        if (entry.pid == 0U)
        {
            continue;
        }

        const QString itemText = QString("%1 [PID:%2]").arg(entry.processName).arg(entry.pid);
        m_driverMemoryBaseCombo->addItem(itemText, QVariant::fromValue(static_cast<uint>(entry.pid)));
        const int row = m_driverMemoryBaseCombo->count() - 1;
        m_driverMemoryBaseCombo->setItemData(row, entry.processName, Qt::UserRole + 1);
    }

    // 优先按上一次明确选择的 PID 恢复；恢复失败再按输入文本恢复。
    int restoreIndex = -1;
    if (previousPid != 0U)
    {
        restoreIndex = m_driverMemoryBaseCombo->findData(
            QVariant::fromValue(static_cast<uint>(previousPid)),
            Qt::UserRole);
    }
    if (restoreIndex < 0 &&
        !previousText.isEmpty() &&
        previousText.compare("0", Qt::CaseInsensitive) != 0 &&
        !previousText.startsWith("0x", Qt::CaseInsensitive))
    {
        (void)findDriverMemoryProcessComboMatch(previousText, restoreIndex);
    }

    // 还原最终展示文本；0/空恢复默认基址，0x 文本保留为用户输入的数值基址。
    if (restoreIndex >= 0)
    {
        m_driverMemoryBaseCombo->setCurrentIndex(restoreIndex);
    }
    else if (previousText.isEmpty() || previousText.compare("0", Qt::CaseInsensitive) == 0)
    {
        m_driverMemoryBaseCombo->setCurrentIndex(0);
    }
    else
    {
        m_driverMemoryBaseCombo->setEditText(previousText);
    }
}

bool MemoryDock::findDriverMemoryProcessComboMatch(
    const QString& filterText,
    int& comboIndexOut) const
{
    // 输出索引默认无效，调用方据此判断是否需要提示用户重新选择。
    comboIndexOut = -1;
    if (m_driverMemoryBaseCombo == nullptr)
    {
        return false;
    }

    const QString needle = filterText.trimmed();
    if (needle.isEmpty())
    {
        return false;
    }

    // 第一轮做精确匹配：PID、进程名、完整显示文本都可直接命中。
    for (int index = 0; index < m_driverMemoryBaseCombo->count(); ++index)
    {
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_driverMemoryBaseCombo->itemData(index, Qt::UserRole).toUInt());
        if (pid == 0U)
        {
            continue;
        }

        const QString itemText = m_driverMemoryBaseCombo->itemText(index);
        const QString processName = m_driverMemoryBaseCombo->itemData(index, Qt::UserRole + 1).toString();
        const QString pidText = QString::number(pid);
        if (pidText.compare(needle, Qt::CaseInsensitive) == 0 ||
            processName.compare(needle, Qt::CaseInsensitive) == 0 ||
            itemText.compare(needle, Qt::CaseInsensitive) == 0)
        {
            comboIndexOut = index;
            return true;
        }
    }

    // 第二轮做模糊筛选：非 0x 输入会按进程名或下拉显示文本包含关系选中第一项。
    for (int index = 0; index < m_driverMemoryBaseCombo->count(); ++index)
    {
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_driverMemoryBaseCombo->itemData(index, Qt::UserRole).toUInt());
        if (pid == 0U)
        {
            continue;
        }

        const QString itemText = m_driverMemoryBaseCombo->itemText(index);
        const QString processName = m_driverMemoryBaseCombo->itemData(index, Qt::UserRole + 1).toString();
        if (itemText.contains(needle, Qt::CaseInsensitive) ||
            processName.contains(needle, Qt::CaseInsensitive))
        {
            comboIndexOut = index;
            return true;
        }
    }

    return false;
}

bool MemoryDock::resolveDriverMemoryRequestFromUi(
    std::uint32_t& targetPidOut,
    QString& targetNameOut,
    std::uint64_t& offsetBaseOut,
    std::uint64_t& centerAddressOut,
    std::uint64_t& effectiveCenterAddressOut,
    QString& errorTextOut)
{
    // 先清空输出，保证失败路径不会留下上一次解析结果。
    targetPidOut = 0U;
    targetNameOut.clear();
    offsetBaseOut = 0ULL;
    centerAddressOut = 0ULL;
    effectiveCenterAddressOut = 0ULL;
    errorTextOut.clear();

    // 偏移基址为空或 0 时表示不加基址；0x 前缀才按数值基址解析。
    const QString baseText = (m_driverMemoryBaseCombo == nullptr) ?
        QStringLiteral("0") :
        m_driverMemoryBaseCombo->currentText().trimmed();
    if (baseText.isEmpty() || baseText.compare("0", Qt::CaseInsensitive) == 0)
    {
        offsetBaseOut = 0ULL;
    }
    else if (baseText.startsWith("0x", Qt::CaseInsensitive))
    {
        if (!parseUnsignedNumber(baseText, offsetBaseOut))
        {
            errorTextOut = "偏移基址格式无效，应输入 0 或 0x 开头的十六进制地址。";
            return false;
        }
    }
    else
    {
        // 非 0x 输入按进程筛选，命中后把下拉框切到真实进程项。
        int matchedIndex = -1;
        if (!findDriverMemoryProcessComboMatch(baseText, matchedIndex))
        {
            errorTextOut = QString("未找到匹配进程：%1。请输入 0、0x基址，或进程名/PID。").arg(baseText);
            return false;
        }

        QSignalBlocker blocker(m_driverMemoryBaseCombo);
        m_driverMemoryBaseCombo->setCurrentIndex(matchedIndex);
        targetPidOut = static_cast<std::uint32_t>(
            m_driverMemoryBaseCombo->itemData(matchedIndex, Qt::UserRole).toUInt());
        targetNameOut = m_driverMemoryBaseCombo->itemData(matchedIndex, Qt::UserRole + 1).toString();
        offsetBaseOut = 0ULL;
    }

    // 如果基址框没有显式选进程，则优先使用已附加 PID，再回退到顶部当前选择。
    if (targetPidOut == 0U && m_attachedPid != 0U)
    {
        targetPidOut = m_attachedPid;
        targetNameOut = m_attachedProcessName;
    }
    if (targetPidOut == 0U && m_processCombo != nullptr)
    {
        const int processIndex = m_processCombo->currentIndex();
        if (processIndex >= 0)
        {
            targetPidOut = static_cast<std::uint32_t>(
                m_processCombo->itemData(processIndex, Qt::UserRole).toUInt());
            targetNameOut = m_processCombo->itemData(processIndex, Qt::UserRole + 1).toString();
        }
    }
    if (targetPidOut == 0U)
    {
        errorTextOut = "请选择有效目标进程；R0 读写需要 PID，但不要求先附加 Win32 进程句柄。";
        return false;
    }

    // 中心地址仍按原有地址解析规则处理；配合 offsetBase 得到最终 R0 地址。
    if (m_driverMemoryAddressEdit == nullptr ||
        !parseAddressText(m_driverMemoryAddressEdit->text().trimmed(), centerAddressOut))
    {
        errorTextOut = "中心地址格式无效。";
        return false;
    }
    if (centerAddressOut > (std::numeric_limits<std::uint64_t>::max)() - offsetBaseOut)
    {
        errorTextOut = "偏移基址与中心地址相加发生地址回绕，已拒绝。";
        return false;
    }

    effectiveCenterAddressOut = offsetBaseOut + centerAddressOut;
    return true;
}

void MemoryDock::driverReadMemoryFromUi()
{
    // 读取入口日志：记录当前附加 PID 和地址文本。
    kLogEvent readStartEvent;
    info << readStartEvent
        << "[MemoryDock] driverReadMemoryFromUi: 开始读取, attachedPid="
        << m_attachedPid
        << ", baseText="
        << ((m_driverMemoryBaseCombo == nullptr) ? "" : m_driverMemoryBaseCombo->currentText().trimmed().toStdString())
        << ", text="
        << m_driverMemoryAddressEdit->text().trimmed().toStdString()
        << eol;

    // R0 内存读写接口需要 PID，不需要 R3 先 OpenProcess/Attach；这里解析独立目标。
    std::uint32_t targetPid = 0U;
    QString targetProcessName;
    std::uint64_t offsetBase = 0ULL;
    std::uint64_t centerAddressInput = 0ULL;
    std::uint64_t centerAddress = 0ULL;
    QString resolveErrorText;
    if (!resolveDriverMemoryRequestFromUi(
        targetPid,
        targetProcessName,
        offsetBase,
        centerAddressInput,
        centerAddress,
        resolveErrorText))
    {
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText(resolveErrorText);
        }
        QMessageBox::warning(this, "驱动内存读写", resolveErrorText);
        return;
    }

    // 计算读取范围：默认中心地址前后各 1KB，并防止低地址下溢。
    const std::uint64_t beforeBytes =
        static_cast<std::uint64_t>(m_driverMemoryBeforeSpin->value());
    const std::uint64_t afterBytes =
        static_cast<std::uint64_t>(m_driverMemoryAfterSpin->value());
    const std::uint64_t baseAddress =
        (centerAddress >= beforeBytes) ? (centerAddress - beforeBytes) : 0ULL;
    if (afterBytes > (std::numeric_limits<std::uint64_t>::max)() - centerAddress)
    {
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText("读取范围发生地址回绕，已拒绝。");
        }
        QMessageBox::warning(this, "驱动内存读写", "读取范围发生地址回绕。");
        return;
    }
    const std::uint64_t endAddress = centerAddress + afterBytes;
    if (endAddress < centerAddress)
    {
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText("读取范围发生地址回绕，已拒绝。");
        }
        QMessageBox::warning(this, "驱动内存读写", "读取范围发生地址回绕。");
        return;
    }

    // totalBytes 是 R0 单次读取长度，受共享协议上限约束。
    const std::uint64_t totalBytes64 = endAddress - baseAddress + 1ULL;
    if (totalBytes64 == 0ULL || totalBytes64 > KSWORD_ARK_MEMORY_READ_MAX_BYTES)
    {
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText("读取范围超过驱动单次请求上限。");
        }
        QMessageBox::warning(this, "驱动内存读写", "读取范围超过驱动单次请求上限。");
        return;
    }

    // 调用 ArkDriverClient，Dock 不直接 DeviceIoControl。
    if (m_driverMemoryStatusLabel != nullptr)
    {
        m_driverMemoryStatusLabel->setText("正在通过 R0 读取内存...");
    }
    ksword::ark::DriverClient driverClient;
    const ksword::ark::VirtualMemoryReadResult readResult =
        driverClient.readVirtualMemory(
            targetPid,
            baseAddress,
            static_cast<std::uint32_t>(totalBytes64),
            KSWORD_ARK_MEMORY_READ_FLAG_ZERO_FILL_UNREADABLE);
    if (!readResult.io.ok)
    {
        resetDriverMemoryRwState();
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText(QString("R0读取失败：%1").arg(QString::fromStdString(readResult.io.message)));
        }
        QMessageBox::warning(this, "驱动内存读写", QString("R0读取失败：\n%1").arg(QString::fromStdString(readResult.io.message)));
        return;
    }

    // R0 按要求把不可读区域补 00，因此 UI 只要求数据长度和请求长度一致。
    if (readResult.data.empty())
    {
        resetDriverMemoryRwState();
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText(QString("R0未返回数据，readStatus=%1 copyStatus=0x%2")
                .arg(readResult.readStatus)
                .arg(static_cast<unsigned long>(readResult.copyStatus), 8, 16, QChar('0')));
        }
        return;
    }

    // 缓存原始备份与编辑副本；后续差异只和 original 比对。
    m_driverMemoryBaseAddress = readResult.requestedBaseAddress;
    m_driverMemoryOffsetBase = offsetBase;
    m_driverMemoryCenterAddress = centerAddress;
    m_driverMemorySnapshotPid = targetPid;
    m_driverMemorySnapshotProcessName = targetProcessName;
    m_driverMemoryOriginalBytes = QByteArray(
        reinterpret_cast<const char*>(readResult.data.data()),
        static_cast<int>(readResult.data.size()));
    m_driverMemoryEditedBytes = m_driverMemoryOriginalBytes;
    m_driverMemoryHasSnapshot = true;

    // 更新 HexEditor；开启可编辑，但不触发真实写入。
    m_driverMemoryHexEditor->setEditable(true);
    m_driverMemoryHexEditor->setBytesPerRow(16);
    m_driverMemoryHexEditor->setByteArray(
        m_driverMemoryEditedBytes,
        m_driverMemoryBaseAddress);

    // 刷新状态标签和按钮状态。
    m_driverMemoryApplyButton->setEnabled(false);
    m_driverMemoryRangeLabel->setText(
        QString("范围: %1 - %2 | 长度: %3 字节 | PID: %4%5 | 基址: %6 | 中心: %7")
        .arg(formatAddress(m_driverMemoryBaseAddress))
        .arg(formatAddress(m_driverMemoryBaseAddress + static_cast<std::uint64_t>(m_driverMemoryEditedBytes.size()) - 1ULL))
        .arg(m_driverMemoryEditedBytes.size())
        .arg(targetPid)
        .arg(targetProcessName.isEmpty() ? QString() : QString(" (%1)").arg(targetProcessName))
        .arg(formatAddress(offsetBase))
        .arg(formatAddress(centerAddress)));
    if (m_driverMemoryStatusLabel != nullptr)
    {
        m_driverMemoryStatusLabel->setText(
            QString("R0读取完成：PID=%1，请求=%2 字节，返回=%3 字节，状态=%4，copyStatus=0x%5。输入中心=%6，不可读字节已按 00 填充。")
            .arg(targetPid)
            .arg(readResult.requestedBytes)
            .arg(readResult.data.size())
            .arg(readResult.readStatus)
            .arg(static_cast<unsigned long>(readResult.copyStatus), 8, 16, QChar('0'))
            .arg(formatAddress(centerAddressInput)));
    }

    // 读取完成日志：记录范围与协议状态。
    kLogEvent readFinishEvent;
    info << readFinishEvent
        << "[MemoryDock] driverReadMemoryFromUi: 读取完成, base="
        << formatAddress(m_driverMemoryBaseAddress).toStdString()
        << ", pid="
        << targetPid
        << ", bytes="
        << m_driverMemoryEditedBytes.size()
        << ", status="
        << readResult.readStatus
        << eol;
}

void MemoryDock::driverApplyMemoryDiffFromUi()
{
    // 应用入口日志：记录是否有快照与当前缓存大小。
    kLogEvent applyStartEvent;
    info << applyStartEvent
        << "[MemoryDock] driverApplyMemoryDiffFromUi: 开始应用差异, hasSnapshot="
        << (m_driverMemoryHasSnapshot ? "true" : "false")
        << ", cacheBytes="
        << m_driverMemoryEditedBytes.size()
        << eol;

    if (m_driverMemorySnapshotPid == 0U || !m_driverMemoryHasSnapshot)
    {
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText("没有可应用的 R0 读取快照。");
        }
        QMessageBox::warning(this, "驱动内存读写", "没有可应用的 R0 读取快照。");
        return;
    }

    // 以 HexEditor 当前数据为准，避免遗漏直接粘贴/编辑导致的缓存变化。
    m_driverMemoryEditedBytes = m_driverMemoryHexEditor->data();
    std::vector<DriverDiffBlock> diffBlocks;
    collectDriverMemoryDiffBlocks(diffBlocks);
    if (diffBlocks.empty())
    {
        m_driverMemoryApplyButton->setEnabled(false);
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText("没有检测到差异，无需写入。");
        }
        return;
    }

    // 写入前二次确认，明确这是修改真实进程内存。
    const QMessageBox::StandardButton confirmResult = QMessageBox::question(
        this,
        "应用内存差异",
        QString("将通过 R0 写入 %1 个差异块到 PID=%2%3。\n只写入和原始备份不同的字节，是否继续？")
        .arg(diffBlocks.size())
        .arg(m_driverMemorySnapshotPid)
        .arg(m_driverMemorySnapshotProcessName.isEmpty() ? QString() : QString(" (%1)").arg(m_driverMemorySnapshotProcessName)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText("用户取消应用差异。");
        }
        return;
    }

    // 按块调用驱动写入，单块超过驱动上限时拆分。
    ksword::ark::DriverClient driverClient;
    std::uint64_t totalRequested = 0;
    std::uint64_t totalWritten = 0;
    int failedBlockCount = 0;
    bool forceWriteApproved = false;
    QString lastFailureText;

    for (const DriverDiffBlock& block : diffBlocks)
    {
        int offset = 0;
        while (offset < block.bytes.size())
        {
            const int chunkBytes = std::min<int>(
                block.bytes.size() - offset,
                static_cast<int>(KSWORD_ARK_MEMORY_WRITE_MAX_BYTES));
            std::vector<std::uint8_t> chunk;
            chunk.resize(static_cast<std::size_t>(chunkBytes));
            std::copy_n(
                reinterpret_cast<const std::uint8_t*>(block.bytes.constData() + offset),
                static_cast<std::size_t>(chunkBytes),
                chunk.begin());

            const std::uint64_t chunkAddress =
                block.address + static_cast<std::uint64_t>(offset);
            unsigned long writeFlags = forceWriteApproved ?
                KSWORD_ARK_MEMORY_WRITE_FLAG_FORCE :
                0UL;
            ksword::ark::VirtualMemoryWriteResult writeResult =
                driverClient.writeVirtualMemory(
                    m_driverMemorySnapshotPid,
                    chunkAddress,
                    chunk,
                    writeFlags);

            if (writeResult.io.ok &&
                writeResult.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_FORCE_REQUIRED)
            {
                const QString forcePromptText =
                    QString("驱动拒绝了普通内存写入请求。\n地址=%1\n请求=%2 字节\nR0 信息：%3")
                    .arg(formatAddress(chunkAddress))
                    .arg(chunkBytes)
                    .arg(QString::fromStdString(writeResult.io.message));
                if (!confirmForceDriverMemoryWrite(
                    chunkAddress,
                    static_cast<std::uint32_t>(chunkBytes),
                    forcePromptText))
                {
                    ++failedBlockCount;
                    lastFailureText = QString("用户未强制继续，地址=%1 请求=%2。")
                        .arg(formatAddress(chunkAddress))
                        .arg(chunkBytes);
                    break;
                }

                forceWriteApproved = true;
                writeFlags = KSWORD_ARK_MEMORY_WRITE_FLAG_FORCE;
                writeResult = driverClient.writeVirtualMemory(
                    m_driverMemorySnapshotPid,
                    chunkAddress,
                    chunk,
                    writeFlags);
            }

            totalRequested += static_cast<std::uint64_t>(chunkBytes);
            totalWritten += static_cast<std::uint64_t>(writeResult.bytesWritten);
            if (!writeResult.io.ok ||
                writeResult.writeStatus != KSWORD_ARK_MEMORY_WRITE_STATUS_OK ||
                writeResult.bytesWritten != static_cast<std::uint32_t>(chunkBytes))
            {
                ++failedBlockCount;
                lastFailureText = QString("地址=%1 请求=%2 写入=%3 状态=%4 NT=0x%5 信息=%6")
                    .arg(formatAddress(chunkAddress))
                    .arg(chunkBytes)
                    .arg(writeResult.bytesWritten)
                    .arg(writeResult.writeStatus)
                    .arg(static_cast<unsigned long>(writeResult.copyStatus), 8, 16, QChar('0'))
                    .arg(QString::fromStdString(writeResult.io.message));
                break;
            }

            offset += chunkBytes;
        }

        if (failedBlockCount > 0)
        {
            break;
        }
    }

    // 成功写入的情况下，把当前编辑缓存提升为新备份，避免重复应用同一差异。
    if (failedBlockCount == 0)
    {
        m_driverMemoryOriginalBytes = m_driverMemoryEditedBytes;
        m_driverMemoryApplyButton->setEnabled(false);
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText(
                QString("应用完成：差异块=%1，请求写入=%2 字节，实际写入=%3 字节。")
                .arg(diffBlocks.size())
                .arg(static_cast<qulonglong>(totalRequested))
                .arg(static_cast<qulonglong>(totalWritten)));
        }
    }
    else
    {
        if (m_driverMemoryStatusLabel != nullptr)
        {
            m_driverMemoryStatusLabel->setText(
                QString("应用部分失败：请求=%1 字节，已写=%2 字节，失败块=%3，%4")
                .arg(static_cast<qulonglong>(totalRequested))
                .arg(static_cast<qulonglong>(totalWritten))
                .arg(failedBlockCount)
                .arg(lastFailureText));
            QMessageBox::warning(this, "驱动内存读写", m_driverMemoryStatusLabel->text());
        }
    }
}

void MemoryDock::resetDriverMemoryRwState()
{
    // 清空缓存日志：记录清空前状态。
    kLogEvent resetEvent;
    dbg << resetEvent
        << "[MemoryDock] resetDriverMemoryRwState: 清空驱动读写页缓存。"
        << eol;

    m_driverMemoryBaseAddress = 0;
    m_driverMemoryOffsetBase = 0;
    m_driverMemoryCenterAddress = 0;
    m_driverMemorySnapshotPid = 0;
    m_driverMemorySnapshotProcessName.clear();
    m_driverMemoryOriginalBytes.clear();
    m_driverMemoryEditedBytes.clear();
    m_driverMemoryHasSnapshot = false;

    if (m_driverMemoryHexEditor != nullptr)
    {
        m_driverMemoryHexEditor->clearData();
        m_driverMemoryHexEditor->setEditable(false);
    }
    if (m_driverMemoryApplyButton != nullptr)
    {
        m_driverMemoryApplyButton->setEnabled(false);
    }
    if (m_driverMemoryRangeLabel != nullptr)
    {
        m_driverMemoryRangeLabel->setText("范围: 未读取");
    }
    if (m_driverMemoryStatusLabel != nullptr)
    {
        m_driverMemoryStatusLabel->setText("缓存已清空。");
    }
}

bool MemoryDock::confirmForceDriverMemoryWrite(
    const std::uint64_t blockAddress,
    const std::uint32_t requestedBytes,
    const QString& failureText)
{
    // 强制确认入口：普通写入被 R0 拒绝后才会走到这里。
    QMessageBox warningBox(this);
    warningBox.setIcon(QMessageBox::Warning);
    warningBox.setWindowTitle(QStringLiteral("强制写入确认"));
    warningBox.setText(QStringLiteral("R0 已拒绝普通内存写入请求。"));
    warningBox.setInformativeText(
        QStringLiteral("目标 PID=%1\n目标地址=%2\n请求长度=%3 字节\n\n%4\n\n强制继续会绕过本次普通请求保护，只应在确认目标进程和地址无误时使用。")
        .arg(m_driverMemorySnapshotPid)
        .arg(formatAddress(blockAddress))
        .arg(requestedBytes)
        .arg(failureText));
    warningBox.setStandardButtons(QMessageBox::Cancel);
    warningBox.setDefaultButton(QMessageBox::Cancel);

    // 自定义按钮用于明确表达 force 语义，避免把普通 Yes/Ok 误当成强制写入。
    QPushButton* const forceButton =
        warningBox.addButton(QStringLiteral("强制继续"), QMessageBox::DestructiveRole);
    warningBox.exec();

    // 返回值只在用户点中强制按钮时为 true；关闭窗口或取消均停止写入。
    return warningBox.clickedButton() == forceButton;
}

void MemoryDock::collectDriverMemoryDiffBlocks(std::vector<DriverDiffBlock>& diffBlocksOut) const
{
    // 差异收集入口：输出容器由调用方持有，这里先清空。
    diffBlocksOut.clear();
    if (!m_driverMemoryHasSnapshot ||
        m_driverMemoryOriginalBytes.size() != m_driverMemoryEditedBytes.size())
    {
        return;
    }

    // 扫描整段缓存，把相邻变化字节合并为连续块，减少 IOCTL 次数。
    int index = 0;
    while (index < m_driverMemoryOriginalBytes.size())
    {
        if (m_driverMemoryOriginalBytes.at(index) == m_driverMemoryEditedBytes.at(index))
        {
            ++index;
            continue;
        }

        const int blockStart = index;
        while (index < m_driverMemoryOriginalBytes.size() &&
            m_driverMemoryOriginalBytes.at(index) != m_driverMemoryEditedBytes.at(index))
        {
            ++index;
        }

        DriverDiffBlock block{};
        block.address = m_driverMemoryBaseAddress + static_cast<std::uint64_t>(blockStart);
        block.bytes = m_driverMemoryEditedBytes.mid(blockStart, index - blockStart);
        diffBlocksOut.push_back(std::move(block));
    }
}

