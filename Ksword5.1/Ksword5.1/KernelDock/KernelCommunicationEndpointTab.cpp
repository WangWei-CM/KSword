#include "KernelCommunicationEndpointTab.h"

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
#include <QVBoxLayout>

#include <algorithm>
#include <thread>
#include <utility>

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
    m_refreshButton->setFixedWidth(34);
    m_refreshButton->setToolTip(QStringLiteral("刷新通信端点对象"));
    m_refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("按名称、类型、路径筛选"));
    m_filterEdit->setClearButtonEnabled(true);

    m_statusLabel = new QLabel(QStringLiteral("状态：等待刷新"), this);
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    toolbarLayout->addWidget(m_refreshButton, 0);
    toolbarLayout->addWidget(m_filterEdit, 1);
    toolbarLayout->addWidget(m_statusLabel, 0);
    rootLayout->addLayout(toolbarLayout);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(static_cast<int>(CommunicationEndpointColumn::Count));
    m_table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("来源目录"),
        QStringLiteral("名称"),
        QStringLiteral("类型"),
        QStringLiteral("完整路径"),
        QStringLiteral("状态")
        });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setStyleSheet(QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}").arg(KswordTheme::PrimaryBlueHex));
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
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() {
        rebuildTable();
    });
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
    m_statusLabel->setText(QStringLiteral("状态：刷新中..."));
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
            errorText += QStringLiteral("\\ 枚举失败：%1\n").arg(rootResult.errorText);
        }
        if (!rpcResult.success)
        {
            errorText += QStringLiteral("\\RPC Control 枚举失败：%1\n").arg(rpcResult.errorText);
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
    m_refreshing.store(false);
    m_refreshButton->setEnabled(true);
    m_rows = std::move(rows);
    rebuildTable();

    if (!success)
    {
        m_statusLabel->setText(QStringLiteral("状态：刷新失败 - %1").arg(errorText));
        m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
    }
}

void KernelCommunicationEndpointTab::rebuildTable()
{
    if (m_table == nullptr)
    {
        return;
    }

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    std::size_t visibleCount = 0;
    for (const KernelObjectDirectoryDeepEntry& entry : m_rows)
    {
        if (!rowMatchesFilter(entry))
        {
            continue;
        }

        const int rowIndex = m_table->rowCount();
        m_table->insertRow(rowIndex);
        m_table->setItem(rowIndex, static_cast<int>(CommunicationEndpointColumn::Source), readOnlyItem(entry.directoryPath));
        m_table->setItem(rowIndex, static_cast<int>(CommunicationEndpointColumn::Name), readOnlyItem(entry.objectName));
        m_table->setItem(rowIndex, static_cast<int>(CommunicationEndpointColumn::Type), readOnlyItem(entry.objectType));
        m_table->setItem(rowIndex, static_cast<int>(CommunicationEndpointColumn::FullPath), readOnlyItem(entry.fullPath));
        m_table->setItem(rowIndex, static_cast<int>(CommunicationEndpointColumn::Status), readOnlyItem(entry.statusText));
        ++visibleCount;
    }

    m_table->setSortingEnabled(true);
    m_statusLabel->setText(QStringLiteral("状态：已加载 %1 项，显示 %2 项")
        .arg(static_cast<qulonglong>(m_rows.size()))
        .arg(static_cast<qulonglong>(visibleCount)));
    m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#3A8F3A")));
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
    QAction* copyRowAction = menu.addAction(QStringLiteral("复制当前行"));
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

bool KernelCommunicationEndpointTab::rowMatchesFilter(const KernelObjectDirectoryDeepEntry& entry) const
{
    const QString keyword = m_filterEdit != nullptr ? m_filterEdit->text().trimmed() : QString();
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
