#include "KernelCommunicationEndpointTab.h"
#include "../UI/TableContextMenu.h"
#include "../UI/PageControlStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "KernelDock.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TableRowFilterSupport.h"
#include "../UI/VisibleTableWidget.h"

// ============================================================
// KernelCommunicationEndpointTab.cpp
// 作用说明：
// 1) 聚合根目录 ALPC/Port 与 \RPC Control 下的通信端点对象；
// 2) 使用 Object Manager 目录枚举结果做只读展示；
// 3) 页面不枚举任何进程句柄，也不调用 KswordARK 驱动。
// ============================================================

#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>
#include <thread>
#include <utility>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    enum class CommunicationEndpointColumn : int
    {
        Source = 0,
        Name,
        Type,
        FullPath,
        Status,
        Count
    };

    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    KernelObjectDirectoryDeepResult queryEndpointRoot(const QString& rootPath)
    {
        KernelObjectDirectoryDeepOptions options;
        options.rootPath = rootPath;
        options.maxDepth = 0;
        options.maxEntriesPerDirectory = 8192;
        options.maxTotalEntries = 8192;
        return runKernelObjectDirectoryDeepSnapshotTask(options);
    }
}

KernelCommunicationEndpointTab::KernelCommunicationEndpointTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();

    QMetaObject::invokeMethod(this, [this]() {
        refreshAsync();
    }, Qt::QueuedConnection);
}

void KernelCommunicationEndpointTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    auto* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);

    m_refreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), this);
    KswordTheme::ApplyCompactIconButtonMetrics(m_refreshButton);
    m_refreshButton->setToolTip(kernelText("kernel.communication_endpoint.toolbar.refresh.tooltip", QStringLiteral("刷新通信端点对象")));
    m_refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    m_filterEdit = new QLineEdit(this);
    ks::ui::StyleSearchField(m_filterEdit);
    m_filterEdit->setPlaceholderText(kernelText("kernel.communication_endpoint.toolbar.filter.placeholder", QStringLiteral("按名称、类型、路径筛选")));
    m_filterEdit->setClearButtonEnabled(true);

    m_statusLabel = new QLabel(kernelText("kernel.communication_endpoint.status.waiting", QStringLiteral("状态：等待刷新")), this);
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    toolbarLayout->addWidget(m_refreshButton, 0);
    toolbarLayout->addWidget(m_filterEdit, 1);
    toolbarLayout->addWidget(m_statusLabel, 0);
    ks::ui::NormalizeToolbarRow(toolbarLayout);
    rootLayout->addLayout(toolbarLayout);

    m_table = new ks::ui::VisibleTableWidget(this);
    // 通信端点增删是主要排查内容，保留跨次采集对比。
    ks::ui::SetTableActionBarMode(m_table, ks::ui::TableActionBarMode::Full);
    m_table->setColumnCount(static_cast<int>(CommunicationEndpointColumn::Count));
    m_table->setHorizontalHeaderLabels(QStringList{
        kernelText("kernel.communication_endpoint.header.source", QStringLiteral("来源目录")),
        kernelText("kernel.communication_endpoint.header.name", QStringLiteral("名称")),
        kernelText("kernel.communication_endpoint.header.type", QStringLiteral("类型")),
        kernelText("kernel.communication_endpoint.header.full_path", QStringLiteral("完整路径")),
        kernelText("kernel.communication_endpoint.header.status", QStringLiteral("状态"))
        });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(CommunicationEndpointColumn::FullPath), QHeaderView::Stretch);
    rootLayout->addWidget(m_table, 1);
}

void KernelCommunicationEndpointTab::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        refreshAsync();
    });
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(120);
    connect(m_filterTimer, &QTimer::timeout, this, [this]() { applyFilter(); });
    connect(m_table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this, [this]() {
        QTimer::singleShot(0, this, [this]() { applyFilter(); });
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() { m_filterTimer->start(); });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showContextMenu(localPosition);
    });
}

void KernelCommunicationEndpointTab::refreshAsync()
{
    if (m_refreshing.exchange(true))
    {
        return;
    }

    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.communication_endpoint.status.refreshing", QStringLiteral("状态：刷新中...")));
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelCommunicationEndpointTab> guardThis(this);
    std::thread([guardThis]() {
        QString errorText;
        std::vector<KernelObjectDirectoryDeepEntry> rows;

        const KernelObjectDirectoryDeepResult rootResult = queryEndpointRoot(QStringLiteral("\\"));
        const KernelObjectDirectoryDeepResult rpcResult = queryEndpointRoot(QStringLiteral("\\RPC Control"));
        const bool success = rootResult.success || rpcResult.success;

        if (!rootResult.success)
        {
            errorText += kernelText("kernel.communication_endpoint.error.root_failed", QStringLiteral("\\ 枚举失败：%1\n")).arg(rootResult.errorText);
        }
        if (!rpcResult.success)
        {
            errorText += kernelText("kernel.communication_endpoint.error.rpc_failed", QStringLiteral("\\RPC Control 枚举失败：%1\n")).arg(rpcResult.errorText);
        }

        for (const KernelObjectDirectoryDeepEntry& row : rootResult.rows)
        {
            if (isCommunicationEndpoint(row))
            {
                rows.push_back(row);
            }
        }
        for (const KernelObjectDirectoryDeepEntry& row : rpcResult.rows)
        {
            if (row.querySucceeded)
            {
                rows.push_back(row);
            }
        }

        std::sort(rows.begin(), rows.end(), [](const KernelObjectDirectoryDeepEntry& left, const KernelObjectDirectoryDeepEntry& right) {
            const int sourceCompare = QString::compare(left.directoryPath, right.directoryPath, Qt::CaseInsensitive);
            if (sourceCompare != 0)
            {
                return sourceCompare < 0;
            }
            return QString::compare(left.objectName, right.objectName, Qt::CaseInsensitive) < 0;
        });

        KernelCommunicationEndpointTab* const contextObject = guardThis.data();
        if (contextObject == nullptr)
        {
            return;
        }

        QMetaObject::invokeMethod(contextObject, [guardThis, rows = std::move(rows), errorText, success]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->applyRefreshResult(std::move(rows), errorText.trimmed(), success);
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelCommunicationEndpointTab::applyRefreshResult(
    std::vector<KernelObjectDirectoryDeepEntry> rows,
    const QString& errorText,
    const bool success)
{
    const QPointer<KernelCommunicationEndpointTab> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("kernel-communication-endpoint-snapshot"),
        { m_table },
        [safeThis, rows, errorText, success]() mutable
        {
            if (!safeThis.isNull())
            {
                safeThis->applyRefreshResult(std::move(rows), errorText, success);
            }
        }))
    {
        return;
    }

    m_refreshing.store(false);
    m_refreshButton->setEnabled(true);
    m_rows = std::move(rows);
    m_snapshotSucceeded = success;
    m_snapshotError = errorText;
    m_filterTimer->stop();
    m_filterActive = false;
    m_unfilteredSourceIndex = std::numeric_limits<std::size_t>::max();
    rebuildTable();
}

void KernelCommunicationEndpointTab::rebuildTable()
{
    if (m_table == nullptr) return;
    m_diagnosticItems.fill(nullptr);
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    for (std::size_t sourceIndex = 0; sourceIndex < m_rows.size(); ++sourceIndex)
    {
        const auto& entry = m_rows[sourceIndex];
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QStringList fields{entry.directoryPath, entry.objectName, entry.objectType, entry.fullPath, entry.statusText};
        for (int column = 0; column < fields.size(); ++column)
        {
            auto* item = readOnlyItem(fields[column]);
            item->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
            m_table->setItem(row, column, item);
        }
    }
    // Reuse a single diagnostic row; never overwrite accepted source records.
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    for (int column = 0; column < static_cast<int>(m_diagnosticItems.size()); ++column)
    {
        auto* item = readOnlyItem(QString());
        item->setData(Qt::UserRole, std::numeric_limits<qulonglong>::max());
        m_diagnosticItems[column] = item;
        m_table->setItem(row, column, item);
    }
    m_table->setSortingEnabled(true);
    applyFilter(false);
}

void KernelCommunicationEndpointTab::applyFilter(const bool allowDeferred)
{
    const QPointer<KernelCommunicationEndpointTab> guard(this);
    if (allowDeferred && ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("kernel-communication-filter"),
        {m_table}, [guard]() { if (guard) guard->applyFilter(); })) return;
    const QString keyword = m_filterEdit->text().trimmed();
    const bool active = !keyword.isEmpty();
    if (active && !m_filterActive)
    {
        const auto* current = m_table->item(m_table->currentRow(), 0);
        m_unfilteredSourceIndex = current != nullptr
            ? static_cast<std::size_t>(current->data(Qt::UserRole).toULongLong())
            : std::numeric_limits<std::size_t>::max();
        m_unfilteredHorizontalScroll = m_table->horizontalScrollBar()->value();
        m_unfilteredVerticalScroll = m_table->verticalScrollBar()->value();
    }
    std::size_t visibleCount = 0;
    QTableWidgetItem* restoredItem = nullptr;
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        const auto* item = m_table->item(row, 0);
        const auto index = item != nullptr ? static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong()) : m_rows.size();
        const bool visible = index < m_rows.size() && rowMatchesFilter(m_rows[index], keyword);
        if (visible) ++visibleCount;
        if (!active && m_filterActive && index == m_unfilteredSourceIndex && index < m_rows.size()) restoredItem = m_table->item(row, 0);
    }
    if (!m_snapshotSucceeded)
    {
        m_statusLabel->setText(kernelText("kernel.communication_endpoint.status.failed", QStringLiteral("状态：刷新失败 - %1")).arg(m_snapshotError));
        m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::ErrorHex()));
        insertDiagnosticRow(kernelText("kernel.communication_endpoint.placeholder.refresh_failed", QStringLiteral("<刷新失败>")),
            buildDiagnosticText(kernelText("kernel.communication_endpoint.diagnostic.refresh_failed", QStringLiteral("通信对象枚举失败：%1")).arg(m_snapshotError)));
    }
    else
    {
        m_statusLabel->setText(kernelText("kernel.communication_endpoint.status.summary", QStringLiteral("状态：已加载 %1 项，显示 %2 项"))
            .arg(static_cast<qulonglong>(m_rows.size())).arg(static_cast<qulonglong>(visibleCount)));
        m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::SuccessHex()));
        if (visibleCount == 0U)
        {
            const QString reason = m_rows.empty()
                ? kernelText("kernel.communication_endpoint.diagnostic.no_records", QStringLiteral("\\ 和 \\RPC Control 未返回可显示通信端点。"))
                : kernelText("kernel.communication_endpoint.diagnostic.filter_empty", QStringLiteral("当前筛选条件没有命中通信端点。"));
            insertDiagnosticRow(m_rows.empty()
                ? kernelText("kernel.communication_endpoint.placeholder.no_records", QStringLiteral("<无通信对象>"))
                : kernelText("kernel.communication_endpoint.placeholder.filter_empty", QStringLiteral("<筛选无结果>")), buildDiagnosticText(reason));
        }
    }
    // Diagnostic text can move that item under the active sort; apply visibility
    // and selection by source identity only after that sorting has completed.
    const bool showDiagnostic = !m_snapshotSucceeded || visibleCount == 0U;
    QSet<QPersistentModelIndex> excluded;
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        const auto* item = m_table->item(row, 0);
        const auto index = item != nullptr ? static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong()) : m_rows.size();
        const bool visible = index < m_rows.size() ? rowMatchesFilter(m_rows[index], keyword) : showDiagnostic;
        if (!visible) excluded.insert(m_table->model()->index(row, 0));
    }
    ks::ui::ApplyTableRowFilter(m_table, excluded);
    if (restoredItem != nullptr) m_table->setCurrentCell(restoredItem->row(), static_cast<int>(CommunicationEndpointColumn::Name));
    else if (m_table->currentRow() < 0 || ks::ui::IsTableRowExcludedByFilter(m_table, m_table->currentRow()))
    {
        for (int row = 0; row < m_table->rowCount(); ++row)
            if (!ks::ui::IsTableRowExcludedByFilter(m_table, row)) { m_table->setCurrentCell(row, static_cast<int>(CommunicationEndpointColumn::Name)); break; }
    }
    if (!active && m_filterActive)
    {
        m_table->horizontalScrollBar()->setValue(m_unfilteredHorizontalScroll);
        m_table->verticalScrollBar()->setValue(m_unfilteredVerticalScroll);
        m_unfilteredSourceIndex = std::numeric_limits<std::size_t>::max();
    }
    m_filterActive = active;
}

void KernelCommunicationEndpointTab::showContextMenu(const QPoint& localPosition)
{
    if (m_table == nullptr)
    {
        return;
    }

    const QModelIndex clickedIndex = m_table->indexAt(localPosition);
    if (clickedIndex.isValid())
    {
        m_table->setCurrentCell(clickedIndex.row(), clickedIndex.column());
    }

    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* copyRowAction = menu.addAction(kernelText("kernel.communication_endpoint.menu.copy_row", QStringLiteral("复制当前行")));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
    copyRowAction->setEnabled(m_table->currentRow() >= 0);
    const QAction* selectedAction = menu.exec(m_table->viewport()->mapToGlobal(localPosition));
    if (selectedAction == copyRowAction)
    {
        copyCurrentRow();
    }
}

void KernelCommunicationEndpointTab::copyCurrentRow() const
{
    if (m_table == nullptr || QApplication::clipboard() == nullptr)
    {
        return;
    }

    const int rowIndex = m_table->currentRow();
    if (rowIndex < 0)
    {
        return;
    }

    QStringList fields;
    for (int columnIndex = 0; columnIndex < m_table->columnCount(); ++columnIndex)
    {
        const QTableWidgetItem* item = m_table->item(rowIndex, columnIndex);
        fields.push_back(item != nullptr ? item->text() : QString());
    }
    QApplication::clipboard()->setText(fields.join('\t'));
}

QString KernelCommunicationEndpointTab::buildDiagnosticText(const QString& reasonText) const
{
    // buildDiagnosticText：
    // - 输入：空表或失败原因；
    // - 处理：补充当前筛选、数据来源和排查建议；
    // - 返回：适合放在表格状态列和复制文本中的诊断说明。
    QStringList lines;
    lines << kernelText("kernel.communication_endpoint.diagnostic.reason", QStringLiteral("原因：%1")).arg(reasonText.trimmed().isEmpty()
        ? kernelText("kernel.communication_endpoint.placeholder.not_provided", QStringLiteral("<未提供>"))
        : reasonText.trimmed());
    lines << kernelText("kernel.communication_endpoint.diagnostic.current_filter", QStringLiteral("当前筛选：%1")).arg(m_filterEdit != nullptr && !m_filterEdit->text().trimmed().isEmpty()
        ? m_filterEdit->text().trimmed()
        : kernelText("kernel.communication_endpoint.placeholder.no_filter", QStringLiteral("<无筛选>")));
    lines << kernelText("kernel.communication_endpoint.diagnostic.source_count", QStringLiteral("源记录总数：%1")).arg(static_cast<qulonglong>(m_rows.size()));
    lines << kernelText("kernel.communication_endpoint.diagnostic.source", QStringLiteral("数据来源：Object Manager 目录枚举根目录与 \\RPC Control，只读聚合 ALPC/Port/RPC 相关对象。"));
    lines << kernelText("kernel.communication_endpoint.diagnostic.next_step", QStringLiteral("建议：清空筛选，或切换到 Object Directory Deep / ALPC 页查看更具体的目录与端口关系。"));
    return lines.join(QStringLiteral(" | "));
}

void KernelCommunicationEndpointTab::insertDiagnosticRow(const QString& titleText, const QString& detailText)
{
    if (m_table == nullptr || m_diagnosticItems[0] == nullptr) return;
    const bool sorting = m_table->isSortingEnabled();
    m_table->setSortingEnabled(false);
    const QStringList fields{
        kernelText("kernel.communication_endpoint.placeholder.diagnostic", QStringLiteral("<诊断>")), titleText,
        QStringLiteral("Diagnostic"), kernelText("kernel.communication_endpoint.placeholder.no_path", QStringLiteral("<无路径>")), detailText};
    for (int column = 0; column < fields.size(); ++column) m_diagnosticItems[column]->setText(fields[column]);
    m_table->setSortingEnabled(sorting);
}

bool KernelCommunicationEndpointTab::rowMatchesFilter(const KernelObjectDirectoryDeepEntry& entry, const QString& keyword) const
{
    if (keyword.isEmpty())
    {
        return true;
    }

    return entry.directoryPath.contains(keyword, Qt::CaseInsensitive)
        || entry.objectName.contains(keyword, Qt::CaseInsensitive)
        || entry.objectType.contains(keyword, Qt::CaseInsensitive)
        || entry.fullPath.contains(keyword, Qt::CaseInsensitive)
        || entry.statusText.contains(keyword, Qt::CaseInsensitive);
}

bool KernelCommunicationEndpointTab::isCommunicationEndpoint(const KernelObjectDirectoryDeepEntry& entry)
{
    return entry.objectType.compare(QStringLiteral("ALPC Port"), Qt::CaseInsensitive) == 0
        || entry.objectType.compare(QStringLiteral("Port"), Qt::CaseInsensitive) == 0
        || entry.objectType.contains(QStringLiteral("Port"), Qt::CaseInsensitive);
}

QTableWidgetItem* KernelCommunicationEndpointTab::readOnlyItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
