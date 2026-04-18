#include "HandleDock.h"
#include "../theme.h"

// ============================================================
// HandleDock.Detail.cpp
// 作用：
// - 承载句柄详情异步刷新与详情表回填逻辑；
// - 承载句柄表头的列管理菜单；
// - 与主 UI 文件拆开，控制单文件规模。
// ============================================================

#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>

void HandleDock::requestHandleDetailRefresh(const bool forceRefresh)
{
    if (m_handleDetailRefreshInProgress)
    {
        if (forceRefresh)
        {
            m_handleDetailRefreshPending = true;
        }
        return;
    }

    HandleRow* row = selectedHandleRow();
    if (row == nullptr)
    {
        showHandleDetailPlaceholder(QStringLiteral("请选择一个句柄查看详情。"));
        return;
    }

    const HandleRow rowSnapshot = *row;
    const std::uint64_t currentTicket = ++m_handleDetailRefreshTicket;
    m_handleDetailRefreshInProgress = true;
    if (m_handleDetailStatusLabel != nullptr)
    {
        m_handleDetailStatusLabel->setText(QStringLiteral("● 正在刷新句柄详情..."));
    }

    if (m_handleDetailRefreshProgressPid <= 0)
    {
        m_handleDetailRefreshProgressPid = kPro.add("句柄详情", "准备读取句柄详情");
    }
    kPro.set(m_handleDetailRefreshProgressPid, "后台查询句柄详情", 0, 20.0f);

    QPointer<HandleDock> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, currentTicket, rowSnapshot]()
        {
            const HandleDetailRefreshResult refreshResult =
                buildHandleDetailRefreshResult(rowSnapshot);
            if (guardThis == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, currentTicket, refreshResult]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->applyHandleDetailRefreshResult(currentTicket, refreshResult);
                },
                Qt::QueuedConnection);
        });
    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void HandleDock::applyHandleDetailRefreshResult(
    const std::uint64_t refreshTicket,
    const HandleDetailRefreshResult& refreshResult)
{
    if (refreshTicket < m_handleDetailRefreshTicket)
    {
        return;
    }

    m_handleDetailRefreshInProgress = false;
    kPro.set(m_handleDetailRefreshProgressPid, "句柄详情刷新完成", 0, 100.0f);

    if (m_handleDetailTable == nullptr)
    {
        return;
    }
    m_handleDetailTable->clear();

    for (const HandleDetailField& field : refreshResult.fields)
    {
        auto* item = new QTreeWidgetItem();
        item->setText(0, field.keyText);
        item->setText(1, field.valueText);
        m_handleDetailTable->addTopLevelItem(item);
    }

    QString statusText = QStringLiteral("● 详情刷新完成 %1 ms").arg(refreshResult.elapsedMs);
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QStringLiteral(" | %1").arg(refreshResult.diagnosticText);
    }
    if (m_handleDetailStatusLabel != nullptr)
    {
        m_handleDetailStatusLabel->setText(statusText);
    }

    if (m_handleDetailRefreshPending)
    {
        m_handleDetailRefreshPending = false;
        QMetaObject::invokeMethod(this, [this]()
            {
                requestHandleDetailRefresh(true);
            }, Qt::QueuedConnection);
    }
}

void HandleDock::showHandleHeaderContextMenu(const QPoint& localPosition)
{
    if (m_tableWidget == nullptr || m_tableWidget->header() == nullptr)
    {
        return;
    }

    QHeaderView* header = m_tableWidget->header();
    QMenu menu(this);
    // 显式填充菜单背景，避免浅色模式下继承透明样式出现黑底。
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    for (int columnIndex = 0; columnIndex < static_cast<int>(HandleTableColumn::Count); ++columnIndex)
    {
        const QString columnTitle = m_tableWidget->headerItem()->text(columnIndex);
        QAction* columnAction = menu.addAction(columnTitle);
        columnAction->setCheckable(true);
        columnAction->setChecked(!header->isSectionHidden(columnIndex));
        connect(columnAction, &QAction::toggled, this, [header, columnIndex](const bool checked)
            {
                header->setSectionHidden(columnIndex, !checked);
            });
    }
    menu.exec(header->viewport()->mapToGlobal(localPosition));
}

void HandleDock::showHandleDetailPlaceholder(const QString& messageText)
{
    if (m_handleDetailStatusLabel != nullptr)
    {
        m_handleDetailStatusLabel->setText(messageText);
    }
    if (m_handleDetailTable == nullptr)
    {
        return;
    }
    m_handleDetailTable->clear();
}
