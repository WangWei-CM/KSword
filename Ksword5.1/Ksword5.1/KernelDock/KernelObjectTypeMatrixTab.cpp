#include "KernelObjectTypeMatrixTab.h"
#include "../UI/VisibleTableWidget.h"

// ============================================================
// KernelObjectTypeMatrixTab.cpp
// 作用说明：
// 1) 展示 NtQueryObject(ObjectTypesInformation) 返回的对象类型统计；
// 2) 为每类对象补充“是否可继续枚举/应使用什么 API”的策略提示；
// 3) 页面独立异步刷新，便于挂入对象命名空间二级 Tab。
// ============================================================

#include "KernelDockQueryWorker.h"
#include "../UI/CodeEditorWidget.h"
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
#include <QVBoxLayout>

#include <thread>
#include <utility>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    enum class ObjectTypeMatrixColumn : int
    {
        TypeIndex = 0,
        TypeName,
        ObjectCount,
        HandleCount,
        AccessMask,
        Strategy,
        Count
    };

    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString cellText(const KernelObjectTypeEntry& entry, const ObjectTypeMatrixColumn column)
    {
        switch (column)
        {
        case ObjectTypeMatrixColumn::TypeIndex:
            return QString::number(entry.typeIndex);
        case ObjectTypeMatrixColumn::TypeName:
            return entry.typeNameText;
        case ObjectTypeMatrixColumn::ObjectCount:
            return QString::number(entry.totalObjectCount);
        case ObjectTypeMatrixColumn::HandleCount:
            return QString::number(entry.totalHandleCount);
        case ObjectTypeMatrixColumn::AccessMask:
            return KernelObjectTypeMatrixTab::formatAccessMask(entry.validAccessMask);
        case ObjectTypeMatrixColumn::Strategy:
            return KernelObjectTypeMatrixTab::strategyForType(entry.typeNameText);
        default:
            return QString();
        }
    }
}

KernelObjectTypeMatrixTab::KernelObjectTypeMatrixTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();

    QMetaObject::invokeMethod(this, [this]() {
        refreshAsync();
    }, Qt::QueuedConnection);
}

void KernelObjectTypeMatrixTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    auto* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);

    m_refreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), this);
    m_refreshButton->setFixedWidth(34);
    m_refreshButton->setToolTip(kernelText("kernel.object_type.toolbar.refresh.tooltip", QStringLiteral("刷新对象类型统计")));
    m_refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(kernelText("kernel.object_type.toolbar.filter.placeholder", QStringLiteral("按类型名、编号、策略筛选")));
    m_filterEdit->setClearButtonEnabled(true);

    m_statusLabel = new QLabel(kernelText("kernel.object_type.status.waiting", QStringLiteral("状态：等待刷新")), this);
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    toolbarLayout->addWidget(m_refreshButton, 0);
    toolbarLayout->addWidget(m_filterEdit, 1);
    toolbarLayout->addWidget(m_statusLabel, 0);
    rootLayout->addLayout(toolbarLayout);

    m_table = new ks::ui::VisibleTableWidget(this);
    m_table->setColumnCount(static_cast<int>(ObjectTypeMatrixColumn::Count));
    m_table->setHorizontalHeaderLabels(QStringList{
        kernelText("kernel.object_type.header.index", QStringLiteral("类型编号")),
        kernelText("kernel.object_type.header.name", QStringLiteral("类型名")),
        kernelText("kernel.object_type.header.object_count", QStringLiteral("对象数")),
        kernelText("kernel.object_type.header.handle_count", QStringLiteral("句柄数")),
        kernelText("kernel.object_type.header.access_mask", QStringLiteral("访问掩码")),
        kernelText("kernel.object_type.header.strategy", QStringLiteral("枚举策略"))
        });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setStyleSheet(QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}").arg(KswordTheme::PrimaryBlueHex));
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(ObjectTypeMatrixColumn::Strategy), QHeaderView::Stretch);
    rootLayout->addWidget(m_table, 2);

    // 详情区用途：
    // - 输入：当前表格选中对象类型；
    // - 处理：展开访问掩码、对象/句柄数量、枚举策略和后续下钻建议；
    // - 返回：无返回值，文本写入项目统一 CodeEditorWidget，避免这个新页只剩摘要表格。
    m_detailEditor = new CodeEditorWidget(this);
    m_detailEditor->setReadOnly(true);
    m_detailEditor->setText(kernelText("kernel.object_type.detail.select_hint", QStringLiteral("请选择一个对象类型查看枚举策略和下钻建议。")));
    rootLayout->addWidget(m_detailEditor, 1);
}

void KernelObjectTypeMatrixTab::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        refreshAsync();
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() {
        rebuildTable();
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](const int currentRow, int, int, int) {
        updateDetailForRow(currentRow);
    });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showContextMenu(localPosition);
    });
}

void KernelObjectTypeMatrixTab::refreshAsync()
{
    if (m_refreshing.exchange(true))
    {
        return;
    }

    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.object_type.status.refreshing", QStringLiteral("状态：刷新中...")));
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelObjectTypeMatrixTab> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelObjectTypeEntry> rows;
        QString errorText;
        const bool success = runKernelTypeSnapshotTask(rows, errorText);

        KernelObjectTypeMatrixTab* const contextObject = guardThis.data();
        if (contextObject == nullptr)
        {
            return;
        }

        QMetaObject::invokeMethod(contextObject, [guardThis, rows = std::move(rows), errorText, success]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->applyRefreshResult(std::move(rows), errorText, success);
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelObjectTypeMatrixTab::applyRefreshResult(
    std::vector<KernelObjectTypeEntry> rows,
    const QString& errorText,
    const bool success)
{
    m_refreshing.store(false);
    m_refreshButton->setEnabled(true);

    if (!success)
    {
        m_rows.clear();
        m_statusLabel->setText(kernelText("kernel.object_type.status.refresh_failed", QStringLiteral("状态：刷新失败 - %1")).arg(errorText));
        m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
        insertDiagnosticRow(
            kernelText("kernel.object_type.placeholder.refresh_failed", QStringLiteral("<刷新失败>")),
            buildDiagnosticDetailText(kernelText("kernel.object_type.diagnostic.refresh_failed", QStringLiteral("对象类型矩阵刷新失败：%1")).arg(errorText)));
        if (m_detailEditor != nullptr)
        {
            m_detailEditor->setText(buildDiagnosticDetailText(kernelText("kernel.object_type.diagnostic.refresh_failed", QStringLiteral("对象类型矩阵刷新失败：%1")).arg(errorText)));
        }
        return;
    }

    m_rows = std::move(rows);
    rebuildTable();
}

void KernelObjectTypeMatrixTab::rebuildTable()
{
    if (m_table == nullptr)
    {
        return;
    }

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    std::size_t visibleCount = 0;
    for (const KernelObjectTypeEntry& entry : m_rows)
    {
        if (!rowMatchesFilter(entry))
        {
            continue;
        }

        const int rowIndex = m_table->rowCount();
        m_table->insertRow(rowIndex);
        for (int columnIndex = 0; columnIndex < static_cast<int>(ObjectTypeMatrixColumn::Count); ++columnIndex)
        {
            const auto column = static_cast<ObjectTypeMatrixColumn>(columnIndex);
            auto* item = readOnlyItem(cellText(entry, column));
            item->setData(Qt::UserRole, static_cast<qulonglong>(&entry - m_rows.data()));
            m_table->setItem(rowIndex, columnIndex, item);
        }
        ++visibleCount;
    }

    m_table->setSortingEnabled(true);
    m_statusLabel->setText(kernelText("kernel.object_type.status.summary", QStringLiteral("状态：已加载 %1 类，显示 %2 类"))
        .arg(static_cast<qulonglong>(m_rows.size()))
        .arg(static_cast<qulonglong>(visibleCount)));
    m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#3A8F3A")));

    if (m_table->rowCount() > 0)
    {
        const int targetRow = m_table->currentRow() >= 0 ? m_table->currentRow() : 0;
        m_table->setCurrentCell(qMin(targetRow, m_table->rowCount() - 1), 0);
        updateDetailForRow(m_table->currentRow());
    }
    else if (m_detailEditor != nullptr)
    {
        const QString reasonText = m_rows.empty()
            ? kernelText("kernel.object_type.diagnostic.no_records", QStringLiteral("NtQueryObject(ObjectTypesInformation) 未返回对象类型记录。"))
            : kernelText("kernel.object_type.diagnostic.filter_empty", QStringLiteral("当前筛选条件下没有对象类型记录。"));
        const QString detailText = buildDiagnosticDetailText(reasonText);
        insertDiagnosticRow(
            m_rows.empty()
                ? kernelText("kernel.object_type.placeholder.no_records", QStringLiteral("<无对象类型>"))
                : kernelText("kernel.object_type.placeholder.filter_empty", QStringLiteral("<筛选无结果>")),
            detailText);
        m_table->setCurrentCell(0, static_cast<int>(ObjectTypeMatrixColumn::TypeName));
        m_detailEditor->setText(detailText);
    }
}

std::size_t KernelObjectTypeMatrixTab::sourceIndexForTableRow(const int tableRow) const
{
    // sourceIndexForTableRow：
    // - 输入：当前可见表格行号；
    // - 处理：读取 UserRole 中保存的 m_rows 原始下标，兼容排序/过滤；
    // - 返回：有效 source index；失败时返回 size_t(-1)。
    if (m_table == nullptr || tableRow < 0 || tableRow >= m_table->rowCount())
    {
        return static_cast<std::size_t>(-1);
    }

    const QTableWidgetItem* item = m_table->item(tableRow, 0);
    if (item == nullptr)
    {
        return static_cast<std::size_t>(-1);
    }
    if (!item->data(Qt::UserRole + 2).toString().isEmpty())
    {
        return static_cast<std::size_t>(-1);
    }

    const qulonglong sourceIndex = item->data(Qt::UserRole).toULongLong();
    if (sourceIndex >= static_cast<qulonglong>(m_rows.size()))
    {
        return static_cast<std::size_t>(-1);
    }
    return static_cast<std::size_t>(sourceIndex);
}

QString KernelObjectTypeMatrixTab::buildDetailText(const KernelObjectTypeEntry& entry) const
{
    // buildDetailText：
    // - 输入：对象类型矩阵的一条源记录；
    // - 处理：把表格里的短摘要展开为可读审计说明；
    // - 返回：供 CodeEditorWidget 展示的多行文本，不访问 R0、不修改系统状态。
    QStringList lines;
    lines << QStringLiteral("[Object Type Matrix Detail]");
    lines << QStringLiteral("TypeIndex: %1").arg(entry.typeIndex);
    lines << QStringLiteral("TypeName: %1").arg(entry.typeNameText);
    lines << QStringLiteral("TotalObjectCount: %1").arg(entry.totalObjectCount);
    lines << QStringLiteral("TotalHandleCount: %1").arg(entry.totalHandleCount);
    lines << QStringLiteral("ValidAccessMask: %1").arg(formatAccessMask(entry.validAccessMask));
    lines << QString();
    lines << QStringLiteral("[Enumeration Strategy]");
    lines << strategyForType(entry.typeNameText);
    lines << QString();
    lines << QStringLiteral("[Audit Meaning]");
    lines << kernelText("kernel.object_type.detail.audit.object_count", QStringLiteral("对象数用于判断该类型在 Object Manager 命名空间中的总体存在感。"));
    lines << kernelText("kernel.object_type.detail.audit.handle_count", QStringLiteral("句柄数用于判断用户态/内核态是否大量持有该类型对象。"));
    lines << kernelText("kernel.object_type.detail.audit.access_mask", QStringLiteral("访问掩码来自 NtQueryObject(ObjectTypesInformation)，用于解释该类型支持的权限位范围。"));
    lines << QString();
    lines << QStringLiteral("[Next Step]");
    if (entry.typeNameText.compare(QStringLiteral("Directory"), Qt::CaseInsensitive) == 0)
    {
        lines << kernelText("kernel.object_type.detail.next.directory", QStringLiteral("可切换到 Object Directory Deep 页，对目标目录做递归只读枚举。"));
    }
    else if (entry.typeNameText.compare(QStringLiteral("SymbolicLink"), Qt::CaseInsensitive) == 0)
    {
        lines << kernelText("kernel.object_type.detail.next.symbolic_link", QStringLiteral("可在命名对象页查看符号链接目标，重点关注跨命名空间或设备路径跳转。"));
    }
    else if (entry.typeNameText.compare(QStringLiteral("Driver"), Qt::CaseInsensitive) == 0 ||
        entry.typeNameText.compare(QStringLiteral("Device"), Qt::CaseInsensitive) == 0)
    {
        lines << kernelText("kernel.object_type.detail.next.device_driver", QStringLiteral("可切换到 Device/Driver Objects 页查看 DriverObject、DeviceObject 和 Major/FastIo 归属。"));
    }
    else if (entry.typeNameText.contains(QStringLiteral("Port"), Qt::CaseInsensitive))
    {
        lines << kernelText("kernel.object_type.detail.next.ipc", QStringLiteral("可切换到 IPC/ALPC/NamedPipe 页，把通信端点与进程、句柄和命名空间路径关联。"));
    }
    else
    {
        lines << kernelText("kernel.object_type.detail.next.generic", QStringLiteral("该类型通常需要专项页或句柄表交叉验证；当前页只提供类型级只读证据。"));
    }
    return lines.join(QChar('\n'));
}

void KernelObjectTypeMatrixTab::updateDetailForRow(const int tableRow)
{
    // updateDetailForRow：
    // - 输入：当前可见表格行号；
    // - 处理：找到源记录并刷新详情区；
    // - 返回：无返回值，失败时给出明确提示而不是留空。
    if (m_detailEditor == nullptr)
    {
        return;
    }

    const std::size_t sourceIndex = sourceIndexForTableRow(tableRow);
    if (sourceIndex == static_cast<std::size_t>(-1))
    {
        if (m_table != nullptr && tableRow >= 0 && tableRow < m_table->rowCount())
        {
            const QTableWidgetItem* item = m_table->item(tableRow, static_cast<int>(ObjectTypeMatrixColumn::TypeIndex));
            const QString diagnosticText = item != nullptr
                ? item->data(Qt::UserRole + 2).toString()
                : QString();
            if (!diagnosticText.isEmpty())
            {
                m_detailEditor->setText(diagnosticText);
                return;
            }
        }
        m_detailEditor->setText(kernelText("kernel.object_type.detail.select_hint", QStringLiteral("请选择一个对象类型查看枚举策略和下钻建议。")));
        return;
    }

    m_detailEditor->setText(buildDetailText(m_rows[sourceIndex]));
}

QString KernelObjectTypeMatrixTab::buildDiagnosticDetailText(const QString& reasonText) const
{
    // buildDiagnosticDetailText：
    // - 输入：刷新失败、源数据为空或筛选空命中的原因；
    // - 处理：补充当前筛选和本页数据来源说明；
    // - 返回：用于诊断占位行与 CodeEditorWidget 的多行文本。
    QStringList lines;
    lines << QStringLiteral("[Object Type Matrix Diagnostic]");
    lines << kernelText("kernel.object_type.diagnostic.reason", QStringLiteral("原因：%1")).arg(reasonText.trimmed().isEmpty()
        ? kernelText("kernel.object_type.placeholder.not_provided", QStringLiteral("<未提供>"))
        : reasonText.trimmed());
    lines << kernelText("kernel.object_type.diagnostic.current_filter", QStringLiteral("当前筛选：%1")).arg(m_filterEdit != nullptr && !m_filterEdit->text().trimmed().isEmpty()
        ? m_filterEdit->text().trimmed()
        : kernelText("kernel.object_type.placeholder.no_filter", QStringLiteral("<无筛选>")));
    lines << kernelText("kernel.object_type.diagnostic.source_count", QStringLiteral("源记录总数：%1")).arg(static_cast<qulonglong>(m_rows.size()));
    lines << QStringLiteral("");
    lines << kernelText("kernel.object_type.diagnostic.source_heading", QStringLiteral("[数据来源]"));
    lines << kernelText("kernel.object_type.diagnostic.source", QStringLiteral("本页使用 NtQueryObject(ObjectTypesInformation) 的对象类型统计，不访问 R0、不修改系统对象。"));
    lines << kernelText("kernel.object_type.diagnostic.no_data_explanation", QStringLiteral("若这里没有记录，通常是 API 查询失败、权限/兼容性问题，或筛选条件过窄。"));
    lines << QStringLiteral("");
    lines << kernelText("kernel.object_type.diagnostic.next_heading", QStringLiteral("[下一步]"));
    lines << kernelText("kernel.object_type.diagnostic.next.clear_filter", QStringLiteral("1. 清空筛选关键字，确认不是过滤导致空表。"));
    lines << kernelText("kernel.object_type.diagnostic.next.specialized_pages", QStringLiteral("2. 切换到 Object Directory Deep / NamedPipe / Device-Driver Objects 等专项页查看具体对象。"));
    lines << kernelText("kernel.object_type.diagnostic.next.record_status", QStringLiteral("3. 如果刷新失败，请记录状态栏错误文本用于定位 NtQueryObject 返回状态。"));
    return lines.join(QChar('\n'));
}

void KernelObjectTypeMatrixTab::insertDiagnosticRow(const QString& titleText, const QString& detailText)
{
    // insertDiagnosticRow：
    // - 输入：表格标题和详情文本；
    // - 处理：插入一行可复制的诊断占位，并把详情放入 UserRole + 2；
    // - 返回：无返回值，仅更新 UI 表格。
    if (m_table == nullptr)
    {
        return;
    }

    m_table->setSortingEnabled(false);
    m_table->setRowCount(1);

    auto* indexItem = readOnlyItem(kernelText("kernel.object_type.placeholder.diagnostic", QStringLiteral("<诊断>")));
    indexItem->setData(Qt::UserRole + 2, detailText);
    m_table->setItem(0, static_cast<int>(ObjectTypeMatrixColumn::TypeIndex), indexItem);
    m_table->setItem(0, static_cast<int>(ObjectTypeMatrixColumn::TypeName), readOnlyItem(titleText));
    m_table->setItem(0, static_cast<int>(ObjectTypeMatrixColumn::ObjectCount), readOnlyItem(QStringLiteral("0")));
    m_table->setItem(0, static_cast<int>(ObjectTypeMatrixColumn::HandleCount), readOnlyItem(QStringLiteral("0")));
    m_table->setItem(0, static_cast<int>(ObjectTypeMatrixColumn::AccessMask), readOnlyItem(formatAccessMask(0)));
    m_table->setItem(0, static_cast<int>(ObjectTypeMatrixColumn::Strategy), readOnlyItem(kernelText("kernel.object_type.placeholder.view_diagnostic", QStringLiteral("请查看下方诊断详情"))));

    m_table->setSortingEnabled(true);
}

void KernelObjectTypeMatrixTab::showContextMenu(const QPoint& localPosition)
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
    QAction* copyRowAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/process_copy_row.svg")),
        kernelText("kernel.object_type.menu.copy_row", QStringLiteral("复制当前行")));
    copyRowAction->setEnabled(m_table->currentRow() >= 0);
    const QAction* selectedAction = menu.exec(m_table->viewport()->mapToGlobal(localPosition));
    if (selectedAction == copyRowAction)
    {
        copyCurrentRow();
    }
}

void KernelObjectTypeMatrixTab::copyCurrentRow() const
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

bool KernelObjectTypeMatrixTab::rowMatchesFilter(const KernelObjectTypeEntry& entry) const
{
    const QString keyword = m_filterEdit != nullptr ? m_filterEdit->text().trimmed() : QString();
    if (keyword.isEmpty())
    {
        return true;
    }

    return QString::number(entry.typeIndex).contains(keyword, Qt::CaseInsensitive)
        || entry.typeNameText.contains(keyword, Qt::CaseInsensitive)
        || strategyForType(entry.typeNameText).contains(keyword, Qt::CaseInsensitive);
}

QString KernelObjectTypeMatrixTab::strategyForType(const QString& typeNameText)
{
    if (typeNameText.compare(QStringLiteral("Directory"), Qt::CaseInsensitive) == 0)
    {
        return kernelText("kernel.object_type.strategy.directory", QStringLiteral("NtQueryDirectoryObject 可递归枚举子对象"));
    }
    if (typeNameText.compare(QStringLiteral("SymbolicLink"), Qt::CaseInsensitive) == 0)
    {
        return kernelText("kernel.object_type.strategy.symbolic_link", QStringLiteral("NtQuerySymbolicLinkObject 可解析目标"));
    }
    if (typeNameText.compare(QStringLiteral("File"), Qt::CaseInsensitive) == 0
        || typeNameText.compare(QStringLiteral("NamedPipe"), Qt::CaseInsensitive) == 0)
    {
        return kernelText("kernel.object_type.strategy.file_pipe", QStringLiteral("文件系统目录枚举，NamedPipe 走 NPFS/NtQueryDirectoryFile"));
    }
    if (typeNameText.contains(QStringLiteral("Port"), Qt::CaseInsensitive))
    {
        return kernelText("kernel.object_type.strategy.port", QStringLiteral("通信端点对象，通常是叶子；可做命名空间聚合"));
    }
    if (typeNameText.compare(QStringLiteral("Device"), Qt::CaseInsensitive) == 0
        || typeNameText.compare(QStringLiteral("Driver"), Qt::CaseInsensitive) == 0)
    {
        return kernelText("kernel.object_type.strategy.device_driver", QStringLiteral("通常是叶子对象；可做专项属性/目录聚合"));
    }
    return kernelText("kernel.object_type.strategy.generic", QStringLiteral("通常不可下钻；按类型查询属性或在专项页聚合"));
}

QString KernelObjectTypeMatrixTab::formatAccessMask(const std::uint32_t accessMask)
{
    return QStringLiteral("0x%1")
        .arg(accessMask, 8, 16, QChar('0'))
        .toUpper();
}

QTableWidgetItem* KernelObjectTypeMatrixTab::readOnlyItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
