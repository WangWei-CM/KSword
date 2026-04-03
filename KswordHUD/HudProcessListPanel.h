#pragma once

#include <QFutureWatcher>
#include <QColor>
#include <QHash>
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

    struct CounterSample
    {
        quint64 cpuTime100ns = 0;
        quint64 ioBytes = 0;
        qint64 sampleMs = 0;
    };

    struct ProcessEntry
    {
        quint32 pid = 0;
        QString processName;
        double cpuPercent = 0.0;
        double ramMB = 0.0;
        double diskMBps = 0.0;
        double gpuPercent = 0.0;
        double netKBps = 0.0;
    };

    struct RefreshResult
    {
        QVector<ProcessEntry> entries;
        QHash<quint32, CounterSample> nextSamples;
        double totalCpuPercent = 0.0;
        double totalRamMB = 0.0;
        double totalDiskMBps = 0.0;
        double totalGpuPercent = 0.0;
        double totalNetKBps = 0.0;
        double maxRamMB = 0.0;
        double maxDiskMBps = 0.0;
        double maxNetKBps = 0.0;
    };

    void initializeUi();
    void showContextMenu(const QPoint& localPosition);
    bool terminateProcessByPid(quint32 processIdValue);
    void startRefreshing();
    void stopRefreshing();
    void requestRefresh();
    static RefreshResult collectRefreshResult(
        const QHash<quint32, CounterSample>& previousSamples,
        int logicalCpuCount);
    void applyRefreshResult(const RefreshResult& result);
    void updateHeaderSummary(const RefreshResult& result);
    void updateOrCreateRow(
        const ProcessEntry& entry,
        double maxRamMB,
        double maxDiskMBps,
        double maxNetKBps);
    static double usageRatioForEntry(const ProcessEntry& entry, int columnIndex, double maxRamMB, double maxDiskMBps, double maxNetKBps);
    static QString formatPercent(double value, int decimals = 2);
    static QString formatRamMB(double value);
    static QString formatDiskMBps(double value);
    static QString formatNetKBps(double value);

    QTreeWidget* m_treeWidget = nullptr;
    QStyledItemDelegate* m_metricDelegate = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QFutureWatcher<RefreshResult>* m_refreshWatcher = nullptr;
    bool m_refreshInProgress = false;
    int m_logicalCpuCount = 1;
    QColor m_tableTextColor = QColor(255, 255, 255);
    QHash<quint32, CounterSample> m_previousSamples;
    QHash<quint32, QTreeWidgetItem*> m_itemByPid;
};
