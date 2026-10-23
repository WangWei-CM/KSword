#include "KernelSymbolicLinkTab.h"
#include "../UI/TableContextMenu.h"
#include "../UI/PageControlStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "KernelDock.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TableHeaderSortingSupport.h"
#include "../UI/TableRowFilterSupport.h"
#include "../UI/VisibleTableWidget.h"

// ============================================================
// KernelSymbolicLinkTab.cpp
// 作用说明：
// 1) 构建符号链接专项视图；
// 2) 后台执行 R3 SymbolicLink 枚举，主线程刷新表格；
// 3) 提供按对象字段/目标路径过滤和复制操作。
// ============================================================

#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QStringList>
#include <QTableView>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QScrollBar>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    QString buttonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString inputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %1;border-radius:3px;background:transparent;color:%2;padding:3px 6px;}"
            "QLineEdit:focus{border:1px solid %3;}")
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }
}

class KernelSymbolicLinkTab::LinkFilter final : public QSortFilterProxyModel
{
public:
    explicit LinkFilter(Model* source, QObject* parent) : QSortFilterProxyModel(parent), m_source(source)
    {
        // QTableWidgetItem text ordering is a case-sensitive locale comparison.
        // Preserve its one-shot policy instead of re-sorting on each source edit.
        setDynamicSortFilter(false);
        setSortRole(Qt::DisplayRole);
        setSortCaseSensitivity(Qt::CaseSensitive);
        setSortLocaleAware(true);
        setSourceModel(source);
    }
    void setKeywords(const QString& keyword, const QString& target)
    {
        if (m_keyword == keyword && m_target == target) return;
        m_keyword = keyword;
        m_target = target;
        invalidateFilter();
    }
protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex&) const override
    {
        const auto* row = m_source->rowAt(sourceRow);
        return row != nullptr
            && (m_keyword.isEmpty() || row->mergedText.contains(m_keyword, Qt::CaseInsensitive)
                || row->targetMergedText.contains(m_keyword, Qt::CaseInsensitive))
            && (m_target.isEmpty() || row->targetMergedText.contains(m_target, Qt::CaseInsensitive));
    }
private:
    Model* m_source = nullptr;
    QString m_keyword;
    QString m_target;
};

KernelSymbolicLinkTab::KernelSymbolicLinkTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
}

void KernelSymbolicLinkTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    auto* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(6);

    m_refreshButton = new QPushButton(this);
    m_refreshButton->setText(kernelText("kernel.symbolic_link.toolbar.refresh", QStringLiteral("刷新")));
    // 本页的解析边界并入刷新按钮提示，页面不再为一句说明常驻一行。
    m_refreshButton->setToolTip(kernelText("kernel.symbolic_link.toolbar.refresh.tooltip", QStringLiteral("枚举常见对象目录中的 SymbolicLink。本页只解析目标，不递归展开；目标指向 Directory 时由「目录递归」页处理。")));
    m_refreshButton->setStyleSheet(buttonStyle());

    m_copyTargetButton = new QPushButton(this);
    m_copyTargetButton->setText(kernelText("kernel.symbolic_link.toolbar.copy_target", QStringLiteral("复制目标")));
    m_copyTargetButton->setToolTip(kernelText("kernel.symbolic_link.toolbar.copy_target.tooltip", QStringLiteral("复制当前选中符号链接的 targetPath。")));
    m_copyTargetButton->setStyleSheet(buttonStyle());

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(kernelText("kernel.symbolic_link.toolbar.filter.placeholder", QStringLiteral("过滤目录 / 名称 / 完整路径 / 状态")));
    m_filterEdit->setClearButtonEnabled(true);
    ks::ui::StyleSearchField(m_filterEdit);

    m_targetFilterEdit = new QLineEdit(this);
    m_targetFilterEdit->setPlaceholderText(kernelText("kernel.symbolic_link.toolbar.target_filter.placeholder", QStringLiteral("按目标路径 / DOS 候选过滤")));
    m_targetFilterEdit->setClearButtonEnabled(true);
    ks::ui::StyleSearchField(m_targetFilterEdit);

    m_statusLabel = new QLabel(kernelText("kernel.symbolic_link.status.waiting", QStringLiteral("状态：等待刷新")), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(m_refreshButton, 0);
    toolLayout->addWidget(m_copyTargetButton, 0);
    toolLayout->addWidget(m_filterEdit, 1);
    toolLayout->addWidget(m_targetFilterEdit, 1);
    toolLayout->addWidget(m_statusLabel, 0);
    ks::ui::NormalizeToolbarRow(toolLayout);
    rootLayout->addLayout(toolLayout);

    m_table = new ks::ui::TableActionTableView(this);
    // 目标路径检索主要用于定位与复制，收拢完整快照栏。
    ks::ui::SetTableActionBarMode(m_table, ks::ui::TableActionBarMode::Compact);
    m_model = new Model({
        {QStringLiteral("sourceDirectory")}, {QStringLiteral("linkName")},
        {QStringLiteral("fullPath")}, {QStringLiteral("targetPath")},
        {QStringLiteral("dosCandidate")}, {QStringLiteral("statusText")}},
        [](const TableRow& row, const int column, const int role) -> QVariant {
            if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
            switch (static_cast<Column>(column))
            {
            case Column::SourceDirectory: return row.entry.sourceDirectory;
            case Column::LinkName: return row.entry.linkName;
            case Column::FullPath: return row.entry.fullPath;
            case Column::TargetPath: return row.entry.targetPath;
            case Column::DosCandidate: return row.entry.dosCandidate;
            case Column::StatusText: return row.entry.statusText;
            default: return {};
            }
        }, m_table);
    m_proxy = new LinkFilter(m_model, m_table);
    m_table->setModel(m_proxy);
    ks::ui::InstallTableHeaderClickSorting(m_table);
    ks::ui::SetTableRowFilterBusinessOwnership(m_table, ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::SourceDirectory), QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::LinkName), QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::FullPath), QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::TargetPath), QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::DosCandidate), QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::StatusText), QHeaderView::ResizeToContents);
    rootLayout->addWidget(m_table, 1);
}

void KernelSymbolicLinkTab::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        refreshAsync();
    });
    connect(m_copyTargetButton, &QPushButton::clicked, this, [this]() {
        copyCurrentTarget();
    });
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(120);
    connect(m_filterTimer, &QTimer::timeout, this, [this]() { applyFilters(); });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() { m_filterTimer->start(); });
    connect(m_targetFilterEdit, &QLineEdit::textChanged, this, [this]() { m_filterTimer->start(); });
    connect(m_table, &QTableView::customContextMenuRequested, this, [this](const QPoint& position) {
        showContextMenu(position);
    });
}

void KernelSymbolicLinkTab::refreshAsync()
{
    if (m_refreshing)
    {
        return;
    }

    m_refreshing = true;
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.symbolic_link.status.enumerating", QStringLiteral("状态：正在枚举 SymbolicLink...")));

    QPointer<KernelSymbolicLinkTab> guardThis(this);
    auto* task = QRunnable::create([guardThis]() {
        std::vector<KernelSymbolicLinkEntry> rows;
        QString errorText;
        const bool ok = runKernelSymbolicLinkSnapshotTask(rows, errorText);

        QMetaObject::invokeMethod(qApp, [guardThis, rows = std::move(rows), errorText, ok]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->applySnapshotResult(std::move(rows), errorText, ok);
        }, Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void KernelSymbolicLinkTab::applySnapshotResult(
    std::vector<KernelSymbolicLinkEntry> rows,
    const QString& errorText,
    const bool ok)
{
    const QPointer<KernelSymbolicLinkTab> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("kernel-symbolic-link-snapshot"),
        { m_table },
        [safeThis, rows, errorText, ok]() mutable
        {
            if (!safeThis.isNull())
            {
                safeThis->applySnapshotResult(std::move(rows), errorText, ok);
            }
        }))
    {
        return;
    }

    m_refreshing = false;
    m_refreshButton->setEnabled(true);

    if (!ok)
    {
        m_model->clearRows();
        m_unfilteredSelection = QModelIndex();
        m_filterActive = false;
        m_statusLabel->setText(kernelText("kernel.symbolic_link.status.failed", QStringLiteral("状态：失败 | %1")).arg(errorText));
        return;
    }

    std::vector<TableRow> tableRows;
    tableRows.reserve(rows.size());
    for (auto& row : rows)
    {
        TableRow value;
        value.mergedText = QStringList{row.sourceDirectory, row.linkName, row.fullPath, row.statusText}.join(QStringLiteral(" | "));
        value.targetMergedText = QStringList{row.targetPath, row.dosCandidate}.join(QStringLiteral(" | "));
        value.entry = std::move(row);
        tableRows.push_back(std::move(value));
    }
    m_filterTimer->stop();
    m_unfilteredSelection = QModelIndex();
    m_filterActive = false;
    m_model->setRows(std::move(tableRows));
    applyFilters();
}

void KernelSymbolicLinkTab::applyFilters()
{
    const QPointer<KernelSymbolicLinkTab> guard(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("kernel-symbolic-link-filter"),
        {m_table}, [guard]() { if (guard) guard->applyFilters(); })) return;
    const QString keyword = m_filterEdit->text().trimmed();
    const QString target = m_targetFilterEdit->text().trimmed();
    const bool active = !keyword.isEmpty() || !target.isEmpty();
    if (active && !m_filterActive)
    {
        m_unfilteredSelection = m_proxy->mapToSource(m_table->currentIndex());
        m_unfilteredHorizontalScroll = m_table->horizontalScrollBar()->value();
        m_unfilteredVerticalScroll = m_table->verticalScrollBar()->value();
    }
    m_proxy->setKeywords(keyword, target);
    if (!active && m_filterActive)
    {
        if (m_unfilteredSelection.isValid()) m_table->setCurrentIndex(m_proxy->mapFromSource(m_unfilteredSelection));
        m_table->horizontalScrollBar()->setValue(m_unfilteredHorizontalScroll);
        m_table->verticalScrollBar()->setValue(m_unfilteredVerticalScroll);
        m_unfilteredSelection = QModelIndex();
    }
    m_filterActive = active;
    m_statusLabel->setText(kernelText("kernel.symbolic_link.status.summary", QStringLiteral("状态：显示 %1 / %2"))
        .arg(m_proxy->rowCount()).arg(m_model->rowCount()));
}

const KernelSymbolicLinkEntry* KernelSymbolicLinkTab::entryAtVisibleRow(const int row) const
{
    const QModelIndex source = m_proxy->mapToSource(m_proxy->index(row, 0));
    const auto* value = source.isValid() ? m_model->rowAt(source.row()) : nullptr;
    return value != nullptr ? &value->entry : nullptr;
}

void KernelSymbolicLinkTab::copyCurrentTarget() const
{
    const auto* entry = entryAtVisibleRow(m_table->currentIndex().row());
    if (entry != nullptr) QApplication::clipboard()->setText(entry->targetPath);
}

void KernelSymbolicLinkTab::copyCurrentRow() const
{
    const auto* entry = entryAtVisibleRow(m_table->currentIndex().row());
    if (entry != nullptr) QApplication::clipboard()->setText(rowToTsv(*entry));
}

void KernelSymbolicLinkTab::showContextMenu(const QPoint& position)
{
    const QModelIndex index = m_table->indexAt(position);
    if (!index.isValid())
    {
        return;
    }
    m_table->setCurrentIndex(index);

    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* copyCellAction = menu.addAction(kernelText("kernel.symbolic_link.menu.copy_cell", QStringLiteral("复制单元格")));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyCellAction, ks::ui::TableContextActionRole::CopyCell);
    QAction* copyTargetAction = menu.addAction(kernelText("kernel.symbolic_link.menu.copy_target", QStringLiteral("复制 targetPath")));
    QAction* copyDosAction = menu.addAction(kernelText("kernel.symbolic_link.menu.copy_dos_candidate", QStringLiteral("复制 dosCandidate")));
    QAction* copyRowAction = menu.addAction(kernelText("kernel.symbolic_link.menu.copy_row", QStringLiteral("复制整行")));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
    QAction* filterTargetAction = menu.addAction(kernelText("kernel.symbolic_link.menu.filter_target", QStringLiteral("按此目标路径过滤")));

    const QAction* selectedAction = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (selectedAction == nullptr)
    {
        return;
    }

    const auto* entry = entryAtVisibleRow(index.row());
    if (entry == nullptr) return;
    const KernelSymbolicLinkEntry& row = *entry;

    if (selectedAction == copyCellAction)
    {
        QApplication::clipboard()->setText(index.data().toString());
    }
    else if (selectedAction == copyTargetAction)
    {
        QApplication::clipboard()->setText(row.targetPath);
    }
    else if (selectedAction == copyDosAction)
    {
        QApplication::clipboard()->setText(row.dosCandidate);
    }
    else if (selectedAction == copyRowAction)
    {
        copyCurrentRow();
    }
    else if (selectedAction == filterTargetAction)
    {
        m_targetFilterEdit->setText(row.targetPath);
    }
}


QString KernelSymbolicLinkTab::rowToTsv(const KernelSymbolicLinkEntry& row)
{
    return QStringList{
        row.sourceDirectory,
        row.linkName,
        row.fullPath,
        row.targetPath,
        row.dosCandidate,
        row.statusText
    }.join('\t');
}
