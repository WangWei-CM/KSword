#include "BootEditorTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>

namespace
{
    // 分离文件内重复常量：
    // - kColumnIdentifier 与主文件保持一致；
    // - kDefaultCommandTimeoutMs 与主文件保持一致。
    constexpr int kColumnIdentifier = 0;
    constexpr int kDefaultCommandTimeoutMs = 30000;

    // parseBoolText：
    // - 从字符串解释布尔值；
    // - 识别失败时回退 defaultValue。
    bool parseBoolText(const QString& rawText, const bool defaultValue)
    {
        const QString normalizedText = rawText.trimmed().toLower();
        if (normalizedText == QStringLiteral("yes")
            || normalizedText == QStringLiteral("on")
            || normalizedText == QStringLiteral("true")
            || normalizedText == QStringLiteral("1")
            || normalizedText == QStringLiteral("enabled"))
        {
            return true;
        }
        if (normalizedText == QStringLiteral("no")
            || normalizedText == QStringLiteral("off")
            || normalizedText == QStringLiteral("false")
            || normalizedText == QStringLiteral("0")
            || normalizedText == QStringLiteral("disabled"))
        {
            return false;
        }
        return defaultValue;
    }
}

void BootEditorTab::applySelectedEntryChanges()
{
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }

    const QString identifierText = selectedEntry->identifierText.trimmed();
    if (identifierText.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("引导编辑器"), QStringLiteral("当前条目标识符为空，无法写入。"));
        return;
    }

    // 批量写入策略：
    // - 按字段逐条执行 bcdedit /set；
    // - 任一步失败就终止后续写入，避免状态部分落地。
    if (!m_descriptionEdit->text().trimmed().isEmpty())
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("description"), m_descriptionEdit->text().trimmed() },
            QStringLiteral("写入 description"),
            false))
        {
            return;
        }
    }
    if (!m_deviceEdit->text().trimmed().isEmpty())
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("device"), m_deviceEdit->text().trimmed() },
            QStringLiteral("写入 device"),
            false))
        {
            return;
        }
    }
    if (!m_osDeviceEdit->text().trimmed().isEmpty())
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("osdevice"), m_osDeviceEdit->text().trimmed() },
            QStringLiteral("写入 osdevice"),
            false))
        {
            return;
        }
    }
    if (!m_pathEdit->text().trimmed().isEmpty())
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("path"), m_pathEdit->text().trimmed() },
            QStringLiteral("写入 path"),
            false))
        {
            return;
        }
    }
    if (!m_systemRootEdit->text().trimmed().isEmpty())
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("systemroot"), m_systemRootEdit->text().trimmed() },
            QStringLiteral("写入 systemroot"),
            false))
        {
            return;
        }
    }
    if (!m_localeEdit->text().trimmed().isEmpty())
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("locale"), m_localeEdit->text().trimmed() },
            QStringLiteral("写入 locale"),
            false))
        {
            return;
        }
    }

    const QString bootMenuPolicyValue = m_bootMenuPolicyCombo->currentData().toString().trimmed();
    if (!bootMenuPolicyValue.isEmpty())
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("bootmenupolicy"), bootMenuPolicyValue },
            QStringLiteral("写入 bootmenupolicy"),
            false))
        {
            return;
        }
    }

    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("testsigning"), boolToBcdOnOff(m_testSigningCheck->isChecked()) },
        QStringLiteral("写入 testsigning"),
        false))
    {
        return;
    }
    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("nointegritychecks"), boolToBcdOnOff(m_noIntegrityCheck->isChecked()) },
        QStringLiteral("写入 nointegritychecks"),
        false))
    {
        return;
    }
    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("debug"), boolToBcdOnOff(m_debugCheck->isChecked()) },
        QStringLiteral("写入 debug"),
        false))
    {
        return;
    }
    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("bootlog"), boolToBcdYesNo(m_bootLogCheck->isChecked()) },
        QStringLiteral("写入 bootlog"),
        false))
    {
        return;
    }
    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("basevideo"), boolToBcdYesNo(m_baseVideoCheck->isChecked()) },
        QStringLiteral("写入 basevideo"),
        false))
    {
        return;
    }
    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("recoveryenabled"), boolToBcdYesNo(m_recoveryEnabledCheck->isChecked()) },
        QStringLiteral("写入 recoveryenabled"),
        false))
    {
        return;
    }

    // safeboot 处理：
    // - 关闭模式：删除 safeboot 与 safebootalternateshell；
    // - 其余模式：按组合值写入。
    const QString safeBootModeValue = m_safeBootCombo->currentData().toString().trimmed().toLower();

    // deleteValueWithMissingTolerance：
    // - 统一处理 deletevalue 的返回；
    // - 若仅因“字段不存在”失败则视为目标状态已满足；
    // - 其它失败直接中止，避免最终误报“写入完成”。
    const auto deleteValueWithMissingTolerance = [this, &identifierText](const QString& elementName) -> bool
        {
            const QString operationText = QStringLiteral("删除 %1").arg(elementName);
            const BcdCommandResult deleteResult = runBcdEdit(
                QStringList{
                    QStringLiteral("/deletevalue"),
                    identifierText,
                    elementName
                },
                kDefaultCommandTimeoutMs,
                operationText);
            appendCommandLog(
                QStringLiteral("bcdedit /deletevalue %1 %2").arg(identifierText, elementName),
                deleteResult);

            if (deleteResult.startSucceeded && !deleteResult.timeout && deleteResult.exitCode == 0)
            {
                return true;
            }

            const QString outputLowerText = deleteResult.mergedOutputText.toLower();
            const bool likelyMissingElement =
                outputLowerText.contains(QStringLiteral("not found"))
                || outputLowerText.contains(QStringLiteral("cannot find"))
                || outputLowerText.contains(QStringLiteral("does not exist"))
                || deleteResult.mergedOutputText.contains(QStringLiteral("找不到"))
                || deleteResult.mergedOutputText.contains(QStringLiteral("不存在"))
                || deleteResult.mergedOutputText.contains(QStringLiteral("未找到"));

            if (likelyMissingElement)
            {
                kLogEvent warnEvent;
                warn << warnEvent
                    << "[BootEditor] deletevalue 字段不存在，按目标状态继续: element="
                    << elementName.toStdString()
                    << ", identifier="
                    << identifierText.toStdString()
                    << eol;
                return true;
            }

            QMessageBox::warning(
                this,
                QStringLiteral("引导编辑器"),
                QStringLiteral("%1失败：\n%2")
                .arg(operationText, deleteResult.mergedOutputText.trimmed()));
            return false;
        };

    if (safeBootModeValue == QStringLiteral("off"))
    {
        if (!deleteValueWithMissingTolerance(QStringLiteral("safeboot")))
        {
            return;
        }
        if (!deleteValueWithMissingTolerance(QStringLiteral("safebootalternateshell")))
        {
            return;
        }
    }
    else if (safeBootModeValue == QStringLiteral("minimal"))
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("safeboot"), QStringLiteral("minimal") },
            QStringLiteral("写入 safeboot=minimal"),
            false))
        {
            return;
        }
        if (!deleteValueWithMissingTolerance(QStringLiteral("safebootalternateshell")))
        {
            return;
        }
    }
    else if (safeBootModeValue == QStringLiteral("network"))
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("safeboot"), QStringLiteral("network") },
            QStringLiteral("写入 safeboot=network"),
            false))
        {
            return;
        }
        if (!deleteValueWithMissingTolerance(QStringLiteral("safebootalternateshell")))
        {
            return;
        }
    }
    else if (safeBootModeValue == QStringLiteral("alternateshell"))
    {
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("safeboot"), QStringLiteral("minimal") },
            QStringLiteral("写入 safeboot=minimal"),
            false))
        {
            return;
        }
        if (!runBcdAndExpectSuccess(
            QStringList{ QStringLiteral("/set"), identifierText, QStringLiteral("safebootalternateshell"), QStringLiteral("yes") },
            QStringLiteral("写入 safebootalternateshell=yes"),
            false))
        {
            return;
        }
    }

    QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("当前条目已写入完成。"));
    refreshBcdEntries();
}

void BootEditorTab::applyBootManagerChanges()
{
    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/timeout"), QString::number(m_timeoutSpin->value()) },
        QStringLiteral("写入 timeout"),
        true))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::setLegacyBootForSelectedEntry()
{
    // 当前项传统引导：
    // - 对当前选中条目写入 bootmenupolicy Legacy；
    // - 成功后刷新列表与右侧详情。
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("传统引导"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }

    if (!applyBootMenuPolicyByIdentifier(
        selectedEntry->identifierText.trimmed(),
        QStringLiteral("Legacy"),
        QStringLiteral("当前项启用传统引导")))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::setLegacyBootForDefaultEntry()
{
    // 默认项传统引导：
    // - 对 {bootmgr}.default 对应条目写入 bootmenupolicy Legacy；
    // - 若未读取到默认标识符则提示刷新后重试。
    const QString defaultIdentifierText = m_defaultIdentifierText.trimmed();
    if (defaultIdentifierText.isEmpty())
    {
        QMessageBox::warning(
            this,
            QStringLiteral("传统引导"),
            QStringLiteral("当前未识别到默认启动项，请先刷新 BCD。"));
        return;
    }

    if (!applyBootMenuPolicyByIdentifier(
        defaultIdentifierText,
        QStringLiteral("Legacy"),
        QStringLiteral("默认项启用传统引导")))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::setStandardBootForSelectedEntry()
{
    // 当前项恢复标准：
    // - 对当前选中条目写入 bootmenupolicy Standard；
    // - 用于回退“传统引导”快捷设置。
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("传统引导"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }

    if (!applyBootMenuPolicyByIdentifier(
        selectedEntry->identifierText.trimmed(),
        QStringLiteral("Standard"),
        QStringLiteral("当前项恢复标准引导")))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::setSelectedAsDefaultEntry()
{
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }
    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/default"), selectedEntry->identifierText.trimmed() },
        QStringLiteral("设置默认启动项"),
        true))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::addSelectedToBootSequence()
{
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }
    if (!runBcdAndExpectSuccess(
        QStringList{
            QStringLiteral("/bootsequence"),
            selectedEntry->identifierText.trimmed(),
            QStringLiteral("/addfirst")
        },
        QStringLiteral("设置下一次启动项"),
        true))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::createCopyFromSelectedEntry()
{
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }

    const QString sourceDescription = readElementValue(
        *selectedEntry,
        QStringList{ QStringLiteral("description"), QStringLiteral("描述") });
    const QString defaultNewDescription = sourceDescription.trimmed().isEmpty()
        ? QStringLiteral("新建引导项")
        : QStringLiteral("%1 - 副本").arg(sourceDescription.trimmed());

    bool inputOk = false;
    const QString newDescription = QInputDialog::getText(
        this,
        QStringLiteral("复制引导项"),
        QStringLiteral("新引导项描述："),
        QLineEdit::Normal,
        defaultNewDescription,
        &inputOk).trimmed();
    if (!inputOk || newDescription.isEmpty())
    {
        return;
    }

    const BcdCommandResult copyResult = runBcdEdit(
        QStringList{
            QStringLiteral("/copy"),
            selectedEntry->identifierText.trimmed(),
            QStringLiteral("/d"),
            newDescription
        },
        kDefaultCommandTimeoutMs,
        QStringLiteral("复制引导项"));
    appendCommandLog(QStringLiteral("bcdedit /copy"), copyResult);

    if (!copyResult.startSucceeded || copyResult.timeout || copyResult.exitCode != 0)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("复制引导项"),
            QStringLiteral("复制失败：\n%1").arg(copyResult.mergedOutputText.trimmed()));
        return;
    }

    // newIdentifierRegex：
    // - 从命令输出中抽取新 GUID；
    // - 成功后自动追加到 displayorder 尾部。
    const QRegularExpression newIdentifierRegex(QStringLiteral("\\{[^\\}\\s]+\\}"));
    QRegularExpressionMatchIterator matchIterator = newIdentifierRegex.globalMatch(copyResult.mergedOutputText);
    QString newIdentifierText;
    while (matchIterator.hasNext())
    {
        const QRegularExpressionMatch match = matchIterator.next();
        newIdentifierText = match.captured(0);
    }

    if (!newIdentifierText.trimmed().isEmpty())
    {
        runBcdAndExpectSuccess(
            QStringList{
                QStringLiteral("/displayorder"),
                newIdentifierText.trimmed(),
                QStringLiteral("/addlast")
            },
            QStringLiteral("追加到 displayorder"),
            false);
    }

    QMessageBox::information(this, QStringLiteral("复制引导项"), QStringLiteral("复制成功。"));
    refreshBcdEntries();
}

void BootEditorTab::deleteSelectedEntry()
{
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }

    const QString identifierText = selectedEntry->identifierText.trimmed();
    if (identifierText.compare(QStringLiteral("{bootmgr}"), Qt::CaseInsensitive) == 0)
    {
        QMessageBox::warning(this, QStringLiteral("删除引导项"), QStringLiteral("不允许删除 {bootmgr}。"));
        return;
    }
    if (identifierText.compare(QStringLiteral("{current}"), Qt::CaseInsensitive) == 0)
    {
        QMessageBox::warning(this, QStringLiteral("删除引导项"), QStringLiteral("不允许删除 {current}。"));
        return;
    }

    const int confirmResult = QMessageBox::warning(
        this,
        QStringLiteral("删除引导项"),
        QStringLiteral("即将删除条目：%1\n该操作不可撤销，是否继续？").arg(identifierText),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/delete"), identifierText },
        QStringLiteral("删除引导项"),
        true))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::exportBcdStore()
{
    const QString defaultFileName = QStringLiteral("bcd_backup_%1.bcd")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出 BCD"),
        defaultFileName,
        QStringLiteral("BCD 文件 (*.bcd);;所有文件 (*.*)"));
    if (outputPath.trimmed().isEmpty())
    {
        return;
    }

    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/export"), outputPath },
        QStringLiteral("导出 BCD"),
        true))
    {
        return;
    }
}

void BootEditorTab::importBcdStore()
{
    const QString inputPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("导入 BCD"),
        QString(),
        QStringLiteral("BCD 文件 (*.bcd);;所有文件 (*.*)"));
    if (inputPath.trimmed().isEmpty())
    {
        return;
    }

    const int confirmResult = QMessageBox::warning(
        this,
        QStringLiteral("导入 BCD"),
        QStringLiteral("导入会覆盖当前 BCD 存储，可能影响系统启动。\n确认继续吗？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    if (!runBcdAndExpectSuccess(
        QStringList{ QStringLiteral("/import"), inputPath },
        QStringLiteral("导入 BCD"),
        true))
    {
        return;
    }
    refreshBcdEntries();
}

void BootEditorTab::executeCustomCommand()
{
    const QString rawCommandText = m_customCommandEdit->text().trimmed();
    if (rawCommandText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("自定义命令"), QStringLiteral("请输入命令参数。"));
        return;
    }

    // 参数解析策略：
    // - 支持“/set ...”；
    // - 也支持“bcdedit /set ...”，会自动去掉前缀 program。
    QStringList argumentList = QProcess::splitCommand(rawCommandText);
    if (!argumentList.isEmpty()
        && argumentList.front().compare(QStringLiteral("bcdedit"), Qt::CaseInsensitive) == 0)
    {
        argumentList.removeFirst();
    }
    if (argumentList.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("自定义命令"), QStringLiteral("未解析到有效参数。"));
        return;
    }

    const BcdCommandResult commandResult = runBcdEdit(
        argumentList,
        kDefaultCommandTimeoutMs,
        QStringLiteral("执行自定义 bcdedit"));
    appendCommandLog(QStringLiteral("bcdedit %1").arg(argumentList.join(' ')), commandResult);

    if (!commandResult.startSucceeded || commandResult.timeout || commandResult.exitCode != 0)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("自定义命令"),
            QStringLiteral("命令执行失败：\n%1").arg(commandResult.mergedOutputText.trimmed()));
        return;
    }

    QMessageBox::information(this, QStringLiteral("自定义命令"), QStringLiteral("命令执行成功。"));

    const QString normalizedCommandText = rawCommandText.toLower();
    if (!normalizedCommandText.contains(QStringLiteral("/enum")))
    {
        refreshBcdEntries();
    }
}

void BootEditorTab::copySelectedRowToClipboard()
{
    const int rowIndex = m_entryTable->currentRow();
    if (rowIndex < 0)
    {
        return;
    }

    QStringList cellTextList;
    for (int colIndex = 0; colIndex < m_entryTable->columnCount(); ++colIndex)
    {
        QTableWidgetItem* item = m_entryTable->item(rowIndex, colIndex);
        cellTextList.push_back(item != nullptr ? item->text() : QString());
    }
    QApplication::clipboard()->setText(cellTextList.join('\t'));
}

int BootEditorTab::currentEntryIndex() const
{
    const int rowIndex = m_entryTable->currentRow();
    if (rowIndex < 0)
    {
        return -1;
    }
    QTableWidgetItem* identifierItem = m_entryTable->item(rowIndex, kColumnIdentifier);
    if (identifierItem == nullptr)
    {
        return -1;
    }
    return identifierItem->data(Qt::UserRole).toInt();
}

const BootEditorTab::BcdEntry* BootEditorTab::currentEntry() const
{
    const int index = currentEntryIndex();
    if (index < 0 || index >= static_cast<int>(m_entryList.size()))
    {
        return nullptr;
    }
    return &m_entryList[static_cast<std::size_t>(index)];
}

bool BootEditorTab::entryMatchesFilter(const BcdEntry& entry) const
{
    const QString keywordText = m_filterEdit->text().trimmed();
    if (keywordText.isEmpty())
    {
        return true;
    }
    const QString lowerKeyword = keywordText.toLower();
    const QString descriptionText = readElementValue(entry, QStringList{
        QStringLiteral("description"),
        QStringLiteral("描述")
        });
    const QString pathText = readElementValue(entry, QStringList{
        QStringLiteral("path"),
        QStringLiteral("路径")
        });
    const QString deviceText = readElementValue(entry, QStringList{
        QStringLiteral("device"),
        QStringLiteral("设备")
        });

    return entry.identifierText.toLower().contains(lowerKeyword)
        || entry.objectTypeText.toLower().contains(lowerKeyword)
        || descriptionText.toLower().contains(lowerKeyword)
        || pathText.toLower().contains(lowerKeyword)
        || deviceText.toLower().contains(lowerKeyword);
}

QString BootEditorTab::readElementValue(
    const BcdEntry& entry,
    const QStringList& candidateKeyList) const
{
    // 只做精确键匹配：
    // - 避免 device/osdevice 等相似字段被 contains 误命中；
    // - 若字段缺失则返回空，交给调用方决定后续行为。
    for (const QString& candidateRawKey : candidateKeyList)
    {
        const QString normalizedCandidateKey = normalizeElementKey(candidateRawKey);
        if (entry.elementMap.contains(normalizedCandidateKey))
        {
            return entry.elementMap.value(normalizedCandidateKey);
        }
    }
    return QString();
}

bool BootEditorTab::readElementBool(
    const BcdEntry& entry,
    const QStringList& candidateKeyList,
    const bool defaultValue) const
{
    const QString valueText = readElementValue(entry, candidateKeyList);
    return parseBoolText(valueText, defaultValue);
}

void BootEditorTab::appendCommandLog(
    const QString& commandTitle,
    const BcdCommandResult& commandResult)
{
    // 日志块格式：
    // - 首行展示时间与命令标题；
    // - 第二行开始展示原始输出；
    // - 末尾追加执行状态摘要。
    const QString timestampText = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    QString statusText = QStringLiteral("start=%1, timeout=%2, exit=%3")
        .arg(commandResult.startSucceeded ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(commandResult.timeout ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(commandResult.exitCode);
    QString logText = QStringLiteral("[%1] %2\n%3\n[%4]\n\n")
        .arg(timestampText)
        .arg(commandTitle)
        .arg(commandResult.mergedOutputText.trimmed())
        .arg(statusText);

    m_rawOutputEdit->moveCursor(QTextCursor::End);
    m_rawOutputEdit->insertPlainText(logText);
    m_rawOutputEdit->moveCursor(QTextCursor::End);
}

BootEditorTab::BcdCommandResult BootEditorTab::runBcdEdit(
    const QStringList& argumentList,
    const int timeoutMs,
    const QString& commandDescription)
{
    BcdCommandResult result;
    QProcess process(this);

    process.setProgram(QStringLiteral("bcdedit"));
    process.setArguments(argumentList);
    process.setProcessChannelMode(QProcess::SeparateChannels);

    process.start();
    result.startSucceeded = process.waitForStarted(3000);
    if (!result.startSucceeded)
    {
        result.standardErrorText = process.errorString();
        result.mergedOutputText = result.standardErrorText;
        kLogEvent event;
        err << event
            << "[BootEditor] 命令启动失败: "
            << commandDescription.toStdString()
            << ", error="
            << result.standardErrorText.toStdString()
            << eol;
        return result;
    }

    // 可响应等待策略：
    // - 使用短周期 waitForFinished + processEvents；
    // - 避免单次长阻塞造成“界面假死”。
    QElapsedTimer elapsedTimer;
    elapsedTimer.start();
    while (process.state() != QProcess::NotRunning)
    {
        if (process.waitForFinished(50))
        {
            break;
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        if (elapsedTimer.elapsed() >= timeoutMs)
        {
            result.timeout = true;
            process.kill();
            process.waitForFinished(1000);
            break;
        }
    }

    if (result.timeout)
    {
        result.standardOutputText = QString::fromLocal8Bit(process.readAllStandardOutput());
        result.standardErrorText = QString::fromLocal8Bit(process.readAllStandardError());
        result.mergedOutputText = result.standardOutputText + QStringLiteral("\n") + result.standardErrorText;

        kLogEvent timeoutEvent;
        err << timeoutEvent
            << "[BootEditor] 命令执行超时: "
            << commandDescription.toStdString()
            << ", timeoutMs="
            << timeoutMs
            << eol;
        return result;
    }

    result.exitCode = process.exitCode();
    result.standardOutputText = QString::fromLocal8Bit(process.readAllStandardOutput());
    result.standardErrorText = QString::fromLocal8Bit(process.readAllStandardError());
    result.mergedOutputText = result.standardOutputText;
    if (!result.standardErrorText.trimmed().isEmpty())
    {
        if (!result.mergedOutputText.trimmed().isEmpty())
        {
            result.mergedOutputText += QStringLiteral("\n");
        }
        result.mergedOutputText += result.standardErrorText;
    }

    kLogEvent event;
    if (result.exitCode == 0)
    {
        info << event
            << "[BootEditor] 命令执行完成: "
            << commandDescription.toStdString()
            << ", exitCode="
            << result.exitCode
            << eol;
    }
    else
    {
        warn << event
            << "[BootEditor] 命令返回非零: "
            << commandDescription.toStdString()
            << ", exitCode="
            << result.exitCode
            << eol;
    }
    return result;
}

bool BootEditorTab::runBcdAndExpectSuccess(
    const QStringList& argumentList,
    const QString& operationText,
    const bool showSuccessToast)
{
    const BcdCommandResult result = runBcdEdit(argumentList, kDefaultCommandTimeoutMs, operationText);
    appendCommandLog(QStringLiteral("bcdedit %1").arg(argumentList.join(' ')), result);

    if (!result.startSucceeded)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("引导编辑器"),
            QStringLiteral("%1失败：命令未能启动。\n%2").arg(operationText, result.mergedOutputText.trimmed()));
        return false;
    }
    if (result.timeout)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("引导编辑器"),
            QStringLiteral("%1失败：命令执行超时。").arg(operationText));
        return false;
    }
    if (result.exitCode != 0)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("引导编辑器"),
            QStringLiteral("%1失败：\n%2").arg(operationText, result.mergedOutputText.trimmed()));
        return false;
    }

    if (showSuccessToast)
    {
        QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("%1成功。").arg(operationText));
    }
    return true;
}

bool BootEditorTab::applyBootMenuPolicyByIdentifier(
    const QString& identifierText,
    const QString& policyValueText,
    const QString& operationText)
{
    // 公共策略写入：
    // - 统一校验标识符与目标策略参数；
    // - 成功后给出明确结果提示。
    const QString normalizedIdentifierText = identifierText.trimmed();
    const QString normalizedPolicyText = policyValueText.trimmed();
    if (normalizedIdentifierText.isEmpty() || normalizedPolicyText.isEmpty())
    {
        QMessageBox::warning(
            this,
            QStringLiteral("传统引导"),
            QStringLiteral("参数无效：标识符或策略为空。"));
        return false;
    }

    if (!runBcdAndExpectSuccess(
        QStringList{
            QStringLiteral("/set"),
            normalizedIdentifierText,
            QStringLiteral("bootmenupolicy"),
            normalizedPolicyText
        },
        operationText,
        false))
    {
        return false;
    }

    QMessageBox::information(
        this,
        QStringLiteral("传统引导"),
        QStringLiteral("%1成功。\n标识符：%2\n策略：%3")
        .arg(operationText)
        .arg(normalizedIdentifierText)
        .arg(normalizedPolicyText));
    return true;
}

QString BootEditorTab::normalizeElementKey(const QString& rawKeyText)
{
    // 规范化规则：
    // - 转小写；
    // - 去除空格、连字符、下划线与制表符；
    // - 保留英数字与常见中文字段名。
    QString normalizedText = rawKeyText.trimmed().toLower();
    normalizedText.remove(QRegularExpression(QStringLiteral("[\\s\\-_:]")));
    return normalizedText;
}

std::vector<BootEditorTab::BcdEntry> BootEditorTab::parseBcdEnumOutput(const QString& enumOutputText)
{
    std::vector<BcdEntry> entryList;

    BcdEntry currentEntry;
    QStringList currentRawLineList;
    bool insideBlock = false;
    bool nextNonEmptyLineIsType = false;

    // flushCurrent：
    // - 将当前构建中的对象推入结果；
    // - 清理状态以便开始下一对象。
    const auto flushCurrent = [&entryList, &currentEntry, &currentRawLineList]()
        {
            if (currentEntry.identifierText.trimmed().isEmpty()
                && currentEntry.elementMap.isEmpty()
                && currentEntry.objectTypeText.trimmed().isEmpty())
            {
                currentRawLineList.clear();
                currentEntry = BcdEntry();
                return;
            }

            const QString idLowerText = currentEntry.identifierText.trimmed().toLower();
            currentEntry.isBootManager = (idLowerText == QStringLiteral("{bootmgr}"));
            currentEntry.isCurrent = (idLowerText == QStringLiteral("{current}"));
            currentEntry.rawBlockText = currentRawLineList.join('\n');
            entryList.push_back(currentEntry);

            currentEntry = BcdEntry();
            currentRawLineList.clear();
        };

    const QStringList allLineList = enumOutputText.split('\n');
    const QRegularExpression separatorRegex(QStringLiteral("^-{3,}$"));
    const QRegularExpression pairRegex(QStringLiteral("^(.+?)\\s{2,}(.+)$"));

    for (const QString& rawLine : allLineList)
    {
        const QString lineText = rawLine;
        const QString trimmedLineText = lineText.trimmed();

        if (trimmedLineText.isEmpty())
        {
            if (insideBlock)
            {
                currentRawLineList.push_back(lineText);
            }
            continue;
        }

        if (separatorRegex.match(trimmedLineText).hasMatch())
        {
            flushCurrent();
            insideBlock = true;
            nextNonEmptyLineIsType = true;
            currentRawLineList.push_back(lineText);
            continue;
        }

        if (!insideBlock)
        {
            continue;
        }

        currentRawLineList.push_back(lineText);

        if (nextNonEmptyLineIsType)
        {
            currentEntry.objectTypeText = trimmedLineText;
            nextNonEmptyLineIsType = false;
            continue;
        }

        const QRegularExpressionMatch pairMatch = pairRegex.match(lineText);
        if (!pairMatch.hasMatch())
        {
            continue;
        }

        const QString keyText = pairMatch.captured(1).trimmed();
        const QString valueText = pairMatch.captured(2).trimmed();
        const QString normalizedKeyText = normalizeElementKey(keyText);

        if (!normalizedKeyText.isEmpty())
        {
            currentEntry.elementMap.insert(normalizedKeyText, valueText);
        }

        const bool maybeIdentifierKey =
            normalizedKeyText.contains(QStringLiteral("identifier"))
            || normalizedKeyText.contains(QStringLiteral("标识符"))
            || normalizedKeyText.contains(QStringLiteral("识别符"));
        if (currentEntry.identifierText.trimmed().isEmpty())
        {
            if (maybeIdentifierKey)
            {
                currentEntry.identifierText = valueText;
            }
            else if (valueText.startsWith('{') && valueText.endsWith('}'))
            {
                currentEntry.identifierText = valueText;
            }
        }
    }

    flushCurrent();
    return entryList;
}

QString BootEditorTab::boolToBcdOnOff(const bool enabled)
{
    return enabled ? QStringLiteral("on") : QStringLiteral("off");
}

QString BootEditorTab::boolToBcdYesNo(const bool enabled)
{
    return enabled ? QStringLiteral("yes") : QStringLiteral("no");
}
