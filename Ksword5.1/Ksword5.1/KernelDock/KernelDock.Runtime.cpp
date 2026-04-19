
#include "KernelDock.h"

// ============================================================
// KernelDock.Runtime.cpp
// 作用说明：
// 1) 承载 KernelDock 的异步刷新流程；
// 2) 承载三个表格的重建逻辑；
// 3) 承载详情联动和当前选中项解析。
// ============================================================

#include "KernelDockAtomWorker.h"
#include "KernelDockObjectNamespaceWorker.h"
#include "KernelDockQueryWorker.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QBrush>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <algorithm> // std::count_if：统计失败项。
#include <thread>    // std::thread：后台刷新任务。

namespace
{
    // statusLabelStyle：
    // - 作用：统一状态标签颜色与字重。
    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    // safeText：
    // - 作用：把空文本替换为占位符，避免详情区出现空白字段。
    QString safeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
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

    // NtQueryColumn：历史 NtQuery 表列索引。
    enum class NtQueryColumn : int
    {
        Category = 0,
        Function,
        QueryItem,
        Status,
        Summary,
        Count
    };
}

void KernelDock::refreshObjectNamespaceAsync()
{
    if (m_objectNamespaceRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] 对象命名空间刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshObjectNamespaceButton->setEnabled(false);
    m_objectNamespaceStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_objectNamespaceStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelObjectNamespaceEntry> resultRows;
        QString errorText;
        const bool success = runObjectNamespaceSnapshotTask(resultRows, errorText);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_objectNamespaceRefreshRunning.store(false);
            guardThis->m_refreshObjectNamespaceButton->setEnabled(true);

            if (!success)
            {
                guardThis->m_objectNamespaceStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_objectNamespaceStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_objectNamespaceDetailEditor->setText(errorText);

                kLogEvent failEvent;
                err << failEvent
                    << "[KernelDock] 对象命名空间刷新失败: "
                    << errorText.toStdString()
                    << eol;
                return;
            }

            guardThis->m_objectNamespaceRows = std::move(resultRows);
            guardThis->rebuildObjectNamespaceTable(guardThis->m_objectNamespaceFilterEdit->text().trimmed());

            const std::size_t failedCount = static_cast<std::size_t>(
                std::count_if(
                    guardThis->m_objectNamespaceRows.begin(),
                    guardThis->m_objectNamespaceRows.end(),
                    [](const KernelObjectNamespaceEntry& entry) {
                        return !entry.querySucceeded;
                    }));

            guardThis->m_objectNamespaceStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项，异常 %2 项")
                .arg(guardThis->m_objectNamespaceRows.size())
                .arg(failedCount));
            guardThis->m_objectNamespaceStatusLabel->setStyleSheet(
                statusLabelStyle(failedCount == 0 ? QStringLiteral("#3A8F3A") : QStringLiteral("#D77A00")));

            if (guardThis->m_objectNamespaceTable->rowCount() > 0)
            {
                guardThis->m_objectNamespaceTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_objectNamespaceDetailEditor->setText(QStringLiteral("当前筛选条件下无可见对象记录。"));
            }

            kLogEvent doneEvent;
            info << doneEvent
                << "[KernelDock] 对象命名空间刷新完成, total="
                << guardThis->m_objectNamespaceRows.size()
                << ", failed="
                << failedCount
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::refreshAtomTableAsync()
{
    if (m_atomRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] 原子表刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshAtomButton->setEnabled(false);
    m_atomStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_atomStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelAtomEntry> resultRows;
        QString errorText;
        const bool success = runAtomTableSnapshotTask(resultRows, errorText);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_atomRefreshRunning.store(false);
            guardThis->m_refreshAtomButton->setEnabled(true);

            if (!success)
            {
                guardThis->m_atomStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_atomStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_atomDetailEditor->setText(errorText);

                kLogEvent failEvent;
                err << failEvent
                    << "[KernelDock] 原子表刷新失败: "
                    << errorText.toStdString()
                    << eol;
                return;
            }

            guardThis->m_atomRows = std::move(resultRows);
            guardThis->rebuildAtomTable(guardThis->m_atomFilterEdit->text().trimmed());
            guardThis->m_atomStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项")
                .arg(guardThis->m_atomRows.size()));
            guardThis->m_atomStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#3A8F3A")));

            if (guardThis->m_atomTable->rowCount() > 0)
            {
                guardThis->m_atomTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_atomDetailEditor->setText(QStringLiteral("当前环境未发现可见原子记录。"));
            }

            kLogEvent doneEvent;
            info << doneEvent
                << "[KernelDock] 原子表刷新完成, count="
                << guardThis->m_atomRows.size()
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::refreshNtQueryAsync()
{
    if (m_ntQueryRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] NtQuery 刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshNtQueryButton->setEnabled(false);
    m_ntQueryStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_ntQueryStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelNtQueryResultEntry> resultRows;
        QString errorText;
        const bool success = runNtQuerySnapshotTask(resultRows, errorText);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_ntQueryRefreshRunning.store(false);
            guardThis->m_refreshNtQueryButton->setEnabled(true);

            if (!success)
            {
                guardThis->m_ntQueryStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_ntQueryStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_ntQueryDetailEditor->setText(errorText);

                kLogEvent failEvent;
                err << failEvent
                    << "[KernelDock] NtQuery 刷新失败: "
                    << errorText.toStdString()
                    << eol;
                return;
            }

            guardThis->m_ntQueryResults = std::move(resultRows);
            guardThis->rebuildNtQueryTable();

            int successCount = 0;
            for (const KernelNtQueryResultEntry& entry : guardThis->m_ntQueryResults)
            {
                if (entry.statusCode >= 0)
                {
                    ++successCount;
                }
            }

            guardThis->m_ntQueryStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项，成功 %2 项")
                .arg(guardThis->m_ntQueryResults.size())
                .arg(successCount));
            guardThis->m_ntQueryStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#3A8F3A")));

            if (guardThis->m_ntQueryTable->rowCount() > 0)
            {
                guardThis->m_ntQueryTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_ntQueryDetailEditor->setText(QStringLiteral("无可展示的 NtQuery 结果。"));
            }

            kLogEvent doneEvent;
            info << doneEvent
                << "[KernelDock] NtQuery 刷新完成, total="
                << guardThis->m_ntQueryResults.size()
                << ", success="
                << successCount
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildObjectNamespaceTable(const QString& filterKeyword)
{
    if (m_objectNamespaceTable == nullptr)
    {
        return;
    }

    m_objectNamespaceTable->setSortingEnabled(false);
    m_objectNamespaceTable->setRowCount(0);

    for (std::size_t sourceIndex = 0; sourceIndex < m_objectNamespaceRows.size(); ++sourceIndex)
    {
        const KernelObjectNamespaceEntry& entry = m_objectNamespaceRows[sourceIndex];
        const bool matched = filterKeyword.isEmpty()
            || entry.rootPathText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.scopeDescriptionText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.directoryPathText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.objectNameText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.objectTypeText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.fullPathText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.enumApiText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.symbolicLinkTargetText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.statusText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_objectNamespaceTable->rowCount();
        m_objectNamespaceTable->insertRow(rowIndex);

        auto* rootItem = new QTableWidgetItem(entry.rootPathText);
        rootItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* scopeItem = new QTableWidgetItem(entry.scopeDescriptionText);
        auto* directoryItem = new QTableWidgetItem(entry.directoryPathText);
        auto* objectNameItem = new QTableWidgetItem(entry.objectNameText);
        auto* objectTypeItem = new QTableWidgetItem(entry.objectTypeText);
        auto* fullPathItem = new QTableWidgetItem(entry.fullPathText);
        auto* enumApiItem = new QTableWidgetItem(entry.enumApiText);
        auto* symbolicTargetItem = new QTableWidgetItem(entry.symbolicLinkTargetText);
        auto* statusItem = new QTableWidgetItem(entry.statusText);

        rootItem->setFlags(rootItem->flags() & ~Qt::ItemIsEditable);
        scopeItem->setFlags(scopeItem->flags() & ~Qt::ItemIsEditable);
        directoryItem->setFlags(directoryItem->flags() & ~Qt::ItemIsEditable);
        objectNameItem->setFlags(objectNameItem->flags() & ~Qt::ItemIsEditable);
        objectTypeItem->setFlags(objectTypeItem->flags() & ~Qt::ItemIsEditable);
        fullPathItem->setFlags(fullPathItem->flags() & ~Qt::ItemIsEditable);
        enumApiItem->setFlags(enumApiItem->flags() & ~Qt::ItemIsEditable);
        symbolicTargetItem->setFlags(symbolicTargetItem->flags() & ~Qt::ItemIsEditable);
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);

        if (!entry.querySucceeded)
        {
            statusItem->setForeground(QBrush(KswordTheme::WarningAccentColor()));
        }
        else if (entry.isDirectory)
        {
            objectTypeItem->setForeground(QBrush(KswordTheme::PrimaryBlueColor));
        }

        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::RootPath), rootItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::Scope), scopeItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::DirectoryPath), directoryItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::ObjectName), objectNameItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::ObjectType), objectTypeItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::FullPath), fullPathItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::EnumApi), enumApiItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::SymbolicTarget), symbolicTargetItem);
        m_objectNamespaceTable->setItem(rowIndex, static_cast<int>(ObjectNamespaceColumn::Status), statusItem);
    }

    m_objectNamespaceTable->setSortingEnabled(true);
}

void KernelDock::rebuildAtomTable(const QString& filterKeyword)
{
    if (m_atomTable == nullptr)
    {
        return;
    }

    m_atomTable->setSortingEnabled(false);
    m_atomTable->setRowCount(0);

    for (std::size_t sourceIndex = 0; sourceIndex < m_atomRows.size(); ++sourceIndex)
    {
        const KernelAtomEntry& entry = m_atomRows[sourceIndex];
        const QString valueText = QString::number(entry.atomValue);
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(entry.atomValue), 4, 16, QChar('0'))
            .toUpper();

        const bool matched = filterKeyword.isEmpty()
            || valueText.contains(filterKeyword, Qt::CaseInsensitive)
            || hexText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.atomNameText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.sourceText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.statusText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_atomTable->rowCount();
        m_atomTable->insertRow(rowIndex);

        auto* valueItem = new QTableWidgetItem(valueText);
        valueItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* hexItem = new QTableWidgetItem(hexText);
        auto* nameItem = new QTableWidgetItem(entry.atomNameText);
        auto* sourceItem = new QTableWidgetItem(entry.sourceText);
        auto* statusItem = new QTableWidgetItem(entry.statusText);

        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        hexItem->setFlags(hexItem->flags() & ~Qt::ItemIsEditable);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        sourceItem->setFlags(sourceItem->flags() & ~Qt::ItemIsEditable);
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);

        if (!entry.querySucceeded)
        {
            statusItem->setForeground(QBrush(KswordTheme::WarningAccentColor()));
        }

        m_atomTable->setItem(rowIndex, static_cast<int>(AtomColumn::Value), valueItem);
        m_atomTable->setItem(rowIndex, static_cast<int>(AtomColumn::Hex), hexItem);
        m_atomTable->setItem(rowIndex, static_cast<int>(AtomColumn::Name), nameItem);
        m_atomTable->setItem(rowIndex, static_cast<int>(AtomColumn::Source), sourceItem);
        m_atomTable->setItem(rowIndex, static_cast<int>(AtomColumn::Status), statusItem);
    }

    m_atomTable->setSortingEnabled(true);
}

void KernelDock::rebuildNtQueryTable()
{
    if (m_ntQueryTable == nullptr)
    {
        return;
    }

    m_ntQueryTable->setSortingEnabled(false);
    m_ntQueryTable->setRowCount(0);

    for (std::size_t sourceIndex = 0; sourceIndex < m_ntQueryResults.size(); ++sourceIndex)
    {
        const KernelNtQueryResultEntry& entry = m_ntQueryResults[sourceIndex];
        const int rowIndex = m_ntQueryTable->rowCount();
        m_ntQueryTable->insertRow(rowIndex);

        auto* categoryItem = new QTableWidgetItem(entry.categoryText);
        categoryItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* functionItem = new QTableWidgetItem(entry.functionNameText);
        auto* queryItem = new QTableWidgetItem(entry.queryItemText);
        auto* statusItem = new QTableWidgetItem(entry.statusText);
        auto* summaryItem = new QTableWidgetItem(entry.summaryText);

        categoryItem->setFlags(categoryItem->flags() & ~Qt::ItemIsEditable);
        functionItem->setFlags(functionItem->flags() & ~Qt::ItemIsEditable);
        queryItem->setFlags(queryItem->flags() & ~Qt::ItemIsEditable);
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        summaryItem->setFlags(summaryItem->flags() & ~Qt::ItemIsEditable);

        if (entry.statusCode < 0)
        {
            statusItem->setForeground(QBrush(KswordTheme::WarningAccentColor()));
        }

        m_ntQueryTable->setItem(rowIndex, static_cast<int>(NtQueryColumn::Category), categoryItem);
        m_ntQueryTable->setItem(rowIndex, static_cast<int>(NtQueryColumn::Function), functionItem);
        m_ntQueryTable->setItem(rowIndex, static_cast<int>(NtQueryColumn::QueryItem), queryItem);
        m_ntQueryTable->setItem(rowIndex, static_cast<int>(NtQueryColumn::Status), statusItem);
        m_ntQueryTable->setItem(rowIndex, static_cast<int>(NtQueryColumn::Summary), summaryItem);
    }

    m_ntQueryTable->setSortingEnabled(true);
}

bool KernelDock::currentObjectNamespaceSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0;

    if (m_objectNamespaceTable == nullptr)
    {
        return false;
    }

    const int currentRow = m_objectNamespaceTable->currentRow();
    if (currentRow < 0)
    {
        return false;
    }

    QTableWidgetItem* rootItem = m_objectNamespaceTable->item(currentRow, static_cast<int>(ObjectNamespaceColumn::RootPath));
    if (rootItem == nullptr)
    {
        return false;
    }

    sourceIndexOut = static_cast<std::size_t>(rootItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_objectNamespaceRows.size();
}

bool KernelDock::currentAtomSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0;

    if (m_atomTable == nullptr)
    {
        return false;
    }

    const int currentRow = m_atomTable->currentRow();
    if (currentRow < 0)
    {
        return false;
    }

    QTableWidgetItem* valueItem = m_atomTable->item(currentRow, static_cast<int>(AtomColumn::Value));
    if (valueItem == nullptr)
    {
        return false;
    }

    sourceIndexOut = static_cast<std::size_t>(valueItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_atomRows.size();
}

const KernelObjectNamespaceEntry* KernelDock::currentObjectNamespaceEntry() const
{
    std::size_t sourceIndex = 0;
    if (!currentObjectNamespaceSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_objectNamespaceRows[sourceIndex];
}

const KernelAtomEntry* KernelDock::currentAtomEntry() const
{
    std::size_t sourceIndex = 0;
    if (!currentAtomSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_atomRows[sourceIndex];
}

void KernelDock::showObjectNamespaceDetailByCurrentRow()
{
    if (m_objectNamespaceDetailEditor == nullptr)
    {
        return;
    }

    const KernelObjectNamespaceEntry* entry = currentObjectNamespaceEntry();
    if (entry == nullptr)
    {
        m_objectNamespaceDetailEditor->setText(QStringLiteral("请选择一条对象命名空间记录查看详情。"));
        return;
    }

    const QString detailText = QStringLiteral(
        "目录路径: %1\n"
        "作用说明: %2\n"
        "当前目录: %3\n"
        "对象名: %4\n"
        "对象类型: %5\n"
        "完整路径: %6\n"
        "枚举 API: %7\n"
        "符号链接目标: %8\n"
        "状态: %9\n"
        "是否目录: %10\n"
        "是否符号链接: %11\n\n"
        "Worker详情:\n%12")
        .arg(
            safeText(entry->rootPathText),
            safeText(entry->scopeDescriptionText),
            safeText(entry->directoryPathText),
            safeText(entry->objectNameText),
            safeText(entry->objectTypeText),
            safeText(entry->fullPathText),
            safeText(entry->enumApiText),
            safeText(entry->symbolicLinkTargetText),
            safeText(entry->statusText),
            entry->isDirectory ? QStringLiteral("是") : QStringLiteral("否"),
            entry->isSymbolicLink ? QStringLiteral("是") : QStringLiteral("否"),
            safeText(entry->detailText));

    m_objectNamespaceDetailEditor->setText(detailText);
}

void KernelDock::showAtomDetailByCurrentRow()
{
    if (m_atomDetailEditor == nullptr)
    {
        return;
    }

    const KernelAtomEntry* entry = currentAtomEntry();
    if (entry == nullptr)
    {
        m_atomDetailEditor->setText(QStringLiteral("请选择一条原子记录查看详情。"));
        return;
    }

    const QString detailText = QStringLiteral(
        "Atom值: %1\n"
        "十六进制: 0x%2\n"
        "名称: %3\n"
        "来源: %4\n"
        "状态: %5\n\n"
        "Worker详情:\n%6")
        .arg(entry->atomValue)
        .arg(static_cast<unsigned int>(entry->atomValue), 4, 16, QChar('0'))
        .arg(safeText(entry->atomNameText))
        .arg(safeText(entry->sourceText))
        .arg(safeText(entry->statusText))
        .arg(safeText(entry->detailText));

    m_atomDetailEditor->setText(detailText);
}

void KernelDock::showNtQueryDetailByCurrentRow()
{
    if (m_ntQueryTable == nullptr || m_ntQueryDetailEditor == nullptr)
    {
        return;
    }

    const int currentRow = m_ntQueryTable->currentRow();
    if (currentRow < 0)
    {
        m_ntQueryDetailEditor->setText(QStringLiteral("请选择一条 NtQuery 结果查看详情。"));
        return;
    }

    QTableWidgetItem* categoryItem = m_ntQueryTable->item(currentRow, static_cast<int>(NtQueryColumn::Category));
    if (categoryItem == nullptr)
    {
        m_ntQueryDetailEditor->setText(QStringLiteral("当前行无有效数据。"));
        return;
    }

    const std::size_t sourceIndex = static_cast<std::size_t>(categoryItem->data(Qt::UserRole).toULongLong());
    if (sourceIndex >= m_ntQueryResults.size())
    {
        m_ntQueryDetailEditor->setText(QStringLiteral("索引越界。"));
        return;
    }

    const KernelNtQueryResultEntry& entry = m_ntQueryResults[sourceIndex];
    const QString detailText = QStringLiteral(
        "类别: %1\n"
        "函数: %2\n"
        "查询项: %3\n"
        "状态: %4\n"
        "摘要: %5\n\n"
        "详细输出:\n%6")
        .arg(entry.categoryText)
        .arg(entry.functionNameText)
        .arg(entry.queryItemText)
        .arg(entry.statusText)
        .arg(entry.summaryText)
        .arg(entry.detailText);

    m_ntQueryDetailEditor->setText(detailText);
}
