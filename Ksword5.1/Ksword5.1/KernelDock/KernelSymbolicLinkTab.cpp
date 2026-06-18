#include "KernelSymbolicLinkTab.h"

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
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace
{
    QString buttonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString inputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %1;border-radius:3px;background:%2;color:%3;padding:3px 6px;}"
            "QLineEdit:focus{border:1px solid %4;}"
            "QTableWidget{border:1px solid %1;border-radius:3px;background:%2;color:%3;gridline-color:%1;}"
            "QTableWidget::item:selected{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString headerStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;padding:4px;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }
}

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
    m_refreshButton->setText(QStringLiteral("刷新"));
    m_refreshButton->setToolTip(QStringLiteral("枚举常见对象目录中的 SymbolicLink。"));
    m_refreshButton->setStyleSheet(buttonStyle());

    m_copyTargetButton = new QPushButton(this);
    m_copyTargetButton->setText(QStringLiteral("复制目标"));
    m_copyTargetButton->setToolTip(QStringLiteral("复制当前选中符号链接的 targetPath。"));
    m_copyTargetButton->setStyleSheet(buttonStyle());

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("过滤目录 / 名称 / 完整路径 / 状态"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setStyleSheet(inputStyle());

    m_targetFilterEdit = new QLineEdit(this);
    m_targetFilterEdit->setPlaceholderText(QStringLiteral("按目标路径 / DOS 候选过滤"));
    m_targetFilterEdit->setClearButtonEnabled(true);
    m_targetFilterEdit->setStyleSheet(inputStyle());

    m_statusLabel = new QLabel(QStringLiteral("状态：等待刷新"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(m_refreshButton, 0);
    toolLayout->addWidget(m_copyTargetButton, 0);
    toolLayout->addWidget(m_filterEdit, 1);
    toolLayout->addWidget(m_targetFilterEdit, 1);
    toolLayout->addWidget(m_statusLabel, 0);
    rootLayout->addLayout(toolLayout);

    m_noteLabel = new QLabel(
        QStringLiteral("说明：SymbolicLink 本身不是可递归容器，本页只解析目标；若目标指向 Directory，后续由目录递归 tab 处理。"),
        this);
    m_noteLabel->setWordWrap(true);
    m_noteLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    rootLayout->addWidget(m_noteLabel, 0);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(static_cast<int>(Column::Count));
    m_table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("sourceDirectory"),
        QStringLiteral("linkName"),
        QStringLiteral("fullPath"),
        QStringLiteral("targetPath"),
        QStringLiteral("dosCandidate"),
        QStringLiteral("statusText")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->horizontalHeader()->setStyleSheet(headerStyle());
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::SourceDirectory), QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::LinkName), QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::FullPath), QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::TargetPath), QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::DosCandidate), QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(Column::StatusText), QHeaderView::ResizeToContents);
    m_table->setStyleSheet(inputStyle());
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
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() {
        applyFilters();
    });
    connect(m_targetFilterEdit, &QLineEdit::textChanged, this, [this]() {
        applyFilters();
    });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
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
    m_statusLabel->setText(QStringLiteral("状态：正在枚举 SymbolicLink..."));

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
    m_refreshing = false;
    m_refreshButton->setEnabled(true);

    if (!ok)
    {
        m_allRows.clear();
        m_visibleRows.clear();
        rebuildTable();
        m_statusLabel->setText(QStringLiteral("状态：失败 | %1").arg(errorText));
        return;
    }

    m_allRows = std::move(rows);
    applyFilters();
}

void KernelSymbolicLinkTab::applyFilters()
{
    const QString keywordText = m_filterEdit != nullptr ? m_filterEdit->text().trimmed() : QString();
    const QString targetKeywordText = m_targetFilterEdit != nullptr ? m_targetFilterEdit->text().trimmed() : QString();
    m_visibleRows.clear();
    m_visibleRows.reserve(m_allRows.size());

    for (const KernelSymbolicLinkEntry& row : m_allRows)
    {
        const QString mergedText = QStringList{
            row.sourceDirectory,
            row.linkName,
            row.fullPath,
            row.statusText
        }.join(QStringLiteral(" | "));
        const QString targetMergedText = QStringList{
            row.targetPath,
            row.dosCandidate
        }.join(QStringLiteral(" | "));

        const bool keywordMatched = keywordText.isEmpty()
            || mergedText.contains(keywordText, Qt::CaseInsensitive)
            || targetMergedText.contains(keywordText, Qt::CaseInsensitive);
        const bool targetMatched = targetKeywordText.isEmpty()
            || targetMergedText.contains(targetKeywordText, Qt::CaseInsensitive);
        if (keywordMatched && targetMatched)
        {
            m_visibleRows.push_back(row);
        }
    }

    rebuildTable();
    m_statusLabel->setText(
        QStringLiteral("状态：显示 %1 / %2")
        .arg(m_visibleRows.size())
        .arg(m_allRows.size()));
}

void KernelSymbolicLinkTab::rebuildTable()
{
    m_table->setSortingEnabled(false);
    m_table->clearContents();
    m_table->setRowCount(static_cast<int>(m_visibleRows.size()));

    for (int rowIndex = 0; rowIndex < static_cast<int>(m_visibleRows.size()); ++rowIndex)
    {
        const KernelSymbolicLinkEntry& row = m_visibleRows[static_cast<std::size_t>(rowIndex)];
        m_table->setItem(rowIndex, static_cast<int>(Column::SourceDirectory), createReadOnlyItem(row.sourceDirectory));
        m_table->setItem(rowIndex, static_cast<int>(Column::LinkName), createReadOnlyItem(row.linkName));
        m_table->setItem(rowIndex, static_cast<int>(Column::FullPath), createReadOnlyItem(row.fullPath));
        m_table->setItem(rowIndex, static_cast<int>(Column::TargetPath), createReadOnlyItem(row.targetPath));
        m_table->setItem(rowIndex, static_cast<int>(Column::DosCandidate), createReadOnlyItem(row.dosCandidate));
        m_table->setItem(rowIndex, static_cast<int>(Column::StatusText), createReadOnlyItem(row.statusText));
    }
}

void KernelSymbolicLinkTab::copyCurrentTarget() const
{
    if (m_table == nullptr || m_table->currentRow() < 0)
    {
        return;
    }
    const int rowIndex = m_table->currentRow();
    if (rowIndex >= static_cast<int>(m_visibleRows.size()))
    {
        return;
    }
    QApplication::clipboard()->setText(m_visibleRows[static_cast<std::size_t>(rowIndex)].targetPath);
}

void KernelSymbolicLinkTab::copyCurrentRow() const
{
    if (m_table == nullptr || m_table->currentRow() < 0)
    {
        return;
    }
    const int rowIndex = m_table->currentRow();
    if (rowIndex >= static_cast<int>(m_visibleRows.size()))
    {
        return;
    }
    QApplication::clipboard()->setText(rowToTsv(m_visibleRows[static_cast<std::size_t>(rowIndex)]));
}

void KernelSymbolicLinkTab::showContextMenu(const QPoint& position)
{
    const QModelIndex index = m_table->indexAt(position);
    if (!index.isValid())
    {
        return;
    }
    m_table->setCurrentCell(index.row(), index.column());

    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* copyCellAction = menu.addAction(QStringLiteral("复制单元格"));
    QAction* copyTargetAction = menu.addAction(QStringLiteral("复制 targetPath"));
    QAction* copyDosAction = menu.addAction(QStringLiteral("复制 dosCandidate"));
    QAction* copyRowAction = menu.addAction(QStringLiteral("复制整行"));
    QAction* filterTargetAction = menu.addAction(QStringLiteral("按此目标路径过滤"));

    const QAction* selectedAction = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (selectedAction == nullptr)
    {
        return;
    }

    const int rowIndex = index.row();
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_visibleRows.size()))
    {
        return;
    }
    const KernelSymbolicLinkEntry& row = m_visibleRows[static_cast<std::size_t>(rowIndex)];

    if (selectedAction == copyCellAction)
    {
        QTableWidgetItem* item = m_table->item(index.row(), index.column());
        QApplication::clipboard()->setText(item != nullptr ? item->text() : QString());
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

QTableWidgetItem* KernelSymbolicLinkTab::createReadOnlyItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setToolTip(text);
    return item;
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
