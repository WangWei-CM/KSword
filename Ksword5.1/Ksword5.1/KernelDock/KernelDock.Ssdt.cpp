#include "KernelDock.h"

#include "KernelDockSsdtWorker.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <thread>

namespace
{
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:%5;border:1px solid %2;border-radius:2px;padding:3px 8px;}"
            "QPushButton:hover{background:%3;color:#FFFFFF;border:1px solid %3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
    }

    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString headerStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString itemSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:#2E8BFF;color:#FFFFFF;}");
    }

    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString safeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString formatAddressHex(const std::uint64_t addressValue)
    {
        return QStringLiteral("0x%1")
            .arg(addressValue, 16, 16, QChar('0'))
            .toUpper();
    }

    enum class SsdtColumn : int
    {
        Index = 0,
        ServiceName,
        ZwAddress,
        ServiceAddress,
        Module,
        Status,
        Count
    };
}

void KernelDock::initializeSsdtTab()
{
    if (m_ssdtPage == nullptr || m_ssdtLayout != nullptr)
    {
        return;
    }

    m_ssdtLayout = new QVBoxLayout(m_ssdtPage);
    m_ssdtLayout->setContentsMargins(4, 4, 4, 4);
    m_ssdtLayout->setSpacing(6);

    m_ssdtToolLayout = new QHBoxLayout();
    m_ssdtToolLayout->setContentsMargins(0, 0, 0, 0);
    m_ssdtToolLayout->setSpacing(6);

    m_refreshSsdtButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_ssdtPage);
    m_refreshSsdtButton->setToolTip(QStringLiteral("刷新 SSDT 遍历结果"));
    m_refreshSsdtButton->setStyleSheet(blueButtonStyle());
    m_refreshSsdtButton->setFixedWidth(34);

    m_ssdtFilterEdit = new QLineEdit(m_ssdtPage);
    m_ssdtFilterEdit->setPlaceholderText(QStringLiteral("按索引/服务名/地址/模块/状态筛选"));
    m_ssdtFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤 SSDT 结果"));
    m_ssdtFilterEdit->setClearButtonEnabled(true);
    m_ssdtFilterEdit->setStyleSheet(blueInputStyle());

    m_ssdtStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_ssdtPage);
    m_ssdtStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_ssdtToolLayout->addWidget(m_refreshSsdtButton, 0);
    m_ssdtToolLayout->addWidget(m_ssdtFilterEdit, 1);
    m_ssdtToolLayout->addWidget(m_ssdtStatusLabel, 0);
    m_ssdtLayout->addLayout(m_ssdtToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_ssdtPage);
    m_ssdtLayout->addWidget(splitter, 1);

    m_ssdtTable = new QTableWidget(splitter);
    m_ssdtTable->setColumnCount(static_cast<int>(SsdtColumn::Count));
    m_ssdtTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("索引"),
        QStringLiteral("服务名"),
        QStringLiteral("Zw导出地址"),
        QStringLiteral("表项地址"),
        QStringLiteral("模块"),
        QStringLiteral("状态")
        });
    m_ssdtTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ssdtTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ssdtTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ssdtTable->setAlternatingRowColors(true);
    m_ssdtTable->setStyleSheet(itemSelectionStyle());
    m_ssdtTable->setCornerButtonEnabled(false);
    m_ssdtTable->verticalHeader()->setVisible(false);
    m_ssdtTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_ssdtTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_ssdtTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(SsdtColumn::ServiceName), QHeaderView::Stretch);
    m_ssdtTable->setColumnWidth(static_cast<int>(SsdtColumn::Index), 90);
    m_ssdtTable->setColumnWidth(static_cast<int>(SsdtColumn::ZwAddress), 180);
    m_ssdtTable->setColumnWidth(static_cast<int>(SsdtColumn::ServiceAddress), 180);
    m_ssdtTable->setColumnWidth(static_cast<int>(SsdtColumn::Module), 150);

    m_ssdtDetailEditor = new CodeEditorWidget(splitter);
    m_ssdtDetailEditor->setReadOnly(true);
    m_ssdtDetailEditor->setText(QStringLiteral("请选择一条 SSDT 记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshSsdtButton, &QPushButton::clicked, this, [this]() {
        refreshSsdtAsync();
    });
    connect(m_ssdtFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildSsdtTable(filterText.trimmed());
    });
    connect(m_ssdtTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showSsdtDetailByCurrentRow();
    });
}

void KernelDock::refreshSsdtAsync()
{
    if (m_ssdtRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] SSDT 刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshSsdtButton->setEnabled(false);
    m_ssdtStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_ssdtStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelSsdtEntry> resultRows;
        QString errorText;
        const bool success = runSsdtSnapshotTask(resultRows, errorText);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_ssdtRefreshRunning.store(false);
            guardThis->m_refreshSsdtButton->setEnabled(true);

            if (!success)
            {
                guardThis->m_ssdtStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_ssdtStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_ssdtDetailEditor->setText(errorText);
                return;
            }

            guardThis->m_ssdtRows = std::move(resultRows);
            guardThis->rebuildSsdtTable(guardThis->m_ssdtFilterEdit->text().trimmed());

            std::size_t unresolvedCount = 0U;
            for (const KernelSsdtEntry& entry : guardThis->m_ssdtRows)
            {
                if (!entry.indexResolved)
                {
                    ++unresolvedCount;
                }
            }

            guardThis->m_ssdtStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项，未解析索引 %2 项")
                .arg(guardThis->m_ssdtRows.size())
                .arg(unresolvedCount));
            guardThis->m_ssdtStatusLabel->setStyleSheet(
                statusLabelStyle(unresolvedCount == 0U ? QStringLiteral("#3A8F3A") : QStringLiteral("#D77A00")));

            if (guardThis->m_ssdtTable->rowCount() > 0)
            {
                guardThis->m_ssdtTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_ssdtDetailEditor->setText(QStringLiteral("当前环境未返回可见 SSDT 条目。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildSsdtTable(const QString& filterKeyword)
{
    if (m_ssdtTable == nullptr)
    {
        return;
    }

    m_ssdtTable->setSortingEnabled(false);
    m_ssdtTable->setRowCount(0);

    for (std::size_t sourceIndex = 0; sourceIndex < m_ssdtRows.size(); ++sourceIndex)
    {
        const KernelSsdtEntry& entry = m_ssdtRows[sourceIndex];
        const QString indexText = entry.indexResolved
            ? QString::number(entry.serviceIndex)
            : QStringLiteral("<未知>");
        const QString zwAddressText = formatAddressHex(entry.zwRoutineAddress);
        const QString serviceAddressText = formatAddressHex(entry.serviceRoutineAddress);

        const bool matched = filterKeyword.isEmpty()
            || indexText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.serviceNameText.contains(filterKeyword, Qt::CaseInsensitive)
            || zwAddressText.contains(filterKeyword, Qt::CaseInsensitive)
            || serviceAddressText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.moduleNameText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.statusText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_ssdtTable->rowCount();
        m_ssdtTable->insertRow(rowIndex);

        auto* indexItem = new QTableWidgetItem(indexText);
        indexItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* serviceNameItem = new QTableWidgetItem(safeText(entry.serviceNameText));
        auto* zwAddressItem = new QTableWidgetItem(zwAddressText);
        auto* serviceAddressItem = new QTableWidgetItem(serviceAddressText);
        auto* moduleItem = new QTableWidgetItem(safeText(entry.moduleNameText));
        auto* statusItem = new QTableWidgetItem(safeText(entry.statusText));

        indexItem->setFlags(indexItem->flags() & ~Qt::ItemIsEditable);
        serviceNameItem->setFlags(serviceNameItem->flags() & ~Qt::ItemIsEditable);
        zwAddressItem->setFlags(zwAddressItem->flags() & ~Qt::ItemIsEditable);
        serviceAddressItem->setFlags(serviceAddressItem->flags() & ~Qt::ItemIsEditable);
        moduleItem->setFlags(moduleItem->flags() & ~Qt::ItemIsEditable);
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);

        if (!entry.indexResolved)
        {
            statusItem->setForeground(QBrush(KswordTheme::WarningAccentColor()));
        }

        m_ssdtTable->setItem(rowIndex, static_cast<int>(SsdtColumn::Index), indexItem);
        m_ssdtTable->setItem(rowIndex, static_cast<int>(SsdtColumn::ServiceName), serviceNameItem);
        m_ssdtTable->setItem(rowIndex, static_cast<int>(SsdtColumn::ZwAddress), zwAddressItem);
        m_ssdtTable->setItem(rowIndex, static_cast<int>(SsdtColumn::ServiceAddress), serviceAddressItem);
        m_ssdtTable->setItem(rowIndex, static_cast<int>(SsdtColumn::Module), moduleItem);
        m_ssdtTable->setItem(rowIndex, static_cast<int>(SsdtColumn::Status), statusItem);
    }

    m_ssdtTable->setSortingEnabled(true);
}

bool KernelDock::currentSsdtSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;

    if (m_ssdtTable == nullptr)
    {
        return false;
    }

    const int currentRow = m_ssdtTable->currentRow();
    if (currentRow < 0)
    {
        return false;
    }

    QTableWidgetItem* indexItem = m_ssdtTable->item(currentRow, static_cast<int>(SsdtColumn::Index));
    if (indexItem == nullptr)
    {
        return false;
    }

    sourceIndexOut = static_cast<std::size_t>(indexItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_ssdtRows.size();
}

const KernelSsdtEntry* KernelDock::currentSsdtEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentSsdtSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_ssdtRows[sourceIndex];
}

void KernelDock::showSsdtDetailByCurrentRow()
{
    if (m_ssdtDetailEditor == nullptr)
    {
        return;
    }

    const KernelSsdtEntry* entry = currentSsdtEntry();
    if (entry == nullptr)
    {
        m_ssdtDetailEditor->setText(QStringLiteral("请选择一条 SSDT 记录查看详情。"));
        return;
    }

    const QString detailText = QStringLiteral(
        "服务索引: %1\n"
        "服务名: %2\n"
        "模块: %3\n"
        "Zw导出地址: %4\n"
        "服务表基址: %5\n"
        "表项服务地址: %6\n"
        "状态: %7\n"
        "标志: 0x%8\n\n"
        "Worker详情:\n%9")
        .arg(entry->indexResolved ? QString::number(entry->serviceIndex) : QStringLiteral("<未知>"))
        .arg(safeText(entry->serviceNameText))
        .arg(safeText(entry->moduleNameText))
        .arg(formatAddressHex(entry->zwRoutineAddress))
        .arg(formatAddressHex(entry->serviceTableBase))
        .arg(formatAddressHex(entry->serviceRoutineAddress))
        .arg(safeText(entry->statusText))
        .arg(static_cast<unsigned int>(entry->flags), 8, 16, QChar('0'))
        .arg(safeText(entry->detailText));

    m_ssdtDetailEditor->setText(detailText);
}
