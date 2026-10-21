#pragma once

#include <QAbstractTableModel>
#include <QDateTime>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QTableView;

namespace ks::ui
{
    class CompleteExportCapture;
    class AsyncOperation;
    // TableSnapshotColumn keeps the source-model column identity as well as the text displayed
    // in the column header.  sourceColumn lets two snapshots remain comparable after users move
    // a header section.
    struct TableSnapshotColumn
    {
        int sourceColumn = -1;
        QString headerText;
    };

    // TableSnapshotRow stores display-role text only.  The snapshot is therefore independent of
    // the source model lifetime and intentionally represents exactly what the user saw.
    struct TableSnapshotRow
    {
        int sourceRow = -1;
        QStringList values;
    };

    // Capture limits are deliberately small enough to keep a global table action responsive and
    // bounded even when the source model exposes millions of rows.  The model is always read on
    // its owning UI thread; eventLoopYieldRows controls cooperative event-loop yields.
    struct TableSnapshotCaptureLimits
    {
        int maximumRows = 10'000;
        int maximumColumns = 256;
        int maximumCellCharacters = 16 * 1024;
        int maximumHeaderCharacters = 1024;
        quint64 maximumEstimatedBytes = 16ULL * 1024ULL * 1024ULL;
        int eventLoopYieldRows = 128;
    };

    // TableSnapshot is an in-memory capture of a single table's visible state.
    struct TableSnapshot
    {
        QString label;
        QDateTime capturedAt;
        quint64 sequence = 0;
        QVector<TableSnapshotColumn> visibleColumns;
        QVector<TableSnapshotRow> rows;
        int sortColumn = -1;
        Qt::SortOrder sortOrder = Qt::AscendingOrder;
        QVector<int> keyColumns;
        int sourceRowCount = 0;
        int visitedSourceRows = 0;
        int sourceColumnCount = 0;
        int visitedSourceColumns = 0;
        quint64 estimatedBytes = 0;
        bool truncatedByRowLimit = false;
        bool truncatedByColumnLimit = false;
        bool truncatedByByteLimit = false;
        bool truncatedByValueLimit = false;
        int truncatedHeaderValueCount = 0;
        int truncatedCellValueCount = 0;
        int unsupportedDisplayValueCount = 0;
        bool sourceInvalidated = false;

        bool isTruncated() const
        {
            return truncatedByRowLimit ||
                truncatedByColumnLimit ||
                truncatedByByteLimit ||
                truncatedByValueLimit ||
                sourceInvalidated;
        }
    };

    // Per-table storage limits prevent repeated snapshots from retaining unbounded memory.
    struct TableSnapshotRetentionLimits
    {
        int maximumSnapshots = 8;
        quint64 maximumEstimatedBytes = 64ULL * 1024ULL * 1024ULL;
    };

    struct TableSnapshotRetentionResult
    {
        QStringList evictedLabels;
        quint64 remainingEstimatedBytes = 0;
    };

    enum class TableComparisonSource
    {
        EarlierOnly,
        LaterOnly,
        Both
    };

    // TableComparisonRow values follow TableComparisonResult::columns.  changedSourceColumns is
    // populated only when stable key columns make field-level comparison safe.
    struct TableComparisonRow
    {
        TableComparisonSource source = TableComparisonSource::Both;
        int earlierRowIndex = -1;
        int laterRowIndex = -1;
        bool displayLater = false;
        QSet<int> changedSourceColumns;
    };

    // Comparison owns only implicitly-shared snapshot copies and lazy row references.  The
    // independent 64 MiB result budget includes the logical size of both source snapshots; the
    // temporary hash/index budget and work-unit ceiling prevent comparison-time amplification.
    struct TableSnapshotComparisonLimits
    {
        quint64 maximumEstimatedBytes = 64ULL * 1024ULL * 1024ULL;
        quint64 maximumTemporaryEstimatedBytes = 32ULL * 1024ULL * 1024ULL;
        quint64 maximumWorkUnits = 16'000'000ULL;
        int eventLoopYieldRows = 64;
    };

    struct TableComparisonResult
    {
        TableSnapshot earlierSnapshot;
        TableSnapshot laterSnapshot;
        QVector<TableSnapshotColumn> columns;
        QVector<int> earlierValueIndexes;
        QVector<int> laterValueIndexes;
        QVector<TableComparisonRow> rows;
        QSet<int> ignoredSourceColumns;
        quint64 estimatedBytes = 0;
        quint64 temporaryPeakEstimatedBytes = 0;
        quint64 workUnits = 0;
        bool usesStableKeys = false;
        bool truncatedByResultByteLimit = false;
        bool truncatedByTemporaryByteLimit = false;
        bool truncatedByWorkLimit = false;
        bool cancelled = false;

        bool isTruncated() const
        {
            return truncatedByResultByteLimit ||
                truncatedByTemporaryByteLimit ||
                truncatedByWorkLimit ||
                cancelled;
        }
    };

    class TableSnapshotCompareEngine final
    {
    public:
        // capture collects visible columns in visual-header order and visible rows in their
        // current visual order.  It also records the optional kswordSnapshotKeyColumns property.
        // Synchronous compatibility entry: model access stays on its owner thread without
        // a nested event pump. UI actions use captureAsync for genuinely queued chunks.
        static TableSnapshot capture(
            QTableView* tableView,
            const QString& label,
            quint64 sequence,
            const TableSnapshotCaptureLimits& limits = TableSnapshotCaptureLimits());

        // Uses the existing queued GUI capture and value worker controllers.
        // Callers cancel/close both controllers; no model is read by the worker.
        static quint64 captureAsync(QTableView* tableView, CompleteExportCapture* capture,
            AsyncOperation* operation, const QString& label, quint64 sequence,
            std::function<void(TableSnapshot)> completed,
            const TableSnapshotCaptureLimits& limits = TableSnapshotCaptureLimits());

        // Returns the sum of retained estimated snapshot bytes with saturating arithmetic.
        static quint64 totalEstimatedBytes(const QVector<TableSnapshot>& snapshots);

        // Evicts oldest snapshots until both count and total-estimated-byte limits are satisfied.
        static TableSnapshotRetentionResult enforceRetentionLimits(
            QVector<TableSnapshot>& snapshots,
            const TableSnapshotRetentionLimits& limits = TableSnapshotRetentionLimits());

        // Built-in bilingual names for columns whose values normally change on every refresh.
        static QStringList defaultIgnoredColumnKeywords();

        // Returns the built-in default ignored source-model columns for one snapshot.  Callers
        // comparing two snapshots should unite the two returned sets.
        static QSet<int> defaultIgnoredColumnIndexes(const TableSnapshot& snapshot);

        // Returns source-model column numbers whose header contains any keyword.  The caller can
        // use this to render and adjust the temporary ignore-columns menu.
        static QSet<int> ignoredColumnIndexesForKeywords(
            const TableSnapshot& earlier,
            const TableSnapshot& later,
            const QStringList& keywords);

        // Compares an earlier and later snapshot.  Without usable stable keys it performs a
        // Git-style text multiset compare.  With usable declared keys it returns AB rows for the
        // same key and marks changed cells by source column.
        static TableComparisonResult compare(
            const TableSnapshot& earlier,
            const TableSnapshot& later,
            const QSet<int>& ignoredOriginalColumnIndexes,
            const QVector<int>& keyColumnIndexes = QVector<int>(),
            const TableSnapshotComparisonLimits& limits = TableSnapshotComparisonLimits(),
            const std::function<bool()>& shouldCancel = std::function<bool()>());

        // Converts a monotonically increasing snapshot ordinal to A..Z, AA, AB, etc.  The owner
        // must keep the ordinal monotonic even after deleting a snapshot.
        static QString snapshotLabelForOrdinal(quint64 ordinal);
    };

    // A read-only comparison view model.  Its first column is the source marker A, B or AB.
    class TableComparisonModel final : public QAbstractTableModel
    {
    public:
        explicit TableComparisonModel(QObject* parent = nullptr);
        explicit TableComparisonModel(TableComparisonResult comparison, QObject* parent = nullptr);

        void setComparison(TableComparisonResult comparison);
        const TableComparisonResult& comparison() const;

        void setShowDifferencesOnly(bool enabled);
        bool showDifferencesOnly() const;

        int rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        QVariant headerData(
            int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;
        Qt::ItemFlags flags(const QModelIndex& index) const override;

    private:
        void rebuildVisibleRows();

        TableComparisonResult m_comparison;
        QVector<int> m_visibleRows;
        bool m_showDifferencesOnly = true;
    };
}
