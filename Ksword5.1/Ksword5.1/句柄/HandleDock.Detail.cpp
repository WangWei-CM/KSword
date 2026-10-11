#include "HandleDock.h"
#include "../UI/TableInteractionSupport.h"
#include "../ksword/log/log.h"
#include "../theme.h"
#include "../UI/StructuredFieldView.h"

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

#include <memory>

// 析构先关闭详情投递门禁，再让派生成员和子控件开始销毁。
HandleDock::~HandleDock()
{
    if (m_handleDetailOperation)
    {
        m_handleDetailOperation->close();
    }
}

void HandleDock::requestHandleDetailRefresh(const bool forceRefresh)
{
    (void)forceRefresh; // 选择变化与手动请求都保留最新行快照，最多一个待执行请求。
    if (!m_handleDetailOperation)
    {
        m_handleDetailOperation = std::make_unique<ks::ui::AsyncOperation>(this);
        m_handleDetailOperation->setStateChangedCallback([this]()
        {
            m_handleDetailRefreshInProgress = m_handleDetailOperation->isBusy();
            if (m_handleDetailRefreshInProgress && m_handleDetailRefreshProgressPid > 0)
            {
                kPro.set(m_handleDetailRefreshProgressPid, "后台查询句柄详情", 0, 20.0f);
            }
        });
    }
    const HandleRow* const row = selectedHandleRow(); // 只在 UI 线程取得当前行。
    if (row == nullptr)
    {
        m_handleDetailOperation->cancel();
        m_handleDetailRefreshTicket = m_handleDetailOperation->generation();
        showHandleDetailPlaceholder(QStringLiteral("请选择一个句柄查看详情。"));
        return;
    }
    const HandleRow rowSnapshot = *row; // 后台查询和延期重放共用不可变身份快照。
    if (m_handleDetailStatusLabel != nullptr)
    {
        m_handleDetailStatusLabel->setText(QStringLiteral("● 正在刷新句柄详情..."));
    }
    if (m_handleDetailRefreshProgressPid <= 0)
    {
        m_handleDetailRefreshProgressPid = kPro.addReusable(this, "句柄详情", "准备读取句柄详情");
    }
    const int progressPid = m_handleDetailRefreshProgressPid; // 收尾不得借用页面成员。
    m_handleDetailRefreshTicket = m_handleDetailOperation->submit<HandleRow, HandleDetailRefreshResult>(
        rowSnapshot,
        [](const HandleRow& snapshot, const ks::ui::AsyncOperationToken& token)
        {
            if (token.isCanceled())
            {
                return HandleDetailRefreshResult{};
            }
            return buildHandleDetailRefreshResult(snapshot);
        },
        [this, rowSnapshot](const HandleDetailRefreshResult& result, const std::uint64_t ticket)
        {
            return tryApplyHandleDetailRefreshResult(ticket, rowSnapshot, result);
        },
        [this](std::exception_ptr)
        {
            if (m_handleDetailStatusLabel != nullptr)
            {
                m_handleDetailStatusLabel->setText(QStringLiteral("● 句柄详情刷新失败。"));
            }
        },
        [progressPid](const ks::ui::AsyncOperationOutcome outcome)
        {
            kPro.set(progressPid, outcome == ks::ui::AsyncOperationOutcome::Applied
                ? "句柄详情刷新完成" : "句柄详情查询已结束", 0, 100.0f);
        });
}

bool HandleDock::tryApplyHandleDetailRefreshResult(const std::uint64_t refreshTicket,
    const HandleRow& expectedRow, const HandleDetailRefreshResult& refreshResult)
{
    // 代次和当前选择身份都必须匹配；对象地址和进程创建时间避免复用旧句柄行。
    const HandleRow* const selected = selectedHandleRow(); // 仅主线程读取当前缓存。
    if (!m_handleDetailOperation || !m_handleDetailOperation->isCurrent(refreshTicket)
        || refreshTicket != m_handleDetailRefreshTicket || selected == nullptr
        || selected->processId != expectedRow.processId
        || selected->processCreationTime != expectedRow.processCreationTime
        || selected->handleValue != expectedRow.handleValue
        || selected->objectAddress != expectedRow.objectAddress
        || selected->typeIndex != expectedRow.typeIndex)
    {
        return true;
    }
    // 详情已是稳定字段文档；旧隐藏树不存在，不再为它执行无意义的菜单延期与重建。
    applyHandleDetailRefreshResult(refreshTicket, refreshResult);
    return true;
}

void HandleDock::applyHandleDetailRefreshResult(
    const std::uint64_t refreshTicket,
    const HandleDetailRefreshResult& refreshResult)
{
    if (refreshTicket < m_handleDetailRefreshTicket)
    {
        return;
    }

    if (m_handleDetailFields == nullptr)
    {
        return;
    }
    // 唯一文档直接来自同一异步结果，投影原位更新真实变化字段。
    ks::ui::FieldDocument document;
    document.section(QStringLiteral("Object Header"));
    for (const HandleDetailField& field : refreshResult.fields)
    {
        document.field(field.keyText, field.valueText);
    }
    m_handleDetailFields->setDocument(document);

    QString statusText = QStringLiteral("● 详情刷新完成 %1 ms").arg(refreshResult.elapsedMs);
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QStringLiteral(" | 存在诊断；详情已写入日志。");
        kLogEvent diagnosticEvent;
        warn << diagnosticEvent
            << "[HandleDock] detail refresh completed with diagnostics, fieldCount="
            << refreshResult.fields.size()
            << ", detail=" << refreshResult.diagnosticText.toStdString()
            << eol;
    }
    if (m_handleDetailStatusLabel != nullptr)
    {
        m_handleDetailStatusLabel->setText(statusText);
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
    // 状态提示已有单独标签，选择清空时只清空规范文档，不制造占位字段。
    if (m_handleDetailFields != nullptr)
    {
        m_handleDetailFields->setDocument(ks::ui::FieldDocument{});
    }
}
