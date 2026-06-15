#include "ProcessDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/TableColumnAutoFit.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QSize>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <sstream>

namespace
{
    enum class CrossViewColumn : int
    {
        Id = 0,
        Object,
        Process,
        Public,
        ActiveOrThreadList,
        Cid,
        Anomaly,
        Confidence,
        Detail,
        Count
    };

    int columnIndex(const CrossViewColumn column)
    {
        // 输入：Cross-View 表格列枚举。
        // 处理：转换为 Qt 表格列索引。
        // 返回：列号。
        return static_cast<int>(column);
    }

    QString hex64(const std::uint64_t value)
    {
        // 输入：地址或 capability 值。
        // 处理：格式化为 0x 前缀大写十六进制。
        // 返回：展示文本。
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString sourceYesNo(const std::uint32_t sourceMask, const std::uint32_t bit)
    {
        // 输入：来源矩阵 sourceMask 和目标 bit。
        // 处理：映射为勾选文本。
        // 返回：是/否。
        return (sourceMask & bit) ? QStringLiteral("是") : QStringLiteral("-");
    }

    QString anomalyText(const std::uint32_t flags)
    {
        // 输入：KSWORD_ARK_CROSSVIEW_ANOMALY_* 位集合。
        // 处理：转换为短标签，便于 ProcessDock/MonitorDock 使用一致语义。
        // 返回：异常文本；无异常返回“正常”。
        if (flags == 0U)
        {
            return QStringLiteral("正常");
        }
        QStringList parts;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) parts << QStringLiteral("CID-only");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) parts << QStringLiteral("Active-only");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) parts << QStringLiteral("缺 ActiveList");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) parts << QStringLiteral("缺 CID");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) parts << QStringLiteral("孤儿线程");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) parts << QStringLiteral("线程进程缺失");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) parts << QStringLiteral("入口出模块");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) parts << QStringLiteral("悬空对象");
        return parts.join(QStringLiteral(" | "));
    }

    QString narrowToQString(const std::string& value)
    {
        // 输入：ArkDriverClient 的窄字符串。
        // 处理：按 UTF-8 转换，失败场景 Qt 会保留可显示替代字符。
        // 返回：QString。
        return QString::fromStdString(value);
    }

    class NumericItem final : public QTableWidgetItem
    {
    public:
        NumericItem(const QString& text, const qulonglong value)
            : QTableWidgetItem(text)
        {
            // 输入：显示文本和排序数值。
            // 处理：数值写入 UserRole，保留文本原样。
            // 返回：构造函数无返回值。
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(value));
            setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        }

        bool operator<(const QTableWidgetItem& other) const override
        {
            // 输入：另一单元格。
            // 处理：优先按 UserRole 数值排序。
            // 返回：true 表示当前项更小。
            bool leftOk = false;
            bool rightOk = false;
            const qulonglong leftValue = data(Qt::UserRole).toULongLong(&leftOk);
            const qulonglong rightValue = other.data(Qt::UserRole).toULongLong(&rightOk);
            if (leftOk && rightOk)
            {
                return leftValue < rightValue;
            }
            return QTableWidgetItem::operator<(other);
        }
    };

    QTableWidgetItem* textItem(const QString& value)
    {
        // 输入：展示文本。
        // 处理：创建只读单元格项。
        // 返回：交给 QTableWidget 接管生命周期的 item。
        QTableWidgetItem* item = new QTableWidgetItem(value);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        return item;
    }

    QTableWidgetItem* numericItem(const QString& text, const qulonglong value)
    {
        // 输入：展示文本和排序数值。
        // 处理：创建可数值排序单元格。
        // 返回：交给 QTableWidget 接管生命周期的 item。
        return new NumericItem(text, value);
    }

    bool textContainsFilter(const QStringList& fields, const QString& filter)
    {
        // 输入：待匹配字段集合和过滤文本。
        // 处理：大小写不敏感 contains。
        // 返回：true 表示命中过滤。
        if (filter.isEmpty())
        {
            return true;
        }
        for (const QString& field : fields)
        {
            if (field.contains(filter, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

    QString offsetsText(const ksword::ark::CrossViewFieldOffsets& offsets)
    {
        // 输入：R0 返回的 DynData offset 快照。
        // 处理：展开为多行诊断文本。
        // 返回：可复制的偏移说明。
        QString text;
        text += QStringLiteral("EPROCESS.UniqueProcessId: 0x%1\n").arg(offsets.epUniqueProcessId, 8, 16, QChar('0'));
        text += QStringLiteral("EPROCESS.ActiveProcessLinks: 0x%1\n").arg(offsets.epActiveProcessLinks, 8, 16, QChar('0'));
        text += QStringLiteral("EPROCESS.ThreadListHead: 0x%1\n").arg(offsets.epThreadListHead, 8, 16, QChar('0'));
        text += QStringLiteral("EPROCESS.ImageFileName: 0x%1\n").arg(offsets.epImageFileName, 8, 16, QChar('0'));
        text += QStringLiteral("ETHREAD.Cid: 0x%1\n").arg(offsets.etCid, 8, 16, QChar('0'));
        text += QStringLiteral("ETHREAD.ThreadListEntry: 0x%1\n").arg(offsets.etThreadListEntry, 8, 16, QChar('0'));
        text += QStringLiteral("ETHREAD.StartAddress: 0x%1\n").arg(offsets.etStartAddress, 8, 16, QChar('0'));
        text += QStringLiteral("KTHREAD.Process: 0x%1\n").arg(offsets.ktProcess, 8, 16, QChar('0'));
        text += QStringLiteral("PspCidTableRva: 0x%1\n").arg(offsets.pspCidTableRva, 8, 16, QChar('0'));
        text += QStringLiteral("PspCidTableAddress: %1\n").arg(hex64(offsets.pspCidTableAddress));
        return text;
    }
}

void ProcessDock::initializeCrossViewPage()
{
    // 输入：无，由 initializeUi 调用。
    // 处理：创建 Cross-View 页，展示进程和线程来源矩阵。
    // 返回：无。
    m_crossViewPage = new QWidget(this);
    m_crossViewPageLayout = new QVBoxLayout(m_crossViewPage);
    m_crossViewPageLayout->setContentsMargins(6, 6, 6, 6);
    m_crossViewPageLayout->setSpacing(6);

    m_crossViewTopLayout = new QHBoxLayout();
    m_crossViewTopLayout->setContentsMargins(0, 0, 0, 0);
    m_crossViewTopLayout->setSpacing(8);

    m_crossViewRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), m_crossViewPage);
    m_crossViewRefreshButton->setFixedSize(QSize(32, 32));
    m_crossViewRefreshButton->setToolTip(QStringLiteral("查询 R0 Process/Thread Cross-View 证据"));

    m_crossViewSearchEdit = new QLineEdit(m_crossViewPage);
    m_crossViewSearchEdit->setClearButtonEnabled(true);
    m_crossViewSearchEdit->setPlaceholderText(QStringLiteral("过滤 PID/TID/进程名/异常/详情"));

    m_crossViewAnomalyOnlyCheck = new QCheckBox(QStringLiteral("仅异常"), m_crossViewPage);
    m_crossViewAnomalyOnlyCheck->setChecked(true);

    m_crossViewStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_crossViewPage);
    m_crossViewStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_crossViewStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_crossViewTopLayout->addWidget(m_crossViewRefreshButton);
    m_crossViewTopLayout->addWidget(m_crossViewAnomalyOnlyCheck);
    m_crossViewTopLayout->addWidget(m_crossViewSearchEdit, 1);
    m_crossViewTopLayout->addWidget(m_crossViewStatusLabel);
    m_crossViewPageLayout->addLayout(m_crossViewTopLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_crossViewPage);
    m_crossViewPageLayout->addWidget(splitter, 1);

    QTabWidget* innerTabs = new QTabWidget(splitter);
    m_processCrossViewTable = new QTableWidget(innerTabs);
    m_threadCrossViewTable = new QTableWidget(innerTabs);
    for (QTableWidget* table : { m_processCrossViewTable, m_threadCrossViewTable })
    {
        table->setColumnCount(columnIndex(CrossViewColumn::Count));
        table->setHorizontalHeaderLabels(QStringList{
            QStringLiteral("ID"),
            QStringLiteral("对象"),
            QStringLiteral("进程"),
            QStringLiteral("PublicWalk"),
            QStringLiteral("Active/ThreadList"),
            QStringLiteral("CID"),
            QStringLiteral("异常"),
            QStringLiteral("置信度"),
            QStringLiteral("Detail")
            });
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setSortingEnabled(true);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(columnIndex(CrossViewColumn::Detail), QHeaderView::Stretch);
    }
    innerTabs->addTab(m_processCrossViewTable, QStringLiteral("进程矩阵"));
    innerTabs->addTab(m_threadCrossViewTable, QStringLiteral("线程矩阵"));
    splitter->addWidget(innerTabs);

    m_crossViewDetailEdit = new QPlainTextEdit(splitter);
    m_crossViewDetailEdit->setReadOnly(true);
    m_crossViewDetailEdit->setPlaceholderText(QStringLiteral("选择 Cross-View 行查看 source mask、anomaly flags、DynData offsets 和 R0 detail。"));
    splitter->addWidget(m_crossViewDetailEdit);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_sideTabWidget->addTab(m_crossViewPage, blueTintedIcon(":/Icon/process_tree.svg"), QStringLiteral("Cross-View"));
}

void ProcessDock::initializeCrossViewConnections()
{
    // 输入：无，由 initializeConnections 调用。
    // 处理：连接刷新、过滤和选择变化。
    // 返回：无。
    connect(m_crossViewRefreshButton, &QPushButton::clicked, this, [this]() {
        refreshCrossViewAsync();
    });
    connect(m_crossViewSearchEdit, &QLineEdit::textChanged, this, [this]() {
        rebuildCrossViewTables();
    });
    connect(m_crossViewAnomalyOnlyCheck, &QCheckBox::toggled, this, [this]() {
        rebuildCrossViewTables();
    });
    connect(m_processCrossViewTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showCrossViewDetailForCurrentRow(false);
    });
    connect(m_threadCrossViewTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showCrossViewDetailForCurrentRow(true);
    });
}

void ProcessDock::refreshCrossViewAsync()
{
    // 输入：用户刷新动作。
    // 处理：后台同时请求进程和线程 cross-view，主线程回填缓存。
    // 返回：无。
    if (m_crossViewRefreshInProgress)
    {
        return;
    }
    m_crossViewRefreshInProgress = true;
    const std::uint64_t ticket = ++m_crossViewRefreshTicket;
    if (m_crossViewRefreshButton != nullptr)
    {
        m_crossViewRefreshButton->setEnabled(false);
    }
    if (m_crossViewStatusLabel != nullptr)
    {
        m_crossViewStatusLabel->setText(QStringLiteral("状态：查询中..."));
        m_crossViewStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }

    QPointer<ProcessDock> guardThis(this);
    QRunnable* task = QRunnable::create([guardThis, ticket]() {
        const ksword::ark::DriverClient client;
        ksword::ark::ProcessCrossViewResult processResult = client.queryProcessCrossView();
        ksword::ark::ThreadCrossViewResult threadResult = client.queryThreadCrossView();

        QMetaObject::invokeMethod(guardThis.data(), [guardThis, ticket, processResult = std::move(processResult), threadResult = std::move(threadResult)]() mutable {
            if (guardThis == nullptr || guardThis->m_crossViewRefreshTicket != ticket)
            {
                return;
            }
            guardThis->m_crossViewRefreshInProgress = false;
            if (guardThis->m_crossViewRefreshButton != nullptr)
            {
                guardThis->m_crossViewRefreshButton->setEnabled(true);
            }

            guardThis->m_lastProcessCrossViewResult = processResult;
            guardThis->m_lastThreadCrossViewResult = threadResult;
            guardThis->m_processCrossViewCache = processResult.entries;
            guardThis->m_threadCrossViewCache = threadResult.entries;
            guardThis->rebuildCrossViewTables();

            QString statusText;
            if (!processResult.io.ok || !threadResult.io.ok)
            {
                statusText = QStringLiteral("状态：%1 / %2")
                    .arg(processResult.unsupported ? QStringLiteral("进程未集成/驱动过旧") : QString::fromStdString(processResult.io.message))
                    .arg(threadResult.unsupported ? QStringLiteral("线程未集成/驱动过旧") : QString::fromStdString(threadResult.io.message));
                guardThis->m_crossViewStatusLabel->setStyleSheet(QStringLiteral("color:#B23A3A; font-weight:700;"));
            }
            else
            {
                statusText = QStringLiteral("状态：进程 %1/%2，线程 %3/%4，missingCaps=0x%5/0x%6")
                    .arg(processResult.entries.size())
                    .arg(processResult.totalCount)
                    .arg(threadResult.entries.size())
                    .arg(threadResult.totalCount)
                    .arg(static_cast<qulonglong>(processResult.missingCapabilityMask), 0, 16)
                    .arg(static_cast<qulonglong>(threadResult.missingCapabilityMask), 0, 16);
                guardThis->m_crossViewStatusLabel->setStyleSheet(QStringLiteral("color:#2F7D32; font-weight:700;"));
            }
            guardThis->m_crossViewStatusLabel->setText(statusText);
            guardThis->showCrossViewDetailForCurrentRow(false);
        }, Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void ProcessDock::rebuildCrossViewTables()
{
    // 输入：无，读取 cross-view 缓存和过滤控件。
    // 处理：分别重绘进程/线程来源矩阵。
    // 返回：无。
    const QString filter = m_crossViewSearchEdit != nullptr ? m_crossViewSearchEdit->text().trimmed() : QString();
    const bool anomalyOnly = m_crossViewAnomalyOnlyCheck != nullptr && m_crossViewAnomalyOnlyCheck->isChecked();

    if (m_processCrossViewTable != nullptr)
    {
        QSignalBlocker blocker(m_processCrossViewTable);
        m_processCrossViewTable->setSortingEnabled(false);
        std::vector<std::size_t> indexes;
        for (std::size_t index = 0; index < m_processCrossViewCache.size(); ++index)
        {
            const auto& row = m_processCrossViewCache[index];
            if (anomalyOnly && row.anomalyFlags == 0U)
            {
                continue;
            }
            if (!textContainsFilter({
                QString::number(row.processId),
                narrowToQString(row.imageName),
                hex64(row.objectAddress),
                anomalyText(row.anomalyFlags),
                narrowToQString(row.detail)
                }, filter))
            {
                continue;
            }
            indexes.push_back(index);
        }
        m_processCrossViewTable->setRowCount(static_cast<int>(indexes.size()));
        for (int tableRow = 0; tableRow < static_cast<int>(indexes.size()); ++tableRow)
        {
            const std::size_t cacheIndex = indexes[static_cast<std::size_t>(tableRow)];
            const auto& row = m_processCrossViewCache[cacheIndex];
            QTableWidgetItem* idItem = numericItem(QString::number(row.processId), row.processId);
            idItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Id), idItem);
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Object), numericItem(hex64(row.objectAddress), row.objectAddress));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Process), textItem(narrowToQString(row.imageName)));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Public), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK)));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::ActiveOrThreadList), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST)));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Cid), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE)));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Anomaly), textItem(anomalyText(row.anomalyFlags)));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Confidence), numericItem(QString::number(row.confidence), row.confidence));
            m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Detail), textItem(narrowToQString(row.detail)));
        }
        if (m_processCrossViewTable->rowCount() > 0 && m_processCrossViewTable->currentRow() < 0)
        {
            m_processCrossViewTable->setCurrentCell(0, columnIndex(CrossViewColumn::Id));
        }
        m_processCrossViewTable->setSortingEnabled(true);
        ks::ui::RequestTableColumnAutoFit(m_processCrossViewTable);
    }

    if (m_threadCrossViewTable != nullptr)
    {
        QSignalBlocker blocker(m_threadCrossViewTable);
        m_threadCrossViewTable->setSortingEnabled(false);
        std::vector<std::size_t> indexes;
        for (std::size_t index = 0; index < m_threadCrossViewCache.size(); ++index)
        {
            const auto& row = m_threadCrossViewCache[index];
            if (anomalyOnly && row.anomalyFlags == 0U)
            {
                continue;
            }
            if (!textContainsFilter({
                QString::number(row.threadId),
                QString::number(row.processId),
                narrowToQString(row.imageName),
                hex64(row.objectAddress),
                anomalyText(row.anomalyFlags),
                narrowToQString(row.detail)
                }, filter))
            {
                continue;
            }
            indexes.push_back(index);
        }
        m_threadCrossViewTable->setRowCount(static_cast<int>(indexes.size()));
        for (int tableRow = 0; tableRow < static_cast<int>(indexes.size()); ++tableRow)
        {
            const std::size_t cacheIndex = indexes[static_cast<std::size_t>(tableRow)];
            const auto& row = m_threadCrossViewCache[cacheIndex];
            QTableWidgetItem* idItem = numericItem(QString::number(row.threadId), row.threadId);
            idItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Id), idItem);
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Object), numericItem(hex64(row.objectAddress), row.objectAddress));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Process), textItem(QStringLiteral("%1 %2").arg(row.processId).arg(narrowToQString(row.imageName))));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Public), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK)));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::ActiveOrThreadList), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_THREAD_LIST)));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Cid), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE)));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Anomaly), textItem(anomalyText(row.anomalyFlags)));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Confidence), numericItem(QString::number(row.confidence), row.confidence));
            m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Detail), textItem(narrowToQString(row.detail)));
        }
        if (m_threadCrossViewTable->rowCount() > 0 && m_threadCrossViewTable->currentRow() < 0)
        {
            m_threadCrossViewTable->setCurrentCell(0, columnIndex(CrossViewColumn::Id));
        }
        m_threadCrossViewTable->setSortingEnabled(true);
        ks::ui::RequestTableColumnAutoFit(m_threadCrossViewTable);
    }
}

void ProcessDock::showCrossViewDetailForCurrentRow(const bool preferThreadTable)
{
    // 输入：preferThreadTable 指示优先读取线程表还是进程表。
    // 处理：展开当前行 source/anomaly/DynData 细节到只读文本框。
    // 返回：无。
    if (m_crossViewDetailEdit == nullptr)
    {
        return;
    }

    if (preferThreadTable && m_threadCrossViewTable != nullptr && m_threadCrossViewTable->currentRow() >= 0)
    {
        const QTableWidgetItem* item = m_threadCrossViewTable->item(m_threadCrossViewTable->currentRow(), columnIndex(CrossViewColumn::Id));
        bool ok = false;
        const qulonglong cacheIndex = item != nullptr ? item->data(Qt::UserRole + 1).toULongLong(&ok) : 0ULL;
        if (ok && cacheIndex < static_cast<qulonglong>(m_threadCrossViewCache.size()))
        {
            const auto& row = m_threadCrossViewCache[static_cast<std::size_t>(cacheIndex)];
            QString text;
            text += QStringLiteral("线程 Cross-View 详情\n");
            text += QStringLiteral("TID: %1 PID: %2 Image: %3\n").arg(row.threadId).arg(row.processId).arg(narrowToQString(row.imageName));
            text += QStringLiteral("ThreadObject: %1\nProcessObject: %2\nStartAddress: %3\n")
                .arg(hex64(row.objectAddress), hex64(row.processObjectAddress), hex64(row.startAddress));
            text += QStringLiteral("SourceMask: 0x%1\nAnomalyFlags: %2 (0x%3)\n")
                .arg(row.sourceMask, 8, 16, QChar('0'))
                .arg(anomalyText(row.anomalyFlags))
                .arg(row.anomalyFlags, 8, 16, QChar('0'));
            text += QStringLiteral("DynDataCapabilityMask: %1\n").arg(hex64(row.dynDataCapabilityMask));
            text += offsetsText(row.fieldOffsets);
            text += QStringLiteral("LastStatus: 0x%1\nConfidence: %2\nDetail: %3\n")
                .arg(static_cast<qulonglong>(static_cast<unsigned long>(row.lastStatus)), 8, 16, QChar('0'))
                .arg(row.confidence)
                .arg(narrowToQString(row.detail));
            m_crossViewDetailEdit->setPlainText(text);
            return;
        }
    }

    if (m_processCrossViewTable != nullptr && m_processCrossViewTable->currentRow() >= 0)
    {
        const QTableWidgetItem* item = m_processCrossViewTable->item(m_processCrossViewTable->currentRow(), columnIndex(CrossViewColumn::Id));
        bool ok = false;
        const qulonglong cacheIndex = item != nullptr ? item->data(Qt::UserRole + 1).toULongLong(&ok) : 0ULL;
        if (ok && cacheIndex < static_cast<qulonglong>(m_processCrossViewCache.size()))
        {
            const auto& row = m_processCrossViewCache[static_cast<std::size_t>(cacheIndex)];
            QString text;
            text += QStringLiteral("进程 Cross-View 详情\n");
            text += QStringLiteral("PID: %1 PPID: %2 Image: %3\n").arg(row.processId).arg(row.parentProcessId).arg(narrowToQString(row.imageName));
            text += QStringLiteral("ProcessObject: %1\nStartAddress: %2\n")
                .arg(hex64(row.objectAddress), hex64(row.startAddress));
            text += QStringLiteral("SourceMask: 0x%1\nAnomalyFlags: %2 (0x%3)\n")
                .arg(row.sourceMask, 8, 16, QChar('0'))
                .arg(anomalyText(row.anomalyFlags))
                .arg(row.anomalyFlags, 8, 16, QChar('0'));
            text += QStringLiteral("DynDataCapabilityMask: %1\n").arg(hex64(row.dynDataCapabilityMask));
            text += offsetsText(row.fieldOffsets);
            text += QStringLiteral("LastStatus: 0x%1\nConfidence: %2\nDetail: %3\n")
                .arg(static_cast<qulonglong>(static_cast<unsigned long>(row.lastStatus)), 8, 16, QChar('0'))
                .arg(row.confidence)
                .arg(narrowToQString(row.detail));
            m_crossViewDetailEdit->setPlainText(text);
            return;
        }
    }

    m_crossViewDetailEdit->setPlainText(QStringLiteral("请选择一条 Cross-View 记录查看详情。"));
}
