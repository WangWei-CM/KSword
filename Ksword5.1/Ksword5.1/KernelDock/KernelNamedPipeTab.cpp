#include "KernelNamedPipeTab.h"
#include "../UI/TableContextMenu.h"
#include "../UI/PageControlStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "KernelDock.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TableRowFilterSupport.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ThemeBinding.h"

#include <memory>

// ============================================================
// KernelNamedPipeTab.cpp
// 作用说明：
// 1) 展示 R3 Named Pipe NPFS 目录枚举结果；
// 2) 支持刷新、过滤、复制当前行和详情查看；
// 3) 本文件不负责挂入 KernelDock，由集成会话处理项目文件和 Tab 注册。
// ============================================================

#include "../theme.h"
#include "../UI/StructuredFieldView.h"
#include "../UI/DetailLayoutRegistry.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QSplitter>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QResizeEvent>
#include <QSize>
#include <QStringList>
#include <QThreadPool>
#include <QTableView>
#include <QSortFilterProxyModel>
#include <QItemSelectionModel>
#include <QPersistentModelIndex>
#include <QTimer>
#include <QShowEvent>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    QString tableColumnText(const KernelNamedPipeEntry& row, const KernelNamedPipeTab::TableColumn column)
    {
        switch (column)
        {
        case KernelNamedPipeTab::TableColumn::PipeName:
            return row.pipeName;
        case KernelNamedPipeTab::TableColumn::NtPath:
            return row.ntPath;
        case KernelNamedPipeTab::TableColumn::Attributes:
            return row.attributesText;
        case KernelNamedPipeTab::TableColumn::LastWriteTime:
            return row.lastWriteTimeText;
        case KernelNamedPipeTab::TableColumn::Status:
            return row.statusText;
        case KernelNamedPipeTab::TableColumn::SourceDirectory:
            return row.sourceDirectory;
        default:
            return QString();
        }
    }

    ks::ui::FieldDocument buildDirectoryStatusText(const KernelNamedPipeSnapshot& snapshot)
    {
        ks::ui::FieldDocument lines;
        lines.section(QStringLiteral("路径候选状态"));
        for (const KernelNamedPipeDirectoryStatus& status : snapshot.directories)
        {
            lines.field(QStringLiteral(""), QStringLiteral("- %1").arg(QStringLiteral("%1").arg(status.candidatePath)));
            lines.field(QStringLiteral("open"), QStringLiteral("%1").arg(status.openSucceeded ? QStringLiteral("yes") : QStringLiteral("no")));
            lines.field(QStringLiteral("query"), QStringLiteral("%1").arg(status.querySucceeded ? QStringLiteral("yes") : QStringLiteral("no")));
            lines.field(QStringLiteral("rows"), QStringLiteral("%1").arg(static_cast<qulonglong>(status.returnedRows)));
            lines.field(QStringLiteral("status"), QStringLiteral("%1").arg(status.statusText));
        }
        return lines;
    }

    ks::ui::FieldDocument buildSelectedRowDetailText(const KernelNamedPipeEntry* row)
    {
        if (row == nullptr)
        {
            return ks::ui::FieldDocument{}.section(QStringLiteral("当前行")).note(QStringLiteral("<未选择>"));
        }

        ks::ui::FieldDocument lines;
        lines.section(QStringLiteral("当前行"));
        lines.field(QStringLiteral("Pipe Name"), QStringLiteral("%1").arg(row->pipeName));
        lines.field(QStringLiteral("NT Path"), QStringLiteral("%1").arg(row->ntPath));
        lines.field(QStringLiteral("Source Directory"), QStringLiteral("%1").arg(row->sourceDirectory));
        lines.field(QStringLiteral("Query Succeeded"), QStringLiteral("%1").arg(row->querySucceeded ? QStringLiteral("true") : QStringLiteral("false")));
        lines.field(QStringLiteral("Attributes"), QStringLiteral("%1").arg(row->attributesText));
        lines.field(QStringLiteral("LastWriteTime"), QStringLiteral("%1").arg(row->lastWriteTimeText));
        lines.field(QStringLiteral("LastWriteTimeRaw"), QStringLiteral("%1").arg(static_cast<qlonglong>(row->lastWriteTime)));
        lines.field(QStringLiteral("Status"), QStringLiteral("%1").arg(row->statusText));
        return lines;
    }
}

class KernelNamedPipeTab::PipeFilter final : public QSortFilterProxyModel
{
public:
    explicit PipeFilter(Model* source, QObject* parent) : QSortFilterProxyModel(parent), m_source(source)
    {
        setSortLocaleAware(true);
        setSortCaseSensitivity(Qt::CaseSensitive);
        setSourceModel(source);
    }
    void setNeedle(QString needle)
    {
        needle = needle.trimmed().toCaseFolded();
        if (m_needle == needle) return;
        m_needle = std::move(needle);
        invalidateFilter();
    }
protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex&) const override
    {
        if (m_needle.isEmpty()) return true;
        const auto* row = m_source->rowAt(sourceRow);
        if (row == nullptr) return false;
        for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
            if (tableColumnText(*row, static_cast<TableColumn>(column)).toCaseFolded().contains(m_needle)) return true;
        return false;
    }
private:
    Model* m_source = nullptr;
    QString m_needle;
};

KernelNamedPipeTab::KernelNamedPipeTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();

    QMetaObject::invokeMethod(
        this,
        [this]()
        {
            requestRefresh(false);
        },
        Qt::QueuedConnection);
}

void KernelNamedPipeTab::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(8, 8, 8, 8);
    m_rootLayout->setSpacing(6);

    m_toolbarLayout = new QHBoxLayout();
    m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolbarLayout->setSpacing(6);

    m_refreshButton = new QPushButton(this);
    m_refreshButton->setIcon(QIcon(":/Icon/handle_refresh.svg"));
    KswordTheme::ApplyCompactIconButtonMetrics(m_refreshButton);
    m_refreshButton->setToolTip(kernelText("kernel.named_pipe.toolbar.refresh.tooltip", QStringLiteral("刷新命名管道列表")));
    m_refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    m_copyButton = new QPushButton(this);
    m_copyButton->setIcon(QIcon(":/Icon/handle_copy_row.svg"));
    KswordTheme::ApplyCompactIconButtonMetrics(m_copyButton);
    m_copyButton->setToolTip(kernelText("kernel.named_pipe.toolbar.copy.tooltip", QStringLiteral("复制当前行")));
    m_copyButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    m_detailButton = new QPushButton(this);
    m_detailButton->setIcon(QIcon(":/Icon/process_details.svg"));
    KswordTheme::ApplyCompactIconButtonMetrics(m_detailButton);
    m_detailButton->setToolTip(kernelText("kernel.named_pipe.toolbar.detail.tooltip", QStringLiteral("刷新详情面板")));
    m_detailButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    m_filterEdit = new QLineEdit(this);
    // 命名管道结果树的本地文本过滤使用搜索面板主题。
    ks::ui::StyleSearchField(m_filterEdit);
    m_filterEdit->setPlaceholderText(kernelText("kernel.named_pipe.toolbar.filter.placeholder", QStringLiteral("过滤管道名、NT路径、状态")));
    m_filterEdit->setClearButtonEnabled(true);

    m_statusLabel = new QLabel(kernelText("kernel.named_pipe.status.waiting", QStringLiteral("● 等待刷新")), this);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_copyButton);
    m_toolbarLayout->addWidget(m_detailButton);
    m_toolbarLayout->addWidget(m_filterEdit, 1);
    ks::ui::NormalizeToolbarRow(m_toolbarLayout);
    m_rootLayout->addLayout(m_toolbarLayout);
    m_rootLayout->addWidget(m_statusLabel);

    m_resultTable = new ks::ui::TableActionTableView(this);
    ks::ui::SetTableActionBarMode(m_resultTable, ks::ui::TableActionBarMode::Full);
    ks::ui::SetTableRowFilterBusinessOwnership(m_resultTable, ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
    m_model = new Model({{QStringLiteral("Pipe Name")}, {QStringLiteral("NT Path")},
        {QStringLiteral("Attributes")}, {QStringLiteral("LastWriteTime")},
        {QStringLiteral("Status")}, {QStringLiteral("Source")}},
        [](const KernelNamedPipeEntry& row, int column, int role) -> QVariant {
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
                return tableColumnText(row, static_cast<TableColumn>(column));
            return {};
        }, m_resultTable);
    m_proxy = new PipeFilter(m_model, m_resultTable);
    m_resultTable->setModel(m_proxy);
    m_resultTable->verticalHeader()->setVisible(false);
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setSortingEnabled(true);
    m_resultTable->setContextMenuPolicy(Qt::CustomContextMenu);
    if (m_resultTable->horizontalHeader() != nullptr)
    {
        m_resultTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        m_resultTable->horizontalHeader()->setStretchLastSection(false);
    }

    // m_detailEdit 使用项目内置 ks::ui::StructuredFieldView：
    // - 输入：NPFS 枚举说明、候选路径状态和当前行详情；
    // - 处理：以只读代码/日志视图展示多行文本；
    // - 返回：无，控件由 Qt 父子关系释放。
    m_detailEdit = new ks::ui::StructuredFieldView(this);


    m_detailEdit->setDocument(ks::ui::FieldDocument{}.note(kernelText(
        "kernel.named_pipe.detail.intro",
        QStringLiteral(
            "说明：命名管道属于 NPFS 文件系统目录枚举，本页使用 NtOpenFile + NtQueryDirectoryFile 读取 \\Device\\NamedPipe。"
            "\n这不是 NtQueryDirectoryObject 下钻，也不是系统句柄表枚举。"))));

    // 页面显式拥有两面板的布局，详情宿主不再从 m_rootLayout 推断并重挂载。
    auto* const detailSplitter = new QSplitter(Qt::Vertical, this);
    detailSplitter->setChildrenCollapsible(false);
    detailSplitter->addWidget(m_resultTable);
    detailSplitter->addWidget(m_detailEdit);
    m_rootLayout->addWidget(detailSplitter, 1);
    ks::ui::DetailLayoutRegistry::registerStructuredHost(
        m_resultTable, m_detailEdit, this, detailSplitter, m_resultTable, m_detailEdit);
    applyAdaptiveColumnWidths();
}

void KernelNamedPipeTab::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
        {
            requestRefresh(true);
        });
    connect(m_copyButton, &QPushButton::clicked, this, [this]()
        {
            copyCurrentRow();
        });
    connect(m_detailButton, &QPushButton::clicked, this, [this]()
        {
            updateDetailPanel();
        });
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(120);
    connect(m_filterTimer, &QTimer::timeout, this, [this]() { applyFilter(); });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() { m_filterTimer->start(); });
    connect(m_resultTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]()
        {
            updateDetailPanel();
        });
    connect(m_resultTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& localPosition)
        {
            showContextMenu(localPosition);
        });
}

void KernelNamedPipeTab::requestRefresh(const bool forceRefresh)
{
    if (m_refreshInProgress)
    {
        if (forceRefresh)
        {
            m_refreshPending = true;
        }
        return;
    }

    const std::uint64_t currentTicket = ++m_refreshTicket;
    m_refreshInProgress = true;
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.named_pipe.status.enumerating", QStringLiteral("● 正在枚举 NPFS Named Pipe 目录...")));
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:700;").arg(KswordTheme::PrimaryBlueHex));

    QPointer<KernelNamedPipeTab> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, currentTicket]()
        {
            auto snapshot = std::make_shared<KernelNamedPipeSnapshot>(runKernelNamedPipeSnapshotTask());
            if (guardThis == nullptr)
            {
                return;
            }

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, currentTicket, snapshot]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->applySnapshot(currentTicket, snapshot);
                },
                Qt::QueuedConnection);
        });
    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void KernelNamedPipeTab::applySnapshot(
    const std::uint64_t refreshTicket,
    std::shared_ptr<KernelNamedPipeSnapshot> snapshot)
{
    if (refreshTicket < m_refreshTicket)
    {
        return;
    }

    const QPointer<KernelNamedPipeTab> guardThis(this);
    const auto deferredSnapshot = snapshot;
    if (ks::ui::DeferItemViewUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("kernel-named-pipe-snapshot-apply"),
        { m_resultTable },
        [guardThis, refreshTicket, deferredSnapshot]()
        {
            if (!guardThis.isNull())
            {
                guardThis->applySnapshot(refreshTicket, deferredSnapshot);
            }
        }))
    {
        return;
    }

    m_model->setRows(std::move(snapshot->rows));
    m_lastSnapshot = std::move(*snapshot);
    rebuildTable();
    applyFilter();
    updateDetailPanel();

    m_refreshInProgress = false;
    m_refreshButton->setEnabled(true);

    QString statusText = m_lastSnapshot.taskSucceeded
        ? kernelText("kernel.named_pipe.status.completed", QStringLiteral("● 刷新完成 | %1")).arg(m_lastSnapshot.summaryText)
        : kernelText("kernel.named_pipe.status.failed", QStringLiteral("● 刷新失败 | %1")).arg(m_lastSnapshot.errorText);
    if (!m_lastSnapshot.errorText.trimmed().isEmpty() && m_lastSnapshot.taskSucceeded)
    {
        statusText += kernelText("kernel.named_pipe.status.additional_error", QStringLiteral(" | %1")).arg(m_lastSnapshot.errorText);
    }
    m_statusLabel->setText(statusText);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
        .arg(m_lastSnapshot.anyQuerySucceeded ? KswordTheme::SuccessHex() : KswordTheme::WarningHex()));

    if (m_refreshPending)
    {
        m_refreshPending = false;
        QMetaObject::invokeMethod(
            this,
            [this]()
            {
                requestRefresh(true);
            },
            Qt::QueuedConnection);
    }
}

void KernelNamedPipeTab::rebuildTable()
{
    if (m_resultTable == nullptr) return;
    ks::ui::DetailLayoutRegistry::prepareDataRebuild(m_detailEdit);
    applyFilter();
    // The old tree selected the first canonical row before sorting was restored.
    if (m_model->rowCount() > 0)
        m_resultTable->setCurrentIndex(m_proxy->mapFromSource(m_model->index(0, 0)));
    applyAdaptiveColumnWidths();
}

void KernelNamedPipeTab::applyFilter()
{
    if (m_resultTable == nullptr || m_filterEdit == nullptr) return;
    const QPointer<KernelNamedPipeTab> guard(this);
    if (ks::ui::DeferItemViewUiCommitIfContextMenuOpen(this, QStringLiteral("kernel-named-pipe-filter"),
        {m_resultTable}, [guard]() { if (guard) guard->applyFilter(); })) return;
    if (!isVisible()) return;
    m_proxy->setNeedle(m_filterEdit->text());
}

void KernelNamedPipeTab::updateDetailPanel()
{
    if (m_detailEdit == nullptr)
    {
        return;
    }

    ks::ui::FieldDocument detailLines;
    detailLines.section(QStringLiteral("说明"));
    detailLines.note(QStringLiteral("命名管道属于 NPFS 文件系统目录枚举，本页使用 NtOpenFile + NtQueryDirectoryFile 读取 \\Device\\NamedPipe 或等价路径。"));
    detailLines.note(QStringLiteral("这不是 NtQueryDirectoryObject 下钻，也不是系统句柄表枚举"));
    detailLines.note(QStringLiteral("因此不会列出持有管道句柄的进程。"));

    detailLines.nodes += buildDirectoryStatusText(m_lastSnapshot).nodes;

    detailLines.nodes += buildSelectedRowDetailText(selectedRow()).nodes;
    m_detailEdit->setDocument(detailLines);
}

void KernelNamedPipeTab::copyCurrentRow()
{
    const auto* row = selectedRow();
    if (row == nullptr) return;
    QStringList fields;
    for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
        fields.push_back(tableColumnText(*row, static_cast<TableColumn>(column)));
    QApplication::clipboard()->setText(fields.join('\t'));
}

void KernelNamedPipeTab::showContextMenu(const QPoint& localPosition)
{
    if (m_resultTable == nullptr) return;
    const QPersistentModelIndex clicked(m_resultTable->indexAt(localPosition));
    if (!clicked.isValid()) return;
    m_resultTable->setCurrentIndex(clicked);
    const QPointer<KernelNamedPipeTab> guard(this);
    const QPointer<QTableView> table(m_resultTable);
    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* copyRowAction = menu.addAction(QIcon(":/Icon/handle_copy_row.svg"), kernelText("kernel.named_pipe.menu.copy_row", QStringLiteral("复制当前行")));
    ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
    QAction* detailAction = menu.addAction(QIcon(":/Icon/process_details.svg"), kernelText("kernel.named_pipe.menu.refresh_detail", QStringLiteral("刷新详情")));
    QAction* selectedAction = menu.exec(table->viewport()->mapToGlobal(localPosition));
    if (guard.isNull() || table.isNull() || !clicked.isValid() || clicked.model() != table->model()) return;
    table->setCurrentIndex(clicked);
    if (selectedAction == copyRowAction) copyCurrentRow();
    else if (selectedAction == detailAction) updateDetailPanel();
}

const KernelNamedPipeEntry* KernelNamedPipeTab::selectedRow() const
{
    if (m_resultTable == nullptr || m_resultTable->model() != m_proxy) return nullptr;
    const auto source = m_proxy->mapToSource(m_resultTable->currentIndex());
    return source.isValid() ? m_model->rowAt(source.row()) : nullptr;
}

void KernelNamedPipeTab::applyAdaptiveColumnWidths()
{
    if (m_resultTable == nullptr || m_resultTable->horizontalHeader() == nullptr)
    {
        return;
    }

    QHeaderView* header = m_resultTable->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);

    const int viewportWidth = m_resultTable->viewport()->width();
    if (viewportWidth <= 0)
    {
        return;
    }

    const int nameWidth = 220;
    const int attributesWidth = 180;
    const int timeWidth = 190;
    const int statusWidth = 220;
    const int sourceWidth = 160;
    const int pathWidth = std::max(320, viewportWidth - nameWidth - attributesWidth - timeWidth - statusWidth - sourceWidth - 24);

    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::PipeName), nameWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::NtPath), pathWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::Attributes), attributesWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::LastWriteTime), timeWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::Status), statusWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::SourceDirectory), sourceWidth);
}

void KernelNamedPipeTab::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    applyAdaptiveColumnWidths();
}

void KernelNamedPipeTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    applyFilter();
}
