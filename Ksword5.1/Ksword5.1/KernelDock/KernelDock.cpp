#include "KernelDock.h"

// ============================================================
// KernelDock.cpp
// 作用说明：
// 1) 负责 KernelDock 的 UI 构建与交互；
// 2) 耗时查询通过 KernelDockQueryWorker 在后台线程执行；
// 3) 本文件只处理表格渲染与详情展示，不做底层 Nt 解析。
// ============================================================

#include "KernelDockQueryWorker.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>

#include <thread> // std::thread：执行后台刷新任务。

namespace
{
    // blueButtonStyle：
    // - 作用：统一蓝色按钮样式，保证内核页与全局主题一致。
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:#FFFFFF;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
            "QPushButton:hover{background:%3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // blueInputStyle：
    // - 作用：统一输入框/详情框样式，降低视觉跳变。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit,QPlainTextEdit{border:1px solid #C8DDF4;border-radius:3px;background:#FFFFFF;padding:2px 6px;}"
            "QLineEdit:focus,QPlainTextEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // headerStyle：
    // - 作用：统一表头颜色和字重，突出关键信息列。
    QString headerStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;font-weight:600;}").arg(KswordTheme::PrimaryBlueHex);
    }

    // boolText：
    // - 作用：布尔值统一输出中文“是/否”。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }
}

KernelDock::KernelDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent event;
    info << event << "[KernelDock] 构造开始，初始化内核模块。" << eol;

    initializeUi();
    initializeConnections();

    // 首次进入自动刷新，减少用户第一次点击等待。
    refreshKernelTypeAsync();
    refreshNtQueryAsync();

    kLogEvent finishEvent;
    info << finishEvent << "[KernelDock] 构造完成。" << eol;
}

void KernelDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeKernelTypeTab();
    initializeNtQueryTab();
}

void KernelDock::initializeKernelTypeTab()
{
    m_kernelTypePage = new QWidget(m_tabWidget);
    m_kernelTypeLayout = new QVBoxLayout(m_kernelTypePage);
    m_kernelTypeLayout->setContentsMargins(4, 4, 4, 4);
    m_kernelTypeLayout->setSpacing(6);

    m_kernelTypeToolLayout = new QHBoxLayout();
    m_kernelTypeToolLayout->setContentsMargins(0, 0, 0, 0);
    m_kernelTypeToolLayout->setSpacing(6);

    m_refreshKernelTypeButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_kernelTypePage);
    m_refreshKernelTypeButton->setToolTip(QStringLiteral("刷新内核对象类型"));
    m_refreshKernelTypeButton->setStyleSheet(blueButtonStyle());
    m_refreshKernelTypeButton->setFixedWidth(34);

    m_kernelTypeFilterEdit = new QLineEdit(m_kernelTypePage);
    m_kernelTypeFilterEdit->setPlaceholderText(QStringLiteral("输入类型名或编号过滤"));
    m_kernelTypeFilterEdit->setToolTip(QStringLiteral("根据对象类型名称或编号筛选"));
    m_kernelTypeFilterEdit->setStyleSheet(blueInputStyle());

    m_kernelTypeStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_kernelTypePage);

    m_kernelTypeToolLayout->addWidget(m_refreshKernelTypeButton);
    m_kernelTypeToolLayout->addWidget(m_kernelTypeFilterEdit, 1);
    m_kernelTypeToolLayout->addWidget(m_kernelTypeStatusLabel, 0);
    m_kernelTypeLayout->addLayout(m_kernelTypeToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_kernelTypePage);
    m_kernelTypeLayout->addWidget(splitter, 1);

    m_kernelTypeTable = new QTableWidget(splitter);
    m_kernelTypeTable->setColumnCount(7);
    m_kernelTypeTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类型编号"),
        QStringLiteral("类型名"),
        QStringLiteral("对象数"),
        QStringLiteral("句柄数"),
        QStringLiteral("访问掩码"),
        QStringLiteral("安全要求"),
        QStringLiteral("维护计数")
        });
    m_kernelTypeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_kernelTypeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_kernelTypeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_kernelTypeTable->setAlternatingRowColors(true);
    m_kernelTypeTable->verticalHeader()->setVisible(false);
    m_kernelTypeTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_kernelTypeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_kernelTypeTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    m_kernelTypeDetailEdit = new QPlainTextEdit(splitter);
    m_kernelTypeDetailEdit->setReadOnly(true);
    m_kernelTypeDetailEdit->setStyleSheet(blueInputStyle());
    m_kernelTypeDetailEdit->setPlainText(QStringLiteral("请选择一条类型记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_tabWidget->addTab(m_kernelTypePage, QIcon(":/Icon/process_tree.svg"), QStringLiteral("内核对象类型"));
}

void KernelDock::initializeNtQueryTab()
{
    m_ntQueryPage = new QWidget(m_tabWidget);
    m_ntQueryLayout = new QVBoxLayout(m_ntQueryPage);
    m_ntQueryLayout->setContentsMargins(4, 4, 4, 4);
    m_ntQueryLayout->setSpacing(6);

    m_ntQueryToolLayout = new QHBoxLayout();
    m_ntQueryToolLayout->setContentsMargins(0, 0, 0, 0);
    m_ntQueryToolLayout->setSpacing(6);

    m_refreshNtQueryButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_ntQueryPage);
    m_refreshNtQueryButton->setToolTip(QStringLiteral("刷新 NtQuery 信息"));
    m_refreshNtQueryButton->setStyleSheet(blueButtonStyle());
    m_refreshNtQueryButton->setFixedWidth(34);

    m_ntQueryStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_ntQueryPage);

    m_ntQueryToolLayout->addWidget(m_refreshNtQueryButton);
    m_ntQueryToolLayout->addWidget(m_ntQueryStatusLabel, 1);
    m_ntQueryLayout->addLayout(m_ntQueryToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_ntQueryPage);
    m_ntQueryLayout->addWidget(splitter, 1);

    m_ntQueryTable = new QTableWidget(splitter);
    m_ntQueryTable->setColumnCount(5);
    m_ntQueryTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类别"),
        QStringLiteral("函数"),
        QStringLiteral("查询项"),
        QStringLiteral("状态"),
        QStringLiteral("摘要")
        });
    m_ntQueryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ntQueryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ntQueryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ntQueryTable->setAlternatingRowColors(true);
    m_ntQueryTable->verticalHeader()->setVisible(false);
    m_ntQueryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

    m_ntQueryDetailEdit = new QPlainTextEdit(splitter);
    m_ntQueryDetailEdit->setReadOnly(true);
    m_ntQueryDetailEdit->setStyleSheet(blueInputStyle());
    m_ntQueryDetailEdit->setPlainText(QStringLiteral("请选择一条 NtQuery 结果查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_tabWidget->addTab(m_ntQueryPage, QIcon(":/Icon/process_details.svg"), QStringLiteral("NtQuery信息"));
}

void KernelDock::initializeConnections()
{
    connect(m_refreshKernelTypeButton, &QPushButton::clicked, this, [this]() {
        refreshKernelTypeAsync();
    });
    connect(m_refreshNtQueryButton, &QPushButton::clicked, this, [this]() {
        refreshNtQueryAsync();
    });
    connect(m_kernelTypeFilterEdit, &QLineEdit::textChanged, this, [this](const QString& keywordText) {
        rebuildKernelTypeTable(keywordText.trimmed());
    });
    connect(m_kernelTypeTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showKernelTypeDetailByCurrentRow();
    });
    connect(m_ntQueryTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showNtQueryDetailByCurrentRow();
    });
}

void KernelDock::refreshKernelTypeAsync()
{
    if (m_kernelTypeRefreshRunning.exchange(true))
    {
        kLogEvent event;
        dbg << event << "[KernelDock] 对象类型刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshKernelTypeButton->setEnabled(false);
    m_kernelTypeStatusLabel->setText(QStringLiteral("状态：刷新中..."));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelObjectTypeEntry> resultRows;
        QString errorText;
        const bool success = runKernelTypeSnapshotTask(resultRows, errorText);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_kernelTypeRefreshRunning.store(false);
            guardThis->m_refreshKernelTypeButton->setEnabled(true);

            if (!success)
            {
                guardThis->m_kernelTypeStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_kernelTypeDetailEdit->setPlainText(errorText);
                kLogEvent event;
                err << event << "[KernelDock] 内核对象类型刷新失败: " << errorText.toStdString() << eol;
                return;
            }

            guardThis->m_kernelTypeRows = std::move(resultRows);
            guardThis->rebuildKernelTypeTable(guardThis->m_kernelTypeFilterEdit->text().trimmed());
            guardThis->m_kernelTypeStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项").arg(guardThis->m_kernelTypeRows.size()));

            if (guardThis->m_kernelTypeTable->rowCount() > 0)
            {
                guardThis->m_kernelTypeTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_kernelTypeDetailEdit->setPlainText(QStringLiteral("当前过滤条件下无可见数据。"));
            }

            kLogEvent event;
            info << event << "[KernelDock] 内核对象类型刷新完成，条目数=" << guardThis->m_kernelTypeRows.size() << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::refreshNtQueryAsync()
{
    if (m_ntQueryRefreshRunning.exchange(true))
    {
        kLogEvent event;
        dbg << event << "[KernelDock] NtQuery 刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshNtQueryButton->setEnabled(false);
    m_ntQueryStatusLabel->setText(QStringLiteral("状态：刷新中..."));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelNtQueryResultEntry> resultList;
        QString errorText;
        const bool success = runNtQuerySnapshotTask(resultList, errorText);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, resultList = std::move(resultList)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_ntQueryRefreshRunning.store(false);
            guardThis->m_refreshNtQueryButton->setEnabled(true);

            if (!success)
            {
                guardThis->m_ntQueryStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_ntQueryDetailEdit->setPlainText(errorText);
                kLogEvent event;
                err << event << "[KernelDock] NtQuery 刷新失败: " << errorText.toStdString() << eol;
                return;
            }

            guardThis->m_ntQueryResults = std::move(resultList);
            guardThis->rebuildNtQueryTable();

            int successCount = 0;
            for (const KernelNtQueryResultEntry& entry : guardThis->m_ntQueryResults)
            {
                if (entry.statusCode >= 0)
                {
                    ++successCount;
                }
            }

            guardThis->m_ntQueryStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项，成功 %2 项")
                .arg(guardThis->m_ntQueryResults.size())
                .arg(successCount));

            if (guardThis->m_ntQueryTable->rowCount() > 0)
            {
                guardThis->m_ntQueryTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_ntQueryDetailEdit->setPlainText(QStringLiteral("无可展示结果。"));
            }

            kLogEvent event;
            info << event << "[KernelDock] NtQuery 刷新完成，结果数=" << guardThis->m_ntQueryResults.size() << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildKernelTypeTable(const QString& filterKeyword)
{
    if (m_kernelTypeTable == nullptr)
    {
        return;
    }

    m_kernelTypeTable->setRowCount(0);
    int visibleCount = 0;

    for (std::size_t sourceIndex = 0; sourceIndex < m_kernelTypeRows.size(); ++sourceIndex)
    {
        const KernelObjectTypeEntry& entry = m_kernelTypeRows[sourceIndex];
        const bool matched = filterKeyword.isEmpty()
            || entry.typeNameText.contains(filterKeyword, Qt::CaseInsensitive)
            || QString::number(entry.typeIndex).contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int row = m_kernelTypeTable->rowCount();
        m_kernelTypeTable->insertRow(row);

        auto* indexItem = new QTableWidgetItem(QString::number(entry.typeIndex));
        indexItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* typeItem = new QTableWidgetItem(entry.typeNameText);
        auto* objItem = new QTableWidgetItem(QString::number(entry.totalObjectCount));
        auto* handleItem = new QTableWidgetItem(QString::number(entry.totalHandleCount));
        auto* maskItem = new QTableWidgetItem(QStringLiteral("0x%1").arg(entry.validAccessMask, 0, 16).toUpper());
        auto* securityItem = new QTableWidgetItem(boolText(entry.securityRequired));
        auto* maintainItem = new QTableWidgetItem(boolText(entry.maintainHandleCount));

        indexItem->setFlags(indexItem->flags() & ~Qt::ItemIsEditable);
        typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
        objItem->setFlags(objItem->flags() & ~Qt::ItemIsEditable);
        handleItem->setFlags(handleItem->flags() & ~Qt::ItemIsEditable);
        maskItem->setFlags(maskItem->flags() & ~Qt::ItemIsEditable);
        securityItem->setFlags(securityItem->flags() & ~Qt::ItemIsEditable);
        maintainItem->setFlags(maintainItem->flags() & ~Qt::ItemIsEditable);

        m_kernelTypeTable->setItem(row, 0, indexItem);
        m_kernelTypeTable->setItem(row, 1, typeItem);
        m_kernelTypeTable->setItem(row, 2, objItem);
        m_kernelTypeTable->setItem(row, 3, handleItem);
        m_kernelTypeTable->setItem(row, 4, maskItem);
        m_kernelTypeTable->setItem(row, 5, securityItem);
        m_kernelTypeTable->setItem(row, 6, maintainItem);
        ++visibleCount;
    }

    kLogEvent event;
    dbg << event
        << "[KernelDock] 重建对象类型表，total="
        << m_kernelTypeRows.size()
        << ", visible="
        << visibleCount
        << ", filter="
        << filterKeyword.toStdString()
        << eol;
}

void KernelDock::rebuildNtQueryTable()
{
    if (m_ntQueryTable == nullptr)
    {
        return;
    }

    m_ntQueryTable->setRowCount(0);
    for (std::size_t sourceIndex = 0; sourceIndex < m_ntQueryResults.size(); ++sourceIndex)
    {
        const KernelNtQueryResultEntry& entry = m_ntQueryResults[sourceIndex];
        const int row = m_ntQueryTable->rowCount();
        m_ntQueryTable->insertRow(row);

        auto* categoryItem = new QTableWidgetItem(entry.categoryText);
        categoryItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* functionItem = new QTableWidgetItem(entry.functionNameText);
        auto* queryItem = new QTableWidgetItem(entry.queryItemText);
        auto* statusItem = new QTableWidgetItem(entry.statusText);
        auto* summaryItem = new QTableWidgetItem(entry.summaryText);

        categoryItem->setFlags(categoryItem->flags() & ~Qt::ItemIsEditable);
        functionItem->setFlags(functionItem->flags() & ~Qt::ItemIsEditable);
        queryItem->setFlags(queryItem->flags() & ~Qt::ItemIsEditable);
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        summaryItem->setFlags(summaryItem->flags() & ~Qt::ItemIsEditable);

        if (entry.statusCode < 0)
        {
            statusItem->setForeground(QBrush(QColor(166, 52, 52)));
        }

        m_ntQueryTable->setItem(row, 0, categoryItem);
        m_ntQueryTable->setItem(row, 1, functionItem);
        m_ntQueryTable->setItem(row, 2, queryItem);
        m_ntQueryTable->setItem(row, 3, statusItem);
        m_ntQueryTable->setItem(row, 4, summaryItem);
    }
}

void KernelDock::showKernelTypeDetailByCurrentRow()
{
    if (m_kernelTypeTable == nullptr || m_kernelTypeDetailEdit == nullptr)
    {
        return;
    }

    const int row = m_kernelTypeTable->currentRow();
    if (row < 0)
    {
        m_kernelTypeDetailEdit->setPlainText(QStringLiteral("请选择一条类型记录查看详情。"));
        return;
    }

    QTableWidgetItem* indexItem = m_kernelTypeTable->item(row, 0);
    if (indexItem == nullptr)
    {
        m_kernelTypeDetailEdit->setPlainText(QStringLiteral("当前行无有效数据。"));
        return;
    }

    const std::size_t sourceIndex = static_cast<std::size_t>(indexItem->data(Qt::UserRole).toULongLong());
    if (sourceIndex >= m_kernelTypeRows.size())
    {
        m_kernelTypeDetailEdit->setPlainText(QStringLiteral("索引越界。"));
        return;
    }

    const KernelObjectTypeEntry& entry = m_kernelTypeRows[sourceIndex];
    const QString detailText = QStringLiteral(
        "类型编号: %1\n"
        "类型名称: %2\n"
        "对象总数: %3\n"
        "句柄总数: %4\n"
        "访问掩码: 0x%5\n"
        "安全要求: %6\n"
        "维护句柄计数: %7\n"
        "池类型: %8\n"
        "默认分页池配额: %9\n"
        "默认非分页池配额: %10")
        .arg(entry.typeIndex)
        .arg(entry.typeNameText)
        .arg(entry.totalObjectCount)
        .arg(entry.totalHandleCount)
        .arg(entry.validAccessMask, 0, 16)
        .arg(boolText(entry.securityRequired))
        .arg(boolText(entry.maintainHandleCount))
        .arg(entry.poolType)
        .arg(entry.defaultPagedPoolCharge)
        .arg(entry.defaultNonPagedPoolCharge);
    m_kernelTypeDetailEdit->setPlainText(detailText);
}

void KernelDock::showNtQueryDetailByCurrentRow()
{
    if (m_ntQueryTable == nullptr || m_ntQueryDetailEdit == nullptr)
    {
        return;
    }

    const int row = m_ntQueryTable->currentRow();
    if (row < 0)
    {
        m_ntQueryDetailEdit->setPlainText(QStringLiteral("请选择一条 NtQuery 结果查看详情。"));
        return;
    }

    QTableWidgetItem* categoryItem = m_ntQueryTable->item(row, 0);
    if (categoryItem == nullptr)
    {
        m_ntQueryDetailEdit->setPlainText(QStringLiteral("当前行无有效数据。"));
        return;
    }

    const std::size_t sourceIndex = static_cast<std::size_t>(categoryItem->data(Qt::UserRole).toULongLong());
    if (sourceIndex >= m_ntQueryResults.size())
    {
        m_ntQueryDetailEdit->setPlainText(QStringLiteral("索引越界。"));
        return;
    }

    const KernelNtQueryResultEntry& entry = m_ntQueryResults[sourceIndex];
    const QString detailText = QStringLiteral(
        "类别: %1\n"
        "函数: %2\n"
        "查询项: %3\n"
        "状态: %4\n"
        "摘要: %5\n\n"
        "详细输出:\n%6")
        .arg(entry.categoryText)
        .arg(entry.functionNameText)
        .arg(entry.queryItemText)
        .arg(entry.statusText)
        .arg(entry.summaryText)
        .arg(entry.detailText);
    m_ntQueryDetailEdit->setPlainText(detailText);
}

