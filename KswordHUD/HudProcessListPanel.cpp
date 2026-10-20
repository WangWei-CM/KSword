#include "HudProcessListPanel.h"
#include "HudColors.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QElapsedTimer>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QStyle>
#include <QPixmap>
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

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <Pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <tcpestats.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Pdh.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace
{
    constexpr int kNameColumn = 0;
    constexpr int kUsageRatioRole = Qt::UserRole + 2;
    constexpr int kExpansionKeyRole = Qt::UserRole + 3;
    constexpr int kTerminateProcessIdentityListRole = Qt::UserRole + 4;
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

    quint32 extractPidFromGpuEngineInstanceName(const QString& instanceNameText)
    {
        // Inputs: a PDH GPU Engine wildcard instance name such as "pid_1234_luid_..._engtype_3D".
        // Processing: find the "pid_" marker and parse the following decimal digits only.
        // Return: the owning process ID, or 0 when the counter instance does not expose a PID.
        const QString lowerText = instanceNameText.toLower();
        const int pidMarkerIndex = lowerText.indexOf(QStringLiteral("pid_"));
        if (pidMarkerIndex < 0)
        {
            return 0;
        }

        QString pidText;
        int characterIndex = pidMarkerIndex + 4;
        while (characterIndex < lowerText.size() && lowerText.at(characterIndex).isDigit())
        {
            pidText.append(lowerText.at(characterIndex));
            ++characterIndex;
        }

        bool parseOk = false;
        const uint pidValue = pidText.toUInt(&parseOk);
        return parseOk ? static_cast<quint32>(pidValue) : 0;
    }

    QHash<quint32, double> sampleProcessGpuUsagePercentByPid()
    {
        // Inputs: none; the Windows PDH "GPU Engine(*)\\Utilization Percentage" counter is sampled.
        // Processing: keep one process-wide PDH wildcard query, parse PID-bearing GPU engine instances,
        //             and add all active engine usage for the same PID with a 100% display cap.
        // Return: PID -> current GPU utilization percent; an empty map is valid on first sample or PDH failure.
        static std::mutex samplerMutex;
        static PDH_HQUERY queryHandle = nullptr;
        static PDH_HCOUNTER counterHandle = nullptr;

        std::lock_guard<std::mutex> lockGuard(samplerMutex);
        QHash<quint32, double> usagePercentByPid;

        if (queryHandle == nullptr)
        {
            PDH_HQUERY newQueryHandle = nullptr;
            if (::PdhOpenQueryW(nullptr, 0, &newQueryHandle) != ERROR_SUCCESS || newQueryHandle == nullptr)
            {
                return usagePercentByPid;
            }

            PDH_HCOUNTER newCounterHandle = nullptr;
            const PDH_STATUS addStatus = ::PdhAddEnglishCounterW(
                newQueryHandle,
                L"\\GPU Engine(*)\\Utilization Percentage",
                0,
                &newCounterHandle);
            if (addStatus != ERROR_SUCCESS || newCounterHandle == nullptr)
            {
                ::PdhCloseQuery(newQueryHandle);
                return usagePercentByPid;
            }

            queryHandle = newQueryHandle;
            counterHandle = newCounterHandle;
            ::PdhCollectQueryData(queryHandle);
            return usagePercentByPid;
        }

        if (::PdhCollectQueryData(queryHandle) != ERROR_SUCCESS)
        {
            ::PdhCloseQuery(queryHandle);
            queryHandle = nullptr;
            counterHandle = nullptr;
            return usagePercentByPid;
        }

        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PDH_STATUS queryStatus = ::PdhGetFormattedCounterArrayW(
            counterHandle,
            PDH_FMT_DOUBLE,
            &bufferSize,
            &itemCount,
            nullptr);
        if (queryStatus != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0)
        {
            return usagePercentByPid;
        }

        std::vector<unsigned char> rawBuffer(bufferSize);
        auto* itemPointer = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(rawBuffer.data());
        queryStatus = ::PdhGetFormattedCounterArrayW(
            counterHandle,
            PDH_FMT_DOUBLE,
            &bufferSize,
            &itemCount,
            itemPointer);
        if (queryStatus != ERROR_SUCCESS)
        {
            return usagePercentByPid;
        }

        for (DWORD itemIndex = 0; itemIndex < itemCount; ++itemIndex)
        {
            const PDH_FMT_COUNTERVALUE_ITEM_W& itemValue = itemPointer[itemIndex];
            if (itemValue.FmtValue.CStatus != ERROR_SUCCESS)
            {
                continue;
            }

            const QString instanceNameText = QString::fromWCharArray(
                itemValue.szName != nullptr ? itemValue.szName : L"");
            const quint32 pidValue = extractPidFromGpuEngineInstanceName(instanceNameText);
            if (pidValue == 0)
            {
                continue;
            }

            const double boundedUsagePercent = qBound(0.0, itemValue.FmtValue.doubleValue, 100.0);
            const double accumulatedUsagePercent =
                usagePercentByPid.value(pidValue, 0.0) + boundedUsagePercent;
            usagePercentByPid.insert(pidValue, qBound(0.0, accumulatedUsagePercent, 100.0));
        }

        return usagePercentByPid;
    }

    bool tcpStateCanCarryData(const DWORD stateValue)
    {
        // Inputs: a MIB_TCP_STATE numeric value from the TCP owner-PID tables.
        // Processing: skip passive/terminal rows that cannot produce meaningful byte deltas.
        // Return: true when querying TCP EStats data counters is worthwhile for this connection.
        return stateValue != static_cast<DWORD>(MIB_TCP_STATE_CLOSED)
            && stateValue != static_cast<DWORD>(MIB_TCP_STATE_LISTEN)
            && stateValue != static_cast<DWORD>(MIB_TCP_STATE_TIME_WAIT)
            && stateValue != static_cast<DWORD>(MIB_TCP_STATE_DELETE_TCB);
    }

    quint64 cumulativeTcpDataBytes(const TCP_ESTATS_DATA_ROD_v0& dataStats)
    {
        // Inputs: one TCP EStats data snapshot for a live connection.
        // Processing: prefer direct data-byte counters, while tolerating systems that populate throughput counters.
        // Return: cumulative inbound plus outbound bytes for later per-PID rate delta calculation.
        const quint64 directDataBytes =
            static_cast<quint64>(dataStats.DataBytesIn)
            + static_cast<quint64>(dataStats.DataBytesOut);
        const quint64 throughputBytes =
            static_cast<quint64>(dataStats.ThruBytesReceived)
            + static_cast<quint64>(dataStats.ThruBytesAcked);
        return std::max(directDataBytes, throughputBytes);
    }

    bool queryTcp4DataStats(const MIB_TCPROW_OWNER_PID& ownerRow, TCP_ESTATS_DATA_ROD_v0* dataStatsOut)
    {
        // Inputs: one IPv4 TCP owner-PID row and an output buffer.
        // Processing: enable TCP EStats collection for the row, then read the read-only data counters.
        // Return: true when dataStatsOut receives a valid cumulative byte snapshot.
        if (dataStatsOut == nullptr)
        {
            return false;
        }

        MIB_TCPROW tcpRow{};
        tcpRow.dwState = ownerRow.dwState;
        tcpRow.dwLocalAddr = ownerRow.dwLocalAddr;
        tcpRow.dwLocalPort = ownerRow.dwLocalPort;
        tcpRow.dwRemoteAddr = ownerRow.dwRemoteAddr;
        tcpRow.dwRemotePort = ownerRow.dwRemotePort;

        TCP_ESTATS_DATA_RW_v0 rwStats{};
        rwStats.EnableCollection = TRUE;
        ::SetPerTcpConnectionEStats(
            &tcpRow,
            TcpConnectionEstatsData,
            reinterpret_cast<PUCHAR>(&rwStats),
            0,
            sizeof(rwStats),
            0);

        TCP_ESTATS_DATA_ROD_v0 rodStats{};
        const ULONG queryStatus = ::GetPerTcpConnectionEStats(
            &tcpRow,
            TcpConnectionEstatsData,
            nullptr,
            0,
            0,
            nullptr,
            0,
            0,
            reinterpret_cast<PUCHAR>(&rodStats),
            0,
            sizeof(rodStats));
        if (queryStatus != NO_ERROR)
        {
            return false;
        }

        *dataStatsOut = rodStats;
        return true;
    }

    bool queryTcp6DataStats(const MIB_TCP6ROW_OWNER_PID& ownerRow, TCP_ESTATS_DATA_ROD_v0* dataStatsOut)
    {
        // Inputs: one IPv6 TCP owner-PID row and an output buffer.
        // Processing: build the plain MIB_TCP6ROW key required by IP Helper EStats APIs.
        // Return: true when dataStatsOut receives a valid cumulative byte snapshot.
        if (dataStatsOut == nullptr)
        {
            return false;
        }

        MIB_TCP6ROW tcpRow{};
        std::memcpy(&tcpRow.LocalAddr, ownerRow.ucLocalAddr, sizeof(tcpRow.LocalAddr));
        tcpRow.dwLocalScopeId = ownerRow.dwLocalScopeId;
        tcpRow.dwLocalPort = ownerRow.dwLocalPort;
        std::memcpy(&tcpRow.RemoteAddr, ownerRow.ucRemoteAddr, sizeof(tcpRow.RemoteAddr));
        tcpRow.dwRemoteScopeId = ownerRow.dwRemoteScopeId;
        tcpRow.dwRemotePort = ownerRow.dwRemotePort;
        tcpRow.State = static_cast<MIB_TCP_STATE>(ownerRow.dwState);

        TCP_ESTATS_DATA_RW_v0 rwStats{};
        rwStats.EnableCollection = TRUE;
        ::SetPerTcp6ConnectionEStats(
            &tcpRow,
            TcpConnectionEstatsData,
            reinterpret_cast<PUCHAR>(&rwStats),
            0,
            sizeof(rwStats),
            0);

        TCP_ESTATS_DATA_ROD_v0 rodStats{};
        const ULONG queryStatus = ::GetPerTcp6ConnectionEStats(
            &tcpRow,
            TcpConnectionEstatsData,
            nullptr,
            0,
            0,
            nullptr,
            0,
            0,
            reinterpret_cast<PUCHAR>(&rodStats),
            0,
            sizeof(rodStats));
        if (queryStatus != NO_ERROR)
        {
            return false;
        }

        *dataStatsOut = rodStats;
        return true;
    }

    void collectTcp4NetworkBytesByPid(QHash<quint32, quint64>* networkBytesByPid)
    {
        // Inputs: mutable PID -> bytes map.
        // Processing: enumerate IPv4 TCP connections with owner PIDs and aggregate cumulative EStats bytes.
        // Return behavior: no value is returned; unsupported or denied rows are skipped.
        if (networkBytesByPid == nullptr)
        {
            return;
        }

        ULONG tableSize = 0;
        ULONG tableStatus = ::GetExtendedTcpTable(
            nullptr,
            &tableSize,
            FALSE,
            AF_INET,
            TCP_TABLE_OWNER_PID_ALL,
            0);
        if ((tableStatus != ERROR_INSUFFICIENT_BUFFER && tableStatus != NO_ERROR) || tableSize == 0)
        {
            return;
        }

        std::vector<unsigned char> tableBuffer(tableSize);
        auto* tablePointer = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(tableBuffer.data());
        tableStatus = ::GetExtendedTcpTable(
            tablePointer,
            &tableSize,
            FALSE,
            AF_INET,
            TCP_TABLE_OWNER_PID_ALL,
            0);
        if (tableStatus != NO_ERROR)
        {
            return;
        }

        for (DWORD rowIndex = 0; rowIndex < tablePointer->dwNumEntries; ++rowIndex)
        {
            const MIB_TCPROW_OWNER_PID& rowValue = tablePointer->table[rowIndex];
            const quint32 pidValue = static_cast<quint32>(rowValue.dwOwningPid);
            if (pidValue == 0 || !tcpStateCanCarryData(rowValue.dwState))
            {
                continue;
            }

            TCP_ESTATS_DATA_ROD_v0 dataStats{};
            if (queryTcp4DataStats(rowValue, &dataStats))
            {
                (*networkBytesByPid)[pidValue] += cumulativeTcpDataBytes(dataStats);
            }
        }
    }

    void collectTcp6NetworkBytesByPid(QHash<quint32, quint64>* networkBytesByPid)
    {
        // Inputs: mutable PID -> bytes map.
        // Processing: enumerate IPv6 TCP connections with owner PIDs and aggregate cumulative EStats bytes.
        // Return behavior: no value is returned; unsupported or denied rows are skipped.
        if (networkBytesByPid == nullptr)
        {
            return;
        }

        ULONG tableSize = 0;
        ULONG tableStatus = ::GetExtendedTcpTable(
            nullptr,
            &tableSize,
            FALSE,
            AF_INET6,
            TCP_TABLE_OWNER_PID_ALL,
            0);
        if ((tableStatus != ERROR_INSUFFICIENT_BUFFER && tableStatus != NO_ERROR) || tableSize == 0)
        {
            return;
        }

        std::vector<unsigned char> tableBuffer(tableSize);
        auto* tablePointer = reinterpret_cast<PMIB_TCP6TABLE_OWNER_PID>(tableBuffer.data());
        tableStatus = ::GetExtendedTcpTable(
            tablePointer,
            &tableSize,
            FALSE,
            AF_INET6,
            TCP_TABLE_OWNER_PID_ALL,
            0);
        if (tableStatus != NO_ERROR)
        {
            return;
        }

        for (DWORD rowIndex = 0; rowIndex < tablePointer->dwNumEntries; ++rowIndex)
        {
            const MIB_TCP6ROW_OWNER_PID& rowValue = tablePointer->table[rowIndex];
            const quint32 pidValue = static_cast<quint32>(rowValue.dwOwningPid);
            if (pidValue == 0 || !tcpStateCanCarryData(rowValue.dwState))
            {
                continue;
            }

            TCP_ESTATS_DATA_ROD_v0 dataStats{};
            if (queryTcp6DataStats(rowValue, &dataStats))
            {
                (*networkBytesByPid)[pidValue] += cumulativeTcpDataBytes(dataStats);
            }
        }
    }

    QHash<quint32, quint64> sampleTcpNetworkBytesByPid()
    {
        // Inputs: none; Windows IP Helper owner-PID TCP tables are sampled.
        // Processing: collect IPv4 and IPv6 TCP EStats byte counters keyed by owning PID.
        // Return: PID -> cumulative TCP bytes. UDP and driver-attributed traffic are intentionally absent.
        QHash<quint32, quint64> networkBytesByPid;
        collectTcp4NetworkBytesByPid(&networkBytesByPid);
        collectTcp6NetworkBytesByPid(&networkBytesByPid);
        return networkBytesByPid;
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
        return KswordHudColors::UsageHighlight(usageRatio);
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

        // 输入面板实际合成底色，供选中行按实际绘制覆盖层校准文字。
        void setBackgroundColor(const QColor& colorValue)
        {
            m_backgroundColor = colorValue;
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
            const QVariant usageRatioVariant = index.data(kUsageRatioRole);
            const QColor selectedBackground = KswordHudColors::Composite(
                QColor(89, 139, 214, 88), m_backgroundColor);
            // 文字先按所有覆盖底色求解，轨道绘制随后使用同一文字色做 alpha 校准。
            const QColor textColor = selected
                ? KswordHudColors::SelectedText(m_textColor, m_backgroundColor,
                    usageRatioVariant.isValid() ? usageRatioVariant.toDouble() : -1.0)
                : m_textColor;
            if (selected)
            {
                painter->fillRect(option.rect, QColor(89, 139, 214, 88));
            }

            if (usageRatioVariant.isValid())
            {
                const double usageRatio = std::clamp(usageRatioVariant.toDouble(), 0.0, 1.0);
                const QRectF trackRect = QRectF(option.rect.adjusted(6, 5, -6, -5));
                if (trackRect.width() > 2.0 && trackRect.height() > 2.0)
                {
                    painter->setPen(Qt::NoPen);
                    const QColor trackColor = selected
                        ? KswordHudColors::LegibleOverlay(QColor(255, 255, 255, 18), selectedBackground, textColor)
                        : QColor(255, 255, 255, 18);
                    painter->setBrush(trackColor);
                    painter->drawRoundedRect(trackRect, 8.0, 8.0);

                    QRectF fillRect = trackRect;
                    fillRect.setWidth(std::max(2.0, trackRect.width() * usageRatio));
                    const QColor fillColor = usageRatioToHighlightColor(usageRatio);
                    painter->setBrush(selected
                        ? KswordHudColors::LegibleOverlay(fillColor,
                            KswordHudColors::Composite(trackColor, selectedBackground), textColor)
                        : fillColor);
                    painter->drawRoundedRect(fillRect, 8.0, 8.0);
                }
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
        QColor m_backgroundColor = QColor(10, 15, 22); // 不含选中覆盖层的有效底色。
    };
}

HudProcessListPanel::HudProcessListPanel(QWidget* parent)
    : QWidget(parent)
{
    m_logicalCpuCount = static_cast<int>(std::max<DWORD>(
        1,
        ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));

    initializeUi();
    m_fallbackIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    m_iconWatcher = new QFutureWatcher<IconResult>(this);
    connect(m_iconWatcher, &QFutureWatcher<IconResult>::finished, this, [this]() {
        const auto result = m_iconWatcher->result();
        const QIcon icon = result.pixels.isNull() ? m_fallbackIcon : QIcon(QPixmap::fromImage(result.pixels));
        m_iconCacheByPath.insert(result.key, icon);
        requestNextIcon();
    });
    connect(m_treeWidget->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { requestNextIcon(); });
    connect(m_treeWidget, &QTreeWidget::itemExpanded, this, [this]() { requestNextIcon(); });
    connect(m_treeWidget, &QTreeWidget::itemCollapsed, this, [this]() { requestNextIcon(); });

    m_applyTimer = new QTimer(this);
    m_applyTimer->setInterval(0);
    connect(m_applyTimer, &QTimer::timeout, this, &HudProcessListPanel::applyRefreshChunk);
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
    if (!m_pendingRows.isEmpty() || !m_pendingRemovals.isEmpty()) m_applyTimer->start();
    startRefreshing();
    requestNextIcon();
}

void HudProcessListPanel::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    stopRefreshing();
    if (m_applyTimer) m_applyTimer->stop();
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
    delegate->setBackgroundColor(m_effectiveBackgroundColor);
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
        "color:palette(highlighted-text);"
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
        setEffectiveBackgroundColor(m_effectiveBackgroundColor);
        applyTreeWidgetStyle();
        m_treeWidget->viewport()->update();
    }
}

void HudProcessListPanel::setEffectiveBackgroundColor(const QColor& colorValue)
{
    m_effectiveBackgroundColor = colorValue;
    if (auto* delegate = dynamic_cast<HudProcessMetricDelegate*>(m_metricDelegate))
    {
        delegate->setBackgroundColor(colorValue);
    }
    if (m_treeWidget != nullptr)
    {
        // QSS 的选中文字与自绘 delegate 使用同一底色，不持有固定白色快照。
        QPalette colors = m_treeWidget->palette();
        colors.setColor(QPalette::Base, colorValue);
        colors.setColor(QPalette::HighlightedText, KswordHudColors::SelectedText(m_tableTextColor, colorValue));
        m_treeWidget->setPalette(colors);
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

    QVector<ProcessIdentity> terminateIdentityList;
    const QVariant terminateIdentityListVariant =
        itemPointer->data(NameColumn, kTerminateProcessIdentityListRole);
    if (terminateIdentityListVariant.canConvert<QVariantList>())
    {
        const QVariantList variantList = terminateIdentityListVariant.toList();
        terminateIdentityList.reserve(variantList.size());
        for (const QVariant& identityVariant : variantList)
        {
            if (!identityVariant.canConvert<QVariantList>())
            {
                continue;
            }

            const QVariantList identityValues = identityVariant.toList();
            if (identityValues.size() != 2)
            {
                continue;
            }

            bool childPidOk = false;
            bool creationTimeOk = false;
            const quint32 childPidValue = identityValues.at(0).toUInt(&childPidOk);
            const quint64 childCreationTime100ns = identityValues.at(1).toULongLong(&creationTimeOk);
            if (childPidOk && creationTimeOk
                && childPidValue != 0 && childCreationTime100ns != 0)
            {
                terminateIdentityList.push_back({ childPidValue, childCreationTime100ns });
            }
        }
    }
    if (terminateIdentityList.isEmpty())
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
        terminateIdentityList.size() > 1
        ? QStringLiteral("结束此应用")
        : QStringLiteral("结束进程"));
    QAction* selectedAction =
        menu.exec(m_treeWidget->viewport()->mapToGlobal(localPosition));
    if (selectedAction == terminateAction)
    {
        terminateProcessesByIdentityList(terminateIdentityList);
        requestRefresh();
    }
}

bool HudProcessListPanel::terminateProcessByIdentity(const ProcessIdentity& processIdentity)
{
    if (processIdentity.pid == 0 || processIdentity.creationTime100ns == 0)
    {
        return false;
    }

    const HANDLE processHandle = ::OpenProcess(
        PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        processIdentity.pid);
    if (processHandle == nullptr)
    {
        return false;
    }

    FILETIME creationTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    const bool identityMatches =
        ::GetProcessTimes(processHandle, &creationTime, &exitTime, &kernelTime, &userTime) != FALSE
        && fileTimeToUint64(creationTime) == processIdentity.creationTime100ns;
    const BOOL terminateOk = identityMatches ? ::TerminateProcess(processHandle, 1) : FALSE;
    ::CloseHandle(processHandle);
    return terminateOk != FALSE;
}

int HudProcessListPanel::terminateProcessesByIdentityList(
    const QVector<ProcessIdentity>& processIdentityList)
{
    // Inputs: process identities from one refresh snapshot, usually one process row or all children of one application row.
    // Processing: reopen each PID, verify its creation time on that handle, then terminate verified children/helpers first.
    // Return: the number of identities for which TerminateProcess returned success.
    int terminatedCount = 0;
    for (auto reverseIterator = processIdentityList.crbegin();
        reverseIterator != processIdentityList.crend();
        ++reverseIterator)
    {
        if (terminateProcessByIdentity(*reverseIterator))
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
    if (m_refreshInProgress || m_refreshWatcher == nullptr || m_applyTimer->isActive())
    {
        return;
    }

    if ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0)
    {
        return;
    }

    const QHash<QString, CounterSample> previousSamples = m_previousSamples;
    const QHash<QString, QString> cachedImagePathByIdentity = m_imagePathByIdentity;
    const int logicalCpuCount = m_logicalCpuCount;

    m_refreshInProgress = true;
    m_refreshWatcher->setFuture(QtConcurrent::run([previousSamples, cachedImagePathByIdentity, logicalCpuCount]() {
        return collectRefreshResult(previousSamples, cachedImagePathByIdentity, logicalCpuCount);
        }));
}

void HudProcessListPanel::captureExpandedState()
{
    // Inputs: current QTreeWidget state before a successful delta projection.
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

HudProcessListPanel::RefreshResult HudProcessListPanel::collectRefreshResult(
    const QHash<QString, CounterSample>& previousSamples,
    const QHash<QString, QString>& cachedImagePathByIdentity,
    const int logicalCpuCount)
{
    RefreshResult result;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QSet<quint32> visibleWindowPidSet = collectVisibleWindowPidSet();
    const QString windowsDirectoryPath = normalizedWindowsDirectoryPath();
    const QHash<quint32, double> gpuUsagePercentByPid = sampleProcessGpuUsagePercentByPid();
    const QHash<quint32, quint64> networkBytesByPid = sampleTcpNetworkBytesByPid();

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

        CounterSample nextSample{};
        nextSample.sampleMs = nowMs;
        nextSample.networkBytes = networkBytesByPid.value(entry.pid, 0);

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

        QString processInstanceKey;
        if (processHandle != nullptr)
        {
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
                entry.creationTime100ns = fileTimeToUint64(creationTime);
                nextSample.cpuTime100ns =
                    fileTimeToUint64(kernelTime) + fileTimeToUint64(userTime);
            }

            processInstanceKey =
                buildProcessInstanceKey(entry.pid, entry.creationTime100ns);
            if (!processInstanceKey.isEmpty())
            {
                entry.imagePath = cachedImagePathByIdentity.value(processInstanceKey);
            }

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

        const auto previousIterator = processInstanceKey.isEmpty()
            ? previousSamples.cend()
            : previousSamples.constFind(processInstanceKey);
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

                const quint64 networkDeltaBytes =
                    nextSample.networkBytes >= previousIterator->networkBytes
                    ? (nextSample.networkBytes - previousIterator->networkBytes)
                    : 0;
                entry.netKBps =
                    (static_cast<double>(networkDeltaBytes) / std::max(0.001, elapsedSeconds)) / 1024.0;
            }
        }

        entry.gpuPercent = gpuUsagePercentByPid.value(entry.pid, 0.0);

        result.entries.push_back(entry);
        if (!processInstanceKey.isEmpty())
        {
            result.nextSamples.insert(processInstanceKey, nextSample);
        }
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
    preparePresentationRows(result);
    result.ok = true;
    return result;
}

void HudProcessListPanel::preparePresentationRows(RefreshResult& result)
{
    const auto sorter = [](const ProcessEntry& a, const ProcessEntry& b) {
        const int order = a.processName.compare(b.processName, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a.pid < b.pid;
    };
    const auto keyFor = [](const ProcessEntry& entry) {
        const QString identity = buildProcessInstanceKey(entry.pid, entry.creationTime100ns);
        return QStringLiteral("process:") + (identity.isEmpty()
            ? QStringLiteral("%1|%2|%3").arg(entry.pid).arg(entry.processName, entry.imagePath) : identity);
    };
    const auto addRow = [&](const ProcessEntry& entry, const QString& key,
                            const QString& parent, const QVector<ProcessEntry>& identities,
                            bool aggregate) {
        PresentationRow row;
        row.key = key; row.parentKey = parent; row.entry = entry; row.aggregate = aggregate;
        row.cells = {aggregate && identities.size() > 1
                ? QStringLiteral("%1 (%2)").arg(entry.processName).arg(identities.size()) : entry.processName,
            QString::number(entry.pid), formatPercent(entry.cpuPercent, 2), formatRamMB(entry.ramMB),
            formatDiskMBps(entry.diskMBps), formatPercent(entry.gpuPercent, 1), formatNetKBps(entry.netKBps)};
        for (int column = CpuColumn; column < ColumnCount; ++column)
            row.ratios[column - CpuColumn] = usageRatioForEntry(entry, column, result.maxRamMB, result.maxDiskMBps, result.maxNetKBps);
        for (const auto& identity : identities) {
            if (identity.pid != 0 && identity.creationTime100ns != 0) {
                QVariantList pair; pair << QVariant::fromValue(identity.pid) << QVariant::fromValue(identity.creationTime100ns);
                row.terminateIdentities.append(QVariant(pair));
            }
        }
        result.rows.append(std::move(row));
    };
    QHash<quint32, QVector<ProcessEntry>> applications;
    QVector<ProcessEntry> background, system;
    for (const auto& entry : result.entries) {
        if (entry.groupType == ProcessGroupType::Application)
            applications[entry.applicationRootPid ? entry.applicationRootPid : entry.pid].append(entry);
        else if (entry.groupType == ProcessGroupType::WindowsSystem) system.append(entry);
        else background.append(entry);
    }
    struct Bucket { ProcessEntry aggregate; QVector<ProcessEntry> entries; };
    QVector<Bucket> buckets;
    for (auto it = applications.cbegin(); it != applications.cend(); ++it) {
        auto entries = it.value(); std::sort(entries.begin(), entries.end(), sorter);
        buckets.append({aggregateApplicationEntry(entries), std::move(entries)});
    }
    std::sort(buckets.begin(), buckets.end(), [&](const Bucket& a, const Bucket& b) { return sorter(a.aggregate, b.aggregate); });
    std::sort(background.begin(), background.end(), sorter);
    std::sort(system.begin(), system.end(), sorter);
    const auto group = [&](ProcessGroupType type, int count) {
        PresentationRow row; row.key = expansionKeyForGroup(type); row.group = true;
        row.groupOrder = processGroupOrder(type); row.cells = {processGroupTitle(type, count)};
        result.rows.append(std::move(row));
    };
    const QString appGroup = expansionKeyForGroup(ProcessGroupType::Application);
    group(ProcessGroupType::Application, buckets.size());
    for (const auto& bucket : buckets) {
        const QString appKey = QStringLiteral("app:") + keyFor(bucket.aggregate);
        addRow(bucket.aggregate, appKey, appGroup, bucket.entries, true);
        QHash<quint32, QVector<ProcessEntry>> children;
        for (const auto& entry : bucket.entries) children[entry.parentPid].append(entry);
        QSet<quint32> visited;
        const auto append = [&](const auto& self, const ProcessEntry& entry, const QString& parent) -> void {
            if (visited.contains(entry.pid)) return;
            visited.insert(entry.pid);
            const QString key = keyFor(entry); addRow(entry, key, parent, {entry}, false);
            for (const auto& child : children.value(entry.pid)) self(self, child, key);
        };
        for (const auto& entry : bucket.entries) if (entry.pid == bucket.aggregate.pid) append(append, entry, appKey);
        for (const auto& entry : bucket.entries) append(append, entry, appKey);
    }
    group(ProcessGroupType::Background, background.size());
    for (const auto& entry : background) addRow(entry, keyFor(entry), expansionKeyForGroup(ProcessGroupType::Background), {entry}, false);
    group(ProcessGroupType::WindowsSystem, system.size());
    for (const auto& entry : system) addRow(entry, keyFor(entry), expansionKeyForGroup(ProcessGroupType::WindowsSystem), {entry}, false);
}

void HudProcessListPanel::applyRefreshResult(const RefreshResult& result)
{
    if (!m_treeWidget || !result.ok || !isVisible()
        || (::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) return;
    m_previousSamples = result.nextSamples;
    updateHeaderSummary(result);
    m_imagePathByIdentity.clear();
    for (const auto& entry : result.entries) {
        const QString identity = buildProcessInstanceKey(entry.pid, entry.creationTime100ns);
        if (!identity.isEmpty() && !entry.imagePath.isEmpty()) m_imagePathByIdentity.insert(identity, entry.imagePath);
    }
    captureExpandedState();
    QSet<QString> live;
    for (const auto& row : result.rows) live.insert(row.key);
    m_pendingRemovals.clear();
    for (auto it = m_itemByKey.cbegin(); it != m_itemByKey.cend(); ++it)
        if (!live.contains(it.key())) m_pendingRemovals.append(it.key());
    m_pendingRows = result.rows;
    m_applyOffset = m_removeOffset = 0;
    m_siblingIndex.clear();
    m_treeWidget->setSortingEnabled(false);
    m_applyTimer->start();
}

void HudProcessListPanel::applyRefreshChunk()
{
    if (!isVisible()) { m_applyTimer->stop(); return; }
    if ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) { m_applyTimer->setInterval(30); return; }
    m_applyTimer->setInterval(0);
    QElapsedTimer elapsed; elapsed.start();
    const QSignalBlocker blocker(m_treeWidget);
    m_treeWidget->setUpdatesEnabled(false);
    int budget = 128;
    while (budget-- > 0 && elapsed.elapsed() < 4 && m_removeOffset < m_pendingRemovals.size()) {
        const QString removedKey = m_pendingRemovals.at(m_removeOffset++);
        m_iconPathByRowKey.remove(removedKey);
        auto* item = m_itemByKey.take(removedKey);
        if (!item) continue;
        // Surviving descendants retain their objects until their new parent is projected.
        const auto children = item->takeChildren();
        for (auto* child : children) m_treeWidget->addTopLevelItem(child);
        delete item;
    }
    while (m_removeOffset == m_pendingRemovals.size() && budget-- > 0 && elapsed.elapsed() < 4 && m_applyOffset < m_pendingRows.size()) {
        const auto& row = m_pendingRows.at(m_applyOffset++);
        auto* item = m_itemByKey.value(row.key);
        const bool created = item == nullptr;
        if (created) {
            item = new QTreeWidgetItem(); m_itemByKey.insert(row.key, item);
            item->setData(NameColumn, kExpansionKeyRole, row.key);
            for (int col = PidColumn; col < ColumnCount; ++col) item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);
            QFont font = item->font(NameColumn); font.setBold(row.group || row.aggregate); item->setFont(NameColumn, font);
            if (row.group) {
                item->setData(NameColumn, Qt::UserRole + 8, row.groupOrder);
                item->setFlags((item->flags() | Qt::ItemIsEnabled) & ~Qt::ItemIsSelectable);
                item->setForeground(NameColumn, QColor(96, 165, 250));
                item->setBackground(NameColumn, QColor(255, 255, 255, 12));
            }
        }
        auto* parent = m_itemByKey.value(row.parentKey);
        const int desired = m_siblingIndex[row.parentKey]++;
        const int current = item->parent() ? item->parent()->indexOfChild(item) : m_treeWidget->indexOfTopLevelItem(item);
        if (created || item->parent() != parent || current != desired) {
            const bool selected = item->isSelected();
            const bool currentItem = m_treeWidget->currentItem() == item;
            if (item->parent()) item->parent()->takeChild(item->parent()->indexOfChild(item));
            else if (!created) m_treeWidget->takeTopLevelItem(m_treeWidget->indexOfTopLevelItem(item));
            if (parent) parent->insertChild(desired, item); else m_treeWidget->insertTopLevelItem(desired, item);
            if (selected) item->setSelected(true);
            if (currentItem) m_treeWidget->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        }
        if (created && row.group) item->setFirstColumnSpanned(true);
        for (int col = 0; col < row.cells.size(); ++col)
            if (item->text(col) != row.cells.at(col)) item->setText(col, row.cells.at(col));
        if (!row.group) {
            m_iconPathByRowKey.insert(row.key, row.entry.imagePath);
            const auto icon = resolveProcessIcon(row.entry);
            if (item->icon(NameColumn).cacheKey() != icon.cacheKey()) item->setIcon(NameColumn, icon);
            if (item->data(NameColumn, kTerminateProcessIdentityListRole).toList() != row.terminateIdentities)
                item->setData(NameColumn, kTerminateProcessIdentityListRole, row.terminateIdentities);
            for (int col = CpuColumn; col < ColumnCount; ++col)
                if (item->data(col, kUsageRatioRole).toDouble() != row.ratios[col - CpuColumn]) item->setData(col, kUsageRatioRole, row.ratios[col - CpuColumn]);
        }
        if (created) item->setExpanded(m_expandedStateByKey.value(row.key, parent ? parent->isExpanded() : true));
    }
    m_treeWidget->setUpdatesEnabled(true);
    if (m_applyOffset == m_pendingRows.size() && m_removeOffset == m_pendingRemovals.size()) {
        m_applyTimer->stop(); m_pendingRows.clear(); m_pendingRemovals.clear();
        requestNextIcon();
    }
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
    // Processing: map the enum to a stable group identity that survives delta projection.
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

QString HudProcessListPanel::buildProcessInstanceKey(
    const quint32 pidValue,
    const quint64 creationTime100ns)
{
    if (pidValue == 0 || creationTime100ns == 0)
    {
        return {};
    }

    return QStringLiteral("%1|%2").arg(pidValue).arg(creationTime100ns);
}

QIcon HudProcessListPanel::resolveProcessIcon(const ProcessEntry& entry)
{
    const QString identity = buildProcessInstanceKey(entry.pid, entry.creationTime100ns) + QLatin1Char('|') + iconCacheKey(entry.imagePath, devicePixelRatioF());
    const auto found = m_iconCacheByIdentity.constFind(identity);
    if (!identity.isEmpty() && found != m_iconCacheByIdentity.cend()) return found.value();
    const auto path = m_iconCacheByPath.constFind(iconCacheKey(entry.imagePath, devicePixelRatioF()));
    if (path != m_iconCacheByPath.cend()) {
        if (!identity.isEmpty()) m_iconCacheByIdentity.insert(identity, path.value());
        return path.value();
    }
    return m_fallbackIcon;
}

QString HudProcessListPanel::iconCacheKey(const QString& path, qreal dpr)
{
    return path + QLatin1Char('|') + QString::number(qMax(16, qRound(18 * dpr)));
}

HudProcessListPanel::IconResult HudProcessListPanel::collectIconPixels(QString path, qreal dpr)
{
    IconResult result; result.path = std::move(path); result.key = iconCacheKey(result.path, dpr);
    const HRESULT apartment = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HICON icon = nullptr;
    const int pixels = qMax(16, qRound(18 * dpr));
    // Ask the executable resource decoder for the actual physical display size.
    ::SHDefExtractIconW(reinterpret_cast<LPCWSTR>(result.path.utf16()), 0, 0, &icon, nullptr, MAKELONG(pixels, pixels));
    if (!icon) {
        SHFILEINFOW info{};
        if (::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(result.path.utf16()), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)) icon = info.hIcon;
    }
    if (icon) {
        result.pixels = QImage::fromHICON(icon);
        result.pixels.setDevicePixelRatio(dpr);
        ::DestroyIcon(icon);
    }
    if (SUCCEEDED(apartment)) ::CoUninitialize();
    return result;
}

void HudProcessListPanel::requestNextIcon()
{
    if (!m_iconWatcher || !isVisible() || m_iconWatcher->isRunning()
        || (::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) return;
    QString pendingPath;
    const qreal dpr = devicePixelRatioF();
    auto* item = m_treeWidget->itemAt(QPoint(2, 0));
    if (!item && m_treeWidget->topLevelItemCount()) item = m_treeWidget->topLevelItem(0);
    while (item && m_treeWidget->visualItemRect(item).top() < m_treeWidget->viewport()->height()) {
        const QString key = item->data(NameColumn, kExpansionKeyRole).toString();
        const QString path = m_iconPathByRowKey.value(key);
        if (!path.isEmpty()) {
            const auto cached = m_iconCacheByPath.constFind(iconCacheKey(path, dpr));
            if (cached != m_iconCacheByPath.cend()) {
                if (item->icon(NameColumn).cacheKey() != cached.value().cacheKey()) item->setIcon(NameColumn, cached.value());
            } else if (pendingPath.isEmpty()) pendingPath = path;
        }
        item = m_treeWidget->itemBelow(item);
    }
    if (!pendingPath.isEmpty()) m_iconWatcher->setFuture(QtConcurrent::run([pendingPath, dpr]() { return collectIconPixels(pendingPath, dpr); }));
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
