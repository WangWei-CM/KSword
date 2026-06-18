#include "HudProcessListPanel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
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
#include <cmath>

#pragma comment(lib, "Psapi.lib")

namespace
{
    constexpr int kNameColumn = 0;
    constexpr int kPidRole = Qt::UserRole + 1;
    constexpr int kUsageRatioRole = Qt::UserRole + 2;
    constexpr int kExpansionKeyRole = Qt::UserRole + 3;
    constexpr int kTerminatePidListRole = Qt::UserRole + 4;
    constexpr int kMetricColumnFirst = 2;
    constexpr int kMetricColumnLast = 6;

    struct VisibleWindowEnumContext
    {
        QSet<quint32>* pidSet = nullptr;
    };

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

    // EnumWindows callback used by collectVisibleWindowPidSet().
    // Input: hwndValue is a candidate top-level window, lParamValue points to VisibleWindowEnumContext.
    // Processing: keep normal visible owner/root windows with a non-empty title and record their owning PID.
    // Return: TRUE keeps enumeration running; FALSE is never used because one bad window must not stop refresh.
    BOOL CALLBACK collectVisibleWindowProc(HWND hwndValue, LPARAM lParamValue)
    {
        auto* contextPointer = reinterpret_cast<VisibleWindowEnumContext*>(lParamValue);
        if (contextPointer == nullptr || contextPointer->pidSet == nullptr)
        {
            return TRUE;
        }

        if (hwndValue == nullptr || ::IsWindowVisible(hwndValue) == FALSE)
        {
            return TRUE;
        }

        if (::GetAncestor(hwndValue, GA_ROOTOWNER) != hwndValue)
        {
            return TRUE;
        }

        const LONG_PTR extendedStyle = ::GetWindowLongPtrW(hwndValue, GWL_EXSTYLE);
        if ((extendedStyle & WS_EX_TOOLWINDOW) != 0)
        {
            return TRUE;
        }

        if (::GetWindowTextLengthW(hwndValue) <= 0)
        {
            return TRUE;
        }

        DWORD processIdValue = 0;
        ::GetWindowThreadProcessId(hwndValue, &processIdValue);
        if (processIdValue != 0)
        {
            contextPointer->pidSet->insert(static_cast<quint32>(processIdValue));
        }

        return TRUE;
    }

    // Collects process IDs that own normal visible top-level windows.
    // Input: none; the current desktop window list is read through user32.
    // Processing: EnumWindows filters out invisible/tool/owned/titleless windows to approximate Task Manager apps.
    // Return: a PID set used as application roots; an empty set is valid when no window can be enumerated.
    QSet<quint32> collectVisibleWindowPidSet()
    {
        QSet<quint32> visibleWindowPidSet;
        VisibleWindowEnumContext context{ &visibleWindowPidSet };
        ::EnumWindows(collectVisibleWindowProc, reinterpret_cast<LPARAM>(&context));
        return visibleWindowPidSet;
    }

    // Reads the Windows installation directory in normalized lower-case form.
    // Input: none; GetWindowsDirectoryW supplies the local Windows path.
    // Processing: convert backslashes to forward slashes so path-prefix checks are stable.
    // Return: lower-case Windows directory path, or "c:/windows" as a defensive fallback.
    QString normalizedWindowsDirectoryPath()
    {
        std::array<wchar_t, MAX_PATH> windowsPathBuffer{};
        const UINT pathLength = ::GetWindowsDirectoryW(
            windowsPathBuffer.data(),
            static_cast<UINT>(windowsPathBuffer.size()));
        QString windowsDirectoryPath =
            pathLength > 0
            ? QString::fromWCharArray(windowsPathBuffer.data(), static_cast<int>(pathLength))
            : QStringLiteral("C:\\Windows");
        return QDir::fromNativeSeparators(windowsDirectoryPath).toLower();
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

            const int columnIndex = index.column();
            QRect textRect = option.rect.adjusted(10, 0, -10, 0);
            if (columnIndex == kNameColumn)
            {
                const QVariant decorationVariant = index.data(Qt::DecorationRole);
                if (decorationVariant.canConvert<QIcon>())
                {
                    const QIcon iconValue = qvariant_cast<QIcon>(decorationVariant);
                    if (!iconValue.isNull())
                    {
                        const int iconSize = std::min(18, std::max(14, option.rect.height() - 8));
                        const QRect iconRect(
                            option.rect.left() + 8,
                            option.rect.top() + (option.rect.height() - iconSize) / 2,
                            iconSize,
                            iconSize);
                        iconValue.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal, QIcon::On);
                        textRect.adjust(iconSize + 8, 0, 0, 0);
                    }
                }
            }
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

    m_fileIconProvider = new QFileIconProvider();
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
    delete m_fileIconProvider;
    m_fileIconProvider = nullptr;
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
    m_treeWidget->setRootIsDecorated(true);
    m_treeWidget->setItemsExpandable(true);
    m_treeWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeWidget->setUniformRowHeights(true);
    m_treeWidget->setSortingEnabled(false);
    m_treeWidget->setAlternatingRowColors(false);
    m_treeWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_treeWidget->setFrameShape(QFrame::NoFrame);
    m_treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    applyTreeWidgetStyle();

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

void HudProcessListPanel::applyTreeWidgetStyle()
{
    // Inputs: current process-table text color.
    // Processing: applies the full tree, header, item, and custom scrollbar stylesheet in one place.
    // Return behavior: no value is returned; the QTreeWidget visual style is updated when the widget exists.
    if (m_treeWidget == nullptr)
    {
        return;
    }

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
        "}"
        "QScrollBar:vertical{"
        "background:rgba(255,255,255,10);"
        "width:10px;"
        "margin:4px 2px 4px 2px;"
        "border-radius:5px;"
        "}"
        "QScrollBar::handle:vertical{"
        "background:rgba(96,165,250,150);"
        "min-height:32px;"
        "border-radius:5px;"
        "}"
        "QScrollBar::handle:vertical:hover{"
        "background:rgba(125,185,255,210);"
        "}"
        "QScrollBar::add-line:vertical,"
        "QScrollBar::sub-line:vertical{"
        "height:0px;"
        "background:transparent;"
        "border:none;"
        "}"
        "QScrollBar::add-page:vertical,"
        "QScrollBar::sub-page:vertical{"
        "background:transparent;"
        "}"
        "QScrollBar:horizontal{"
        "height:0px;"
        "background:transparent;"
        "}").arg(m_tableTextColor.red()).arg(m_tableTextColor.green()).arg(m_tableTextColor.blue()).arg(m_tableTextColor.alpha()));
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
        applyTreeWidgetStyle();
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
    QVector<quint32> terminatePidList;
    const QVariant terminatePidListVariant = itemPointer->data(NameColumn, kTerminatePidListRole);
    if (terminatePidListVariant.canConvert<QVariantList>())
    {
        const QVariantList variantList = terminatePidListVariant.toList();
        terminatePidList.reserve(variantList.size());
        for (const QVariant& pidVariant : variantList)
        {
            bool childPidOk = false;
            const quint32 childPidValue = pidVariant.toUInt(&childPidOk);
            if (childPidOk && childPidValue != 0)
            {
                terminatePidList.push_back(childPidValue);
            }
        }
    }
    if (terminatePidList.isEmpty() && pidOk && pidValue != 0)
    {
        terminatePidList.push_back(pidValue);
    }
    if (terminatePidList.isEmpty())
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

    QAction* terminateAction = menu.addAction(
        terminatePidList.size() > 1
        ? QStringLiteral("结束此应用")
        : QStringLiteral("结束进程"));
    QAction* selectedAction =
        menu.exec(m_treeWidget->viewport()->mapToGlobal(localPosition));
    if (selectedAction == terminateAction)
    {
        terminateProcessesByPidList(terminatePidList);
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

int HudProcessListPanel::terminateProcessesByPidList(const QVector<quint32>& processIdList)
{
    // Inputs: a list of process IDs, usually one real process row or all children of one application row.
    // Processing: terminate each PID independently in reverse order so child/helper processes are attempted first.
    // Return: the number of processes for which TerminateProcess returned success.
    int terminatedCount = 0;
    for (auto reverseIterator = processIdList.crbegin();
        reverseIterator != processIdList.crend();
        ++reverseIterator)
    {
        if (terminateProcessByPid(*reverseIterator))
        {
            ++terminatedCount;
        }
    }
    return terminatedCount;
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

    if ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0)
    {
        return;
    }

    const QHash<quint32, CounterSample> previousSamples = m_previousSamples;
    const QHash<QString, QString> cachedImagePathByIdentity = m_imagePathByIdentity;
    const int logicalCpuCount = m_logicalCpuCount;

    m_refreshInProgress = true;
    m_refreshWatcher->setFuture(QtConcurrent::run([previousSamples, cachedImagePathByIdentity, logicalCpuCount]() {
        return collectRefreshResult(previousSamples, cachedImagePathByIdentity, logicalCpuCount);
        }));
}

void HudProcessListPanel::captureExpandedState()
{
    // Inputs: current QTreeWidget state before the refresh rebuild clears all items.
    // Processing: recursively records expansion for every row that carries a stable expansion key.
    // Return behavior: no value is returned; m_expandedStateByKey is updated in place.
    if (m_treeWidget == nullptr)
    {
        return;
    }

    m_expandedStateByKey.clear();
    for (int itemIndex = 0; itemIndex < m_treeWidget->topLevelItemCount(); ++itemIndex)
    {
        captureExpandedStateForItem(m_treeWidget->topLevelItem(itemIndex));
    }
}

void HudProcessListPanel::captureExpandedStateForItem(QTreeWidgetItem* itemPointer)
{
    // Inputs: one tree item from the pre-refresh tree.
    // Processing: saves this item's expanded state by key, then processes child rows.
    // Return behavior: no value is returned; invalid or unkeyed rows are skipped.
    if (itemPointer == nullptr)
    {
        return;
    }

    const QString stateKey = itemPointer->data(NameColumn, kExpansionKeyRole).toString();
    if (!stateKey.isEmpty())
    {
        m_expandedStateByKey.insert(stateKey, itemPointer->isExpanded());
    }

    for (int childIndex = 0; childIndex < itemPointer->childCount(); ++childIndex)
    {
        captureExpandedStateForItem(itemPointer->child(childIndex));
    }
}

void HudProcessListPanel::restoreExpandedState(
    QTreeWidgetItem* itemPointer,
    const QString& stateKey,
    const bool defaultExpanded)
{
    // Inputs: a newly-created tree item, its stable expansion key, and the default state for first appearance.
    // Processing: stores the key on the row and applies a previously captured state when present.
    // Return behavior: no value is returned; null rows are ignored.
    if (itemPointer == nullptr)
    {
        return;
    }

    itemPointer->setData(NameColumn, kExpansionKeyRole, stateKey);
    itemPointer->setExpanded(m_expandedStateByKey.value(stateKey, defaultExpanded));
}

HudProcessListPanel::RefreshResult HudProcessListPanel::collectRefreshResult(
    const QHash<quint32, CounterSample>& previousSamples,
    const QHash<QString, QString>& cachedImagePathByIdentity,
    const int logicalCpuCount)
{
    RefreshResult result;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QSet<quint32> visibleWindowPidSet = collectVisibleWindowPidSet();
    const QString windowsDirectoryPath = normalizedWindowsDirectoryPath();

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
        entry.parentPid = static_cast<quint32>(processEntryNative.th32ParentProcessID);
        entry.processName = QString::fromWCharArray(processEntryNative.szExeFile);
        const QString processIdentityKey =
            buildProcessIdentityKey(entry.pid, entry.processName);
        entry.imagePath = cachedImagePathByIdentity.value(processIdentityKey);

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
            if (entry.imagePath.isEmpty())
            {
                std::array<wchar_t, 32768> imagePathBuffer{};
                DWORD imagePathLength = static_cast<DWORD>(imagePathBuffer.size());
                if (::QueryFullProcessImageNameW(
                    processHandle,
                    0,
                    imagePathBuffer.data(),
                    &imagePathLength) != FALSE
                    && imagePathLength > 0)
                {
                    entry.imagePath = QString::fromWCharArray(
                        imagePathBuffer.data(),
                        static_cast<int>(imagePathLength));
                }
            }

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

        entry.groupType = isWindowsSystemProcess(entry, windowsDirectoryPath)
            ? ProcessGroupType::WindowsSystem
            : ProcessGroupType::Background;

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
    classifyProcessGroups(&result.entries, visibleWindowPidSet);
    return result;
}

void HudProcessListPanel::applyRefreshResult(const RefreshResult& result)
{
    if (m_treeWidget == nullptr)
    {
        return;
    }

    if ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0)
    {
        return;
    }

    captureExpandedState();
    m_previousSamples = result.nextSamples;
    updateHeaderSummary(result);
    m_imagePathByIdentity.clear();
    m_itemByPid.clear();

    m_treeWidget->setUpdatesEnabled(false);
    m_treeWidget->setSortingEnabled(false);
    m_treeWidget->clear();

    const QVector<ProcessEntry> entries = result.entries;
    QVector<ProcessEntry> applicationEntries;
    QVector<ProcessEntry> backgroundEntries;
    QVector<ProcessEntry> systemEntries;
    applicationEntries.reserve(entries.size());
    backgroundEntries.reserve(entries.size());
    systemEntries.reserve(entries.size());

    for (const ProcessEntry& entry : entries)
    {
        m_imagePathByIdentity.insert(buildProcessIdentityKey(entry.pid, entry.processName), entry.imagePath);
        switch (entry.groupType)
        {
        case ProcessGroupType::Application:
            applicationEntries.push_back(entry);
            break;
        case ProcessGroupType::WindowsSystem:
            systemEntries.push_back(entry);
            break;
        case ProcessGroupType::Background:
        default:
            backgroundEntries.push_back(entry);
            break;
        }
    }

    const auto processSorter = [](const ProcessEntry& left, const ProcessEntry& right)
    {
        if (std::abs(left.cpuPercent - right.cpuPercent) > 0.001)
        {
            return left.cpuPercent > right.cpuPercent;
        }
        if (std::abs(left.ramMB - right.ramMB) > 0.001)
        {
            return left.ramMB > right.ramMB;
        }
        if (left.processName.compare(right.processName, Qt::CaseInsensitive) != 0)
        {
            return left.processName.compare(right.processName, Qt::CaseInsensitive) < 0;
        }
        return left.pid < right.pid;
    };

    std::sort(applicationEntries.begin(), applicationEntries.end(), processSorter);
    std::sort(backgroundEntries.begin(), backgroundEntries.end(), processSorter);
    std::sort(systemEntries.begin(), systemEntries.end(), processSorter);

    QHash<quint32, QVector<ProcessEntry>> applicationEntriesByRootPid;
    applicationEntriesByRootPid.reserve(applicationEntries.size());
    for (const ProcessEntry& entry : applicationEntries)
    {
        const quint32 rootPidValue = entry.applicationRootPid != 0 ? entry.applicationRootPid : entry.pid;
        applicationEntriesByRootPid[rootPidValue].push_back(entry);
    }

    QVector<QVector<ProcessEntry>> applicationBuckets;
    applicationBuckets.reserve(applicationEntriesByRootPid.size());
    for (auto bucketIterator = applicationEntriesByRootPid.cbegin();
        bucketIterator != applicationEntriesByRootPid.cend();
        ++bucketIterator)
    {
        applicationBuckets.push_back(bucketIterator.value());
    }
    std::sort(applicationBuckets.begin(), applicationBuckets.end(), [processSorter](const QVector<ProcessEntry>& left, const QVector<ProcessEntry>& right)
        {
            const ProcessEntry leftAggregate = aggregateApplicationEntry(left);
            const ProcessEntry rightAggregate = aggregateApplicationEntry(right);
            return processSorter(leftAggregate, rightAggregate);
        });

    QTreeWidgetItem* applicationGroupItem =
        createProcessGroupItem(ProcessGroupType::Application, applicationBuckets.size());
    if (applicationGroupItem != nullptr)
    {
        m_treeWidget->addTopLevelItem(applicationGroupItem);
        restoreExpandedState(
            applicationGroupItem,
            expansionKeyForGroup(ProcessGroupType::Application),
            true);

        for (QVector<ProcessEntry>& applicationBucket : applicationBuckets)
        {
            std::sort(applicationBucket.begin(), applicationBucket.end(), processSorter);
            const ProcessEntry aggregateEntry = aggregateApplicationEntry(applicationBucket);
            QTreeWidgetItem* applicationRootItem = createApplicationRootItem(
                aggregateEntry,
                applicationBucket,
                applicationBucket.size(),
                result.maxRamMB,
                result.maxDiskMBps,
                result.maxNetKBps);
            if (applicationRootItem == nullptr)
            {
                continue;
            }

            applicationGroupItem->addChild(applicationRootItem);
            restoreExpandedState(
                applicationRootItem,
                expansionKeyForApplication(aggregateEntry.pid),
                false);

            QHash<quint32, QTreeWidgetItem*> childItemByPid;
            childItemByPid.reserve(applicationBucket.size());
            QHash<quint32, QVector<ProcessEntry>> childrenByParentPid;
            childrenByParentPid.reserve(applicationBucket.size());
            for (const ProcessEntry& childEntry : applicationBucket)
            {
                childrenByParentPid[childEntry.parentPid].push_back(childEntry);
            }

            const auto appendChildrenRecursive =
                [&](const auto& self, const quint32 parentPidValue, QTreeWidgetItem* parentTreeItem) -> void
            {
                QVector<ProcessEntry> childEntries = childrenByParentPid.value(parentPidValue);
                std::sort(childEntries.begin(), childEntries.end(), processSorter);
                for (const ProcessEntry& childEntry : childEntries)
                {
                    if (childItemByPid.contains(childEntry.pid))
                    {
                        continue;
                    }

                    QTreeWidgetItem* childTreeItem = updateOrCreateRow(
                        childEntry,
                        parentTreeItem,
                        result.maxRamMB,
                        result.maxDiskMBps,
                        result.maxNetKBps);
                    if (childTreeItem == nullptr)
                    {
                        continue;
                    }

                    restoreExpandedState(
                        childTreeItem,
                        expansionKeyForProcess(childEntry.pid),
                        false);
                    childItemByPid.insert(childEntry.pid, childTreeItem);
                    self(self, childEntry.pid, childTreeItem);
                }
            };

            const auto rootEntryIterator = std::find_if(
                applicationBucket.cbegin(),
                applicationBucket.cend(),
                [&aggregateEntry](const ProcessEntry& childEntry)
                {
                    return childEntry.pid == aggregateEntry.pid;
                });
            if (rootEntryIterator != applicationBucket.cend())
            {
                QTreeWidgetItem* rootProcessItem = updateOrCreateRow(
                    *rootEntryIterator,
                    applicationRootItem,
                    result.maxRamMB,
                    result.maxDiskMBps,
                    result.maxNetKBps);
                if (rootProcessItem != nullptr)
                {
                    restoreExpandedState(
                        rootProcessItem,
                        expansionKeyForProcess(rootEntryIterator->pid),
                        false);
                    childItemByPid.insert(rootEntryIterator->pid, rootProcessItem);
                    appendChildrenRecursive(appendChildrenRecursive, rootEntryIterator->pid, rootProcessItem);
                }
            }

            for (const ProcessEntry& childEntry : applicationBucket)
            {
                if (childItemByPid.contains(childEntry.pid))
                {
                    continue;
                }

                QTreeWidgetItem* childTreeItem = updateOrCreateRow(
                    childEntry,
                    applicationRootItem,
                    result.maxRamMB,
                    result.maxDiskMBps,
                    result.maxNetKBps);
                if (childTreeItem == nullptr)
                {
                    continue;
                }

                restoreExpandedState(
                    childTreeItem,
                    expansionKeyForProcess(childEntry.pid),
                    false);
                childItemByPid.insert(childEntry.pid, childTreeItem);
                appendChildrenRecursive(appendChildrenRecursive, childEntry.pid, childTreeItem);
            }
        }
    }

    const struct GroupBucket
    {
        ProcessGroupType type;
        const QVector<ProcessEntry>* entries;
    } groupBuckets[] = {
        { ProcessGroupType::Background, &backgroundEntries },
        { ProcessGroupType::WindowsSystem, &systemEntries }
    };

    for (const GroupBucket& bucket : groupBuckets)
    {
        QTreeWidgetItem* groupItem = createProcessGroupItem(bucket.type, bucket.entries->size());
        if (groupItem == nullptr)
        {
            continue;
        }
        m_treeWidget->addTopLevelItem(groupItem);
        restoreExpandedState(
            groupItem,
            expansionKeyForGroup(bucket.type),
            true);

        for (const ProcessEntry& entry : *bucket.entries)
        {
            updateOrCreateRow(entry, groupItem, result.maxRamMB, result.maxDiskMBps, result.maxNetKBps);
        }
    }

    m_treeWidget->setSortingEnabled(false);
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

void HudProcessListPanel::classifyProcessGroups(
    QVector<ProcessEntry>* entries,
    const QSet<quint32>& visibleWindowPidSet)
{
    // Inputs: collected process entries and PIDs that own visible top-level windows.
    // Processing: build a PID -> parent PID index, then mark visible-window owners and their descendants as apps.
    // Return behavior: no value is returned; each ProcessEntry::groupType is updated in place.
    if (entries == nullptr)
    {
        return;
    }

    QHash<quint32, quint32> parentPidByPid;
    parentPidByPid.reserve(entries->size());
    for (const ProcessEntry& entry : *entries)
    {
        parentPidByPid.insert(entry.pid, entry.parentPid);
    }

    for (ProcessEntry& entry : *entries)
    {
        const quint32 applicationRootPidValue =
            findApplicationRootPid(entry.pid, parentPidByPid, visibleWindowPidSet);
        if (applicationRootPidValue != 0)
        {
            entry.groupType = ProcessGroupType::Application;
            entry.applicationRootPid = applicationRootPidValue;
        }
    }
}

quint32 HudProcessListPanel::findApplicationRootPid(
    const quint32 pidValue,
    const QHash<quint32, quint32>& parentPidByPid,
    const QSet<quint32>& visibleWindowPidSet)
{
    // Inputs: a PID, parent map collected from ToolHelp, and visible-window application root PIDs.
    // Processing: walk the parent chain defensively and stop on cycles, missing parents, or PID zero.
    // Return: the visible-window root PID when the PID belongs to an application tree; otherwise 0.
    QSet<quint32> visitedPidSet;
    quint32 currentPidValue = pidValue;

    while (currentPidValue != 0 && !visitedPidSet.contains(currentPidValue))
    {
        if (visibleWindowPidSet.contains(currentPidValue))
        {
            return currentPidValue;
        }

        visitedPidSet.insert(currentPidValue);
        const auto parentIterator = parentPidByPid.constFind(currentPidValue);
        if (parentIterator == parentPidByPid.cend())
        {
            break;
        }

        const quint32 parentPidValue = parentIterator.value();
        if (parentPidValue == currentPidValue)
        {
            break;
        }
        currentPidValue = parentPidValue;
    }

    return 0;
}

bool HudProcessListPanel::isWindowsSystemProcess(
    const ProcessEntry& entry,
    const QString& windowsDirectoryPath)
{
    // Inputs: one process entry and the normalized Windows directory path.
    // Processing: classify kernel/session-manager/core-service names and Windows-directory images as system.
    // Return: true when the process should appear under the Windows system group before app overrides.
    if (entry.pid == 0 || entry.pid == 4)
    {
        return true;
    }

    const QString processName = entry.processName.trimmed().toLower();
    static const QSet<QString> kSystemProcessNames = {
        QStringLiteral("system"),
        QStringLiteral("registry"),
        QStringLiteral("smss.exe"),
        QStringLiteral("csrss.exe"),
        QStringLiteral("wininit.exe"),
        QStringLiteral("winlogon.exe"),
        QStringLiteral("services.exe"),
        QStringLiteral("lsass.exe"),
        QStringLiteral("lsaiso.exe"),
        QStringLiteral("fontdrvhost.exe"),
        QStringLiteral("dwm.exe"),
        QStringLiteral("wudfhost.exe"),
        QStringLiteral("audiodg.exe"),
        QStringLiteral("memory compression")
    };
    if (kSystemProcessNames.contains(processName))
    {
        return true;
    }

    if (entry.imagePath.isEmpty() || windowsDirectoryPath.isEmpty())
    {
        return false;
    }

    const QString normalizedImagePath = QDir::fromNativeSeparators(entry.imagePath).toLower();
    return normalizedImagePath.startsWith(windowsDirectoryPath + QStringLiteral("/"));
}

HudProcessListPanel::ProcessEntry HudProcessListPanel::aggregateApplicationEntry(
    const QVector<ProcessEntry>& applicationEntries)
{
    // Inputs: all ProcessEntry objects belonging to one visible application root.
    // Processing: choose the root process as the display identity and sum metrics across all child processes.
    // Return: an aggregate ProcessEntry used only for the application parent row.
    ProcessEntry aggregateEntry{};
    if (applicationEntries.isEmpty())
    {
        return aggregateEntry;
    }

    aggregateEntry = applicationEntries.first();
    const quint32 rootPidValue =
        aggregateEntry.applicationRootPid != 0 ? aggregateEntry.applicationRootPid : aggregateEntry.pid;
    for (const ProcessEntry& entry : applicationEntries)
    {
        if (entry.pid == rootPidValue)
        {
            aggregateEntry = entry;
            break;
        }
    }

    aggregateEntry.pid = rootPidValue;
    aggregateEntry.parentPid = 0;
    aggregateEntry.applicationRootPid = rootPidValue;
    aggregateEntry.groupType = ProcessGroupType::Application;
    aggregateEntry.cpuPercent = 0.0;
    aggregateEntry.ramMB = 0.0;
    aggregateEntry.diskMBps = 0.0;
    aggregateEntry.gpuPercent = 0.0;
    aggregateEntry.netKBps = 0.0;
    for (const ProcessEntry& entry : applicationEntries)
    {
        aggregateEntry.cpuPercent += entry.cpuPercent;
        aggregateEntry.ramMB += entry.ramMB;
        aggregateEntry.diskMBps += entry.diskMBps;
        aggregateEntry.gpuPercent += entry.gpuPercent;
        aggregateEntry.netKBps += entry.netKBps;
    }

    return aggregateEntry;
}

QString HudProcessListPanel::expansionKeyForGroup(const ProcessGroupType groupType)
{
    // Inputs: one top-level process group type.
    // Processing: map the enum to a stable string that survives full refresh rebuilds.
    // Return: expansion-state key for the group row.
    switch (groupType)
    {
    case ProcessGroupType::Application:
        return QStringLiteral("group:application");
    case ProcessGroupType::WindowsSystem:
        return QStringLiteral("group:system");
    case ProcessGroupType::Background:
    default:
        return QStringLiteral("group:background");
    }
}

QString HudProcessListPanel::expansionKeyForApplication(const quint32 rootPidValue)
{
    // Inputs: an application root PID.
    // Processing: format a stable key for one Task-Manager-style application row.
    // Return: expansion-state key for that application aggregate node.
    return QStringLiteral("app:%1").arg(rootPidValue);
}

QString HudProcessListPanel::expansionKeyForProcess(const quint32 pidValue)
{
    // Inputs: a concrete process PID.
    // Processing: format a stable key for process rows that may own child rows.
    // Return: expansion-state key for that process node.
    return QStringLiteral("pid:%1").arg(pidValue);
}

QString HudProcessListPanel::processGroupTitle(const ProcessGroupType groupType, const int entryCount)
{
    // Inputs: process group type and number of processes currently assigned to that group.
    // Processing: format the localized group caption used by the left process tree.
    // Return: display text for the collapsible group row.
    switch (groupType)
    {
    case ProcessGroupType::Application:
        return QStringLiteral("应用 (%1)").arg(entryCount);
    case ProcessGroupType::WindowsSystem:
        return QStringLiteral("系统 (%1)").arg(entryCount);
    case ProcessGroupType::Background:
    default:
        return QStringLiteral("后台进程 (%1)").arg(entryCount);
    }
}

int HudProcessListPanel::processGroupOrder(const ProcessGroupType groupType)
{
    // Inputs: a group type enum.
    // Processing: mirror Task Manager ordering so apps stay above background and system rows.
    // Return: a stable numeric order for possible future sorting or diagnostics.
    switch (groupType)
    {
    case ProcessGroupType::Application:
        return 0;
    case ProcessGroupType::Background:
        return 1;
    case ProcessGroupType::WindowsSystem:
    default:
        return 2;
    }
}

QTreeWidgetItem* HudProcessListPanel::createProcessGroupItem(
    const ProcessGroupType groupType,
    const int entryCount)
{
    // Inputs: process group type and group size.
    // Processing: create a non-process parent row with a bold blue caption and no PID role.
    // Return: a heap-allocated tree item owned by QTreeWidget after insertion, or nullptr only on allocation failure.
    auto* groupItem = new QTreeWidgetItem();
    groupItem->setText(NameColumn, processGroupTitle(groupType, entryCount));
    groupItem->setData(NameColumn, kPidRole, QVariant());
    groupItem->setData(NameColumn, Qt::UserRole + 8, processGroupOrder(groupType));
    groupItem->setFirstColumnSpanned(true);
    groupItem->setFlags((groupItem->flags() | Qt::ItemIsEnabled) & ~Qt::ItemIsSelectable);

    QFont groupFont = groupItem->font(NameColumn);
    groupFont.setBold(true);
    groupItem->setFont(NameColumn, groupFont);
    groupItem->setForeground(NameColumn, QColor(96, 165, 250));
    groupItem->setBackground(NameColumn, QColor(255, 255, 255, 12));
    return groupItem;
}

QTreeWidgetItem* HudProcessListPanel::createApplicationRootItem(
    const ProcessEntry& aggregateEntry,
    const QVector<ProcessEntry>& applicationEntries,
    const int childProcessCount,
    const double maxRamMB,
    const double maxDiskMBps,
    const double maxNetKBps)
{
    // Inputs: aggregate metrics for one application tree, child count, and metric normalization maxima.
    // Processing: create a virtual parent row that mirrors Task Manager's "App name (N)" row.
    // Return: a heap-allocated tree item that the caller inserts under the Applications group.
    auto* itemPointer = new QTreeWidgetItem();
    itemPointer->setTextAlignment(PidColumn, Qt::AlignRight | Qt::AlignVCenter);
    itemPointer->setTextAlignment(CpuColumn, Qt::AlignRight | Qt::AlignVCenter);
    itemPointer->setTextAlignment(RamColumn, Qt::AlignRight | Qt::AlignVCenter);
    itemPointer->setTextAlignment(DiskColumn, Qt::AlignRight | Qt::AlignVCenter);
    itemPointer->setTextAlignment(GpuColumn, Qt::AlignRight | Qt::AlignVCenter);
    itemPointer->setTextAlignment(NetColumn, Qt::AlignRight | Qt::AlignVCenter);

    itemPointer->setText(
        NameColumn,
        childProcessCount > 1
        ? QStringLiteral("%1 (%2)").arg(aggregateEntry.processName).arg(childProcessCount)
        : aggregateEntry.processName);
    itemPointer->setText(PidColumn, QString::number(aggregateEntry.pid));
    itemPointer->setIcon(NameColumn, resolveProcessIcon(aggregateEntry));
    itemPointer->setText(CpuColumn, formatPercent(aggregateEntry.cpuPercent, 2));
    itemPointer->setText(RamColumn, formatRamMB(aggregateEntry.ramMB));
    itemPointer->setText(DiskColumn, formatDiskMBps(aggregateEntry.diskMBps));
    itemPointer->setText(GpuColumn, formatPercent(aggregateEntry.gpuPercent, 1));
    itemPointer->setText(NetColumn, formatNetKBps(aggregateEntry.netKBps));
    itemPointer->setData(NameColumn, kPidRole, QVariant());
    QVariantList terminatePidVariantList;
    terminatePidVariantList.reserve(applicationEntries.size());
    for (const ProcessEntry& entry : applicationEntries)
    {
        if (entry.pid != 0)
        {
            terminatePidVariantList.push_back(entry.pid);
        }
    }
    itemPointer->setData(NameColumn, kTerminatePidListRole, terminatePidVariantList);
    itemPointer->setData(CpuColumn, kUsageRatioRole, usageRatioForEntry(aggregateEntry, CpuColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(RamColumn, kUsageRatioRole, usageRatioForEntry(aggregateEntry, RamColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(DiskColumn, kUsageRatioRole, usageRatioForEntry(aggregateEntry, DiskColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(GpuColumn, kUsageRatioRole, usageRatioForEntry(aggregateEntry, GpuColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(NetColumn, kUsageRatioRole, usageRatioForEntry(aggregateEntry, NetColumn, maxRamMB, maxDiskMBps, maxNetKBps));

    QFont appFont = itemPointer->font(NameColumn);
    appFont.setBold(true);
    itemPointer->setFont(NameColumn, appFont);
    return itemPointer;
}

QTreeWidgetItem* HudProcessListPanel::updateOrCreateRow(
    const ProcessEntry& entry,
    QTreeWidgetItem* parentItem,
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
        if (parentItem != nullptr)
        {
            parentItem->addChild(itemPointer);
        }
        else
        {
            m_treeWidget->addTopLevelItem(itemPointer);
        }
        m_itemByPid.insert(entry.pid, itemPointer);
    }

    itemPointer->setText(NameColumn, entry.processName);
    itemPointer->setText(PidColumn, QString::number(entry.pid));
    itemPointer->setIcon(NameColumn, resolveProcessIcon(entry));
    itemPointer->setText(CpuColumn, formatPercent(entry.cpuPercent, 2));
    itemPointer->setText(RamColumn, formatRamMB(entry.ramMB));
    itemPointer->setText(DiskColumn, formatDiskMBps(entry.diskMBps));
    itemPointer->setText(GpuColumn, formatPercent(entry.gpuPercent, 1));
    itemPointer->setText(NetColumn, formatNetKBps(entry.netKBps));
    itemPointer->setData(NameColumn, kPidRole, entry.pid);
    QVariantList terminatePidVariantList;
    terminatePidVariantList.push_back(entry.pid);
    itemPointer->setData(NameColumn, kTerminatePidListRole, terminatePidVariantList);
    itemPointer->setData(CpuColumn, kUsageRatioRole, usageRatioForEntry(entry, CpuColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(RamColumn, kUsageRatioRole, usageRatioForEntry(entry, RamColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(DiskColumn, kUsageRatioRole, usageRatioForEntry(entry, DiskColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(GpuColumn, kUsageRatioRole, usageRatioForEntry(entry, GpuColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    itemPointer->setData(NetColumn, kUsageRatioRole, usageRatioForEntry(entry, NetColumn, maxRamMB, maxDiskMBps, maxNetKBps));
    return itemPointer;
}

QString HudProcessListPanel::buildProcessIdentityKey(const quint32 pidValue, const QString& processName)
{
    return QStringLiteral("%1|%2").arg(pidValue).arg(processName.trimmed().toLower());
}

QIcon HudProcessListPanel::resolveProcessIcon(const ProcessEntry& entry)
{
    const QString identityKey = buildProcessIdentityKey(entry.pid, entry.processName);
    const auto identityCacheIterator = m_iconCacheByIdentity.constFind(identityKey);
    if (identityCacheIterator != m_iconCacheByIdentity.cend())
    {
        return identityCacheIterator.value();
    }

    if (!entry.imagePath.isEmpty())
    {
        const auto pathCacheIterator = m_iconCacheByPath.constFind(entry.imagePath);
        if (pathCacheIterator != m_iconCacheByPath.cend())
        {
            m_iconCacheByIdentity.insert(identityKey, pathCacheIterator.value());
            return pathCacheIterator.value();
        }

        if (m_fileIconProvider != nullptr)
        {
            const QFileInfo fileInfo(entry.imagePath);
            if (fileInfo.exists())
            {
                const QIcon resolvedIcon = m_fileIconProvider->icon(fileInfo);
                if (!resolvedIcon.isNull())
                {
                    m_iconCacheByPath.insert(entry.imagePath, resolvedIcon);
                    m_iconCacheByIdentity.insert(identityKey, resolvedIcon);
                    return resolvedIcon;
                }
            }
        }
    }

    const QIcon fallbackIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    m_iconCacheByIdentity.insert(identityKey, fallbackIcon);
    return fallbackIcon;
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
