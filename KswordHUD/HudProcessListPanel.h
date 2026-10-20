#pragma once

#include <QFutureWatcher>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QSet>
#include <QVector>
#include <QVariant>
#include <QStringList>
#include <array>
#include <QWidget>

class QHideEvent;
class QPoint;
class QShowEvent;
class QStyledItemDelegate;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class HudProcessListPanel final : public QWidget
{
public:
    explicit HudProcessListPanel(QWidget* parent = nullptr);
    ~HudProcessListPanel() override;
    void setTableTextColor(const QColor& colorValue);
    // 设置实际合成底色，仅用于选中行的对比校准，不改变普通文字或背景配置。
    void setEffectiveBackgroundColor(const QColor& colorValue);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum ColumnIndex
    {
        NameColumn = 0,
        PidColumn,
        CpuColumn,
        RamColumn,
        DiskColumn,
        GpuColumn,
        NetColumn,
        ColumnCount
    };

    enum class ProcessGroupType
    {
        Application = 0,
        Background,
        WindowsSystem
    };

    struct CounterSample
    {
        quint64 cpuTime100ns = 0;
        quint64 ioBytes = 0;
        quint64 networkBytes = 0;
        qint64 sampleMs = 0;
    };

    struct ProcessIdentity
    {
        quint32 pid = 0;
        quint64 creationTime100ns = 0;
    };

    struct ProcessEntry
    {
        quint32 pid = 0;
        quint32 parentPid = 0;
        quint32 applicationRootPid = 0;
        quint64 creationTime100ns = 0;
        QString processName;
        QString imagePath;
        ProcessGroupType groupType = ProcessGroupType::Background;
        double cpuPercent = 0.0;
        double ramMB = 0.0;
        double diskMBps = 0.0;
        double gpuPercent = 0.0;
        double netKBps = 0.0;
    };

    struct PresentationRow
    {
        QString key;
        QString parentKey;
        ProcessEntry entry;
        QStringList cells;
        std::array<double, 5> ratios{};
        QVariantList terminateIdentities;
        bool group = false;
        bool aggregate = false;
        int groupOrder = 0;
    };

    struct RefreshResult
    {
        QVector<ProcessEntry> entries;
        QVector<PresentationRow> rows;
        bool ok = false;
        QHash<QString, CounterSample> nextSamples;
        double totalCpuPercent = 0.0;
        double totalRamMB = 0.0;
        double totalDiskMBps = 0.0;
        double totalGpuPercent = 0.0;
        double totalNetKBps = 0.0;
        double maxRamMB = 0.0;
        double maxDiskMBps = 0.0;
        double maxNetKBps = 0.0;
    };

    struct IconResult { QString path; QString key; QImage pixels; };
    void requestNextIcon();
    static QString iconCacheKey(const QString& path, qreal dpr);
    static IconResult collectIconPixels(QString path, qreal dpr);
    void initializeUi();
    void applyTreeWidgetStyle();
    void showContextMenu(const QPoint& localPosition);
    bool terminateProcessByIdentity(const ProcessIdentity& processIdentity);
    int terminateProcessesByIdentityList(const QVector<ProcessIdentity>& processIdentityList);
    void startRefreshing();
    void stopRefreshing();
    void requestRefresh();
    void captureExpandedState();
    void captureExpandedStateForItem(QTreeWidgetItem* itemPointer);
    static RefreshResult collectRefreshResult(
        const QHash<QString, CounterSample>& previousSamples,
        const QHash<QString, QString>& cachedImagePathByIdentity,
        int logicalCpuCount);
    static void preparePresentationRows(RefreshResult& result);
    void applyRefreshResult(const RefreshResult& result);
    void applyRefreshChunk();
    void updateHeaderSummary(const RefreshResult& result);
    QIcon resolveProcessIcon(const ProcessEntry& entry);
    static void classifyProcessGroups(QVector<ProcessEntry>* entries, const QSet<quint32>& visibleWindowPidSet);
    static quint32 findApplicationRootPid(quint32 pidValue, const QHash<quint32, quint32>& parentPidByPid, const QSet<quint32>& visibleWindowPidSet);
    static bool isWindowsSystemProcess(const ProcessEntry& entry, const QString& windowsDirectoryPath);
    static ProcessEntry aggregateApplicationEntry(const QVector<ProcessEntry>& applicationEntries);
    static QString expansionKeyForGroup(ProcessGroupType groupType);
    static QString processGroupTitle(ProcessGroupType groupType, int entryCount);
    static int processGroupOrder(ProcessGroupType groupType);
    static QString buildProcessInstanceKey(quint32 pidValue, quint64 creationTime100ns);
    static double usageRatioForEntry(const ProcessEntry& entry, int columnIndex, double maxRamMB, double maxDiskMBps, double maxNetKBps);
    static QString formatPercent(double value, int decimals = 2);
    static QString formatRamMB(double value);
    static QString formatDiskMBps(double value);
    static QString formatNetKBps(double value);

    QTreeWidget* m_treeWidget = nullptr;
    QStyledItemDelegate* m_metricDelegate = nullptr;
    QFutureWatcher<IconResult>* m_iconWatcher = nullptr;
    QIcon m_fallbackIcon;
    QHash<QString, QString> m_iconPathByRowKey;
    QTimer* m_refreshTimer = nullptr;
    QFutureWatcher<RefreshResult>* m_refreshWatcher = nullptr;
    bool m_refreshInProgress = false;
    int m_logicalCpuCount = 1;
    QColor m_tableTextColor = QColor(255, 255, 255);
    QColor m_effectiveBackgroundColor = QColor(10, 15, 22); // 资源轨道下方的实际合成底色。
    QHash<QString, CounterSample> m_previousSamples;
    QHash<QString, QString> m_imagePathByIdentity;
    QHash<QString, QIcon> m_iconCacheByIdentity;
    QHash<QString, QIcon> m_iconCacheByPath;
    QHash<QString, QTreeWidgetItem*> m_itemByKey;
    QTimer* m_applyTimer = nullptr;
    QVector<PresentationRow> m_pendingRows;
    QStringList m_pendingRemovals;
    QHash<QString, int> m_siblingIndex;
    int m_applyOffset = 0;
    int m_removeOffset = 0;
    QHash<QString, bool> m_expandedStateByKey;
};
