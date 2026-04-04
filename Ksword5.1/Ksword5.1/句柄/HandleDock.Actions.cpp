#include "HandleDock.h"

// ============================================================
// HandleDock.Actions.cpp
// 作用：
// - 承载句柄模块的交互动作实现；
// - 包括对象类型详情展示、复制、关闭句柄、批量关闭句柄；
// - 与主 UI 文件拆开，控制单文件规模。
// ============================================================

#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace
{
    // boolText：
    // - 作用：把布尔值转成中文“是/否”；
    // - 本文件单独实现，避免依赖 UI cpp 内部匿名命名空间。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }
}

void HandleDock::showObjectTypeDetailByCurrentRow()
{
    m_objectTypeDetailTable->clear();
    if (m_objectTypeTable == nullptr || m_objectTypeTable->currentItem() == nullptr)
    {
        return;
    }

    const QVariant rowIndexValue =
        m_objectTypeTable->currentItem()->data(static_cast<int>(ObjectTypeTableColumn::TypeIndex), Qt::UserRole);
    if (!rowIndexValue.isValid())
    {
        return;
    }
    const std::size_t rowIndex = static_cast<std::size_t>(rowIndexValue.toULongLong());
    if (rowIndex >= m_objectTypeRows.size())
    {
        return;
    }

    const HandleObjectTypeEntry& row = m_objectTypeRows[rowIndex];
    auto addDetailRow = [this](const QString& keyText, const QString& valueText)
        {
            auto* detailItem = new QTreeWidgetItem();
            detailItem->setText(0, keyText);
            detailItem->setText(1, valueText);
            m_objectTypeDetailTable->addTopLevelItem(detailItem);
        };

    addDetailRow(QStringLiteral("类型编号"), QString::number(row.typeIndex));
    addDetailRow(QStringLiteral("类型名称"), row.typeNameText);
    addDetailRow(QStringLiteral("对象总数"), QString::number(row.totalObjectCount));
    addDetailRow(QStringLiteral("句柄总数"), QString::number(row.totalHandleCount));
    addDetailRow(QStringLiteral("访问掩码"), formatHex(row.validAccessMask, 0));
    addDetailRow(QStringLiteral("安全要求"), boolText(row.securityRequired));
    addDetailRow(QStringLiteral("维护句柄计数"), boolText(row.maintainHandleCount));
    addDetailRow(QStringLiteral("池类型"), QString::number(row.poolType));
    addDetailRow(QStringLiteral("默认分页池配额"), QString::number(row.defaultPagedPoolCharge));
    addDetailRow(QStringLiteral("默认非分页池配额"), QString::number(row.defaultNonPagedPoolCharge));
}

HandleDock::HandleRow* HandleDock::selectedHandleRow()
{
    QTreeWidgetItem* currentItem = m_tableWidget->currentItem();
    if (currentItem == nullptr)
    {
        return nullptr;
    }
    const QVariant rowIndexValue = currentItem->data(static_cast<int>(HandleTableColumn::ProcessId), Qt::UserRole);
    if (!rowIndexValue.isValid())
    {
        return nullptr;
    }
    const std::size_t rowIndex = static_cast<std::size_t>(rowIndexValue.toULongLong());
    if (rowIndex >= m_rows.size())
    {
        return nullptr;
    }
    return &m_rows[rowIndex];
}

void HandleDock::copyCurrentHandleCell()
{
    QTreeWidgetItem* currentItem = m_tableWidget->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }
    const int columnIndex = m_tableWidget->currentColumn();
    if (columnIndex < 0)
    {
        return;
    }
    QApplication::clipboard()->setText(currentItem->text(columnIndex));
}

void HandleDock::copyCurrentHandleRow()
{
    QTreeWidgetItem* currentItem = m_tableWidget->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }
    QStringList textList;
    for (int columnIndex = 0; columnIndex < static_cast<int>(HandleTableColumn::Count); ++columnIndex)
    {
        textList.push_back(currentItem->text(columnIndex));
    }
    QApplication::clipboard()->setText(textList.join('\t'));
}

void HandleDock::closeCurrentHandle()
{
    HandleRow* row = selectedHandleRow();
    if (row == nullptr)
    {
        return;
    }

    const QString confirmText = QStringLiteral(
        "确认关闭目标句柄？\nPID=%1\nHandle=%2\nTypeIndex=%3\n类型=%4\n对象名=%5")
        .arg(row->processId)
        .arg(formatHex(row->handleValue, 0))
        .arg(row->typeIndex)
        .arg(row->typeName)
        .arg(row->objectName.trimmed().isEmpty() ? QStringLiteral("-") : row->objectName);
    if (QMessageBox::question(this, QStringLiteral("关闭句柄"), confirmText) != QMessageBox::Yes)
    {
        return;
    }

    std::string detailText;
    const bool closeOk = closeRemoteHandle(row->processId, row->handleValue, detailText);
    kLogEvent closeEvent;
    (closeOk ? info : err) << closeEvent
        << "[HandleDock] closeCurrentHandle: pid="
        << row->processId
        << ", handle="
        << formatHex(row->handleValue, 0).toStdString()
        << ", typeIndex="
        << row->typeIndex
        << ", ok="
        << (closeOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;

    if (closeOk)
    {
        QMessageBox::information(this, QStringLiteral("关闭句柄"), QStringLiteral("句柄关闭成功。\n%1").arg(QString::fromStdString(detailText)));
        requestAsyncRefresh(true);
        return;
    }
    QMessageBox::warning(this, QStringLiteral("关闭句柄"), QStringLiteral("句柄关闭失败。\n%1").arg(QString::fromStdString(detailText)));
}

void HandleDock::closeSameTypeHandlesInCurrentProcess()
{
    HandleRow* selectedRow = selectedHandleRow();
    if (selectedRow == nullptr)
    {
        return;
    }

    std::vector<std::uint64_t> targetHandles;
    targetHandles.reserve(128);
    for (const HandleRow& row : m_rows)
    {
        if (row.processId == selectedRow->processId && row.typeIndex == selectedRow->typeIndex)
        {
            targetHandles.push_back(row.handleValue);
        }
    }
    if (targetHandles.empty())
    {
        return;
    }

    const QString confirmText = QStringLiteral(
        "确认批量关闭同类型句柄？\nPID=%1\nTypeIndex=%2\n类型=%3\n目标数量=%4")
        .arg(selectedRow->processId)
        .arg(selectedRow->typeIndex)
        .arg(selectedRow->typeName)
        .arg(targetHandles.size());
    if (QMessageBox::question(this, QStringLiteral("批量关闭句柄"), confirmText) != QMessageBox::Yes)
    {
        return;
    }

    std::size_t successCount = 0;
    std::size_t failCount = 0;
    std::string lastErrorText;
    for (const std::uint64_t handleValue : targetHandles)
    {
        std::string detailText;
        const bool closeOk = closeRemoteHandle(selectedRow->processId, handleValue, detailText);
        if (closeOk)
        {
            ++successCount;
        }
        else
        {
            ++failCount;
            lastErrorText = detailText;
        }
    }

    kLogEvent batchCloseEvent;
    info << batchCloseEvent
        << "[HandleDock] closeSameTypeHandlesInCurrentProcess: pid="
        << selectedRow->processId
        << ", typeIndex="
        << selectedRow->typeIndex
        << ", total="
        << targetHandles.size()
        << ", success="
        << successCount
        << ", fail="
        << failCount
        << ", lastError="
        << lastErrorText
        << eol;

    QMessageBox::information(
        this,
        QStringLiteral("批量关闭句柄"),
        QStringLiteral("执行完成。\n成功: %1\n失败: %2\n最后错误: %3")
        .arg(successCount)
        .arg(failCount)
        .arg(QString::fromStdString(lastErrorText)));

    requestAsyncRefresh(true);
}

