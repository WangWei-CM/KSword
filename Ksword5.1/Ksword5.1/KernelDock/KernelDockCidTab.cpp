#include "KernelDockCidTab.h"
#include "KernelDock.h"
#include "../UI/VisibleTableWidget.h"

// ============================================================
// KernelDockCidTab.cpp
// 作用说明：
// 1) 聚合进程 / 线程 cross-view 证据，形成只读 CID 视图；
// 2) 通过 ArkDriverClient 查询 R0 结果，不在 UI 线程做耗时采集；
// 3) 不暴露任何 patch、unlink、restore 或其他写路径。
// ============================================================

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QModelIndex>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <thread>
#include <utility>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    enum class CidColumn : int
    {
        Kind = 0,
        CidValue,
        ProcessId,
        ThreadId,
        Image,
        ObjectAddress,
        StartAddress,
        SourceMask,
        AnomalyFlags,
        CidEntryFlags,
        Confidence,
        DetailStatus,
        Denoise,
        Status,
        Count
    };

    QString blueButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:transparent;/* %3 */color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString headerStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:transparent;/* %2 */border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString itemSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:palette(highlighted-text);}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString menuStyle()
    {
        // CID 页右键菜单：
        // - 输入：无；
        // - 处理：统一复用全局不透明 QMenu 样式，避免 palette role 在浅色/深色 Dock 中出现透明背景；
        // - 返回：可直接 setStyleSheet 的菜单样式文本。
        return KswordTheme::ContextMenuStyle();
    }

    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString safeText(const QString& valueText, const QString& fallbackText)
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString safeText(const QString& valueText)
    {
        return safeText(valueText, kernelText("kernel.cid.placeholder.empty", QStringLiteral("<空>")));
    }

    QString emptyText()
    {
        return kernelText("kernel.cid.placeholder.empty", QStringLiteral("<空>"));
    }

    QString friendlyIoMessage(const QString& messageText)
    {
        // friendlyIoMessage：
        // - 输入：ArkDriverClient::IoResult::message 或派生诊断文本；
        // - 处理：把常见 DeviceIoControl/unsupported/capability 文本转换为中文可读说明；
        // - 返回：适合状态栏、详情区展示的短文本，不隐藏 status/count 等结构化字段。
        const QString trimmedText = messageText.trimmed();
        if (trimmedText.isEmpty())
        {
            return kernelText("kernel.cid.message.no_driver_message", QStringLiteral("无额外驱动消息"));
        }
        if (trimmedText.contains(QStringLiteral("DeviceIoControl"), Qt::CaseInsensitive))
        {
            return kernelText("kernel.cid.message.device_io_failure", QStringLiteral("驱动接口调用失败或当前驱动版本不匹配"));
        }
        if (trimmedText.contains(QStringLiteral("unsupported"), Qt::CaseInsensitive) ||
            trimmedText.contains(QStringLiteral("not supported"), Qt::CaseInsensitive))
        {
            return kernelText("kernel.cid.message.unsupported", QStringLiteral("当前驱动不支持该 CID/cross-view 查询入口"));
        }
        if (trimmedText.contains(QStringLiteral("capability"), Qt::CaseInsensitive) ||
            trimmedText.contains(QStringLiteral("DynData"), Qt::CaseInsensitive))
        {
            return kernelText("kernel.cid.message.capability", QStringLiteral("动态偏移能力未满足，请先查看 DynData/Capability 状态"));
        }
        return trimmedText;
    }

    QString formatAddressText(const std::uint64_t value)
    {
        return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 16, 16, QChar('0')).toUpper();
    }

}

KernelDockCidTab::KernelDockCidTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();

    QMetaObject::invokeMethod(this, [this]() {
        refreshAsync();
    }, Qt::QueuedConnection);
}

void KernelDockCidTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    m_toolbarLayout = new QHBoxLayout();
    m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolbarLayout->setSpacing(6);

    m_refreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), this);
    m_refreshButton->setFixedWidth(34);
    m_refreshButton->setToolTip(kernelText("kernel.cid.toolbar.refresh.tooltip", QStringLiteral("刷新 CID / cross-view 证据")));
    m_refreshButton->setStyleSheet(blueButtonStyle());

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(kernelText("kernel.cid.toolbar.filter.placeholder", QStringLiteral("按类型/PID/TID/地址/状态/异常/详情筛选")));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setStyleSheet(blueInputStyle());

    m_statusLabel = new QLabel(kernelText("kernel.cid.status.waiting", QStringLiteral("状态：等待刷新")), this);
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_toolbarLayout->addWidget(m_refreshButton, 0);
    m_toolbarLayout->addWidget(m_filterEdit, 1);
    m_toolbarLayout->addWidget(m_statusLabel, 0);
    rootLayout->addLayout(m_toolbarLayout);

    m_table = new ks::ui::VisibleTableWidget(this);
    m_table->setColumnCount(static_cast<int>(CidColumn::Count));
    m_table->setHorizontalHeaderLabels(QStringList{
        kernelText("kernel.cid.header.kind", QStringLiteral("类型")),
        QStringLiteral("CID"),
        QStringLiteral("PID"),
        QStringLiteral("TID"),
        kernelText("kernel.cid.header.image", QStringLiteral("图像")),
        kernelText("kernel.cid.header.object_address", QStringLiteral("对象地址")),
        kernelText("kernel.cid.header.start_address", QStringLiteral("起始地址")),
        QStringLiteral("SourceMask"),
        QStringLiteral("AnomalyFlags"),
        QStringLiteral("CID Flags"),
        QStringLiteral("Confidence"),
        QStringLiteral("DetailStatus"),
        QStringLiteral("Denoise"),
        kernelText("kernel.cid.header.status", QStringLiteral("状态"))
        });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setStyleSheet(itemSelectionStyle());
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStyleSheet(headerStyle());
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(CidColumn::Image), QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(CidColumn::Status), QHeaderView::Stretch);
    rootLayout->addWidget(m_table, 1);

    m_detailEditor = new CodeEditorWidget(this);
    m_detailEditor->setReadOnly(true);
    m_detailEditor->setText(kernelText("kernel.cid.detail.initial", QStringLiteral("请选择一条 cross-view 记录查看详情。")));
    rootLayout->addWidget(m_detailEditor, 1);
}

void KernelDockCidTab::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        refreshAsync();
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() {
        rebuildTable();
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        const CidEvidenceRow* row = selectedRow();
        if (m_detailEditor != nullptr)
        {
            m_detailEditor->setText(buildDetailText(row));
        }
    });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showContextMenu(localPosition);
    });
}

void KernelDockCidTab::refreshAsync()
{
    if (m_refreshing.exchange(true))
    {
        return;
    }

    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.cid.status.refreshing", QStringLiteral("状态：刷新中...")));
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDockCidTab> guardThis(this);
    std::thread([guardThis]() {
        std::vector<CidEvidenceRow> rows;
        QString errorText;

        const ksword::ark::DriverClient client;
        const ksword::ark::ProcessCrossViewResult processResult = client.queryProcessCrossView();
        const ksword::ark::ThreadCrossViewResult threadResult = client.queryThreadCrossView();
        const ksword::ark::CidTableAuditResult cidTableResult = client.enumCidTable();
        const bool processOk = processResult.io.ok;
        const bool threadOk = threadResult.io.ok;
        const bool cidOk = cidTableResult.io.ok;
        CidTableSummary cidSummary{};
        cidSummary.queried = true;
        cidSummary.ok = cidTableResult.io.ok;
        cidSummary.unsupported = cidTableResult.unsupported;
        cidSummary.status = cidTableResult.status;
        cidSummary.totalCount = cidTableResult.totalCount;
        cidSummary.returnedCount = cidTableResult.returnedCount;
        cidSummary.visitedCount = cidTableResult.visitedCount;
        cidSummary.maxVisitCount = cidTableResult.maxVisitCount;
        cidSummary.flags = cidTableResult.flags;
        cidSummary.lastStatus = cidTableResult.lastStatus;
        cidSummary.pspCidTableAddress = cidTableResult.pspCidTableAddress;
        cidSummary.dynDataCapabilityMask = cidTableResult.dynDataCapabilityMask;
        cidSummary.htTableCodeOffset = cidTableResult.htTableCodeOffset;
        cidSummary.hteLowValueOffset = cidTableResult.hteLowValueOffset;
        cidSummary.messageText = friendlyIoMessage(QString::fromStdString(cidTableResult.io.message));

        if (processOk)
        {
            for (const ksword::ark::ProcessCrossViewEntry& entry : processResult.entries)
            {
                CidEvidenceRow row{};
                row.isThread = false;
                row.processId = entry.processId;
                row.parentProcessId = entry.parentProcessId;
                row.objectAddress = entry.objectAddress;
                row.startAddress = entry.startAddress;
                row.sourceMask = entry.sourceMask;
                row.anomalyFlags = entry.anomalyFlags;
                row.dynDataCapabilityMask = entry.dynDataCapabilityMask;
                row.lastStatus = entry.lastStatus;
                row.confidence = entry.confidence;
                row.detailStatus = entry.detailStatus;
                row.denoiseFlags = entry.denoiseFlags;
                row.publicProcessId = entry.publicProcessId;
                row.activeListProcessId = entry.activeListProcessId;
                row.cidTableProcessId = entry.cidTableProcessId;
                row.publicWalkStatus = entry.publicWalkStatus;
                row.activeListStatus = entry.activeListStatus;
                row.cidTableStatus = entry.cidTableStatus;
                row.imageNameText = QString::fromStdString(entry.imageName);
                row.detailText = friendlyIoMessage(QString::fromStdString(entry.detail));
                rows.push_back(std::move(row));
            }
        }
        else
        {
            errorText += kernelText("kernel.cid.error.process", QStringLiteral("进程 cross-view 失败: %1\n"))
                .arg(friendlyIoMessage(QString::fromStdString(processResult.io.message)));
        }

        if (threadOk)
        {
            for (const ksword::ark::ThreadCrossViewEntry& entry : threadResult.entries)
            {
                CidEvidenceRow row{};
                row.isThread = true;
                row.processId = entry.processId;
                row.threadId = entry.threadId;
                row.processObjectAddress = entry.processObjectAddress;
                row.objectAddress = entry.objectAddress;
                row.startAddress = entry.startAddress;
                row.sourceMask = entry.sourceMask;
                row.anomalyFlags = entry.anomalyFlags;
                row.dynDataCapabilityMask = entry.dynDataCapabilityMask;
                row.lastStatus = entry.lastStatus;
                row.confidence = entry.confidence;
                row.detailStatus = entry.detailStatus;
                row.denoiseFlags = entry.denoiseFlags;
                row.publicThreadId = entry.publicThreadId;
                row.threadListThreadId = entry.threadListThreadId;
                row.cidTableThreadId = entry.cidTableThreadId;
                row.publicProcessId = entry.publicProcessId;
                row.threadListProcessId = entry.threadListProcessId;
                row.cidTableProcessId = entry.cidTableProcessId;
                row.publicWalkStatus = entry.publicWalkStatus;
                row.threadListStatus = entry.threadListStatus;
                row.cidTableStatus = entry.cidTableStatus;
                row.startAddressStatus = entry.startAddressStatus;
                row.imageNameText = QString::fromStdString(entry.imageName);
                row.detailText = friendlyIoMessage(QString::fromStdString(entry.detail));
                rows.push_back(std::move(row));
            }
        }
        else
        {
            errorText += kernelText("kernel.cid.error.thread", QStringLiteral("线程 cross-view 失败: %1\n"))
                .arg(friendlyIoMessage(QString::fromStdString(threadResult.io.message)));
        }

        if (cidOk)
        {
            for (const KSWORD_ARK_CID_TABLE_ENTRY& entry : cidTableResult.entries)
            {
                CidEvidenceRow row{};
                row.isRawCid = true;
                row.isThread = entry.expectedObjectKind == KSWORD_ARK_CID_OBJECT_KIND_THREAD;
                row.cidValue = entry.cidValue;
                row.cidHandleIndex = entry.handleIndex;
                row.cidExpectedKind = entry.expectedObjectKind;
                row.cidEntryFlags = entry.flags;
                row.cidReferenceStatus = entry.referenceStatus;
                row.objectAddress = entry.objectAddress;
                row.sourceMask = KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE;
                row.detailStatus = entry.lookupStatus;
                row.lastStatus = entry.referenceStatus;
                if (entry.expectedObjectKind == KSWORD_ARK_CID_OBJECT_KIND_PROCESS)
                {
                    row.processId = entry.cidValue;
                    row.cidTableProcessId = entry.cidValue;
                }
                else if (entry.expectedObjectKind == KSWORD_ARK_CID_OBJECT_KIND_THREAD)
                {
                    row.threadId = entry.cidValue;
                    row.cidTableThreadId = entry.cidValue;
                }
                if ((entry.flags & KSWORD_ARK_CID_ENTRY_FLAG_DANGLING) != 0U)
                {
                    row.anomalyFlags |= KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT;
                }
                if ((entry.flags & KSWORD_ARK_CID_ENTRY_FLAG_TYPE_MISMATCH) != 0U)
                {
                    row.anomalyFlags |= KSWORD_ARK_CROSSVIEW_ANOMALY_PID_FIELD_MISMATCH;
                }
                row.imageNameText = cidKindText(entry.expectedObjectKind);
                row.detailText = kernelText("kernel.cid.detail.raw_row", QStringLiteral("R0 CID table row：handleIndex=%1，lookupStatus=%2，referenceStatus=%3，flags=%4"))
                    .arg(entry.handleIndex)
                    .arg(statusLabelText(entry.lookupStatus))
                    .arg(statusLabelText(entry.referenceStatus))
                    .arg(cidEntryFlagsText(entry.flags));
                rows.push_back(std::move(row));
            }
        }
        else
        {
            errorText += kernelText("kernel.cid.error.cid_table", QStringLiteral("R0 CID 表枚举失败: %1\n"))
                .arg(friendlyIoMessage(QString::fromStdString(cidTableResult.io.message)));
        }

        std::sort(rows.begin(), rows.end(), [](const CidEvidenceRow& left, const CidEvidenceRow& right) {
            if (left.isRawCid != right.isRawCid)
            {
                return left.isRawCid < right.isRawCid;
            }
            if (left.isThread != right.isThread)
            {
                return left.isThread < right.isThread;
            }
            if (left.processId != right.processId)
            {
                return left.processId < right.processId;
            }
            return left.threadId < right.threadId;
        });

        KernelDockCidTab* const contextObject = guardThis.data();
        if (contextObject == nullptr)
        {
            return;
        }

        QMetaObject::invokeMethod(contextObject, [guardThis, rows = std::move(rows), errorText, processOk, threadOk, cidOk, cidSummary]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_cidSummary = cidSummary;
            guardThis->applyRefreshResult(std::move(rows), errorText.trimmed(), processOk || threadOk || cidOk);
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDockCidTab::applyRefreshResult(std::vector<CidEvidenceRow> rows, const QString& errorText, const bool success)
{
    m_refreshing.store(false);
    m_refreshButton->setEnabled(true);
    m_rows = std::move(rows);
    rebuildTable();

    if (!success)
    {
        m_statusLabel->setText(kernelText("kernel.cid.status.failed", QStringLiteral("状态：刷新失败 - %1")).arg(errorText));
        m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::ErrorHex()));
    }
}

void KernelDockCidTab::rebuildTable()
{
    if (m_table == nullptr)
    {
        return;
    }

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    std::size_t visibleCount = 0;
    for (std::size_t index = 0; index < m_rows.size(); ++index)
    {
        const CidEvidenceRow& row = m_rows[index];
        if (!rowMatchesFilter(row))
        {
            continue;
        }

        const int rowIndex = m_table->rowCount();
        m_table->insertRow(rowIndex);

        auto* kindItem = readOnlyItem(row.isRawCid
            ? QStringLiteral("CID/%1").arg(cidKindText(row.cidExpectedKind))
            : roleText(row.isThread));
        auto* cidValueItem = readOnlyItem(row.cidValue == 0U ? emptyText() : QString::number(row.cidValue));
        auto* pidItem = readOnlyItem(row.processId == 0U ? emptyText() : QString::number(row.processId));
        auto* tidItem = readOnlyItem(row.isThread ? QString::number(row.threadId) : emptyText());
        auto* imageItem = readOnlyItem(safeText(row.imageNameText));
        auto* objectItem = readOnlyItem(formatHex64(row.objectAddress));
        auto* startItem = readOnlyItem(formatHex64(row.startAddress));
        auto* sourceItem = readOnlyItem(sourceMaskText(row.sourceMask));
        auto* anomalyItem = readOnlyItem(anomalyFlagsText(row.anomalyFlags));
        auto* cidFlagsItem = readOnlyItem(cidEntryFlagsText(row.cidEntryFlags));
        auto* confidenceItem = readOnlyItem(QString::number(row.confidence));
        auto* detailStatusItem = readOnlyItem(detailStatusText(row.detailStatus));
        auto* denoiseItem = readOnlyItem(denoiseFlagsText(row.denoiseFlags));
        auto* statusItem = readOnlyItem(statusLabelText(row.lastStatus));

        kindItem->setData(Qt::UserRole, static_cast<qulonglong>(index));
        if ((row.anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) != 0U ||
            (row.anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) != 0U)
        {
            anomalyItem->setForeground(QBrush(KswordTheme::WarningColor()));
        }
        if ((row.anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_PID_FIELD_MISMATCH) != 0U ||
            (row.anomalyFlags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) != 0U)
        {
            anomalyItem->setForeground(QBrush(KswordTheme::ErrorColor()));
        }

        m_table->setItem(rowIndex, static_cast<int>(CidColumn::Kind), kindItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::CidValue), cidValueItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::ProcessId), pidItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::ThreadId), tidItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::Image), imageItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::ObjectAddress), objectItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::StartAddress), startItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::SourceMask), sourceItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::AnomalyFlags), anomalyItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::CidEntryFlags), cidFlagsItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::Confidence), confidenceItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::DetailStatus), detailStatusItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::Denoise), denoiseItem);
        m_table->setItem(rowIndex, static_cast<int>(CidColumn::Status), statusItem);
        ++visibleCount;
    }

    m_table->setSortingEnabled(true);
    const QString cidSummaryText = m_cidSummary.queried
        ? kernelText("kernel.cid.status.summary_detail", QStringLiteral("；PspCidTable=%1；CID returned/total=%2/%3；status=%4；truncated=%5；visited=%6/%7"))
            .arg(formatHex64(m_cidSummary.pspCidTableAddress))
            .arg(m_cidSummary.returnedCount)
            .arg(m_cidSummary.totalCount)
            .arg(cidEnumStatusText(m_cidSummary.status))
            .arg(cidSummaryTruncated(m_cidSummary)
                ? kernelText("kernel.cid.value.yes", QStringLiteral("是"))
                : kernelText("kernel.cid.value.no", QStringLiteral("否")))
            .arg(m_cidSummary.visitedCount)
            .arg(m_cidSummary.maxVisitCount)
        : QString();
    m_statusLabel->setText(kernelText("kernel.cid.status.loaded", QStringLiteral("状态：已加载 %1 项，显示 %2 项%3"))
        .arg(static_cast<qulonglong>(m_rows.size()))
        .arg(static_cast<qulonglong>(visibleCount))
        .arg(cidSummaryText));
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::SuccessHex()));

    if (visibleCount == 0U)
    {
        // 空表诊断：
        // - 输入：刷新结果总量与筛选关键字；
        // - 处理：插入一行可复制、可选中的诊断行，避免 CID 页看起来像渲染失败；
        // - 返回：无返回值，详情面板会从 UserRole + 2 读取展开文本。
        const QString reasonText = m_rows.empty()
            ? kernelText("kernel.cid.empty.no_records", QStringLiteral("本轮未返回任何 CID / cross-view 记录。"))
            : kernelText("kernel.cid.empty.no_matches", QStringLiteral("当前筛选条件没有命中 CID / cross-view 记录。"));
        const QString detailText = buildDiagnosticDetailText(reasonText);
        insertDiagnosticRow(
            m_rows.empty()
                ? kernelText("kernel.cid.placeholder.no_records", QStringLiteral("<无 CID 记录>"))
                : kernelText("kernel.cid.placeholder.no_matches", QStringLiteral("<筛选无结果>")),
            reasonText,
            detailText);
        m_table->setCurrentCell(0, static_cast<int>(CidColumn::Kind));
        if (m_detailEditor != nullptr)
        {
            m_detailEditor->setText(detailText);
        }
        return;
    }

    const int targetRow = m_table->currentRow() >= 0 ? m_table->currentRow() : 0;
    m_table->setCurrentCell(qMin(targetRow, m_table->rowCount() - 1), static_cast<int>(CidColumn::Kind));
    if (m_detailEditor != nullptr)
    {
        m_detailEditor->setText(buildDetailText(selectedRow()));
    }
}

void KernelDockCidTab::showContextMenu(const QPoint& localPosition)
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
    menu.setStyleSheet(menuStyle());
    QAction* copyRowAction = menu.addAction(kernelText("kernel.cid.menu.copy_row", QStringLiteral("复制当前行")));
    copyRowAction->setEnabled(m_table->currentRow() >= 0);
    const QAction* selectedAction = menu.exec(m_table->viewport()->mapToGlobal(localPosition));
    if (selectedAction == copyRowAction)
    {
        copyCurrentRow();
    }
}

void KernelDockCidTab::copyCurrentRow() const
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

const KernelDockCidTab::CidEvidenceRow* KernelDockCidTab::selectedRow() const
{
    if (m_table == nullptr || m_table->currentRow() < 0)
    {
        return nullptr;
    }

    const QTableWidgetItem* kindItem = m_table->item(m_table->currentRow(), static_cast<int>(CidColumn::Kind));
    if (kindItem == nullptr)
    {
        return nullptr;
    }
    const QString diagnosticText = kindItem->data(Qt::UserRole + 2).toString();
    if (!diagnosticText.isEmpty())
    {
        return nullptr;
    }

    const std::size_t sourceIndex = static_cast<std::size_t>(kindItem->data(Qt::UserRole).toULongLong());
    return sourceIndex < m_rows.size() ? &m_rows[sourceIndex] : nullptr;
}

QString KernelDockCidTab::buildDetailText(const CidEvidenceRow* row) const
{
    if (row == nullptr)
    {
        if (m_table != nullptr && m_table->currentRow() >= 0)
        {
            // 诊断行详情：
            // - 输入：当前表格行 Kind 列 UserRole + 2；
            // - 处理：空表/筛选空命中时优先展示完整诊断文本；
            // - 返回：若不是诊断行，则继续给出常规选择提示。
            const QTableWidgetItem* kindItem = m_table->item(m_table->currentRow(), static_cast<int>(CidColumn::Kind));
            const QString diagnosticText = kindItem != nullptr
                ? kindItem->data(Qt::UserRole + 2).toString()
                : QString();
            if (!diagnosticText.isEmpty())
            {
                return diagnosticText;
            }
        }
        return kernelText("kernel.cid.detail.unavailable", QStringLiteral("请选择一条 cross-view 记录查看详情。"));
    }

    QStringList lines;
    lines << QStringLiteral("[CrossView]");
    if (m_cidSummary.queried)
    {
        lines << QStringLiteral("[R0 CID Table Summary]");
        lines << QStringLiteral("PspCidTable address：%1").arg(formatHex64(m_cidSummary.pspCidTableAddress));
        lines << QStringLiteral("returnedCount / totalCount：%1 / %2").arg(m_cidSummary.returnedCount).arg(m_cidSummary.totalCount);
        lines << QStringLiteral("status：%1 (%2)").arg(formatHex32(m_cidSummary.status), cidEnumStatusText(m_cidSummary.status));
        lines << kernelText("kernel.cid.detail.truncated", QStringLiteral("truncated：%1")).arg(cidSummaryTruncated(m_cidSummary)
            ? kernelText("kernel.cid.value.yes", QStringLiteral("是"))
            : kernelText("kernel.cid.value.no", QStringLiteral("否")));
        lines << QStringLiteral("visitedCount / maxVisitCount：%1 / %2").arg(m_cidSummary.visitedCount).arg(m_cidSummary.maxVisitCount);
        lines << QStringLiteral("responseFlags：%1").arg(formatHex32(m_cidSummary.flags));
        lines << QStringLiteral("lastStatus：%1").arg(statusLabelText(m_cidSummary.lastStatus));
        lines << QStringLiteral("DynDataCapabilityMask：%1").arg(formatHex64(m_cidSummary.dynDataCapabilityMask));
        lines << QStringLiteral("HtTableCodeOffset：%1").arg(formatHex32(m_cidSummary.htTableCodeOffset));
        lines << QStringLiteral("HteLowValueOffset：%1").arg(formatHex32(m_cidSummary.hteLowValueOffset));
        lines << kernelText("kernel.cid.detail.r3r0_note", QStringLiteral("R3/R0 说明：%1")).arg(safeText(m_cidSummary.messageText));
        lines << QStringLiteral("");
    }
    lines << kernelText("kernel.cid.detail.kind", QStringLiteral("类型：%1")).arg(roleText(row->isThread));
    lines << kernelText("kernel.cid.detail.source", QStringLiteral("来源：%1")).arg(row->isRawCid ? QStringLiteral("R0 enumCidTable") : QStringLiteral("Process/Thread cross-view"));
    lines << kernelText("kernel.cid.detail.cid", QStringLiteral("CID：%1")).arg(row->cidValue == 0U ? emptyText() : QString::number(row->cidValue));
    lines << kernelText("kernel.cid.detail.cid_handle_index", QStringLiteral("CID HandleIndex：%1")).arg(row->cidHandleIndex);
    lines << kernelText("kernel.cid.detail.cid_entry_kind", QStringLiteral("CID EntryKind：%1 (%2)")).arg(formatHex32(row->cidExpectedKind), cidKindText(row->cidExpectedKind));
    lines << kernelText("kernel.cid.detail.cid_entry_flags", QStringLiteral("CID EntryFlags：%1 (%2)")).arg(formatHex32(row->cidEntryFlags), cidEntryFlagsText(row->cidEntryFlags));
    lines << kernelText("kernel.cid.detail.cid_reference_status", QStringLiteral("CID ReferenceStatus：%1")).arg(statusLabelText(row->cidReferenceStatus));
    lines << QStringLiteral("PID：%1").arg(row->processId);
    lines << kernelText("kernel.cid.detail.tid", QStringLiteral("TID：%1")).arg(row->threadId == 0U ? emptyText() : QString::number(row->threadId));
    lines << kernelText("kernel.cid.detail.parent_pid", QStringLiteral("父 PID：%1")).arg(row->parentProcessId == 0U ? emptyText() : QString::number(row->parentProcessId));
    lines << kernelText("kernel.cid.detail.object_address", QStringLiteral("对象地址：%1")).arg(formatHex64(row->objectAddress));
    lines << kernelText("kernel.cid.detail.process_object_address", QStringLiteral("进程对象地址：%1")).arg(formatHex64(row->processObjectAddress));
    lines << kernelText("kernel.cid.detail.start_address", QStringLiteral("起始地址：%1")).arg(formatHex64(row->startAddress));
    lines << kernelText("kernel.cid.detail.image", QStringLiteral("图像名：%1")).arg(safeText(row->imageNameText));
    lines << QStringLiteral("SourceMask：%1 (%2)").arg(formatHex32(row->sourceMask), sourceMaskText(row->sourceMask));
    lines << QStringLiteral("AnomalyFlags：%1 (%2)").arg(formatHex32(row->anomalyFlags), anomalyFlagsText(row->anomalyFlags));
    lines << QStringLiteral("Confidence：%1").arg(row->confidence);
    lines << QStringLiteral("DetailStatus：%1 (%2)").arg(formatHex32(row->detailStatus), detailStatusText(row->detailStatus));
    lines << QStringLiteral("DenoiseFlags：%1 (%2)").arg(formatHex32(row->denoiseFlags), denoiseFlagsText(row->denoiseFlags));
    lines << QStringLiteral("LastStatus：%1").arg(statusLabelText(row->lastStatus));
    lines << QStringLiteral("DynDataCapabilityMask：%1").arg(formatHex64(row->dynDataCapabilityMask));
    lines << QStringLiteral("PublicProcessId: %1").arg(row->publicProcessId == 0U ? emptyText() : QString::number(row->publicProcessId));
    lines << QStringLiteral("ActiveListProcessId: %1").arg(row->activeListProcessId == 0U ? emptyText() : QString::number(row->activeListProcessId));
    lines << QStringLiteral("CidTableProcessId: %1").arg(row->cidTableProcessId == 0U ? emptyText() : QString::number(row->cidTableProcessId));
    lines << QStringLiteral("PublicThreadId: %1").arg(row->publicThreadId == 0U ? emptyText() : QString::number(row->publicThreadId));
    lines << QStringLiteral("ThreadListThreadId: %1").arg(row->threadListThreadId == 0U ? emptyText() : QString::number(row->threadListThreadId));
    lines << QStringLiteral("CidTableThreadId: %1").arg(row->cidTableThreadId == 0U ? emptyText() : QString::number(row->cidTableThreadId));
    lines << QStringLiteral("ThreadListProcessId: %1").arg(row->threadListProcessId == 0U ? emptyText() : QString::number(row->threadListProcessId));
    lines << QStringLiteral("PublicWalkStatus：%1").arg(statusLabelText(row->publicWalkStatus));
    lines << QStringLiteral("ActiveListStatus：%1").arg(statusLabelText(row->activeListStatus));
    lines << QStringLiteral("ThreadListStatus：%1").arg(statusLabelText(row->threadListStatus));
    lines << QStringLiteral("CidTableStatus：%1").arg(statusLabelText(row->cidTableStatus));
    lines << QStringLiteral("StartAddressStatus：%1").arg(statusLabelText(row->startAddressStatus));
    lines << QStringLiteral("");
    lines << kernelText("kernel.cid.detail.detail_header", QStringLiteral("详情："));
    lines << safeText(row->detailText, kernelText("kernel.cid.placeholder.no_detail", QStringLiteral("<无详情>")));

    const unsigned long targetKind = row->cidExpectedKind != KSWORD_ARK_CID_OBJECT_KIND_UNKNOWN
        ? row->cidExpectedKind
        : (row->isThread ? KSWORD_ARK_CID_OBJECT_KIND_THREAD : KSWORD_ARK_CID_OBJECT_KIND_PROCESS);
    const unsigned long cidValue = row->cidValue != 0U
        ? row->cidValue
        : (row->isThread ? row->threadId : row->processId);
    if (cidValue != 0U)
    {
        const ksword::ark::DriverClient client;
        const ksword::ark::KernelObjectSummaryAuditResult summary =
            client.queryKernelObjectSummary(targetKind, cidValue, row->objectAddress);
        const auto& response = summary.response;
        lines << QStringLiteral("");
        lines << QStringLiteral("[KernelObjectSummary]");
        lines << kernelText("kernel.cid.detail.io_summary", QStringLiteral("IO：%1，unsupported=%2，说明=%3"))
            .arg(summary.io.ok ? QStringLiteral("OK") : QStringLiteral("FAIL"))
            .arg(summary.unsupported ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(friendlyIoMessage(QString::fromStdString(summary.io.message)));
        lines << QStringLiteral("Status：%1 (%2)").arg(formatHex32(response.status), objectSummaryStatusText(response.status));
        lines << QStringLiteral("FieldFlags：%1").arg(formatHex32(response.fieldFlags));
        lines << QStringLiteral("TargetKind：%1 (%2)").arg(formatHex32(response.targetKind), cidKindText(response.targetKind));
        lines << QStringLiteral("CidValue：%1").arg(response.cidValue);
        lines << QStringLiteral("LookupStatus：%1").arg(statusLabelText(response.lookupStatus));
        lines << QStringLiteral("TypeStatus：%1").arg(statusLabelText(response.typeStatus));
        lines << QStringLiteral("CounterStatus：%1").arg(statusLabelText(response.counterStatus));
        lines << QStringLiteral("ObjectHeaderStatus：%1 (%2)").arg(formatHex32(response.objectHeaderStatus), objectHeaderStatusText(response.objectHeaderStatus));
        lines << QStringLiteral("ObjectAddress：%1").arg(formatHex64(response.objectAddress));
        lines << QStringLiteral("ExpectedObjectAddress：%1").arg(formatHex64(response.expectedObjectAddress));
        lines << QStringLiteral("ObjectTypeAddress：%1").arg(formatHex64(response.objectTypeAddress));
        lines << QStringLiteral("TypeIndex：%1").arg(response.typeIndex);
        lines << QStringLiteral("PointerCount：%1").arg(response.pointerCount);
        lines << QStringLiteral("HandleCount：%1").arg(response.handleCount);
        lines << QStringLiteral("TypeName：%1").arg(safeText(fixedWideText(response.typeName, KSWORD_ARK_KERNEL_OBJECT_TYPE_NAME_CHARS)));
        lines << QStringLiteral("Detail：%1").arg(safeText(fixedWideText(response.detail, KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS)));
        lines << QStringLiteral("DynDataCapabilityMask：%1").arg(formatHex64(response.dynDataCapabilityMask));
        lines << QStringLiteral("OtNameOffset：%1").arg(formatHex32(response.otNameOffset));
        lines << QStringLiteral("OtIndexOffset：%1").arg(formatHex32(response.otIndexOffset));
    }
    return lines.join('\n');
}

QString KernelDockCidTab::buildDiagnosticDetailText(const QString& reasonText) const
{
    // buildDiagnosticDetailText：
    // - 输入：空表或过滤空命中的原因；
    // - 处理：补充 CID summary、筛选关键字和可执行排查方向；
    // - 返回：给详情面板和诊断行复用的多行文本。
    QStringList lines;
    lines << QStringLiteral("[CID / CrossView Diagnostic]");
    lines << kernelText("kernel.cid.diagnostic.reason", QStringLiteral("原因：%1")).arg(safeText(reasonText));
    lines << kernelText("kernel.cid.diagnostic.filter", QStringLiteral("当前筛选：%1")).arg(m_filterEdit != nullptr
        ? safeText(m_filterEdit->text().trimmed(), kernelText("kernel.cid.placeholder.no_filter", QStringLiteral("<无筛选>")))
        : kernelText("kernel.cid.placeholder.no_filter_widget", QStringLiteral("<无筛选控件>")));
    lines << kernelText("kernel.cid.diagnostic.source_total", QStringLiteral("源记录总数：%1")).arg(static_cast<qulonglong>(m_rows.size()));
    if (m_cidSummary.queried)
    {
        lines << QStringLiteral("");
        lines << QStringLiteral("[R0 CID Table Summary]");
        lines << QStringLiteral("IO：%1").arg(m_cidSummary.ok ? QStringLiteral("OK") : QStringLiteral("Unavailable"));
        lines << kernelText("kernel.cid.diagnostic.unsupported", QStringLiteral("Unsupported：%1")).arg(m_cidSummary.unsupported
            ? kernelText("kernel.cid.value.yes", QStringLiteral("是"))
            : kernelText("kernel.cid.value.no", QStringLiteral("否")));
        lines << QStringLiteral("PspCidTable：%1").arg(formatHex64(m_cidSummary.pspCidTableAddress));
        lines << QStringLiteral("returnedCount / totalCount：%1 / %2").arg(m_cidSummary.returnedCount).arg(m_cidSummary.totalCount);
        lines << QStringLiteral("visitedCount / maxVisitCount：%1 / %2").arg(m_cidSummary.visitedCount).arg(m_cidSummary.maxVisitCount);
        lines << QStringLiteral("status：%1 (%2)").arg(formatHex32(m_cidSummary.status), cidEnumStatusText(m_cidSummary.status));
        lines << kernelText("kernel.cid.diagnostic.truncated", QStringLiteral("truncated：%1")).arg(cidSummaryTruncated(m_cidSummary)
            ? kernelText("kernel.cid.value.yes", QStringLiteral("是"))
            : kernelText("kernel.cid.value.no", QStringLiteral("否")));
        lines << QStringLiteral("lastStatus：%1").arg(statusLabelText(m_cidSummary.lastStatus));
        lines << QStringLiteral("DynDataCapabilityMask：%1").arg(formatHex64(m_cidSummary.dynDataCapabilityMask));
        lines << kernelText("kernel.cid.detail.r3r0_note", QStringLiteral("R3/R0 说明：%1")).arg(safeText(m_cidSummary.messageText));
    }
    lines << QStringLiteral("");
    lines << kernelText("kernel.cid.diagnostic.recommendations", QStringLiteral("[排查建议]"));
    lines << kernelText("kernel.cid.diagnostic.recommendation1", QStringLiteral("1. 若显示 capability/DynData 不足，请先查看 Kernel -> DynData 页确认 Process/Thread/CID 相关能力位。"));
    lines << kernelText("kernel.cid.diagnostic.recommendation2", QStringLiteral("2. 若当前筛选不为空，清空筛选框后再确认是否有源记录。"));
    lines << kernelText("kernel.cid.diagnostic.recommendation3", QStringLiteral("3. 若 returnedCount 为 0 或 PspCidTable 为空，说明当前驱动/动态偏移还没有提供可用 CID 表证据。"));
    return lines.join('\n');
}

void KernelDockCidTab::insertDiagnosticRow(
    const QString& titleText,
    const QString& statusText,
    const QString& detailText)
{
    // insertDiagnosticRow：
    // - 输入：诊断标题、状态列文本和详情文本；
    // - 处理：构造一行只读占位，所有列都可通过右键复制；
    // - 返回：无返回值，UserRole + 2 保存详情区文本。
    if (m_table == nullptr)
    {
        return;
    }

    m_table->setSortingEnabled(false);
    m_table->setRowCount(1);

    auto* kindItem = readOnlyItem(titleText);
    kindItem->setData(Qt::UserRole + 2, detailText);
    m_table->setItem(0, static_cast<int>(CidColumn::Kind), kindItem);
    m_table->setItem(0, static_cast<int>(CidColumn::CidValue), readOnlyItem(kernelText("kernel.cid.placeholder.diagnostic", QStringLiteral("<诊断>"))));
    m_table->setItem(0, static_cast<int>(CidColumn::ProcessId), readOnlyItem(emptyText()));
    m_table->setItem(0, static_cast<int>(CidColumn::ThreadId), readOnlyItem(emptyText()));
    m_table->setItem(0, static_cast<int>(CidColumn::Image), readOnlyItem(QStringLiteral("CID / CrossView")));
    m_table->setItem(0, static_cast<int>(CidColumn::ObjectAddress), readOnlyItem(formatHex64(0)));
    m_table->setItem(0, static_cast<int>(CidColumn::StartAddress), readOnlyItem(formatHex64(0)));
    m_table->setItem(0, static_cast<int>(CidColumn::SourceMask), readOnlyItem(kernelText("kernel.cid.placeholder.no_source", QStringLiteral("<无源>"))));
    m_table->setItem(0, static_cast<int>(CidColumn::AnomalyFlags), readOnlyItem(kernelText("kernel.cid.placeholder.none", QStringLiteral("<无>"))));
    m_table->setItem(0, static_cast<int>(CidColumn::CidEntryFlags), readOnlyItem(kernelText("kernel.cid.placeholder.none", QStringLiteral("<无>"))));
    m_table->setItem(0, static_cast<int>(CidColumn::Confidence), readOnlyItem(QStringLiteral("0")));
    m_table->setItem(0, static_cast<int>(CidColumn::DetailStatus), readOnlyItem(QStringLiteral("Diagnostic")));
    m_table->setItem(0, static_cast<int>(CidColumn::Denoise), readOnlyItem(kernelText("kernel.cid.placeholder.none", QStringLiteral("<无>"))));
    m_table->setItem(0, static_cast<int>(CidColumn::Status), readOnlyItem(statusText));
    m_table->setSortingEnabled(true);
}

bool KernelDockCidTab::rowMatchesFilter(const CidEvidenceRow& row) const
{
    const QString keyword = m_filterEdit != nullptr ? m_filterEdit->text().trimmed() : QString();
    if (keyword.isEmpty())
    {
        return true;
    }

    return roleText(row.isThread).contains(keyword, Qt::CaseInsensitive) ||
        QString::number(row.processId).contains(keyword, Qt::CaseInsensitive) ||
        QString::number(row.threadId).contains(keyword, Qt::CaseInsensitive) ||
        QString::number(row.cidValue).contains(keyword, Qt::CaseInsensitive) ||
        cidKindText(row.cidExpectedKind).contains(keyword, Qt::CaseInsensitive) ||
        cidEntryFlagsText(row.cidEntryFlags).contains(keyword, Qt::CaseInsensitive) ||
        safeText(row.imageNameText).contains(keyword, Qt::CaseInsensitive) ||
        formatHex64(row.objectAddress).contains(keyword, Qt::CaseInsensitive) ||
        formatHex64(row.startAddress).contains(keyword, Qt::CaseInsensitive) ||
        sourceMaskText(row.sourceMask).contains(keyword, Qt::CaseInsensitive) ||
        anomalyFlagsText(row.anomalyFlags).contains(keyword, Qt::CaseInsensitive) ||
        denoiseFlagsText(row.denoiseFlags).contains(keyword, Qt::CaseInsensitive) ||
        detailStatusText(row.detailStatus).contains(keyword, Qt::CaseInsensitive) ||
        statusLabelText(row.lastStatus).contains(keyword, Qt::CaseInsensitive) ||
        row.detailText.contains(keyword, Qt::CaseInsensitive);
}

QTableWidgetItem* KernelDockCidTab::readOnlyItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString KernelDockCidTab::formatHex32(const std::uint32_t value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')).toUpper();
}

QString KernelDockCidTab::formatHex64(const std::uint64_t value)
{
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 16, 16, QChar('0')).toUpper();
}

QString KernelDockCidTab::statusLabelText(const long statusValue)
{
    return formatHex32(static_cast<std::uint32_t>(statusValue));
}

QString KernelDockCidTab::sourceMaskText(const std::uint32_t mask)
{
    QStringList parts;
    if ((mask & KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK) != 0U) { parts << QStringLiteral("PublicWalk"); }
    if ((mask & KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST) != 0U) { parts << QStringLiteral("ActiveList"); }
    if ((mask & KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE) != 0U) { parts << QStringLiteral("CidTable"); }
    if ((mask & KSWORD_ARK_CROSSVIEW_SOURCE_THREAD_LIST) != 0U) { parts << QStringLiteral("ThreadList"); }
    if (parts.isEmpty()) { parts << emptyText(); }
    return QStringLiteral("%1 [%2]").arg(formatHex32(mask), parts.join(QStringLiteral(", ")));
}

QString KernelDockCidTab::anomalyFlagsText(const std::uint32_t flags)
{
    QStringList parts;
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) != 0U) { parts << QStringLiteral("CID_ONLY"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) != 0U) { parts << QStringLiteral("ACTIVE_ONLY"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) != 0U) { parts << QStringLiteral("MISSING_FROM_ACTIVE_LIST"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) != 0U) { parts << QStringLiteral("MISSING_FROM_CID_TABLE"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) != 0U) { parts << QStringLiteral("THREAD_ORPHAN"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) != 0U) { parts << QStringLiteral("THREAD_NOT_IN_PROCESS_LIST"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) != 0U) { parts << QStringLiteral("START_ADDRESS_OUTSIDE_MODULE"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) != 0U) { parts << QStringLiteral("DANGLING_OBJECT"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_ANOMALY_PID_FIELD_MISMATCH) != 0U) { parts << QStringLiteral("PID_FIELD_MISMATCH"); }
    if (parts.isEmpty()) { parts << emptyText(); }
    return QStringLiteral("%1 [%2]").arg(formatHex32(flags), parts.join(QStringLiteral(", ")));
}

QString KernelDockCidTab::denoiseFlagsText(const std::uint32_t flags)
{
    QStringList parts;
    if ((flags & KSWORD_ARK_CROSSVIEW_DENOISE_PARTIAL_EVIDENCE) != 0U) { parts << QStringLiteral("PARTIAL_EVIDENCE"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_DENOISE_READ_FAILURE) != 0U) { parts << QStringLiteral("READ_FAILURE"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_DENOISE_REFERENCE_FAILURE) != 0U) { parts << QStringLiteral("REFERENCE_FAILURE"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_DENOISE_POSSIBLE_TERMINATING) != 0U) { parts << QStringLiteral("POSSIBLE_TERMINATING"); }
    if ((flags & KSWORD_ARK_CROSSVIEW_DENOISE_UNSUPPORTED_PDB_FIELD) != 0U) { parts << QStringLiteral("UNSUPPORTED_PDB_FIELD"); }
    if (parts.isEmpty()) { parts << emptyText(); }
    return QStringLiteral("%1 [%2]").arg(formatHex32(flags), parts.join(QStringLiteral(", ")));
}

QString KernelDockCidTab::detailStatusText(const std::uint32_t status)
{
    switch (status)
    {
    case KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_OK: return QStringLiteral("OK");
    case KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_PARTIAL: return QStringLiteral("PARTIAL");
    case KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_UNSUPPORTED: return QStringLiteral("UNSUPPORTED");
    case KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_READ_FAILED: return QStringLiteral("READ_FAILED");
    case KSWORD_ARK_CROSSVIEW_DETAIL_STATUS_DATA_MISMATCH: return QStringLiteral("DATA_MISMATCH");
    default: return QStringLiteral("UNKNOWN(%1)").arg(status);
    }
}

QString KernelDockCidTab::roleText(bool isThread)
{
    return isThread ? QStringLiteral("Thread") : QStringLiteral("Process");
}

QString KernelDockCidTab::cidKindText(const std::uint32_t kind)
{
    switch (kind)
    {
    case KSWORD_ARK_CID_OBJECT_KIND_PROCESS: return QStringLiteral("Process");
    case KSWORD_ARK_CID_OBJECT_KIND_THREAD: return QStringLiteral("Thread");
    case KSWORD_ARK_CID_OBJECT_KIND_UNKNOWN:
    default:
        return QStringLiteral("Unknown");
    }
}

QString KernelDockCidTab::cidEntryFlagsText(const std::uint32_t flags)
{
    QStringList parts;
    if ((flags & KSWORD_ARK_CID_ENTRY_FLAG_DANGLING) != 0U) { parts << QStringLiteral("DANGLING"); }
    if ((flags & KSWORD_ARK_CID_ENTRY_FLAG_TYPE_MISMATCH) != 0U) { parts << QStringLiteral("TYPE_MISMATCH"); }
    if ((flags & KSWORD_ARK_CID_ENTRY_FLAG_REFERENCED) != 0U) { parts << QStringLiteral("REFERENCED"); }
    if (parts.isEmpty()) { parts << emptyText(); }
    return QStringLiteral("%1 [%2]").arg(formatHex32(flags), parts.join(QStringLiteral(", ")));
}

QString KernelDockCidTab::cidEnumStatusText(const std::uint32_t status)
{
    switch (status)
    {
    case KSWORD_ARK_CID_ENUM_STATUS_OK: return QStringLiteral("OK");
    case KSWORD_ARK_CID_ENUM_STATUS_PARTIAL: return QStringLiteral("PARTIAL");
    case KSWORD_ARK_CID_ENUM_STATUS_DYNDATA_MISSING: return QStringLiteral("DYNDATA_MISSING");
    case KSWORD_ARK_CID_ENUM_STATUS_PSPCID_UNAVAILABLE: return QStringLiteral("PSPCID_UNAVAILABLE");
    case KSWORD_ARK_CID_ENUM_STATUS_TYPE_UNAVAILABLE: return QStringLiteral("TYPE_UNAVAILABLE");
    case KSWORD_ARK_CID_ENUM_STATUS_BUFFER_TRUNCATED: return QStringLiteral("BUFFER_TRUNCATED");
    case KSWORD_ARK_CID_ENUM_STATUS_BUDGET_EXHAUSTED: return QStringLiteral("BUDGET_EXHAUSTED");
    case KSWORD_ARK_CID_ENUM_STATUS_UNAVAILABLE:
    default:
        return QStringLiteral("UNAVAILABLE");
    }
}

QString KernelDockCidTab::objectSummaryStatusText(const std::uint32_t status)
{
    switch (status)
    {
    case KSWORD_ARK_OBJECT_SUMMARY_STATUS_OK: return QStringLiteral("OK");
    case KSWORD_ARK_OBJECT_SUMMARY_STATUS_PARTIAL: return QStringLiteral("PARTIAL");
    case KSWORD_ARK_OBJECT_SUMMARY_STATUS_UNSUPPORTED_TARGET: return QStringLiteral("UNSUPPORTED_TARGET");
    case KSWORD_ARK_OBJECT_SUMMARY_STATUS_LOOKUP_FAILED: return QStringLiteral("LOOKUP_FAILED");
    case KSWORD_ARK_OBJECT_SUMMARY_STATUS_TYPE_QUERY_FAILED: return QStringLiteral("TYPE_QUERY_FAILED");
    case KSWORD_ARK_OBJECT_SUMMARY_STATUS_COUNTERS_UNAVAILABLE: return QStringLiteral("COUNTERS_UNAVAILABLE");
    case KSWORD_ARK_OBJECT_SUMMARY_STATUS_UNAVAILABLE:
    default:
        return QStringLiteral("UNAVAILABLE");
    }
}

QString KernelDockCidTab::objectHeaderStatusText(const std::uint32_t status)
{
    switch (status)
    {
    case KSWORD_ARK_OBJECT_HEADER_STATUS_PROFILE_MISSING: return QStringLiteral("PROFILE_MISSING");
    case KSWORD_ARK_OBJECT_HEADER_STATUS_PARTIAL_PROFILE: return QStringLiteral("PARTIAL_PROFILE");
    case KSWORD_ARK_OBJECT_HEADER_STATUS_AVAILABLE: return QStringLiteral("AVAILABLE");
    case KSWORD_ARK_OBJECT_HEADER_STATUS_UNAVAILABLE:
    default:
        return QStringLiteral("UNAVAILABLE");
    }
}

QString KernelDockCidTab::fixedWideText(const wchar_t* text, const std::size_t maxChars)
{
    if (text == nullptr || maxChars == 0U)
    {
        return {};
    }
    std::size_t length = 0U;
    while (length < maxChars && text[length] != L'\0')
    {
        ++length;
    }
    return QString::fromWCharArray(text, static_cast<int>(length));
}

bool KernelDockCidTab::cidSummaryTruncated(const CidTableSummary& summary)
{
    return summary.returnedCount < summary.totalCount ||
        summary.status == KSWORD_ARK_CID_ENUM_STATUS_BUFFER_TRUNCATED ||
        summary.status == KSWORD_ARK_CID_ENUM_STATUS_BUDGET_EXHAUSTED;
}
