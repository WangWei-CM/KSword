#include "FileHandleUsageWindow.h"

// ============================================================
// FileHandleUsageWindow.cpp
// 作用：
// - 实现占用句柄结果窗口 UI；
// - 实现异步刷新、右键菜单与进程详情跳转；
// - 保持主线程仅负责渲染，扫描任务放在线程池。
// ============================================================

#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <utility>

namespace
{
    // buildBlueButtonStyle 作用：生成统一蓝色按钮样式（图标按钮紧凑尺寸）。
    QString buildBlueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{"
            "  color:%1;"
            "  background:%5;"
            "  border:1px solid %2;"
            "  border-radius:3px;"
            "  padding:4px;"
            "}"
            "QPushButton:hover{"
            "  background:%3;"
            "  color:#FFFFFF;"
            "  border:1px solid %3;"
            "}"
            "QPushButton:pressed{"
            "  background:%4;"
            "  color:#FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
    }

    // formatHex 作用：把整数转为 0x 前缀十六进制文本。
    QString formatHex(const std::uint64_t value, const int width = 0)
    {
        if (width > 0)
        {
            return QStringLiteral("0x%1")
                .arg(static_cast<qulonglong>(value), width, 16, QChar('0'))
                .toUpper();
        }
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 0, 16)
            .toUpper();
    }

}

FileHandleUsageWindow::FileHandleUsageWindow(const std::vector<QString>& targetPaths, QWidget* parent)
    : QDialog(parent)
    , m_targetPaths(targetPaths)
{
    initializeUi();
    initializeConnections();
    requestRefresh(true);
}

void FileHandleUsageWindow::setOpenProcessDetailCallback(OpenProcessDetailCallback callback)
{
    m_openProcessDetailCallback = std::move(callback);
}

void FileHandleUsageWindow::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    applyAdaptiveColumnWidths();
}

void FileHandleUsageWindow::initializeUi()
{
    setWindowTitle(QStringLiteral("占用句柄扫描结果"));
    setMinimumSize(1100, 680);

    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(8, 8, 8, 8);
    m_rootLayout->setSpacing(6);

    m_toolbarLayout = new QHBoxLayout();
    m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolbarLayout->setSpacing(6);

    // 刷新按钮：图标化并通过 tooltip 解释。
    m_refreshButton = new QPushButton(this);
    m_refreshButton->setIcon(QIcon(":/Icon/handle_refresh.svg"));
    m_refreshButton->setIconSize(QSize(16, 16));
    m_refreshButton->setFixedSize(28, 28);
    m_refreshButton->setToolTip(QStringLiteral("刷新扫描结果"));
    m_refreshButton->setStyleSheet(buildBlueButtonStyle());

    // 转到进程详情按钮：图标化并通过 tooltip 解释。
    m_openProcessButton = new QPushButton(this);
    m_openProcessButton->setIcon(QIcon(":/Icon/process_details.svg"));
    m_openProcessButton->setIconSize(QSize(16, 16));
    m_openProcessButton->setFixedSize(28, 28);
    m_openProcessButton->setToolTip(QStringLiteral("转到当前行的进程详细信息"));
    m_openProcessButton->setStyleSheet(buildBlueButtonStyle());

    QStringList pathTextList;
    for (const QString& pathText : m_targetPaths)
    {
        pathTextList.push_back(QDir::toNativeSeparators(pathText));
    }
    m_targetLabel = new QLabel(QStringLiteral("目标：%1").arg(pathTextList.join(QStringLiteral(" | "))), this);
    m_targetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_targetLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextPrimaryHex()));

    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_openProcessButton);
    m_toolbarLayout->addWidget(m_targetLabel, 1);

    m_statusLabel = new QLabel(QStringLiteral("● 等待扫描"), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_resultTable = new QTreeWidget(this);
    m_resultTable->setColumnCount(static_cast<int>(TableColumn::Count));
    m_resultTable->setHeaderLabels(QStringList{
        QStringLiteral("PID"),
        QStringLiteral("进程名"),
        QStringLiteral("句柄"),
        QStringLiteral("类型"),
        QStringLiteral("对象名"),
        QStringLiteral("访问掩码"),
        QStringLiteral("命中目标"),
        QStringLiteral("命中规则"),
        QStringLiteral("进程路径")
        });
    m_resultTable->setRootIsDecorated(false);
    m_resultTable->setItemsExpandable(false);
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setSortingEnabled(true);
    m_resultTable->setContextMenuPolicy(Qt::CustomContextMenu);

    if (m_resultTable->header() != nullptr)
    {
        m_resultTable->header()->setSectionResizeMode(QHeaderView::Interactive);
        m_resultTable->header()->setStretchLastSection(false);
    }

    m_rootLayout->addLayout(m_toolbarLayout);
    m_rootLayout->addWidget(m_statusLabel);
    m_rootLayout->addWidget(m_resultTable, 1);
    applyAdaptiveColumnWidths();
}

void FileHandleUsageWindow::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
        {
            requestRefresh(true);
        });

    connect(m_openProcessButton, &QPushButton::clicked, this, [this]()
        {
            openCurrentProcessDetail();
        });

    connect(m_resultTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition)
        {
            showTableContextMenu(localPosition);
        });
}

void FileHandleUsageWindow::requestRefresh(const bool forceRefresh)
{
    if (m_refreshInProgress)
    {
        if (forceRefresh)
        {
            m_refreshPending = true;
        }
        return;
    }

    const std::uint64_t currentTicket = ++m_refreshTicket;
    m_refreshInProgress = true;
    m_statusLabel->setText(QStringLiteral("● 正在扫描占用句柄..."));
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:700;").arg(KswordTheme::PrimaryBlueHex));

    if (m_refreshProgressPid <= 0)
    {
        m_refreshProgressPid = kPro.add("文件句柄扫描", "准备扫描目标占用句柄");
    }
    kPro.set(m_refreshProgressPid, "后台扫描中", 0, 20.0f);

    kLogEvent refreshEvent;
    info << refreshEvent
        << "[FileHandleUsageWindow] requestRefresh: ticket="
        << currentTicket
        << ", targetCount="
        << m_targetPaths.size()
        << eol;

    const std::vector<QString> targetPathsSnapshot = m_targetPaths;
    const int progressPid = m_refreshProgressPid;
    QPointer<FileHandleUsageWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, currentTicket, targetPathsSnapshot, progressPid]()
        {
            const filedock::handleusage::HandleUsageScanResult refreshResult =
                filedock::handleusage::scanHandleUsageByPaths(targetPathsSnapshot, progressPid);
            if (guardThis == nullptr)
            {
                return;
            }

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, currentTicket, refreshResult]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->applyRefreshResult(currentTicket, refreshResult);
                },
                Qt::QueuedConnection);
        });
    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void FileHandleUsageWindow::applyRefreshResult(
    const std::uint64_t refreshTicket,
    const filedock::handleusage::HandleUsageScanResult& refreshResult)
{
    if (refreshTicket < m_refreshTicket)
    {
        return;
    }

    m_entries = refreshResult.entries;
    rebuildTable(m_entries);

    m_refreshInProgress = false;
    kPro.set(m_refreshProgressPid, "扫描完成", 0, 100.0f);

    QString statusText = QStringLiteral("● 扫描完成 %1 ms | 总句柄:%2 | 文件句柄命中:%3 | 总命中:%4")
        .arg(refreshResult.elapsedMs)
        .arg(refreshResult.totalHandleCount)
        .arg(refreshResult.fileLikeHandleCount)
        .arg(refreshResult.matchedHandleCount);
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QStringLiteral(" | %1").arg(refreshResult.diagnosticText);
    }
    m_statusLabel->setText(statusText);

    const bool hasDiagnostic = !refreshResult.diagnosticText.trimmed().isEmpty();
    m_statusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
        .arg(hasDiagnostic ? QStringLiteral("#D77A00") : QStringLiteral("#3A8F3A")));

    kLogEvent doneEvent;
    info << doneEvent
        << "[FileHandleUsageWindow] applyRefreshResult: ticket="
        << refreshTicket
        << ", total="
        << refreshResult.totalHandleCount
        << ", fileLike="
        << refreshResult.fileLikeHandleCount
        << ", matched="
        << refreshResult.matchedHandleCount
        << ", elapsedMs="
        << refreshResult.elapsedMs
        << eol;

    if (m_refreshPending)
    {
        m_refreshPending = false;
        QMetaObject::invokeMethod(this, [this]()
            {
                requestRefresh(true);
            }, Qt::QueuedConnection);
    }
}

void FileHandleUsageWindow::rebuildTable(const std::vector<filedock::handleusage::HandleUsageEntry>& entries)
{
    m_resultTable->setSortingEnabled(false);
    m_resultTable->clear();

    for (std::size_t rowIndex = 0; rowIndex < entries.size(); ++rowIndex)
    {
        const filedock::handleusage::HandleUsageEntry& entry = entries[rowIndex];
        auto* item = new QTreeWidgetItem();
        item->setText(static_cast<int>(TableColumn::ProcessId), QString::number(entry.processId));
        item->setText(static_cast<int>(TableColumn::ProcessName), entry.processName);
        item->setText(
            static_cast<int>(TableColumn::HandleValue),
            entry.handleValue == 0 ? QStringLiteral("-") : formatHex(entry.handleValue, 0));
        item->setText(
            static_cast<int>(TableColumn::TypeName),
            entry.typeIndex == 0
            ? entry.typeName
            : QStringLiteral("%1 (%2)").arg(entry.typeName).arg(entry.typeIndex));
        item->setText(static_cast<int>(TableColumn::ObjectName), entry.objectName);
        item->setText(
            static_cast<int>(TableColumn::AccessMask),
            entry.grantedAccess == 0 ? QStringLiteral("-") : formatHex(entry.grantedAccess, 8));
        item->setText(static_cast<int>(TableColumn::MatchPath), entry.matchedTargetPath);
        item->setText(
            static_cast<int>(TableColumn::MatchRule),
            entry.matchRuleText.trimmed().isEmpty()
            ? (entry.matchedByDirectoryRule ? QStringLiteral("目录前缀") : QStringLiteral("精确"))
            : entry.matchRuleText);
        item->setText(static_cast<int>(TableColumn::ProcessPath), entry.processImagePath);
        item->setData(static_cast<int>(TableColumn::ProcessId), Qt::UserRole, static_cast<qulonglong>(rowIndex));
        m_resultTable->addTopLevelItem(item);
    }

    if (m_resultTable->topLevelItemCount() > 0)
    {
        m_resultTable->setCurrentItem(m_resultTable->topLevelItem(0));
    }

    applyAdaptiveColumnWidths();
    m_resultTable->setSortingEnabled(true);
}

void FileHandleUsageWindow::applyAdaptiveColumnWidths()
{
    if (m_resultTable == nullptr || m_resultTable->header() == nullptr)
    {
        return;
    }

    QHeaderView* header = m_resultTable->header();
    header->setSectionResizeMode(QHeaderView::Interactive);

    const int viewportWidth = m_resultTable->viewport()->width();
    if (viewportWidth <= 0)
    {
        return;
    }

    const int pidWidth = 82;
    const int processNameWidth = 150;
    const int handleWidth = 110;
    const int typeWidth = 140;
    const int accessWidth = 120;
    const int ruleWidth = 150;

    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ProcessId), pidWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ProcessName), processNameWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::HandleValue), handleWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::TypeName), typeWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::AccessMask), accessWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::MatchRule), ruleWidth);

    const int fixedWidth =
        pidWidth + processNameWidth + handleWidth + typeWidth + accessWidth + ruleWidth;
    const int flexibleWidth = std::max(420, viewportWidth - fixedWidth - 24);

    const int objectNameWidth = std::max(240, static_cast<int>(flexibleWidth * 0.40));
    const int matchPathWidth = std::max(220, static_cast<int>(flexibleWidth * 0.26));
    const int processPathWidth = std::max(220, flexibleWidth - objectNameWidth - matchPathWidth);

    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ObjectName), objectNameWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::MatchPath), matchPathWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ProcessPath), processPathWidth);
}

const filedock::handleusage::HandleUsageEntry* FileHandleUsageWindow::selectedEntry() const
{
    if (m_resultTable == nullptr || m_resultTable->currentItem() == nullptr)
    {
        return nullptr;
    }

    const QVariant rowIndexValue =
        m_resultTable->currentItem()->data(static_cast<int>(TableColumn::ProcessId), Qt::UserRole);
    if (!rowIndexValue.isValid())
    {
        return nullptr;
    }
    const std::size_t rowIndex = static_cast<std::size_t>(rowIndexValue.toULongLong());
    if (rowIndex >= m_entries.size())
    {
        return nullptr;
    }
    return &m_entries[rowIndex];
}

void FileHandleUsageWindow::copyCurrentRow()
{
    if (m_resultTable == nullptr || m_resultTable->currentItem() == nullptr)
    {
        return;
    }

    QStringList fields;
    for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
    {
        fields.push_back(m_resultTable->currentItem()->text(columnIndex));
    }
    QApplication::clipboard()->setText(fields.join('\t'));
}

void FileHandleUsageWindow::openCurrentProcessDetail()
{
    const filedock::handleusage::HandleUsageEntry* entry = selectedEntry();
    if (entry == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("进程详情"), QStringLiteral("请先选择一条句柄记录。"));
        return;
    }

    if (!m_openProcessDetailCallback)
    {
        QMessageBox::warning(this, QStringLiteral("进程详情"), QStringLiteral("未配置进程详情跳转回调。"));
        return;
    }

    kLogEvent openDetailEvent;
    info << openDetailEvent
        << "[FileHandleUsageWindow] openCurrentProcessDetail: pid="
        << entry->processId
        << ", process="
        << entry->processName.toStdString()
        << eol;

    m_openProcessDetailCallback(entry->processId);
}

void FileHandleUsageWindow::showTableContextMenu(const QPoint& localPosition)
{
    if (m_resultTable == nullptr)
    {
        return;
    }

    QTreeWidgetItem* clickedItem = m_resultTable->itemAt(localPosition);
    if (clickedItem == nullptr)
    {
        return;
    }
    m_resultTable->setCurrentItem(clickedItem);

    QMenu menu(this);
    QAction* openProcessAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));
    QAction* copyRowAction = menu.addAction(QIcon(":/Icon/handle_copy_row.svg"), QStringLiteral("复制整行"));

    QAction* selectedAction = menu.exec(m_resultTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }
    if (selectedAction == openProcessAction)
    {
        openCurrentProcessDetail();
        return;
    }
    if (selectedAction == copyRowAction)
    {
        copyCurrentRow();
    }
}
