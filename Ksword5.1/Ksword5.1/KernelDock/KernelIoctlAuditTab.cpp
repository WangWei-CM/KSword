#include "KernelIoctlAuditTab.h"

#include "KernelDeviceDriverObjectsWorker.h"
#include "KernelDock.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/VisibleTableWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <sstream>
#include <thread>
#include <utility>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    QString buttonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString headerStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;background:transparent;border:1px solid %2;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex());
    }

    QTableWidgetItem* readOnlyItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }
}

KernelIoctlAuditTab::KernelIoctlAuditTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    QMetaObject::invokeMethod(this, [this]() { refreshAsync(); }, Qt::QueuedConnection);
}

void KernelIoctlAuditTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(5);

    auto* toolbar = new QHBoxLayout();
    m_refreshButton = new QPushButton(kernelText("kernel.ioctl_audit.refresh", QStringLiteral("刷新派遣表")), this);
    m_refreshButton->setStyleSheet(buttonStyle());
    m_statusLabel = new QLabel(kernelText("kernel.ioctl_audit.loading", QStringLiteral("正在加载全局 DriverObject 与 KswordARK IOCTL registry...")), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_statusLabel, 1);
    rootLayout->addLayout(toolbar);

    m_innerTabs = new QTabWidget(this);
    m_dispatchPage = new QWidget(m_innerTabs);
    m_registryPage = new QWidget(m_innerTabs);
    m_dispatchTable = new ks::ui::VisibleTableWidget(m_dispatchPage);
    m_registryTable = new ks::ui::VisibleTableWidget(m_registryPage);
    for (QTableWidget* table : {m_dispatchTable, m_registryTable})
    {
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        table->setStyleSheet(QStringLiteral("QTableWidget{background:transparent;color:%1;}" ).arg(KswordTheme::TextPrimaryHex()));
        table->horizontalHeader()->setStyleSheet(headerStyle());
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->setVisible(false);
    }
    m_dispatchTable->setColumnCount(7);
    m_dispatchTable->setHorizontalHeaderLabels({
        kernelText("kernel.ioctl_audit.header.driver", QStringLiteral("DriverObject")),
        kernelText("kernel.ioctl_audit.header.major", QStringLiteral("MajorFunction")),
        kernelText("kernel.ioctl_audit.header.dispatch", QStringLiteral("派遣地址")),
        kernelText("kernel.ioctl_audit.header.module_base", QStringLiteral("模块基址")),
        kernelText("kernel.ioctl_audit.header.module", QStringLiteral("归属模块")),
        kernelText("kernel.ioctl_audit.header.flags", QStringLiteral("Flags")),
        kernelText("kernel.ioctl_audit.header.status", QStringLiteral("状态"))});
    m_registryTable->setColumnCount(7);
    m_registryTable->setHorizontalHeaderLabels({
        kernelText("kernel.ioctl_audit.header.code", QStringLiteral("控制码")),
        kernelText("kernel.ioctl_audit.header.function", QStringLiteral("Function")),
        kernelText("kernel.ioctl_audit.header.method_access", QStringLiteral("Method/Access")),
        kernelText("kernel.ioctl_audit.header.capability", QStringLiteral("能力门槛")),
        kernelText("kernel.ioctl_audit.header.handler", QStringLiteral("Handler")),
        kernelText("kernel.ioctl_audit.header.name", QStringLiteral("名称")),
        kernelText("kernel.ioctl_audit.header.flags", QStringLiteral("Flags"))});

    auto* dispatchLayout = new QVBoxLayout(m_dispatchPage);
    dispatchLayout->setContentsMargins(2, 2, 2, 2);
    dispatchLayout->addWidget(m_dispatchTable);
    auto* registryLayout = new QVBoxLayout(m_registryPage);
    registryLayout->setContentsMargins(2, 2, 2, 2);
    registryLayout->addWidget(m_registryTable);
    m_innerTabs->addTab(m_dispatchPage, kernelText("kernel.ioctl_audit.tab.dispatch", QStringLiteral("全局 MajorFunction")));
    m_innerTabs->addTab(m_registryPage, kernelText("kernel.ioctl_audit.tab.registry", QStringLiteral("KswordARK IOCTL 注册表")));
    rootLayout->addWidget(m_innerTabs, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refreshAsync(); });
    connect(m_dispatchTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showCopyMenu(m_dispatchTable, position);
    });
    connect(m_registryTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showCopyMenu(m_registryTable, position);
    });
}

void KernelIoctlAuditTab::refreshAsync()
{
    if (m_refreshRunning)
    {
        return;
    }
    m_refreshRunning = true;
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.ioctl_audit.refreshing", QStringLiteral("正在枚举 DriverObject 派遣入口并读取 IOCTL registry...")));
    QPointer<KernelIoctlAuditTab> safeThis(this);
    std::thread([safeThis]() {
        Snapshot snapshot;
        std::vector<KernelDeviceDriverObjectEntry> objectRows;
        QString workerError;
        const bool workerOk = runKernelDeviceDriverObjectsSnapshotTask(objectRows, workerError);
        if (!workerOk)
        {
            snapshot.errorText = workerError;
        }

        ksword::ark::DriverClient client;
        if (workerOk)
        {
            for (const KernelDeviceDriverObjectEntry& objectRow : objectRows)
            {
                if (objectRow.isScopeEntry || objectRow.objectTypeText.compare(QStringLiteral("Driver"), Qt::CaseInsensitive) != 0)
                {
                    continue;
                }
                const std::wstring driverName = objectRow.fullPathText.toStdWString();
                const ksword::ark::DriverObjectQueryResult query = client.queryDriverObject(driverName);
                if (!query.io.ok)
                {
                    snapshot.errorText = query.io.message.empty()
                        ? QStringLiteral("DriverObject 查询失败")
                        : QString::fromStdString(query.io.message);
                    continue;
                }
                for (const ksword::ark::DriverMajorFunctionEntry& major : query.majorFunctions)
                {
                    DispatchRow row;
                    row.driverName = objectRow.fullPathText;
                    row.majorFunction = major.majorFunction;
                    row.dispatchAddress = major.dispatchAddress;
                    row.moduleBase = major.moduleBase;
                    row.moduleName = QString::fromStdWString(major.moduleName);
                    row.flags = major.flags;
                    row.status = QStringLiteral("0x%1").arg(static_cast<unsigned long>(query.lastStatus), 8, 16, QLatin1Char('0'));
                    snapshot.dispatchRows.push_back(std::move(row));
                }
            }
        }

        const ksword::ark::IoctlRegistryQueryResult registry = client.queryIoctlRegistry();
        snapshot.registryOk = registry.io.ok;
        snapshot.registryTotal = registry.totalCount;
        snapshot.registryDuplicate = registry.duplicateCount;
        snapshot.registryRows = registry.entries;
        if (!registry.io.ok && snapshot.errorText.isEmpty())
        {
            snapshot.errorText = registry.io.message.empty()
                ? QStringLiteral("KswordARK IOCTL registry 查询失败")
                : QString::fromStdString(registry.io.message);
        }

        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(safeThis, [safeThis, snapshot = std::move(snapshot)]() mutable {
            if (safeThis != nullptr)
            {
                safeThis->applySnapshot(std::move(snapshot));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelIoctlAuditTab::applySnapshot(Snapshot snapshot)
{
    m_refreshRunning = false;
    m_refreshButton->setEnabled(true);
    m_dispatchRows = std::move(snapshot.dispatchRows);
    m_registryRows = std::move(snapshot.registryRows);
    m_registryTotal = snapshot.registryTotal;
    m_registryDuplicate = snapshot.registryDuplicate;
    m_registryOk = snapshot.registryOk;
    m_errorText = std::move(snapshot.errorText);
    populateTables();
}

QString KernelIoctlAuditTab::hex64(const std::uint64_t value)
{
    return QStringLiteral("0x%1").arg(value, 16, 16, QLatin1Char('0'));
}

QString KernelIoctlAuditTab::hex32(const std::uint32_t value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
}

void KernelIoctlAuditTab::populateTables()
{
    m_dispatchTable->setRowCount(0);
    for (const DispatchRow& row : m_dispatchRows)
    {
        const int tableRow = m_dispatchTable->rowCount();
        m_dispatchTable->insertRow(tableRow);
        m_dispatchTable->setItem(tableRow, 0, readOnlyItem(row.driverName));
        m_dispatchTable->setItem(tableRow, 1, readOnlyItem(QString::number(row.majorFunction)));
        m_dispatchTable->setItem(tableRow, 2, readOnlyItem(hex64(row.dispatchAddress)));
        m_dispatchTable->setItem(tableRow, 3, readOnlyItem(hex64(row.moduleBase)));
        m_dispatchTable->setItem(tableRow, 4, readOnlyItem(row.moduleName));
        m_dispatchTable->setItem(tableRow, 5, readOnlyItem(hex32(row.flags)));
        m_dispatchTable->setItem(tableRow, 6, readOnlyItem(row.status));
    }
    m_registryTable->setRowCount(0);
    for (const ksword::ark::IoctlRegistryEntry& row : m_registryRows)
    {
        const int tableRow = m_registryTable->rowCount();
        m_registryTable->insertRow(tableRow);
        m_registryTable->setItem(tableRow, 0, readOnlyItem(hex32(row.ioControlCode)));
        m_registryTable->setItem(tableRow, 1, readOnlyItem(QString::number(row.functionNumber)));
        m_registryTable->setItem(tableRow, 2, readOnlyItem(QStringLiteral("%1 / %2").arg(row.method).arg(row.access)));
        m_registryTable->setItem(tableRow, 3, readOnlyItem(hex64(row.requiredCapability)));
        m_registryTable->setItem(tableRow, 4, readOnlyItem(hex64(row.handlerAddress)));
        m_registryTable->setItem(tableRow, 5, readOnlyItem(QString::fromStdString(row.name)));
        m_registryTable->setItem(tableRow, 6, readOnlyItem(hex32(row.flags)));
    }
    m_dispatchTable->resizeColumnsToContents();
    m_registryTable->resizeColumnsToContents();
    const QString summary = kernelText("kernel.ioctl_audit.summary", QStringLiteral("全局派遣 %1 行，KswordARK registry %2/%3 行，重复控制码 %4。"))
        .arg(static_cast<qulonglong>(m_dispatchRows.size()))
        .arg(static_cast<qulonglong>(m_registryRows.size()))
        .arg(m_registryTotal)
        .arg(m_registryDuplicate);
    m_statusLabel->setText(m_errorText.isEmpty() ? summary : summary + QStringLiteral(" ") + m_errorText);
}

QString KernelIoctlAuditTab::tableRowText(QTableWidget* table, const int row, const bool includeHeader)
{
    if (table == nullptr || row < 0 || row >= table->rowCount())
    {
        return {};
    }
    QStringList values;
    if (includeHeader)
    {
        for (int column = 0; column < table->columnCount(); ++column)
        {
            values << table->horizontalHeaderItem(column)->text();
        }
    }
    QStringList rowValues;
    for (int column = 0; column < table->columnCount(); ++column)
    {
        rowValues << (table->item(row, column) == nullptr ? QString() : table->item(row, column)->text());
    }
    values << rowValues.join(QLatin1Char('\t'));
    return values.join(QLatin1Char('\n'));
}

void KernelIoctlAuditTab::showCopyMenu(QTableWidget* table, const QPoint& position)
{
    if (table == nullptr)
    {
        return;
    }
    const QModelIndex index = table->indexAt(position);
    const int row = index.isValid() ? index.row() : -1;
    QMenu menu(this);
    QAction* copyRow = menu.addAction(kernelText("kernel.ioctl_audit.copy_row", QStringLiteral("复制当前行")));
    QAction* copyAll = menu.addAction(kernelText("kernel.ioctl_audit.copy_all", QStringLiteral("复制全部行")));
    copyRow->setEnabled(row >= 0);
    copyAll->setEnabled(table->rowCount() > 0);
    QAction* selected = menu.exec(table->viewport()->mapToGlobal(position));
    if (selected == copyRow)
    {
        QApplication::clipboard()->setText(tableRowText(table, row, true));
    }
    else if (selected == copyAll)
    {
        QStringList lines;
        for (int rowIndex = 0; rowIndex < table->rowCount(); ++rowIndex)
        {
            lines << tableRowText(table, rowIndex, rowIndex == 0);
        }
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    }
}
