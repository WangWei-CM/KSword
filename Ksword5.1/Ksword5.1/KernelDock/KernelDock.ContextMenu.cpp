
#include "KernelDock.h"

// ============================================================
// KernelDock.ContextMenu.cpp
// 作用说明：
// 1) 承载对象命名空间表格右键菜单；
// 2) 承载原子表右键菜单；
// 3) 实现复制与针对对象/原子的快捷操作。
// ============================================================

#include "KernelDockAtomWorker.h"
#include "KernelDockObjectNamespaceWorker.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QApplication>
#include <QClipboard>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QModelIndex>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>

namespace
{
    // safeText：
    // - 作用：复制时将空文本替换为占位符，避免 TSV 字段错位。
    QString safeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    // isNtDevicePath：
    // - 作用：判断路径是否为 NT 设备路径（\Device\...）。
    bool isNtDevicePath(const QString& pathText)
    {
        return pathText.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive);
    }

    // copyTextToClipboard：
    // - 作用：统一剪贴板写入逻辑。
    void copyTextToClipboard(const QString& contentText)
    {
        if (QApplication::clipboard() != nullptr)
        {
            QApplication::clipboard()->setText(contentText);
        }
    }

    // tableRowAsTsv：
    // - 作用：把当前表格行序列化为 TSV。
    QString tableRowAsTsv(const QTableWidget* tableWidget, const int rowIndex)
    {
        if (tableWidget == nullptr || rowIndex < 0 || rowIndex >= tableWidget->rowCount())
        {
            return QString();
        }

        QStringList fieldList;
        for (int columnIndex = 0; columnIndex < tableWidget->columnCount(); ++columnIndex)
        {
            const QTableWidgetItem* cellItem = tableWidget->item(rowIndex, columnIndex);
            fieldList.push_back(cellItem == nullptr ? QString() : cellItem->text());
        }
        return fieldList.join('\t');
    }

    // objectNamespaceEntryAsTsv：
    // - 作用：把对象命名空间条目序列化为 TSV。
    QString objectNamespaceEntryAsTsv(const KernelObjectNamespaceEntry& entry)
    {
        return QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9")
            .arg(
                safeText(entry.rootPathText),
                safeText(entry.scopeDescriptionText),
                safeText(entry.directoryPathText),
                safeText(entry.objectNameText),
                safeText(entry.objectTypeText),
                safeText(entry.fullPathText),
                safeText(entry.enumApiText),
                safeText(entry.symbolicLinkTargetText),
                safeText(entry.statusText));
    }

    // atomEntryAsTsv：
    // - 作用：把原子条目序列化为 TSV。
    QString atomEntryAsTsv(const KernelAtomEntry& entry)
    {
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(entry.atomValue), 4, 16, QChar('0'))
            .toUpper();

        return QStringLiteral("%1\t%2\t%3\t%4\t%5")
            .arg(QString::number(entry.atomValue), hexText, safeText(entry.atomNameText), safeText(entry.sourceText), safeText(entry.statusText));
    }

    // ObjectNamespaceColumn：对象命名空间表列索引。
    enum class ObjectNamespaceColumn : int
    {
        RootPath = 0,
        Scope,
        DirectoryPath,
        ObjectName,
        ObjectType,
        FullPath,
        EnumApi,
        SymbolicTarget,
        Status,
        Count
    };

    // AtomColumn：原子表列索引。
    enum class AtomColumn : int
    {
        Value = 0,
        Hex,
        Name,
        Source,
        Status,
        Count
    };
}

void KernelDock::showObjectNamespaceContextMenu(const QPoint& localPosition)
{
    if (m_objectNamespaceTable == nullptr)
    {
        return;
    }

    const QModelIndex clickedIndex = m_objectNamespaceTable->indexAt(localPosition);
    if (clickedIndex.isValid())
    {
        m_objectNamespaceTable->setCurrentCell(clickedIndex.row(), clickedIndex.column());
    }

    const KernelObjectNamespaceEntry* entry = currentObjectNamespaceEntry();
    const bool hasEntry = (entry != nullptr);

    QMenu contextMenu(this);
    contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());

    QAction* refreshAction = contextMenu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新对象命名空间"));
    contextMenu.addSeparator();

    QMenu* copyMenu = contextMenu.addMenu(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制"));
    QAction* copyCellAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制当前单元格"));
    QAction* copyObjectNameAction = copyMenu->addAction(QStringLiteral("复制对象名"));
    QAction* copyObjectTypeAction = copyMenu->addAction(QStringLiteral("复制对象类型"));
    QAction* copyFullPathAction = copyMenu->addAction(QStringLiteral("复制完整路径"));
    QAction* copySymbolicTargetAction = copyMenu->addAction(QStringLiteral("复制符号链接目标"));
    QAction* copyEnumApiAction = copyMenu->addAction(QStringLiteral("复制枚举 API"));
    QAction* copyRowAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制当前行"));
    QAction* copySameRootRowsAction = copyMenu->addAction(QStringLiteral("复制同目录路径全部行"));

    copyObjectNameAction->setEnabled(hasEntry);
    copyObjectTypeAction->setEnabled(hasEntry);
    copyFullPathAction->setEnabled(hasEntry);
    copySymbolicTargetAction->setEnabled(hasEntry && !entry->symbolicLinkTargetText.trimmed().isEmpty());
    copyEnumApiAction->setEnabled(hasEntry);
    copyRowAction->setEnabled(hasEntry);
    copySameRootRowsAction->setEnabled(hasEntry);

    QMenu* operationMenu = contextMenu.addMenu(QIcon(":/Icon/process_tree.svg"), QStringLiteral("对象操作"));
    QAction* filterByRootAction = operationMenu->addAction(QStringLiteral("用目录路径过滤"));
    QAction* filterByDirectoryAction = operationMenu->addAction(QStringLiteral("用当前目录过滤"));
    QAction* filterByObjectNameAction = operationMenu->addAction(QStringLiteral("用对象名过滤"));
    QAction* resolveSymbolicLinkAction = operationMenu->addAction(QStringLiteral("解析符号链接目标"));
    QAction* mapDosPathAction = operationMenu->addAction(QStringLiteral("尝试映射为 DOS 路径"));

    filterByRootAction->setEnabled(hasEntry);
    filterByDirectoryAction->setEnabled(hasEntry);
    filterByObjectNameAction->setEnabled(hasEntry && !entry->objectNameText.trimmed().isEmpty());
    resolveSymbolicLinkAction->setEnabled(hasEntry && entry->isSymbolicLink);
    mapDosPathAction->setEnabled(hasEntry && (isNtDevicePath(entry->fullPathText) || isNtDevicePath(entry->symbolicLinkTargetText)));

    QAction* selectedAction = contextMenu.exec(m_objectNamespaceTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }

    kLogEvent menuEvent;
    info << menuEvent
        << "[KernelDock] 对象命名空间右键动作: "
        << selectedAction->text().toStdString()
        << eol;

    if (selectedAction == refreshAction)
    {
        refreshObjectNamespaceAsync();
        return;
    }

    if (selectedAction == copyCellAction)
    {
        const int rowIndex = m_objectNamespaceTable->currentRow();
        const int columnIndex = m_objectNamespaceTable->currentColumn();
        if (rowIndex >= 0 && columnIndex >= 0)
        {
            QTableWidgetItem* cellItem = m_objectNamespaceTable->item(rowIndex, columnIndex);
            if (cellItem != nullptr)
            {
                copyTextToClipboard(cellItem->text());
            }
        }
        return;
    }

    if (!hasEntry)
    {
        return;
    }

    if (selectedAction == copyObjectNameAction)
    {
        copyTextToClipboard(entry->objectNameText);
        return;
    }
    if (selectedAction == copyObjectTypeAction)
    {
        copyTextToClipboard(entry->objectTypeText);
        return;
    }
    if (selectedAction == copyFullPathAction)
    {
        copyTextToClipboard(entry->fullPathText);
        return;
    }
    if (selectedAction == copySymbolicTargetAction)
    {
        copyTextToClipboard(entry->symbolicLinkTargetText);
        return;
    }
    if (selectedAction == copyEnumApiAction)
    {
        copyTextToClipboard(entry->enumApiText);
        return;
    }
    if (selectedAction == copyRowAction)
    {
        copyTextToClipboard(tableRowAsTsv(m_objectNamespaceTable, m_objectNamespaceTable->currentRow()));
        return;
    }
    if (selectedAction == copySameRootRowsAction)
    {
        QStringList rowList;
        for (const KernelObjectNamespaceEntry& rowEntry : m_objectNamespaceRows)
        {
            if (QString::compare(rowEntry.rootPathText, entry->rootPathText, Qt::CaseInsensitive) == 0)
            {
                rowList.push_back(objectNamespaceEntryAsTsv(rowEntry));
            }
        }
        copyTextToClipboard(rowList.join('\n'));
        return;
    }

    if (selectedAction == filterByRootAction)
    {
        m_objectNamespaceFilterEdit->setText(entry->rootPathText);
        return;
    }
    if (selectedAction == filterByDirectoryAction)
    {
        m_objectNamespaceFilterEdit->setText(entry->directoryPathText);
        return;
    }
    if (selectedAction == filterByObjectNameAction)
    {
        m_objectNamespaceFilterEdit->setText(entry->objectNameText);
        return;
    }
    if (selectedAction == resolveSymbolicLinkAction)
    {
        QString targetText;
        QString statusText;
        const bool resolveOk = queryObjectNamespaceSymbolicLinkTarget(entry->fullPathText, targetText, statusText);

        QString resultText = QStringLiteral(
            "符号链接路径: %1\n"
            "解析状态: %2\n"
            "目标路径: %3")
            .arg(entry->fullPathText)
            .arg(statusText)
            .arg(resolveOk ? targetText : QStringLiteral("<解析失败>"));

        std::size_t sourceIndex = 0;
        if (resolveOk && currentObjectNamespaceSourceIndex(sourceIndex))
        {
            m_objectNamespaceRows[sourceIndex].symbolicLinkTargetText = targetText;
            if (m_objectNamespaceTable->currentRow() >= 0)
            {
                QTableWidgetItem* targetItem = m_objectNamespaceTable->item(
                    m_objectNamespaceTable->currentRow(),
                    static_cast<int>(ObjectNamespaceColumn::SymbolicTarget));
                if (targetItem != nullptr)
                {
                    targetItem->setText(targetText);
                }
            }
        }

        m_objectNamespaceDetailEditor->setText(resultText);
        return;
    }
    if (selectedAction == mapDosPathAction)
    {
        QString sourcePathText;
        if (isNtDevicePath(entry->fullPathText))
        {
            sourcePathText = entry->fullPathText;
        }
        else
        {
            sourcePathText = entry->symbolicLinkTargetText;
        }

        const std::vector<QString> candidateList = queryDosPathCandidatesByNtPath(sourcePathText);
        if (candidateList.empty())
        {
            m_objectNamespaceDetailEditor->setText(
                QStringLiteral("路径: %1\n未找到可用 DOS 路径映射。")
                .arg(sourcePathText));
            return;
        }

        QStringList candidateTextList;
        for (const QString& candidateText : candidateList)
        {
            candidateTextList.push_back(candidateText);
        }

        const QString joinedText = candidateTextList.join('\n');
        copyTextToClipboard(joinedText);
        m_objectNamespaceDetailEditor->setText(
            QStringLiteral("路径: %1\n已找到 DOS 路径映射（并已复制）：\n%2")
            .arg(sourcePathText, joinedText));
        return;
    }
}

void KernelDock::showAtomContextMenu(const QPoint& localPosition)
{
    if (m_atomTable == nullptr)
    {
        return;
    }

    const QModelIndex clickedIndex = m_atomTable->indexAt(localPosition);
    if (clickedIndex.isValid())
    {
        m_atomTable->setCurrentCell(clickedIndex.row(), clickedIndex.column());
    }

    const KernelAtomEntry* entry = currentAtomEntry();
    const bool hasEntry = (entry != nullptr);

    QMenu contextMenu(this);
    contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());

    QAction* refreshAction = contextMenu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新原子表"));
    contextMenu.addSeparator();

    QMenu* copyMenu = contextMenu.addMenu(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制"));
    QAction* copyCellAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制当前单元格"));
    QAction* copyValueAction = copyMenu->addAction(QStringLiteral("复制Atom值"));
    QAction* copyHexAction = copyMenu->addAction(QStringLiteral("复制十六进制"));
    QAction* copyNameAction = copyMenu->addAction(QStringLiteral("复制名称"));
    QAction* copySourceAction = copyMenu->addAction(QStringLiteral("复制来源"));
    QAction* copyRowAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制当前行"));

    copyValueAction->setEnabled(hasEntry);
    copyHexAction->setEnabled(hasEntry);
    copyNameAction->setEnabled(hasEntry);
    copySourceAction->setEnabled(hasEntry);
    copyRowAction->setEnabled(hasEntry);

    QMenu* operationMenu = contextMenu.addMenu(QIcon(":/Icon/process_threads.svg"), QStringLiteral("原子操作"));
    QAction* filterByNameAction = operationMenu->addAction(QStringLiteral("用名称过滤"));
    QAction* verifyByNameAction = operationMenu->addAction(QStringLiteral("使用GlobalFindAtomW校验"));
    QAction* copySnippetAction = operationMenu->addAction(QStringLiteral("复制调用代码片段"));

    filterByNameAction->setEnabled(hasEntry && !entry->atomNameText.trimmed().isEmpty());
    verifyByNameAction->setEnabled(hasEntry && !entry->atomNameText.trimmed().isEmpty());
    copySnippetAction->setEnabled(hasEntry && !entry->atomNameText.trimmed().isEmpty());

    QAction* selectedAction = contextMenu.exec(m_atomTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }

    kLogEvent menuEvent;
    info << menuEvent
        << "[KernelDock] 原子表右键动作: "
        << selectedAction->text().toStdString()
        << eol;

    if (selectedAction == refreshAction)
    {
        refreshAtomTableAsync();
        return;
    }

    if (selectedAction == copyCellAction)
    {
        const int rowIndex = m_atomTable->currentRow();
        const int columnIndex = m_atomTable->currentColumn();
        if (rowIndex >= 0 && columnIndex >= 0)
        {
            QTableWidgetItem* cellItem = m_atomTable->item(rowIndex, columnIndex);
            if (cellItem != nullptr)
            {
                copyTextToClipboard(cellItem->text());
            }
        }
        return;
    }

    if (!hasEntry)
    {
        return;
    }

    if (selectedAction == copyValueAction)
    {
        copyTextToClipboard(QString::number(entry->atomValue));
        return;
    }
    if (selectedAction == copyHexAction)
    {
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(entry->atomValue), 4, 16, QChar('0'))
            .toUpper();
        copyTextToClipboard(hexText);
        return;
    }
    if (selectedAction == copyNameAction)
    {
        copyTextToClipboard(entry->atomNameText);
        return;
    }
    if (selectedAction == copySourceAction)
    {
        copyTextToClipboard(entry->sourceText);
        return;
    }
    if (selectedAction == copyRowAction)
    {
        copyTextToClipboard(atomEntryAsTsv(*entry));
        return;
    }

    if (selectedAction == filterByNameAction)
    {
        m_atomFilterEdit->setText(entry->atomNameText);
        return;
    }
    if (selectedAction == verifyByNameAction)
    {
        std::uint16_t foundAtomValue = 0;
        QString verifyDetailText;
        const bool verifyOk = verifyGlobalAtomByName(entry->atomNameText, foundAtomValue, verifyDetailText);

        if (verifyOk)
        {
            for (int rowIndex = 0; rowIndex < m_atomTable->rowCount(); ++rowIndex)
            {
                QTableWidgetItem* valueItem = m_atomTable->item(rowIndex, static_cast<int>(AtomColumn::Value));
                if (valueItem == nullptr)
                {
                    continue;
                }

                if (valueItem->text() == QString::number(foundAtomValue))
                {
                    m_atomTable->setCurrentCell(rowIndex, static_cast<int>(AtomColumn::Value));
                    break;
                }
            }
        }

        m_atomDetailEditor->setText(verifyDetailText);
        return;
    }
    if (selectedAction == copySnippetAction)
    {
        QString escapedNameText = entry->atomNameText;
        escapedNameText.replace('\\', QStringLiteral("\\\\"));
        escapedNameText.replace('"', QStringLiteral("\\\""));

        const QString snippetText = QStringLiteral("ATOM atomValue = GlobalFindAtomW(L\"%1\");")
            .arg(escapedNameText);

        copyTextToClipboard(snippetText);
        m_atomDetailEditor->setText(
            QStringLiteral("已复制调用代码片段：\n%1")
            .arg(snippetText));
        return;
    }
}
