#include "KernelDescriptorTableTab.h"

#include "KernelDock.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/VisibleTableWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <thread>
#include <utility>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    enum DescriptorColumn : int
    {
        ColumnTable = 0,
        ColumnCpu,
        ColumnVectorSelector,
        ColumnTableBase,
        ColumnTableLimit,
        ColumnEntryAddress,
        ColumnSize,
        ColumnTargetBase,
        ColumnSelector,
        ColumnType,
        ColumnDpl,
        ColumnPresent,
        ColumnIst,
        ColumnGranularity,
        ColumnOwner,
        ColumnRisk,
        ColumnRaw,
        ColumnCount
    };

    QTableWidgetItem* readOnlyItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    QString tableStyle()
    {
        return QStringLiteral("QTableWidget{background:transparent;color:%1;} QHeaderView::section{color:%2;background:transparent;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex());
    }
}

KernelDescriptorTableTab::KernelDescriptorTableTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
}

void KernelDescriptorTableTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_firstRefreshStarted)
    {
        m_firstRefreshStarted = true;
        QMetaObject::invokeMethod(this, [this]() { refreshAsync(); }, Qt::QueuedConnection);
    }
}

void KernelDescriptorTableTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(5);

    auto* toolbar = new QHBoxLayout();
    m_refreshButton = new QPushButton(kernelText("kernel.descriptor.refresh", QStringLiteral("刷新 IDT/GDT")), this);
    m_refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    m_tableFilterCombo = new QComboBox(this);
    m_tableFilterCombo->addItem(kernelText("kernel.descriptor.filter.all", QStringLiteral("全部")), 0);
    m_tableFilterCombo->addItem(QStringLiteral("IDT"), QVariant::fromValue(static_cast<quint32>(KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER)));
    m_tableFilterCombo->addItem(QStringLiteral("GDT"), QVariant::fromValue(static_cast<quint32>(KSWORD_ARK_DRIVER_INTEGRITY_CLASS_GDT_DESCRIPTOR)));
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(kernelText("kernel.descriptor.filter.placeholder", QStringLiteral("按 CPU、向量/选择子、地址、类型、模块和风险筛选")));
    m_statusLabel = new QLabel(kernelText("kernel.descriptor.status.waiting", QStringLiteral("状态：等待刷新")), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_tableFilterCombo);
    toolbar->addWidget(m_filterEdit, 1);
    toolbar->addWidget(m_statusLabel);
    rootLayout->addLayout(toolbar);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    m_table = new ks::ui::VisibleTableWidget(splitter);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({
        kernelText("kernel.descriptor.header.table", QStringLiteral("表")),
        kernelText("kernel.descriptor.header.cpu", QStringLiteral("CPU")),
        kernelText("kernel.descriptor.header.vector_selector", QStringLiteral("向量/选择子")),
        kernelText("kernel.descriptor.header.table_base", QStringLiteral("表基址")),
        kernelText("kernel.descriptor.header.table_limit", QStringLiteral("表 Limit")),
        kernelText("kernel.descriptor.header.entry", QStringLiteral("表项地址")),
        kernelText("kernel.descriptor.header.size", QStringLiteral("大小")),
        kernelText("kernel.descriptor.header.target_base", QStringLiteral("Handler/段基址")),
        kernelText("kernel.descriptor.header.selector", QStringLiteral("SEL")),
        kernelText("kernel.descriptor.header.type", QStringLiteral("类型")),
        kernelText("kernel.descriptor.header.dpl", QStringLiteral("DPL")),
        kernelText("kernel.descriptor.header.present", QStringLiteral("Present")),
        kernelText("kernel.descriptor.header.ist", QStringLiteral("IST")),
        kernelText("kernel.descriptor.header.granularity", QStringLiteral("Granularity")),
        kernelText("kernel.descriptor.header.owner", QStringLiteral("归属模块")),
        kernelText("kernel.descriptor.header.risk", QStringLiteral("完整性")),
        kernelText("kernel.descriptor.header.raw", QStringLiteral("原始值"))});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setStyleSheet(tableStyle());
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);

    m_detailEdit = new QTextEdit(splitter);
    m_detailEdit->setReadOnly(true);
    m_detailEdit->setPlaceholderText(kernelText("kernel.descriptor.detail.placeholder", QStringLiteral("选择 IDT/GDT 表项查看结构化位域和 R0 诊断详情")));
    splitter->addWidget(m_table);
    splitter->addWidget(m_detailEdit);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(splitter, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refreshAsync(); });
    connect(m_tableFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { rebuildTable(); });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString&) { rebuildTable(); });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) { showCurrentDetail(); });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) { showCopyMenu(position); });
}

void KernelDescriptorTableTab::refreshAsync()
{
    if (m_refreshRunning)
    {
        return;
    }
    m_refreshRunning = true;
    m_firstRefreshStarted = true;
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.descriptor.status.refreshing", QStringLiteral("正在按 CPU 读取 IDTR/GDTR 与描述符...")));
    QPointer<KernelDescriptorTableTab> safeThis(this);
    std::thread([safeThis]() {
        ksword::ark::DriverClient client;
        ksword::ark::DriverIntegrityResult result = client.queryKernelCpuIntegrity(
            KSWORD_ARK_DRIVER_INTEGRITY_FLAG_CPU |
                KSWORD_ARK_DRIVER_INTEGRITY_FLAG_IDT_ENTRIES |
                KSWORD_ARK_DRIVER_INTEGRITY_FLAG_GDT_ENTRIES,
            KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS,
            KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(safeThis, [safeThis, result = std::move(result)]() mutable {
            if (safeThis != nullptr)
            {
                safeThis->applyResult(std::move(result));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDescriptorTableTab::applyResult(ksword::ark::DriverIntegrityResult result)
{
    m_refreshRunning = false;
    m_refreshButton->setEnabled(true);
    m_rows.clear();
    if (!result.io.ok)
    {
        const QString errorText = result.io.message.empty()
            ? kernelText("kernel.descriptor.status.failed", QStringLiteral("IDT/GDT 查询失败"))
            : QString::fromStdString(result.io.message);
        m_statusLabel->setText(errorText);
        rebuildTable();
        return;
    }

    std::size_t idtCount = 0U;
    std::size_t gdtCount = 0U;
    for (ksword::ark::DriverIntegrityEvidenceEntry& row : result.entries)
    {
        if (row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER)
        {
            ++idtCount;
            m_rows.push_back(std::move(row));
        }
        else if (row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_GDT_DESCRIPTOR)
        {
            ++gdtCount;
            m_rows.push_back(std::move(row));
        }
    }
    QString summary = kernelText(
        "kernel.descriptor.status.completed",
        QStringLiteral("已读取 %1 个 IDT 表项、%2 个 GDT 表项，CPU %3，协议 v%4"))
        .arg(static_cast<qulonglong>(idtCount))
        .arg(static_cast<qulonglong>(gdtCount))
        .arg(result.cpuCount)
        .arg(result.version);
    if ((result.flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_TRUNCATED) != 0U ||
        (result.statusFlags & KSWORD_ARK_DRIVER_INTEGRITY_STATUS_FLAG_TRUNCATED) != 0U)
    {
        summary += kernelText("kernel.descriptor.status.truncated", QStringLiteral("；响应已截断，部分 CPU 表项未返回"));
    }
    m_statusLabel->setText(summary);
    rebuildTable();
}

bool KernelDescriptorTableTab::rowMatchesFilter(const ksword::ark::DriverIntegrityEvidenceEntry& row) const
{
    const std::uint32_t selectedClass = static_cast<std::uint32_t>(m_tableFilterCombo->currentData().toUInt());
    if (selectedClass != 0U && row.evidenceClass != selectedClass)
    {
        return false;
    }
    const QString keyword = m_filterEdit->text().trimmed();
    if (keyword.isEmpty())
    {
        return true;
    }
    QStringList values;
    for (int column = 0; column < ColumnCount; ++column)
    {
        values.push_back(columnText(row, column));
    }
    values.push_back(QString::fromStdWString(row.detail));
    return values.join(QLatin1Char(' ')).contains(keyword, Qt::CaseInsensitive);
}

void KernelDescriptorTableTab::rebuildTable()
{
    m_table->setRowCount(0);
    for (std::size_t sourceIndex = 0U; sourceIndex < m_rows.size(); ++sourceIndex)
    {
        const ksword::ark::DriverIntegrityEvidenceEntry& row = m_rows[sourceIndex];
        if (!rowMatchesFilter(row))
        {
            continue;
        }
        const int tableRow = m_table->rowCount();
        m_table->insertRow(tableRow);
        for (int column = 0; column < ColumnCount; ++column)
        {
            QTableWidgetItem* item = readOnlyItem(columnText(row, column));
            if (column == ColumnTable)
            {
                item->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
            }
            if (column == ColumnRisk)
            {
                item->setForeground(row.riskFlags == 0U
                    ? KswordTheme::SuccessColor()
                    : KswordTheme::ErrorColor());
            }
            m_table->setItem(tableRow, column, item);
        }
    }
    m_table->resizeColumnsToContents();
    showCurrentDetail();
}

QString KernelDescriptorTableTab::tableName(const ksword::ark::DriverIntegrityEvidenceEntry& row)
{
    return row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER
        ? QStringLiteral("IDT")
        : QStringLiteral("GDT");
}

QString KernelDescriptorTableTab::descriptorTypeText(const ksword::ark::DriverIntegrityEvidenceEntry& row)
{
    if (row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER)
    {
        if (row.descriptorType == 0xEU)
        {
            return kernelText("kernel.descriptor.type.interrupt_gate", QStringLiteral("Interrupt Gate"));
        }
        if (row.descriptorType == 0xFU)
        {
            return kernelText("kernel.descriptor.type.trap_gate", QStringLiteral("Trap Gate"));
        }
        return kernelText("kernel.descriptor.type.gate", QStringLiteral("Gate 0x%1")).arg(row.descriptorType, 0, 16);
    }
    if ((row.descriptorFlags & KSWORD_ARK_DESCRIPTOR_FLAG_USER_SEGMENT) != 0U)
    {
        const bool code = (row.descriptorType & 0x8U) != 0U;
        const bool accessed = (row.descriptorType & 0x1U) != 0U;
        const bool readableWritable = (row.descriptorType & 0x2U) != 0U;
        if (code)
        {
            return kernelText("kernel.descriptor.type.code", QStringLiteral("Code%1%2"))
                .arg(readableWritable ? QStringLiteral(" R") : QString())
                .arg(accessed ? QStringLiteral(" A") : QString());
        }
        return kernelText("kernel.descriptor.type.data", QStringLiteral("Data%1%2"))
            .arg(readableWritable ? QStringLiteral(" W") : QString())
            .arg(accessed ? QStringLiteral(" A") : QString());
    }
    switch (row.descriptorType)
    {
    case 0x0U: return kernelText("kernel.descriptor.type.reserved", QStringLiteral("Reserved"));
    case 0x2U: return QStringLiteral("LDT");
    case 0x9U: return kernelText("kernel.descriptor.type.tss_available", QStringLiteral("TSS Available"));
    case 0xBU: return kernelText("kernel.descriptor.type.tss_busy", QStringLiteral("TSS Busy"));
    case 0xCU: return kernelText("kernel.descriptor.type.call_gate", QStringLiteral("Call Gate"));
    case 0xEU: return kernelText("kernel.descriptor.type.interrupt_gate", QStringLiteral("Interrupt Gate"));
    case 0xFU: return kernelText("kernel.descriptor.type.trap_gate", QStringLiteral("Trap Gate"));
    default: return kernelText("kernel.descriptor.type.system", QStringLiteral("System 0x%1")).arg(row.descriptorType, 0, 16);
    }
}

QString KernelDescriptorTableTab::riskText(const std::uint32_t riskFlags)
{
    if (riskFlags == 0U)
    {
        return kernelText("kernel.descriptor.risk.clean", QStringLiteral("正常"));
    }
    QStringList risks;
    if ((riskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DESCRIPTOR_INVALID) != 0U)
    {
        risks.push_back(kernelText("kernel.descriptor.risk.invalid", QStringLiteral("描述符异常")));
    }
    if ((riskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER) != 0U)
    {
        risks.push_back(kernelText("kernel.descriptor.risk.non_core", QStringLiteral("非核心模块")));
    }
    if ((riskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED) != 0U)
    {
        risks.push_back(kernelText("kernel.descriptor.risk.unresolved", QStringLiteral("模块未解析")));
    }
    if ((riskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_QUERY_FAILED) != 0U)
    {
        risks.push_back(kernelText("kernel.descriptor.risk.read_failed", QStringLiteral("读取失败")));
    }
    if (risks.isEmpty())
    {
        risks.push_back(hex32(riskFlags));
    }
    return risks.join(QStringLiteral(" / "));
}

QString KernelDescriptorTableTab::columnText(const ksword::ark::DriverIntegrityEvidenceEntry& row, const int column) const
{
    const bool isIdt = row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER;
    switch (column)
    {
    case ColumnTable: return tableName(row);
    case ColumnCpu: return QStringLiteral("%1:%2").arg(row.processorGroup).arg(row.processorNumber);
    case ColumnVectorSelector:
        return isIdt ? QString::number(row.vector) : hex32(row.descriptorSelector);
    case ColumnTableBase: return hex64(row.descriptorTableBase);
    case ColumnTableLimit: return hex32(row.descriptorTableLimit);
    case ColumnEntryAddress: return hex64(row.objectAddress);
    case ColumnSize: return QString::number(row.descriptorSize);
    case ColumnTargetBase: return hex64(row.descriptorBase);
    case ColumnSelector: return hex32(row.descriptorSelector);
    case ColumnType: return descriptorTypeText(row);
    case ColumnDpl: return QString::number(row.descriptorDpl);
    case ColumnPresent:
        return (row.descriptorFlags & KSWORD_ARK_DESCRIPTOR_FLAG_PRESENT) != 0U ? QStringLiteral("P") : QStringLiteral("-");
    case ColumnIst:
        return isIdt ? QString::number(static_cast<unsigned int>((row.descriptorRawLow >> 32) & 0x7ULL)) : QStringLiteral("-");
    case ColumnGranularity:
        return isIdt
            ? QStringLiteral("-")
            : ((row.descriptorFlags & KSWORD_ARK_DESCRIPTOR_FLAG_GRANULARITY_PAGE) != 0U ? QStringLiteral("PAGE") : QStringLiteral("BYTE"));
    case ColumnOwner: return QString::fromStdWString(row.ownerModule);
    case ColumnRisk: return riskText(row.riskFlags);
    case ColumnRaw:
        return row.descriptorSize > 8U
            ? hex64(row.descriptorRawLow) + QStringLiteral(" / ") + hex64(row.descriptorRawHigh)
            : hex64(row.descriptorRawLow);
    default: return {};
    }
}

QString KernelDescriptorTableTab::detailText(const ksword::ark::DriverIntegrityEvidenceEntry& row) const
{
    QStringList lines;
    lines << kernelText("kernel.descriptor.detail.table", QStringLiteral("表: %1")).arg(tableName(row));
    lines << kernelText("kernel.descriptor.detail.cpu", QStringLiteral("CPU: %1:%2")).arg(row.processorGroup).arg(row.processorNumber);
    lines << kernelText("kernel.descriptor.detail.table_range", QStringLiteral("表基址: %1  Limit: %2")).arg(hex64(row.descriptorTableBase), hex32(row.descriptorTableLimit));
    lines << kernelText("kernel.descriptor.detail.entry", QStringLiteral("表项: %1  大小: %2")).arg(hex64(row.objectAddress)).arg(row.descriptorSize);
    lines << kernelText("kernel.descriptor.detail.decoded", QStringLiteral("选择子: %1  类型: %2  DPL: %3  基址/Handler: %4  Limit: %5"))
        .arg(hex32(row.descriptorSelector), descriptorTypeText(row))
        .arg(row.descriptorDpl)
        .arg(hex64(row.descriptorBase), hex64(row.descriptorLimit));
    lines << kernelText("kernel.descriptor.detail.flags", QStringLiteral("Flags: %1  风险: %2")).arg(hex32(row.descriptorFlags), riskText(row.riskFlags));
    lines << kernelText("kernel.descriptor.detail.raw", QStringLiteral("Raw: %1 / %2")).arg(hex64(row.descriptorRawLow), hex64(row.descriptorRawHigh));
    if (!row.ownerModule.empty())
    {
        lines << kernelText("kernel.descriptor.detail.owner", QStringLiteral("归属模块: %1 [%2 +%3]"))
            .arg(QString::fromStdWString(row.ownerModule), hex64(row.ownerModuleBase), hex32(row.ownerModuleSize));
    }
    if (!row.detail.empty())
    {
        lines << QString() << QString::fromStdWString(row.detail);
    }
    return lines.join(QLatin1Char('\n'));
}

void KernelDescriptorTableTab::showCurrentDetail()
{
    if (m_table->currentRow() < 0)
    {
        m_detailEdit->clear();
        return;
    }
    const QTableWidgetItem* item = m_table->item(m_table->currentRow(), ColumnTable);
    if (item == nullptr)
    {
        m_detailEdit->clear();
        return;
    }
    const std::size_t sourceIndex = static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong());
    m_detailEdit->setPlainText(sourceIndex < m_rows.size() ? detailText(m_rows[sourceIndex]) : QString());
}

QString KernelDescriptorTableTab::hex64(const std::uint64_t value)
{
    return QStringLiteral("0x%1").arg(value, 16, 16, QLatin1Char('0')).toUpper();
}

QString KernelDescriptorTableTab::hex32(const std::uint32_t value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

QString KernelDescriptorTableTab::rowClipboardText(QTableWidget* table, const int row, const bool includeHeader)
{
    if (table == nullptr || row < 0 || row >= table->rowCount())
    {
        return {};
    }
    QStringList lines;
    if (includeHeader)
    {
        QStringList headers;
        for (int column = 0; column < table->columnCount(); ++column)
        {
            headers << (table->horizontalHeaderItem(column) == nullptr ? QString() : table->horizontalHeaderItem(column)->text());
        }
        lines << headers.join(QLatin1Char('\t'));
    }
    QStringList values;
    for (int column = 0; column < table->columnCount(); ++column)
    {
        values << (table->item(row, column) == nullptr ? QString() : table->item(row, column)->text());
    }
    lines << values.join(QLatin1Char('\t'));
    return lines.join(QLatin1Char('\n'));
}

void KernelDescriptorTableTab::showCopyMenu(const QPoint& position)
{
    const QModelIndex index = m_table->indexAt(position);
    const int row = index.isValid() ? index.row() : m_table->currentRow();
    QMenu menu(this);
    QAction* copyCell = menu.addAction(kernelText("kernel.descriptor.copy.cell", QStringLiteral("复制单元格")));
    QAction* copyRow = menu.addAction(kernelText("kernel.descriptor.copy.row", QStringLiteral("复制当前行")));
    QAction* copyAll = menu.addAction(kernelText("kernel.descriptor.copy.all", QStringLiteral("复制全部行")));
    copyCell->setEnabled(index.isValid());
    copyRow->setEnabled(row >= 0);
    copyAll->setEnabled(m_table->rowCount() > 0);
    QAction* selected = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (selected == copyCell && index.isValid())
    {
        const QTableWidgetItem* item = m_table->item(index.row(), index.column());
        QApplication::clipboard()->setText(item == nullptr ? QString() : item->text());
    }
    else if (selected == copyRow)
    {
        QApplication::clipboard()->setText(rowClipboardText(m_table, row, true));
    }
    else if (selected == copyAll)
    {
        QStringList lines;
        for (int tableRow = 0; tableRow < m_table->rowCount(); ++tableRow)
        {
            lines << rowClipboardText(m_table, tableRow, tableRow == 0);
        }
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    }
}
