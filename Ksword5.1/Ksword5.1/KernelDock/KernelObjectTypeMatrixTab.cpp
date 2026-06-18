#include "KernelObjectTypeMatrixTab.h"

// ============================================================
// KernelObjectTypeMatrixTab.cpp
// 作用说明：
// 1) 展示 NtQueryObject(ObjectTypesInformation) 返回的对象类型统计；
// 2) 为每类对象补充“是否可继续枚举/应使用什么 API”的策略提示；
// 3) 页面独立异步刷新，便于挂入对象命名空间二级 Tab。
// ============================================================

#include "KernelDockQueryWorker.h"
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
    m_refreshButton->setToolTip(QStringLiteral("刷新对象类型统计"));
    m_refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("按类型名、编号、策略筛选"));
    m_filterEdit->setClearButtonEnabled(true);

    m_statusLabel = new QLabel(QStringLiteral("状态：等待刷新"), this);
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    toolbarLayout->addWidget(m_refreshButton, 0);
    toolbarLayout->addWidget(m_filterEdit, 1);
    toolbarLayout->addWidget(m_statusLabel, 0);
    rootLayout->addLayout(toolbarLayout);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(static_cast<int>(ObjectTypeMatrixColumn::Count));
    m_table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类型编号"),
        QStringLiteral("类型名"),
        QStringLiteral("对象数"),
        QStringLiteral("句柄数"),
        QStringLiteral("访问掩码"),
        QStringLiteral("枚举策略")
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
    rootLayout->addWidget(m_table, 1);
}

void KernelObjectTypeMatrixTab::initializeConnections()
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

void KernelObjectTypeMatrixTab::refreshAsync()
{
    if (m_refreshing.exchange(true))
    {
        return;
    }

    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("状态：刷新中..."));
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
        m_table->setRowCount(0);
        m_statusLabel->setText(QStringLiteral("状态：刷新失败 - %1").arg(errorText));
        m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
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
    m_statusLabel->setText(QStringLiteral("状态：已加载 %1 类，显示 %2 类")
        .arg(static_cast<qulonglong>(m_rows.size()))
        .arg(static_cast<qulonglong>(visibleCount)));
    m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#3A8F3A")));
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
    QAction* copyRowAction = menu.addAction(QStringLiteral("复制当前行"));
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
        return QStringLiteral("NtQueryDirectoryObject 可递归枚举子对象");
    }
    if (typeNameText.compare(QStringLiteral("SymbolicLink"), Qt::CaseInsensitive) == 0)
    {
        return QStringLiteral("NtQuerySymbolicLinkObject 可解析目标");
    }
    if (typeNameText.compare(QStringLiteral("File"), Qt::CaseInsensitive) == 0
        || typeNameText.compare(QStringLiteral("NamedPipe"), Qt::CaseInsensitive) == 0)
    {
        return QStringLiteral("文件系统目录枚举，NamedPipe 走 NPFS/NtQueryDirectoryFile");
    }
    if (typeNameText.contains(QStringLiteral("Port"), Qt::CaseInsensitive))
    {
        return QStringLiteral("通信端点对象，通常是叶子；可做命名空间聚合");
    }
    if (typeNameText.compare(QStringLiteral("Device"), Qt::CaseInsensitive) == 0
        || typeNameText.compare(QStringLiteral("Driver"), Qt::CaseInsensitive) == 0)
    {
        return QStringLiteral("通常是叶子对象；可做专项属性/目录聚合");
    }
    return QStringLiteral("通常不可下钻；按类型查询属性或在专项页聚合");
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
