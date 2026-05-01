#include "HandleDock.h"

// ============================================================
// HandleDock.Filter.cpp
// 作用：
// - 承载句柄模块的本地过滤、状态文本与右键菜单逻辑；
// - 避免主 UI 文件过长；
// - 让“枚举”和“本地交互”职责分离。
// ============================================================

#include "../theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <set>

void HandleDock::applyLocalHandleFilters()
{
    const QString pidText = (m_pidFilterEdit != nullptr) ? m_pidFilterEdit->text().trimmed() : QString();
    const QString keywordText = (m_keywordFilterEdit != nullptr) ? m_keywordFilterEdit->text().trimmed().toLower() : QString();
    const QString typeFilterText = (m_typeFilterCombo != nullptr) ? m_typeFilterCombo->currentText().trimmed() : QString();
    const HandleDiffStatus diffFilter = resolveHandleDiffFilterFromText(
        (m_diffFilterCombo != nullptr) ? m_diffFilterCombo->currentText().trimmed() : QString());
    const bool onlyNamed = (m_onlyNamedCheckBox != nullptr) && m_onlyNamedCheckBox->isChecked();

    bool pidParseOk = false;
    const std::uint32_t parsedPid = pidText.toUInt(&pidParseOk, 10);

    std::uint32_t selectedPid = 0;
    std::uint16_t selectedTypeIndex = 0;
    std::uint64_t selectedHandleValue = 0;
    std::uint64_t selectedObjectAddress = 0;
    if (HandleRow* selectedRow = selectedHandleRow())
    {
        selectedPid = selectedRow->processId;
        selectedTypeIndex = selectedRow->typeIndex;
        selectedHandleValue = selectedRow->handleValue;
        selectedObjectAddress = selectedRow->objectAddress;
    }

    m_rows.clear();
    m_rows.reserve(m_allRows.size());
    for (const HandleRow& row : m_allRows)
    {
        if (!pidText.isEmpty() && (!pidParseOk || row.processId != parsedPid))
        {
            continue;
        }
        if (typeFilterText != QStringLiteral("全部类型") &&
            !typeFilterText.trimmed().isEmpty() &&
            row.typeName != typeFilterText)
        {
            continue;
        }
        if (diffFilter != HandleDiffStatus::NotCompared && row.diffStatus != diffFilter)
        {
            continue;
        }
        if (onlyNamed && row.objectName.trimmed().isEmpty())
        {
            continue;
        }

        const QString normalizedKeyword = keywordText;
        if (!normalizedKeyword.isEmpty())
        {
            const QString pidValueText = QString::number(row.processId);
            const QString typeIndexText = QString::number(row.typeIndex);
            const QString handleText = QStringLiteral("0x%1").arg(static_cast<qulonglong>(row.handleValue), 0, 16).toLower();
            const QString addressText = QStringLiteral("0x%1").arg(static_cast<qulonglong>(row.objectAddress), 0, 16).toLower();
            const QString accessText = QStringLiteral("0x%1").arg(row.grantedAccess, 8, 16, QChar('0')).toLower();
            const QString accessDecodedText = decodeGrantedAccessText(row.typeName, row.grantedAccess).toLower();
            const QString sourceText = formatHandleSourceText(row.sourceMode).toLower();
            const QString decodeStatusText = formatHandleDecodeStatusText(row.decodeStatus).toLower();
            const QString diffStatusText = formatHandleDiffStatusText(row.diffStatus).toLower();

            const bool matched =
                row.processName.toLower().contains(normalizedKeyword) ||
                row.typeName.toLower().contains(normalizedKeyword) ||
                row.objectName.toLower().contains(normalizedKeyword) ||
                pidValueText.contains(normalizedKeyword) ||
                typeIndexText.contains(normalizedKeyword) ||
                handleText.contains(normalizedKeyword) ||
                addressText.contains(normalizedKeyword) ||
                accessText.contains(normalizedKeyword) ||
                accessDecodedText.contains(normalizedKeyword) ||
                sourceText.contains(normalizedKeyword) ||
                decodeStatusText.contains(normalizedKeyword) ||
                diffStatusText.contains(normalizedKeyword);
            if (!matched)
            {
                continue;
            }
        }
        m_rows.push_back(row);
    }

    rebuildHandleTable();

    for (int row = 0; row < m_tableWidget->topLevelItemCount(); ++row)
    {
        QTreeWidgetItem* item = m_tableWidget->topLevelItem(row);
        if (item == nullptr)
        {
            continue;
        }
        const QVariant rowIndexValue = item->data(static_cast<int>(HandleTableColumn::ProcessId), Qt::UserRole);
        if (!rowIndexValue.isValid())
        {
            continue;
        }
        const std::size_t rowIndex = static_cast<std::size_t>(rowIndexValue.toULongLong());
        if (rowIndex >= m_rows.size())
        {
            continue;
        }
        const HandleRow& rowData = m_rows[rowIndex];
        if (rowData.processId == selectedPid &&
            rowData.typeIndex == selectedTypeIndex &&
            rowData.handleValue == selectedHandleValue &&
            rowData.objectAddress == selectedObjectAddress)
        {
            m_tableWidget->setCurrentItem(item);
            return;
        }
    }

    if (m_tableWidget->topLevelItemCount() > 0)
    {
        m_tableWidget->setCurrentItem(m_tableWidget->topLevelItem(0));
    }
    else
    {
        showHandleDetailPlaceholder(QStringLiteral("当前过滤条件下无可见句柄。"));
    }
}

void HandleDock::updateTypeFilterItems(const std::vector<QString>& availableTypeList)
{
    const QString previousTypeText = m_typeFilterCombo->currentText();
    QSignalBlocker comboBlocker(m_typeFilterCombo);
    m_typeFilterCombo->clear();
    m_typeFilterCombo->addItem(QStringLiteral("全部类型"));
    for (const QString& typeNameText : availableTypeList)
    {
        m_typeFilterCombo->addItem(typeNameText);
    }

    const int previousIndex = m_typeFilterCombo->findText(previousTypeText);
    if (previousIndex >= 0)
    {
        m_typeFilterCombo->setCurrentIndex(previousIndex);
    }
}

void HandleDock::refreshTypeFilterItemsFromAllRows()
{
    std::set<QString> typeNameSet;
    for (const HandleRow& row : m_allRows)
    {
        if (!row.typeName.trimmed().isEmpty())
        {
            typeNameSet.insert(row.typeName);
        }
    }

    std::vector<QString> availableTypeList;
    availableTypeList.reserve(typeNameSet.size());
    for (const QString& typeNameText : typeNameSet)
    {
        availableTypeList.push_back(typeNameText);
    }
    updateTypeFilterItems(availableTypeList);
}

void HandleDock::syncHandleTypeNamesFromObjectTypeMap()
{
    if (m_allRows.empty() || m_typeNameMapByIndexFromObjectTab.empty())
    {
        return;
    }

    for (HandleRow& row : m_allRows)
    {
        const auto foundIt = m_typeNameMapByIndexFromObjectTab.find(row.typeIndex);
        if (foundIt != m_typeNameMapByIndexFromObjectTab.end() && !foundIt->second.empty())
        {
            row.typeName = QString::fromStdString(foundIt->second);
        }
    }
    refreshTypeFilterItemsFromAllRows();
    applyLocalHandleFilters();

    kLogEvent syncTypeNameEvent;
    info << syncTypeNameEvent
        << "[HandleDock] syncHandleTypeNamesFromObjectTypeMap: syncedRows="
        << m_allRows.size()
        << ", mappedTypes="
        << m_typeNameMapByIndexFromObjectTab.size()
        << eol;
}

void HandleDock::updateHandleStatusLabel(const QString& statusText, const bool refreshing)
{
    if (m_statusLabel == nullptr)
    {
        return;
    }
    m_statusLabel->setText(statusText);
    if (refreshing)
    {
        m_statusLabel->setStyleSheet(
            QStringLiteral("color:%1;font-weight:700;")
            .arg(KswordTheme::PrimaryBlueHex));
        return;
    }

    const bool hasDiagnostic =
        statusText.contains(QStringLiteral("失败")) ||
        statusText.contains(QStringLiteral("预算")) ||
        statusText.contains(QStringLiteral("异常")) ||
        statusText.contains(QStringLiteral("截断"));
    const QString textColor = hasDiagnostic
        ? QStringLiteral("#D77A00")
        : QStringLiteral("#3A8F3A");
    m_statusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
        .arg(textColor));
}

void HandleDock::updateObjectTypeStatusLabel(const QString& statusText, const bool refreshing)
{
    if (m_objectTypeStatusLabel == nullptr)
    {
        return;
    }
    m_objectTypeStatusLabel->setText(statusText);
    if (refreshing)
    {
        m_objectTypeStatusLabel->setStyleSheet(
            QStringLiteral("color:%1;font-weight:700;")
            .arg(KswordTheme::PrimaryBlueHex));
        return;
    }

    const bool hasDiagnostic = statusText.contains(QStringLiteral("失败"));
    const QString textColor = hasDiagnostic
        ? QStringLiteral("#D77A00")
        : QStringLiteral("#3A8F3A");
    m_objectTypeStatusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
        .arg(textColor));
}

void HandleDock::focusObjectTypeByIndex(const std::uint16_t typeIndex)
{
    if (m_tabWidget != nullptr && m_objectTypePage != nullptr)
    {
        m_tabWidget->setCurrentWidget(m_objectTypePage);
    }

    if (m_objectTypeRows.empty() && !m_objectTypeRefreshInProgress)
    {
        requestObjectTypeRefreshAsync(true);
        return;
    }

    if (m_objectTypeFilterEdit != nullptr)
    {
        m_objectTypeFilterEdit->setText(QString::number(typeIndex));
    }

    for (int row = 0; row < m_objectTypeTable->topLevelItemCount(); ++row)
    {
        QTreeWidgetItem* item = m_objectTypeTable->topLevelItem(row);
        if (item == nullptr)
        {
            continue;
        }
        if (item->text(static_cast<int>(ObjectTypeTableColumn::TypeIndex)).toUInt() == typeIndex)
        {
            m_objectTypeTable->setCurrentItem(item);
            break;
        }
    }
}

void HandleDock::showHandleTableContextMenu(const QPoint& localPosition)
{
    QTreeWidgetItem* clickedItem = m_tableWidget->itemAt(localPosition);
    if (clickedItem == nullptr)
    {
        return;
    }
    m_tableWidget->setCurrentItem(clickedItem);

    QMenu menu(this);
    // 显式填充菜单背景，避免浅色模式下继承透明样式出现黑底。
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* copyCellAction = menu.addAction(QIcon(":/Icon/handle_copy.svg"), QStringLiteral("复制单元格"));
    QAction* copyRowAction = menu.addAction(QIcon(":/Icon/handle_copy_row.svg"), QStringLiteral("复制整行"));
    menu.addSeparator();
    QAction* closeHandleAction = menu.addAction(QIcon(":/Icon/handle_close.svg"), QStringLiteral("关闭句柄"));
    QAction* closeBatchAction = menu.addAction(QIcon(":/Icon/handle_close.svg"), QStringLiteral("批量关闭同类型句柄"));
    menu.addSeparator();
    QAction* gotoTypeAction = menu.addAction(QIcon(":/Icon/process_tree.svg"), QStringLiteral("转到对象类型"));
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/handle_refresh.svg"), QStringLiteral("刷新"));

    QAction* selectedAction = menu.exec(m_tableWidget->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }
    if (selectedAction == copyCellAction)
    {
        copyCurrentHandleCell();
        return;
    }
    if (selectedAction == copyRowAction)
    {
        copyCurrentHandleRow();
        return;
    }
    if (selectedAction == closeHandleAction)
    {
        closeCurrentHandle();
        return;
    }
    if (selectedAction == closeBatchAction)
    {
        closeSameTypeHandlesInCurrentProcess();
        return;
    }
    if (selectedAction == gotoTypeAction)
    {
        HandleRow* row = selectedHandleRow();
        if (row != nullptr)
        {
            focusObjectTypeByIndex(row->typeIndex);
        }
        return;
    }
    if (selectedAction == refreshAction)
    {
        requestAsyncRefresh(true);
    }
}
