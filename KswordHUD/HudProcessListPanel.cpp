#include "HudProcessListPanel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QHeaderView>
#include <QHideEvent>
#include <QMenu>
#include <QPainter>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <QtConcurrent/QtConcurrentRun>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>

#include <algorithm>
#include <array>

#pragma comment(lib, "Psapi.lib")

namespace
{
    constexpr int kPidRole = Qt::UserRole + 1;
    constexpr int kUsageRatioRole = Qt::UserRole + 2;
    constexpr int kMetricColumnFirst = 2;
    constexpr int kMetricColumnLast = 6;

    QStringList processHeaders()
    {
        return {
            QStringLiteral("进程名"),
            QStringLiteral("PID"),
            QStringLiteral("CPU"),
            QStringLiteral("RAM"),
            QStringLiteral("DISK"),
            QStringLiteral("GPU"),
            QStringLiteral("Net")
        };
    }

    quint64 fileTimeToUint64(const FILETIME& fileTimeValue)
    {
        ULARGE_INTEGER unsignedValue{};
        unsignedValue.LowPart = fileTimeValue.dwLowDateTime;
        unsignedValue.HighPart = fileTimeValue.dwHighDateTime;
        return unsignedValue.QuadPart;
    }

    QColor usageRatioToHighlightColor(double usageRatio)
    {
        usageRatio = std::clamp(usageRatio, 0.0, 1.0);
        const int alphaValue = static_cast<int>(24.0 + usageRatio * 146.0);
        QColor highlightColor(46, 139, 255);
        highlightColor.setAlpha(alphaValue);
        return highlightColor;
    }

    class HudProcessMetricDelegate final : public QStyledItemDelegate
    {
    public:
        explicit HudProcessMetricDelegate(QObject* parent = nullptr)
            : QStyledItemDelegate(parent)
        {
        }

        void setTextColor(const QColor& colorValue)
        {
            m_textColor = colorValue;
        }

    protected:
        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            if (painter == nullptr)
            {
                return;
            }

            QStyleOptionViewItem viewOption(option);
            initStyleOption(&viewOption, index);
            viewOption.text.clear();

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);

            const bool selected = (option.state & QStyle::State_Selected) != 0;
            if (selected)
            {
                painter->fillRect(option.rect, QColor(89, 139, 214, 88));
            }

            const QVariant usageRatioVariant = index.data(kUsageRatioRole);
            if (usageRatioVariant.isValid())
            {
                const double usageRatio = std::clamp(usageRatioVariant.toDouble(), 0.0, 1.0);
                const QRectF trackRect = QRectF(option.rect.adjusted(6, 5, -6, -5));
                if (trackRect.width() > 2.0 && trackRect.height() > 2.0)
                {
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(QColor(255, 255, 255, 18));
                    painter->drawRoundedRect(trackRect, 8.0, 8.0);

                    QRectF fillRect = trackRect;
                    fillRect.setWidth(std::max(2.0, trackRect.width() * usageRatio));
                    painter->setBrush(usageRatioToHighlightColor(usageRatio));
                    painter->drawRoundedRect(fillRect, 8.0, 8.0);
                }
            }

            QColor textColor = m_textColor;
            if (selected)
            {
                textColor = QColor(255, 255, 255);
            }
            painter->setPen(textColor);

            const QRect textRect = option.rect.adjusted(10, 0, -10, 0);
            const int columnIndex = index.column();
            const int textFlags =
                (columnIndex >= kMetricColumnFirst ? Qt::AlignRight : Qt::AlignLeft)
                | Qt::AlignVCenter
                | Qt::TextSingleLine;
            painter->drawText(textRect, textFlags, index.data(Qt::DisplayRole).toString());
            painter->restore();
        }

    private:
        QColor m_textColor = QColor(255, 255, 255);
    };
}

HudProcessListPanel::HudProcessListPanel(QWidget* parent)
    : QWidget(parent)
{
    m_logicalCpuCount = static_cast<int>(std::max<DWORD>(
        1,
        ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));

    initializeUi();

    m_refreshWatcher = new QFutureWatcher<RefreshResult>(this);
    connect(m_refreshWatcher, &QFutureWatcher<RefreshResult>::finished, this, [this]() {
        m_refreshInProgress = false;
        applyRefreshResult(m_refreshWatcher->result());
        });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(1000);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        requestRefresh();
        });
}

HudProcessListPanel::~HudProcessListPanel()
{
    stopRefreshing();
}

void HudProcessListPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    startRefreshing();
}

void HudProcessListPanel::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    stopRefreshing();
}

void HudProcessListPanel::initializeUi()
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background:transparent;"));

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_treeWidget = new QTreeWidget(this);
    m_treeWidget->setColumnCount(ColumnCount);
    m_treeWidget->setHeaderLabels(processHeaders());
    m_treeWidget->setRootIsDecorated(false);
    m_treeWidget->setItemsExpandable(false);
    m_treeWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeWidget->setUniformRowHeights(true);
    m_treeWidget->setSortingEnabled(true);
    m_treeWidget->setAlternatingRowColors(false);
    m_treeWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_treeWidget->setFrameShape(QFrame::NoFrame);
    m_treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeWidget->setStyleSheet(QStringLiteral(
        "QTreeWidget{"
        "background:transparent;"
        "color:rgba(255,255,255,235);"
        "border:none;"
        "font:10pt \"Segoe UI\";"
        "outline:0;"
        "}"
        "QTreeWidget::item{height:28px;}"
        "QTreeWidget::item:selected{"
        "background:rgba(89,139,214,88);"
        "color:rgba(255,255,255,245);"
        "}"
        "QHeaderView::section{"
        "background:transparent;"
        "color:rgba(255,255,255,225);"
        "border-right:1px solid rgba(255,255,255,28);"
        "border-bottom:1px solid rgba(255,255,255,28);"
        "padding:4px 6px;"
        "}"
        ));

    auto* delegate = new HudProcessMetricDelegate(m_treeWidget);
    delegate->setTextColor(m_tableTextColor);
    m_metricDelegate = delegate;
    m_treeWidget->setItemDelegate(delegate);

    QHeaderView* headerView = m_treeWidget->header();
    if (headerView != nullptr)
    {
        headerView->setStretchLastSection(false);
        headerView->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
        headerView->setSectionResizeMode(PidColumn, QHeaderView::Fixed);
        headerView->setSectionResizeMode(CpuColumn, QHeaderView::Fixed);
        headerView->setSectionResizeMode(RamColumn, QHeaderView::Fixed);
        headerView->setSectionResizeMode(DiskColumn, QHeaderView::Fixed);
        headerView->setSectionResizeMode(GpuColumn, QHeaderView::Fixed);
        headerView->setSectionResizeMode(NetColumn, QHeaderView::Fixed);
        m_treeWidget->setColumnWidth(PidColumn, 80);
        m_treeWidget->setColumnWidth(CpuColumn, 80);
        m_treeWidget->setColumnWidth(RamColumn, 90);
        m_treeWidget->setColumnWidth(DiskColumn, 95);
        m_treeWidget->setColumnWidth(GpuColumn, 80);
        m_treeWidget->setColumnWidth(NetColumn, 95);
    }

    connect(m_treeWidget, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showContextMenu(localPosition);
        });

    rootLayout->addWidget(m_treeWidget, 1);
}

void HudProcessListPanel::setTableTextColor(const QColor& colorValue)
{
    m_tableTextColor = colorValue;
    auto* metricDelegate = dynamic_cast<HudProcessMetricDelegate*>(m_metricDelegate);
    if (metricDelegate != nullptr)
    {
        metricDelegate->setTextColor(colorValue);
    }

    if (m_treeWidget != nullptr)
    {
        m_treeWidget->setStyleSheet(QStringLiteral(
            "QTreeWidget{"
            "background:transparent;"
            "color:rgba(%1,%2,%3,%4);"
            "border:none;"
            "font:10pt \"Segoe UI\";"
            "outline:0;"
            "}"
            "QTreeWidget::item{height:28px;}"
            "QTreeWidget::item:selected{"
            "background:rgba(89,139,214,88);"
            "color:rgba(255,255,255,245);"
            "}"
            "QHeaderView::section{"
            "background:transparent;"
            "color:rgba(%1,%2,%3,225);"
            "border-right:1px solid rgba(255,255,255,28);"
            "border-bottom:1px solid rgba(255,255,255,28);"
            "padding:4px 6px;"
            "}").arg(colorValue.red()).arg(colorValue.green()).arg(colorValue.blue()).arg(colorValue.alpha()));
        m_treeWidget->viewport()->update();
    }
}

void HudProcessListPanel::showContextMenu(const QPoint& localPosition)
{
    if (m_treeWidget == nullptr)
    {
        return;
    }

    QTreeWidgetItem* itemPointer = m_treeWidget->itemAt(localPosition);
    if (itemPointer == nullptr)
    {
        return;
    }

    bool pidOk = false;
    const quint32 pidValue = itemPointer->data(NameColumn, kPidRole).toUInt(&pidOk);
    if (!pidOk || pidValue == 0)
    {
        return;
    }

    QMenu menu(m_treeWidget);
    menu.setAttribute(Qt::WA_TranslucentBackground, true);
    menu.setStyleSheet(QStringLiteral(
        "QMenu{"
        "background-color: rgba(24,28,34,236);"
        "border: 1px solid rgba(255,255,255,28);"
        "border-radius: 12px;"
        "padding: 6px;"
        "color: rgba(248,250,255,245);"
        "}"
        "QMenu::item{"
        "padding: 8px 28px 8px 12px;"
        "border-radius: 8px;"
        "background: transparent;"
        "}"
        "QMenu::item:selected{"
        "background-color: rgba(59,130,246,140);"
        "}"
        "QMenu::separator{"
        "height: 1px;"
        "background: rgba(255,255,255,18);"
        "margin: 4px 8px;"
        "}"));

    QAction* terminateAction = menu.addAction(QStringLiteral("结束进程"));
    QAction* selectedAction =
        menu.exec(m_treeWidget->viewport()->mapToGlobal(localPosition));
    if (selectedAction == terminateAction)
    {
        terminateProcessByPid(pidValue);
        requestRefresh();
    }
}

bool HudProcessListPanel::terminateProcessByPid(const quint32 processIdValue)
{
    const HANDLE processHandle = ::OpenProcess(PROCESS_TERMINATE, FALSE, processIdValue);
    if (processHandle == nullptr)
    {
        return false;
    }

    const BOOL terminateOk = ::TerminateProcess(processHandle, 1);
    ::CloseHandle(processHandle);
    return terminateOk != FALSE;
}

void HudProcessListPanel::startRefreshing()
{
    if (m_refreshTimer != nullptr && !m_refreshTimer->isActive())
    {
        m_refreshTimer->start();
    }
    requestRefresh();
}

void HudProcessListPanel::stopRefreshing()
{
    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->stop();
    }
}

void HudProcessListPanel::requestRefresh()
{
    if (m_refreshInProgress || m_refreshWatcher == nullptr)
    {
        return;
    }

    const QHash<quint32, CounterSample> previousSamples = m_previousSamples;
    const int logicalCpuCount = m_logicalCpuCount;

    m_refreshInProgress = true;
    m_refreshWatcher->setFuture(QtConcurrent::run([previousSamples, logicalCpuCount]() {
        return collectRefreshResult(previousSamples, logicalCpuCount);
        }));
}

HudProcessListPanel::RefreshResult HudProcessListPanel::collectRefreshResult(
    const QHash<quint32, CounterSample>& previousSamples,
    const int logicalCpuCount)
{
    RefreshResult result;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshotHandle == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    PROCESSENTRY32W processEntryNative{};
    processEntryNative.dwSize = sizeof(processEntryNative);
    if (::Process32FirstW(snapshotHandle, &processEntryNative) == FALSE)
    {
        ::CloseHandle(snapshotHandle);
        return result;
    }

    do
    {
        ProcessEntry entry{};
        entry.pid = static_cast<quint32>(processEntryNative.th32ProcessID);
        entry.processName = QString::fromWCharArray(processEntryNative.szExeFile);

        CounterSample nextSample{};
        nextSample.sampleMs = nowMs;

        HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE,
            static_cast<DWORD>(entry.pid));
        if (processHandle == nullptr)
        {
            processHandle = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                static_cast<DWORD>(entry.pid));
        }

        if (processHandle != nullptr)
        {
            PROCESS_MEMORY_COUNTERS_EX memoryInfo{};
            if (::GetProcessMemoryInfo(
                processHandle,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryInfo),
                sizeof(memoryInfo)) != FALSE)
            {
                entry.ramMB = static_cast<double>(memoryInfo.WorkingSetSize) / (1024.0 * 1024.0);
            }

            FILETIME creationTime{};
            FILETIME exitTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};
            if (::GetProcessTimes(
                processHandle,
                &creationTime,
                &exitTime,
                &kernelTime,
                &userTime) != FALSE)
            {
                nextSample.cpuTime100ns =
                    fileTimeToUint64(kernelTime) + fileTimeToUint64(userTime);
            }

            IO_COUNTERS ioCounters{};
            if (::GetProcessIoCounters(processHandle, &ioCounters) != FALSE)
            {
                nextSample.ioBytes =
                    static_cast<quint64>(ioCounters.ReadTransferCount + ioCounters.WriteTransferCount);
            }

            ::CloseHandle(processHandle);
        }

        const auto previousIterator = previousSamples.constFind(entry.pid);
        if (previousIterator != previousSamples.cend())
        {
            const qint64 elapsedMs = nowMs - previousIterator->sampleMs;
            if (elapsedMs > 0)
            {
                const double elapsedSeconds = static_cast<double>(elapsedMs) / 1000.0;
                const quint64 cpuDelta100ns =
                    nextSample.cpuTime100ns >= previousIterator->cpuTime100ns
                    ? (nextSample.cpuTime100ns - previousIterator->cpuTime100ns)
                    : 0;
                if (elapsedSeconds > 0.0 && logicalCpuCount > 0)
                {
                    entry.cpuPercent = std::clamp(
                        (static_cast<double>(cpuDelta100ns)
                            / (elapsedSeconds * 10000000.0 * static_cast<double>(logicalCpuCount))) * 100.0,
                        0.0,
                        100.0);
                }

                const quint64 ioDeltaBytes =
                    nextSample.ioBytes >= previousIterator->ioBytes
                    ? (nextSample.ioBytes - previousIterator->ioBytes)
                    : 0;
                entry.diskMBps =
                    (static_cast<double>(ioDeltaBytes) / std::max(0.001, elapsedSeconds)) / (1024.0 * 1024.0);
            }
        }

        entry.gpuPercent = 0.0;
        entry.netKBps = 0.0;

        result.entries.push_back(entry);
        result.nextSamples.insert(entry.pid, nextSample);
        result.totalCpuPercent += entry.cpuPercent;
        result.totalRamMB += entry.ramMB;
        result.totalDiskMBps += entry.diskMBps;
        result.totalGpuPercent += entry.gpuPercent;
        result.totalNetKBps += entry.netKBps;
        result.maxRamMB = std::max(result.maxRamMB, entry.ramMB);
        result.maxDiskMBps = std::max(result.maxDiskMBps, entry.diskMBps);
        result.maxNetKBps = std::max(result.maxNetKBps, entry.netKBps);
    } while (::Process32NextW(snapshotHandle, &processEntryNative) != FALSE);

    ::CloseHandle(snapshotHandle);
    return result;
}

void HudProcessListPanel::applyRefreshResult(const RefreshResult& result)
{
    if (m_treeWidget == nullptr)
    {
        return;
    }

    m_previousSamples = result.nextSamples;
    updateHeaderSummary(result);

    QSet<quint32> livePidSet;
    livePidSet.reserve(result.entries.size());

    m_treeWidget->setUpdatesEnabled(false);
    m_treeWidget->setSortingEnabled(false);

    for (const ProcessEntry& entry : result.entries)
    {
        livePidSet.insert(entry.pid);
        updateOrCreateRow(entry, result.maxRamMB, result.maxDiskMBps, result.maxNetKBps);
    }

    const QList<quint32> existingPidList = m_itemByPid.keys();
    for (const quint32 pidValue : existingPidList)
    {
        if (livePidSet.contains(pidValue))
        {
            continue;
        }

        QTreeWidgetItem* itemPointer = m_itemByPid.take(pidValue);
        delete itemPointer;
    }

    m_treeWidget->setSortingEnabled(true);
    m_treeWidget->sortItems(CpuColumn, Qt::DescendingOrder);
    m_treeWidget->setUpdatesEnabled(true);
    m_treeWidget->viewport()->update();
}

void HudProcessListPanel::updateHeaderSummary(const RefreshResult& result)
{
    if (m_treeWidget == nullptr || m_treeWidget->headerItem() == nullptr)
    {
        return;
    }

    QTreeWidgetItem* headerItem = m_treeWidget->headerItem();
    headerItem->setText(NameColumn, processHeaders().at(NameColumn));
    headerItem->setText(PidColumn, processHeaders().at(PidColumn));
    headerItem->setText(CpuColumn, QStringLiteral("CPU %1").arg(formatPercent(result.totalCpuPercent, 2)));
    headerItem->setText(RamColumn, QStringLiteral("RAM %1 MB").arg(QString::number(result.totalRamMB, 'f', 1)));
    headerItem->setText(DiskColumn, QStringLiteral("DISK %1 MB/s").arg(QString::number(result.totalDiskMBps, 'f', 2)));
    headerItem->setText(GpuColumn, QStringLiteral("GPU %1").arg(formatPercent(result.totalGpuPercent, 1)));
    headerItem->setText(NetColumn, QStringLiteral("Net %1 KB/s").arg(QString::number(result.totalNetKBps, 'f', 2)));
}

void HudProcessListPanel::updateOrCreateRow(
    const ProcessEntry& entry,
    const double maxRamMB,
    const double maxDiskMBps,
    const double maxNetKBps)
{
    QTreeWidgetItem* itemPointer = m_itemByPid.value(entry.pid, nullptr);
    if (itemPointer == nullptr)
    {
        itemPointer = new QTreeWidgetItem();
        itemPointer->setTextAlignment(PidColumn, Qt::AlignRight | Qt::AlignVCenter);
        itemPointer->setTextAlignment(CpuColumn, Qt::AlignRight | Qt::AlignVCenter);
        itemPointer->setTextAlignment(RamColumn, Qt::AlignRight | Qt::AlignVCenter);
        itemPointer->setTextAlignment(DiskColumn, Qt::AlignRight | Qt::AlignVCenter);
        itemPointer->setTextAlignment(GpuColumn, Qt::AlignRight | Qt::AlignVCenter);
        itemPointer->setTextAlignment(NetColumn, Qt::AlignRight | Qt::AlignVCenter);
        m_treeWidget->addTopLevelItem(itemPointer);
        m_itemByPid.insert(entry.pid, itemPointer);
    }

    itemPointer->setText(NameColumn, entry.processName);
    itemPointer->setText(PidColumn, QString::number(entry.pid));
    itemPointer->setText(CpuColumn, formatPercent(entry.cpuPercent, 2));
    itemPointer->setText(RamColumn, formatRamMB(entry.ramMB));
    itemPointer->setText(DiskColumn, formatDiskMBps(entry.diskMBps));
    itemPointer->setText(GpuColumn, formatPercent(entry.gpuPercent, 1));
    itemPointer->setText(NetColumn, formatNetKBps(entry.netKBps));
    itemPointer->setData(NameColumn, kPidRole, entry.pid);
    itemPointer->setData(CpuColumn, kUsageRatioRole, usageRatioForEntry(entry, CpuColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(RamColumn, kUsageRatioRole, usageRatioForEntry(entry, RamColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(DiskColumn, kUsageRatioRole, usageRatioForEntry(entry, DiskColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(GpuColumn, kUsageRatioRole, usageRatioForEntry(entry, GpuColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(NetColumn, kUsageRatioRole, usageRatioForEntry(entry, NetColumn, maxRamMB, maxDiskMBps, maxNetKBps));
}

double HudProcessListPanel::usageRatioForEntry(
    const ProcessEntry& entry,
    const int columnIndex,
    const double maxRamMB,
    const double maxDiskMBps,
    const double maxNetKBps)
{
    switch (columnIndex)
    {
    case CpuColumn:
        return std::clamp(entry.cpuPercent / 100.0, 0.0, 1.0);
    case RamColumn:
        return maxRamMB > 0.0 ? std::clamp(entry.ramMB / maxRamMB, 0.0, 1.0) : 0.0;
    case DiskColumn:
        return maxDiskMBps > 0.0 ? std::clamp(entry.diskMBps / maxDiskMBps, 0.0, 1.0) : 0.0;
    case GpuColumn:
        return std::clamp(entry.gpuPercent / 100.0, 0.0, 1.0);
    case NetColumn:
        return maxNetKBps > 0.0 ? std::clamp(entry.netKBps / maxNetKBps, 0.0, 1.0) : 0.0;
    default:
        return 0.0;
    }
}

QString HudProcessListPanel::formatPercent(const double value, const int decimals)
{
    return QStringLiteral("%1%").arg(QString::number(value, 'f', decimals));
}

QString HudProcessListPanel::formatRamMB(const double value)
{
    return QStringLiteral("%1 MB").arg(QString::number(value, 'f', 1));
}

QString HudProcessListPanel::formatDiskMBps(const double value)
{
    return QStringLiteral("%1 MB/s").arg(QString::number(value, 'f', 2));
}

QString HudProcessListPanel::formatNetKBps(const double value)
{
    return QStringLiteral("%1 KB/s").arg(QString::number(value, 'f', 2));
}
